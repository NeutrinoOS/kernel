#include "fat32.hpp"

#include <stddef.h>
#include <stdint.h>

#include "drivers/log/logging.hpp"
#include "fs/vfs.hpp"
#include "lib/mem.hpp"

namespace {

struct __attribute__((packed)) BpbFat32 {
    uint8_t jump[3];
    char oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t bk_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    char volume_label[11];
    char fs_type[8];
};

constexpr uint8_t ATTR_LONG_NAME = 0x0F;
constexpr uint8_t ATTR_DIRECTORY = 0x10;
constexpr uint8_t ATTR_VOLUME_ID = 0x08;
constexpr size_t kMaxSegmentLength = 128;
constexpr uint8_t kLfnSequenceMask = 0x1F;
constexpr uint8_t kLfnLastFlag = 0x40;
constexpr uint8_t kLfnType = 0x00;
constexpr uint8_t kLfnMaxEntries = 20;
constexpr size_t kLfnCharsPerEntry = 13;
constexpr size_t kMaxLfnLength = 128;

constexpr uint32_t kFatEntryMask = 0x0FFFFFFF;
constexpr uint32_t kFatEoc = 0x0FFFFFF8;
constexpr uint32_t kFatBadCluster = 0x0FFFFFF7;
constexpr uint32_t kFatFreeCluster = 0;

constexpr uint32_t kFsInfoLeadSignature = 0x41615252;
constexpr uint32_t kFsInfoStructSignature = 0x61417272;
constexpr uint32_t kFsInfoTrailSignature = 0xAA550000;

alignas(512) uint8_t sector_buffer[512];
alignas(512) uint8_t cluster_buffer[32768];
alignas(512) uint8_t fat_cache[512];
uint32_t fat_cache_sector = 0xFFFFFFFF;
const fs::BlockDevice* fat_cache_device = nullptr;

struct LfnState {
    char name[kMaxLfnLength];
    uint8_t checksum;
    uint8_t expected;
    bool active;
    bool complete;
    bool overflow;
};

inline uint32_t cluster_to_lba(const Fat32Volume& vol, uint32_t cluster) {
    return vol.cluster_begin_lba +
           ((cluster - 2) * vol.sectors_per_cluster);
}

bool read_sector(const fs::BlockDevice& device, uint32_t lba, void* buffer) {
    fs::BlockIoStatus status = fs::block_read(device, lba, 1, buffer);
    if (status != fs::BlockIoStatus::Ok) {
        log_message(LogLevel::Error,
                    "FAT32: failed to read sector %u (status %d)",
                    lba, static_cast<int>(status));
        return false;
    }
    return true;
}

bool read_sectors(const fs::BlockDevice& device, uint32_t lba, uint8_t count,
                  void* buffer) {
    fs::BlockIoStatus status = fs::block_read(device, lba, count, buffer);
    if (status != fs::BlockIoStatus::Ok) {
        log_message(LogLevel::Error,
                    "FAT32: failed to read sectors %u (+%u) (status %d)",
                    lba, count, static_cast<int>(status));
        return false;
    }
    return true;
}

bool write_sector(const fs::BlockDevice& device, uint32_t lba,
                  const void* buffer) {
    fs::BlockIoStatus status = fs::block_write(device, lba, 1, buffer);
    if (status != fs::BlockIoStatus::Ok) {
        log_message(LogLevel::Error,
                    "FAT32: failed to write sector %u (status %d)",
                    lba, static_cast<int>(status));
        return false;
    }
    return true;
}

bool write_sectors(const fs::BlockDevice& device, uint32_t lba, uint8_t count,
                   const void* buffer) {
    fs::BlockIoStatus status = fs::block_write(device, lba, count, buffer);
    if (status != fs::BlockIoStatus::Ok) {
        log_message(LogLevel::Error,
                    "FAT32: failed to write sectors %u (+%u) (status %d)",
                    lba, count, static_cast<int>(status));
        return false;
    }
    return true;
}

uint32_t read_fat_entry(Fat32Volume& volume, uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = volume.fat_begin_lba + (fat_offset / 512);
    uint32_t within_sector = fat_offset % 512;

    if (fat_cache_sector != fat_sector || fat_cache_device != &volume.device) {
        if (!read_sector(volume.device, fat_sector, fat_cache)) {
            return 0x0FFFFFFF;
        }
        fat_cache_sector = fat_sector;
        fat_cache_device = &volume.device;
    }

    uint32_t value = *reinterpret_cast<uint32_t*>(fat_cache + within_sector);
    return value & kFatEntryMask;
}

bool write_fat_entry(Fat32Volume& volume, uint32_t cluster,
                     uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t sector_index = fat_offset / 512;
    uint32_t within_sector = fat_offset % 512;
    uint32_t fat_sector = volume.fat_begin_lba + sector_index;

    if (!read_sector(volume.device, fat_sector, fat_cache)) {
        return false;
    }

    uint32_t original =
        *reinterpret_cast<uint32_t*>(fat_cache + within_sector);
    uint32_t new_value =
        (original & 0xF0000000u) | (value & kFatEntryMask);
    *reinterpret_cast<uint32_t*>(fat_cache + within_sector) = new_value;

    if (!write_sector(volume.device, fat_sector, fat_cache)) {
        return false;
    }

    // Update cache to reflect new value.
    fat_cache_sector = fat_sector;
    fat_cache_device = &volume.device;

    // Mirror to additional FATs if present.
    for (uint32_t fat = 1; fat < volume.num_fats; ++fat) {
        uint32_t mirror_sector =
            volume.fat_begin_lba + (fat * volume.fat_size_sectors) +
            sector_index;
        if (!write_sector(volume.device, mirror_sector, fat_cache)) {
            return false;
        }
    }

    return true;
}

void format_83_name(const uint8_t* raw, char* out) {
    int pos = 0;
    for (int i = 0; i < 8; ++i) {
        if (raw[i] == ' ') continue;
        char ch = static_cast<char>(raw[i]);
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch + ('a' - 'A'));
        }
        out[pos++] = ch;
    }
    int ext_len = 0;
    for (int i = 8; i < 11; ++i) {
        if (raw[i] != ' ') ext_len++;
    }
    if (ext_len > 0) {
        out[pos++] = '.';
        for (int i = 8; i < 11; ++i) {
            if (raw[i] == ' ') continue;
            char ch = static_cast<char>(raw[i]);
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch + ('a' - 'A'));
            }
            out[pos++] = ch;
        }
    }
    out[pos] = '\0';
}

bool names_equal(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'a' && *a <= 'z') ? *a - ('a' - 'A') : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? *b - ('a' - 'A') : *b;
        if (ca != cb) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
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

void reset_lfn(LfnState& state) {
    memset(&state, 0, sizeof(state));
}

uint16_t read_lfn_char(const uint8_t* entry, size_t offset) {
    return static_cast<uint16_t>(entry[offset]) |
           (static_cast<uint16_t>(entry[offset + 1]) << 8);
}

void store_lfn_chars(LfnState& state, const uint8_t* entry, uint8_t seq) {
    static const uint8_t kOffsets[kLfnCharsPerEntry] = {
        1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30,
    };
    size_t base = (static_cast<size_t>(seq) - 1u) * kLfnCharsPerEntry;
    for (size_t i = 0; i < kLfnCharsPerEntry; ++i) {
        size_t out_index = base + i;
        uint16_t code = read_lfn_char(entry, kOffsets[i]);
        if (code == 0x0000 || code == 0xFFFF) {
            if (out_index < sizeof(state.name)) {
                state.name[out_index] = '\0';
            }
            break;
        }
        if (out_index + 1 >= sizeof(state.name)) {
            state.overflow = true;
            continue;
        }
        char ch = (code <= 0x7F) ? static_cast<char>(code) : '?';
        state.name[out_index] = ch;
    }
    state.name[sizeof(state.name) - 1] = '\0';
}

uint8_t lfn_checksum(const uint8_t* short_entry) {
    uint8_t sum = 0;
    for (size_t i = 0; i < 11; ++i) {
        sum = static_cast<uint8_t>(((sum & 1u) ? 0x80u : 0u) +
                                   (sum >> 1u) + short_entry[i]);
    }
    return sum;
}

void consume_lfn_entry(LfnState& state, const uint8_t* entry) {
    if (entry[11] != ATTR_LONG_NAME || entry[0] == 0xE5) {
        reset_lfn(state);
        return;
    }
    if (entry[12] != kLfnType || entry[26] != 0 || entry[27] != 0) {
        reset_lfn(state);
        return;
    }

    uint8_t order = entry[0];
    uint8_t seq = static_cast<uint8_t>(order & kLfnSequenceMask);
    if (seq == 0 || seq > kLfnMaxEntries) {
        reset_lfn(state);
        return;
    }

    bool last = (order & kLfnLastFlag) != 0;
    if (last) {
        reset_lfn(state);
        state.active = true;
        state.expected = seq;
        state.checksum = entry[13];
    } else if (!state.active || state.expected != seq) {
        reset_lfn(state);
        return;
    }

    if (state.checksum != entry[13]) {
        reset_lfn(state);
        return;
    }

    store_lfn_chars(state, entry, seq);
    if (state.expected > 0) {
        --state.expected;
        if (state.expected == 0) {
            state.complete = true;
        }
    }
}

bool build_short_name(const char* name, uint8_t (&out)[11]) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }

    for (size_t i = 0; i < 11; ++i) {
        out[i] = ' ';
    }

    size_t base_len = 0;
    size_t ext_len = 0;
    bool seen_dot = false;

    for (size_t i = 0; name[i] != '\0'; ++i) {
        char ch = name[i];
        if (ch == '.') {
            if (seen_dot) {
                return false;
            }
            seen_dot = true;
            if (i == 0) {
                return false;
            }
            continue;
        }
        if (ch == ' ' || ch == '\t') {
            return false;
        }
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - ('a' - 'A'));
        }
        bool valid = (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                     ch == '_' || ch == '-';
        if (!valid) {
            return false;
        }

        if (!seen_dot) {
            if (base_len >= 8) {
                return false;
            }
            out[base_len++] = static_cast<uint8_t>(ch);
        } else {
            if (ext_len >= 3) {
                return false;
            }
            out[8 + ext_len++] = static_cast<uint8_t>(ch);
        }
    }

    if (base_len == 0) {
        return false;
    }
    if (seen_dot && ext_len == 0) {
        return false;
    }
    return true;
}

size_t entries_per_cluster(const Fat32Volume& volume) {
    return (volume.sectors_per_cluster * 512u) / 32u;
}

enum class IterationResult {
    Continue,
    StopSuccess,
    StopFailure,
};

template <typename Fn>
bool iterate_directory(Fat32Volume& volume, uint32_t start_cluster, Fn&& fn) {
    uint32_t current_cluster = start_cluster;
    bool done = false;
    size_t raw_index = 0;
    size_t visible_index = 0;
    LfnState lfn{};
    reset_lfn(lfn);

    while (!done) {
        uint32_t lba = cluster_to_lba(volume, current_cluster);
        if (!read_sectors(volume.device, lba, volume.sectors_per_cluster,
                          cluster_buffer)) {
            return false;
        }

        size_t count = entries_per_cluster(volume);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* entry =
                reinterpret_cast<const uint8_t*>(cluster_buffer + i * 32);

            if (entry[0] == 0x00) {
                done = true;
                break;
            }

            if (entry[0] == 0xE5) {
                reset_lfn(lfn);
                ++raw_index;
                continue;
            }

            if (entry[11] == ATTR_LONG_NAME) {
                consume_lfn_entry(lfn, entry);
                ++raw_index;
                continue;
            }

            if ((entry[11] & ATTR_VOLUME_ID) != 0) {
                reset_lfn(lfn);
                ++raw_index;
                continue;
            }

            const char* long_name = nullptr;
            if (lfn.active && lfn.complete && !lfn.overflow &&
                lfn_checksum(entry) == lfn.checksum) {
                long_name = lfn.name;
            }

            IterationResult res = fn(entry,
                                     start_cluster,
                                     static_cast<uint32_t>(raw_index),
                                     static_cast<uint32_t>(visible_index),
                                     long_name);
            reset_lfn(lfn);
            if (res == IterationResult::StopSuccess) {
                return true;
            }
            if (res == IterationResult::StopFailure) {
                return false;
            }
            ++visible_index;

            ++raw_index;
        }

        if (done) {
            break;
        }

        uint32_t next = read_fat_entry(volume, current_cluster);
        if (next >= kFatEoc) {
            break;
        }
        if (next == kFatBadCluster) {
            log_message(LogLevel::Warn, "FAT32: bad cluster %u", next);
            return false;
        }
        current_cluster = next;
    }

    return true;
}

void copy_entry(const uint8_t* raw,
                uint32_t directory_cluster,
                uint32_t raw_index,
                const char* long_name,
                Fat32DirEntry& out) {
    memset(&out, 0, sizeof(Fat32DirEntry));
    if (long_name != nullptr && long_name[0] != '\0') {
        size_t i = 0;
        while (long_name[i] != '\0' && i + 1 < sizeof(out.name)) {
            out.name[i] = long_name[i];
            ++i;
        }
        out.name[i] = '\0';
    } else {
        format_83_name(raw, out.name);
    }
    out.attributes = raw[11];
    out.first_cluster =
        (static_cast<uint32_t>(raw[20]) << 16) |
        (static_cast<uint32_t>(raw[21]) << 24) |
        (static_cast<uint32_t>(raw[26])) |
        (static_cast<uint32_t>(raw[27]) << 8);
    out.size = *reinterpret_cast<const uint32_t*>(raw + 28);
    out.directory_cluster = directory_cluster;
    out.raw_entry_index = raw_index;
}

void initialize_next_free(Fat32Volume& volume) {
    volume.next_free_cluster = 2;

    if (volume.fs_info_sector == 0) {
        return;
    }
    if (volume.fs_info_sector >= volume.total_sectors) {
        return;
    }

    if (!read_sector(volume.device, volume.fs_info_sector, sector_buffer)) {
        log_message(LogLevel::Warn,
                    "FAT32: failed to read FSINFO, using default allocator");
        return;
    }

    uint32_t lead =
        *reinterpret_cast<const uint32_t*>(sector_buffer + 0);
    uint32_t struct_sig =
        *reinterpret_cast<const uint32_t*>(sector_buffer + 484);
    uint32_t trail =
        *reinterpret_cast<const uint32_t*>(sector_buffer + 508);

    if (lead != kFsInfoLeadSignature ||
        struct_sig != kFsInfoStructSignature ||
        trail != kFsInfoTrailSignature) {
        return;
    }

    uint32_t next_free =
        *reinterpret_cast<const uint32_t*>(sector_buffer + 492);
    uint32_t max_cluster = volume.total_clusters + 1;
    if (next_free >= 2 && next_free <= max_cluster) {
        volume.next_free_cluster = next_free;
    }
}

bool clear_cluster(Fat32Volume& volume, uint32_t cluster) {
    size_t bytes =
        static_cast<size_t>(volume.sectors_per_cluster) * 512u;
    if (bytes > sizeof(cluster_buffer)) {
        log_message(LogLevel::Warn,
                    "FAT32: cluster buffer too small for cluster %u",
                    cluster);
        return false;
    }
    memset(cluster_buffer, 0, bytes);
    uint32_t lba = cluster_to_lba(volume, cluster);
    return write_sectors(volume.device,
                         lba,
                         volume.sectors_per_cluster,
                         cluster_buffer);
}

bool allocate_cluster(Fat32Volume& volume, uint32_t& out_cluster) {
    if (volume.total_clusters == 0) {
        log_message(LogLevel::Warn, "FAT32: no clusters available");
        return false;
    }

    uint32_t max_cluster = volume.total_clusters + 1;
    uint32_t start = volume.next_free_cluster;
    if (start < 2 || start > max_cluster) {
        start = 2;
    }

    uint32_t cluster = start;
    do {
        uint32_t status = read_fat_entry(volume, cluster);
        if (status == kFatFreeCluster) {
            if (!write_fat_entry(volume, cluster, kFatEoc)) {
                return false;
            }
            if (!clear_cluster(volume, cluster)) {
                return false;
            }
            out_cluster = cluster;

            uint32_t next_candidate = cluster + 1;
            if (next_candidate > max_cluster) {
                next_candidate = 2;
            }
            volume.next_free_cluster = next_candidate;
            return true;
        }

        ++cluster;
        if (cluster > max_cluster) {
            cluster = 2;
        }
    } while (cluster != start);

    log_message(LogLevel::Warn, "FAT32: out of free clusters");
    return false;
}

bool get_chain_info(Fat32Volume& volume,
                    uint32_t first_cluster,
                    uint32_t& out_length,
                    uint32_t& out_tail) {
    out_length = 0;
    out_tail = 0;

    if (first_cluster < 2) {
        return true;
    }

    uint32_t max_cluster = volume.total_clusters + 1;
    if (first_cluster > max_cluster) {
        log_message(LogLevel::Warn,
                    "FAT32: invalid first cluster %u", first_cluster);
        return false;
    }

    uint32_t cluster = first_cluster;
    uint32_t traversed = 0;
    while (cluster >= 2 && cluster <= max_cluster) {
        ++traversed;
        out_tail = cluster;

        if (traversed > volume.total_clusters + 1) {
            log_message(LogLevel::Warn, "FAT32: cluster chain loop detected");
            return false;
        }

        uint32_t next = read_fat_entry(volume, cluster);
        if (next == kFatBadCluster) {
            log_message(LogLevel::Warn, "FAT32: bad cluster in chain");
            return false;
        }
        if (next >= kFatEoc) {
            break;
        }
        if (next < 2 || next > max_cluster) {
            log_message(LogLevel::Warn,
                        "FAT32: cluster %u points to invalid next %u",
                        cluster, next);
            return false;
        }

        cluster = next;
    }

    out_length = traversed;
    return true;
}

bool ensure_cluster_count(Fat32Volume& volume,
                          Fat32DirEntry& entry,
                          uint32_t required_clusters) {
    if (required_clusters == 0) {
        return true;
    }

    uint32_t length = 0;
    uint32_t tail = 0;
    if (!get_chain_info(volume, entry.first_cluster, length, tail)) {
        return false;
    }

    if (length == 0) {
        uint32_t new_cluster = 0;
        if (!allocate_cluster(volume, new_cluster)) {
            return false;
        }
        entry.first_cluster = new_cluster;
        tail = new_cluster;
        length = 1;
    }

    while (length < required_clusters) {
        uint32_t new_cluster = 0;
        if (!allocate_cluster(volume, new_cluster)) {
            return false;
        }
        if (!write_fat_entry(volume, tail, new_cluster)) {
            return false;
        }
        tail = new_cluster;
        ++length;
    }

    return true;
}

bool get_cluster_at_index(Fat32Volume& volume,
                          uint32_t first_cluster,
                          uint32_t index,
                          uint32_t& out_cluster) {
    if (first_cluster < 2) {
        return false;
    }

    uint32_t cluster = first_cluster;
    for (uint32_t i = 0; i < index; ++i) {
        uint32_t next = read_fat_entry(volume, cluster);
        if (next == kFatBadCluster) {
            log_message(LogLevel::Warn, "FAT32: bad cluster in chain");
            return false;
        }
        if (next >= kFatEoc) {
            return false;
        }
        cluster = next;
        if (i > volume.total_clusters + 1) {
            log_message(LogLevel::Warn, "FAT32: cluster traversal overflow");
            return false;
        }
    }

    out_cluster = cluster;
    return true;
}

bool zero_range(Fat32Volume& volume,
                Fat32DirEntry& entry,
                uint32_t start,
                uint32_t length) {
    if (length == 0) {
        return true;
    }

    size_t cluster_size =
        static_cast<size_t>(volume.sectors_per_cluster) * 512u;
    if (cluster_size == 0) {
        return false;
    }
    if (cluster_size > sizeof(cluster_buffer)) {
        log_message(LogLevel::Warn,
                    "FAT32: cluster size %zu exceeds buffer capacity",
                    cluster_size);
        return false;
    }
    if (cluster_size > sizeof(cluster_buffer)) {
        log_message(LogLevel::Warn,
                    "FAT32: cluster size %zu exceeds buffer capacity",
                    cluster_size);
        return false;
    }

    uint32_t end = start + length;
    uint32_t offset = start;

    while (offset < end) {
        uint32_t cluster_index =
            offset / static_cast<uint32_t>(cluster_size);
        uint32_t cluster_offset =
            offset % static_cast<uint32_t>(cluster_size);
        uint32_t cluster_number = 0;
        if (!get_cluster_at_index(volume,
                                  entry.first_cluster,
                                  cluster_index,
                                  cluster_number)) {
            return false;
        }

        size_t chunk = cluster_size - cluster_offset;
        uint32_t remaining = end - offset;
        if (chunk > remaining) {
            chunk = remaining;
        }

        uint32_t lba = cluster_to_lba(volume, cluster_number);
        if (chunk == cluster_size && cluster_offset == 0) {
            memset(cluster_buffer, 0, cluster_size);
            if (!write_sectors(volume.device,
                               lba,
                               volume.sectors_per_cluster,
                               cluster_buffer)) {
                return false;
            }
        } else {
            if (!read_sectors(volume.device,
                              lba,
                              volume.sectors_per_cluster,
                              cluster_buffer)) {
                return false;
            }
            memset(cluster_buffer + cluster_offset, 0, chunk);
            if (!write_sectors(volume.device,
                               lba,
                               volume.sectors_per_cluster,
                               cluster_buffer)) {
                return false;
            }
        }

        offset += static_cast<uint32_t>(chunk);
    }

    return true;
}

bool resolve_entry(Fat32Volume& volume,
                   const char* path,
                   Fat32DirEntry& out_entry);
bool read_directory_raw_entry(Fat32Volume& volume,
                              uint32_t directory_cluster,
                              uint32_t raw_index,
                              uint8_t (&out)[32]);
bool write_directory_raw_entry(Fat32Volume& volume,
                               uint32_t directory_cluster,
                               uint32_t raw_index,
                               const uint8_t (&entry)[32]);

bool update_directory_entry(Fat32Volume& volume,
                            const Fat32DirEntry& entry) {
    uint32_t entries_per_cluster32 =
        static_cast<uint32_t>(entries_per_cluster(volume));
    if (entries_per_cluster32 == 0) {
        return false;
    }

    uint32_t raw_index = entry.raw_entry_index;
    uint32_t cluster_offset = raw_index / entries_per_cluster32;
    uint32_t index_in_cluster = raw_index % entries_per_cluster32;

    uint32_t cluster = entry.directory_cluster;
    if (cluster < 2) {
        return false;
    }

    for (uint32_t i = 0; i < cluster_offset; ++i) {
        uint32_t next = read_fat_entry(volume, cluster);
        if (next == kFatBadCluster) {
            log_message(LogLevel::Warn, "FAT32: bad cluster in directory chain");
            return false;
        }
        if (next >= kFatEoc) {
            log_message(LogLevel::Warn,
                        "FAT32: directory chain too short for entry update");
            return false;
        }
        cluster = next;
    }

    uint32_t lba = cluster_to_lba(volume, cluster);
    uint32_t entry_byte = index_in_cluster * 32;
    uint32_t sector_offset = entry_byte / 512;
    uint32_t byte_offset = entry_byte % 512;

    if (!read_sector(volume.device, lba + sector_offset, sector_buffer)) {
        return false;
    }

    uint8_t* raw = sector_buffer + byte_offset;
    uint32_t first_cluster = entry.first_cluster;
    uint16_t high = static_cast<uint16_t>(first_cluster >> 16);
    uint16_t low = static_cast<uint16_t>(first_cluster & 0xFFFF);

    raw[20] = static_cast<uint8_t>(high & 0xFF);
    raw[21] = static_cast<uint8_t>((high >> 8) & 0xFF);
    raw[26] = static_cast<uint8_t>(low & 0xFF);
    raw[27] = static_cast<uint8_t>((low >> 8) & 0xFF);

    *reinterpret_cast<uint32_t*>(raw + 28) = entry.size;

    return write_sector(volume.device, lba + sector_offset, sector_buffer);
}

bool mark_directory_entry_deleted(Fat32Volume& volume,
                                  const Fat32DirEntry& entry) {
    uint8_t short_entry[32];
    if (!read_directory_raw_entry(volume,
                                  entry.directory_cluster,
                                  entry.raw_entry_index,
                                  short_entry)) {
        return false;
    }

    uint8_t checksum = lfn_checksum(short_entry);
    uint32_t index = entry.raw_entry_index;
    for (uint32_t count = 0; count < kLfnMaxEntries && index != 0; ++count) {
        --index;
        uint8_t raw[32];
        if (!read_directory_raw_entry(volume,
                                      entry.directory_cluster,
                                      index,
                                      raw) ||
            raw[11] != ATTR_LONG_NAME || raw[13] != checksum) {
            break;
        }
        bool last = (raw[0] & kLfnLastFlag) != 0;
        raw[0] = 0xE5;
        if (!write_directory_raw_entry(volume,
                                       entry.directory_cluster,
                                       index,
                                       raw)) {
            return false;
        }
        if (last) {
            break;
        }
    }

    short_entry[0] = 0xE5;
    return write_directory_raw_entry(volume,
                                     entry.directory_cluster,
                                     entry.raw_entry_index,
                                     short_entry);
}

bool free_cluster_chain(Fat32Volume& volume, uint32_t first_cluster) {
    if (first_cluster < 2) {
        return true;
    }

    uint32_t max_cluster = volume.total_clusters + 1;
    uint32_t cluster = first_cluster;
    uint32_t traversed = 0;
    while (cluster >= 2 && cluster <= max_cluster) {
        if (++traversed > volume.total_clusters + 1) {
            log_message(LogLevel::Warn, "FAT32: cluster chain loop detected");
            return false;
        }
        uint32_t next = read_fat_entry(volume, cluster);
        if (next == kFatBadCluster) {
            log_message(LogLevel::Warn, "FAT32: bad cluster in chain");
            return false;
        }
        if (!write_fat_entry(volume, cluster, kFatFreeCluster)) {
            return false;
        }
        if (cluster < volume.next_free_cluster) {
            volume.next_free_cluster = cluster;
        }
        if (next >= kFatEoc) {
            return true;
        }
        cluster = next;
    }
    return false;
}

bool directory_is_empty(Fat32Volume& volume, uint32_t directory_cluster) {
    bool empty = true;
    bool ok = iterate_directory(volume, directory_cluster,
                                [&](const uint8_t* raw,
                                    uint32_t,
                                    uint32_t,
                                    uint32_t,
                                    const char* long_name) -> IterationResult {
        const char* entry_long =
            (long_name != nullptr && long_name[0] != '\0') ? long_name : nullptr;
        char entry_name[64] = {0};
        format_83_name(raw, entry_name);
        const char* name = (entry_long != nullptr) ? entry_long : entry_name;
        if (names_equal(name, ".") || names_equal(name, "..")) {
            return IterationResult::Continue;
        }
        empty = false;
        return IterationResult::StopSuccess;
    });
    return ok && empty;
}

bool fat32_remove_file(Fat32Volume& volume, const char* path) {
    if (!volume.mounted || path == nullptr) {
        return false;
    }

    Fat32DirEntry entry{};
    if (!resolve_entry(volume, path, entry)) {
        return false;
    }
    if ((entry.attributes & ATTR_DIRECTORY) != 0) {
        return false;
    }

    if (!mark_directory_entry_deleted(volume, entry)) {
        return false;
    }
    return free_cluster_chain(volume, entry.first_cluster);
}

bool fat32_remove_directory(Fat32Volume& volume, const char* path) {
    if (!volume.mounted || path == nullptr) {
        return false;
    }

    Fat32DirEntry entry{};
    if (!resolve_entry(volume, path, entry)) {
        return false;
    }
    if ((entry.attributes & ATTR_DIRECTORY) == 0) {
        return false;
    }
    if (!directory_is_empty(volume, entry.first_cluster)) {
        return false;
    }

    if (!mark_directory_entry_deleted(volume, entry)) {
        return false;
    }
    return free_cluster_chain(volume, entry.first_cluster);
}

const char* trim_leading_slashes(const char* path) {
    if (path == nullptr) {
        return nullptr;
    }
    while (*path == '/') {
        ++path;
    }
    return path;
}

bool next_segment(const char*& cursor, char* segment, bool& has_more) {
    cursor = trim_leading_slashes(cursor);
    if (cursor == nullptr || *cursor == '\0') {
        return false;
    }

    size_t len = 0;
    while (cursor[len] != '\0' && cursor[len] != '/') {
        if (len + 1 >= kMaxSegmentLength) {
            return false;
        }
        segment[len] = cursor[len];
        ++len;
    }
    segment[len] = '\0';

    const char* next = cursor + len;
    while (*next == '/') {
        ++next;
    }
    has_more = (*next != '\0');
    cursor = next;
    return true;
}

bool resolve_directory_cluster(Fat32Volume& volume,
                               const char* path,
                               uint32_t& out_cluster) {
    uint32_t current = volume.root_dir_first_cluster;
    if (path == nullptr) {
        out_cluster = current;
        return true;
    }

    const char* cursor = path;
    char segment[kMaxSegmentLength]{};
    bool has_more = false;

    while (next_segment(cursor, segment, has_more)) {
        Fat32DirEntry entry{};
        if (!fat32_find_entry(volume, current, segment, entry)) {
            return false;
        }
        if ((entry.attributes & ATTR_DIRECTORY) == 0) {
            return false;
        }
        current = entry.first_cluster;
        if (!has_more) {
            break;
        }
    }

    const char* remaining = trim_leading_slashes(cursor);
    if (remaining != nullptr && *remaining != '\0') {
        return false;
    }

    out_cluster = current;
    return true;
}

bool resolve_entry(Fat32Volume& volume,
                   const char* path,
                   Fat32DirEntry& out_entry) {
    if (path == nullptr) {
        return false;
    }

    const char* cursor = path;
    char segment[kMaxSegmentLength]{};
    bool has_more = false;
    uint32_t current_cluster = volume.root_dir_first_cluster;

    while (next_segment(cursor, segment, has_more)) {
        Fat32DirEntry entry{};
        if (!fat32_find_entry(volume, current_cluster, segment, entry)) {
            return false;
        }
        if (!has_more) {
            out_entry = entry;
            return true;
        }
        if ((entry.attributes & ATTR_DIRECTORY) == 0) {
            return false;
        }
        current_cluster = entry.first_cluster;
    }
    return false;
}

struct DirectorySlot {
    uint32_t start_raw_index;
    uint32_t short_raw_index;
};

bool split_parent_and_name(Fat32Volume& volume,
                           const char* path,
                           uint32_t& out_parent_cluster,
                           char (&out_name)[kMaxSegmentLength]) {
    if (path == nullptr) {
        return false;
    }

    const char* cursor = path;
    char segment[kMaxSegmentLength]{};
    bool has_more = false;
    uint32_t current_cluster = volume.root_dir_first_cluster;

    while (next_segment(cursor, segment, has_more)) {
        if (!has_more) {
            if (segment[0] == '\0') {
                return false;
            }
            size_t len = string_length(segment);
            if (len >= kMaxSegmentLength) {
                return false;
            }
            for (size_t i = 0; i < len; ++i) {
                out_name[i] = segment[i];
            }
            out_name[len] = '\0';
            out_parent_cluster = current_cluster;
            return true;
        }

        Fat32DirEntry entry{};
        if (!fat32_find_entry(volume, current_cluster, segment, entry)) {
            return false;
        }
        if ((entry.attributes & ATTR_DIRECTORY) == 0) {
            return false;
        }
        current_cluster = entry.first_cluster;
    }
    return false;
}

bool ensure_directory_capacity(Fat32Volume& volume,
                               uint32_t directory_cluster,
                               uint32_t required_entries) {
    const size_t cluster_entries = entries_per_cluster(volume);
    if (cluster_entries == 0 || directory_cluster < 2 || required_entries == 0) {
        return false;
    }

    uint32_t required_clusters = static_cast<uint32_t>(
        (required_entries + cluster_entries - 1) / cluster_entries);
    uint32_t length = 0;
    uint32_t tail = 0;
    if (!get_chain_info(volume, directory_cluster, length, tail) || length == 0) {
        return false;
    }
    while (length < required_clusters) {
        uint32_t new_cluster = 0;
        if (!allocate_cluster(volume, new_cluster)) {
            return false;
        }
        if (!write_fat_entry(volume, tail, new_cluster)) {
            (void)write_fat_entry(volume, new_cluster, kFatFreeCluster);
            return false;
        }
        tail = new_cluster;
        ++length;
    }
    return true;
}

bool directory_cluster_for_raw_index(Fat32Volume& volume,
                                     uint32_t directory_cluster,
                                     uint32_t raw_index,
                                     uint32_t& out_cluster,
                                     uint32_t& out_index_in_cluster) {
    uint32_t cluster_entries =
        static_cast<uint32_t>(entries_per_cluster(volume));
    if (cluster_entries == 0 || directory_cluster < 2) {
        return false;
    }
    uint32_t cluster_offset = raw_index / cluster_entries;
    uint32_t cluster = directory_cluster;
    for (uint32_t i = 0; i < cluster_offset; ++i) {
        uint32_t next = read_fat_entry(volume, cluster);
        if (next == kFatBadCluster || next >= kFatEoc) {
            return false;
        }
        cluster = next;
    }
    out_cluster = cluster;
    out_index_in_cluster = raw_index % cluster_entries;
    return true;
}

bool read_directory_raw_entry(Fat32Volume& volume,
                              uint32_t directory_cluster,
                              uint32_t raw_index,
                              uint8_t (&out)[32]) {
    uint32_t cluster = 0;
    uint32_t index = 0;
    if (!directory_cluster_for_raw_index(volume,
                                         directory_cluster,
                                         raw_index,
                                         cluster,
                                         index)) {
        return false;
    }
    uint32_t entry_byte = index * 32;
    uint32_t lba = cluster_to_lba(volume, cluster) + entry_byte / 512;
    if (!read_sector(volume.device, lba, sector_buffer)) {
        return false;
    }
    memcpy(out, sector_buffer + entry_byte % 512, 32);
    return true;
}

bool write_directory_raw_entry(Fat32Volume& volume,
                               uint32_t directory_cluster,
                               uint32_t raw_index,
                               const uint8_t (&entry)[32]) {
    uint32_t cluster = 0;
    uint32_t index = 0;
    if (!directory_cluster_for_raw_index(volume,
                                         directory_cluster,
                                         raw_index,
                                         cluster,
                                         index)) {
        return false;
    }
    uint32_t entry_byte = index * 32;
    uint32_t lba = cluster_to_lba(volume, cluster) + entry_byte / 512;
    if (!read_sector(volume.device, lba, sector_buffer)) {
        return false;
    }
    memcpy(sector_buffer + entry_byte % 512, entry, 32);
    return write_sector(volume.device, lba, sector_buffer);
}

bool find_directory_slots(Fat32Volume& volume,
                          uint32_t directory_cluster,
                          uint32_t needed,
                          DirectorySlot& out_slot) {
    const uint32_t cluster_entries =
        static_cast<uint32_t>(entries_per_cluster(volume));
    if (cluster_entries == 0 || needed == 0 || directory_cluster < 2) {
        return false;
    }

    uint32_t current_cluster = directory_cluster;
    uint32_t raw_index = 0;
    uint32_t run_start = 0;
    uint32_t run_length = 0;

    while (true) {
        if (!read_sectors(volume.device,
                          cluster_to_lba(volume, current_cluster),
                          volume.sectors_per_cluster,
                          cluster_buffer)) {
            return false;
        }

        for (uint32_t i = 0; i < cluster_entries; ++i, ++raw_index) {
            uint8_t* entry =
                reinterpret_cast<uint8_t*>(cluster_buffer + i * 32);
            uint8_t first = entry[0];
            if (first == 0x00) {
                if (run_length == 0) {
                    run_start = raw_index;
                }
                uint32_t required_entries = run_start + needed;
                if (!ensure_directory_capacity(volume,
                                               directory_cluster,
                                               required_entries)) {
                    return false;
                }
                out_slot.start_raw_index = run_start;
                out_slot.short_raw_index = required_entries - 1;
                return true;
            }
            if (first == 0xE5) {
                if (run_length == 0) {
                    run_start = raw_index;
                }
                ++run_length;
                if (run_length == needed) {
                    out_slot.start_raw_index = run_start;
                    out_slot.short_raw_index = raw_index;
                    return true;
                }
            } else {
                run_length = 0;
            }
        }

        uint32_t next = read_fat_entry(volume, current_cluster);
        if (next == kFatBadCluster) {
            log_message(LogLevel::Warn,
                        "FAT32: directory cluster chain corrupt");
            return false;
        }
        if (next >= kFatEoc) {
            if (run_length == 0) {
                run_start = raw_index;
            }
            uint32_t required_entries = run_start + needed;
            if (!ensure_directory_capacity(volume,
                                           directory_cluster,
                                           required_entries)) {
                return false;
            }
            out_slot.start_raw_index = run_start;
            out_slot.short_raw_index = required_entries - 1;
            return true;
        }
        current_cluster = next;
    }
}

bool valid_long_name(const char* name, size_t& out_length) {
    out_length = 0;
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    for (; name[out_length] != '\0'; ++out_length) {
        unsigned char ch = static_cast<unsigned char>(name[out_length]);
        if (out_length + 1 >= sizeof(Fat32DirEntry::name) || ch < 0x20 ||
            ch > 0x7E || ch == '"' || ch == '*' || ch == '/' || ch == ':' ||
            ch == '<' || ch == '>' || ch == '?' || ch == '\\' || ch == '|') {
            return false;
        }
    }
    return name[out_length - 1] != ' ' && name[out_length - 1] != '.';
}

bool short_name_available(Fat32Volume& volume,
                          uint32_t directory_cluster,
                          const uint8_t (&candidate)[11]) {
    bool found = false;
    bool ok = iterate_directory(
        volume,
        directory_cluster,
        [&](const uint8_t* raw, uint32_t, uint32_t, uint32_t,
            const char*) -> IterationResult {
            if (memcmp(raw, candidate, 11) == 0) {
                found = true;
                return IterationResult::StopSuccess;
            }
            return IterationResult::Continue;
        });
    return ok && !found;
}

bool build_unique_short_alias(Fat32Volume& volume,
                              uint32_t directory_cluster,
                              const char* name,
                              uint8_t (&out)[11]) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; name[i] != '\0'; ++i) {
        hash ^= static_cast<uint8_t>(name[i]);
        hash *= 16777619u;
    }

    const char* extension = nullptr;
    for (const char* cursor = name; *cursor != '\0'; ++cursor) {
        if (*cursor == '.') {
            extension = cursor + 1;
        }
    }
    constexpr char kHex[] = "0123456789ABCDEF";
    for (uint32_t attempt = 0; attempt < 256; ++attempt) {
        memset(out, ' ', 11);
        uint32_t value = hash ^ (attempt * 0x9E3779B9u);
        out[0] = '~';
        for (size_t i = 0; i < 7; ++i) {
            out[7 - i] = static_cast<uint8_t>(kHex[value & 0x0Fu]);
            value = (value >> 4) ^ (value << 28);
        }
        if (extension != nullptr) {
            for (size_t i = 0; i < 3 && extension[i] != '\0'; ++i) {
                char ch = extension[i];
                if (ch >= 'a' && ch <= 'z') {
                    ch = static_cast<char>(ch - ('a' - 'A'));
                }
                if (!((ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9'))) {
                    ch = '_';
                }
                out[8 + i] = static_cast<uint8_t>(ch);
            }
        }
        if (short_name_available(volume, directory_cluster, out)) {
            return true;
        }
    }
    return false;
}

void store_utf16(uint8_t* entry, size_t offset, uint16_t value) {
    entry[offset] = static_cast<uint8_t>(value & 0xFFu);
    entry[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void build_lfn_entry(const char* name,
                     size_t name_length,
                     uint8_t sequence,
                     uint8_t sequence_count,
                     uint8_t checksum,
                     uint8_t (&out)[32]) {
    memset(out, 0, 32);
    out[0] = static_cast<uint8_t>(sequence |
                                  (sequence == sequence_count
                                       ? kLfnLastFlag
                                       : 0));
    out[11] = ATTR_LONG_NAME;
    out[12] = kLfnType;
    out[13] = checksum;
    static constexpr uint8_t kOffsets[kLfnCharsPerEntry] = {
        1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30,
    };
    size_t base = (static_cast<size_t>(sequence) - 1u) * kLfnCharsPerEntry;
    for (size_t i = 0; i < kLfnCharsPerEntry; ++i) {
        size_t index = base + i;
        uint16_t value = 0xFFFFu;
        if (index < name_length) {
            value = static_cast<uint8_t>(name[index]);
        } else if (index == name_length) {
            value = 0x0000u;
        }
        store_utf16(out, kOffsets[i], value);
    }
}

bool create_named_entry(Fat32Volume& volume,
                        uint32_t parent_cluster,
                        const char* name,
                        uint8_t attributes,
                        uint32_t first_cluster,
                        Fat32DirEntry& out_entry) {
    uint8_t short_name[11];
    bool needs_lfn = !build_short_name(name, short_name);
    size_t name_length = 0;
    if (needs_lfn &&
        (!valid_long_name(name, name_length) ||
         !build_unique_short_alias(volume, parent_cluster, name, short_name))) {
        return false;
    }

    uint32_t lfn_count = needs_lfn
                             ? static_cast<uint32_t>(
                                   (name_length + kLfnCharsPerEntry - 1) /
                                   kLfnCharsPerEntry)
                             : 0;
    if (lfn_count > kLfnMaxEntries) {
        return false;
    }
    DirectorySlot slot{};
    if (!find_directory_slots(volume,
                              parent_cluster,
                              lfn_count + 1,
                              slot)) {
        return false;
    }

    uint8_t checksum = lfn_checksum(short_name);
    for (uint32_t disk_index = 0; disk_index < lfn_count; ++disk_index) {
        uint8_t sequence = static_cast<uint8_t>(lfn_count - disk_index);
        uint8_t raw_lfn[32];
        build_lfn_entry(name,
                        name_length,
                        sequence,
                        static_cast<uint8_t>(lfn_count),
                        checksum,
                        raw_lfn);
        if (!write_directory_raw_entry(volume,
                                       parent_cluster,
                                       slot.start_raw_index + disk_index,
                                       raw_lfn)) {
            return false;
        }
    }

    uint8_t raw[32]{};
    memcpy(raw, short_name, 11);
    raw[11] = attributes;
    raw[20] = static_cast<uint8_t>((first_cluster >> 16) & 0xFFu);
    raw[21] = static_cast<uint8_t>((first_cluster >> 24) & 0xFFu);
    raw[26] = static_cast<uint8_t>(first_cluster & 0xFFu);
    raw[27] = static_cast<uint8_t>((first_cluster >> 8) & 0xFFu);
    if (!write_directory_raw_entry(volume,
                                   parent_cluster,
                                   slot.short_raw_index,
                                   raw)) {
        return false;
    }

    copy_entry(raw,
               parent_cluster,
               slot.short_raw_index,
               needs_lfn ? name : nullptr,
               out_entry);
    return true;
}

bool fat32_create_file(Fat32Volume& volume,
                       const char* path,
                       Fat32DirEntry& out_entry) {
    if (!volume.mounted || path == nullptr) {
        return false;
    }

    char name[kMaxSegmentLength]{};
    uint32_t parent_cluster = 0;
    if (!split_parent_and_name(volume, path, parent_cluster, name)) {
        return false;
    }

    Fat32DirEntry existing{};
    if (fat32_find_entry(volume, parent_cluster, name, existing)) {
        return false;
    }

    return create_named_entry(volume,
                              parent_cluster,
                              name,
                              0x00,
                              0,
                              out_entry);
}

bool fat32_create_directory(Fat32Volume& volume,
                            const char* path,
                            Fat32DirEntry& out_entry) {
    if (!volume.mounted || path == nullptr) {
        return false;
    }

    char name[kMaxSegmentLength]{};
    uint32_t parent_cluster = 0;
    if (!split_parent_and_name(volume, path, parent_cluster, name)) {
        return false;
    }

    Fat32DirEntry existing{};
    if (fat32_find_entry(volume, parent_cluster, name, existing)) {
        return false;
    }

    uint32_t new_cluster = 0;
    if (!allocate_cluster(volume, new_cluster)) {
        return false;
    }

    // Initialize directory cluster with "." and ".."
    size_t cluster_size =
        static_cast<size_t>(volume.sectors_per_cluster) * 512u;
    if (cluster_size > sizeof(cluster_buffer)) {
        log_message(LogLevel::Warn,
                    "FAT32: cluster size %zu exceeds buffer capacity",
                    cluster_size);
        return false;
    }
    memset(cluster_buffer, 0, cluster_size);
    uint8_t* dot = reinterpret_cast<uint8_t*>(cluster_buffer);
    uint8_t* dotdot = reinterpret_cast<uint8_t*>(cluster_buffer + 32);

    auto fill_entry = [&](uint8_t* entry, const uint8_t* name83,
                          uint32_t first_cluster) {
        memcpy(entry, name83, 11);
        entry[11] = ATTR_DIRECTORY;
        *reinterpret_cast<uint16_t*>(entry + 20) =
            static_cast<uint16_t>(first_cluster >> 16);
        *reinterpret_cast<uint16_t*>(entry + 26) =
            static_cast<uint16_t>(first_cluster & 0xFFFF);
        *reinterpret_cast<uint32_t*>(entry + 28) = 0;
    };

    uint8_t dot_name[11]{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    dot_name[0] = '.';
    uint8_t dotdot_name[11]{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    dotdot_name[0] = '.';
    dotdot_name[1] = '.';

    fill_entry(dot, dot_name, new_cluster);
    fill_entry(dotdot, dotdot_name, parent_cluster == 0 ? volume.root_dir_first_cluster : parent_cluster);

    if (!write_sectors(volume.device,
                       cluster_to_lba(volume, new_cluster),
                       volume.sectors_per_cluster,
                       cluster_buffer)) {
        return false;
    }

    if (!create_named_entry(volume,
                            parent_cluster,
                            name,
                            ATTR_DIRECTORY,
                            new_cluster,
                            out_entry)) {
        (void)write_fat_entry(volume, new_cluster, kFatFreeCluster);
        if (new_cluster < volume.next_free_cluster) {
            volume.next_free_cluster = new_cluster;
        }
        return false;
    }
    return true;
}

void to_vfs_entry(const Fat32DirEntry& source, vfs::DirEntry& dest) {
    memset(&dest, 0, sizeof(dest));
    size_t i = 0;
    while (source.name[i] != '\0' && i + 1 < sizeof(dest.name)) {
        dest.name[i] = source.name[i];
        ++i;
    }
    dest.name[i] = '\0';
    dest.flags = ((source.attributes & ATTR_DIRECTORY) != 0)
                     ? vfs::kDirEntryFlagDirectory
                     : 0u;
    dest.reserved = 0;
    dest.size = source.size;
}

}  // namespace

bool fat32_mount(Fat32Volume& volume, const fs::BlockDevice& device) {
    memset(&volume, 0, sizeof(volume));
    volume.device = device;
    volume.mounted = false;
    const char* device_name =
        volume.device.name != nullptr ? volume.device.name : "(unnamed)";

    if (volume.device.sector_size != 512) {
        log_message(LogLevel::Warn,
                    "FAT32: unsupported sector size %zu on device %s",
                    volume.device.sector_size, device_name);
        return false;
    }

    if (!read_sector(volume.device, 0, sector_buffer)) {
        return false;
    }

    auto* bpb = reinterpret_cast<BpbFat32*>(sector_buffer);
    if (bpb->bytes_per_sector != 512) {
        log_message(LogLevel::Warn,
                    "FAT32: unsupported bytes per sector: %u",
                    bpb->bytes_per_sector);
        return false;
    }
    if (bpb->sectors_per_cluster == 0) {
        log_message(LogLevel::Warn, "FAT32: invalid sectors per cluster");
        return false;
    }
    if (bpb->fat_size_32 == 0) {
        log_message(LogLevel::Warn, "FAT32: invalid fat size");
        return false;
    }
    if (bpb->num_fats == 0) {
        log_message(LogLevel::Warn, "FAT32: volume reports zero FATs");
        return false;
    }
    if (bpb->fat_size_16 != 0 || bpb->root_entry_count != 0) {
        log_message(LogLevel::Warn,
                    "FAT32: volume reports FAT16 parameters (root entries=%u fat16=%u)",
                    bpb->root_entry_count, bpb->fat_size_16);
        return false;
    }
    if (bpb->total_sectors_32 == 0) {
        log_message(LogLevel::Warn, "FAT32: invalid total sectors");
        return false;
    }
    if (bpb->root_cluster < 2) {
        log_message(LogLevel::Warn, "FAT32: invalid root cluster: %u",
                    bpb->root_cluster);
        return false;
    }

    // FAT labels are space padded. Present a trimmed label to the VFS, which
    // remains responsible for namespace validation and conflict handling.
    size_t label_length = sizeof(bpb->volume_label);
    while (label_length > 0 && bpb->volume_label[label_length - 1] == ' ') {
        --label_length;
    }
    if (label_length == 7 && bpb->volume_label[0] == 'N' &&
        bpb->volume_label[1] == 'O' && bpb->volume_label[2] == ' ' &&
        bpb->volume_label[3] == 'N' && bpb->volume_label[4] == 'A' &&
        bpb->volume_label[5] == 'M' && bpb->volume_label[6] == 'E') {
        label_length = 0;
    }
    if (label_length > 0) {
        memcpy(volume.preferred_alias, bpb->volume_label, label_length);
        volume.preferred_alias[label_length] = '\0';
    }

    volume.sectors_per_cluster = bpb->sectors_per_cluster;
    volume.reserved_sectors = bpb->reserved_sector_count;
    volume.fat_size_sectors = bpb->fat_size_32;
    volume.fat_begin_lba = volume.reserved_sectors;
    volume.cluster_begin_lba =
        volume.fat_begin_lba + (bpb->num_fats * volume.fat_size_sectors);
    volume.root_dir_first_cluster = bpb->root_cluster;
    volume.num_fats = bpb->num_fats;
    volume.fs_info_sector = bpb->fs_info;

    uint32_t total_sectors =
        (bpb->total_sectors_32 != 0) ? bpb->total_sectors_32
                                     : static_cast<uint32_t>(bpb->total_sectors_16);
    volume.total_sectors = total_sectors;

    uint32_t fats_total =
        static_cast<uint32_t>(volume.num_fats) * volume.fat_size_sectors;
    uint32_t overhead = volume.reserved_sectors + fats_total;
    if (overhead >= total_sectors) {
        log_message(LogLevel::Warn, "FAT32: invalid volume geometry");
        return false;
    }
    uint32_t data_sectors = total_sectors - overhead;
    uint32_t total_clusters =
        data_sectors / static_cast<uint32_t>(volume.sectors_per_cluster);
    if (total_clusters == 0) {
        log_message(LogLevel::Warn, "FAT32: no data clusters available");
        return false;
    }
    volume.total_clusters = total_clusters;
    initialize_next_free(volume);

    fat_cache_sector = 0xFFFFFFFF;
    fat_cache_device = &volume.device;

    volume.mounted = true;

    log_message(LogLevel::Info,
                "FAT32: mounted %s root cluster=%u spc=%u reserved=%u fat=%u",
                device_name, volume.root_dir_first_cluster,
                volume.sectors_per_cluster, volume.reserved_sectors,
                volume.fat_size_sectors);

    return true;
}

bool fat32_list_directory(Fat32Volume& volume, uint32_t start_cluster,
                          Fat32DirEntry* out_entries, size_t max_entries,
                          size_t& out_count) {
    out_count = 0;
    if (!volume.mounted || out_entries == nullptr || max_entries == 0) {
        return false;
    }

    size_t collected = 0;
    bool ok = iterate_directory(volume, start_cluster,
                                [&](const uint8_t* entry,
                                    uint32_t dir_cluster,
                                    uint32_t raw_index,
                                    uint32_t,
                                    const char* long_name) -> IterationResult {
        if (collected < max_entries) {
            copy_entry(entry,
                       dir_cluster,
                       raw_index,
                       long_name,
                       out_entries[collected]);
        }
        ++collected;
        return IterationResult::Continue;
    });

    if (!ok) {
        return false;
    }

    out_count = (collected > max_entries) ? max_entries : collected;
    return true;
}

bool fat32_find_entry(Fat32Volume& volume, uint32_t directory_cluster,
                      const char* name, Fat32DirEntry& out_entry) {
    if (!volume.mounted || name == nullptr) {
        return false;
    }

    bool found = false;
    bool ok = iterate_directory(volume, directory_cluster,
                                [&](const uint8_t* raw,
                                    uint32_t dir_cluster,
                                    uint32_t raw_index,
                                    uint32_t,
                                    const char* long_name) -> IterationResult {
        const char* entry_long =
            (long_name != nullptr && long_name[0] != '\0') ? long_name : nullptr;
        char entry_name[64] = {0};
        format_83_name(raw, entry_name);
        if ((entry_long != nullptr && names_equal(entry_long, name)) ||
            names_equal(entry_name, name)) {
            copy_entry(raw, dir_cluster, raw_index, entry_long, out_entry);
            found = true;
            return IterationResult::StopSuccess;
        }
        return IterationResult::Continue;
    });

    return ok && found;
}

bool fat32_read_file(Fat32Volume& volume, const Fat32DirEntry& entry,
                     void* buffer, size_t buffer_size, size_t& out_size) {
    return fat32_read_file_range(volume, entry, 0, buffer, buffer_size,
                                 out_size);
}

bool fat32_read_file_range(Fat32Volume& volume, const Fat32DirEntry& entry,
                           uint32_t offset, void* buffer, size_t buffer_size,
                           size_t& out_size) {
    out_size = 0;
    if (!volume.mounted || buffer == nullptr) {
        return false;
    }
    if ((entry.attributes & ATTR_DIRECTORY) != 0) {
        log_message(LogLevel::Warn, "FAT32: attempt to read directory");
        return false;
    }

    if (offset >= entry.size) {
        return true;
    }

    const size_t cluster_size = static_cast<size_t>(volume.sectors_per_cluster) *
                                512u;
    if (cluster_size == 0) {
        return false;
    }
    if (cluster_size > sizeof(cluster_buffer)) {
        log_message(LogLevel::Warn,
                    "FAT32: cluster size %zu exceeds buffer capacity",
                    cluster_size);
        return false;
    }

    uint32_t cluster_index = offset / static_cast<uint32_t>(cluster_size);
    size_t cluster_offset =
        static_cast<size_t>(offset % static_cast<uint32_t>(cluster_size));
    uint32_t cluster = 0;
    if (!get_cluster_at_index(volume, entry.first_cluster, cluster_index, cluster)) {
        return false;
    }

    uint8_t* out_ptr = static_cast<uint8_t*>(buffer);
    size_t remaining_in_file = static_cast<size_t>(entry.size - offset);
    size_t remaining =
        (buffer_size < remaining_in_file) ? buffer_size : remaining_in_file;

    while (remaining != 0) {
        uint32_t lba = cluster_to_lba(volume, cluster);
        if (!read_sectors(volume.device, lba, volume.sectors_per_cluster,
                          cluster_buffer)) {
            return false;
        }

        size_t available = cluster_size - cluster_offset;
        if (available > remaining) {
            available = remaining;
        }

        memcpy(out_ptr, cluster_buffer + cluster_offset, available);
        out_ptr += available;
        out_size += available;
        remaining -= available;
        cluster_offset = 0;
        if (remaining == 0) {
            return true;
        }

        uint32_t next = read_fat_entry(volume, cluster);
        if (next >= kFatEoc) {
            return true;
        }
        if (next == kFatBadCluster) {
            log_message(LogLevel::Warn, "FAT32: bad cluster in chain");
            return false;
        }
        cluster = next;
    }

    return true;
}

bool fat32_write_file_range(Fat32Volume& volume,
                            Fat32DirEntry& entry,
                            uint32_t offset,
                            const void* buffer,
                            size_t buffer_size,
                            size_t& out_size) {
    out_size = 0;
    if (!volume.mounted || buffer == nullptr) {
        return false;
    }
    if ((entry.attributes & ATTR_DIRECTORY) != 0) {
        log_message(LogLevel::Warn, "FAT32: attempt to write directory");
        return false;
    }
    if (buffer_size == 0) {
        return true;
    }

    size_t cluster_size =
        static_cast<size_t>(volume.sectors_per_cluster) * 512u;
    if (cluster_size == 0) {
        return false;
    }
    if (cluster_size > sizeof(cluster_buffer)) {
        log_message(LogLevel::Warn,
                    "FAT32: cluster size %zu exceeds buffer capacity",
                    cluster_size);
        return false;
    }

    uint64_t end_offset64 =
        static_cast<uint64_t>(offset) + static_cast<uint64_t>(buffer_size);
    if (end_offset64 > UINT32_MAX) {
        log_message(LogLevel::Warn,
                    "FAT32: write exceeds maximum file size");
        return false;
    }
    uint32_t end_offset = static_cast<uint32_t>(end_offset64);

    uint32_t required_clusters =
        (end_offset == 0)
            ? 0
            : static_cast<uint32_t>(
                  (end_offset + static_cast<uint32_t>(cluster_size) - 1) /
                  static_cast<uint32_t>(cluster_size));

    if (required_clusters > 0) {
        if (!ensure_cluster_count(volume, entry, required_clusters)) {
            return false;
        }
    }

    if (offset > entry.size) {
        uint32_t gap = offset - entry.size;
        if (!zero_range(volume, entry, entry.size, gap)) {
            return false;
        }
    }

    const uint8_t* src = static_cast<const uint8_t*>(buffer);
    size_t remaining = buffer_size;
    uint32_t current_offset = offset;

    while (remaining > 0) {
        uint32_t cluster_index =
            current_offset / static_cast<uint32_t>(cluster_size);
        uint32_t cluster_offset =
            current_offset % static_cast<uint32_t>(cluster_size);
        uint32_t cluster_number = 0;
        if (!get_cluster_at_index(volume,
                                  entry.first_cluster,
                                  cluster_index,
                                  cluster_number)) {
            return false;
        }

        size_t chunk = cluster_size - cluster_offset;
        if (chunk > remaining) {
            chunk = remaining;
        }

        uint32_t lba = cluster_to_lba(volume, cluster_number);
        if (chunk == cluster_size && cluster_offset == 0) {
            if (!write_sectors(volume.device,
                               lba,
                               volume.sectors_per_cluster,
                               src)) {
                return false;
            }
        } else {
            if (!read_sectors(volume.device,
                              lba,
                              volume.sectors_per_cluster,
                              cluster_buffer)) {
                return false;
            }
            memcpy(cluster_buffer + cluster_offset, src, chunk);
            if (!write_sectors(volume.device,
                               lba,
                               volume.sectors_per_cluster,
                               cluster_buffer)) {
                return false;
            }
        }

        src += chunk;
        remaining -= chunk;
        current_offset += static_cast<uint32_t>(chunk);
        out_size += chunk;
    }

    if (end_offset > entry.size) {
        entry.size = end_offset;
    }

    return update_directory_entry(volume, entry);
}

bool fat32_get_entry_by_index(Fat32Volume& volume, uint32_t directory_cluster,
                              size_t index, Fat32DirEntry& out_entry) {
    if (!volume.mounted) {
        return false;
    }

    bool found = false;
    bool ok = iterate_directory(volume, directory_cluster,
                                [&](const uint8_t* raw,
                                    uint32_t dir_cluster,
                                    uint32_t raw_index,
                                    uint32_t visible_index,
                                    const char* long_name) -> IterationResult {
        if (visible_index == index) {
            copy_entry(raw, dir_cluster, raw_index, long_name, out_entry);
            found = true;
            return IterationResult::StopSuccess;
        }
        return IterationResult::Continue;
    });

    return ok && found;
}

namespace {

constexpr size_t kMaxOpenFiles = 64;
constexpr size_t kMaxOpenDirectories = 32;

struct Fat32FileContext {
    Fat32Volume* volume;
    Fat32DirEntry entry;
    uint32_t cached_cluster;
    uint32_t cached_cluster_index;
    bool cache_valid;
};

struct Fat32DirectoryContext {
    Fat32Volume* volume;
    uint32_t cluster;
    uint32_t next_index;
};

Fat32FileContext g_file_contexts[kMaxOpenFiles];
bool g_file_context_used[kMaxOpenFiles];

Fat32DirectoryContext g_directory_contexts[kMaxOpenDirectories];
bool g_directory_context_used[kMaxOpenDirectories];

Fat32FileContext* allocate_file_context() {
    for (size_t i = 0; i < kMaxOpenFiles; ++i) {
        if (!g_file_context_used[i]) {
            g_file_context_used[i] = true;
            g_file_contexts[i].volume = nullptr;
            memset(&g_file_contexts[i].entry, 0, sizeof(Fat32DirEntry));
            g_file_contexts[i].cached_cluster = 0;
            g_file_contexts[i].cached_cluster_index = 0;
            g_file_contexts[i].cache_valid = false;
            return &g_file_contexts[i];
        }
    }
    return nullptr;
}

bool get_file_context_cluster_at_index(Fat32FileContext& ctx,
                                       uint32_t index,
                                       uint32_t& out_cluster) {
    if (ctx.volume == nullptr) {
        return false;
    }

    uint32_t cluster = ctx.entry.first_cluster;
    uint32_t cluster_index = 0;
    if (ctx.cache_valid && ctx.cached_cluster >= 2 &&
        ctx.cached_cluster_index <= index) {
        cluster = ctx.cached_cluster;
        cluster_index = ctx.cached_cluster_index;
    }

    while (cluster_index < index) {
        uint32_t next = read_fat_entry(*ctx.volume, cluster);
        if (next == kFatBadCluster) {
            log_message(LogLevel::Warn, "FAT32: bad cluster in chain");
            return false;
        }
        if (next >= kFatEoc) {
            return false;
        }
        cluster = next;
        ++cluster_index;
        if (cluster_index > ctx.volume->total_clusters + 1) {
            log_message(LogLevel::Warn, "FAT32: cluster traversal overflow");
            return false;
        }
    }

    ctx.cached_cluster = cluster;
    ctx.cached_cluster_index = cluster_index;
    ctx.cache_valid = true;
    out_cluster = cluster;
    return true;
}

void release_file_context(Fat32FileContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    size_t index = static_cast<size_t>(ctx - g_file_contexts);
    if (index < kMaxOpenFiles) {
        g_file_context_used[index] = false;
    }
}

Fat32DirectoryContext* allocate_directory_context() {
    for (size_t i = 0; i < kMaxOpenDirectories; ++i) {
        if (!g_directory_context_used[i]) {
            g_directory_context_used[i] = true;
            g_directory_contexts[i].volume = nullptr;
            g_directory_contexts[i].cluster = 0;
            g_directory_contexts[i].next_index = 0;
            return &g_directory_contexts[i];
        }
    }
    return nullptr;
}

void release_directory_context(Fat32DirectoryContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    size_t index = static_cast<size_t>(ctx - g_directory_contexts);
    if (index < kMaxOpenDirectories) {
        g_directory_context_used[index] = false;
    }
}

bool fat32_vfs_list_directory(void* fs_context,
                              const char* path,
                              vfs::DirEntry* entries,
                              size_t max_entries,
                              size_t& out_count) {
    out_count = 0;
    if (fs_context == nullptr || entries == nullptr || max_entries == 0) {
        return false;
    }

    auto* volume = static_cast<Fat32Volume*>(fs_context);
    uint32_t cluster = 0;
    if (!resolve_directory_cluster(*volume, path, cluster)) {
        return false;
    }

    size_t collected = 0;
    while (collected < max_entries) {
        Fat32DirEntry entry{};
        if (!fat32_get_entry_by_index(*volume, cluster, collected, entry)) {
            break;
        }
        to_vfs_entry(entry, entries[collected]);
        ++collected;
    }

    out_count = collected;
    return true;
}

bool fat32_vfs_open_file(void* fs_context,
                         const char* path,
                         void*& out_file_context,
                         vfs::DirEntry* out_metadata) {
    if (fs_context == nullptr || path == nullptr || *path == '\0') {
        return false;
    }

    auto* volume = static_cast<Fat32Volume*>(fs_context);
    Fat32DirEntry entry{};
    if (!resolve_entry(*volume, path, entry)) {
        return false;
    }
    if ((entry.attributes & ATTR_DIRECTORY) != 0) {
        return false;
    }

    Fat32FileContext* ctx = allocate_file_context();
    if (ctx == nullptr) {
        log_message(LogLevel::Warn, "FAT32: out of file contexts");
        return false;
    }
    ctx->volume = volume;
    ctx->entry = entry;
    out_file_context = ctx;

    if (out_metadata != nullptr) {
        to_vfs_entry(entry, *out_metadata);
    }
    return true;
}

bool fat32_vfs_create_file(void* fs_context,
                           const char* path,
                           void*& out_file_context,
                           vfs::DirEntry* out_metadata) {
    if (fs_context == nullptr || path == nullptr || *path == '\0') {
        return false;
    }

    auto* volume = static_cast<Fat32Volume*>(fs_context);
    Fat32DirEntry entry{};
    if (!fat32_create_file(*volume, path, entry)) {
        return false;
    }

    Fat32FileContext* ctx = allocate_file_context();
    if (ctx == nullptr) {
        log_message(LogLevel::Warn, "FAT32: out of file contexts");
        return false;
    }

    ctx->volume = volume;
    ctx->entry = entry;
    out_file_context = ctx;

    if (out_metadata != nullptr) {
        to_vfs_entry(entry, *out_metadata);
    }
    return true;
}

bool fat32_vfs_create_directory(void* fs_context, const char* path) {
    auto* volume = static_cast<Fat32Volume*>(fs_context);
    if (volume == nullptr || path == nullptr) {
        return false;
    }
    Fat32DirEntry entry{};
    if (!fat32_create_directory(*volume, path, entry)) {
        return false;
    }
    (void)entry;
    return true;
}

bool fat32_vfs_remove_file(void* fs_context, const char* path) {
    auto* volume = static_cast<Fat32Volume*>(fs_context);
    if (volume == nullptr || path == nullptr || *path == '\0') {
        return false;
    }
    return fat32_remove_file(*volume, path);
}

bool fat32_vfs_remove_directory(void* fs_context, const char* path) {
    auto* volume = static_cast<Fat32Volume*>(fs_context);
    if (volume == nullptr || path == nullptr || *path == '\0') {
        return false;
    }
    return fat32_remove_directory(*volume, path);
}

bool fat32_vfs_read_file(void* file_context,
                         uint64_t offset,
                         void* buffer,
                         size_t buffer_size,
                         size_t& out_size) {
    out_size = 0;
    if (file_context == nullptr || buffer == nullptr) {
        return false;
    }

    auto* ctx = static_cast<Fat32FileContext*>(file_context);
    if (offset > 0xFFFFFFFFull) {
        return true;
    }

    uint32_t offset32 = static_cast<uint32_t>(offset);
    if (offset32 >= ctx->entry.size) {
        return true;
    }
    if (buffer_size == 0) {
        return true;
    }

    Fat32Volume& volume = *ctx->volume;
    const size_t cluster_size =
        static_cast<size_t>(volume.sectors_per_cluster) * 512u;
    if (cluster_size == 0) {
        return false;
    }
    if (cluster_size > sizeof(cluster_buffer)) {
        log_message(LogLevel::Warn,
                    "FAT32: cluster size %zu exceeds buffer capacity",
                    cluster_size);
        return false;
    }

    uint32_t cluster_index = offset32 / static_cast<uint32_t>(cluster_size);
    size_t cluster_offset =
        static_cast<size_t>(offset32 % static_cast<uint32_t>(cluster_size));
    uint32_t cluster = 0;
    if (!get_file_context_cluster_at_index(*ctx, cluster_index, cluster)) {
        return false;
    }

    auto* out_ptr = static_cast<uint8_t*>(buffer);
    size_t remaining_in_file = static_cast<size_t>(ctx->entry.size - offset32);
    size_t remaining =
        (buffer_size < remaining_in_file) ? buffer_size : remaining_in_file;

    while (remaining != 0) {
        uint32_t lba = cluster_to_lba(volume, cluster);
        if (!read_sectors(volume.device, lba, volume.sectors_per_cluster,
                          cluster_buffer)) {
            return false;
        }

        size_t available = cluster_size - cluster_offset;
        if (available > remaining) {
            available = remaining;
        }

        memcpy(out_ptr, cluster_buffer + cluster_offset, available);
        out_ptr += available;
        out_size += available;
        remaining -= available;
        cluster_offset = 0;
        ctx->cached_cluster = cluster;
        ctx->cached_cluster_index = cluster_index;
        ctx->cache_valid = true;
        if (remaining == 0) {
            return true;
        }

        uint32_t next = read_fat_entry(volume, cluster);
        if (next >= kFatEoc) {
            return true;
        }
        if (next == kFatBadCluster) {
            log_message(LogLevel::Warn, "FAT32: bad cluster in chain");
            return false;
        }
        cluster = next;
        ++cluster_index;
        if (cluster_index > volume.total_clusters + 1) {
            log_message(LogLevel::Warn, "FAT32: cluster traversal overflow");
            return false;
        }
    }

    return true;
}

bool fat32_vfs_write_file(void* file_context,
                          uint64_t offset,
                          const void* buffer,
                          size_t buffer_size,
                          size_t& out_size) {
    out_size = 0;
    if (file_context == nullptr || buffer == nullptr) {
        return false;
    }

    auto* ctx = static_cast<Fat32FileContext*>(file_context);
    if (offset > 0xFFFFFFFFull) {
        return true;
    }

    uint32_t offset32 = static_cast<uint32_t>(offset);
    ctx->cache_valid = false;
    return fat32_write_file_range(*ctx->volume,
                                  ctx->entry,
                                  offset32,
                                  buffer,
                                  buffer_size,
                                  out_size);
}

void fat32_vfs_close_file(void* file_context) {
    auto* ctx = static_cast<Fat32FileContext*>(file_context);
    release_file_context(ctx);
}

bool fat32_vfs_open_directory(void* fs_context,
                              const char* path,
                              void*& out_dir_context) {
    if (fs_context == nullptr) {
        return false;
    }

    auto* volume = static_cast<Fat32Volume*>(fs_context);
    uint32_t cluster = 0;
    if (!resolve_directory_cluster(*volume, path, cluster)) {
        return false;
    }

    Fat32DirectoryContext* ctx = allocate_directory_context();
    if (ctx == nullptr) {
        log_message(LogLevel::Warn, "FAT32: out of directory contexts");
        return false;
    }

    ctx->volume = volume;
    ctx->cluster = cluster;
    ctx->next_index = 0;
    out_dir_context = ctx;
    return true;
}

bool fat32_vfs_directory_next(void* dir_context, vfs::DirEntry& out_entry) {
    if (dir_context == nullptr) {
        return false;
    }

    auto* ctx = static_cast<Fat32DirectoryContext*>(dir_context);
    Fat32DirEntry entry{};
    if (!fat32_get_entry_by_index(*ctx->volume,
                                  ctx->cluster,
                                  ctx->next_index,
                                  entry)) {
        return false;
    }

    ++ctx->next_index;
    to_vfs_entry(entry, out_entry);
    return true;
}

void fat32_vfs_close_directory(void* dir_context) {
    auto* ctx = static_cast<Fat32DirectoryContext*>(dir_context);
    release_directory_context(ctx);
}

const vfs::FilesystemOps kFat32FilesystemOps{
    [](void* fs_context) -> const char* {
        if (fs_context == nullptr) return nullptr;
        auto* volume = static_cast<Fat32Volume*>(fs_context);
        return volume->preferred_alias[0] != '\0' ? volume->preferred_alias
                                                   : nullptr;
    },
    &fat32_vfs_list_directory,
    &fat32_vfs_open_file,
    &fat32_vfs_create_file,
    &fat32_vfs_create_directory,
    &fat32_vfs_remove_file,
    &fat32_vfs_remove_directory,
    &fat32_vfs_read_file,
    &fat32_vfs_write_file,
    &fat32_vfs_close_file,
    &fat32_vfs_open_directory,
    &fat32_vfs_directory_next,
    &fat32_vfs_close_directory,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

}  // namespace

const vfs::FilesystemOps& fat32_vfs_ops() {
    return kFat32FilesystemOps;
}
