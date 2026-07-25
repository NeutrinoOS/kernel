#pragma once

#include <stddef.h>
#include <stdint.h>

namespace memory {

void init();
bool kernel_allocator_ready();

uint64_t alloc_kernel_block_pages(size_t pages);
uint64_t alloc_kernel_page();
void free_kernel_block(uint64_t phys);
void free_kernel_page(uint64_t phys);

// General-purpose, aligned kernel heap backed by the kernel buddy allocator.
void* alloc_kernel(size_t bytes, size_t alignment = alignof(uint64_t));
void free_kernel(void* ptr);

uint64_t alloc_user_page();
void free_user_page(uint64_t phys);

uint64_t kernel_pool_base();
uint64_t kernel_pool_size();
size_t kernel_free_pages();
size_t user_free_pages();

}  // namespace memory
