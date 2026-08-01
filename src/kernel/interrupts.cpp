#include "interrupts.hpp"

#include "drivers/interrupts/ioapic.hpp"
#include "kernel/error.hpp"
#include "kernel/sync.hpp"

namespace {

interrupts::VectorHandler g_handlers[256]{};
bool g_reserved[256]{};
uint8_t g_isa_vectors[16]{};
sync::SpinLock g_lock;
constexpr uint8_t kFirstAllocVector = 0x50;
constexpr uint8_t kLastAllocVector = 0xFE;

}  // namespace

namespace interrupts {

uint8_t allocate_vector() {
    sync::IrqLockGuard guard(g_lock);
    for (uint16_t candidate = kFirstAllocVector;
         candidate <= kLastAllocVector;
         ++candidate) {
        uint8_t vector = static_cast<uint8_t>(candidate);
        if (!g_reserved[vector] && g_handlers[vector] == nullptr) {
            g_reserved[vector] = true;
            return vector;
        }
    }
    return 0;
}

void free_vector(uint8_t vector) {
    sync::IrqLockGuard guard(g_lock);
    if (vector < kFirstAllocVector || vector > kLastAllocVector) {
        return;
    }
    g_reserved[vector] = false;
}

bool register_vector(uint8_t vector, VectorHandler handler) {
    sync::IrqLockGuard guard(g_lock);
    if (handler == nullptr || vector < 32) {
        return false;
    }
    if (g_handlers[vector] != nullptr && g_handlers[vector] != handler) {
        return false;
    }
    g_reserved[vector] = true;
    g_handlers[vector] = handler;
    return true;
}

void unregister_vector(uint8_t vector) {
    sync::IrqLockGuard guard(g_lock);
    if (vector < 32) {
        return;
    }
    g_handlers[vector] = nullptr;
    if (vector >= kFirstAllocVector && vector <= kLastAllocVector) {
        g_reserved[vector] = false;
    }
}

bool dispatch(uint8_t vector) {
    VectorHandler handler = g_handlers[vector];
    if (handler == nullptr) {
        return false;
    }
    handler();
    return true;
}

bool register_isa_irq(uint8_t irq, IrqHandler handler, uint8_t* out_vector) {
    if (irq >= 16 || handler == nullptr) return false;
    uint8_t vector = 0;
    {
        sync::IrqLockGuard guard(g_lock);
        if (g_isa_vectors[irq] != 0) return false;
        for (uint16_t candidate = kFirstAllocVector; candidate <= kLastAllocVector; ++candidate) {
            uint8_t possible = static_cast<uint8_t>(candidate);
            if (!g_reserved[possible] && g_handlers[possible] == nullptr) {
                g_reserved[possible] = true;
                g_handlers[possible] = handler;
                g_isa_vectors[irq] = possible;
                vector = possible;
                break;
            }
        }
    }
    if (vector == 0 || !ioapic::route_isa_irq(irq, vector)) {
        if (vector != 0) unregister_isa_irq(irq);
        return false;
    }
    if (out_vector != nullptr) *out_vector = vector;
    return true;
}

void unregister_isa_irq(uint8_t irq) {
    if (irq >= 16) return;
    sync::IrqLockGuard guard(g_lock);
    uint8_t vector = g_isa_vectors[irq];
    if (vector != 0) {
        KERNEL_ASSERT_MSG(vector >= kFirstAllocVector &&
                              vector <= kLastAllocVector,
                          "ISA IRQ references an invalid interrupt vector");
        KERNEL_ASSERT_MSG(g_reserved[vector],
                          "ISA IRQ vector is not reserved");
        KERNEL_ASSERT_MSG(g_handlers[vector] != nullptr,
                          "ISA IRQ vector has no registered handler");
        g_handlers[vector] = nullptr;
        g_reserved[vector] = false;
        g_isa_vectors[irq] = 0;
    }
}

}  // namespace interrupts
