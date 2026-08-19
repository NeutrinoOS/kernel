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
constexpr uint8_t kMouseDisableStream = 0xF5;
constexpr uint8_t kMouseEnableStream = 0xF4;
constexpr uint8_t kMouseGetInfo = 0xE9;
constexpr uint8_t kMouseSetScale11 = 0xE6;
constexpr uint8_t kMouseReset = 0xFF;
constexpr uint8_t kMouseAck = 0xFA;
constexpr uint8_t kMouseSelfTestPassed = 0xAA;

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
bool g_elan_detected = false;

bool wait_input_clear() {
    for (size_t i = 0; i < 100000; ++i) {
        if ((inb(kStatusPort) & kStatusInputFull) == 0) {
            return true;
        }
    }
    return false;
}

bool write_command(uint8_t cmd) {
    if (!wait_input_clear()) {
        return false;
    }
    outb(kStatusPort, cmd);
    return true;
}

bool write_data(uint8_t data) {
    if (!wait_input_clear()) {
        return false;
    }
    outb(kDataPort, data);
    return true;
}

bool read_data(uint8_t& data, bool auxiliary_only = false,
               size_t attempts = 100000) {
    for (size_t i = 0; i < attempts; ++i) {
        uint8_t status = inb(kStatusPort);
        if ((status & kStatusOutputFull) == 0) {
            continue;
        }
        uint8_t value = inb(kDataPort);
        if (auxiliary_only && (status & kStatusAuxData) == 0) {
            continue;
        }
        data = value;
        return true;
    }
    return false;
}

void drain_output() {
    for (size_t i = 0; i < 32; ++i) {
        if ((inb(kStatusPort) & kStatusOutputFull) == 0) {
            return;
        }
        (void)inb(kDataPort);
    }
}

bool write_mouse(uint8_t data) {
    if (!write_command(kCommandWriteAux) || !write_data(data)) {
        return false;
    }
    uint8_t ack = 0;
    return read_data(ack, true) && ack == kMouseAck;
}

bool get_mouse_info(uint8_t (&info)[3]) {
    if (!write_mouse(kMouseGetInfo)) {
        return false;
    }
    return read_data(info[0], true) && read_data(info[1], true) &&
           read_data(info[2], true);
}

bool detect_elan() {
    // ELAN PS/2 touchpads answer this vendor-defined magic knock. Keep the
    // device in relative mode afterwards because Neutrino currently exposes
    // relative mouse events to the desktop.
    if (!write_mouse(kMouseDisableStream) ||
        !write_mouse(kMouseSetScale11) ||
        !write_mouse(kMouseSetScale11) ||
        !write_mouse(kMouseSetScale11)) {
        return false;
    }

    uint8_t info[3]{};
    if (!get_mouse_info(info)) {
        return false;
    }
    return info[0] == 0x3C && info[1] == 0x03 &&
           (info[2] == 0xC8 || info[2] == 0x00);
}

bool configure_auxiliary_port(bool reset_device) {
    drain_output();
    if (!write_command(kCommandEnableAux) ||
        !write_command(kCommandReadConfig)) {
        return false;
    }

    uint8_t config = 0;
    if (!read_data(config)) {
        return false;
    }
    config |= (1u << 1);   // enable IRQ12
    config &= ~(1u << 5);  // enable mouse clock
    if (!write_command(kCommandWriteConfig) || !write_data(config)) {
        return false;
    }

    if (reset_device && write_mouse(kMouseReset)) {
        uint8_t self_test = 0;
        uint8_t device_id = 0;
        if (!read_data(self_test, true, 10000000) ||
            self_test != kMouseSelfTestPassed) {
            log_message(LogLevel::Warn,
                        "Mouse: auxiliary device reset did not complete");
        } else {
            // The device ID follows BAT. It is diagnostic only; some
            // firmware-backed controllers omit it.
            (void)read_data(device_id, true);
        }
    }

    g_elan_detected = detect_elan();
    if (!write_mouse(kMouseSetDefaults)) {
        log_message(LogLevel::Warn, "Mouse: failed to set defaults");
    }
    if (!write_mouse(kMouseEnableStream)) {
        log_message(LogLevel::Warn, "Mouse: failed to enable streaming");
        return false;
    }
    return true;
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

    if (!configure_auxiliary_port(false)) {
        log_message(LogLevel::Warn,
                    "Mouse: failed to initialize auxiliary port");
    } else if (g_elan_detected) {
        log_message(LogLevel::Info,
                    "Mouse: ELAN PS/2-compatible touchpad detected");
    }

    if (!ioapic::handles_irq(12)) {
        pic::set_mask(2, false);
        pic::set_mask(12, false);
    }

    g_initialized = true;
}

void recover_after_acpi_mode() {
    if (!g_initialized) {
        return;
    }

    const uint64_t interrupt_state = sync::disable_interrupts();
    g_packet_index = 0;
    const bool recovered = configure_auxiliary_port(true);
    sync::restore_interrupts(interrupt_state);

    if (!recovered) {
        log_message(LogLevel::Warn,
                    "Mouse: failed to recover auxiliary port after ACPI mode switch");
        return;
    }
    log_message(LogLevel::Info,
                g_elan_detected
                    ? "Mouse: ELAN touchpad recovered after ACPI mode switch"
                    : "Mouse: auxiliary device recovered after ACPI mode switch");
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
