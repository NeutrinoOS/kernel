#include "page_cache.hpp"

#include "arch/x86_64/memory/paging.hpp"
#include "fs/vfs.hpp"
#include "kernel/error.hpp"
#include "kernel/memory/physical_allocator.hpp"
#include "kernel/sync.hpp"
#include "lib/mem.hpp"

namespace {

constexpr size_t kMaxCacheKey = 128;

struct CacheEntry {
    char key[kMaxCacheKey];
    uint64_t file_offset;
    uint64_t phys;
    uint32_t refcount;
    bool in_use;
};

CacheEntry g_entries[page_cache::kMaxCachedPages]{};
sync::SpinLock g_cache_lock;

bool strings_equal(const char* lhs, const char* rhs) {
    if (lhs == nullptr || rhs == nullptr) {
        return lhs == rhs;
    }
    size_t i = 0;
    while (lhs[i] != '\0' && rhs[i] != '\0') {
        if (lhs[i] != rhs[i]) {
            return false;
        }
        ++i;
    }
    return lhs[i] == rhs[i];
}

bool copy_key(char (&dest)[kMaxCacheKey], const char* source) {
    if (source == nullptr || source[0] == '\0') {
        return false;
    }
    size_t i = 0;
    while (source[i] != '\0') {
        if (i + 1 >= sizeof(dest)) {
            return false;
        }
        dest[i] = source[i];
        ++i;
    }
    dest[i] = '\0';
    return true;
}

CacheEntry* find_entry(const char* key, uint64_t file_offset) {
    for (auto& entry : g_entries) {
        if (entry.in_use && entry.file_offset == file_offset &&
            strings_equal(entry.key, key)) {
            return &entry;
        }
    }
    return nullptr;
}

CacheEntry* allocate_entry() {
    for (auto& entry : g_entries) {
        if (!entry.in_use) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

namespace page_cache {

bool acquire_private_page(const char* key,
                          vfs::FileHandle& file,
                          uint64_t file_offset,
                          uint64_t& out_phys) {
    out_phys = 0;
    if (key == nullptr || (file_offset & (kPageSize - 1)) != 0) {
        return false;
    }

    {
        sync::IrqLockGuard guard(g_cache_lock);
        if (CacheEntry* existing = find_entry(key, file_offset)) {
            if (existing->refcount == UINT32_MAX) {
                return false;
            }
            ++existing->refcount;
            out_phys = existing->phys;
            return true;
        }
    }

    char stable_key[kMaxCacheKey]{};
    if (!copy_key(stable_key, key)) {
        return false;
    }

    uint64_t phys = memory::alloc_user_page();
    if (phys == 0) {
        return false;
    }
    auto* page = static_cast<uint8_t*>(paging_phys_to_virt(phys));
    memset(page, 0, kPageSize);

    size_t read_size = 0;
    if (!vfs::read_file(file,
                        file_offset,
                        page,
                        kPageSize,
                        read_size) ||
        read_size > kPageSize) {
        memory::free_user_page(phys);
        return false;
    }

    sync::IrqLockGuard guard(g_cache_lock);
    if (CacheEntry* existing = find_entry(stable_key, file_offset)) {
        if (existing->refcount == UINT32_MAX) {
            memory::free_user_page(phys);
            return false;
        }
        ++existing->refcount;
        out_phys = existing->phys;
        memory::free_user_page(phys);
        return true;
    }

    CacheEntry* entry = allocate_entry();
    if (entry == nullptr || !copy_key(entry->key, stable_key)) {
        memory::free_user_page(phys);
        return false;
    }
    entry->file_offset = file_offset;
    entry->phys = phys;
    entry->refcount = 1;
    entry->in_use = true;
    out_phys = phys;
    return true;
}

void release_private_page(uint64_t phys) {
    if (phys == 0) {
        return;
    }
    sync::IrqLockGuard guard(g_cache_lock);
    for (auto& entry : g_entries) {
        if (!entry.in_use || entry.phys != phys) {
            continue;
        }
        KERNEL_ASSERT_MSG(entry.refcount != 0,
                          "live page-cache entry has a zero refcount");
        if (entry.refcount > 1) {
            --entry.refcount;
            return;
        }
        memory::free_user_page(entry.phys);
        entry = {};
        return;
    }
    KERNEL_ASSERT_MSG(false,
                      "page cache was asked to release an unknown page");
}

size_t cached_pages() {
    sync::IrqLockGuard guard(g_cache_lock);
    size_t count = 0;
    for (const auto& entry : g_entries) {
        if (entry.in_use) {
            ++count;
        }
    }
    return count;
}

}  // namespace page_cache
