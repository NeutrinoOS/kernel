#pragma once

#include <stddef.h>
#include <stdint.h>

namespace memory {

class BuddyAllocator {
public:
    static constexpr uint64_t kPageSize = 0x1000;
    static constexpr uint8_t kMaxOrder = 24;
    static constexpr size_t kMaxRanges = 64;

    void init(uint64_t hhdm_offset, uint8_t max_order);
    bool add_range(uint64_t base,
                   uint64_t length,
                   int8_t* order_map,
                   size_t map_entries);

    uint64_t alloc_pages(size_t pages, bool log_failure = true);
    uint64_t alloc_order(uint8_t order, bool log_failure = true);
    void free(uint64_t phys);
    bool owns(uint64_t phys) const;
    size_t free_pages() const { return free_pages_; }
    uint8_t max_order() const { return max_order_; }
    size_t range_count() const { return range_count_; }

private:
    struct Range {
        uint64_t base;
        size_t pages;
        int8_t* order_map;
    };

    struct FreeBlock {
        FreeBlock* next;
    };

    static constexpr int8_t kMapNonHead = -1;
    static constexpr int8_t kMapAllocatedBase = -2;

    uint64_t hhdm_offset_{0};
    uint8_t max_order_{0};
    Range ranges_[kMaxRanges]{};
    size_t range_count_{0};
    FreeBlock* free_lists_[kMaxOrder + 1]{};
    size_t free_pages_{0};

    Range* find_range(uint64_t phys);
    const Range* find_range(uint64_t phys) const;
    size_t index_for_phys(const Range& range, uint64_t phys) const;
    uint64_t phys_for_index(const Range& range, size_t index) const;
    void* phys_to_virt(uint64_t phys) const;
    uint64_t virt_to_phys(const void* virt) const;

    uint8_t order_for_pages(size_t pages) const;
    uint8_t max_order_for_pages(size_t pages) const;
    void push_free(uint64_t phys, uint8_t order);
    void remove_free(uint64_t phys, uint8_t order);
    void mark_block_free(Range& range, size_t index, uint8_t order);
    void mark_block_allocated(Range& range, size_t index, uint8_t order);
};

}  // namespace memory
