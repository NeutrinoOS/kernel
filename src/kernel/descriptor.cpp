#include "descriptor.hpp"

#include "process.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "error.hpp"
#include "scheduler.hpp"
#include "string_util.hpp"
#include "vm.hpp"
#include "../lib/mem.hpp"

namespace descriptor {

namespace descriptor_pipe {
bool query_wait(DescriptorEntry& entry, uint32_t events, uint32_t& revents);
}

namespace descriptor_net_endpoint {
bool query_wait(DescriptorEntry& entry, uint32_t events, uint32_t& revents);
}

namespace descriptor_net_device {
bool query_wait(DescriptorEntry& entry, uint32_t events, uint32_t& revents);
}

namespace descriptor_keyboard {
bool query_wait(DescriptorEntry& entry, uint32_t events, uint32_t& revents);
}

namespace descriptor_mouse {
bool query_wait(DescriptorEntry& entry, uint32_t events, uint32_t& revents);
}

namespace descriptor_vty {
bool query_wait(DescriptorEntry& entry, uint32_t events, uint32_t& revents);
}

namespace {

process::Task g_kernel_process{};
process::ProcessResources g_kernel_resources{};
bool g_kernel_process_initialized = false;
process::Task* g_waiter_worker = nullptr;
uint32_t g_waiters_pending = 0;

process::Task& kernel_process() {
    if (!g_kernel_process_initialized) {
        memset(&g_kernel_process, 0, sizeof(g_kernel_process));
        memset(&g_kernel_resources, 0, sizeof(g_kernel_resources));
        g_kernel_resources.in_use = true;
        g_kernel_resources.refcount = 1;
        g_kernel_resources.limits.max_descriptors = kMaxDescriptors;
        init_table(g_kernel_resources.descriptors);
        g_kernel_process.resources = &g_kernel_resources;
        g_kernel_process.cr3 = paging_kernel_cr3();
        g_kernel_process.fs_base = 0;
        g_kernel_process_initialized = true;
    }
    if (g_kernel_process.cr3 == 0) {
        g_kernel_process.cr3 = paging_kernel_cr3();
    }
    return g_kernel_process;
}

struct TypeRegistration {
    uint16_t type;
    OpenFn open;
    const Ops* ops;
    bool used;
};

constexpr size_t kMaxRegisteredTypes = 32;
TypeRegistration g_type_registry[kMaxRegisteredTypes];

TypeRegistration* find_registration(uint16_t type) {
    for (size_t i = 0; i < kMaxRegisteredTypes; ++i) {
        if (g_type_registry[i].used && g_type_registry[i].type == type) {
            return &g_type_registry[i];
        }
    }
    return nullptr;
}

void reset_entry(DescriptorEntry& entry, bool bump_generation) {
    uint16_t generation = entry.generation;
    if (bump_generation) {
        uint16_t next_generation =
            static_cast<uint16_t>(static_cast<uint32_t>(generation) + 1u);
        generation = (next_generation == 0) ? 1 : next_generation;
    }
    entry.type = 0;
    entry.flags = 0;
    entry.extended_flags = 0;
    entry.created_tick = 0;
    entry.last_access_tick = 0;
    entry.object = nullptr;
    entry.subsystem_data = nullptr;
    entry.name = nullptr;
    entry.ops = nullptr;
    entry.ext = nullptr;
    entry.close = nullptr;
    entry.lock_word = 0;
    entry.refcount = 0;
    entry.has_extended_flags = false;
    entry.in_use = false;
    entry.generation = (generation == 0) ? 1 : generation;
}

DescriptorEntry* lookup_entry(Table& table, uint32_t handle) {
    uint16_t index = handle_index(handle);
    uint16_t generation = handle_generation(handle);
    if (index >= kMaxDescriptors) {
        return nullptr;
    }
    DescriptorEntry& entry = table.entries[index];
    if (!entry.in_use) {
        return nullptr;
    }
    KERNEL_ASSERT_MSG(entry.generation != 0,
                      "live descriptor has a zero generation");
    KERNEL_ASSERT_MSG(entry.refcount != 0,
                      "live descriptor has a zero refcount");
    if (generation == 0 || entry.generation != generation) {
        return nullptr;
    }
    return &entry;
}

const DescriptorEntry* lookup_entry(const Table& table, uint32_t handle) {
    uint16_t index = handle_index(handle);
    uint16_t generation = handle_generation(handle);
    if (index >= kMaxDescriptors) {
        return nullptr;
    }
    const DescriptorEntry& entry = table.entries[index];
    if (!entry.in_use) {
        return nullptr;
    }
    KERNEL_ASSERT_MSG(entry.generation != 0,
                      "live descriptor has a zero generation");
    KERNEL_ASSERT_MSG(entry.refcount != 0,
                      "live descriptor has a zero refcount");
    if (generation == 0 || entry.generation != generation) {
        return nullptr;
    }
    return &entry;
}

bool query_entry_wait(DescriptorEntry& entry,
                      uint32_t events,
                      uint32_t& revents) {
    revents = 0;
    switch (entry.type) {
        case kTypePipe:
            return descriptor_pipe::query_wait(entry, events, revents);
        case kTypeNetEndpoint:
            return descriptor_net_endpoint::query_wait(entry, events, revents);
        case kTypeNetDevice:
            return descriptor_net_device::query_wait(entry, events, revents);
        case kTypeKeyboard:
            return descriptor_keyboard::query_wait(entry, events, revents);
        case kTypeMouse:
            return descriptor_mouse::query_wait(entry, events, revents);
        case kTypeVty:
            return descriptor_vty::query_wait(entry, events, revents);
        case kTypeConsole:
        case kTypeSerial:
        case kTypeFramebuffer:
            revents = events & descriptor_defs::kWaitWrite;
            return true;
        default:
            return false;
    }
}

int evaluate_waits(Table& table,
                   descriptor_defs::DescriptorWait* items,
                   size_t count) {
    if (items == nullptr || count == 0 || count > kMaxWaitDescriptors) {
        return -1;
    }

    uint32_t ready_count = 0;
    for (size_t i = 0; i < count; ++i) {
        descriptor_defs::DescriptorWait& item = items[i];
        item.revents = 0;
        item.reserved = 0;
        if ((item.events &
             ~(descriptor_defs::kWaitRead | descriptor_defs::kWaitWrite)) != 0 ||
            item.events == 0) {
            return -1;
        }
        DescriptorEntry* entry = lookup_entry(table, item.handle);
        if (entry == nullptr) {
            return -1;
        }
        if (!query_entry_wait(*entry, item.events, item.revents)) {
            return -1;
        }
        if (item.revents != 0) {
            ++ready_count;
        }
    }

    return static_cast<int>(ready_count);
}

void populate_entry(DescriptorEntry& entry, const Allocation& alloc) {
    KERNEL_ASSERT_MSG(!entry.in_use,
                      "descriptor population attempted on a live entry");
    KERNEL_ASSERT_MSG(alloc.type != 0,
                      "descriptor allocation has an invalid type");
    uint16_t generation = entry.generation;
    if (generation == 0) {
        generation = 1;
    }
    reset_entry(entry, false);
    entry.type = alloc.type;
    entry.flags = alloc.flags;
    entry.extended_flags = alloc.extended_flags;
    entry.has_extended_flags = alloc.has_extended_flags;
    entry.object = alloc.object;
    entry.subsystem_data = alloc.subsystem_data;
    entry.name = alloc.name;
    entry.ops = alloc.ops;
    entry.ext = alloc.ext;
    entry.close = alloc.close;
    entry.refcount = 1;
    entry.created_tick = 0;
    entry.last_access_tick = 0;
    entry.lock_word = 0;
    entry.in_use = true;
    entry.generation = generation;
}

}  // namespace

bool register_type(uint32_t type, OpenFn open, const Ops* ops) {
    if (type > 0xFFFFu) {
        return false;
    }
    uint16_t type_id = static_cast<uint16_t>(type);
    if (find_registration(type_id) != nullptr) {
        return false;
    }
    for (size_t i = 0; i < kMaxRegisteredTypes; ++i) {
        if (!g_type_registry[i].used) {
            g_type_registry[i] = TypeRegistration{
                .type = type_id,
                .open = open,
                .ops = ops,
                .used = true,
            };
            return true;
        }
    }
    return false;
}

void init() {
    for (size_t i = 0; i < kMaxRegisteredTypes; ++i) {
        g_type_registry[i].used = false;
        g_type_registry[i].type = 0;
        g_type_registry[i].open = nullptr;
        g_type_registry[i].ops = nullptr;
    }
    g_kernel_process_initialized = false;
    (void)kernel_process();
}

void init_table(Table& table) {
    for (size_t i = 0; i < kMaxDescriptors; ++i) {
        table.entries[i].generation = 1;
        reset_entry(table.entries[i], false);
    }
}

void destroy_table(process::Task& proc, Table& table) {
    (void)proc;
    for (size_t i = 0; i < kMaxDescriptors; ++i) {
        DescriptorEntry& entry = table.entries[i];
        if (!entry.in_use) {
            reset_entry(entry, false);
            continue;
        }
        if (entry.close != nullptr) {
            entry.close(entry);
        }
        reset_entry(entry, true);
    }
}

uint32_t install(process::Task& proc,
                 Table& table,
                 const Allocation& alloc) {
    size_t limit = proc.resources != nullptr
                       ? proc.resources->limits.max_descriptors
                       : kMaxDescriptors;
    if (limit > kMaxDescriptors) {
        limit = kMaxDescriptors;
    }
    size_t used = 0;
    for (const auto& entry : table.entries) {
        used += entry.in_use ? 1u : 0u;
    }
    if (used >= limit) {
        return kInvalidHandle;
    }
    for (size_t i = 0; i < kMaxDescriptors; ++i) {
        DescriptorEntry& entry = table.entries[i];
        if (entry.in_use) {
            continue;
        }
        populate_entry(entry, alloc);
        return make_handle(static_cast<uint16_t>(i), entry.generation);
    }
    return kInvalidHandle;
}

uint32_t install_at(process::Task& proc,
                    Table& table,
                    uint16_t index,
                    const Allocation& alloc) {
    if (index >= kMaxDescriptors) {
        return kInvalidHandle;
    }
    DescriptorEntry& entry = table.entries[index];
    if (!entry.in_use && proc.resources != nullptr) {
        size_t used = 0;
        for (const auto& candidate : table.entries) {
            used += candidate.in_use ? 1u : 0u;
        }
        if (used >= proc.resources->limits.max_descriptors) {
            return kInvalidHandle;
        }
    }
    if (entry.in_use) {
        if (entry.close != nullptr) {
            entry.close(entry);
        }
        // Replacing a live descriptor must invalidate every old handle for
        // this slot before publishing the replacement.
        reset_entry(entry, true);
    }
    uint16_t generation = entry.generation;
    if (generation == 0) {
        generation = 1;
    }
    populate_entry(entry, alloc);
    return make_handle(index, generation);
}

uint32_t open(process::Task& proc,
              Table& table,
              uint32_t type,
              uint64_t arg0,
              uint64_t arg1,
              uint64_t arg2) {
    uint16_t type_id = static_cast<uint16_t>(type & 0xFFFFu);
    TypeRegistration* reg = find_registration(type_id);
    if (reg == nullptr || reg->open == nullptr) {
        return kInvalidHandle;
    }
    Allocation alloc{
        .type = type_id,
        .flags = 0,
        .extended_flags = 0,
        .has_extended_flags = false,
        .object = nullptr,
        .subsystem_data = nullptr,
        .name = nullptr,
        .ops = reg->ops,
        .ext = nullptr,
        .close = nullptr,
    };
    if (!reg->open(proc, arg0, arg1, arg2, alloc)) {
        if (alloc.close != nullptr) {
            DescriptorEntry temp{};
            temp.type = alloc.type;
            temp.flags = alloc.flags;
            temp.extended_flags = alloc.extended_flags;
            temp.has_extended_flags = alloc.has_extended_flags;
            temp.object = alloc.object;
            temp.subsystem_data = alloc.subsystem_data;
            temp.name = alloc.name;
            temp.ops = alloc.ops;
            temp.close = alloc.close;
            temp.in_use = true;
            alloc.close(temp);
        }
        return kInvalidHandle;
    }
    alloc.type = (alloc.type == 0) ? type_id : alloc.type;
    if (alloc.ops == nullptr) {
        alloc.ops = reg->ops;
    }
    uint32_t handle = install(proc, table, alloc);
    if (handle == kInvalidHandle && alloc.close != nullptr) {
        DescriptorEntry temp{};
        temp.type = alloc.type;
        temp.flags = alloc.flags;
        temp.extended_flags = alloc.extended_flags;
        temp.has_extended_flags = alloc.has_extended_flags;
        temp.object = alloc.object;
        temp.subsystem_data = alloc.subsystem_data;
        temp.name = alloc.name;
        temp.ops = alloc.ops;
        temp.close = alloc.close;
        temp.in_use = true;
        alloc.close(temp);
    }
    return handle;
}

uint32_t open_at(process::Task& proc,
                 Table& table,
                 uint16_t index,
                 uint32_t type,
                 uint64_t arg0,
                 uint64_t arg1,
                 uint64_t arg2) {
    uint16_t type_id = static_cast<uint16_t>(type & 0xFFFFu);
    TypeRegistration* reg = find_registration(type_id);
    if (reg == nullptr || reg->open == nullptr) {
        return kInvalidHandle;
    }
    Allocation alloc{
        .type = type_id,
        .flags = 0,
        .extended_flags = 0,
        .has_extended_flags = false,
        .object = nullptr,
        .subsystem_data = nullptr,
        .name = nullptr,
        .ops = reg->ops,
        .ext = nullptr,
        .close = nullptr,
    };
    if (!reg->open(proc, arg0, arg1, arg2, alloc)) {
        if (alloc.close != nullptr) {
            DescriptorEntry temp{};
            temp.type = alloc.type;
            temp.flags = alloc.flags;
            temp.extended_flags = alloc.extended_flags;
            temp.has_extended_flags = alloc.has_extended_flags;
            temp.object = alloc.object;
            temp.subsystem_data = alloc.subsystem_data;
            temp.name = alloc.name;
            temp.ops = alloc.ops;
            temp.close = alloc.close;
            temp.in_use = true;
            alloc.close(temp);
        }
        return kInvalidHandle;
    }
    alloc.type = (alloc.type == 0) ? type_id : alloc.type;
    if (alloc.ops == nullptr) {
        alloc.ops = reg->ops;
    }
    uint32_t handle = install_at(proc, table, index, alloc);
    if (handle == kInvalidHandle && alloc.close != nullptr) {
        DescriptorEntry temp{};
        temp.type = alloc.type;
        temp.flags = alloc.flags;
        temp.extended_flags = alloc.extended_flags;
        temp.has_extended_flags = alloc.has_extended_flags;
        temp.object = alloc.object;
        temp.subsystem_data = alloc.subsystem_data;
        temp.name = alloc.name;
        temp.ops = alloc.ops;
        temp.close = alloc.close;
        temp.in_use = true;
        alloc.close(temp);
    }
    return handle;
}

int64_t read(process::Task& proc,
             Table& table,
             uint32_t handle,
             uint64_t user_address,
             uint64_t length,
             uint64_t offset) {
    DescriptorEntry* entry = lookup_entry(table, handle);
    if (entry == nullptr) {
        return -1;
    }
    if (!has_flag(entry->flags, Flag::Readable)) {
        return -1;
    }
    if (entry->ops == nullptr || entry->ops->read == nullptr) {
        return -1;
    }
    if (!is_kernel_process(proc) &&
        !vm::validate_user_buffer(proc.cr3,
                                  user_address,
                                  static_cast<size_t>(length),
                                  true)) {
        return -1;
    }
    entry->last_access_tick = 0;
    return entry->ops->read(proc, *entry, user_address, length, offset);
}

int64_t write(process::Task& proc,
              Table& table,
              uint32_t handle,
              uint64_t user_address,
              uint64_t length,
              uint64_t offset) {
    DescriptorEntry* entry = lookup_entry(table, handle);
    if (entry == nullptr) {
        return -1;
    }
    if (!has_flag(entry->flags, Flag::Writable)) {
        return -1;
    }
    if (entry->ops == nullptr || entry->ops->write == nullptr) {
        return -1;
    }
    if (!is_kernel_process(proc) &&
        !vm::validate_user_buffer(proc.cr3,
                                  user_address,
                                  static_cast<size_t>(length),
                                  false)) {
        return -1;
    }
    entry->last_access_tick = 0;
    return entry->ops->write(proc, *entry, user_address, length, offset);
}

bool close(process::Task& proc, Table& table, uint32_t handle) {
    (void)proc;
    DescriptorEntry* entry = lookup_entry(table, handle);
    if (entry == nullptr) {
        return false;
    }
    if (entry->close != nullptr) {
        entry->close(*entry);
    }
    reset_entry(*entry, true);
    return true;
}

bool get_type(const Table& table, uint32_t handle, uint16_t& out_type) {
    const DescriptorEntry* entry = lookup_entry(table, handle);
    if (entry == nullptr) {
        return false;
    }
    out_type = entry->type;
    return true;
}

bool test_flag(const Table& table,
               uint32_t handle,
               uint64_t flag,
               bool& out_value) {
    const DescriptorEntry* entry = lookup_entry(table, handle);
    if (entry == nullptr) {
        return false;
    }
    uint64_t source = entry->flags;
    if ((source & flag) == 0 && entry->has_extended_flags) {
        source = entry->extended_flags;
    }
    out_value = (source & flag) != 0;
    return true;
}

bool get_flags(const Table& table,
               uint32_t handle,
               bool extended_set,
               uint64_t& out_flags) {
    const DescriptorEntry* entry = lookup_entry(table, handle);
    if (entry == nullptr) {
        return false;
    }
    out_flags = extended_set
                    ? (entry->has_extended_flags ? entry->extended_flags : 0)
                    : entry->flags;
    return true;
}

int get_property_trusted(Table& table,
                         uint32_t handle,
                         uint32_t property,
                         void* out,
                         size_t out_size) {
    DescriptorEntry* entry = lookup_entry(table, handle);
    if (entry == nullptr) {
        return -1;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::CommonName)) {
        if (entry->name == nullptr || out == nullptr || out_size == 0) {
            return -1;
        }
        size_t name_len = string_util::length(entry->name);
        if (name_len + 1 > out_size) {
            return -1;
        }
        char* dest = reinterpret_cast<char*>(out);
        for (size_t i = 0; i <= name_len; ++i) {
            dest[i] = entry->name[i];
        }
        return 0;
    }
    if (entry->ops == nullptr || entry->ops->get_property == nullptr) {
        return -1;
    }
    if (out == nullptr && out_size != 0) {
        return -1;
    }
    return entry->ops->get_property(*entry, property, out, out_size);
}

int get_property(process::Task& proc,
                 Table& table,
                 uint32_t handle,
                 uint32_t property,
                 uint64_t out_ptr,
                 uint64_t size) {
    size_t out_size = static_cast<size_t>(size);
    if (!is_kernel_process(proc) &&
        !vm::validate_user_buffer(proc.cr3, out_ptr, out_size, true)) {
        return -1;
    }
    return get_property_trusted(table,
                                handle,
                                property,
                                reinterpret_cast<void*>(out_ptr),
                                out_size);
}

int set_property(process::Task& proc,
                 Table& table,
                 uint32_t handle,
                 uint32_t property,
                 uint64_t in_ptr,
                 uint64_t size) {
    const void* in = reinterpret_cast<const void*>(in_ptr);
    if (in == nullptr && size != 0) {
        return -1;
    }
    if (!is_kernel_process(proc) &&
        !vm::validate_user_buffer(proc.cr3,
                                  in_ptr,
                                  static_cast<size_t>(size),
                                  false)) {
        return -1;
    }
    return set_property_trusted(table,
                                handle,
                                property,
                                in,
                                static_cast<size_t>(size));
}

int set_property_trusted(Table& table,
                         uint32_t handle,
                         uint32_t property,
                         const void* in,
                         size_t size) {
    DescriptorEntry* entry = lookup_entry(table, handle);
    if (entry == nullptr || entry->ops == nullptr ||
        entry->ops->set_property == nullptr ||
        (in == nullptr && size != 0)) {
        return -1;
    }
    return entry->ops->set_property(*entry, property, in, size);
}

int wait(process::Task& proc,
         Table& table,
         uint64_t user_address,
         size_t count) {
    if (user_address == 0 || count == 0 || count > kMaxWaitDescriptors) {
        return -1;
    }

    descriptor_defs::DescriptorWait items[kMaxWaitDescriptors];
    if (!vm::copy_from_user(proc.cr3,
                            items,
                            user_address,
                            count * sizeof(items[0]))) {
        return -1;
    }

    int ready = evaluate_waits(table, items, count);
    if (ready < 0) {
        return -1;
    }
    if (ready > 0) {
        if (!vm::copy_to_user(proc.cr3,
                              user_address,
                              items,
                              count * sizeof(items[0]))) {
            return -1;
        }
        return ready;
    }

    for (size_t i = 0; i < count; ++i) {
        proc.wait_descriptors[i] = items[i];
    }
    proc.wait_descriptors_user = user_address;
    proc.wait_descriptor_count = static_cast<uint32_t>(count);
    proc.waiting_on = nullptr;
    process::store_state(proc, process::State::Blocked);

    // Close the registration race with an IRQ or another producer.  An event
    // can become ready after the first evaluation but before the process is
    // visibly Blocked; a waiter-worker pass in that window cannot wake us.
    // Once Blocked is published, rechecking either observes that event or
    // leaves any later producer able to wake the registered waiter normally.
    ready = evaluate_waits(table, proc.wait_descriptors, count);
    if (ready != 0) {
        process::State expected = process::State::Blocked;
        if (!process::compare_exchange_state(proc,
                                             expected,
                                             process::State::Running)) {
            return kWouldBlock;
        }
        int result = ready;
        if (ready > 0 &&
            !vm::copy_to_user(proc.cr3,
                              user_address,
                              proc.wait_descriptors,
                              count * sizeof(proc.wait_descriptors[0]))) {
            result = -1;
        }
        proc.wait_descriptors_user = 0;
        proc.wait_descriptor_count = 0;
        return result;
    }
    return kWouldBlock;
}

void service_waiters() {
    for (size_t i = 0; i < process::kMaxTasks; ++i) {
        process::Task* proc = process::task_table_entry(i);
        if (proc == nullptr ||
            proc->wait_descriptor_count == 0) {
            continue;
        }
        process::State state = process::load_state(*proc);
        if (state != process::State::Blocked &&
            !(state == process::State::Suspended &&
              proc->suspended_from == process::State::Blocked)) {
            continue;
        }

        size_t count = static_cast<size_t>(proc->wait_descriptor_count);
        sync::LockGuard descriptor_guard(
            proc->resources->descriptor_lock);
        int ready = evaluate_waits(proc->resources->descriptors,
                                   proc->wait_descriptors,
                                   count);
        if (ready <= 0) {
            continue;
        }
        if (!process::begin_wake(*proc)) {
            continue;
        }

        int result = ready;
        if (!vm::copy_to_user(proc->cr3,
                              proc->wait_descriptors_user,
                              proc->wait_descriptors,
                              count * sizeof(proc->wait_descriptors[0]))) {
            result = -1;
        }

        proc->wait_descriptors_user = 0;
        proc->wait_descriptor_count = 0;
        proc->waiting_on = nullptr;
        process::finish_wake_with_result(*proc, result);
    }
}

void waiter_worker(process::Task& worker) {
    while (__atomic_exchange_n(&g_waiters_pending,
                               static_cast<uint32_t>(0),
                               __ATOMIC_ACQ_REL) != 0) {
        service_waiters();
    }

    // Publish Blocked before the final pending check.  A concurrent IRQ either
    // observes Blocked and enqueues us, or its pending bit makes us remain
    // Ready when this callback returns.
    worker.waiting_on = nullptr;
    process::store_state(worker, process::State::Blocked);
    if (__atomic_load_n(&g_waiters_pending, __ATOMIC_ACQUIRE) != 0) {
        (void)process::wake(worker);
    }
}

void wake_waiters() {
    __atomic_store_n(&g_waiters_pending,
                     static_cast<uint32_t>(1),
                     __ATOMIC_RELEASE);
    process::Task* worker =
        __atomic_load_n(&g_waiter_worker, __ATOMIC_ACQUIRE);
    if (worker != nullptr) {
        (void)process::wake(*worker);
    }
}

void start_waiter_worker() {
    if (__atomic_load_n(&g_waiter_worker, __ATOMIC_ACQUIRE) != nullptr) {
        return;
    }
    process::Task* worker = process::allocate_kernel_task(waiter_worker);
    if (worker == nullptr) {
        return;
    }
    worker->preferred_cpu = 0;
    __atomic_store_n(&g_waiter_worker, worker, __ATOMIC_RELEASE);
    scheduler::enqueue(worker);
}

uint32_t open_kernel(uint32_t type,
                     uint64_t arg0,
                     uint64_t arg1,
                     uint64_t arg2) {
    process::Task& proc = kernel_process();
    sync::LockGuard guard(proc.resources->descriptor_lock);
    return open(proc, proc.resources->descriptors, type, arg0, arg1, arg2);
}

int64_t read_kernel(uint32_t handle,
                    void* buffer,
                    uint64_t length,
                    uint64_t offset) {
    process::Task& proc = kernel_process();
    sync::LockGuard guard(proc.resources->descriptor_lock);
    return read(proc,
                proc.resources->descriptors,
                handle,
                reinterpret_cast<uint64_t>(buffer),
                length,
                offset);
}

int64_t write_kernel(uint32_t handle,
                     const void* buffer,
                     uint64_t length,
                     uint64_t offset) {
    process::Task& proc = kernel_process();
    sync::LockGuard guard(proc.resources->descriptor_lock);
    return write(proc,
                 proc.resources->descriptors,
                 handle,
                 reinterpret_cast<uint64_t>(buffer),
                 length,
                 offset);
}

bool close_kernel(uint32_t handle) {
    process::Task& proc = kernel_process();
    sync::LockGuard guard(proc.resources->descriptor_lock);
    return close(proc, proc.resources->descriptors, handle);
}

int get_property_kernel(uint32_t handle,
                        uint32_t property,
                        void* out,
                        uint64_t size) {
    process::Task& proc = kernel_process();
    sync::LockGuard guard(proc.resources->descriptor_lock);
    return get_property(proc,
                        proc.resources->descriptors,
                        handle,
                        property,
                        reinterpret_cast<uint64_t>(out),
                        size);
}

int set_property_kernel(uint32_t handle,
                        uint32_t property,
                        const void* in,
                        uint64_t size) {
    process::Task& proc = kernel_process();
    sync::LockGuard guard(proc.resources->descriptor_lock);
    return set_property(proc,
                        proc.resources->descriptors,
                        handle,
                        property,
                        reinterpret_cast<uint64_t>(in),
                        size);
}

bool is_kernel_process(const process::Task& proc) {
    return &proc == &kernel_process();
}

}  // namespace descriptor
