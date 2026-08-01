#include "../descriptor.hpp"

#include "../../drivers/serial/serial.hpp"

namespace descriptor {

namespace serial_descriptor {

int64_t serial_read(process::Task&,
                    DescriptorEntry&,
                    uint64_t user_address,
                    uint64_t length,
                    uint64_t offset) {
    if (offset != 0) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    auto* buffer = reinterpret_cast<char*>(user_address);
    if (buffer == nullptr) {
        return -1;
    }
    size_t to_read = static_cast<size_t>(length);
    size_t read_count = serial::read(buffer, to_read);
    return static_cast<int64_t>(read_count);
}

int64_t serial_write(process::Task&,
                     DescriptorEntry&,
                     uint64_t user_address,
                     uint64_t length,
                     uint64_t offset) {
    if (offset != 0) {
        return -1;
    }
    const char* data = reinterpret_cast<const char*>(user_address);
    if (data == nullptr || length == 0) {
        return 0;
    }
    size_t to_write = static_cast<size_t>(length);
    serial::write(data, to_write);
    return static_cast<int64_t>(to_write);
}

const Ops kSerialOps{
    .read = serial_read,
    .write = serial_write,
    .get_property = nullptr,
    .set_property = nullptr,
};

bool open_serial(process::Task&,
                 uint64_t,
                 uint64_t,
                 uint64_t,
                 Allocation& alloc) {
    serial::init();
    alloc.type = kTypeSerial;
    alloc.flags = static_cast<uint64_t>(Flag::Readable) |
                  static_cast<uint64_t>(Flag::Writable);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = nullptr;
    alloc.close = nullptr;
    alloc.name = "serial";
    alloc.ops = &kSerialOps;
    return true;
}

}  // namespace serial_descriptor

bool register_serial_descriptor() {
    return register_type(kTypeSerial, serial_descriptor::open_serial, &serial_descriptor::kSerialOps);
}

}  // namespace descriptor
