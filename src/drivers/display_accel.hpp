#pragma once

#include <stddef.h>
#include <stdint.h>

// A narrowly scoped kernel-to-display-driver interface.  The kernel owns the
// source pages; drivers receive only a read-only description and may decline
// any operation they cannot execute safely.
namespace display_accel {

struct Surface {
    const uint64_t* physical_pages;
    size_t physical_page_count;
    size_t byte_length;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_bytes;
    uint16_t bits_per_pixel;
};

using PresentFn = bool (*)(const Surface& source,
                           uint32_t x,
                           uint32_t y,
                           uint32_t width,
                           uint32_t height);
using FillFn = bool (*)(const Surface& target,
                        uint32_t x,
                        uint32_t y,
                        uint32_t width,
                        uint32_t height,
                        uint32_t color);

struct Ops {
    PresentFn present;
    FillFn fill;
};

bool present(const Surface& source,
             uint32_t x,
             uint32_t y,
             uint32_t width,
             uint32_t height);
bool fill(const Surface& target,
          uint32_t x,
          uint32_t y,
          uint32_t width,
          uint32_t height,
          uint32_t color);

// True only after a display driver has registered a presenter.  Callers must
// still handle a failed operation because drivers may reject an individual
// surface or damage rectangle.
bool available();

}  // namespace display_accel

// Kept as a C symbol so loadable display drivers can resolve it without
// depending on C++ name mangling or the kernel module ABI layout.
extern "C" bool neutrino_register_framebuffer_presenter(
    const display_accel::Ops* ops);
