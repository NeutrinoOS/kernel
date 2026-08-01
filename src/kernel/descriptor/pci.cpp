#include "../descriptor.hpp"

#include "../../drivers/pci/pci.hpp"

namespace descriptor {
namespace pci_descriptor {

int64_t read(process::Task&,
             DescriptorEntry&,
             uint64_t user_address,
             uint64_t length,
             uint64_t offset) {
    constexpr uint64_t entry_size = sizeof(descriptor_defs::PciDeviceInfo);
    if (user_address == 0 || length == 0 ||
        (offset % entry_size) != 0 || (length % entry_size) != 0) {
        return -1;
    }

    size_t first = static_cast<size_t>(offset / entry_size);
    size_t count = pci::device_count();
    if (first >= count) {
        return 0;
    }

    size_t capacity = static_cast<size_t>(length / entry_size);
    size_t available = count - first;
    size_t to_copy = capacity < available ? capacity : available;
    auto* out = reinterpret_cast<descriptor_defs::PciDeviceInfo*>(user_address);
    const pci::PciDevice* devices = pci::devices();

    for (size_t i = 0; i < to_copy; ++i) {
        const pci::PciDevice& device = devices[first + i];
        out[i] = {
            .vendor_id = device.vendor,
            .device_id = device.device,
            .bus = device.bus,
            .slot = device.slot,
            .function = device.function,
            .class_code = device.class_code,
            .subclass = device.subclass,
            .prog_if = device.prog_if,
            .revision = device.revision,
            .reserved = 0,
        };
    }
    return static_cast<int64_t>(to_copy * entry_size);
}

int64_t write(process::Task&, DescriptorEntry&, uint64_t, uint64_t, uint64_t) {
    return -1;
}

int get_property(DescriptorEntry&, uint32_t, void*, size_t) {
    return -1;
}

const Ops kPciOps{
    .read = read,
    .write = write,
    .get_property = get_property,
    .set_property = nullptr,
};

bool open(process::Task&, uint64_t, uint64_t, uint64_t, Allocation& alloc) {
    alloc.type = kTypePci;
    alloc.flags = static_cast<uint64_t>(Flag::Readable) |
                  static_cast<uint64_t>(Flag::Seekable) |
                  static_cast<uint64_t>(Flag::Device);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = nullptr;
    alloc.subsystem_data = nullptr;
    alloc.name = "pci";
    alloc.ops = &kPciOps;
    alloc.ext = nullptr;
    alloc.close = nullptr;
    return true;
}

}  // namespace pci_descriptor

bool register_pci_descriptor() {
    return register_type(kTypePci, pci_descriptor::open, &pci_descriptor::kPciOps);
}

}  // namespace descriptor
