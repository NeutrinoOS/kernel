#include "percpu.hpp"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/tss.hpp"

namespace {

constexpr uint32_t kMsrGsBase = 0xC0000101;
constexpr uint32_t kMsrKernelGsBase = 0xC0000102;

percpu::Cpu g_cpus[percpu::kMaxCpus];
size_t g_cpu_count = 0;

inline void write_msr(uint32_t msr, uint64_t value) {
    uint32_t low = static_cast<uint32_t>(value & 0xFFFFFFFF);
    uint32_t high = static_cast<uint32_t>(value >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

inline uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | low;
}

}  // namespace

namespace percpu {

void setup_cpu_tss(Cpu& cpu) {
    init_tss_for_cpu(cpu.tss, cpu.tss_stack, sizeof(cpu.tss_stack));
}

void setup_cpu_gdt(Cpu& cpu) {
    gdt_install_for_cpu(&cpu.tss, cpu.gdt_area, sizeof(cpu.gdt_area));
}

Cpu* register_cpu(uint32_t lapic_id, uint32_t processor_id) {
    if (g_cpu_count >= kMaxCpus) {
        return nullptr;
    }
    Cpu& cpu = g_cpus[g_cpu_count++];
    cpu.lapic_id = lapic_id;
    cpu.processor_id = processor_id;
    cpu.index = static_cast<uint32_t>(g_cpu_count - 1);
    cpu.registered = false;
    cpu.reserved0[0] = 0;
    cpu.reserved0[1] = 0;
    cpu.reserved0[2] = 0;
    cpu.syscall_user_rsp = 0;
    cpu.current_task = nullptr;
    cpu.user_ticks = 0;
    cpu.kernel_ticks = 0;
    cpu.idle_ticks = 0;
    cpu.irq_ticks = 0;
    cpu.kernel_fpu_depth = 0;
    cpu.kernel_fpu_reserved = 0;
    cpu.kernel_fpu_rflags = 0;
    cpu.kernel_fpu_task = nullptr;
    return &cpu;
}

Cpu* cpu_from_index(size_t index) {
    if (index >= g_cpu_count) {
        return nullptr;
    }
    return &g_cpus[index];
}

Cpu* current_cpu() {
    uint64_t base = read_msr(kMsrKernelGsBase);
    if (base == 0) {
        base = read_msr(kMsrGsBase);
    }
    return reinterpret_cast<Cpu*>(base);
}

Cpu* find_by_lapic(uint32_t lapic_id) {
    for (size_t i = 0; i < g_cpu_count; ++i) {
        if (g_cpus[i].lapic_id == lapic_id) {
            return &g_cpus[i];
        }
    }
    return nullptr;
}

size_t cpu_count() {
    return g_cpu_count;
}

void set_current_cpu(Cpu* cpu) {
    if (cpu != nullptr) {
        write_msr(kMsrGsBase, 0);
        write_msr(kMsrKernelGsBase, reinterpret_cast<uint64_t>(cpu));
    }
}

void init_bsp(uint32_t lapic_id, uint32_t processor_id) {
    Cpu* cpu = register_cpu(lapic_id, processor_id);
    if (cpu == nullptr) {
        return;
    }
    setup_cpu_tss(*cpu);
    setup_cpu_gdt(*cpu);
    set_current_cpu(cpu);
}

void set_current_task(process::Task* proc) {
    Cpu* cpu = current_cpu();
    if (cpu != nullptr) {
        cpu->current_task = proc;
    }
}

process::Task* get_current_task() {
    Cpu* cpu = current_cpu();
    return cpu ? cpu->current_task : nullptr;
}

void record_tick(bool user_mode, bool has_task) {
    Cpu* cpu = current_cpu();
    if (cpu == nullptr) {
        return;
    }
    if (!has_task) {
        ++cpu->idle_ticks;
        return;
    }
    if (user_mode) {
        ++cpu->user_ticks;
    } else {
        ++cpu->kernel_ticks;
    }
}

void record_irq() {
    Cpu* cpu = current_cpu();
    if (cpu == nullptr) {
        return;
    }
    ++cpu->irq_ticks;
}

size_t usage_snapshot(descriptor_defs::CpuUsage* out, size_t max_entries) {
    if (out == nullptr || max_entries == 0) {
        return 0;
    }
    size_t count = cpu_count();
    if (count > max_entries) {
        count = max_entries;
    }
    for (size_t i = 0; i < count; ++i) {
        Cpu* cpu = cpu_from_index(i);
        if (cpu == nullptr) {
            continue;
        }
        out[i].cpu_index = static_cast<uint32_t>(i);
        out[i].reserved = 0;
        out[i].user_ticks = cpu->user_ticks;
        out[i].kernel_ticks = cpu->kernel_ticks;
        out[i].idle_ticks = cpu->idle_ticks;
        out[i].irq_ticks = cpu->irq_ticks;
    }
    return count;
}

}  // namespace percpu
