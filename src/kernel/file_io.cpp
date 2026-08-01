#include "file_io.hpp"

#include <limits.h>
#include <stddef.h>

#include "drivers/log/logging.hpp"
#include "drivers/fs/block_cache.hpp"
#include "fs/vfs.hpp"
#include "lib/mem.hpp"
#include "capabilities.hpp"
#include "path_util.hpp"
#include "string_util.hpp"
#include "sync.hpp"
#include "vm.hpp"

namespace {

// Match the largest filesystem cluster so sequential writes reach the VFS as
// full-cluster operations instead of repeated partial-cluster updates.
constexpr size_t kFileIoBounceSize = 32768;
alignas(4096) uint8_t g_file_io_bounce[kFileIoBounceSize];
volatile int g_file_io_bounce_lock = 0;

class PrincipalSnapshot {
public:
    explicit PrincipalSnapshot(const process::Task& proc) {
        sync::LockGuard guard(proc.resources->lock);
        principal_ = proc.resources->principal;
        valid_ = principal_ == nullptr ||
                 capabilities::principal_add_ref(principal_);
    }
    ~PrincipalSnapshot() {
        if (principal_ != nullptr && valid_) {
            capabilities::principal_release(principal_);
        }
    }
    bool valid() const { return valid_; }
    capabilities::Principal* get() const {
        return valid_ ? principal_ : nullptr;
    }
private:
    capabilities::Principal* principal_{nullptr};
    bool valid_{false};
};

void lock_file_io_bounce() {
    while (__atomic_test_and_set(&g_file_io_bounce_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock_file_io_bounce() {
    __atomic_clear(&g_file_io_bounce_lock, __ATOMIC_RELEASE);
}

process::FileHandle* get_file_handle(process::Task& proc, uint32_t handle) {
    if (handle >= process::kMaxFileHandles) {
        return nullptr;
    }
    process::FileHandle& entry = proc.resources->file_handles[handle];
    return __atomic_load_n(&entry.in_use, __ATOMIC_ACQUIRE) ? &entry : nullptr;
}

process::DirectoryHandle* get_directory_handle(process::Task& proc,
                                               uint32_t handle) {
    if (handle >= process::kMaxDirectoryHandles) {
        return nullptr;
    }
    process::DirectoryHandle& entry =
        proc.resources->directory_handles[handle];
    return __atomic_load_n(&entry.in_use, __ATOMIC_ACQUIRE) ? &entry : nullptr;
}

int32_t allocate_file_handle(process::Task& proc) {
    sync::LockGuard guard(proc.resources->lock);
    uint32_t limit = proc.resources->limits.max_file_handles;
    if (limit > process::kMaxFileHandles) {
        limit = process::kMaxFileHandles;
    }
    for (uint32_t i = 0; i < limit; ++i) {
        sync::LockGuard slot_guard(proc.resources->file_locks[i]);
        process::FileHandle& handle = proc.resources->file_handles[i];
        if (!__atomic_load_n(&handle.in_use, __ATOMIC_ACQUIRE) &&
            !handle.reserved) {
            handle.reserved = true;
            proc.resources->file_handles[i].can_read = false;
            proc.resources->file_handles[i].can_write = false;
            proc.resources->file_handles[i].append = false;
            proc.resources->file_handles[i].handle = {};
            proc.resources->file_handles[i].position = 0;
            proc.resources->file_handles[i].path[0] = '\0';
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

int32_t allocate_directory_handle(process::Task& proc) {
    sync::LockGuard guard(proc.resources->lock);
    uint32_t limit = proc.resources->limits.max_directory_handles;
    if (limit > process::kMaxDirectoryHandles) {
        limit = process::kMaxDirectoryHandles;
    }
    for (uint32_t i = 0; i < limit; ++i) {
        sync::LockGuard slot_guard(proc.resources->directory_locks[i]);
        process::DirectoryHandle& handle =
            proc.resources->directory_handles[i];
        if (!__atomic_load_n(&handle.in_use, __ATOMIC_ACQUIRE) &&
            !handle.reserved) {
            handle.reserved = true;
            proc.resources->directory_handles[i].handle = {};
            proc.resources->directory_handles[i].path[0] = '\0';
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

void release_file_reservation(process::Task& proc, size_t slot) {
    sync::LockGuard guard(proc.resources->lock);
    proc.resources->file_handles[slot].reserved = false;
}

void publish_file_handle(process::Task& proc, size_t slot) {
    sync::LockGuard guard(proc.resources->lock);
    process::FileHandle& handle = proc.resources->file_handles[slot];
    handle.reserved = false;
    __atomic_store_n(&handle.in_use, true, __ATOMIC_RELEASE);
}

void publish_directory_handle(process::Task& proc, size_t slot) {
    sync::LockGuard guard(proc.resources->lock);
    process::DirectoryHandle& handle =
        proc.resources->directory_handles[slot];
    handle.reserved = false;
    __atomic_store_n(&handle.in_use, true, __ATOMIC_RELEASE);
}

bool copy_path(process::Task& proc,
               const char* user_path,
               char (&out)[path_util::kMaxPathLength]) {
    if (user_path == nullptr) {
        return false;
    }

    char temp[path_util::kMaxPathLength];
    if (!vm::copy_user_string(proc.cr3,
                              user_path,
                              temp,
                              sizeof(temp)) ||
        temp[0] == '\0') {
        return false;
    }
    char cwd[sizeof(proc.resources->cwd)];
    {
        sync::LockGuard guard(proc.resources->lock);
        string_util::copy(cwd, sizeof(cwd), proc.resources->cwd);
    }
    return path_util::build_absolute_path(cwd, temp, out);
}

bool build_child_path(process::Task& proc,
                      const char* base,
                      const char* name,
                      char (&out)[path_util::kMaxPathLength]) {
    if (base == nullptr || name == nullptr) {
        return false;
    }
    char local_name[path_util::kMaxPathLength];
    if (!vm::copy_user_string(proc.cr3,
                              name,
                              local_name,
                              sizeof(local_name)) ||
        local_name[0] == '\0') {
        return false;
    }
    if (string_util::contains(local_name, '/')) {
        return false;
    }
    return path_util::build_absolute_path(base, local_name, out);
}

vfs::AclValue permission_value(const vfs::AclEntry& entry,
                               vfs::AclPermission permission) {
    switch (permission) {
        case vfs::AclPermission::Read: return entry.read;
        case vfs::AclPermission::Write: return entry.write;
        case vfs::AclPermission::Delete: return entry.delete_permission;
        case vfs::AclPermission::Edit: return entry.edit;
    }
    return vfs::AclValue::Deny;
}

bool acl_allows(const process::Task& proc,
                const char* path,
                vfs::AclPermission permission) {
    PrincipalSnapshot principal(proc);
    if (!principal.valid()) {
        return false;
    }
    if (principal.get() == nullptr ||
        capabilities::principal_allows(
            *principal.get(),
            capabilities::CapabilityKind::SecurityManage)) {
        return true;
    }
    if (!vfs::acl_supported(path)) {
        return true;
    }

    vfs::AclEntry entries[vfs::kMaxAclEntries]{};
    size_t count = 0;
    if (!vfs::get_acl(path, entries, vfs::kMaxAclEntries, count)) {
        return false;
    }
    if (count == 0) {
        return true;
    }

    uint64_t machine_id = 0;
    uint64_t local_id = 0;
    if (!capabilities::principal_user_id(principal.get(),
                                         machine_id,
                                         local_id)) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        const vfs::AclEntry& entry = entries[i];
        if (entry.machine_id != machine_id || entry.local_id != local_id) {
            continue;
        }
        vfs::AclValue value = permission_value(entry, permission);
        return value == vfs::AclValue::Allow;
    }
    return false;
}

bool acl_check_required(capabilities::Principal* principal) {
    return principal != nullptr &&
           !capabilities::principal_allows(
               *principal,
               capabilities::CapabilityKind::SecurityManage);
}

struct AclDecision {
    bool read;
    bool write;
    bool delete_permission;
    bool edit;
};

AclDecision acl_snapshot_decision(capabilities::Principal* principal,
                                  const vfs::AclSnapshot& acl) {
    if (!acl.supported || acl.count == 0) {
        return {true, true, true, true};
    }
    uint64_t machine_id = 0;
    uint64_t local_id = 0;
    if (!capabilities::principal_user_id(principal,
                                         machine_id,
                                         local_id)) {
        return {};
    }
    for (size_t i = 0; i < acl.count; ++i) {
        const vfs::AclEntry& entry = acl.entries[i];
        if (entry.machine_id == machine_id && entry.local_id == local_id) {
            return {
                entry.read == vfs::AclValue::Allow,
                entry.write == vfs::AclValue::Allow,
                entry.delete_permission == vfs::AclValue::Allow,
                entry.edit == vfs::AclValue::Allow,
            };
        }
    }
    return {};
}

bool parent_path(const char* path,
                 char (&out)[path_util::kMaxPathLength]) {
    size_t length = string_util::length(path);
    while (length > 1 && path[length - 1] == '/') {
        --length;
    }
    size_t slash = length;
    while (slash > 0 && path[slash - 1] != '/') {
        --slash;
    }
    size_t parent_length = slash > 1 ? slash - 1 : 1;
    if (parent_length >= sizeof(out)) {
        return false;
    }
    memcpy(out, path, parent_length);
    out[parent_length] = '\0';
    return true;
}

}  // namespace

namespace file_io {

static int32_t open_file_impl(process::Task& proc,
                              const char* path,
                              uint32_t flags,
                              bool legacy_write_if_allowed);

int32_t open_file(process::Task& proc, const char* path) {
    return open_file_impl(proc, path, OpenRead, true);
}

int32_t open_file(process::Task& proc, const char* path, uint32_t flags) {
    return open_file_impl(proc, path, flags, false);
}

static int32_t open_file_impl(process::Task& proc,
                              const char* path,
                              uint32_t flags,
                              bool legacy_write_if_allowed) {
    constexpr uint32_t kKnownFlags =
        OpenRead | OpenWrite | OpenCreate | OpenExclusive | OpenAppend;
    if ((flags & ~kKnownFlags) != 0 ||
        (flags & (OpenRead | OpenWrite)) == 0 ||
        ((flags & OpenExclusive) != 0 && (flags & OpenCreate) == 0) ||
        ((flags & OpenAppend) != 0 && (flags & OpenWrite) == 0)) {
        return -1;
    }
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return -1;
    }
    int32_t slot = allocate_file_handle(proc);
    if (slot < 0) {
        log_message(LogLevel::Warn,
                    "FileIO: no free file handles for process %u",
                    static_cast<unsigned int>(proc.pid));
        return -1;
    }

    vfs::FileHandle vfs_handle{};
    vfs::AclSnapshot acl{};
    PrincipalSnapshot principal(proc);
    if (!principal.valid()) {
        release_file_reservation(proc, static_cast<size_t>(slot));
        return -1;
    }
    bool check_acl = acl_check_required(principal.get());
    bool opened = false;
    if ((flags & OpenExclusive) == 0) {
        opened = vfs::open_file(local_path,
                                vfs_handle,
                                check_acl ? &acl : nullptr);
    }
    if (!opened && (flags & OpenCreate) != 0) {
        char parent[path_util::kMaxPathLength];
        if (parent_path(local_path, parent) &&
            acl_allows(proc, parent, vfs::AclPermission::Write)) {
            opened = vfs::create_file(local_path, vfs_handle);
            if (opened) {
                acl = {};
            }
        }
    }
    if (!opened) {
        vfs::close_file(vfs_handle);
        proc.resources->file_handles[static_cast<size_t>(slot)].handle = {};
        release_file_reservation(proc, static_cast<size_t>(slot));
        return -1;
    }
    AclDecision decision = check_acl
                               ? acl_snapshot_decision(principal.get(), acl)
                               : AclDecision{true, true, true, true};
    if (((flags & OpenRead) != 0 && !decision.read) ||
        ((flags & OpenWrite) != 0 && !decision.write)) {
        vfs::close_file(vfs_handle);
        proc.resources->file_handles[static_cast<size_t>(slot)].handle = {};
        release_file_reservation(proc, static_cast<size_t>(slot));
        return -1;
    }

    process::FileHandle& handle = proc.resources->file_handles[static_cast<size_t>(slot)];
    handle.handle = vfs_handle;
    handle.can_read = (flags & OpenRead) != 0;
    handle.can_write = legacy_write_if_allowed
                           ? decision.write
                           : (flags & OpenWrite) != 0;
    handle.append = (flags & OpenAppend) != 0;
    handle.position = handle.append ? handle.handle.size : 0;
    string_util::copy(handle.path, sizeof(handle.path), local_path);
    publish_file_handle(proc, static_cast<size_t>(slot));
    return slot;
}

int32_t create_file(process::Task& proc, const char* path) {
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return -1;
    }
    char parent[path_util::kMaxPathLength];
    if (!parent_path(local_path, parent) ||
        !acl_allows(proc, parent, vfs::AclPermission::Write)) {
        return -1;
    }

    int32_t slot = allocate_file_handle(proc);
    if (slot < 0) {
        log_message(LogLevel::Warn,
                    "FileIO: no free file handles for process %u",
                    static_cast<unsigned int>(proc.pid));
        return -1;
    }

    vfs::FileHandle vfs_handle{};
    if (!vfs::create_file(local_path, vfs_handle)) {
        proc.resources->file_handles[static_cast<size_t>(slot)].handle = {};
        release_file_reservation(proc, static_cast<size_t>(slot));
        return -1;
    }

    process::FileHandle& handle = proc.resources->file_handles[static_cast<size_t>(slot)];
    handle.handle = vfs_handle;
    handle.can_read = true;
    handle.can_write = true;
    handle.append = false;
    handle.position = 0;
    string_util::copy(handle.path, sizeof(handle.path), local_path);
    publish_file_handle(proc, static_cast<size_t>(slot));
    return slot;
}

bool close_file(process::Task& proc, uint32_t handle) {
    if (handle >= process::kMaxFileHandles) {
        return false;
    }
    sync::LockGuard guard(proc.resources->file_locks[handle]);
    process::FileHandle* entry = get_file_handle(proc, handle);
    if (entry == nullptr) {
        return false;
    }
    vfs::close_file(entry->handle);
    __atomic_store_n(&entry->in_use, false, __ATOMIC_RELEASE);
    entry->can_write = false;
    entry->can_read = false;
    entry->append = false;
    entry->handle = {};
    entry->position = 0;
    entry->path[0] = '\0';
    return true;
}

bool sync_file(process::Task& proc, uint32_t handle) {
    if (handle >= process::kMaxFileHandles) {
        return false;
    }
    sync::LockGuard guard(proc.resources->file_locks[handle]);
    process::FileHandle* entry = get_file_handle(proc, handle);
    if (entry == nullptr) {
        return false;
    }
    return fs::block_cache::flush_all();
}

int64_t read_file(process::Task& proc, uint32_t handle, uint64_t user_addr,
                  uint64_t length) {
    if (handle >= process::kMaxFileHandles) {
        return -1;
    }
    sync::LockGuard guard(proc.resources->file_locks[handle]);
    process::FileHandle* entry = get_file_handle(proc, handle);
    if (entry == nullptr || !entry->can_read) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    size_t requested =
        (length > static_cast<uint64_t>(SIZE_MAX))
            ? static_cast<size_t>(SIZE_MAX)
            : static_cast<size_t>(length);
    if (user_addr == 0) {
        return -1;
    }

    size_t total_read = 0;
    while (total_read < requested) {
        size_t chunk = requested - total_read;
        if (chunk > kFileIoBounceSize) {
            chunk = kFileIoBounceSize;
        }

        size_t out_size = 0;
        lock_file_io_bounce();
        if (!vfs::read_file(entry->handle,
                            entry->position + total_read,
                            g_file_io_bounce,
                            chunk,
                            out_size)) {
            unlock_file_io_bounce();
            return total_read == 0 ? -1 : static_cast<int64_t>(total_read);
        }
        if (out_size == 0) {
            unlock_file_io_bounce();
            break;
        }
        if (!vm::copy_to_user(proc.cr3,
                              user_addr + total_read,
                              g_file_io_bounce,
                              out_size)) {
            unlock_file_io_bounce();
            return total_read == 0 ? -1 : static_cast<int64_t>(total_read);
        }
        unlock_file_io_bounce();
        total_read += out_size;
        if (out_size < chunk) {
            break;
        }
    }

    entry->position += static_cast<uint64_t>(total_read);
    return static_cast<int64_t>(total_read);
}

int64_t read_file_at(process::Task& proc, uint32_t handle,
                     uint64_t user_addr, uint64_t length, uint64_t offset) {
    if (handle >= process::kMaxFileHandles) {
        return -1;
    }
    sync::LockGuard guard(proc.resources->file_locks[handle]);
    process::FileHandle* entry = get_file_handle(proc, handle);
    if (entry == nullptr || !entry->can_read ||
        (length != 0 && user_addr == 0)) {
        return -1;
    }
    size_t requested = length > static_cast<uint64_t>(SIZE_MAX)
                           ? static_cast<size_t>(SIZE_MAX)
                           : static_cast<size_t>(length);
    size_t total_read = 0;
    while (total_read < requested) {
        if (offset > UINT64_MAX - total_read) {
            return total_read == 0 ? -1 : static_cast<int64_t>(total_read);
        }
        size_t chunk = requested - total_read;
        if (chunk > kFileIoBounceSize) {
            chunk = kFileIoBounceSize;
        }
        size_t out_size = 0;
        lock_file_io_bounce();
        bool ok = vfs::read_file(entry->handle,
                                 offset + total_read,
                                 g_file_io_bounce,
                                 chunk,
                                 out_size) &&
                  vm::copy_to_user(proc.cr3,
                                   user_addr + total_read,
                                   g_file_io_bounce,
                                   out_size);
        unlock_file_io_bounce();
        if (!ok) {
            return total_read == 0 ? -1 : static_cast<int64_t>(total_read);
        }
        total_read += out_size;
        if (out_size < chunk) {
            break;
        }
    }
    return static_cast<int64_t>(total_read);
}

int64_t write_file(process::Task& proc, uint32_t handle, uint64_t user_addr,
                   uint64_t length) {
    if (handle >= process::kMaxFileHandles) {
        return -1;
    }
    sync::LockGuard guard(proc.resources->file_locks[handle]);
    process::FileHandle* entry = get_file_handle(proc, handle);
    if (entry == nullptr) {
        return -1;
    }
    if (!entry->can_write) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }

    size_t requested =
        (length > static_cast<uint64_t>(SIZE_MAX))
            ? static_cast<size_t>(SIZE_MAX)
            : static_cast<size_t>(length);
    if (user_addr == 0) {
        return -1;
    }

    if (entry->append) {
        entry->position = entry->handle.size;
    }
    size_t total_written = 0;
    while (total_written < requested) {
        size_t chunk = requested - total_written;
        if (chunk > kFileIoBounceSize) {
            chunk = kFileIoBounceSize;
        }

        lock_file_io_bounce();
        if (!vm::copy_from_user(proc.cr3,
                                g_file_io_bounce,
                                user_addr + total_written,
                                chunk)) {
            unlock_file_io_bounce();
            return total_written == 0 ? -1 : static_cast<int64_t>(total_written);
        }

        size_t out_size = 0;
        if (!vfs::write_file(entry->handle,
                             entry->position + total_written,
                             g_file_io_bounce,
                             chunk,
                             out_size)) {
            unlock_file_io_bounce();
            return total_written == 0 ? -1 : static_cast<int64_t>(total_written);
        }
        if (out_size == 0) {
            unlock_file_io_bounce();
            break;
        }
        unlock_file_io_bounce();
        total_written += out_size;
        if (out_size < chunk) {
            break;
        }
    }

    entry->position += static_cast<uint64_t>(total_written);
    return static_cast<int64_t>(total_written);
}

int64_t seek_file(process::Task& proc, uint32_t handle,
                  int64_t offset, uint32_t whence) {
    if (handle >= process::kMaxFileHandles || whence > 2) {
        return -1;
    }
    sync::LockGuard guard(proc.resources->file_locks[handle]);
    process::FileHandle* entry = get_file_handle(proc, handle);
    if (entry == nullptr) {
        return -1;
    }
    uint64_t base = whence == 0 ? 0 :
                    whence == 1 ? entry->position : entry->handle.size;
    uint64_t new_position = 0;
    if (offset < 0) {
        uint64_t magnitude = static_cast<uint64_t>(-(offset + 1)) + 1;
        if (magnitude > base) {
            return -1;
        }
        new_position = base - magnitude;
    } else {
        uint64_t positive = static_cast<uint64_t>(offset);
        if (positive > UINT64_MAX - base || base + positive > INT64_MAX) {
            return -1;
        }
        new_position = base + positive;
    }
    entry->position = new_position;
    return static_cast<int64_t>(new_position);
}

bool stat_file(process::Task& proc, uint32_t handle, Metadata& metadata) {
    if (handle >= process::kMaxFileHandles) {
        return false;
    }
    sync::LockGuard guard(proc.resources->file_locks[handle]);
    process::FileHandle* entry = get_file_handle(proc, handle);
    if (entry == nullptr) {
        return false;
    }
    metadata = {};
    metadata.size = entry->handle.size;
    return true;
}

bool stat_path(process::Task& proc, const char* path, Metadata& metadata) {
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return false;
    }
    if (!acl_allows(proc, local_path, vfs::AclPermission::Read)) {
        return false;
    }
    vfs::FileHandle file{};
    if (vfs::open_file(local_path, file)) {
        metadata = {};
        metadata.size = file.size;
        vfs::close_file(file);
        return true;
    }
    vfs::DirectoryHandle directory{};
    if (!vfs::open_directory(local_path, directory)) {
        return false;
    }
    metadata = {};
    metadata.flags = MetadataDirectory;
    vfs::close_directory(directory);
    return true;
}

uint64_t map_file_private(process::Task& proc,
                          uint32_t handle,
                          uint64_t file_offset,
                          size_t length,
                          uint64_t flags) {
    if (handle >= process::kMaxFileHandles) {
        return 0;
    }
    sync::LockGuard guard(proc.resources->file_locks[handle]);
    process::FileHandle* entry = get_file_handle(proc, handle);
    if (entry == nullptr || entry->path[0] == '\0') {
        return 0;
    }
    return vm::map_file_private(proc.cr3,
                                entry->path,
                                entry->handle,
                                file_offset,
                                length,
                                flags);
}

int32_t open_directory(process::Task& proc, const char* path) {
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return -1;
    }
    vfs::DirectoryHandle vfs_handle{};
    vfs::AclSnapshot acl{};
    PrincipalSnapshot principal(proc);
    if (!principal.valid()) {
        return -1;
    }
    bool check_acl = acl_check_required(principal.get());
    if (!vfs::open_directory(local_path,
                             vfs_handle,
                             check_acl ? &acl : nullptr)) {
        vfs::close_directory(vfs_handle);
        return -1;
    }
    if (check_acl && !acl_snapshot_decision(principal.get(), acl).read) {
        vfs::close_directory(vfs_handle);
        return -1;
    }

    int32_t slot = allocate_directory_handle(proc);
    if (slot < 0) {
        log_message(LogLevel::Warn,
                    "FileIO: no free directory handles for process %u",
                    static_cast<unsigned int>(proc.pid));
        vfs::close_directory(vfs_handle);
        return -1;
    }

    process::DirectoryHandle& handle =
        proc.resources->directory_handles[static_cast<size_t>(slot)];
    handle.handle = vfs_handle;
    string_util::copy(handle.path, sizeof(handle.path), local_path);
    publish_directory_handle(proc, static_cast<size_t>(slot));
    return slot;
}

int32_t open_directory_root(process::Task& proc) {
    char local_path[path_util::kMaxPathLength];
    local_path[0] = '/';
    local_path[1] = '\0';

    vfs::DirectoryHandle vfs_handle{};
    if (!vfs::open_directory(local_path, vfs_handle)) {
        return -1;
    }

    int32_t slot = allocate_directory_handle(proc);
    if (slot < 0) {
        log_message(LogLevel::Warn,
                    "FileIO: no free directory handles for process %u",
                    static_cast<unsigned int>(proc.pid));
        vfs::close_directory(vfs_handle);
        return -1;
    }

    process::DirectoryHandle& handle =
        proc.resources->directory_handles[static_cast<size_t>(slot)];
    handle.handle = vfs_handle;
    string_util::copy(handle.path, sizeof(handle.path), local_path);
    publish_directory_handle(proc, static_cast<size_t>(slot));
    return slot;
}

int32_t open_directory_at(process::Task& proc,
                          uint32_t dir_handle,
                          const char* name) {
    if (dir_handle >= process::kMaxDirectoryHandles) {
        return -1;
    }
    char base_path[path_util::kMaxPathLength];
    {
        sync::LockGuard parent_guard(
            proc.resources->directory_locks[dir_handle]);
        process::DirectoryHandle* parent =
            get_directory_handle(proc, dir_handle);
        if (parent == nullptr) {
            return -1;
        }
        string_util::copy(base_path, sizeof(base_path), parent->path);
    }
    char local_path[path_util::kMaxPathLength];
    if (!build_child_path(proc, base_path, name, local_path)) {
        return -1;
    }
    vfs::DirectoryHandle vfs_handle{};
    vfs::AclSnapshot acl{};
    PrincipalSnapshot principal(proc);
    if (!principal.valid()) {
        return -1;
    }
    bool check_acl = acl_check_required(principal.get());
    if (!vfs::open_directory(local_path,
                             vfs_handle,
                             check_acl ? &acl : nullptr)) {
        vfs::close_directory(vfs_handle);
        return -1;
    }
    if (check_acl && !acl_snapshot_decision(principal.get(), acl).read) {
        vfs::close_directory(vfs_handle);
        return -1;
    }

    int32_t slot = allocate_directory_handle(proc);
    if (slot < 0) {
        log_message(LogLevel::Warn,
                    "FileIO: no free directory handles for process %u",
                    static_cast<unsigned int>(proc.pid));
        vfs::close_directory(vfs_handle);
        return -1;
    }

    process::DirectoryHandle& handle =
        proc.resources->directory_handles[static_cast<size_t>(slot)];
    handle.handle = vfs_handle;
    string_util::copy(handle.path, sizeof(handle.path), local_path);
    publish_directory_handle(proc, static_cast<size_t>(slot));
    return slot;
}

bool create_directory(process::Task& proc, const char* path) {
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return false;
    }
    char parent[path_util::kMaxPathLength];
    return parent_path(local_path, parent) &&
           acl_allows(proc, parent, vfs::AclPermission::Write) &&
           vfs::create_directory(local_path);
}

bool remove_file(process::Task& proc, const char* path) {
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return false;
    }
    if (!acl_allows(proc, local_path, vfs::AclPermission::Delete)) {
        return false;
    }
    return vfs::remove_file(local_path);
}

bool remove_directory(process::Task& proc, const char* path) {
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return false;
    }
    return acl_allows(proc, local_path, vfs::AclPermission::Delete) &&
           vfs::remove_directory(local_path);
}

int32_t open_file_at(process::Task& proc,
                     uint32_t dir_handle,
                     const char* name) {
    if (dir_handle >= process::kMaxDirectoryHandles) {
        return -1;
    }
    char base_path[path_util::kMaxPathLength];
    {
        sync::LockGuard parent_guard(
            proc.resources->directory_locks[dir_handle]);
        process::DirectoryHandle* parent =
            get_directory_handle(proc, dir_handle);
        if (parent == nullptr) {
            return -1;
        }
        string_util::copy(base_path, sizeof(base_path), parent->path);
    }
    char local_path[path_util::kMaxPathLength];
    if (!build_child_path(proc, base_path, name, local_path)) {
        return -1;
    }
    int32_t slot = allocate_file_handle(proc);
    if (slot < 0) {
        log_message(LogLevel::Warn,
                    "FileIO: no free file handles for process %u",
                    static_cast<unsigned int>(proc.pid));
        return -1;
    }

    vfs::FileHandle vfs_handle{};
    vfs::AclSnapshot acl{};
    PrincipalSnapshot principal(proc);
    if (!principal.valid()) {
        release_file_reservation(proc, static_cast<size_t>(slot));
        return -1;
    }
    bool check_acl = acl_check_required(principal.get());
    if (!vfs::open_file(local_path,
                        vfs_handle,
                        check_acl ? &acl : nullptr)) {
        vfs::close_file(vfs_handle);
        proc.resources->file_handles[static_cast<size_t>(slot)].handle = {};
        release_file_reservation(proc, static_cast<size_t>(slot));
        return -1;
    }
    AclDecision decision = check_acl
                               ? acl_snapshot_decision(principal.get(), acl)
                               : AclDecision{true, true, true, true};
    if (!decision.read) {
        vfs::close_file(vfs_handle);
        proc.resources->file_handles[static_cast<size_t>(slot)].handle = {};
        release_file_reservation(proc, static_cast<size_t>(slot));
        return -1;
    }

    process::FileHandle& handle = proc.resources->file_handles[static_cast<size_t>(slot)];
    handle.handle = vfs_handle;
    handle.can_read = true;
    handle.can_write = decision.write;
    handle.append = false;
    handle.position = 0;
    string_util::copy(handle.path, sizeof(handle.path), local_path);
    publish_file_handle(proc, static_cast<size_t>(slot));
    return slot;
}

int32_t create_file_at(process::Task& proc,
                       uint32_t dir_handle,
                       const char* name) {
    if (dir_handle >= process::kMaxDirectoryHandles) {
        return -1;
    }
    char base_path[path_util::kMaxPathLength];
    {
        sync::LockGuard parent_guard(
            proc.resources->directory_locks[dir_handle]);
        process::DirectoryHandle* parent =
            get_directory_handle(proc, dir_handle);
        if (parent == nullptr) {
            return -1;
        }
        string_util::copy(base_path, sizeof(base_path), parent->path);
    }
    char local_path[path_util::kMaxPathLength];
    if (!build_child_path(proc, base_path, name, local_path)) {
        return -1;
    }
    char parent_path_buffer[path_util::kMaxPathLength];
    if (!parent_path(local_path, parent_path_buffer) ||
        !acl_allows(proc,
                    parent_path_buffer,
                    vfs::AclPermission::Write)) {
        return -1;
    }

    int32_t slot = allocate_file_handle(proc);
    if (slot < 0) {
        log_message(LogLevel::Warn,
                    "FileIO: no free file handles for process %u",
                    static_cast<unsigned int>(proc.pid));
        return -1;
    }

    vfs::FileHandle vfs_handle{};
    if (!vfs::create_file(local_path, vfs_handle)) {
        proc.resources->file_handles[static_cast<size_t>(slot)].handle = {};
        release_file_reservation(proc, static_cast<size_t>(slot));
        return -1;
    }

    process::FileHandle& handle = proc.resources->file_handles[static_cast<size_t>(slot)];
    handle.handle = vfs_handle;
    handle.can_read = true;
    handle.can_write = true;
    handle.append = false;
    handle.position = 0;
    string_util::copy(handle.path, sizeof(handle.path), local_path);
    publish_file_handle(proc, static_cast<size_t>(slot));
    return slot;
}

bool close_directory(process::Task& proc, uint32_t handle) {
    if (handle >= process::kMaxDirectoryHandles) {
        return false;
    }
    sync::LockGuard guard(proc.resources->directory_locks[handle]);
    process::DirectoryHandle* entry = get_directory_handle(proc, handle);
    if (entry == nullptr) {
        return false;
    }
    vfs::close_directory(entry->handle);
    __atomic_store_n(&entry->in_use, false, __ATOMIC_RELEASE);
    entry->handle = {};
    entry->path[0] = '\0';
    return true;
}

int64_t read_directory(process::Task& proc, uint32_t handle,
                       uint64_t user_addr) {
    if (handle >= process::kMaxDirectoryHandles) {
        return -1;
    }
    sync::LockGuard guard(proc.resources->directory_locks[handle]);
    process::DirectoryHandle* entry = get_directory_handle(proc, handle);
    if (entry == nullptr) {
        return -1;
    }

    vfs::DirEntry result{};
    if (!vfs::read_directory(entry->handle, result)) {
        return 0;
    }

    if (!vm::copy_to_user(proc.cr3, user_addr, &result, sizeof(vfs::DirEntry))) {
        return -1;
    }
    return 1;
}

int64_t get_acl(process::Task& proc,
                const char* path,
                uint64_t user_entries,
                uint64_t max_entries) {
    if (max_entries == 0 || max_entries > vfs::kMaxAclEntries ||
        user_entries == 0) {
        return -1;
    }
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return -1;
    }
    vfs::AclEntry entries[vfs::kMaxAclEntries]{};
    size_t count = 0;
    if (!vfs::get_acl(local_path,
                      entries,
                      static_cast<size_t>(max_entries),
                      count) ||
        !vm::copy_to_user(proc.cr3,
                          user_entries,
                          entries,
                          count * sizeof(vfs::AclEntry))) {
        return -1;
    }
    return static_cast<int64_t>(count);
}

bool set_acl(process::Task& proc,
             const char* path,
             uint64_t user_entries,
             uint64_t entry_count) {
    if (entry_count > vfs::kMaxAclEntries ||
        (entry_count != 0 && user_entries == 0)) {
        return false;
    }
    char local_path[path_util::kMaxPathLength];
    if (!copy_path(proc, path, local_path)) {
        return false;
    }
    vfs::AclEntry entries[vfs::kMaxAclEntries]{};
    if (entry_count != 0 &&
        !vm::copy_from_user(proc.cr3,
                            entries,
                            user_entries,
                            static_cast<size_t>(entry_count) *
                                sizeof(vfs::AclEntry))) {
        return false;
    }
    return vfs::set_acl(local_path,
                        entries,
                        static_cast<size_t>(entry_count));
}

}  // namespace file_io
