#include "../descriptor.hpp"

#include "../../arch/x86_64/cpu_features.hpp"
#include "../../arch/x86_64/percpu.hpp"
#include "../../lib/mem.hpp"

namespace descriptor {

namespace cpu_info_descriptor {

int64_t read(process::Task&,
             DescriptorEntry&,
             uint64_t user_address,
             uint64_t length,
             uint64_t offset) {
    if (user_address == 0) {
        return -1;
    }

    if (offset == 0) {
        if (length < sizeof(descriptor_defs::CpuInfo)) return -1;
        descriptor_defs::CpuInfo info = cpu::info();
        info.logical_cpus = static_cast<uint32_t>(percpu::cpu_count());
        *reinterpret_cast<descriptor_defs::CpuInfo*>(user_address) = info;
        return sizeof(info);
    }

    if (offset < sizeof(descriptor_defs::CpuInfo)) return -1;
    const uint64_t feature_offset = offset - sizeof(descriptor_defs::CpuInfo);
    const uint64_t feature_bytes =
        (static_cast<uint64_t>(descriptor_defs::kCpuFeatureCount) + 7) / 8;
    if (feature_offset >= feature_bytes) return 0;

    uint64_t bytes = length;
    if (bytes > feature_bytes - feature_offset) bytes = feature_bytes - feature_offset;
    auto* out = reinterpret_cast<uint8_t*>(user_address);
    memset(out, 0, static_cast<size_t>(bytes));
    for (uint32_t feature = 0; feature < descriptor_defs::kCpuFeatureCount; ++feature) {
        const uint64_t byte = feature / 8;
        if (byte < feature_offset || byte >= feature_offset + bytes) continue;
        if (cpu::has_feature(static_cast<descriptor_defs::CpuFeature>(feature))) {
            out[byte - feature_offset] |= static_cast<uint8_t>(1u << (feature % 8));
        }
    }
    return static_cast<int64_t>(bytes);
}

int64_t write(process::Task&, DescriptorEntry&, uint64_t, uint64_t, uint64_t) {
    return -1;
}

int get_property(DescriptorEntry&, uint32_t, void*, size_t) {
    return -1;
}

const Ops kCpuInfoOps{
    .read = read,
    .write = write,
    .get_property = get_property,
    .set_property = nullptr,
};

bool open(process::Task&, uint64_t, uint64_t, uint64_t, Allocation& alloc) {
    alloc.type = kTypeCpuInfo;
    alloc.flags = static_cast<uint64_t>(Flag::Readable) |
                  static_cast<uint64_t>(Flag::Device);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = nullptr;
    alloc.subsystem_data = nullptr;
    alloc.name = "cpu_info";
    alloc.ops = &kCpuInfoOps;
    alloc.ext = nullptr;
    alloc.close = nullptr;
    return true;
}

}  // namespace cpu_info_descriptor

bool register_cpu_info_descriptor() {
    return register_type(kTypeCpuInfo,
                         cpu_info_descriptor::open,
                         &cpu_info_descriptor::kCpuInfoOps);
}

}  // namespace descriptor
