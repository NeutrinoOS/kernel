#include "render_accel.hpp"

namespace { render_accel::Ops g_ops{}; }

extern "C" bool neutrino_register_render_accelerator(
    const render_accel::Ops* ops) {
    if (ops == nullptr || ops->fill == nullptr) return false;
    render_accel::FillFn expected = nullptr;
    return __atomic_compare_exchange_n(&g_ops.fill, &expected, ops->fill,
                                       false, __ATOMIC_RELEASE,
                                       __ATOMIC_RELAXED);
}

namespace render_accel {
bool available() { return __atomic_load_n(&g_ops.fill, __ATOMIC_ACQUIRE) != nullptr; }
bool fill(const Surface& surface, uint64_t offset, uint32_t pitch,
          uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) {
    FillFn fn = __atomic_load_n(&g_ops.fill, __ATOMIC_ACQUIRE);
    return fn != nullptr && surface.physical_pages != nullptr &&
           surface.physical_page_count != 0 && surface.byte_length != 0 &&
           fn(surface, offset, pitch, x, y, width, height, color);
}
}  // namespace render_accel
