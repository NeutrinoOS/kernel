#include "block_cache.hpp"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/memory/paging.hpp"
#include "drivers/log/logging.hpp"
#include "kernel/error.hpp"
#include "kernel/memory/physical_allocator.hpp"
#include "lib/mem.hpp"

namespace fs {
namespace block_cache {
namespace {

constexpr size_t kMaxCachedDevices = 32;
constexpr size_t kCacheEntryCount = 65536;
constexpr size_t kCacheHashBucketCount = 65536;
constexpr size_t kCacheSectorSize = 512;
constexpr size_t kCachePageSize = 4096;
constexpr uint8_t kSectorsPerPage =
    static_cast<uint8_t>(kCachePageSize / kCacheSectorSize);
constexpr size_t kIdleFlushBudget = 16;
constexpr uint16_t kSequentialWriteThroughSectors = 4;

static_assert(kSectorsPerPage == 8,
              "block-cache page mask requires eight sectors");

struct CachedDevice {
    BlockDevice backing;
    volatile int io_lock;
    uint64_t write_sequence;
    bool in_use;
    bool have_last_write;
    uint16_t sequential_write_sectors;
    uint32_t last_write_end_lba;
};

// Metadata stays resident, but payload pages come from the user-page pool on
// demand. The table can describe up to 256 MiB and clean pages are reclaimable.
struct CacheEntry {
    CachedDevice* owner;
    uint32_t block_lba;
    uint64_t age;
    uint64_t generation;
    uint64_t phys;
    uint8_t valid_mask;
    uint8_t dirty_mask;
    bool valid;
    bool flushing;
    bool referenced;
    int32_t hash_next;
};

CachedDevice g_devices[kMaxCachedDevices]{};
CacheEntry g_entries[kCacheEntryCount]{};
int32_t g_hash_heads[kCacheHashBucketCount]{};
volatile int g_cache_lock = 0;
uint64_t g_clock = 0;
size_t g_victim_cursor = 0;
size_t g_reclaim_cursor = 0;
size_t g_cached_pages = 0;
bool g_enabled = true;
bool g_initialized = false;
size_t g_active_cached_ops = 0;
volatile int g_mode_lock = 0;

uint32_t block_lba_for(uint32_t lba) {
    return lba & ~static_cast<uint32_t>(kSectorsPerPage - 1);
}

uint8_t sector_index_for(uint32_t lba) {
    return static_cast<uint8_t>(lba & (kSectorsPerPage - 1));
}

uint8_t sector_bit_for(uint32_t lba) {
    return static_cast<uint8_t>(1u << sector_index_for(lba));
}

uint8_t* entry_data(CacheEntry& entry) {
    return static_cast<uint8_t*>(paging_phys_to_virt(entry.phys));
}

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
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
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
        if (!cached_) return;
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

const void* byte_offset(const void* ptr, size_t offset) {
    return static_cast<const void*>(
        static_cast<const uint8_t*>(ptr) + offset);
}

void lock_io(CachedDevice& cached) {
    while (__atomic_test_and_set(&cached.io_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock_io(CachedDevice& cached) {
    __atomic_clear(&cached.io_lock, __ATOMIC_RELEASE);
}

BlockIoStatus read_uncached(CachedDevice& cached, uint32_t lba,
                            uint8_t count, void* buffer) {
    lock_io(cached);
    BlockIoStatus status = block_read(cached.backing, lba, count, buffer);
    unlock_io(cached);
    return status;
}

BlockIoStatus write_uncached(CachedDevice& cached, uint32_t lba,
                             uint8_t count, const void* buffer) {
    lock_io(cached);
    BlockIoStatus status = block_write(cached.backing, lba, count, buffer);
    unlock_io(cached);
    return status;
}

size_t cache_hash(const CachedDevice& cached, uint32_t block_lba) {
    uintptr_t owner = reinterpret_cast<uintptr_t>(&cached);
    uint64_t mixed =
        static_cast<uint64_t>(block_lba) * 11400714819323198485ull;
    mixed ^= static_cast<uint64_t>(owner >> 4);
    return static_cast<size_t>(mixed) & (kCacheHashBucketCount - 1);
}

CacheEntry* find_entry(CachedDevice& cached, uint32_t lba) {
    uint32_t block_lba = block_lba_for(lba);
    int32_t index = g_hash_heads[cache_hash(cached, block_lba)];
    size_t traversed = 0;
    while (index >= 0) {
        KERNEL_ASSERT_MSG(static_cast<size_t>(index) < kCacheEntryCount,
                          "block-cache hash index is out of bounds");
        KERNEL_ASSERT_MSG(traversed++ < kCacheEntryCount,
                          "block-cache hash chain contains a cycle");
        CacheEntry& entry = g_entries[static_cast<size_t>(index)];
        if (entry.valid && entry.owner == &cached &&
            entry.block_lba == block_lba) {
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
    size_t bucket = cache_hash(*target.owner, target.block_lba);
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
                      "block-cache entry has no owner");
    size_t index = static_cast<size_t>(&entry - g_entries);
    KERNEL_ASSERT_MSG(index < kCacheEntryCount,
                      "block-cache entry is outside the table");
    size_t bucket = cache_hash(*entry.owner, entry.block_lba);
    entry.hash_next = g_hash_heads[bucket];
    g_hash_heads[bucket] = static_cast<int32_t>(index);
}

CacheEntry* choose_clean_victim_locked() {
    for (size_t scanned = 0; scanned < kCacheEntryCount * 2; ++scanned) {
        size_t index = (g_victim_cursor + scanned) % kCacheEntryCount;
        CacheEntry& entry = g_entries[index];
        if (!entry.valid) {
            if (entry.flushing) continue;
            g_victim_cursor = (index + 1) % kCacheEntryCount;
            return &entry;
        }
        if (entry.dirty_mask != 0 || entry.flushing) continue;
        if (entry.referenced) {
            entry.referenced = false;
            continue;
        }
        g_victim_cursor = (index + 1) % kCacheEntryCount;
        return &entry;
    }
    return nullptr;
}

CacheEntry* prepare_entry_locked(CachedDevice& cached, uint32_t lba,
                                 uint64_t spare_phys, bool& used_spare) {
    used_spare = false;
    if (CacheEntry* existing = find_entry(cached, lba)) return existing;

    CacheEntry* entry = choose_clean_victim_locked();
    if (entry == nullptr || (entry->phys == 0 && spare_phys == 0)) {
        return nullptr;
    }
    if (entry->valid) unlink_entry_locked(*entry);
    if (entry->phys == 0) {
        entry->phys = spare_phys;
        used_spare = true;
        ++g_cached_pages;
    }
    entry->owner = &cached;
    entry->block_lba = block_lba_for(lba);
    entry->age = 0;
    entry->generation = 0;
    entry->valid_mask = 0;
    entry->dirty_mask = 0;
    entry->valid = true;
    entry->flushing = false;
    entry->referenced = false;
    entry->hash_next = -1;
    link_entry_locked(*entry);
    return entry;
}

void install_read_sector(CachedDevice& cached, uint32_t lba,
                         uint64_t observed_write_sequence,
                         void* caller_sector) {
    uint64_t spare_phys = 0;
    for (;;) {
        bool used_spare = false;
        lock();
        if (cached.write_sequence != observed_write_sequence) {
            // A write overtook the backing read. Return a cached write when
            // one exists, but never install the now-stale disk snapshot.
            CacheEntry* current = find_entry(cached, lba);
            uint8_t bit = sector_bit_for(lba);
            if (current != nullptr && (current->valid_mask & bit) != 0) {
                const uint8_t* sector = entry_data(*current) +
                    static_cast<size_t>(sector_index_for(lba)) *
                        kCacheSectorSize;
                memcpy(caller_sector, sector, kCacheSectorSize);
                current->referenced = true;
            }
            unlock();
            if (spare_phys != 0) memory::free_user_page(spare_phys);
            return;
        }
        CacheEntry* entry =
            prepare_entry_locked(cached, lba, spare_phys, used_spare);
        if (entry != nullptr) {
            uint8_t bit = sector_bit_for(lba);
            uint8_t* sector = entry_data(*entry) +
                static_cast<size_t>(sector_index_for(lba)) * kCacheSectorSize;
            if ((entry->valid_mask & bit) != 0) {
                // A concurrent cache fill or write is newer than our snapshot.
                memcpy(caller_sector, sector, kCacheSectorSize);
                entry->referenced = true;
            } else {
                memcpy(sector, caller_sector, kCacheSectorSize);
                entry->valid_mask |= bit;
            }
            ++g_clock;
            ++entry->generation;
            entry->age = g_clock;
            unlock();
            if (spare_phys != 0 && !used_spare) {
                memory::free_user_page(spare_phys);
            }
            return;
        }
        unlock();
        if (spare_phys != 0) {
            memory::free_user_page(spare_phys);
            return;
        }
        spare_phys = memory::alloc_user_page();
        if (spare_phys == 0) return;
    }
}

bool flush_dirty_entry(CacheEntry* target) {
    CachedDevice* owner = nullptr;
    uint32_t block_lba = 0;
    uint64_t generation = 0;
    uint8_t dirty_mask = 0;
    alignas(512) uint8_t page_buffer[kCachePageSize];

    lock();
    if (target == nullptr || !target->valid || target->dirty_mask == 0 ||
        target->flushing || target->owner == nullptr || target->phys == 0) {
        unlock();
        return true;
    }
    owner = target->owner;
    block_lba = target->block_lba;
    generation = target->generation;
    dirty_mask = target->dirty_mask;
    lock_io(*owner);
    target->flushing = true;
    memcpy(page_buffer, entry_data(*target), sizeof(page_buffer));
    unlock();

    bool ok = true;
    uint8_t index = 0;
    while (index < kSectorsPerPage) {
        if ((dirty_mask & (1u << index)) == 0) {
            ++index;
            continue;
        }
        uint8_t start = index;
        while (index < kSectorsPerPage &&
               (dirty_mask & (1u << index)) != 0) ++index;
        uint8_t count = static_cast<uint8_t>(index - start);
        if (block_write(owner->backing, block_lba + start, count,
                        page_buffer +
                            static_cast<size_t>(start) * kCacheSectorSize) !=
            BlockIoStatus::Ok) {
            ok = false;
            break;
        }
    }
    unlock_io(*owner);

    lock();
    // The entry cannot be selected or reclaimed while this flag is set. A
    // write-through invalidation may have removed it from the hash while the
    // backing write was in flight, but it is still the same reserved slot.
    target->flushing = false;
    if (target->valid && target->owner == owner &&
        target->block_lba == block_lba) {
        if (target->generation == generation && ok) {
            target->dirty_mask &= static_cast<uint8_t>(~dirty_mask);
        }
    }
    unlock();
    return ok;
}

bool flush_one_dirty() {
    lock();
    CacheEntry* entry = nullptr;
    for (auto& candidate : g_entries) {
        if (!candidate.valid || candidate.dirty_mask == 0 ||
            candidate.flushing) continue;
        if (entry == nullptr || candidate.age < entry->age) entry = &candidate;
    }
    unlock();
    return entry != nullptr && flush_dirty_entry(entry);
}

void invalidate_write_range_locked(CachedDevice& cached, uint32_t lba,
                                   uint8_t count) {
    for (uint16_t i = 0; i < count; ++i) {
        uint32_t sector_lba = lba + static_cast<uint32_t>(i);
        CacheEntry* entry = find_entry(cached, sector_lba);
        if (entry == nullptr) continue;
        uint8_t bit = sector_bit_for(sector_lba);
        entry->valid_mask &= static_cast<uint8_t>(~bit);
        entry->dirty_mask &= static_cast<uint8_t>(~bit);
        ++entry->generation;
        if (entry->valid_mask == 0 && entry->dirty_mask == 0) {
            unlink_entry_locked(*entry);
            entry->owner = nullptr;
            entry->valid = false;
            entry->referenced = false;
        }
    }
}

BlockIoStatus write_through(CachedDevice& cached, uint32_t lba,
                            uint8_t count, const void* buffer) {
    lock();
    lock_io(cached);
    ++cached.write_sequence;
    invalidate_write_range_locked(cached, lba, count);
    unlock();
    BlockIoStatus status = block_write(cached.backing, lba, count, buffer);
    unlock_io(cached);
    return status;
}

bool should_write_through(CachedDevice& cached, uint32_t lba, uint8_t count) {
    LockGuard guard;
    if (cached.have_last_write && lba == cached.last_write_end_lba) {
        uint32_t total = static_cast<uint32_t>(cached.sequential_write_sectors) +
                         static_cast<uint32_t>(count);
        cached.sequential_write_sectors = static_cast<uint16_t>(
            total > UINT16_MAX ? UINT16_MAX : total);
    } else {
        cached.sequential_write_sectors = count;
    }
    cached.have_last_write = true;
    cached.last_write_end_lba = lba + static_cast<uint32_t>(count);
    return count > 1 ||
           cached.sequential_write_sectors >= kSequentialWriteThroughSectors;
}

BlockIoStatus cached_read(void* context, uint32_t lba, uint8_t count,
                          void* buffer) {
    auto* cached = static_cast<CachedDevice*>(context);
    if (cached == nullptr || buffer == nullptr) return BlockIoStatus::IoError;
    if (count == 0) return BlockIoStatus::Ok;
    if (cached->backing.sector_size != kCacheSectorSize) {
        return read_uncached(*cached, lba, count, buffer);
    }
    CacheOperation operation;
    if (!operation.uses_cache()) {
        return read_uncached(*cached, lba, count, buffer);
    }

    bool all_cached = true;
    lock();
    for (uint8_t i = 0; i < count; ++i) {
        uint32_t sector_lba = lba + i;
        CacheEntry* entry = find_entry(*cached, sector_lba);
        uint8_t bit = sector_bit_for(sector_lba);
        if (entry == nullptr || (entry->valid_mask & bit) == 0) {
            all_cached = false;
            continue;
        }
        const uint8_t* sector = entry_data(*entry) +
            static_cast<size_t>(sector_index_for(sector_lba)) *
                kCacheSectorSize;
        memcpy(byte_offset(buffer,
                           static_cast<size_t>(i) * kCacheSectorSize),
               sector, kCacheSectorSize);
        ++g_clock;
        entry->age = g_clock;
        entry->referenced = true;
    }
    unlock();
    if (all_cached) return BlockIoStatus::Ok;

    // Keep a filesystem cluster as one device command on a cold range.
    lock();
    uint64_t observed_write_sequence = cached->write_sequence;
    unlock();
    BlockIoStatus status = read_uncached(*cached, lba, count, buffer);
    if (status != BlockIoStatus::Ok) return status;
    for (uint8_t i = 0; i < count; ++i) {
        install_read_sector(
            *cached, lba + i, observed_write_sequence,
            byte_offset(buffer, static_cast<size_t>(i) * kCacheSectorSize));
    }
    return BlockIoStatus::Ok;
}

bool cache_write_sector(CachedDevice& cached, uint32_t lba,
                        const void* input) {
    uint64_t spare_phys = 0;
    for (;;) {
        bool used_spare = false;
        lock();
        CacheEntry* entry =
            prepare_entry_locked(cached, lba, spare_phys, used_spare);
        if (entry != nullptr) {
            uint8_t bit = sector_bit_for(lba);
            uint8_t* sector = entry_data(*entry) +
                static_cast<size_t>(sector_index_for(lba)) * kCacheSectorSize;
            memcpy(sector, input, kCacheSectorSize);
            entry->valid_mask |= bit;
            entry->dirty_mask |= bit;
            entry->referenced = true;
            ++cached.write_sequence;
            ++g_clock;
            ++entry->generation;
            entry->age = g_clock;
            unlock();
            if (spare_phys != 0 && !used_spare) {
                memory::free_user_page(spare_phys);
            }
            return true;
        }
        unlock();
        if (spare_phys == 0) {
            spare_phys = memory::alloc_user_page();
            if (spare_phys != 0) continue;
        }
        if (spare_phys != 0) memory::free_user_page(spare_phys);
        return false;
    }
}

BlockIoStatus cached_write(void* context, uint32_t lba, uint8_t count,
                           const void* buffer) {
    auto* cached = static_cast<CachedDevice*>(context);
    if (cached == nullptr || buffer == nullptr) return BlockIoStatus::IoError;
    if (count == 0) return BlockIoStatus::Ok;
    if (cached->backing.sector_size != kCacheSectorSize) {
        return write_uncached(*cached, lba, count, buffer);
    }
    CacheOperation operation;
    if (!operation.uses_cache()) {
        return write_through(*cached, lba, count, buffer);
    }
    if (should_write_through(*cached, lba, count)) {
        return write_through(*cached, lba, count, buffer);
    }
    for (uint8_t i = 0; i < count; ++i) {
        const void* input = byte_offset(
            buffer, static_cast<size_t>(i) * kCacheSectorSize);
        if (!cache_write_sector(*cached, lba + i, input)) {
            BlockIoStatus status =
                write_through(*cached, lba + i, 1, input);
            if (status != BlockIoStatus::Ok) return status;
        }
    }
    return BlockIoStatus::Ok;
}

size_t reclaim_clean_pages(size_t target_pages) {
    size_t reclaimed = 0;
    while (reclaimed < target_pages) {
        uint64_t phys = 0;
        lock();
        for (size_t scanned = 0; scanned < kCacheEntryCount; ++scanned) {
            size_t index = (g_reclaim_cursor + scanned) % kCacheEntryCount;
            CacheEntry& entry = g_entries[index];
            if (entry.phys == 0 || entry.flushing || entry.dirty_mask != 0) {
                continue;
            }
            if (entry.valid) unlink_entry_locked(entry);
            phys = entry.phys;
            entry = {};
            entry.hash_next = -1;
            KERNEL_ASSERT_MSG(g_cached_pages != 0,
                              "block-cache page count underflow");
            --g_cached_pages;
            g_reclaim_cursor = (index + 1) % kCacheEntryCount;
            break;
        }
        unlock();
        if (phys == 0) break;
        memory::free_user_page(phys);
        ++reclaimed;
    }
    return reclaimed;
}

}  // namespace

void init() {
    LockGuard guard;
    if (g_initialized) return;
    for (auto& device : g_devices) device = {};
    for (auto& entry : g_entries) {
        entry = {};
        entry.hash_next = -1;
    }
    for (auto& head : g_hash_heads) head = -1;
    g_clock = 0;
    g_victim_cursor = 0;
    g_reclaim_cursor = 0;
    g_cached_pages = 0;
    g_active_cached_ops = 0;
    __atomic_store_n(&g_enabled, true, __ATOMIC_RELEASE);
    g_initialized = true;
    if (!memory::register_user_page_reclaimer(reclaim_clean_pages)) {
        log_message(LogLevel::Warn,
                    "BlockCache: user-page reclaimer registration failed");
    }
}

void service_idle_flush() {
    for (size_t i = 0; i < kIdleFlushBudget; ++i) {
        if (!flush_one_dirty()) break;
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
            bool dirty = entry.valid && entry.dirty_mask != 0;
            bool flushing = dirty && entry.flushing;
            unlock();
            if (!dirty) continue;
            has_dirty = true;
            if (flushing) {
                has_in_flight = true;
                continue;
            }
            if (flush_dirty_entry(&entry)) flushed_any = true;
            else failed_any = true;
        }
        if (!has_dirty) return true;
        if (failed_any && !flushed_any) return false;
        if (has_in_flight && !flushed_any) asm volatile("pause");
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
            if (idle) break;
            asm volatile("pause");
        }
        (void)flush_all();
        (void)reclaim_clean_pages(static_cast<size_t>(-1));
        unlock_mode();
        return;
    }
    lock();
    __atomic_store_n(&g_enabled, true, __ATOMIC_RELEASE);
    unlock();
    unlock_mode();
}

bool enabled() { return cache_enabled(); }

bool wrap_device(const BlockDevice& backing, BlockDevice& out_device) {
    out_device = backing;
    if (backing.sector_size != kCacheSectorSize ||
        backing.read == cached_read || backing.write == cached_write) {
        return true;
    }
    LockGuard guard;
    for (auto& device : g_devices) {
        if (device.in_use) continue;
        device.backing = backing;
        device.in_use = true;
        out_device.read = cached_read;
        out_device.write = backing.write != nullptr ? cached_write : nullptr;
        out_device.context = &device;
        out_device.descriptor_handle = descriptor::kInvalidHandle;
        return true;
    }
    log_message(LogLevel::Warn, "BlockCache: no wrapper slots for %s",
                backing.name != nullptr ? backing.name : "(unnamed)");
    return false;
}

}  // namespace block_cache
}  // namespace fs
