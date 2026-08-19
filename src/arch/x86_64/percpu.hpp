#pragma once

#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"
#include "arch/x86_64/tss.hpp"

namespace process {
struct Task;
}  // namespace process

namespace percpu {

constexpr size_t kMaxCpus = 16;
constexpr size_t kBootstrapStackSize = 0x4000;

struct Cpu {
    uint32_t lapic_id;
    uint32_t processor_id;
    uint32_t index;
    bool registered;
    uint8_t reserved0[3];
    uint64_t syscall_user_rsp;
    TSS tss;
    alignas(16) uint8_t tss_stack[65536];
    alignas(16) uint8_t gdt_area[8 * 8];
    alignas(16) uint8_t bootstrap_stack[kBootstrapStackSize];
    process::Task* current_task;
    uint64_t user_ticks;
    uint64_t kernel_ticks;
    uint64_t idle_ticks;
    uint64_t irq_ticks;
    uint32_t kernel_fpu_depth;
    uint32_t kernel_fpu_reserved;
    uint64_t kernel_fpu_rflags;
    process::Task* kernel_fpu_task;
};

static_assert(offsetof(Cpu, syscall_user_rsp) == 16,
              "Cpu syscall_user_rsp offset changed");
static_assert(offsetof(Cpu, tss) + offsetof(TSS, rsp0) == 28,
              "Cpu tss.rsp0 offset changed");

void init_bsp(uint32_t lapic_id, uint32_t processor_id);
Cpu* register_cpu(uint32_t lapic_id, uint32_t processor_id);
Cpu* cpu_from_index(size_t index);
Cpu* current_cpu();
Cpu* find_by_lapic(uint32_t lapic_id);
size_t cpu_count();
void set_current_cpu(Cpu* cpu);
void prepare_user_entry();
void setup_cpu_tss(Cpu& cpu);
void setup_cpu_gdt(Cpu& cpu);
void set_current_task(process::Task* proc);
process::Task* get_current_task();
void record_tick(bool user_mode, bool has_task);
void record_irq();
size_t usage_snapshot(descriptor_defs::CpuUsage* out, size_t max_entries);

}  // namespace percpu
