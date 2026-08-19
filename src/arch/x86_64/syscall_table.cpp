#include "arch/x86_64/syscall_table.hpp"

#include "../../drivers/log/logging.hpp"
#include "../../drivers/fs/block_cache.hpp"
#include "../../drivers/fs/mount_manager.hpp"
#include "../../drivers/driver_registry.hpp"
#include "../../kernel/descriptor.hpp"
#include "../../kernel/capabilities.hpp"
#include "../../kernel/file_io.hpp"
#include "../../kernel/loader.hpp"
#include "../../kernel/module.hpp"
#include "../../kernel/process.hpp"
#include "../../kernel/random.hpp"
#include "../../kernel/path_util.hpp"
#include "../../kernel/scheduler.hpp"
#include "../../kernel/time.hpp"
#include "../../kernel/vm.hpp"
#include "../../fs/vfs.hpp"
#include "../../lib/mem.hpp"
#include "../../kernel/string_util.hpp"
#include "../../kernel/sync.hpp"
#include "../../kernel/users.hpp"
#include "../../kernel/version.hpp"
#include "arch/x86_64/percpu.hpp"
#include "arch/x86_64/io.hpp"

namespace syscall {

namespace {

constexpr uint64_t kAbiMajor = 2;
constexpr uint64_t kAbiMinor = 0;

class PrincipalRefGuard {
public:
    explicit PrincipalRefGuard(capabilities::Principal* principal)
        : principal_(principal) {}
    ~PrincipalRefGuard() {
        capabilities::principal_release(principal_);
    }
    PrincipalRefGuard(const PrincipalRefGuard&) = delete;
    PrincipalRefGuard& operator=(const PrincipalRefGuard&) = delete;

    capabilities::Principal* release() {
        capabilities::Principal* principal = principal_;
        principal_ = nullptr;
        return principal;
    }

private:
    capabilities::Principal* principal_;
};

class ResourcePairGuard {
public:
    ResourcePairGuard(process::ProcessResources& a,
                      process::ProcessResources& b) {
        first_ = &a;
        second_ = &b;
        if (reinterpret_cast<uintptr_t>(second_) <
            reinterpret_cast<uintptr_t>(first_)) {
            process::ProcessResources* temp = first_;
            first_ = second_;
            second_ = temp;
        }
        first_->lock.lock();
        if (second_ != first_) {
            second_->lock.lock();
        }
    }
    ~ResourcePairGuard() {
        if (second_ != first_) {
            second_->lock.unlock();
        }
        first_->lock.unlock();
    }
    ResourcePairGuard(const ResourcePairGuard&) = delete;
    ResourcePairGuard& operator=(const ResourcePairGuard&) = delete;
private:
    process::ProcessResources* first_;
    process::ProcessResources* second_;
};

class DescriptorPairGuard {
public:
    DescriptorPairGuard(process::ProcessResources& a,
                        process::ProcessResources& b) {
        first_ = &a;
        second_ = &b;
        if (reinterpret_cast<uintptr_t>(second_) <
            reinterpret_cast<uintptr_t>(first_)) {
            process::ProcessResources* temp = first_;
            first_ = second_;
            second_ = temp;
        }
        first_->descriptor_lock.lock();
        if (second_ != first_) {
            second_->descriptor_lock.lock();
        }
    }
    ~DescriptorPairGuard() {
        if (second_ != first_) {
            second_->descriptor_lock.unlock();
        }
        first_->descriptor_lock.unlock();
    }
    DescriptorPairGuard(const DescriptorPairGuard&) = delete;
    DescriptorPairGuard& operator=(const DescriptorPairGuard&) = delete;
private:
    process::ProcessResources* first_;
    process::ProcessResources* second_;
};

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

void snapshot_cwd(const process::Task& proc,
                  char (&out)[path_util::kMaxPathLength]) {
    sync::LockGuard guard(proc.resources->lock);
    string_util::copy(out, sizeof(out), proc.resources->cwd);
}

bool build_process_path(const process::Task& proc,
                        const char* path,
                        char (&out)[path_util::kMaxPathLength]) {
    char cwd[path_util::kMaxPathLength];
    snapshot_cwd(proc, cwd);
    return path_util::build_absolute_path(cwd, path, out);
}

struct UserInfo {
    uint64_t id_machine;
    uint64_t id_local;
    char name[32];
    uint64_t allowed_caps;
    uint64_t generation;
    uint32_t password_set;
    uint32_t active;
};

static_assert(sizeof(UserInfo) == 72, "UserInfo size mismatch");

struct SystemInfo {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

static_assert(sizeof(SystemInfo) == 325, "SystemInfo size mismatch");

constexpr uint64_t kProcessChildFlagStdioConfig = 1ull << 0;
constexpr uint64_t kProcessSpawnFlagPrincipalConfig = 1ull << 1;

[[noreturn]] void halt_forever() {
    asm volatile("cli");
    for (;;) {
        asm volatile("hlt");
    }
}

void request_cpu_reset() {
    log_message(LogLevel::Info, "Shutdown: requesting CPU reset");

    uint8_t port92 = inb(0x92);
    outb(0x92, static_cast<uint8_t>(port92 | 0x01));
    io_wait();

    outb(0xCF9, 0x02);
    io_wait();
    outb(0xCF9, 0x06);
    io_wait();

    for (uint32_t i = 0; i < 0x10000; ++i) {
        if ((inb(0x64) & 0x02) == 0) {
            outb(0x64, 0xFE);
            break;
        }
        io_wait();
    }
}

uint64_t place_args_on_stack(process::Task& child, const char* args) {
    if (args == nullptr) {
        return 0;
    }

    constexpr size_t kMaxArgBytes = 512;
    char arg_copy[kMaxArgBytes];
    process::Task* parent = process::current();
    if (parent == nullptr ||
        !vm::copy_user_string(parent->cr3,
                              args,
                              arg_copy,
                              sizeof(arg_copy))) {
        log_message(LogLevel::Error,
                    "ExecArgs: copy_user_string failed ptr=%llx",
                    static_cast<unsigned long long>(
                        reinterpret_cast<uint64_t>(args)));
        return 0;
    }
    size_t len = string_util::length(arg_copy);
    if (len == 0) {
        return 0;
    }

    uint64_t target_cr3 = child.cr3;
    if (target_cr3 == 0) {
        return 0;
    }
    constexpr size_t kArgRegionBytes = 512;
    vm::Region arg_region = vm::allocate_user_region(target_cr3, kArgRegionBytes);
    if (arg_region.base == 0 || arg_region.length == 0) {
        log_message(LogLevel::Error,
                    "ExecArgs: allocate_user_region failed len=%zu",
                    len + 1);
        return 0;
    }
    if (len + 1 > arg_region.length) {
        len = arg_region.length - 1;
    }

    uint64_t dest = arg_region.base;
    if (!vm::copy_to_user(target_cr3, dest, arg_copy, len + 1)) {
        log_message(LogLevel::Error,
                    "ExecArgs: copy_to_user failed dest=%llx len=%zu",
                    static_cast<unsigned long long>(dest),
                    len + 1);
        return 0;
    }
    char verify_copy[kMaxArgBytes];
    if (vm::copy_from_user(target_cr3, verify_copy, dest, len + 1)) {
        verify_copy[len] = '\0';
    } else {
        log_message(LogLevel::Error,
                    "ExecArgs: copy_from_user verify failed dest=%llx len=%zu",
                    static_cast<unsigned long long>(dest),
                    len + 1);
    }
    return dest;
}

uint32_t pick_child_cpu(process::Task* parent) {
    if (parent != nullptr && parent->preferred_cpu != UINT32_MAX) {
        return parent->preferred_cpu;
    }
    if (auto* cpu = percpu::current_cpu()) {
        return cpu->index;
    }
    return UINT32_MAX;
}

bool mapping_within_limit(const process::Task& proc, size_t length) {
    if (proc.resources == nullptr || length == 0) {
        return false;
    }
    vm::Usage usage = vm::usage(proc.cr3);
    uint64_t limit = proc.resources->limits.max_virtual_bytes;
    return usage.virtual_bytes <= limit &&
           static_cast<uint64_t>(length) <= limit - usage.virtual_bytes;
}

// Validate that all capability handles provided in user memory correspond to
// tokens permitting the requested kind. r12: pointer to handles array in user
// space, r13: number of handles. We cap the number to avoid large copies.
bool require_capability(process::Task& proc,
                        capabilities::CapabilityKind kind,
                        const syscall::SyscallFrame& frame) {
    sync::LockGuard resource_guard(proc.resources->lock);
    if (capabilities::principal_allows_or_unconfined(
            proc.resources->principal,
            kind)) {
        return true;
    }
    const uint64_t* user_handles = reinterpret_cast<const uint64_t*>(frame.r12);
    size_t count = static_cast<size_t>(frame.r13 & 0xFFFF);
    constexpr size_t kMaxHandles = 8;
    if (count > kMaxHandles) {
        count = kMaxHandles;
    }
    if (count == 0) {
        return false;
    }

    uint64_t local_handles[kMaxHandles];
    if (!vm::copy_from_user(proc.cr3,
                            local_handles,
                            reinterpret_cast<uint64_t>(user_handles),
                            count * sizeof(uint64_t))) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        uint64_t h = local_handles[i];
        capabilities::CapabilityToken* tok = capabilities::cap_table_lookup(
            proc.resources->cap_handles,
            capabilities::kMaxProcessCapabilities,
            h,
            true);
        if (tok != nullptr && tok->kind == kind &&
            capabilities::token_valid(*tok)) {
            return true;
        }
    }
    return false;
}

bool prepare_spawn_config(process::Task& parent,
                          const SyscallFrame& frame,
                          uint64_t flags,
                          bool allow_stdio,
                          const char*& cwd,
                          ProcessStdioConfig* stdio,
                          capabilities::Principal*& child_principal) {
    const bool has_stdio =
        allow_stdio && (flags & kProcessChildFlagStdioConfig) != 0;
    const bool has_principal =
        (flags & kProcessSpawnFlagPrincipalConfig) != 0;
    child_principal = nullptr;
    if (!has_stdio && !has_principal) {
        sync::LockGuard guard(parent.resources->lock);
        child_principal = parent.resources->principal;
        return capabilities::principal_add_ref(child_principal);
    }

    auto* config_user =
        reinterpret_cast<const ProcessSpawnConfig*>(frame.r10);
    if (config_user == nullptr) {
        return false;
    }

    uint64_t cwd_ptr = 0;
    if (!vm::copy_from_user(parent.cr3,
                            &cwd_ptr,
                            reinterpret_cast<uint64_t>(&config_user->cwd_ptr),
                            sizeof(cwd_ptr))) {
        return false;
    }
    cwd = reinterpret_cast<const char*>(cwd_ptr);

    if (has_stdio &&
        (stdio == nullptr ||
         !vm::copy_from_user(parent.cr3,
                             stdio,
                             reinterpret_cast<uint64_t>(&config_user->stdio),
                             sizeof(*stdio)))) {
        return false;
    }

    if (!has_principal) {
        sync::LockGuard guard(parent.resources->lock);
        child_principal = parent.resources->principal;
        return capabilities::principal_add_ref(child_principal);
    }
    // IdentityManage is the kernel's explicit impersonation authority; the
    // same permission is required by PrincipalSet and PrincipalCreate.
    if (!require_capability(parent,
                            capabilities::CapabilityKind::IdentityManage,
                            frame)) {
        return false;
    }
    uint64_t principal_handle = 0;
    if (!vm::copy_from_user(
            parent.cr3,
            &principal_handle,
            reinterpret_cast<uint64_t>(&config_user->principal_handle),
            sizeof(principal_handle))) {
        return false;
    }
    capabilities::Principal* requested =
        capabilities::principal_acquire_from_handle(principal_handle);
    if (requested == nullptr) {
        return false;
    }
    child_principal = requested;
    return true;
}

bool descriptor_type_capability(
    uint32_t type,
    capabilities::CapabilityKind& out_kind) {
    switch (type) {
        case descriptor::kTypeSerial:
            out_kind = capabilities::CapabilityKind::Serial;
            return true;
        case descriptor::kTypeBlockDevice:
        case descriptor::kTypeDisk:
        case descriptor::kTypePartition:
            out_kind = capabilities::CapabilityKind::StorageRawRead;
            return true;
        case descriptor::kTypeNetDevice:
            out_kind = capabilities::CapabilityKind::NetworkManage;
            return true;
        case descriptor::kTypeNetEndpoint:
            out_kind = capabilities::CapabilityKind::Network;
            return true;
        case descriptor::kTypePci:
            out_kind = capabilities::CapabilityKind::Pci;
            return true;
        case descriptor::kTypeAudioOutput:
            out_kind = capabilities::CapabilityKind::Audio;
            return true;
        case descriptor::kTypeCpuStats:
        case descriptor::kTypeSensor:
            out_kind = capabilities::CapabilityKind::SystemMonitor;
            return true;
        case descriptor::kTypeTaskStats:
            out_kind = capabilities::CapabilityKind::ProcessInspect;
            return true;
        case descriptor::kTypeKernelLog:
            out_kind = capabilities::CapabilityKind::KernelLog;
            return true;
        case descriptor::kTypeGraphicalSession:
        case descriptor::kTypeFramebuffer:
            out_kind = capabilities::CapabilityKind::GraphicalSession;
            return true;
        case descriptor::kTypeKeyboard:
        case descriptor::kTypeMouse:
            out_kind = capabilities::CapabilityKind::InputDevices;
            return true;
        default:
            return false;
    }
}

bool descriptor_handle_is_storage(process::Task& proc, uint32_t handle) {
    uint16_t type = 0;
    {
        sync::LockGuard descriptor_guard(proc.resources->descriptor_lock);
        if (!descriptor::get_type(proc.resources->descriptors, handle, type)) {
            return false;
        }
    }
    return type == descriptor::kTypeBlockDevice ||
           type == descriptor::kTypeDisk ||
           type == descriptor::kTypePartition;
}

bool console_property_requires_settings_access(uint32_t property) {
    return property == static_cast<uint32_t>(
                           descriptor_defs::Property::ConsoleKernelLog) ||
           property == static_cast<uint32_t>(
                           descriptor_defs::Property::ConsoleScale) ||
           property == static_cast<uint32_t>(
                           descriptor_defs::Property::ConsoleFont);
}

uint32_t duplicate_stream_descriptor(process::Task& from,
                                     process::Task& to,
                                     uint32_t handle,
                                     uint16_t target_index) {
    if (handle == descriptor::kInvalidHandle) {
        return descriptor::kInvalidHandle;
    }
    DescriptorPairGuard descriptor_guard(*from.resources, *to.resources);

    uint16_t type = 0;
    if (!descriptor::get_type(from.resources->descriptors, handle, type)) {
        return descriptor::kInvalidHandle;
    }

    uint64_t flags = 0;
    if (!descriptor::get_flags(from.resources->descriptors, handle, false, flags)) {
        return descriptor::kInvalidHandle;
    }
    flags &= (static_cast<uint64_t>(descriptor::Flag::Readable) |
              static_cast<uint64_t>(descriptor::Flag::Writable) |
              static_cast<uint64_t>(descriptor::Flag::Async));

    if (type == descriptor::kTypePipe) {
        descriptor_defs::PipeInfo info{};
        if (descriptor::get_property_trusted(
                from.resources->descriptors,
                handle,
                static_cast<uint32_t>(descriptor_defs::Property::PipeInfo),
                &info,
                sizeof(info)) != 0 ||
            info.id == 0) {
            return descriptor::kInvalidHandle;
        }
        return descriptor::open_at(to,
                                   to.resources->descriptors,
                                   target_index,
                                   descriptor::kTypePipe,
                                   flags,
                                   info.id,
                                   0);
    }

    if (type == descriptor::kTypeNetEndpoint) {
        descriptor_defs::NetEndpointInfo info{};
        if (descriptor::get_property_trusted(
                from.resources->descriptors,
                handle,
                static_cast<uint32_t>(descriptor_defs::Property::NetEndpointInfo),
                &info,
                sizeof(info)) != 0 ||
            info.id == 0) {
            return descriptor::kInvalidHandle;
        }
        uint64_t context =
            info.role == 1
                ? static_cast<uint64_t>(descriptor_defs::kNetEndpointOpenService)
                : 0;
        return descriptor::open_at(to,
                                   to.resources->descriptors,
                                   target_index,
                                   descriptor::kTypeNetEndpoint,
                                   flags,
                                   info.id,
                                   context);
    }

    if (type == descriptor::kTypeVty) {
        descriptor_defs::VtyInfo info{};
        if (descriptor::get_property_trusted(
                from.resources->descriptors,
                handle,
                static_cast<uint32_t>(descriptor_defs::Property::VtyInfo),
                &info,
                sizeof(info)) != 0 ||
            info.id == 0) {
            return descriptor::kInvalidHandle;
        }
        uint32_t previous_vty = to.resources->vty_id;
        to.resources->vty_id = info.id;
        uint32_t duplicate = descriptor::open_at(to,
                                                 to.resources->descriptors,
                                                 target_index,
                                                 descriptor::kTypeVty,
                                                 info.id,
                                                 flags,
                                                 0);
        if (duplicate == descriptor::kInvalidHandle) {
            to.resources->vty_id = previous_vty;
        }
        return duplicate;
    }

    return descriptor::kInvalidHandle;
}

void inherit_standard_descriptors(process::Task& parent,
                                  process::Task& child) {
    for (size_t i = 0; i < 3; ++i) {
        child.resources->standard_descriptors[i] =
            duplicate_stream_descriptor(parent,
                                        child,
                                        parent.resources->standard_descriptors[i],
                                        static_cast<uint16_t>(i));
    }
}

void install_standard_descriptors(process::Task& parent,
                                  process::Task& child,
                                  const ProcessStdioConfig& config) {
    const uint32_t handles[3] = {
        config.stdin_handle,
        config.stdout_handle,
        config.stderr_handle,
    };
    for (size_t i = 0; i < 3; ++i) {
        if (handles[i] == descriptor::kInvalidHandle) {
            continue;
        }
        uint32_t child_handle =
            duplicate_stream_descriptor(parent,
                                        child,
                                        handles[i],
                                        static_cast<uint16_t>(i));
        if (child_handle != descriptor::kInvalidHandle) {
            child.resources->standard_descriptors[i] = child_handle;
        }
    }
}

}  // namespace

Result handle_syscall(SyscallFrame& frame) {
    switch (static_cast<SystemCall>(frame.rax)) {
        case SystemCall::AbiMajor: {
            frame.rax = kAbiMajor;
            return Result::Continue;
        }
        case SystemCall::AbiMinor: {
            frame.rax = kAbiMinor;
            return Result::Continue;
        }
        case SystemCall::SystemInfo: {
            process::Task* proc = process::current();
            if (proc == nullptr || frame.rdi == 0 ||
                frame.rsi < sizeof(SystemInfo)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            SystemInfo info{};
            string_util::copy(info.sysname, sizeof(info.sysname), "Neutrino");
            string_util::copy(info.nodename, sizeof(info.nodename), "neutrino");
            string_util::copy(info.release,
                              sizeof(info.release),
                              kernel_version::release());
            string_util::copy(info.version,
                              sizeof(info.version),
                              "Neutrino kernel");
            string_util::copy(info.machine, sizeof(info.machine), "x86_64");
            if (!vm::copy_to_user(proc->cr3, frame.rdi, &info, sizeof(info))) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            frame.rax = 0;
            return Result::Continue;
        }
        case SystemCall::Exit: {
            frame.rax = frame.rdi & 0xFFFFu;
            process::Task* proc = process::current();
            if (proc != nullptr) {
                process::terminate_group(
                    *proc,
                    static_cast<uint16_t>(frame.rax));
            }
            return Result::Reschedule;
        }
        case SystemCall::Yield: {
            return Result::Reschedule;
        }
        case SystemCall::Sleep: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            uint64_t duration_ns = frame.rdi;
            uint64_t ticks = timekeeping::ticks_for_duration_ns(duration_ns);
            if (ticks == 0) {
                frame.rax = 0;
                return Result::Continue;
            }

            uint64_t now_ticks = timekeeping::tick_count();
            proc->sleep_until_tick =
                ticks > UINT64_MAX - now_ticks ? UINT64_MAX
                                                : now_ticks + ticks;
            proc->waiting_on = nullptr;
            process::store_state(*proc, process::State::Blocked);
            frame.rax = 0;
            return Result::Reschedule;
        }
        case SystemCall::DescriptorOpen: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            uint32_t type = static_cast<uint32_t>(frame.rdi & 0xFFFFu);
            capabilities::CapabilityKind required{};
            if (descriptor_type_capability(type, required) &&
                !require_capability(*proc, required, frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            sync::LockGuard descriptor_guard(
                proc->resources->descriptor_lock);
            uint32_t handle =
                descriptor::open(*proc,
                                 proc->resources->descriptors,
                                 type,
                                 frame.rsi,
                                 frame.rdx,
                                 frame.r10);
            frame.rax = (handle == descriptor::kInvalidHandle)
                            ? static_cast<uint64_t>(-1)
                            : static_cast<uint64_t>(handle);
            return Result::Continue;
        }
        case SystemCall::DescriptorRead: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            uint32_t handle =
                static_cast<uint32_t>(frame.rdi & 0xFFFFFFFFu);
            sync::LockGuard descriptor_guard(
                proc->resources->descriptor_lock);
            int64_t result = descriptor::read(*proc,
                                              proc->resources->descriptors,
                                              handle,
                                              frame.rsi,
                                              frame.rdx,
                                              frame.r10);
            if (result == descriptor::kWouldBlock) {
                frame.rax = static_cast<uint64_t>(static_cast<int64_t>(result));
                return Result::Reschedule;
            }
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(result));
            return Result::Continue;
        }
        case SystemCall::DescriptorWrite: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            uint32_t handle =
                static_cast<uint32_t>(frame.rdi & 0xFFFFFFFFu);
            if (descriptor_handle_is_storage(*proc, handle) &&
                 !require_capability(
                     *proc,
                     capabilities::CapabilityKind::StorageRawWrite,
                     frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            sync::LockGuard descriptor_guard(
                proc->resources->descriptor_lock);
            int64_t result = descriptor::write(*proc,
                                               proc->resources->descriptors,
                                               handle,
                                               frame.rsi,
                                               frame.rdx,
                                               frame.r10);
            if (result == descriptor::kWouldBlock) {
                frame.rax = static_cast<uint64_t>(static_cast<int64_t>(result));
                return Result::Reschedule;
            }
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(result));
            return Result::Continue;
        }
        case SystemCall::DescriptorClose: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            sync::LockGuard descriptor_guard(
                proc->resources->descriptor_lock);
            bool ok = descriptor::close(*proc,
                                        proc->resources->descriptors,
                                        static_cast<uint32_t>(frame.rdi &
                                                               0xFFFFFFFFu));
            frame.rax = ok ? 0 : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::DescriptorGetType: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            sync::LockGuard descriptor_guard(
                proc->resources->descriptor_lock);
            uint16_t type = 0;
            bool ok = descriptor::get_type(
                proc->resources->descriptors,
                static_cast<uint32_t>(frame.rdi & 0xFFFFFFFFu),
                type);
            frame.rax =
                ok ? static_cast<uint64_t>(type) : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::DescriptorTestFlag: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            sync::LockGuard descriptor_guard(
                proc->resources->descriptor_lock);
            bool value = false;
            bool ok = descriptor::test_flag(
                proc->resources->descriptors,
                static_cast<uint32_t>(frame.rdi & 0xFFFFFFFFu),
                static_cast<uint64_t>(frame.rsi),
                value);
            if (!ok) {
                frame.rax = static_cast<uint64_t>(-1);
            } else {
                frame.rax = value ? 1 : 0;
            }
            return Result::Continue;
        }
        case SystemCall::DescriptorGetFlags: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            sync::LockGuard descriptor_guard(
                proc->resources->descriptor_lock);
            uint64_t flags = 0;
            bool ok = descriptor::get_flags(
                proc->resources->descriptors,
                static_cast<uint32_t>(frame.rdi & 0xFFFFFFFFu),
                frame.rsi != 0,
                flags);
            frame.rax =
                ok ? flags : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::DescriptorGetProperty: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            uint32_t handle =
                static_cast<uint32_t>(frame.rdi & 0xFFFFFFFFu);
            sync::LockGuard descriptor_guard(
                proc->resources->descriptor_lock);
            int result = descriptor::get_property(
                *proc,
                proc->resources->descriptors,
                handle,
                static_cast<uint32_t>(frame.rsi & 0xFFFFFFFFu),
                frame.rdx,
                frame.r10);
            frame.rax = (result == 0) ? 0 : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::DescriptorSetProperty: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            uint32_t handle =
                static_cast<uint32_t>(frame.rdi & 0xFFFFFFFFu);
            uint32_t property =
                static_cast<uint32_t>(frame.rsi & 0xFFFFFFFFu);
            uint16_t type = 0;
            {
                sync::LockGuard descriptor_guard(
                    proc->resources->descriptor_lock);
                (void)descriptor::get_type(proc->resources->descriptors,
                                           handle,
                                           type);
            }
            if (type == descriptor::kTypeConsole &&
                console_property_requires_settings_access(property) &&
                !require_capability(
                    *proc,
                    capabilities::CapabilityKind::SystemSettings,
                    frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            sync::LockGuard descriptor_guard(
                proc->resources->descriptor_lock);
            int result = descriptor::set_property(
                *proc,
                proc->resources->descriptors,
                handle,
                property,
                frame.rdx,
                frame.r10);
            frame.rax = (result == 0) ? 0 : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::DescriptorWait: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            sync::LockGuard descriptor_guard(
                proc->resources->descriptor_lock);
            int result = descriptor::wait(*proc,
                                          proc->resources->descriptors,
                                          frame.rdi,
                                          static_cast<size_t>(frame.rsi));
            if (result == descriptor::kWouldBlock) {
                frame.rax = static_cast<uint64_t>(
                    static_cast<int64_t>(result));
                return Result::Reschedule;
            }
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(result));
            return Result::Continue;
        }
        case SystemCall::Mount: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            if (!require_capability(
                    *proc,
                    capabilities::CapabilityKind::FilesystemMount,
                    frame) ||
                !require_capability(
                    *proc,
                    capabilities::CapabilityKind::StorageRawRead,
                    frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            fs::BlockDevice device{};
            if (!descriptor::block_device_from_descriptor(
                    *proc,
                    static_cast<uint32_t>(frame.rdi & 0xFFFFFFFFu),
                    device)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            char mount_name[64]{};
            const char* mount_name_ptr = nullptr;
            if (frame.rsi != 0) {
                if (!vm::copy_user_string(
                        proc->cr3,
                        reinterpret_cast<const char*>(frame.rsi),
                        mount_name,
                        sizeof(mount_name))) {
                    frame.rax = static_cast<uint64_t>(-1);
                    return Result::Continue;
                }
                if (mount_name[0] != '\0') {
                    mount_name_ptr = mount_name;
                }
            }

            bool ok = fs::mount_block_device(device, mount_name_ptr);
            frame.rax = ok ? 0 : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::RescanBlockDevices: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            if (!require_capability(
                    *proc,
                    capabilities::CapabilityKind::StorageManage,
                    frame) ||
                !require_capability(
                    *proc,
                    capabilities::CapabilityKind::FilesystemMount,
                    frame) ||
                !require_capability(
                    *proc,
                    capabilities::CapabilityKind::StorageRawRead,
                    frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            size_t mounted = 0;
            bool ok = fs::mount_requested_filesystems(nullptr, nullptr, 0, mounted);
            frame.rax = ok ? 0 : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::FileOpen: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            const char* path = reinterpret_cast<const char*>(frame.rdi);
            int32_t handle = file_io::open_file(*proc, path);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(handle));
            return Result::Continue;
        }
        case SystemCall::FileOpenFlags: {
            process::Task* proc = process::current();
            uint32_t flags = static_cast<uint32_t>(frame.rsi);
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            int32_t handle = file_io::open_file(
                *proc,
                reinterpret_cast<const char*>(frame.rdi),
                flags);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(handle));
            return Result::Continue;
        }
        case SystemCall::FileSeek: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            int64_t result = file_io::seek_file(
                *proc,
                static_cast<uint32_t>(frame.rdi),
                static_cast<int64_t>(frame.rsi),
                static_cast<uint32_t>(frame.rdx));
            frame.rax = static_cast<uint64_t>(result);
            return Result::Continue;
        }
        case SystemCall::FileStat: {
            process::Task* proc = process::current();
            file_io::Metadata metadata{};
            if (proc == nullptr || frame.rsi == 0 ||
                !file_io::stat_file(
                    *proc, static_cast<uint32_t>(frame.rdi), metadata) ||
                !vm::copy_to_user(proc->cr3,
                                  frame.rsi,
                                  &metadata,
                                  sizeof(metadata))) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            frame.rax = 0;
            return Result::Continue;
        }
        case SystemCall::PathStat: {
            process::Task* proc = process::current();
            file_io::Metadata metadata{};
            if (proc == nullptr || frame.rsi == 0 ||
                !file_io::stat_path(
                    *proc, reinterpret_cast<const char*>(frame.rdi), metadata) ||
                !vm::copy_to_user(proc->cr3,
                                  frame.rsi,
                                  &metadata,
                                  sizeof(metadata))) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            frame.rax = 0;
            return Result::Continue;
        }
        case SystemCall::FileClose: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            bool ok =
                file_io::close_file(*proc, static_cast<uint32_t>(frame.rdi));
            frame.rax = ok ? 0 : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::FileSync: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            bool ok =
                file_io::sync_file(*proc, static_cast<uint32_t>(frame.rdi));
            frame.rax = ok ? 0 : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::Sync: {
            process::Task* proc = process::current();
            if (proc != nullptr &&
                !require_capability(*proc,
                                    capabilities::CapabilityKind::StorageManage,
                                    frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            bool ok = fs::block_cache::flush_all();
            frame.rax = ok ? 0 : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::Shutdown: {
            process::Task* proc = process::current();
            if (proc != nullptr &&
                !require_capability(*proc,
                                    capabilities::CapabilityKind::SystemPower,
                                    frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            bool ok = fs::block_cache::flush_all();
            log_message(ok ? LogLevel::Info : LogLevel::Error,
                        ok ? "Shutdown: block cache flushed"
                           : "Shutdown: block cache flush failed");
            if (ok) {
                request_cpu_reset();
            }
            log_message(LogLevel::Info, "Shutdown: halted");
            halt_forever();
        }
        case SystemCall::ModuleLoad: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            if (!require_capability(*proc,
                                    capabilities::CapabilityKind::ModuleLoad,
                                    frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            const char* path_user = reinterpret_cast<const char*>(frame.rdi);
            if (path_user == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            char path_input[path_util::kMaxPathLength];
            if (!vm::copy_user_string(proc->cr3,
                                      path_user,
                                      path_input,
                                      sizeof(path_input)) ||
                path_input[0] == '\0') {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            char resolved[path_util::kMaxPathLength];
            if (!build_process_path(*proc, path_input, resolved)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            if (!kernel_module::load_from_file(resolved)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            driver_registry::probe_pci_drivers();
            frame.rax = 0;
            return Result::Continue;
        }
        case SystemCall::ModuleCount: {
            process::Task* proc = process::current();
            if (proc == nullptr ||
                !require_capability(
                    *proc,
                    capabilities::CapabilityKind::SystemMonitor,
                    frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            frame.rax = static_cast<uint64_t>(kernel_module::loaded_count());
            return Result::Continue;
        }
        case SystemCall::ModuleInfo: {
            process::Task* proc = process::current();
            if (proc == nullptr || frame.rsi == 0 ||
                !require_capability(
                    *proc,
                    capabilities::CapabilityKind::SystemMonitor,
                    frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            kernel_module::ModuleInfo info{};
            if (!kernel_module::info_at(static_cast<size_t>(frame.rdi), info)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            if (!vm::copy_to_user(proc->cr3,
                                  frame.rsi,
                                  &info,
                                  sizeof(info))) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            frame.rax = 0;
            return Result::Continue;
        }
        case SystemCall::FileRead: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            int64_t result =
                file_io::read_file(*proc,
                                   static_cast<uint32_t>(frame.rdi),
                                   frame.rsi,
                                   frame.rdx);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(result));
            return Result::Continue;
        }
        case SystemCall::FileReadAt: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            int64_t result = file_io::read_file_at(
                *proc,
                static_cast<uint32_t>(frame.rdi),
                frame.rsi,
                frame.rdx,
                frame.r10);
            frame.rax = static_cast<uint64_t>(result);
            return Result::Continue;
        }
        case SystemCall::FileWrite: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            int64_t result =
                file_io::write_file(*proc,
                                    static_cast<uint32_t>(frame.rdi),
                                    frame.rsi,
                                    frame.rdx);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(result));
            return Result::Continue;
        }
        case SystemCall::FileCreate: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            const char* path = reinterpret_cast<const char*>(frame.rdi);
            int32_t handle = file_io::create_file(*proc, path);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(handle));
            return Result::Continue;
        }
        case SystemCall::ProcessExec: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            if (!require_capability(*proc, capabilities::CapabilityKind::ProcessSpawn, frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            const char* path_user = reinterpret_cast<const char*>(frame.rdi);
            const char* args = reinterpret_cast<const char*>(frame.rsi);
            uint64_t flags = frame.rdx;
            const char* cwd_user = reinterpret_cast<const char*>(frame.r10);
            capabilities::Principal* child_principal = nullptr;
            if (!prepare_spawn_config(*proc,
                                      frame,
                                      flags,
                                      false,
                                      cwd_user,
                                      nullptr,
                                      child_principal)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            PrincipalRefGuard child_principal_ref(child_principal);

            if (path_user == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            char path_input[path_util::kMaxPathLength];
            if (!vm::copy_user_string(proc->cr3,
                                      path_user,
                                      path_input,
                                      sizeof(path_input)) ||
                path_input[0] == '\0') {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            char resolved_exec[path_util::kMaxPathLength];
            if (!build_process_path(*proc, path_input, resolved_exec)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            process::Task* child = process::allocate();
            if (child == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            child->parent = proc;
            child->waiting_on = nullptr;
            child->exit_code = 0;
            child->has_exited = false;
            child->resources->parent_process_id = proc->pid;
            child->resources->process_group_id =
                proc->resources->process_group_id;
            child->resources->session_id = proc->resources->session_id;
            child->resources->vty_id = proc->resources->vty_id;
            child->resources->principal = child_principal_ref.release();
            char child_cwd_buffer[path_util::kMaxPathLength];
            bool child_cwd_valid = false;
            if (cwd_user != nullptr) {
                char cwd_input[path_util::kMaxPathLength];
                if (vm::copy_user_string(proc->cr3,
                                         cwd_user,
                                         cwd_input,
                                         sizeof(cwd_input)) &&
                    cwd_input[0] != '\0') {
                    child_cwd_valid = build_process_path(*proc, cwd_input, child_cwd_buffer);
                }
            }
            if (!child_cwd_valid) {
                snapshot_cwd(*proc, child_cwd_buffer);
            }
            string_util::copy(child->resources->cwd, sizeof(child->resources->cwd), child_cwd_buffer);
            string_util::copy(child->image_path,
                              sizeof(child->image_path),
                              resolved_exec);

            if (!loader::load_file_into_process(resolved_exec, *child)) {
                process::reclaim(*child);
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            inherit_standard_descriptors(*proc, *child);

            uint64_t arg_ptr = place_args_on_stack(*child, args);

            memset(&child->context, 0, sizeof(child->context));
            child->context.user_rip = child->user_ip;
            child->context.user_rsp = child->user_sp;
            child->context.user_rflags = 0x202;
            child->context.r11 = 0x202;
            child->context.rdi = arg_ptr;
            child->context.rsi =
                flags & ~kProcessSpawnFlagPrincipalConfig;
            child->context.rax = 0;
            child->has_context = true;

            child->preferred_cpu = pick_child_cpu(proc);
            proc->waiting_on = child;
            process::store_state(*proc, process::State::Blocked);
            bool transferred = descriptor::transfer_console_owner(*proc, *child);
            proc->console_transferred = transferred;
            process::store_state(*child, process::State::Ready);
            scheduler::enqueue(child);
            frame.rax = 0;
            return Result::Reschedule;
        }
        case SystemCall::Child: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            if (!require_capability(*proc, capabilities::CapabilityKind::ProcessSpawn, frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            const char* path_user = reinterpret_cast<const char*>(frame.rdi);
            const char* args = reinterpret_cast<const char*>(frame.rsi);
            uint64_t flags = frame.rdx;
            const char* cwd_user = reinterpret_cast<const char*>(frame.r10);
            bool has_stdio_config =
                (flags & kProcessChildFlagStdioConfig) != 0;
            ProcessStdioConfig stdio{};
            capabilities::Principal* child_principal = nullptr;
            if (!prepare_spawn_config(*proc,
                                      frame,
                                      flags,
                                      true,
                                      cwd_user,
                                      &stdio,
                                      child_principal)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            PrincipalRefGuard child_principal_ref(child_principal);

            if (path_user == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            char path_input[path_util::kMaxPathLength];
            if (!vm::copy_user_string(proc->cr3,
                                      path_user,
                                      path_input,
                                      sizeof(path_input)) ||
                path_input[0] == '\0') {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            char resolved_exec[path_util::kMaxPathLength];
            if (!build_process_path(*proc, path_input, resolved_exec)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            process::Task* child = process::allocate();
            if (child == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            child->parent = proc;
            child->waiting_on = nullptr;
            child->exit_code = 0;
            child->has_exited = false;
            child->resources->parent_process_id = proc->pid;
            child->resources->process_group_id =
                proc->resources->process_group_id;
            child->resources->session_id = proc->resources->session_id;
            child->resources->vty_id = proc->resources->vty_id;
            child->resources->principal = child_principal_ref.release();
            char child_cwd_buffer[path_util::kMaxPathLength];
            bool child_cwd_valid = false;
            if (cwd_user != nullptr) {
                char cwd_input[path_util::kMaxPathLength];
                if (vm::copy_user_string(proc->cr3,
                                         cwd_user,
                                         cwd_input,
                                         sizeof(cwd_input)) &&
                    cwd_input[0] != '\0') {
                    child_cwd_valid = build_process_path(*proc, cwd_input, child_cwd_buffer);
                }
            }
            if (!child_cwd_valid) {
                snapshot_cwd(*proc, child_cwd_buffer);
            }
            string_util::copy(child->resources->cwd, sizeof(child->resources->cwd), child_cwd_buffer);
            string_util::copy(child->image_path,
                              sizeof(child->image_path),
                              resolved_exec);

            if (!loader::load_file_into_process(resolved_exec, *child)) {
                process::reclaim(*child);
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            inherit_standard_descriptors(*proc, *child);
            if (has_stdio_config) {
                install_standard_descriptors(*proc, *child, stdio);
            }

            uint64_t arg_ptr = place_args_on_stack(*child, args);

            memset(&child->context, 0, sizeof(child->context));
            child->context.user_rip = child->user_ip;
            child->context.user_rsp = child->user_sp;
            child->context.user_rflags = 0x202;
            child->context.r11 = 0x202;
            child->context.rdi = arg_ptr;
            child->context.rsi =
                flags & ~(kProcessChildFlagStdioConfig |
                          kProcessSpawnFlagPrincipalConfig);
            child->context.rax = 0;
            child->has_context = true;

            child->preferred_cpu = pick_child_cpu(proc);
            process::store_state(*child, process::State::Ready);
            scheduler::enqueue(child);

            frame.rax = static_cast<uint64_t>(child->tid);
            return Result::Continue;
        }
        case SystemCall::ProcessSetCwd: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            const char* path_user = reinterpret_cast<const char*>(frame.rdi);
            if (path_user == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            char path_input[path_util::kMaxPathLength];
            if (!vm::copy_user_string(proc->cr3,
                                      path_user,
                                      path_input,
                                      sizeof(path_input)) ||
                path_input[0] == '\0') {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            char resolved[path_util::kMaxPathLength];
            if (!build_process_path(*proc, path_input, resolved)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            {
                sync::LockGuard guard(proc->resources->lock);
                string_util::copy(proc->resources->cwd,
                                  sizeof(proc->resources->cwd),
                                  resolved);
            }
            frame.rax = 0;
            return Result::Continue;
        }
        case SystemCall::ProcessGetCwd: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            char* buffer = reinterpret_cast<char*>(frame.rdi);
            uint64_t buffer_size = frame.rsi;
            if (buffer == nullptr || buffer_size == 0) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            char cwd[path_util::kMaxPathLength];
            snapshot_cwd(*proc, cwd);
            char user_cwd[path_util::kMaxPathLength];
            if (!path_util::build_user_path(cwd, user_cwd)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            size_t cwd_len = string_util::length(user_cwd);
            if (cwd_len + 1 > buffer_size) {
                if (buffer_size == 0) {
                    frame.rax = static_cast<uint64_t>(-1);
                    return Result::Continue;
                }
                cwd_len = static_cast<size_t>(buffer_size - 1);
            }

            if (!vm::copy_to_user(proc->cr3,
                                  reinterpret_cast<uint64_t>(buffer),
                                  user_cwd,
                                  cwd_len)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            const char terminator = '\0';
            if (!vm::copy_to_user(proc->cr3,
                                  reinterpret_cast<uint64_t>(buffer) + cwd_len,
                                  &terminator,
                                  sizeof(terminator))) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            frame.rax = static_cast<uint64_t>(cwd_len);
            return Result::Continue;
        }
        case SystemCall::DirectoryOpen: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            const char* path = reinterpret_cast<const char*>(frame.rdi);
            int32_t handle = file_io::open_directory(*proc, path);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(handle));
            return Result::Continue;
        }
        case SystemCall::DirectoryRead: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            int64_t result =
                file_io::read_directory(*proc,
                                        static_cast<uint32_t>(frame.rdi),
                                        frame.rsi);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(result));
            return Result::Continue;
        }
        case SystemCall::DirectoryClose: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            bool ok = file_io::close_directory(*proc,
                                               static_cast<uint32_t>(frame.rdi));
            frame.rax = ok ? 0 : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::DirectoryOpenRoot: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            int32_t handle = file_io::open_directory_root(*proc);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(handle));
            return Result::Continue;
        }
        case SystemCall::DirectoryOpenAt: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            uint32_t parent = static_cast<uint32_t>(frame.rdi);
            const char* name = reinterpret_cast<const char*>(frame.rsi);
            int32_t handle = file_io::open_directory_at(*proc, parent, name);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(handle));
            return Result::Continue;
        }
        case SystemCall::FileOpenAt: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            uint32_t parent = static_cast<uint32_t>(frame.rdi);
            const char* name = reinterpret_cast<const char*>(frame.rsi);
            int32_t handle = file_io::open_file_at(*proc, parent, name);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(handle));
            return Result::Continue;
        }
        case SystemCall::FileCreateAt: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            uint32_t parent = static_cast<uint32_t>(frame.rdi);
            const char* name = reinterpret_cast<const char*>(frame.rsi);
            int32_t handle = file_io::create_file_at(*proc, parent, name);
            frame.rax = static_cast<uint64_t>(static_cast<int64_t>(handle));
            return Result::Continue;
        }
        case SystemCall::DirectoryCreate: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            const char* path = reinterpret_cast<const char*>(frame.rdi);
            frame.rax = file_io::create_directory(*proc, path)
                            ? 0
                            : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::FileRemove: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            const char* path = reinterpret_cast<const char*>(frame.rdi);
            frame.rax = file_io::remove_file(*proc, path)
                            ? 0
                            : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::FileGetAcl: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            frame.rax = static_cast<uint64_t>(file_io::get_acl(
                *proc,
                reinterpret_cast<const char*>(frame.rdi),
                frame.rsi,
                frame.rdx));
            return Result::Continue;
        }
        case SystemCall::FileSetAcl: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            frame.rax = file_io::set_acl(
                            *proc,
                            reinterpret_cast<const char*>(frame.rdi),
                            frame.rsi,
                            frame.rdx)
                            ? 0
                            : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::DirectoryRemove: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            const char* path = reinterpret_cast<const char*>(frame.rdi);
            frame.rax = file_io::remove_directory(*proc, path)
                            ? 0
                            : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::TimeGet: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                log_message(LogLevel::Warn,
                            "TimeGet: no current process");
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            auto* out_time =
                reinterpret_cast<NeutrinoWallTime*>(frame.rdi);
            uint64_t out_size = frame.rsi;
            if (out_time == nullptr || out_size < sizeof(NeutrinoWallTime)) {
                log_message(LogLevel::Warn,
                            "TimeGet: invalid output ptr=%llx size=%llu expected=%zu",
                            static_cast<unsigned long long>(frame.rdi),
                            static_cast<unsigned long long>(out_size),
                            sizeof(NeutrinoWallTime));
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            NeutrinoWallTime snapshot{};
            if (!timekeeping::snapshot(snapshot)) {
                log_message(LogLevel::Warn,
                            "TimeGet: wall clock snapshot unavailable");
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            if (!vm::copy_to_user(proc->cr3,
                                  reinterpret_cast<uint64_t>(out_time),
                                  &snapshot,
                                  sizeof(snapshot))) {
                log_message(LogLevel::Warn,
                            "TimeGet: copy_to_user failed pid=%u cr3=%llx ptr=%llx size=%zu",
                            static_cast<unsigned>(proc->tid),
                            static_cast<unsigned long long>(proc->cr3),
                            static_cast<unsigned long long>(frame.rdi),
                            sizeof(snapshot));
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            frame.rax = 0;
            return Result::Continue;
        }
        case SystemCall::ClockGet: {
            if (frame.rdi == 0) {
                frame.rax = timekeeping::nanoseconds_since_boot();
                return Result::Continue;
            }
            if (frame.rdi == 1) {
                NeutrinoWallTime snapshot{};
                if (!timekeeping::snapshot(snapshot) ||
                    snapshot.unix_seconds >
                        (UINT64_MAX - snapshot.nanoseconds) / 1000000000ull) {
                    frame.rax = static_cast<uint64_t>(-1);
                    return Result::Continue;
                }
                frame.rax = snapshot.unix_seconds * 1000000000ull +
                            snapshot.nanoseconds;
                return Result::Continue;
            }
            frame.rax = static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::RandomGet: {
            process::Task* proc = process::current();
            uint64_t user_address = frame.rdi;
            size_t length = static_cast<size_t>(frame.rsi);
            constexpr size_t kMaxRandomRequest = 1024 * 1024;
            if (proc == nullptr || length > kMaxRandomRequest ||
                (length != 0 && user_address == 0)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            if (length != 0 &&
                !vm::validate_user_buffer(proc->cr3,
                                          user_address,
                                          length,
                                          true)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }

            uint8_t buffer[64];
            size_t offset = 0;
            while (offset < length) {
                size_t chunk = length - offset;
                if (chunk > sizeof(buffer)) {
                    chunk = sizeof(buffer);
                }
                if (!kernel_random::secure_fill(buffer, chunk) ||
                    !vm::copy_to_user(proc->cr3,
                                      user_address + offset,
                                      buffer,
                                      chunk)) {
                    memset(buffer, 0, sizeof(buffer));
                    frame.rax = static_cast<uint64_t>(-1);
                    return Result::Continue;
                }
                offset += chunk;
            }
            memset(buffer, 0, sizeof(buffer));
            frame.rax = length;
            return Result::Continue;
        }
        case SystemCall::MapAnonymous: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            size_t length = static_cast<size_t>(frame.rdi);
            uint64_t addr =
                mapping_within_limit(*proc, length)
                    ? vm::map_anonymous(proc->cr3, length, frame.rsi)
                    : 0;
            frame.rax = (addr == 0) ? static_cast<uint64_t>(-1) : addr;
            return Result::Continue;
        }
        case SystemCall::MapAt: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            size_t length = static_cast<size_t>(frame.rsi);
            uint64_t addr =
                mapping_within_limit(*proc, length)
                    ? vm::map_at(
                          proc->cr3, frame.rdi, length, frame.rdx)
                    : 0;
            frame.rax = (addr == 0) ? static_cast<uint64_t>(-1) : addr;
            return Result::Continue;
        }
        case SystemCall::Unmap: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            bool ok = vm::unmap_region(proc->cr3,
                                       frame.rdi,
                                       static_cast<size_t>(frame.rsi));
            frame.rax = ok ? 0 : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::ProtectMemory: {
            process::Task* proc = process::current();
            uint64_t flags = frame.rdx;
            if (proc == nullptr || frame.rsi == 0 ||
                (flags & ~(vm::kMapWrite | vm::kMapExecute)) != 0 ||
                (flags & (vm::kMapWrite | vm::kMapExecute)) ==
                    (vm::kMapWrite | vm::kMapExecute) ||
                !vm::set_user_region_writable(
                    proc->cr3, frame.rdi, frame.rsi,
                    (flags & vm::kMapWrite) != 0) ||
                !vm::set_user_region_executable(
                    proc->cr3, frame.rdi, frame.rsi,
                    (flags & vm::kMapExecute) != 0)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            frame.rax = 0;
            return Result::Continue;
        }
        case SystemCall::MapFilePrivate: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            size_t length = static_cast<size_t>(frame.rdx);
            uint64_t addr =
                mapping_within_limit(*proc, length)
                    ? file_io::map_file_private(
                          *proc,
                          static_cast<uint32_t>(frame.rdi),
                          frame.rsi,
                          length,
                          frame.r10)
                    : 0;
            frame.rax = addr == 0 ? static_cast<uint64_t>(-1) : addr;
            return Result::Continue;
        }
        case SystemCall::ThreadCreate: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            process::Task* thread = process::create_user_thread(
                *proc,
                frame.rdi,
                frame.rsi,
                static_cast<size_t>(frame.rdx),
                frame.r10);
            frame.rax = thread != nullptr
                            ? static_cast<uint64_t>(thread->tid)
                            : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::ThreadExit: {
            frame.rax = frame.rdi & 0xFFFFu;
            return Result::Unschedule;
        }
        case SystemCall::ThreadJoin: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            int64_t result = -1;
            bool blocked = false;
            if (!process::join_thread(*proc,
                                      static_cast<uint32_t>(frame.rdi),
                                      result,
                                      blocked)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            if (blocked) {
                frame.rax = 0;
                return Result::Reschedule;
            }
            frame.rax = static_cast<uint64_t>(result);
            return Result::Continue;
        }
        case SystemCall::ThreadDetach: {
            process::Task* proc = process::current();
            frame.rax = proc != nullptr && process::detach_thread(
                            *proc, static_cast<uint32_t>(frame.rdi))
                            ? 0
                            : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::FutexWait: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            int64_t result = process::futex_wait(
                *proc,
                frame.rdi,
                static_cast<uint32_t>(frame.rsi));
            frame.rax = static_cast<uint64_t>(result);
            return result == 0 ? Result::Reschedule : Result::Continue;
        }
        case SystemCall::FutexWake: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            frame.rax = process::futex_wake(
                *proc,
                frame.rdi,
                static_cast<size_t>(frame.rsi));
            return Result::Continue;
        }
        case SystemCall::FutexWaitTimed: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            int64_t result = process::futex_wait_timed(
                *proc,
                frame.rdi,
                static_cast<uint32_t>(frame.rsi),
                frame.rdx);
            frame.rax = static_cast<uint64_t>(result);
            return result == 0 ? Result::Reschedule : Result::Continue;
        }
        case SystemCall::ThreadId: {
            process::Task* proc = process::current();
            frame.rax = proc != nullptr ? proc->tid : 0;
            return Result::Continue;
        }
        case SystemCall::ProcessId: {
            process::Task* proc = process::current();
            frame.rax = proc != nullptr ? proc->pid : 0;
            return Result::Continue;
        }
        case SystemCall::ThreadSetTls: {
            process::Task* proc = process::current();
            frame.rax =
                proc != nullptr && process::set_thread_tls(*proc, frame.rdi)
                    ? 0
                    : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::ThreadGetTls: {
            process::Task* proc = process::current();
            frame.rax = proc != nullptr ? proc->fs_base : 0;
            return Result::Continue;
        }
        case SystemCall::ProcessEventSend: {
            process::Task* proc = process::current();
            process::Task* target =
                process::find_by_tid(static_cast<uint32_t>(frame.rdi));
            if (proc == nullptr || target == nullptr ||
                (target->pid != proc->pid &&
                 !require_capability(
                     *proc,
                     capabilities::CapabilityKind::ProcessControl,
                     frame))) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            process::ProcessEvent event{
                .type = static_cast<uint32_t>(frame.rsi),
                .sender_process_id = proc->pid,
                .value = frame.rdx,
            };
            frame.rax =
                process::send_event(*proc, target->tid, event)
                    ? 0
                    : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::ProcessEventReceive: {
            process::Task* proc = process::current();
            if (proc == nullptr || frame.rdi == 0) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            process::ProcessEvent event{};
            int64_t result = process::receive_event(
                *proc,
                event,
                (frame.rsi & 1u) != 0);
            if (result == 0 &&
                !vm::copy_to_user(proc->cr3,
                                  frame.rdi,
                                  &event,
                                  sizeof(event))) {
                result = -1;
            }
            frame.rax = static_cast<uint64_t>(result);
            return result == 1 ? Result::Reschedule : Result::Continue;
        }
        case SystemCall::ProcessControl: {
            process::Task* proc = process::current();
            process::Task* target =
                process::find_by_tid(static_cast<uint32_t>(frame.rdi));
            uint32_t action = static_cast<uint32_t>(frame.rsi);
            if (proc == nullptr || target == nullptr ||
                (target->pid != proc->pid &&
                 !require_capability(
                     *proc,
                     capabilities::CapabilityKind::ProcessControl,
                     frame)) ||
                !process::control_group(
                    *proc,
                    target->tid,
                    action,
                    static_cast<uint16_t>(frame.rdx))) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            frame.rax = 0;
            return target->pid == proc->pid &&
                           (action == 1 || action == 2)
                       ? Result::Reschedule
                       : Result::Continue;
        }
        case SystemCall::ProcessWaitChild: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            int64_t result = process::wait_child(
                *proc,
                static_cast<uint32_t>(frame.rdi),
                (frame.rsi & 1u) != 0);
            frame.rax = static_cast<uint64_t>(result);
            return result == 0 ? Result::Reschedule : Result::Continue;
        }
        case SystemCall::ProcessSetGroup: {
            process::Task* proc = process::current();
            process::Task* target =
                frame.rdi == 0
                    ? proc
                    : process::find_by_tid(
                          static_cast<uint32_t>(frame.rdi));
            bool own_child =
                proc != nullptr && target != nullptr &&
                target->resources != nullptr &&
                target->resources->parent_process_id == proc->pid;
            if (proc == nullptr || target == nullptr ||
                (target->pid != proc->pid && !own_child &&
                 !require_capability(
                     *proc,
                     capabilities::CapabilityKind::ProcessControl,
                     frame))) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            frame.rax = process::set_process_group(
                            *proc,
                            target->tid,
                            static_cast<uint32_t>(frame.rsi))
                            ? 0
                            : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::ProcessGetGroup:
        case SystemCall::ProcessGetSession: {
            SystemCall operation = static_cast<SystemCall>(frame.rax);
            process::Task* proc = process::current();
            process::Task* target =
                frame.rdi == 0
                    ? proc
                    : process::find_by_tid(
                          static_cast<uint32_t>(frame.rdi));
            if (proc == nullptr || target == nullptr ||
                (target->pid != proc->pid &&
                 !require_capability(
                     *proc,
                     capabilities::CapabilityKind::ProcessInspect,
                     frame))) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            frame.rax =
                operation == SystemCall::ProcessGetGroup
                    ? process::process_group_id(*target)
                    : process::session_id(*target);
            return Result::Continue;
        }
        case SystemCall::ProcessCreateSession: {
            process::Task* proc = process::current();
            frame.rax =
                proc != nullptr && process::create_session(*proc)
                    ? static_cast<uint64_t>(proc->pid)
                    : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::ProcessSetForeground: {
            process::Task* proc = process::current();
            frame.rax =
                proc != nullptr &&
                        process::set_foreground_group(
                            *proc,
                            static_cast<uint32_t>(frame.rdi))
                    ? 0
                    : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::ProcessGetForeground: {
            process::Task* proc = process::current();
            frame.rax =
                proc != nullptr
                    ? process::foreground_group(*proc)
                    : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::ProcessSetLimits:
        case SystemCall::ProcessGetLimits:
        case SystemCall::ProcessGetUsage: {
            process::Task* proc = process::current();
            process::Task* target =
                frame.rdi == 0
                    ? proc
                    : process::find_by_tid(
                          static_cast<uint32_t>(frame.rdi));
            SystemCall operation = static_cast<SystemCall>(frame.rax);
            bool external =
                proc != nullptr && target != nullptr &&
                target->pid != proc->pid;
            capabilities::CapabilityKind permission =
                operation == SystemCall::ProcessSetLimits
                    ? capabilities::CapabilityKind::ProcessControl
                    : capabilities::CapabilityKind::ProcessInspect;
            if (proc == nullptr || target == nullptr || frame.rsi == 0 ||
                (external &&
                 !require_capability(*proc, permission, frame))) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            if (operation == SystemCall::ProcessSetLimits) {
                process::ProcessLimits requested{};
                process::ProcessLimits current{};
                if (!vm::copy_from_user(proc->cr3,
                                        &requested,
                                        frame.rsi,
                                        sizeof(requested)) ||
                    !process::get_limits(*target, current) ||
                    (!external &&
                     (requested.max_threads > current.max_threads ||
                      requested.max_descriptors >
                          current.max_descriptors ||
                      requested.max_file_handles >
                          current.max_file_handles ||
                      requested.max_directory_handles >
                          current.max_directory_handles ||
                      requested.max_virtual_bytes >
                          current.max_virtual_bytes ||
                      requested.max_cpu_ticks >
                          current.max_cpu_ticks)) ||
                    !process::set_limits(*target, requested)) {
                    frame.rax = static_cast<uint64_t>(-1);
                } else {
                    frame.rax = 0;
                }
                return Result::Continue;
            }
            if (operation == SystemCall::ProcessGetLimits) {
                process::ProcessLimits limits{};
                frame.rax =
                    process::get_limits(*target, limits) &&
                            vm::copy_to_user(proc->cr3,
                                             frame.rsi,
                                             &limits,
                                             sizeof(limits))
                        ? 0
                        : static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            process::ProcessUsage usage{};
            frame.rax =
                process::get_usage(*target, usage) &&
                        vm::copy_to_user(proc->cr3,
                                         frame.rsi,
                                         &usage,
                                         sizeof(usage))
                    ? 0
                    : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::ProcessTrace: {
            process::Task* proc = process::current();
            if (proc == nullptr ||
                !require_capability(
                    *proc,
                    capabilities::CapabilityKind::ProcessTrace,
                    frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            uint32_t target = static_cast<uint32_t>(frame.rdi);
            uint32_t operation = static_cast<uint32_t>(frame.rsi);
            bool ok = false;
            switch (operation) {
                case 1:
                    ok = process::trace_attach(*proc, target);
                    break;
                case 2:
                    ok = process::trace_detach(*proc, target);
                    break;
                case 3:
                    ok = process::trace_read_memory(
                        *proc, target, frame.rdx, frame.r10, frame.r8);
                    break;
                case 4:
                    ok = process::trace_write_memory(
                        *proc, target, frame.rdx, frame.r10, frame.r8);
                    break;
                case 5:
                    ok = process::trace_get_registers(
                        *proc, target, frame.r10, frame.r8);
                    break;
                case 6:
                    ok = process::trace_stopped(*proc, target);
                    break;
                default:
                    break;
            }
            frame.rax = ok ? 0 : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::ChangeSlot: {
            process::Task* proc = process::current();
            if (proc == nullptr ||
                !require_capability(
                    *proc,
                    capabilities::CapabilityKind::GraphicalSession,
                    frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            uint32_t slot = static_cast<uint32_t>(frame.rdi);
            frame.rax = descriptor::framebuffer_activate_for_process(*proc,
                                                                      slot)
                            ? 0
                            : static_cast<uint64_t>(-1);
            return Result::Continue;
        }
        case SystemCall::PrincipalCreate: {
            process::Task* proc = process::current();
            if (proc == nullptr ||
                !require_capability(*proc,
                                    capabilities::CapabilityKind::IdentityManage,
                                    frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            users::User* backing_user = users::from_handle(frame.rdi);
            if (frame.rdi != 0 && backing_user == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            uint64_t allowed_mask = frame.rsi;
            capabilities::Principal* principal =
                capabilities::create_principal(backing_user, allowed_mask);
            frame.rax = capabilities::principal_handle(principal);
            return Result::Continue;
        }
        case SystemCall::PrincipalSet: {
            process::Task* proc = process::current();
            if (proc == nullptr ||
                !require_capability(*proc,
                                    capabilities::CapabilityKind::IdentityManage,
                                    frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            auto* principal =
                capabilities::principal_acquire_from_handle(frame.rdi);
            if (principal == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            sync::LockGuard resource_guard(proc->resources->lock);
            if (proc->resources->principal != nullptr) {
                capabilities::principal_release(proc->resources->principal);
            }
            proc->resources->principal = principal;
            capabilities::cap_table_clear(proc->resources->cap_handles,
                                          capabilities::kMaxProcessCapabilities);
            frame.rax = 0;
            return Result::Continue;
        }
        case SystemCall::CapabilityGrant: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            capabilities::CapabilityKind kind{};
            if (!capabilities::capability_from_value(frame.rdi, kind)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            sync::LockGuard resource_guard(proc->resources->lock);
            if (proc->resources->principal == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            capabilities::CapabilityToken* token =
                capabilities::issue_token(*proc->resources->principal, kind);
            if (token == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            uint64_t handle = 0;
            if (!capabilities::cap_table_insert(proc->resources->cap_handles,
                                                capabilities::kMaxProcessCapabilities,
                                                token,
                                                handle)) {
                capabilities::discard_unreferenced_token(token);
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            frame.rax = handle;
            return Result::Continue;
        }
        case SystemCall::CapabilityPass: {
            process::Task* proc = process::current();
            if (proc == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            process::Task* child = process::find_by_tid(static_cast<uint32_t>(frame.rdi));
            const uint64_t* user_handles = reinterpret_cast<const uint64_t*>(frame.rsi);
            size_t handle_count = static_cast<size_t>(frame.rdx & 0xFFFF);
            constexpr size_t kMaxHandles = 8;
            if (handle_count > kMaxHandles) {
                handle_count = kMaxHandles;
            }
            if (child == nullptr || !child->has_context) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            bool own_child = child->resources != nullptr &&
                             child->resources->parent_process_id == proc->pid;
            if (!own_child &&
                !require_capability(
                    *proc,
                    capabilities::CapabilityKind::ProcessControl,
                    frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            ResourcePairGuard resource_guard(*proc->resources,
                                             *child->resources);
            uint64_t local_handles[kMaxHandles];
            if (handle_count > 0) {
                if (!vm::copy_from_user(proc->cr3,
                                        local_handles,
                                        reinterpret_cast<uint64_t>(user_handles),
                                        handle_count * sizeof(uint64_t))) {
                    frame.rax = static_cast<uint64_t>(-1);
                    return Result::Continue;
                }
                if (!capabilities::cap_table_copy_handles(
                        child->resources->cap_handles,
                        capabilities::kMaxProcessCapabilities,
                        proc->resources->cap_handles,
                        capabilities::kMaxProcessCapabilities,
                        local_handles,
                        handle_count)) {
                    frame.rax = static_cast<uint64_t>(-1);
                    return Result::Continue;
                }
            }
            frame.rax = 0;
            return Result::Continue;
        }
        case SystemCall::UserCreate: {
            process::Task* proc = process::current();
            if (proc == nullptr ||
                !require_capability(*proc,
                                    capabilities::CapabilityKind::IdentityManage,
                                    frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            char name[32];
            if (!vm::copy_user_string(proc->cr3,
                                      reinterpret_cast<const char*>(frame.rdi),
                                      name,
                                      sizeof(name))) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            uint64_t caps = frame.rsi;
            users::User* user = users::create(name, caps);
            frame.rax = users::handle_for(user);
            return Result::Continue;
        }
        case SystemCall::UserFind: {
            process::Task* proc = process::current();
            if (proc == nullptr ||
                !require_capability(*proc,
                                    capabilities::CapabilityKind::IdentityManage,
                                    frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            char name[32];
            if (!vm::copy_user_string(proc->cr3,
                                      reinterpret_cast<const char*>(frame.rdi),
                                      name,
                                      sizeof(name))) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            users::User* user = users::find(name);
            frame.rax = users::handle_for(user);
            return Result::Continue;
        }
        case SystemCall::UserBumpGeneration: {
            process::Task* proc = process::current();
            if (proc == nullptr ||
                !require_capability(*proc,
                                    capabilities::CapabilityKind::IdentityManage,
                                    frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            auto* user = users::from_handle(frame.rdi);
            if (user != nullptr) {
                users::bump_generation(*user);
                frame.rax = 0;
            } else {
                frame.rax = static_cast<uint64_t>(-1);
            }
            return Result::Continue;
        }
        case SystemCall::UserSetPassword: {
            process::Task* proc = process::current();
            if (proc == nullptr ||
                !require_capability(*proc,
                                    capabilities::CapabilityKind::IdentityManage,
                                    frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            auto* user = users::from_handle(frame.rdi);
            const auto* user_salt = reinterpret_cast<const uint8_t*>(frame.rsi);
            const auto* user_hash = reinterpret_cast<const uint8_t*>(frame.rdx);
            uint32_t iterations = static_cast<uint32_t>(frame.r10);
            uint8_t salt[16];
            uint8_t hash[32];
            if (user == nullptr ||
                !vm::copy_from_user(proc->cr3,
                                    salt,
                                    reinterpret_cast<uint64_t>(user_salt),
                                    sizeof(salt)) ||
                !vm::copy_from_user(proc->cr3,
                                    hash,
                                    reinterpret_cast<uint64_t>(user_hash),
                                    sizeof(hash)) ||
                !users::set_password(*user, salt, hash, iterations)) {
                frame.rax = static_cast<uint64_t>(-1);
            } else {
                frame.rax = 0;
            }
            return Result::Continue;
        }
        case SystemCall::UserInfo: {
            process::Task* proc = process::current();
            if (proc == nullptr ||
                !require_capability(*proc,
                                    capabilities::CapabilityKind::IdentityManage,
                                    frame)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            auto* user = users::from_handle(frame.rdi);
            auto* user_info = reinterpret_cast<syscall::UserInfo*>(frame.rsi);
            if (user == nullptr || user_info == nullptr) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            users::UserInfo snapshot{};
            if (!users::snapshot_info(*user, snapshot)) {
                frame.rax = static_cast<uint64_t>(-1);
                return Result::Continue;
            }
            syscall::UserInfo info{};
            info.id_machine = snapshot.id_machine;
            info.id_local = snapshot.id_local;
            for (size_t i = 0; i < sizeof(info.name); ++i) {
                info.name[i] = snapshot.name[i];
                if (snapshot.name[i] == '\0') {
                    break;
                }
            }
            info.allowed_caps = snapshot.allowed_caps;
            info.generation = snapshot.generation;
            info.password_set = snapshot.password_set;
            info.active = snapshot.active;
            if (!vm::copy_to_user(proc->cr3,
                                  reinterpret_cast<uint64_t>(user_info),
                                  &info,
                                  sizeof(info))) {
                frame.rax = static_cast<uint64_t>(-1);
            } else {
                frame.rax = 0;
            }
            return Result::Continue;
        }
        default: {
            log_message(LogLevel::Warn, "Unhandled syscall %llx", frame.rax);
            frame.rax = static_cast<uint64_t>(-1);
            return Result::Continue;
        }
    }
}

}  // namespace syscall
