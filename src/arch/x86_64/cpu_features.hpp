#pragma once

#include "descriptors.hpp"

namespace cpu {

constexpr unsigned int kFpuStateSize = 512;
constexpr unsigned int kFpuStateAlign = 16;
constexpr unsigned int kCpuFeatureWordCount = 4;

struct FeatureState {
    bool mmx;
    bool sse;
    bool sse2;
    bool pge;
    bool pcid;
};

const FeatureState& feature_state();
const descriptor_defs::CpuInfo& info();
bool has_feature(descriptor_defs::CpuFeature feature);
bool init_boot_features();
void init_current_cpu_features();
void init_fpu_state(void* state);
void save_fpu_state(void* state);
void restore_fpu_state(const void* state);
bool kernel_fpu_begin();
void kernel_fpu_end();

}  // namespace cpu
