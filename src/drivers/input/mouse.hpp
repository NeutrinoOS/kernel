#pragma once

#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"

namespace mouse {

using Event = descriptor_defs::MouseEvent;

void init();
// ACPI mode transitions can reset firmware-backed 8042 auxiliary ports.
void recover_after_acpi_mode();
void handle_irq();
size_t read(uint32_t slot, Event* buffer, size_t max_events);
bool has_data(uint32_t slot);

}  // namespace mouse
