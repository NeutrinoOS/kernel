#include "kernel/descriptor.hpp"

#include "drivers/sensors/sensor.hpp"

namespace descriptor {
namespace sensor_descriptor {

int64_t read(process::Task&,
             DescriptorEntry& entry,
             uint64_t user_address,
             uint64_t length,
             uint64_t offset) {
    if (offset != 0 || user_address == 0 ||
        length < sizeof(descriptor_defs::SensorSample)) {
        return -1;
    }

    size_t index = reinterpret_cast<size_t>(entry.object);
    auto* sample = reinterpret_cast<descriptor_defs::SensorSample*>(user_address);
    if (!sensors::read(index, *sample)) {
        return -1;
    }
    return sizeof(*sample);
}

int64_t write(process::Task&, DescriptorEntry&, uint64_t, uint64_t, uint64_t) {
    return -1;
}

int get_property(DescriptorEntry& entry, uint32_t property, void* out, size_t size) {
    if (property != static_cast<uint32_t>(descriptor_defs::Property::SensorInfo) ||
        out == nullptr || size < sizeof(descriptor_defs::SensorInfo)) {
        return -1;
    }
    size_t index = reinterpret_cast<size_t>(entry.object);
    return sensors::info(index, *static_cast<descriptor_defs::SensorInfo*>(out))
               ? 0
               : -1;
}

const Ops kOps{
    .read = read,
    .write = write,
    .get_property = get_property,
    .set_property = nullptr,
};

bool open(process::Task&,
          uint64_t selector,
          uint64_t,
          uint64_t,
          Allocation& allocation) {
    if (selector >= sensors::count()) {
        return false;
    }
    allocation.type = kTypeSensor;
    allocation.flags = static_cast<uint64_t>(Flag::Readable) |
                       static_cast<uint64_t>(Flag::Device);
    allocation.extended_flags = 0;
    allocation.has_extended_flags = false;
    allocation.object = reinterpret_cast<void*>(static_cast<size_t>(selector));
    allocation.subsystem_data = nullptr;
    allocation.name = "sensor";
    allocation.ops = &kOps;
    allocation.ext = nullptr;
    allocation.close = nullptr;
    return true;
}

}  // namespace sensor_descriptor

bool register_sensor_descriptor() {
    return register_type(kTypeSensor, sensor_descriptor::open,
                         &sensor_descriptor::kOps);
}

}  // namespace descriptor
