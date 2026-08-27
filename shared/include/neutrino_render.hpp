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
    CreateBuffer = 0x00013003u,
    DestroyBuffer = 0x00013004u,
    BufferInfo = 0x00013005u,
    FenceInfo = 0x00013006u,
    SubmitFill2 = 0x00013007u,
    WaitFence = 0x00013008u,
    BufferInfo0 = 0x00013010u,
    BufferInfo1 = 0x00013011u,
    BufferInfo2 = 0x00013012u,
    BufferInfo3 = 0x00013013u,
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
struct Fill2 { uint32_t buffer_handle; uint32_t reserved; Fill fill; };
// Handles are chosen by the caller (1..4); setters are not in/out syscalls.
struct BufferCreate { uint32_t handle; uint32_t flags; uint64_t byte_length; };
struct BufferDestroy { uint32_t handle; uint32_t flags; };
struct BufferInfo { uint32_t handle; uint32_t flags; uint64_t virtual_base; uint64_t byte_length; uint64_t gpu_va; };
enum FenceState : uint32_t { kFencePending = 1, kFenceComplete = 2, kFenceFailed = 3, kFenceCancelled = 4 };
struct FenceInfo { uint64_t submitted_fence; uint64_t completed_fence; uint32_t state; int32_t error; };
struct FenceWait { uint64_t fence; uint64_t timeout_ticks; uint32_t flags; uint32_t reserved; };


static_assert(sizeof(Info) == 32, "render info ABI mismatch");
static_assert(sizeof(Fill) == 48, "render fill ABI mismatch");
static_assert(sizeof(Fill2) == 56, "render fill2 ABI mismatch");
static_assert(sizeof(BufferCreate) == 16, "render buffer-create ABI mismatch");
static_assert(sizeof(BufferInfo) == 32, "render buffer-info ABI mismatch");
static_assert(sizeof(FenceInfo) == 24, "render fence-info ABI mismatch");
static_assert(sizeof(FenceWait) == 24, "render fence-wait ABI mismatch");
}  // namespace neutrino_render
