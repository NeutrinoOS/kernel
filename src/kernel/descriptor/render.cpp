#include "../descriptor.hpp"

#include "../../drivers/render_accel.hpp"
#include "../../lib/mem.hpp"
#include "../memory/physical_allocator.hpp"
#include "../process.hpp"
#include "../sync.hpp"
#include "../vm.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "neutrino_render.hpp"

namespace descriptor {
namespace render_descriptor {

constexpr size_t kPageSize = 0x1000;
constexpr size_t kMaxContexts = 4;
constexpr size_t kMaxBytes = 16ull * 1024 * 1024;
constexpr size_t kMaxPages = kMaxBytes / kPageSize;
constexpr uint64_t kKernelBase = 0xFFFFE30000000000ull;

struct Context {
    uint64_t pages[kMaxPages];
    uint8_t* kernel_base;
    uint64_t user_base;
    size_t byte_length;
    size_t page_count;
    process::Task* owner;
    uint64_t completed_fence;
    bool used;
};

Context g_contexts[kMaxContexts]{};
sync::SpinLock g_lock;

uint64_t kernel_base_for(const Context& context) {
    return kKernelBase + static_cast<uint64_t>(&context - g_contexts) * kMaxBytes;
}

void release(Context& context) {
    const uint64_t kernel_base = kernel_base_for(context);
    for (size_t i = 0; i < context.page_count; ++i) {
        uint64_t ignored = 0;
        (void)paging_unmap_page(kernel_base + i * kPageSize, ignored);
        if (context.pages[i] != 0) memory::free_user_page(context.pages[i]);
        context.pages[i] = 0;
    }
    context = {};
}

bool map_user(process::Task& proc, Context& context) {
    vm::Region region = vm::reserve_user_region(proc.cr3, context.byte_length,
                                                  vm::MappingKind::Device);
    if (region.base == 0 || region.length != context.byte_length) return false;
    for (size_t i = 0; i < context.page_count; ++i) {
        if (!paging_map_page_cr3(proc.cr3, region.base + i * kPageSize,
                                 context.pages[i], PAGE_FLAG_USER | PAGE_FLAG_WRITE |
                                                   PAGE_FLAG_NO_EXECUTE)) {
            for (size_t j = 0; j < i; ++j) {
                uint64_t ignored = 0;
                (void)paging_unmap_page_cr3(proc.cr3, region.base + j * kPageSize, ignored);
            }
            vm::release_external_region(proc.cr3, region);
            return false;
        }
    }
    if (!vm::mark_region_resident(proc.cr3, region, context.page_count)) {
        for (size_t i = 0; i < context.page_count; ++i) {
            uint64_t ignored = 0;
            (void)paging_unmap_page_cr3(proc.cr3, region.base + i * kPageSize, ignored);
        }
        vm::release_external_region(proc.cr3, region);
        return false;
    }
    context.user_base = region.base;
    return true;
}

bool allocate(process::Task& proc, Context& context, size_t requested) {
    if (requested == 0) requested = 4ull * 1024 * 1024;
    if (requested > kMaxBytes || requested > SIZE_MAX - (kPageSize - 1)) return false;
    const size_t pages = (requested + kPageSize - 1) / kPageSize;
    if (pages == 0 || memory::user_free_pages() < pages) return false;
    context.byte_length = pages * kPageSize;
    context.page_count = pages;
    const uint64_t kernel_base = kernel_base_for(context);
    for (size_t i = 0; i < pages; ++i) {
        uint64_t page = memory::alloc_user_page();
        if (page == 0 || !paging_map_page(kernel_base + i * kPageSize, page,
                                           PAGE_FLAG_WRITE | PAGE_FLAG_NO_EXECUTE)) {
            if (page != 0) memory::free_user_page(page);
            context.page_count = i;
            release(context);
            return false;
        }
        context.pages[i] = page;
    }
    context.kernel_base = reinterpret_cast<uint8_t*>(kernel_base);
    memset(context.kernel_base, 0, context.byte_length);
    if (!map_user(proc, context)) {
        release(context);
        return false;
    }
    return true;
}

bool validate_fill(const Context& context, const neutrino_render::Fill& fill) {
    if (fill.fence == 0 || fill.fence <= context.completed_fence || fill.flags != 0 ||
        fill.width == 0 || fill.height == 0 || fill.pitch_bytes == 0 ||
        (fill.pitch_bytes & 3u) != 0 || fill.x > UINT32_MAX / 4 ||
        fill.width > (UINT32_MAX / 4) || fill.x > fill.pitch_bytes / 4 ||
        fill.width > fill.pitch_bytes / 4 - fill.x ||
        fill.y > UINT64_MAX / fill.pitch_bytes ||
        fill.byte_offset > context.byte_length || (fill.byte_offset & (kPageSize - 1)) != 0) return false;
    const uint64_t first = fill.byte_offset + static_cast<uint64_t>(fill.y) * fill.pitch_bytes +
                           static_cast<uint64_t>(fill.x) * 4;
    const uint64_t row_bytes = static_cast<uint64_t>(fill.width) * 4;
    if (first > context.byte_length || row_bytes > context.byte_length - first ||
        fill.height - 1 > UINT64_MAX / fill.pitch_bytes) return false;
    const uint64_t last = first + static_cast<uint64_t>(fill.height - 1) * fill.pitch_bytes;
    return last <= context.byte_length && row_bytes <= context.byte_length - last;
}

void cpu_fill(Context& context, const neutrino_render::Fill& fill) {
    for (uint32_t row = 0; row < fill.height; ++row) {
        uint64_t offset = fill.byte_offset + static_cast<uint64_t>(fill.y + row) * fill.pitch_bytes +
                          static_cast<uint64_t>(fill.x) * 4;
        auto* pixel = reinterpret_cast<uint32_t*>(context.kernel_base + offset);
        for (uint32_t column = 0; column < fill.width; ++column) pixel[column] = fill.color_xrgb8888;
    }
}

int get_property(DescriptorEntry& entry, uint32_t property, void* out, size_t size) {
    auto* context = static_cast<Context*>(entry.object);
    if (context == nullptr || !context->used || out == nullptr ||
        property != static_cast<uint32_t>(neutrino_render::Property::Info) ||
        size < sizeof(neutrino_render::Info)) return -1;
    *static_cast<neutrino_render::Info*>(out) = {
        .abi_major = 1, .abi_minor = 0,
        .flags = neutrino_render::kInfoCpuFallback |
                 (render_accel::available() ? neutrino_render::kInfoGpuBlt : 0u),
        .virtual_base = context->user_base, .byte_length = context->byte_length,
        .completed_fence = context->completed_fence,
    };
    return 0;
}

int set_property(DescriptorEntry& entry, uint32_t property, const void* in, size_t size) {
    auto* context = static_cast<Context*>(entry.object);
    if (context == nullptr || !context->used || in == nullptr ||
        property != static_cast<uint32_t>(neutrino_render::Property::SubmitFill) ||
        size < sizeof(neutrino_render::Fill)) return -1;
    const auto& fill = *static_cast<const neutrino_render::Fill*>(in);
    g_lock.lock();
    if (!validate_fill(*context, fill)) { g_lock.unlock(); return -1; }
    const render_accel::Surface surface{context->pages, context->page_count, context->byte_length};
    if (!render_accel::fill(surface, fill.byte_offset, fill.pitch_bytes, fill.x, fill.y,
                            fill.width, fill.height, fill.color_xrgb8888)) cpu_fill(*context, fill);
    context->completed_fence = fill.fence;
    g_lock.unlock();
    return 0;
}

void close(DescriptorEntry& entry) {
    auto* context = static_cast<Context*>(entry.object);
    if (context == nullptr) return;
    g_lock.lock();
    if (context->used && context->owner != nullptr && entry.subsystem_data != nullptr) {
        for (size_t i = 0; i < context->page_count; ++i) {
            uint64_t ignored = 0;
            (void)paging_unmap_page_cr3(context->owner->cr3, context->user_base + i * kPageSize, ignored);
        }
        vm::release_external_region(context->owner->cr3, {context->user_base, context->byte_length});
        release(*context);
    }
    g_lock.unlock();
}

const Ops kOps{.read = nullptr, .write = nullptr, .get_property = get_property, .set_property = set_property};

bool open(process::Task& proc, uint64_t bytes, uint64_t flags, uint64_t context_flags, Allocation& alloc) {
    if (is_kernel_process(proc) || flags != 0 || context_flags != 0) return false;
    g_lock.lock();
    Context* context = nullptr;
    for (Context& candidate : g_contexts) if (!candidate.used) { context = &candidate; break; }
    if (context == nullptr || !allocate(proc, *context, static_cast<size_t>(bytes))) {
        g_lock.unlock(); return false;
    }
    context->owner = &proc;
    context->used = true;
    alloc = {.type = kTypeRender,
             .flags = static_cast<uint64_t>(Flag::Mappable) | static_cast<uint64_t>(Flag::Device),
             .extended_flags = 0, .has_extended_flags = false, .object = context,
             .subsystem_data = reinterpret_cast<void*>(context->user_base), .name = "render-blt",
             .ops = &kOps, .ext = nullptr, .close = close};
    g_lock.unlock();
    return true;
}
}  // namespace render_descriptor

bool register_render_descriptor() { return register_type(kTypeRender, render_descriptor::open, &render_descriptor::kOps); }
}  // namespace descriptor
