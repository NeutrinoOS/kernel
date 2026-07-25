#pragma once

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/cpu_features.hpp"
#include "arch/x86_64/syscall.hpp"
#include "descriptor.hpp"
#include "fs/vfs.hpp"
#include "path_util.hpp"
#include "capabilities.hpp"
#include "sync.hpp"
#include "vm.hpp"

namespace process {

constexpr size_t kMaxProcesses = 256;
constexpr size_t kKernelStackSize = 0x4000;
constexpr size_t kMaxFileHandles = 16;
constexpr size_t kMaxDirectoryHandles = 8;
constexpr size_t kMaxProcessEvents = 32;
constexpr size_t kDefaultThreadStackSize = 64 * 1024;
constexpr size_t kMinThreadStackSize = 16 * 1024;
constexpr size_t kMaxThreadStackSize = 8 * 1024 * 1024;

struct AddressSpace;
struct ProcessResources;

struct ProcessLimits {
    uint32_t max_threads;
    uint32_t max_descriptors;
    uint32_t max_file_handles;
    uint32_t max_directory_handles;
    uint64_t max_virtual_bytes;
    uint64_t max_cpu_ticks;
};

struct ProcessEvent {
    uint32_t type;
    uint32_t sender_process_id;
    uint64_t value;
};

struct ProcessUsage {
    uint32_t threads;
    uint32_t descriptors;
    uint32_t file_handles;
    uint32_t directory_handles;
    uint64_t virtual_bytes;
    uint64_t resident_bytes;
    uint64_t cpu_ticks;
};

enum class State {
    Unused = 0,
    Ready,
    Running,
    Blocked,
    Terminated,
    Allocating,
    Reclaiming,
    Waking,
    Suspending,
    Suspended,
};

enum class WaitReason : uint8_t {
    None,
    ThreadJoin,
    Futex,
    Child,
    Event,
};

struct FileHandle {
    bool in_use;
    bool reserved;
    bool can_write;
    vfs::FileHandle handle;
    uint64_t position;
    char path[path_util::kMaxPathLength];
};

struct DirectoryHandle {
    bool in_use;
    bool reserved;
    vfs::DirectoryHandle handle;
    char path[path_util::kMaxPathLength];
};

struct ProcessResources {
    uint32_t refcount;
    bool in_use;
    sync::SpinLock lock;
    sync::SpinLock descriptor_lock;
    sync::SpinLock event_lock;
    sync::SpinLock file_locks[kMaxFileHandles];
    sync::SpinLock directory_locks[kMaxDirectoryHandles];
    uint32_t vty_id;
    uint32_t process_group_id;
    uint32_t session_id;
    uint32_t parent_process_id;
    uint32_t tracer_process_id;
    uint64_t cpu_ticks;
    ProcessLimits limits;
    ProcessEvent events[kMaxProcessEvents];
    uint32_t event_head;
    uint32_t event_count;
    char cwd[128];
    uint32_t standard_descriptors[3];
    descriptor::Table descriptors;
    capabilities::Principal* principal;
    capabilities::CapHandleEntry
        cap_handles[capabilities::kMaxProcessCapabilities];
    FileHandle file_handles[kMaxFileHandles];
    DirectoryHandle directory_handles[kMaxDirectoryHandles];
};

struct Process {
    uint32_t pid;
    uint32_t process_id;
    State state;
    uint64_t cr3;
    AddressSpace* address_space;
    ProcessResources* resources;
    uint64_t fs_base;
    uint64_t user_ip;
    uint64_t user_sp;
    alignas(cpu::kFpuStateAlign) uint8_t fpu_state[cpu::kFpuStateSize];
    uint64_t kernel_stack_base;
    uint64_t kernel_stack_top;
    vm::Region code_region;
    vm::Stack stack_region;
    syscall::SyscallFrame context;
    Process* parent;
    Process* thread_joiner;
    void* waiting_on;
    WaitReason wait_reason;
    State suspended_from;
    uint32_t wait_child_pid;
    uint16_t exit_code;
    bool has_exited;
    bool console_transferred;
    bool has_context;
    bool is_kernel_task;
    bool is_thread;
    bool thread_join_claimed;
    bool child_wait_claimed;
    bool reclaim_pending;
    uint32_t reclaim_cpu;
    void (*kernel_entry)(Process&);
    uint32_t preferred_cpu;  // UINT32_MAX means unassigned
    uint64_t sleep_until_tick;
    uint64_t user_ticks;
    uint64_t kernel_ticks;
    uint64_t wait_descriptors_user;
    uint32_t wait_descriptor_count;
    uint32_t wait_descriptor_reserved;
    int64_t wait_result;
    bool wait_result_pending;
    char image_path[path_util::kMaxPathLength];
    descriptor_defs::DescriptorWait
        wait_descriptors[descriptor::kMaxWaitDescriptors];
};

inline ProcessResources& shared_resources(Process& proc) {
    return *proc.resources;
}

inline const ProcessResources& shared_resources(const Process& proc) {
    return *proc.resources;
}

inline State load_state(const Process& proc) {
    return __atomic_load_n(&proc.state, __ATOMIC_ACQUIRE);
}

inline void store_state(Process& proc, State state) {
    __atomic_store_n(&proc.state, state, __ATOMIC_RELEASE);
}

inline bool compare_exchange_state(Process& proc,
                                   State& expected,
                                   State desired) {
    return __atomic_compare_exchange_n(&proc.state,
                                       &expected,
                                       desired,
                                       false,
                                       __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}

void init();
Process* allocate();
Process* allocate_init_task();
Process* allocate_kernel_task(void (*entry)(Process&));
bool attach_new_resources(Process& proc);
Process* create_user_thread(Process& owner,
                            uint64_t entry,
                            uint64_t argument,
                            size_t stack_size,
                            uint64_t tls_base = 0);
bool set_thread_tls(Process& thread, uint64_t fs_base);
Process* current();
void set_current(Process* proc);
Process* table_entry(size_t index);
Process* find_by_pid(uint32_t pid);
void record_tick(bool user_mode);
size_t usage_snapshot(descriptor_defs::TaskUsage* out, size_t max_entries);
void wake_ready_sleepers(uint64_t current_tick);
bool wake(Process& proc);
bool begin_wake(Process& proc);
void finish_wake(Process& proc);
void finish_wake_with_result(Process& proc, int64_t result);
bool wake_with_result(Process& proc, int64_t result);
void terminate(Process& proc, uint16_t exit_code);
void terminate_group(Process& proc, uint16_t exit_code);
bool join_thread(Process& caller,
                 uint32_t thread_id,
                 int64_t& immediate_result,
                 bool& blocked);
int64_t wait_child(Process& caller,
                   uint32_t child_pid,
                   bool nonblocking);
bool send_event(Process& sender,
                uint32_t target_pid,
                const ProcessEvent& event);
int64_t receive_event(Process& caller,
                      ProcessEvent& event,
                      bool nonblocking);
bool control_group(Process& caller,
                   uint32_t target_pid,
                   uint32_t action,
                   uint16_t exit_code);
bool set_process_group(Process& caller,
                       uint32_t target_pid,
                       uint32_t group_id);
uint32_t process_group_id(const Process& proc);
uint32_t session_id(const Process& proc);
bool create_session(Process& caller);
bool set_foreground_group(Process& caller, uint32_t group_id);
uint32_t foreground_group(const Process& caller);
bool is_foreground(const Process& proc);
bool get_limits(const Process& proc, ProcessLimits& limits);
bool set_limits(Process& proc, const ProcessLimits& limits);
bool get_usage(const Process& proc, ProcessUsage& usage);
bool trace_attach(Process& tracer, uint32_t target_pid);
bool trace_detach(Process& tracer, uint32_t target_pid);
bool trace_read_memory(Process& tracer,
                       uint32_t target_pid,
                       uint64_t target_address,
                       uint64_t user_buffer,
                       size_t length);
bool trace_write_memory(Process& tracer,
                        uint32_t target_pid,
                        uint64_t target_address,
                        uint64_t user_buffer,
                        size_t length);
bool trace_get_registers(Process& tracer,
                         uint32_t target_tid,
                         uint64_t user_buffer,
                         size_t length);
bool trace_stopped(Process& tracer, uint32_t target_pid);
int64_t futex_wait(Process& caller,
                   uint64_t user_address,
                   uint32_t expected);
size_t futex_wake(Process& caller,
                  uint64_t user_address,
                  size_t max_count);
bool consume_wait_result(Process& proc, int64_t& out_result);
void defer_reclaim(Process& proc);
void reap_deferred();
void reclaim(Process& proc);

}  // namespace process
