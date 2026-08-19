#pragma once

#include <stddef.h>
#include <stdint.h>

#include "descriptors.hpp"

namespace keyboard {

void init();
void handle_irq();
void poll_controller();
size_t read(uint32_t slot,
            descriptor_defs::KeyboardEvent* buffer,
            size_t max_events);
bool has_data(uint32_t slot);
void inject_scancode(uint8_t scancode, bool extended, bool pressed);

}  // namespace keyboard
