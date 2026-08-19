#pragma once
#include <stdint.h>

constexpr uint64_t PAGE_FLAG_WRITE  = 1ull << 1;
constexpr uint64_t PAGE_FLAG_USER   = 1ull << 2;
constexpr uint64_t PAGE_FLAG_WRITE_THROUGH = 1ull << 3;
constexpr uint64_t PAGE_FLAG_CACHE_DISABLE = 1ull << 4;
constexpr uint64_t PAGE_FLAG_WRITE_COMBINING = 1ull << 7;
constexpr uint64_t PAGE_FLAG_GLOBAL = 1ull << 8;
// Software-owned bit: the kernel allocated this user page and may reclaim it.
constexpr uint64_t PAGE_FLAG_MANAGED = 1ull << 9;
constexpr uint64_t PAGE_FLAG_NO_EXECUTE = 1ull << 63;

void paging_init();
bool paging_finish_smp_bootstrap();
bool paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t paging_virt_to_phys(uint64_t virt);
void* paging_alloc_page();
bool paging_mark_wc(uint64_t virt, uint64_t length);
uint64_t paging_cr3();
uint64_t paging_kernel_cr3();
uint64_t paging_hhdm_offset();
uint64_t paging_kernel_phys_base();
uint64_t paging_kernel_phys_size();
void paging_switch_cr3(uint64_t new_cr3);
uint64_t paging_create_address_space();
bool paging_address_space_has_run(uint64_t cr3);
void paging_destroy_address_space(uint64_t cr3);
bool paging_map_page_cr3(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags);
bool paging_unmap_page_cr3(uint64_t cr3, uint64_t virt, uint64_t& phys_out);
bool paging_resolve_cr3(uint64_t cr3, uint64_t virt, uint64_t& phys_out);
bool paging_flags_cr3(uint64_t cr3, uint64_t virt, uint64_t& flags_out);
bool paging_set_writable_cr3(uint64_t cr3, uint64_t virt, bool writable);
bool paging_set_writable_cr3_deferred(uint64_t cr3,
                                      uint64_t virt,
                                      bool writable);
bool paging_set_executable_cr3(uint64_t cr3, uint64_t virt, bool executable);
bool paging_set_executable_cr3_deferred(uint64_t cr3,
                                        uint64_t virt,
                                        bool executable);
bool paging_flush_tlb_context(uint64_t cr3);
bool paging_flush_tlb_all_cpus();
void* paging_phys_to_virt(uint64_t phys);
bool paging_unmap_page(uint64_t virt, uint64_t& phys_out);
void paging_free_physical(uint64_t phys);
