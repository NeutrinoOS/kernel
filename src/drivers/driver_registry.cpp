#include "drivers/driver_registry.hpp"

#include "drivers/log/logging.hpp"
#include "drivers/pci/pci.hpp"
#include "kernel/process.hpp"
#include "kernel/scheduler.hpp"

namespace driver_registry {

namespace {

struct PciDriverEntry {
    const char* name;
    const PciMatch* matches;
    size_t match_count;
    InitFn init;
    bool used;
    bool probed;
};

constexpr size_t kMaxPciDrivers = 32;
PciDriverEntry g_pci_drivers[kMaxPciDrivers]{};
process::Task* g_pci_probe_worker = nullptr;
bool g_pci_probe_worker_started = false;

bool match_field(uint32_t actual, uint32_t expected, uint32_t any_value) {
    return expected == any_value || actual == expected;
}

bool pci_device_matches(const pci::PciDevice& device, const PciMatch& match) {
    return match_field(device.vendor, match.vendor, kAnyVendor) &&
           match_field(device.device, match.device, kAnyDevice) &&
           match_field(device.class_code, match.class_code, kAnyClass) &&
           match_field(device.subclass, match.subclass, kAnySubclass) &&
           match_field(device.prog_if, match.prog_if, kAnyProgIf);
}

bool driver_matches_any_pci_device(const PciDriverEntry& entry) {
    const pci::PciDevice* devices = pci::devices();
    size_t device_count = pci::device_count();
    for (size_t i = 0; i < device_count; ++i) {
        for (size_t j = 0; j < entry.match_count; ++j) {
            if (pci_device_matches(devices[i], entry.matches[j])) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

bool register_pci_driver(const char* name,
                         const PciMatch* matches,
                         size_t match_count,
                         InitFn init) {
    if (name == nullptr || matches == nullptr || match_count == 0 || init == nullptr) {
        return false;
    }

    for (size_t i = 0; i < kMaxPciDrivers; ++i) {
        if (g_pci_drivers[i].used) {
            continue;
        }
        g_pci_drivers[i] = PciDriverEntry{
            .name = name,
            .matches = matches,
            .match_count = match_count,
            .init = init,
            .used = true,
            .probed = false,
        };
        log_message(LogLevel::Info,
                    "DriverRegistry: registered PCI driver %s",
                    name);
        return true;
    }
    return false;
}

void probe_pci_drivers() {
    for (size_t i = 0; i < kMaxPciDrivers; ++i) {
        PciDriverEntry& entry = g_pci_drivers[i];
        if (!entry.used || entry.init == nullptr || entry.probed) {
            continue;
        }
        if (!driver_matches_any_pci_device(entry)) {
            entry.probed = true;
            continue;
        }
        log_message(LogLevel::Info, "Initializing %s", entry.name);
        entry.init();
        entry.probed = true;
    }
}

namespace {

void pci_probe_worker(process::Task& proc) {
    for (size_t i = 0; i < kMaxPciDrivers; ++i) {
        PciDriverEntry& entry = g_pci_drivers[i];
        if (!entry.used || entry.init == nullptr || entry.probed) {
            continue;
        }

        entry.probed = true;
        if (!driver_matches_any_pci_device(entry)) {
            log_message(LogLevel::Debug,
                        "DriverRegistry: no PCI match for %s",
                        entry.name);
            process::store_state(proc, process::State::Ready);
            return;
        }

        log_message(LogLevel::Info, "Initializing %s", entry.name);
        entry.init();
        process::store_state(proc, process::State::Ready);
        return;
    }

    process::store_state(proc, process::State::Blocked);
}

}  // namespace

void start_pci_probe_worker() {
    if (g_pci_probe_worker_started) {
        return;
    }
    g_pci_probe_worker_started = true;

    process::Task* worker = process::allocate_kernel_task(pci_probe_worker);
    if (worker == nullptr) {
        log_message(LogLevel::Warn,
                    "DriverRegistry: failed to allocate PCI probe worker");
        return;
    }
    g_pci_probe_worker = worker;
    scheduler::enqueue(worker);
    log_message(LogLevel::Info, "DriverRegistry: PCI probe worker started");
}

}  // namespace driver_registry
