#include "kernel/memory/physical_allocator.hpp"

#include "arch/x86_64/memory/paging.hpp"
#include "drivers/limine/limine_requests.hpp"
#include "drivers/log/logging.hpp"
#include "kernel/error.hpp"
#include "kernel/memory/buddy.hpp"
#include "lib/mem.hpp"

namespace memory {

namespace {

constexpr uint64_t kPageSize = BuddyAllocator::kPageSize;
constexpr uint64_t kKernelPoolTargetSize = 64ull * 1024 * 1024;
constexpr size_t kKernelPoolPages = kKernelPoolTargetSize / kPageSize;

BuddyAllocator g_kernel_buddy;
BuddyAllocator g_user_buddy;
int8_t g_kernel_order_map[kKernelPoolPages];

uint64_t g_kernel_pool_base = 0;
uint64_t g_kernel_pool_size = 0;
bool g_initialized = false;
bool g_kernel_ready = false;

volatile int g_alloc_lock = 0;

struct Range {
    uint64_t base;
    uint64_t length;
};

struct AllocationHeader {
    uint64_t phys;
    uint64_t magic;
};
constexpr uint64_t kAllocationMagic = 0x4E45555452494E4Full;

constexpr uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

void lock_alloc() {
    while (__atomic_test_and_set(&g_alloc_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock_alloc() {
    __atomic_clear(&g_alloc_lock, __ATOMIC_RELEASE);
}

Range trim_range(Range range) {
    uint64_t start = align_up(range.base, kPageSize);
    uint64_t end = align_down(range.base + range.length, kPageSize);
    if (start == 0) {
        start = kPageSize;
    }
    if (end <= start) {
        return Range{0, 0};
    }
    return Range{start, end - start};
}

void subtract_range(const Range& input,
                    const Range& reserved,
                    Range& out_a,
                    Range& out_b,
                    size_t& out_count) {
    out_count = 0;
    if (input.length == 0) {
        return;
    }
    if (reserved.length == 0) {
        out_a = input;
        out_count = 1;
        return;
    }
    uint64_t input_end = input.base + input.length;
    uint64_t reserved_end = reserved.base + reserved.length;

    if (reserved_end <= input.base || reserved.base >= input_end) {
        out_a = input;
        out_count = 1;
        return;
    }
    if (reserved.base <= input.base && reserved_end >= input_end) {
        out_count = 0;
        return;
    }
    if (reserved.base <= input.base) {
        out_a = Range{reserved_end, input_end - reserved_end};
        out_count = 1;
        return;
    }
    if (reserved_end >= input_end) {
        out_a = Range{input.base, reserved.base - input.base};
        out_count = 1;
        return;
    }
    out_a = Range{input.base, reserved.base - input.base};
    out_b = Range{reserved_end, input_end - reserved_end};
    out_count = 2;
}

uint8_t max_order_for_pages(size_t pages) {
    uint8_t order = 0;
    size_t size = 1;
    while ((size << 1) <= pages && order < BuddyAllocator::kMaxOrder) {
        size <<= 1;
        ++order;
    }
    return order;
}

bool is_user_pool_type(uint64_t type) {
    switch (type) {
        case LIMINE_MEMMAP_USABLE:
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
            return true;
        default:
            return false;
    }
}

void log_pool(const char* name, uint64_t base, uint64_t size) {
    log_message(LogLevel::Info,
                "%s pool: base=%016llx size=%llu KB",
                name,
                static_cast<unsigned long long>(base),
                static_cast<unsigned long long>(size / 1024));
}

}  // namespace

void init() {
    if (g_initialized) {
        return;
    }
    if (memmap_request.response == nullptr) {
        log_message(LogLevel::Error, "Memory init failed: no memmap");
        return;
    }

    uint64_t hhdm_offset = paging_hhdm_offset();
    Range kernel_region{paging_kernel_phys_base(), paging_kernel_phys_size()};

    Range best_range{0, 0};
    auto memmap = memmap_request.response;
    for (uint64_t i = 0; i < memmap->entry_count; ++i) {
        auto entry = memmap->entries[i];
        if (entry == nullptr || entry->length == 0) {
            continue;
        }
        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }
        Range range{entry->base, entry->length};
        Range seg_a{};
        Range seg_b{};
        size_t seg_count = 0;
        subtract_range(range, kernel_region, seg_a, seg_b, seg_count);
        for (size_t seg = 0; seg < seg_count; ++seg) {
            Range candidate = (seg == 0) ? seg_a : seg_b;
            candidate = trim_range(candidate);
            if (candidate.length > best_range.length) {
                best_range = candidate;
            }
        }
    }

    if (best_range.length == 0) {
        log_message(LogLevel::Error, "Memory init failed: no usable range");
        return;
    }

    uint64_t pool_size = kKernelPoolTargetSize;
    if (pool_size > best_range.length) {
        pool_size = align_down(best_range.length, kPageSize);
    }
    if (pool_size == 0) {
        log_message(LogLevel::Error, "Memory init failed: kernel pool too small");
        return;
    }
    uint64_t pool_base = align_down(best_range.base + best_range.length - pool_size,
                                    kPageSize);
    g_kernel_pool_base = pool_base;
    g_kernel_pool_size = pool_size;

    size_t kernel_pages = static_cast<size_t>(pool_size / kPageSize);
    uint8_t kernel_order = max_order_for_pages(kernel_pages);
    g_kernel_buddy.init(hhdm_offset, kernel_order);
    if (!g_kernel_buddy.add_range(pool_base,
                                  pool_size,
                                  g_kernel_order_map,
                                  kKernelPoolPages)) {
        log_message(LogLevel::Error, "Memory init failed: kernel pool add failed");
        return;
    }
    g_kernel_ready = true;

    log_pool("Kernel", g_kernel_pool_base, g_kernel_pool_size);

    Range kernel_pool_range{g_kernel_pool_base, g_kernel_pool_size};
    size_t max_user_pages = 0;
    size_t candidate_ranges = 0;
    for (uint64_t i = 0; i < memmap->entry_count; ++i) {
        auto entry = memmap->entries[i];
        if (entry == nullptr || entry->length == 0) {
            continue;
        }
        if (!is_user_pool_type(entry->type)) {
            continue;
        }
        ++candidate_ranges;
        Range range{entry->base, entry->length};
        Range segs[4]{};
        size_t seg_count = 0;

        Range temp_a{};
        Range temp_b{};
        size_t temp_count = 0;
        subtract_range(range, kernel_region, temp_a, temp_b, temp_count);
        for (size_t s = 0; s < temp_count; ++s) {
            segs[seg_count++] = (s == 0) ? temp_a : temp_b;
        }

        Range next_segs[4]{};
        size_t next_count = 0;
        for (size_t s = 0; s < seg_count; ++s) {
            Range seg_a{};
            Range seg_b{};
            size_t count = 0;
            subtract_range(segs[s], kernel_pool_range, seg_a, seg_b, count);
            if (count >= 1) {
                next_segs[next_count++] = seg_a;
            }
            if (count == 2) {
                next_segs[next_count++] = seg_b;
            }
        }

        for (size_t s = 0; s < next_count; ++s) {
            Range candidate = trim_range(next_segs[s]);
            if (candidate.length == 0) {
                continue;
            }
            size_t pages = static_cast<size_t>(candidate.length / kPageSize);
            if (pages > max_user_pages) {
                max_user_pages = pages;
            }
        }
    }

    uint8_t user_order = max_order_for_pages(max_user_pages);
    g_user_buddy.init(hhdm_offset, user_order);

    size_t user_pages_total = 0;
    size_t user_ranges_added = 0;
    for (uint64_t i = 0; i < memmap->entry_count; ++i) {
        auto entry = memmap->entries[i];
        if (entry == nullptr || entry->length == 0) {
            continue;
        }
        if (!is_user_pool_type(entry->type)) {
            continue;
        }
        Range range{entry->base, entry->length};
        Range segs[4]{};
        size_t seg_count = 0;

        Range temp_a{};
        Range temp_b{};
        size_t temp_count = 0;
        subtract_range(range, kernel_region, temp_a, temp_b, temp_count);
        for (size_t s = 0; s < temp_count; ++s) {
            segs[seg_count++] = (s == 0) ? temp_a : temp_b;
        }

        Range final_segs[4]{};
        size_t final_count = 0;
        for (size_t s = 0; s < seg_count; ++s) {
            Range seg_a{};
            Range seg_b{};
            size_t count = 0;
            subtract_range(segs[s], kernel_pool_range, seg_a, seg_b, count);
            if (count >= 1) {
                final_segs[final_count++] = seg_a;
            }
            if (count == 2) {
                final_segs[final_count++] = seg_b;
            }
        }

        for (size_t s = 0; s < final_count; ++s) {
            Range candidate = trim_range(final_segs[s]);
            if (candidate.length == 0) {
                continue;
            }
            size_t pages = static_cast<size_t>(candidate.length / kPageSize);
            size_t map_bytes = pages;
            size_t map_pages = static_cast<size_t>(align_up(map_bytes, kPageSize) /
                                                   kPageSize);
            uint64_t map_phys = alloc_kernel_block_pages(map_pages);
            if (map_phys == 0) {
                log_message(LogLevel::Warn,
                            "Skipping user range (order map alloc failed) base=%llx len=%llx",
                            static_cast<unsigned long long>(candidate.base),
                            static_cast<unsigned long long>(candidate.length));
                continue;
            }
            auto* map_ptr = static_cast<int8_t*>(paging_phys_to_virt(map_phys));
            if (!g_user_buddy.add_range(candidate.base,
                                        candidate.length,
                                        map_ptr,
                                        pages)) {
                log_message(LogLevel::Warn,
                            "Skipping user range (buddy add failed) base=%llx len=%llx",
                            static_cast<unsigned long long>(candidate.base),
                            static_cast<unsigned long long>(candidate.length));
                free_kernel_block(map_phys);
                continue;
            }
            user_pages_total += pages;
            ++user_ranges_added;
        }
    }

    log_message(LogLevel::Info,
                "User pool: ranges=%llu/%llu pages=%llu",
                static_cast<unsigned long long>(user_ranges_added),
                static_cast<unsigned long long>(candidate_ranges),
                static_cast<unsigned long long>(user_pages_total));
    if (user_pages_total == 0) {
        log_message(LogLevel::Error,
                    "User pool empty: check memmap types or pool carving");
    } else {
        uint64_t test_phys = g_user_buddy.alloc_pages(1);
        if (test_phys == 0) {
            log_message(LogLevel::Error,
                        "User pool self-test failed: alloc_user_page");
        } else {
            g_user_buddy.free(test_phys);
        }
    }

    g_initialized = true;
}

bool kernel_allocator_ready() {
    return g_kernel_ready;
}

uint64_t alloc_kernel_block_pages(size_t pages) {
    if (!g_kernel_ready || pages == 0) {
        return 0;
    }
    lock_alloc();
    uint64_t phys = g_kernel_buddy.alloc_pages(pages);
    unlock_alloc();
    if (phys != 0) {
        memset(paging_phys_to_virt(phys), 0, pages * kPageSize);
    }
    return phys;
}

uint64_t alloc_kernel_page() {
    return alloc_kernel_block_pages(1);
}

void free_kernel_block(uint64_t phys) {
    if (!g_kernel_ready || phys == 0) {
        return;
    }
    lock_alloc();
    g_kernel_buddy.free(phys);
    unlock_alloc();
}

void free_kernel_page(uint64_t phys) {
    free_kernel_block(phys);
}

void* alloc_kernel(size_t bytes, size_t alignment) {
    if (bytes == 0) return nullptr;
    if (alignment < alignof(AllocationHeader)) alignment = alignof(AllocationHeader);
    if ((alignment & (alignment - 1)) != 0) return nullptr;
    if (bytes > static_cast<size_t>(-1) - alignment - sizeof(AllocationHeader)) return nullptr;
    const size_t total = bytes + alignment - 1 + sizeof(AllocationHeader);
    const size_t pages = static_cast<size_t>(align_up(total, kPageSize) / kPageSize);
    uint64_t phys = alloc_kernel_block_pages(pages);
    if (phys == 0) return nullptr;
    uintptr_t start = reinterpret_cast<uintptr_t>(paging_phys_to_virt(phys));
    uintptr_t result = align_up(start + sizeof(AllocationHeader), alignment);
    auto* header = reinterpret_cast<AllocationHeader*>(result - sizeof(AllocationHeader));
    header->phys = phys;
    header->magic = kAllocationMagic;
    return reinterpret_cast<void*>(result);
}

void free_kernel(void* ptr) {
    if (ptr == nullptr) return;
    auto* header = reinterpret_cast<AllocationHeader*>(reinterpret_cast<uintptr_t>(ptr) - sizeof(AllocationHeader));
    KERNEL_ASSERT_MSG(header->magic == kAllocationMagic,
                      "free_kernel received an invalid or already-freed allocation");
    uint64_t phys = header->phys;
    header->magic = 0;
    free_kernel_block(phys);
}

uint64_t alloc_user_page() {
    if (!g_initialized) {
        return 0;
    }
    lock_alloc();
    uint64_t phys = g_user_buddy.alloc_pages(1);
    unlock_alloc();
    if (phys == 0) {
        log_message(LogLevel::Error,
                    "User pool alloc failed (free=%llu pages max_order=%u ranges=%llu)",
                    static_cast<unsigned long long>(g_user_buddy.free_pages()),
                    static_cast<unsigned int>(g_user_buddy.max_order()),
                    static_cast<unsigned long long>(g_user_buddy.range_count()));
    }
    return phys;
}

void free_user_page(uint64_t phys) {
    if (!g_initialized || phys == 0) {
        return;
    }
    lock_alloc();
    g_user_buddy.free(phys);
    unlock_alloc();
}

uint64_t kernel_pool_base() {
    return g_kernel_pool_base;
}

uint64_t kernel_pool_size() {
    return g_kernel_pool_size;
}

size_t kernel_free_pages() {
    lock_alloc();
    size_t pages = g_kernel_buddy.free_pages();
    unlock_alloc();
    return pages;
}

size_t user_free_pages() {
    lock_alloc();
    size_t pages = g_user_buddy.free_pages();
    unlock_alloc();
    return pages;
}

}  // namespace memory
