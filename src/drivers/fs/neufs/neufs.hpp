#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../block_device.hpp"
#include "fs/vfs.hpp"

namespace neufs {

struct NeufsVolume {
    bool mounted;
    fs::BlockDevice device;
    uint64_t root_offset;
    uint64_t meta_size;
    uint64_t next_free_metadata;
    uint64_t next_free_data;
    char name[32];
    char preferred_alias[33];
    size_t rvt_size;
    // Bitmap bookkeeping (stored within the meta section)
    uint64_t data_bitmap_offset;      // offset from start of volume where data bitmap lives
    uint64_t data_bitmap_size_bytes;  // size in bytes of the data bitmap
    uint64_t meta_bitmap_offset;      // offset from start of volume where meta bitmap lives
    uint64_t meta_bitmap_size_bytes;  // size in bytes of the meta bitmap
    bool has_bitmaps;
};

bool neufs_mount(NeufsVolume& volume, const fs::BlockDevice& device);
const vfs::FilesystemOps& neufs_vfs_ops();

}  // namespace neufs
