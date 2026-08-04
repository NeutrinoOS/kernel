#include "vm.hpp"

#include "arch/x86_64/memory/paging.hpp"
#include "kernel/memory/physical_allocator.hpp"
#include "kernel/page_cache.hpp"
#include "kernel/sync.hpp"
#include "fs/vfs.hpp"
#include "lib/mem.hpp"
#include "drivers/log/logging.hpp"

namespace {

constexpr uint64_t kPageSize = 0x1000;
constexpr uint64_t kPageMask = kPageSize - 1;
constexpr size_t kMaxAddressSpaces = 256;
constexpr size_t kMaxVmAreas = 128;

constexpr uint64_t kUserCodeBase = vm::kUserAddressSpaceBase;
constexpr uint64_t kUserStackCeiling = vm::kUserAddressSpaceTop;
uint64_t g_next_shared_user_code = kUserCodeBase;
sync::SpinLock g_address_space_state_lock;

struct VmArea {
    uint64_t base;
    uint64_t length;
    uint64_t flags;
    uint64_t resident_pages;
    uint64_t reservation_base;
    uint64_t reservation_length;
    vm::MappingKind kind;
    bool in_use;
};

struct AddressSpaceState {
    uint64_t cr3;
    uint64_t next_user_code;
    uint64_t next_user_stack;
    VmArea areas[kMaxVmAreas];
    bool in_use;
};

AddressSpaceState g_address_space_states[kMaxAddressSpaces]{};

constexpr uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

bool page_aligned_length(size_t length, size_t& out) {
    if (length == 0 || length > static_cast<size_t>(-1) - kPageMask) {
        out = 0;
        return false;
    }
    out = (length + kPageMask) & ~static_cast<size_t>(kPageMask);
    return out != 0;
}

void rollback_user_pages(uint64_t cr3, uint64_t base, size_t page_count) {
    for (size_t i = 0; i < page_count; ++i) {
        uint64_t phys = 0;
        if (paging_unmap_page_cr3(
                cr3, base + static_cast<uint64_t>(i) * kPageSize, phys)) {
            memory::free_user_page(phys);
        }
    }
}

AddressSpaceState* find_address_space_state_locked(uint64_t cr3,
                                                   bool create) {
    if (cr3 == 0) {
        return nullptr;
    }
    for (auto& state : g_address_space_states) {
        if (state.in_use && state.cr3 == cr3) {
            return &state;
        }
    }
    if (!create) {
        return nullptr;
    }
    for (auto& state : g_address_space_states) {
        if (state.in_use) {
            continue;
        }
        state.in_use = true;
        state.cr3 = cr3;
        state.next_user_code = kUserCodeBase;
        state.next_user_stack = kUserStackCeiling;
        for (auto& area : state.areas) {
            area = {};
        }
        return &state;
    }
    return nullptr;
}

bool ranges_overlap(uint64_t lhs_base,
                    uint64_t lhs_length,
                    uint64_t rhs_base,
                    uint64_t rhs_length) {
    return lhs_base < rhs_base + rhs_length &&
           rhs_base < lhs_base + lhs_length;
}

VmArea* find_area_locked(AddressSpaceState& state, uint64_t address) {
    for (auto& area : state.areas) {
        if (!area.in_use || address < area.base) {
            continue;
        }
        if (address - area.base < area.length) {
            return &area;
        }
    }
    return nullptr;
}

bool area_range_available_locked(const AddressSpaceState& state,
                                 uint64_t base,
                                 uint64_t length) {
    for (const auto& area : state.areas) {
        const uint64_t occupied_base =
            area.reservation_length != 0 ? area.reservation_base
                                         : area.base;
        const uint64_t occupied_length =
            area.reservation_length != 0 ? area.reservation_length
                                         : area.length;
        if (area.in_use &&
            ranges_overlap(base, length, occupied_base, occupied_length)) {
            return false;
        }
    }
    return true;
}

VmArea* allocate_area_locked(AddressSpaceState& state) {
    for (auto& area : state.areas) {
        if (!area.in_use) {
            return &area;
        }
    }
    return nullptr;
}

bool register_area(uint64_t cr3,
                   uint64_t base,
                   uint64_t length,
                   uint64_t flags,
                   vm::MappingKind kind,
                   uint64_t reservation_base = 0,
                   uint64_t reservation_length = 0) {
    sync::IrqLockGuard guard(g_address_space_state_lock);
    AddressSpaceState* state =
        find_address_space_state_locked(cr3, true);
    const uint64_t occupied_base =
        reservation_length != 0 ? reservation_base : base;
    const uint64_t occupied_length =
        reservation_length != 0 ? reservation_length : length;
    if (state == nullptr ||
        !area_range_available_locked(*state,
                                     occupied_base,
                                     occupied_length)) {
        return false;
    }
    VmArea* area = allocate_area_locked(*state);
    if (area == nullptr) {
        return false;
    }
    area->base = base;
    area->length = length;
    area->flags = flags;
    area->resident_pages = 0;
    area->reservation_base = reservation_base;
    area->reservation_length = reservation_length;
    area->kind = kind;
    area->in_use = true;
    return true;
}

bool unregister_area_locked(AddressSpaceState& state,
                            uint64_t base,
                            uint64_t length,
                            vm::MappingKind* out_kind = nullptr) {
    for (auto& area : state.areas) {
        if (!area.in_use || area.base != base || area.length != length) {
            continue;
        }
        if (out_kind != nullptr) {
            *out_kind = area.kind;
        }
        area = {};
        return true;
    }
    return false;
}

bool unregister_area(uint64_t cr3,
                     uint64_t base,
                     uint64_t length,
                     vm::MappingKind* out_kind = nullptr) {
    sync::IrqLockGuard guard(g_address_space_state_lock);
    AddressSpaceState* state =
        find_address_space_state_locked(cr3, false);
    return state != nullptr &&
           unregister_area_locked(*state, base, length, out_kind);
}

bool set_area_resident_pages(uint64_t cr3,
                             uint64_t base,
                             uint64_t length,
                             uint64_t pages) {
    sync::IrqLockGuard guard(g_address_space_state_lock);
    AddressSpaceState* state =
        find_address_space_state_locked(cr3, false);
    if (state == nullptr) {
        return false;
    }
    for (auto& area : state->areas) {
        if (area.in_use && area.base == base && area.length == length) {
            area.resident_pages = pages;
            return true;
        }
    }
    return false;
}

bool materialize_anonymous_page_locked(AddressSpaceState& state,
                                       uint64_t address,
                                       bool write,
                                       bool execute) {
    VmArea* area = find_area_locked(state, address);
    if (area == nullptr || area->kind != vm::MappingKind::Anonymous ||
        execute || (write && (area->flags & vm::kMapWrite) == 0)) {
        return false;
    }

    uint64_t page = align_down(address, kPageSize);
    uint64_t existing = 0;
    if (paging_resolve_cr3(state.cr3, page, existing)) {
        uint64_t existing_flags = 0;
        return paging_flags_cr3(state.cr3, page, existing_flags) &&
               (existing_flags & PAGE_FLAG_USER) != 0 &&
               (!write || (existing_flags & PAGE_FLAG_WRITE) != 0);
    }

    uint64_t phys = memory::alloc_user_page();
    if (phys == 0) {
        return false;
    }
    auto* page_data = static_cast<uint8_t*>(paging_phys_to_virt(phys));
    memset(page_data, 0, kPageSize);

    uint64_t page_flags =
        PAGE_FLAG_USER | PAGE_FLAG_MANAGED | PAGE_FLAG_NO_EXECUTE;
    if ((area->flags & vm::kMapWrite) != 0) {
        page_flags |= PAGE_FLAG_WRITE;
    }
    if (!paging_map_page_cr3(state.cr3, page, phys, page_flags)) {
        memory::free_user_page(phys);
        return false;
    }
    ++area->resident_pages;
    return true;
}

bool copy_private_file_page_locked(AddressSpaceState& state,
                                   uint64_t address,
                                   bool write,
                                   bool execute) {
    VmArea* area = find_area_locked(state, address);
    if (area == nullptr || area->kind != vm::MappingKind::FilePrivate ||
        !write || execute || (area->flags & vm::kMapWrite) == 0) {
        return false;
    }

    uint64_t page = align_down(address, kPageSize);
    uint64_t source_phys = 0;
    uint64_t source_flags = 0;
    if (!paging_resolve_cr3(state.cr3, page, source_phys) ||
        !paging_flags_cr3(state.cr3, page, source_flags) ||
        (source_flags & PAGE_FLAG_USER) == 0) {
        return false;
    }
    if ((source_flags & PAGE_FLAG_WRITE) != 0) {
        return true;
    }

    uint64_t private_phys = memory::alloc_user_page();
    if (private_phys == 0) {
        return false;
    }
    void* source = paging_phys_to_virt(source_phys);
    void* destination = paging_phys_to_virt(private_phys);
    memcpy(destination, source, kPageSize);

    uint64_t unmapped_phys = 0;
    if (!paging_unmap_page_cr3(state.cr3, page, unmapped_phys) ||
        unmapped_phys != align_down(source_phys, kPageSize) ||
        !paging_map_page_cr3(state.cr3,
                             page,
                             private_phys,
                             PAGE_FLAG_WRITE | PAGE_FLAG_USER |
                                 PAGE_FLAG_MANAGED |
                                 PAGE_FLAG_NO_EXECUTE)) {
        if (unmapped_phys != 0) {
            (void)paging_map_page_cr3(state.cr3,
                                      page,
                                      unmapped_phys,
                                      source_flags &
                                          ~static_cast<uint64_t>(1));
        }
        memory::free_user_page(private_phys);
        return false;
    }
    page_cache::release_private_page(unmapped_phys);
    return true;
}

bool grow_stack_locked(AddressSpaceState& state,
                       uint64_t address,
                       bool write,
                       bool execute,
                       vm::Stack& stack,
                       uint64_t stack_pointer,
                       size_t max_stack_length) {
    if (!write || execute || stack.base == 0 || stack.top <= stack.base ||
        stack.length != stack.top - stack.base ||
        (stack.base & kPageMask) != 0 ||
        (stack.top & kPageMask) != 0 || address >= stack.base ||
        stack_pointer < kUserCodeBase || stack_pointer >= stack.top) {
        return false;
    }

    // Accept ordinary pushes, the System V red zone, and a stack pointer that
    // has already been moved into a larger frame. Reject unrelated accesses
    // far below RSP so an arbitrary bad pointer cannot grow the stack.
    if ((address <= stack_pointer &&
         stack_pointer - address > kPageSize) ||
        (address > stack_pointer && address - stack_pointer > 128)) {
        return false;
    }

    const uint64_t new_base = align_down(address, kPageSize);
    if (new_base < kUserCodeBase + kPageSize) {
        return false;
    }
    const uint64_t new_length = stack.top - new_base;
    if (new_length <= stack.length || new_length > max_stack_length) {
        return false;
    }

    VmArea* area = find_area_locked(state, stack.base);
    if (area == nullptr || area->base != stack.base ||
        area->length != stack.length ||
        area->kind != vm::MappingKind::Stack ||
        area->reservation_length == 0 ||
        area->reservation_base > UINT64_MAX - kPageSize ||
        new_base < area->reservation_base + kPageSize) {
        return false;
    }

    const uint64_t guard_base = new_base - kPageSize;
    for (uint64_t page = guard_base; page < stack.base;
         page += kPageSize) {
        uint64_t ignored_phys = 0;
        if (paging_resolve_cr3(state.cr3, page, ignored_phys)) {
            return false;
        }
    }

    const uint64_t added_length = stack.base - new_base;
    const size_t added_pages =
        static_cast<size_t>(added_length / kPageSize);
    for (size_t i = 0; i < added_pages; ++i) {
        uint64_t phys = memory::alloc_user_page();
        if (phys == 0) {
            rollback_user_pages(state.cr3, new_base, i);
            return false;
        }
        auto* page_data = static_cast<uint8_t*>(paging_phys_to_virt(phys));
        memset(page_data, 0, kPageSize);
        const uint64_t virt =
            new_base + static_cast<uint64_t>(i) * kPageSize;
        if (!paging_map_page_cr3(state.cr3,
                                 virt,
                                 phys,
                                 PAGE_FLAG_WRITE | PAGE_FLAG_USER |
                                     PAGE_FLAG_MANAGED |
                                     PAGE_FLAG_NO_EXECUTE)) {
            memory::free_user_page(phys);
            rollback_user_pages(state.cr3, new_base, i);
            return false;
        }
    }

    area->base = new_base;
    area->length = new_length;
    area->resident_pages += added_pages;
    stack.base = new_base;
    stack.length = static_cast<size_t>(new_length);
    return true;
}

bool resolve_managed_fault(uint64_t cr3,
                           uint64_t address,
                           bool write,
                           bool execute) {
    sync::IrqLockGuard guard(g_address_space_state_lock);
    AddressSpaceState* state =
        find_address_space_state_locked(cr3, false);
    if (state == nullptr) {
        return false;
    }
    VmArea* area = find_area_locked(*state, address);
    if (area == nullptr) {
        return false;
    }
    if (area->kind == vm::MappingKind::Anonymous) {
        return materialize_anonymous_page_locked(*state,
                                                 address,
                                                 write,
                                                 execute);
    }
    if (area->kind == vm::MappingKind::FilePrivate) {
        return copy_private_file_page_locked(*state,
                                             address,
                                             write,
                                             execute);
    }
    return false;
}

uint64_t count_present_pages(uint64_t cr3,
                             uint64_t base,
                             uint64_t length) {
    uint64_t count = 0;
    for (uint64_t offset = 0; offset < length; offset += kPageSize) {
        uint64_t phys = 0;
        if (paging_resolve_cr3(cr3, base + offset, phys)) {
            ++count;
        }
    }
    return count;
}

vm::Region reserve_private_region(uint64_t cr3, size_t length) {
    vm::Region region{0, 0};
    if (cr3 == 0 || length == 0) {
        return region;
    }
    size_t padded = 0;
    if (!page_aligned_length(length, padded)) {
        return region;
    }
    sync::IrqLockGuard guard(g_address_space_state_lock);
    AddressSpaceState* state =
        find_address_space_state_locked(cr3, true);
    if (state == nullptr) {
        return region;
    }

    size_t pages = padded / kPageSize;
    uint64_t total = static_cast<uint64_t>(pages) * kPageSize;
    uint64_t base = align_up(state->next_user_code, kPageSize);
    for (;;) {
        bool advanced = false;
        for (const auto& area : state->areas) {
            const uint64_t occupied_base =
                area.reservation_length != 0 ? area.reservation_base
                                             : area.base;
            const uint64_t occupied_length =
                area.reservation_length != 0 ? area.reservation_length
                                             : area.length;
            if (!area.in_use ||
                !ranges_overlap(base,
                                total,
                                occupied_base,
                                occupied_length)) {
                continue;
            }
            if (occupied_base > UINT64_MAX - occupied_length) {
                return vm::Region{0, 0};
            }
            base = align_up(occupied_base + occupied_length, kPageSize);
            advanced = true;
            break;
        }
        if (!advanced) {
            break;
        }
    }
    if (!vm::is_user_range(base, total)) {
        return vm::Region{0, 0};
    }

    region.base = base;
    region.length = pages * kPageSize;
    state->next_user_code = region.base + region.length;
    return region;
}

vm::Stack reserve_private_stack(uint64_t cr3, size_t total) {
    sync::IrqLockGuard guard(g_address_space_state_lock);
    AddressSpaceState* state =
        find_address_space_state_locked(cr3, true);
    if (state == nullptr) {
        return vm::Stack{0, 0, 0};
    }

    uint64_t top = align_down(state->next_user_stack, kPageSize);
    if (static_cast<uint64_t>(total) > top) {
        return vm::Stack{0, 0, 0};
    }
    uint64_t base = top - static_cast<uint64_t>(total);
    if (!vm::is_user_range(base, total) || base < state->next_user_code) {
        return vm::Stack{0, 0, 0};
    }
    state->next_user_stack = base;
    return vm::Stack{base, top, total};
}

void cancel_private_region(uint64_t cr3, const vm::Region& region) {
    if (cr3 == 0 || region.base == 0 || region.length == 0 ||
        region.base > static_cast<uint64_t>(-1) - region.length) {
        return;
    }

    const uint64_t reservation_end = region.base + region.length;
    sync::IrqLockGuard guard(g_address_space_state_lock);
    for (auto& state : g_address_space_states) {
        if (!state.in_use || state.cr3 != cr3) {
            continue;
        }
        // Reservations are monotonic. Rewind only when this is still the most
        // recent reservation, so a concurrent later reservation is untouched.
        if (state.next_user_code == reservation_end) {
            state.next_user_code = region.base;
        }
        return;
    }
}

void cancel_private_stack(uint64_t cr3, const vm::Stack& stack) {
    if (cr3 == 0 || stack.base == 0 || stack.top <= stack.base ||
        stack.length != stack.top - stack.base) {
        return;
    }

    sync::IrqLockGuard guard(g_address_space_state_lock);
    for (auto& state : g_address_space_states) {
        if (!state.in_use || state.cr3 != cr3) {
            continue;
        }
        // Stack reservations grow downward. As above, only reclaim the tip.
        if (state.next_user_stack == stack.base) {
            state.next_user_stack = stack.top;
        }
        return;
    }
}

}  // namespace

namespace vm {

Region map_user_code(uint64_t cr3,
                     const uint8_t* data,
                     size_t length,
                     uint64_t entry_offset, uint64_t& entry_point) {
    Region region{0, 0};
    if (cr3 == 0 || data == nullptr || length == 0) {
        entry_point = 0;
        return region;
    }
    region = reserve_private_region(cr3, length);
    if (region.base == 0 || region.length == 0) {
        entry_point = 0;
        return Region{0, 0};
    }
    if (!register_area(cr3,
                       region.base,
                       region.length,
                       kMapExecute,
                       MappingKind::Image)) {
        cancel_private_region(cr3, region);
        entry_point = 0;
        return Region{0, 0};
    }
    uint64_t base = region.base;
    size_t pages = region.length / kPageSize;

    for (size_t i = 0; i < pages; ++i) {
        uint64_t phys = memory::alloc_user_page();
        if (phys == 0) {
            rollback_user_pages(cr3, base, i);
            (void)unregister_area(cr3, region.base, region.length);
            cancel_private_region(cr3, region);
            entry_point = 0;
            return Region{0, 0};
        }
        auto* page = static_cast<uint8_t*>(paging_phys_to_virt(phys));
        uint64_t virt = base + static_cast<uint64_t>(i) * kPageSize;
        if (!paging_map_page_cr3(cr3,
                                 virt,
                                 phys,
                                 PAGE_FLAG_USER | PAGE_FLAG_MANAGED)) {
            memory::free_user_page(phys);
            rollback_user_pages(cr3, base, i);
            (void)unregister_area(cr3, region.base, region.length);
            cancel_private_region(cr3, region);
            entry_point = 0;
            return Region{0, 0};
        }

        size_t offset = i * kPageSize;
        size_t remaining = (offset < length) ? (length - offset) : 0;
        size_t copy_len = (remaining > kPageSize) ? kPageSize : remaining;
        if (copy_len > 0) {
            memcpy(page, data + offset, copy_len);
        }
        if (copy_len < kPageSize) {
            memset(page + copy_len, 0, kPageSize - copy_len);
        }
    }

    uint64_t safe_offset = (entry_offset < length) ? entry_offset : 0;
    (void)set_area_resident_pages(cr3,
                                  region.base,
                                  region.length,
                                  pages);
    entry_point = region.base + safe_offset;
    return region;
}

Region reserve_user_region(size_t length) {
    Region region{0, 0};
    if (length == 0) {
        return region;
    }

    size_t padded = 0;
    if (!page_aligned_length(length, padded)) {
        return region;
    }
    sync::IrqLockGuard guard(g_address_space_state_lock);
    uint64_t base = align_up(g_next_shared_user_code, kPageSize);
    size_t pages = padded / kPageSize;
    uint64_t total = static_cast<uint64_t>(pages) * kPageSize;
    if (!is_user_range(base, total)) {
        return Region{0, 0};
    }

    region.base = base;
    region.length = pages * kPageSize;
    g_next_shared_user_code = region.base + region.length;
    return region;
}

Region reserve_user_region(uint64_t cr3,
                           size_t length,
                           MappingKind kind) {
    Region region = reserve_private_region(cr3, length);
    if (region.base == 0 || region.length == 0) {
        return {};
    }
    if (kind != MappingKind::Shared && kind != MappingKind::Device) {
        cancel_private_region(cr3, region);
        return {};
    }
    if (!register_area(cr3,
                       region.base,
                       region.length,
                       kMapWrite,
                       kind)) {
        cancel_private_region(cr3, region);
        return {};
    }
    return region;
}

Region allocate_user_region(uint64_t cr3, size_t length) {
    Region region{0, 0};
    if (cr3 == 0 || length == 0) {
        return region;
    }

    region = reserve_private_region(cr3, length);
    if (region.base == 0 || region.length == 0) {
        return Region{0, 0};
    }
    if (!register_area(cr3,
                       region.base,
                       region.length,
                       kMapWrite,
                       MappingKind::Private)) {
        cancel_private_region(cr3, region);
        return Region{0, 0};
    }
    uint64_t base = region.base;
    size_t pages = region.length / kPageSize;

    for (size_t i = 0; i < pages; ++i) {
        uint64_t phys = memory::alloc_user_page();
        if (phys == 0) {
            rollback_user_pages(cr3, base, i);
            (void)unregister_area(cr3, region.base, region.length);
            cancel_private_region(cr3, region);
            return Region{0, 0};
        }
        auto* page = static_cast<uint8_t*>(paging_phys_to_virt(phys));
        memset(page, 0, kPageSize);
        uint64_t virt = base + static_cast<uint64_t>(i) * kPageSize;
        if (!paging_map_page_cr3(cr3,
                                 virt,
                                 phys,
                                 PAGE_FLAG_WRITE | PAGE_FLAG_USER |
                                     PAGE_FLAG_MANAGED |
                                     PAGE_FLAG_NO_EXECUTE)) {
            memory::free_user_page(phys);
            rollback_user_pages(cr3, base, i);
            (void)unregister_area(cr3, region.base, region.length);
            cancel_private_region(cr3, region);
            return Region{0, 0};
        }
    }

    (void)set_area_resident_pages(cr3,
                                  region.base,
                                  region.length,
                                  pages);
    return region;
}

Stack allocate_user_stack(uint64_t cr3, size_t length) {
    if (cr3 == 0) {
        log_message(LogLevel::Error,
                    "VM: stack alloc failed (cr3=0)");
        return Stack{0, 0, 0};
    }
    if (length == 0) {
        length = kPageSize;
    }
    size_t total = 0;
    if (!page_aligned_length(length, total)) {
        return Stack{0, 0, 0};
    }
    size_t pages = total / kPageSize;

    if (total > static_cast<size_t>(-1) - kPageSize) {
        return Stack{0, 0, 0};
    }
    size_t capacity = total;
    if (capacity < kMaxAutomaticStackSize) {
        capacity = kMaxAutomaticStackSize;
    }
    if (capacity > static_cast<size_t>(-1) - kPageSize) {
        return Stack{0, 0, 0};
    }
    Stack reservation =
        reserve_private_stack(cr3, capacity + kPageSize);
    if (reservation.base == 0) {
        log_message(LogLevel::Error,
                    "VM: stack alloc failed (state unavailable)");
        return Stack{0, 0, 0};
    }
    // Reserve room below the initial mapping for page-fault-driven growth.
    // The page immediately below the mapped portion remains the moving guard;
    // the lowest page of the reservation is the final overflow guard.
    Stack stack{
        reservation.top - total,
        reservation.top,
        total,
    };
    if (!register_area(cr3,
                       stack.base,
                       stack.length,
                       kMapWrite,
                       MappingKind::Stack,
                       reservation.base,
                       reservation.length)) {
        cancel_private_stack(cr3, reservation);
        return Stack{0, 0, 0};
    }

    for (size_t i = 0; i < pages; ++i) {
        uint64_t phys = memory::alloc_user_page();
        if (phys == 0) {
            log_message(LogLevel::Error,
                        "VM: stack alloc failed (page %zu/%zu)",
                        i + 1,
                        pages);
            rollback_user_pages(cr3, stack.base, i);
            (void)unregister_area(cr3, stack.base, stack.length);
            cancel_private_stack(cr3, reservation);
            return Stack{0, 0, 0};
        }
        auto* page = static_cast<uint8_t*>(paging_phys_to_virt(phys));
        uint64_t virt =
            stack.base + static_cast<uint64_t>(i) * kPageSize;
        if (!paging_map_page_cr3(cr3,
                                 virt,
                                 phys,
                                 PAGE_FLAG_WRITE | PAGE_FLAG_USER |
                                     PAGE_FLAG_MANAGED |
                                     PAGE_FLAG_NO_EXECUTE)) {
            memory::free_user_page(phys);
            rollback_user_pages(cr3, stack.base, i);
            (void)unregister_area(cr3, stack.base, stack.length);
            cancel_private_stack(cr3, reservation);
            return Stack{0, 0, 0};
        }
        memset(page, 0, kPageSize);
    }

    (void)set_area_resident_pages(cr3,
                                  stack.base,
                                  stack.length,
                                  pages);
    return stack;
}

uint64_t map_anonymous(uint64_t cr3, size_t length, uint64_t flags) {
    Region region = reserve_private_region(cr3, length);
    if (region.base == 0 || region.length == 0) {
        return 0;
    }
    if (!register_area(cr3,
                       region.base,
                       region.length,
                       flags,
                       MappingKind::Anonymous)) {
        cancel_private_region(cr3, region);
        return 0;
    }
    return region.base;
}

void release_address_space(uint64_t cr3) {
    if (cr3 == 0) {
        return;
    }
    sync::IrqLockGuard guard(g_address_space_state_lock);
    for (auto& state : g_address_space_states) {
        if (!state.in_use || state.cr3 != cr3) {
            continue;
        }
        for (auto& area : state.areas) {
            if (!area.in_use ||
                area.kind != MappingKind::FilePrivate) {
                continue;
            }
            for (uint64_t offset = 0; offset < area.length;
                 offset += kPageSize) {
                uint64_t phys = 0;
                uint64_t page_flags = 0;
                (void)paging_flags_cr3(cr3,
                                       area.base + offset,
                                       page_flags);
                if (paging_unmap_page_cr3(cr3,
                                          area.base + offset,
                                          phys)) {
                    if ((page_flags & PAGE_FLAG_MANAGED) != 0) {
                        memory::free_user_page(phys);
                    } else {
                        page_cache::release_private_page(phys);
                    }
                }
            }
        }
        state.in_use = false;
        state.cr3 = 0;
        state.next_user_code = 0;
        state.next_user_stack = 0;
        for (auto& area : state.areas) {
            area = {};
        }
        return;
    }
}

uint64_t map_at(uint64_t cr3, uint64_t addr_hint, size_t length, uint64_t flags) {
    if (addr_hint == 0) {
        return map_anonymous(cr3, length, flags);
    }
    if ((addr_hint & kPageMask) != 0) {
        return 0;
    }
    size_t total = 0;
    if (!page_aligned_length(length, total) ||
        !is_user_range(addr_hint, total)) {
        return 0;
    }
    for (uint64_t offset = 0; offset < total; offset += kPageSize) {
        uint64_t phys = 0;
        if (paging_resolve_cr3(cr3, addr_hint + offset, phys)) {
            return 0;
        }
    }
    if (!register_area(cr3,
                       addr_hint,
                       total,
                       flags,
                       MappingKind::Anonymous)) {
        return 0;
    }
    return addr_hint;
}

uint64_t map_file_private(uint64_t cr3,
                          const char* cache_key,
                          vfs::FileHandle& file,
                          uint64_t file_offset,
                          size_t length,
                          uint64_t flags) {
    if (cr3 == 0 || cache_key == nullptr || cache_key[0] == '\0' ||
        length == 0 || (file_offset & kPageMask) != 0 ||
        file_offset >= file.size) {
        return 0;
    }
    uint64_t available = file.size - file_offset;
    if (static_cast<uint64_t>(length) > available) {
        length = static_cast<size_t>(available);
    }
    Region region = reserve_private_region(cr3, length);
    if (region.base == 0 || region.length == 0) {
        return 0;
    }
    if (!register_area(cr3,
                       region.base,
                       region.length,
                       flags & kMapWrite,
                       MappingKind::FilePrivate)) {
        cancel_private_region(cr3, region);
        return 0;
    }

    size_t page_count = region.length / kPageSize;
    for (size_t i = 0; i < page_count; ++i) {
        uint64_t phys = 0;
        uint64_t page_file_offset =
            file_offset + static_cast<uint64_t>(i) * kPageSize;
        if (!page_cache::acquire_private_page(cache_key,
                                              file,
                                              page_file_offset,
                                              phys) ||
            !paging_map_page_cr3(cr3,
                                 region.base +
                                     static_cast<uint64_t>(i) * kPageSize,
                                 phys,
                                 PAGE_FLAG_USER |
                                     PAGE_FLAG_NO_EXECUTE)) {
            if (phys != 0) {
                page_cache::release_private_page(phys);
            }
            for (size_t rollback = 0; rollback < i; ++rollback) {
                uint64_t rollback_phys = 0;
                if (paging_unmap_page_cr3(
                        cr3,
                        region.base +
                            static_cast<uint64_t>(rollback) * kPageSize,
                        rollback_phys)) {
                    page_cache::release_private_page(rollback_phys);
                }
            }
            (void)unregister_area(cr3, region.base, region.length);
            cancel_private_region(cr3, region);
            return 0;
        }
    }
    (void)set_area_resident_pages(cr3,
                                  region.base,
                                  region.length,
                                  page_count);
    return region.base;
}

bool unmap_region(uint64_t cr3, uint64_t addr, size_t length) {
    if (cr3 == 0 || addr == 0 || length == 0) {
        return false;
    }
    if ((addr & kPageMask) != 0) {
        return false;
    }
    size_t total = 0;
    if (!page_aligned_length(length, total)) {
        return false;
    }
    if (!is_user_range(addr, total)) {
        return false;
    }

    {
        sync::IrqLockGuard guard(g_address_space_state_lock);
        AddressSpaceState* state =
            find_address_space_state_locked(cr3, false);
        VmArea* area = state != nullptr ? find_area_locked(*state, addr)
                                       : nullptr;
        if (area == nullptr ||
            (area->kind != MappingKind::Anonymous &&
             area->kind != MappingKind::FilePrivate) ||
            addr < area->base ||
            total > area->length - (addr - area->base)) {
            return false;
        }

        for (uint64_t offset = 0; offset < total; offset += kPageSize) {
            uint64_t phys = 0;
            uint64_t page_flags = 0;
            if (!paging_resolve_cr3(cr3, addr + offset, phys)) {
                continue;
            }
            if (!paging_flags_cr3(cr3, addr + offset, page_flags) ||
                (area->kind == MappingKind::Anonymous &&
                 (page_flags & PAGE_FLAG_MANAGED) == 0) ||
                (area->kind == MappingKind::FilePrivate &&
                 (page_flags & PAGE_FLAG_MANAGED) != 0 &&
                 (page_flags & PAGE_FLAG_WRITE) == 0)) {
                return false;
            }
        }

        const uint64_t area_end = area->base + area->length;
        const uint64_t unmap_end = addr + total;
        const MappingKind kind = area->kind;
        const uint64_t original_base = area->base;
        const uint64_t head_pages =
            addr > original_base
                ? count_present_pages(cr3,
                                      original_base,
                                      addr - original_base)
                : 0;
        const uint64_t tail_pages =
            unmap_end < area_end
                ? count_present_pages(cr3,
                                      unmap_end,
                                      area_end - unmap_end)
                : 0;
        if (addr == area->base && unmap_end == area_end) {
            *area = {};
        } else if (addr == area->base) {
            area->base = unmap_end;
            area->length = area_end - unmap_end;
            area->resident_pages = tail_pages;
        } else if (unmap_end == area_end) {
            area->length = addr - area->base;
            area->resident_pages = head_pages;
        } else {
            VmArea* tail = allocate_area_locked(*state);
            if (tail == nullptr) {
                return false;
            }
            tail->base = unmap_end;
            tail->length = area_end - unmap_end;
            tail->flags = area->flags;
            tail->kind = area->kind;
            tail->resident_pages = tail_pages;
            tail->in_use = true;
            area->length = addr - area->base;
            area->resident_pages = head_pages;
        }

        for (uint64_t offset = 0; offset < total; offset += kPageSize) {
            uint64_t phys = 0;
            uint64_t page_flags = 0;
            if (!paging_resolve_cr3(cr3, addr + offset, phys)) {
                continue;
            }
            (void)paging_flags_cr3(cr3, addr + offset, page_flags);
            if (paging_unmap_page_cr3(cr3, addr + offset, phys)) {
                if (kind == MappingKind::FilePrivate) {
                    if ((page_flags & PAGE_FLAG_MANAGED) != 0) {
                        memory::free_user_page(phys);
                    } else {
                        page_cache::release_private_page(phys);
                    }
                } else {
                    memory::free_user_page(phys);
                }
            }
        }
    }
    return true;
}

bool handle_page_fault(uint64_t cr3,
                       uint64_t address,
                       bool write,
                       bool execute,
                       Stack* current_stack,
                       uint64_t stack_pointer,
                       size_t max_stack_length) {
    if (cr3 == 0 || !is_user_range(address, 1)) {
        return false;
    }
    if (resolve_managed_fault(cr3, address, write, execute)) {
        return true;
    }
    if (current_stack == nullptr) {
        return false;
    }
    sync::IrqLockGuard guard(g_address_space_state_lock);
    AddressSpaceState* state =
        find_address_space_state_locked(cr3, false);
    return state != nullptr &&
           grow_stack_locked(*state,
                             address,
                             write,
                             execute,
                             *current_stack,
                             stack_pointer,
                             max_stack_length);
}

bool set_user_region_writable(uint64_t cr3,
                              uint64_t addr,
                              size_t length,
                              bool writable) {
    if (cr3 == 0 || addr == 0 || length == 0) {
        return false;
    }

    if (!is_user_range(addr, length)) {
        return false;
    }
    uint64_t base = align_down(addr, kPageSize);
    uint64_t end = align_up(addr + length, kPageSize);
    if (end < base || !is_user_range(base, end - base)) {
        return false;
    }

    for (uint64_t virt = base; virt < end; virt += kPageSize) {
        if (!paging_set_writable_cr3(cr3, virt, writable)) {
            return false;
        }
    }
    {
        sync::IrqLockGuard guard(g_address_space_state_lock);
        AddressSpaceState* state =
            find_address_space_state_locked(cr3, false);
        if (state != nullptr) {
            for (auto& area : state->areas) {
                if (!area.in_use ||
                    !ranges_overlap(base,
                                    end - base,
                                    area.base,
                                    area.length)) {
                    continue;
                }
                if (writable) {
                    area.flags |= kMapWrite;
                } else {
                    area.flags &= ~static_cast<uint64_t>(kMapWrite);
                }
            }
        }
    }

    return true;
}

bool set_user_region_executable(uint64_t cr3,
                                uint64_t addr,
                                size_t length,
                                bool executable) {
    if (cr3 == 0 || addr == 0 || length == 0) {
        return false;
    }

    if (!is_user_range(addr, length)) {
        return false;
    }
    uint64_t base = align_down(addr, kPageSize);
    uint64_t end = align_up(addr + length, kPageSize);
    if (end < base || !is_user_range(base, end - base)) {
        return false;
    }

    for (uint64_t virt = base; virt < end; virt += kPageSize) {
        if (!paging_set_executable_cr3(cr3, virt, executable)) {
            return false;
        }
    }
    {
        sync::IrqLockGuard guard(g_address_space_state_lock);
        AddressSpaceState* state =
            find_address_space_state_locked(cr3, false);
        if (state != nullptr) {
            for (auto& area : state->areas) {
                if (!area.in_use ||
                    !ranges_overlap(base,
                                    end - base,
                                    area.base,
                                    area.length)) {
                    continue;
                }
                if (executable) {
                    area.flags |= kMapExecute;
                } else {
                    area.flags &= ~static_cast<uint64_t>(kMapExecute);
                }
            }
        }
    }
    return true;
}

void release_user_region(uint64_t cr3, const Region& region) {
    if (region.base == 0 || region.length == 0) {
        return;
    }
    uint64_t base = align_down(region.base, kPageSize);
    uint64_t limit = align_up(region.length, kPageSize);
    MappingKind kind = MappingKind::Private;
    (void)unregister_area(cr3, base, limit, &kind);
    for (uint64_t offset = 0; offset < limit; offset += kPageSize) {
        uint64_t virt = base + offset;
        uint64_t phys = 0;
        uint64_t page_flags = 0;
        (void)paging_flags_cr3(cr3, virt, page_flags);
        if (!paging_unmap_page_cr3(cr3, virt, phys)) {
            continue;
        }
        if (kind == MappingKind::FilePrivate &&
            (page_flags & PAGE_FLAG_MANAGED) == 0) {
            page_cache::release_private_page(phys);
        } else {
            memory::free_user_page(phys);
        }
    }
}

void release_external_region(uint64_t cr3, const Region& region) {
    if (cr3 == 0 || region.base == 0 || region.length == 0) {
        return;
    }
    uint64_t base = align_down(region.base, kPageSize);
    uint64_t length = align_up(region.length, kPageSize);
    (void)unregister_area(cr3, base, length);
}

bool mark_region_resident(uint64_t cr3,
                          const Region& region,
                          size_t page_count) {
    if (region.base == 0 || region.length == 0 ||
        page_count > region.length / kPageSize) {
        return false;
    }
    return set_area_resident_pages(cr3,
                                   region.base,
                                   region.length,
                                   page_count);
}

bool is_user_range(uint64_t address, uint64_t length) {
    if (address < kUserAddressSpaceBase ||
        address >= kUserAddressSpaceTop) {
        return false;
    }
    if (length == 0) {
        return true;
    }
    uint64_t max_len = kUserAddressSpaceTop - address;
    if (length > max_len) {
        return false;
    }
    return true;
}

bool validate_user_buffer(uint64_t cr3,
                          uint64_t address,
                          size_t length,
                          bool writable) {
    if (length == 0) {
        return true;
    }
    if (cr3 == 0 || address == 0 ||
        !is_user_range(address, static_cast<uint64_t>(length))) {
        return false;
    }

    uint64_t current = address;
    size_t remaining = length;
    while (remaining != 0) {
        uint64_t phys = 0;
        uint64_t flags = 0;
        if (!paging_resolve_cr3(cr3, current, phys) &&
            !resolve_managed_fault(cr3, current, writable, false)) {
            return false;
        }
        if (writable &&
            paging_flags_cr3(cr3, current, flags) &&
            (flags & PAGE_FLAG_WRITE) == 0 &&
            !resolve_managed_fault(cr3, current, true, false)) {
            return false;
        }
        if (!paging_resolve_cr3(cr3, current, phys) ||
            !paging_flags_cr3(cr3, current, flags) ||
            (flags & PAGE_FLAG_USER) == 0 ||
            (writable && (flags & PAGE_FLAG_WRITE) == 0)) {
            return false;
        }
        size_t page_remaining =
            kPageSize - static_cast<size_t>(current & kPageMask);
        size_t chunk = remaining < page_remaining ? remaining : page_remaining;
        current += chunk;
        remaining -= chunk;
    }
    return true;
}

bool copy_user_string(uint64_t cr3,
                      const char* user,
                      char* dest,
                      size_t dest_size) {
    if (dest == nullptr || dest_size == 0) {
        return false;
    }
    dest[0] = '\0';
    if (user == nullptr) {
        return false;
    }
    size_t idx = 0;
    while (idx + 1 < dest_size) {
        uint64_t addr = reinterpret_cast<uint64_t>(user + idx);
        if (!copy_from_user(cr3, &dest[idx], addr, 1)) {
            dest[0] = '\0';
            return false;
        }
        char ch = dest[idx++];
        if (ch == '\0') {
            return true;
        }
    }
    dest[dest_size - 1] = '\0';
    return false;
}

bool copy_to_user(uint64_t cr3,
                  uint64_t dest,
                  const void* src,
                  size_t length) {
    if (length == 0) {
        return true;
    }
    if (cr3 == 0 || src == nullptr || dest == 0) {
        return false;
    }
    if (!is_user_range(dest, static_cast<uint64_t>(length))) {
        return false;
    }
    const auto* src_bytes = reinterpret_cast<const uint8_t*>(src);
    size_t offset = 0;
    while (offset < length) {
        uint64_t dest_addr = dest + offset;
        uint64_t phys = 0;
        if (!paging_resolve_cr3(cr3, dest_addr, phys) &&
            !resolve_managed_fault(cr3, dest_addr, true, false)) {
            return false;
        }
        uint64_t initial_flags = 0;
        if (paging_flags_cr3(cr3, dest_addr, initial_flags) &&
            (initial_flags & PAGE_FLAG_WRITE) == 0 &&
            !resolve_managed_fault(cr3, dest_addr, true, false)) {
            return false;
        }
        if (!paging_resolve_cr3(cr3, dest_addr, phys)) {
            return false;
        }
        uint64_t page_flags = 0;
        if (!paging_flags_cr3(cr3, dest_addr, page_flags) ||
            (page_flags & PAGE_FLAG_USER) == 0 ||
            (page_flags & PAGE_FLAG_WRITE) == 0) {
            return false;
        }
        size_t page_off = static_cast<size_t>(dest_addr & kPageMask);
        size_t chunk = kPageSize - page_off;
        if (chunk > length - offset) {
            chunk = length - offset;
        }
        void* dest_ptr = paging_phys_to_virt(phys);
        memcpy(dest_ptr, src_bytes + offset, chunk);
        offset += chunk;
    }
    return true;
}

bool copy_from_user(uint64_t cr3,
                    void* dest,
                    uint64_t src,
                    size_t length) {
    if (length == 0) {
        return true;
    }
    if (cr3 == 0 || dest == nullptr || src == 0) {
        return false;
    }
    if (!is_user_range(src, static_cast<uint64_t>(length))) {
        return false;
    }
    auto* dest_bytes = reinterpret_cast<uint8_t*>(dest);
    size_t offset = 0;
    while (offset < length) {
        uint64_t src_addr = src + offset;
        uint64_t phys = 0;
        if (!paging_resolve_cr3(cr3, src_addr, phys) &&
            !resolve_managed_fault(cr3, src_addr, false, false)) {
            return false;
        }
        if (!paging_resolve_cr3(cr3, src_addr, phys)) {
            return false;
        }
        uint64_t page_flags = 0;
        if (!paging_flags_cr3(cr3, src_addr, page_flags) ||
            (page_flags & PAGE_FLAG_USER) == 0) {
            return false;
        }
        size_t page_off = static_cast<size_t>(src_addr & kPageMask);
        size_t chunk = kPageSize - page_off;
        if (chunk > length - offset) {
            chunk = length - offset;
        }
        void* src_ptr = paging_phys_to_virt(phys);
        memcpy(dest_bytes + offset, src_ptr, chunk);
        offset += chunk;
    }
    return true;
}

bool fill_user(uint64_t cr3,
               uint64_t dest,
               uint8_t value,
               size_t length) {
    if (length == 0) {
        return true;
    }
    if (cr3 == 0 || dest == 0) {
        return false;
    }
    if (!is_user_range(dest, static_cast<uint64_t>(length))) {
        return false;
    }
    size_t offset = 0;
    while (offset < length) {
        uint64_t dest_addr = dest + offset;
        uint64_t phys = 0;
        if (!paging_resolve_cr3(cr3, dest_addr, phys) &&
            !resolve_managed_fault(cr3, dest_addr, true, false)) {
            return false;
        }
        uint64_t initial_flags = 0;
        if (paging_flags_cr3(cr3, dest_addr, initial_flags) &&
            (initial_flags & PAGE_FLAG_WRITE) == 0 &&
            !resolve_managed_fault(cr3, dest_addr, true, false)) {
            return false;
        }
        if (!paging_resolve_cr3(cr3, dest_addr, phys)) {
            return false;
        }
        uint64_t page_flags = 0;
        if (!paging_flags_cr3(cr3, dest_addr, page_flags) ||
            (page_flags & PAGE_FLAG_USER) == 0 ||
            (page_flags & PAGE_FLAG_WRITE) == 0) {
            return false;
        }
        size_t page_off = static_cast<size_t>(dest_addr & kPageMask);
        size_t chunk = kPageSize - page_off;
        if (chunk > length - offset) {
            chunk = length - offset;
        }
        void* dest_ptr = paging_phys_to_virt(phys);
        memset(dest_ptr, value, chunk);
        offset += chunk;
    }
    return true;
}

Usage usage(uint64_t cr3) {
    Usage result{};
    if (cr3 == 0) {
        return result;
    }
    sync::IrqLockGuard guard(g_address_space_state_lock);
    AddressSpaceState* state =
        find_address_space_state_locked(cr3, false);
    if (state == nullptr) {
        return result;
    }
    for (const auto& area : state->areas) {
        if (!area.in_use) {
            continue;
        }
        result.virtual_bytes += area.length;
        result.resident_bytes += area.resident_pages * kPageSize;
        if (area.kind == MappingKind::Shared ||
            area.kind == MappingKind::Device) {
            result.shared_bytes += area.length;
        } else if (area.kind == MappingKind::FilePrivate) {
            result.file_bytes += area.length;
        }
    }
    return result;
}

}  // namespace vm
