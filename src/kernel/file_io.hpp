#pragma once

#include <stdint.h>

#include "process.hpp"

namespace file_io {

int32_t open_file(process::Process& proc, const char* path);
int32_t create_file(process::Process& proc, const char* path);
int32_t open_file_at(process::Process& proc,
                     uint32_t dir_handle,
                     const char* name);
int32_t create_file_at(process::Process& proc,
                       uint32_t dir_handle,
                       const char* name);
bool close_file(process::Process& proc, uint32_t handle);
bool sync_file(process::Process& proc, uint32_t handle);
int64_t read_file(process::Process& proc, uint32_t handle, uint64_t user_addr,
                  uint64_t length);
int64_t write_file(process::Process& proc, uint32_t handle, uint64_t user_addr,
                   uint64_t length);
uint64_t map_file_private(process::Process& proc,
                          uint32_t handle,
                          uint64_t file_offset,
                          size_t length,
                          uint64_t flags);

int32_t open_directory(process::Process& proc, const char* path);
int32_t open_directory_root(process::Process& proc);
int32_t open_directory_at(process::Process& proc,
                          uint32_t dir_handle,
                          const char* name);
bool create_directory(process::Process& proc, const char* path);
bool remove_file(process::Process& proc, const char* path);
bool remove_directory(process::Process& proc, const char* path);
bool close_directory(process::Process& proc, uint32_t handle);
int64_t read_directory(process::Process& proc, uint32_t handle,
                       uint64_t user_addr);
int64_t get_acl(process::Process& proc,
                const char* path,
                uint64_t user_entries,
                uint64_t max_entries);
bool set_acl(process::Process& proc,
             const char* path,
             uint64_t user_entries,
             uint64_t entry_count);

}  // namespace file_io
