#pragma once

#include "../framebarcode.h"
#include <drm_fourcc.h>
#include <poll.h>
#include <sys/mman.h>

#include "renderer.h"
#include "swframemapper.h"

#ifdef HAVE_EGL
#include "eglimagefactory.h"
#endif

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <set>
#include <unordered_map>
#include <mutex>

// This is only defined in Linux 6.8+ headers
#ifndef DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP
#define DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP	0x15
#endif

// Newer libdrm headers have these HDR structs, but some older ones don't.
namespace DrmDefs
{
    // HDR structs is copied from linux include/linux/hdmi.h
    struct hdr_metadata_infoframe
    {
        uint8_t eotf;
        uint8_t metadata_type;

        struct
        {
            uint16_t x, y;
        } display_primaries[3];

        struct
        {
            uint16_t x, y;
        } white_point;

        uint16_t max_display_mastering_luminance;
        uint16_t min_display_mastering_luminance;

        uint16_t max_cll;
        uint16_t max_fall;
    };

    struct hdr_output_metadata
    {
        uint32_t metadata_type;

        union {
            struct hdr_metadata_infoframe hdmi_metadata_type1;
        };
    };
}

class DrmRenderer : public IFFmpegRenderer {
    class DrmProperty {
    public:
        DrmProperty(uint32_t objectId, uint32_t objectType, drmModePropertyPtr prop, uint64_t initialValue) :
            m_ObjectId(objectId), m_ObjectType(objectType), m_Prop(prop), m_InitialValue(initialValue) {
            for (int i = 0; i < m_Prop->count_enums; i++) {
                m_Values.emplace(m_Prop->enums[i].name, m_Prop->enums[i].value);
            }
        }

        ~DrmProperty() noexcept {
            if (m_Prop) {
                drmModeFreeProperty(m_Prop);
            }
        }

        DrmProperty(DrmProperty &&other) = delete;
        DrmProperty(const DrmProperty &) = delete;

        bool isImmutable() const {
            return m_Prop->flags & DRM_MODE_PROP_IMMUTABLE;
        }

        const char* name() const {
            return m_Prop->name;
        }

        uint32_t id() const {
            return m_Prop->prop_id;
        }

        std::optional<std::pair<uint64_t, uint64_t>> range() const {
            if ((m_Prop->flags & DRM_MODE_PROP_RANGE) && m_Prop->count_values == 2) {
                return std::make_pair(m_Prop->values[0], m_Prop->values[1]);
            }
            else {
                return std::nullopt;
            }
        }

        std::optional<std::pair<int64_t, int64_t>> srange() const {
            if ((m_Prop->flags & DRM_MODE_PROP_SIGNED_RANGE) && m_Prop->count_values == 2) {
                return std::make_pair((int64_t)m_Prop->values[0], (int64_t)m_Prop->values[1]);
            }
            else {
                return std::nullopt;
            }
        }

        uint64_t clamp(uint64_t value) const {
            if (auto srange = this->srange()) {
                return (uint64_t)std::clamp((int64_t)value, srange->first, srange->second);
            }
            else if (auto range = this->range()) {
                return std::clamp(value, range->first, range->second);
            }
            else {
                return value;
            }
        }

        bool containsValue(const std::string &name) const {
            return m_Values.find(name) != m_Values.end();
        }

        std::optional<uint64_t> value(const std::string &name) const {
            if (auto it = m_Values.find(name); it != m_Values.end()) {
                return it->second;
            }
            else {
                return std::nullopt;
            }
        }

        uint32_t objectId() const {
            return m_ObjectId;
        }

        uint32_t objectType() const {
            return m_ObjectType;
        }

        uint64_t initialValue() const {
            return m_InitialValue;
        }

    private:
        uint32_t m_ObjectId;
        uint32_t m_ObjectType;
        drmModePropertyPtr m_Prop;
        std::unordered_map<std::string, uint64_t> m_Values;
        uint64_t m_InitialValue;
    };

    class DrmPropertyMap {
    public:
        DrmPropertyMap() {}
        DrmPropertyMap(int fd, uint32_t objectId, uint32_t objectType) {
            SDL_assert(m_ObjectType == 0);
            SDL_assert(m_ObjectId == 0);
            load(fd, objectId, objectType);
        }

        DrmPropertyMap(const DrmPropertyMap &) = delete;
        DrmPropertyMap(DrmPropertyMap &&other) {
            m_Props = std::move(other.m_Props);
            m_ObjectId = other.m_ObjectId;
            m_ObjectType = other.m_ObjectType;
        }

        bool isValid() const {
            return m_ObjectType != 0;
        }

        void load(int fd, uint32_t objectId, uint32_t objectType) {
            drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, objectId, objectType);
            if (props) {
                for (uint32_t i = 0; i < props->count_props; i++) {
                    drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
                    if (prop) {
                        // DrmProperty takes ownership of the drmModePropertyPtr
                        m_Props.try_emplace(prop->name, objectId, objectType, prop, props->prop_values[i]);
                    }
                }

                drmModeFreeObjectProperties(props);
            }

            m_ObjectId = objectId;
            m_ObjectType = objectType;
        }

        bool hasProperty(const std::string& name) const {
            return m_Props.find(name) != m_Props.end();
        }

        const DrmProperty* property(const std::string& name) const {
            auto it = m_Props.find(name);
            if (it == m_Props.end()) {
                return nullptr;
            }

            return &it->second;
        }

        uint32_t objectId() const {
            return m_ObjectId;
        }

        uint32_t objectType() const {
            return m_ObjectType;
        }

        const std::unordered_map<std::string, DrmProperty>& properties() const {
            return m_Props;
        }

    private:
        uint32_t m_ObjectId = 0;
        uint32_t m_ObjectType = 0;
        std::unordered_map<std::string, DrmProperty> m_Props;
    };

    class DrmPropertySetter {
        struct PlaneConfiguration {
            uint32_t crtcId;
            int32_t crtcX, crtcY;
            uint32_t crtcW, crtcH, srcX, srcY, srcW, srcH;
        };

        struct PlaneBuffer {
            uint32_t fbId;
            uint32_t dumbBufferHandle;

            // Atomic only
            uint32_t pendingFbId;
            uint32_t pendingDumbBuffer;
            uint32_t fbDamageClipsPropId;
            std::vector<drm_mode_rect> pendingFbDamageRects;
            bool modified;
        };

    public:
        DrmPropertySetter() {}
        ~DrmPropertySetter() {
            for (auto it = m_PlaneBuffers.begin(); it != m_PlaneBuffers.end(); it++) {
                SDL_assert(!it->second.fbId);
                SDL_assert(!it->second.dumbBufferHandle);

                if (it->second.pendingFbId) {
                    drmModeRmFB(m_Fd, it->second.pendingFbId);
                }
                if (it->second.pendingDumbBuffer) {
                    struct drm_mode_destroy_dumb destroyBuf = {};
                    destroyBuf.handle = it->second.pendingDumbBuffer;
                    drmIoctl(m_Fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroyBuf);
                }
            }

            if (m_AtomicReq) {
                drmModeAtomicFree(m_AtomicReq);
            }

            if (m_TotalCommits != 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Flip stats: %.2f%% async | %.2f%% sync",
                            (100.f * m_AsyncCommits) / m_TotalCommits,
                            (100.f * (m_TotalCommits - m_AsyncCommits)) / m_TotalCommits);
            }
        }

        // --- writeback capture state (Phase 5b; inert unless armed) ---
        bool m_WbWanted = false;
        bool m_WbFailed = false;
        bool m_WbArmed = false;
        uint32_t m_WbConnId = 0;
        uint32_t m_WbCrtcId = 0;
        uint32_t m_WbFbId = 0;
        uint32_t m_WbHandle = 0;
        uint32_t m_WbPitch = 0;
        uint32_t m_WbW = 0;
        uint32_t m_WbH = 0;
        uint64_t m_WbSize = 0;
        uint8_t* m_WbMap = nullptr;
        int32_t m_WbFence = -1;
        uint32_t m_WbCrtcIdProp = 0, m_WbFbProp = 0, m_WbFenceProp = 0;
        unsigned m_WbCounter = 0;
        unsigned m_WbInterval = 15;

        DrmPropertySetter(const DrmPropertySetter &) = delete;
        DrmPropertySetter(DrmPropertySetter &&) = delete;

        void initialize(int drmFd, bool wantsAtomic, bool wantsAsyncFlip) {
            m_Fd = drmFd;
            m_Atomic = wantsAtomic && drmSetClientCap(drmFd, DRM_CLIENT_CAP_ATOMIC, 1) == 0;

            // Writeback connectors are HIDDEN unless a client asks for them,
            // and the cap has to be set before anything enumerates.
            const char* wb = SDL_getenv("MOONLIGHT_FRAMEWB");
            m_WbWanted = m_Atomic && wb && *wb && *wb != '0';
            if (m_WbWanted &&
                drmSetClientCap(m_Fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1) != 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Writeback capture requested but the kernel refused the cap");
                m_WbWanted = false;
            }

            if (wantsAsyncFlip) {
                uint64_t val;
                if (!m_Atomic) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "V-sync cannot be disabled due to lack of atomic support");
                }
                else if (drmGetCap(m_Fd, DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP, &val) < 0 || !val) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "V-sync cannot be disabled due to lack of atomic async page flip support");
                }
                else {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Using atomic async page flips with V-sync disabled");
                    m_AsyncFlip = true;
                }
            }
        }

        int set(uint32_t objectId, uint32_t objectType, uint32_t propId, uint64_t value) {
            int err;

            if (m_Atomic) {
                // Synchronize with other threads that might be committing or setting properties
                std::lock_guard lg { m_Lock };

                if (!m_AtomicReq) {
                    m_AtomicReq = drmModeAtomicAlloc();
                }

                err = drmModeAtomicAddProperty(m_AtomicReq, objectId, propId, value);

                // This returns the new count of properties on success,
                // so normalize it to 0 like drmModeObjectSetProperty()
                if (err > 0) {
                    err = 0;
                }
            }
            else {
                err = drmModeObjectSetProperty(m_Fd, objectId, objectType, propId, value);
            }

            return err;
        }

        bool set(const DrmProperty& prop, uint64_t value, bool verbose = true) {
            int err = set(prop.objectId(), prop.objectType(), prop.id(), value);
            if (verbose && err == 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Set property '%s': %" PRIu64,
                            prop.name(),
                            value);
            }
            else if (err < 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Failed to set property '%s': %d",
                             prop.name(),
                             err);
            }

            return err == 0;
        }

        bool set(const DrmProperty& prop, const std::string &value) {
            std::optional<uint64_t> propValue = prop.value(value);
            if (!propValue) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Property '%s' has no supported enum value '%s'",
                             prop.name(),
                             value.c_str());
                return false;
            }

            if (set(prop, *propValue, false)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Set property '%s': %s",
                            prop.name(),
                            value.c_str());
                return true;
            }
            else {
                return false;
            }
        }

        // Unconditionally takes ownership of fbId and dumbBufferHandle (if present)
        // If provided, frame is referenced to keep the FB's backing DMA-BUFs around
        bool flipPlane(const DrmPropertyMap& plane, uint32_t fbId, uint32_t dumbBufferHandle) {
            bool ret;

            if (m_Atomic) {
                std::lock_guard lg { m_Lock };

                ret = set(*plane.property("FB_ID"), fbId, false);
                if (ret) {
                    // If we updated the FB_ID property, free the old pending buffer.
                    // Otherwise, we'll free the new buffer which was never used.
                    auto &pb = m_PlaneBuffers[plane.objectId()];
                    std::swap(fbId, pb.pendingFbId);
                    std::swap(dumbBufferHandle, pb.pendingDumbBuffer);
                    pb.modified = true;
                }
            }
            else {
                PlaneConfiguration planeConfig;

                {
                    // Latch the plane configuration and release the lock
                    std::lock_guard lg { m_Lock };
                    planeConfig = m_PlaneConfigs.at(plane.objectId());
                }

                int err = drmModeSetPlane(m_Fd, plane.objectId(),
                                          planeConfig.crtcId, fbId, 0,
                                          planeConfig.crtcX, planeConfig.crtcY,
                                          planeConfig.crtcW, planeConfig.crtcH,
                                          planeConfig.srcX, planeConfig.srcY,
                                          planeConfig.srcW, planeConfig.srcH);

                // If we succeeded updating the plane, free the old FB state
                // Otherwise, we'll free the new data which was never used.
                if (err == 0) {
                    std::lock_guard lg { m_Lock };

                    auto &pb = m_PlaneBuffers[plane.objectId()];
                    std::swap(fbId, pb.fbId);
                    std::swap(dumbBufferHandle, pb.dumbBufferHandle);

                    ret = true;
                }
                else {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "drmModeSetPlane() failed: %d",
                                 err);
                    ret = false;
                }
            }

            // Free the unused resources
            if (fbId) {
                drmModeRmFB(m_Fd, fbId);
            }
            if (dumbBufferHandle) {
                struct drm_mode_destroy_dumb destroyBuf = {};
                destroyBuf.handle = dumbBufferHandle;
                drmIoctl(m_Fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroyBuf);
            }

            return ret;
        }

        bool configurePlane(const DrmPropertyMap& plane,
                            uint32_t crtcId,
                            int32_t crtcX, int32_t crtcY,
                            uint32_t crtcW, uint32_t crtcH,
                            uint32_t srcX, uint32_t srcY,
                            uint32_t srcW, uint32_t srcH) {
            bool ret = true;
            std::lock_guard lg { m_Lock };

            if (m_Atomic) {
                ret = ret && set(*plane.property("CRTC_ID"), crtcId, false);
                ret = ret && set(*plane.property("CRTC_X"), crtcX, false);
                ret = ret && set(*plane.property("CRTC_Y"), crtcY, false);
                ret = ret && set(*plane.property("CRTC_W"), crtcW, false);
                ret = ret && set(*plane.property("CRTC_H"), crtcH, false);
                ret = ret && set(*plane.property("SRC_X"), srcX, false);
                ret = ret && set(*plane.property("SRC_Y"), srcY, false);
                ret = ret && set(*plane.property("SRC_W"), srcW, false);
                ret = ret && set(*plane.property("SRC_H"), srcH, false);
            }
            else {
                auto& planeConfig = m_PlaneConfigs[plane.objectId()];
                planeConfig.crtcId = crtcId;
                planeConfig.crtcX = crtcX;
                planeConfig.crtcY = crtcY;
                planeConfig.crtcW = crtcW;
                planeConfig.crtcH = crtcH;
                planeConfig.srcX = srcX;
                planeConfig.srcY = srcY;
                planeConfig.srcW = srcW;
                planeConfig.srcH = srcH;
            }

            return ret;
        }

        void disablePlane(const DrmPropertyMap& plane) {
            if (plane.isValid()) {
                configurePlane(plane, 0, 0, 0, 0, 0, 0, 0, 0, 0);
                flipPlane(plane, 0, 0);
            }
        }

        void restorePlane(const DrmPropertyMap& plane) {
            if (!plane.isValid()) {
                return;
            }

            if (isAtomic() && plane.property("FB_ID")->initialValue() != 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Restoring previously active plane: %u",
                            plane.objectId());

                {
                    std::lock_guard lg { m_Lock };
                    auto &pb = m_PlaneBuffers[plane.objectId()];

                    // Free any pending buffers first
                    if (pb.pendingFbId) {
                        drmModeRmFB(m_Fd, pb.pendingFbId);
                        pb.pendingFbId = 0;
                    }
                    if (pb.pendingDumbBuffer) {
                        struct drm_mode_destroy_dumb destroyBuf = {};
                        destroyBuf.handle = pb.pendingDumbBuffer;
                        drmIoctl(m_Fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroyBuf);
                        pb.pendingDumbBuffer = 0;
                    }

                    // Since we restore the FB_ID, remember that we need to swap
                    // pending buffers to current after committing in apply().
                    // This would normally be done in flipPlane(), but we can't
                    // use that here as it takes ownership of the FB.
                    //
                    // Since we cleared our pending buffers, the result is that
                    // apply() will free the current buffers and believe that no
                    // buffers are currently set. This will ensure it doesn't
                    // later free the restored FB_ID (which would happen if we
                    // used flipPlane() normally).
                    pb.modified = true;
                }

                // Restore the old plane properties and FB_ID
                restoreToInitial(plane);
            }
            else {
                disablePlane(plane);
            }
        }

        // The damage rect is relative to the FB
        void damagePlane(const DrmPropertyMap& plane, const SDL_Rect& rect) {
            SDL_assert(m_Atomic);

            if (auto prop = plane.property("FB_DAMAGE_CLIPS")) {
                std::lock_guard lg { m_Lock };
                auto& pb = m_PlaneBuffers[plane.objectId()];

                // Append this new damage rect to our existing list
                drm_mode_rect drmRect;
                drmRect.x1 = rect.x;
                drmRect.x2 = rect.x + rect.w;
                drmRect.y1 = rect.y;
                drmRect.y2 = rect.y + rect.h;
                pb.pendingFbDamageRects.emplace_back(std::move(drmRect));

                pb.fbDamageClipsPropId = prop->id();
            }
        }

        bool testPlane(const DrmPropertyMap& plane,
                       uint32_t crtcId, uint32_t fbId,
                       int32_t crtcX, int32_t crtcY,
                       uint32_t crtcW, uint32_t crtcH,
                       uint32_t srcX, uint32_t srcY,
                       uint32_t srcW, uint32_t srcH)
        {
            // Normally we wouldn't want to hold this lock across a blocking atomic commit,
            // but we do in this case because this operation is infrequent and it significantly
            // simplifies our state management to avoid racing atomic commits or modifications
            // to our atomic request object. It's also only a test commit, so it should be fast.
            std::lock_guard lg { m_Lock };

            SDL_assert(m_Atomic);

            // Store the old atomic request to restore after testing
            drmModeAtomicReqPtr oldReq = nullptr;
            std::swap(oldReq, m_AtomicReq);

            bool ret = true;
            ret = ret && set(*plane.property("CRTC_ID"), crtcId, false);
            ret = ret && set(*plane.property("FB_ID"), fbId, false);
            ret = ret && set(*plane.property("CRTC_X"), crtcX, false);
            ret = ret && set(*plane.property("CRTC_Y"), crtcY, false);
            ret = ret && set(*plane.property("CRTC_W"), crtcW, false);
            ret = ret && set(*plane.property("CRTC_H"), crtcH, false);
            ret = ret && set(*plane.property("SRC_X"), srcX, false);
            ret = ret && set(*plane.property("SRC_Y"), srcY, false);
            ret = ret && set(*plane.property("SRC_W"), srcW, false);
            ret = ret && set(*plane.property("SRC_H"), srcH, false);

            if (ret) {
                SDL_assert(m_AtomicReq);
                int err = drmModeAtomicCommit(m_Fd, m_AtomicReq,
                                              DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET,
                                              nullptr);
                if (err < 0) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "drmModeAtomicCommit(DRM_MODE_ATOMIC_TEST_ONLY) failed: %d",
                                 err);
                    ret = false;
                }
            }

            // Swap the old atomic request back and free the test one
            std::swap(oldReq, m_AtomicReq);
            if (oldReq) {
                drmModeAtomicFree(oldReq);
            }

            return ret;
        }

        // ---------------------------------------------------------------
        // stability/stream Phase 5(b): capture what was actually SCANNED OUT.
        //
        // The decoded-frame check one layer up proves the client decoded a new
        // picture. It cannot prove the display controller composed and scanned
        // that picture out — a stuck plane, a pacer that never flips, or a
        // compositor drawing over the video all look identical from there.
        // DRM writeback is the only instrument that answers it, because it
        // reads back the composed CRTC output itself.
        //
        // Hard-won requirement: the writeback buffer must be NV12. VOP2's
        // writeback path does no colour conversion ("YUV2RGB is not supported
        // by writeback" — drivers/gpu/drm/rockchip), and this display pipeline
        // runs YUV, so an RGB buffer is rejected with a bare EINVAL that looks
        // exactly like "writeback is broken". It is not.
        //
        // Off unless MOONLIGHT_FRAMEWB is set: this adds a buffer and a fence
        // wait to a per-frame commit, and a client someone is watching should
        // pay nothing for a test harness.
        //
        // INERT ON THIS BOX, and the reason is worth writing down. Writeback
        // needs an ATOMIC commit from the DRM master. Here the master belongs
        // to Qt EGLFS, which drives the display through the LEGACY KMS API
        // ("Captured Qt EGLFS DRM master fd (legacy)"), and DrmRenderer does
        // not even initialise ("DRM encoder not found!") — so there is no
        // atomic presentation path to attach to. The capability itself is
        // real: ~/display/tools/drm-writeback-probe captures the composed
        // output and decodes it correctly, standalone. Making this run would
        // mean moving Qt EGLFS to atomic, which is a Qt-level change. Kept as
        // the reference implementation for a stack that gets there.
        // ---------------------------------------------------------------
        bool wbPrepare(uint32_t crtcId, uint32_t width, uint32_t height) {
            if (!m_WbWanted || m_WbFailed) {
                return false;
            }
            if (m_WbFbId != 0) {
                return true;
            }

            drmModeRes* res = drmModeGetResources(m_Fd);
            if (res == nullptr) {
                m_WbFailed = true;
                return false;
            }
            for (int i = 0; i < res->count_connectors && m_WbConnId == 0; i++) {
                drmModeConnector* c = drmModeGetConnector(m_Fd, res->connectors[i]);
                if (c == nullptr) {
                    continue;
                }
                if (c->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
                    m_WbConnId = c->connector_id;
                }
                drmModeFreeConnector(c);
            }
            drmModeFreeResources(res);
            if (m_WbConnId == 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "No DRM writeback connector; scanout capture disabled");
                m_WbFailed = true;
                return false;
            }

            m_WbCrtcIdProp = wbProp("CRTC_ID");
            m_WbFbProp = wbProp("WRITEBACK_FB_ID");
            m_WbFenceProp = wbProp("WRITEBACK_OUT_FENCE_PTR");
            if (!m_WbCrtcIdProp || !m_WbFbProp || !m_WbFenceProp) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Writeback connector is a stub (missing properties)");
                m_WbFailed = true;
                return false;
            }

            // NV12 dumb buffer: luma plane then interleaved chroma, one
            // allocation, which is all the barcode reader needs (luma only).
            struct drm_mode_create_dumb creq = {};
            creq.width = width;
            creq.height = height * 3 / 2;
            creq.bpp = 8;
            if (drmIoctl(m_Fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
                m_WbFailed = true;
                return false;
            }
            uint32_t handles[4] = { creq.handle, creq.handle, 0, 0 };
            uint32_t pitches[4] = { creq.pitch, creq.pitch, 0, 0 };
            uint32_t offsets[4] = { 0, creq.pitch * height, 0, 0 };
            if (drmModeAddFB2(m_Fd, width, height, DRM_FORMAT_NV12,
                              handles, pitches, offsets, &m_WbFbId, 0) < 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Writeback AddFB2(NV12) failed: %d", errno);
                m_WbFailed = true;
                return false;
            }
            struct drm_mode_map_dumb mreq = {};
            mreq.handle = creq.handle;
            if (drmIoctl(m_Fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
                m_WbFailed = true;
                return false;
            }
            m_WbMap = (uint8_t*)mmap(nullptr, creq.size, PROT_READ, MAP_SHARED,
                                     m_Fd, mreq.offset);
            if (m_WbMap == MAP_FAILED) {
                m_WbMap = nullptr;
                m_WbFailed = true;
                return false;
            }
            m_WbHandle = creq.handle;
            m_WbSize = creq.size;
            m_WbPitch = creq.pitch;
            m_WbW = width;
            m_WbH = height;
            m_WbCrtcId = crtcId;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Writeback capture armed: connector %u, NV12 %ux%u",
                        m_WbConnId, width, height);
            return true;
        }

        uint32_t wbProp(const char* name) {
            uint32_t id = 0;
            drmModeObjectProperties* props =
                drmModeObjectGetProperties(m_Fd, m_WbConnId, DRM_MODE_OBJECT_CONNECTOR);
            if (props == nullptr) {
                return 0;
            }
            for (uint32_t i = 0; i < props->count_props && id == 0; i++) {
                drmModePropertyPtr p = drmModeGetProperty(m_Fd, props->props[i]);
                if (p == nullptr) {
                    continue;
                }
                if (SDL_strcmp(p->name, name) == 0) {
                    id = p->prop_id;
                }
                drmModeFreeProperty(p);
            }
            drmModeFreeObjectProperties(props);
            return id;
        }

        // Add the writeback attachment to the request that is about to commit.
        void wbArm() {
            if (m_WbFbId == 0 || m_WbFailed) {
                return;
            }
            m_WbFence = -1;
            set(m_WbConnId, DRM_MODE_OBJECT_CONNECTOR, m_WbCrtcIdProp, m_WbCrtcId);
            set(m_WbConnId, DRM_MODE_OBJECT_CONNECTOR, m_WbFbProp, m_WbFbId);
            set(m_WbConnId, DRM_MODE_OBJECT_CONNECTOR, m_WbFenceProp,
                (uint64_t)(uintptr_t)&m_WbFence);
            m_WbArmed = true;
        }

        // Wait for the capture and read the frame index out of it.
        void wbHarvest(bool commitOk) {
            if (!m_WbArmed) {
                return;
            }
            m_WbArmed = false;
            if (!commitOk) {
                // One rejection is enough: retrying every frame would turn a
                // harness feature into a permanent per-frame cost.
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Writeback commit rejected; scanout capture disabled");
                m_WbFailed = true;
                if (m_WbFence >= 0) {
                    close(m_WbFence);
                    m_WbFence = -1;
                }
                return;
            }
            if (m_WbFence >= 0) {
                struct pollfd pfd = { m_WbFence, POLLIN, 0 };
                poll(&pfd, 1, 100);
                close(m_WbFence);
                m_WbFence = -1;
            }
            long idx = fbcDecode(m_WbMap, (int)m_WbPitch, (int)m_WbW, (int)m_WbH, 1);
            if (idx >= 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ML-FRAMEWB,%llu,%ld",
                            (unsigned long long)LiGetMicroseconds(), idx);
            }
        }

        bool wbDue() {
            if (!m_WbWanted || m_WbFailed) {
                return false;
            }
            return (++m_WbCounter % m_WbInterval) == 0;
        }

        bool apply() {
            if (!m_Atomic) {
                return true;
            }

            drmModeAtomicReqPtr req = nullptr;
            std::unordered_map<uint32_t, PlaneBuffer> pendingBuffers;
            std::vector<uint32_t> damageBlobIds;

            {
                std::lock_guard lg { m_Lock };

                // Create property blobs for any pending FB damage clip rects
                for (auto &[planeId, pb] : m_PlaneBuffers) {
                    uint32_t damageBlob;

                    if (!pb.pendingFbDamageRects.empty()) {
                        int err = drmModeCreatePropertyBlob(m_Fd,
                                                            pb.pendingFbDamageRects.data(),
                                                            pb.pendingFbDamageRects.size() * sizeof(drm_mode_rect),
                                                            &damageBlob);
                        if (err < 0) {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                         "drmModeCreatePropertyBlob(FB_DAMAGE_CLIPS) failed: %d",
                                         err);
                            continue;
                        }

                        err = set(planeId, DRM_MODE_OBJECT_PLANE, pb.fbDamageClipsPropId, damageBlob);
                        if (err < 0) {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                         "Failed to set property 'FB_DAMAGE_CLIPS': %d",
                                         err);
                            // Fall-through
                        }

                        damageBlobIds.emplace_back(damageBlob);
                    }
                }

                // Take ownership of the current atomic request to commit it and
                // allow other threads to queue up changes for the next one.
                std::swap(req, m_AtomicReq);
                std::swap(pendingBuffers, m_PlaneBuffers);
            }

            if (!req) {
                // Nothing to apply
                return true;
            }

            // Try an async flip if requested
            int err = drmModeAtomicCommit(m_Fd, req,
                                          m_AsyncFlip ? DRM_MODE_PAGE_FLIP_ASYNC : DRM_MODE_ATOMIC_ALLOW_MODESET,
                                          nullptr);

            if (m_AsyncFlip) {
                if (err == 0) {
                    m_AsyncCommits++;
                }
                else {
                    // The driver may not support async flips (especially if we changed a non-FB_ID property),
                    // so try again with a regular flip if we get an error from the async flip attempt.
                    //
                    // We pass DRM_MODE_ATOMIC_ALLOW_MODESET because changing HDR state may require a modeset.
                    err = drmModeAtomicCommit(m_Fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
                }
            }

            wbHarvest(err == 0);

            if (err == 0) {
                m_TotalCommits++;
            }
            else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "drmModeAtomicCommit() failed: %d",
                             err);
            }

            // Free the damage clip blobs
            for (auto blobId : damageBlobIds) {
                drmModeDestroyPropertyBlob(m_Fd, blobId);
            }

            // Update the buffer state for any modified planes
            std::lock_guard lg { m_Lock };
            for (auto it = pendingBuffers.begin(); it != pendingBuffers.end(); it++) {
                if (err == 0 && it->second.modified) {
                    if (it->second.fbId) {
                        drmModeRmFB(m_Fd, it->second.fbId);
                        it->second.fbId = 0;
                    }
                    if (it->second.dumbBufferHandle) {
                        struct drm_mode_destroy_dumb destroyBuf = {};
                        destroyBuf.handle = it->second.dumbBufferHandle;
                        drmIoctl(m_Fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroyBuf);
                        it->second.dumbBufferHandle = 0;
                    }

                    // The pending buffers become the active buffers for this FB
                    auto &pb = m_PlaneBuffers[it->first];
                    pb.fbId = it->second.pendingFbId;
                    pb.dumbBufferHandle = it->second.pendingDumbBuffer;
                }
                else if (err < 0 || it->second.fbId || it->second.dumbBufferHandle) {
                    // Free the old pending buffers on a failed commit
                    if (it->second.pendingFbId) {
                        SDL_assert(err < 0);
                        drmModeRmFB(m_Fd, it->second.pendingFbId);
                    }
                    if (it->second.pendingDumbBuffer) {
                        SDL_assert(err < 0);
                        struct drm_mode_destroy_dumb destroyBuf = {};
                        destroyBuf.handle = it->second.pendingDumbBuffer;
                        drmIoctl(m_Fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroyBuf);
                    }

                    // This FB wasn't modified in this commit, so the current buffers stay around
                    auto &pb = m_PlaneBuffers[it->first];
                    pb.fbId = it->second.fbId;
                    pb.dumbBufferHandle = it->second.dumbBufferHandle;
                }

                // NB: We swapped in a new plane buffers map which will clear the modified value.
                // It's important that we don't try to clear it here because we might stomp on
                // a flipPlane() performed by another thread that queued up another modification.
            }

            drmModeAtomicFree(req);
            return err == 0;
        }

        bool isAtomic() {
            return m_Atomic;
        }

        void restoreToInitial(const DrmPropertyMap& object) {
            SDL_assert(m_Atomic);

            // Set all mutable properties back to their initial values
            for (auto&[id, prop] : object.properties()) {
                restorePropertyToInitial(prop);
            }
        }

        void restorePropertyToInitial(const DrmProperty& prop) {
            if (!prop.isImmutable()) {
                // We clamp() here because some DRM drivers actually initialize certain
                // properties (max bpc) to values outside the legal range. Obviously,
                // the kernel rejects these values when we later restore them back.
                set(prop, prop.clamp(prop.initialValue()));
            }
        }

    private:
        int m_Fd = -1;
        bool m_Atomic = false;
        bool m_AsyncFlip = false;
        std::recursive_mutex m_Lock;
        std::unordered_map<uint32_t, PlaneBuffer> m_PlaneBuffers;

        // Legacy context
        std::unordered_map<uint32_t, PlaneConfiguration> m_PlaneConfigs;

        // Atomic context
        drmModeAtomicReqPtr m_AtomicReq = nullptr;
        std::atomic<uint32_t> m_AsyncCommits {0};
        std::atomic<uint32_t> m_TotalCommits {0};
    };

public:
    DrmRenderer(AVHWDeviceType hwDeviceType = AV_HWDEVICE_TYPE_NONE, IFFmpegRenderer *backendRenderer = nullptr);
    virtual ~DrmRenderer() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    virtual bool prepareDecoderContextInGetFormat(AVCodecContext*, AVPixelFormat) override;
    virtual void prepareToRender() override;
    virtual void cleanupRenderContext() override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual enum AVPixelFormat getPreferredPixelFormat(int videoFormat) override;
    virtual bool isPixelFormatSupported(int videoFormat, AVPixelFormat pixelFormat) override;
    virtual int getRendererAttributes() override;
    virtual bool testRenderFrame(AVFrame* frame) override;
    virtual bool isDirectRenderingSupported() override;
    virtual int getDecoderColorspace() override;
    virtual void setHdrMode(bool enabled) override;
    virtual void notifyOverlayUpdated(Overlay::OverlayType type) override;
#ifdef HAVE_EGL
    virtual bool canExportEGL() override;
    virtual AVPixelFormat getEGLImagePixelFormat() override;
    virtual bool initializeEGL(EGLDisplay dpy, const EGLExtensions &ext) override;
    virtual ssize_t exportEGLImages(AVFrame *frame, EGLDisplay dpy, EGLImage images[EGL_MAX_PLANES]) override;
#endif

private:
    const char* getDrmColorEncodingValue(AVFrame* frame);
    const char* getDrmColorRangeValue(AVFrame* frame);
    bool mapSoftwareFrame(AVFrame* frame, AVDRMFrameDescriptor* mappedFrame);
    bool addFbForFrame(AVFrame* frame, uint32_t* newFbId, bool testMode);
    bool uploadSurfaceToFb(SDL_Surface *surface, uint32_t* handle, uint32_t* fbId);
    bool mapDumbBuffer(uint32_t handle, size_t size, void** mapping);
    bool createFbForDumbBuffer(struct drm_mode_create_dumb* createBuf, uint32_t* fbId);
    void enterOverlayCompositionMode();
    void blitOverlayToCompositionSurface(Overlay::OverlayType type, SDL_Surface* newSurface, SDL_Rect* overlayRect);
    static bool drmFormatMatchesVideoFormat(uint32_t drmFormat, int videoFormat);

    IFFmpegRenderer* m_BackendRenderer;
    SDL_Window* m_Window;
    bool m_DrmPrimeBackend;
    AVHWDeviceType m_HwDeviceType;
    AVBufferRef* m_HwContext;
    int m_DrmFd;
    bool m_DrmIsMaster;
    bool m_DrmStateModified;
    bool m_DrmSupportsModifiers;
    bool m_MustCloseDrmFd;
    bool m_SupportsDirectRendering;
    int m_VideoFormat;
    bool m_Vsync;
    DrmPropertyMap m_Encoder;
    DrmPropertyMap m_Connector;
    DrmPropertyMap m_Crtc;
    std::unordered_map<uint32_t, DrmPropertyMap> m_UnusedActivePlanes;
    DrmPropertyMap m_VideoPlane;
    uint64_t m_VideoPlaneZpos;
    DrmPropertyMap m_OverlayPlanes[Overlay::OverlayMax];
    DrmPropertySetter m_PropSetter;
    SDL_Surface* m_OverlayCompositionSurface;
    std::mutex m_OverlayLock;
    SDL_Rect m_OverlayRects[Overlay::OverlayMax];
    drmVersionPtr m_Version;
    uint32_t m_HdrOutputMetadataBlobId;
    SDL_Rect m_OutputRect;
    std::set<uint32_t> m_SupportedVideoPlaneFormats;

    static constexpr int k_SwFrameCount = 2;
    SwFrameMapper m_SwFrameMapper;
    int m_CurrentSwFrameIdx;
    struct {
        int width;
        int height;
        uint32_t format;

        uint32_t handle;
        uint32_t pitch;
        uint64_t size;
        uint8_t* mapping;
        int primeFd;
    } m_SwFrame[k_SwFrameCount];

#ifdef HAVE_EGL
    EglImageFactory m_EglImageFactory;
#endif
};

