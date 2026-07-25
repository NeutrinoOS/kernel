#include "../descriptor.hpp"

#include "../../drivers/console/console.hpp"
#include "../../drivers/log/logging.hpp"
#include "../../lib/mem.hpp"
#include "../memory/physical_allocator.hpp"
#include "../process.hpp"
#include "../sync.hpp"
#include "../vm.hpp"
#include "arch/x86_64/memory/paging.hpp"

namespace descriptor {

namespace framebuffer_descriptor {

constexpr size_t kFramebufferSlots = 6;
constexpr size_t kPageSize = 0x1000;
constexpr size_t kMaxFramebufferBytes = 64ull * 1024 * 1024;
constexpr size_t kMaxFramebufferPages = kMaxFramebufferBytes / kPageSize;
constexpr uint64_t kFramebufferBackingVirtBase = 0xFFFFE20000000000ull;
constexpr uint64_t kFramebufferBackingSlotStride = kMaxFramebufferBytes;
constexpr size_t kPageTableEntries = 512;
constexpr size_t kFramebufferAllocationReserveBasePages = 8;

struct FramebufferSlot {
    Framebuffer fb;
    uint8_t* buffer;
    size_t buffer_bytes;
    uint64_t physical_base;
    uint64_t physical_pages[kMaxFramebufferPages];
    size_t physical_page_count;
    process::Process* owner;
    process::Process* session_owner;
    uint32_t open_count;
    uint32_t session_id;
    bool kernel_reserved;
};

FramebufferSlot g_framebuffers[kFramebufferSlots]{};
Framebuffer g_hw_fb{};
uint8_t* g_hw_base = nullptr;
uint64_t g_hw_physical_base = 0;
size_t g_frame_bytes = 0;
uint32_t g_active_slot = 0;
sync::SpinLock g_selection_lock;
sync::SpinLock g_lease_lock;
uint32_t g_next_session_id = 1;

class LeaseGuard {
public:
    LeaseGuard() { g_lease_lock.lock(); }
    ~LeaseGuard() { g_lease_lock.unlock(); }
    LeaseGuard(const LeaseGuard&) = delete;
    LeaseGuard& operator=(const LeaseGuard&) = delete;
};

class SelectionGuard {
public:
    SelectionGuard()
        : interrupt_flags_(sync::disable_interrupts()),
          locked_(g_selection_lock.try_lock()) {}
    ~SelectionGuard() {
        if (locked_) {
            g_selection_lock.unlock();
        }
        sync::restore_interrupts(interrupt_flags_);
    }
    bool locked() const { return locked_; }

private:
    uint64_t interrupt_flags_;
    bool locked_;
};

class BlockingSelectionGuard {
public:
    BlockingSelectionGuard() { g_selection_lock.lock(); }
    ~BlockingSelectionGuard() { g_selection_lock.unlock(); }
    BlockingSelectionGuard(const BlockingSelectionGuard&) = delete;
    BlockingSelectionGuard& operator=(const BlockingSelectionGuard&) = delete;
};

FramebufferSlot* slot_from_entry(DescriptorEntry& entry) {
    return static_cast<FramebufferSlot*>(entry.object);
}

uint64_t slot_backing_virtual_base(const FramebufferSlot& slot) {
    size_t index = static_cast<size_t>(&slot - g_framebuffers);
    return kFramebufferBackingVirtBase +
           static_cast<uint64_t>(index) * kFramebufferBackingSlotStride;
}

void release_slot_buffer(FramebufferSlot& slot) {
    if (slot.physical_page_count == 0) {
        return;
    }
    uint64_t virtual_base = slot_backing_virtual_base(slot);
    for (size_t i = 0; i < slot.physical_page_count; ++i) {
        uint64_t ignored = 0;
        (void)paging_unmap_page(
            virtual_base + static_cast<uint64_t>(i) * kPageSize,
            ignored);
        if (slot.physical_pages[i] != 0) {
            memory::free_user_page(slot.physical_pages[i]);
            slot.physical_pages[i] = 0;
        }
    }
    slot.buffer = nullptr;
    slot.buffer_bytes = 0;
    slot.physical_base = 0;
    slot.physical_page_count = 0;
}

bool ensure_slot_buffer(FramebufferSlot& slot) {
    if (slot.buffer != nullptr) {
        return true;
    }
    if (g_frame_bytes == 0) {
        return false;
    }
    if (g_frame_bytes > static_cast<size_t>(-1) - (kPageSize - 1)) {
        return false;
    }
    size_t pages = (g_frame_bytes + kPageSize - 1) / kPageSize;
    if (pages > kMaxFramebufferPages) {
        log_message(
            LogLevel::Warn,
            "Framebuffer: session backing too large slot=%zu bytes=%zu limit=%zu",
            static_cast<size_t>(&slot - g_framebuffers),
            g_frame_bytes,
            kMaxFramebufferBytes);
        return false;
    }
    size_t page_table_pages =
        (pages + kPageTableEntries - 1) / kPageTableEntries;
    size_t reserve_pages =
        kFramebufferAllocationReserveBasePages +
        2 * (page_table_pages + 3);
    size_t free_kernel_pages = memory::kernel_free_pages();
    if (free_kernel_pages < reserve_pages) {
        log_message(
            LogLevel::Warn,
            "Framebuffer: insufficient kernel page-table reserve slot=%zu free=%zu reserve=%zu",
            static_cast<size_t>(&slot - g_framebuffers),
            free_kernel_pages,
            reserve_pages);
        return false;
    }
    size_t free_user_pages = memory::user_free_pages();
    if (free_user_pages < pages) {
        log_message(
            LogLevel::Warn,
            "Framebuffer: insufficient user backing pages slot=%zu need=%zu free=%zu",
            static_cast<size_t>(&slot - g_framebuffers),
            pages,
            free_user_pages);
        return false;
    }

    uint64_t virtual_base = slot_backing_virtual_base(slot);
    for (size_t i = 0; i < pages; ++i) {
        uint64_t phys = memory::alloc_user_page();
        if (phys == 0) {
            log_message(
                LogLevel::Warn,
                "Framebuffer: backing allocation failed slot=%zu page=%zu/%zu free=%zu",
                static_cast<size_t>(&slot - g_framebuffers),
                i,
                pages,
                memory::user_free_pages());
            slot.physical_page_count = i;
            release_slot_buffer(slot);
            return false;
        }
        slot.physical_pages[i] = phys;
        slot.physical_page_count = i + 1;
        if (!paging_map_page(
                virtual_base + static_cast<uint64_t>(i) * kPageSize,
                phys,
                PAGE_FLAG_WRITE | PAGE_FLAG_NO_EXECUTE)) {
            log_message(
                LogLevel::Warn,
                "Framebuffer: backing map failed slot=%zu page=%zu/%zu",
                static_cast<size_t>(&slot - g_framebuffers),
                i,
                pages);
            release_slot_buffer(slot);
            return false;
        }
    }

    slot.buffer = reinterpret_cast<uint8_t*>(virtual_base);
    slot.buffer_bytes = pages * kPageSize;
    slot.physical_base = 0;
    memset(slot.buffer, 0, slot.buffer_bytes);
    slot.fb = g_hw_fb;
    slot.fb.base = slot.buffer;
    return true;
}

size_t usable_frame_bytes(const FramebufferSlot& slot) {
    return slot.buffer_bytes < g_frame_bytes ? slot.buffer_bytes
                                             : g_frame_bytes;
}

void copy_to_hardware(const FramebufferSlot& slot) {
    if (g_hw_base == nullptr || g_frame_bytes == 0 || slot.buffer == nullptr) {
        return;
    }
    size_t bytes = g_frame_bytes;
    if (slot.buffer_bytes < bytes) {
        bytes = slot.buffer_bytes;
    }
    memcpy_simd(g_hw_base, slot.buffer, bytes);
}

void select_locked(uint32_t index) {
    if (index >= kFramebufferSlots || g_frame_bytes == 0 ||
        g_hw_base == nullptr ||
        index == __atomic_load_n(&g_active_slot, __ATOMIC_ACQUIRE)) {
        return;
    }
    if (index != 0) {
        FramebufferSlot& target = g_framebuffers[index];
        if (target.buffer == nullptr ||
            __atomic_load_n(&target.open_count, __ATOMIC_ACQUIRE) == 0 ||
            __atomic_load_n(&target.owner, __ATOMIC_ACQUIRE) == nullptr) {
            return;
        }
        copy_to_hardware(target);
    }

    __atomic_store_n(&g_active_slot, index, __ATOMIC_RELEASE);
    if (index == 0) {
        if (kconsole != nullptr) {
            kconsole->present();
        } else {
            copy_to_hardware(g_framebuffers[0]);
        }
    }
}

bool copy_rect_to_hardware(const FramebufferSlot& slot,
                           uint32_t x,
                           uint32_t y,
                           uint32_t width,
                           uint32_t height) {
    if (g_hw_base == nullptr || slot.buffer == nullptr) {
        return false;
    }
    const Framebuffer* fb = &slot.fb;
    if (fb->width == 0 || fb->height == 0 || fb->pitch == 0) {
        return false;
    }
    if (x >= fb->width || y >= fb->height) {
        return false;
    }
    if (width == 0 || height == 0) {
        return false;
    }
    if (static_cast<size_t>(width) > fb->width - x) {
        width = static_cast<uint32_t>(fb->width - x);
    }
    if (static_cast<size_t>(height) > fb->height - y) {
        height = static_cast<uint32_t>(fb->height - y);
    }
    uint32_t bytes_per_pixel = (fb->bpp + 7u) / 8u;
    if (bytes_per_pixel == 0) {
        return false;
    }
    if (static_cast<size_t>(width) >
        static_cast<size_t>(-1) / bytes_per_pixel) {
        return false;
    }
    size_t row_bytes = static_cast<size_t>(width) * bytes_per_pixel;
    if (static_cast<size_t>(x) >
        static_cast<size_t>(-1) / bytes_per_pixel) {
        return false;
    }
    size_t x_offset = static_cast<size_t>(x) * bytes_per_pixel;
    if (x_offset > fb->pitch || row_bytes > fb->pitch - x_offset) {
        return false;
    }
    size_t frame_bytes = usable_frame_bytes(slot);
    for (uint32_t row = 0; row < height; ++row) {
        size_t row_index = static_cast<size_t>(y) + row;
        if (row_index > static_cast<size_t>(-1) / fb->pitch) {
            return false;
        }
        size_t row_offset = row_index * fb->pitch;
        if (row_offset > static_cast<size_t>(-1) - x_offset) {
            return false;
        }
        size_t offset = row_offset + x_offset;
        if (offset > frame_bytes || row_bytes > frame_bytes - offset) {
            return false;
        }
        memcpy_simd(g_hw_base + offset, slot.buffer + offset, row_bytes);
    }
    return true;
}

bool map_slot_into_process(process::Process& proc,
                           FramebufferSlot& slot,
                           uint64_t& out_base) {
    if (proc.cr3 == 0 || slot.physical_page_count == 0 ||
        slot.buffer_bytes == 0) {
        return false;
    }
    vm::Region region = vm::reserve_user_region(
        proc.cr3,
        slot.buffer_bytes,
        vm::MappingKind::Device);
    if (region.base == 0 || region.length == 0) {
        return false;
    }
    uint64_t base = region.base;
    uint64_t total = region.length;
    uint64_t flags = PAGE_FLAG_WRITE | PAGE_FLAG_USER | PAGE_FLAG_NO_EXECUTE;
    for (uint64_t offset = 0; offset < total; offset += kPageSize) {
        size_t page = static_cast<size_t>(offset / kPageSize);
        if (page >= slot.physical_page_count) {
            log_message(
                LogLevel::Warn,
                "Framebuffer: user map exceeded backing slot=%zu page=%zu/%zu",
                static_cast<size_t>(&slot - g_framebuffers),
                page,
                slot.physical_page_count);
            for (uint64_t rollback = 0; rollback < offset;
                 rollback += kPageSize) {
                uint64_t freed = 0;
                (void)paging_unmap_page_cr3(
                    proc.cr3, base + rollback, freed);
            }
            vm::release_external_region(proc.cr3, region);
            return false;
        }
        uint64_t phys = slot.physical_pages[page];
        if (!paging_map_page_cr3(proc.cr3,
                                 base + offset,
                                 phys,
                                 flags)) {
            log_message(
                LogLevel::Warn,
                "Framebuffer: user map failed slot=%zu page=%zu/%zu",
                static_cast<size_t>(&slot - g_framebuffers),
                page,
                slot.physical_page_count);
            for (uint64_t rollback = 0; rollback < offset; rollback += kPageSize) {
                uint64_t freed = 0;
                paging_unmap_page_cr3(proc.cr3, base + rollback, freed);
            }
            vm::release_external_region(proc.cr3, region);
            return false;
        }
    }
    if (!vm::mark_region_resident(proc.cr3,
                                  region,
                                  static_cast<size_t>(total / kPageSize))) {
        log_message(
            LogLevel::Warn,
            "Framebuffer: unable to account user mapping slot=%zu pages=%zu",
            static_cast<size_t>(&slot - g_framebuffers),
            slot.physical_page_count);
        for (uint64_t offset = 0; offset < total; offset += kPageSize) {
            uint64_t ignored = 0;
            (void)paging_unmap_page_cr3(proc.cr3, base + offset, ignored);
        }
        vm::release_external_region(proc.cr3, region);
        return false;
    }
    out_base = base;
    return true;
}

int64_t framebuffer_read(process::Process&,
                         DescriptorEntry& entry,
                         uint64_t user_address,
                         uint64_t length,
                         uint64_t offset) {
    auto* slot = slot_from_entry(entry);
    if (slot == nullptr || slot->buffer == nullptr) {
        return -1;
    }
    size_t frame_bytes = usable_frame_bytes(*slot);
    if (offset > frame_bytes) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    if (length > static_cast<uint64_t>(frame_bytes) - offset) {
        return -1;
    }
    auto* src = slot->buffer;
    auto* dest = reinterpret_cast<uint8_t*>(user_address);
    if (src == nullptr || dest == nullptr) {
        return -1;
    }
    for (uint64_t i = 0; i < length; ++i) {
        dest[i] = src[offset + i];
    }
    return static_cast<int64_t>(length);
}

int64_t framebuffer_write(process::Process&,
                          DescriptorEntry& entry,
                          uint64_t user_address,
                          uint64_t length,
                          uint64_t offset) {
    auto* slot = slot_from_entry(entry);
    if (slot == nullptr || slot->buffer == nullptr) {
        return -1;
    }
    size_t frame_bytes = usable_frame_bytes(*slot);
    if (offset > frame_bytes) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    if (length > static_cast<uint64_t>(frame_bytes) - offset) {
        return -1;
    }
    auto* dest = slot->buffer;
    auto* src = reinterpret_cast<const uint8_t*>(user_address);
    if (dest == nullptr || src == nullptr) {
        return -1;
    }
    for (uint64_t i = 0; i < length; ++i) {
        dest[offset + i] = src[i];
    }
    return static_cast<int64_t>(length);
}

int framebuffer_get_property(DescriptorEntry& entry,
                             uint32_t property,
                             void* out,
                             size_t size) {
    auto* slot = slot_from_entry(entry);
    if (slot == nullptr || slot->buffer == nullptr) {
        return -1;
    }
    Framebuffer* fb = &slot->fb;
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::FramebufferInfo)) {
        if (out == nullptr || size < sizeof(descriptor_defs::FramebufferInfo)) {
            return -1;
        }
        auto* info = reinterpret_cast<descriptor_defs::FramebufferInfo*>(out);
        uint32_t slot_index = static_cast<uint32_t>(slot - g_framebuffers);
        // Users need the mapped virtual address, not a kernel/device physical
        // address disclosure. Slot zero is kernel-only.
        info->physical_base =
            slot_index == 0 ? g_hw_physical_base : 0;
        if (entry.subsystem_data != nullptr) {
            info->virtual_base =
                reinterpret_cast<uint64_t>(entry.subsystem_data);
        } else {
            info->virtual_base =
                reinterpret_cast<uint64_t>(slot->buffer);
        }
        info->width = static_cast<uint32_t>(fb->width);
        info->height = static_cast<uint32_t>(fb->height);
        info->pitch = static_cast<uint32_t>(fb->pitch);
        info->bpp = fb->bpp;
        info->memory_model = fb->memory_model;
        info->reserved = 0;
        info->red_mask_size = fb->red_mask_size;
        info->red_mask_shift = fb->red_mask_shift;
        info->green_mask_size = fb->green_mask_size;
        info->green_mask_shift = fb->green_mask_shift;
        info->blue_mask_size = fb->blue_mask_size;
        info->blue_mask_shift = fb->blue_mask_shift;
        return 0;
    }
    return -1;
}

int framebuffer_set_property(DescriptorEntry& entry,
                             uint32_t property,
                             const void* in,
                             size_t size) {
    if (property !=
        static_cast<uint32_t>(descriptor_defs::Property::FramebufferPresent)) {
        return -1;
    }
    BlockingSelectionGuard selection_guard;
    auto* slot = slot_from_entry(entry);
    if (slot == nullptr || slot->buffer == nullptr) {
        return -1;
    }
    uint32_t slot_index =
        static_cast<uint32_t>(slot - g_framebuffers);
    if (__atomic_load_n(&g_active_slot, __ATOMIC_ACQUIRE) != slot_index) {
        return -1;
    }
    if (size == 0 || in == nullptr) {
        copy_to_hardware(*slot);
        return 0;
    }
    if (size < sizeof(descriptor_defs::FramebufferRect)) {
        return -1;
    }
    auto* rect =
        reinterpret_cast<const descriptor_defs::FramebufferRect*>(in);
    return copy_rect_to_hardware(*slot,
                                 rect->x,
                                 rect->y,
                                 rect->width,
                                 rect->height)
               ? 0
               : -1;
}

void framebuffer_close(DescriptorEntry& entry) {
    auto* slot = slot_from_entry(entry);
    if (slot == nullptr) {
        return;
    }
    process::Process* owner = nullptr;
    bool last_close = false;
    {
        LeaseGuard lease_guard;
        owner = slot->owner;
        if (owner != nullptr && !is_kernel_process(*owner) &&
            entry.subsystem_data != nullptr) {
            uint64_t base =
                reinterpret_cast<uint64_t>(entry.subsystem_data);
            for (uint64_t offset = 0; offset < slot->buffer_bytes;
                 offset += kPageSize) {
                uint64_t ignored = 0;
                (void)paging_unmap_page_cr3(owner->cr3,
                                            base + offset,
                                            ignored);
            }
            vm::release_external_region(
                owner->cr3,
                vm::Region{base, slot->buffer_bytes});
        }
        uint32_t open_count =
            __atomic_load_n(&slot->open_count, __ATOMIC_RELAXED);
        if (open_count > 0) {
            --open_count;
            __atomic_store_n(&slot->open_count,
                             open_count,
                             __ATOMIC_RELEASE);
        }
        if (open_count == 0 && !slot->kernel_reserved) {
            if (slot->owner == owner) {
                __atomic_store_n(&slot->owner,
                                 static_cast<process::Process*>(nullptr),
                                 __ATOMIC_RELEASE);
            }
            last_close = true;
        }
    }
    if (last_close && owner != nullptr && !is_kernel_process(*owner)) {
        BlockingSelectionGuard selection_guard;
        LeaseGuard lease_guard;
        if (__atomic_load_n(&slot->open_count, __ATOMIC_ACQUIRE) == 0 &&
            __atomic_load_n(&slot->owner, __ATOMIC_ACQUIRE) == nullptr) {
            uint32_t slot_index =
                static_cast<uint32_t>(slot - g_framebuffers);
            if (__atomic_load_n(&g_active_slot, __ATOMIC_ACQUIRE) ==
                slot_index) {
                select_locked(0);
            }
            release_slot_buffer(*slot);
        }
    }
}

const Ops kFramebufferOps{
    .read = framebuffer_read,
    .write = framebuffer_write,
    .get_property = framebuffer_get_property,
    .set_property = framebuffer_set_property,
};

FramebufferSlot* allocate_user_slot(process::Process& proc) {
    // A graphical session reserves its display slot before opening the
    // framebuffer, making the session descriptor the authority for display
    // and input ownership.
    for (size_t i = 1; i < kFramebufferSlots; ++i) {
        FramebufferSlot& slot = g_framebuffers[i];
        if (slot.session_owner == &proc &&
            (slot.owner == nullptr || slot.owner == &proc)) {
            return &slot;
        }
    }
    for (size_t i = 1; i < kFramebufferSlots; ++i) {
        FramebufferSlot& slot = g_framebuffers[i];
        if (slot.session_owner == nullptr &&
            (slot.owner == nullptr || slot.owner == &proc)) {
            return &slot;
        }
    }
    return nullptr;
}

bool open_framebuffer(process::Process& proc,
                      uint64_t arg0,
                      uint64_t,
                      uint64_t,
                      Allocation& alloc) {
    LeaseGuard lease_guard;
    bool is_kernel = is_kernel_process(proc);
    FramebufferSlot* slot = nullptr;
    if (arg0 != 0) {
        uint32_t index = static_cast<uint32_t>(arg0);
        if (index >= kFramebufferSlots) {
            return false;
        }
        slot = &g_framebuffers[index];
    } else if (is_kernel) {
        slot = &g_framebuffers[0];
    } else {
        slot = allocate_user_slot(proc);
    }
    if (slot == nullptr) {
        return false;
    }
    if (!is_kernel && slot->kernel_reserved) {
        return false;
    }
    if (!is_kernel && slot->session_owner != nullptr &&
        slot->session_owner != &proc) {
        return false;
    }
    if (slot->owner != nullptr && slot->owner != &proc) {
        return false;
    }
    bool first_open_for_owner = slot->owner == nullptr;
    if (!ensure_slot_buffer(*slot)) {
        return false;
    }
    if (!is_kernel && first_open_for_owner) {
        // Slots are recycled across sessions; never expose the previous
        // owner's pixels or page-rounded tail bytes to a new process.
        memset(slot->buffer, 0, slot->buffer_bytes);
    }
    uint64_t mapped_base = 0;
    if (!is_kernel) {
        if (!map_slot_into_process(proc, *slot, mapped_base)) {
            return false;
        }
    }
    __atomic_store_n(&slot->owner, &proc, __ATOMIC_RELAXED);
    __atomic_fetch_add(&slot->open_count,
                       static_cast<uint32_t>(1),
                       __ATOMIC_RELEASE);
    alloc.type = kTypeFramebuffer;
    alloc.flags = static_cast<uint64_t>(Flag::Readable) |
                  static_cast<uint64_t>(Flag::Writable) |
                  static_cast<uint64_t>(Flag::Mappable) |
                  static_cast<uint64_t>(Flag::Device);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = slot;
    if (is_kernel) {
        alloc.subsystem_data = slot->buffer;
    } else {
        alloc.subsystem_data =
            reinterpret_cast<void*>(mapped_base);
    }
    alloc.close = framebuffer_close;
    alloc.name = "framebuffer";
    alloc.ops = &kFramebufferOps;
    return true;
}

FramebufferSlot* graphical_session_slot(DescriptorEntry& entry) {
    return static_cast<FramebufferSlot*>(entry.object);
}

int graphical_session_get_property(DescriptorEntry& entry,
                                   uint32_t property,
                                   void* out,
                                   size_t size) {
    if (property != static_cast<uint32_t>(
                        descriptor_defs::Property::GraphicalSessionInfo) ||
        out == nullptr ||
        size < sizeof(descriptor_defs::GraphicalSessionInfo)) {
        return -1;
    }

    LeaseGuard lease_guard;
    FramebufferSlot* slot = graphical_session_slot(entry);
    if (slot == nullptr || slot->session_owner == nullptr ||
        slot->session_id == 0) {
        return -1;
    }
    uint32_t slot_index = static_cast<uint32_t>(slot - g_framebuffers);
    auto* info =
        static_cast<descriptor_defs::GraphicalSessionInfo*>(out);
    info->abi_major = descriptor_defs::kGraphicalSessionAbiMajor;
    info->abi_minor = descriptor_defs::kGraphicalSessionAbiMinor;
    info->display_slot = static_cast<uint16_t>(slot_index);
    info->reserved0 = 0;
    info->session_id = slot->session_id;
    info->flags = 0;
    if (__atomic_load_n(&g_active_slot, __ATOMIC_ACQUIRE) == slot_index) {
        info->flags |= descriptor_defs::kGraphicalSessionFlagActive;
    }
    if (slot->owner == slot->session_owner &&
        __atomic_load_n(&slot->open_count, __ATOMIC_ACQUIRE) != 0) {
        info->flags |= descriptor_defs::kGraphicalSessionFlagFramebufferOpen;
    }
    info->features =
        descriptor_defs::kGraphicalSessionFeatureDisplayLease |
        descriptor_defs::kGraphicalSessionFeatureInputSeat |
        descriptor_defs::kGraphicalSessionFeatureDamagePresent;
    info->reserved1 = 0;
    return 0;
}

int graphical_session_set_property(DescriptorEntry& entry,
                                   uint32_t property,
                                   const void* in,
                                   size_t size) {
    if (property != static_cast<uint32_t>(
                        descriptor_defs::Property::GraphicalSessionControl) ||
        in == nullptr ||
        size < sizeof(descriptor_defs::GraphicalSessionControl)) {
        return -1;
    }
    const auto* control =
        static_cast<const descriptor_defs::GraphicalSessionControl*>(in);
    if (control->flags != 0) {
        return -1;
    }

    FramebufferSlot* slot = graphical_session_slot(entry);
    if (slot == nullptr || slot->session_owner == nullptr) {
        return -1;
    }
    uint32_t slot_index = static_cast<uint32_t>(slot - g_framebuffers);
    if (control->command ==
        descriptor_defs::kGraphicalSessionCommandActivate) {
        return framebuffer_activate_for_process(*slot->session_owner,
                                                slot_index)
                   ? 0
                   : -1;
    }
    if (control->command ==
        descriptor_defs::kGraphicalSessionCommandDeactivate) {
        if (!framebuffer_is_active(slot_index)) {
            return 0;
        }
        framebuffer_select(0);
        return framebuffer_is_active(0) ? 0 : -1;
    }
    return -1;
}

void graphical_session_close(DescriptorEntry& entry) {
    FramebufferSlot* slot = graphical_session_slot(entry);
    if (slot == nullptr) {
        return;
    }
    uint32_t slot_index = static_cast<uint32_t>(slot - g_framebuffers);
    BlockingSelectionGuard selection_guard;
    LeaseGuard lease_guard;
    if (__atomic_load_n(&g_active_slot, __ATOMIC_ACQUIRE) == slot_index) {
        select_locked(0);
    }
    slot->session_owner = nullptr;
    slot->session_id = 0;
}

const Ops kGraphicalSessionOps{
    .read = nullptr,
    .write = nullptr,
    .get_property = graphical_session_get_property,
    .set_property = graphical_session_set_property,
};

bool open_graphical_session(process::Process& proc,
                            uint64_t resource_selector,
                            uint64_t requested_flags,
                            uint64_t open_context,
                            Allocation& alloc) {
    if (is_kernel_process(proc) || resource_selector != 0 ||
        requested_flags != 0 || open_context != 0) {
        return false;
    }

    LeaseGuard lease_guard;
    for (size_t i = 1; i < kFramebufferSlots; ++i) {
        if (g_framebuffers[i].session_owner == &proc) {
            return false;
        }
    }

    FramebufferSlot* slot = nullptr;
    for (size_t i = 1; i < kFramebufferSlots; ++i) {
        FramebufferSlot& candidate = g_framebuffers[i];
        if (candidate.session_owner == nullptr &&
            (candidate.owner == nullptr || candidate.owner == &proc)) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) {
        return false;
    }

    uint32_t session_id = g_next_session_id++;
    if (session_id == 0) {
        session_id = g_next_session_id++;
    }
    slot->session_owner = &proc;
    slot->session_id = session_id;

    alloc.type = kTypeGraphicalSession;
    alloc.flags = static_cast<uint64_t>(Flag::Device);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = slot;
    alloc.subsystem_data = nullptr;
    alloc.close = graphical_session_close;
    alloc.name = "graphical-session";
    alloc.ops = &kGraphicalSessionOps;
    return true;
}

}  // namespace framebuffer_descriptor

bool register_framebuffer_descriptor() {
    return register_type(kTypeFramebuffer, framebuffer_descriptor::open_framebuffer, &framebuffer_descriptor::kFramebufferOps);
}

bool register_graphical_session_descriptor() {
    return register_type(
        kTypeGraphicalSession,
        framebuffer_descriptor::open_graphical_session,
        &framebuffer_descriptor::kGraphicalSessionOps);
}

void register_framebuffer_device(Framebuffer& framebuffer,
                                 uint64_t physical_base) {
    using namespace framebuffer_descriptor;
    for (size_t i = 1; i < kFramebufferSlots; ++i) {
        release_slot_buffer(g_framebuffers[i]);
    }
    g_hw_fb = framebuffer;
    g_hw_base = framebuffer.base;
    g_hw_physical_base = physical_base;
    g_frame_bytes = 0;
    if (framebuffer.pitch != 0 &&
        framebuffer.height <=
            static_cast<size_t>(-1) / framebuffer.pitch) {
        g_frame_bytes = framebuffer.pitch * framebuffer.height;
    }
    for (size_t i = 0; i < kFramebufferSlots; ++i) {
        FramebufferSlot& slot = g_framebuffers[i];
        slot.fb = framebuffer;
        slot.buffer = nullptr;
        slot.buffer_bytes = 0;
        slot.physical_base = 0;
        slot.physical_page_count = 0;
        slot.owner = nullptr;
        slot.session_owner = nullptr;
        slot.open_count = 0;
        slot.session_id = 0;
        slot.kernel_reserved = (i == 0);
        slot.physical_base = (i == 0) ? physical_base : 0;
    }
    g_framebuffers[0].buffer = g_hw_base;
    g_framebuffers[0].buffer_bytes = g_frame_bytes;
    __atomic_store_n(&g_active_slot, 0u, __ATOMIC_RELEASE);
    g_next_session_id = 1;
}

void framebuffer_select(uint32_t index) {
    using namespace framebuffer_descriptor;
    SelectionGuard guard;
    if (!guard.locked()) {
        return;
    }
    select_locked(index);
}

bool framebuffer_is_active(uint32_t index) {
    using namespace framebuffer_descriptor;
    return __atomic_load_n(&g_active_slot, __ATOMIC_ACQUIRE) == index;
}

uint32_t framebuffer_active_slot() {
    using namespace framebuffer_descriptor;
    return __atomic_load_n(&g_active_slot, __ATOMIC_ACQUIRE);
}

int32_t framebuffer_slot_for_process(const process::Process& proc) {
    using namespace framebuffer_descriptor;
    if (is_kernel_process(proc)) {
        return 0;
    }
    LeaseGuard lease_guard;
    for (size_t i = 1; i < kFramebufferSlots; ++i) {
        if (g_framebuffers[i].session_owner == &proc) {
            return static_cast<int32_t>(i);
        }
    }
    for (size_t i = 1; i < kFramebufferSlots; ++i) {
        if (g_framebuffers[i].owner == &proc) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

bool framebuffer_process_owns_slot(const process::Process& proc,
                                   uint32_t slot) {
    using namespace framebuffer_descriptor;
    if (is_kernel_process(proc)) {
        return slot == 0;
    }
    if (slot == 0 || slot >= kFramebufferSlots) {
        return false;
    }
    LeaseGuard lease_guard;
    const FramebufferSlot& candidate = g_framebuffers[slot];
    return candidate.session_owner == &proc || candidate.owner == &proc;
}

bool framebuffer_activate_for_process(const process::Process& proc,
                                      uint32_t slot) {
    using namespace framebuffer_descriptor;
    if (is_kernel_process(proc) || slot == 0 || slot >= kFramebufferSlots) {
        return false;
    }

    // User-driven activation may enter from the console or refresh the
    // caller's already-active lease. Switching away from another graphical
    // session is reserved for the kernel's secure session-switch path.
    BlockingSelectionGuard selection_guard;
    LeaseGuard lease_guard;
    const FramebufferSlot& candidate = g_framebuffers[slot];
    if ((candidate.session_owner != &proc && candidate.owner != &proc) ||
        candidate.owner != &proc ||
        __atomic_load_n(&candidate.open_count, __ATOMIC_ACQUIRE) == 0 ||
        (__atomic_load_n(&g_active_slot, __ATOMIC_ACQUIRE) != 0 &&
         __atomic_load_n(&g_active_slot, __ATOMIC_ACQUIRE) != slot)) {
        return false;
    }
    select_locked(slot);
    return __atomic_load_n(&g_active_slot, __ATOMIC_ACQUIRE) == slot;
}

}  // namespace descriptor
