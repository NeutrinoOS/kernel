#include "mouse.hpp"

#include "arch/x86_64/io.hpp"
#include "../interrupts/ioapic.hpp"
#include "../interrupts/pic.hpp"
#include "../log/logging.hpp"
#include "../../kernel/descriptor.hpp"
#include "../../kernel/error.hpp"
#include "../../kernel/sync.hpp"

namespace mouse {
namespace {

constexpr size_t kBufferSize = 64;
constexpr size_t kInputSlots = 6;

constexpr uint16_t kDataPort = 0x60;
constexpr uint16_t kStatusPort = 0x64;

constexpr uint8_t kStatusOutputFull = 1u << 0;
constexpr uint8_t kStatusInputFull = 1u << 1;
constexpr uint8_t kStatusAuxData = 1u << 5;

constexpr uint8_t kCommandEnableAux = 0xA8;
constexpr uint8_t kCommandReadConfig = 0x20;
constexpr uint8_t kCommandWriteConfig = 0x60;
constexpr uint8_t kCommandWriteAux = 0xD4;

constexpr uint8_t kMouseSetDefaults = 0xF6;
constexpr uint8_t kMouseEnableStream = 0xF4;
constexpr uint8_t kMouseAck = 0xFA;

struct SlotBuffer {
    Event data[kBufferSize];
    size_t head;
    size_t tail;
    sync::SpinLock lock;
};

SlotBuffer g_buffers[kInputSlots];
bool g_initialized = false;
uint8_t g_packet[3];
uint8_t g_packet_index = 0;
bool g_have_last_event = false;
Event g_last_event{};

bool wait_input_clear() {
    for (size_t i = 0; i < 100000; ++i) {
        if ((inb(kStatusPort) & kStatusInputFull) == 0) {
            return true;
        }
    }
    return false;
}

bool wait_output_full() {
    for (size_t i = 0; i < 100000; ++i) {
        if ((inb(kStatusPort) & kStatusOutputFull) != 0) {
            return true;
        }
    }
    return false;
}

void write_command(uint8_t cmd) {
    if (!wait_input_clear()) {
        return;
    }
    outb(kStatusPort, cmd);
}

void write_data(uint8_t data) {
    if (!wait_input_clear()) {
        return;
    }
    outb(kDataPort, data);
}

uint8_t read_data() {
    if (!wait_output_full()) {
        return 0;
    }
    return inb(kDataPort);
}

bool write_mouse(uint8_t data) {
    write_command(kCommandWriteAux);
    write_data(data);
    uint8_t ack = read_data();
    return ack == kMouseAck;
}

void enqueue(uint32_t slot, const Event& ev) {
    if (slot >= kInputSlots) {
        return;
    }
    SlotBuffer& buf = g_buffers[slot];
    bool queued = false;
    {
        sync::IrqLockGuard guard(buf.lock);
        KERNEL_ASSERT_MSG(buf.head < kBufferSize,
                          "mouse queue head is out of bounds");
        KERNEL_ASSERT_MSG(buf.tail < kBufferSize,
                          "mouse queue tail is out of bounds");
        size_t next = (buf.head + 1) % kBufferSize;
        if (next != buf.tail) {
            buf.data[buf.head] = ev;
            buf.head = next;
            queued = true;
        }
    }
    if (queued) {
        descriptor::wake_waiters();
    }
}

bool dequeue(uint32_t slot, Event& ev) {
    if (slot >= kInputSlots) {
        return false;
    }
    SlotBuffer& buf = g_buffers[slot];
    sync::IrqLockGuard guard(buf.lock);
    KERNEL_ASSERT_MSG(buf.head < kBufferSize,
                      "mouse queue head is out of bounds");
    KERNEL_ASSERT_MSG(buf.tail < kBufferSize,
                      "mouse queue tail is out of bounds");
    if (buf.head == buf.tail) {
        return false;
    }
    ev = buf.data[buf.tail];
    buf.tail = (buf.tail + 1) % kBufferSize;
    return true;
}

}  // namespace

void init() {
    if (g_initialized) {
        return;
    }
    for (auto& buf : g_buffers) {
        buf.head = 0;
        buf.tail = 0;
    }
    g_packet_index = 0;
    g_have_last_event = false;
    g_last_event = {};

    write_command(kCommandEnableAux);
    write_command(kCommandReadConfig);
    uint8_t config = read_data();
    config |= (1u << 1);   // enable IRQ12
    config &= ~(1u << 5);  // enable mouse clock
    write_command(kCommandWriteConfig);
    write_data(config);

    if (!write_mouse(kMouseSetDefaults)) {
        log_message(LogLevel::Warn, "Mouse: failed to set defaults");
    }
    if (!write_mouse(kMouseEnableStream)) {
        log_message(LogLevel::Warn, "Mouse: failed to enable streaming");
    }

    if (!ioapic::handles_irq(12)) {
        pic::set_mask(2, false);
        pic::set_mask(12, false);
    }

    g_initialized = true;
}

void handle_irq() {
    while (true) {
        uint8_t status = inb(kStatusPort);
        if ((status & kStatusOutputFull) == 0) {
            return;
        }
        if ((status & kStatusAuxData) == 0) {
            return;
        }

        uint8_t data = inb(kDataPort);
        if (g_packet_index == 0 && (data & 0x08) == 0) {
            continue;
        }
        g_packet[g_packet_index++] = data;
        if (g_packet_index < 3) {
            continue;
        }
        g_packet_index = 0;

        Event ev{};
        ev.buttons = static_cast<uint8_t>(g_packet[0] & 0x07);
        ev.dx = static_cast<int8_t>(g_packet[1]);
        ev.dy = static_cast<int8_t>(g_packet[2]);
        ev.reserved = 0;

        if (g_have_last_event &&
            ev.buttons == g_last_event.buttons &&
            ev.dx == 0 && ev.dy == 0 &&
            g_last_event.dx == 0 && g_last_event.dy == 0) {
            continue;
        }
        g_last_event = ev;
        g_have_last_event = true;

        uint32_t slot = descriptor::framebuffer_active_slot();
        if (slot >= kInputSlots) {
            slot = 0;
        }
        enqueue(slot, ev);
    }
}

size_t read(uint32_t slot, Event* buffer, size_t max_events) {
    if (buffer == nullptr || max_events == 0) {
        return 0;
    }
    if (slot >= kInputSlots) {
        return 0;
    }

    size_t count = 0;
    while (count < max_events) {
        Event ev{};
        if (!dequeue(slot, ev)) {
            break;
        }
        buffer[count++] = ev;
    }
    return count;
}

bool has_data(uint32_t slot) {
    if (slot >= kInputSlots) {
        return false;
    }
    SlotBuffer& buf = g_buffers[slot];
    sync::IrqLockGuard guard(buf.lock);
    return buf.head != buf.tail;
}

}  // namespace mouse
