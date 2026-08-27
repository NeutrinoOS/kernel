#pragma once

#include <stdint.h>

// Neutrino DRM/KMS descriptor ABI.  This is deliberately small: one card,
// connector, CRTC and primary plane are exposed for the boot display.  Buffer
// storage is always CPU-mappable; scanout may be accelerated by a registered
// display driver.
namespace neutrino_drm {

constexpr uint32_t kDescriptorType = 0x012u;
constexpr uint32_t kConnectorId = 1u;
constexpr uint32_t kCrtcId = 1u;
constexpr uint32_t kPrimaryPlaneId = 1u;
constexpr uint32_t kFramebufferId = 1u;
constexpr uint32_t kDumbHandle = 1u;

enum class Property : uint32_t {
    DeviceInfo = 0x00012001u,
    ModeInfo = 0x00012002u,
    SetCrtc = 0x00012003u,
    Present = 0x00012004u,
};

enum DeviceFlag : uint32_t {
    kDeviceCpuPresent = 1u << 0,
    kDeviceGpuPresent = 1u << 1,
};

struct DeviceInfo {
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t primary_plane_id;
    uint32_t framebuffer_id;
    uint32_t dumb_handle;
};

struct ModeInfo {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;  // DRM_FORMAT_XRGB8888 where supported.
    uint32_t vrefresh_millihz;
    uint32_t flags;
};

struct SetCrtc {
    uint32_t crtc_id;
    uint32_t framebuffer_id;
    uint32_t connector_id;
    uint32_t flags;
    ModeInfo mode;
};

struct Rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

static_assert(sizeof(DeviceInfo) == 28, "DRM device ABI mismatch");
static_assert(sizeof(ModeInfo) == 24, "DRM mode ABI mismatch");
static_assert(sizeof(SetCrtc) == 40, "DRM CRTC ABI mismatch");
static_assert(sizeof(Rect) == 16, "DRM damage ABI mismatch");

}  // namespace neutrino_drm
