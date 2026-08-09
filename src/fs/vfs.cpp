#include "vfs.hpp"

#include <stddef.h>
#include <stdint.h>

#include "drivers/log/logging.hpp"
#include "lib/mem.hpp"

namespace vfs {

namespace {

constexpr size_t kMaxMounts = 16;
constexpr size_t kMaxRootDirHandles = 8;
constexpr size_t kMaxVolumeAliasLength = 63;

struct MountEntry {
    const char* name;
    const FilesystemOps* ops;
    void* fs_context;
    char volume_alias[kMaxVolumeAliasLength + 1];
    bool in_use;
};

MountEntry g_mounts[kMaxMounts]{};
const char* g_root_mount = nullptr;

struct RootDirectoryContext {
    size_t index;
    size_t count;
    const char* names[kMaxMounts];
};

RootDirectoryContext g_root_dir_contexts[kMaxRootDirHandles]{};
bool g_root_dir_in_use[kMaxRootDirHandles]{};
volatile int g_vfs_lock = 0;

void lock() {
    while (__atomic_test_and_set(&g_vfs_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock() {
    __atomic_clear(&g_vfs_lock, __ATOMIC_RELEASE);
}

class LockGuard {
public:
    LockGuard() { lock(); }
    ~LockGuard() { unlock(); }
};

size_t string_length(const char* str) {
    size_t len = 0;
    if (str == nullptr) {
        return 0;
    }
    while (str[len] != '\0') {
        ++len;
    }
    return len;
}

int string_compare_n(const char* a, const char* b, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca != cb) {
            return (ca < cb) ? -1 : 1;
        }
    }
    return 0;
}

bool ascii_equal_ignore_case(const char* a, const char* b, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return true;
}

bool copy_valid_volume_alias(const char* preferred,
                             char (&out)[kMaxVolumeAliasLength + 1]) {
    out[0] = '\0';
    if (preferred == nullptr || preferred[0] == '\0') return false;

    size_t length = 0;
    while (preferred[length] != '\0') {
        const unsigned char c = static_cast<unsigned char>(preferred[length]);
        const bool valid = (c >= 'a' && c <= 'z') ||
                           (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                           c == '.';
        if (!valid || length >= kMaxVolumeAliasLength) return false;
        ++length;
    }
    if ((length == 1 && preferred[0] == '.') ||
        (length == 2 && preferred[0] == '.' && preferred[1] == '.') ||
        (length == 3 && ascii_equal_ignore_case(preferred, "sys", 3))) {
        return false;
    }
    memcpy(out, preferred, length);
    out[length] = '\0';
    return true;
}

MountEntry* find_unique_alias(const char* alias, size_t alias_length) {
    MountEntry* match = nullptr;
    for (auto& mount : g_mounts) {
        if (!mount.in_use || string_length(mount.volume_alias) != alias_length ||
            string_compare_n(mount.volume_alias, alias, alias_length) != 0) {
            continue;
        }
        if (match != nullptr) return nullptr;
        match = &mount;
    }
    return match;
}

const char* skip_leading_slash(const char* path) {
    if (path == nullptr) {
        return nullptr;
    }
    while (*path == '/') {
        ++path;
    }
    return path;
}

MountEntry* find_mount(const char* name, size_t name_len) {
    for (auto& mount : g_mounts) {
        if (!mount.in_use) {
            continue;
        }
        if (string_length(mount.name) != name_len) {
            continue;
        }
        if (string_compare_n(mount.name, name, name_len) == 0) {
            return &mount;
        }
    }
    return nullptr;
}

MountEntry* find_mount_for_path(const char* path, const char*& remainder) {
    if (path == nullptr) {
        return nullptr;
    }

    const char* trimmed = skip_leading_slash(path);
    if (trimmed == nullptr || *trimmed == '\0') {
        remainder = trimmed;
        return nullptr;
    }

    const char* mount_end = trimmed;
    while (*mount_end != '\0' && *mount_end != '/') {
        ++mount_end;
    }

    size_t mount_len = static_cast<size_t>(mount_end - trimmed);
    MountEntry* entry = find_mount(trimmed, mount_len);
    if (entry == nullptr) {
        if (g_root_mount == nullptr) {
            return nullptr;
        }
        entry = find_mount(g_root_mount, string_length(g_root_mount));
        if (entry == nullptr) {
            return nullptr;
        }
        remainder = trimmed;
        return entry;
    }

    const char* next = mount_end;
    while (*next == '/') {
        ++next;
    }
    remainder = next;
    return entry;
}

MountEntry* resolve_mount_for_path(const char* path, const char*& remainder) {
    LockGuard guard;
    return find_mount_for_path(path, remainder);
}

const char* normalize_relative_path(const char* path) {
    if (path == nullptr) {
        return "";
    }
    return path;
}

bool is_root_path(const char* path) {
    if (path == nullptr) {
        return true;
    }
    const char* trimmed = skip_leading_slash(path);
    return trimmed == nullptr || *trimmed == '\0';
}

RootDirectoryContext* allocate_root_dir_context() {
    LockGuard guard;
    for (size_t i = 0; i < kMaxRootDirHandles; ++i) {
        if (!g_root_dir_in_use[i]) {
            g_root_dir_in_use[i] = true;
            RootDirectoryContext& ctx = g_root_dir_contexts[i];
            memset(&ctx, 0, sizeof(RootDirectoryContext));
            return &ctx;
        }
    }
    return nullptr;
}

void release_root_dir_context(RootDirectoryContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    LockGuard guard;
    for (size_t i = 0; i < kMaxRootDirHandles; ++i) {
        if (&g_root_dir_contexts[i] == ctx) {
            g_root_dir_in_use[i] = false;
            return;
        }
    }
}

void populate_entry_from_mount(const char* name, DirEntry& entry) {
    memset(&entry, 0, sizeof(DirEntry));
    if (name == nullptr) {
        entry.name[0] = '\0';
    } else {
        size_t len = string_length(name);
        if (len >= sizeof(entry.name)) {
            len = sizeof(entry.name) - 1;
        }
        memcpy(entry.name, name, len);
        entry.name[len] = '\0';
    }
    entry.flags = kDirEntryFlagDirectory;
    entry.size = 0;
    entry.reserved = 0;
}

}  // namespace

void init() {
    LockGuard guard;
    for (auto& mount : g_mounts) {
        mount = {};
    }
    memset(g_root_dir_contexts, 0, sizeof(g_root_dir_contexts));
    memset(g_root_dir_in_use, 0, sizeof(g_root_dir_in_use));
    g_root_mount = nullptr;
}

bool register_mount(const char* name,
                    const FilesystemOps* ops,
                    void* fs_context) {
    if (name == nullptr || *name == '\0' || ops == nullptr) {
        log_message(LogLevel::Warn,
                    "VFS: register mount failed (invalid parameters)");
        return false;
    }

    LockGuard guard;
    size_t name_len = string_length(name);
    if (find_mount(name, name_len) != nullptr) {
        log_message(LogLevel::Warn, "VFS: mount '%s' already exists", name);
        return false;
    }

    for (auto& mount : g_mounts) {
        if (mount.in_use) {
            continue;
        }
        mount.name = name;
        mount.ops = ops;
        mount.fs_context = fs_context;
        if (ops->get_volume_alias != nullptr) {
            const char* preferred = ops->get_volume_alias(fs_context);
            if (preferred != nullptr && preferred[0] != '\0' &&
                !copy_valid_volume_alias(preferred, mount.volume_alias)) {
                log_message(LogLevel::Warn,
                            "VFS: rejected invalid or reserved alias '@%s' for '%s'",
                            preferred, name);
            }
        }
        mount.in_use = true;
        if (mount.volume_alias[0] != '\0') {
            size_t alias_length = string_length(mount.volume_alias);
            if (find_unique_alias(mount.volume_alias, alias_length) == nullptr) {
                log_message(LogLevel::Warn,
                            "VFS: mounted '%s'; alias '@%s' unavailable due to conflict",
                            name, mount.volume_alias);
            } else {
                log_message(LogLevel::Info,
                            "VFS: mounted '%s' (preferred alias '@%s')",
                            name, mount.volume_alias);
            }
        } else {
            log_message(LogLevel::Info, "VFS: mounted '%s'", name);
        }
        return true;
    }

    log_message(LogLevel::Warn, "VFS: no free mount slots for '%s'", name);
    return false;
}

void set_root_mount(const char* name) {
    LockGuard guard;
    if (name == nullptr || *name == '\0') {
        g_root_mount = nullptr;
        return;
    }
    size_t name_len = string_length(name);
    MountEntry* mount = find_mount(name, name_len);
    if (mount == nullptr) {
        log_message(LogLevel::Warn,
                    "VFS: cannot set root mount to unknown '%s'",
                    name);
        return;
    }
    g_root_mount = mount->name;
    log_message(LogLevel::Info, "VFS: root mount alias set to '%s'", g_root_mount);
}

const char* root_mount_name() {
    LockGuard guard;
    return g_root_mount;
}

bool has_explicit_mount_prefix(const char* path) {
    if (path == nullptr) {
        return false;
    }

    const char* trimmed = skip_leading_slash(path);
    if (trimmed == nullptr || *trimmed == '\0') {
        return false;
    }

    const char* mount_end = trimmed;
    while (*mount_end != '\0' && *mount_end != '/') {
        ++mount_end;
    }

    size_t mount_len = static_cast<size_t>(mount_end - trimmed);
    LockGuard guard;
    return find_mount(trimmed, mount_len) != nullptr;
}

const char* mount_name_for_alias(const char* alias, size_t alias_length) {
    if (alias == nullptr || alias_length == 0) return nullptr;
    LockGuard guard;
    MountEntry* mount = find_unique_alias(alias, alias_length);
    return mount != nullptr ? mount->name : nullptr;
}

const char* volume_alias_for_mount(const char* name, size_t name_length) {
    if (name == nullptr || name_length == 0) return nullptr;
    LockGuard guard;
    MountEntry* mount = find_mount(name, name_length);
    if (mount == nullptr || mount->volume_alias[0] == '\0') return nullptr;
    size_t alias_length = string_length(mount->volume_alias);
    return find_unique_alias(mount->volume_alias, alias_length) == mount
               ? mount->volume_alias
               : nullptr;
}

size_t enumerate_mounts(const char** names, size_t max_names) {
    LockGuard guard;
    size_t count = 0;
    for (auto& mount : g_mounts) {
        if (!mount.in_use) {
            continue;
        }
        if (names != nullptr && count < max_names) {
            names[count] = mount.name;
        }
        ++count;
    }
    return count;
}

bool list(const char* path,
          DirEntry* entries,
          size_t max_entries,
          size_t& out_count) {
    out_count = 0;
    if (entries == nullptr || max_entries == 0) {
        return false;
    }

    if (is_root_path(path)) {
        const char* names[kMaxMounts];
        size_t mount_count = enumerate_mounts(names, kMaxMounts);
        size_t stored =
            (mount_count < kMaxMounts) ? mount_count : kMaxMounts;
        size_t limit = (stored < max_entries) ? stored : max_entries;
        for (size_t i = 0; i < limit; ++i) {
            populate_entry_from_mount(names[i], entries[i]);
        }
        out_count = limit;
        return true;
    }

    const char* remainder = nullptr;
    MountEntry* mount = resolve_mount_for_path(path, remainder);
    if (mount == nullptr) {
        log_message(LogLevel::Warn,
                    "VFS: mount '%s' not found for list operation",
                    (path != nullptr) ? path : "(null)");
        return false;
    }

    if (mount->ops == nullptr || mount->ops->list_directory == nullptr) {
        return false;
    }

    const char* relative = normalize_relative_path(remainder);
    size_t count = 0;
    if (!mount->ops->list_directory(mount->fs_context,
                                    relative,
                                    entries,
                                    max_entries,
                                    count) ||
        count > max_entries) {
        out_count = 0;
        return false;
    }
    out_count = count;
    return true;
}

bool read_file(const char* path,
               void* buffer,
               size_t buffer_size,
               size_t& out_size) {
    out_size = 0;
    if (buffer == nullptr) {
        return false;
    }

    FileHandle handle{};
    if (!open_file(path, handle)) {
        return false;
    }

    bool ok = read_file(handle, 0, buffer, buffer_size, out_size);
    close_file(handle);
    return ok;
}

bool open_file(const char* path, FileHandle& out_handle) {
    return open_file(path, out_handle, nullptr);
}

bool open_file(const char* path,
               FileHandle& out_handle,
               AclSnapshot* out_acl) {
    out_handle = {};
    if (out_acl != nullptr) {
        *out_acl = {};
    }

    const char* remainder = nullptr;
    MountEntry* mount = resolve_mount_for_path(path, remainder);
    if (mount == nullptr) {
        log_message(LogLevel::Warn,
                    "VFS: mount not found for path '%s'",
                    (path != nullptr) ? path : "(null)");
        return false;
    }

    if (mount->ops == nullptr || mount->ops->open_file == nullptr ||
        mount->ops->read_file == nullptr || mount->ops->close_file == nullptr) {
        return false;
    }

    const char* relative = normalize_relative_path(remainder);
    if (relative == nullptr || *relative == '\0') {
        return false;
    }

    DirEntry metadata{};
    void* file_context = nullptr;
    if (!mount->ops->open_file(mount->fs_context,
                               relative,
                               file_context,
                               &metadata)) {
        return false;
    }

    if (out_acl != nullptr) {
        out_acl->supported = mount->ops->get_acl != nullptr ||
                             mount->ops->get_open_file_acl != nullptr;
        bool acl_ok = true;
        if (mount->ops->get_open_file_acl != nullptr) {
            acl_ok = mount->ops->get_open_file_acl(file_context,
                                                    out_acl->entries,
                                                    kMaxAclEntries,
                                                    out_acl->count);
        } else if (out_acl->supported) {
            // Preserve fail-closed behavior for ACL-aware filesystems that
            // have not implemented the resolved-handle fast path yet.
            acl_ok = mount->ops->get_acl(mount->fs_context,
                                         relative,
                                         out_acl->entries,
                                         kMaxAclEntries,
                                         out_acl->count);
        }
        if (!acl_ok || out_acl->count > kMaxAclEntries) {
            mount->ops->close_file(file_context);
            return false;
        }
    }

    out_handle.ops = mount->ops;
    out_handle.fs_context = mount->fs_context;
    out_handle.file_context = file_context;
    out_handle.size = metadata.size;
    return true;
}

bool create_file(const char* path, FileHandle& out_handle) {
    out_handle = {};

    const char* remainder = nullptr;
    MountEntry* mount = resolve_mount_for_path(path, remainder);
    if (mount == nullptr) {
        log_message(LogLevel::Warn,
                    "VFS: mount not found for path '%s'",
                    (path != nullptr) ? path : "(null)");
        return false;
    }

    if (mount->ops == nullptr || mount->ops->create_file == nullptr ||
        mount->ops->read_file == nullptr || mount->ops->write_file == nullptr ||
        mount->ops->close_file == nullptr) {
        return false;
    }

    const char* relative = normalize_relative_path(remainder);
    if (relative == nullptr || *relative == '\0') {
        return false;
    }

    DirEntry metadata{};
    void* file_context = nullptr;
    if (!mount->ops->create_file(mount->fs_context,
                                 relative,
                                 file_context,
                                 &metadata)) {
        return false;
    }

    out_handle.ops = mount->ops;
    out_handle.fs_context = mount->fs_context;
    out_handle.file_context = file_context;
    out_handle.size = metadata.size;
    return true;
}

bool create_directory(const char* path) {
    const char* remainder = nullptr;
    MountEntry* mount = resolve_mount_for_path(path, remainder);
    if (mount == nullptr) {
        log_message(LogLevel::Warn,
                    "VFS: mount not found for path '%s'",
                    (path != nullptr) ? path : "(null)");
        return false;
    }
    if (mount->ops == nullptr || mount->ops->create_directory == nullptr) {
        return false;
    }
    const char* relative = normalize_relative_path(remainder);
    if (relative == nullptr || *relative == '\0') {
        return false;
    }
    return mount->ops->create_directory(mount->fs_context, relative);
}

bool remove_file(const char* path) {
    const char* remainder = nullptr;
    MountEntry* mount = resolve_mount_for_path(path, remainder);
    if (mount == nullptr) {
        log_message(LogLevel::Warn,
                    "VFS: mount not found for path '%s'",
                    (path != nullptr) ? path : "(null)");
        return false;
    }
    if (mount->ops == nullptr || mount->ops->remove_file == nullptr) {
        return false;
    }
    const char* relative = normalize_relative_path(remainder);
    if (relative == nullptr || *relative == '\0') {
        return false;
    }
    return mount->ops->remove_file(mount->fs_context, relative);
}

bool remove_directory(const char* path) {
    const char* remainder = nullptr;
    MountEntry* mount = resolve_mount_for_path(path, remainder);
    if (mount == nullptr) {
        log_message(LogLevel::Warn,
                    "VFS: mount not found for path '%s'",
                    (path != nullptr) ? path : "(null)");
        return false;
    }
    if (mount->ops == nullptr || mount->ops->remove_directory == nullptr) {
        return false;
    }
    const char* relative = normalize_relative_path(remainder);
    if (relative == nullptr || *relative == '\0') {
        return false;
    }
    return mount->ops->remove_directory(mount->fs_context, relative);
}

void close_file(FileHandle& handle) {
    if (handle.ops != nullptr && handle.ops->close_file != nullptr &&
        handle.file_context != nullptr) {
        handle.ops->close_file(handle.file_context);
    }
    handle = {};
}

bool read_file(FileHandle& handle,
               uint64_t offset,
               void* buffer,
               size_t buffer_size,
               size_t& out_size) {
    out_size = 0;
    if (handle.ops == nullptr || handle.ops->read_file == nullptr ||
        handle.file_context == nullptr || buffer == nullptr ||
        static_cast<uint64_t>(buffer_size) > UINT64_MAX - offset) {
        return false;
    }
    if (!handle.ops->read_file(handle.file_context,
                               offset,
                               buffer,
                               buffer_size,
                               out_size) ||
        out_size > buffer_size) {
        out_size = 0;
        return false;
    }
    return true;
}

bool write_file(FileHandle& handle,
                uint64_t offset,
                const void* buffer,
                size_t buffer_size,
                size_t& out_size) {
    out_size = 0;
    if (handle.ops == nullptr || handle.ops->write_file == nullptr ||
        handle.file_context == nullptr || buffer == nullptr ||
        static_cast<uint64_t>(buffer_size) > UINT64_MAX - offset) {
        return false;
    }
    if (!handle.ops->write_file(handle.file_context,
                                offset,
                                buffer,
                                buffer_size,
                                out_size)) {
        out_size = 0;
        return false;
    }

    if (out_size > buffer_size ||
        static_cast<uint64_t>(out_size) > UINT64_MAX - offset) {
        out_size = 0;
        return false;
    }

    uint64_t end_offset = offset + static_cast<uint64_t>(out_size);
    if (end_offset > handle.size) {
        handle.size = end_offset;
    }
    return true;
}

bool open_directory(const char* path, DirectoryHandle& out_handle) {
    return open_directory(path, out_handle, nullptr);
}

bool open_directory(const char* path,
                    DirectoryHandle& out_handle,
                    AclSnapshot* out_acl) {
    out_handle = {};
    if (out_acl != nullptr) {
        *out_acl = {};
    }

    if (is_root_path(path)) {
        RootDirectoryContext* ctx = allocate_root_dir_context();
        if (ctx == nullptr) {
            log_message(LogLevel::Warn,
                        "VFS: no free root directory contexts");
            return false;
        }
        size_t mount_count = enumerate_mounts(ctx->names, kMaxMounts);
        ctx->count = (mount_count < kMaxMounts) ? mount_count : kMaxMounts;
        ctx->index = 0;

        out_handle.dir_context = ctx;
        out_handle.fs_context = nullptr;
        out_handle.ops = nullptr;
        out_handle.is_root = true;
        return true;
    }

    const char* remainder = nullptr;
    MountEntry* mount = resolve_mount_for_path(path, remainder);
    if (mount == nullptr) {
        log_message(LogLevel::Warn,
                    "VFS: mount not found for path '%s'",
                    (path != nullptr) ? path : "(null)");
        return false;
    }

    if (mount->ops == nullptr || mount->ops->open_directory == nullptr ||
        mount->ops->directory_next == nullptr ||
        mount->ops->close_directory == nullptr) {
        return false;
    }

    const char* relative = normalize_relative_path(remainder);
    void* dir_context = nullptr;
    if (!mount->ops->open_directory(mount->fs_context,
                                    relative,
                                    dir_context)) {
        return false;
    }

    if (out_acl != nullptr) {
        out_acl->supported = mount->ops->get_acl != nullptr ||
                             mount->ops->get_open_directory_acl != nullptr;
        bool acl_ok = true;
        if (mount->ops->get_open_directory_acl != nullptr) {
            acl_ok = mount->ops->get_open_directory_acl(
                dir_context,
                out_acl->entries,
                kMaxAclEntries,
                out_acl->count);
        } else if (out_acl->supported) {
            acl_ok = mount->ops->get_acl(mount->fs_context,
                                         relative,
                                         out_acl->entries,
                                         kMaxAclEntries,
                                         out_acl->count);
        }
        if (!acl_ok || out_acl->count > kMaxAclEntries) {
            mount->ops->close_directory(dir_context);
            return false;
        }
    }

    out_handle.ops = mount->ops;
    out_handle.fs_context = mount->fs_context;
    out_handle.dir_context = dir_context;
    out_handle.is_root = false;
    return true;
}

bool read_directory(DirectoryHandle& handle, DirEntry& out_entry) {
    if (handle.is_root) {
        LockGuard guard;
        auto* ctx = static_cast<RootDirectoryContext*>(handle.dir_context);
        if (ctx == nullptr || ctx->index >= ctx->count) {
            return false;
        }
        populate_entry_from_mount(ctx->names[ctx->index], out_entry);
        ++ctx->index;
        return true;
    }

    if (handle.ops == nullptr || handle.ops->directory_next == nullptr ||
        handle.dir_context == nullptr) {
        return false;
    }
    return handle.ops->directory_next(handle.dir_context, out_entry);
}

void close_directory(DirectoryHandle& handle) {
    if (handle.is_root) {
        release_root_dir_context(
            static_cast<RootDirectoryContext*>(handle.dir_context));
        handle = {};
        return;
    }

    if (handle.ops != nullptr && handle.ops->close_directory != nullptr &&
        handle.dir_context != nullptr) {
        handle.ops->close_directory(handle.dir_context);
    }
    handle = {};
}

bool acl_supported(const char* path) {
    const char* remainder = nullptr;
    MountEntry* mount = resolve_mount_for_path(path, remainder);
    return mount != nullptr && mount->ops != nullptr &&
           mount->ops->get_acl != nullptr && mount->ops->set_acl != nullptr;
}

bool get_acl(const char* path,
             AclEntry* entries,
             size_t max_entries,
             size_t& out_count) {
    out_count = 0;
    if (entries == nullptr || max_entries == 0) {
        return false;
    }
    const char* remainder = nullptr;
    MountEntry* mount = resolve_mount_for_path(path, remainder);
    if (mount == nullptr || mount->ops == nullptr ||
        mount->ops->get_acl == nullptr) {
        return false;
    }
    return mount->ops->get_acl(mount->fs_context,
                               normalize_relative_path(remainder),
                               entries,
                               max_entries,
                               out_count) &&
           out_count <= max_entries;
}

bool set_acl(const char* path,
             const AclEntry* entries,
             size_t entry_count) {
    if ((entries == nullptr && entry_count != 0) ||
        entry_count > kMaxAclEntries) {
        return false;
    }
    const char* remainder = nullptr;
    MountEntry* mount = resolve_mount_for_path(path, remainder);
    if (mount == nullptr || mount->ops == nullptr ||
        mount->ops->set_acl == nullptr) {
        return false;
    }
    return mount->ops->set_acl(mount->fs_context,
                               normalize_relative_path(remainder),
                               entries,
                               entry_count);
}

}  // namespace vfs
