#include "process.hpp"

#include "arch/x86_64/percpu.hpp"
#include "arch/x86_64/registers.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "capabilities.hpp"
#include "drivers/log/logging.hpp"
#include "error.hpp"
#include "lib/mem.hpp"
#include "loader.hpp"
#include "scheduler.hpp"
#include "string_util.hpp"
#include "sync.hpp"
#include "time.hpp"

namespace {

process::Task g_task_table[process::kMaxTasks];
alignas(16) uint8_t g_kernel_stacks[process::kMaxTasks][process::kKernelStackSize];
uint32_t g_next_tid = 1;
bool g_init_pid_reserved = true;
sync::SpinLock g_address_space_lock;
sync::SpinLock g_resource_pool_lock;
sync::SpinLock g_thread_lock;
sync::SpinLock g_futex_lock;
sync::SpinLock g_child_lock;
sync::SpinLock g_control_lock;

struct Session {
    uint32_t id;
    uint32_t foreground_group_id;
    bool in_use;
};

Session g_sessions[process::kMaxTasks]{};

constexpr uint32_t kInitPid = 1;

}  // namespace

namespace process {

struct AddressSpace {
    uint64_t cr3;
    uint32_t refcount;
    bool in_use;
    bool exiting;
};

}  // namespace process

namespace {

process::AddressSpace g_address_spaces[process::kMaxTasks]{};
process::ProcessResources g_process_resources[process::kMaxTasks]{};

bool running_on_task_stack(const process::Task& proc) {
    uint64_t rsp = 0;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    return rsp >= proc.kernel_stack_base && rsp < proc.kernel_stack_top;
}

void reset_task(process::Task& proc) {
    proc.pid = 0;
    proc.address_space = nullptr;
    proc.resources = nullptr;
    proc.fs_base = 0;
    proc.user_ip = 0;
    proc.user_sp = 0;
    cpu::init_fpu_state(proc.fpu_state);
    proc.code_region = vm::Region{0, 0};
    proc.stack_region = vm::Stack{0, 0, 0};
    memset(&proc.context, 0, sizeof(proc.context));
    proc.parent = nullptr;
    proc.thread_joiner = nullptr;
    proc.waiting_on = nullptr;
    proc.wait_reason = process::WaitReason::None;
    proc.suspended_from = process::State::Ready;
    proc.wait_child_pid = 0;
    proc.exit_code = 0;
    proc.has_exited = false;
    proc.console_transferred = false;
    proc.has_context = false;
    proc.is_kernel_task = false;
    proc.is_thread = false;
    proc.thread_join_claimed = false;
    proc.child_wait_claimed = false;
    proc.core_dump_pending = false;
    proc.reclaim_pending = false;
    proc.reclaim_cpu = UINT32_MAX;
    proc.kernel_entry = nullptr;
    proc.preferred_cpu = UINT32_MAX;
    proc.sleep_until_tick = 0;
    proc.user_ticks = 0;
    proc.kernel_ticks = 0;
    proc.wait_descriptors_user = 0;
    proc.wait_descriptor_count = 0;
    proc.wait_descriptor_reserved = 0;
    proc.wait_result = 0;
    proc.wait_result_pending = false;
    proc.image_path[0] = '\0';
}

void initialize_shared_resources(process::ProcessResources& resources) {
    resources.vty_id = 0;
    resources.process_group_id = 0;
    resources.session_id = 0;
    resources.parent_process_id = 0;
    resources.tracer_process_id = 0;
    resources.cpu_ticks = 0;
    resources.limits = {
        .max_threads = 64,
        .max_descriptors = descriptor::kMaxDescriptors,
        .max_file_handles = process::kMaxFileHandles,
        .max_directory_handles = process::kMaxDirectoryHandles,
        .max_virtual_bytes = UINT64_MAX,
        .max_cpu_ticks = UINT64_MAX,
    };
    resources.event_head = 0;
    resources.event_count = 0;
    resources.cwd[0] = '/';
    resources.cwd[1] = '\0';
    for (size_t i = 0; i < 3; ++i) {
        resources.standard_descriptors[i] = descriptor::kInvalidHandle;
    }
    resources.principal = nullptr;
    capabilities::cap_table_clear(resources.cap_handles,
                                  capabilities::kMaxProcessCapabilities);
    descriptor::init_table(resources.descriptors);
    for (size_t fh = 0; fh < process::kMaxFileHandles; ++fh) {
        resources.file_handles[fh].in_use = false;
        resources.file_handles[fh].reserved = false;
        resources.file_handles[fh].can_write = false;
        resources.file_handles[fh].handle = {};
        resources.file_handles[fh].position = 0;
        resources.file_handles[fh].path[0] = '\0';
    }
    for (size_t dh = 0; dh < process::kMaxDirectoryHandles; ++dh) {
        resources.directory_handles[dh].in_use = false;
        resources.directory_handles[dh].reserved = false;
        resources.directory_handles[dh].handle = {};
        resources.directory_handles[dh].path[0] = '\0';
    }
}

bool retain_shared_resources(process::ProcessResources* resources) {
    if (resources == nullptr) {
        return false;
    }
    sync::IrqLockGuard guard(g_resource_pool_lock);
    if (!resources->in_use || resources->refcount == 0 ||
        resources->refcount == UINT32_MAX ||
        resources->refcount >= resources->limits.max_threads) {
        return false;
    }
    ++resources->refcount;
    return true;
}

void release_shared_resources(process::Task& proc) {
    process::ProcessResources* resources = proc.resources;
    if (resources == nullptr) {
        return;
    }
    bool destroy = false;
    {
        sync::IrqLockGuard guard(g_resource_pool_lock);
        if (!resources->in_use || resources->refcount == 0) {
            proc.resources = nullptr;
            return;
        }
        --resources->refcount;
        destroy = resources->refcount == 0;
    }
    proc.resources = nullptr;
    if (!destroy) {
        return;
    }

    for (size_t i = 0; i < process::kMaxFileHandles; ++i) {
        resources->file_locks[i].lock();
        if (resources->file_handles[i].in_use) {
            vfs::close_file(resources->file_handles[i].handle);
            resources->file_handles[i].in_use = false;
            resources->file_handles[i].can_write = false;
        }
        resources->file_locks[i].unlock();
    }
    for (size_t i = 0; i < process::kMaxDirectoryHandles; ++i) {
        resources->directory_locks[i].lock();
        if (resources->directory_handles[i].in_use) {
            vfs::close_directory(resources->directory_handles[i].handle);
            resources->directory_handles[i].in_use = false;
        }
        resources->directory_locks[i].unlock();
    }
    resources->descriptor_lock.lock();
    descriptor::destroy_table(proc, resources->descriptors);
    resources->descriptor_lock.unlock();
    if (resources->principal != nullptr) {
        capabilities::principal_release(resources->principal);
        resources->principal = nullptr;
    }
    capabilities::cap_table_clear(resources->cap_handles,
                                  capabilities::kMaxProcessCapabilities);

    sync::IrqLockGuard guard(g_resource_pool_lock);
    resources->in_use = false;
}

process::AddressSpace* create_address_space() {
    uint64_t cr3 = paging_create_address_space();
    if (cr3 == 0) {
        return nullptr;
    }
    sync::IrqLockGuard guard(g_address_space_lock);
    for (auto& space : g_address_spaces) {
        if (!space.in_use) {
            space.cr3 = cr3;
            space.refcount = 1;
            space.in_use = true;
            return &space;
        }
    }
    paging_destroy_address_space(cr3);
    return nullptr;
}

void release_address_space(process::AddressSpace* space) {
    if (space == nullptr) {
        return;
    }
    uint64_t cr3 = 0;
    {
        sync::IrqLockGuard guard(g_address_space_lock);
        if (!space->in_use || space->refcount == 0) {
            return;
        }
        --space->refcount;
        if (space->refcount != 0) {
            return;
        }
        cr3 = space->cr3;
        *space = {};
    }
    vm::release_address_space(cr3);
    paging_destroy_address_space(cr3);
}

bool tid_in_use(uint32_t tid) {
    if (tid == 0) {
        return true;
    }
    for (size_t i = 0; i < process::kMaxTasks; ++i) {
        const process::Task& proc = g_task_table[i];
        process::State state = process::load_state(proc);
        if (state != process::State::Unused &&
            (proc.tid == tid || proc.pid == tid)) {
            return true;
        }
    }
    return false;
}

uint32_t allocate_tid() {
    for (uint32_t attempts = 0; attempts < UINT32_MAX; ++attempts) {
        uint32_t tid = __atomic_fetch_add(&g_next_tid,
                                          uint32_t{1},
                                          __ATOMIC_RELAXED);
        if (tid == 0 ||
            (__atomic_load_n(&g_init_pid_reserved, __ATOMIC_ACQUIRE) &&
             tid == kInitPid)) {
            continue;
        }
        if (!tid_in_use(tid)) {
            return tid;
        }
    }
    return 0;
}

process::Task* allocate_slot(uint32_t tid) {
    if (tid == 0 || tid_in_use(tid)) {
        return nullptr;
    }
    for (size_t i = 0; i < process::kMaxTasks; ++i) {
        process::Task& proc = g_task_table[i];
        process::State expected = process::State::Unused;
        if (!process::compare_exchange_state(proc,
                                             expected,
                                             process::State::Allocating)) {
            continue;
        }
        memset(&proc.context, 0, sizeof(proc.context));
        reset_task(proc);
        proc.tid = tid;
        return &proc;
    }
    return nullptr;
}

}  // namespace

namespace process {

void init() {
    memset(g_task_table, 0, sizeof(g_task_table));
    memset(g_address_spaces, 0, sizeof(g_address_spaces));
    memset(g_process_resources, 0, sizeof(g_process_resources));
    memset(g_sessions, 0, sizeof(g_sessions));
    g_next_tid = 1;
    g_init_pid_reserved = true;
    for (size_t i = 0; i < kMaxTasks; ++i) {
        store_state(g_task_table[i], State::Unused);
        g_task_table[i].has_context = false;
        g_task_table[i].is_kernel_task = false;
        g_task_table[i].kernel_entry = nullptr;
        g_task_table[i].tid = 0;
        g_task_table[i].cr3 = paging_kernel_cr3();
        g_task_table[i].kernel_stack_base =
            reinterpret_cast<uint64_t>(&g_kernel_stacks[i][0]);
        g_task_table[i].kernel_stack_top =
            g_task_table[i].kernel_stack_base + kKernelStackSize;
        g_task_table[i].kernel_stack_top &= ~0xFULL;
        reset_task(g_task_table[i]);
    }
}

bool attach_new_resources(Task& proc) {
    ProcessResources* selected = nullptr;
    {
        sync::IrqLockGuard guard(g_resource_pool_lock);
        for (auto& resources : g_process_resources) {
            if (!resources.in_use) {
                resources.in_use = true;
                resources.refcount = 1;
                selected = &resources;
                break;
            }
        }
    }
    if (selected == nullptr) {
        return false;
    }
    initialize_shared_resources(*selected);
    selected->process_group_id = proc.tid;
    selected->session_id = proc.tid;
    proc.resources = selected;
    return true;
}

Task* allocate() {
    Task* proc = allocate_slot(allocate_tid());
    if (proc == nullptr) {
        return nullptr;
    }
    AddressSpace* space = create_address_space();
    if (space == nullptr) {
        proc->tid = 0;
        store_state(*proc, State::Unused);
        return nullptr;
    }
    proc->pid = proc->tid;
    proc->address_space = space;
    proc->cr3 = space->cr3;
    if (!attach_new_resources(*proc)) {
        release_address_space(space);
        proc->tid = 0;
        reset_task(*proc);
        store_state(*proc, State::Unused);
        return nullptr;
    }
    store_state(*proc, State::Ready);
    return proc;
}

Task* allocate_init_task() {
    if (!__atomic_load_n(&g_init_pid_reserved, __ATOMIC_ACQUIRE) ||
        tid_in_use(kInitPid)) {
        return nullptr;
    }
    Task* proc = allocate_slot(kInitPid);
    if (proc == nullptr) {
        return nullptr;
    }
    AddressSpace* space = create_address_space();
    if (space == nullptr) {
        proc->tid = 0;
        store_state(*proc, State::Unused);
        return nullptr;
    }
    proc->pid = proc->tid;
    proc->address_space = space;
    proc->cr3 = space->cr3;
    if (!attach_new_resources(*proc)) {
        release_address_space(space);
        proc->tid = 0;
        reset_task(*proc);
        store_state(*proc, State::Unused);
        return nullptr;
    }
    if (!create_session(*proc)) {
        release_shared_resources(*proc);
        release_address_space(space);
        proc->tid = 0;
        reset_task(*proc);
        store_state(*proc, State::Unused);
        return nullptr;
    }
    store_state(*proc, State::Ready);
    __atomic_store_n(&g_init_pid_reserved, false, __ATOMIC_RELEASE);
    uint32_t next_tid = __atomic_load_n(&g_next_tid, __ATOMIC_RELAXED);
    while (next_tid <= kInitPid &&
           !__atomic_compare_exchange_n(&g_next_tid,
                                        &next_tid,
                                        kInitPid + 1,
                                        false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
    return proc;
}

Task* create_user_thread(Task& owner,
                         uint64_t entry,
                         uint64_t argument,
                         size_t stack_size,
                         uint64_t tls_base) {
    if (owner.is_kernel_task || owner.address_space == nullptr ||
        owner.cr3 == 0 || !vm::is_user_range(entry, 1) ||
        (tls_base != 0 && !vm::is_user_range(tls_base, 1))) {
        return nullptr;
    }
    uint64_t entry_flags = 0;
    if (!paging_flags_cr3(owner.cr3, entry, entry_flags) ||
        (entry_flags & PAGE_FLAG_USER) == 0 ||
        (entry_flags & PAGE_FLAG_NO_EXECUTE) != 0) {
        return nullptr;
    }
    if (stack_size == 0) {
        stack_size = kDefaultThreadStackSize;
    }
    if (stack_size < kMinThreadStackSize ||
        stack_size > kMaxThreadStackSize) {
        return nullptr;
    }
    vm::Usage memory_usage = vm::usage(owner.cr3);
    if (memory_usage.virtual_bytes >
            owner.resources->limits.max_virtual_bytes ||
        stack_size > owner.resources->limits.max_virtual_bytes -
                         memory_usage.virtual_bytes) {
        return nullptr;
    }

    Task* thread = allocate_slot(allocate_tid());
    if (thread == nullptr) {
        return nullptr;
    }
    {
        sync::IrqLockGuard guard(g_address_space_lock);
        AddressSpace* space = owner.address_space;
        if (!space->in_use || space->exiting ||
            space->refcount == UINT32_MAX) {
            thread->tid = 0;
            store_state(*thread, State::Unused);
            return nullptr;
        }
        ++space->refcount;
        thread->pid =
            owner.pid != 0 ? owner.pid : owner.tid;
        thread->address_space = space;
        thread->cr3 = space->cr3;
    }
    if (!retain_shared_resources(owner.resources)) {
        release_address_space(thread->address_space);
        thread->tid = 0;
        reset_task(*thread);
        store_state(*thread, State::Unused);
        return nullptr;
    }
    thread->resources = owner.resources;

    thread->is_thread = true;
    // Keep every thread belonging to init on the BSP.  Init's login loop and
    // service manager share process resources during early userspace startup,
    // and splitting those threads across CPUs can leave the login thread no
    // longer making progress.  Threads in ordinary applications remain free
    // for the scheduler to distribute across CPUs.
    thread->preferred_cpu = owner.pid == 1 ? owner.preferred_cpu : UINT32_MAX;
    thread->fs_base = tls_base != 0 ? tls_base : owner.fs_base;
    string_util::copy(thread->image_path,
                      sizeof(thread->image_path),
                      owner.image_path);

    thread->stack_region = vm::allocate_user_stack(thread->cr3, stack_size);
    if (thread->stack_region.top == 0) {
        release_shared_resources(*thread);
        release_address_space(thread->address_space);
        thread->tid = 0;
        reset_task(*thread);
        store_state(*thread, State::Unused);
        return nullptr;
    }

    thread->user_ip = entry;
    // A userspace thread entry is an ordinary System V AMD64 function, but
    // the scheduler enters it directly instead of using `call`. Emulate the
    // call instruction's eight-byte return-address push so the function sees
    // RSP % 16 == 8 on entry. Compilers rely on this for aligned SSE spills.
    thread->user_sp =
        ((thread->stack_region.top - 16ull) & ~0xFull) - sizeof(uint64_t);
    memset(&thread->context, 0, sizeof(thread->context));
    thread->context.user_rip = thread->user_ip;
    thread->context.user_rsp = thread->user_sp;
    thread->context.user_rflags = 0x202;
    thread->context.r11 = 0x202;
    thread->context.rdi = argument;
    thread->context.rax = 0;
    thread->has_context = true;
    store_state(*thread, State::Ready);
    scheduler::enqueue(thread);
    log_message(LogLevel::Debug,
                "Thread: created tid=%u pid=%u entry=%016llx stack=%016llx..%016llx",
                static_cast<unsigned int>(thread->tid),
                static_cast<unsigned int>(thread->pid),
                static_cast<unsigned long long>(thread->user_ip),
                static_cast<unsigned long long>(thread->stack_region.base),
                static_cast<unsigned long long>(thread->stack_region.top));
    return thread;
}

bool set_thread_tls(Task& thread, uint64_t fs_base) {
    if (fs_base != 0 && !vm::is_user_range(fs_base, 1)) {
        return false;
    }
    thread.fs_base = fs_base;
    if (&thread == current()) {
        cpu::write_fs_base(fs_base);
    }
    return true;
}

Task* current() {
    return percpu::get_current_task();
}

void set_current(Task* proc) {
    percpu::set_current_task(proc);
    if (proc != nullptr) {
        uint64_t target_cr3 =
            (proc->cr3 != 0) ? proc->cr3 : paging_kernel_cr3();
        if (target_cr3 != 0) {
            paging_switch_cr3(target_cr3);
        }
        cpu::write_fs_base(proc->fs_base);
    }
}

Task* task_table_entry(size_t index) {
    if (index >= kMaxTasks) {
        return nullptr;
    }
    return &g_task_table[index];
}

Task* find_by_tid(uint32_t tid) {
    if (tid == 0) {
        return nullptr;
    }
    for (size_t i = 0; i < kMaxTasks; ++i) {
        Task& p = g_task_table[i];
        if (load_state(p) != State::Unused && p.tid == tid) {
            return &p;
        }
    }
    return nullptr;
}

void record_tick(bool user_mode) {
    Task* proc = current();
    if (proc == nullptr) {
        return;
    }
    if (user_mode) {
        ++proc->user_ticks;
    } else {
        ++proc->kernel_ticks;
    }
    if (proc->resources != nullptr) {
        uint64_t total = __atomic_add_fetch(&proc->resources->cpu_ticks,
                                            uint64_t{1},
                                            __ATOMIC_RELAXED);
        uint64_t limit = __atomic_load_n(
            &proc->resources->limits.max_cpu_ticks,
            __ATOMIC_RELAXED);
        if (limit != UINT64_MAX && total > limit) {
            terminate_group(*proc, 137);
        }
    }
}

size_t usage_snapshot(descriptor_defs::TaskUsage* out, size_t max_entries) {
    if (out == nullptr || max_entries == 0) {
        return 0;
    }

    size_t written = 0;
    for (size_t i = 0; i < kMaxTasks && written < max_entries; ++i) {
        const Task& proc = g_task_table[i];
        State state = load_state(proc);
        if (state == State::Unused || proc.tid == 0) {
            continue;
        }

        descriptor_defs::TaskUsage& snapshot = out[written++];
        snapshot.pid = proc.tid;
        snapshot.parent_pid =
            proc.resources != nullptr
                ? proc.resources->parent_process_id
                : 0;
        snapshot.state = static_cast<uint32_t>(state);
        snapshot.flags = 0;
        if (proc.is_kernel_task) {
            snapshot.flags |= descriptor_defs::kTaskStatFlagKernel;
        }
        if (proc.has_exited) {
            snapshot.flags |= descriptor_defs::kTaskStatFlagExited;
        }
        if (proc.is_thread) {
            snapshot.flags |= descriptor_defs::kTaskStatFlagThread;
        }
        snapshot.preferred_cpu = proc.preferred_cpu;
        snapshot.process_id = proc.pid;
        snapshot.user_ticks = proc.user_ticks;
        snapshot.kernel_ticks = proc.kernel_ticks;
        vm::Usage memory_usage = vm::usage(proc.cr3);
        snapshot.virtual_bytes = memory_usage.virtual_bytes;
        snapshot.resident_bytes = memory_usage.resident_bytes;
        snapshot.shared_bytes = memory_usage.shared_bytes;
        snapshot.file_bytes = memory_usage.file_bytes;
        string_util::copy(snapshot.image_path,
                          sizeof(snapshot.image_path),
                          proc.image_path[0] != '\0' ? proc.image_path : "(kernel)");
    }
    return written;
}

void wake_ready_sleepers(uint64_t current_tick) {
    for (size_t i = 0; i < kMaxTasks; ++i) {
        Task& proc = g_task_table[i];
        State state = load_state(proc);
        if ((state != State::Blocked &&
             !(state == State::Suspended &&
               proc.suspended_from == State::Blocked)) ||
            proc.sleep_until_tick == 0) {
            continue;
        }
        if (current_tick < proc.sleep_until_tick) {
            continue;
        }
        proc.sleep_until_tick = 0;
        if (proc.wait_reason == WaitReason::Futex) {
            (void)wake_with_result(proc, -3);
        } else {
            (void)wake(proc);
        }
    }
}

bool wake(Task& proc) {
    if (load_state(proc) == State::Suspended &&
        proc.suspended_from == State::Blocked) {
        proc.waiting_on = nullptr;
        proc.wait_reason = WaitReason::None;
        proc.suspended_from = State::Ready;
        return true;
    }
    if (!begin_wake(proc)) {
        return false;
    }
    finish_wake(proc);
    return true;
}

bool begin_wake(Task& proc) {
    State expected = State::Blocked;
    if (!compare_exchange_state(proc, expected, State::Waking)) {
        return false;
    }
    return true;
}

void finish_wake(Task& proc) {
    proc.waiting_on = nullptr;
    proc.wait_reason = WaitReason::None;
    store_state(proc, State::Ready);
    scheduler::enqueue(&proc);
}

void finish_wake_with_result(Task& proc, int64_t result) {
    proc.wait_result = result;
    __atomic_store_n(&proc.wait_result_pending, true, __ATOMIC_RELEASE);
    finish_wake(proc);
}

bool wake_with_result(Task& proc, int64_t result) {
    if (load_state(proc) == State::Suspended &&
        proc.suspended_from == State::Blocked) {
        proc.wait_result = result;
        __atomic_store_n(&proc.wait_result_pending,
                         true,
                         __ATOMIC_RELEASE);
        proc.waiting_on = nullptr;
        proc.wait_reason = WaitReason::None;
        proc.suspended_from = State::Ready;
        return true;
    }
    if (!begin_wake(proc)) {
        return false;
    }
    finish_wake_with_result(proc, result);
    return true;
}

void terminate(Task& proc, uint16_t exit_code) {
    State state = load_state(proc);
    for (;;) {
        if (state == State::Waking) {
            asm volatile("pause");
            state = load_state(proc);
            continue;
        }
        if (state == State::Unused || state == State::Allocating ||
            state == State::Terminated || state == State::Reclaiming) {
            return;
        }
        if (compare_exchange_state(proc, state, State::Terminated)) {
            break;
        }
    }
    proc.has_exited = true;
    proc.exit_code = exit_code;
    if (proc.is_thread) {
        log_message(LogLevel::Debug,
                    "Thread: tid=%u exited code=%u",
                    static_cast<unsigned int>(proc.tid),
                    static_cast<unsigned int>(exit_code));
    }

    Task* thread_joiner = nullptr;
    {
        sync::IrqLockGuard guard(g_thread_lock);
        if (proc.is_thread && proc.thread_join_claimed) {
            thread_joiner = proc.thread_joiner;
            proc.thread_joiner = nullptr;
        }
    }
    if (thread_joiner != nullptr &&
        thread_joiner->waiting_on == &proc &&
        thread_joiner->wait_reason == WaitReason::ThreadJoin) {
        (void)wake_with_result(*thread_joiner, proc.exit_code);
    }

    Task* parent = proc.parent;
    if (parent != nullptr && parent->waiting_on == &proc) {
        {
            sync::IrqLockGuard guard(g_child_lock);
            proc.child_wait_claimed = true;
        }
        if (begin_wake(*parent)) {
            if (parent->console_transferred) {
                descriptor::restore_console_owner(*parent);
                parent->console_transferred = false;
            }
            finish_wake_with_result(*parent, proc.exit_code);
        }
        proc.parent = nullptr;
    } else if (!proc.is_thread && proc.resources != nullptr) {
        sync::IrqLockGuard guard(g_child_lock);
        for (size_t i = 0; i < kMaxTasks; ++i) {
            Task& waiter = g_task_table[i];
            if (waiter.pid != proc.resources->parent_process_id ||
                waiter.wait_reason != WaitReason::Child ||
                (waiter.wait_child_pid != 0 &&
                 waiter.wait_child_pid != proc.tid)) {
                continue;
            }
            proc.child_wait_claimed = true;
            proc.parent = nullptr;
            uint64_t result =
                (static_cast<uint64_t>(proc.tid) << 16) | proc.exit_code;
            (void)wake_with_result(waiter,
                                   static_cast<int64_t>(result));
            break;
        }
    }
}

void terminate_group(Task& proc, uint16_t exit_code) {
    const uint32_t process_id =
        proc.pid != 0 ? proc.pid : proc.tid;
    if (proc.address_space != nullptr) {
        sync::IrqLockGuard guard(g_address_space_lock);
        if (proc.address_space->in_use) {
            proc.address_space->exiting = true;
        }
    }

    for (size_t i = 0; i < kMaxTasks; ++i) {
        Task& member = g_task_table[i];
        if (&member == &proc || member.pid != process_id ||
            load_state(member) == State::Unused) {
            continue;
        }
        if (member.is_thread) {
            sync::IrqLockGuard guard(g_thread_lock);
            member.thread_join_claimed = true;
        }
        terminate(member, exit_code);
    }
    if (proc.is_thread) {
        sync::IrqLockGuard guard(g_thread_lock);
        proc.thread_join_claimed = true;
    }
    terminate(proc, exit_code);
}

bool join_thread(Task& caller,
                 uint32_t thread_id,
                 int64_t& immediate_result,
                 bool& blocked) {
    immediate_result = -1;
    blocked = false;
    if (thread_id == 0 || thread_id == caller.tid) {
        return false;
    }

    Task* target = nullptr;
    bool arm_reclaim = false;
    {
        sync::IrqLockGuard guard(g_thread_lock);
        target = find_by_tid(thread_id);
        if (target == nullptr || !target->is_thread ||
            target->pid != caller.pid ||
            target->thread_join_claimed) {
            return false;
        }
        target->thread_join_claimed = true;
        if (load_state(*target) == State::Terminated) {
            immediate_result = target->exit_code;
            arm_reclaim =
                __atomic_load_n(&target->reclaim_cpu,
                                __ATOMIC_ACQUIRE) != UINT32_MAX;
        } else {
            target->thread_joiner = &caller;
            caller.waiting_on = target;
            caller.wait_reason = WaitReason::ThreadJoin;
            store_state(caller, State::Blocked);
            blocked = true;
        }
    }
    if (arm_reclaim) {
        __atomic_store_n(&target->reclaim_pending,
                         true,
                         __ATOMIC_RELEASE);
    }
    return true;
}

bool detach_thread(Task& caller, uint32_t thread_id) {
    if (thread_id == 0) return false;
    Task* target = nullptr;
    bool arm_reclaim = false;
    {
        sync::IrqLockGuard guard(g_thread_lock);
        target = find_by_tid(thread_id);
        if (target == nullptr || !target->is_thread ||
            target->pid != caller.pid ||
            target->thread_join_claimed) {
            return false;
        }
        target->thread_join_claimed = true;
        arm_reclaim = load_state(*target) == State::Terminated &&
            __atomic_load_n(&target->reclaim_cpu, __ATOMIC_ACQUIRE) !=
                UINT32_MAX;
    }
    if (arm_reclaim) {
        __atomic_store_n(&target->reclaim_pending, true, __ATOMIC_RELEASE);
    }
    return true;
}

int64_t wait_child(Task& caller,
                   uint32_t child_pid,
                   bool nonblocking) {
    sync::IrqLockGuard guard(g_child_lock);
    bool found_child = false;
    for (size_t i = 0; i < kMaxTasks; ++i) {
        Task& child = g_task_table[i];
        if (child.is_thread || child.resources == nullptr ||
            child.resources->parent_process_id != caller.pid ||
            (child_pid != 0 && child.tid != child_pid) ||
            child.child_wait_claimed) {
            continue;
        }
        found_child = true;
        if (load_state(child) != State::Terminated) {
            continue;
        }
        child.child_wait_claimed = true;
        child.parent = nullptr;
        if (__atomic_load_n(&child.reclaim_cpu,
                            __ATOMIC_ACQUIRE) != UINT32_MAX) {
            __atomic_store_n(&child.reclaim_pending,
                             true,
                             __ATOMIC_RELEASE);
        }
        return static_cast<int64_t>(
            (static_cast<uint64_t>(child.tid) << 16) | child.exit_code);
    }
    if (!found_child || nonblocking) {
        return found_child ? -2 : -1;
    }
    caller.wait_child_pid = child_pid;
    caller.wait_reason = WaitReason::Child;
    caller.waiting_on = nullptr;
    store_state(caller, State::Blocked);
    return 0;
}

bool send_event(Task& sender,
                uint32_t target_pid,
                const ProcessEvent& event) {
    Task* target = find_by_tid(target_pid);
    if (target == nullptr || target->resources == nullptr) {
        return false;
    }
    ProcessResources& resources = *target->resources;
    {
        sync::LockGuard guard(resources.event_lock);
        KERNEL_ASSERT_MSG(resources.event_head < kMaxProcessEvents,
                          "process event queue head is out of bounds");
        KERNEL_ASSERT_MSG(resources.event_count <= kMaxProcessEvents,
                          "process event queue count is out of bounds");
        if (resources.event_count >= kMaxProcessEvents) {
            return false;
        }
        size_t tail =
            (resources.event_head + resources.event_count) % kMaxProcessEvents;
        resources.events[tail] = event;
        resources.events[tail].sender_process_id = sender.pid;
        ++resources.event_count;
    }
    for (size_t i = 0; i < kMaxTasks; ++i) {
        Task& waiter = g_task_table[i];
        if (waiter.resources == &resources &&
            waiter.wait_reason == WaitReason::Event) {
            (void)wake_with_result(waiter, 1);
            break;
        }
    }
    return true;
}

int64_t receive_event(Task& caller,
                      ProcessEvent& event,
                      bool nonblocking) {
    ProcessResources& resources = *caller.resources;
    sync::LockGuard guard(resources.event_lock);
    KERNEL_ASSERT_MSG(resources.event_head < kMaxProcessEvents,
                      "process event queue head is out of bounds");
    KERNEL_ASSERT_MSG(resources.event_count <= kMaxProcessEvents,
                      "process event queue count is out of bounds");
    if (resources.event_count != 0) {
        event = resources.events[resources.event_head];
        resources.event_head =
            (resources.event_head + 1) % kMaxProcessEvents;
        --resources.event_count;
        return 0;
    }
    if (nonblocking) {
        return -2;
    }
    // Publish the wait state while holding the queue lock so a sender cannot
    // enqueue between the empty check and waiter registration.
    caller.wait_reason = WaitReason::Event;
    caller.waiting_on = nullptr;
    store_state(caller, State::Blocked);
    return 1;
}

bool control_group(Task&,
                   uint32_t target_pid,
                   uint32_t action,
                   uint16_t exit_code) {
    Task* target = find_by_tid(target_pid);
    if (target == nullptr || target->is_kernel_task) {
        return false;
    }
    uint32_t target_process_id = target->pid;
    if (action == 1) {
        terminate_group(*target, exit_code);
        return true;
    }
    if (action != 2 && action != 3) {
        return false;
    }

    bool changed = false;
    sync::IrqLockGuard guard(g_control_lock);
    for (size_t i = 0; i < kMaxTasks; ++i) {
        Task& member = g_task_table[i];
        if (member.pid != target_process_id) {
            continue;
        }
        State state = load_state(member);
        if (action == 2) {
            if (state == State::Ready || state == State::Blocked) {
                member.suspended_from = state;
                State expected = state;
                if (compare_exchange_state(member,
                                           expected,
                                           State::Suspended)) {
                    scheduler::remove(&member);
                    changed = true;
                }
            } else if (state == State::Running) {
                member.suspended_from = state;
                State expected = State::Running;
                changed |= compare_exchange_state(member,
                                                  expected,
                                                  State::Suspending);
            } else if (state == State::Suspending ||
                       state == State::Suspended) {
                changed = true;
            }
            continue;
        }

        if (state == State::Suspended) {
            State resume_state =
                member.suspended_from == State::Blocked
                    ? State::Blocked
                    : State::Ready;
            store_state(member, resume_state);
            if (resume_state == State::Ready) {
                scheduler::enqueue(&member);
            }
            changed = true;
        } else if (state == State::Suspending) {
            store_state(member, State::Running);
            changed = true;
        }
    }
    return changed;
}

bool set_process_group(Task& caller,
                       uint32_t target_pid,
                       uint32_t group_id) {
    Task* target = target_pid == 0 ? &caller : find_by_tid(target_pid);
    if (target == nullptr || target->resources == nullptr ||
        target->is_thread || target->resources->session_id == 0) {
        return false;
    }
    if (group_id == 0) {
        group_id = target->pid;
    }
    bool group_exists = group_id == target->pid;
    for (size_t i = 0; i < kMaxTasks; ++i) {
        Task& member = g_task_table[i];
        if (member.resources == nullptr || member.is_thread ||
            member.resources->process_group_id != group_id) {
            continue;
        }
        if (member.resources->session_id !=
            target->resources->session_id) {
            return false;
        }
        group_exists = true;
    }
    if (!group_exists) {
        return false;
    }
    sync::LockGuard guard(target->resources->lock);
    target->resources->process_group_id = group_id;
    return true;
}

uint32_t process_group_id(const Task& proc) {
    return proc.resources != nullptr
               ? proc.resources->process_group_id
               : 0;
}

uint32_t session_id(const Task& proc) {
    return proc.resources != nullptr ? proc.resources->session_id : 0;
}

bool create_session(Task& caller) {
    if (caller.is_thread || caller.resources == nullptr) {
        return false;
    }
    sync::IrqLockGuard guard(g_control_lock);
    uint32_t id = caller.pid;
    for (auto& session : g_sessions) {
        if (session.in_use && session.id == id) {
            return false;
        }
    }
    for (auto& session : g_sessions) {
        if (!session.in_use) {
            session.in_use = true;
            session.id = id;
            session.foreground_group_id = id;
            caller.resources->session_id = id;
            caller.resources->process_group_id = id;
            return true;
        }
    }
    return false;
}

bool set_foreground_group(Task& caller, uint32_t group_id) {
    if (caller.resources == nullptr || group_id == 0) {
        return false;
    }
    uint32_t sid = caller.resources->session_id;
    bool group_exists = false;
    for (size_t i = 0; i < kMaxTasks; ++i) {
        Task& member = g_task_table[i];
        if (member.resources != nullptr &&
            member.resources->session_id == sid &&
            member.resources->process_group_id == group_id) {
            group_exists = true;
            break;
        }
    }
    if (!group_exists) {
        return false;
    }
    sync::IrqLockGuard guard(g_control_lock);
    for (auto& session : g_sessions) {
        if (session.in_use && session.id == sid) {
            session.foreground_group_id = group_id;
            return true;
        }
    }
    for (auto& session : g_sessions) {
        if (!session.in_use) {
            session.in_use = true;
            session.id = sid;
            session.foreground_group_id = group_id;
            return true;
        }
    }
    return false;
}

uint32_t foreground_group(const Task& caller) {
    if (caller.resources == nullptr) {
        return 0;
    }
    uint32_t sid = caller.resources->session_id;
    sync::IrqLockGuard guard(g_control_lock);
    for (const auto& session : g_sessions) {
        if (session.in_use && session.id == sid) {
            return session.foreground_group_id;
        }
    }
    return 0;
}

bool is_foreground(const Task& proc) {
    return proc.resources != nullptr &&
           foreground_group(proc) == proc.resources->process_group_id;
}

bool get_limits(const Task& proc, ProcessLimits& limits) {
    if (proc.resources == nullptr) {
        return false;
    }
    sync::LockGuard guard(proc.resources->lock);
    limits = proc.resources->limits;
    return true;
}

bool set_limits(Task& proc, const ProcessLimits& limits) {
    if (proc.resources == nullptr || limits.max_threads == 0 ||
        limits.max_descriptors == 0 ||
        limits.max_descriptors > descriptor::kMaxDescriptors ||
        limits.max_file_handles == 0 ||
        limits.max_file_handles > kMaxFileHandles ||
        limits.max_directory_handles == 0 ||
        limits.max_directory_handles > kMaxDirectoryHandles ||
        limits.max_virtual_bytes == 0 || limits.max_cpu_ticks == 0) {
        return false;
    }
    sync::LockGuard guard(proc.resources->lock);
    proc.resources->limits = limits;
    return true;
}

bool get_usage(const Task& proc, ProcessUsage& usage) {
    if (proc.resources == nullptr) {
        return false;
    }
    memset(&usage, 0, sizeof(usage));
    for (size_t i = 0; i < kMaxTasks; ++i) {
        if (g_task_table[i].resources == proc.resources &&
            load_state(g_task_table[i]) != State::Unused) {
            ++usage.threads;
        }
    }
    {
        sync::LockGuard guard(proc.resources->descriptor_lock);
        for (const auto& entry : proc.resources->descriptors.entries) {
            usage.descriptors += entry.in_use ? 1u : 0u;
        }
    }
    for (size_t i = 0; i < kMaxFileHandles; ++i) {
        usage.file_handles +=
            __atomic_load_n(&proc.resources->file_handles[i].in_use,
                            __ATOMIC_ACQUIRE)
                ? 1u
                : 0u;
    }
    for (size_t i = 0; i < kMaxDirectoryHandles; ++i) {
        usage.directory_handles +=
            __atomic_load_n(&proc.resources->directory_handles[i].in_use,
                            __ATOMIC_ACQUIRE)
                ? 1u
                : 0u;
    }
    vm::Usage memory = vm::usage(proc.cr3);
    usage.virtual_bytes = memory.virtual_bytes;
    usage.resident_bytes = memory.resident_bytes;
    usage.cpu_ticks =
        __atomic_load_n(&proc.resources->cpu_ticks, __ATOMIC_RELAXED);
    return true;
}

namespace {

bool trace_authorized(const Task& tracer, const Task& target) {
    return target.resources != nullptr &&
           target.resources->tracer_process_id == tracer.pid;
}

bool group_is_stopped(uint32_t process_id) {
    for (size_t i = 0; i < kMaxTasks; ++i) {
        const Task& member = g_task_table[i];
        if (member.pid != process_id) {
            continue;
        }
        State state = load_state(member);
        if (state != State::Suspended && state != State::Terminated &&
            state != State::Reclaiming) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool trace_attach(Task& tracer, uint32_t target_pid) {
    Task* target = find_by_tid(target_pid);
    if (target == nullptr || target->resources == nullptr ||
        target->pid == tracer.pid || target->is_kernel_task) {
        return false;
    }
    {
        sync::LockGuard guard(target->resources->lock);
        if (target->resources->tracer_process_id != 0 &&
            target->resources->tracer_process_id != tracer.pid) {
            return false;
        }
        target->resources->tracer_process_id = tracer.pid;
    }
    return control_group(tracer, target_pid, 2, 0);
}

bool trace_detach(Task& tracer, uint32_t target_pid) {
    Task* target = find_by_tid(target_pid);
    if (target == nullptr || !trace_authorized(tracer, *target)) {
        return false;
    }
    {
        sync::LockGuard guard(target->resources->lock);
        target->resources->tracer_process_id = 0;
    }
    return control_group(tracer, target_pid, 3, 0);
}

bool trace_read_memory(Task& tracer,
                       uint32_t target_pid,
                       uint64_t target_address,
                       uint64_t user_buffer,
                       size_t length) {
    Task* target = find_by_tid(target_pid);
    if (target == nullptr || length == 0 ||
        !trace_authorized(tracer, *target) ||
        !group_is_stopped(target->pid)) {
        return false;
    }
    uint8_t bounce[256];
    size_t copied = 0;
    while (copied < length) {
        size_t chunk = length - copied;
        if (chunk > sizeof(bounce)) {
            chunk = sizeof(bounce);
        }
        if (!vm::copy_from_user(target->cr3,
                                bounce,
                                target_address + copied,
                                chunk) ||
            !vm::copy_to_user(tracer.cr3,
                              user_buffer + copied,
                              bounce,
                              chunk)) {
            memset(bounce, 0, sizeof(bounce));
            return false;
        }
        copied += chunk;
    }
    memset(bounce, 0, sizeof(bounce));
    return true;
}

bool trace_write_memory(Task& tracer,
                        uint32_t target_pid,
                        uint64_t target_address,
                        uint64_t user_buffer,
                        size_t length) {
    Task* target = find_by_tid(target_pid);
    if (target == nullptr || length == 0 ||
        !trace_authorized(tracer, *target) ||
        !group_is_stopped(target->pid)) {
        return false;
    }
    uint8_t bounce[256];
    size_t copied = 0;
    while (copied < length) {
        size_t chunk = length - copied;
        if (chunk > sizeof(bounce)) {
            chunk = sizeof(bounce);
        }
        if (!vm::copy_from_user(tracer.cr3,
                                bounce,
                                user_buffer + copied,
                                chunk) ||
            !vm::copy_to_user(target->cr3,
                              target_address + copied,
                              bounce,
                              chunk)) {
            memset(bounce, 0, sizeof(bounce));
            return false;
        }
        copied += chunk;
    }
    memset(bounce, 0, sizeof(bounce));
    return true;
}

bool trace_get_registers(Task& tracer,
                         uint32_t target_tid,
                         uint64_t user_buffer,
                         size_t length) {
    Task* target = find_by_tid(target_tid);
    if (target == nullptr || length < sizeof(target->context) ||
        !trace_authorized(tracer, *target) ||
        load_state(*target) != State::Suspended) {
        return false;
    }
    return vm::copy_to_user(tracer.cr3,
                            user_buffer,
                            &target->context,
                            sizeof(target->context));
}

bool trace_stopped(Task& tracer, uint32_t target_pid) {
    Task* target = find_by_tid(target_pid);
    return target != nullptr && trace_authorized(tracer, *target) &&
           group_is_stopped(target->pid);
}

int64_t futex_wait(Task& caller,
                   uint64_t user_address,
                   uint32_t expected) {
    if ((user_address & (alignof(uint32_t) - 1)) != 0 ||
        !vm::is_user_range(user_address, sizeof(uint32_t))) {
        return -1;
    }
    sync::IrqLockGuard guard(g_futex_lock);
    uint32_t observed = 0;
    if (!vm::copy_from_user(caller.cr3,
                            &observed,
                            user_address,
                            sizeof(observed))) {
        return -1;
    }
    if (observed != expected) {
        return -2;
    }
    caller.waiting_on = reinterpret_cast<void*>(user_address);
    caller.wait_reason = WaitReason::Futex;
    store_state(caller, State::Blocked);
    return 0;
}

int64_t futex_wait_timed(Task& caller,
                         uint64_t user_address,
                         uint32_t expected,
                         uint64_t timeout_ns) {
    if (timeout_ns == 0) return -3;
    if ((user_address & (alignof(uint32_t) - 1)) != 0 ||
        !vm::is_user_range(user_address, sizeof(uint32_t))) {
        return -1;
    }
    sync::IrqLockGuard guard(g_futex_lock);
    uint32_t observed = 0;
    if (!vm::copy_from_user(caller.cr3,
                            &observed,
                            user_address,
                            sizeof(observed))) {
        return -1;
    }
    if (observed != expected) return -2;
    uint64_t ticks = timekeeping::ticks_for_duration_ns(timeout_ns);
    uint64_t now = timekeeping::tick_count();
    caller.sleep_until_tick =
        ticks > UINT64_MAX - now ? UINT64_MAX : now + ticks;
    caller.waiting_on = reinterpret_cast<void*>(user_address);
    caller.wait_reason = WaitReason::Futex;
    store_state(caller, State::Blocked);
    return 0;
}

size_t futex_wake(Task& caller,
                  uint64_t user_address,
                  size_t max_count) {
    if ((user_address & (alignof(uint32_t) - 1)) != 0 ||
        !vm::is_user_range(user_address, sizeof(uint32_t)) ||
        max_count == 0) {
        return 0;
    }
    size_t woken = 0;
    sync::IrqLockGuard guard(g_futex_lock);
    for (size_t i = 0; i < kMaxTasks && woken < max_count; ++i) {
        Task& waiter = g_task_table[i];
        if (waiter.cr3 != caller.cr3 ||
            waiter.wait_reason != WaitReason::Futex ||
            waiter.waiting_on != reinterpret_cast<void*>(user_address)) {
            continue;
        }
        waiter.sleep_until_tick = 0;
        if (wake_with_result(waiter, 0)) {
            ++woken;
        }
    }
    return woken;
}

bool consume_wait_result(Task& proc, int64_t& out_result) {
    if (!__atomic_exchange_n(&proc.wait_result_pending,
                             false,
                             __ATOMIC_ACQ_REL)) {
        return false;
    }
    out_result = proc.wait_result;
    return true;
}

void defer_reclaim(Task& proc) {
    State state = load_state(proc);
    if (state != State::Terminated) {
        return;
    }
    const percpu::Cpu* cpu = percpu::current_cpu();
    if (cpu == nullptr || cpu->index >= percpu::kMaxCpus) {
        return;
    }
    __atomic_store_n(&proc.reclaim_cpu, cpu->index, __ATOMIC_RELAXED);
    if (proc.is_thread) {
        sync::IrqLockGuard guard(g_thread_lock);
        if (!proc.thread_join_claimed) {
            return;
        }
    } else if (proc.resources != nullptr &&
               proc.resources->parent_process_id != 0 &&
               !proc.child_wait_claimed) {
        return;
    }
    __atomic_store_n(&proc.reclaim_pending, true, __ATOMIC_RELEASE);
}

void reap_deferred() {
    const percpu::Cpu* cpu = percpu::current_cpu();
    if (cpu == nullptr || cpu->index >= percpu::kMaxCpus) {
        return;
    }
    for (size_t i = 0; i < kMaxTasks; ++i) {
        Task& proc = g_task_table[i];
        if (!__atomic_load_n(&proc.reclaim_pending, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&proc.core_dump_pending, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&proc.reclaim_cpu, __ATOMIC_RELAXED) != cpu->index ||
            running_on_task_stack(proc)) {
            continue;
        }
        bool expected = true;
        if (!__atomic_compare_exchange_n(&proc.reclaim_pending,
                                         &expected,
                                         false,
                                         false,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            continue;
        }
        reclaim(proc);
    }
}

Task* allocate_kernel_task(void (*entry)(Task&)) {
    if (entry == nullptr) {
        return nullptr;
    }
    Task* proc = allocate_slot(allocate_tid());
    if (proc == nullptr) {
        return nullptr;
    }
    proc->cr3 = paging_kernel_cr3();
    proc->pid = proc->tid;
    proc->is_kernel_task = true;
    proc->kernel_entry = entry;
    if (!attach_new_resources(*proc)) {
        proc->tid = 0;
        reset_task(*proc);
        store_state(*proc, State::Unused);
        return nullptr;
    }
    store_state(*proc, State::Ready);
    return proc;
}

void reclaim(Task& proc) {
    if (__atomic_load_n(&proc.core_dump_pending, __ATOMIC_ACQUIRE)) {
        defer_reclaim(proc);
        return;
    }
    if (running_on_task_stack(proc)) {
        defer_reclaim(proc);
        return;
    }

    State state = load_state(proc);
    for (;;) {
        if (state != State::Ready && state != State::Terminated) {
            return;
        }
        if (compare_exchange_state(proc, state, State::Reclaiming)) {
            break;
        }
    }
    __atomic_store_n(&proc.reclaim_pending, false, __ATOMIC_RELEASE);
    __atomic_store_n(&proc.reclaim_cpu, UINT32_MAX, __ATOMIC_RELAXED);
    scheduler::remove(&proc);

    uint32_t reclaimed_process_id = proc.pid;
    uint32_t reclaimed_session_id =
        proc.resources != nullptr ? proc.resources->session_id : 0;
    if (!proc.is_thread) {
        for (size_t i = 0; i < kMaxTasks; ++i) {
            Task& child = g_task_table[i];
            if (child.resources != nullptr &&
                child.resources->parent_process_id ==
                    reclaimed_process_id) {
                child.resources->parent_process_id = 0;
                child.parent = nullptr;
                child.child_wait_claimed = true;
                if (load_state(child) == State::Terminated &&
                    __atomic_load_n(&child.reclaim_cpu,
                                    __ATOMIC_ACQUIRE) != UINT32_MAX) {
                    __atomic_store_n(&child.reclaim_pending,
                                     true,
                                     __ATOMIC_RELEASE);
                }
            }
            if (child.resources != nullptr &&
                child.resources->tracer_process_id ==
                    reclaimed_process_id) {
                child.resources->tracer_process_id = 0;
                (void)control_group(proc, child.tid, 3, 0);
            }
        }
    }

    release_shared_resources(proc);
    loader::release_dynamic_objects(proc);
    vm::release_user_region(proc.cr3,
                            vm::Region{proc.stack_region.base,
                                       proc.stack_region.length});
    if (!proc.is_kernel_task && proc.address_space != nullptr) {
        release_address_space(proc.address_space);
    }

    // Do not leave children pointing at a slot that can be reused for an
    // unrelated process.
    for (size_t i = 0; i < kMaxTasks; ++i) {
        if (g_task_table[i].parent == &proc) {
            g_task_table[i].parent = nullptr;
        }
        if (g_task_table[i].thread_joiner == &proc) {
            g_task_table[i].thread_joiner = nullptr;
        }
    }

    if (!proc.is_thread && reclaimed_session_id != 0) {
        bool session_alive = false;
        for (size_t i = 0; i < kMaxTasks; ++i) {
            if (&g_task_table[i] != &proc &&
                g_task_table[i].resources != nullptr &&
                g_task_table[i].resources->session_id ==
                    reclaimed_session_id) {
                session_alive = true;
                break;
            }
        }
        if (!session_alive) {
            sync::IrqLockGuard guard(g_control_lock);
            for (auto& session : g_sessions) {
                if (session.in_use &&
                    session.id == reclaimed_session_id) {
                    session = {};
                    break;
                }
            }
        }
    }

    proc.tid = 0;
    proc.cr3 = paging_kernel_cr3();
    reset_task(proc);
    store_state(proc, State::Unused);
}

}  // namespace process
