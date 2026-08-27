#include "kernel/memory/buddy.hpp"

#include "lib/mem.hpp"
#include "drivers/log/logging.hpp"
#include "kernel/error.hpp"

namespace memory {

namespace {

constexpr uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

}  // namespace

void BuddyAllocator::init(uint64_t hhdm_offset, uint8_t max_order) {
    hhdm_offset_ = hhdm_offset;
    max_order_ = (max_order > kMaxOrder) ? kMaxOrder : max_order;
    range_count_ = 0;
    free_pages_ = 0;
    for (auto& head : free_lists_) {
        head = nullptr;
    }
    for (auto& range : ranges_) {
        range.base = 0;
        range.pages = 0;
        range.order_map = nullptr;
    }
}

bool BuddyAllocator::add_range(uint64_t base,
                               uint64_t length,
                               int8_t* order_map,
                               size_t map_entries) {
    if (order_map == nullptr || length < kPageSize) {
        return false;
    }
    if (range_count_ >= kMaxRanges) {
        return false;
    }

    uint64_t aligned_base = align_up(base, kPageSize);
    uint64_t aligned_end = align_down(base + length, kPageSize);
    if (aligned_end <= aligned_base) {
        return false;
    }

    size_t pages = static_cast<size_t>((aligned_end - aligned_base) / kPageSize);
    if (pages == 0 || map_entries < pages) {
        return false;
    }

    Range& range = ranges_[range_count_++];
    range.base = aligned_base;
    range.pages = pages;
    range.order_map = order_map;
    memset(range.order_map, kMapNonHead, pages);
    free_pages_ += pages;

    size_t index = 0;
    size_t remaining = pages;
    while (remaining > 0) {
        uint8_t order = max_order_for_pages(remaining);
        if (order > max_order_) {
            order = max_order_;
        }
        size_t block_pages = 1ull << order;
        while (order > 0 &&
               ((index & (block_pages - 1)) != 0 || block_pages > remaining)) {
            --order;
            block_pages = 1ull << order;
        }
        mark_block_free(range, index, order);
        uint64_t phys = phys_for_index(range, index);
        push_free(phys, order);
        index += block_pages;
        remaining -= block_pages;
    }

    return true;
}

uint64_t BuddyAllocator::alloc_pages(size_t pages, bool log_failure) {
    if (pages == 0) {
        return 0;
    }
    uint8_t order = order_for_pages(pages);
    if (order > max_order_) {
        return 0;
    }
    if ((1ull << order) < pages) {
        return 0;
    }
    return alloc_order(order, log_failure);
}

uint64_t BuddyAllocator::alloc_order(uint8_t order, bool log_failure) {
    if (order > max_order_) {
        return 0;
    }
    uint8_t current = order;
    while (current <= max_order_ && free_lists_[current] == nullptr) {
        ++current;
    }
    if (current > max_order_) {
        if (!log_failure) return 0;
        log_message(LogLevel::Error,
                    "Buddy alloc failed: order=%u max=%u",
                    static_cast<unsigned int>(order),
                    static_cast<unsigned int>(max_order_));
        return 0;
    }

    FreeBlock* block = free_lists_[current];
    free_lists_[current] = block->next;

    uint64_t phys = virt_to_phys(block);
    Range* range = find_range(phys);
    KERNEL_ASSERT_MSG(range != nullptr,
                      "buddy free list contains an address outside its ranges");
    size_t index = index_for_phys(*range, phys);
    KERNEL_ASSERT_MSG(index < range->pages,
                      "buddy free list index is outside its range");

    while (current > order) {
        --current;
        size_t half_pages = 1ull << current;
        size_t buddy_index = index + half_pages;
        if (buddy_index < range->pages) {
            range->order_map[buddy_index] = static_cast<int8_t>(current);
            uint64_t buddy_phys = phys_for_index(*range, buddy_index);
            push_free(buddy_phys, current);
        }
        range->order_map[index] = static_cast<int8_t>(current);
    }

    mark_block_allocated(*range, index, order);
    KERNEL_ASSERT_MSG(free_pages_ >= (1ull << order),
                      "buddy free-page count underflow");
    free_pages_ -= (1ull << order);
    return phys;
}

void BuddyAllocator::free(uint64_t phys) {
    if (phys == 0) {
        return;
    }
    KERNEL_ASSERT_MSG((phys & (kPageSize - 1)) == 0,
                      "buddy allocator received an unaligned free");
    Range* range = find_range(phys);
    KERNEL_ASSERT_MSG(range != nullptr,
                      "buddy allocator received a free outside its ranges");
    size_t index = index_for_phys(*range, phys);
    KERNEL_ASSERT_MSG(index < range->pages,
                      "buddy free index is outside its range");

    int8_t entry = range->order_map[index];
    KERNEL_ASSERT_MSG(entry != kMapNonHead,
                      "buddy allocator received a non-head free");
    KERNEL_ASSERT_MSG(entry < 0,
                      "buddy allocator detected a double free");
    uint8_t order = static_cast<uint8_t>(-entry - 2);
    KERNEL_ASSERT_MSG(order <= max_order_,
                      "buddy allocation metadata contains an invalid order");

    range->order_map[index] = static_cast<int8_t>(order);

    size_t current_index = index;
    uint8_t current_order = order;
    while (current_order < max_order_) {
        size_t block_pages = 1ull << current_order;
        size_t buddy_index = current_index ^ block_pages;
        if (buddy_index >= range->pages) {
            break;
        }
        int8_t buddy_entry = range->order_map[buddy_index];
        if (buddy_entry != static_cast<int8_t>(current_order)) {
            break;
        }
        uint64_t buddy_phys = phys_for_index(*range, buddy_index);
        remove_free(buddy_phys, current_order);
        range->order_map[buddy_index] = kMapNonHead;
        if (buddy_index < current_index) {
            range->order_map[current_index] = kMapNonHead;
            current_index = buddy_index;
        }
        ++current_order;
        range->order_map[current_index] = static_cast<int8_t>(current_order);
    }

    push_free(phys_for_index(*range, current_index), current_order);
    free_pages_ += (1ull << order);
}

bool BuddyAllocator::owns(uint64_t phys) const {
    return find_range(phys) != nullptr;
}

BuddyAllocator::Range* BuddyAllocator::find_range(uint64_t phys) {
    for (size_t i = 0; i < range_count_; ++i) {
        Range& range = ranges_[i];
        if (range.pages == 0) {
            continue;
        }
        uint64_t end = range.base + range.pages * kPageSize;
        if (phys >= range.base && phys < end) {
            return &range;
        }
    }
    return nullptr;
}

const BuddyAllocator::Range* BuddyAllocator::find_range(uint64_t phys) const {
    for (size_t i = 0; i < range_count_; ++i) {
        const Range& range = ranges_[i];
        if (range.pages == 0) {
            continue;
        }
        uint64_t end = range.base + range.pages * kPageSize;
        if (phys >= range.base && phys < end) {
            return &range;
        }
    }
    return nullptr;
}

size_t BuddyAllocator::index_for_phys(const Range& range, uint64_t phys) const {
    return static_cast<size_t>((phys - range.base) / kPageSize);
}

uint64_t BuddyAllocator::phys_for_index(const Range& range, size_t index) const {
    return range.base + static_cast<uint64_t>(index) * kPageSize;
}

void* BuddyAllocator::phys_to_virt(uint64_t phys) const {
    return reinterpret_cast<void*>(phys + hhdm_offset_);
}

uint64_t BuddyAllocator::virt_to_phys(const void* virt) const {
    return reinterpret_cast<uint64_t>(virt) - hhdm_offset_;
}

uint8_t BuddyAllocator::order_for_pages(size_t pages) const {
    uint8_t order = 0;
    size_t size = 1;
    while (size < pages && order < max_order_) {
        size <<= 1;
        ++order;
    }
    return order;
}

uint8_t BuddyAllocator::max_order_for_pages(size_t pages) const {
    uint8_t order = 0;
    size_t size = 1;
    while ((size << 1) <= pages && order < kMaxOrder) {
        size <<= 1;
        ++order;
    }
    return order;
}

void BuddyAllocator::push_free(uint64_t phys, uint8_t order) {
    if (order > max_order_) {
        return;
    }
    auto* block = static_cast<FreeBlock*>(phys_to_virt(phys));
    block->next = free_lists_[order];
    free_lists_[order] = block;
}

void BuddyAllocator::remove_free(uint64_t phys, uint8_t order) {
    if (order > max_order_) {
        return;
    }
    FreeBlock* prev = nullptr;
    FreeBlock* current = free_lists_[order];
    auto* target = static_cast<FreeBlock*>(phys_to_virt(phys));
    while (current != nullptr) {
        if (current == target) {
            if (prev == nullptr) {
                free_lists_[order] = current->next;
            } else {
                prev->next = current->next;
            }
            return;
        }
        prev = current;
        current = current->next;
    }
}

void BuddyAllocator::mark_block_free(Range& range, size_t index, uint8_t order) {
    if (index >= range.pages) {
        return;
    }
    range.order_map[index] = static_cast<int8_t>(order);
    size_t pages = 1ull << order;
    for (size_t i = 1; i < pages && (index + i) < range.pages; ++i) {
        range.order_map[index + i] = kMapNonHead;
    }
}

void BuddyAllocator::mark_block_allocated(Range& range,
                                          size_t index,
                                          uint8_t order) {
    if (index >= range.pages) {
        return;
    }
    range.order_map[index] =
        static_cast<int8_t>(-static_cast<int8_t>(order) - 2);
    size_t pages = 1ull << order;
    for (size_t i = 1; i < pages && (index + i) < range.pages; ++i) {
        range.order_map[index + i] = kMapNonHead;
    }
}

}  // namespace memory
