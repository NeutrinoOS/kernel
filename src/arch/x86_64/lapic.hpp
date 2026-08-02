#pragma once

#include <stdint.h>

namespace lapic {

constexpr uint8_t kSchedulerWakeVector = 0x41;

void init(uint64_t hhdm_offset);
void setup_timer(uint8_t vector, uint32_t initial_count);
void eoi();
void send_ipi(uint32_t lapic_id, uint8_t vector);
void send_ipi_all_others(uint8_t vector);
uint32_t id();

}  // namespace lapic
