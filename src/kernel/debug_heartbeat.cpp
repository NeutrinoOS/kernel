#include "debug_heartbeat.hpp"

#include <stddef.h>

#include "drivers/console/console.hpp"
#include "kernel/cmdline.hpp"

namespace {

constexpr size_t kHeartbeatSize = 24;
constexpr uint64_t kTicksPerColor = 128;
constexpr uint8_t kMemoryModelRgb = 1;

Framebuffer g_framebuffer{};
bool g_enabled = false;

uint64_t scale_component(uint8_t value, uint8_t bits) {
    if (bits == 0) {
        return 0;
    }
    if (bits >= 8) {
        return static_cast<uint64_t>(value) << (bits - 8);
    }
    uint32_t maximum = (1u << bits) - 1u;
    return (static_cast<uint32_t>(value) * maximum + 127u) / 255u;
}

uint64_t pack_color(uint32_t rgb) {
    uint8_t red = static_cast<uint8_t>((rgb >> 16) & 0xffu);
    uint8_t green = static_cast<uint8_t>((rgb >> 8) & 0xffu);
    uint8_t blue = static_cast<uint8_t>(rgb & 0xffu);
    return (scale_component(red, g_framebuffer.red_mask_size)
            << g_framebuffer.red_mask_shift) |
           (scale_component(green, g_framebuffer.green_mask_size)
            << g_framebuffer.green_mask_shift) |
           (scale_component(blue, g_framebuffer.blue_mask_size)
            << g_framebuffer.blue_mask_shift);
}

void draw(uint32_t rgb) {
    const size_t bytes_per_pixel = (g_framebuffer.bpp + 7u) / 8u;
    const size_t start_x = g_framebuffer.width - kHeartbeatSize;
    const uint64_t packed = pack_color(rgb);

    for (size_t y = 0; y < kHeartbeatSize; ++y) {
        volatile uint8_t* pixel = g_framebuffer.base + y * g_framebuffer.pitch +
                                  start_x * bytes_per_pixel;
        for (size_t x = 0; x < kHeartbeatSize; ++x) {
            for (size_t byte = 0; byte < bytes_per_pixel; ++byte) {
                pixel[byte] = static_cast<uint8_t>(packed >> (byte * 8u));
            }
            pixel += bytes_per_pixel;
        }
    }
}

}  // namespace

namespace debug_heartbeat {

void init(const Framebuffer& framebuffer) {
    const bool debug_enabled = kernel_cmdline::has_flag("DEBUG") ||
                               kernel_cmdline::has_value("DEBUG", "ON");
    if (!debug_enabled || framebuffer.base == nullptr ||
        framebuffer.width < kHeartbeatSize ||
        framebuffer.height < kHeartbeatSize ||
        framebuffer.memory_model != kMemoryModelRgb) {
        return;
    }

    const size_t bytes_per_pixel = (framebuffer.bpp + 7u) / 8u;
    const uint16_t storage_bits = static_cast<uint16_t>(bytes_per_pixel * 8u);
    const bool valid_masks =
        framebuffer.red_mask_size != 0 &&
        framebuffer.green_mask_size != 0 &&
        framebuffer.blue_mask_size != 0 &&
        static_cast<uint16_t>(framebuffer.red_mask_shift) +
                framebuffer.red_mask_size <= storage_bits &&
        static_cast<uint16_t>(framebuffer.green_mask_shift) +
                framebuffer.green_mask_size <= storage_bits &&
        static_cast<uint16_t>(framebuffer.blue_mask_shift) +
                framebuffer.blue_mask_size <= storage_bits;
    if (bytes_per_pixel == 0 || bytes_per_pixel > sizeof(uint32_t) ||
        !valid_masks || framebuffer.width > SIZE_MAX / bytes_per_pixel ||
        framebuffer.pitch < framebuffer.width * bytes_per_pixel) {
        return;
    }

    g_framebuffer = framebuffer;
    g_enabled = true;
    draw(0x00ff00u);
}

void tick(uint64_t scheduler_tick) {
    if (!g_enabled) {
        return;
    }

    uint8_t phase = static_cast<uint8_t>((scheduler_tick / kTicksPerColor) % 3u);
    constexpr uint32_t colors[] = {0x00ff00u, 0xffff00u, 0xff4000u};
    // The console owns a full-screen back buffer and may overwrite the marker
    // on any tick.  Redraw the marker every tick, even when the color phase
    // has not changed.
    draw(colors[phase]);
}

}  // namespace debug_heartbeat
