#pragma once

#include "arch/x86_64/isr.hpp"
#include "arch/x86_64/syscall.hpp"
#include "arch/x86_64/percpu.hpp"
#include "process.hpp"

namespace scheduler {

using PollFn = void (*)();

void init();
void enqueue(process::Task* proc);
void remove(process::Task* proc);
[[noreturn]] void run();
[[noreturn]] void run_cpu();
void reschedule(syscall::SyscallFrame& frame);
void reschedule_from_interrupt(InterruptFrame& frame);
process::Task* current();
void register_cpu(percpu::Cpu* cpu);
void tick(InterruptFrame& frame);
size_t cpu_total();
bool register_poll(PollFn fn);
void service_polls();

}  // namespace scheduler
