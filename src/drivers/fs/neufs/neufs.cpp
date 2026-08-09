#include "neufs.hpp"

#include <stddef.h>
#include <stdint.h>

#include "drivers/log/logging.hpp"
#include "lib/mem.hpp"

namespace neufs {

constexpr size_t kNeufsNameLength = 16;
constexpr size_t kNeufsDirNameLength = 256;
constexpr size_t kNeufsFileNameLength = 256;
constexpr uint64_t kNeufsMagicSize = 8;
constexpr uint8_t kNeufsMagic[kNeufsMagicSize] = {
    0x4E, 0x45, 0x55, 0x46, 0x53, 0x00, 0x77, 0x42};
constexpr int32_t kNeufsVersionLegacy = 1;
constexpr int32_t kNeufsVersion = 2;
constexpr uint8_t kTypeNdir = 0;
constexpr uint8_t kTypeFile = 1;

#pragma pack(push, 1)
struct NeufsRvt {
    char magic[8];
    int32_t version;
    char name[16];
    uint64_t root;
    char preferred_alias[32];
};

constexpr size_t kLegacyRvtSize = 8 + 4 + 16 + 8;

struct NeufsAclEntry {
    int64_t machine_id;
    int64_t local_id;
    uint8_t write;
    uint8_t read;
    uint8_t delete_flag;
    uint8_t acledit;
    uint8_t reserved[4];
};

struct NeufsNdir {
    uint8_t type;
    uint8_t reserved[7];
    char name[256];
    int64_t ctime;
    int64_t utime;
    uint64_t acl[32];
    uint64_t parent;
    uint64_t contents[64];
    uint64_t next;
    uint64_t last;
};

struct NeufsFile {
    uint8_t type;
    uint8_t reserved[7];
    char name[256];
    int64_t ctime;
    int64_t utime;
    int64_t atime;
    uint64_t size;
    uint64_t acl[32];
    uint64_t parent;
    uint64_t content[512];
};

struct NeufsDcblk {
    uint64_t dcptrs[512];
};

struct NeufsDcptr {
    uint64_t start;
    uint64_t len;
    uint64_t crc64;
};
#pragma pack(pop)

static_assert(sizeof(NeufsRvt) % 4 == 0, "NEUFS header must align to 32 bits");
static_assert(sizeof(NeufsNdir) % 4 == 0, "NEUFS ndir must align to 32 bits");
static_assert(sizeof(NeufsFile) % 4 == 0, "NEUFS file must align to 32 bits");
static_assert(sizeof(NeufsDcblk) % 4 == 0, "NEUFS dcblk must align to 32 bits");
static_assert(sizeof(NeufsDcptr) % 4 == 0, "NEUFS dcptr must align to 32 bits");

constexpr size_t kMaxOpenFiles = 64;
constexpr size_t kMaxOpenDirectories = 32;
constexpr size_t kMaxPathLength = 512;
constexpr size_t kSectorScratchSize = 4096;

struct NeufsFileContext {
    neufs::NeufsVolume* volume;
    uint64_t file_offset;
    NeufsFile entry;
};

struct NeufsDirectoryContext {
    neufs::NeufsVolume* volume;
    uint64_t current_dir_offset;
    size_t next_index;
};

NeufsFileContext g_file_contexts[kMaxOpenFiles];
bool g_file_context_used[kMaxOpenFiles];

NeufsDirectoryContext g_directory_contexts[kMaxOpenDirectories];
bool g_directory_context_used[kMaxOpenDirectories];

inline uint64_t align_up(uint64_t value, uint64_t alignment) {
    if (alignment == 0) {
        return value;
    }
    uint64_t remainder = value % alignment;
    if (remainder == 0) {
        return value;
    }
    return value + (alignment - remainder);
}

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

bool string_equals(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    size_t index = 0;
    while (a[index] != '\0' && b[index] != '\0') {
        if (a[index] != b[index]) {
            return false;
        }
        ++index;
    }
    return a[index] == '\0' && b[index] == '\0';
}

bool compare_magic(const char* magic) {
    for (size_t i = 0; i < kNeufsMagicSize; ++i) {
        if (static_cast<uint8_t>(magic[i]) != kNeufsMagic[i]) {
            return false;
        }
    }
    return true;
}

bool read_sector(const fs::BlockDevice& device,
                 uint32_t lba,
                 void* buffer) {
    fs::BlockIoStatus status = fs::block_read(device, lba, 1, buffer);
    if (status != fs::BlockIoStatus::Ok) {
        log_message(LogLevel::Error,
                    "NEUFS: failed to read sector %u (status %d)",
                    lba,
                    static_cast<int>(status));
        return false;
    }
    return true;
}

bool read_sectors(const fs::BlockDevice& device,
                  uint32_t lba,
                  uint8_t count,
                  void* buffer) {
    fs::BlockIoStatus status = fs::block_read(device, lba, count, buffer);
    if (status != fs::BlockIoStatus::Ok) {
        log_message(LogLevel::Error,
                    "NEUFS: failed to read sectors %u (+%u) (status %d)",
                    lba,
                    count,
                    static_cast<int>(status));
        return false;
    }
    return true;
}

bool write_sector(const fs::BlockDevice& device,
                  uint32_t lba,
                  const void* buffer) {
    fs::BlockIoStatus status = fs::block_write(device, lba, 1, buffer);
    if (status != fs::BlockIoStatus::Ok) {
        log_message(LogLevel::Error,
                    "NEUFS: failed to write sector %u (status %d)",
                    lba,
                    static_cast<int>(status));
        return false;
    }
    return true;
}

bool write_sectors(const fs::BlockDevice& device,
                   uint32_t lba,
                   uint8_t count,
                   const void* buffer) {
    fs::BlockIoStatus status = fs::block_write(device, lba, count, buffer);
    if (status != fs::BlockIoStatus::Ok) {
        log_message(LogLevel::Error,
                    "NEUFS: failed to write sectors %u (+%u) (status %d)",
                    lba,
                    count,
                    static_cast<int>(status));
        return false;
    }
    return true;
}

bool read_bytes(const fs::BlockDevice& device,
                uint64_t offset,
                void* buffer,
                size_t size) {
    if (buffer == nullptr) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    uint64_t total_size = device.sector_size * device.sector_count;
    if (offset > total_size || offset + size > total_size) {
        return false;
    }

    uint8_t temp[kSectorScratchSize];
    uint8_t* out = static_cast<uint8_t*>(buffer);
    uint64_t sector_size = device.sector_size;
    uint64_t sector = offset / sector_size;
    size_t offset_in_sector = static_cast<size_t>(offset % sector_size);

    if (offset_in_sector != 0) {
        if (!read_sector(device, static_cast<uint32_t>(sector), temp)) {
            return false;
        }
        size_t chunk = sector_size - offset_in_sector;
        if (chunk > size) {
            chunk = size;
        }
        memcpy(out, temp + offset_in_sector, chunk);
        out += chunk;
        size -= chunk;
        ++sector;
    }

    while (size >= sector_size) {
        uint32_t sectors = static_cast<uint32_t>(size / sector_size);
        if (sectors > 255) {
            sectors = 255;
        }
        if (!read_sectors(device, static_cast<uint32_t>(sector),
                          static_cast<uint8_t>(sectors), out)) {
            return false;
        }
        size -= static_cast<size_t>(sectors) * sector_size;
        out += static_cast<size_t>(sectors) * sector_size;
        sector += sectors;
    }

    if (size > 0) {
        if (!read_sector(device, static_cast<uint32_t>(sector), temp)) {
            return false;
        }
        memcpy(out, temp, size);
    }
    return true;
}

bool write_bytes(const fs::BlockDevice& device,
                 uint64_t offset,
                 const void* buffer,
                 size_t size) {
    if (buffer == nullptr) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    uint64_t total_size = device.sector_size * device.sector_count;
    if (offset > total_size || offset + size > total_size) {
        return false;
    }

    uint8_t temp[kSectorScratchSize];
    const uint8_t* src = static_cast<const uint8_t*>(buffer);
    uint64_t sector_size = device.sector_size;
    uint64_t sector = offset / sector_size;
    size_t offset_in_sector = static_cast<size_t>(offset % sector_size);

    if (offset_in_sector != 0) {
        if (!read_sector(device, static_cast<uint32_t>(sector), temp)) {
            return false;
        }
        size_t chunk = sector_size - offset_in_sector;
        if (chunk > size) {
            chunk = size;
        }
        memcpy(temp + offset_in_sector, src, chunk);
        if (!write_sector(device, static_cast<uint32_t>(sector), temp)) {
            return false;
        }
        src += chunk;
        size -= chunk;
        ++sector;
    }

    while (size >= sector_size) {
        uint32_t sectors = static_cast<uint32_t>(size / sector_size);
        if (sectors > 255) {
            sectors = 255;
        }
        size_t bytes = static_cast<size_t>(sectors) * sector_size;
        if (!write_sectors(device, static_cast<uint32_t>(sector),
                           static_cast<uint8_t>(sectors), src)) {
            return false;
        }
        src += bytes;
        size -= bytes;
        sector += sectors;
    }

    if (size > 0) {
        if (!read_sector(device, static_cast<uint32_t>(sector), temp)) {
            return false;
        }
        memcpy(temp, src, size);
        if (!write_sector(device, static_cast<uint32_t>(sector), temp)) {
            return false;
        }
    }

    return true;
}

bool zero_bytes(const fs::BlockDevice& device,
                uint64_t offset,
                size_t size) {
    if (size == 0) {
        return true;
    }
    uint8_t temp[kSectorScratchSize];
    memset(temp, 0, sizeof(temp));
    while (size > 0) {
        size_t write_size = (size > sizeof(temp)) ? sizeof(temp) : size;
        if (!write_bytes(device, offset, temp, write_size)) {
            return false;
        }
        offset += write_size;
        size -= write_size;
    }
    return true;
}

bool read_object(const fs::BlockDevice& device,
                 uint64_t offset,
                 void* object,
                 size_t object_size) {
    if (object == nullptr) {
        return false;
    }
    return read_bytes(device, offset, object, object_size);
}

bool write_object(const fs::BlockDevice& device,
                  uint64_t offset,
                  const void* object,
                  size_t object_size) {
    if (object == nullptr) {
        return false;
    }
    return write_bytes(device, offset, object, object_size);
}

bool load_rvt(const neufs::NeufsVolume& volume, NeufsRvt& out) {
    return read_object(volume.device, 0, &out, sizeof(out));
}

bool persist_rvt(const neufs::NeufsVolume& volume, const NeufsRvt& rvt) {
    return write_object(volume.device, 0, &rvt, sizeof(rvt));
}

bool load_ndir(const neufs::NeufsVolume& volume,
               uint64_t offset,
               NeufsNdir& out) {
    return read_object(volume.device, offset, &out, sizeof(out));
}

bool persist_ndir(const neufs::NeufsVolume& volume,
                  uint64_t offset,
                  const NeufsNdir& object) {
    return write_object(volume.device, offset, &object, sizeof(object));
}

bool load_file(const neufs::NeufsVolume& volume,
               uint64_t offset,
               NeufsFile& out) {
    return read_object(volume.device, offset, &out, sizeof(out));
}

bool persist_file(const neufs::NeufsVolume& volume,
                  uint64_t offset,
                  const NeufsFile& object) {
    return write_object(volume.device, offset, &object, sizeof(object));
}

bool load_acl_entry(const neufs::NeufsVolume& volume,
                    uint64_t offset,
                    NeufsAclEntry& out) {
    return read_object(volume.device, offset, &out, sizeof(out));
}

bool persist_acl_entry(const neufs::NeufsVolume& volume,
                       uint64_t offset,
                       const NeufsAclEntry& object) {
    return write_object(volume.device, offset, &object, sizeof(object));
}

bool load_dcblk(const neufs::NeufsVolume& volume,
                uint64_t offset,
                NeufsDcblk& out) {
    return read_object(volume.device, offset, &out, sizeof(out));
}

bool persist_dcblk(const neufs::NeufsVolume& volume,
                   uint64_t offset,
                   const NeufsDcblk& object) {
    return write_object(volume.device, offset, &object, sizeof(object));
}

bool load_dcptr(const neufs::NeufsVolume& volume,
                uint64_t offset,
                NeufsDcptr& out) {
    return read_object(volume.device, offset, &out, sizeof(out));
}

bool persist_dcptr(const neufs::NeufsVolume& volume,
                   uint64_t offset,
                   const NeufsDcptr& object) {
    return write_object(volume.device, offset, &object, sizeof(object));
}

// Bitmap helpers
static bool bitmap_get_bit(const fs::BlockDevice& device,
                           uint64_t bitmap_offset,
                           uint64_t bit_index,
                           bool& out_bit) {
    uint64_t byte_off = bitmap_offset + (bit_index / 8);
    uint8_t b = 0;
    if (!read_bytes(device, byte_off, &b, 1)) {
        return false;
    }
    out_bit = ((b >> (bit_index % 8)) & 1) != 0;
    return true;
}

static bool bitmap_set_bit(const fs::BlockDevice& device,
                           uint64_t bitmap_offset,
                           uint64_t bit_index) {
    uint64_t byte_off = bitmap_offset + (bit_index / 8);
    uint8_t b = 0;
    if (!read_bytes(device, byte_off, &b, 1)) {
        return false;
    }
    b |= static_cast<uint8_t>(1u << (bit_index % 8));
    if (!write_bytes(device, byte_off, &b, 1)) {
        return false;
    }
    return true;
}

static bool bitmap_clear_bit(const fs::BlockDevice& device,
                             uint64_t bitmap_offset,
                             uint64_t bit_index) {
    uint64_t byte_off = bitmap_offset + (bit_index / 8);
    uint8_t b = 0;
    if (!read_bytes(device, byte_off, &b, 1)) {
        return false;
    }
    b &= static_cast<uint8_t>(~(1u << (bit_index % 8)));
    if (!write_bytes(device, byte_off, &b, 1)) {
        return false;
    }
    return true;
}

static bool bitmap_set_range(const fs::BlockDevice& device,
                             uint64_t bitmap_offset,
                             uint64_t start_bit,
                             uint64_t count_bits) {
    for (uint64_t i = 0; i < count_bits; ++i) {
        if (!bitmap_set_bit(device, bitmap_offset, start_bit + i)) {
            return false;
        }
    }
    return true;
}

static bool bitmap_clear_range(const fs::BlockDevice& device,
                               uint64_t bitmap_offset,
                               uint64_t start_bit,
                               uint64_t count_bits) {
    for (uint64_t i = 0; i < count_bits; ++i) {
        if (!bitmap_clear_bit(device, bitmap_offset, start_bit + i)) {
            return false;
        }
    }
    return true;
}

static bool bitmap_range_all_set(const fs::BlockDevice& device,
                                 uint64_t bitmap_offset,
                                 uint64_t start_bit,
                                 uint64_t count_bits,
                                 bool& out_all_set) {
    out_all_set = false;
    uint64_t current_bit = start_bit;
    uint64_t end_bit = start_bit + count_bits;

    while (current_bit < end_bit && (current_bit % 8) != 0) {
        bool used = false;
        if (!bitmap_get_bit(device, bitmap_offset, current_bit, used)) {
            return false;
        }
        if (!used) {
            return true;
        }
        ++current_bit;
    }

    uint8_t buffer[kSectorScratchSize];
    while (current_bit + 8 <= end_bit) {
        uint64_t bytes_remaining = (end_bit - current_bit) / 8;
        size_t chunk = static_cast<size_t>(bytes_remaining);
        if (chunk > sizeof(buffer)) {
            chunk = sizeof(buffer);
        }
        if (!read_bytes(device, bitmap_offset + (current_bit / 8), buffer, chunk)) {
            return false;
        }
        for (size_t i = 0; i < chunk; ++i) {
            if (buffer[i] != 0xFF) {
                return true;
            }
        }
        current_bit += static_cast<uint64_t>(chunk) * 8;
    }

    while (current_bit < end_bit) {
        bool used = false;
        if (!bitmap_get_bit(device, bitmap_offset, current_bit, used)) {
            return false;
        }
        if (!used) {
            return true;
        }
        ++current_bit;
    }

    out_all_set = true;
    return true;
}

static bool bitmap_find_run(const fs::BlockDevice& device,
                            uint64_t bitmap_offset,
                            uint64_t bitmap_bits,
                            uint64_t run_bits,
                            uint64_t& out_start_bit) {
    if (run_bits == 0) {
        out_start_bit = 0;
        return true;
    }
    uint64_t consecutive = 0;
    for (uint64_t bit = 0; bit < bitmap_bits; ++bit) {
        bool used = false;
        if (!bitmap_get_bit(device, bitmap_offset, bit, used)) {
            return false;
        }
        if (!used) {
            ++consecutive;
            if (consecutive == run_bits) {
                out_start_bit = bit - run_bits + 1;
                return true;
            }
        } else {
            consecutive = 0;
        }
    }
    return false;
}

static bool bitmap_find_run_from(const fs::BlockDevice& device,
                                 uint64_t bitmap_offset,
                                 uint64_t bitmap_bits,
                                 uint64_t start_bit,
                                 uint64_t run_bits,
                                 uint64_t& out_start_bit) {
    if (run_bits == 0) {
        out_start_bit = start_bit;
        return start_bit <= bitmap_bits;
    }
    if (start_bit >= bitmap_bits) {
        return false;
    }

    uint64_t consecutive = 0;
    for (uint64_t bit = start_bit; bit < bitmap_bits; ++bit) {
        bool used = false;
        if (!bitmap_get_bit(device, bitmap_offset, bit, used)) {
            return false;
        }
        if (!used) {
            ++consecutive;
            if (consecutive == run_bits) {
                out_start_bit = bit - run_bits + 1;
                return true;
            }
        } else {
            consecutive = 0;
        }
    }
    return false;
}

static bool bitmap_find_first_clear_from(const fs::BlockDevice& device,
                                         uint64_t bitmap_offset,
                                         uint64_t bitmap_bits,
                                         uint64_t start_bit,
                                         uint64_t& out_bit) {
    if (start_bit >= bitmap_bits) {
        return false;
    }

    uint64_t current_bit = start_bit;
    while (current_bit < bitmap_bits && (current_bit % 8) != 0) {
        bool used = false;
        if (!bitmap_get_bit(device, bitmap_offset, current_bit, used)) {
            return false;
        }
        if (!used) {
            out_bit = current_bit;
            return true;
        }
        ++current_bit;
    }

    uint8_t buffer[kSectorScratchSize];
    while (current_bit + 8 <= bitmap_bits) {
        uint64_t bytes_remaining = (bitmap_bits - current_bit) / 8;
        size_t chunk = static_cast<size_t>(bytes_remaining);
        if (chunk > sizeof(buffer)) {
            chunk = sizeof(buffer);
        }
        if (!read_bytes(device, bitmap_offset + (current_bit / 8), buffer, chunk)) {
            return false;
        }
        for (size_t byte_index = 0; byte_index < chunk; ++byte_index) {
            if (buffer[byte_index] == 0xFF) {
                continue;
            }
            uint64_t byte_bit =
                current_bit + static_cast<uint64_t>(byte_index) * 8;
            for (uint8_t bit = 0; bit < 8; ++bit) {
                uint64_t candidate = byte_bit + bit;
                if (candidate >= bitmap_bits) {
                    return false;
                }
                if ((buffer[byte_index] & (1u << bit)) == 0) {
                    out_bit = candidate;
                    return true;
                }
            }
        }
        current_bit += static_cast<uint64_t>(chunk) * 8;
    }

    while (current_bit < bitmap_bits) {
        bool used = false;
        if (!bitmap_get_bit(device, bitmap_offset, current_bit, used)) {
            return false;
        }
        if (!used) {
            out_bit = current_bit;
            return true;
        }
        ++current_bit;
    }

    return false;
}

[[maybe_unused]] static bool bitmap_reserved_ranges_valid(
    const neufs::NeufsVolume& volume) {
    if (!volume.has_bitmaps) {
        return false;
    }

    uint64_t used_data_sectors =
        align_up(volume.meta_size, volume.device.sector_size) /
        volume.device.sector_size;
    bool range_set = false;
    if (!bitmap_range_all_set(volume.device,
                              volume.data_bitmap_offset,
                              0,
                              used_data_sectors,
                              range_set) ||
        !range_set) {
        return false;
    }

    uint64_t rvt_blocks = align_up(volume.rvt_size, 8) / 8;
    uint64_t data_bitmap_blocks =
        align_up(volume.data_bitmap_size_bytes, 8) / 8;
    uint64_t meta_bitmap_blocks =
        align_up(volume.meta_bitmap_size_bytes, 8) / 8;
    uint64_t root_blocks = align_up(sizeof(NeufsNdir), 8) / 8;

    if (!bitmap_range_all_set(volume.device,
                              volume.meta_bitmap_offset,
                              0,
                              rvt_blocks,
                              range_set) ||
        !range_set) {
        return false;
    }
    if (!bitmap_range_all_set(volume.device,
                              volume.meta_bitmap_offset,
                              volume.data_bitmap_offset / 8,
                              data_bitmap_blocks,
                              range_set) ||
        !range_set) {
        return false;
    }
    if (!bitmap_range_all_set(volume.device,
                              volume.meta_bitmap_offset,
                              volume.meta_bitmap_offset / 8,
                              meta_bitmap_blocks,
                              range_set) ||
        !range_set) {
        return false;
    }
    if (!bitmap_range_all_set(volume.device,
                              volume.meta_bitmap_offset,
                              volume.root_offset / 8,
                              root_blocks,
                              range_set) ||
        !range_set) {
        return false;
    }

    return true;
}

bool mark_metadata_bytes(neufs::NeufsVolume& volume, uint64_t offset, size_t size) {
    if (size == 0) {
        return true;
    }
    uint64_t end = align_up(offset + size, 8);
    if (end > volume.next_free_metadata && end <= volume.meta_size) {
        volume.next_free_metadata = end;
    }
    if (!volume.has_bitmaps || volume.meta_bitmap_size_bytes == 0) {
        return true;
    }
    uint64_t start_block = offset / 8;
    uint64_t block_count = (size + 7) / 8;
    return bitmap_set_range(volume.device, volume.meta_bitmap_offset, start_block, block_count);
}

bool mark_data_bytes(neufs::NeufsVolume& volume, uint64_t offset, size_t size) {
    if (size == 0) {
        return true;
    }
    uint64_t end = align_up(offset + size, volume.device.sector_size);
    uint64_t total_bytes = volume.device.sector_size * volume.device.sector_count;
    if (end > volume.next_free_data && end <= total_bytes) {
        volume.next_free_data = end;
    }
    if (!volume.has_bitmaps || volume.data_bitmap_size_bytes == 0) {
        return true;
    }
    uint64_t start_sector = offset / volume.device.sector_size;
    uint64_t sector_count = (size + volume.device.sector_size - 1) /
                            volume.device.sector_size;
    return bitmap_set_range(volume.device, volume.data_bitmap_offset, start_sector, sector_count);
}

bool mark_acl_entries(neufs::NeufsVolume& volume, const uint64_t* acl_entries) {
    if (acl_entries == nullptr) {
        return false;
    }
    for (size_t index = 0; index < 32; ++index) {
        if (acl_entries[index] == 0) {
            continue;
        }
        if (!mark_metadata_bytes(volume, acl_entries[index], sizeof(NeufsAclEntry))) {
            return false;
        }
    }
    return true;
}

bool mark_existing_file_storage(neufs::NeufsVolume& volume, const NeufsFile& file) {
    if (!mark_acl_entries(volume, file.acl)) {
        return false;
    }

    for (size_t dcblk_index = 0; dcblk_index < 512; ++dcblk_index) {
        uint64_t dcblk_offset = file.content[dcblk_index];
        if (dcblk_offset == 0) {
            continue;
        }
        if (!mark_metadata_bytes(volume, dcblk_offset, sizeof(NeufsDcblk))) {
            return false;
        }

        NeufsDcblk dcblk{};
        if (!load_dcblk(volume, dcblk_offset, dcblk)) {
            return false;
        }
        for (size_t dcptr_index = 0; dcptr_index < 512; ++dcptr_index) {
            uint64_t dcptr_offset = dcblk.dcptrs[dcptr_index];
            if (dcptr_offset == 0) {
                continue;
            }
            if (!mark_metadata_bytes(volume, dcptr_offset, sizeof(NeufsDcptr))) {
                return false;
            }

            NeufsDcptr dcptr{};
            if (!load_dcptr(volume, dcptr_offset, dcptr)) {
                return false;
            }
            if (!mark_data_bytes(volume, dcptr.start, static_cast<size_t>(dcptr.len))) {
                return false;
            }
        }
    }
    return true;
}

bool mark_existing_directory_tree(neufs::NeufsVolume& volume, uint64_t dir_offset) {
    uint64_t current = dir_offset;
    while (current != 0) {
        if (!mark_metadata_bytes(volume, current, sizeof(NeufsNdir))) {
            return false;
        }

        NeufsNdir dir{};
        if (!load_ndir(volume, current, dir)) {
            return false;
        }
        if (!mark_acl_entries(volume, dir.acl)) {
            return false;
        }

        for (size_t index = 0; index < 64; ++index) {
            uint64_t child = dir.contents[index];
            if (child == 0) {
                continue;
            }

            uint8_t entry_type = 0xFF;
            if (!read_bytes(volume.device, child, &entry_type, 1)) {
                return false;
            }
            if (entry_type == kTypeNdir) {
                if (!mark_existing_directory_tree(volume, child)) {
                    return false;
                }
            } else if (entry_type == kTypeFile) {
                if (!mark_metadata_bytes(volume, child, sizeof(NeufsFile))) {
                    return false;
                }
                NeufsFile file{};
                if (!load_file(volume, child, file)) {
                    return false;
                }
                if (!mark_existing_file_storage(volume, file)) {
                    return false;
                }
            }
        }
        current = dir.next;
    }
    return true;
}

// Free helpers
static bool free_data_bytes(neufs::NeufsVolume& volume, uint64_t offset, size_t size) {
    if (!volume.has_bitmaps || volume.data_bitmap_size_bytes == 0) {
        return false;
    }
    uint64_t start_sector = offset / volume.device.sector_size;
    uint64_t sector_count = (size + volume.device.sector_size - 1) / volume.device.sector_size;
    return bitmap_clear_range(volume.device, volume.data_bitmap_offset, start_sector, sector_count);
}

static bool extend_data_bytes(neufs::NeufsVolume& volume,
                              uint64_t offset,
                              size_t old_size,
                              size_t new_size) {
    if (new_size <= old_size) {
        return true;
    }

    size_t old_aligned =
        static_cast<size_t>(align_up(old_size, volume.device.sector_size));
    size_t new_aligned =
        static_cast<size_t>(align_up(new_size, volume.device.sector_size));
    if (new_aligned <= old_aligned) {
        return true;
    }

    uint64_t extension_offset = offset + old_aligned;
    uint64_t extension_size = new_aligned - old_aligned;
    uint64_t disk_size = volume.device.sector_size * volume.device.sector_count;
    if (extension_offset > disk_size || extension_offset + extension_size > disk_size) {
        return false;
    }

    uint64_t start_sector = extension_offset / volume.device.sector_size;
    uint64_t sector_count = extension_size / volume.device.sector_size;

    if (volume.has_bitmaps && volume.data_bitmap_size_bytes > 0) {
        for (uint64_t sector = 0; sector < sector_count; ++sector) {
            bool used = true;
            if (!bitmap_get_bit(volume.device,
                                volume.data_bitmap_offset,
                                start_sector + sector,
                                used) ||
                used) {
                return false;
            }
        }
        if (!bitmap_set_range(volume.device,
                              volume.data_bitmap_offset,
                              start_sector,
                              sector_count)) {
            return false;
        }
        uint64_t extension_end = extension_offset + extension_size;
        if (extension_end > volume.next_free_data) {
            volume.next_free_data = extension_end;
        }
        return true;
    }

    if (volume.next_free_data != extension_offset) {
        return false;
    }
    volume.next_free_data += extension_size;
    return true;
}

static bool free_metadata_bytes(neufs::NeufsVolume& volume, uint64_t offset, size_t size) {
    if (!volume.has_bitmaps || volume.meta_bitmap_size_bytes == 0) {
        return false;
    }
    uint64_t start_block = offset / 8;
    uint64_t block_count = (size + 7) / 8;
    return bitmap_clear_range(volume.device, volume.meta_bitmap_offset, start_block, block_count);
}

bool allocate_metadata_bytes(neufs::NeufsVolume& volume,
                             size_t size,
                             uint64_t& out_offset) {
    size = static_cast<size_t>(align_up(size, 8));

    if (volume.has_bitmaps && volume.meta_bitmap_size_bytes > 0) {
        uint64_t blocks_needed = (size + 7) / 8;
        uint64_t meta_bits = volume.meta_size / 8;
        uint64_t start_bit = 0;
        uint64_t cursor_bit = volume.next_free_metadata / 8;
        if (!bitmap_find_run_from(volume.device,
                                  volume.meta_bitmap_offset,
                                  meta_bits,
                                  cursor_bit,
                                  blocks_needed,
                                  start_bit) &&
            !bitmap_find_run(volume.device,
                             volume.meta_bitmap_offset,
                             meta_bits,
                             blocks_needed,
                             start_bit)) {
            return false;
        }
        if (!bitmap_set_range(volume.device,
                              volume.meta_bitmap_offset,
                              start_bit,
                              blocks_needed)) {
            return false;
        }
        out_offset = start_bit * 8;
        uint64_t next_clear = 0;
        if (bitmap_find_first_clear_from(volume.device,
                                         volume.meta_bitmap_offset,
                                         meta_bits,
                                         start_bit + blocks_needed,
                                         next_clear)) {
            volume.next_free_metadata = next_clear * 8;
        } else {
            volume.next_free_metadata = volume.meta_size;
        }
        return true;
    }

    if (volume.next_free_metadata + size <= volume.meta_size) {
        out_offset = volume.next_free_metadata;
        volume.next_free_metadata += size;
        return true;
    }

    return false;
}

bool allocate_data_bytes(neufs::NeufsVolume& volume,
                         size_t size,
                         uint64_t& out_offset) {
    if (size == 0) {
        out_offset = volume.next_free_data;
        return true;
    }
    size = static_cast<size_t>(align_up(size, volume.device.sector_size));
    uint64_t sector_count_needed = size / volume.device.sector_size;

    uint64_t total_bytes = volume.device.sector_size * volume.device.sector_count;
    if (volume.has_bitmaps && volume.data_bitmap_size_bytes > 0) {
        uint64_t data_bits = volume.device.sector_count;
        uint64_t start_bit = 0;
        uint64_t cursor_bit = volume.next_free_data / volume.device.sector_size;
        if (!bitmap_find_run_from(volume.device,
                                  volume.data_bitmap_offset,
                                  data_bits,
                                  cursor_bit,
                                  sector_count_needed,
                                  start_bit) &&
            !bitmap_find_run(volume.device,
                             volume.data_bitmap_offset,
                             data_bits,
                             sector_count_needed,
                             start_bit)) {
            return false;
        }
        if (!bitmap_set_range(volume.device,
                              volume.data_bitmap_offset,
                              start_bit,
                              sector_count_needed)) {
            return false;
        }
        out_offset = start_bit * volume.device.sector_size;
        uint64_t next_clear = 0;
        if (bitmap_find_first_clear_from(volume.device,
                                         volume.data_bitmap_offset,
                                         data_bits,
                                         start_bit + sector_count_needed,
                                         next_clear)) {
            volume.next_free_data = next_clear * volume.device.sector_size;
        } else {
            volume.next_free_data = total_bytes;
        }
        return true;
    }

    if (volume.next_free_data + size <= total_bytes) {
        out_offset = volume.next_free_data;
        volume.next_free_data += size;
        return true;
    }

    return false;
}

bool ensure_name(char* dest, size_t dest_size, const char* source) {
    if (dest == nullptr || source == nullptr || dest_size == 0) {
        return false;
    }
    size_t length = string_length(source);
    if (length >= dest_size) {
        length = dest_size - 1;
    }
    memcpy(dest, source, length);
    dest[length] = '\0';
    if (length + 1 < dest_size) {
        memset(dest + length + 1, 0, dest_size - length - 1);
    }
    return true;
}

bool read_entry_name(const neufs::NeufsVolume& volume,
                     uint64_t entry_offset,
                     char* out_name,
                     size_t name_size) {
    if (out_name == nullptr || name_size == 0) {
        return false;
    }
    if (name_size > kNeufsDirNameLength) {
        name_size = kNeufsDirNameLength;
    }
    if (!read_bytes(volume.device, entry_offset + 8, out_name, name_size)) {
        return false;
    }
    out_name[name_size - 1] = '\0';
    return true;
}

bool directory_contains_name(const neufs::NeufsVolume& volume,
                             uint64_t dir_offset,
                             const char* name,
                             uint64_t& out_entry_offset,
                             uint8_t& out_type) {
    if (name == nullptr || *name == '\0') {
        return false;
    }

    uint64_t current = dir_offset;
    while (current != 0) {
        NeufsNdir dir{};
        if (!load_ndir(volume, current, dir)) {
            return false;
        }

        for (size_t index = 0; index < 64; ++index) {
            uint64_t candidate = dir.contents[index];
            if (candidate == 0) {
                continue;
            }

            uint8_t entry_type = 0xFF;
            if (!read_bytes(volume.device, candidate, &entry_type, 1)) {
                return false;
            }
            if (entry_type != kTypeNdir && entry_type != kTypeFile) {
                continue;
            }

            char candidate_name[kNeufsDirNameLength];
            if (!read_entry_name(volume, candidate, candidate_name, sizeof(candidate_name))) {
                return false;
            }
            if (string_equals(candidate_name, name)) {
                out_entry_offset = candidate;
                out_type = entry_type;
                return true;
            }
        }
        current = dir.next;
    }

    return false;
}

bool add_entry_to_directory(neufs::NeufsVolume& volume,
                            uint64_t dir_offset,
                            uint64_t entry_offset) {
    uint64_t current = dir_offset;
    while (current != 0) {
        NeufsNdir dir{};
        if (!load_ndir(volume, current, dir)) {
            return false;
        }

        for (size_t index = 0; index < 64; ++index) {
            if (dir.contents[index] == 0) {
                dir.contents[index] = entry_offset;
                return persist_ndir(volume, current, dir);
            }
        }

        if (dir.next == 0) {
            uint64_t extension_offset = 0;
            if (!allocate_metadata_bytes(volume, sizeof(NeufsNdir), extension_offset)) {
                return false;
            }
            NeufsNdir extension{};
            extension.type = kTypeNdir;
            extension.parent = dir.parent;
            extension.last = current;
            if (!persist_ndir(volume, extension_offset, extension)) {
                return false;
            }
            dir.next = extension_offset;
            if (!persist_ndir(volume, current, dir)) {
                return false;
            }
            current = extension_offset;
            continue;
        }
        current = dir.next;
    }

    return false;
}

bool remove_entry_from_directory(neufs::NeufsVolume& volume,
                                 uint64_t dir_offset,
                                 uint64_t entry_offset) {
    uint64_t current = dir_offset;
    while (current != 0) {
        NeufsNdir dir{};
        if (!load_ndir(volume, current, dir)) {
            return false;
        }

        for (size_t index = 0; index < 64; ++index) {
            if (dir.contents[index] == entry_offset) {
                dir.contents[index] = 0;
                return persist_ndir(volume, current, dir);
            }
        }
        current = dir.next;
    }

    return false;
}

bool directory_is_empty(const neufs::NeufsVolume& volume,
                        uint64_t dir_offset) {
    uint64_t current = dir_offset;
    while (current != 0) {
        NeufsNdir dir{};
        if (!load_ndir(volume, current, dir)) {
            return false;
        }
        for (size_t index = 0; index < 64; ++index) {
            if (dir.contents[index] != 0) {
                return false;
            }
        }
        current = dir.next;
    }
    return true;
}

bool resolve_path(const neufs::NeufsVolume& volume,
                  const char* path,
                  uint64_t& out_offset,
                  uint8_t& out_type) {
    if (path == nullptr || *path == '\0') {
        out_offset = volume.root_offset;
        out_type = kTypeNdir;
        return true;
    }

    char segment[kMaxPathLength];
    const char* current = path;
    uint64_t directory_offset = volume.root_offset;

    while (*current != '\0') {
        size_t segment_length = 0;
        while (*current != '\0' && *current != '/') {
            if (segment_length + 1 < sizeof(segment)) {
                segment[segment_length++] = *current;
            }
            ++current;
        }
        segment[segment_length] = '\0';

        while (*current == '/') {
            ++current;
        }

        if (segment_length == 0) {
            continue;
        }

        uint64_t next_offset = 0;
        uint8_t next_type = 0xFF;
        if (!directory_contains_name(volume, directory_offset, segment,
                                     next_offset, next_type)) {
            return false;
        }
        if (*current == '\0') {
            out_offset = next_offset;
            out_type = next_type;
            return true;
        }
        if (next_type != kTypeNdir) {
            return false;
        }
        directory_offset = next_offset;
    }

    return false;
}

bool resolve_parent(const neufs::NeufsVolume& volume,
                    const char* path,
                    uint64_t& out_parent,
                    char* out_name,
                    size_t name_size) {
    if (path == nullptr || *path == '\0') {
        return false;
    }

    size_t length = string_length(path);
    const char* last_slash = nullptr;
    for (size_t index = 0; index < length; ++index) {
        if (path[index] == '/') {
            last_slash = path + index;
        }
    }

    if (last_slash == nullptr) {
        out_parent = volume.root_offset;
        return ensure_name(out_name, name_size, path);
    }

    size_t name_length = length - static_cast<size_t>(last_slash - path) - 1;
    if (name_length == 0) {
        return false;
    }

    if (!ensure_name(out_name, name_size, last_slash + 1)) {
        return false;
    }

    char parent_path[kMaxPathLength];
    size_t parent_length = static_cast<size_t>(last_slash - path);
    if (parent_length >= sizeof(parent_path)) {
        return false;
    }
    memcpy(parent_path, path, parent_length);
    parent_path[parent_length] = '\0';

    uint8_t parent_type = 0;
    if (!resolve_path(volume, parent_path, out_parent, parent_type)) {
        return false;
    }
    return parent_type == kTypeNdir;
}

bool to_vfs_entry(const neufs::NeufsVolume& volume,
                  uint64_t entry_offset,
                  vfs::DirEntry& out_entry) {
    uint8_t entry_type = 0xFF;
    if (!read_bytes(volume.device, entry_offset, &entry_type, 1)) {
        return false;
    }

    memset(&out_entry, 0, sizeof(out_entry));
    if (entry_type == kTypeNdir) {
        NeufsNdir dir{};
        if (!load_ndir(volume, entry_offset, dir)) {
            return false;
        }
        ensure_name(out_entry.name, sizeof(out_entry.name), dir.name);
        out_entry.flags = vfs::kDirEntryFlagDirectory;
        out_entry.size = 0;
        return true;
    }
    if (entry_type == kTypeFile) {
        NeufsFile file{};
        if (!load_file(volume, entry_offset, file)) {
            return false;
        }
        ensure_name(out_entry.name, sizeof(out_entry.name), file.name);
        out_entry.flags = 0;
        out_entry.size = file.size;
        return true;
    }

    return false;
}

bool read_file_data(const neufs::NeufsVolume& volume,
                    const NeufsFile& file,
                    uint64_t offset,
                    void* buffer,
                    size_t size,
                    size_t& out_size) {
    out_size = 0;
    if (file.size == 0 || offset >= file.size || size == 0) {
        return true;
    }

    uint64_t target_end = offset + size;
    if (target_end > file.size) {
        target_end = file.size;
    }

    uint8_t* dst = static_cast<uint8_t*>(buffer);
    uint64_t current_position = 0;
    uint64_t remaining = target_end - offset;

    for (size_t dcblk_index = 0; dcblk_index < 512 && remaining > 0;
         ++dcblk_index) {
        uint64_t dcblk_pointer = file.content[dcblk_index];
        if (dcblk_pointer == 0) {
            continue;
        }

        NeufsDcblk dcblk{};
        if (!load_dcblk(volume, dcblk_pointer, dcblk)) {
            return false;
        }

        for (size_t dcptr_index = 0;
             dcptr_index < 512 && remaining > 0;
             ++dcptr_index) {
            uint64_t dcptr_pointer = dcblk.dcptrs[dcptr_index];
            if (dcptr_pointer == 0) {
                continue;
            }

            NeufsDcptr dcptr{};
            if (!load_dcptr(volume, dcptr_pointer, dcptr)) {
                return false;
            }
            if (dcptr.len == 0) {
                continue;
            }
            uint64_t segment_end = current_position + dcptr.len;
            if (segment_end <= offset) {
                current_position = segment_end;
                continue;
            }

            uint64_t source_offset = dcptr.start;
            uint64_t segment_offset = 0;
            if (offset > current_position) {
                segment_offset = offset - current_position;
                source_offset += segment_offset;
            }

            uint64_t available = dcptr.len - segment_offset;
            if (available > remaining) {
                available = remaining;
            }

            if (!read_bytes(volume.device, source_offset, dst,
                            static_cast<size_t>(available))) {
                return false;
            }
            dst += static_cast<size_t>(available);
            out_size += static_cast<size_t>(available);
            remaining -= available;
            current_position = segment_end;
        }
    }

    return true;
}

bool copy_existing_file_data(const neufs::NeufsVolume& volume,
                             const NeufsFile& file,
                             uint64_t destination_offset,
                             uint64_t copy_size) {
    if (copy_size == 0) {
        return true;
    }
    uint64_t current_position = 0;
    uint8_t buffer[kSectorScratchSize];

    for (size_t dcblk_index = 0; dcblk_index < 512 && copy_size > 0;
         ++dcblk_index) {
        uint64_t dcblk_pointer = file.content[dcblk_index];
        if (dcblk_pointer == 0) {
            continue;
        }

        NeufsDcblk dcblk{};
        if (!load_dcblk(volume, dcblk_pointer, dcblk)) {
            return false;
        }

        for (size_t dcptr_index = 0;
             dcptr_index < 512 && copy_size > 0;
             ++dcptr_index) {
            uint64_t dcptr_pointer = dcblk.dcptrs[dcptr_index];
            if (dcptr_pointer == 0) {
                continue;
            }

            NeufsDcptr dcptr{};
            if (!load_dcptr(volume, dcptr_pointer, dcptr)) {
                return false;
            }
            if (dcptr.len == 0) {
                continue;
            }
            uint64_t segment_end = current_position + dcptr.len;

            uint64_t available = dcptr.len;
            if (available > copy_size) {
                available = copy_size;
            }

            uint64_t source_offset = dcptr.start;
            uint64_t destination_cursor = destination_offset + current_position;
            uint64_t remaining = available;
            while (remaining > 0) {
                size_t chunk = (remaining > sizeof(buffer)) ? sizeof(buffer) : static_cast<size_t>(remaining);
                if (!read_bytes(volume.device, source_offset, buffer, chunk)) {
                    return false;
                }
                if (!write_bytes(volume.device, destination_cursor, buffer, chunk)) {
                    return false;
                }
                source_offset += chunk;
                destination_cursor += chunk;
                remaining -= chunk;
            }
            copy_size -= available;
            current_position = segment_end;
        }
    }

    return true;
}

bool free_file_storage(neufs::NeufsVolume& volume, const NeufsFile& file) {
    bool ok = true;
    for (size_t dcblk_index = 0; dcblk_index < 512; ++dcblk_index) {
        uint64_t dcblk_offset = file.content[dcblk_index];
        if (dcblk_offset == 0) {
            continue;
        }

        NeufsDcblk dcblk{};
        if (!load_dcblk(volume, dcblk_offset, dcblk)) {
            ok = false;
        } else {
            for (size_t dcptr_index = 0; dcptr_index < 512; ++dcptr_index) {
                uint64_t dcptr_offset = dcblk.dcptrs[dcptr_index];
                if (dcptr_offset == 0) {
                    continue;
                }

                NeufsDcptr dcptr{};
                if (!load_dcptr(volume, dcptr_offset, dcptr)) {
                    ok = false;
                } else if (dcptr.len != 0 &&
                           !free_data_bytes(volume,
                                            dcptr.start,
                                            static_cast<size_t>(dcptr.len))) {
                    ok = false;
                }

                if (!free_metadata_bytes(volume, dcptr_offset, sizeof(NeufsDcptr))) {
                    ok = false;
                }
            }
        }

        if (!free_metadata_bytes(volume, dcblk_offset, sizeof(NeufsDcblk))) {
            ok = false;
        }
    }
    return ok;
}

bool load_single_extent(const neufs::NeufsVolume& volume,
                        const NeufsFile& file,
                        uint64_t& out_dcptr_offset,
                        NeufsDcptr& out_dcptr) {
    if (file.content[0] == 0) {
        return false;
    }
    for (size_t index = 1; index < 512; ++index) {
        if (file.content[index] != 0) {
            return false;
        }
    }

    NeufsDcblk dcblk{};
    if (!load_dcblk(volume, file.content[0], dcblk)) {
        return false;
    }
    if (dcblk.dcptrs[0] == 0) {
        return false;
    }
    for (size_t index = 1; index < 512; ++index) {
        if (dcblk.dcptrs[index] != 0) {
            return false;
        }
    }

    NeufsDcptr dcptr{};
    if (!load_dcptr(volume, dcblk.dcptrs[0], dcptr)) {
        return false;
    }
    if (dcptr.len != file.size) {
        return false;
    }

    out_dcptr_offset = dcblk.dcptrs[0];
    out_dcptr = dcptr;
    return true;
}

bool write_single_extent_file(neufs::NeufsVolume& volume,
                              uint64_t file_offset,
                              NeufsFile& file,
                              uint64_t write_offset,
                              const void* buffer,
                              size_t buffer_size,
                              size_t& out_size) {
    uint64_t target_end = write_offset + buffer_size;
    if (target_end < write_offset) {
        return false;
    }

    uint64_t dcptr_offset = 0;
    NeufsDcptr dcptr{};
    if (!load_single_extent(volume, file, dcptr_offset, dcptr)) {
        return false;
    }

    if (target_end > file.size) {
        if (write_offset != file.size) {
            return false;
        }
        if (!extend_data_bytes(volume,
                               dcptr.start,
                               static_cast<size_t>(file.size),
                               static_cast<size_t>(target_end))) {
            return false;
        }
    }

    if (!write_bytes(volume.device, dcptr.start + write_offset, buffer, buffer_size)) {
        return false;
    }

    if (target_end > file.size) {
        dcptr.len = target_end;
        file.size = target_end;
        if (!persist_dcptr(volume, dcptr_offset, dcptr) ||
            !persist_file(volume, file_offset, file)) {
            return false;
        }
    }

    out_size = buffer_size;
    return true;
}

bool write_file_contents(neufs::NeufsVolume& volume,
                         uint64_t file_offset,
                         NeufsFile& file,
                         uint64_t write_offset,
                         const void* buffer,
                         size_t buffer_size,
                         size_t& out_size) {
    out_size = 0;
    if (buffer == nullptr) {
        return false;
    }
    if (buffer_size == 0) {
        out_size = 0;
        return true;
    }
    if (write_offset + buffer_size < write_offset) {
        return false;
    }

    uint64_t current_size = file.size;
    uint64_t target_end = write_offset + buffer_size;
    uint64_t new_size = current_size;
    if (target_end > current_size) {
        new_size = target_end;
    }

    if (new_size > static_cast<uint64_t>(static_cast<size_t>(new_size))) {
        return false;
    }

    if (current_size != 0 &&
        write_single_extent_file(volume,
                                 file_offset,
                                 file,
                                 write_offset,
                                 buffer,
                                 buffer_size,
                                 out_size)) {
        return true;
    }

    uint64_t data_offset = 0;
    if (!allocate_data_bytes(volume, static_cast<size_t>(new_size), data_offset)) {
        return false;
    }

    if (current_size > 0) {
        if (!copy_existing_file_data(volume, file, data_offset, current_size)) {
            return false;
        }
    }

    if (current_size < write_offset) {
        uint64_t gap_size = write_offset - current_size;
        if (!zero_bytes(volume.device, data_offset + current_size,
                        static_cast<size_t>(gap_size))) {
            return false;
        }
    }

    if (buffer_size > 0) {
        if (!write_bytes(volume.device, data_offset + write_offset, buffer,
                         buffer_size)) {
            return false;
        }
    }

    uint64_t dcblk_offset = 0;
    if (!allocate_metadata_bytes(volume, sizeof(NeufsDcblk), dcblk_offset)) {
        return false;
    }
    uint64_t dcptr_offset = 0;
    if (!allocate_metadata_bytes(volume, sizeof(NeufsDcptr), dcptr_offset)) {
        return false;
    }

    NeufsDcblk dcblk{};
    NeufsDcptr first_ptr{};
    first_ptr.start = data_offset;
    first_ptr.len = new_size;
    first_ptr.crc64 = 0;
    dcblk.dcptrs[0] = dcptr_offset;

    if (!persist_dcptr(volume, dcptr_offset, first_ptr)) {
        return false;
    }
    if (!persist_dcblk(volume, dcblk_offset, dcblk)) {
        return false;
    }

    NeufsFile old_file = file;
    file.content[0] = dcblk_offset;
    for (size_t index = 1; index < 512; ++index) {
        file.content[index] = 0;
    }
    file.size = new_size;

    if (!persist_file(volume, file_offset, file)) {
        return false;
    }

    free_file_storage(volume, old_file);

    out_size = buffer_size;
    return true;
}

bool open_file_context(NeufsFileContext*& out_context) {
    for (size_t index = 0; index < kMaxOpenFiles; ++index) {
        if (!g_file_context_used[index]) {
            g_file_context_used[index] = true;
            out_context = &g_file_contexts[index];
            out_context->volume = nullptr;
            out_context->file_offset = 0;
            memset(&out_context->entry, 0, sizeof(out_context->entry));
            return true;
        }
    }
    return false;
}

void close_file_context(NeufsFileContext* context) {
    if (context == nullptr) {
        return;
    }
    size_t index = static_cast<size_t>(context - g_file_contexts);
    if (index < kMaxOpenFiles) {
        g_file_context_used[index] = false;
    }
}

NeufsDirectoryContext* allocate_directory_context() {
    for (size_t index = 0; index < kMaxOpenDirectories; ++index) {
        if (!g_directory_context_used[index]) {
            g_directory_context_used[index] = true;
            g_directory_contexts[index].volume = nullptr;
            g_directory_contexts[index].current_dir_offset = 0;
            g_directory_contexts[index].next_index = 0;
            return &g_directory_contexts[index];
        }
    }
    return nullptr;
}

void release_directory_context(NeufsDirectoryContext* context) {
    if (context == nullptr) {
        return;
    }
    size_t index = static_cast<size_t>(context - g_directory_contexts);
    if (index < kMaxOpenDirectories) {
        g_directory_context_used[index] = false;
    }
}

bool valid_acl_value(vfs::AclValue value) {
    return value == vfs::AclValue::Deny ||
           value == vfs::AclValue::Allow ||
           value == vfs::AclValue::Default;
}

bool load_acl_entries(const neufs::NeufsVolume& volume,
                      const uint64_t* acl_offsets,
                      vfs::AclEntry* entries,
                      size_t max_entries,
                      size_t& out_count) {
    out_count = 0;
    if (acl_offsets == nullptr) {
        return false;
    }
    if (entries == nullptr) {
        return max_entries == 0;
    }
    for (size_t index = 0; index < vfs::kMaxAclEntries; ++index) {
        uint64_t acl_offset = acl_offsets[index];
        if (acl_offset == 0) {
            continue;
        }
        if (out_count >= max_entries) {
            return false;
        }
        NeufsAclEntry disk_entry{};
        if (!load_acl_entry(volume, acl_offset, disk_entry)) {
            return false;
        }
        vfs::AclEntry& entry = entries[out_count++];
        // IDs are opaque 64-bit values. The v1 disk format spelled these
        // fields as int64_t, so preserve their complete bit patterns.
        memcpy(&entry.machine_id,
               &disk_entry.machine_id,
               sizeof(entry.machine_id));
        memcpy(&entry.local_id,
               &disk_entry.local_id,
               sizeof(entry.local_id));
        entry.write = static_cast<vfs::AclValue>(disk_entry.write);
        entry.read = static_cast<vfs::AclValue>(disk_entry.read);
        entry.delete_permission =
            static_cast<vfs::AclValue>(disk_entry.delete_flag);
        entry.edit = static_cast<vfs::AclValue>(disk_entry.acledit);
        memset(entry.reserved, 0, sizeof(entry.reserved));
        if (!valid_acl_value(entry.write) || !valid_acl_value(entry.read) ||
            !valid_acl_value(entry.delete_permission) ||
            !valid_acl_value(entry.edit)) {
            return false;
        }
    }
    return true;
}

bool neufs_list_directory(void* fs_context,
                          const char* path,
                          vfs::DirEntry* entries,
                          size_t max_entries,
                          size_t& out_count) {
    out_count = 0;
    if (fs_context == nullptr || entries == nullptr || max_entries == 0) {
        return false;
    }

    auto* volume = static_cast<neufs::NeufsVolume*>(fs_context);
    uint64_t directory_offset = 0;
    uint8_t entry_type = 0;
    if (!resolve_path(*volume, path, directory_offset, entry_type)) {
        return false;
    }
    if (entry_type != kTypeNdir) {
        return false;
    }

    uint64_t current = directory_offset;
    size_t collected = 0;
    while (current != 0 && collected < max_entries) {
        NeufsNdir dir{};
        if (!load_ndir(*volume, current, dir)) {
            return false;
        }
        for (size_t index = 0; index < 64 && collected < max_entries;
             ++index) {
            uint64_t child = dir.contents[index];
            if (child == 0) {
                continue;
            }
            if (!to_vfs_entry(*volume, child, entries[collected])) {
                return false;
            }
            ++collected;
        }
        current = dir.next;
    }
    out_count = collected;
    return true;
}

bool neufs_open_file(void* fs_context,
                     const char* path,
                     void*& out_file_context,
                     vfs::DirEntry* out_metadata) {
    if (fs_context == nullptr || path == nullptr || *path == '\0') {
        return false;
    }

    auto* volume = static_cast<neufs::NeufsVolume*>(fs_context);
    uint64_t entry_offset = 0;
    uint8_t entry_type = 0;
    if (!resolve_path(*volume, path, entry_offset, entry_type)) {
        return false;
    }
    if (entry_type != kTypeFile) {
        return false;
    }

    NeufsFile file{};
    if (!load_file(*volume, entry_offset, file)) {
        return false;
    }
    NeufsFileContext* context = nullptr;
    if (!open_file_context(context)) {
        log_message(LogLevel::Warn, "NEUFS: out of file contexts");
        return false;
    }

    context->volume = volume;
    context->file_offset = entry_offset;
    context->entry = file;
    out_file_context = context;

    if (out_metadata != nullptr) {
        out_metadata->flags = 0;
        ensure_name(out_metadata->name, sizeof(out_metadata->name), file.name);
        out_metadata->size = file.size;
        out_metadata->reserved = 0;
    }
    return true;
}

bool neufs_create_file(void* fs_context,
                       const char* path,
                       void*& out_file_context,
                       vfs::DirEntry* out_metadata) {
    if (fs_context == nullptr || path == nullptr || *path == '\0') {
        return false;
    }

    auto* volume = static_cast<neufs::NeufsVolume*>(fs_context);
    uint64_t parent_offset = 0;
    char name[kNeufsFileNameLength];
    if (!resolve_parent(*volume, path, parent_offset, name, sizeof(name))) {
        return false;
    }

    uint64_t existing_offset = 0;
    uint8_t existing_type = 0;
    if (resolve_path(*volume, path, existing_offset, existing_type)) {
        return false;
    }

    uint64_t file_offset = 0;
    if (!allocate_metadata_bytes(*volume, sizeof(NeufsFile), file_offset)) {
        return false;
    }

    NeufsFile file{};
    file.type = kTypeFile;
    file.ctime = 0;
    file.utime = 0;
    file.atime = 0;
    file.size = 0;
    file.parent = parent_offset;
    ensure_name(file.name, sizeof(file.name), name);

    if (!persist_file(*volume, file_offset, file)) {
        return false;
    }

    if (!add_entry_to_directory(*volume, parent_offset, file_offset)) {
        return false;
    }

    NeufsFileContext* context = nullptr;
    if (!open_file_context(context)) {
        log_message(LogLevel::Warn, "NEUFS: out of file contexts");
        return false;
    }
    context->volume = volume;
    context->file_offset = file_offset;
    context->entry = file;
    out_file_context = context;

    if (out_metadata != nullptr) {
        out_metadata->flags = 0;
        ensure_name(out_metadata->name, sizeof(out_metadata->name), file.name);
        out_metadata->size = file.size;
        out_metadata->reserved = 0;
    }
    return true;
}

bool neufs_create_directory(void* fs_context, const char* path) {
    if (fs_context == nullptr || path == nullptr || *path == '\0') {
        return false;
    }

    auto* volume = static_cast<neufs::NeufsVolume*>(fs_context);
    uint64_t parent_offset = 0;
    char name[kNeufsDirNameLength];
    if (!resolve_parent(*volume, path, parent_offset, name, sizeof(name))) {
        return false;
    }

    uint64_t existing_offset = 0;
    uint8_t existing_type = 0;
    if (resolve_path(*volume, path, existing_offset, existing_type)) {
        return false;
    }

    uint64_t dir_offset = 0;
    if (!allocate_metadata_bytes(*volume, sizeof(NeufsNdir), dir_offset)) {
        return false;
    }

    NeufsNdir dir{};
    dir.type = kTypeNdir;
    dir.parent = parent_offset;
    ensure_name(dir.name, sizeof(dir.name), name);

    if (!persist_ndir(*volume, dir_offset, dir)) {
        return false;
    }

    return add_entry_to_directory(*volume, parent_offset, dir_offset);
}

bool neufs_remove_file(void* fs_context, const char* path) {
    if (fs_context == nullptr || path == nullptr || *path == '\0') {
        return false;
    }

    auto* volume = static_cast<neufs::NeufsVolume*>(fs_context);
    uint64_t entry_offset = 0;
    uint8_t entry_type = 0;
    if (!resolve_path(*volume, path, entry_offset, entry_type)) {
        return false;
    }
    if (entry_type != kTypeFile) {
        return false;
    }

    NeufsFile file{};
    if (!load_file(*volume, entry_offset, file)) {
        return false;
    }
    if (file.parent == 0) {
        return false;
    }

    if (!free_file_storage(*volume, file)) {
        log_message(LogLevel::Warn,
                    "NEUFS: failed to free all storage for %s",
                    path);
    }

    for (uint64_t acl_offset : file.acl) {
        if (acl_offset != 0) {
            (void)free_metadata_bytes(*volume,
                                      acl_offset,
                                      sizeof(NeufsAclEntry));
        }
    }

    if (!free_metadata_bytes(*volume, entry_offset, sizeof(NeufsFile))) {
        log_message(LogLevel::Warn, "NEUFS: failed to free file metadata at %llu", entry_offset);
    }

    return remove_entry_from_directory(*volume, file.parent, entry_offset);
}

bool neufs_remove_directory(void* fs_context, const char* path) {
    if (fs_context == nullptr || path == nullptr || *path == '\0') {
        return false;
    }

    auto* volume = static_cast<neufs::NeufsVolume*>(fs_context);
    uint64_t entry_offset = 0;
    uint8_t entry_type = 0;
    if (!resolve_path(*volume, path, entry_offset, entry_type)) {
        return false;
    }
    if (entry_type != kTypeNdir) {
        return false;
    }
    if (entry_offset == volume->root_offset) {
        return false;
    }

    if (!directory_is_empty(*volume, entry_offset)) {
        return false;
    }

    NeufsNdir dir{};
    if (!load_ndir(*volume, entry_offset, dir)) {
        return false;
    }
    if (dir.parent == 0) {
        return false;
    }

    for (uint64_t acl_offset : dir.acl) {
        if (acl_offset != 0) {
            (void)free_metadata_bytes(*volume,
                                      acl_offset,
                                      sizeof(NeufsAclEntry));
        }
    }

    // Free ndir metadata
    if (!free_metadata_bytes(*volume, entry_offset, sizeof(NeufsNdir))) {
        log_message(LogLevel::Warn, "NEUFS: failed to free ndir metadata at %llu", entry_offset);
    }

    return remove_entry_from_directory(*volume, dir.parent, entry_offset);
}

bool neufs_read_file(void* file_context,
                     uint64_t offset,
                     void* buffer,
                     size_t buffer_size,
                     size_t& out_size) {
    out_size = 0;
    if (file_context == nullptr || buffer == nullptr) {
        return false;
    }

    auto* context = static_cast<NeufsFileContext*>(file_context);
    return read_file_data(*context->volume, context->entry, offset, buffer,
                          buffer_size, out_size);
}

bool neufs_write_file(void* file_context,
                      uint64_t offset,
                      const void* buffer,
                      size_t buffer_size,
                      size_t& out_size) {
    out_size = 0;
    if (file_context == nullptr || buffer == nullptr) {
        return false;
    }

    auto* context = static_cast<NeufsFileContext*>(file_context);
    if (!write_file_contents(*context->volume, context->file_offset,
                             context->entry, offset, buffer, buffer_size,
                             out_size)) {
        return false;
    }

    context->entry.size = (offset + out_size > context->entry.size)
                               ? offset + out_size
                               : context->entry.size;
    return true;
}

void neufs_close_file(void* file_context) {
    if (file_context == nullptr) {
        return;
    }
    auto* context = static_cast<NeufsFileContext*>(file_context);
    close_file_context(context);
}

bool neufs_open_directory(void* fs_context,
                          const char* path,
                          void*& out_dir_context) {
    if (fs_context == nullptr) {
        return false;
    }

    auto* volume = static_cast<neufs::NeufsVolume*>(fs_context);
    uint64_t directory_offset = 0;
    uint8_t entry_type = 0;
    if (!resolve_path(*volume, path, directory_offset, entry_type)) {
        return false;
    }
    if (entry_type != kTypeNdir) {
        return false;
    }
    NeufsDirectoryContext* ctx = allocate_directory_context();
    if (ctx == nullptr) {
        log_message(LogLevel::Warn, "NEUFS: out of directory contexts");
        return false;
    }

    ctx->volume = volume;
    ctx->current_dir_offset = directory_offset;
    ctx->next_index = 0;
    out_dir_context = ctx;
    return true;
}

bool neufs_directory_next(void* dir_context, vfs::DirEntry& out_entry) {
    if (dir_context == nullptr) {
        return false;
    }

    auto* ctx = static_cast<NeufsDirectoryContext*>(dir_context);
    while (ctx->current_dir_offset != 0) {
        NeufsNdir dir{};
        if (!load_ndir(*ctx->volume, ctx->current_dir_offset, dir)) {
            return false;
        }

        while (ctx->next_index < 64) {
            uint64_t child = dir.contents[ctx->next_index];
            ++ctx->next_index;
            if (child == 0) {
                continue;
            }
            if (!to_vfs_entry(*ctx->volume, child, out_entry)) {
                return false;
            }
            return true;
        }

        ctx->current_dir_offset = dir.next;
        ctx->next_index = 0;
    }
    return false;
}

void neufs_close_directory(void* dir_context) {
    if (dir_context == nullptr) {
        return;
    }
    release_directory_context(static_cast<NeufsDirectoryContext*>(dir_context));
}

bool neufs_get_open_file_acl(void* file_context,
                             vfs::AclEntry* entries,
                             size_t max_entries,
                             size_t& out_count) {
    if (file_context == nullptr) {
        return false;
    }
    auto* context = static_cast<NeufsFileContext*>(file_context);
    return load_acl_entries(*context->volume,
                            context->entry.acl,
                            entries,
                            max_entries,
                            out_count);
}

bool neufs_get_open_directory_acl(void* dir_context,
                                  vfs::AclEntry* entries,
                                  size_t max_entries,
                                  size_t& out_count) {
    if (dir_context == nullptr) {
        return false;
    }
    auto* context = static_cast<NeufsDirectoryContext*>(dir_context);
    NeufsNdir dir{};
    if (!load_ndir(*context->volume,
                   context->current_dir_offset,
                   dir)) {
        return false;
    }
    return load_acl_entries(*context->volume,
                            dir.acl,
                            entries,
                            max_entries,
                            out_count);
}

bool neufs_get_acl(void* fs_context,
                   const char* path,
                   vfs::AclEntry* entries,
                   size_t max_entries,
                   size_t& out_count) {
    out_count = 0;
    if (fs_context == nullptr || entries == nullptr || max_entries == 0) {
        return false;
    }
    auto* volume = static_cast<neufs::NeufsVolume*>(fs_context);
    uint64_t entry_offset = 0;
    uint8_t entry_type = 0;
    if (!resolve_path(*volume, path, entry_offset, entry_type)) {
        return false;
    }

    uint64_t acl_offsets[vfs::kMaxAclEntries]{};
    if (entry_type == kTypeFile) {
        NeufsFile file{};
        if (!load_file(*volume, entry_offset, file)) {
            return false;
        }
        memcpy(acl_offsets, file.acl, sizeof(acl_offsets));
    } else if (entry_type == kTypeNdir) {
        NeufsNdir dir{};
        if (!load_ndir(*volume, entry_offset, dir)) {
            return false;
        }
        memcpy(acl_offsets, dir.acl, sizeof(acl_offsets));
    } else {
        return false;
    }

    return load_acl_entries(*volume,
                            acl_offsets,
                            entries,
                            max_entries,
                            out_count);
}

bool neufs_set_acl(void* fs_context,
                   const char* path,
                   const vfs::AclEntry* entries,
                   size_t entry_count) {
    if (fs_context == nullptr || entry_count > vfs::kMaxAclEntries ||
        (entries == nullptr && entry_count != 0)) {
        return false;
    }
    for (size_t i = 0; i < entry_count; ++i) {
        if (!valid_acl_value(entries[i].write) ||
            !valid_acl_value(entries[i].read) ||
            !valid_acl_value(entries[i].delete_permission) ||
            !valid_acl_value(entries[i].edit)) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (entries[i].machine_id == entries[j].machine_id &&
                entries[i].local_id == entries[j].local_id) {
                return false;
            }
        }
    }

    auto* volume = static_cast<neufs::NeufsVolume*>(fs_context);
    uint64_t object_offset = 0;
    uint8_t entry_type = 0;
    if (!resolve_path(*volume, path, object_offset, entry_type)) {
        return false;
    }

    uint64_t old_offsets[vfs::kMaxAclEntries]{};
    uint64_t new_offsets[vfs::kMaxAclEntries]{};
    NeufsFile file{};
    NeufsNdir dir{};
    if (entry_type == kTypeFile) {
        if (!load_file(*volume, object_offset, file)) {
            return false;
        }
        memcpy(old_offsets, file.acl, sizeof(old_offsets));
    } else if (entry_type == kTypeNdir) {
        if (!load_ndir(*volume, object_offset, dir)) {
            return false;
        }
        memcpy(old_offsets, dir.acl, sizeof(old_offsets));
    } else {
        return false;
    }

    for (size_t i = 0; i < entry_count; ++i) {
        if (!allocate_metadata_bytes(*volume,
                                     sizeof(NeufsAclEntry),
                                     new_offsets[i])) {
            goto rollback;
        }
        NeufsAclEntry disk_entry{};
        memcpy(&disk_entry.machine_id,
               &entries[i].machine_id,
               sizeof(disk_entry.machine_id));
        memcpy(&disk_entry.local_id,
               &entries[i].local_id,
               sizeof(disk_entry.local_id));
        disk_entry.write = static_cast<uint8_t>(entries[i].write);
        disk_entry.read = static_cast<uint8_t>(entries[i].read);
        disk_entry.delete_flag =
            static_cast<uint8_t>(entries[i].delete_permission);
        disk_entry.acledit = static_cast<uint8_t>(entries[i].edit);
        if (!persist_acl_entry(*volume, new_offsets[i], disk_entry)) {
            goto rollback;
        }
    }

    if (entry_type == kTypeFile) {
        memcpy(file.acl, new_offsets, sizeof(new_offsets));
        if (!persist_file(*volume, object_offset, file)) {
            goto rollback;
        }
    } else {
        memcpy(dir.acl, new_offsets, sizeof(new_offsets));
        if (!persist_ndir(*volume, object_offset, dir)) {
            goto rollback;
        }
    }

    for (uint64_t old_offset : old_offsets) {
        if (old_offset != 0) {
            (void)free_metadata_bytes(*volume,
                                      old_offset,
                                      sizeof(NeufsAclEntry));
        }
    }
    return true;

rollback:
    for (uint64_t new_offset : new_offsets) {
        if (new_offset != 0) {
            (void)free_metadata_bytes(*volume,
                                      new_offset,
                                      sizeof(NeufsAclEntry));
        }
    }
    return false;
}

bool neufs_mount(neufs::NeufsVolume& volume, const fs::BlockDevice& device) {
    memset(&volume, 0, sizeof(volume));
    volume.device = device;
    volume.mounted = false;

    NeufsRvt rvt{};
    if (!read_object(device, 0, &rvt, sizeof(rvt))) {
        return false;
    }
    if (!compare_magic(rvt.magic)) {
        return false;
    }
    if (rvt.version != kNeufsVersionLegacy && rvt.version != kNeufsVersion) {
        return false;
    }
    if (rvt.root == 0) {
        return false;
    }

    const uint64_t total_bytes = device.sector_size * device.sector_count;
    uint64_t suggested_meta = (total_bytes * 225) / 10000;
    const uint64_t min_meta = 256ull * 1024ull * 1024ull;
    const uint64_t max_meta = 16ull * 1024ull * 1024ull * 1024ull;
    if (suggested_meta < min_meta && total_bytes >= min_meta * 2) {
        suggested_meta = min_meta;
    }
    if (suggested_meta > max_meta) {
        suggested_meta = max_meta;
    }
    if (device.sector_size != 0 && suggested_meta > total_bytes - device.sector_size) {
        suggested_meta = total_bytes - device.sector_size;
    } else if (device.sector_size == 0 && suggested_meta > total_bytes) {
        suggested_meta = total_bytes;
    }

    volume.rvt_size = rvt.version == kNeufsVersion ? sizeof(NeufsRvt)
                                                   : kLegacyRvtSize;
    volume.root_offset = rvt.root;
    volume.meta_size = align_up(suggested_meta, device.sector_size);
    if (volume.meta_size > total_bytes) {
        return false;
    }
    if (volume.meta_size < volume.rvt_size) {
        return false;
    }
    if (volume.root_offset < volume.rvt_size ||
        volume.root_offset + sizeof(NeufsNdir) > volume.meta_size) {
        return false;
    }

    volume.next_free_metadata = align_up(volume.root_offset + sizeof(NeufsNdir), 8);
    if (volume.next_free_metadata < volume.rvt_size) {
        volume.next_free_metadata = volume.rvt_size;
    }
    if (volume.next_free_metadata > volume.meta_size) {
        volume.next_free_metadata = volume.meta_size;
    }

    volume.next_free_data = align_up(volume.meta_size, device.sector_size);
    if (volume.next_free_data < volume.meta_size) {
        volume.next_free_data = align_up(volume.meta_size,
                                        device.sector_size);
    }
    if (volume.next_free_data > total_bytes) {
        return false;
    }

    size_t volume_name_length = 0;
    while (volume_name_length < sizeof(rvt.name) &&
           rvt.name[volume_name_length] != '\0') {
        ++volume_name_length;
    }
    memcpy(volume.name, rvt.name, volume_name_length);
    volume.name[volume_name_length] = '\0';
    if (rvt.version == kNeufsVersion) {
        size_t alias_length = 0;
        while (alias_length < sizeof(rvt.preferred_alias) &&
               rvt.preferred_alias[alias_length] != '\0') {
            ++alias_length;
        }
        memcpy(volume.preferred_alias, rvt.preferred_alias, alias_length);
        volume.preferred_alias[alias_length] = '\0';
    }

    // Compute and initialize bitmaps inside the meta section if there is space.
    volume.has_bitmaps = false;
    volume.data_bitmap_offset = 0;
    volume.data_bitmap_size_bytes = 0;
    volume.meta_bitmap_offset = 0;
    volume.meta_bitmap_size_bytes = 0;

    // Data bitmap: one bit per sector
    uint64_t data_bitmap_bytes = (device.sector_count + 7) / 8;
    // Meta bitmap: track 8-byte blocks inside meta
    uint64_t meta_blocks = volume.meta_size / 8;
    uint64_t meta_bitmap_bytes = (meta_blocks + 7) / 8;

    uint64_t meta_cursor = align_up(volume.rvt_size, 8);
    uint64_t proposed_data_bitmap_offset = meta_cursor;
    uint64_t proposed_meta_bitmap_offset = align_up(meta_cursor + data_bitmap_bytes, 8);

    uint64_t bitmap_end = proposed_meta_bitmap_offset + meta_bitmap_bytes;
    if (bitmap_end <= volume.meta_size && bitmap_end <= volume.root_offset) {
        volume.data_bitmap_offset = proposed_data_bitmap_offset;
        volume.data_bitmap_size_bytes = data_bitmap_bytes;
        volume.meta_bitmap_offset = proposed_meta_bitmap_offset;
        volume.meta_bitmap_size_bytes = meta_bitmap_bytes;
        volume.has_bitmaps = true;
    }

    NeufsNdir root_dir{};
    if (!load_ndir(volume, volume.root_offset, root_dir)) {
        return false;
    }
    if (root_dir.type != kTypeNdir) {
        return false;
    }

    if (volume.has_bitmaps) {
        // The bitmap layout and bounds were validated above. Start at the
        // first legal allocation point instead of rescanning the large,
        // deliberately reserved prefix on every mount. On eMMC that prefix
        // scan dominated boot time.
        uint64_t next_meta_bit = 0;
        uint64_t meta_bits = volume.meta_size / 8;
        uint64_t first_meta_bit =
            align_up(volume.root_offset + sizeof(NeufsNdir), 8) / 8;
        if (bitmap_find_first_clear_from(volume.device,
                                         volume.meta_bitmap_offset,
                                         meta_bits,
                                         first_meta_bit,
                                         next_meta_bit)) {
            volume.next_free_metadata = next_meta_bit * 8;
        } else {
            volume.next_free_metadata = volume.meta_size;
        }

        uint64_t next_data_bit = 0;
        uint64_t first_data_bit =
            align_up(volume.meta_size, device.sector_size) /
            device.sector_size;
        if (bitmap_find_first_clear_from(volume.device,
                                         volume.data_bitmap_offset,
                                         device.sector_count,
                                         first_data_bit,
                                         next_data_bit)) {
            volume.next_free_data = next_data_bit * device.sector_size;
        } else {
            volume.next_free_data = total_bytes;
        }
    } else {
        volume.has_bitmaps = false;
        volume.data_bitmap_offset = 0;
        volume.data_bitmap_size_bytes = 0;
        volume.meta_bitmap_offset = 0;
        volume.meta_bitmap_size_bytes = 0;
        if (!mark_existing_directory_tree(volume, volume.root_offset)) {
            return false;
        }
    }

    volume.mounted = true;
    return true;
}

const vfs::FilesystemOps& neufs_vfs_ops() {
    static const auto get_volume_alias = [](void* fs_context) -> const char* {
        if (fs_context == nullptr) return nullptr;
        auto* volume = static_cast<neufs::NeufsVolume*>(fs_context);
        return volume->preferred_alias[0] != '\0' ? volume->preferred_alias
                                                   : nullptr;
    };
    static const vfs::FilesystemOps kOps = {
        get_volume_alias,
        &neufs_list_directory,
        &neufs_open_file,
        &neufs_create_file,
        &neufs_create_directory,
        &neufs_remove_file,
        &neufs_remove_directory,
        &neufs_read_file,
        &neufs_write_file,
        &neufs_close_file,
        &neufs_open_directory,
        &neufs_directory_next,
        &neufs_close_directory,
        &neufs_get_acl,
        &neufs_set_acl,
        &neufs_get_open_file_acl,
        &neufs_get_open_directory_acl,
    };
    return kOps;
}

}  // namespace
