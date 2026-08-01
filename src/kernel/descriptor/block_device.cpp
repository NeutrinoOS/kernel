#include "../descriptor.hpp"

#include "../../drivers/fs/block_device.hpp"
#include "../../drivers/log/logging.hpp"
#include "../process.hpp"
#include "../string_util.hpp"
#include "../vm.hpp"
#include "../../lib/mem.hpp"

namespace descriptor {

namespace block_device_descriptor {

constexpr size_t kMaxBlockDescriptors = 32;
constexpr size_t kMaxBlockDeviceNameLen = 32;
constexpr size_t kBlockIoBufferSize = 128 * 1024;

struct BlockDeviceRecord {
    fs::BlockDevice device;
    uint32_t handle;
    bool locked;
    bool in_use;
    char name[kMaxBlockDeviceNameLen];
};

struct DiskRecord {
    bool in_use;
    char name[kMaxBlockDeviceNameLen];
};

BlockDeviceRecord g_block_devices[kMaxBlockDescriptors];
DiskRecord g_disk_records[kMaxBlockDescriptors];

BlockDeviceRecord* find_block_device_by_name(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (auto& record : g_block_devices) {
        if (!record.in_use) {
            continue;
        }
        if (string_util::equals(record.name, name)) {
            return &record;
        }
    }
    return nullptr;
}

BlockDeviceRecord* find_block_device_by_index(uint64_t index) {
    size_t count = 0;
    for (auto& record : g_block_devices) {
        if (!record.in_use) {
            continue;
        }
        if (count == index) {
            return &record;
        }
        ++count;
    }
    return nullptr;
}

bool is_whole_disk_record(const BlockDeviceRecord& record) {
    return record.in_use &&
           record.device.parent_name == nullptr &&
           record.device.partition_index == 0xFFFFFFFFu;
}

bool starts_with_disk_partition_prefix(const char* name, const char* disk_name) {
    if (name == nullptr || disk_name == nullptr || disk_name[0] == '\0') {
        return false;
    }
    size_t idx = 0;
    while (disk_name[idx] != '\0') {
        if (name[idx] != disk_name[idx]) {
            return false;
        }
        ++idx;
    }
    return name[idx] == '_';
}

bool block_device_belongs_to_disk(const BlockDeviceRecord& record,
                                  const char* disk_name) {
    if (!record.in_use) {
        return false;
    }
    if (is_whole_disk_record(record)) {
        return false;
    }
    if (record.device.parent_name != nullptr &&
        string_util::equals(record.device.parent_name, disk_name)) {
        return true;
    }
    return starts_with_disk_partition_prefix(record.name, disk_name);
}

BlockDeviceRecord* find_partition_by_index(const char* disk_name,
                                           uint64_t index) {
    size_t count = 0;
    for (auto& record : g_block_devices) {
        if (!block_device_belongs_to_disk(record, disk_name)) {
            continue;
        }
        if (count == index) {
            return &record;
        }
        ++count;
    }
    return nullptr;
}

size_t partition_count_for_disk(const char* disk_name) {
    size_t count = 0;
    for (auto& record : g_block_devices) {
        if (block_device_belongs_to_disk(record, disk_name)) {
            ++count;
        }
    }
    return count;
}

bool disk_is_removable(const char* disk_name) {
    BlockDeviceRecord* disk = find_block_device_by_name(disk_name);
    if (disk != nullptr && is_whole_disk_record(*disk)) {
        return disk->device.removable;
    }
    for (auto& record : g_block_devices) {
        if (block_device_belongs_to_disk(record, disk_name)) {
            return record.device.removable;
        }
    }
    return false;
}

bool disk_exists(const char* disk_name) {
    BlockDeviceRecord* record = find_block_device_by_name(disk_name);
    if (record != nullptr && is_whole_disk_record(*record)) {
        return true;
    }
    return partition_count_for_disk(disk_name) != 0;
}

bool disk_name_seen(const char* disk_name, char names[][kMaxBlockDeviceNameLen],
                    size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (string_util::equals(names[i], disk_name)) {
            return true;
        }
    }
    return false;
}

bool disk_name_from_partition(const char* partition_name,
                              char* out,
                              size_t out_size) {
    if (partition_name == nullptr || out == nullptr || out_size == 0) {
        return false;
    }
    size_t len = string_util::length(partition_name);
    while (len > 0 && partition_name[len - 1] != '_') {
        --len;
    }
    if (len == 0 || len >= out_size) {
        return false;
    }
    --len;
    if (len == 0 || len >= out_size) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        out[i] = partition_name[i];
    }
    out[len] = '\0';
    return true;
}

bool disk_name_by_index(uint64_t index, char* out, size_t out_size) {
    char seen[kMaxBlockDescriptors][kMaxBlockDeviceNameLen]{};
    size_t seen_count = 0;
    for (auto& record : g_block_devices) {
        if (!record.in_use) {
            continue;
        }
        char disk_name[kMaxBlockDeviceNameLen]{};
        if (is_whole_disk_record(record)) {
            string_util::copy(disk_name, sizeof(disk_name), record.name);
        } else if (record.device.parent_name != nullptr) {
            string_util::copy(disk_name,
                              sizeof(disk_name),
                              record.device.parent_name);
        } else if (!disk_name_from_partition(record.name,
                                             disk_name,
                                             sizeof(disk_name))) {
            continue;
        }
        if (disk_name_seen(disk_name, seen, seen_count)) {
            continue;
        }
        if (seen_count == index) {
            string_util::copy(out, out_size, disk_name);
            return true;
        }
        string_util::copy(seen[seen_count], sizeof(seen[seen_count]), disk_name);
        ++seen_count;
    }
    return false;
}

DiskRecord* allocate_disk_record(const char* name) {
    for (auto& record : g_disk_records) {
        if (!record.in_use) {
            record.in_use = true;
            string_util::copy(record.name, sizeof(record.name), name);
            return &record;
        }
    }
    return nullptr;
}

DescriptorEntry* lookup_process_entry(process::Task& proc, uint32_t handle) {
    uint16_t index = handle_index(handle);
    uint16_t generation = handle_generation(handle);
    if (index >= kMaxDescriptors || generation == 0) {
        return nullptr;
    }
    DescriptorEntry& entry = proc.resources->descriptors.entries[index];
    if (!entry.in_use || entry.generation != generation) {
        return nullptr;
    }
    return &entry;
}

alignas(4096) uint8_t g_sync_block_io_buffer[kBlockIoBufferSize];
volatile int g_sync_io_lock = 0;

void lock_sync_io() {
    while (__atomic_test_and_set(&g_sync_io_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock_sync_io() {
    __atomic_clear(&g_sync_io_lock, __ATOMIC_RELEASE);
}

void close_disk(DescriptorEntry& entry) {
    auto* record = static_cast<DiskRecord*>(entry.object);
    if (record != nullptr) {
        record->in_use = false;
        record->name[0] = '\0';
    }
}

bool copy_from_caller(const process::Task& proc,
                      void* dest,
                      uint64_t src,
                      size_t length) {
    if (length == 0) {
        return true;
    }
    if (dest == nullptr || src == 0) {
        return false;
    }
    if (is_kernel_process(proc)) {
        memcpy(dest, reinterpret_cast<const void*>(src), length);
        return true;
    }
    return vm::copy_from_user(proc.cr3, dest, src, length);
}

bool copy_to_caller(const process::Task& proc,
                    uint64_t dest,
                    const void* src,
                    size_t length) {
    if (length == 0) {
        return true;
    }
    if (dest == 0 || src == nullptr) {
        return false;
    }
    if (is_kernel_process(proc)) {
        memcpy(reinterpret_cast<void*>(dest), src, length);
        return true;
    }
    return vm::copy_to_user(proc.cr3, dest, src, length);
}

int64_t block_device_read(process::Task& proc,
                          DescriptorEntry& entry,
                          uint64_t user_address,
                          uint64_t length,
                          uint64_t offset) {
    auto* record = static_cast<BlockDeviceRecord*>(entry.object);
    if (record == nullptr || !record->in_use) {
        return -1;
    }
    if (record->locked && !is_kernel_process(proc)) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    uint64_t sector_size = record->device.sector_size;
    if (sector_size == 0) {
        return -1;
    }
    if ((offset % sector_size) != 0) {
        return -1;
    }
    if ((length % sector_size) != 0) {
        return -1;
    }
    uint64_t sector_count = length / sector_size;
    if (sector_count == 0) {
        return -1;
    }
    uint64_t lba = offset / sector_size;
    if (lba >= record->device.sector_count) {
        return -1;
    }
    if (lba + sector_count > record->device.sector_count) {
        return -1;
    }

    auto read_fn = record->device.read;
    if (read_fn == nullptr) {
        return -1;
    }
    uint64_t remaining = sector_count;
    uint64_t current_lba = lba;
    if (user_address == 0) {
        return -1;
    }
    size_t max_sectors =
        static_cast<size_t>(sizeof(g_sync_block_io_buffer) / sector_size);
    if (max_sectors == 0) {
        return -1;
    }

    lock_sync_io();
    while (remaining > 0) {
        uint64_t chunk64 = remaining;
        if (chunk64 > 0xFFu) {
            chunk64 = 0xFFu;
        }
        if (chunk64 > max_sectors) {
            chunk64 = max_sectors;
        }
        uint8_t chunk = static_cast<uint8_t>(chunk64);
        fs::BlockIoStatus status =
            read_fn(record->device.context, static_cast<uint32_t>(current_lba),
                    chunk, g_sync_block_io_buffer);
        if (status != fs::BlockIoStatus::Ok) {
            unlock_sync_io();
            return -1;
        }
        size_t transfer_bytes =
            static_cast<size_t>(chunk) * static_cast<size_t>(sector_size);
        uint64_t dest = user_address +
                        (current_lba - lba) * sector_size;
        if (!copy_to_caller(proc, dest, g_sync_block_io_buffer, transfer_bytes)) {
            unlock_sync_io();
            return -1;
        }
        current_lba += chunk;
        remaining -= chunk;
    }
    unlock_sync_io();
    return static_cast<int64_t>(length);
}

int64_t block_device_write(process::Task& proc,
                           DescriptorEntry& entry,
                           uint64_t user_address,
                           uint64_t length,
                           uint64_t offset) {
    auto* record = static_cast<BlockDeviceRecord*>(entry.object);
    if (record == nullptr || !record->in_use) {
        return -1;
    }
    if (record->locked && !is_kernel_process(proc)) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    uint64_t sector_size = record->device.sector_size;
    if (sector_size == 0) {
        return -1;
    }
    if ((offset % sector_size) != 0) {
        return -1;
    }
    if ((length % sector_size) != 0) {
        return -1;
    }
    uint64_t sector_count = length / sector_size;
    if (sector_count == 0) {
        return -1;
    }
    uint64_t lba = offset / sector_size;
    if (lba >= record->device.sector_count) {
        return -1;
    }
    if (lba + sector_count > record->device.sector_count) {
        return -1;
    }

    auto write_fn = record->device.write;
    if (write_fn == nullptr) {
        return -1;
    }
    uint64_t remaining = sector_count;
    uint64_t current_lba = lba;
    if (user_address == 0) {
        return -1;
    }
    size_t max_sectors =
        static_cast<size_t>(sizeof(g_sync_block_io_buffer) / sector_size);
    if (max_sectors == 0) {
        return -1;
    }

    lock_sync_io();
    while (remaining > 0) {
        uint64_t chunk64 = remaining;
        if (chunk64 > 0xFFu) {
            chunk64 = 0xFFu;
        }
        if (chunk64 > max_sectors) {
            chunk64 = max_sectors;
        }
        uint8_t chunk = static_cast<uint8_t>(chunk64);
        size_t transfer_bytes =
            static_cast<size_t>(chunk) * static_cast<size_t>(sector_size);
        uint64_t src = user_address +
                       (current_lba - lba) * sector_size;
        if (!copy_from_caller(proc, g_sync_block_io_buffer, src, transfer_bytes)) {
            unlock_sync_io();
            return -1;
        }
        fs::BlockIoStatus status =
            write_fn(record->device.context,
                     static_cast<uint32_t>(current_lba),
                     chunk,
                     g_sync_block_io_buffer);
        if (status != fs::BlockIoStatus::Ok) {
            unlock_sync_io();
            return -1;
        }
        current_lba += chunk;
        remaining -= chunk;
    }
    unlock_sync_io();
    return static_cast<int64_t>(length);
}

int block_device_get_property(DescriptorEntry& entry,
                              uint32_t property,
                              void* out,
                              size_t size) {
    auto* record = static_cast<BlockDeviceRecord*>(entry.object);
    if (record == nullptr || !record->in_use) {
        return -1;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::BlockGeometry)) {
        if (out == nullptr || size < sizeof(descriptor_defs::BlockGeometry)) {
            return -1;
        }
        auto* geom = reinterpret_cast<descriptor_defs::BlockGeometry*>(out);
        geom->sector_size = record->device.sector_size;
        geom->sector_count = record->device.sector_count;
        return 0;
    }
    return -1;
}

int disk_get_property(DescriptorEntry& entry,
                      uint32_t property,
                      void* out,
                      size_t size) {
    auto* record = static_cast<DiskRecord*>(entry.object);
    if (record == nullptr || !record->in_use) {
        return -1;
    }
    if (property == static_cast<uint32_t>(descriptor_defs::Property::DiskInfo)) {
        if (out == nullptr || size < sizeof(descriptor_defs::DiskInfo)) {
            return -1;
        }
        auto* info = reinterpret_cast<descriptor_defs::DiskInfo*>(out);
        *info = {};
        string_util::copy(info->name, sizeof(info->name), record->name);
        info->partition_count =
            static_cast<uint32_t>(partition_count_for_disk(record->name));
        if (disk_is_removable(record->name)) {
            info->flags |= descriptor_defs::kDiskFlagRemovable;
        }
        return 0;
    }
    return -1;
}

int partition_get_property(DescriptorEntry& entry,
                           uint32_t property,
                           void* out,
                           size_t size) {
    if (property == static_cast<uint32_t>(
                        descriptor_defs::Property::PartitionInfo)) {
        auto* record = static_cast<BlockDeviceRecord*>(entry.object);
        if (record == nullptr || !record->in_use || out == nullptr ||
            size < sizeof(descriptor_defs::PartitionInfo)) {
            return -1;
        }
        auto* info = reinterpret_cast<descriptor_defs::PartitionInfo*>(out);
        *info = {};
        string_util::copy(info->name, sizeof(info->name), record->name);
        info->start_lba = record->device.start_lba;
        info->sector_count = record->device.sector_count;
        info->index = record->device.partition_index;
        info->type = record->device.partition_type;
        return 0;
    }
    return block_device_get_property(entry, property, out, size);
}

int64_t disk_read(process::Task& proc,
                  DescriptorEntry& entry,
                  uint64_t user_address,
                  uint64_t length,
                  uint64_t offset) {
    auto* disk = static_cast<DiskRecord*>(entry.object);
    if (disk == nullptr || !disk->in_use || user_address == 0 ||
        length == 0 ||
        (offset % sizeof(descriptor_defs::PartitionInfo)) != 0) {
        return -1;
    }

    uint64_t index = offset / sizeof(descriptor_defs::PartitionInfo);
    size_t written = 0;
    while (written + sizeof(descriptor_defs::PartitionInfo) <= length) {
        BlockDeviceRecord* part = find_partition_by_index(disk->name, index);
        if (part == nullptr) {
            break;
        }

        descriptor_defs::PartitionInfo info{};
        string_util::copy(info.name, sizeof(info.name), part->name);
        info.start_lba = part->device.start_lba;
        info.sector_count = part->device.sector_count;
        info.index = part->device.partition_index;
        info.type = part->device.partition_type;
        if (!copy_to_caller(proc,
                            user_address + written,
                            &info,
                            sizeof(info))) {
            return -1;
        }
        written += sizeof(info);
        ++index;
    }
    return static_cast<int64_t>(written);
}

const Ops kBlockDeviceOps{
    .read = block_device_read,
    .write = block_device_write,
    .get_property = block_device_get_property,
    .set_property = nullptr,
};

const Ops kDiskOps{
    .read = disk_read,
    .write = nullptr,
    .get_property = disk_get_property,
    .set_property = nullptr,
};

const Ops kPartitionOps{
    .read = block_device_read,
    .write = block_device_write,
    .get_property = partition_get_property,
    .set_property = nullptr,
};

bool open_block_device(process::Task& proc,
                       uint64_t name_ptr,
                       uint64_t index,
                       uint64_t,
                       Allocation& alloc) {
    BlockDeviceRecord* record = nullptr;
    if (name_ptr != 0) {
        char name[kMaxBlockDeviceNameLen];
        if (is_kernel_process(proc)) {
            string_util::copy(name,
                              sizeof(name),
                              reinterpret_cast<const char*>(name_ptr));
        } else if (!vm::copy_user_string(
                       proc.cr3,
                       reinterpret_cast<const char*>(name_ptr),
                       name,
                       sizeof(name))) {
            return false;
        }
        record = find_block_device_by_name(name);
    } else {
        record = find_block_device_by_index(index);
    }
    if (record == nullptr || !record->in_use) {
        return false;
    }
    if (record->locked && !is_kernel_process(proc)) {
        return false;
    }
    alloc.type = kTypeBlockDevice;
    uint64_t flags = static_cast<uint64_t>(Flag::Seekable) |
                     static_cast<uint64_t>(Flag::Device) |
                     static_cast<uint64_t>(Flag::Block) |
                     static_cast<uint64_t>(Flag::Async);
    if (record->device.read != nullptr) {
        flags |= static_cast<uint64_t>(Flag::Readable);
    }
    if (record->device.write != nullptr) {
        flags |= static_cast<uint64_t>(Flag::Writable);
    }
    alloc.flags = flags;
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = record;
    alloc.subsystem_data = nullptr;
    // Registered block devices are owned by the device registry. Closing a
    // descriptor must not treat their records as temporary DiskRecords.
    alloc.close = nullptr;
    alloc.name = record->name;
    alloc.ops = &kBlockDeviceOps;
    return true;
}

bool open_disk(process::Task& proc,
               uint64_t name_ptr,
               uint64_t index,
               uint64_t,
               Allocation& alloc) {
    char disk_name[kMaxBlockDeviceNameLen]{};
    if (name_ptr != 0) {
        if (is_kernel_process(proc)) {
            string_util::copy(disk_name,
                              sizeof(disk_name),
                              reinterpret_cast<const char*>(name_ptr));
        } else if (!vm::copy_user_string(
                       proc.cr3,
                       reinterpret_cast<const char*>(name_ptr),
                       disk_name,
                       sizeof(disk_name))) {
            return false;
        }
    } else if (!disk_name_by_index(index, disk_name, sizeof(disk_name))) {
        return false;
    }
    if (!disk_exists(disk_name)) {
        return false;
    }
    DiskRecord* record = allocate_disk_record(disk_name);
    if (record == nullptr) {
        return false;
    }

    alloc.type = kTypeDisk;
    alloc.flags = static_cast<uint64_t>(Flag::Readable) |
                  static_cast<uint64_t>(Flag::Device) |
                  static_cast<uint64_t>(Flag::Block);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = record;
    alloc.subsystem_data = nullptr;
    alloc.close = close_disk;
    alloc.name = record->name;
    alloc.ops = &kDiskOps;
    return true;
}

bool open_partition(process::Task& proc,
                    uint64_t disk_handle,
                    uint64_t partition_index,
                    uint64_t,
                    Allocation& alloc) {
    DescriptorEntry* disk_entry =
        lookup_process_entry(proc, static_cast<uint32_t>(disk_handle));
    if (disk_entry == nullptr || disk_entry->type != kTypeDisk) {
        return false;
    }
    auto* disk = static_cast<DiskRecord*>(disk_entry->object);
    if (disk == nullptr || !disk->in_use) {
        return false;
    }

    BlockDeviceRecord* record =
        find_partition_by_index(disk->name, partition_index);
    if (record == nullptr ||
        (record->locked && !is_kernel_process(proc))) {
        return false;
    }

    alloc.type = kTypePartition;
    uint64_t flags = static_cast<uint64_t>(Flag::Seekable) |
                     static_cast<uint64_t>(Flag::Device) |
                     static_cast<uint64_t>(Flag::Block) |
                     static_cast<uint64_t>(Flag::Async);
    if (record->device.read != nullptr) {
        flags |= static_cast<uint64_t>(Flag::Readable);
    }
    if (record->device.write != nullptr) {
        flags |= static_cast<uint64_t>(Flag::Writable);
    }
    alloc.flags = flags;
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = record;
    alloc.subsystem_data = nullptr;
    alloc.close = nullptr;
    alloc.name = record->name;
    alloc.ops = &kPartitionOps;
    return true;
}

void clear_block_devices() {
    for (auto& record : g_block_devices) {
        if (record.handle != kInvalidHandle) {
            close_kernel(record.handle);
        }
        record.in_use = false;
        record.name[0] = '\0';
        record.device = {};
        record.device.descriptor_handle = kInvalidHandle;
        record.handle = kInvalidHandle;
        record.locked = false;
    }
    for (auto& record : g_disk_records) {
        record = {};
    }
}

}  // namespace block_device_descriptor

bool register_block_device_descriptor() {
    bool ok = register_type(kTypeBlockDevice,
                            block_device_descriptor::open_block_device,
                            &block_device_descriptor::kBlockDeviceOps);
    ok = register_type(kTypeDisk,
                       block_device_descriptor::open_disk,
                       &block_device_descriptor::kDiskOps) && ok;
    ok = register_type(kTypePartition,
                       block_device_descriptor::open_partition,
                       &block_device_descriptor::kPartitionOps) && ok;
    return ok;
}

bool block_device_from_descriptor(process::Task& proc,
                                  uint32_t handle,
                                  fs::BlockDevice& out) {
    sync::LockGuard guard(proc.resources->descriptor_lock);
    DescriptorEntry* entry =
        block_device_descriptor::lookup_process_entry(proc, handle);
    if (entry == nullptr ||
        (entry->type != kTypeBlockDevice && entry->type != kTypePartition)) {
        return false;
    }
    auto* record =
        static_cast<block_device_descriptor::BlockDeviceRecord*>(entry->object);
    if (record == nullptr || !record->in_use) {
        return false;
    }
    out = record->device;
    out.name = entry->name;
    out.descriptor_handle = kInvalidHandle;
    return true;
}

bool register_block_device(fs::BlockDevice& device, bool lock_for_kernel) {
    if (device.name == nullptr) {
        return false;
    }
    block_device_descriptor::BlockDeviceRecord* slot = block_device_descriptor::find_block_device_by_name(device.name);
    if (slot == nullptr) {
        for (auto& candidate : block_device_descriptor::g_block_devices) {
            if (!candidate.in_use) {
                slot = &candidate;
                break;
            }
        }
    }
    if (slot == nullptr) {
        log_message(LogLevel::Warn,
                    "Descriptor: block device registry full, dropping %s",
                    device.name);
        return false;
    }
    slot->device = device;
    string_util::copy(slot->name, block_device_descriptor::kMaxBlockDeviceNameLen, device.name);
    slot->device.name = slot->name;
    slot->locked = lock_for_kernel;
    slot->in_use = true;
    slot->handle = kInvalidHandle;
    slot->device.descriptor_handle = kInvalidHandle;
    device.name = slot->name;
    device.descriptor_handle = kInvalidHandle;

    if (lock_for_kernel) {
        slot->handle = open_kernel(kTypeBlockDevice,
                                   reinterpret_cast<uint64_t>(slot->name),
                                   0,
                                   0);
        if (slot->handle == kInvalidHandle) {
            log_message(LogLevel::Warn,
                        "Descriptor: failed to open block device descriptor for %s",
                        slot->name);
            slot->device = {};
            slot->in_use = false;
            slot->locked = false;
            slot->name[0] = '\0';
            device.descriptor_handle = kInvalidHandle;
            return false;
        }
        slot->device.descriptor_handle = slot->handle;
        device.descriptor_handle = slot->handle;
    }

    return true;
}

void reset_block_device_registry() {
    block_device_descriptor::clear_block_devices();
}

}  // namespace descriptor
