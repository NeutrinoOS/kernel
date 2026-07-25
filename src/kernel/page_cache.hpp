#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vfs {
struct FileHandle;
}

namespace page_cache {

constexpr size_t kPageSize = 0x1000;
constexpr size_t kMaxCachedPages = 256;

bool acquire_private_page(const char* key,
                          vfs::FileHandle& file,
                          uint64_t file_offset,
                          uint64_t& out_phys);
void release_private_page(uint64_t phys);
size_t cached_pages();

}  // namespace page_cache
