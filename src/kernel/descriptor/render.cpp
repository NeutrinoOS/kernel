#include "../../drivers/render_accel.hpp"
#include "../../lib/mem.hpp"
#include "../descriptor.hpp"
#include "../memory/physical_allocator.hpp"
#include "../process.hpp"
#include "../sync.hpp"
#include "../vm.hpp"
#include "../work.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "neutrino_render.hpp"

namespace descriptor::render_descriptor {
constexpr size_t kPage = 0x1000, kContexts = 4, kBuffers = 4;
constexpr size_t kBufferBytes = 4ull * 1024 * 1024,
                 kBufferPages = kBufferBytes / kPage;
constexpr uint64_t kKernelBase = 0xFFFFE30000000000ull;
struct Buffer {
    uint64_t pages[kBufferPages];
    uint8_t* kernel;
    uint64_t user;
    uint64_t gpu_va;
    size_t bytes, count;
    bool used;
};
struct Context {
    Buffer buffers[kBuffers];
    process::Task* owner;
    uint64_t submitted, completed;
    uint32_t fence_state;
    int32_t fence_error;
    uint32_t jobs;
    bool used, closing;
};
struct Job {
    Context* context;
    uint32_t buffer;
    neutrino_render::Fill fill;
    bool demo;
    bool used;
};
Context g_contexts[kContexts]{};
Job g_jobs[16]{};
sync::SpinLock g_lock;
uint64_t kbase(const Context& c, size_t b) {
    return kKernelBase +
           (static_cast<uint64_t>(&c - g_contexts) * kBuffers + b) *
               kBufferBytes;
}
void release_buffer(Context& c, size_t bi) {
    Buffer& b = c.buffers[bi];
    if (b.gpu_va) render_accel::unbind(b.gpu_va);
    for (size_t i = 0; i < b.count; i++) {
        uint64_t x = 0;
        (void)paging_unmap_page(kbase(c, bi) + i * kPage, x);
        if (b.pages[i]) memory::free_user_page(b.pages[i]);
    }
    b = {};
}
void release_context(Context& c) {
    for (size_t i = 0; i < kBuffers; i++) release_buffer(c, i);
    c = {};
}
bool map_user(process::Task& p, Context& c, size_t bi) {
    Buffer& b = c.buffers[bi];
    vm::Region r =
        vm::reserve_user_region(p.cr3, b.bytes, vm::MappingKind::Device);
    if (!r.base || r.length != b.bytes) return false;
    for (size_t i = 0; i < b.count; i++)
        if (!paging_map_page_cr3(
                p.cr3, r.base + i * kPage, b.pages[i],
                PAGE_FLAG_USER | PAGE_FLAG_WRITE | PAGE_FLAG_NO_EXECUTE)) {
            for (size_t j = 0; j < i; j++) {
                uint64_t x = 0;
                (void)paging_unmap_page_cr3(p.cr3, r.base + j * kPage, x);
            }
            vm::release_external_region(p.cr3, r);
            return false;
        }
    if (!vm::mark_region_resident(p.cr3, r, b.count)) {
        for (size_t i = 0; i < b.count; i++) {
            uint64_t x = 0;
            (void)paging_unmap_page_cr3(p.cr3, r.base + i * kPage, x);
        }
        vm::release_external_region(p.cr3, r);
        return false;
    }
    b.user = r.base;
    return true;
}
bool allocate(process::Task& p, Context& c, size_t bi, size_t request) {
    if (bi >= kBuffers || c.buffers[bi].used) return false;
    if (!request) request = kBufferBytes;
    if (request > kBufferBytes || request > SIZE_MAX - (kPage - 1))
        return false;
    Buffer& b = c.buffers[bi];
    b.count = (request + kPage - 1) / kPage;
    b.bytes = b.count * kPage;
    if (!b.count || memory::user_free_pages() < b.count) return false;
    for (size_t i = 0; i < b.count; i++) {
        uint64_t pg = memory::alloc_user_page();
        if (!pg || !paging_map_page(kbase(c, bi) + i * kPage, pg,
                                    PAGE_FLAG_WRITE | PAGE_FLAG_NO_EXECUTE)) {
            if (pg) memory::free_user_page(pg);
            b.count = i;
            release_buffer(c, bi);
            return false;
        }
        b.pages[i] = pg;
    }
    b.kernel = reinterpret_cast<uint8_t*>(kbase(c, bi));
    memset(b.kernel, 0, b.bytes);
    b.used = true;
    const render_accel::Surface surface{b.pages, b.count, b.bytes};
    (void)render_accel::bind(surface, b.gpu_va);
    if (!map_user(p, c, bi)) {
        release_buffer(c, bi);
        return false;
    }
    return true;
}
bool valid(const Buffer& b, const neutrino_render::Fill& f, uint64_t prior) {
    if (!f.fence || f.fence <= prior || f.flags || !f.width || !f.height ||
        !f.pitch_bytes || (f.pitch_bytes & 3) || f.x > f.pitch_bytes / 4 ||
        f.width > f.pitch_bytes / 4 - f.x || f.byte_offset > b.bytes ||
        (f.byte_offset & (kPage - 1)))
        return false;
    uint64_t first = f.byte_offset + (uint64_t)f.y * f.pitch_bytes +
                     (uint64_t)f.x * 4,
             row = (uint64_t)f.width * 4;
    if (first > b.bytes || row > b.bytes - first ||
        f.height - 1 > UINT64_MAX / f.pitch_bytes)
        return false;
    uint64_t last = first + (uint64_t)(f.height - 1) * f.pitch_bytes;
    return last <= b.bytes && row <= b.bytes - last;
}
void cpu(Buffer& b, const neutrino_render::Fill& f) {
    for (uint32_t y = 0; y < f.height; y++) {
        auto* p = reinterpret_cast<uint32_t*>(
            b.kernel + f.byte_offset + (uint64_t)(f.y + y) * f.pitch_bytes +
            (uint64_t)f.x * 4);
        for (uint32_t x = 0; x < f.width; x++) p[x] = f.color_xrgb8888;
    }
}
void complete_job(void* raw) {
    auto* j = static_cast<Job*>(raw);
    g_lock.lock();
    if (!j->used) {
        g_lock.unlock();
        return;
    }
    Context& c = *j->context;
    Buffer& b = c.buffers[j->buffer];
    if (c.closing) {
        c.fence_state = neutrino_render::kFenceCancelled;
        c.fence_error = -1;
    } else {
        render_accel::Surface s{b.pages, b.count, b.bytes};
        bool completed = true;
        if (j->demo) {
            completed = render_accel::draw_demo(
                s, j->fill.pitch_bytes, j->fill.width, j->fill.height);
        } else if (!render_accel::fill(
                       s, b.gpu_va, j->fill.byte_offset,
                       j->fill.pitch_bytes, j->fill.x, j->fill.y,
                       j->fill.width, j->fill.height,
                       j->fill.color_xrgb8888)) {
            cpu(b, j->fill);
        }
        if (completed) {
            c.fence_state = neutrino_render::kFenceComplete;
            c.fence_error = 0;
            c.completed = j->fill.fence;
        } else {
            c.fence_state = neutrino_render::kFenceFailed;
            c.fence_error = -1;
        }
    }
    if (c.jobs) --c.jobs;
    j->used = false;
    if (c.closing && !c.jobs) release_context(c);
    g_lock.unlock();
}
int get(DescriptorEntry& e, uint32_t prop, void* out, size_t n) {
    auto* c = static_cast<Context*>(e.object);
    if (!c || !c->used || !out) return -1;
    if (prop == (uint32_t)neutrino_render::Property::Info &&
        n >= sizeof(neutrino_render::Info)) {
        Buffer& b = c->buffers[0];
        *static_cast<neutrino_render::Info*>(out) = {
            neutrino_render::kAbiMajor,
            neutrino_render::kAbiMinor,
            neutrino_render::kInfoCpuFallback |
                (render_accel::available() ? neutrino_render::kInfoGpuBlt : 0u),
            b.user,
            b.bytes,
            c->completed};
        return 0;
    }
    if (prop == (uint32_t)neutrino_render::Property::DeviceInfo &&
        n >= sizeof(neutrino_render::DeviceInfo)) {
        render_accel::DeviceInfo device{};
        const bool present = render_accel::query_device_info(device);
        *static_cast<neutrino_render::DeviceInfo*>(out) = {
            neutrino_render::kAbiMajor,
            neutrino_render::kAbiMinor,
            present ? device.vendor_id : static_cast<uint16_t>(0),
            present ? device.device_id : static_cast<uint16_t>(0),
            present ? device.graphics_version : static_cast<uint16_t>(0),
            present ? device.graphics_version_minor : static_cast<uint16_t>(0),
            present ? device.engines : 0u,
            present ? device.capabilities : 0u,
            static_cast<uint32_t>(kBuffers),
            static_cast<uint64_t>(kBufferBytes),
        };
        return 0;
    }
    if (prop >= (uint32_t)neutrino_render::Property::BufferInfo0 &&
        prop <= (uint32_t)neutrino_render::Property::BufferInfo3 &&
        n >= sizeof(neutrino_render::BufferInfo)) {
        size_t i = prop - (uint32_t)neutrino_render::Property::BufferInfo0;
        Buffer& b = c->buffers[i];
        if (!b.used) return -1;
        *static_cast<neutrino_render::BufferInfo*>(out) = {
            static_cast<uint32_t>(i + 1), 0, b.user, b.bytes, b.gpu_va};
        return 0;
    }
    if (prop == (uint32_t)neutrino_render::Property::FenceInfo &&
        n >= sizeof(neutrino_render::FenceInfo)) {
        *static_cast<neutrino_render::FenceInfo*>(out) = {
            c->submitted, c->completed, c->fence_state, c->fence_error};
        return 0;
    }
    return -1;
}
int submit(Context& c, uint32_t bi, const neutrino_render::Fill& f) {
    if (bi >= kBuffers || !c.buffers[bi].used ||
        !valid(c.buffers[bi], f, c.submitted))
        return -1;
    Job* j = nullptr;
    for (auto& x : g_jobs)
        if (!x.used) {
            j = &x;
            break;
        }
    if (!j) return -1;
    j->used = true;
    j->context = &c;
    j->buffer = bi;
    j->fill = f;
    j->demo = false;
    c.submitted = f.fence;
    c.fence_state = neutrino_render::kFencePending;
    c.fence_error = 0;
    c.jobs++;
    if (!work::schedule(complete_job, j)) {
        j->used = false;
        c.jobs--;
        c.fence_state = neutrino_render::kFenceFailed;
        c.fence_error = -1;
        return -1;
    }
    return 0;
}
int submit_demo(Context& c, const neutrino_render::DemoDraw& q) {
    if (!q.fence || q.fence <= c.submitted || q.flags || q.reserved ||
        q.buffer_handle == 0 || q.buffer_handle > kBuffers ||
        q.pitch_bytes != 64u * sizeof(uint32_t) ||
        q.width != 64 || q.height != 64)
        return -1;
    const uint32_t bi = q.buffer_handle - 1;
    Buffer& b = c.buffers[bi];
    if (!b.used || b.bytes < static_cast<uint64_t>(q.pitch_bytes) * q.height)
        return -1;
    Job* j = nullptr;
    for (auto& x : g_jobs)
        if (!x.used) {
            j = &x;
            break;
        }
    if (!j) return -1;
    j->used = true;
    j->demo = true;
    j->context = &c;
    j->buffer = bi;
    j->fill = {q.fence, 0, q.pitch_bytes, 0, 0, q.width, q.height, 0, 0};
    c.submitted = q.fence;
    c.fence_state = neutrino_render::kFencePending;
    c.fence_error = 0;
    ++c.jobs;
    if (!work::schedule(complete_job, j)) {
        j->used = false;
        --c.jobs;
        c.fence_state = neutrino_render::kFenceFailed;
        c.fence_error = -1;
        return -1;
    }
    return 0;
}
int wait_fence(Context& c, const neutrino_render::FenceWait& request) {
    if (!request.fence || request.flags || request.reserved) return -1;
    // Descriptor property handlers run inside the caller's syscall and must
    // never spin waiting for deferred work. In particular, render completion
    // is serviced by a CPU-0 worker which the caller could otherwise starve.
    // libdrm performs the blocking policy in userspace by polling FenceInfo
    // and sleeping, while this legacy property remains a nonblocking check.
    g_lock.lock();
    const bool submitted = request.fence <= c.submitted;
    const bool done = c.completed >= request.fence;
    g_lock.unlock();
    return submitted && done ? 0 : -1;
}
int sync_buffer(Context& c, const neutrino_render::BufferSync& request) {
    constexpr uint32_t kValidFlags =
        neutrino_render::kBufferSyncCpuToDevice |
        neutrino_render::kBufferSyncDeviceToCpu;
    if (request.buffer_handle == 0 || request.buffer_handle > kBuffers ||
        request.flags == 0 || (request.flags & ~kValidFlags) != 0 ||
        request.byte_length == 0 || c.jobs != 0)
        return -1;
    Buffer& buffer = c.buffers[request.buffer_handle - 1];
    if (!buffer.used || request.byte_offset > buffer.bytes ||
        request.byte_length > buffer.bytes - request.byte_offset)
        return -1;
    const render_accel::Surface surface{
        buffer.pages, buffer.count, buffer.bytes};
    return render_accel::sync(surface, request.byte_offset,
                              request.byte_length, request.flags)
               ? 0
               : -1;
}
int set(DescriptorEntry& e, uint32_t prop, const void* in, size_t n) {
    auto* c = static_cast<Context*>(e.object);
    if (!c || !c->used || c->closing || !in) return -1;
  if (prop == (uint32_t)neutrino_render::Property::WaitFence &&
      n >= sizeof(neutrino_render::FenceWait))
    return wait_fence(*c, *static_cast<const neutrino_render::FenceWait*>(in));
  g_lock.lock();
    int r = -1;
    if (prop == (uint32_t)neutrino_render::Property::SubmitFill &&
        n >= sizeof(neutrino_render::Fill))
        r = submit(*c, 0, *static_cast<const neutrino_render::Fill*>(in));
    else if (prop == (uint32_t)neutrino_render::Property::CreateBuffer &&
             n >= sizeof(neutrino_render::BufferCreate)) {
        auto& q = *static_cast<const neutrino_render::BufferCreate*>(in);
        if (!q.flags && q.handle > 0 && q.handle <= kBuffers)
            r = allocate(*c->owner, *c, q.handle - 1, (size_t)q.byte_length)
                    ? 0
                    : -1;
    } else if (prop == (uint32_t)neutrino_render::Property::DestroyBuffer &&
               n >= sizeof(neutrino_render::BufferDestroy)) {
        auto& q = *static_cast<const neutrino_render::BufferDestroy*>(in);
        size_t i = q.handle ? q.handle - 1 : kBuffers;
        if (!q.flags && q.handle > 1 && i < kBuffers && c->buffers[i].used &&
            !c->jobs) {
            Buffer& b = c->buffers[i];
            for (size_t p = 0; p < b.count; p++) {
                uint64_t x = 0;
                (void)paging_unmap_page_cr3(c->owner->cr3, b.user + p * kPage,
                                            x);
            }
            vm::release_external_region(c->owner->cr3, {b.user, b.bytes});
            release_buffer(*c, i);
            r = 0;
        }
    } else if (prop == (uint32_t)neutrino_render::Property::SubmitFill2 &&
               n >= sizeof(neutrino_render::Fill2)) {
        auto& q = *static_cast<const neutrino_render::Fill2*>(in);
        if (!q.reserved && q.buffer_handle > 0 && q.buffer_handle <= kBuffers)
            r = submit(*c, q.buffer_handle - 1, q.fill);
    } else if (prop == (uint32_t)neutrino_render::Property::SubmitDemo &&
               n >= sizeof(neutrino_render::DemoDraw)) {
        r = submit_demo(*c,
                        *static_cast<const neutrino_render::DemoDraw*>(in));
    } else if (prop == (uint32_t)neutrino_render::Property::SyncBuffer &&
               n >= sizeof(neutrino_render::BufferSync)) {
        r = sync_buffer(*c,
                        *static_cast<const neutrino_render::BufferSync*>(in));
    }
    g_lock.unlock();
    return r;
}
void close(DescriptorEntry& e) {
    auto* c = static_cast<Context*>(e.object);
    if (!c) return;
    g_lock.lock();
    if (c->used) {
        c->closing = true;
        for (size_t bi = 0; bi < kBuffers; bi++) {
            Buffer& b = c->buffers[bi];
            if (!b.used) continue;
            for (size_t i = 0; i < b.count; i++) {
                uint64_t x = 0;
                (void)paging_unmap_page_cr3(c->owner->cr3, b.user + i * kPage,
                                            x);
            }
            vm::release_external_region(c->owner->cr3, {b.user, b.bytes});
            b.user = 0;
        }
        if (!c->jobs) release_context(*c);
    }
    g_lock.unlock();
}
const Ops kOps{nullptr, nullptr, get, set};
bool open(process::Task& p, uint64_t bytes, uint64_t flags, uint64_t ctx,
          Allocation& a) {
    if (is_kernel_process(p) || flags || ctx) return false;
    g_lock.lock();
    Context* c = nullptr;
    for (auto& x : g_contexts)
        if (!x.used) {
            c = &x;
            break;
        }
    if (!c || !allocate(p, *c, 0, (size_t)bytes)) {
        g_lock.unlock();
        return false;
    }
    c->owner = &p;
    c->used = true;
    c->fence_state = neutrino_render::kFenceComplete;
    a = {kTypeRender,
         (uint64_t)Flag::Mappable | (uint64_t)Flag::Device,
         0,
         false,
         c,
         reinterpret_cast<void*>(c->buffers[0].user),
         "render-blt",
         &kOps,
         nullptr,
         close};
    g_lock.unlock();
    return true;
}
}  // namespace descriptor::render_descriptor
namespace descriptor {
bool register_render_descriptor() {
    return register_type(kTypeRender, render_descriptor::open,
                         &render_descriptor::kOps);
}
}  // namespace descriptor
