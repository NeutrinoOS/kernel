#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel_module {

using InitFn = bool (*)();
using PciDriverInitFn = void (*)();

enum class Phase : uint8_t {
    Core = 0,
    Bus = 1,
    Driver = 2,
    Late = 3,
};

struct PciMatch {
    uint16_t vendor;
    uint16_t device;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
};

constexpr uint32_t kDescriptorAbiVersion = 2;
constexpr uint16_t kAnyVendor = 0xFFFFu;
constexpr uint16_t kAnyDevice = 0xFFFFu;
constexpr uint8_t kAnyClass = 0xFFu;
constexpr uint8_t kAnySubclass = 0xFFu;
constexpr uint8_t kAnyProgIf = 0xFFu;

struct Descriptor {
    uint32_t abi_version;
    const char* name;
    Phase phase;
    InitFn init;
    const PciMatch* pci_matches;
    size_t pci_match_count;
};

struct Api {
    uint32_t abi_version;
    void (*log)(uint8_t level, const char* message);
    bool (*register_pci_driver)(const char* name,
                                const PciMatch* matches,
                                size_t match_count,
                                PciDriverInitFn init);
};

enum ModuleInfoFlag : uint32_t {
    kModuleInfoBuiltin = 1u << 0,
    kModuleInfoDynamic = 1u << 1,
};

struct ModuleInfo {
    char name[64];
    char path[128];
    uint64_t image_size;
    uint32_t flags;
    uint32_t reserved;
};

size_t count();
bool initialize_phase(Phase phase);
bool load_from_file(const char* path);
size_t loaded_count();
bool info_at(size_t index, ModuleInfo& out_info);

}  // namespace kernel_module

#define KERNEL_BUILTIN_MODULE(symbol, module_name, module_phase, module_init, \
                              module_pci_matches, module_pci_match_count)    \
    extern "C" [[gnu::used, gnu::section(".kernel_modules"),                 \
                  gnu::aligned(8)]]                                           \
    const kernel_module::Descriptor symbol = {                                \
        .abi_version = kernel_module::kDescriptorAbiVersion,                  \
        .name = module_name,                                                  \
        .phase = module_phase,                                                \
        .init = module_init,                                                   \
        .pci_matches = module_pci_matches,                                    \
        .pci_match_count = module_pci_match_count,                            \
    }

#define KERNEL_DYNAMIC_MODULE_DESCRIPTOR(module_name, module_phase,            \
                                         module_pci_matches,                   \
                                         module_pci_match_count)               \
    extern "C" [[gnu::used, gnu::visibility("default")]]                     \
    const kernel_module::Descriptor neutrino_module_descriptor = {             \
        .abi_version = kernel_module::kDescriptorAbiVersion,                   \
        .name = module_name,                                                   \
        .phase = module_phase,                                                 \
        .init = nullptr,                                                       \
        .pci_matches = module_pci_matches,                                     \
        .pci_match_count = module_pci_match_count,                             \
    }
