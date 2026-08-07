#pragma once

#include <stdint.h>

struct InterruptFrame;

namespace process {
struct Task;
}

namespace core_dump {

// Captures exception metadata and queues an ELF core dump. Safe to call from
// interrupt context. The task address space remains pinned until completion.
bool schedule(process::Task& task,
              const InterruptFrame& frame,
              uint64_t fault_address);

}  // namespace core_dump
