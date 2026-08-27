#include "display_accel.hpp"

namespace {

display_accel::Ops g_ops{};

}  // namespace

extern "C" bool neutrino_register_framebuffer_presenter(
    const display_accel::Ops* ops) {
    if (ops == nullptr || ops->present == nullptr || ops->fill == nullptr) {
        return false;
    }
    display_accel::PresentFn expected = nullptr;
    if (!__atomic_compare_exchange_n(&g_ops.present,
                                     &expected,
                                     ops->present,
                                       false,
                                       __ATOMIC_RELEASE,
                                       __ATOMIC_RELAXED)) {
        return false;
    }
    __atomic_store_n(&g_ops.fill, ops->fill, __ATOMIC_RELEASE);
    return true;
}

namespace display_accel {

bool available() {
    return __atomic_load_n(&g_ops.present, __ATOMIC_ACQUIRE) != nullptr;
}

bool present(const Surface& source,
             uint32_t x,
             uint32_t y,
             uint32_t width,
             uint32_t height) {
    PresentFn presenter = __atomic_load_n(&g_ops.present, __ATOMIC_ACQUIRE);
    if (presenter == nullptr || source.physical_pages == nullptr ||
        source.physical_page_count == 0 || source.byte_length == 0 ||
        source.width == 0 || source.height == 0 || source.pitch_bytes == 0 ||
        width == 0 || height == 0 || x >= source.width || y >= source.height ||
        width > source.width - x || height > source.height - y) {
        return false;
    }
    return presenter(source, x, y, width, height);
}

bool fill(const Surface& target,
          uint32_t x,
          uint32_t y,
          uint32_t width,
          uint32_t height,
          uint32_t color) {
    FillFn filler = __atomic_load_n(&g_ops.fill, __ATOMIC_ACQUIRE);
    if (filler == nullptr || target.physical_pages == nullptr ||
        target.physical_page_count == 0 || target.byte_length == 0 ||
        target.width == 0 || target.height == 0 || target.pitch_bytes == 0 ||
        width == 0 || height == 0 || x >= target.width || y >= target.height ||
        width > target.width - x || height > target.height - y) {
        return false;
    }
    return filler(target, x, y, width, height, color);
}

}  // namespace display_accel
