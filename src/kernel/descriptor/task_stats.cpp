#include "../descriptor.hpp"

#include "../process.hpp"

namespace descriptor {

namespace task_stats_descriptor {

int64_t read(process::Task&,
             DescriptorEntry&,
             uint64_t user_address,
             uint64_t length,
             uint64_t offset) {
    if (offset != 0) {
        return -1;
    }
    if (user_address == 0 || length < sizeof(descriptor_defs::TaskUsage)) {
        return -1;
    }

    auto* out = reinterpret_cast<descriptor_defs::TaskUsage*>(user_address);
    size_t max_entries =
        static_cast<size_t>(length / sizeof(descriptor_defs::TaskUsage));
    size_t written = process::usage_snapshot(out, max_entries);
    return static_cast<int64_t>(written * sizeof(descriptor_defs::TaskUsage));
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

const Ops kTaskStatsOps{
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
    alloc.type = kTypeTaskStats;
    alloc.flags = static_cast<uint64_t>(Flag::Readable) |
                  static_cast<uint64_t>(Flag::Device);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = nullptr;
    alloc.subsystem_data = nullptr;
    alloc.name = "task_stats";
    alloc.ops = &kTaskStatsOps;
    alloc.ext = nullptr;
    alloc.close = nullptr;
    return true;
}

}  // namespace task_stats_descriptor

bool register_task_stats_descriptor() {
    return register_type(kTypeTaskStats,
                         task_stats_descriptor::open,
                         &task_stats_descriptor::kTaskStatsOps);
}

}  // namespace descriptor
