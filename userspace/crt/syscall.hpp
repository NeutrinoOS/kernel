#pragma once

#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"
#include "neutrino_time.h"

enum class SystemCall : long {
    AbiMajor             = 0,
    AbiMinor             = 1,
    Exit                 = 2,
    Yield                = 3,
    Sleep                = 4,
    DescriptorOpen       = 5,
    DescriptorRead       = 6,
    DescriptorWrite      = 7,
    DescriptorClose      = 8,
    DescriptorGetType    = 9,
    DescriptorTestFlag   = 10,
    DescriptorGetFlags   = 11,
    DescriptorGetProperty= 12,
    DescriptorSetProperty= 13,
    DescriptorWait       = 14,
    FileOpen             = 15,
    FileClose            = 16,
    FileRead             = 17,
    FileWrite            = 18,
    FileCreate           = 19,
    FileOpenAt           = 20,
    FileCreateAt         = 21,
    FileRemove           = 22,
    DirectoryOpen        = 23,
    DirectoryRead        = 24,
    DirectoryClose       = 25,
    DirectoryOpenRoot    = 26,
    DirectoryOpenAt      = 27,
    DirectoryCreate      = 28,
    DirectoryRemove      = 29,
    ProcessExec          = 30,
    Child                = 31,
    ProcessSetCwd        = 32,
    ProcessGetCwd        = 33,
    MapAnonymous         = 34,
    MapAt                = 35,
    Unmap                = 36,
    ChangeSlot           = 37,
    TimeGet              = 38,
    Mount                = 39,
    RescanBlockDevices   = 40,
    PrincipalCreate      = 41,
    PrincipalSet         = 42,
    CapabilityGrant      = 43,
    CapabilityPass       = 44,
    UserCreate           = 45,
    UserFind             = 46,
    UserBumpGeneration   = 47,
    UserSetPassword      = 48,
    UserInfo             = 49,
    FileSync             = 50,
    Sync                 = 51,
    Shutdown             = 52,
    ModuleLoad           = 53,
    ModuleCount          = 54,
    ModuleInfo           = 55,
    RandomGet            = 56,
    FileGetAcl           = 57,
    FileSetAcl           = 58,
    MapFilePrivate       = 59,
    ThreadCreate         = 60,
    ThreadExit           = 61,
    ThreadJoin           = 62,
    FutexWait            = 63,
    FutexWake            = 64,
    ThreadId             = 65,
    ProcessId            = 66,
    ThreadSetTls         = 67,
    ThreadGetTls         = 68,
    ProcessEventSend     = 69,
    ProcessEventReceive  = 70,
    ProcessControl       = 71,
    ProcessWaitChild     = 72,
    ProcessSetGroup      = 73,
    ProcessGetGroup      = 74,
    ProcessCreateSession = 75,
    ProcessGetSession    = 76,
    ProcessSetForeground = 77,
    ProcessGetForeground = 78,
    ProcessSetLimits     = 79,
    ProcessGetLimits     = 80,
    ProcessGetUsage      = 81,
    ProcessTrace         = 82,
    FileOpenFlags        = 83,
    FileSeek             = 84,
    FileStat             = 85,
    PathStat             = 86,
    FileReadAt           = 87,
    ClockGet             = 88,
    ThreadDetach         = 89,
    FutexWaitTimed       = 90,
    ProtectMemory        = 91,
};

enum : uint32_t {
    FILE_OPEN_READ = 1u << 0,
    FILE_OPEN_WRITE = 1u << 1,
    FILE_OPEN_CREATE = 1u << 2,
    FILE_OPEN_EXCLUSIVE = 1u << 3,
    FILE_OPEN_APPEND = 1u << 4,
};

enum : uint32_t {
    FILE_METADATA_DIRECTORY = 1u << 0,
};

struct FileMetadata {
    uint64_t size;
    uint64_t modified_nanoseconds;
    uint32_t flags;
    uint32_t reserved;
};

static_assert(sizeof(FileMetadata) == 24, "file metadata size mismatch");

enum : uint32_t {
    DIR_ENTRY_FLAG_DIRECTORY = 1u << 0,
};

enum : uint64_t {
    MAP_WRITE = 1ull << 0,
};

enum class AclValue : uint8_t {
    Deny = 0,
    Allow = 1,
    Default = 2,
};

struct FileAclEntry {
    uint64_t machine_id;
    uint64_t local_id;
    AclValue write;
    AclValue read;
    AclValue delete_permission;
    AclValue edit;
    uint8_t reserved[4];
};

static_assert(sizeof(FileAclEntry) == 24, "file ACL entry size mismatch");

constexpr uint32_t kInvalidDescriptor = 0xFFFFFFFFu;
constexpr long kDescriptorWouldBlock = -2;
constexpr uint64_t kProcessChildFlagStdioConfig = 1ull << 0;
constexpr uint64_t kProcessSpawnFlagPrincipalConfig = 1ull << 1;
constexpr uint32_t kStandardInputDescriptor = 0x00010000u;
constexpr uint32_t kStandardOutputDescriptor = 0x00010001u;
constexpr uint32_t kStandardErrorDescriptor = 0x00010002u;

struct ProcessStdioConfig {
    uint32_t stdin_handle;
    uint32_t stdout_handle;
    uint32_t stderr_handle;
    uint32_t reserved;
};

struct ProcessSpawnConfig {
    uint64_t cwd_ptr;
    ProcessStdioConfig stdio;
    uint64_t principal_handle;
};

static_assert(sizeof(ProcessSpawnConfig) == 32,
              "ProcessSpawnConfig size mismatch");

enum : uint32_t {
    PROCESS_CONTROL_KILL = 1,
    PROCESS_CONTROL_SUSPEND = 2,
    PROCESS_CONTROL_RESUME = 3,
    PROCESS_EVENT_NONBLOCK = 1u << 0,
    PROCESS_WAIT_NONBLOCK = 1u << 0,
    PROCESS_TRACE_ATTACH = 1,
    PROCESS_TRACE_DETACH = 2,
    PROCESS_TRACE_READ_MEMORY = 3,
    PROCESS_TRACE_WRITE_MEMORY = 4,
    PROCESS_TRACE_GET_REGISTERS = 5,
    PROCESS_TRACE_STOPPED = 6,
};

struct ProcessEvent {
    uint32_t type;
    uint32_t sender_process_id;
    uint64_t value;
};

struct ProcessLimits {
    uint32_t max_threads;
    uint32_t max_descriptors;
    uint32_t max_file_handles;
    uint32_t max_directory_handles;
    uint64_t max_virtual_bytes;
    uint64_t max_cpu_ticks;
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

struct ProcessTraceRegisters {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t user_rsp;
    uint64_t user_rip;
    uint64_t user_rflags;
};

static_assert(sizeof(ProcessEvent) == 16, "process event size mismatch");
static_assert(sizeof(ProcessLimits) == 32, "process limits size mismatch");
static_assert(sizeof(ProcessUsage) == 40, "process usage size mismatch");
static_assert(sizeof(ProcessTraceRegisters) == 144,
              "trace register size mismatch");

struct UserInfo {
    uint64_t id_machine;
    uint64_t id_local;
    char name[32];
    uint64_t allowed_caps;
    uint64_t generation;
    uint32_t password_set;
    uint32_t active;
};

enum ModuleInfoFlag : uint32_t {
    kModuleInfoBuiltin = 1u << 0,
    kModuleInfoDynamic = 1u << 1,
};

struct ModuleInfo {
    char name[64];
    char path[128];
    uint64_t image_size;
    uint32_t flags;
    uint32_t reserved;
};

struct DirEntry {
    char name[64];
    uint32_t flags;
    uint32_t reserved;
    uint64_t size;
};

static inline long raw_syscall6(SystemCall num,
                                long a1,
                                long a2,
                                long a3,
                                long a4,
                                long a5,
                                long a6) {
    long ret;
    long cn = static_cast<long>(num);
    asm volatile("mov %5, %%r10\n\t"
                 "mov %6, %%r8\n\t"
                 "mov %7, %%r9\n\t"
                 "syscall"
                 : "=a"(ret)
                 : "a"(cn), "D"(a1), "S"(a2), "d"(a3),
                   "r"(a4), "r"(a5), "r"(a6)
                 : "rcx", "r11", "r10", "r8", "r9", "memory");
    return ret;
}

static inline long raw_syscall5(SystemCall num,
                                long a1,
                                long a2,
                                long a3,
                                long a4,
                                long a5) {
    return raw_syscall6(num, a1, a2, a3, a4, a5, 0);
}

static inline long raw_syscall4(SystemCall num,
                                long a1,
                                long a2,
                                long a3,
                                long a4) {
    return raw_syscall6(num, a1, a2, a3, a4, 0, 0);
}

static inline long raw_syscall3(SystemCall num,
                                long a1,
                                long a2,
                                long a3) {
    return raw_syscall6(num, a1, a2, a3, 0, 0, 0);
}

static inline long raw_syscall2(SystemCall num,
                                long a1,
                                long a2) {
    return raw_syscall6(num, a1, a2, 0, 0, 0, 0);
}

static inline long raw_syscall1(SystemCall num,
                                long a1) {
    return raw_syscall6(num, a1, 0, 0, 0, 0, 0);
}

static inline long raw_syscall0(SystemCall num) {
    return raw_syscall6(num, 0, 0, 0, 0, 0, 0);
}

static inline long abi_major() {
    return raw_syscall0(SystemCall::AbiMajor);
}

static inline long abi_minor() {
    return raw_syscall0(SystemCall::AbiMinor);
}

[[noreturn]] static inline void exit(uint16_t code) {
    raw_syscall1(SystemCall::Exit, static_cast<long>(code));
    __builtin_unreachable();
}

static inline long yield() {
    return raw_syscall0(SystemCall::Yield);
}

static inline long sleep_ns(uint64_t duration_ns) {
    return raw_syscall1(SystemCall::Sleep,
                        static_cast<long>(duration_ns));
}

static inline long sleep_ms(uint64_t duration_ms) {
    return sleep_ns(duration_ms * 1000000ull);
}

static inline long sleep_seconds(uint64_t duration_seconds) {
    return sleep_ns(duration_seconds * 1000000000ull);
}

static inline long time_get(NeutrinoWallTime* out_time) {
    if (out_time == nullptr) {
        return -1;
    }
    return raw_syscall2(SystemCall::TimeGet,
                        static_cast<long>(reinterpret_cast<uintptr_t>(out_time)),
                        static_cast<long>(sizeof(*out_time)));
}

static inline long random_get(void* output, size_t length) {
    if (output == nullptr && length != 0) {
        return -1;
    }
    return raw_syscall2(SystemCall::RandomGet,
                        static_cast<long>(reinterpret_cast<uintptr_t>(output)),
                        static_cast<long>(length));
}

static inline long descriptor_wait(descriptor_defs::DescriptorWait* items,
                                   size_t count) {
    if (items == nullptr || count == 0) {
        return -1;
    }
    return raw_syscall2(SystemCall::DescriptorWait,
                        static_cast<long>(
                            reinterpret_cast<uintptr_t>(items)),
                        static_cast<long>(count));
}

static inline void* map_anonymous(size_t length, uint64_t flags) {
    long ret = raw_syscall2(SystemCall::MapAnonymous,
                            static_cast<long>(length),
                            static_cast<long>(flags));
    if (ret < 0) {
        return nullptr;
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(ret));
}

static inline void* map_at(void* addr_hint, size_t length, uint64_t flags) {
    long ret = raw_syscall3(SystemCall::MapAt,
                            static_cast<long>(reinterpret_cast<uintptr_t>(addr_hint)),
                            static_cast<long>(length),
                            static_cast<long>(flags));
    if (ret < 0) {
        return nullptr;
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(ret));
}

static inline long unmap(void* addr, size_t length) {
    return raw_syscall2(SystemCall::Unmap,
                        static_cast<long>(reinterpret_cast<uintptr_t>(addr)),
                        static_cast<long>(length));
}

static inline void* map_file_private(uint32_t handle,
                                     uint64_t file_offset,
                                     size_t length,
                                     uint64_t flags = 0) {
    long ret = raw_syscall4(SystemCall::MapFilePrivate,
                            static_cast<long>(handle),
                            static_cast<long>(file_offset),
                            static_cast<long>(length),
                            static_cast<long>(flags));
    if (ret < 0) {
        return nullptr;
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(ret));
}

using ThreadEntry = void (*)(void*);

static inline long thread_create(ThreadEntry entry,
                                 void* argument,
                                 size_t stack_size = 0) {
    return raw_syscall3(
        SystemCall::ThreadCreate,
        static_cast<long>(reinterpret_cast<uintptr_t>(entry)),
        static_cast<long>(reinterpret_cast<uintptr_t>(argument)),
        static_cast<long>(stack_size));
}

static inline long thread_create_tls(ThreadEntry entry,
                                     void* argument,
                                     void* tls_base,
                                     size_t stack_size = 0) {
    return raw_syscall4(
        SystemCall::ThreadCreate,
        static_cast<long>(reinterpret_cast<uintptr_t>(entry)),
        static_cast<long>(reinterpret_cast<uintptr_t>(argument)),
        static_cast<long>(stack_size),
        static_cast<long>(reinterpret_cast<uintptr_t>(tls_base)));
}

[[noreturn]] static inline void thread_exit(uint16_t code = 0) {
    raw_syscall1(SystemCall::ThreadExit, static_cast<long>(code));
    __builtin_unreachable();
}

static inline long thread_join(uint32_t thread_id) {
    return raw_syscall1(SystemCall::ThreadJoin,
                        static_cast<long>(thread_id));
}

static inline long futex_wait(uint32_t* address, uint32_t expected) {
    return raw_syscall2(
        SystemCall::FutexWait,
        static_cast<long>(reinterpret_cast<uintptr_t>(address)),
        static_cast<long>(expected));
}

static inline long futex_wake(uint32_t* address, size_t max_count = 1) {
    return raw_syscall2(
        SystemCall::FutexWake,
        static_cast<long>(reinterpret_cast<uintptr_t>(address)),
        static_cast<long>(max_count));
}

static inline uint32_t thread_id() {
    return static_cast<uint32_t>(raw_syscall0(SystemCall::ThreadId));
}

static inline uint32_t process_id() {
    return static_cast<uint32_t>(raw_syscall0(SystemCall::ProcessId));
}

static inline long thread_set_tls(void* base) {
    return raw_syscall1(
        SystemCall::ThreadSetTls,
        static_cast<long>(reinterpret_cast<uintptr_t>(base)));
}

static inline void* thread_get_tls() {
    long value = raw_syscall0(SystemCall::ThreadGetTls);
    return reinterpret_cast<void*>(static_cast<uintptr_t>(value));
}

static inline long process_event_send(uint32_t process,
                                      uint32_t type,
                                      uint64_t value = 0) {
    return raw_syscall3(SystemCall::ProcessEventSend,
                        static_cast<long>(process),
                        static_cast<long>(type),
                        static_cast<long>(value));
}

static inline long process_event_receive(ProcessEvent* event,
                                         bool nonblocking = false) {
    if (event == nullptr) {
        return -1;
    }
    long result = raw_syscall2(
        SystemCall::ProcessEventReceive,
        reinterpret_cast<long>(event),
        nonblocking ? static_cast<long>(PROCESS_EVENT_NONBLOCK) : 0);
    if (result == 1 && !nonblocking) {
        result = raw_syscall2(
            SystemCall::ProcessEventReceive,
            reinterpret_cast<long>(event),
            static_cast<long>(PROCESS_EVENT_NONBLOCK));
    }
    return result;
}

static inline long process_control(uint32_t process,
                                   uint32_t action,
                                   uint16_t exit_code = 0) {
    return raw_syscall3(SystemCall::ProcessControl,
                        static_cast<long>(process),
                        static_cast<long>(action),
                        static_cast<long>(exit_code));
}

// Returns (pid << 16) | exit_code, -2 in nonblocking mode when children live,
// or -1 when no matching child exists.
static inline long process_wait_child(uint32_t process = 0,
                                      bool nonblocking = false) {
    return raw_syscall2(
        SystemCall::ProcessWaitChild,
        static_cast<long>(process),
        nonblocking ? static_cast<long>(PROCESS_WAIT_NONBLOCK) : 0);
}

static inline long process_set_group(uint32_t process, uint32_t group) {
    return raw_syscall2(SystemCall::ProcessSetGroup,
                        static_cast<long>(process),
                        static_cast<long>(group));
}

static inline long process_get_group(uint32_t process = 0) {
    return raw_syscall1(SystemCall::ProcessGetGroup,
                        static_cast<long>(process));
}

static inline long process_create_session() {
    return raw_syscall0(SystemCall::ProcessCreateSession);
}

static inline long process_get_session(uint32_t process = 0) {
    return raw_syscall1(SystemCall::ProcessGetSession,
                        static_cast<long>(process));
}

static inline long process_set_foreground(uint32_t group) {
    return raw_syscall1(SystemCall::ProcessSetForeground,
                        static_cast<long>(group));
}

static inline long process_get_foreground() {
    return raw_syscall0(SystemCall::ProcessGetForeground);
}

static inline long process_set_limits(uint32_t process,
                                      const ProcessLimits* limits) {
    return limits == nullptr
               ? -1
               : raw_syscall2(
                     SystemCall::ProcessSetLimits,
                     static_cast<long>(process),
                     reinterpret_cast<long>(limits));
}

static inline long process_get_limits(uint32_t process,
                                      ProcessLimits* limits) {
    return limits == nullptr
               ? -1
               : raw_syscall2(
                     SystemCall::ProcessGetLimits,
                     static_cast<long>(process),
                     reinterpret_cast<long>(limits));
}

static inline long process_get_usage(uint32_t process,
                                     ProcessUsage* usage) {
    return usage == nullptr
               ? -1
               : raw_syscall2(
                     SystemCall::ProcessGetUsage,
                     static_cast<long>(process),
                     reinterpret_cast<long>(usage));
}

static inline long process_trace(uint32_t process,
                                 uint32_t operation,
                                 uint64_t address = 0,
                                 void* buffer = nullptr,
                                 size_t length = 0) {
    return raw_syscall5(
        SystemCall::ProcessTrace,
        static_cast<long>(process),
        static_cast<long>(operation),
        static_cast<long>(address),
        reinterpret_cast<long>(buffer),
        static_cast<long>(length));
}

static inline long change_slot(uint32_t slot) {
    return raw_syscall1(SystemCall::ChangeSlot,
                        static_cast<long>(slot));
}

static inline long child(const char* path,
                         const char* args,
                         uint64_t flags,
                         const char* cwd) {
    return raw_syscall4(SystemCall::Child,
                        static_cast<long>(reinterpret_cast<uintptr_t>(path)),
                        static_cast<long>(reinterpret_cast<uintptr_t>(args)),
                        static_cast<long>(flags),
                        static_cast<long>(reinterpret_cast<uintptr_t>(cwd)));
}

static inline long child_with_stdio(const char* path,
                                    const char* args,
                                    uint64_t flags,
                                    const char* cwd,
                                    const ProcessStdioConfig* stdio) {
    if (stdio == nullptr) {
        return -1;
    }
    ProcessSpawnConfig config{};
    config.cwd_ptr = reinterpret_cast<uint64_t>(cwd);
    config.stdio = *stdio;
    return raw_syscall4(SystemCall::Child,
                        static_cast<long>(reinterpret_cast<uintptr_t>(path)),
                        static_cast<long>(reinterpret_cast<uintptr_t>(args)),
                        static_cast<long>(flags | kProcessChildFlagStdioConfig),
                        static_cast<long>(reinterpret_cast<uintptr_t>(&config)));
}

// Spawn-time principal assignment is atomic with child creation and requires
// SecurityManage in addition to the normal ProcessSpawn permission.
static inline long child_as(const char* path,
                            const char* args,
                            uint64_t flags,
                            const char* cwd,
                            void* principal) {
    if (principal == nullptr) {
        return -1;
    }
    ProcessSpawnConfig config{};
    config.cwd_ptr = reinterpret_cast<uint64_t>(cwd);
    config.principal_handle = reinterpret_cast<uint64_t>(principal);
    return raw_syscall4(SystemCall::Child,
                        static_cast<long>(reinterpret_cast<uintptr_t>(path)),
                        static_cast<long>(reinterpret_cast<uintptr_t>(args)),
                        static_cast<long>(
                            flags | kProcessSpawnFlagPrincipalConfig),
                        static_cast<long>(reinterpret_cast<uintptr_t>(&config)));
}

static inline long child_with_stdio_as(const char* path,
                                       const char* args,
                                       uint64_t flags,
                                       const char* cwd,
                                       const ProcessStdioConfig* stdio,
                                       void* principal) {
    if (stdio == nullptr || principal == nullptr) {
        return -1;
    }
    ProcessSpawnConfig config{};
    config.cwd_ptr = reinterpret_cast<uint64_t>(cwd);
    config.stdio = *stdio;
    config.principal_handle = reinterpret_cast<uint64_t>(principal);
    return raw_syscall4(
        SystemCall::Child,
        static_cast<long>(reinterpret_cast<uintptr_t>(path)),
        static_cast<long>(reinterpret_cast<uintptr_t>(args)),
        static_cast<long>(flags | kProcessChildFlagStdioConfig |
                          kProcessSpawnFlagPrincipalConfig),
        static_cast<long>(reinterpret_cast<uintptr_t>(&config)));
}

// requests a descriptor of the given type. the optional parameters allow
// callers to select a specific resource instance, request particular
// flag bits, or pass type-specific context understood by the kernel
// or provider.
static inline long descriptor_open(uint32_t type,
                                   uint64_t resource_selector = 0,
                                   uint64_t requested_flags = 0,
                                   uint64_t open_context = 0) {
    return raw_syscall4(SystemCall::DescriptorOpen,
                        static_cast<long>(type),
                        static_cast<long>(resource_selector),
                        static_cast<long>(requested_flags),
                        static_cast<long>(open_context));
}

static inline long descriptor_get_type(uint32_t handle) {
    return raw_syscall1(SystemCall::DescriptorGetType,
                        static_cast<long>(handle));
}

static inline long process_get_standard_descriptor(uint32_t index) {
    uint32_t handle = kInvalidDescriptor;
    if (index == 0) {
        handle = kStandardInputDescriptor;
    } else if (index == 1) {
        handle = kStandardOutputDescriptor;
    } else if (index == 2) {
        handle = kStandardErrorDescriptor;
    } else {
        return -1;
    }
    return descriptor_get_type(handle) >= 0 ? static_cast<long>(handle) : -1;
}

static inline long descriptor_test_flag(uint32_t handle, uint64_t flag) {
    return raw_syscall2(SystemCall::DescriptorTestFlag,
                        static_cast<long>(handle),
                        static_cast<long>(flag));
}

static inline long descriptor_get_flags(uint32_t handle, bool extended) {
    return raw_syscall2(SystemCall::DescriptorGetFlags,
                        static_cast<long>(handle),
                        static_cast<long>(extended ? 1 : 0));
}

static inline void* ensure_reusable_mapping(size_t size,
                                            void*& buffer,
                                            size_t& capacity) {
    if (size == 0) {
        return nullptr;
    }
    if (buffer != nullptr && capacity >= size) {
        return buffer;
    }
    if (buffer != nullptr) {
        unmap(buffer, capacity);
        buffer = nullptr;
        capacity = 0;
    }
    void* mapped = map_anonymous(size, MAP_WRITE);
    if (mapped == nullptr) {
        return nullptr;
    }
    buffer = mapped;
    capacity = size;
    return buffer;
}

static inline long descriptor_get_property(uint32_t handle,
                                           uint32_t property,
                                           void* out,
                                           size_t size) {
    if (size == 0) {
        return raw_syscall4(SystemCall::DescriptorGetProperty,
                            static_cast<long>(handle),
                            static_cast<long>(property),
                            static_cast<long>(reinterpret_cast<uintptr_t>(out)),
                            static_cast<long>(size));
    }
    if (out == nullptr) {
        return -1;
    }
    static void* bounce = nullptr;
    static size_t bounce_capacity = 0;
    bounce = ensure_reusable_mapping(size, bounce, bounce_capacity);
    if (bounce == nullptr) {
        return -1;
    }
    long result = raw_syscall4(SystemCall::DescriptorGetProperty,
                               static_cast<long>(handle),
                               static_cast<long>(property),
                               static_cast<long>(reinterpret_cast<uintptr_t>(bounce)),
                               static_cast<long>(size));
    if (result == 0) {
        auto* src = static_cast<const uint8_t*>(bounce);
        auto* dest = static_cast<uint8_t*>(out);
        for (size_t i = 0; i < size; ++i) {
            dest[i] = src[i];
        }
    }
    return result;
}

static inline long descriptor_set_property(uint32_t handle,
                                           uint32_t property,
                                           const void* in,
                                           size_t size) {
    if (size == 0) {
        return raw_syscall4(SystemCall::DescriptorSetProperty,
                            static_cast<long>(handle),
                            static_cast<long>(property),
                            static_cast<long>(reinterpret_cast<uintptr_t>(in)),
                            static_cast<long>(size));
    }
    if (in == nullptr) {
        return -1;
    }
    static void* bounce = nullptr;
    static size_t bounce_capacity = 0;
    bounce = ensure_reusable_mapping(size, bounce, bounce_capacity);
    if (bounce == nullptr) {
        return -1;
    }
    auto* src = static_cast<const uint8_t*>(in);
    auto* dest = static_cast<uint8_t*>(bounce);
    for (size_t i = 0; i < size; ++i) {
        dest[i] = src[i];
    }
    long result = raw_syscall4(SystemCall::DescriptorSetProperty,
                               static_cast<long>(handle),
                               static_cast<long>(property),
                               static_cast<long>(reinterpret_cast<uintptr_t>(bounce)),
                               static_cast<long>(size));
    return result;
}

static inline long console_get_scale(uint32_t handle, uint32_t* scale) {
    if (scale == nullptr) {
        return -1;
    }
    return descriptor_get_property(
        handle,
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleScale),
        scale,
        sizeof(*scale));
}

static inline long console_set_scale(uint32_t handle, uint32_t scale) {
    return descriptor_set_property(
        handle,
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleScale),
        &scale,
        sizeof(scale));
}

static inline long console_get_font(uint32_t handle,
                                    void* font_payload,
                                    size_t payload_size) {
    if (font_payload == nullptr ||
        payload_size < sizeof(descriptor_defs::ConsoleFont)) {
        return -1;
    }
    return descriptor_get_property(
        handle,
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleFont),
        font_payload,
        payload_size);
}

static inline long console_set_font(uint32_t handle,
                                    const void* font_payload,
                                    size_t payload_size) {
    if (font_payload == nullptr ||
        payload_size < sizeof(descriptor_defs::ConsoleFont)) {
        return -1;
    }
    return descriptor_set_property(
        handle,
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleFont),
        font_payload,
        payload_size);
}

static inline long mount_descriptor(uint32_t block_handle,
                                    const char* mount_name) {
    return raw_syscall2(SystemCall::Mount,
                        static_cast<long>(block_handle),
                        static_cast<long>(
                            reinterpret_cast<uintptr_t>(mount_name)));
}

static inline long rescan_block_devices() {
    return raw_syscall0(SystemCall::RescanBlockDevices);
}

static inline long shared_memory_open(const char* name, size_t length) {
    return descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::SharedMemory),
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(name)),
        static_cast<uint64_t>(length),
        0);
}

static inline long shared_memory_get_info(
    uint32_t handle,
    descriptor_defs::SharedMemoryInfo* info) {
    if (info == nullptr) {
        return -1;
    }
    return descriptor_get_property(
        handle,
        static_cast<uint32_t>(descriptor_defs::Property::SharedMemoryInfo),
        info,
        sizeof(*info));
}

static inline long framebuffer_open() {
    return descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Framebuffer),
        0,
        0,
        0);
}

static inline long framebuffer_open_slot(uint32_t slot) {
    return descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Framebuffer),
        static_cast<uint64_t>(slot),
        0,
        0);
}

static inline long framebuffer_get_info(
    uint32_t handle,
    descriptor_defs::FramebufferInfo* info) {
    if (info == nullptr) {
        return -1;
    }
    return descriptor_get_property(
        handle,
        static_cast<uint32_t>(descriptor_defs::Property::FramebufferInfo),
        info,
        sizeof(*info));
}

static inline long framebuffer_present(
    uint32_t handle,
    const descriptor_defs::FramebufferRect* rect) {
    return descriptor_set_property(
        handle,
        static_cast<uint32_t>(descriptor_defs::Property::FramebufferPresent),
        rect,
        rect ? sizeof(*rect) : 0);
}

static inline long graphical_session_open() {
    return descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::GraphicalSession),
        0,
        0,
        0);
}

static inline long graphical_session_get_info(
    uint32_t handle,
    descriptor_defs::GraphicalSessionInfo* info) {
    if (info == nullptr) {
        return -1;
    }
    return descriptor_get_property(
        handle,
        static_cast<uint32_t>(
            descriptor_defs::Property::GraphicalSessionInfo),
        info,
        sizeof(*info));
}

static inline long graphical_session_set_active(uint32_t handle,
                                                bool active) {
    descriptor_defs::GraphicalSessionControl control{
        .command = active
                       ? descriptor_defs::kGraphicalSessionCommandActivate
                       : descriptor_defs::kGraphicalSessionCommandDeactivate,
        .flags = 0,
    };
    return descriptor_set_property(
        handle,
        static_cast<uint32_t>(
            descriptor_defs::Property::GraphicalSessionControl),
        &control,
        sizeof(control));
}

static inline long mouse_open() {
    return descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Mouse),
        0,
        0,
        0);
}

static inline long pipe_open_new(uint64_t flags) {
    return descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Pipe),
        flags,
        0,
        0);
}

static inline long pipe_open_existing(uint64_t flags, uint64_t pipe_id) {
    return descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::Pipe),
        flags,
        pipe_id,
        0);
}

static inline long pipe_get_info(uint32_t handle,
                                 descriptor_defs::PipeInfo* info) {
    if (info == nullptr) {
        return -1;
    }
    return descriptor_get_property(
        handle,
        static_cast<uint32_t>(descriptor_defs::Property::PipeInfo),
        info,
        sizeof(*info));
}

static inline long net_endpoint_open_new(uint64_t flags,
                                         uint64_t open_context = 0) {
    return descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::NetEndpoint),
        flags,
        0,
        open_context);
}

static inline long net_endpoint_open_existing(uint64_t flags,
                                              uint64_t endpoint_id,
                                              uint64_t open_context = 0) {
    return descriptor_open(
        static_cast<uint32_t>(descriptor_defs::Type::NetEndpoint),
        flags,
        endpoint_id,
        open_context);
}

static inline long net_endpoint_get_info(
    uint32_t handle,
    descriptor_defs::NetEndpointInfo* info) {
    if (info == nullptr) {
        return -1;
    }
    return descriptor_get_property(
        handle,
        static_cast<uint32_t>(descriptor_defs::Property::NetEndpointInfo),
        info,
        sizeof(*info));
}

static inline long net_device_open(uint32_t index = 0,
                                   uint64_t requested_flags = 0) {
    return descriptor_open(static_cast<uint32_t>(descriptor_defs::Type::NetDevice),
                           index,
                           requested_flags,
                           0);
}

static inline long net_device_get_info(uint32_t handle,
                                       descriptor_defs::NetDeviceInfo* info) {
    if (info == nullptr) {
        return -1;
    }
    return descriptor_get_property(
        handle,
        static_cast<uint32_t>(descriptor_defs::Property::NetDeviceInfo),
        info,
        sizeof(*info));
}

static inline long net_device_get_ipv4_config(
    uint32_t handle,
    descriptor_defs::NetIpv4Config* config) {
    if (config == nullptr) {
        return -1;
    }
    return descriptor_get_property(
        handle,
        static_cast<uint32_t>(descriptor_defs::Property::NetIpv4Config),
        config,
        sizeof(*config));
}

static inline long net_device_set_ipv4_config(
    uint32_t handle,
    const descriptor_defs::NetIpv4Config* config) {
    if (config == nullptr) {
        return -1;
    }
    return descriptor_set_property(
        handle,
        static_cast<uint32_t>(descriptor_defs::Property::NetIpv4Config),
        config,
        sizeof(*config));
}

static inline long descriptor_read(uint32_t handle,
                                       void* buffer,
                                       size_t length,
                                       uint64_t offset = 0) {
    return raw_syscall4(SystemCall::DescriptorRead,
                        static_cast<long>(handle),
                        static_cast<long>(reinterpret_cast<uintptr_t>(buffer)),
                        static_cast<long>(length),
                        static_cast<long>(offset));
}

static inline long descriptor_write(uint32_t handle,
                                        const void* buffer,
                                        size_t length,
                                        uint64_t offset = 0) {
    return raw_syscall4(SystemCall::DescriptorWrite,
                        static_cast<long>(handle),
                        static_cast<long>(reinterpret_cast<uintptr_t>(buffer)),
                        static_cast<long>(length),
                        static_cast<long>(offset));
}

static inline long descriptor_close(uint32_t handle) {
    return raw_syscall1(SystemCall::DescriptorClose,
                        static_cast<long>(handle));
}

static inline long file_open(const char* path) {
    return raw_syscall1(SystemCall::FileOpen,
                        static_cast<long>(reinterpret_cast<uintptr_t>(path)));
}

static inline long file_close(uint32_t handle) {
    return raw_syscall1(SystemCall::FileClose,
                        static_cast<long>(handle));
}

static inline long file_sync(uint32_t handle) {
    return raw_syscall1(SystemCall::FileSync,
                        static_cast<long>(handle));
}

static inline long system_sync() {
    return raw_syscall0(SystemCall::Sync);
}

static inline long system_shutdown() {
    return raw_syscall0(SystemCall::Shutdown);
}

static inline long module_load(const char* path) {
    return raw_syscall1(SystemCall::ModuleLoad,
                        static_cast<long>(reinterpret_cast<uintptr_t>(path)));
}

static inline long module_count() {
    return raw_syscall0(SystemCall::ModuleCount);
}

static inline long module_info(size_t index, ModuleInfo* info) {
    if (info == nullptr) {
        return -1;
    }
    return raw_syscall2(SystemCall::ModuleInfo,
                        static_cast<long>(index),
                        static_cast<long>(reinterpret_cast<uintptr_t>(info)));
}

static inline long file_read(uint32_t handle,
                             void* buffer,
                             size_t length) {
    return raw_syscall3(SystemCall::FileRead,
                        static_cast<long>(handle),
                        static_cast<long>(reinterpret_cast<uintptr_t>(buffer)),
                        static_cast<long>(length));
}

static inline long file_write(uint32_t handle,
                              const void* buffer,
                              size_t length) {
    return raw_syscall3(SystemCall::FileWrite,
                        static_cast<long>(handle),
                        static_cast<long>(reinterpret_cast<uintptr_t>(buffer)),
                        static_cast<long>(length));
}

static inline long exec(const char* path,
                        const char* args,
                        uint64_t flags,
                        const char* cwd) {
    return raw_syscall4(SystemCall::ProcessExec,
                        static_cast<long>(reinterpret_cast<uintptr_t>(path)),
                        static_cast<long>(reinterpret_cast<uintptr_t>(args)),
                        static_cast<long>(flags),
                        static_cast<long>(reinterpret_cast<uintptr_t>(cwd)));
}

static inline long exec_as(const char* path,
                           const char* args,
                           uint64_t flags,
                           const char* cwd,
                           void* principal) {
    if (principal == nullptr) {
        return -1;
    }
    ProcessSpawnConfig config{};
    config.cwd_ptr = reinterpret_cast<uint64_t>(cwd);
    config.principal_handle = reinterpret_cast<uint64_t>(principal);
    return raw_syscall4(
        SystemCall::ProcessExec,
        static_cast<long>(reinterpret_cast<uintptr_t>(path)),
        static_cast<long>(reinterpret_cast<uintptr_t>(args)),
        static_cast<long>(flags | kProcessSpawnFlagPrincipalConfig),
        static_cast<long>(reinterpret_cast<uintptr_t>(&config)));
}

static inline long setcwd(const char* path) {
    return raw_syscall1(SystemCall::ProcessSetCwd,
                        static_cast<long>(reinterpret_cast<uintptr_t>(path)));
}

static inline long getcwd(char* buffer, size_t length) {
    return raw_syscall2(SystemCall::ProcessGetCwd,
                        static_cast<long>(reinterpret_cast<uintptr_t>(buffer)),
                        static_cast<long>(length));
}

static inline long directory_open(const char* path) {
    return raw_syscall1(SystemCall::DirectoryOpen,
                        static_cast<long>(reinterpret_cast<uintptr_t>(path)));
}

static inline long directory_open_root() {
    return raw_syscall0(SystemCall::DirectoryOpenRoot);
}

static inline long directory_open_at(uint32_t dir_handle, const char* name) {
    return raw_syscall2(SystemCall::DirectoryOpenAt,
                        static_cast<long>(dir_handle),
                        static_cast<long>(reinterpret_cast<uintptr_t>(name)));
}

static inline long file_create(const char* path) {
    return raw_syscall1(SystemCall::FileCreate,
                        static_cast<long>(reinterpret_cast<uintptr_t>(path)));
}

static inline long file_open_at(uint32_t dir_handle, const char* name) {
    return raw_syscall2(SystemCall::FileOpenAt,
                        static_cast<long>(dir_handle),
                        static_cast<long>(reinterpret_cast<uintptr_t>(name)));
}

static inline long file_create_at(uint32_t dir_handle, const char* name) {
    return raw_syscall2(SystemCall::FileCreateAt,
                        static_cast<long>(dir_handle),
                        static_cast<long>(reinterpret_cast<uintptr_t>(name)));
}

static inline long directory_read(uint32_t handle, DirEntry* out_entry) {
    return raw_syscall2(SystemCall::DirectoryRead,
                        static_cast<long>(handle),
                        static_cast<long>(reinterpret_cast<uintptr_t>(out_entry)));
}

static inline long directory_close(uint32_t handle) {
    return raw_syscall1(SystemCall::DirectoryClose,
                        static_cast<long>(handle));
}

static inline long directory_create(const char* path) {
    return raw_syscall1(SystemCall::DirectoryCreate,
                        static_cast<long>(reinterpret_cast<uintptr_t>(path)));
}

static inline long file_remove(const char* path) {
    return raw_syscall1(SystemCall::FileRemove,
                        static_cast<long>(reinterpret_cast<uintptr_t>(path)));
}

static inline long file_get_acl(const char* path,
                                FileAclEntry* entries,
                                size_t max_entries) {
    return raw_syscall3(
        SystemCall::FileGetAcl,
        reinterpret_cast<long>(path),
        reinterpret_cast<long>(entries),
        static_cast<long>(max_entries));
}

static inline long file_set_acl(const char* path,
                                const FileAclEntry* entries,
                                size_t entry_count) {
    return raw_syscall3(
        SystemCall::FileSetAcl,
        reinterpret_cast<long>(path),
        reinterpret_cast<long>(entries),
        static_cast<long>(entry_count));
}

static inline long directory_remove(const char* path) {
    return raw_syscall1(SystemCall::DirectoryRemove,
                        static_cast<long>(reinterpret_cast<uintptr_t>(path)));
}
// user and principal helpers
static inline void* user_create(const char* name, uint64_t caps) {
    long ret = raw_syscall2(SystemCall::UserCreate,
                            reinterpret_cast<long>(name),
                            static_cast<long>(caps));
    return reinterpret_cast<void*>(static_cast<uintptr_t>(ret));
}

static inline void* user_find(const char* name) {
    long ret = raw_syscall1(SystemCall::UserFind,
                            reinterpret_cast<long>(name));
    return reinterpret_cast<void*>(static_cast<uintptr_t>(ret));
}

static inline long user_bump_generation(void* user) {
    return raw_syscall1(SystemCall::UserBumpGeneration,
                        reinterpret_cast<long>(user));
}

static inline long user_set_password(void* user,
                                     const uint8_t* salt,
                                     const uint8_t* hash,
                                     uint32_t iterations) {
    return raw_syscall4(SystemCall::UserSetPassword,
                        reinterpret_cast<long>(user),
                        reinterpret_cast<long>(salt),
                        reinterpret_cast<long>(hash),
                        static_cast<long>(iterations));
}

static inline long user_info(void* user, UserInfo* out) {
    return raw_syscall2(SystemCall::UserInfo,
                        reinterpret_cast<long>(user),
                        reinterpret_cast<long>(out));
}

static inline void* principal_create(void* backing_user, uint64_t allowed_caps) {
    long ret = raw_syscall2(SystemCall::PrincipalCreate,
                            reinterpret_cast<long>(backing_user),
                            static_cast<long>(allowed_caps));
    return reinterpret_cast<void*>(static_cast<uintptr_t>(ret));
}

static inline long principal_set(void* principal) {
    return raw_syscall1(SystemCall::PrincipalSet,
                        reinterpret_cast<long>(principal));
}

static inline long capability_grant(uint64_t kind_value) {
    return raw_syscall1(SystemCall::CapabilityGrant,
                        static_cast<long>(kind_value));
}

static inline long capability_pass(uint32_t child_pid,
                                   const uint64_t* handles,
                                   uint64_t count) {
    return raw_syscall3(SystemCall::CapabilityPass,
                        static_cast<long>(child_pid),
                        reinterpret_cast<long>(handles),
                        static_cast<long>(count));
}
