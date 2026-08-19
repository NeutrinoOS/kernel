#include "arch/x86_64/memory/paging.hpp"

#include <stddef.h>
#include <stdint.h>

#include "../../drivers/console/console.hpp"
#include "arch/x86_64/lapic.hpp"
#include "arch/x86_64/smp.hpp"
#include "drivers/limine/limine_requests.hpp"
#include "kernel/interrupts.hpp"
#include "kernel/memory/physical_allocator.hpp"
#include "kernel/sync.hpp"
#include "lib/mem.hpp"
#include "kernel/error.hpp"

extern "C" char kernel_start[];
extern "C" char kernel_end[];
extern "C" char kernel_text_start[];
extern "C" char kernel_text_end[];
extern "C" char kernel_rodata_start[];
extern "C" char kernel_rodata_end[];

namespace {

constexpr uint64_t PAGE_SIZE = 0x1000;
constexpr uint64_t PAGE_MASK = PAGE_SIZE - 1;
constexpr uint64_t PAGE_LARGE_SIZE = 0x200000;
constexpr uint64_t PAGE_LARGE_MASK = PAGE_LARGE_SIZE - 1;
constexpr uint64_t PAGE_HUGE_SIZE = 0x40000000;
constexpr uint64_t PAGE_HUGE_MASK = PAGE_HUGE_SIZE - 1;
// Long-mode page-table entries carry flags above the physical frame number
// (notably NX in bit 63).  Never derive a physical address by merely clearing
// the page offset, because that would retain those flags.
constexpr uint64_t PHYSICAL_ADDRESS_MASK = 0x000FFFFFFFFFF000ull;

constexpr uint64_t PTE_PRESENT = 1ull << 0;
constexpr uint64_t PTE_WRITE = 1ull << 1;
constexpr uint64_t PTE_USER = 1ull << 2;
constexpr uint64_t PTE_PWT = 1ull << 3;
constexpr uint64_t PTE_PCD = 1ull << 4;
constexpr uint64_t PTE_PAT = 1ull << 7;
constexpr uint64_t PTE_LARGE = 1ull << 7;
constexpr uint64_t PTE_GLOBAL = 1ull << 8;
constexpr uint64_t PTE_MANAGED = PAGE_FLAG_MANAGED;
constexpr uint64_t PTE_NX = 1ull << 63;
constexpr uint64_t CR0_WRITE_PROTECT = 1ull << 16;
constexpr uint64_t CR4_PAGE_GLOBAL_ENABLE = 1ull << 7;
constexpr uint64_t CR4_PCID_ENABLE = 1ull << 17;
constexpr uint64_t CR3_NO_FLUSH = 1ull << 63;
constexpr uint64_t PCID_MASK = 0xfffull;
constexpr size_t PCID_COUNT = 4096;
constexpr uint32_t MSR_EFER = 0xC0000080;
constexpr uint64_t EFER_NXE = 1ull << 11;

constexpr size_t PAGE_TABLE_ENTRIES = 512;
constexpr size_t MAX_ADDRESS_SPACES = 256;

constexpr size_t BOOT_POOL_PAGES = 4096;
constexpr size_t BOOT_POOL_SIZE = BOOT_POOL_PAGES * PAGE_SIZE;

constexpr uint64_t LAPIC_BASE = 0xFEE00000;
// x86 AP startup begins below 1 MiB. Limine keeps its APs in a trampoline in
// bootloader-reclaimable low memory until the kernel fills in goto_address.
constexpr uint64_t SMP_TRAMPOLINE_PHYS_LIMIT = 0x100000;

alignas(PAGE_SIZE) uint8_t boot_pool[BOOT_POOL_SIZE];
size_t boot_pool_off = 0;
void* boot_pool_freelist[BOOT_POOL_PAGES];
size_t boot_pool_free_count = 0;

uint64_t g_kernel_phys_base = 0;
uint64_t g_kernel_virt_base = 0;
uint64_t g_kernel_size = 0;
uint64_t g_hhdm_offset = 0;
uint64_t g_kernel_cr3 = 0;

uint64_t* pml4_table = nullptr;
uint64_t* g_address_space_roots[MAX_ADDRESS_SPACES];
bool g_address_space_activated[MAX_ADDRESS_SPACES]{};
size_t g_address_space_count = 0;
bool g_pcid_in_use[PCID_COUNT]{};
sync::SpinLock g_address_space_registry_lock;

sync::SpinLock g_tlb_shootdown_lock;
uint8_t g_tlb_shootdown_vector = 0;
volatile size_t g_tlb_shootdown_acks = 0;
enum class TlbShootdownKind : uint8_t {
    All,
    Context,
    Page,
};
volatile TlbShootdownKind g_tlb_shootdown_kind = TlbShootdownKind::All;
volatile uint64_t g_tlb_shootdown_cr3 = 0;
volatile uint64_t g_tlb_shootdown_address = 0;

uint64_t read_cr3() {
    uint64_t value = 0;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

uint64_t read_cr4() {
    uint64_t value = 0;
    asm volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

bool pcid_enabled() {
    return (read_cr4() & CR4_PCID_ENABLE) != 0;
}

uint64_t cr3_context(uint64_t cr3) {
    return cr3 & (PHYSICAL_ADDRESS_MASK | PCID_MASK);
}

void flush_all_local() {
    uint64_t cr4 = read_cr4();
    uint64_t toggled = cr4 ^ CR4_PAGE_GLOBAL_ENABLE;
    asm volatile("mov %0, %%cr4" : : "r"(toggled) : "memory");
    asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

void with_context_local(uint64_t target_cr3,
                        uint64_t address,
                        bool single_page) {
    uint64_t current = read_cr3();
    target_cr3 = cr3_context(target_cr3);
    if (cr3_context(current) == target_cr3) {
        if (single_page) {
            asm volatile("invlpg (%0)"
                         :
                         : "r"(reinterpret_cast<void*>(address))
                         : "memory");
        } else {
            asm volatile("mov %0, %%cr3" : : "r"(current) : "memory");
        }
        return;
    }
    if (!pcid_enabled()) {
        return;
    }

    // Every process root shares the kernel half. Temporarily entering the
    // target context lets CPUs without INVPCID invalidate a dormant PCID.
    asm volatile("mov %0, %%cr3" : : "r"(target_cr3) : "memory");
    if (single_page) {
        asm volatile("invlpg (%0)"
                     :
                     : "r"(reinterpret_cast<void*>(address))
                     : "memory");
    }
    asm volatile("mov %0, %%cr3"
                 :
                 : "r"(cr3_context(current) | CR3_NO_FLUSH)
                 : "memory");
}

void tlb_shootdown_handler() {
    TlbShootdownKind kind = g_tlb_shootdown_kind;
    if (kind == TlbShootdownKind::All) {
        flush_all_local();
    } else {
        with_context_local(g_tlb_shootdown_cr3,
                           g_tlb_shootdown_address,
                           kind == TlbShootdownKind::Page);
    }
    __atomic_fetch_add(&g_tlb_shootdown_acks,
                       static_cast<size_t>(1),
                       __ATOMIC_RELEASE);
}

uint16_t allocate_pcid_locked() {
    if (!pcid_enabled()) {
        return 0;
    }
    for (size_t pcid = 1; pcid < PCID_COUNT; ++pcid) {
        if (!g_pcid_in_use[pcid]) {
            g_pcid_in_use[pcid] = true;
            return static_cast<uint16_t>(pcid);
        }
    }
    return 0;
}

void release_pcid_locked(uint16_t pcid) {
    if (pcid != 0) {
        g_pcid_in_use[pcid] = false;
    }
}

bool shootdown_all_cpus(TlbShootdownKind kind,
                        uint64_t cr3 = 0,
                        uint64_t address = 0) {
    const uint64_t interrupt_state = sync::disable_interrupts();
    while (!g_tlb_shootdown_lock.try_lock()) {
        // The owner can be waiting for this CPU's shootdown acknowledgement.
        // Page faults may arrive through a frame whose saved IF is clear, so
        // merely restoring the caller's flags while spinning is insufficient:
        // explicitly open a one-instruction interrupt window for the IPI.
        asm volatile("sti; pause; cli" ::: "memory");
    }
    if (g_tlb_shootdown_vector == 0) {
        uint8_t vector = interrupts::allocate_vector();
        if (vector == 0 ||
            !interrupts::register_vector(vector, tlb_shootdown_handler)) {
            if (vector != 0) {
                interrupts::free_vector(vector);
            }
            g_tlb_shootdown_lock.unlock();
            sync::restore_interrupts(interrupt_state);
            return false;
        }
        g_tlb_shootdown_vector = vector;
    }

    g_tlb_shootdown_kind = kind;
    g_tlb_shootdown_cr3 = cr3_context(cr3);
    g_tlb_shootdown_address = address;
    if (kind == TlbShootdownKind::All) {
        flush_all_local();
    } else {
        with_context_local(cr3, address, kind == TlbShootdownKind::Page);
    }

    size_t online = smp::online_cpus();
    if (online <= 1) {
        g_tlb_shootdown_lock.unlock();
        sync::restore_interrupts(interrupt_state);
        return true;
    }
    __atomic_store_n(&g_tlb_shootdown_acks,
                     static_cast<size_t>(0),
                     __ATOMIC_RELEASE);
    lapic::send_ipi_all_others(g_tlb_shootdown_vector);
    while (__atomic_load_n(&g_tlb_shootdown_acks, __ATOMIC_ACQUIRE) <
           online - 1) {
        asm volatile("pause");
    }
    g_tlb_shootdown_lock.unlock();
    sync::restore_interrupts(interrupt_state);
    return true;
}

constexpr uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

constexpr uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

uint64_t read_msr(uint32_t msr) {
    uint32_t low = 0;
    uint32_t high = 0;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | low;
}

void write_msr(uint32_t msr, uint64_t value) {
    asm volatile("wrmsr"
                 :
                 : "c"(msr),
                   "a"(static_cast<uint32_t>(value)),
                   "d"(static_cast<uint32_t>(value >> 32)));
}

void* alloc_boot_page() {
    KERNEL_ASSERT_MSG(boot_pool_free_count <= BOOT_POOL_PAGES,
                      "paging boot-pool freelist count is out of bounds");
    if (boot_pool_free_count > 0) {
        void* v = boot_pool_freelist[--boot_pool_free_count];
        memset(v, 0, PAGE_SIZE);
        return v;
    }
    if (boot_pool_off + PAGE_SIZE > BOOT_POOL_SIZE) {
        error_screen::display("PAGING_", "BOOT_POOL_EXHAUSTED", nullptr);
    }

    void* v = &boot_pool[boot_pool_off];
    boot_pool_off += PAGE_SIZE;
    memset(v, 0, PAGE_SIZE);
    return v;
}

struct PagingStructurePage {
    void* virt;
    uint64_t phys;
};

void free_boot_page(void* page) {
    if (page == nullptr) {
        return;
    }
    auto* ptr = reinterpret_cast<uint8_t*>(page);
    auto* pool_begin = &boot_pool[0];
    auto* pool_end = pool_begin + BOOT_POOL_SIZE;
    KERNEL_ASSERT_MSG(ptr >= pool_begin && ptr < pool_end,
                      "paging boot pool received a foreign page");
    KERNEL_ASSERT_MSG(
        (static_cast<size_t>(ptr - pool_begin) & PAGE_MASK) == 0,
        "paging boot pool received an unaligned page");
    KERNEL_ASSERT_MSG(boot_pool_free_count < BOOT_POOL_PAGES,
                      "paging boot-pool freelist overflow");
    boot_pool_freelist[boot_pool_free_count++] = page;
}

uint64_t virt_to_phys(uint64_t virt) {
    return virt - g_kernel_virt_base + g_kernel_phys_base;
}

uint64_t phys_to_virt(uint64_t phys) {
    return phys - g_kernel_phys_base + g_kernel_virt_base;
}

void* table_phys_to_virt(uint64_t phys) {
    if (g_hhdm_offset != 0) {
        return reinterpret_cast<void*>(phys + g_hhdm_offset);
    }
    return reinterpret_cast<void*>(phys_to_virt(phys));
}

PagingStructurePage alloc_paging_structure_page() {
    if (memory::kernel_allocator_ready()) {
        uint64_t phys = memory::alloc_kernel_page();
        if (phys == 0) {
            size_t free_pages = memory::kernel_free_pages();
            if (kconsole != nullptr) {
                kconsole->set_color(0xFFFF0000, 0x00000000);
                kconsole->printf("Paging structure allocation failed, free=%llu pages, halting.\n",
                                 static_cast<unsigned long long>(free_pages));
            }
            error_screen::display("PAGING_", "STRUCTURE_ALLOCATION_FAILED", nullptr);
        }
        return PagingStructurePage{paging_phys_to_virt(phys), phys};
    }
    void* virt = alloc_boot_page();
    return PagingStructurePage{virt, virt_to_phys(reinterpret_cast<uint64_t>(virt))};
}

uint64_t* table_from_entry(uint64_t entry) {
    uint64_t phys = entry & PHYSICAL_ADDRESS_MASK;
    return reinterpret_cast<uint64_t*>(table_phys_to_virt(phys));
}

uint64_t* ensure_table(uint64_t* table, size_t index, uint64_t flags) {
    uint64_t entry = table[index];
    if ((entry & PTE_PRESENT) == 0) {
        PagingStructurePage child_page = alloc_paging_structure_page();
        auto* child = static_cast<uint64_t*>(child_page.virt);
        uint64_t phys = child_page.phys;
        uint64_t child_flags = PTE_PRESENT | PTE_WRITE;
        if (flags & PTE_USER) {
            child_flags |= PTE_USER;
        }
        table[index] = phys | child_flags;
        return child;
    }

    if (entry & PTE_LARGE) {
        PagingStructurePage child_page = alloc_paging_structure_page();
        auto* child = static_cast<uint64_t*>(child_page.virt);
        uint64_t phys_base =
            entry & (PHYSICAL_ADDRESS_MASK & ~PAGE_LARGE_MASK);
        uint64_t base_flags = entry & ((1ull << 12) - 1);
        base_flags &= ~PTE_LARGE;
        base_flags |= PTE_PRESENT;
        uint64_t nx_flag = entry & PTE_NX;

        for (size_t i = 0; i < PAGE_TABLE_ENTRIES; ++i) {
            uint64_t child_phys = phys_base + (i * PAGE_SIZE);
            uint64_t child_entry = (child_phys & ~PAGE_MASK) | base_flags;
            child_entry |= nx_flag;
            child[i] = child_entry;
        }

        uint64_t child_phys_addr = child_page.phys;
        uint64_t pointer_flags = entry & ((1ull << 12) - 1);
        pointer_flags &= ~PTE_LARGE;
        pointer_flags |= PTE_PRESENT | PTE_WRITE;
        table[index] = child_phys_addr | pointer_flags;
        entry = table[index];
    }

    if ((flags & PTE_USER) != 0 && (entry & PTE_USER) == 0) {
        table[index] |= PTE_USER;
        entry = table[index];
    }

    uint64_t phys = entry & PHYSICAL_ADDRESS_MASK;
    return reinterpret_cast<uint64_t*>(table_phys_to_virt(phys));
}

bool map_page_with_root(uint64_t* root, uint64_t virt, uint64_t phys, uint64_t flags) {
    size_t pml4_index = (virt >> 39) & 0x1FF;
    size_t pdpt_index = (virt >> 30) & 0x1FF;
    size_t pd_index = (virt >> 21) & 0x1FF;
    size_t pt_index = (virt >> 12) & 0x1FF;

    uint64_t* pdpt = ensure_table(root, pml4_index, flags);
    uint64_t* pd = ensure_table(pdpt, pdpt_index, flags);
    uint64_t* pt = ensure_table(pd, pd_index, flags);

    bool replaced_present = (pt[pt_index] & PTE_PRESENT) != 0;
    pt[pt_index] = (phys & ~PAGE_MASK) | flags | PTE_PRESENT;
    return replaced_present;
}

void sync_kernel_pml4_entry(size_t pml4_index) {
    if (pml4_index < PAGE_TABLE_ENTRIES / 2) {
        return;
    }
    sync::IrqLockGuard guard(g_address_space_registry_lock);
    for (size_t i = 0; i < g_address_space_count; ++i) {
        uint64_t* root = g_address_space_roots[i];
        if (root == nullptr || root == pml4_table) {
            continue;
        }
        root[pml4_index] = pml4_table[pml4_index];
    }
}

void destroy_table_level(uint64_t table_phys, int level) {
    if (table_phys == 0 || level <= 0) {
        return;
    }

    auto* table =
        reinterpret_cast<uint64_t*>(table_phys_to_virt(table_phys));
    if (table == nullptr) {
        return;
    }

    if (level == 1) {
        for (size_t i = 0; i < PAGE_TABLE_ENTRIES; ++i) {
            uint64_t entry = table[i];
            if ((entry & (PTE_PRESENT | PTE_MANAGED)) ==
                (PTE_PRESENT | PTE_MANAGED)) {
                memory::free_user_page(entry & PHYSICAL_ADDRESS_MASK);
            }
            table[i] = 0;
        }
        return;
    }

    for (size_t i = 0; i < PAGE_TABLE_ENTRIES; ++i) {
        uint64_t entry = table[i];
        if ((entry & PTE_PRESENT) == 0) {
            continue;
        }
        if ((entry & PTE_LARGE) != 0) {
            table[i] = 0;
            continue;
        }

        uint64_t child_phys = entry & PHYSICAL_ADDRESS_MASK;
        destroy_table_level(child_phys, level - 1);
        memory::free_kernel_page(child_phys);
        table[i] = 0;
    }
}

bool table_empty(const uint64_t* table) {
    if (table == nullptr) {
        return true;
    }
    for (size_t i = 0; i < PAGE_TABLE_ENTRIES; ++i) {
        if ((table[i] & PTE_PRESENT) != 0) {
            return false;
        }
    }
    return true;
}

bool try_map_large_page(uint64_t* root, uint64_t virt, uint64_t phys, uint64_t flags) {
    if ((virt & PAGE_LARGE_MASK) != 0 || (phys & PAGE_LARGE_MASK) != 0) {
        return false;
    }

    size_t pml4_index = (virt >> 39) & 0x1FF;
    size_t pdpt_index = (virt >> 30) & 0x1FF;
    size_t pd_index = (virt >> 21) & 0x1FF;

    uint64_t* pdpt = ensure_table(root, pml4_index, flags);
    uint64_t* pd = ensure_table(pdpt, pdpt_index, flags);

    uint64_t entry = pd[pd_index];
    if (entry & PTE_PRESENT) {
        if ((entry & PTE_LARGE) != 0) {
            return true;  // Already mapped as large page
        }
        return false;  // Already mapped via 4K pages, cannot convert
    }

    pd[pd_index] = (phys & ~PAGE_LARGE_MASK) | flags | PTE_PRESENT | PTE_LARGE;
    return true;
}

void map_range_with_root(uint64_t* root,
                         uint64_t virt_start,
                         uint64_t phys_start,
                         uint64_t length,
                         uint64_t flags) {
    if (length == 0) return;

    uint64_t phys_begin = align_down(phys_start, PAGE_SIZE);
    uint64_t phys_end = align_up(phys_start + length, PAGE_SIZE);

    uint64_t virt_begin = align_down(virt_start, PAGE_SIZE);
    uint64_t offset = virt_begin - phys_begin;

    for (uint64_t phys = phys_begin; phys < phys_end;) {
        uint64_t virt = phys + offset;
        if ((phys_end - phys) >= PAGE_LARGE_SIZE &&
            try_map_large_page(root, virt, phys, flags)) {
            phys += PAGE_LARGE_SIZE;
            continue;
        }

        map_page_with_root(root, virt, phys, flags);
        phys += PAGE_SIZE;
    }
}

void map_identity_range(uint64_t phys_start, uint64_t length, uint64_t flags) {
    if (length == 0) return;

    uint64_t start = align_down(phys_start, PAGE_SIZE);
    uint64_t end = align_up(phys_start + length, PAGE_SIZE);

    for (uint64_t phys = start; phys < end;) {
        if ((end - phys) >= PAGE_LARGE_SIZE &&
            try_map_large_page(pml4_table, phys, phys, flags)) {
            phys += PAGE_LARGE_SIZE;
            continue;
        }

        map_page_with_root(pml4_table, phys, phys, flags);
        phys += PAGE_SIZE;
    }
}

bool should_map(uint64_t type) {
    switch (type) {
        case LIMINE_MEMMAP_USABLE:
        case LIMINE_MEMMAP_RESERVED:
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
        case LIMINE_MEMMAP_ACPI_NVS:
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
#if LIMINE_API_REVISION >= 2
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
#else
        case LIMINE_MEMMAP_KERNEL_AND_MODULES:
#endif
        case LIMINE_MEMMAP_FRAMEBUFFER:
            return true;
        default:
            return false;
    }
}

}  // namespace

void paging_init() {
    if (memmap_request.response == nullptr ||
        kernel_addr_request.response == nullptr) {
        error_screen::display("PAGING_", "LIMINE_REQUEST_FAILED", nullptr);
    }
    if (hhdm_request.response != nullptr) {
        g_hhdm_offset = hhdm_request.response->offset;
    } else {
        g_hhdm_offset = 0;
    }

    uint64_t efer = read_msr(MSR_EFER);
    if ((efer & EFER_NXE) == 0) {
        write_msr(MSR_EFER, efer | EFER_NXE);
    }

    g_kernel_phys_base = kernel_addr_request.response->physical_base;
    g_kernel_virt_base = reinterpret_cast<uint64_t>(&kernel_start);
    uint64_t kernel_virtual_end = reinterpret_cast<uint64_t>(&kernel_end);
    if (kernel_virtual_end < g_kernel_virt_base) {
        error_screen::display("PAGING_", "KERNEL_ADDRESS_INVALID", nullptr);
    }
    g_kernel_size =
        align_up(kernel_virtual_end - g_kernel_virt_base, PAGE_SIZE);

    boot_pool_off = 0;
    pml4_table = static_cast<uint64_t*>(alloc_boot_page());

    const uint64_t map_flags = PTE_WRITE | PTE_GLOBAL | PTE_NX;

    auto memmap = memmap_request.response;
    for (uint64_t i = 0; i < memmap->entry_count; ++i) {
        auto entry = memmap->entries[i];
        if (entry == nullptr || entry->length == 0) {
            continue;
        }

        if (!should_map(entry->type)) {
            continue;
        }

        map_identity_range(entry->base, entry->length, map_flags);
        if (g_hhdm_offset != 0) {
            map_range_with_root(pml4_table,
                                entry->base + g_hhdm_offset,
                                entry->base,
                                entry->length,
                                map_flags);

            // The APs can still be executing Limine's low-memory trampoline
            // after the BSP installs this CR3. Keep only the low portion of
            // bootloader-reclaimable memory executable, and make that alias
            // read-only to preserve W^X. It is returned to NX once every AP
            // has entered the kernel.
            if (entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE &&
                entry->base < SMP_TRAMPOLINE_PHYS_LIMIT) {
                uint64_t executable_end = entry->base + entry->length;
                if (executable_end < entry->base ||
                    executable_end > SMP_TRAMPOLINE_PHYS_LIMIT) {
                    executable_end = SMP_TRAMPOLINE_PHYS_LIMIT;
                }
                map_range_with_root(pml4_table,
                                    entry->base + g_hhdm_offset,
                                    entry->base,
                                    executable_end - entry->base,
                                    PTE_GLOBAL);
                map_range_with_root(pml4_table,
                                    entry->base,
                                    entry->base,
                                    executable_end - entry->base,
                                    PTE_GLOBAL | PTE_NX);
            }
        }
    }

    if (g_kernel_size != 0) {
        map_range_with_root(pml4_table,
                            g_kernel_virt_base,
                            g_kernel_phys_base,
                            g_kernel_size,
                            map_flags);

        uint64_t text_start = reinterpret_cast<uint64_t>(kernel_text_start);
        uint64_t text_end = reinterpret_cast<uint64_t>(kernel_text_end);
        if (text_end > text_start) {
            uint64_t text_phys = virt_to_phys(text_start);
            map_range_with_root(pml4_table,
                                text_start,
                                text_phys,
                                text_end - text_start,
                                PTE_GLOBAL);
            // The identity and HHDM aliases must not provide a writable route
            // to executable kernel text.
            map_range_with_root(pml4_table,
                                text_phys,
                                text_phys,
                                text_end - text_start,
                                PTE_GLOBAL | PTE_NX);
            if (g_hhdm_offset != 0) {
                map_range_with_root(pml4_table,
                                    text_phys + g_hhdm_offset,
                                    text_phys,
                                    text_end - text_start,
                                    PTE_GLOBAL | PTE_NX);
            }
        }

        uint64_t rodata_start =
            reinterpret_cast<uint64_t>(kernel_rodata_start);
        uint64_t rodata_end = reinterpret_cast<uint64_t>(kernel_rodata_end);
        if (rodata_end > rodata_start) {
            uint64_t rodata_phys = virt_to_phys(rodata_start);
            map_range_with_root(pml4_table,
                                rodata_start,
                                rodata_phys,
                                rodata_end - rodata_start,
                                PTE_GLOBAL | PTE_NX);
            map_range_with_root(pml4_table,
                                rodata_phys,
                                rodata_phys,
                                rodata_end - rodata_start,
                                PTE_GLOBAL | PTE_NX);
            if (g_hhdm_offset != 0) {
                map_range_with_root(pml4_table,
                                    rodata_phys + g_hhdm_offset,
                                    rodata_phys,
                                    rodata_end - rodata_start,
                                    PTE_GLOBAL | PTE_NX);
            }
        }
    }

    // Map Local APIC MMIO (identity + HHDM) with cache disabled.
    const uint64_t lapic_flags =
        PTE_PRESENT | PTE_WRITE | PTE_PCD | PTE_PWT | PTE_NX;
    map_page_with_root(pml4_table, LAPIC_BASE, LAPIC_BASE, lapic_flags);
    if (g_hhdm_offset != 0) {
        map_page_with_root(pml4_table,
                           LAPIC_BASE + g_hhdm_offset,
                           LAPIC_BASE,
                           lapic_flags);
    }

    uint64_t new_cr3 = virt_to_phys(reinterpret_cast<uint64_t>(pml4_table));
    asm volatile("mov %0, %%cr3" : : "r"(new_cr3) : "memory");
    g_kernel_cr3 = new_cr3;

    uint64_t cr0 = 0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_WRITE_PROTECT;
    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    // Reserve the shootdown vector before address spaces begin retaining
    // tagged translations. Failure here would make safe PCID reuse impossible.
    if (!shootdown_all_cpus(TlbShootdownKind::All)) {
        error_screen::display("PAGING_", "TLB_SHOOTDOWN_INIT_FAILED", nullptr);
    }
}

bool paging_finish_smp_bootstrap() {
    if (g_hhdm_offset == 0 || memmap_request.response == nullptr) {
        return true;
    }

    auto* memmap = memmap_request.response;
    bool ok = true;
    for (uint64_t i = 0; i < memmap->entry_count; ++i) {
        auto* entry = memmap->entries[i];
        if (entry == nullptr || entry->length == 0 ||
            entry->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
            entry->base >= SMP_TRAMPOLINE_PHYS_LIMIT) {
            continue;
        }

        uint64_t end = entry->base + entry->length;
        if (end < entry->base || end > SMP_TRAMPOLINE_PHYS_LIMIT) {
            end = SMP_TRAMPOLINE_PHYS_LIMIT;
        }
        uint64_t start = align_down(entry->base, PAGE_SIZE);
        end = align_up(end, PAGE_SIZE);
        for (uint64_t phys = start; phys < end; phys += PAGE_SIZE) {
            ok = paging_set_executable_cr3(g_kernel_cr3,
                                            phys + g_hhdm_offset,
                                            false) && ok;
            ok = paging_set_writable_cr3(g_kernel_cr3,
                                         phys + g_hhdm_offset,
                                         true) && ok;
            ok = paging_set_writable_cr3(g_kernel_cr3,
                                         phys,
                                         true) && ok;
        }
    }

    return paging_flush_tlb_all_cpus() && ok;
}

bool paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    map_page_with_root(pml4_table, virt, phys, flags);
    sync_kernel_pml4_entry((virt >> 39) & 0x1FF);
    asm volatile("invlpg (%0)" : : "r"(reinterpret_cast<void*>(virt)) : "memory");
    return true;
}

uint64_t paging_virt_to_phys(uint64_t virt) {
    return virt_to_phys(virt);
}

void* paging_alloc_page() {
    return alloc_boot_page();
}

bool paging_unmap_page(uint64_t virt, uint64_t& phys_out) {
    phys_out = 0;
    size_t pml4_index = (virt >> 39) & 0x1FF;
    size_t pdpt_index = (virt >> 30) & 0x1FF;
    size_t pd_index = (virt >> 21) & 0x1FF;
    size_t pt_index = (virt >> 12) & 0x1FF;

    uint64_t pml4_entry = pml4_table[pml4_index];
    if ((pml4_entry & PTE_PRESENT) == 0) {
        return false;
    }
    uint64_t* pdpt = table_from_entry(pml4_entry);

    uint64_t pdpt_entry = pdpt[pdpt_index];
    if ((pdpt_entry & PTE_PRESENT) == 0) {
        return false;
    }
    uint64_t* pd = table_from_entry(pdpt_entry);

    uint64_t pd_entry = pd[pd_index];
    if ((pd_entry & PTE_PRESENT) == 0 || (pd_entry & PTE_LARGE) != 0) {
        return false;
    }
    uint64_t* pt = table_from_entry(pd_entry);

    uint64_t pt_entry = pt[pt_index];
    if ((pt_entry & PTE_PRESENT) == 0) {
        return false;
    }
    phys_out = pt_entry & PHYSICAL_ADDRESS_MASK;
    pt[pt_index] = 0;
    asm volatile("invlpg (%0)" : : "r"(reinterpret_cast<void*>(virt)) : "memory");
    return true;
}

void paging_free_physical(uint64_t phys) {
    if (phys == 0) {
        return;
    }
    void* page = table_phys_to_virt(phys);
    free_boot_page(page);
}

bool paging_mark_wc(uint64_t virt, uint64_t length) {
    if (length == 0) {
        return false;
    }

    uint64_t start = align_down(virt, PAGE_SIZE);
    uint64_t end = align_up(virt + length, PAGE_SIZE);

    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
        size_t pml4_index = (addr >> 39) & 0x1FF;
        size_t pdpt_index = (addr >> 30) & 0x1FF;
        size_t pd_index = (addr >> 21) & 0x1FF;
        size_t pt_index = (addr >> 12) & 0x1FF;

        uint64_t* pdpt = ensure_table(pml4_table, pml4_index, PTE_WRITE);
        uint64_t* pd = ensure_table(pdpt, pdpt_index, PTE_WRITE);
        uint64_t* pt = ensure_table(pd, pd_index, PTE_WRITE);

        uint64_t entry = pt[pt_index];
        if ((entry & PTE_PRESENT) == 0) {
            continue;
        }

        entry &= ~(PTE_PWT | PTE_PCD);
        entry |= PTE_PAT;
        pt[pt_index] = entry;

        asm volatile("invlpg (%0)" : : "r"(reinterpret_cast<void*>(addr)) : "memory");
    }

    return true;
}

uint64_t paging_cr3() {
    // AP bootstrap callers need the kernel address space, not a process CR3
    // that happened to be active on another CPU.
    return g_kernel_cr3;
}

uint64_t paging_kernel_cr3() {
    return g_kernel_cr3;
}

uint64_t paging_hhdm_offset() {
    return g_hhdm_offset;
}

uint64_t paging_kernel_phys_base() {
    return g_kernel_phys_base;
}

uint64_t paging_kernel_phys_size() {
    return g_kernel_size;
}

void paging_switch_cr3(uint64_t new_cr3) {
    if (new_cr3 == 0) {
        return;
    }
    uint64_t current_cr3 = read_cr3();
    new_cr3 = cr3_context(new_cr3);
    if (cr3_context(current_cr3) == new_cr3) {
        return;
    }
    uint64_t root_phys = new_cr3 & PHYSICAL_ADDRESS_MASK;
    for (size_t i = 0; i < g_address_space_count; ++i) {
        auto* root = g_address_space_roots[i];
        if (root == reinterpret_cast<uint64_t*>(
                        table_phys_to_virt(root_phys))) {
            __atomic_store_n(&g_address_space_activated[i],
                             true,
                             __ATOMIC_RELEASE);
            break;
        }
    }
    uint64_t value = pcid_enabled() ? new_cr3 | CR3_NO_FLUSH : new_cr3;
    asm volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

uint64_t paging_create_address_space() {
    if (pml4_table == nullptr) {
        return 0;
    }
    PagingStructurePage root_page = alloc_paging_structure_page();
    auto* new_root = static_cast<uint64_t*>(root_page.virt);
    if (new_root == nullptr) {
        return 0;
    }
    memset(new_root, 0, PAGE_SIZE);
    sync::IrqLockGuard guard(g_address_space_registry_lock);
    uint16_t pcid = allocate_pcid_locked();
    if (pcid_enabled() && pcid == 0) {
        memory::free_kernel_page(root_page.phys);
        return 0;
    }
    constexpr size_t kKernelStart = PAGE_TABLE_ENTRIES / 2;
    for (size_t i = kKernelStart; i < PAGE_TABLE_ENTRIES; ++i) {
        new_root[i] = pml4_table[i];
    }
    size_t registry_slot = MAX_ADDRESS_SPACES;
    for (size_t i = 0; i < g_address_space_count; ++i) {
        if (g_address_space_roots[i] == nullptr) {
            registry_slot = i;
            break;
        }
    }
    if (registry_slot == MAX_ADDRESS_SPACES) {
        if (g_address_space_count >= MAX_ADDRESS_SPACES) {
            release_pcid_locked(pcid);
            memory::free_kernel_page(root_page.phys);
            return 0;
        }
        registry_slot = g_address_space_count++;
    }
    g_address_space_roots[registry_slot] = new_root;
    g_address_space_activated[registry_slot] = false;
    return root_page.phys | pcid;
}

bool paging_address_space_has_run(uint64_t cr3) {
    if (cr3 == 0 || cr3_context(cr3) == cr3_context(g_kernel_cr3)) {
        return true;
    }
    uint64_t root_phys = cr3 & PHYSICAL_ADDRESS_MASK;
    for (size_t i = 0; i < g_address_space_count; ++i) {
        auto* root = g_address_space_roots[i];
        if (root == reinterpret_cast<uint64_t*>(
                        table_phys_to_virt(root_phys))) {
            return __atomic_load_n(&g_address_space_activated[i],
                                   __ATOMIC_ACQUIRE);
        }
    }
    return true;
}

void paging_destroy_address_space(uint64_t cr3) {
    if (cr3 == 0 || cr3 == g_kernel_cr3) {
        return;
    }

    uint16_t pcid = static_cast<uint16_t>(cr3 & PCID_MASK);
    uint64_t root_phys = cr3 & PHYSICAL_ADDRESS_MASK;
    auto* root = reinterpret_cast<uint64_t*>(table_phys_to_virt(root_phys));
    if (root == nullptr) {
        return;
    }

    // Purge the tag on every CPU before its page tables or PCID can be reused.
    if (!shootdown_all_cpus(TlbShootdownKind::Context, cr3)) {
        // Leaking an unreachable address space is safer than freeing page
        // tables or reusing a PCID that another CPU may still have cached.
        return;
    }

    // Stop publishing the root before freeing any of its tables, so a
    // concurrent kernel-mapping update cannot write through a stale pointer.
    {
        sync::IrqLockGuard guard(g_address_space_registry_lock);
        for (size_t i = 0; i < g_address_space_count; ++i) {
            if (g_address_space_roots[i] == root) {
                g_address_space_roots[i] = nullptr;
                g_address_space_activated[i] = false;
                break;
            }
        }
    }

    constexpr size_t kUserPml4Entries = PAGE_TABLE_ENTRIES / 2;
    for (size_t i = 0; i < kUserPml4Entries; ++i) {
        uint64_t entry = root[i];
        if ((entry & PTE_PRESENT) == 0) {
            continue;
        }
        if ((entry & PTE_LARGE) != 0) {
            root[i] = 0;
            continue;
        }

        uint64_t child_phys = entry & PHYSICAL_ADDRESS_MASK;
        destroy_table_level(child_phys, 3);
        memory::free_kernel_page(child_phys);
        root[i] = 0;
    }

    memory::free_kernel_page(root_phys);
    {
        sync::IrqLockGuard guard(g_address_space_registry_lock);
        release_pcid_locked(pcid);
    }
}

bool paging_map_page_cr3(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (cr3 == 0) {
        return false;
    }
    uint64_t root_phys = cr3 & ~PAGE_MASK;
    auto* root = reinterpret_cast<uint64_t*>(table_phys_to_virt(root_phys));
    bool replaced_present = map_page_with_root(root, virt, phys, flags);
    // A not-present leaf cannot have a cached translation.  Avoid a global,
    // synchronous IPI round trip for every page while constructing a new
    // process or shared-memory mapping.  Replacements still require an
    // immediate shootdown.
    return !replaced_present ||
           shootdown_all_cpus(TlbShootdownKind::Page, cr3, virt);
}

bool paging_unmap_page_cr3(uint64_t cr3, uint64_t virt, uint64_t& phys_out) {
    phys_out = 0;
    if (cr3 == 0) {
        return false;
    }
    uint64_t root_phys = cr3 & ~PAGE_MASK;
    auto* root = reinterpret_cast<uint64_t*>(table_phys_to_virt(root_phys));
    size_t pml4_index = (virt >> 39) & 0x1FF;
    size_t pdpt_index = (virt >> 30) & 0x1FF;
    size_t pd_index = (virt >> 21) & 0x1FF;
    size_t pt_index = (virt >> 12) & 0x1FF;

    uint64_t pml4_entry = root[pml4_index];
    if ((pml4_entry & PTE_PRESENT) == 0) {
        return false;
    }
    uint64_t pdpt_phys = pml4_entry & PHYSICAL_ADDRESS_MASK;
    uint64_t* pdpt = table_from_entry(pml4_entry);

    uint64_t pdpt_entry = pdpt[pdpt_index];
    if ((pdpt_entry & PTE_PRESENT) == 0) {
        return false;
    }
    if ((pdpt_entry & PTE_LARGE) != 0) {
        return false;
    }
    uint64_t pd_phys = pdpt_entry & PHYSICAL_ADDRESS_MASK;
    uint64_t* pd = table_from_entry(pdpt_entry);

    uint64_t pd_entry = pd[pd_index];
    if ((pd_entry & PTE_PRESENT) == 0 || (pd_entry & PTE_LARGE) != 0) {
        return false;
    }
    uint64_t pt_phys = pd_entry & PHYSICAL_ADDRESS_MASK;
    uint64_t* pt = table_from_entry(pd_entry);

    uint64_t pt_entry = pt[pt_index];
    if ((pt_entry & PTE_PRESENT) == 0) {
        return false;
    }
    phys_out = pt_entry & PHYSICAL_ADDRESS_MASK;
    pt[pt_index] = 0;
    bool flushed =
        shootdown_all_cpus(TlbShootdownKind::Page, cr3, virt);
    if (pml4_index < PAGE_TABLE_ENTRIES / 2) {
        if (table_empty(pt)) {
            pd[pd_index] = 0;
            memory::free_kernel_page(pt_phys);
            if (table_empty(pd)) {
                pdpt[pdpt_index] = 0;
                memory::free_kernel_page(pd_phys);
                if (table_empty(pdpt)) {
                    root[pml4_index] = 0;
                    memory::free_kernel_page(pdpt_phys);
                }
            }
        }
    }
    return flushed;
}

bool paging_resolve_cr3(uint64_t cr3, uint64_t virt, uint64_t& phys_out) {
    phys_out = 0;
    if (cr3 == 0) {
        return false;
    }
    uint64_t root_phys = cr3 & ~PAGE_MASK;
    auto* root = reinterpret_cast<uint64_t*>(table_phys_to_virt(root_phys));
    size_t pml4_index = (virt >> 39) & 0x1FF;
    size_t pdpt_index = (virt >> 30) & 0x1FF;
    size_t pd_index = (virt >> 21) & 0x1FF;
    size_t pt_index = (virt >> 12) & 0x1FF;

    uint64_t pml4_entry = root[pml4_index];
    if ((pml4_entry & PTE_PRESENT) == 0) {
        return false;
    }
    uint64_t* pdpt = table_from_entry(pml4_entry);

    uint64_t pdpt_entry = pdpt[pdpt_index];
    if ((pdpt_entry & PTE_PRESENT) == 0) {
        return false;
    }
    if ((pdpt_entry & PTE_LARGE) != 0) {
        uint64_t base =
            pdpt_entry & (PHYSICAL_ADDRESS_MASK & ~PAGE_HUGE_MASK);
        phys_out = base + (virt & PAGE_HUGE_MASK);
        return true;
    }
    uint64_t* pd = table_from_entry(pdpt_entry);

    uint64_t pd_entry = pd[pd_index];
    if ((pd_entry & PTE_PRESENT) == 0) {
        return false;
    }
    if ((pd_entry & PTE_LARGE) != 0) {
        uint64_t base =
            pd_entry & (PHYSICAL_ADDRESS_MASK & ~PAGE_LARGE_MASK);
        phys_out = base + (virt & PAGE_LARGE_MASK);
        return true;
    }
    uint64_t* pt = table_from_entry(pd_entry);

    uint64_t pt_entry = pt[pt_index];
    if ((pt_entry & PTE_PRESENT) == 0) {
        return false;
    }
    phys_out = (pt_entry & PHYSICAL_ADDRESS_MASK) | (virt & PAGE_MASK);
    return true;
}

bool paging_flags_cr3(uint64_t cr3, uint64_t virt, uint64_t& flags_out) {
    flags_out = 0;
    if (cr3 == 0) {
        return false;
    }
    uint64_t root_phys = cr3 & ~PAGE_MASK;
    auto* root = reinterpret_cast<uint64_t*>(table_phys_to_virt(root_phys));
    size_t pml4_index = (virt >> 39) & 0x1FF;
    size_t pdpt_index = (virt >> 30) & 0x1FF;
    size_t pd_index = (virt >> 21) & 0x1FF;
    size_t pt_index = (virt >> 12) & 0x1FF;

    uint64_t pml4_entry = root[pml4_index];
    if ((pml4_entry & PTE_PRESENT) == 0) {
        return false;
    }
    uint64_t effective = pml4_entry;
    uint64_t* pdpt = table_from_entry(pml4_entry);

    uint64_t pdpt_entry = pdpt[pdpt_index];
    if ((pdpt_entry & PTE_PRESENT) == 0) {
        return false;
    }
    effective &= pdpt_entry;
    if ((pdpt_entry & PTE_LARGE) != 0) {
        flags_out = (effective & PAGE_MASK) | (pdpt_entry & PTE_NX);
        return true;
    }
    uint64_t* pd = table_from_entry(pdpt_entry);

    uint64_t pd_entry = pd[pd_index];
    if ((pd_entry & PTE_PRESENT) == 0) {
        return false;
    }
    effective &= pd_entry;
    if ((pd_entry & PTE_LARGE) != 0) {
        flags_out = (effective & PAGE_MASK) | (pd_entry & PTE_NX);
        return true;
    }
    uint64_t* pt = table_from_entry(pd_entry);

    uint64_t pt_entry = pt[pt_index];
    if ((pt_entry & PTE_PRESENT) == 0) {
        return false;
    }
    effective &= pt_entry;
    // Effective hardware permissions are the intersection of all levels,
    // while software ownership is stored only on the leaf entry.
    flags_out = (effective & PAGE_MASK) |
                (pt_entry & (PTE_NX | PTE_MANAGED));
    return true;
}

bool paging_set_writable_cr3_impl(uint64_t cr3,
                                  uint64_t virt,
                                  bool writable,
                                  bool flush) {
    if (cr3 == 0) {
        return false;
    }
    uint64_t root_phys = cr3 & ~PAGE_MASK;
    auto* root = reinterpret_cast<uint64_t*>(table_phys_to_virt(root_phys));
    size_t pml4_index = (virt >> 39) & 0x1FF;
    size_t pdpt_index = (virt >> 30) & 0x1FF;
    size_t pd_index = (virt >> 21) & 0x1FF;
    size_t pt_index = (virt >> 12) & 0x1FF;

    uint64_t pml4_entry = root[pml4_index];
    if ((pml4_entry & PTE_PRESENT) == 0) {
        return false;
    }
    uint64_t* pdpt = table_from_entry(pml4_entry);

    uint64_t pdpt_entry = pdpt[pdpt_index];
    if ((pdpt_entry & PTE_PRESENT) == 0 ||
        (pdpt_entry & PTE_LARGE) != 0) {
        return false;
    }
    uint64_t* pd = table_from_entry(pdpt_entry);

    uint64_t pd_entry = pd[pd_index];
    if ((pd_entry & PTE_PRESENT) == 0 ||
        (pd_entry & PTE_LARGE) != 0) {
        return false;
    }
    uint64_t* pt = table_from_entry(pd_entry);

    uint64_t entry = pt[pt_index];
    if ((entry & PTE_PRESENT) == 0) {
        return false;
    }
    if (writable) {
        entry |= PTE_WRITE;
    } else {
        entry &= ~PTE_WRITE;
    }
    pt[pt_index] = entry;
    return !flush || shootdown_all_cpus(TlbShootdownKind::Page, cr3, virt);
}

bool paging_set_writable_cr3(uint64_t cr3, uint64_t virt, bool writable) {
    return paging_set_writable_cr3_impl(cr3, virt, writable, true);
}

bool paging_set_writable_cr3_deferred(uint64_t cr3,
                                      uint64_t virt,
                                      bool writable) {
    return paging_set_writable_cr3_impl(cr3, virt, writable, false);
}

bool paging_set_executable_cr3_impl(uint64_t cr3,
                                    uint64_t virt,
                                    bool executable,
                                    bool flush) {
    if (cr3 == 0) {
        return false;
    }
    uint64_t root_phys = cr3 & ~PAGE_MASK;
    auto* root = reinterpret_cast<uint64_t*>(table_phys_to_virt(root_phys));
    size_t pml4_index = (virt >> 39) & 0x1FF;
    size_t pdpt_index = (virt >> 30) & 0x1FF;
    size_t pd_index = (virt >> 21) & 0x1FF;
    size_t pt_index = (virt >> 12) & 0x1FF;

    uint64_t pml4_entry = root[pml4_index];
    if ((pml4_entry & PTE_PRESENT) == 0) {
        return false;
    }
    uint64_t* pdpt = table_from_entry(pml4_entry);
    uint64_t pdpt_entry = pdpt[pdpt_index];
    if ((pdpt_entry & PTE_PRESENT) == 0 ||
        (pdpt_entry & PTE_LARGE) != 0) {
        return false;
    }
    uint64_t* pd = table_from_entry(pdpt_entry);
    uint64_t pd_entry = pd[pd_index];
    if ((pd_entry & PTE_PRESENT) == 0 ||
        (pd_entry & PTE_LARGE) != 0) {
        return false;
    }
    uint64_t* pt = table_from_entry(pd_entry);
    uint64_t entry = pt[pt_index];
    if ((entry & PTE_PRESENT) == 0) {
        return false;
    }
    if (executable) {
        entry &= ~PTE_NX;
    } else {
        entry |= PTE_NX;
    }
    pt[pt_index] = entry;
    return !flush || shootdown_all_cpus(TlbShootdownKind::Page, cr3, virt);
}

bool paging_set_executable_cr3(uint64_t cr3,
                               uint64_t virt,
                               bool executable) {
    return paging_set_executable_cr3_impl(cr3, virt, executable, true);
}

bool paging_set_executable_cr3_deferred(uint64_t cr3,
                                        uint64_t virt,
                                        bool executable) {
    return paging_set_executable_cr3_impl(cr3, virt, executable, false);
}

bool paging_flush_tlb_context(uint64_t cr3) {
    return cr3 != 0 &&
           shootdown_all_cpus(TlbShootdownKind::Context, cr3);
}

bool paging_flush_tlb_all_cpus() {
    return shootdown_all_cpus(TlbShootdownKind::All);
}

void* paging_phys_to_virt(uint64_t phys) {
    return table_phys_to_virt(phys);
}
