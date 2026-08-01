#include "userspace.hpp"

#include "arch/x86_64/cpu_features.hpp"
#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/tss.hpp"
#include "process.hpp"

namespace {

extern "C" [[noreturn]] void userspace_enter_frame(
    const syscall::SyscallFrame* frame);

[[noreturn]] void transfer_to_userspace(uint64_t entry, uint64_t user_stack) {
    constexpr uint64_t kUserFlags = 0x202;
    asm volatile(
        "pushq %[ds]\n"
        "pushq %[stack]\n"
        "pushq %[flags]\n"
        "pushq %[cs]\n"
        "pushq %[entry]\n"
        "iretq\n"
        :
        : [ds] "r"(static_cast<uint64_t>(USER_DS)),
          [stack] "r"(user_stack),
          [flags] "r"(kUserFlags),
          [cs] "r"(static_cast<uint64_t>(USER_CS)),
          [entry] "r"(entry)
        : "memory");

    __builtin_unreachable();
}

}  // namespace

namespace userspace {

[[noreturn]] void enter_task(process::Task& proc) {
    set_rsp0(proc.kernel_stack_top);
    cpu::restore_fpu_state(proc.fpu_state);
    if (proc.has_context) {
        userspace_enter_frame(&proc.context);
    }
    uint64_t entry = proc.user_ip;
    uint64_t user_stack = proc.user_sp & ~0xFull;
    transfer_to_userspace(entry, user_stack);
}

}  // namespace userspace
