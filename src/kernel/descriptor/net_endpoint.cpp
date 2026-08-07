#include "../descriptor.hpp"

#include "../../lib/mem.hpp"
#include "../process.hpp"
#include "../random.hpp"
#include "../scheduler.hpp"
#include "../sync.hpp"
#include "../vm.hpp"

namespace descriptor {

namespace descriptor_net_endpoint {

constexpr size_t kEndpointBufferSize = 65536;
constexpr size_t kMaxEndpoints = 64;
constexpr size_t kMaxEndpointHandles = kMaxEndpoints * 2;
constexpr size_t kMaxEndpointWaiters = 128;

enum class Role : uint32_t {
    App = 0,
    Service = 1,
};

struct EndpointWaiter {
    process::Task* proc;
    uint64_t user_address;
    uint64_t length;
    bool is_read;
    Role role;
    bool in_use;
    EndpointWaiter* next;
};

struct Ring {
    uint8_t buffer[kEndpointBufferSize];
    size_t head;
    size_t tail;
    size_t count;
};

struct NetEndpoint {
    Ring app_to_service;
    Ring service_to_app;
    size_t app_handles;
    size_t service_handles;
    bool app_ever_attached;
    bool service_ever_attached;
    size_t refcount;
    uint32_t id;
    bool in_use;
    volatile int lock;
    EndpointWaiter* app_read_waiters;
    EndpointWaiter* app_write_waiters;
    EndpointWaiter* service_read_waiters;
    EndpointWaiter* service_write_waiters;
};

struct EndpointHandle {
    NetEndpoint* endpoint;
    process::Task* owner;
    Role role;
    bool in_use;
};

NetEndpoint g_endpoints[kMaxEndpoints]{};
EndpointHandle g_handles[kMaxEndpointHandles]{};
EndpointWaiter g_waiters[kMaxEndpointWaiters]{};
sync::SpinLock g_endpoint_pool_lock;

inline size_t min_size(size_t a, size_t b) {
    return (a < b) ? a : b;
}

uint64_t lock_endpoint(NetEndpoint& endpoint) {
    uint64_t flags = sync::disable_interrupts();
    while (__atomic_test_and_set(&endpoint.lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
    return flags;
}

void unlock_endpoint(NetEndpoint& endpoint, uint64_t flags) {
    __atomic_clear(&endpoint.lock, __ATOMIC_RELEASE);
    sync::restore_interrupts(flags);
}

bool lock_handle_endpoint(EndpointHandle* handle,
                          NetEndpoint*& out_endpoint,
                          uint64_t& out_irq_flags) {
    out_endpoint = nullptr;
    out_irq_flags = sync::disable_interrupts();
    g_endpoint_pool_lock.lock();
    if (handle == nullptr ||
        !__atomic_load_n(&handle->in_use, __ATOMIC_ACQUIRE) ||
        handle->endpoint == nullptr ||
        !__atomic_load_n(&handle->endpoint->in_use, __ATOMIC_ACQUIRE)) {
        g_endpoint_pool_lock.unlock();
        sync::restore_interrupts(out_irq_flags);
        return false;
    }

    NetEndpoint* endpoint = handle->endpoint;
    while (__atomic_test_and_set(&endpoint->lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
    g_endpoint_pool_lock.unlock();
    out_endpoint = endpoint;
    return true;
}

void reset_ring(Ring& ring) {
    ring.head = 0;
    ring.tail = 0;
    ring.count = 0;
}

Ring& incoming_ring(NetEndpoint& endpoint, Role role) {
    return role == Role::App ? endpoint.service_to_app
                             : endpoint.app_to_service;
}

Ring& outgoing_ring(NetEndpoint& endpoint, Role role) {
    return role == Role::App ? endpoint.app_to_service
                             : endpoint.service_to_app;
}

size_t peer_handles(const NetEndpoint& endpoint, Role role) {
    return role == Role::App ? endpoint.service_handles
                             : endpoint.app_handles;
}

bool peer_ever_attached(const NetEndpoint& endpoint, Role role) {
    return role == Role::App ? endpoint.service_ever_attached
                             : endpoint.app_ever_attached;
}

bool peer_closed(const NetEndpoint& endpoint, Role role) {
    return peer_handles(endpoint, role) == 0 &&
           peer_ever_attached(endpoint, role);
}

EndpointWaiter*& read_waiters(NetEndpoint& endpoint, Role role) {
    return role == Role::App ? endpoint.app_read_waiters
                             : endpoint.service_read_waiters;
}

EndpointWaiter*& write_waiters(NetEndpoint& endpoint, Role role) {
    return role == Role::App ? endpoint.app_write_waiters
                             : endpoint.service_write_waiters;
}

NetEndpoint* find_endpoint_locked(uint32_t id);

NetEndpoint* allocate_endpoint_locked() {
    for (auto& endpoint : g_endpoints) {
        if (__atomic_load_n(&endpoint.in_use, __ATOMIC_ACQUIRE)) {
            continue;
        }
        reset_ring(endpoint.app_to_service);
        reset_ring(endpoint.service_to_app);
        endpoint.app_handles = 0;
        endpoint.service_handles = 0;
        endpoint.app_ever_attached = false;
        endpoint.service_ever_attached = false;
        endpoint.refcount = 0;
        endpoint.lock = 0;
        endpoint.app_read_waiters = nullptr;
        endpoint.app_write_waiters = nullptr;
        endpoint.service_read_waiters = nullptr;
        endpoint.service_write_waiters = nullptr;
        endpoint.id = 0;
        uint32_t candidate = 0;
        do {
            candidate = kernel_random::opaque_id();
        } while (candidate == 0 || find_endpoint_locked(candidate) != nullptr);
        endpoint.id = candidate;
        __atomic_store_n(&endpoint.in_use, true, __ATOMIC_RELEASE);
        return &endpoint;
    }
    return nullptr;
}

NetEndpoint* find_endpoint_locked(uint32_t id) {
    if (id == 0) {
        return nullptr;
    }
    for (auto& endpoint : g_endpoints) {
        if (__atomic_load_n(&endpoint.in_use, __ATOMIC_ACQUIRE) &&
            endpoint.id == id) {
            return &endpoint;
        }
    }
    return nullptr;
}

EndpointHandle* allocate_handle_locked(NetEndpoint* endpoint,
                                       process::Task* owner,
                                       Role role) {
    for (auto& handle : g_handles) {
        if (__atomic_load_n(&handle.in_use, __ATOMIC_ACQUIRE)) {
            continue;
        }
        handle.endpoint = endpoint;
        handle.owner = owner;
        handle.role = role;
        __atomic_store_n(&handle.in_use, true, __ATOMIC_RELEASE);
        return &handle;
    }
    return nullptr;
}

void release_handle_locked(EndpointHandle* handle) {
    if (handle == nullptr) {
        return;
    }
    handle->endpoint = nullptr;
    handle->owner = nullptr;
    handle->role = Role::App;
    __atomic_store_n(&handle->in_use, false, __ATOMIC_RELEASE);
}

EndpointWaiter* allocate_waiter() {
    for (auto& waiter : g_waiters) {
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
        waiter.role = Role::App;
        waiter.next = nullptr;
        return &waiter;
    }
    return nullptr;
}

void release_waiter(EndpointWaiter* waiter) {
    if (waiter == nullptr) {
        return;
    }
    waiter->proc = nullptr;
    waiter->user_address = 0;
    waiter->length = 0;
    waiter->is_read = false;
    waiter->role = Role::App;
    waiter->next = nullptr;
    __atomic_store_n(&waiter->in_use, false, __ATOMIC_RELEASE);
}

void push_waiter(EndpointWaiter*& head, EndpointWaiter* waiter) {
    if (head == nullptr) {
        head = waiter;
        return;
    }
    EndpointWaiter* cur = head;
    while (cur->next != nullptr) {
        cur = cur->next;
    }
    cur->next = waiter;
}

void complete_waiter(EndpointWaiter* waiter, int64_t result) {
    if (waiter == nullptr || waiter->proc == nullptr) {
        release_waiter(waiter);
        return;
    }
    (void)process::wake_with_result(*waiter->proc, result);
    release_waiter(waiter);
}

int64_t ring_copy_out_to_user(Ring& ring,
                              process::Task& proc,
                              uint64_t user_address,
                              size_t max_bytes) {
    if (user_address == 0 || proc.cr3 == 0) {
        return -1;
    }
    size_t copied = 0;
    while (copied < max_bytes && ring.count > 0) {
        size_t chunk = min_size(
            min_size(max_bytes - copied, ring.count),
            kEndpointBufferSize - ring.head);
        if (!vm::copy_to_user(proc.cr3,
                              user_address + copied,
                              ring.buffer + ring.head,
                              chunk)) {
            return copied > 0 ? static_cast<int64_t>(copied) : -1;
        }
        ring.head = (ring.head + chunk) % kEndpointBufferSize;
        ring.count -= chunk;
        copied += chunk;
    }
    return static_cast<int64_t>(copied);
}

int64_t ring_copy_in_from_user(Ring& ring,
                               process::Task& proc,
                               uint64_t user_address,
                               size_t max_bytes) {
    if (user_address == 0 || proc.cr3 == 0) {
        return -1;
    }
    size_t copied = 0;
    while (copied < max_bytes && ring.count < kEndpointBufferSize) {
        size_t space = kEndpointBufferSize - ring.count;
        size_t chunk = min_size(
            min_size(max_bytes - copied, space),
            kEndpointBufferSize - ring.tail);
        if (!vm::copy_from_user(proc.cr3,
                                ring.buffer + ring.tail,
                                user_address + copied,
                                chunk)) {
            return copied > 0 ? static_cast<int64_t>(copied) : -1;
        }
        ring.tail = (ring.tail + chunk) % kEndpointBufferSize;
        ring.count += chunk;
        copied += chunk;
    }
    return static_cast<int64_t>(copied);
}

bool service_read_waiters_locked(NetEndpoint& endpoint, Role role) {
    EndpointWaiter*& waiters = read_waiters(endpoint, role);
    Ring& ring = incoming_ring(endpoint, role);
    bool progressed = false;
    while (waiters != nullptr) {
        EndpointWaiter* waiter = waiters;
        if (ring.count == 0 && peer_closed(endpoint, role)) {
            waiters = waiter->next;
            waiter->next = nullptr;
            complete_waiter(waiter, 0);
            progressed = true;
            continue;
        }
        if (ring.count == 0) {
            break;
        }
        if (waiter->proc == nullptr) {
            waiters = waiter->next;
            waiter->next = nullptr;
            complete_waiter(waiter, -1);
            progressed = true;
            continue;
        }
        int64_t copied =
            ring_copy_out_to_user(ring,
                                  *waiter->proc,
                                  waiter->user_address,
                                  static_cast<size_t>(waiter->length));
        waiters = waiter->next;
        waiter->next = nullptr;
        complete_waiter(waiter, copied);
        progressed = true;
    }
    return progressed;
}

bool service_write_waiters_locked(NetEndpoint& endpoint, Role role) {
    EndpointWaiter*& waiters = write_waiters(endpoint, role);
    Ring& ring = outgoing_ring(endpoint, role);
    bool progressed = false;
    while (waiters != nullptr) {
        EndpointWaiter* waiter = waiters;
        if (peer_closed(endpoint, role)) {
            waiters = waiter->next;
            waiter->next = nullptr;
            complete_waiter(waiter, -1);
            progressed = true;
            continue;
        }
        if (ring.count >= kEndpointBufferSize) {
            break;
        }
        if (waiter->proc == nullptr) {
            waiters = waiter->next;
            waiter->next = nullptr;
            complete_waiter(waiter, -1);
            progressed = true;
            continue;
        }
        int64_t copied =
            ring_copy_in_from_user(ring,
                                   *waiter->proc,
                                   waiter->user_address,
                                   static_cast<size_t>(waiter->length));
        waiters = waiter->next;
        waiter->next = nullptr;
        complete_waiter(waiter, copied);
        progressed = true;
        if (ring.count >= kEndpointBufferSize) {
            break;
        }
    }
    return progressed;
}

void pump_waiters_locked(NetEndpoint& endpoint) {
    // Reads make room for peer writers and writes feed peer readers. Iterate
    // until no waiter can advance; every successful pass removes at least one
    // waiter, so this is bounded by the fixed waiter pool without recursion.
    bool progressed = false;
    do {
        progressed = service_read_waiters_locked(endpoint, Role::App);
        progressed = service_write_waiters_locked(endpoint, Role::App) ||
                     progressed;
        progressed = service_read_waiters_locked(endpoint, Role::Service) ||
                     progressed;
        progressed = service_write_waiters_locked(endpoint, Role::Service) ||
                     progressed;
    } while (progressed);
}

int64_t endpoint_read(process::Task& proc,
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
    auto* handle = static_cast<EndpointHandle*>(entry.subsystem_data);
    if (user_address == 0) {
        return -1;
    }
    bool async = has_flag(entry.flags, Flag::Async);
    size_t requested = static_cast<size_t>(length);

    NetEndpoint* endpoint_ptr = nullptr;
    uint64_t irq_flags = 0;
    if (!lock_handle_endpoint(handle, endpoint_ptr, irq_flags)) {
        return -1;
    }
    NetEndpoint& endpoint = *endpoint_ptr;
    Ring& ring = incoming_ring(endpoint, handle->role);
    if (ring.count > 0) {
        int64_t copied = ring_copy_out_to_user(ring, proc, user_address, requested);
        pump_waiters_locked(endpoint);
        unlock_endpoint(endpoint, irq_flags);
        descriptor::wake_waiters();
        return copied;
    }
    if (peer_closed(endpoint, handle->role)) {
        unlock_endpoint(endpoint, irq_flags);
        return 0;
    }
    if (async) {
        unlock_endpoint(endpoint, irq_flags);
        return kWouldBlock;
    }

    EndpointWaiter* waiter = allocate_waiter();
    if (waiter == nullptr) {
        unlock_endpoint(endpoint, irq_flags);
        return -1;
    }
    waiter->proc = &proc;
    waiter->user_address = user_address;
    waiter->length = length;
    waiter->is_read = true;
    waiter->role = handle->role;
    waiter->next = nullptr;
    push_waiter(read_waiters(endpoint, handle->role), waiter);
    proc.waiting_on = &endpoint;
    process::store_state(proc, process::State::Blocked);
    unlock_endpoint(endpoint, irq_flags);
    return kWouldBlock;
}

int64_t endpoint_write(process::Task& proc,
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
    auto* handle = static_cast<EndpointHandle*>(entry.subsystem_data);
    if (user_address == 0) {
        return -1;
    }
    bool async = has_flag(entry.flags, Flag::Async);
    size_t requested = static_cast<size_t>(length);

    NetEndpoint* endpoint_ptr = nullptr;
    uint64_t irq_flags = 0;
    if (!lock_handle_endpoint(handle, endpoint_ptr, irq_flags)) {
        return -1;
    }
    NetEndpoint& endpoint = *endpoint_ptr;
    if (peer_closed(endpoint, handle->role)) {
        unlock_endpoint(endpoint, irq_flags);
        return -1;
    }
    Ring& ring = outgoing_ring(endpoint, handle->role);
    if (ring.count < kEndpointBufferSize) {
        int64_t copied = ring_copy_in_from_user(ring, proc, user_address, requested);
        pump_waiters_locked(endpoint);
        unlock_endpoint(endpoint, irq_flags);
        descriptor::wake_waiters();
        return copied;
    }
    if (async) {
        unlock_endpoint(endpoint, irq_flags);
        return kWouldBlock;
    }

    EndpointWaiter* waiter = allocate_waiter();
    if (waiter == nullptr) {
        unlock_endpoint(endpoint, irq_flags);
        return -1;
    }
    waiter->proc = &proc;
    waiter->user_address = user_address;
    waiter->length = length;
    waiter->is_read = false;
    waiter->role = handle->role;
    waiter->next = nullptr;
    push_waiter(write_waiters(endpoint, handle->role), waiter);
    proc.waiting_on = &endpoint;
    process::store_state(proc, process::State::Blocked);
    unlock_endpoint(endpoint, irq_flags);
    return kWouldBlock;
}

int endpoint_get_property(DescriptorEntry& entry,
                          uint32_t property,
                          void* out,
                          size_t size) {
    if (property !=
        static_cast<uint32_t>(descriptor_defs::Property::NetEndpointInfo)) {
        return -1;
    }
    if (out == nullptr || size < sizeof(descriptor_defs::NetEndpointInfo)) {
        return -1;
    }
    auto* handle = static_cast<EndpointHandle*>(entry.subsystem_data);
    NetEndpoint* endpoint = nullptr;
    uint64_t irq_flags = 0;
    if (!lock_handle_endpoint(handle, endpoint, irq_flags)) {
        return -1;
    }
    auto* info = reinterpret_cast<descriptor_defs::NetEndpointInfo*>(out);
    info->id = endpoint->id;
    info->flags = static_cast<uint32_t>(entry.flags & 0xFFFFFFFFu);
    info->role = static_cast<uint32_t>(handle->role);
    info->reserved = 0;
    unlock_endpoint(*endpoint, irq_flags);
    return 0;
}

void drop_waiters_for_owner_locked(NetEndpoint& endpoint, process::Task* owner) {
    EndpointWaiter** lists[] = {
        &endpoint.app_read_waiters,
        &endpoint.app_write_waiters,
        &endpoint.service_read_waiters,
        &endpoint.service_write_waiters,
    };
    for (auto** list : lists) {
        EndpointWaiter* prev = nullptr;
        EndpointWaiter* cur = *list;
        while (cur != nullptr) {
            EndpointWaiter* next = cur->next;
            if (cur->proc == owner) {
                if (prev == nullptr) {
                    *list = next;
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
}

void close_endpoint(DescriptorEntry& entry) {
    auto* handle = static_cast<EndpointHandle*>(entry.subsystem_data);
    if (handle == nullptr) {
        return;
    }
    bool notify_waiters = false;
    {
        sync::IrqLockGuard pool_guard(g_endpoint_pool_lock);
        if (!__atomic_load_n(&handle->in_use, __ATOMIC_ACQUIRE)) {
            return;
        }
        NetEndpoint* endpoint = handle->endpoint;
        if (endpoint == nullptr ||
            !__atomic_load_n(&endpoint->in_use, __ATOMIC_ACQUIRE)) {
            release_handle_locked(handle);
            return;
        }

        uint64_t irq_flags = lock_endpoint(*endpoint);
        if (!__atomic_load_n(&handle->in_use, __ATOMIC_ACQUIRE) ||
            handle->endpoint != endpoint ||
            !__atomic_load_n(&endpoint->in_use, __ATOMIC_ACQUIRE)) {
            unlock_endpoint(*endpoint, irq_flags);
            return;
        }

        if (endpoint->refcount > 0) {
            --endpoint->refcount;
        }
        if (handle->role == Role::App && endpoint->app_handles > 0) {
            --endpoint->app_handles;
        }
        if (handle->role == Role::Service && endpoint->service_handles > 0) {
            --endpoint->service_handles;
        }

        pump_waiters_locked(*endpoint);
        notify_waiters = true;
        drop_waiters_for_owner_locked(*endpoint, handle->owner);

        if (endpoint->refcount == 0) {
            EndpointWaiter** lists[] = {
                &endpoint->app_read_waiters,
                &endpoint->app_write_waiters,
                &endpoint->service_read_waiters,
                &endpoint->service_write_waiters,
            };
            for (auto** list : lists) {
                while (*list != nullptr) {
                    EndpointWaiter* waiter = *list;
                    *list = waiter->next;
                    waiter->next = nullptr;
                    complete_waiter(waiter, -1);
                }
            }
            reset_ring(endpoint->app_to_service);
            reset_ring(endpoint->service_to_app);
            endpoint->app_handles = 0;
            endpoint->service_handles = 0;
            endpoint->id = 0;
            __atomic_store_n(&endpoint->in_use, false, __ATOMIC_RELEASE);
        }

        release_handle_locked(handle);
        unlock_endpoint(*endpoint, irq_flags);
    }
    if (notify_waiters) {
        descriptor::wake_waiters();
    }
}

bool query_wait(DescriptorEntry& entry, uint32_t events, uint32_t& revents) {
    revents = 0;
    auto* handle = static_cast<EndpointHandle*>(entry.subsystem_data);
    NetEndpoint* endpoint = nullptr;
    uint64_t irq_flags = 0;
    if (!lock_handle_endpoint(handle, endpoint, irq_flags)) {
        return false;
    }
    Ring& incoming = incoming_ring(*endpoint, handle->role);
    Ring& outgoing = outgoing_ring(*endpoint, handle->role);
    bool closed = peer_closed(*endpoint, handle->role);
    if ((events & descriptor_defs::kWaitRead) != 0 &&
        (incoming.count > 0 || closed)) {
        revents |= descriptor_defs::kWaitRead;
    }
    if ((events & descriptor_defs::kWaitWrite) != 0 &&
        !closed &&
        outgoing.count < kEndpointBufferSize) {
        revents |= descriptor_defs::kWaitWrite;
    }
    unlock_endpoint(*endpoint, irq_flags);
    return true;
}

const Ops kEndpointOps{
    .read = endpoint_read,
    .write = endpoint_write,
    .get_property = endpoint_get_property,
    .set_property = nullptr,
};

bool open_endpoint(process::Task& proc,
                   uint64_t flags,
                   uint64_t existing_id,
                   uint64_t open_context,
                   Allocation& alloc) {
    bool want_service =
        (open_context & descriptor_defs::kNetEndpointOpenService) != 0;
    Role role = want_service ? Role::Service : Role::App;
    bool async = (flags & static_cast<uint64_t>(Flag::Async)) != 0;
    bool created = existing_id == 0;
    NetEndpoint* endpoint = nullptr;
    EndpointHandle* handle = nullptr;
    {
        sync::IrqLockGuard pool_guard(g_endpoint_pool_lock);
        endpoint = created
                       ? allocate_endpoint_locked()
                       : find_endpoint_locked(
                             static_cast<uint32_t>(existing_id));
        if (endpoint == nullptr ||
            !__atomic_load_n(&endpoint->in_use, __ATOMIC_ACQUIRE)) {
            return false;
        }

        handle = allocate_handle_locked(endpoint, &proc, role);
        if (handle == nullptr) {
            if (created) {
                endpoint->id = 0;
                __atomic_store_n(&endpoint->in_use,
                                 false,
                                 __ATOMIC_RELEASE);
            }
            return false;
        }

        uint64_t irq_flags = lock_endpoint(*endpoint);
        ++endpoint->refcount;
        if (role == Role::App) {
            ++endpoint->app_handles;
            endpoint->app_ever_attached = true;
        } else {
            ++endpoint->service_handles;
            endpoint->service_ever_attached = true;
        }
        unlock_endpoint(*endpoint, irq_flags);
    }
    descriptor::wake_waiters();

    uint64_t descriptor_flags =
        static_cast<uint64_t>(Flag::Readable) |
        static_cast<uint64_t>(Flag::Writable) |
        static_cast<uint64_t>(Flag::CapStream);
    if (async) {
        descriptor_flags |= static_cast<uint64_t>(Flag::Async);
    }

    alloc.type = kTypeNetEndpoint;
    alloc.flags = descriptor_flags;
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = endpoint;
    alloc.subsystem_data = handle;
    alloc.name = "net-endpoint";
    alloc.ops = &kEndpointOps;
    alloc.close = close_endpoint;
    return true;
}

}  // namespace descriptor_net_endpoint

bool register_net_endpoint_descriptor() {
    return register_type(kTypeNetEndpoint,
                         descriptor_net_endpoint::open_endpoint,
                         &descriptor_net_endpoint::kEndpointOps);
}

}  // namespace descriptor
