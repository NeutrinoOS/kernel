#include "block_cache.hpp"

#include <stddef.h>
#include <stdint.h>

#include "drivers/log/logging.hpp"
#include "kernel/error.hpp"
#include "lib/mem.hpp"

namespace fs {
namespace block_cache {
namespace {

constexpr size_t kMaxCachedDevices = 32;
constexpr size_t kCacheEntryCount = 65536;
constexpr size_t kCacheHashBucketCount = 65536;
constexpr size_t kCacheSectorSize = 512;
constexpr size_t kIdleFlushBudget = 16;
constexpr uint16_t kSequentialWriteThroughSectors = 4;

struct CachedDevice {
    BlockDevice backing;
    volatile int io_lock;
    uint64_t write_sequence;
    bool in_use;
    bool have_last_write;
    uint16_t sequential_write_sectors;
    uint32_t last_write_end_lba;
};

struct CacheEntry {
    CachedDevice* owner;
    uint32_t lba;
    uint64_t age;
    uint64_t generation;
    size_t sector_size;
    bool valid;
    bool dirty;
    bool flushing;
    int32_t hash_next;
    alignas(512) uint8_t data[kCacheSectorSize];
};

CachedDevice g_devices[kMaxCachedDevices]{};
CacheEntry g_entries[kCacheEntryCount]{};
int32_t g_hash_heads[kCacheHashBucketCount]{};
volatile int g_cache_lock = 0;
uint64_t g_clock = 0;
size_t g_victim_cursor = 0;
bool g_enabled = true;
size_t g_active_cached_ops = 0;
volatile int g_mode_lock = 0;

bool cache_enabled() {
    return __atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE);
}

void lock() {
    while (__atomic_test_and_set(&g_cache_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock() {
    __atomic_clear(&g_cache_lock, __ATOMIC_RELEASE);
}

class LockGuard {
public:
    LockGuard() { lock(); }
    ~LockGuard() { unlock(); }
};

class CacheOperation {
public:
    CacheOperation() {
        lock();
        cached_ = cache_enabled();
        if (cached_) {
            ++g_active_cached_ops;
        }
        unlock();
    }

    ~CacheOperation() {
        if (!cached_) {
            return;
        }
        lock();
        KERNEL_ASSERT_MSG(g_active_cached_ops != 0,
                          "block-cache active operation count underflow");
        --g_active_cached_ops;
        unlock();
    }

    bool uses_cache() const { return cached_; }

    CacheOperation(const CacheOperation&) = delete;
    CacheOperation& operator=(const CacheOperation&) = delete;

private:
    bool cached_{false};
};

void lock_mode() {
    while (__atomic_test_and_set(&g_mode_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock_mode() {
    __atomic_clear(&g_mode_lock, __ATOMIC_RELEASE);
}

void* byte_offset(void* ptr, size_t offset) {
    return static_cast<void*>(static_cast<uint8_t*>(ptr) + offset);
}

void lock_io(CachedDevice& cached) {
    while (__atomic_test_and_set(&cached.io_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock_io(CachedDevice& cached) {
    __atomic_clear(&cached.io_lock, __ATOMIC_RELEASE);
}

BlockIoStatus read_uncached(CachedDevice& cached,
                            uint32_t lba,
                            uint8_t sector_count,
                            void* buffer) {
    lock_io(cached);
    BlockIoStatus status =
        block_read(cached.backing, lba, sector_count, buffer);
    unlock_io(cached);
    return status;
}

BlockIoStatus write_uncached(CachedDevice& cached,
                             uint32_t lba,
                             uint8_t sector_count,
                             const void* buffer) {
    lock_io(cached);
    BlockIoStatus status =
        block_write(cached.backing, lba, sector_count, buffer);
    unlock_io(cached);
    return status;
}

size_t cache_hash(const CachedDevice& cached, uint32_t lba) {
    uintptr_t owner = reinterpret_cast<uintptr_t>(&cached);
    uint64_t mixed = static_cast<uint64_t>(lba) * 11400714819323198485ull;
    mixed ^= static_cast<uint64_t>(owner >> 4);
    return static_cast<size_t>(mixed) & (kCacheHashBucketCount - 1);
}

CacheEntry* find_entry(CachedDevice& cached, uint32_t lba, size_t sector_size) {
    int32_t index = g_hash_heads[cache_hash(cached, lba)];
    size_t traversed = 0;
    while (index >= 0) {
        KERNEL_ASSERT_MSG(static_cast<size_t>(index) < kCacheEntryCount,
                          "block-cache hash chain index is out of bounds");
        KERNEL_ASSERT_MSG(traversed++ < kCacheEntryCount,
                          "block-cache hash chain contains a cycle");
        CacheEntry& entry = g_entries[static_cast<size_t>(index)];
        if (entry.valid && entry.owner == &cached && entry.lba == lba &&
            entry.sector_size == sector_size) {
            return &entry;
        }
        index = entry.hash_next;
    }
    return nullptr;
}

void unlink_entry_locked(CacheEntry& target) {
    if (!target.valid || target.owner == nullptr) {
        target.hash_next = -1;
        return;
    }
    size_t bucket = cache_hash(*target.owner, target.lba);
    int32_t* link = &g_hash_heads[bucket];
    size_t traversed = 0;
    while (*link >= 0) {
        KERNEL_ASSERT_MSG(static_cast<size_t>(*link) < kCacheEntryCount,
                          "block-cache unlink index is out of bounds");
        KERNEL_ASSERT_MSG(traversed++ < kCacheEntryCount,
                          "block-cache unlink chain contains a cycle");
        CacheEntry& entry = g_entries[static_cast<size_t>(*link)];
        if (&entry == &target) {
            *link = entry.hash_next;
            entry.hash_next = -1;
            return;
        }
        link = &entry.hash_next;
    }
    target.hash_next = -1;
}

void link_entry_locked(CacheEntry& entry) {
    KERNEL_ASSERT_MSG(entry.owner != nullptr,
                      "block-cache entry has no owning device");
    size_t index = static_cast<size_t>(&entry - g_entries);
    KERNEL_ASSERT_MSG(index < kCacheEntryCount,
                      "block-cache entry is outside the cache table");
    size_t bucket = cache_hash(*entry.owner, entry.lba);
    entry.hash_next = g_hash_heads[bucket];
    g_hash_heads[bucket] = static_cast<int32_t>(index);
}

CacheEntry* choose_clean_victim() {
    CacheEntry* oldest = nullptr;
    for (size_t scanned = 0; scanned < kCacheEntryCount; ++scanned) {
        size_t index = (g_victim_cursor + scanned) % kCacheEntryCount;
        CacheEntry& entry = g_entries[index];
        if (!entry.valid) {
            g_victim_cursor = (index + 1) % kCacheEntryCount;
            return &entry;
        }
    }
    for (auto& entry : g_entries) {
        if (entry.dirty || entry.flushing) {
            continue;
        }
        if (oldest == nullptr) {
            oldest = &entry;
            continue;
        }
        if (entry.age < oldest->age) {
            oldest = &entry;
        }
    }
    return oldest;
}

bool flush_dirty_entry(CacheEntry* target) {
    CachedDevice* owner = nullptr;
    uint32_t lba = 0;
    uint64_t generation = 0;
    size_t sector_size = 0;
    alignas(512) uint8_t sector_buffer[kCacheSectorSize];

    lock();
    if (target == nullptr || !target->valid || !target->dirty ||
        target->flushing) {
        unlock();
        return true;
    }

    owner = target->owner;
    KERNEL_ASSERT_MSG(owner != nullptr,
                      "dirty block-cache entry has no owning device");
    lba = target->lba;
    generation = target->generation;
    sector_size = target->sector_size;
    KERNEL_ASSERT_MSG(sector_size != 0 &&
                          sector_size <= kCacheSectorSize,
                      "block-cache entry has an invalid sector size");
    // Claim the backing device while the cache identity is still protected.
    // A write-through invalidation cannot overtake this older snapshot and be
    // overwritten by it later.
    lock_io(*owner);
    target->flushing = true;
    memcpy(sector_buffer, target->data, sector_size);
    unlock();

    BlockIoStatus status =
        block_write(owner->backing, lba, 1, sector_buffer);
    unlock_io(*owner);

    lock();
    if (target->valid && target->owner == owner && target->lba == lba) {
        target->flushing = false;
        if (target->generation == generation &&
            status == BlockIoStatus::Ok) {
            target->dirty = false;
        }
    }
    unlock();
    return status == BlockIoStatus::Ok;
}

bool flush_one_dirty() {
    lock();
    CacheEntry* entry = nullptr;
    for (auto& candidate : g_entries) {
        if (!candidate.valid || !candidate.dirty || candidate.flushing) {
            continue;
        }
        if (entry == nullptr || candidate.age < entry->age) {
            entry = &candidate;
        }
    }
    unlock();

    if (entry == nullptr) {
        return false;
    }
    return flush_dirty_entry(entry);
}

CacheEntry* find_or_prepare_entry_locked(CachedDevice& cached,
                                         uint32_t lba,
                                         size_t sector_size) {
    CacheEntry* entry = find_entry(cached, lba, sector_size);
    if (entry != nullptr) {
        return entry;
    }
    entry = choose_clean_victim();
    if (entry == nullptr) {
        return nullptr;
    }
    if (entry->valid) {
        unlink_entry_locked(*entry);
    }
    entry->owner = &cached;
    entry->lba = lba;
    entry->sector_size = sector_size;
    entry->valid = true;
    entry->dirty = false;
    entry->flushing = false;
    link_entry_locked(*entry);
    return entry;
}

void invalidate_write_range_locked(CachedDevice& cached,
                                   uint32_t lba,
                                   uint8_t sector_count) {
    for (uint16_t i = 0; i < sector_count; ++i) {
        CacheEntry* entry = find_entry(cached,
                                       lba + static_cast<uint32_t>(i),
                                       cached.backing.sector_size);
        if (entry == nullptr) {
            continue;
        }
        unlink_entry_locked(*entry);
        entry->owner = nullptr;
        entry->valid = false;
        entry->dirty = false;
        entry->flushing = false;
    }
}

BlockIoStatus write_through(CachedDevice& cached,
                            uint32_t lba,
                            uint8_t sector_count,
                            const void* buffer) {
    // Establish cache invalidation and backing-I/O order as one operation.
    // Readers snapshot write_sequence under the same cache lock, so they
    // cannot install a backing read that this write has made stale.
    lock();
    lock_io(cached);
    ++cached.write_sequence;
    invalidate_write_range_locked(cached, lba, sector_count);
    unlock();

    BlockIoStatus status =
        block_write(cached.backing, lba, sector_count, buffer);
    unlock_io(cached);
    return status;
}

bool should_write_through(CachedDevice& cached,
                          uint32_t lba,
                          uint8_t sector_count) {
    LockGuard guard;
    if (cached.have_last_write && lba == cached.last_write_end_lba) {
        uint32_t total = static_cast<uint32_t>(cached.sequential_write_sectors) +
                         static_cast<uint32_t>(sector_count);
        cached.sequential_write_sectors =
            static_cast<uint16_t>(total > UINT16_MAX ? UINT16_MAX : total);
    } else {
        cached.sequential_write_sectors = sector_count;
    }
    cached.have_last_write = true;
    cached.last_write_end_lba = lba + static_cast<uint32_t>(sector_count);
    return sector_count > 1 ||
           cached.sequential_write_sectors >= kSequentialWriteThroughSectors;
}

BlockIoStatus cached_read(void* context,
                          uint32_t lba,
                          uint8_t sector_count,
                          void* buffer) {
    auto* cached = static_cast<CachedDevice*>(context);
    if (cached == nullptr || buffer == nullptr) {
        return BlockIoStatus::IoError;
    }
    if (sector_count == 0) {
        return BlockIoStatus::Ok;
    }
    if (cached->backing.sector_size == 0 ||
        cached->backing.sector_size != kCacheSectorSize) {
        return read_uncached(*cached, lba, sector_count, buffer);
    }
    CacheOperation operation;
    if (!operation.uses_cache()) {
        return read_uncached(*cached, lba, sector_count, buffer);
    }

    const size_t sector_size = cached->backing.sector_size;
    for (uint8_t i = 0; i < sector_count; ++i) {
        uint32_t sector_lba = lba + i;
        void* out = byte_offset(buffer, static_cast<size_t>(i) * sector_size);
        for (;;) {
            lock();
            CacheEntry* entry = find_entry(*cached, sector_lba, sector_size);
            if (entry != nullptr) {
                ++g_clock;
                entry->age = g_clock;
                memcpy(out, entry->data, sector_size);
                unlock();
                break;
            }
            uint64_t observed_write_sequence = cached->write_sequence;
            unlock();

            alignas(512) uint8_t sector_buffer[kCacheSectorSize];
            BlockIoStatus status =
                read_uncached(*cached, sector_lba, 1, sector_buffer);
            if (status != BlockIoStatus::Ok) {
                return status;
            }

            bool retry_read = false;
            for (;;) {
                lock();
                // Prefer a cache write that completed while the backing read
                // was in flight. If a write left no entry (write-through),
                // repeat the read after that ordered backing write.
                entry = find_entry(*cached, sector_lba, sector_size);
                if (entry != nullptr) {
                    memcpy(sector_buffer, entry->data, sector_size);
                    ++g_clock;
                    entry->age = g_clock;
                    unlock();
                    break;
                }
                if (cached->write_sequence != observed_write_sequence) {
                    unlock();
                    retry_read = true;
                    break;
                }
                entry = find_or_prepare_entry_locked(*cached,
                                                      sector_lba,
                                                      sector_size);
                if (entry != nullptr) {
                    memcpy(entry->data, sector_buffer, sector_size);
                    ++g_clock;
                    ++entry->generation;
                    entry->age = g_clock;
                    unlock();
                    break;
                }
                unlock();
                if (!flush_one_dirty()) {
                    break;
                }
            }
            if (retry_read) {
                continue;
            }
            memcpy(out, sector_buffer, sector_size);
            break;
        }
    }
    return BlockIoStatus::Ok;
}

BlockIoStatus cached_write(void* context,
                           uint32_t lba,
                           uint8_t sector_count,
                           const void* buffer) {
    auto* cached = static_cast<CachedDevice*>(context);
    if (cached == nullptr || buffer == nullptr) {
        return BlockIoStatus::IoError;
    }
    if (sector_count == 0) {
        return BlockIoStatus::Ok;
    }

    if (cached->backing.sector_size == 0 ||
        cached->backing.sector_size != kCacheSectorSize) {
        return write_uncached(*cached, lba, sector_count, buffer);
    }
    CacheOperation operation;
    if (!operation.uses_cache()) {
        return write_through(*cached, lba, sector_count, buffer);
    }

    // Full-cluster and sustained sequential writes are streaming I/O, even on
    // FAT volumes that use one sector per cluster. Caching them only turns the
    // transfer into a 32 MiB burst followed by one-sector writeback once the
    // cache is full.
    if (should_write_through(*cached, lba, sector_count)) {
        return write_through(*cached, lba, sector_count, buffer);
    }

    const size_t sector_size = cached->backing.sector_size;
    uint8_t cached_count = 0;
    while (cached_count < sector_count) {
        bool cache_full = false;
        lock();
        while (cached_count < sector_count) {
            uint32_t sector_lba = lba + cached_count;
            const void* in = static_cast<const void*>(
                static_cast<const uint8_t*>(buffer) +
                static_cast<size_t>(cached_count) * sector_size);
            CacheEntry* entry = find_or_prepare_entry_locked(*cached,
                                                             sector_lba,
                                                             sector_size);
            if (entry == nullptr) {
                cache_full = true;
                break;
            }
            ++cached->write_sequence;
            memcpy(entry->data, in, sector_size);
            ++g_clock;
            ++entry->generation;
            entry->age = g_clock;
            entry->dirty = true;
            ++cached_count;
        }
        unlock();

        if (cache_full && !flush_one_dirty()) {
            const void* in = static_cast<const void*>(
                static_cast<const uint8_t*>(buffer) +
                static_cast<size_t>(cached_count) * sector_size);
            BlockIoStatus status =
                write_through(*cached, lba + cached_count, 1, in);
            if (status != BlockIoStatus::Ok) {
                return status;
            }
            ++cached_count;
        }
    }
    return BlockIoStatus::Ok;
}

}  // namespace

void init() {
    LockGuard guard;
    for (auto& device : g_devices) {
        device = {};
    }
    for (auto& entry : g_entries) {
        entry = {};
        entry.hash_next = -1;
    }
    for (auto& head : g_hash_heads) {
        head = -1;
    }
    g_clock = 0;
    g_victim_cursor = 0;
    g_active_cached_ops = 0;
    __atomic_store_n(&g_enabled, true, __ATOMIC_RELEASE);
}

void service_idle_flush() {
    for (size_t i = 0; i < kIdleFlushBudget; ++i) {
        if (!flush_one_dirty()) {
            break;
        }
    }
}

bool flush_all() {
    for (;;) {
        bool has_dirty = false;
        bool has_in_flight = false;
        bool flushed_any = false;
        bool failed_any = false;

        for (auto& entry : g_entries) {
            lock();
            bool dirty = entry.valid && entry.dirty;
            bool flushing = dirty && entry.flushing;
            unlock();
            if (!dirty) {
                continue;
            }

            has_dirty = true;
            if (flushing) {
                has_in_flight = true;
                continue;
            }
            if (flush_dirty_entry(&entry)) {
                flushed_any = true;
            } else {
                failed_any = true;
            }
        }

        if (!has_dirty) {
            return true;
        }
        if (failed_any && !flushed_any) {
            return false;
        }
        if (has_in_flight && !flushed_any) {
            asm volatile("pause");
        }
    }
}

void set_enabled(bool enabled) {
    lock_mode();
    if (!enabled) {
        lock();
        __atomic_store_n(&g_enabled, false, __ATOMIC_RELEASE);
        unlock();
        for (;;) {
            lock();
            bool idle = g_active_cached_ops == 0;
            unlock();
            if (idle) {
                break;
            }
            asm volatile("pause");
        }
        (void)flush_all();
        unlock_mode();
        return;
    }
    lock();
    __atomic_store_n(&g_enabled, true, __ATOMIC_RELEASE);
    unlock();
    unlock_mode();
}

bool enabled() {
    return cache_enabled();
}

bool wrap_device(const BlockDevice& backing, BlockDevice& out_device) {
    out_device = backing;
    if (backing.sector_size == 0 || backing.sector_size != kCacheSectorSize ||
        backing.read == cached_read || backing.write == cached_write) {
        return true;
    }

    LockGuard guard;
    for (auto& device : g_devices) {
        if (device.in_use) {
            continue;
        }
        device.backing = backing;
        device.in_use = true;

        out_device.read = cached_read;
        out_device.write = backing.write != nullptr ? cached_write : nullptr;
        out_device.context = &device;
        out_device.descriptor_handle = descriptor::kInvalidHandle;
        return true;
    }

    log_message(LogLevel::Warn,
                "BlockCache: no wrapper slots for %s",
                backing.name != nullptr ? backing.name : "(unnamed)");
    return false;
}

}  // namespace block_cache
}  // namespace fs
