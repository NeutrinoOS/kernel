#include "../descriptor.hpp"

#include "../../arch/x86_64/percpu.hpp"

namespace descriptor {

namespace cpu_stats_descriptor {

int64_t read(process::Task&,
             DescriptorEntry&,
             uint64_t user_address,
             uint64_t length,
             uint64_t offset) {
    if (offset != 0) {
        return -1;
    }
    if (user_address == 0 || length < sizeof(descriptor_defs::CpuUsage)) {
        return -1;
    }
    auto* out = reinterpret_cast<descriptor_defs::CpuUsage*>(user_address);
    size_t max_entries = static_cast<size_t>(length / sizeof(descriptor_defs::CpuUsage));
    size_t written = percpu::usage_snapshot(out, max_entries);
    return static_cast<int64_t>(written * sizeof(descriptor_defs::CpuUsage));
}

int64_t write(process::Task&,
              DescriptorEntry&,
              uint64_t,
              uint64_t,
              uint64_t) {
    return -1;
}

int get_property(DescriptorEntry&,
                 uint32_t,
                 void*,
                 size_t) {
    return -1;
}

const Ops kCpuStatsOps{
    .read = read,
    .write = write,
    .get_property = get_property,
    .set_property = nullptr,
};

bool open(process::Task&,
          uint64_t,
          uint64_t,
          uint64_t,
          Allocation& alloc) {
    alloc.type = kTypeCpuStats;
    alloc.flags = static_cast<uint64_t>(Flag::Readable) |
                 static_cast<uint64_t>(Flag::Device);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = nullptr;
    alloc.subsystem_data = nullptr;
    alloc.name = "cpu_stats";
    alloc.ops = &kCpuStatsOps;
    alloc.ext = nullptr;
    alloc.close = nullptr;
    return true;
}

}  // namespace cpu_stats_descriptor

bool register_cpu_stats_descriptor() {
    return register_type(kTypeCpuStats,
                         cpu_stats_descriptor::open,
                         &cpu_stats_descriptor::kCpuStatsOps);
}

}  // namespace descriptor
