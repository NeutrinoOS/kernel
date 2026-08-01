#include "keyboard.hpp"

#include "arch/x86_64/io.hpp"
#include "../interrupts/ioapic.hpp"
#include "../interrupts/pic.hpp"
#include "../log/logging.hpp"
#include "../../kernel/descriptor.hpp"
#include "../../kernel/error.hpp"
#include "../../kernel/sync.hpp"

namespace keyboard {
namespace {

constexpr size_t kBufferSize = 256;
constexpr size_t kInputSlots = 6;
constexpr uint16_t kDataPort = 0x60;
constexpr uint16_t kStatusPort = 0x64;

constexpr uint8_t kStatusOutputFull = 1u << 0;
constexpr uint8_t kStatusInputFull = 1u << 1;

constexpr uint8_t kCommandReadConfig = 0x20;
constexpr uint8_t kCommandWriteConfig = 0x60;

struct SlotBuffer {
    descriptor_defs::KeyboardEvent data[kBufferSize];
    size_t head;
    size_t tail;
    sync::SpinLock lock;
};

SlotBuffer g_buffers[kInputSlots];
sync::SpinLock g_state_lock;

bool g_shift = false;
bool g_caps_lock = false;
bool g_initialized = false;
bool g_left_shift_down = false;
bool g_right_shift_down = false;
bool g_left_ctrl_down = false;
bool g_left_alt_down = false;
bool g_right_ctrl_down = false;
bool g_right_alt_down = false;
bool g_extended_pending = false;

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

bool enqueue(uint32_t slot, const descriptor_defs::KeyboardEvent& event) {
    if (slot >= kInputSlots) {
        return false;
    }
    SlotBuffer& buf = g_buffers[slot];
    sync::IrqLockGuard guard(buf.lock);
    KERNEL_ASSERT_MSG(buf.head < kBufferSize,
                      "keyboard queue head is out of bounds");
    KERNEL_ASSERT_MSG(buf.tail < kBufferSize,
                      "keyboard queue tail is out of bounds");
    size_t next = (buf.head + 1) % kBufferSize;
    if (next == buf.tail) {
        return false;
    }
    buf.data[buf.head] = event;
    buf.head = next;
    return true;
}

bool dequeue(uint32_t slot, descriptor_defs::KeyboardEvent& event) {
    if (slot >= kInputSlots) {
        return false;
    }
    SlotBuffer& buf = g_buffers[slot];
    sync::IrqLockGuard guard(buf.lock);
    KERNEL_ASSERT_MSG(buf.head < kBufferSize,
                      "keyboard queue head is out of bounds");
    KERNEL_ASSERT_MSG(buf.tail < kBufferSize,
                      "keyboard queue tail is out of bounds");
    if (buf.head == buf.tail) {
        return false;
    }
    event = buf.data[buf.tail];
    buf.tail = (buf.tail + 1) % kBufferSize;
    return true;
}

void update_shift_state() {
    g_shift = g_left_shift_down || g_right_shift_down;
}

void update_modifier_state() {
    update_shift_state();
}

uint8_t current_mods() {
    uint8_t mods = 0;
    if (g_shift) {
        mods |= descriptor_defs::kKeyboardModShift;
    }
    if (g_left_ctrl_down || g_right_ctrl_down) {
        mods |= descriptor_defs::kKeyboardModCtrl;
    }
    if (g_left_alt_down || g_right_alt_down) {
        mods |= descriptor_defs::kKeyboardModAlt;
    }
    if (g_caps_lock) {
        mods |= descriptor_defs::kKeyboardModCaps;
    }
    return mods;
}

void process_scancode(uint8_t scancode, bool extended, bool pressed) {
    descriptor_defs::KeyboardEvent event{};
    bool select_framebuffer = false;
    uint32_t framebuffer_index = 0;
    bool queued = false;
    {
        sync::IrqLockGuard guard(g_state_lock);
        if (!extended) {
            if (scancode == 0x2A) {
                g_left_shift_down = pressed;
            } else if (scancode == 0x36) {
                g_right_shift_down = pressed;
            } else if (scancode == 0x1D) {
                g_left_ctrl_down = pressed;
            } else if (scancode == 0x38) {
                g_left_alt_down = pressed;
            } else if (scancode == 0x3A && pressed) {
                g_caps_lock = !g_caps_lock;
            }
        } else {
            if (scancode == 0x1D) {
                g_right_ctrl_down = pressed;
            } else if (scancode == 0x38) {
                g_right_alt_down = pressed;
            }
        }

        update_modifier_state();

        bool ctrl = g_left_ctrl_down || g_right_ctrl_down;
        bool alt = g_left_alt_down || g_right_alt_down;
        if (pressed && ctrl && alt && g_shift &&
            scancode >= 0x3B && scancode <= 0x40) {
            select_framebuffer = true;
            framebuffer_index = static_cast<uint32_t>(scancode - 0x3B);
        } else if (pressed && ctrl && g_shift &&
                   scancode >= 0x02 && scancode <= 0x07) {
            select_framebuffer = true;
            framebuffer_index = static_cast<uint32_t>(scancode - 0x02);
        }

        event.scancode = scancode;
        event.flags = 0;
        if (pressed) {
            event.flags |= descriptor_defs::kKeyboardFlagPressed;
        }
        if (extended) {
            event.flags |= descriptor_defs::kKeyboardFlagExtended;
        }
        event.mods = current_mods();
        event.reserved = 0;

        if (!select_framebuffer) {
            uint32_t slot = descriptor::framebuffer_active_slot();
            if (slot >= kInputSlots) {
                slot = 0;
            }
            queued = enqueue(slot, event);
        }
    }

    if (select_framebuffer) {
        descriptor::framebuffer_select(framebuffer_index);
        return;
    }
    if (queued) {
        descriptor::wake_waiters();
    }
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
    g_shift = false;
    g_caps_lock = false;
    g_left_shift_down = false;
    g_right_shift_down = false;
    g_left_ctrl_down = false;
    g_left_alt_down = false;
    g_right_ctrl_down = false;
    g_right_alt_down = false;
    g_extended_pending = false;
    g_initialized = true;

    write_command(kCommandReadConfig);
    uint8_t config = read_data();
    config |= (1u << 0);   // enable IRQ1
    config &= ~(1u << 4);  // enable keyboard clock
    write_command(kCommandWriteConfig);
    write_data(config);

    if (!ioapic::handles_irq(1)) {
        pic::set_mask(1, false);
    }
}

void handle_irq() {
    uint8_t status = inb(kStatusPort);
    if ((status & kStatusOutputFull) == 0) {
        return;
    }

    uint8_t scancode = inb(kDataPort);

    if (scancode == 0xE0 || scancode == 0xE1) {
        g_extended_pending = true;
        return;
    }

    bool extended = g_extended_pending;
    g_extended_pending = false;
    bool pressed = (scancode & 0x80u) == 0;
    uint8_t code = static_cast<uint8_t>(scancode & 0x7Fu);
    process_scancode(code, extended, pressed);
}

size_t read(uint32_t slot,
            descriptor_defs::KeyboardEvent* buffer,
            size_t max_events) {
    if (buffer == nullptr || max_events == 0) {
        return 0;
    }
    if (slot >= kInputSlots) {
        return 0;
    }

    size_t count = 0;
    while (count < max_events) {
        descriptor_defs::KeyboardEvent event{};
        if (!dequeue(slot, event)) {
            break;
        }
        buffer[count++] = event;
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

void inject_scancode(uint8_t scancode, bool extended, bool pressed) {
    process_scancode(scancode, extended, pressed);
}

}  // namespace keyboard
