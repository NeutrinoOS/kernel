#include "error.hpp"

#include "drivers/console/console.hpp"
#include "drivers/serial/serial.hpp"

namespace {
constexpr uint32_t kErrorBackground = 0xFF941616;
constexpr uint32_t kErrorForeground = 0xFFFFFFFF;

struct ControlRegisters {
    uint64_t cr0;
    uint64_t cr2;
    uint64_t cr3;
    uint64_t cr4;
};

ControlRegisters read_control_registers() {
    ControlRegisters regs{};
    asm volatile("mov %%cr0, %0" : "=r"(regs.cr0));
    asm volatile("mov %%cr2, %0" : "=r"(regs.cr2));
    asm volatile("mov %%cr3, %0" : "=r"(regs.cr3));
    asm volatile("mov %%cr4, %0" : "=r"(regs.cr4));
    return regs;
}

void print_registers(const InterruptFrame* regs) {
    if (regs == nullptr) {
        if (kconsole != nullptr) {
            kconsole->printf("Register dump unavailable.\n");
        }
        return;
    }

    if (kconsole == nullptr) {
        return;
    }

    auto cr = read_control_registers();

    kconsole->printf("Register dump:\n");
    kconsole->printf("INT=%016x     ERR=%016x     CR2=%016x\n",
                     static_cast<unsigned int>(regs->int_no),
                     static_cast<unsigned long long>(regs->err_code),
                     static_cast<unsigned long long>(cr.cr2));
    kconsole->printf("RAX=%016x     RBX=%016x     RCX=%016x\n",
                     static_cast<unsigned long long>(regs->rax),
                     static_cast<unsigned long long>(regs->rbx),
                     static_cast<unsigned long long>(regs->rcx));
    kconsole->printf("RDX=%016x     RSI=%016x     RDI=%016x\n",
                     static_cast<unsigned long long>(regs->rdx),
                     static_cast<unsigned long long>(regs->rsi),
                     static_cast<unsigned long long>(regs->rdi));
    kconsole->printf("R8 =%016x     R9 =%016x     R10=%016x\n",
                     static_cast<unsigned long long>(regs->r8),
                     static_cast<unsigned long long>(regs->r9),
                     static_cast<unsigned long long>(regs->r10));
    kconsole->printf("R11=%016x     R12=%016x     R13=%016x\n",
                     static_cast<unsigned long long>(regs->r11),
                     static_cast<unsigned long long>(regs->r12),
                     static_cast<unsigned long long>(regs->r13));
    kconsole->printf("R14=%016x     R15=%016x     RBP=%016x\n",
                     static_cast<unsigned long long>(regs->r14),
                     static_cast<unsigned long long>(regs->r15),
                     static_cast<unsigned long long>(regs->rbp));
    kconsole->printf("RIP=%016x     RSP=%016x  RFLAGS=%016x\n",
                     static_cast<unsigned long long>(regs->rip),
                     static_cast<unsigned long long>(regs->rsp),
                     static_cast<unsigned long long>(regs->rflags));
    kconsole->printf("CS=%016x      SS=%016x\n",
                     static_cast<unsigned int>(regs->cs),
                     static_cast<unsigned int>(regs->ss));
    kconsole->printf("CR0=%016x     CR3=%016x     CR4=%016x\n",
                     static_cast<unsigned long long>(cr.cr0),
                     static_cast<unsigned long long>(cr.cr3),
                     static_cast<unsigned long long>(cr.cr4));
}

void prepare_screen(const char* main_message, const char* info_message) {
    if (kconsole == nullptr) {
        return;
    }

    kconsole->set_color(kErrorForeground, kErrorBackground);
    kconsole->clear();
    kconsole->putc('\n');
    kconsole->printf(" An error has occurred: %s%s\n",
                     main_message ? main_message : "",
                     info_message ? info_message : "");
    kconsole->printf(" Neutrino has been halted to prevent damage to your system or data.\n");
    kconsole->printf(" If possible, please record the following information for debugging purposes.\n\n");
}

void print_footer() {
    if (kconsole == nullptr) {
        return;
    }

    kconsole->putc('\n');
    kconsole->printf(" Please create a bug report at https://github.com/i3vie/neutrino.\n");
    kconsole->printf(" Include the information above and any steps to reproduce the issue.\n");
    kconsole->printf(" Thank you for helping to improve Neutrino!\n");
    kconsole->putc('\n');
    kconsole->printf(" System halted.\n");
}

void serial_write_u32(uint32_t value) {
    char digits[10];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0) {
        serial::write_char(digits[--count]);
    }
}

void print_assertion_to_serial(const char* expression,
                               const char* message,
                               const char* file,
                               uint32_t line,
                               const char* function) {
    serial::write_string("[FATAL] FAILED_KERNEL_ASSERTION\n");
    serial::write_string("Assertion: ");
    serial::write_string(expression ? expression : "(unavailable)");
    serial::write_string("\nLocation: ");
    serial::write_string(file ? file : "(unavailable)");
    serial::write_string(":");
    serial_write_u32(line);
    serial::write_string("\nFunction: ");
    serial::write_string(function ? function : "(unavailable)");
    if (message != nullptr && message[0] != '\0') {
        serial::write_string("\nMessage: ");
        serial::write_string(message);
    }
    serial::write_string("\n");
}

[[noreturn]] void halt() {
    while (true) {
        asm volatile("cli; hlt");
    }
}
}  // namespace

namespace error_screen {

[[noreturn]] void display(const char* primary,
                         const char* secondary,
                         const InterruptFrame* regs) {
    const char* main_message = primary ? primary : "";
    const char* info_message = secondary ? secondary : "";

    asm volatile("cli" ::: "memory");
    prepare_screen(main_message, info_message);
    if (kconsole != nullptr) {
        kconsole->putc('\n');
        print_registers(regs);
        print_footer();
    }
    halt();
}

}  // namespace error_screen

extern "C" [[noreturn]] void kernel_assertion_failed(const char* expression,
                                                     const char* message,
                                                     const char* file,
                                                     uint32_t line,
                                                     const char* function) {
    asm volatile("cli" ::: "memory");
    print_assertion_to_serial(expression, message, file, line, function);
    prepare_screen("FAILED_KERNEL_ASSERTION", nullptr);
    if (kconsole != nullptr) {
        kconsole->printf(" Assertion: %s\n",
                         expression ? expression : "(unavailable)");
        if (message != nullptr && message[0] != '\0') {
            kconsole->printf(" Message:   %s\n", message);
        }
        kconsole->printf(" Location:  %s:%u\n",
                         file ? file : "(unavailable)",
                         static_cast<unsigned int>(line));
        kconsole->printf(" Function:  %s\n",
                         function ? function : "(unavailable)");
        kconsole->printf(" Caller RIP: %016x\n",
                         reinterpret_cast<unsigned long long>(
                             __builtin_return_address(0)));
        print_footer();
    }
    halt();
}
