#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vfs {
struct FileHandle;
}

namespace vm {

struct Region {
    uint64_t base;
    size_t length;
};

struct Stack {
    uint64_t base;
    uint64_t top;
    size_t length;
};

enum MapFlags : uint64_t {
    kMapWrite = 1ull << 0,
    kMapExecute = 1ull << 1,
};

enum class MappingKind : uint8_t {
    Anonymous,
    Private,
    Image,
    Stack,
    Shared,
    Device,
    FilePrivate,
};

struct Usage {
    uint64_t virtual_bytes;
    uint64_t resident_bytes;
    uint64_t shared_bytes;
    uint64_t file_bytes;
};

Region map_user_code(uint64_t cr3,
                     const uint8_t* data,
                     size_t length,
                     uint64_t entry_offset, uint64_t& entry_point);
Region reserve_user_region(size_t length);
Region reserve_user_region(uint64_t cr3,
                           size_t length,
                           MappingKind kind = MappingKind::Shared);
Region allocate_user_region(uint64_t cr3, size_t length);
Stack allocate_user_stack(uint64_t cr3, size_t length);
void release_user_region(uint64_t cr3, const Region& region);
void release_external_region(uint64_t cr3, const Region& region);
bool mark_region_resident(uint64_t cr3,
                          const Region& region,
                          size_t page_count);
void release_address_space(uint64_t cr3);
uint64_t map_anonymous(uint64_t cr3, size_t length, uint64_t flags);
uint64_t map_at(uint64_t cr3, uint64_t addr_hint, size_t length, uint64_t flags);
uint64_t map_file_private(uint64_t cr3,
                          const char* cache_key,
                          vfs::FileHandle& file,
                          uint64_t file_offset,
                          size_t length,
                          uint64_t flags);
bool unmap_region(uint64_t cr3, uint64_t addr, size_t length);
bool handle_page_fault(uint64_t cr3,
                       uint64_t address,
                       bool write,
                       bool execute);
bool set_user_region_writable(uint64_t cr3,
                              uint64_t addr,
                              size_t length,
                              bool writable);
bool set_user_region_executable(uint64_t cr3,
                                uint64_t addr,
                                size_t length,
                                bool executable);

inline constexpr uint64_t kUserAddressSpaceBase = 0x0000000040000000ull;
inline constexpr uint64_t kUserAddressSpaceTop = 0x00007ffffff00000ull;

bool is_user_range(uint64_t address, uint64_t length);
bool validate_user_buffer(uint64_t cr3,
                          uint64_t address,
                          size_t length,
                          bool writable);
bool copy_user_string(uint64_t cr3,
                      const char* user,
                      char* dest,
                      size_t dest_size);
bool copy_to_user(uint64_t cr3, uint64_t dest, const void* src, size_t length);
bool copy_from_user(uint64_t cr3, void* dest, uint64_t src, size_t length);
bool fill_user(uint64_t cr3, uint64_t dest, uint8_t value, size_t length);
Usage usage(uint64_t cr3);

}  // namespace vm
