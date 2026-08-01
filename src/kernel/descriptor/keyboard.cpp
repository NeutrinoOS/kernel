#include "../descriptor.hpp"

#include "../../drivers/input/keyboard.hpp"

namespace descriptor {

namespace descriptor_keyboard {

int64_t keyboard_read(process::Task& proc,
                      DescriptorEntry& entry,
                      uint64_t user_address,
                      uint64_t length,
                      uint64_t offset) {
    if (offset != 0) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    auto* buffer =
        reinterpret_cast<descriptor_defs::KeyboardEvent*>(user_address);
    if (buffer == nullptr) {
        return -1;
    }
    uintptr_t slot_raw = reinterpret_cast<uintptr_t>(entry.subsystem_data);
    if (slot_raw == 0) {
        return -1;
    }
    uint32_t slot = static_cast<uint32_t>(slot_raw - 1);
    if (!is_kernel_process(proc) &&
        ((slot == 0 && !console_is_owner(proc)) ||
         (slot != 0 && !framebuffer_process_owns_slot(proc, slot)))) {
        return -1;
    }
    size_t max_events =
        static_cast<size_t>(length) / sizeof(descriptor_defs::KeyboardEvent);
    if (max_events == 0) {
        return 0;
    }
    size_t read_count = keyboard::read(slot, buffer, max_events);
    return static_cast<int64_t>(read_count * sizeof(descriptor_defs::KeyboardEvent));
}

int64_t keyboard_write(process::Task&,
                       DescriptorEntry&,
                       uint64_t,
                       uint64_t,
                       uint64_t) {
    return -1;
}

const Ops kKeyboardOps{
    .read = keyboard_read,
    .write = keyboard_write,
    .get_property = nullptr,
    .set_property = nullptr,
};

bool open_keyboard(process::Task& proc,
                   uint64_t,
                   uint64_t,
                   uint64_t,
                   Allocation& alloc) {
    keyboard::init();
    uint32_t slot = 0;
    if (!is_kernel_process(proc)) {
        int32_t proc_slot = framebuffer_slot_for_process(proc);
        if (proc_slot >= 0) {
            slot = static_cast<uint32_t>(proc_slot);
        } else if (console_is_owner(proc)) {
            slot = 0;
        } else {
            return false;
        }
    }
    alloc.type = kTypeKeyboard;
    alloc.flags = static_cast<uint64_t>(Flag::Readable) |
                  static_cast<uint64_t>(Flag::EventSource) |
                  static_cast<uint64_t>(Flag::Device);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = nullptr;
    alloc.subsystem_data =
        reinterpret_cast<void*>(static_cast<uintptr_t>(slot + 1));
    alloc.close = nullptr;
    alloc.name = "keyboard";
    alloc.ops = &kKeyboardOps;
    return true;
}

bool query_wait(DescriptorEntry& entry, uint32_t events, uint32_t& revents) {
    revents = 0;
    if ((events & descriptor_defs::kWaitRead) == 0) {
        return true;
    }
    uintptr_t slot_raw = reinterpret_cast<uintptr_t>(entry.subsystem_data);
    if (slot_raw == 0) {
        return false;
    }
    uint32_t slot = static_cast<uint32_t>(slot_raw - 1);
    if (keyboard::has_data(slot)) {
        revents |= descriptor_defs::kWaitRead;
    }
    return true;
}

}  // namespace descriptor_keyboard

bool register_keyboard_descriptor() {
    return register_type(kTypeKeyboard, descriptor_keyboard::open_keyboard, &descriptor_keyboard::kKeyboardOps);
}

}  // namespace descriptor
