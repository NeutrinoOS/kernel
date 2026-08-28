#pragma once

#include <stddef.h>
#include <stdint.h>

namespace render_accel {
struct Surface {
    const uint64_t* physical_pages;
    size_t physical_page_count;
    size_t byte_length;
};

struct DeviceInfo {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t graphics_version;
    uint16_t graphics_version_minor;
    uint32_t engines;
    uint32_t capabilities;
};

using FillFn = bool (*)(const Surface&, uint64_t gpu_va, uint64_t byte_offset,
                        uint32_t pitch_bytes, uint32_t x, uint32_t y,
                        uint32_t width, uint32_t height, uint32_t color);
using BindFn = bool (*)(const Surface&, uint64_t& out_gpu_va);
using UnbindFn = void (*)(uint64_t gpu_va);
using QueryDeviceInfoFn = bool (*)(DeviceInfo&);
using DrawDemoFn = bool (*)(const Surface&, uint32_t pitch_bytes,
                            uint32_t width, uint32_t height);
using SyncFn = bool (*)(const Surface&, uint64_t byte_offset,
                        uint64_t byte_length, uint32_t flags);
struct Ops {
    FillFn fill;
    BindFn bind;
    UnbindFn unbind;
    QueryDeviceInfoFn query_device_info;
};

bool available();
bool fill(const Surface&, uint64_t gpu_va, uint64_t byte_offset, uint32_t pitch_bytes,
          uint32_t x, uint32_t y, uint32_t width, uint32_t height,
          uint32_t color);
bool bind(const Surface&, uint64_t& out_gpu_va);
void unbind(uint64_t gpu_va);
bool query_device_info(DeviceInfo&);
bool draw_demo(const Surface&, uint32_t pitch_bytes,
               uint32_t width, uint32_t height);
bool sync(const Surface&, uint64_t byte_offset, uint64_t byte_length,
          uint32_t flags);
}  // namespace render_accel

extern "C" bool neutrino_register_render_accelerator(
    const render_accel::Ops* ops);
extern "C" bool neutrino_register_render_demo_accelerator(
    render_accel::DrawDemoFn draw_demo);
extern "C" bool neutrino_register_render_sync_accelerator(
    render_accel::SyncFn sync);
