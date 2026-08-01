#include "../descriptor.hpp"

#include "../../drivers/log/logging.hpp"

namespace descriptor {
namespace kernel_log_descriptor {

int64_t read(process::Task&,
             DescriptorEntry&,
             uint64_t user_address,
             uint64_t length,
             uint64_t offset) {
    if (user_address == 0 || length == 0) {
        return length == 0 ? 0 : -1;
    }
    size_t bytes = log_read_recent(reinterpret_cast<char*>(user_address),
                                   static_cast<size_t>(length),
                                   static_cast<size_t>(offset));
    return static_cast<int64_t>(bytes);
}

int64_t write(process::Task&, DescriptorEntry&, uint64_t, uint64_t, uint64_t) {
    return -1;
}

int get_property(DescriptorEntry&, uint32_t, void*, size_t) {
    return -1;
}

const Ops kKernelLogOps{
    .read = read,
    .write = write,
    .get_property = get_property,
    .set_property = nullptr,
};

bool open(process::Task&, uint64_t, uint64_t, uint64_t, Allocation& alloc) {
    alloc.type = kTypeKernelLog;
    alloc.flags = static_cast<uint64_t>(Flag::Readable) |
                  static_cast<uint64_t>(Flag::Seekable);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = nullptr;
    alloc.subsystem_data = nullptr;
    alloc.name = "kernel-log";
    alloc.ops = &kKernelLogOps;
    alloc.ext = nullptr;
    alloc.close = nullptr;
    return true;
}

}  // namespace kernel_log_descriptor

bool register_kernel_log_descriptor() {
    return register_type(kTypeKernelLog,
                         kernel_log_descriptor::open,
                         &kernel_log_descriptor::kKernelLogOps);
}

}  // namespace descriptor
