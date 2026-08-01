#pragma once

#include <stdint.h>

#include "process.hpp"

namespace file_io {

enum OpenFlags : uint32_t {
    OpenRead = 1u << 0,
    OpenWrite = 1u << 1,
    OpenCreate = 1u << 2,
    OpenExclusive = 1u << 3,
    OpenAppend = 1u << 4,
};

enum MetadataFlags : uint32_t {
    MetadataDirectory = 1u << 0,
};

struct Metadata {
    uint64_t size;
    uint64_t modified_nanoseconds;
    uint32_t flags;
    uint32_t reserved;
};

static_assert(sizeof(Metadata) == 24, "file metadata ABI mismatch");

// Legacy FileOpen ABI: require read access and retain write access when the
// ACL allows it. New callers should use the flag-aware overload.
int32_t open_file(process::Task& proc, const char* path);
int32_t open_file(process::Task& proc, const char* path, uint32_t flags);
int32_t create_file(process::Task& proc, const char* path);
int32_t open_file_at(process::Task& proc,
                     uint32_t dir_handle,
                     const char* name);
int32_t create_file_at(process::Task& proc,
                       uint32_t dir_handle,
                       const char* name);
bool close_file(process::Task& proc, uint32_t handle);
bool sync_file(process::Task& proc, uint32_t handle);
int64_t read_file(process::Task& proc, uint32_t handle, uint64_t user_addr,
                  uint64_t length);
int64_t read_file_at(process::Task& proc, uint32_t handle,
                     uint64_t user_addr, uint64_t length, uint64_t offset);
int64_t write_file(process::Task& proc, uint32_t handle, uint64_t user_addr,
                   uint64_t length);
int64_t seek_file(process::Task& proc, uint32_t handle,
                  int64_t offset, uint32_t whence);
bool stat_file(process::Task& proc, uint32_t handle, Metadata& metadata);
bool stat_path(process::Task& proc, const char* path, Metadata& metadata);
uint64_t map_file_private(process::Task& proc,
                          uint32_t handle,
                          uint64_t file_offset,
                          size_t length,
                          uint64_t flags);

int32_t open_directory(process::Task& proc, const char* path);
int32_t open_directory_root(process::Task& proc);
int32_t open_directory_at(process::Task& proc,
                          uint32_t dir_handle,
                          const char* name);
bool create_directory(process::Task& proc, const char* path);
bool remove_file(process::Task& proc, const char* path);
bool remove_directory(process::Task& proc, const char* path);
bool close_directory(process::Task& proc, uint32_t handle);
int64_t read_directory(process::Task& proc, uint32_t handle,
                       uint64_t user_addr);
int64_t get_acl(process::Task& proc,
                const char* path,
                uint64_t user_entries,
                uint64_t max_entries);
bool set_acl(process::Task& proc,
             const char* path,
             uint64_t user_entries,
             uint64_t entry_count);

}  // namespace file_io
