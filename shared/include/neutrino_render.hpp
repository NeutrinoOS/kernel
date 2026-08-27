#pragma once

#include <stdint.h>

// Version 1 is intentionally semantic rather than a raw command-stream ABI.
// It proves buffer ownership, mapping, fences, and command validation without
// exposing an attack surface that can program arbitrary GPU state.
namespace neutrino_render {
constexpr uint32_t kDescriptorType = 0x013u;

enum class Property : uint32_t {
    Info = 0x00013001u,
    SubmitFill = 0x00013002u,
};

enum InfoFlag : uint32_t {
    kInfoCpuFallback = 1u << 0,
    kInfoGpuBlt = 1u << 1,
};

struct Info {
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;
    uint64_t virtual_base;
    uint64_t byte_length;
    uint64_t completed_fence;
};

struct Fill {
    uint64_t fence;
    uint64_t byte_offset;
    uint32_t pitch_bytes;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t color_xrgb8888;
    uint32_t flags;
};


static_assert(sizeof(Info) == 32, "render info ABI mismatch");
static_assert(sizeof(Fill) == 48, "render fill ABI mismatch");
}  // namespace neutrino_render
