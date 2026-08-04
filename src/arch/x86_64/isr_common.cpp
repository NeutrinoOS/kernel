#include <stddef.h>
#include <stdint.h>

#include "../../drivers/input/keyboard.hpp"
#include "../../drivers/input/mouse.hpp"
#include "../../drivers/interrupts/ioapic.hpp"
#include "../../drivers/interrupts/pic.hpp"
#include "../../drivers/log/logging.hpp"
#include "../../kernel/error.hpp"
#include "../../kernel/descriptor.hpp"
#include "../../kernel/interrupts.hpp"
#include "../../kernel/process.hpp"
#include "../../kernel/scheduler.hpp"
#include "../../kernel/time.hpp"
#include "../../kernel/vm.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "percpu.hpp"
#include "lapic.hpp"
#include "isr.hpp"
#include "syscall.hpp"

// Interrupt number to name mapping
constexpr const char* exception_names[32] = {
    "DIVIDE_BY_ZERO",
    "DEBUG",
    "NMI",
    "BREAKPOINT",
    "OVERFLOW",
    "BOUND_RANGE_EXCEEDED",
    "INVALID_OPCODE",
    "DEVICE_NOT_AVAILABLE",
    "DOUBLE_FAULT",
    "COPROCESSOR_SEGMENT_OVERRUN",
    "INVALID_TSS",
    "SEGMENT_NOT_PRESENT",
    "STACK_SEGMENT_FAULT",
    "GENERAL_PROTECTION_FAULT",
    "PAGE_FAULT",
    "RESERVED",
    "x87_FLOATING_POINT_EXCEPTION",
    "ALIGNMENT_CHECK",
    "MACHINE_CHECK",
    "SIMD_FLOATING_POINT_EXCEPTION",
    "VIRTUALIZATION_EXCEPTION",
    "CONTROL_PROTECTION_EXCEPTION",
    "RESERVED",
    "RESERVED",
    "RESERVED",
    "RESERVED",
    "RESERVED",
    "RESERVED",
    "RESERVED",
    "RESERVED",
    "RESERVED",
    "RESERVED"
};

namespace {

constexpr uint16_t kExitCodeKernelFaultFlag = 0x8000u;

uint16_t exit_code_from_exception(uint64_t vector) {
    return static_cast<uint16_t>(kExitCodeKernelFaultFlag |
                                 static_cast<uint16_t>(vector & 0x7FFFu));
}

}  // namespace

extern "C" void isr_validate_return(InterruptFrame* regs) {
    if (regs == nullptr || (regs->cs & 0x3) == 0) {
        return;
    }
    if (!syscall::valid_user_return_state(regs->rip, regs->rsp)) {
        process::Task* proc = process::current();
        if (proc != nullptr) {
            process::terminate(*proc, exit_code_from_exception(13));
            scheduler::reschedule_from_interrupt(*regs);
        }
    }
    regs->rflags = syscall::sanitize_user_rflags(regs->rflags);
}

extern "C" void isr_handler(InterruptFrame* regs) {
    if (regs == nullptr) {
        return;
    }

    if (regs->int_no >= 32) {
        uint64_t irq = regs->int_no - 32;
        if (irq == 0) {
            bool user_mode = (regs->cs & 0x3) != 0;
            bool has_task = process::current() != nullptr;
            percpu::record_tick(user_mode, has_task);
            if (has_task) {
                process::record_tick(user_mode);
            }
            timekeeping::tick_pit();
            scheduler::tick(*regs);
            pic::send_eoi(0);
            lapic::eoi();
            return;
        } else if (regs->int_no == 0x40) {
            bool user_mode = (regs->cs & 0x3) != 0;
            bool has_task = process::current() != nullptr;
            percpu::record_tick(user_mode, has_task);
            if (has_task) {
                process::record_tick(user_mode);
            }
            scheduler::tick(*regs);
            lapic::eoi();
            return;
        } else if (regs->int_no == lapic::kSchedulerWakeVector) {
            if ((regs->cs & 0x3) != 0) {
                scheduler::reschedule_from_interrupt(*regs);
            }
            lapic::eoi();
            return;
        }
        percpu::record_irq();
        if (interrupts::dispatch(static_cast<uint8_t>(regs->int_no))) {
            lapic::eoi();
            return;
        }
        if (irq == 1) {
            keyboard::handle_irq();
            if (!ioapic::handles_irq(1)) {
                pic::send_eoi(1);
            }
            lapic::eoi();
            return;
        } else if (irq == 12) {
            mouse::handle_irq();
            if (!ioapic::handles_irq(12)) {
                pic::send_eoi(12);
            }
            lapic::eoi();
            return;
        }
        lapic::eoi();
        return;
    }

    if (regs->int_no == 14 && (regs->cs & 0x3) != 0) {
        uint64_t fault_address = 0;
        asm volatile("mov %%cr2, %0" : "=r"(fault_address));
        process::Task* proc = process::current();
        const bool write = (regs->err_code & (1u << 1)) != 0;
        const bool execute = (regs->err_code & (1u << 4)) != 0;
        const bool recoverable =
            (regs->err_code & 0x1u) == 0 || write;
        size_t max_stack_length = process::kMaxThreadStackSize;
        if (proc != nullptr && proc->resources != nullptr) {
            const vm::Usage memory_usage = vm::usage(proc->cr3);
            const uint64_t virtual_limit =
                proc->resources->limits.max_virtual_bytes;
            uint64_t available = 0;
            if (memory_usage.virtual_bytes < virtual_limit) {
                available = virtual_limit - memory_usage.virtual_bytes;
            }
            const uint64_t permitted =
                available > UINT64_MAX - proc->stack_region.length
                    ? UINT64_MAX
                    : available + proc->stack_region.length;
            if (permitted < max_stack_length) {
                max_stack_length = static_cast<size_t>(permitted);
            }
        }
        if (recoverable && proc != nullptr &&
            vm::handle_page_fault(proc->cr3,
                                  fault_address,
                                  write,
                                  execute,
                                  &proc->stack_region,
                                  regs->rsp,
                                  max_stack_length)) {
            return;
        }
    }

    log_message(LogLevel::Error, "Exception %x %s",
                static_cast<unsigned int>(regs->int_no),
                regs->int_no < 32 ? exception_names[regs->int_no] : "Unknown");
    if (auto* cpu = percpu::current_cpu()) {
        log_message(LogLevel::Error,
                    "CPU: lapic=%u processor=%u",
                    cpu->lapic_id,
                    cpu->processor_id);
    }
    uint16_t selector = static_cast<uint16_t>(regs->err_code & 0xFFF8);
    bool external = (regs->err_code & 0x1) != 0;
    bool idt = (regs->err_code & 0x2) != 0;
    bool ldt = (regs->err_code & 0x4) != 0;
    log_message(LogLevel::Error, "Error code: %x (sel=%04x ext=%d idt=%d ldt=%d)",
                static_cast<unsigned int>(regs->err_code),
                static_cast<unsigned int>(selector),
                external ? 1 : 0,
                idt ? 1 : 0,
                ldt ? 1 : 0);
    uint64_t cr3_reg = 0;
    asm volatile("mov %%cr3, %0" : "=r"(cr3_reg));
    if (auto* cur = process::current()) {
        log_message(LogLevel::Error,
                    "Faulting task tid=%u pid=%u image=%s cr3=%016llx",
                    static_cast<unsigned int>(cur->tid),
                    static_cast<unsigned int>(cur->pid),
                    cur->image_path[0] != '\0' ? cur->image_path : "(unknown)",
                    static_cast<unsigned long long>(cr3_reg));
        uint64_t code_base = cur->code_region.base;
        uint64_t code_len = static_cast<uint64_t>(cur->code_region.length);
        uint64_t stack_base = cur->stack_region.base;
        uint64_t stack_top = cur->stack_region.top;
        bool rip_in_code = code_base != 0 &&
                           regs->rip >= code_base &&
                           regs->rip < code_base + code_len;
        bool rsp_in_stack = stack_base != 0 &&
                            regs->rsp >= stack_base &&
                            regs->rsp < stack_top;
        log_message(LogLevel::Debug,
                    "Task layout code=%016llx+%llu stack=%016llx..%016llx rip_off=%s%llx rsp_in_stack=%u",
                    static_cast<unsigned long long>(code_base),
                    static_cast<unsigned long long>(code_len),
                    static_cast<unsigned long long>(stack_base),
                    static_cast<unsigned long long>(stack_top),
                    rip_in_code ? "" : "!",
                    rip_in_code
                        ? static_cast<unsigned long long>(regs->rip - code_base)
                        : static_cast<unsigned long long>(regs->rip),
                    rsp_in_stack ? 1u : 0u);
    } else {
        log_message(LogLevel::Error,
                    "Faulting process unknown (cr3=%016llx)",
                    static_cast<unsigned long long>(cr3_reg));
    }
    if (regs->int_no == 14) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        log_message(LogLevel::Error, "CR2=%016llx",
                    static_cast<unsigned long long>(cr2));
    }

    if ((regs->cs & 0x3) != 0) {
        uint8_t insn[8]{};
        bool have_insn = false;
        if (auto* cur = process::current()) {
            have_insn = vm::copy_from_user(cur->cr3, insn, regs->rip, sizeof(insn));
            uint64_t rip_phys = 0;
            uint64_t rip_flags = 0;
            bool have_mapping =
                paging_resolve_cr3(cur->cr3, regs->rip, rip_phys) &&
                paging_flags_cr3(cur->cr3, regs->rip, rip_flags);
            if (have_mapping) {
                log_message(LogLevel::Debug,
                            "RIP mapping: phys=%016llx flags=%016llx writable=%u",
                            static_cast<unsigned long long>(rip_phys),
                            static_cast<unsigned long long>(rip_flags),
                            (rip_flags & PAGE_FLAG_WRITE) != 0 ? 1u : 0u);
            } else {
                log_message(LogLevel::Debug, "RIP mapping: unavailable");
            }
        }
        if (have_insn) {
            log_message(LogLevel::Debug,
                        "Insn bytes: %02x %02x %02x %02x %02x %02x %02x %02x",
                        static_cast<unsigned int>(insn[0]),
                        static_cast<unsigned int>(insn[1]),
                        static_cast<unsigned int>(insn[2]),
                        static_cast<unsigned int>(insn[3]),
                        static_cast<unsigned int>(insn[4]),
                        static_cast<unsigned int>(insn[5]),
                        static_cast<unsigned int>(insn[6]),
                        static_cast<unsigned int>(insn[7]));
        } else {
            log_message(LogLevel::Debug, "Insn bytes: unavailable");
        }
    }

    log_message(LogLevel::Debug, "RAX=%016x     RBX=%016x     RCX=%016x",
                static_cast<unsigned long long>(regs->rax),
                static_cast<unsigned long long>(regs->rbx),
                static_cast<unsigned long long>(regs->rcx));
    log_message(LogLevel::Debug, "RDX=%016x     RSI=%016x     RDI=%016x",
                static_cast<unsigned long long>(regs->rdx),
                static_cast<unsigned long long>(regs->rsi),
                static_cast<unsigned long long>(regs->rdi));
    log_message(LogLevel::Debug, "R8 =%016x     R9 =%016x     R10=%016x",
                static_cast<unsigned long long>(regs->r8),
                static_cast<unsigned long long>(regs->r9),
                static_cast<unsigned long long>(regs->r10));
    log_message(LogLevel::Debug, "R11=%016x     R12=%016x     R13=%016x",
                static_cast<unsigned long long>(regs->r11),
                static_cast<unsigned long long>(regs->r12),
                static_cast<unsigned long long>(regs->r13));
    log_message(LogLevel::Debug, "R14=%016x     R15=%016x     RBP=%016x",
                static_cast<unsigned long long>(regs->r14),
                static_cast<unsigned long long>(regs->r15),
                static_cast<unsigned long long>(regs->rbp));
    log_message(LogLevel::Debug, "RIP=%016x     RSP=%016x  RFLAGS=%016x",
                static_cast<unsigned long long>(regs->rip),
                static_cast<unsigned long long>(regs->rsp),
                static_cast<unsigned long long>(regs->rflags));
    log_message(LogLevel::Debug, "CS=%04x    SS=%04x",
                static_cast<unsigned int>(regs->cs),
                static_cast<unsigned int>(regs->ss));

    uint64_t stack_words[6]{};
    bool have_stack_words = false;
    if (regs->rsp != 0) {
        if ((regs->cs & 0x3) != 0) {
            if (auto* cur = process::current()) {
                have_stack_words =
                    vm::copy_from_user(cur->cr3,
                                       stack_words,
                                       regs->rsp,
                                       sizeof(stack_words));
            }
        } else {
            uint64_t* stack = reinterpret_cast<uint64_t*>(regs->rsp);
            for (size_t i = 0; i < 6; ++i) {
                stack_words[i] = stack[i];
            }
            have_stack_words = true;
        }
    }
    if (have_stack_words) {
        log_message(LogLevel::Debug,
                    "Stack[0..5]: %016llx %016llx %016llx %016llx %016llx %016llx",
                    static_cast<unsigned long long>(stack_words[0]),
                    static_cast<unsigned long long>(stack_words[1]),
                    static_cast<unsigned long long>(stack_words[2]),
                    static_cast<unsigned long long>(stack_words[3]),
                    static_cast<unsigned long long>(stack_words[4]),
                    static_cast<unsigned long long>(stack_words[5]));
    } else {
        log_message(LogLevel::Debug,
                    "Stack[0..5]: unavailable");
    }

    // If the fault originated from userspace, terminate the offending process
    // instead of halting the whole system. This lets parent processes observe
    // that the kernel killed the task (exit code has the high bit set) and
    // continue running.
    if ((regs->cs & 0x3) != 0) {
        uint16_t exit_code = exit_code_from_exception(regs->int_no);
        log_message(LogLevel::Error,
                    "Terminating userspace task tid=%u image=%s due to exception %s (#%u) rip=%016llx exit=%u",
                    process::current() ? static_cast<unsigned int>(process::current()->tid) : 0,
                    (process::current() != nullptr &&
                     process::current()->image_path[0] != '\0')
                        ? process::current()->image_path
                        : "(unknown)",
                    regs->int_no < 32 ? exception_names[regs->int_no] : "Unknown",
                    static_cast<unsigned int>(regs->int_no),
                    static_cast<unsigned long long>(regs->rip),
                    static_cast<unsigned int>(exit_code));
        if (process::Task* proc = process::current()) {
            process::terminate(*proc, exit_code);
        }
        scheduler::reschedule_from_interrupt(*regs);
        return;
    }

    const char* secondary =
        regs->int_no < 32 ? exception_names[regs->int_no] : "UNKNOWN_EXCEPTION";
    error_screen::display("UNHANDLED_CPU_EXCEPTION_", secondary, regs);
}
