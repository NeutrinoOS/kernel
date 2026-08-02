#include "lapic.hpp"

#include <stddef.h>
#include <stdint.h>

#include "drivers/log/logging.hpp"

namespace {

constexpr uint64_t kLapicPhysBase = 0xFEE00000;

volatile uint32_t* g_lapic = nullptr;

inline volatile uint32_t* reg(uint32_t offset) {
    return reinterpret_cast<volatile uint32_t*>(
        reinterpret_cast<uintptr_t>(g_lapic) + offset);
}

inline void write(uint32_t offset, uint32_t value) {
    *reg(offset) = value;
    (void)*reg(offset);
}

inline uint32_t read(uint32_t offset) {
    return *reg(offset);
}

}  // namespace

namespace lapic {

void init(uint64_t hhdm_offset) {
    g_lapic = reinterpret_cast<volatile uint32_t*>(kLapicPhysBase + hhdm_offset);

    if (g_lapic == nullptr) {
        log_message(LogLevel::Error, "LAPIC: MMIO base not set");
        return;
    }

    // enable LAPIC and set spurious vector (use 0xFF)
    write(0xF0, read(0xF0) | 0x100 | 0xFF);
}

uint32_t id() {
    if (g_lapic == nullptr) {
        return 0;
    }
    return read(0x20) >> 24;
}

void setup_timer(uint8_t vector, uint32_t initial_count) {
    if (g_lapic == nullptr) return;

    // divide by 16 (binary 0b0011 = 16)
    write(0x3E0, 0x3);
    // mask timer during setup
    write(0x320, (1u << 16) | vector);
    write(0x380, initial_count);
    // unmask periodic
    write(0x320, 0x20000 | vector);
}

void eoi() {
    if (g_lapic == nullptr) return;
    write(0xB0, 0);
}

void send_ipi(uint32_t lapic_id, uint8_t vector) {
    if (g_lapic == nullptr || lapic_id > 0xFFu || vector < 0x20u) return;
    for (uint32_t i = 0; i < 100000u; ++i) {
        if ((read(0x300) & (1u << 12)) == 0) break;
        asm volatile("pause");
    }
    write(0x310, lapic_id << 24);
    write(0x300, vector);
}

void send_ipi_all_others(uint8_t vector) {
    if (g_lapic == nullptr) return;
    // all excluding self, shorthand=3
    write(0x310, 0x0);
    write(0x300, (3u << 18) | vector);
}

}  // namespace lapic
