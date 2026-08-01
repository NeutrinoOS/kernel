#include "../descriptor.hpp"

#include "../../drivers/input/mouse.hpp"

namespace descriptor {

namespace descriptor_mouse {

int64_t mouse_read(process::Task& proc,
                   DescriptorEntry& entry,
                   uint64_t user_address,
                   uint64_t length,
                   uint64_t offset) {
    if (offset != 0) {
        return -1;
    }
    if (length < sizeof(mouse::Event)) {
        return 0;
    }
    auto* buffer = reinterpret_cast<mouse::Event*>(user_address);
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
    size_t max_events = static_cast<size_t>(length / sizeof(mouse::Event));
    size_t count = mouse::read(slot, buffer, max_events);
    return static_cast<int64_t>(count * sizeof(mouse::Event));
}

int64_t mouse_write(process::Task&,
                    DescriptorEntry&,
                    uint64_t,
                    uint64_t,
                    uint64_t) {
    return -1;
}

const Ops kMouseOps{
    .read = mouse_read,
    .write = mouse_write,
    .get_property = nullptr,
    .set_property = nullptr,
};

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
    if (mouse::has_data(slot)) {
        revents |= descriptor_defs::kWaitRead;
    }
    return true;
}

bool open_mouse(process::Task& proc,
                uint64_t,
                uint64_t,
                uint64_t,
                Allocation& alloc) {
    mouse::init();
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
    alloc.type = kTypeMouse;
    alloc.flags = static_cast<uint64_t>(Flag::Readable) |
                  static_cast<uint64_t>(Flag::EventSource) |
                  static_cast<uint64_t>(Flag::Device);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = nullptr;
    alloc.subsystem_data =
        reinterpret_cast<void*>(static_cast<uintptr_t>(slot + 1));
    alloc.close = nullptr;
    alloc.name = "mouse";
    alloc.ops = &kMouseOps;
    return true;
}

}  // namespace descriptor_mouse

bool register_mouse_descriptor() {
    return register_type(kTypeMouse, descriptor_mouse::open_mouse, &descriptor_mouse::kMouseOps);
}

}  // namespace descriptor
