#include "scheduler.hpp"

#include <stdint.h>

#include "drivers/fs/block_cache.hpp"
#include "drivers/log/logging.hpp"
#include "drivers/console/console.hpp"
#include "lib/mem.hpp"
#include "arch/x86_64/cpu_features.hpp"
#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/lapic.hpp"
#include "arch/x86_64/tss.hpp"
#include "arch/x86_64/registers.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "userspace.hpp"
#include "arch/x86_64/percpu.hpp"
#include "arch/x86_64/smp.hpp"
#include "descriptor.hpp"
#include "debug_heartbeat.hpp"
#include "error.hpp"
#include "sync.hpp"
#include "time.hpp"

namespace {

struct RunQueue {
    process::Task* items[process::kMaxTasks];
    size_t head = 0;
    size_t count = 0;
};

RunQueue g_run_queues[percpu::kMaxCpus];
size_t g_cpu_total = 0;
sync::SpinLock g_queue_lock;
uint32_t g_rr_assign = 0;
constexpr size_t kMaxPollFns = 16;
scheduler::PollFn g_poll_fns[kMaxPollFns]{};
sync::SpinLock g_poll_lock;
process::Task* g_poll_worker = nullptr;
bool g_poll_worker_starting = false;
// A regular poll worker is deliberately deferred until a normal kernel-task
// scheduling opportunity.  The realtime worker is reserved for bounded,
// non-blocking service such as audio mixing and runs once per timer tick.
scheduler::PollFn g_realtime_poll = nullptr;
process::Task* g_realtime_poll_worker = nullptr;
bool g_realtime_poll_worker_starting = false;
constexpr uint64_t kTargetLatencyNs = 6'000'000ull;
constexpr uint64_t kMinGranularityNs = 900'000ull;
uint64_t g_slice_start_ticks[percpu::kMaxCpus]{};
uint64_t g_slice_duration_ticks[percpu::kMaxCpus]{};

[[maybe_unused]] void halt_system() {
    for (;;) {
        asm volatile("hlt");
    }
}

RunQueue& queue_for_cpu(size_t idx) {
    if (idx >= percpu::kMaxCpus) {
        return g_run_queues[0];
    }
    return g_run_queues[idx];
}

bool queue_contains(process::Task* proc) {
    for (size_t q = 0; q < percpu::kMaxCpus; ++q) {
        RunQueue& rq = g_run_queues[q];
        size_t idx = rq.head;
        for (size_t i = 0; i < rq.count; ++i) {
            if (rq.items[idx] == proc) {
                return true;
            }
            idx = (idx + 1) % process::kMaxTasks;
        }
    }
    return false;
}

void queue_push(RunQueue& rq, process::Task* proc) {
    KERNEL_ASSERT_MSG(rq.head < process::kMaxTasks,
                      "scheduler run queue head is out of bounds");
    KERNEL_ASSERT_MSG(rq.count <= process::kMaxTasks,
                      "scheduler run queue count is out of bounds");
    KERNEL_ASSERT_MSG(proc != nullptr,
                      "scheduler attempted to enqueue a null process");
    if (rq.count >= process::kMaxTasks) {
        return;
    }
    size_t tail = (rq.head + rq.count) % process::kMaxTasks;
    rq.items[tail] = proc;
    ++rq.count;
}

process::Task* queue_pop(RunQueue& rq) {
    KERNEL_ASSERT_MSG(rq.head < process::kMaxTasks,
                      "scheduler run queue head is out of bounds");
    KERNEL_ASSERT_MSG(rq.count <= process::kMaxTasks,
                      "scheduler run queue count is out of bounds");
    if (rq.count == 0) {
        return nullptr;
    }
    process::Task* proc = rq.items[rq.head];
    rq.head = (rq.head + 1) % process::kMaxTasks;
    --rq.count;
    return proc;
}

size_t poll_count_locked() {
    size_t count = 0;
    for (size_t i = 0; i < kMaxPollFns; ++i) {
        if (g_poll_fns[i] != nullptr) {
            ++count;
        }
    }
    return count;
}

class QueueGuard {
public:
    QueueGuard() : guard_(g_queue_lock) {}
private:
    sync::IrqLockGuard guard_;
};

class PollGuard {
public:
    PollGuard() : guard_(g_poll_lock) {}
private:
    sync::IrqLockGuard guard_;
};

size_t current_cpu_index();

process::Task* pop_next_runnable(bool include_kernel_tasks,
                                 bool include_realtime_kernel_tasks = false) {
    for (;;) {
        process::Task* candidate = nullptr;
        {
            QueueGuard guard;
            RunQueue& queue = queue_for_cpu(current_cpu_index());
            size_t remaining = queue.count;
            while (remaining-- != 0) {
                process::Task* proc = queue_pop(queue);
                if (proc == nullptr) {
                    break;
                }
                process::State state = process::load_state(*proc);
                if (state == process::State::Reclaiming) {
                    continue;
                }
                if (state != process::State::Ready) {
                    continue;
                }
                if (proc->is_kernel_task &&
                    (!include_kernel_tasks &&
                     (!include_realtime_kernel_tasks ||
                      proc != g_realtime_poll_worker))) {
                    queue_push(queue, proc);
                    continue;
                }
                process::State expected = process::State::Ready;
                if (process::compare_exchange_state(*proc,
                                                    expected,
                                                    process::State::Running)) {
                    candidate = proc;
                    break;
                }
            }
        }

        if (candidate == nullptr) {
            return nullptr;
        }
        return candidate;
    }
}

void poll_worker(process::Task& proc) {
    scheduler::service_polls();
    {
        PollGuard guard;
        // Pollers are fallbacks for devices without a working interrupt path;
        // they do not need to make this worker permanently runnable.  Keeping
        // it Ready caused every userspace yield to service every poller inline,
        // which could delay interactive input behind USB and network work.
        if (poll_count_locked() != 0) {
            uint64_t now = timekeeping::tick_count();
            proc.sleep_until_tick = now == UINT64_MAX ? UINT64_MAX : now + 1;
        } else {
            proc.sleep_until_tick = 0;
        }
        process::store_state(proc, process::State::Blocked);
    }
}

void realtime_poll_worker(process::Task& proc) {
    scheduler::PollFn poll = nullptr;
    {
        PollGuard guard;
        poll = g_realtime_poll;
    }
    if (poll != nullptr) poll();

    const uint64_t now = timekeeping::tick_count();
    proc.sleep_until_tick = now == UINT64_MAX ? UINT64_MAX : now + 1;
    process::store_state(proc, process::State::Blocked);
}

void enqueue_locked(process::Task* proc) {
    size_t total = g_cpu_total;
    size_t online = smp::online_cpus();
    if (online != 0 && online < total) {
        total = online;
    }
    if (total == 0) {
        total = 1;
    }
    if (proc->preferred_cpu == UINT32_MAX || proc->preferred_cpu >= total) {
        uint32_t choice = __atomic_fetch_add(&g_rr_assign, 1, __ATOMIC_RELAXED);
        // Explicitly pinned work (notably init and early kernel workers) keeps
        // its CPU.  Ordinary processes and application threads must use the
        // whole machine: excluding the BSP collapses all userspace onto one
        // core on common dual-core laptops.
        proc->preferred_cpu = static_cast<uint32_t>(choice % total);
    }
    size_t target = static_cast<size_t>(proc->preferred_cpu % total);
    queue_push(queue_for_cpu(target), proc);
}

size_t current_cpu_index() {
    percpu::Cpu* cpu = percpu::current_cpu();
    if (cpu == nullptr || cpu->index >= percpu::kMaxCpus) {
        return 0;
    }
    return cpu->index;
}

size_t runnable_user_task_count_locked(process::Task* current_proc) {
    size_t total = 0;
    RunQueue& rq = queue_for_cpu(current_cpu_index());
    size_t idx = rq.head;
    for (size_t i = 0; i < rq.count; ++i) {
        process::Task* proc = rq.items[idx];
        if (proc != nullptr &&
            !proc->is_kernel_task &&
            process::load_state(*proc) == process::State::Ready) {
            ++total;
        }
        idx = (idx + 1) % process::kMaxTasks;
    }
    if (current_proc != nullptr &&
        !current_proc->is_kernel_task &&
        (process::load_state(*current_proc) == process::State::Running ||
         process::load_state(*current_proc) == process::State::Ready)) {
        ++total;
    }
    return total == 0 ? 1 : total;
}

uint64_t timeslice_ticks_locked(process::Task* current_proc) {
    size_t task_count = runnable_user_task_count_locked(current_proc);
    uint64_t slice_ns = kTargetLatencyNs / static_cast<uint64_t>(task_count);
    if (slice_ns < kMinGranularityNs) {
        slice_ns = kMinGranularityNs;
    }
    return timekeeping::ticks_for_duration_ns(slice_ns);
}

void begin_timeslice_locked(process::Task* proc) {
    if (proc == nullptr || proc->is_kernel_task) {
        return;
    }
    size_t idx = current_cpu_index();
    g_slice_start_ticks[idx] = timekeeping::tick_count();
    g_slice_duration_ticks[idx] = timeslice_ticks_locked(proc);
}

bool timeslice_expired_locked(process::Task* proc) {
    if (proc == nullptr || proc->is_kernel_task) {
        return false;
    }
    if (runnable_user_task_count_locked(proc) <= 1) {
        begin_timeslice_locked(proc);
        return false;
    }
    size_t idx = current_cpu_index();
    uint64_t duration = g_slice_duration_ticks[idx];
    if (duration == 0) {
        begin_timeslice_locked(proc);
        duration = g_slice_duration_ticks[idx];
    }
    uint64_t now = timekeeping::tick_count();
    return now - g_slice_start_ticks[idx] >= duration;
}

void prepare_frame_for_task(process::Task& proc,
                               syscall::SyscallFrame& frame) {
    int64_t wait_result = 0;
    if (process::consume_wait_result(proc, wait_result)) {
        proc.context.rax = static_cast<uint64_t>(wait_result);
    }
    if (proc.has_context) {
        frame = proc.context;
    } else {
        memset(&frame, 0, sizeof(frame));
        frame.user_rip = proc.user_ip;
        frame.user_rsp = proc.user_sp;
        frame.user_rflags = 0x202;
        frame.r11 = 0x202;
    }
    frame.user_rflags = syscall::sanitize_user_rflags(frame.user_rflags);
    if (!proc.has_context) {
        log_message(LogLevel::Debug,
                    "Scheduler: starting tid=%u image=%s code=%016llx+%zu rip=%016llx stack=%016llx..%016llx rsp=%016llx",
                    static_cast<unsigned int>(proc.tid),
                    proc.image_path[0] != '\0' ? proc.image_path : "(unknown)",
                    static_cast<unsigned long long>(proc.code_region.base),
                    static_cast<size_t>(proc.code_region.length),
                    static_cast<unsigned long long>(proc.user_ip),
                    static_cast<unsigned long long>(proc.stack_region.base),
                    static_cast<unsigned long long>(proc.stack_region.top),
                    static_cast<unsigned long long>(proc.user_sp));
    }

    set_rsp0(proc.kernel_stack_top);
    cpu::restore_fpu_state(proc.fpu_state);
}

void capture_from_interrupt(const InterruptFrame& in,
                            syscall::SyscallFrame& out) {
    out.rax = in.rax;
    out.rbx = in.rbx;
    out.rcx = in.rcx;
    out.rdx = in.rdx;
    out.rsi = in.rsi;
    out.rdi = in.rdi;
    out.rbp = in.rbp;
    out.r8  = in.r8;
    out.r9  = in.r9;
    out.r10 = in.r10;
    out.r11 = in.r11;
    out.r12 = in.r12;
    out.r13 = in.r13;
    out.r14 = in.r14;
    out.r15 = in.r15;
    out.user_rip = in.rip;
    out.user_rsp = in.rsp;
    out.user_rflags = in.rflags;
}

void apply_to_interrupt(const syscall::SyscallFrame& in,
                        InterruptFrame& out) {
    out.rax = in.rax;
    out.rbx = in.rbx;
    out.rcx = in.rcx;
    out.rdx = in.rdx;
    out.rsi = in.rsi;
    out.rdi = in.rdi;
    out.rbp = in.rbp;
    out.r8  = in.r8;
    out.r9  = in.r9;
    out.r10 = in.r10;
    out.r11 = in.r11;
    out.r12 = in.r12;
    out.r13 = in.r13;
    out.r14 = in.r14;
    out.r15 = in.r15;
    out.rip = in.user_rip;
    out.rsp = in.user_rsp;
    out.rflags = syscall::sanitize_user_rflags(in.user_rflags);
    out.cs = USER_CS;
    out.ss = USER_DS;
}

}  // namespace

namespace scheduler {

void init() {
    for (size_t i = 0; i < percpu::kMaxCpus; ++i) {
        g_run_queues[i].head = 0;
        g_run_queues[i].count = 0;
        g_slice_start_ticks[i] = 0;
        g_slice_duration_ticks[i] = 0;
    }
}

process::Task* current() {
    return process::current();
}

void register_cpu(percpu::Cpu* cpu) {
    if (cpu == nullptr) {
        return;
    }
    if (cpu->registered) {
        return;
    }
    size_t idx = __atomic_fetch_add(&g_cpu_total, 1, __ATOMIC_SEQ_CST);
    if (idx >= percpu::kMaxCpus) {
        __atomic_fetch_sub(&g_cpu_total, 1, __ATOMIC_SEQ_CST);
        return;
    }
    cpu->index = static_cast<uint32_t>(idx);
    g_run_queues[idx].head = 0;
    g_run_queues[idx].count = 0;
    g_slice_start_ticks[idx] = 0;
    g_slice_duration_ticks[idx] = 0;
    cpu->registered = true;
    log_message(LogLevel::Info,
                "Scheduler: registered CPU (LAPIC=%u total=%u)",
                cpu->lapic_id,
                static_cast<unsigned int>(__atomic_load_n(&g_cpu_total, __ATOMIC_SEQ_CST)));
}

size_t cpu_total() {
    return __atomic_load_n(&g_cpu_total, __ATOMIC_SEQ_CST);
}

bool register_poll(PollFn fn) {
    if (fn == nullptr) {
        return false;
    }

    bool registered = false;
    {
        PollGuard guard;
        for (size_t i = 0; i < kMaxPollFns; ++i) {
            if (g_poll_fns[i] == fn) {
                registered = true;
                break;
            }
        }
        if (!registered) {
            for (size_t i = 0; i < kMaxPollFns; ++i) {
                if (g_poll_fns[i] == nullptr) {
                    g_poll_fns[i] = fn;
                    registered = true;
                    break;
                }
            }
        }
    }
    if (!registered) {
        return false;
    }

    bool create_worker = false;
    process::Task* worker_to_wake = nullptr;
    {
        PollGuard guard;
        if (g_poll_worker == nullptr && !g_poll_worker_starting) {
            g_poll_worker_starting = true;
            create_worker = true;
        } else if (g_poll_worker != nullptr) {
            worker_to_wake = g_poll_worker;
        }
    }

    if (create_worker) {
        process::Task* worker = process::allocate_kernel_task(poll_worker);
        {
            PollGuard guard;
            g_poll_worker_starting = false;
            if (worker != nullptr) {
                worker->preferred_cpu = 0;
                g_poll_worker = worker;
                worker_to_wake = worker;
            }
        }
    }
    if (worker_to_wake != nullptr) {
        if (!process::wake(*worker_to_wake)) {
            enqueue(worker_to_wake);
        }
    }
    return true;
}

bool register_realtime_poll(PollFn fn) {
    if (fn == nullptr) return false;

    bool create_worker = false;
    process::Task* worker_to_wake = nullptr;
    {
        PollGuard guard;
        if (g_realtime_poll != nullptr && g_realtime_poll != fn) return false;
        g_realtime_poll = fn;
        if (g_realtime_poll_worker == nullptr &&
            !g_realtime_poll_worker_starting) {
            g_realtime_poll_worker_starting = true;
            create_worker = true;
        } else if (g_realtime_poll_worker != nullptr) {
            worker_to_wake = g_realtime_poll_worker;
        }
    }

    if (create_worker) {
        process::Task* worker = process::allocate_kernel_task(realtime_poll_worker);
        {
            PollGuard guard;
            g_realtime_poll_worker_starting = false;
            if (worker != nullptr) {
                worker->preferred_cpu = 0;
                g_realtime_poll_worker = worker;
                worker_to_wake = worker;
            } else {
                g_realtime_poll = nullptr;
            }
        }
    }
    if (worker_to_wake != nullptr) {
        if (!process::wake(*worker_to_wake)) enqueue(worker_to_wake);
    }
    return worker_to_wake != nullptr;
}

void service_polls() {
    percpu::Cpu* cpu = percpu::current_cpu();
    if (cpu != nullptr && cpu->index != 0) {
        return;
    }

    PollFn pollers[kMaxPollFns]{};
    size_t count = 0;

    {
        PollGuard guard;
        for (size_t i = 0; i < kMaxPollFns; ++i) {
            if (g_poll_fns[i] != nullptr) {
                pollers[count++] = g_poll_fns[i];
            }
        }
    }

    for (size_t i = 0; i < count; ++i) {
        pollers[i]();
    }
}

void enqueue(process::Task* proc) {
    if (proc == nullptr) {
        return;
    }
    size_t target = percpu::kMaxCpus;
    {
        QueueGuard guard;
        process::State state = process::load_state(*proc);
        if (state != process::State::Ready) {
            return;
        }
        if (!queue_contains(proc)) {
            enqueue_locked(proc);
            target = proc->preferred_cpu;
        }
    }

    percpu::Cpu* current_cpu = percpu::current_cpu();
    percpu::Cpu* target_cpu = percpu::cpu_from_index(target);
    if (target_cpu != nullptr && target_cpu->registered &&
        target_cpu != current_cpu) {
        lapic::send_ipi(target_cpu->lapic_id,
                        lapic::kSchedulerWakeVector);
    }
}

void remove(process::Task* proc) {
    if (proc == nullptr) {
        return;
    }
    QueueGuard guard;
    for (size_t q = 0; q < percpu::kMaxCpus; ++q) {
        RunQueue& queue = g_run_queues[q];
        size_t remaining = queue.count;
        while (remaining-- != 0) {
            process::Task* candidate = queue_pop(queue);
            if (candidate != nullptr && candidate != proc) {
                queue_push(queue, candidate);
            }
        }
    }
}

[[noreturn]] static void run_loop() {
    for (;;) {
        process::reap_deferred();
        process::Task* next = pop_next_runnable(true);
        if (next == nullptr) {
            process::set_current(nullptr);
            fs::block_cache::service_idle_flush();
            asm volatile("pause");
            asm volatile("sti; hlt");
            continue;
        }
        if (next->is_kernel_task) {
            process::set_current(next);
            set_rsp0(next->kernel_stack_top);
            asm volatile("sti");
            if (next->kernel_entry != nullptr) {
                next->kernel_entry(*next);
            }
            if (process::load_state(*next) == process::State::Ready) {
                QueueGuard guard;
                if (!queue_contains(next)) {
                    enqueue_locked(next);
                }
            }
            continue;
        }
        if (process::load_state(*next) == process::State::Terminated) {
            continue;
        }
        process::set_current(next);
        {
            QueueGuard guard;
            begin_timeslice_locked(next);
        }
        set_rsp0(next->kernel_stack_top);
        userspace::enter_task(*next);
        log_message(LogLevel::Error,
                    "Scheduler: task %u returned unexpectedly",
                    static_cast<unsigned int>(next->tid));
    }
}

void reschedule_impl(syscall::SyscallFrame& frame, bool run_kernel_tasks,
                     bool run_realtime_kernel_tasks = false) {
    process::reap_deferred();
    process::Task* current_proc = process::current();
    if (current_proc == nullptr) {
        return;
    }

    process::State current_state = process::load_state(*current_proc);
    while (current_state == process::State::Waking) {
        asm volatile("pause");
        current_state = process::load_state(*current_proc);
    }
    bool terminated = current_state == process::State::Terminated;
    bool suspending = current_state == process::State::Suspending;

    if (!terminated) {
        cpu::save_fpu_state(current_proc->fpu_state);
        current_proc->context = frame;
        int64_t wait_result = 0;
        if (process::consume_wait_result(*current_proc, wait_result)) {
            current_proc->context.rax = static_cast<uint64_t>(wait_result);
        }
        current_proc->has_context = true;
        current_proc->user_ip = frame.user_rip;
        current_proc->user_sp = frame.user_rsp;
        current_proc->fs_base = cpu::read_fs_base();
        if (suspending) {
            process::State expected = process::State::Suspending;
            (void)process::compare_exchange_state(
                *current_proc,
                expected,
                process::State::Suspended);
        }
    } else {
        current_proc->has_context = false;
    }

    process::Task* next = nullptr;
    {
        QueueGuard guard;
        if (!terminated) {
            if (process::load_state(*current_proc) == process::State::Running) {
                process::store_state(*current_proc, process::State::Ready);
            }
            if (process::load_state(*current_proc) == process::State::Ready &&
                !queue_contains(current_proc)) {
                enqueue_locked(current_proc);
            }
        }

    }
    next = pop_next_runnable(run_kernel_tasks, run_realtime_kernel_tasks);

    for (;;) {
        // Run any ready kernel tasks immediately (they have no userspace frame)
        while (next != nullptr && next->is_kernel_task) {
            process::set_current(next);
            set_rsp0(next->kernel_stack_top);
            if (next->kernel_entry != nullptr) {
                next->kernel_entry(*next);
            }
            if (process::load_state(*next) == process::State::Ready) {
                QueueGuard guard;
                if (!queue_contains(next)) {
                    enqueue_locked(next);
                }
            }
            // fetch another runnable task (prefer userspace)
            next = pop_next_runnable(run_kernel_tasks, run_realtime_kernel_tasks);
        }

        if (next != nullptr) {
            break;
        }

        if (terminated) {
            process::set_current(nullptr);
            paging_switch_cr3(paging_kernel_cr3());
            cpu::write_fs_base(0);
            process::defer_reclaim(*current_proc);
            current_proc = nullptr;
            run_loop();
        }

        if (current_proc != nullptr) {
            process::State state = process::load_state(*current_proc);
            if (state == process::State::Ready) {
                process::State expected = process::State::Ready;
                if (!process::compare_exchange_state(
                        *current_proc,
                        expected,
                        process::State::Running)) {
                    continue;
                }
                state = process::State::Running;
            }
            if (state == process::State::Running) {
                process::set_current(current_proc);
                prepare_frame_for_task(*current_proc, frame);
                asm volatile("pause");
                return;
            }
        }

        process::set_current(nullptr);
        paging_switch_cr3(paging_kernel_cr3());
        cpu::write_fs_base(0);

        do {
            fs::block_cache::service_idle_flush();
            asm volatile("sti; hlt; cli" ::: "memory");
            next = pop_next_runnable(run_kernel_tasks, run_realtime_kernel_tasks);
        } while (next == nullptr);
    }

    process::set_current(next);
    {
        QueueGuard guard;
        begin_timeslice_locked(next);
    }
    if (terminated && current_proc != nullptr) {
        process::defer_reclaim(*current_proc);
    }
    prepare_frame_for_task(*next, frame);
}

void reschedule(syscall::SyscallFrame& frame) {
    reschedule_impl(frame, true, true);
}

void reschedule_from_interrupt(InterruptFrame& frame) {
    if ((frame.cs & 0x3) == 0) {
        return;
    }

    syscall::SyscallFrame state{};
    capture_from_interrupt(frame, state);
    reschedule_impl(state, false, true);
    apply_to_interrupt(state, frame);
}

void tick(InterruptFrame& frame) {
    percpu::Cpu* cpu = percpu::current_cpu();
    if (cpu != nullptr && cpu->index == 0) {
        const uint64_t tick = timekeeping::tick_count();
        if (kconsole != nullptr) {
            kconsole->tick_cursor(tick);
        }
        // The console may flush its back buffer while updating the cursor.
        // Draw the diagnostic marker afterwards so that refresh cannot erase
        // it in the same tick.
        debug_heartbeat::tick(tick);
        process::wake_ready_sleepers(tick);
    }
    if ((frame.cs & 0x3) != 0) {
        process::Task* current_proc = process::current();
        bool expired = false;
        {
            QueueGuard guard;
            expired = timeslice_expired_locked(current_proc);
        }
        if (expired) {
            scheduler::reschedule_from_interrupt(frame);
        }
        return;
    }
    // If we interrupted kernel mode, avoid clobbering the kernel frame.
    // Potentially blocking I/O is serviced only by its kernel worker.
}

[[noreturn]] void run_cpu() {
    asm volatile("sti");
    run_loop();
}

[[noreturn]] void run() {
    run_cpu();
}

}  // namespace scheduler
