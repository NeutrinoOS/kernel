#include "xhci.hpp"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/memory/paging.hpp"
#include "drivers/driver_registry.hpp"
#include "drivers/log/logging.hpp"
#include "drivers/pci/pci.hpp"
#include "drivers/usb/usb_core.hpp"
#include "kernel/error.hpp"
#include "kernel/memory/physical_allocator.hpp"
#include "kernel/module.hpp"
#include "kernel/process.hpp"
#include "kernel/scheduler.hpp"
#include "lib/mem.hpp"

namespace xhci {
namespace {

constexpr uint64_t kPageSize = 0x1000;
constexpr uint64_t kMmioVirtBase = 0xffff900200000000ull;
constexpr uint64_t kMmioWindowSize = 16ull * 1024 * 1024;
constexpr uint64_t kControllerMmioSize = 0x10000;
constexpr size_t kMaxControllers = 4;
constexpr uint32_t kSpinTimeout = 5000000;
constexpr uint32_t kHaltSpinTimeout = 10000;
constexpr uint32_t kCommandSpinTimeout = kSpinTimeout * 2;
constexpr uint32_t kTransferSpinTimeout = kSpinTimeout * 4;

constexpr uint32_t kUsbCmdRunStop = 1u << 0;
constexpr uint32_t kUsbCmdHostControllerReset = 1u << 1;
constexpr uint32_t kUsbStsHalted = 1u << 0;
constexpr uint32_t kUsbStsControllerNotReady = 1u << 11;
constexpr uint8_t kXhciExtCapLegacySupport = 1;
constexpr uint8_t kPciCapabilityPowerManagement = 0x01;
constexpr uint32_t kUsbLegacyBiosOwned = 1u << 16;
constexpr uint32_t kUsbLegacyOsOwned = 1u << 24;

constexpr uint32_t kPortScCurrentConnectStatus = 1u << 0;
constexpr uint32_t kPortScPortEnabled = 1u << 1;
constexpr uint32_t kPortScPortReset = 1u << 4;
constexpr uint32_t kPortScPortPower = 1u << 9;
constexpr uint32_t kPortScChangeBits = (1u << 17) | (1u << 18) | (1u << 20) |
                                       (1u << 21) | (1u << 22) | (1u << 23);

constexpr uint32_t kTrbTypeNormal = 1;
constexpr uint32_t kTrbTypeSetupStage = 2;
constexpr uint32_t kTrbTypeDataStage = 3;
constexpr uint32_t kTrbTypeStatusStage = 4;
constexpr uint32_t kTrbTypeLink = 6;
constexpr uint32_t kTrbTypeEnableSlotCommand = 9;
constexpr uint32_t kTrbTypeDisableSlotCommand = 10;
constexpr uint32_t kTrbTypeAddressDeviceCommand = 11;
constexpr uint32_t kTrbTypeConfigureEndpointCommand = 12;
constexpr uint32_t kTrbTypeEvaluateContextCommand = 13;
constexpr uint32_t kTrbTypeResetEndpointCommand = 14;
constexpr uint32_t kTrbTypeSetTrDequeuePointerCommand = 16;
constexpr uint32_t kTrbTypeTransferEvent = 32;
constexpr uint32_t kTrbTypeCommandCompletionEvent = 33;
constexpr uint32_t kTrbTypePortStatusChangeEvent = 34;
constexpr uint32_t kTrbCycle = 1u << 0;
constexpr uint32_t kTrbToggleCycle = 1u << 1;
constexpr uint32_t kTrbChain = 1u << 4;
constexpr uint32_t kTrbInterruptOnCompletion = 1u << 5;
constexpr uint32_t kTrbImmediateData = 1u << 6;
constexpr uint32_t kTrbDataStageDirectionIn = 1u << 16;

constexpr uint8_t kCompletionSuccess = 1;
constexpr uint8_t kCompletionStall = 6;
constexpr uint8_t kCompletionShortPacket = 13;

constexpr size_t kRingPageCount = 1;
constexpr size_t kTrbsPerPage = kPageSize / 16;
// HCSParams2 encodes up to 1023 scratchpad buffers. Current hardware in the
// field already exceeds 32 (Intel 9d2f reports 34), while the pointer array
// still fits comfortably in one page at this limit.
constexpr size_t kMaxScratchpadBuffers = 64;
constexpr size_t kMaxDeviceSlots = 32;
constexpr size_t kMaxEndpointContexts = 32;
constexpr size_t kMaxConfigDescriptorBytes = 512;
constexpr uint32_t kContextEntriesMask = 0x1Fu << 27;

constexpr driver_registry::PciMatch kPciMatches[] = {
    {driver_registry::kAnyVendor,
     driver_registry::kAnyDevice,
     0x0C,
     0x03,
     0x30},
};

struct CapabilityInfo {
    uint8_t cap_length;
    uint16_t version;
    uint8_t max_slots;
    uint16_t max_interrupters;
    uint8_t max_ports;
    uint16_t max_scratchpad_buffers;
    bool context_size_64;
    uint32_t doorbell_offset;
    uint32_t runtime_offset;
    uint16_t extended_capabilities_offset;
};

struct Controller {
    pci::PciDevice pci_device;
    volatile uint8_t* capability;
    volatile uint8_t* operational;
    volatile uint8_t* runtime;
    volatile uint8_t* doorbells;
    CapabilityInfo caps;
    uint64_t dcbaa_phys;
    uint64_t command_ring_phys;
    uint64_t event_ring_phys;
    uint64_t erst_phys;
    uint64_t scratchpad_array_phys;
    uint64_t scratchpad_phys[kMaxScratchpadBuffers];
    uint8_t command_cycle;
    size_t command_enqueue;
    uint8_t event_cycle;
    size_t event_dequeue;
    volatile int io_lock;
    volatile uint8_t command_quarantined;
    bool port_enumeration_attempted[256];
    bool active;
};

struct Trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
};

struct EventRingSegmentTableEntry {
    uint64_t base_address;
    uint32_t segment_size;
    uint32_t reserved;
};

struct Ring {
    uint64_t phys;
    uint8_t cycle;
    size_t enqueue;
};

struct DeviceSlot {
    volatile uint8_t allocated;
    volatile uint8_t used;
    volatile uint8_t quarantined;
    Controller* controller;
    uint8_t slot_id;
    uint8_t address;
    uint8_t port;
    uint8_t route_depth;
    uint8_t tt_hub_slot_id;
    uint8_t tt_port;
    uint32_t route_string;
    usb::Speed speed;
    uint64_t input_context_phys;
    uint64_t output_context_phys;
    uint64_t quarantined_dma_phys;
    Ring endpoint_rings[kMaxEndpointContexts];
    usb::Device device;
};

Controller g_controllers[kMaxControllers]{};
size_t g_controller_count = 0;
uint64_t g_mmio_next_virt = kMmioVirtBase;
bool g_initialized = false;
DeviceSlot g_device_slots[kMaxDeviceSlots]{};
uint8_t g_next_usb_address = 1;
process::Task* g_enumeration_worker = nullptr;
Controller* g_pending_controller = nullptr;
uint8_t g_pending_port = 0;
volatile uint8_t g_enumeration_pending = 0;
volatile int g_enumeration_request_lock = 0;

usb::TransferStatus enumerate_hub_port(void* context,
                                       uint8_t downstream_port,
                                       usb::Speed speed);

void cpu_relax() {
    asm volatile("pause");
}

void lock_controller_io(Controller& controller) {
    while (__atomic_test_and_set(&controller.io_lock, __ATOMIC_ACQUIRE)) {
        cpu_relax();
    }
}

void unlock_controller_io(Controller& controller) {
    __atomic_clear(&controller.io_lock, __ATOMIC_RELEASE);
}

class ControllerIoGuard {
public:
    explicit ControllerIoGuard(Controller& controller)
        : controller_(controller) {
        lock_controller_io(controller_);
    }
    ~ControllerIoGuard() { unlock_controller_io(controller_); }

    ControllerIoGuard(const ControllerIoGuard&) = delete;
    ControllerIoGuard& operator=(const ControllerIoGuard&) = delete;

private:
    Controller& controller_;
};

void lock_enumeration_request() {
    while (__atomic_test_and_set(&g_enumeration_request_lock,
                                 __ATOMIC_ACQUIRE)) {
        cpu_relax();
    }
}

void unlock_enumeration_request() {
    __atomic_clear(&g_enumeration_request_lock, __ATOMIC_RELEASE);
}

uint64_t align_down_u64(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

uint64_t align_up_u64(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

uint32_t read32(volatile uint8_t* base, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t*>(base + offset);
}

void write32(volatile uint8_t* base, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(base + offset) = value;
}

void write64(volatile uint8_t* base, uint32_t offset, uint64_t value) {
    *reinterpret_cast<volatile uint64_t*>(base + offset) = value;
}

uint32_t trb_type(const Trb& trb) {
    return (trb.control >> 10) & 0x3Fu;
}

uint8_t completion_code(const Trb& trb) {
    return static_cast<uint8_t>((trb.status >> 24) & 0xFFu);
}

void fence() {
    asm volatile("mfence" ::: "memory");
}

uint64_t pci_bar_base(const pci::PciDevice& device, uint8_t bar_index) {
    if (bar_index >= 6) {
        return 0;
    }

    uint8_t reg = static_cast<uint8_t>(0x10 + (bar_index * 4));
    uint32_t low = pci::read_config32(device, reg);
    if ((low & 0x1u) != 0) {
        return 0;
    }

    uint64_t base = static_cast<uint64_t>(low & ~0xFu);
    uint32_t bar_type = (low >> 1) & 0x3u;
    if (bar_type == 0x2u) {
        if (bar_index + 1 >= 6) {
            return 0;
        }
        uint32_t high = pci::read_config32(device, static_cast<uint8_t>(reg + 4));
        base |= static_cast<uint64_t>(high) << 32;
    }
    return base;
}

volatile uint8_t* map_mmio_range(uint64_t phys_base, uint64_t length) {
    if (phys_base == 0 || length == 0) {
        return nullptr;
    }
    if (length > UINT64_MAX - phys_base ||
        phys_base + length > UINT64_MAX - (kPageSize - 1)) {
        return nullptr;
    }

    uint64_t page_phys = align_down_u64(phys_base, kPageSize);
    uint64_t page_end = align_up_u64(phys_base + length, kPageSize);
    size_t page_count =
        static_cast<size_t>((page_end - page_phys) / kPageSize);
    if (page_count == 0) {
        page_count = 1;
    }

    uint64_t virt_base = g_mmio_next_virt;
    uint64_t virt_end =
        virt_base + static_cast<uint64_t>(page_count) * kPageSize;
    if (virt_end - kMmioVirtBase > kMmioWindowSize) {
        log_message(LogLevel::Warn,
                    "xhci: MMIO window exhausted while mapping %016llx",
                    static_cast<unsigned long long>(phys_base));
        return nullptr;
    }

    const uint64_t flags = PAGE_FLAG_WRITE | PAGE_FLAG_WRITE_THROUGH |
                           PAGE_FLAG_CACHE_DISABLE | PAGE_FLAG_NO_EXECUTE;
    for (size_t i = 0; i < page_count; ++i) {
        uint64_t phys = page_phys + static_cast<uint64_t>(i) * kPageSize;
        uint64_t virt = virt_base + static_cast<uint64_t>(i) * kPageSize;
        if (!paging_map_page(virt, phys, flags)) {
            log_message(LogLevel::Warn,
                        "xhci: failed to map MMIO page phys=%016llx",
                        static_cast<unsigned long long>(phys));
            return nullptr;
        }
    }

    g_mmio_next_virt = virt_end;
    return reinterpret_cast<volatile uint8_t*>(virt_base + (phys_base - page_phys));
}

uint64_t alloc_zeroed_pages(size_t page_count) {
    if (page_count == 0) {
        return 0;
    }
    uint64_t phys = memory::alloc_kernel_block_pages(page_count);
    if (phys == 0) {
        return 0;
    }
    void* virt = paging_phys_to_virt(phys);
    if (virt == nullptr) {
        memory::free_kernel_block(phys);
        return 0;
    }
    memset(virt, 0, page_count * kPageSize);
    return phys;
}

CapabilityInfo read_capabilities(volatile uint8_t* mmio) {
    CapabilityInfo info{};
    uint32_t capability_header = read32(mmio, 0x00);
    info.cap_length = static_cast<uint8_t>(capability_header & 0xFFu);
    info.version = static_cast<uint16_t>((capability_header >> 16) & 0xFFFFu);

    uint32_t hcsparams1 = read32(mmio, 0x04);
    uint32_t hcsparams2 = read32(mmio, 0x08);
    uint32_t hccparams1 = read32(mmio, 0x10);
    info.doorbell_offset = read32(mmio, 0x14) & ~0x3u;
    info.runtime_offset = read32(mmio, 0x18) & ~0x1Fu;
    info.extended_capabilities_offset =
        static_cast<uint16_t>((hccparams1 >> 16) & 0xFFFFu);

    info.max_slots = static_cast<uint8_t>(hcsparams1 & 0xFFu);
    info.max_interrupters = static_cast<uint16_t>((hcsparams1 >> 8) & 0x7FFu);
    info.max_ports = static_cast<uint8_t>((hcsparams1 >> 24) & 0xFFu);

    // HCSPARAMS2 stores the low five bits in 31:27 and the high five
    // bits in 25:21. Keeping their names explicit avoids silently turning
    // (for example) four scratchpads into 128.
    uint16_t scratch_low = static_cast<uint16_t>((hcsparams2 >> 27) & 0x1Fu);
    uint16_t scratch_high = static_cast<uint16_t>((hcsparams2 >> 21) & 0x1Fu);
    info.max_scratchpad_buffers =
        static_cast<uint16_t>(scratch_low | (scratch_high << 5));
    info.context_size_64 = (hccparams1 & (1u << 2)) != 0;
    return info;
}

void claim_bios_ownership(volatile uint8_t* capability,
                          uint16_t extended_capabilities_offset,
                          uint32_t mapped_bytes) {
    uint32_t offset = static_cast<uint32_t>(extended_capabilities_offset) * 4u;
    for (size_t guard = 0; offset != 0 && guard < 64; ++guard) {
        if (offset > mapped_bytes - sizeof(uint32_t)) {
            log_message(LogLevel::Warn,
                        "xhci: extended capability offset out of range");
            return;
        }
        uint32_t value = read32(capability, offset);
        uint8_t capability_id = static_cast<uint8_t>(value & 0xFFu);
        uint8_t next_offset = static_cast<uint8_t>((value >> 8) & 0xFFu);

        if (capability_id == kXhciExtCapLegacySupport) {
            write32(capability, offset, value | kUsbLegacyOsOwned);
            for (uint32_t spins = 0; spins < kSpinTimeout; ++spins) {
                if ((read32(capability, offset) & kUsbLegacyBiosOwned) == 0) {
                    return;
                }
                cpu_relax();
            }
            log_message(LogLevel::Warn,
                        "xhci: BIOS ownership handoff timed out");
            return;
        }

        if (next_offset == 0) {
            return;
        }
        uint32_t step = static_cast<uint32_t>(next_offset) * 4u;
        if (step > UINT32_MAX - offset) {
            return;
        }
        offset += step;
    }
}

bool wait_status_clear(volatile uint8_t* operational, uint32_t bits) {
    for (uint32_t spins = 0; spins < kSpinTimeout; ++spins) {
        if ((read32(operational, 0x04) & bits) == 0) {
            return true;
        }
        cpu_relax();
    }
    return (read32(operational, 0x04) & bits) == 0;
}

bool wait_command_clear(volatile uint8_t* operational, uint32_t bits) {
    for (uint32_t spins = 0; spins < kSpinTimeout; ++spins) {
        if ((read32(operational, 0x00) & bits) == 0) {
            return true;
        }
        cpu_relax();
    }
    return (read32(operational, 0x00) & bits) == 0;
}

uint32_t port_offset(uint8_t port) {
    return 0x400 + (static_cast<uint32_t>(port - 1) * 0x10);
}

void write_portsc(Controller& controller, uint8_t port, uint32_t value) {
    value &= ~kPortScChangeBits;
    write32(controller.operational, port_offset(port), value);
}

void clear_port_change_bits(Controller& controller, uint8_t port, uint32_t value) {
    write32(controller.operational,
            port_offset(port),
            (value & kPortScChangeBits) | (value & kPortScPortPower));
}

bool halt_controller(volatile uint8_t* operational) {
    if ((read32(operational, 0x04) & kUsbStsHalted) != 0) {
        return true;
    }

    uint32_t command = read32(operational, 0x00);
    command &= ~kUsbCmdRunStop;
    write32(operational, 0x00, command);

    if ((read32(operational, 0x04) & kUsbStsHalted) != 0) {
        return true;
    }

    for (uint32_t spins = 0; spins < kHaltSpinTimeout; ++spins) {
        if ((read32(operational, 0x04) & kUsbStsHalted) != 0) {
            return true;
        }
        cpu_relax();
    }
    return (read32(operational, 0x04) & kUsbStsHalted) != 0;
}

bool reset_controller(volatile uint8_t* operational) {
    if (!halt_controller(operational)) {
        uint32_t command = read32(operational, 0x00);
        uint32_t status = read32(operational, 0x04);
        if ((status & kUsbStsHalted) != 0) {
            log_message(LogLevel::Info,
                        "xhci: halt completed after timeout window");
        } else {
            log_message(LogLevel::Warn,
                        "xhci: halt timed out usbcmd=%x usbsts=%x",
                        static_cast<unsigned long long>(command),
                        static_cast<unsigned long long>(status));
            return false;
        }
    }

    uint32_t command = read32(operational, 0x00);
    write32(operational, 0x00, command | kUsbCmdHostControllerReset);
    if (!wait_command_clear(operational, kUsbCmdHostControllerReset)) {
        command = read32(operational, 0x00);
        uint32_t status = read32(operational, 0x04);
        if ((command & kUsbCmdHostControllerReset) == 0) {
            log_message(LogLevel::Info,
                        "xhci: reset command completed after timeout window");
        } else {
            log_message(LogLevel::Warn,
                        "xhci: reset command stuck usbcmd=%x usbsts=%x",
                        static_cast<unsigned long long>(command),
                        static_cast<unsigned long long>(status));
            return false;
        }
    }
    if (!wait_status_clear(operational, kUsbStsControllerNotReady)) {
        uint32_t command = read32(operational, 0x00);
        uint32_t status = read32(operational, 0x04);
        if ((status & kUsbStsControllerNotReady) == 0) {
            log_message(LogLevel::Info,
                        "xhci: controller ready after timeout window");
        } else {
            log_message(LogLevel::Warn,
                        "xhci: controller not ready stuck usbcmd=%x usbsts=%x",
                        static_cast<unsigned long long>(command),
                        static_cast<unsigned long long>(status));
            return false;
        }
    }
    return true;
}

bool start_controller(volatile uint8_t* operational) {
    write32(operational, 0x04, read32(operational, 0x04));
    uint32_t command = read32(operational, 0x00);
    command |= kUsbCmdRunStop;
    write32(operational, 0x00, command);
    if (!wait_status_clear(operational, kUsbStsHalted)) {
        log_message(LogLevel::Warn,
                    "xhci: start timed out usbcmd=%x usbsts=%x",
                    static_cast<unsigned long long>(read32(operational, 0x00)),
                    static_cast<unsigned long long>(read32(operational, 0x04)));
        return false;
    }
    return true;
}

void enable_pci_device(const pci::PciDevice& device) {
    uint8_t pm_cap = pci::find_capability(device,
                                          kPciCapabilityPowerManagement);
    if (pm_cap != 0) {
        uint8_t pmcsr_offset = static_cast<uint8_t>(pm_cap + 4);
        uint16_t pmcsr = pci::read_config16(device, pmcsr_offset);
        uint16_t power_state = static_cast<uint16_t>(pmcsr & 0x3u);
        if (power_state != 0) {
            pci::write_config16(device,
                                pmcsr_offset,
                                static_cast<uint16_t>(pmcsr & ~0x3u));

            // Give D3hot -> D0 a bounded settling interval without relying
            // on timer interrupts being enabled in the PCI probe context.
            for (uint32_t spins = 0; spins < kSpinTimeout; ++spins) {
                cpu_relax();
            }
            log_message(LogLevel::Info,
                        "xhci: powered %02u:%02u.%u from D%u to D0",
                        device.bus,
                        device.slot,
                        device.function,
                        static_cast<unsigned int>(power_state));
        }
    }

    uint16_t command = pci::read_config16(device, 0x04);
    command |= static_cast<uint16_t>((1u << 1) | (1u << 2));
    // This driver polls the event ring and has no IRQ handler yet. Disable
    // legacy INTx so pending port-change events cannot storm as soon as the
    // scheduler enables interrupts.
    command |= static_cast<uint16_t>(1u << 10);
    pci::write_config16(device, 0x04, command);
}

bool setup_scratchpads(Controller& controller) {
    if (controller.caps.max_scratchpad_buffers == 0) {
        return true;
    }
    if (controller.caps.max_scratchpad_buffers > kMaxScratchpadBuffers) {
        log_message(LogLevel::Warn,
                    "xhci: scratchpad count %u exceeds local limit %zu",
                    static_cast<unsigned int>(controller.caps.max_scratchpad_buffers),
                    kMaxScratchpadBuffers);
        return false;
    }

    controller.scratchpad_array_phys = alloc_zeroed_pages(1);
    if (controller.scratchpad_array_phys == 0) {
        return false;
    }

    auto* scratchpad_array =
        static_cast<uint64_t*>(paging_phys_to_virt(controller.scratchpad_array_phys));
    for (uint16_t i = 0; i < controller.caps.max_scratchpad_buffers; ++i) {
        uint64_t phys = alloc_zeroed_pages(1);
        if (phys == 0) {
            return false;
        }
        controller.scratchpad_phys[i] = phys;
        scratchpad_array[i] = phys;
    }

    auto* dcbaa = static_cast<uint64_t*>(paging_phys_to_virt(controller.dcbaa_phys));
    dcbaa[0] = controller.scratchpad_array_phys;
    return true;
}

void free_unpublished_controller_dma(Controller& controller) {
    for (size_t i = 0; i < kMaxScratchpadBuffers; ++i) {
        if (controller.scratchpad_phys[i] != 0) {
            memory::free_kernel_block(controller.scratchpad_phys[i]);
            controller.scratchpad_phys[i] = 0;
        }
    }
    if (controller.scratchpad_array_phys != 0) {
        memory::free_kernel_block(controller.scratchpad_array_phys);
        controller.scratchpad_array_phys = 0;
    }
    if (controller.erst_phys != 0) {
        memory::free_kernel_block(controller.erst_phys);
        controller.erst_phys = 0;
    }
    if (controller.event_ring_phys != 0) {
        memory::free_kernel_block(controller.event_ring_phys);
        controller.event_ring_phys = 0;
    }
    if (controller.command_ring_phys != 0) {
        memory::free_kernel_block(controller.command_ring_phys);
        controller.command_ring_phys = 0;
    }
    if (controller.dcbaa_phys != 0) {
        memory::free_kernel_block(controller.dcbaa_phys);
        controller.dcbaa_phys = 0;
    }
}

bool setup_rings(Controller& controller) {
    controller.dcbaa_phys = alloc_zeroed_pages(1);
    controller.command_ring_phys = alloc_zeroed_pages(kRingPageCount);
    controller.event_ring_phys = alloc_zeroed_pages(kRingPageCount);
    controller.erst_phys = alloc_zeroed_pages(1);
    if (controller.dcbaa_phys == 0 ||
        controller.command_ring_phys == 0 ||
        controller.event_ring_phys == 0 ||
        controller.erst_phys == 0) {
        free_unpublished_controller_dma(controller);
        return false;
    }

    if (!setup_scratchpads(controller)) {
        free_unpublished_controller_dma(controller);
        return false;
    }

    auto* command_ring =
        static_cast<Trb*>(paging_phys_to_virt(controller.command_ring_phys));
    Trb& link = command_ring[kTrbsPerPage - 1];
    link.parameter = controller.command_ring_phys;
    link.status = 0;
    link.control = (kTrbTypeLink << 10) | (1u << 1);
    controller.command_cycle = 1;
    controller.command_enqueue = 0;

    auto* erst = static_cast<EventRingSegmentTableEntry*>(
        paging_phys_to_virt(controller.erst_phys));
    erst[0].base_address = controller.event_ring_phys;
    erst[0].segment_size = static_cast<uint32_t>(kTrbsPerPage);
    erst[0].reserved = 0;

    write64(controller.operational, 0x30, controller.dcbaa_phys);
    write64(controller.operational, 0x18, controller.command_ring_phys | 1u);

    volatile uint8_t* interrupter0 = controller.runtime + 0x20;
    write32(interrupter0, 0x04, 0);
    write32(interrupter0, 0x08, 1);
    write64(interrupter0, 0x10, controller.erst_phys);
    write64(interrupter0, 0x18, controller.event_ring_phys);
    // Clear a pending interrupt, but leave Interrupt Enable clear. Command,
    // transfer, and port events are consumed through polling.
    write32(interrupter0, 0x00, 0x1u);
    controller.event_cycle = 1;
    controller.event_dequeue = 0;
    return true;
}

uint8_t context_size(const Controller& controller) {
    return controller.caps.context_size_64 ? 64 : 32;
}

uint32_t* device_context(const Controller& controller,
                         uint64_t context_phys,
                         uint8_t context_index) {
    return reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(paging_phys_to_virt(context_phys)) +
        static_cast<size_t>(context_index) * context_size(controller));
}

uint32_t* input_context(const Controller& controller,
                        uint64_t context_phys,
                        uint8_t context_index) {
    return reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(paging_phys_to_virt(context_phys)) +
        static_cast<size_t>(context_index + 1) * context_size(controller));
}

uint32_t* input_control_context(uint64_t context_phys) {
    return reinterpret_cast<uint32_t*>(paging_phys_to_virt(context_phys));
}

Trb* ring_trbs(const Ring& ring) {
    return static_cast<Trb*>(paging_phys_to_virt(ring.phys));
}

Trb* controller_command_ring(const Controller& controller) {
    return static_cast<Trb*>(paging_phys_to_virt(controller.command_ring_phys));
}

Trb* controller_event_ring(const Controller& controller) {
    return static_cast<Trb*>(paging_phys_to_virt(controller.event_ring_phys));
}

uint64_t enqueue_ring_trb(Ring& ring,
                          uint64_t parameter,
                          uint32_t status,
                          uint32_t control) {
    KERNEL_ASSERT_MSG(ring.phys != 0,
                      "xHCI endpoint ring is not allocated");
    KERNEL_ASSERT_MSG(ring.enqueue < kTrbsPerPage - 1,
                      "xHCI endpoint enqueue index is out of bounds");
    Trb* trbs = ring_trbs(ring);
    size_t index = ring.enqueue;
    trbs[index].parameter = parameter;
    trbs[index].status = status;
    trbs[index].control = control | (ring.cycle ? kTrbCycle : 0);
    uint64_t trb_phys = ring.phys + index * sizeof(Trb);

    ++ring.enqueue;
    if (ring.enqueue == kTrbsPerPage - 1) {
        Trb& link = trbs[kTrbsPerPage - 1];
        link.parameter = ring.phys;
        link.status = 0;
        link.control = (kTrbTypeLink << 10) | kTrbToggleCycle |
                       (ring.cycle ? kTrbCycle : 0);
        ring.enqueue = 0;
        ring.cycle ^= 1;
    }
    return trb_phys;
}

uint64_t enqueue_command_trb(Controller& controller,
                             uint64_t parameter,
                             uint32_t status,
                             uint32_t control) {
    KERNEL_ASSERT_MSG(controller.command_ring_phys != 0,
                      "xHCI command ring is not allocated");
    KERNEL_ASSERT_MSG(controller.command_enqueue < kTrbsPerPage - 1,
                      "xHCI command enqueue index is out of bounds");
    Trb* trbs = controller_command_ring(controller);
    size_t index = controller.command_enqueue;
    trbs[index].parameter = parameter;
    trbs[index].status = status;
    trbs[index].control = control |
                          (controller.command_cycle ? kTrbCycle : 0);
    uint64_t trb_phys = controller.command_ring_phys + index * sizeof(Trb);

    ++controller.command_enqueue;
    if (controller.command_enqueue == kTrbsPerPage - 1) {
        Trb& link = trbs[kTrbsPerPage - 1];
        link.parameter = controller.command_ring_phys;
        link.status = 0;
        link.control = (kTrbTypeLink << 10) | kTrbToggleCycle |
                       (controller.command_cycle ? kTrbCycle : 0);
        controller.command_enqueue = 0;
        controller.command_cycle ^= 1;
    }
    return trb_phys;
}

void ring_command_doorbell(Controller& controller) {
    fence();
    write32(controller.doorbells, 0, 0);
}

void ring_endpoint_doorbell(DeviceSlot& slot, uint8_t endpoint_id) {
    fence();
    write32(slot.controller->doorbells,
            static_cast<uint32_t>(slot.slot_id) * 4,
            endpoint_id);
}

void update_erdp(Controller& controller) {
    KERNEL_ASSERT_MSG(controller.event_ring_phys != 0,
                      "xHCI event ring is not allocated");
    KERNEL_ASSERT_MSG(controller.event_dequeue < kTrbsPerPage,
                      "xHCI event dequeue index is out of bounds");
    uint64_t erdp = controller.event_ring_phys +
                    controller.event_dequeue * sizeof(Trb);
    volatile uint8_t* interrupter0 = controller.runtime + 0x20;
    write64(interrupter0, 0x18, erdp | (1u << 3));
}

bool poll_event(Controller& controller, Trb& out) {
    KERNEL_ASSERT_MSG(controller.event_ring_phys != 0,
                      "xHCI event ring is not allocated");
    KERNEL_ASSERT_MSG(controller.event_dequeue < kTrbsPerPage,
                      "xHCI event dequeue index is out of bounds");
    Trb* events = controller_event_ring(controller);
    Trb& event = events[controller.event_dequeue];
    if ((event.control & kTrbCycle) != controller.event_cycle) {
        return false;
    }

    out = event;
    ++controller.event_dequeue;
    if (controller.event_dequeue == kTrbsPerPage) {
        controller.event_dequeue = 0;
        controller.event_cycle ^= 1;
    }
    update_erdp(controller);
    return true;
}

bool wait_command_completion(Controller& controller,
                             uint64_t command_trb_phys,
                             Trb& out_event) {
    for (uint32_t spins = 0; spins < kCommandSpinTimeout; ++spins) {
        Trb event{};
        if (!poll_event(controller, event)) {
            cpu_relax();
            continue;
        }
        uint32_t type = trb_type(event);
        if (type == kTrbTypeCommandCompletionEvent &&
            event.parameter == command_trb_phys) {
            out_event = event;
            return true;
        }
        if (type == kTrbTypePortStatusChangeEvent) {
            continue;
        }
    }
    return false;
}

bool wait_transfer_completion(DeviceSlot& slot,
                              uint8_t endpoint_id,
                              uint64_t expected_trb_phys,
                              size_t requested,
                              size_t& transferred,
                              usb::TransferStatus& status) {
    Controller& controller = *slot.controller;
    for (uint32_t spins = 0; spins < kTransferSpinTimeout; ++spins) {
        Trb event{};
        if (!poll_event(controller, event)) {
            cpu_relax();
            continue;
        }
        if (trb_type(event) != kTrbTypeTransferEvent) {
            continue;
        }
        uint8_t event_slot = static_cast<uint8_t>((event.control >> 24) & 0xFFu);
        uint8_t event_ep = static_cast<uint8_t>((event.control >> 16) & 0x1Fu);
        if (event_slot != slot.slot_id || event_ep != endpoint_id) {
            continue;
        }
        if (event.parameter != expected_trb_phys) {
            continue;
        }

        uint8_t code = completion_code(event);
        uint32_t residue = event.status & 0xFFFFFFu;
        transferred = requested >= residue ? requested - residue : 0;
        if (code == kCompletionSuccess || code == kCompletionShortPacket) {
            status = usb::TransferStatus::Ok;
        } else if (code == kCompletionStall) {
            status = usb::TransferStatus::Stall;
        } else {
            status = usb::TransferStatus::IoError;
        }
        return true;
    }
    transferred = 0;
    status = usb::TransferStatus::Timeout;
    return false;
}

bool issue_command_locked(Controller& controller,
                          uint64_t parameter,
                          uint32_t status,
                          uint32_t control,
                          Trb& event) {
    if (__atomic_load_n(&controller.command_quarantined,
                        __ATOMIC_ACQUIRE) != 0) {
        return false;
    }
    uint64_t trb_phys = enqueue_command_trb(controller, parameter, status, control);
    ring_command_doorbell(controller);
    if (!wait_command_completion(controller, trb_phys, event)) {
        // The controller can still own this command TRB and may report its
        // completion later.  Never wrap/reuse the command ring or submit a
        // command whose completion could be confused with that late event.
        __atomic_store_n(&controller.command_quarantined,
                         1,
                         __ATOMIC_RELEASE);
        log_message(LogLevel::Warn,
                    "xhci: quarantining controller command ring after timeout");
        return false;
    }
    return completion_code(event) == kCompletionSuccess;
}

bool issue_command(Controller& controller,
                   uint64_t parameter,
                   uint32_t status,
                   uint32_t control,
                   Trb& event) {
    ControllerIoGuard guard(controller);
    return issue_command_locked(controller, parameter, status, control, event);
}

bool controller_commands_quarantined(const Controller& controller) {
    return __atomic_load_n(&controller.command_quarantined,
                           __ATOMIC_ACQUIRE) != 0;
}

bool allocate_ring(Ring& ring) {
    if (ring.phys != 0) {
        return false;
    }
    ring.phys = alloc_zeroed_pages(kRingPageCount);
    if (ring.phys == 0) {
        return false;
    }
    ring.cycle = 1;
    ring.enqueue = 0;

    Trb* trbs = ring_trbs(ring);
    trbs[kTrbsPerPage - 1].parameter = ring.phys;
    trbs[kTrbsPerPage - 1].status = 0;
    trbs[kTrbsPerPage - 1].control =
        (kTrbTypeLink << 10) | kTrbToggleCycle | kTrbCycle;
    return true;
}

void free_ring(Ring& ring) {
    if (ring.phys != 0) {
        memory::free_kernel_block(ring.phys);
    }
    ring = {};
}

void free_device_slot_memory(DeviceSlot& slot) {
    if (slot.quarantined_dma_phys != 0) {
        memory::free_kernel_block(slot.quarantined_dma_phys);
        slot.quarantined_dma_phys = 0;
    }
    for (size_t i = 0; i < kMaxEndpointContexts; ++i) {
        free_ring(slot.endpoint_rings[i]);
    }
    if (slot.input_context_phys != 0) {
        memory::free_kernel_block(slot.input_context_phys);
        slot.input_context_phys = 0;
    }
    if (slot.output_context_phys != 0) {
        memory::free_kernel_block(slot.output_context_phys);
        slot.output_context_phys = 0;
    }
}

bool slot_quarantined(const DeviceSlot& slot) {
    return __atomic_load_n(&slot.quarantined, __ATOMIC_ACQUIRE) != 0;
}

bool slot_active(const DeviceSlot& slot) {
    return __atomic_load_n(&slot.used, __ATOMIC_ACQUIRE) != 0;
}

void quarantine_timed_out_transfer(DeviceSlot& slot,
                                   uint64_t controller_owned_dma_phys) {
    // Controller I/O serialization guarantees that this is the first and
    // only transfer allowed to time out for the slot.  Preserve every object
    // the controller may still reference and reject all future submissions.
    slot.quarantined_dma_phys = controller_owned_dma_phys;
    __atomic_store_n(&slot.quarantined, 1, __ATOMIC_RELEASE);
}

usb::Speed port_speed(uint32_t speed_id) {
    switch (speed_id) {
        case 1:
            return usb::Speed::Full;
        case 2:
            return usb::Speed::Low;
        case 3:
            return usb::Speed::High;
        case 4:
            return usb::Speed::Super;
        default:
            return usb::Speed::Unknown;
    }
}

uint16_t default_control_packet_size(usb::Speed speed) {
    switch (speed) {
        case usb::Speed::Low:
        case usb::Speed::Full:
            return 8;
        case usb::Speed::Super:
            return 512;
        case usb::Speed::High:
        default:
            return 64;
    }
}

uint32_t xhci_speed_id(usb::Speed speed) {
    switch (speed) {
        case usb::Speed::Full:
            return 1;
        case usb::Speed::Low:
            return 2;
        case usb::Speed::High:
            return 3;
        case usb::Speed::Super:
            return 4;
        default:
            return 0;
    }
}

uint8_t endpoint_id_from_address(uint8_t endpoint_address) {
    uint8_t number = endpoint_address & 0x0Fu;
    bool in = (endpoint_address & 0x80u) != 0;
    return static_cast<uint8_t>(number * 2 + (in ? 1 : 0));
}

uint32_t endpoint_type_value(const usb::Endpoint& endpoint) {
    bool in = (endpoint.address & 0x80u) != 0;
    switch (endpoint.type) {
        case usb::EndpointType::Control:
            return 4;
        case usb::EndpointType::Bulk:
            return in ? 6 : 2;
        case usb::EndpointType::Interrupt:
            return in ? 7 : 3;
        default:
            return 0;
    }
}

bool reset_port(Controller& controller, uint8_t port) {
    uint32_t value = read32(controller.operational, port_offset(port));
    if ((value & kPortScCurrentConnectStatus) == 0) {
        return false;
    }
    if ((value & kPortScPortEnabled) != 0) {
        return true;
    }

    write_portsc(controller, port, value | kPortScPortReset);
    for (uint32_t spins = 0; spins < kCommandSpinTimeout; ++spins) {
        uint32_t portsc = read32(controller.operational, port_offset(port));
        if ((portsc & kPortScPortReset) == 0 &&
            (portsc & kPortScPortEnabled) != 0) {
            clear_port_change_bits(controller, port, portsc);
            return true;
        }
        cpu_relax();
    }
    return false;
}

DeviceSlot* allocate_device_slot(Controller& controller) {
    for (size_t i = 0; i < kMaxDeviceSlots; ++i) {
        if (__atomic_load_n(&g_device_slots[i].allocated,
                            __ATOMIC_ACQUIRE) != 0) {
            continue;
        }
        DeviceSlot& slot = g_device_slots[i];
        slot = {};
        __atomic_store_n(&slot.allocated, 1, __ATOMIC_RELEASE);
        slot.controller = &controller;
        slot.input_context_phys = alloc_zeroed_pages(1);
        slot.output_context_phys = alloc_zeroed_pages(1);
        if (slot.input_context_phys == 0 || slot.output_context_phys == 0 ||
            !allocate_ring(slot.endpoint_rings[1])) {
            free_device_slot_memory(slot);
            slot = {};
            return nullptr;
        }
        return &slot;
    }
    return nullptr;
}

bool enable_slot(Controller& controller, uint8_t& slot_id) {
    Trb event{};
    if (!issue_command(controller,
                       0,
                       0,
                       kTrbTypeEnableSlotCommand << 10,
                       event)) {
        return false;
    }
    slot_id = static_cast<uint8_t>((event.control >> 24) & 0xFFu);
    return slot_id != 0 && slot_id <= controller.caps.max_slots;
}

void release_device_slot(DeviceSlot& slot) {
    __atomic_store_n(&slot.used, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&slot.quarantined, 1, __ATOMIC_RELEASE);

    if (slot.controller != nullptr && slot.slot_id != 0) {
        Controller& controller = *slot.controller;
        ControllerIoGuard io_guard(controller);
        Trb event{};
        bool controller_released = issue_command_locked(
            controller,
            0,
            0,
            (kTrbTypeDisableSlotCommand << 10) |
                (static_cast<uint32_t>(slot.slot_id) << 24),
            event);
        if (!controller_released) {
            // A timed-out or rejected Disable Slot command does not prove that
            // the controller stopped fetching the device contexts and rings.
            // Keep the local slot reserved and retain all controller-visible
            // DMA.  allocated remains set, so this entry cannot be reused.
            log_message(LogLevel::Warn,
                        "xhci: quarantining slot %u after disable failure",
                        static_cast<unsigned int>(slot.slot_id));
            return;
        }

        auto* dcbaa = static_cast<uint64_t*>(
            paging_phys_to_virt(controller.dcbaa_phys));
        dcbaa[slot.slot_id] = 0;
        free_device_slot_memory(slot);
        slot = {};
        return;
    }

    // No xHCI slot was enabled, so none of these allocations was ever made
    // visible to the controller.
    free_device_slot_memory(slot);
    slot = {};
}

void prepare_address_context(DeviceSlot& slot,
                             uint8_t root_port,
                             usb::Speed speed,
                             uint16_t ep0_packet_size) {
    Controller& controller = *slot.controller;
    memset(paging_phys_to_virt(slot.input_context_phys), 0, kPageSize);
    memset(paging_phys_to_virt(slot.output_context_phys), 0, kPageSize);

    uint32_t* input_control = input_control_context(slot.input_context_phys);
    input_control[1] = (1u << 0) | (1u << 1);

    uint32_t* slot_ctx = input_context(controller, slot.input_context_phys, 0);
    slot_ctx[0] = slot.route_string |
                  (xhci_speed_id(speed) << 20) |
                  (1u << 27);
    slot_ctx[1] = static_cast<uint32_t>(root_port) << 16;
    slot_ctx[2] = static_cast<uint32_t>(slot.tt_hub_slot_id) |
                  (static_cast<uint32_t>(slot.tt_port) << 8);

    uint32_t* ep0_ctx = input_context(controller, slot.input_context_phys, 1);
    ep0_ctx[1] = (3u << 1) | (4u << 3) |
                 (static_cast<uint32_t>(ep0_packet_size) << 16);
    ep0_ctx[2] = static_cast<uint32_t>(
        slot.endpoint_rings[1].phys | slot.endpoint_rings[1].cycle);
    ep0_ctx[3] = static_cast<uint32_t>(slot.endpoint_rings[1].phys >> 32);
    ep0_ctx[4] = 8;
    slot.port = root_port;
    slot.speed = speed;
}

bool address_device(DeviceSlot& slot) {
    Controller& controller = *slot.controller;
    auto* dcbaa = static_cast<uint64_t*>(paging_phys_to_virt(controller.dcbaa_phys));
    dcbaa[slot.slot_id] = slot.output_context_phys;

    Trb event{};
    uint32_t control = (kTrbTypeAddressDeviceCommand << 10) |
                       (static_cast<uint32_t>(slot.slot_id) << 24);
    return issue_command(controller, slot.input_context_phys, 0, control, event);
}

usb::TransferStatus control_transfer(void* context,
                                     const usb::ControlRequest& request,
                                     void* data) {
    auto* slot = static_cast<DeviceSlot*>(context);
    if (slot == nullptr || slot_quarantined(*slot) || !slot_active(*slot)) {
        return usb::TransferStatus::NoDevice;
    }
    Controller* controller = slot->controller;
    if (controller == nullptr) {
        return usb::TransferStatus::NoDevice;
    }
    if (request.length != 0 && data == nullptr) {
        return usb::TransferStatus::IoError;
    }

    uint64_t data_phys = 0;
    void* dma_buffer = nullptr;
    if (request.length != 0) {
        size_t page_count =
            (static_cast<size_t>(request.length) + kPageSize - 1) / kPageSize;
        data_phys = memory::alloc_kernel_block_pages(page_count);
        if (data_phys == 0) {
            return usb::TransferStatus::IoError;
        }
        dma_buffer = paging_phys_to_virt(data_phys);
        if (dma_buffer == nullptr) {
            memory::free_kernel_block(data_phys);
            return usb::TransferStatus::IoError;
        }
        memset(dma_buffer, 0, page_count * kPageSize);
        if ((request.request_type & 0x80u) == 0) {
            memcpy(dma_buffer, data, request.length);
        }
    }

    uint64_t setup = static_cast<uint64_t>(request.request_type) |
                     (static_cast<uint64_t>(request.request) << 8) |
                     (static_cast<uint64_t>(request.value) << 16) |
                     (static_cast<uint64_t>(request.index) << 32) |
                     (static_cast<uint64_t>(request.length) << 48);
    uint32_t trt = 0;
    if (request.length != 0) {
        trt = (request.request_type & 0x80u) ? 2u : 3u;
    }

    ControllerIoGuard io_guard(*controller);
    if (slot_quarantined(*slot) || !slot_active(*slot) ||
        slot->controller != controller) {
        if (data_phys != 0) {
            memory::free_kernel_block(data_phys);
        }
        return usb::TransferStatus::NoDevice;
    }
    Ring& ring = slot->endpoint_rings[1];
    (void)enqueue_ring_trb(ring,
                           setup,
                           8,
                           (kTrbTypeSetupStage << 10) | kTrbImmediateData |
                               (trt << 16));

    if (request.length != 0) {
        (void)enqueue_ring_trb(
            ring,
            data_phys,
            request.length,
            (kTrbTypeDataStage << 10) |
                ((request.request_type & 0x80u) ? kTrbDataStageDirectionIn : 0));
    }

    uint32_t status_control = kTrbTypeStatusStage << 10;
    if (request.length == 0 || (request.request_type & 0x80u) == 0) {
        status_control |= kTrbDataStageDirectionIn;
    }
    uint64_t status_trb = enqueue_ring_trb(
        ring, 0, 0, status_control | kTrbInterruptOnCompletion);
    ring_endpoint_doorbell(*slot, 1);

    size_t transferred = 0;
    usb::TransferStatus status = usb::TransferStatus::Ok;
    bool completed = wait_transfer_completion(
        *slot, 1, status_trb, request.length, transferred, status);
    if (dma_buffer != nullptr) {
        if (completed && status == usb::TransferStatus::Ok &&
            (request.request_type & 0x80u) != 0) {
            size_t copy_bytes = transferred < request.length
                                    ? transferred
                                    : request.length;
            memcpy(data, dma_buffer, copy_bytes);
        }
        if (completed) {
            memory::free_kernel_block(data_phys);
        } else {
            quarantine_timed_out_transfer(*slot, data_phys);
            log_message(LogLevel::Warn,
                        "xhci: quarantining slot %u after control timeout",
                        static_cast<unsigned int>(slot->slot_id));
        }
    } else if (!completed) {
        quarantine_timed_out_transfer(*slot, 0);
        log_message(LogLevel::Warn,
                    "xhci: quarantining slot %u after control timeout",
                    static_cast<unsigned int>(slot->slot_id));
    }
    return status;
}

usb::TransferStatus bulk_transfer(void* context,
                                  uint8_t endpoint,
                                  void* data,
                                  size_t length,
                                  size_t& transferred) {
    auto* slot = static_cast<DeviceSlot*>(context);
    transferred = 0;
    if (slot == nullptr || slot_quarantined(*slot) || !slot_active(*slot)) {
        return usb::TransferStatus::NoDevice;
    }
    Controller* controller = slot->controller;
    if (controller == nullptr) {
        return usb::TransferStatus::NoDevice;
    }
    uint8_t endpoint_id = endpoint_id_from_address(endpoint);
    if (endpoint_id >= kMaxEndpointContexts ||
        slot->endpoint_rings[endpoint_id].phys == 0) {
        return usb::TransferStatus::Unsupported;
    }
    if (length != 0 && data == nullptr) {
        return usb::TransferStatus::IoError;
    }

    uint64_t data_phys = 0;
    uint8_t* dma_buffer = nullptr;
    if (length != 0) {
        size_t page_count = length / kPageSize;
        if ((length % kPageSize) != 0) {
            ++page_count;
        }
        data_phys = memory::alloc_kernel_block_pages(page_count);
        if (data_phys == 0) {
            return usb::TransferStatus::IoError;
        }
        dma_buffer = static_cast<uint8_t*>(paging_phys_to_virt(data_phys));
        if (dma_buffer == nullptr) {
            memory::free_kernel_block(data_phys);
            return usb::TransferStatus::IoError;
        }
        memset(dma_buffer, 0, length);
        if ((endpoint & 0x80u) == 0) {
            memcpy(dma_buffer, data, length);
        }
    }

    usb::TransferStatus status = usb::TransferStatus::Ok;
    bool completed = false;
    {
        ControllerIoGuard io_guard(*controller);
        if (slot_quarantined(*slot) || !slot_active(*slot) ||
            slot->controller != controller ||
            slot->endpoint_rings[endpoint_id].phys == 0) {
            if (data_phys != 0) {
                memory::free_kernel_block(data_phys);
            }
            return usb::TransferStatus::NoDevice;
        }
        Ring& ring = slot->endpoint_rings[endpoint_id];
        size_t remaining = length;
        size_t dma_offset = 0;
        uint64_t last_trb_phys = 0;

        if (length == 0) {
            last_trb_phys = enqueue_ring_trb(
                ring,
                0,
                0,
                (kTrbTypeNormal << 10) | kTrbInterruptOnCompletion);
        }

        while (remaining > 0) {
            uint64_t phys = data_phys + dma_offset;
            size_t page_remaining =
                static_cast<size_t>(kPageSize - (phys & (kPageSize - 1)));
            size_t chunk = remaining < page_remaining ? remaining
                                                      : page_remaining;

            remaining -= chunk;
            uint32_t control = kTrbTypeNormal << 10;
            if (remaining != 0) {
                control |= kTrbChain;
            } else {
                control |= kTrbInterruptOnCompletion;
            }
            last_trb_phys = enqueue_ring_trb(
                ring, phys, static_cast<uint32_t>(chunk), control);
            dma_offset += chunk;
        }

        ring_endpoint_doorbell(*slot, endpoint_id);
        completed = wait_transfer_completion(
            *slot, endpoint_id, last_trb_phys, length, transferred, status);
        if (!completed) {
            quarantine_timed_out_transfer(*slot, data_phys);
        }
    }

    if (dma_buffer != nullptr) {
        if (completed && status == usb::TransferStatus::Ok &&
            (endpoint & 0x80u) != 0) {
            size_t copy_bytes = transferred < length ? transferred : length;
            memcpy(data, dma_buffer, copy_bytes);
        }
        if (completed) {
            memory::free_kernel_block(data_phys);
        } else {
            log_message(LogLevel::Warn,
                        "xhci: quarantining slot %u after bulk timeout",
                        static_cast<unsigned int>(slot->slot_id));
        }
    } else if (!completed) {
        log_message(LogLevel::Warn,
                    "xhci: quarantining slot %u after bulk timeout",
                    static_cast<unsigned int>(slot->slot_id));
    }
    return status;
}

usb::TransferStatus reset_endpoint(void* context, uint8_t endpoint) {
    auto* slot = static_cast<DeviceSlot*>(context);
    if (slot == nullptr || slot_quarantined(*slot) || !slot_active(*slot)) {
        return usb::TransferStatus::NoDevice;
    }
    Controller* controller_ptr = slot->controller;
    if (controller_ptr == nullptr) {
        return usb::TransferStatus::NoDevice;
    }

    uint8_t endpoint_id = endpoint_id_from_address(endpoint);
    if (endpoint_id == 0 || endpoint_id >= kMaxEndpointContexts) {
        return usb::TransferStatus::Unsupported;
    }
    Ring& ring = slot->endpoint_rings[endpoint_id];
    if (ring.phys == 0) {
        return usb::TransferStatus::Unsupported;
    }

    Controller& controller = *controller_ptr;
    ControllerIoGuard io_guard(controller);
    if (slot_quarantined(*slot) || !slot_active(*slot) ||
        slot->controller != controller_ptr || ring.phys == 0) {
        return usb::TransferStatus::NoDevice;
    }
    uint32_t* ep_context =
        device_context(controller, slot->output_context_phys, endpoint_id);
    constexpr uint32_t kEndpointStateMask = 0x7u;
    constexpr uint32_t kEndpointStateHalted = 2u;
    if ((ep_context[0] & kEndpointStateMask) != kEndpointStateHalted) {
        // CLEAR_FEATURE is required for both BOT bulk endpoints, even though
        // xHCI only permits Reset Endpoint for one that is actually halted.
        return usb::TransferStatus::Ok;
    }

    Trb event{};
    uint32_t reset_control =
        (kTrbTypeResetEndpointCommand << 10) |
        (static_cast<uint32_t>(endpoint_id) << 16) |
        (static_cast<uint32_t>(slot->slot_id) << 24);
    if (!issue_command_locked(controller, 0, 0, reset_control, event)) {
        if (controller_commands_quarantined(controller)) {
            quarantine_timed_out_transfer(*slot, 0);
        }
        return usb::TransferStatus::IoError;
    }

    // Discard the stopped transfer and restart from a fresh ring.  Merely
    // clearing ENDPOINT_HALT on the USB device does not update xHCI's dequeue
    // pointer, so the controller would otherwise remain stuck on the TRB that
    // produced the stall.
    memset(paging_phys_to_virt(ring.phys), 0, kRingPageCount * kPageSize);
    ring.enqueue = 0;
    ring.cycle = 1;
    Trb* trbs = ring_trbs(ring);
    trbs[kTrbsPerPage - 1].parameter = ring.phys;
    trbs[kTrbsPerPage - 1].status = 0;
    trbs[kTrbsPerPage - 1].control =
        (kTrbTypeLink << 10) | kTrbToggleCycle | kTrbCycle;

    uint32_t dequeue_control =
        (kTrbTypeSetTrDequeuePointerCommand << 10) |
        (static_cast<uint32_t>(endpoint_id) << 16) |
        (static_cast<uint32_t>(slot->slot_id) << 24);
    if (!issue_command_locked(controller, ring.phys | ring.cycle, 0,
                              dequeue_control, event)) {
        if (controller_commands_quarantined(controller)) {
            quarantine_timed_out_transfer(*slot, 0);
        }
        return usb::TransferStatus::IoError;
    }
    return usb::TransferStatus::Ok;
}

usb::TransferStatus configure_hub(void* context,
                                  uint8_t port_count,
                                  bool multi_tt,
                                  uint8_t tt_think_time) {
    auto* slot = static_cast<DeviceSlot*>(context);
    if (slot == nullptr || slot_quarantined(*slot) || !slot_active(*slot)) {
        return usb::TransferStatus::NoDevice;
    }
    if (port_count == 0 || tt_think_time > 3) {
        return usb::TransferStatus::Unsupported;
    }
    Controller* controller_ptr = slot->controller;
    if (controller_ptr == nullptr) {
        return usb::TransferStatus::NoDevice;
    }

    Controller& controller = *controller_ptr;
    ControllerIoGuard io_guard(controller);
    if (slot_quarantined(*slot) || !slot_active(*slot) ||
        slot->controller != controller_ptr) {
        return usb::TransferStatus::NoDevice;
    }

    memset(paging_phys_to_virt(slot->input_context_phys), 0, kPageSize);
    uint32_t* input_control = input_control_context(slot->input_context_phys);
    input_control[1] = 1u << 0;
    uint32_t* slot_ctx = input_context(controller,
                                       slot->input_context_phys,
                                       0);
    uint32_t* current_slot_ctx = device_context(controller,
                                                slot->output_context_phys,
                                                0);
    for (size_t i = 0; i < context_size(controller) / sizeof(uint32_t); ++i) {
        slot_ctx[i] = current_slot_ctx[i];
    }
    constexpr uint32_t kSlotContextMultiTt = 1u << 25;
    constexpr uint32_t kSlotContextHub = 1u << 26;
    constexpr uint32_t kSlotContextTtThinkTimeMask = 0x3u << 16;
    slot_ctx[0] |= kSlotContextHub;
    if (multi_tt) {
        slot_ctx[0] |= kSlotContextMultiTt;
    } else {
        slot_ctx[0] &= ~kSlotContextMultiTt;
    }
    slot_ctx[1] &= 0x00FFFFFFu;
    slot_ctx[1] |= static_cast<uint32_t>(port_count) << 24;
    slot_ctx[2] &= ~kSlotContextTtThinkTimeMask;
    slot_ctx[2] |= static_cast<uint32_t>(tt_think_time) << 16;

    Trb event{};
    uint32_t control = (kTrbTypeEvaluateContextCommand << 10) |
                       (static_cast<uint32_t>(slot->slot_id) << 24);
    if (!issue_command_locked(controller,
                              slot->input_context_phys,
                              0,
                              control,
                              event)) {
        if (controller_commands_quarantined(controller)) {
            quarantine_timed_out_transfer(*slot, 0);
        }
        return usb::TransferStatus::IoError;
    }
    return usb::TransferStatus::Ok;
}

bool get_descriptor(DeviceSlot& slot,
                    uint8_t descriptor_type,
                    uint8_t descriptor_index,
                    void* buffer,
                    uint16_t length) {
    usb::ControlRequest request{
        0x80,
        0x06,
        static_cast<uint16_t>((descriptor_type << 8) | descriptor_index),
        0,
        length,
    };
    return control_transfer(&slot, request, buffer) == usb::TransferStatus::Ok;
}

bool set_configuration(DeviceSlot& slot, uint8_t configuration_value) {
    usb::ControlRequest request{
        0x00,
        0x09,
        configuration_value,
        0,
        0,
    };
    return control_transfer(&slot, request, nullptr) == usb::TransferStatus::Ok;
}

usb::EndpointType endpoint_type_from_attributes(uint8_t attributes) {
    switch (attributes & 0x3u) {
        case 0:
            return usb::EndpointType::Control;
        case 1:
            return usb::EndpointType::Isochronous;
        case 2:
            return usb::EndpointType::Bulk;
        case 3:
        default:
            return usb::EndpointType::Interrupt;
    }
}

bool parse_config_descriptor(usb::Device& device,
                             const uint8_t* data,
                             size_t length,
                             uint8_t& configuration_value) {
    if (length < 9 || data[1] != 2) {
        return false;
    }
    configuration_value = data[5];

    size_t offset = 0;
    while (offset + 2 <= length) {
        uint8_t descriptor_length = data[offset];
        uint8_t descriptor_type = data[offset + 1];
        if (descriptor_length < 2 || offset + descriptor_length > length) {
            break;
        }

        if (descriptor_type == 4 && descriptor_length >= 9 &&
            device.interface_count < 8) {
            usb::Interface& interface =
                device.interfaces[device.interface_count++];
            interface.number = data[offset + 2];
            interface.alternate_setting = data[offset + 3];
            interface.class_code = data[offset + 5];
            interface.subclass = data[offset + 6];
            interface.protocol = data[offset + 7];
        } else if (descriptor_type == 5 && descriptor_length >= 7 &&
                   device.endpoint_count < 16) {
            usb::Endpoint& endpoint = device.endpoints[device.endpoint_count++];
            endpoint.address = data[offset + 2];
            endpoint.type = endpoint_type_from_attributes(data[offset + 3]);
            endpoint.max_packet_size =
                static_cast<uint16_t>(data[offset + 4]) |
                (static_cast<uint16_t>(data[offset + 5]) << 8);
            endpoint.interval = data[offset + 6];
        }

        offset += descriptor_length;
    }
    return true;
}

bool configure_endpoints(DeviceSlot& slot) {
    Controller& controller = *slot.controller;
    memset(paging_phys_to_virt(slot.input_context_phys), 0, kPageSize);
    uint32_t* input_control = input_control_context(slot.input_context_phys);
    input_control[1] = 1u << 0;

    uint8_t highest_context = 1;
    for (size_t i = 0; i < slot.device.endpoint_count; ++i) {
        const usb::Endpoint& endpoint = slot.device.endpoints[i];
        uint8_t endpoint_id = endpoint_id_from_address(endpoint.address);
        uint32_t ep_type = endpoint_type_value(endpoint);
        if (endpoint_id <= 1 || endpoint_id >= kMaxEndpointContexts ||
            ep_type == 0 || endpoint.max_packet_size == 0 ||
            slot.endpoint_rings[endpoint_id].phys != 0) {
            return false;
        }
        if (!allocate_ring(slot.endpoint_rings[endpoint_id])) {
            return false;
        }

        input_control[1] |= 1u << endpoint_id;
        if (endpoint_id > highest_context) {
            highest_context = endpoint_id;
        }

        uint32_t* ep_ctx = input_context(controller,
                                         slot.input_context_phys,
                                         endpoint_id);
        ep_ctx[0] = static_cast<uint32_t>(endpoint.interval) << 16;
        ep_ctx[1] = (3u << 1) | (ep_type << 3) |
                    (static_cast<uint32_t>(endpoint.max_packet_size) << 16);
        ep_ctx[2] = static_cast<uint32_t>(
            slot.endpoint_rings[endpoint_id].phys |
            slot.endpoint_rings[endpoint_id].cycle);
        ep_ctx[3] =
            static_cast<uint32_t>(slot.endpoint_rings[endpoint_id].phys >> 32);
        ep_ctx[4] = endpoint.max_packet_size;
    }

    uint32_t* slot_ctx = input_context(controller, slot.input_context_phys, 0);
    uint32_t* current_slot_ctx = device_context(controller,
                                                slot.output_context_phys,
                                                0);
    for (size_t i = 0; i < context_size(controller) / sizeof(uint32_t); ++i) {
        slot_ctx[i] = current_slot_ctx[i];
    }
    slot_ctx[0] &= ~kContextEntriesMask;
    slot_ctx[0] |= static_cast<uint32_t>(highest_context) << 27;

    Trb event{};
    uint32_t control = (kTrbTypeConfigureEndpointCommand << 10) |
                       (static_cast<uint32_t>(slot.slot_id) << 24);
    return issue_command(controller, slot.input_context_phys, 0, control, event);
}

bool enumerate_device_at_path(Controller& controller,
                              uint8_t root_port,
                              usb::Speed speed,
                              uint8_t route_depth,
                              uint32_t route_string,
                              uint8_t tt_hub_slot_id,
                              uint8_t tt_port) {
    if (controller_commands_quarantined(controller)) {
        return false;
    }

    DeviceSlot* slot = allocate_device_slot(controller);
    if (slot == nullptr) {
        log_message(LogLevel::Warn, "xhci: no local device slots available");
        return false;
    }
    // Record ownership before issuing Enable Slot so a command that cannot be
    // safely unwound still reserves this physical port against replug churn.
    slot->port = root_port;
    slot->route_depth = route_depth;
    slot->route_string = route_string;
    slot->tt_hub_slot_id = tt_hub_slot_id;
    slot->tt_port = tt_port;
    if (!enable_slot(controller, slot->slot_id)) {
        log_message(LogLevel::Warn,
                    "xhci: enable slot failed for route=%05x",
                    static_cast<unsigned int>(route_string));
        release_device_slot(*slot);
        return false;
    }
    log_message(LogLevel::Info,
                "xhci: root port %u route=%05x enabled slot=%u",
                static_cast<unsigned int>(root_port),
                static_cast<unsigned int>(route_string),
                static_cast<unsigned int>(slot->slot_id));

    prepare_address_context(
        *slot, root_port, speed, default_control_packet_size(speed));
    if (!address_device(*slot)) {
        log_message(LogLevel::Warn,
                    "xhci: address device failed route=%05x slot=%u",
                    static_cast<unsigned int>(route_string),
                    static_cast<unsigned int>(slot->slot_id));
        release_device_slot(*slot);
        return false;
    }
    log_message(LogLevel::Info,
                "xhci: root port %u route=%05x addressed slot=%u",
                static_cast<unsigned int>(root_port),
                static_cast<unsigned int>(route_string),
                static_cast<unsigned int>(slot->slot_id));

    __atomic_store_n(&slot->used, 1, __ATOMIC_RELEASE);
    slot->address = g_next_usb_address++;
    if (g_next_usb_address == 0) {
        g_next_usb_address = 1;
    }

    uint8_t device_descriptor[18]{};
    if (!get_descriptor(*slot, 1, 0, device_descriptor, sizeof(device_descriptor))) {
        log_message(LogLevel::Warn, "xhci: failed to read device descriptor");
        release_device_slot(*slot);
        return false;
    }

    usb::Device& device = slot->device;
    device = {};
    device.address = slot->address;
    device.speed = speed;
    device.vendor_id = static_cast<uint16_t>(device_descriptor[8]) |
                       (static_cast<uint16_t>(device_descriptor[9]) << 8);
    device.product_id = static_cast<uint16_t>(device_descriptor[10]) |
                        (static_cast<uint16_t>(device_descriptor[11]) << 8);
    device.class_code = device_descriptor[4];
    device.subclass = device_descriptor[5];
    device.protocol = device_descriptor[6];
    device.transport.context = slot;
    device.transport.control = control_transfer;
    device.transport.bulk = bulk_transfer;
    device.transport.reset_endpoint = reset_endpoint;
    device.transport.configure_hub = configure_hub;
    device.transport.enumerate_hub_port = enumerate_hub_port;

    uint8_t config_header[9]{};
    if (!get_descriptor(*slot, 2, 0, config_header, sizeof(config_header))) {
        log_message(LogLevel::Warn, "xhci: failed to read config header");
        release_device_slot(*slot);
        return false;
    }
    uint16_t total_length = static_cast<uint16_t>(config_header[2]) |
                            (static_cast<uint16_t>(config_header[3]) << 8);
    if (total_length < sizeof(config_header)) {
        log_message(LogLevel::Warn,
                    "xhci: invalid config header total_length=%u",
                    static_cast<unsigned int>(total_length));
        release_device_slot(*slot);
        return false;
    }
    if (total_length > kMaxConfigDescriptorBytes) {
        total_length = kMaxConfigDescriptorBytes;
    }

    uint8_t config_descriptor[kMaxConfigDescriptorBytes]{};
    if (!get_descriptor(*slot, 2, 0, config_descriptor, total_length)) {
        log_message(LogLevel::Warn, "xhci: failed to read config descriptor");
        release_device_slot(*slot);
        return false;
    }

    uint8_t configuration_value = 0;
    if (!parse_config_descriptor(device,
                                 config_descriptor,
                                 total_length,
                                 configuration_value) ||
        configuration_value == 0) {
        log_message(LogLevel::Warn, "xhci: invalid config descriptor");
        release_device_slot(*slot);
        return false;
    }

    if (!set_configuration(*slot, configuration_value)) {
        log_message(LogLevel::Warn, "xhci: set configuration failed");
        release_device_slot(*slot);
        return false;
    }
    if (!configure_endpoints(*slot)) {
        log_message(LogLevel::Warn, "xhci: configure endpoints failed");
        release_device_slot(*slot);
        return false;
    }

    for (size_t i = 0; i < device.interface_count; ++i) {
        const usb::Interface& interface = device.interfaces[i];
        log_message(LogLevel::Info,
                    "xhci: device addr=%u interface=%u class=%02x subclass=%02x protocol=%02x",
                    static_cast<unsigned int>(device.address),
                    static_cast<unsigned int>(interface.number),
                    static_cast<unsigned int>(interface.class_code),
                    static_cast<unsigned int>(interface.subclass),
                    static_cast<unsigned int>(interface.protocol));
    }

    if (!usb::register_device(device)) {
        release_device_slot(*slot);
        return false;
    }
    return true;
}

bool enumerate_device(Controller& controller, uint8_t port) {
    if (controller_commands_quarantined(controller)) {
        return false;
    }
    uint32_t portsc = read32(controller.operational, port_offset(port));
    if ((portsc & kPortScCurrentConnectStatus) == 0) {
        log_message(LogLevel::Warn,
                    "xhci: port %u disconnected before enumeration portsc=%x",
                    static_cast<unsigned int>(port),
                    static_cast<unsigned long long>(portsc));
        return false;
    }
    log_message(LogLevel::Info,
                "xhci: enumerating port %u portsc=%x",
                static_cast<unsigned int>(port),
                static_cast<unsigned long long>(portsc));
    if (!reset_port(controller, port)) {
        log_message(LogLevel::Warn, "xhci: port %u reset failed", port);
        return false;
    }

    portsc = read32(controller.operational, port_offset(port));
    usb::Speed speed = port_speed((portsc >> 10) & 0xFu);
    if (speed == usb::Speed::Unknown) {
        return false;
    }
    return enumerate_device_at_path(controller, port, speed, 0, 0, 0, 0);
}

usb::TransferStatus enumerate_hub_port(void* context,
                                       uint8_t downstream_port,
                                       usb::Speed speed) {
    auto* hub = static_cast<DeviceSlot*>(context);
    if (hub == nullptr || slot_quarantined(*hub) || !slot_active(*hub)) {
        return usb::TransferStatus::NoDevice;
    }
    if (downstream_port == 0 || downstream_port > 15 ||
        hub->route_depth >= 5 || speed == usb::Speed::Unknown ||
        speed == usb::Speed::Super) {
        return usb::TransferStatus::Unsupported;
    }
    Controller* controller = hub->controller;
    if (controller == nullptr || !controller->active) {
        return usb::TransferStatus::NoDevice;
    }

    uint8_t child_depth = static_cast<uint8_t>(hub->route_depth + 1);
    uint32_t child_route =
        hub->route_string |
        (static_cast<uint32_t>(downstream_port) << (hub->route_depth * 4));
    for (size_t i = 0; i < kMaxDeviceSlots; ++i) {
        const DeviceSlot& existing = g_device_slots[i];
        if (__atomic_load_n(&existing.allocated, __ATOMIC_ACQUIRE) != 0 &&
            existing.controller == controller &&
            existing.port == hub->port &&
            existing.route_depth == child_depth &&
            existing.route_string == child_route) {
            return usb::TransferStatus::Ok;
        }
    }

    uint8_t tt_hub_slot_id = 0;
    uint8_t tt_port = 0;
    if (speed == usb::Speed::Low || speed == usb::Speed::Full) {
        if (hub->speed == usb::Speed::High) {
            tt_hub_slot_id = hub->slot_id;
            tt_port = downstream_port;
        } else {
            tt_hub_slot_id = hub->tt_hub_slot_id;
            tt_port = hub->tt_port;
        }
    }

    log_message(LogLevel::Info,
                "xhci: enumerating hub slot=%u port=%u route=%05x speed=%s",
                static_cast<unsigned int>(hub->slot_id),
                static_cast<unsigned int>(downstream_port),
                static_cast<unsigned int>(child_route),
                usb::speed_name(speed));
    return enumerate_device_at_path(*controller,
                                    hub->port,
                                    speed,
                                    child_depth,
                                    child_route,
                                    tt_hub_slot_id,
                                    tt_port)
               ? usb::TransferStatus::Ok
               : usb::TransferStatus::IoError;
}

bool port_has_device(const Controller& controller, uint8_t port) {
    for (size_t i = 0; i < kMaxDeviceSlots; ++i) {
        const DeviceSlot& slot = g_device_slots[i];
        if (__atomic_load_n(&slot.allocated, __ATOMIC_ACQUIRE) != 0 &&
            slot.controller == &controller &&
            slot.port == port) {
            return true;
        }
    }
    return false;
}

const char* port_speed_name(uint32_t speed_id) {
    switch (speed_id) {
        case 1:
            return "full";
        case 2:
            return "low";
        case 3:
            return "high";
        case 4:
            return "super";
        default:
            return "unknown";
    }
}

void scan_ports(Controller& controller) {
    for (uint16_t port_index = 1;
         port_index <= controller.caps.max_ports;
         ++port_index) {
        uint8_t port = static_cast<uint8_t>(port_index);
        uint32_t portsc =
            read32(controller.operational, port_offset(port));
        log_message(LogLevel::Debug,
                    "xhci: port %u status=%x",
                    static_cast<unsigned int>(port),
                    static_cast<unsigned long long>(portsc));
        if ((portsc & kPortScCurrentConnectStatus) == 0) {
            continue;
        }
        uint32_t speed_id = (portsc >> 10) & 0xFu;
        log_message(LogLevel::Info,
                    "xhci: port %u connected speed=%s enabled=%u powered=%u reset=%u",
                    static_cast<unsigned int>(port),
                    port_speed_name(speed_id),
                    (portsc & kPortScPortEnabled) ? 1u : 0u,
                    (portsc & kPortScPortPower) ? 1u : 0u,
                    (portsc & kPortScPortReset) ? 1u : 0u);
        log_message(LogLevel::Info,
                    "xhci: enumerating boot device on port %u",
                    static_cast<unsigned int>(port));
        controller.port_enumeration_attempted[port] = true;
        if (enumerate_device(controller, port)) {
            log_message(LogLevel::Info,
                        "xhci: boot device enumeration complete on port %u",
                        static_cast<unsigned int>(port));
        } else {
            log_message(LogLevel::Warn,
                        "xhci: boot device enumeration failed on port %u; reconnect to retry",
                        static_cast<unsigned int>(port));
        }
    }
}

void power_ports(Controller& controller) {
    bool changed = false;
    for (uint16_t port_index = 1;
         port_index <= controller.caps.max_ports;
         ++port_index) {
        uint8_t port = static_cast<uint8_t>(port_index);
        uint32_t offset = port_offset(port);
        uint32_t portsc = read32(controller.operational, offset);
        if ((portsc & kPortScPortPower) != 0) {
            continue;
        }
        // PORTSC change bits are write-one-to-clear. Writing only PP avoids
        // acknowledging a pending connection event or disabling an enabled
        // port as a read-modify-write could do.
        write32(controller.operational, offset, kPortScPortPower);
        changed = true;
    }
    if (!changed) {
        return;
    }

    // Let port power stabilize before sampling CCS. Port status is also
    // serviced later by the poll worker, so this delay is only the fast path
    // for devices already inserted at boot.
    for (uint32_t spins = 0; spins < kSpinTimeout; ++spins) {
        cpu_relax();
    }
}

bool take_enumeration_request(Controller*& controller, uint8_t& port) {
    lock_enumeration_request();
    if (__atomic_load_n(&g_enumeration_pending, __ATOMIC_ACQUIRE) == 0) {
        unlock_enumeration_request();
        return false;
    }
    controller = g_pending_controller;
    port = g_pending_port;
    __atomic_store_n(&g_enumeration_pending, 0, __ATOMIC_RELEASE);
    unlock_enumeration_request();
    return true;
}

void park_enumeration_worker(process::Task& proc) {
    proc.waiting_on = nullptr;
    process::store_state(proc, process::State::Blocked);
    // A producer that ran while this callback was still Running could not
    // wake it.  Recheck after publishing Blocked to close that race.
    if (__atomic_load_n(&g_enumeration_pending, __ATOMIC_ACQUIRE) != 0) {
        (void)process::wake(proc);
    }
}

void enumeration_worker(process::Task& proc) {
    Controller* controller = nullptr;
    uint8_t port = 0;
    if (!take_enumeration_request(controller, port)) {
        park_enumeration_worker(proc);
        return;
    }

    if (controller == nullptr || port == 0 || !controller->active) {
        park_enumeration_worker(proc);
        return;
    }

    uint32_t portsc = read32(controller->operational, port_offset(port));
    log_message(LogLevel::Info,
                "xhci: hotplug detected on port %u status=%x",
                static_cast<unsigned int>(port),
                static_cast<unsigned long long>(portsc));
    if (enumerate_device(*controller, port)) {
        log_message(LogLevel::Info,
                    "xhci: port %u background enumeration complete",
                    static_cast<unsigned int>(port));
    } else {
        log_message(LogLevel::Warn,
                    "xhci: port %u background enumeration failed; reconnect to retry",
                    static_cast<unsigned int>(port));
    }

    park_enumeration_worker(proc);
}

void queue_port_enumeration(Controller& controller, uint8_t port) {
    bool queued = false;
    lock_enumeration_request();
    if (__atomic_load_n(&g_enumeration_pending, __ATOMIC_ACQUIRE) == 0) {
        g_pending_controller = &controller;
        g_pending_port = port;
        __atomic_store_n(&g_enumeration_pending, 1, __ATOMIC_RELEASE);
        queued = true;
    }
    unlock_enumeration_request();
    if (!queued) {
        return;
    }

    if (g_enumeration_worker != nullptr) {
        (void)process::wake(*g_enumeration_worker);
    }
}

void poll_ports() {
    for (size_t i = 0; i < g_controller_count; ++i) {
        Controller& controller = g_controllers[i];
        if (!controller.active) {
            continue;
        }
        for (uint16_t port_index = 1;
             port_index <= controller.caps.max_ports;
             ++port_index) {
            uint8_t port = static_cast<uint8_t>(port_index);
            uint32_t portsc =
                read32(controller.operational, port_offset(port));
            if ((portsc & kPortScCurrentConnectStatus) == 0) {
                controller.port_enumeration_attempted[port] = false;
                continue;
            }
            if (port_has_device(controller, port) ||
                controller.port_enumeration_attempted[port]) {
                continue;
            }
            if (__atomic_load_n(&g_enumeration_pending,
                                __ATOMIC_ACQUIRE) != 0) {
                return;
            }
            controller.port_enumeration_attempted[port] = true;
            queue_port_enumeration(controller, port);
            return;
        }
    }
}

bool init_controller(const pci::PciDevice& device) {
    if (g_controller_count >= kMaxControllers) {
        log_message(LogLevel::Warn, "xhci: controller table full");
        return false;
    }

    uint64_t bar0 = pci_bar_base(device, 0);
    if (bar0 == 0) {
        log_message(LogLevel::Warn,
                    "xhci: controller %02u:%02u.%u has no MMIO BAR0",
                    device.bus,
                    device.slot,
                    device.function);
        return false;
    }

    enable_pci_device(device);

    volatile uint8_t* mmio = map_mmio_range(bar0, kControllerMmioSize);
    if (mmio == nullptr) {
        return false;
    }

    CapabilityInfo caps = read_capabilities(mmio);
    uint64_t operational_end =
        static_cast<uint64_t>(caps.cap_length) + 0x400u +
        static_cast<uint64_t>(caps.max_ports - 1u) * 0x10u +
        sizeof(uint32_t);
    uint64_t runtime_end =
        static_cast<uint64_t>(caps.runtime_offset) + 0x40u;
    uint64_t doorbell_end =
        static_cast<uint64_t>(caps.doorbell_offset) +
        (static_cast<uint64_t>(caps.max_slots) + 1u) * sizeof(uint32_t);
    uint64_t extended_offset =
        static_cast<uint64_t>(caps.extended_capabilities_offset) * 4u;
    if (caps.cap_length < 0x20 || caps.max_ports == 0 ||
        caps.doorbell_offset == 0 || caps.runtime_offset == 0) {
        log_message(LogLevel::Warn,
                    "xhci: invalid capability registers at %02u:%02u.%u",
                    device.bus,
                    device.slot,
                    device.function);
        return false;
    }
    if (operational_end > kControllerMmioSize ||
        runtime_end > kControllerMmioSize ||
        doorbell_end > kControllerMmioSize ||
        (extended_offset != 0 &&
         extended_offset > kControllerMmioSize - sizeof(uint32_t))) {
        log_message(LogLevel::Warn,
                    "xhci: controller register window exceeds mapping at %02u:%02u.%u",
                    device.bus,
                    device.slot,
                    device.function);
        return false;
    }

    claim_bios_ownership(mmio,
                         caps.extended_capabilities_offset,
                         static_cast<uint32_t>(kControllerMmioSize));

    Controller& controller = g_controllers[g_controller_count];
    controller.pci_device = device;
    controller.capability = mmio;
    controller.operational = mmio + caps.cap_length;
    controller.runtime = mmio + caps.runtime_offset;
    controller.doorbells = mmio + caps.doorbell_offset;
    controller.caps = caps;
    controller.dcbaa_phys = 0;
    controller.command_ring_phys = 0;
    controller.event_ring_phys = 0;
    controller.erst_phys = 0;
    controller.scratchpad_array_phys = 0;
    controller.command_cycle = 1;
    controller.command_enqueue = 0;
    controller.event_cycle = 1;
    controller.event_dequeue = 0;
    controller.io_lock = 0;
    controller.command_quarantined = 0;
    for (size_t i = 0; i < 256; ++i) {
        controller.port_enumeration_attempted[i] = false;
    }
    controller.active = false;
    for (size_t i = 0; i < kMaxScratchpadBuffers; ++i) {
        controller.scratchpad_phys[i] = 0;
    }

    log_message(LogLevel::Info,
                "xhci: %02u:%02u.%u version=%x.%02x slots=%u ports=%u intrs=%u scratchpads=%u ctx=%u",
                device.bus,
                device.slot,
                device.function,
                static_cast<unsigned int>(caps.version >> 8),
                static_cast<unsigned int>(caps.version & 0xFFu),
                static_cast<unsigned int>(caps.max_slots),
                static_cast<unsigned int>(caps.max_ports),
                static_cast<unsigned int>(caps.max_interrupters),
                static_cast<unsigned int>(caps.max_scratchpad_buffers),
                caps.context_size_64 ? 64u : 32u);

    log_message(LogLevel::Info, "xhci: resetting controller");
    if (!reset_controller(controller.operational)) {
        log_message(LogLevel::Warn,
                    "xhci: controller reset timed out at %02u:%02u.%u",
                    device.bus,
                    device.slot,
                    device.function);
        return false;
    }
    log_message(LogLevel::Info, "xhci: reset complete");

    log_message(LogLevel::Info, "xhci: setting up rings");
    if (!setup_rings(controller)) {
        log_message(LogLevel::Warn,
                    "xhci: failed to allocate controller rings at %02u:%02u.%u",
                    device.bus,
                    device.slot,
                    device.function);
        return false;
    }
    log_message(LogLevel::Info, "xhci: rings ready");

    uint32_t config = caps.max_slots;
    write32(controller.operational, 0x38, config);

    log_message(LogLevel::Info, "xhci: starting controller");
    if (!start_controller(controller.operational)) {
        log_message(LogLevel::Warn,
                    "xhci: failed to start controller at %02u:%02u.%u",
                    device.bus,
                    device.slot,
                    device.function);
        return false;
    }
    log_message(LogLevel::Info, "xhci: controller started");

    controller.active = true;
    ++g_controller_count;

    log_message(LogLevel::Info, "xhci: powering ports");
    power_ports(controller);
    log_message(LogLevel::Info, "xhci: scanning ports");
    scan_ports(controller);
    log_message(LogLevel::Info, "xhci: port scan complete");
    return true;
}

}  // namespace

void register_driver() {
    (void)driver_registry::register_pci_driver(
        "xhci",
        kPciMatches,
        sizeof(kPciMatches) / sizeof(kPciMatches[0]),
        init);
}

bool register_module() {
    register_driver();
    return true;
}

KERNEL_BUILTIN_MODULE(xhci_module,
                      "xhci",
                      kernel_module::Phase::Driver,
                      register_module,
                      kPciMatches,
                      sizeof(kPciMatches) / sizeof(kPciMatches[0]));

void init() {
    if (g_initialized) {
        return;
    }
    g_initialized = true;
    g_controller_count = 0;
    g_mmio_next_virt = kMmioVirtBase;

    const pci::PciDevice* list = pci::devices();
    size_t count = pci::device_count();
    size_t matched = 0;
    for (size_t i = 0; i < count; ++i) {
        const pci::PciDevice& device = list[i];
        if (device.class_code != 0x0C ||
            device.subclass != 0x03 ||
            device.prog_if != 0x30) {
            continue;
        }
        ++matched;
        log_message(LogLevel::Info,
                    "xhci: matched %04x:%04x at %02u:%02u.%u class=%02x:%02x:%02x",
                    static_cast<unsigned int>(device.vendor),
                    static_cast<unsigned int>(device.device),
                    static_cast<unsigned int>(device.bus),
                    static_cast<unsigned int>(device.slot),
                    static_cast<unsigned int>(device.function),
                    static_cast<unsigned int>(device.class_code),
                    static_cast<unsigned int>(device.subclass),
                    static_cast<unsigned int>(device.prog_if));
        (void)init_controller(device);
    }

    if (matched == 0) {
        log_message(LogLevel::Info, "xhci: no xHCI controllers found");
    } else if (g_controller_count == 0) {
        log_message(LogLevel::Warn,
                    "xhci: %zu controller(s) matched but none initialized",
                    matched);
    }
    if (g_controller_count != 0 && g_enumeration_worker == nullptr) {
        g_enumeration_worker =
            process::allocate_kernel_task(enumeration_worker);
        if (g_enumeration_worker != nullptr) {
            // xHCI command/event polling is currently BSP-affine. Running
            // enumeration on an AP can leave command completions unserviced
            // on real hardware, making an attached device appear forever
            // stuck before enumeration.
            g_enumeration_worker->preferred_cpu = 0;
            process::store_state(*g_enumeration_worker,
                                 process::State::Blocked);
        } else {
            log_message(LogLevel::Warn,
                        "xhci: failed to allocate enumeration worker");
        }
    }
    if (g_controller_count != 0 && g_enumeration_worker != nullptr &&
        !scheduler::register_poll(poll_ports)) {
        log_message(LogLevel::Warn,
                    "xhci: failed to register port hotplug poller");
    }
}

}  // namespace xhci
