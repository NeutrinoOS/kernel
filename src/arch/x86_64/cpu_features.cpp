#include "arch/x86_64/cpu_features.hpp"

#include <stddef.h>
#include <stdint.h>

#include "drivers/log/logging.hpp"
#include "lib/mem.hpp"
#include "arch/x86_64/percpu.hpp"
#include "kernel/process.hpp"

namespace cpu {
namespace {

struct CpuidResult {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
};

CpuidResult cpuid(uint32_t leaf, uint32_t subleaf = 0) {
    CpuidResult result{};
    asm volatile("cpuid"
                 : "=a"(result.eax), "=b"(result.ebx),
                   "=c"(result.ecx), "=d"(result.edx)
                 : "a"(leaf), "c"(subleaf));
    return result;
}

uint64_t read_cr0() {
    uint64_t value = 0;
    asm volatile("mov %%cr0, %0" : "=r"(value));
    return value;
}

void write_cr0(uint64_t value) {
    asm volatile("mov %0, %%cr0" : : "r"(value) : "memory");
}

uint64_t read_cr4() {
    uint64_t value = 0;
    asm volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

void write_cr4(uint64_t value) {
    asm volatile("mov %0, %%cr4" : : "r"(value) : "memory");
}

constexpr uint64_t kCr0Mp = 1ull << 1;
constexpr uint64_t kCr0Em = 1ull << 2;
constexpr uint64_t kCr0Ts = 1ull << 3;
constexpr uint64_t kCr0Ne = 1ull << 5;
constexpr uint64_t kCr4Pge = 1ull << 7;
constexpr uint64_t kCr4Osfxsr = 1ull << 9;
constexpr uint64_t kCr4Osxmmexcpt = 1ull << 10;
constexpr uint64_t kCr4Pcide = 1ull << 17;

FeatureState g_features{};
alignas(kFpuStateAlign) uint8_t g_initial_fpu_state[kFpuStateSize]{};
bool g_initial_fpu_state_ready = false;

FeatureState detect_baseline_features() {
    FeatureState state{};
    CpuidResult max_basic = cpuid(0);
    if (max_basic.eax < 1) {
        return state;
    }

    CpuidResult basic = cpuid(1);
    state.mmx = (basic.edx & (1u << 23)) != 0;
    state.sse = (basic.edx & (1u << 25)) != 0;
    state.sse2 = (basic.edx & (1u << 26)) != 0;
    state.pge = (basic.edx & (1u << 13)) != 0;
    state.pcid = (basic.ecx & (1u << 17)) != 0;
    return state;
}

void enable_x87_mmx_sse(const FeatureState& features) {
    uint64_t cr0 = read_cr0();
    cr0 |= kCr0Mp | kCr0Ne;
    cr0 &= ~(kCr0Em | kCr0Ts);
    write_cr0(cr0);

    uint64_t cr4 = read_cr4();
    cr4 |= kCr4Osfxsr | kCr4Osxmmexcpt;
    if (features.pge) {
        cr4 |= kCr4Pge;
    }
    write_cr4(cr4);

    uint32_t mxcsr = 0x1F80;
    asm volatile("fninit\n"
                 "ldmxcsr %0"
                 :
                 : "m"(mxcsr)
                 : "memory");
}

bool enable_pcid_if_available(const FeatureState& features) {
    if (!features.pcid) {
        return false;
    }
    uint64_t cr3 = 0;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    if ((cr3 & 0xfffull) != 0) {
        return false;
    }
    write_cr4(read_cr4() | kCr4Pcide);
    return (read_cr4() & kCr4Pcide) != 0;
}

bool is_aligned(const void* ptr, size_t alignment) {
    return (reinterpret_cast<uintptr_t>(ptr) & (alignment - 1)) == 0;
}

uint64_t read_rflags() {
    uint64_t value = 0;
    asm volatile("pushfq\n"
                 "pop %0"
                 : "=r"(value)
                 :
                 : "memory");
    return value;
}

void disable_interrupts() {
    asm volatile("cli" ::: "memory");
}

void restore_interrupt_flag(uint64_t rflags) {
    if ((rflags & (1ull << 9)) != 0) {
        asm volatile("sti" ::: "memory");
    }
}

void load_default_fpu_state() {
    if (g_initial_fpu_state_ready) {
        restore_fpu_state(g_initial_fpu_state);
        return;
    }

    uint32_t mxcsr = 0x1F80;
    asm volatile("fninit\n"
                 "ldmxcsr %0"
                 :
                 : "m"(mxcsr)
                 : "memory");
}

}  // namespace

const FeatureState& feature_state() {
    return g_features;
}

bool init_boot_features() {
    g_features = detect_baseline_features();
    if (!g_features.mmx || !g_features.sse || !g_features.sse2) {
        log_message(LogLevel::Error,
                    "CPU: missing required x86-64 SIMD baseline "
                    "(MMX=%u SSE=%u SSE2=%u)",
                    g_features.mmx ? 1u : 0u,
                    g_features.sse ? 1u : 0u,
                    g_features.sse2 ? 1u : 0u);
        return false;
    }

    enable_x87_mmx_sse(g_features);
    g_features.pge = (read_cr4() & kCr4Pge) != 0;
    g_features.pcid = enable_pcid_if_available(g_features);
    save_fpu_state(g_initial_fpu_state);
    g_initial_fpu_state_ready = true;
    log_message(LogLevel::Info,
                "CPU: enabled x87/MMX/SSE/SSE2 support (PGE=%u PCID=%u)",
                g_features.pge ? 1u : 0u,
                g_features.pcid ? 1u : 0u);
    return true;
}

void init_current_cpu_features() {
    FeatureState local_features = detect_baseline_features();
    if (!local_features.mmx || !local_features.sse ||
        !local_features.sse2) {
        return;
    }
    enable_x87_mmx_sse(local_features);
    (void)enable_pcid_if_available(local_features);
}

void init_fpu_state(void* state) {
    if (state == nullptr || !is_aligned(state, kFpuStateAlign)) {
        return;
    }
    if (g_initial_fpu_state_ready) {
        memcpy(state, g_initial_fpu_state, kFpuStateSize);
        return;
    }

    memset(state, 0, kFpuStateSize);
    auto* bytes = static_cast<uint8_t*>(state);
    bytes[0] = 0x7F;
    bytes[1] = 0x03;
    bytes[24] = 0x80;
    bytes[25] = 0x1F;
}

void save_fpu_state(void* state) {
    if (state == nullptr || !is_aligned(state, kFpuStateAlign)) {
        return;
    }
    asm volatile("fxsave64 %0"
                 : "=m"(*static_cast<uint8_t (*)[kFpuStateSize]>(state))
                 :
                 : "memory");
}

void restore_fpu_state(const void* state) {
    if (state == nullptr || !is_aligned(state, kFpuStateAlign)) {
        return;
    }
    asm volatile("fxrstor64 %0"
                 :
                 : "m"(*static_cast<const uint8_t (*)[kFpuStateSize]>(state))
                 : "memory");
}

bool kernel_fpu_begin() {
    if (!g_features.mmx || !g_features.sse || !g_features.sse2) {
        return false;
    }

    percpu::Cpu* current_cpu = percpu::current_cpu();
    if (current_cpu == nullptr) {
        return false;
    }

    uint64_t rflags = read_rflags();
    disable_interrupts();

    if (current_cpu->kernel_fpu_depth == 0) {
        current_cpu->kernel_fpu_rflags = rflags;
        current_cpu->kernel_fpu_task = current_cpu->current_task;
        if (current_cpu->kernel_fpu_task != nullptr) {
            save_fpu_state(current_cpu->kernel_fpu_task->fpu_state);
        }
        load_default_fpu_state();
    }

    ++current_cpu->kernel_fpu_depth;
    return true;
}

void kernel_fpu_end() {
    percpu::Cpu* current_cpu = percpu::current_cpu();
    if (current_cpu == nullptr || current_cpu->kernel_fpu_depth == 0) {
        return;
    }

    --current_cpu->kernel_fpu_depth;
    if (current_cpu->kernel_fpu_depth != 0) {
        return;
    }

    process::Task* proc = current_cpu->kernel_fpu_task;
    if (proc != nullptr) {
        restore_fpu_state(proc->fpu_state);
    } else {
        load_default_fpu_state();
    }
    current_cpu->kernel_fpu_task = nullptr;

    restore_interrupt_flag(current_cpu->kernel_fpu_rflags);
    current_cpu->kernel_fpu_rflags = 0;
}

}  // namespace cpu
