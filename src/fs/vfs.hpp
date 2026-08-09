#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vfs {

enum : uint32_t {
    kDirEntryFlagDirectory = 1u << 0,
};

constexpr size_t kMaxAclEntries = 32;

enum class AclValue : uint8_t {
    Deny = 0,
    Allow = 1,
    Default = 2,
};

enum class AclPermission : uint8_t {
    Read,
    Write,
    Delete,
    Edit,
};

struct AclEntry {
    uint64_t machine_id;
    uint64_t local_id;
    AclValue write;
    AclValue read;
    AclValue delete_permission;
    AclValue edit;
    uint8_t reserved[4];
};

static_assert(sizeof(AclEntry) == 24, "VFS ACL entry size mismatch");

struct AclSnapshot {
    AclEntry entries[kMaxAclEntries];
    size_t count;
    bool supported;
};

struct DirEntry {
    char name[64];
    uint32_t flags;
    uint32_t reserved;
    uint64_t size;
};

struct FilesystemOps;

struct FileHandle {
    const FilesystemOps* ops;
    void* fs_context;
    void* file_context;
    uint64_t size;
};

struct DirectoryHandle {
    const FilesystemOps* ops;
    void* fs_context;
    void* dir_context;
    bool is_root;
};

struct FilesystemOps {
    // Returns a filesystem-provided preferred name for the global @ namespace,
    // or nullptr when the filesystem has no preference. The VFS validates the
    // value, reserves @sys, and suppresses conflicting aliases.
    const char* (*get_volume_alias)(void* fs_context);
    bool (*list_directory)(void* fs_context,
                           const char* path,
                           DirEntry* entries,
                           size_t max_entries,
                           size_t& out_count);
    bool (*open_file)(void* fs_context,
                      const char* path,
                      void*& out_file_context,
                      DirEntry* out_metadata);
    bool (*create_file)(void* fs_context,
                        const char* path,
                        void*& out_file_context,
                        DirEntry* out_metadata);
    bool (*create_directory)(void* fs_context, const char* path);
    bool (*remove_file)(void* fs_context, const char* path);
    bool (*remove_directory)(void* fs_context, const char* path);
    bool (*read_file)(void* file_context,
                      uint64_t offset,
                      void* buffer,
                      size_t buffer_size,
                      size_t& out_size);
    bool (*write_file)(void* file_context,
                       uint64_t offset,
                       const void* buffer,
                       size_t buffer_size,
                       size_t& out_size);
    void (*close_file)(void* file_context);
    bool (*open_directory)(void* fs_context,
                           const char* path,
                           void*& out_dir_context);
    bool (*directory_next)(void* dir_context, DirEntry& out_entry);
    void (*close_directory)(void* dir_context);
    bool (*get_acl)(void* fs_context,
                    const char* path,
                    AclEntry* entries,
                    size_t max_entries,
                    size_t& out_count);
    bool (*set_acl)(void* fs_context,
                    const char* path,
                    const AclEntry* entries,
                    size_t entry_count);
    bool (*get_open_file_acl)(void* file_context,
                              AclEntry* entries,
                              size_t max_entries,
                              size_t& out_count);
    bool (*get_open_directory_acl)(void* dir_context,
                                   AclEntry* entries,
                                   size_t max_entries,
                                   size_t& out_count);
};

void init();
bool register_mount(const char* name,
                    const FilesystemOps* ops,
                    void* fs_context);
void set_root_mount(const char* name);
const char* root_mount_name();
bool has_explicit_mount_prefix(const char* path);
const char* mount_name_for_alias(const char* alias, size_t alias_length);
const char* volume_alias_for_mount(const char* name, size_t name_length);
size_t enumerate_mounts(const char** names, size_t max_names);

bool list(const char* path,
          DirEntry* entries,
          size_t max_entries,
          size_t& out_count);
bool read_file(const char* path,
               void* buffer,
               size_t buffer_size,
               size_t& out_size);

bool open_file(const char* path, FileHandle& out_handle);
bool open_file(const char* path,
               FileHandle& out_handle,
               AclSnapshot* out_acl);
bool create_file(const char* path, FileHandle& out_handle);
bool create_directory(const char* path);
bool remove_file(const char* path);
bool remove_directory(const char* path);
void close_file(FileHandle& handle);
bool read_file(FileHandle& handle,
               uint64_t offset,
               void* buffer,
               size_t buffer_size,
               size_t& out_size);
bool write_file(FileHandle& handle,
                uint64_t offset,
                const void* buffer,
                size_t buffer_size,
                size_t& out_size);

bool open_directory(const char* path, DirectoryHandle& out_handle);
bool open_directory(const char* path,
                    DirectoryHandle& out_handle,
                    AclSnapshot* out_acl);
bool read_directory(DirectoryHandle& handle, DirEntry& out_entry);
void close_directory(DirectoryHandle& handle);

bool acl_supported(const char* path);
bool get_acl(const char* path,
             AclEntry* entries,
             size_t max_entries,
             size_t& out_count);
bool set_acl(const char* path,
             const AclEntry* entries,
             size_t entry_count);

}  // namespace vfs
