#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../block_device.hpp"

namespace vfs {
struct FilesystemOps;
}

struct Fat32DirEntry {
    char name[64];
    uint8_t attributes;
    uint32_t first_cluster;
    uint32_t size;
    uint32_t directory_cluster;
    uint32_t raw_entry_index;
};

struct Fat32Volume {
    bool mounted;
    fs::BlockDevice device;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_size_sectors;
    uint32_t fat_begin_lba;
    uint32_t cluster_begin_lba;
    uint32_t root_dir_first_cluster;
    uint32_t num_fats;
    uint32_t total_sectors;
    uint32_t fs_info_sector;
    uint32_t total_clusters;
    uint32_t next_free_cluster;
    char preferred_alias[12];
};

bool fat32_mount(Fat32Volume& volume, const fs::BlockDevice& device);
bool fat32_list_directory(Fat32Volume& volume, uint32_t start_cluster,
                          Fat32DirEntry* out_entries, size_t max_entries,
                          size_t& out_count);
bool fat32_find_entry(Fat32Volume& volume, uint32_t directory_cluster,
                      const char* name, Fat32DirEntry& out_entry);
bool fat32_read_file(Fat32Volume& volume, const Fat32DirEntry& entry,
                     void* buffer, size_t buffer_size, size_t& out_size);
bool fat32_read_file_range(Fat32Volume& volume, const Fat32DirEntry& entry,
                           uint32_t offset, void* buffer, size_t buffer_size,
                           size_t& out_size);
bool fat32_get_entry_by_index(Fat32Volume& volume, uint32_t directory_cluster,
                              size_t index, Fat32DirEntry& out_entry);

const vfs::FilesystemOps& fat32_vfs_ops();
