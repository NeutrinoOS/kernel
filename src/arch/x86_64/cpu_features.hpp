#pragma once

namespace cpu {

constexpr unsigned int kFpuStateSize = 512;
constexpr unsigned int kFpuStateAlign = 16;

struct FeatureState {
    bool mmx;
    bool sse;
    bool sse2;
    bool pge;
    bool pcid;
};

const FeatureState& feature_state();
bool init_boot_features();
void init_current_cpu_features();
void init_fpu_state(void* state);
void save_fpu_state(void* state);
void restore_fpu_state(const void* state);
bool kernel_fpu_begin();
void kernel_fpu_end();

}  // namespace cpu
