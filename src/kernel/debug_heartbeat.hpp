#pragma once

#include <stdint.h>

struct Framebuffer;

namespace debug_heartbeat {

void init(const Framebuffer& framebuffer);
void tick(uint64_t scheduler_tick);

}  // namespace debug_heartbeat
