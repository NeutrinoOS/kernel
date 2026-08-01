#include "../descriptor.hpp"

#include "../../drivers/log/logging.hpp"
#include "../../lib/mem.hpp"
#include "../process.hpp"
#include "../random.hpp"
#include "../scheduler.hpp"
#include "../sync.hpp"
#include "../vm.hpp"

namespace descriptor {

namespace descriptor_pipe {

constexpr size_t kPipeBufferSize = 65536;
constexpr size_t kMaxPipes = 64;
constexpr size_t kMaxPipeWaiters = 128;

struct PipeWaiter {
    process::Task* proc;
    uint64_t user_address;
    uint64_t length;
    bool is_read;
    bool in_use;
    PipeWaiter* next;
};

struct Pipe {
    uint8_t buffer[kPipeBufferSize];
    size_t head;
    size_t tail;
    size_t count;
    size_t reader_count;
    size_t writer_count;
    size_t refcount;
    bool in_use;
    volatile int lock;
    PipeWaiter* read_waiters;
    PipeWaiter* write_waiters;
    uint32_t id;
};

struct PipeEndpoint {
    Pipe* pipe;
    process::Task* owner;
    bool can_read;
    bool can_write;
    bool in_use;
};

Pipe g_pipes[kMaxPipes]{};
PipeEndpoint g_pipe_endpoints[kMaxPipes * 2]{};
PipeWaiter g_pipe_waiters[kMaxPipeWaiters]{};
sync::SpinLock g_pipe_pool_lock;

inline size_t min_size(size_t a, size_t b) {
    return (a < b) ? a : b;
}

uint64_t lock_pipe(Pipe& pipe) {
    uint64_t flags = sync::disable_interrupts();
    while (__atomic_test_and_set(&pipe.lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
    return flags;
}

void unlock_pipe(Pipe& pipe, uint64_t flags) {
    __atomic_clear(&pipe.lock, __ATOMIC_RELEASE);
    sync::restore_interrupts(flags);
}

bool lock_endpoint_pipe(PipeEndpoint* endpoint,
                        bool require_read,
                        bool require_write,
                        Pipe*& out_pipe,
                        uint64_t& out_irq_flags) {
    out_pipe = nullptr;
    out_irq_flags = sync::disable_interrupts();
    g_pipe_pool_lock.lock();
    if (endpoint == nullptr ||
        !__atomic_load_n(&endpoint->in_use, __ATOMIC_ACQUIRE) ||
        endpoint->pipe == nullptr ||
        !__atomic_load_n(&endpoint->pipe->in_use, __ATOMIC_ACQUIRE) ||
        (require_read && !endpoint->can_read) ||
        (require_write && !endpoint->can_write)) {
        g_pipe_pool_lock.unlock();
        sync::restore_interrupts(out_irq_flags);
        return false;
    }

    Pipe* pipe = endpoint->pipe;
    while (__atomic_test_and_set(&pipe->lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
    // Lifecycle is pinned by the pipe lock before the pool lock is released.
    g_pipe_pool_lock.unlock();
    out_pipe = pipe;
    return true;
}

Pipe* find_pipe_by_id_locked(uint32_t id);

Pipe* allocate_pipe_locked() {
    for (auto& pipe : g_pipes) {
        if (__atomic_load_n(&pipe.in_use, __ATOMIC_ACQUIRE)) {
            continue;
        }
        pipe.head = 0;
        pipe.tail = 0;
        pipe.count = 0;
        pipe.reader_count = 0;
        pipe.writer_count = 0;
        pipe.refcount = 0;
        pipe.read_waiters = nullptr;
        pipe.write_waiters = nullptr;
        pipe.id = 0;
        uint32_t candidate = 0;
        do {
            candidate = kernel_random::opaque_id();
        } while (candidate == 0 || find_pipe_by_id_locked(candidate) != nullptr);
        pipe.id = candidate;
        // count is zero, so stale buffer bytes are unreachable. Publish only
        // after the complete pipe identity and counters are initialized.
        __atomic_store_n(&pipe.in_use, true, __ATOMIC_RELEASE);
        return &pipe;
    }
    return nullptr;
}

PipeEndpoint* allocate_pipe_endpoint_locked(Pipe* pipe,
                                            process::Task* owner,
                                            bool can_read,
                                            bool can_write) {
    for (auto& endpoint : g_pipe_endpoints) {
        if (__atomic_load_n(&endpoint.in_use, __ATOMIC_ACQUIRE)) {
            continue;
        }
        endpoint.pipe = pipe;
        endpoint.owner = owner;
        endpoint.can_read = can_read;
        endpoint.can_write = can_write;
        __atomic_store_n(&endpoint.in_use, true, __ATOMIC_RELEASE);
        return &endpoint;
    }
    return nullptr;
}

Pipe* find_pipe_by_id_locked(uint32_t id) {
    if (id == 0) {
        return nullptr;
    }
    for (auto& pipe : g_pipes) {
        if (!__atomic_load_n(&pipe.in_use, __ATOMIC_ACQUIRE)) {
            continue;
        }
        if (pipe.id == id) {
            return &pipe;
        }
    }
    return nullptr;
}

void release_pipe_endpoint_locked(PipeEndpoint* endpoint) {
    if (endpoint == nullptr) {
        return;
    }
    endpoint->pipe = nullptr;
    endpoint->owner = nullptr;
    endpoint->can_read = false;
    endpoint->can_write = false;
    __atomic_store_n(&endpoint->in_use, false, __ATOMIC_RELEASE);
}

PipeWaiter* allocate_pipe_waiter() {
    for (auto& waiter : g_pipe_waiters) {
        bool expected = false;
        if (!__atomic_compare_exchange_n(&waiter.in_use,
                                         &expected,
                                         true,
                                         false,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            continue;
        }
        waiter.proc = nullptr;
        waiter.user_address = 0;
        waiter.length = 0;
        waiter.is_read = false;
        waiter.next = nullptr;
        return &waiter;
    }
    return nullptr;
}

void release_pipe_waiter(PipeWaiter* waiter) {
    if (waiter == nullptr) {
        return;
    }
    waiter->proc = nullptr;
    waiter->user_address = 0;
    waiter->length = 0;
    waiter->is_read = false;
    waiter->next = nullptr;
    __atomic_store_n(&waiter->in_use, false, __ATOMIC_RELEASE);
}

void push_waiter(PipeWaiter*& head, PipeWaiter* waiter) {
    if (head == nullptr) {
        head = waiter;
        return;
    }
    PipeWaiter* cur = head;
    while (cur->next != nullptr) {
        cur = cur->next;
    }
    cur->next = waiter;
}

void complete_waiter(PipeWaiter* waiter, int64_t result) {
    if (waiter == nullptr || waiter->proc == nullptr) {
        release_pipe_waiter(waiter);
        return;
    }
    (void)process::wake_with_result(*waiter->proc, result);
    release_pipe_waiter(waiter);
}

size_t pipe_copy_out(Pipe& pipe, uint8_t* dest, size_t max_bytes) {
    size_t copied = 0;
    while (copied < max_bytes && pipe.count > 0) {
        size_t chunk = min_size(
            min_size(max_bytes - copied, pipe.count),
            kPipeBufferSize - pipe.head);
        for (size_t i = 0; i < chunk; ++i) {
            dest[copied + i] = pipe.buffer[pipe.head + i];
        }
        pipe.head = (pipe.head + chunk) % kPipeBufferSize;
        pipe.count -= chunk;
        copied += chunk;
    }
    return copied;
}

size_t pipe_copy_in(Pipe& pipe, const uint8_t* src, size_t max_bytes) {
    size_t copied = 0;
    while (copied < max_bytes && pipe.count < kPipeBufferSize) {
        size_t space = kPipeBufferSize - pipe.count;
        size_t chunk = min_size(
            min_size(max_bytes - copied, space),
            kPipeBufferSize - pipe.tail);
        for (size_t i = 0; i < chunk; ++i) {
            pipe.buffer[pipe.tail + i] = src[copied + i];
        }
        pipe.tail = (pipe.tail + chunk) % kPipeBufferSize;
        pipe.count += chunk;
        copied += chunk;
    }
    return copied;
}

int64_t pipe_copy_out_to_user(Pipe& pipe,
                              process::Task& proc,
                              uint64_t user_address,
                              size_t max_bytes) {
    if (user_address == 0 || proc.cr3 == 0) {
        return -1;
    }
    size_t copied = 0;
    while (copied < max_bytes && pipe.count > 0) {
        size_t chunk = min_size(
            min_size(max_bytes - copied, pipe.count),
            kPipeBufferSize - pipe.head);
        if (!vm::copy_to_user(proc.cr3,
                              user_address + copied,
                              pipe.buffer + pipe.head,
                              chunk)) {
            return (copied > 0) ? static_cast<int64_t>(copied) : -1;
        }
        pipe.head = (pipe.head + chunk) % kPipeBufferSize;
        pipe.count -= chunk;
        copied += chunk;
    }
    return static_cast<int64_t>(copied);
}

int64_t pipe_copy_in_from_user(Pipe& pipe,
                               process::Task& proc,
                               uint64_t user_address,
                               size_t max_bytes) {
    if (user_address == 0 || proc.cr3 == 0) {
        return -1;
    }
    size_t copied = 0;
    while (copied < max_bytes && pipe.count < kPipeBufferSize) {
        size_t space = kPipeBufferSize - pipe.count;
        size_t chunk = min_size(
            min_size(max_bytes - copied, space),
            kPipeBufferSize - pipe.tail);
        if (!vm::copy_from_user(proc.cr3,
                                pipe.buffer + pipe.tail,
                                user_address + copied,
                                chunk)) {
            return (copied > 0) ? static_cast<int64_t>(copied) : -1;
        }
        pipe.tail = (pipe.tail + chunk) % kPipeBufferSize;
        pipe.count += chunk;
        copied += chunk;
    }
    return static_cast<int64_t>(copied);
}

void drop_waiters_for_owner_locked(Pipe& pipe, process::Task* owner) {
    PipeWaiter* prev = nullptr;
    PipeWaiter* cur = pipe.read_waiters;
    while (cur != nullptr) {
        PipeWaiter* next = cur->next;
        if (cur->proc == owner) {
            if (prev == nullptr) {
                pipe.read_waiters = next;
            } else {
                prev->next = next;
            }
            cur->next = nullptr;
            complete_waiter(cur, -1);
        } else {
            prev = cur;
        }
        cur = next;
    }

    prev = nullptr;
    cur = pipe.write_waiters;
    while (cur != nullptr) {
        PipeWaiter* next = cur->next;
        if (cur->proc == owner) {
            if (prev == nullptr) {
                pipe.write_waiters = next;
            } else {
                prev->next = next;
            }
            cur->next = nullptr;
            complete_waiter(cur, -1);
        } else {
            prev = cur;
        }
        cur = next;
    }
}

void wake_read_waiters_locked(Pipe& pipe) {
    while (pipe.read_waiters != nullptr) {
        PipeWaiter* waiter = pipe.read_waiters;
        if (pipe.count == 0 && pipe.writer_count == 0) {
            pipe.read_waiters = waiter->next;
            waiter->next = nullptr;
            complete_waiter(waiter, 0);
            continue;
        }
        if (pipe.count == 0) {
            break;
        }
        if (waiter->proc == nullptr) {
            pipe.read_waiters = waiter->next;
            waiter->next = nullptr;
            complete_waiter(waiter, -1);
            continue;
        }
        int64_t copied =
            pipe_copy_out_to_user(pipe,
                                  *waiter->proc,
                                  waiter->user_address,
                                  static_cast<size_t>(waiter->length));
        pipe.read_waiters = waiter->next;
        waiter->next = nullptr;
        complete_waiter(waiter, copied);
    }
}

void wake_write_waiters_locked(Pipe& pipe) {
    while (pipe.write_waiters != nullptr) {
        PipeWaiter* waiter = pipe.write_waiters;
        if (pipe.reader_count == 0) {
            pipe.write_waiters = waiter->next;
            waiter->next = nullptr;
            complete_waiter(waiter, -1);
            continue;
        }
        if (pipe.count >= kPipeBufferSize) {
            break;
        }
        if (waiter->proc == nullptr) {
            pipe.write_waiters = waiter->next;
            waiter->next = nullptr;
            complete_waiter(waiter, -1);
            continue;
        }
        int64_t copied =
            pipe_copy_in_from_user(pipe,
                                   *waiter->proc,
                                   waiter->user_address,
                                   static_cast<size_t>(waiter->length));
        pipe.write_waiters = waiter->next;
        waiter->next = nullptr;
        complete_waiter(waiter, copied);
        if (pipe.count >= kPipeBufferSize) {
            break;
        }
    }
}

int64_t pipe_read(process::Task& proc,
                  DescriptorEntry& entry,
                  uint64_t user_address,
                  uint64_t length,
                  uint64_t offset) {
    if (offset != 0) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    auto* endpoint = static_cast<PipeEndpoint*>(entry.subsystem_data);
    if (user_address == 0 && length != 0) {
        return -1;
    }

    size_t requested = static_cast<size_t>(length);
    size_t read_count = 0;
    bool async = has_flag(entry.flags, Flag::Async);

    Pipe* pipe = nullptr;
    uint64_t irq_flags = 0;
    if (!lock_endpoint_pipe(endpoint, true, false, pipe, irq_flags)) {
        return -1;
    }

    if (pipe->count > 0) {
        int64_t copied =
            pipe_copy_out_to_user(*pipe, proc, user_address, requested);
        if (copied < 0) {
            unlock_pipe(*pipe, irq_flags);
            return -1;
        }
        read_count = static_cast<size_t>(copied);
    }

    if (read_count > 0) {
        wake_write_waiters_locked(*pipe);
        unlock_pipe(*pipe, irq_flags);
        descriptor::wake_waiters();
        return static_cast<int64_t>(read_count);
    }

    if (pipe->writer_count == 0) {
        wake_write_waiters_locked(*pipe);
        unlock_pipe(*pipe, irq_flags);
        return 0;
    }

    if (async) {
        wake_write_waiters_locked(*pipe);
        unlock_pipe(*pipe, irq_flags);
        return kWouldBlock;
    }

    PipeWaiter* waiter = allocate_pipe_waiter();
    if (waiter == nullptr) {
        unlock_pipe(*pipe, irq_flags);
        return -1;
    }
    waiter->proc = &proc;
    waiter->user_address = user_address;
    waiter->length = length;
    waiter->is_read = true;
    waiter->next = nullptr;

    push_waiter(pipe->read_waiters, waiter);

    proc.waiting_on = pipe;
    process::store_state(proc, process::State::Blocked);

    unlock_pipe(*pipe, irq_flags);
    return kWouldBlock;
}

int64_t pipe_write(process::Task& proc,
                   DescriptorEntry& entry,
                   uint64_t user_address,
                   uint64_t length,
                   uint64_t offset) {
    if (offset != 0) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    auto* endpoint = static_cast<PipeEndpoint*>(entry.subsystem_data);
    if (user_address == 0 && length != 0) {
        return -1;
    }

    size_t requested = static_cast<size_t>(length);
    size_t written = 0;
    bool async = has_flag(entry.flags, Flag::Async);

    Pipe* pipe = nullptr;
    uint64_t irq_flags = 0;
    if (!lock_endpoint_pipe(endpoint, false, true, pipe, irq_flags)) {
        return -1;
    }

    if (pipe->reader_count == 0) {
        unlock_pipe(*pipe, irq_flags);
        return -1;
    }

    if (pipe->count < kPipeBufferSize) {
        int64_t copied =
            pipe_copy_in_from_user(*pipe, proc, user_address, requested);
        if (copied < 0) {
            unlock_pipe(*pipe, irq_flags);
            return -1;
        }
        written = static_cast<size_t>(copied);
    }

    if (written > 0) {
        wake_read_waiters_locked(*pipe);
        unlock_pipe(*pipe, irq_flags);
        descriptor::wake_waiters();
        return static_cast<int64_t>(written);
    }

    if (async) {
        wake_read_waiters_locked(*pipe);
        unlock_pipe(*pipe, irq_flags);
        return kWouldBlock;
    }

    PipeWaiter* waiter = allocate_pipe_waiter();
    if (waiter == nullptr) {
        unlock_pipe(*pipe, irq_flags);
        return -1;
    }
    waiter->proc = &proc;
    waiter->user_address = user_address;
    waiter->length = length;
    waiter->is_read = false;
    waiter->next = nullptr;

    push_waiter(pipe->write_waiters, waiter);

    proc.waiting_on = pipe;
    process::store_state(proc, process::State::Blocked);

    unlock_pipe(*pipe, irq_flags);
    return kWouldBlock;
}

int pipe_get_property(DescriptorEntry& entry,
                      uint32_t property,
                      void* out,
                      size_t size) {
    if (property !=
        static_cast<uint32_t>(descriptor_defs::Property::PipeInfo)) {
        return -1;
    }
    auto* endpoint = static_cast<PipeEndpoint*>(entry.subsystem_data);
    if (out == nullptr || size < sizeof(descriptor_defs::PipeInfo)) {
        return -1;
    }
    Pipe* pipe = nullptr;
    uint64_t irq_flags = 0;
    if (!lock_endpoint_pipe(endpoint, false, false, pipe, irq_flags)) {
        return -1;
    }
    auto* info = reinterpret_cast<descriptor_defs::PipeInfo*>(out);
    info->id = pipe->id;
    info->flags = static_cast<uint32_t>(entry.flags & 0xFFFFFFFFu);
    unlock_pipe(*pipe, irq_flags);
    return 0;
}

void close_pipe(DescriptorEntry& entry) {
    auto* endpoint = static_cast<PipeEndpoint*>(entry.subsystem_data);
    if (endpoint == nullptr) {
        return;
    }
    bool notify_waiters = false;
    {
        sync::IrqLockGuard pool_guard(g_pipe_pool_lock);
        if (!__atomic_load_n(&endpoint->in_use, __ATOMIC_ACQUIRE)) {
            return;
        }
        Pipe* pipe = endpoint->pipe;
        if (pipe == nullptr ||
            !__atomic_load_n(&pipe->in_use, __ATOMIC_ACQUIRE)) {
            release_pipe_endpoint_locked(endpoint);
            return;
        }

        uint64_t irq_flags = lock_pipe(*pipe);
        if (!__atomic_load_n(&endpoint->in_use, __ATOMIC_ACQUIRE) ||
            endpoint->pipe != pipe ||
            !__atomic_load_n(&pipe->in_use, __ATOMIC_ACQUIRE)) {
            unlock_pipe(*pipe, irq_flags);
            return;
        }

        if (pipe->refcount > 0) {
            --pipe->refcount;
        }
        if (endpoint->can_read && pipe->reader_count > 0) {
            --pipe->reader_count;
        }
        if (endpoint->can_write && pipe->writer_count > 0) {
            --pipe->writer_count;
        }

        if (pipe->writer_count == 0) {
            wake_read_waiters_locked(*pipe);
            notify_waiters = true;
        }
        if (pipe->reader_count == 0) {
            wake_write_waiters_locked(*pipe);
            notify_waiters = true;
        }

        drop_waiters_for_owner_locked(*pipe, endpoint->owner);

        if (pipe->refcount == 0) {
            while (pipe->read_waiters != nullptr) {
                PipeWaiter* w = pipe->read_waiters;
                pipe->read_waiters = w->next;
                w->next = nullptr;
                complete_waiter(w, -1);
            }
            while (pipe->write_waiters != nullptr) {
                PipeWaiter* w = pipe->write_waiters;
                pipe->write_waiters = w->next;
                w->next = nullptr;
                complete_waiter(w, -1);
            }
            pipe->head = 0;
            pipe->tail = 0;
            pipe->count = 0;
            pipe->reader_count = 0;
            pipe->writer_count = 0;
            pipe->read_waiters = nullptr;
            pipe->write_waiters = nullptr;
            pipe->id = 0;
            // Unpublish while lookup and ref acquisition are still excluded.
            __atomic_store_n(&pipe->in_use, false, __ATOMIC_RELEASE);
        }

        release_pipe_endpoint_locked(endpoint);
        unlock_pipe(*pipe, irq_flags);
    }
    if (notify_waiters) {
        descriptor::wake_waiters();
    }
}

bool query_wait(DescriptorEntry& entry, uint32_t events, uint32_t& revents) {
    revents = 0;
    auto* endpoint = static_cast<PipeEndpoint*>(entry.subsystem_data);
    Pipe* pipe = nullptr;
    uint64_t irq_flags = 0;
    if (!lock_endpoint_pipe(endpoint, false, false, pipe, irq_flags)) {
        return false;
    }
    if ((events & descriptor_defs::kWaitRead) != 0 &&
        endpoint->can_read &&
        (pipe->count > 0 || pipe->writer_count == 0)) {
        revents |= descriptor_defs::kWaitRead;
    }
    if ((events & descriptor_defs::kWaitWrite) != 0 &&
        endpoint->can_write &&
        pipe->reader_count != 0 &&
        pipe->count < kPipeBufferSize) {
        revents |= descriptor_defs::kWaitWrite;
    }
    unlock_pipe(*pipe, irq_flags);
    return true;
}

const Ops kPipeOps{
    .read = pipe_read,
    .write = pipe_write,
    .get_property = pipe_get_property,
    .set_property = nullptr,
};

bool open_pipe(process::Task& proc,
               uint64_t flags,
               uint64_t existing_id,
               uint64_t,
               Allocation& alloc) {
    bool want_read = (flags & static_cast<uint64_t>(Flag::Readable)) != 0;
    bool want_write = (flags & static_cast<uint64_t>(Flag::Writable)) != 0;
    bool async = (flags & static_cast<uint64_t>(Flag::Async)) != 0;
    if (!want_read && !want_write) {
        return false;
    }

    bool created_pipe = (existing_id == 0);
    Pipe* pipe = nullptr;
    PipeEndpoint* endpoint = nullptr;
    {
        sync::IrqLockGuard pool_guard(g_pipe_pool_lock);
        pipe = created_pipe
                   ? allocate_pipe_locked()
                   : find_pipe_by_id_locked(static_cast<uint32_t>(existing_id));
        if (pipe == nullptr ||
            !__atomic_load_n(&pipe->in_use, __ATOMIC_ACQUIRE)) {
            return false;
        }

        endpoint = allocate_pipe_endpoint_locked(
            pipe, &proc, want_read, want_write);
        if (endpoint == nullptr) {
            if (created_pipe) {
                pipe->id = 0;
                __atomic_store_n(&pipe->in_use, false, __ATOMIC_RELEASE);
            }
            return false;
        }

        uint64_t irq_flags = lock_pipe(*pipe);
        pipe->refcount++;
        if (want_read) {
            pipe->reader_count++;
        }
        if (want_write) {
            pipe->writer_count++;
        }
        unlock_pipe(*pipe, irq_flags);
    }
    descriptor::wake_waiters();

    uint64_t descriptor_flags = 0;
    if (want_read) {
        descriptor_flags |= static_cast<uint64_t>(Flag::Readable);
    }
    if (want_write) {
        descriptor_flags |= static_cast<uint64_t>(Flag::Writable);
    }
    if (async) {
        descriptor_flags |= static_cast<uint64_t>(Flag::Async);
    }

    alloc.type = kTypePipe;
    alloc.flags = descriptor_flags;
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = pipe;
    alloc.subsystem_data = endpoint;
    alloc.name = "pipe";
    alloc.ops = &kPipeOps;
    alloc.close = close_pipe;
    return true;
}

}  // namespace descriptor_pipe

bool register_pipe_descriptor() {
    return register_type(kTypePipe, descriptor_pipe::open_pipe, &descriptor_pipe::kPipeOps);
}

}  // namespace descriptor
