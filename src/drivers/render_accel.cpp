#include "render_accel.hpp"

namespace {
render_accel::Ops g_ops{};
render_accel::DrawDemoFn g_draw_demo = nullptr;
render_accel::SyncFn g_sync = nullptr;
uint32_t g_registration_state = 0;
}

extern "C" bool neutrino_register_render_accelerator(
    const render_accel::Ops* ops) {
    if (ops == nullptr || ops->fill == nullptr || ops->bind == nullptr ||
        ops->unbind == nullptr || ops->query_device_info == nullptr)
        return false;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&g_registration_state, &expected, 1,
                                     false, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED))
        return false;
    // Publish fill last: readers use it as the release/acquire indication that
    // the remaining immutable operation pointers are initialized.  The old
    // registration path never copied bind/unbind, silently forcing all render
    // descriptor BOs onto the CPU fallback path.
    g_ops.bind = ops->bind;
    g_ops.unbind = ops->unbind;
    g_ops.query_device_info = ops->query_device_info;
    __atomic_store_n(&g_ops.fill, ops->fill, __ATOMIC_RELEASE);
    return true;
}

extern "C" bool neutrino_register_render_demo_accelerator(
    render_accel::DrawDemoFn draw_demo) {
    if (draw_demo == nullptr ||
        __atomic_load_n(&g_ops.fill, __ATOMIC_ACQUIRE) == nullptr)
        return false;
    render_accel::DrawDemoFn expected = nullptr;
    return __atomic_compare_exchange_n(&g_draw_demo, &expected, draw_demo,
                                       false, __ATOMIC_RELEASE,
                                       __ATOMIC_RELAXED);
}

extern "C" bool neutrino_register_render_sync_accelerator(
    render_accel::SyncFn sync) {
    if (sync == nullptr ||
        __atomic_load_n(&g_ops.fill, __ATOMIC_ACQUIRE) == nullptr)
        return false;
    render_accel::SyncFn expected = nullptr;
    return __atomic_compare_exchange_n(&g_sync, &expected, sync, false,
                                       __ATOMIC_RELEASE,
                                       __ATOMIC_RELAXED);
}

namespace render_accel {
bool available() { return __atomic_load_n(&g_ops.fill, __ATOMIC_ACQUIRE) != nullptr; }
bool fill(const Surface& surface, uint64_t gpu_va, uint64_t offset, uint32_t pitch,
          uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) {
    FillFn fn = __atomic_load_n(&g_ops.fill, __ATOMIC_ACQUIRE);
    return fn != nullptr && surface.physical_pages != nullptr &&
           surface.physical_page_count != 0 && surface.byte_length != 0 &&
           gpu_va != 0 && fn(surface, gpu_va, offset, pitch, x, y, width, height, color);
}
bool bind(const Surface& surface, uint64_t& out_gpu_va) {
    out_gpu_va = 0;
    BindFn fn = __atomic_load_n(&g_ops.bind, __ATOMIC_ACQUIRE);
    return fn != nullptr && surface.physical_pages != nullptr &&
           surface.physical_page_count != 0 && fn(surface, out_gpu_va) &&
           out_gpu_va != 0;
}
void unbind(uint64_t gpu_va) {
    UnbindFn fn = __atomic_load_n(&g_ops.unbind, __ATOMIC_ACQUIRE);
    if (fn != nullptr && gpu_va != 0) fn(gpu_va);
}
bool query_device_info(DeviceInfo& info) {
    info = {};
    if (__atomic_load_n(&g_ops.fill, __ATOMIC_ACQUIRE) == nullptr)
        return false;
    QueryDeviceInfoFn fn = g_ops.query_device_info;
    return fn != nullptr && fn(info);
}
bool draw_demo(const Surface& surface, uint32_t pitch,
               uint32_t width, uint32_t height) {
    if (__atomic_load_n(&g_ops.fill, __ATOMIC_ACQUIRE) == nullptr)
        return false;
    DrawDemoFn fn = __atomic_load_n(&g_draw_demo, __ATOMIC_ACQUIRE);
    return fn != nullptr && surface.physical_pages != nullptr &&
           surface.physical_page_count != 0 && surface.byte_length != 0 &&
           fn(surface, pitch, width, height);
}
bool sync(const Surface& surface, uint64_t offset, uint64_t length,
          uint32_t flags) {
    SyncFn fn = __atomic_load_n(&g_sync, __ATOMIC_ACQUIRE);
    return fn != nullptr && surface.physical_pages != nullptr &&
           surface.physical_page_count != 0 && surface.byte_length != 0 &&
           fn(surface, offset, length, flags);
}
}  // namespace render_accel
