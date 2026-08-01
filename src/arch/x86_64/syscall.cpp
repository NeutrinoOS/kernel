#include "arch/x86_64/syscall.hpp"

#include <stdint.h>

#include "../../kernel/scheduler.hpp"
#include "../../kernel/descriptor.hpp"
#include "../../kernel/process.hpp"
#include "../../kernel/vm.hpp"
#include "../../drivers/log/logging.hpp"
#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/percpu.hpp"
#include "arch/x86_64/syscall_table.hpp"

extern "C" void syscall_entry();

namespace syscall {

namespace {

constexpr uint32_t MSR_EFER  = 0xC0000080;
constexpr uint32_t MSR_STAR  = 0xC0000081;
constexpr uint32_t MSR_LSTAR = 0xC0000082;
constexpr uint32_t MSR_FMASK = 0xC0000084;

constexpr uint64_t EFER_SCE = 1ull << 0;
constexpr uint64_t RFLAGS_TF = 1ull << 8;
constexpr uint64_t RFLAGS_IF = 1ull << 9;
constexpr uint64_t RFLAGS_DF = 1ull << 10;
constexpr uint64_t RFLAGS_NT = 1ull << 14;
constexpr uint64_t RFLAGS_AC = 1ull << 18;
constexpr uint64_t RFLAGS_SYSCALL_MASK =
    RFLAGS_TF | RFLAGS_IF | RFLAGS_DF | RFLAGS_NT | RFLAGS_AC;

constexpr uint64_t RFLAGS_CF = 1ull << 0;
constexpr uint64_t RFLAGS_FIXED = 1ull << 1;
constexpr uint64_t RFLAGS_PF = 1ull << 2;
constexpr uint64_t RFLAGS_AF = 1ull << 4;
constexpr uint64_t RFLAGS_ZF = 1ull << 6;
constexpr uint64_t RFLAGS_SF = 1ull << 7;
constexpr uint64_t RFLAGS_OF = 1ull << 11;
constexpr uint64_t RFLAGS_RF = 1ull << 16;
constexpr uint64_t RFLAGS_ID = 1ull << 21;
constexpr uint64_t RFLAGS_USER_ALLOWED =
    RFLAGS_CF | RFLAGS_FIXED | RFLAGS_PF | RFLAGS_AF | RFLAGS_ZF |
    RFLAGS_SF | RFLAGS_TF | RFLAGS_IF | RFLAGS_DF | RFLAGS_OF |
    RFLAGS_RF | RFLAGS_AC | RFLAGS_ID;

static_assert(sizeof(SyscallFrame) == 18 * sizeof(uint64_t),
              "SyscallFrame layout mismatch");

inline uint64_t read_msr(uint32_t msr) {
    uint32_t low = 0;
    uint32_t high = 0;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | low;
}

inline void write_msr(uint32_t msr, uint64_t value) {
    uint32_t low = static_cast<uint32_t>(value & 0xFFFFFFFFu);
    uint32_t high = static_cast<uint32_t>(value >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

}  // namespace

bool valid_user_return_state(uint64_t rip, uint64_t rsp) {
    bool valid_rip = vm::is_user_range(rip, 1);
    bool valid_rsp = rsp >= vm::kUserAddressSpaceBase &&
                     rsp <= vm::kUserAddressSpaceTop;
    return valid_rip && valid_rsp;
}

uint64_t sanitize_user_rflags(uint64_t rflags) {
    return (rflags & RFLAGS_USER_ALLOWED) | RFLAGS_FIXED | RFLAGS_IF;
}

extern "C" void syscall_dispatch(SyscallFrame* frame) {
    if (frame == nullptr) {
        return;
    }

    Result res = handle_syscall(*frame);

    switch (res) {
        case Result::Continue:
            break;
        case Result::Reschedule:
            scheduler::reschedule(*frame);
            break;
        case Result::Unschedule: {
            process::Task* proc = process::current();
            if (proc != nullptr) {
                process::terminate(
                    *proc,
                    static_cast<uint16_t>(frame->rax & 0xFFFFu));
            }
            scheduler::reschedule(*frame);
            break;
        }
    }

    if (!valid_user_return_state(frame->user_rip, frame->user_rsp)) {
        process::Task* proc = process::current();
        if (proc != nullptr) {
            process::terminate(*proc, 0x800Du);
            scheduler::reschedule(*frame);
        }
    }
    frame->user_rflags = sanitize_user_rflags(frame->user_rflags);
}

extern "C" uint64_t syscall_kernel_stack_top() {
    process::Task* proc = process::current();
    if (proc != nullptr && proc->kernel_stack_top != 0) {
        return proc->kernel_stack_top;
    }
    percpu::Cpu* cpu = percpu::current_cpu();
    if (cpu != nullptr) {
        return cpu->tss.rsp0;
    }
    return 0;
}

void init() {
    uint64_t star_value =
        (static_cast<uint64_t>(USER_CS) << 48) |
        (static_cast<uint64_t>(KERNEL_CS) << 32);
    write_msr(MSR_STAR, star_value);

    write_msr(MSR_LSTAR, reinterpret_cast<uint64_t>(&syscall_entry));
    write_msr(MSR_FMASK, RFLAGS_SYSCALL_MASK);

    uint64_t efer = read_msr(MSR_EFER);
    if ((efer & EFER_SCE) == 0) {
        write_msr(MSR_EFER, efer | EFER_SCE);
    }
}

}  // namespace syscall
