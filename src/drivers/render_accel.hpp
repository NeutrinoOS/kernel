#pragma once

#include <stddef.h>
#include <stdint.h>

namespace render_accel {
struct Surface {
    const uint64_t* physical_pages;
    size_t physical_page_count;
    size_t byte_length;
};

using FillFn = bool (*)(const Surface&, uint64_t byte_offset,
                        uint32_t pitch_bytes, uint32_t x, uint32_t y,
                        uint32_t width, uint32_t height, uint32_t color);
struct Ops { FillFn fill; };

bool available();
bool fill(const Surface&, uint64_t byte_offset, uint32_t pitch_bytes,
          uint32_t x, uint32_t y, uint32_t width, uint32_t height,
          uint32_t color);
}  // namespace render_accel

extern "C" bool neutrino_register_render_accelerator(
    const render_accel::Ops* ops);
