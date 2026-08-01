#include "drivers/gpu/intel_uhd.hpp"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/memory/paging.hpp"
#include "drivers/console/console.hpp"
#include "drivers/driver_registry.hpp"
#include "drivers/log/logging.hpp"
#include "drivers/pci/pci.hpp"
#include "kernel/cmdline.hpp"
#include "kernel/descriptor.hpp"
#include "kernel/memory/physical_allocator.hpp"
#include "kernel/module.hpp"
#include "lib/mem.hpp"

namespace intel_uhd {

namespace {

constexpr uint16_t kIntelVendorId = 0x8086;

#define INTEL_UHD_SUPPORTED_DEVICES(X) \
    X(0x3184, "Gemini Lake UHD 600/605") \
    X(0x3185, "Gemini Lake UHD 600/605") \
    X(0x5916, "Kaby Lake UHD 620") \
    X(0x5917, "Kaby Lake UHD 620") \
    X(0x591B, "Kaby Lake UHD 630") \
    X(0x3EA0, "Whiskey Lake UHD 620") \
    X(0x3EA9, "Coffee Lake UHD 630") \
    X(0x3E91, "Coffee Lake UHD 630") \
    X(0x3E92, "Coffee Lake UHD 630") \
    X(0x3E98, "Coffee Lake UHD 630") \
    X(0x9BC4, "Comet Lake UHD 630") \
    X(0x9BC5, "Comet Lake UHD 630")

struct DeviceInfo {
    uint16_t device_id;
    const char* name;
};

constexpr DeviceInfo kSupportedDevices[] = {
#define INTEL_UHD_DEVICE_INFO(device_id, name) {device_id, name},
    INTEL_UHD_SUPPORTED_DEVICES(INTEL_UHD_DEVICE_INFO)
#undef INTEL_UHD_DEVICE_INFO
};

constexpr driver_registry::PciMatch kPciMatches[] = {
#define INTEL_UHD_PCI_MATCH(device_id, name) \
    {.vendor = kIntelVendorId, .device = device_id, .class_code = 0x03, .subclass = driver_registry::kAnySubclass, .prog_if = driver_registry::kAnyProgIf},
    INTEL_UHD_SUPPORTED_DEVICES(INTEL_UHD_PCI_MATCH)
#undef INTEL_UHD_PCI_MATCH
};

#undef INTEL_UHD_SUPPORTED_DEVICES

constexpr uint16_t kPciCommandIo = 1u << 0;
constexpr uint16_t kPciCommandMemory = 1u << 1;
constexpr uint16_t kPciCommandBusMaster = 1u << 2;

constexpr uint64_t kMapVirtBase = 0xFFFFE10060000000ull;
constexpr size_t kMapWindowSize = 32ull * 1024 * 1024;
constexpr size_t kPageSize = 4096;
constexpr size_t kMmioMapSize = 2ull * 1024 * 1024;
constexpr size_t kBltRingSize = 16ull * 1024;
constexpr size_t kBltHwspSize = kPageSize;
constexpr uint32_t kBltRingBase = 0x22000;
constexpr uint32_t kRingTail = 0x30;
constexpr uint32_t kRingHead = 0x34;
constexpr uint32_t kRingStart = 0x38;
constexpr uint32_t kRingCtl = 0x3C;
constexpr uint32_t kRingHwsPga = 0x80;
constexpr uint32_t kRingHwstam = 0x98;
constexpr uint32_t kRingMiMode = 0x9C;
constexpr uint32_t kRingValid = 0x1u;
constexpr uint32_t kRingStop = 1u << 8;
constexpr uint32_t kModeIdle = 1u << 9;
constexpr uint32_t kTailAddrMask = 0x001FFFF8u;
constexpr uint32_t kHeadAddrMask = 0x001FFFFCu;
constexpr uint32_t kGfxFlushCntlGen6 = 0x101008;
constexpr uint32_t kGfxFlushCntlEn = 1u << 0;
constexpr uint32_t kMiNoop = 0x00000000u;
constexpr uint32_t kXySrcCopyBltCmd = (2u << 29) | (0x53u << 22);
constexpr uint32_t kBltWriteRgba = (1u << 20) | (2u << 20);
constexpr uint32_t kBltDepth32 = 3u << 24;
constexpr uint32_t kBltRopSrcCopy = 0xCCu << 16;
constexpr uint64_t kGen8GgttPagePresent = 1ull << 0;

constexpr uint32_t REG_GMCH_GMS = 0x50;
constexpr uint32_t REG_GFX_MODE = 0x2520;
constexpr uint32_t REG_GGTT_PAT = 0x40E0;
constexpr uint32_t REG_CDCLK_CTL = 0x46000;

constexpr uint32_t TRANS_HTOTAL_A = 0x60000;
constexpr uint32_t TRANS_VTOTAL_A = 0x6000C;
constexpr uint32_t TRANS_DDI_FUNC_CTL_A = 0x60400;
constexpr uint32_t PIPE_SRC_A = 0x6001C;
constexpr uint32_t PIPECONF_A = 0x70008;
constexpr uint32_t PIPE_STRIDE = 0x1000;
constexpr uint32_t PLANE_CTL_1_A = 0x70180;
constexpr uint32_t PLANE_STRIDE_1_A = 0x70188;
constexpr uint32_t PLANE_SIZE_1_A = 0x70190;
constexpr uint32_t PLANE_SURF_1_A = 0x7019C;
constexpr uint32_t PLANE_OFFSET_1_A = 0x701A4;
constexpr uint32_t PLANE_SURFLIVE_1_A = 0x701AC;

constexpr uint32_t PIPECONF_ENABLE = 1u << 31;
constexpr uint32_t DDI_FUNC_ENABLE = 1u << 31;
constexpr uint32_t PLANE_CTL_ENABLE = 1u << 31;
constexpr uint32_t PLANE_CTL_FORMAT_MASK = 0x0Fu << 24;
constexpr uint32_t PLANE_CTL_FORMAT_XRGB_8888 = 0x04u << 24;
constexpr uint32_t PLANE_CTL_ORDER_RGBX = 1u << 20;
constexpr uint32_t PLANE_CTL_TILED_MASK = 0x07u << 10;
constexpr uint32_t PLANE_CTL_TILED_LINEAR = 0x00u << 10;
constexpr uint32_t PLANE_STRIDE_MASK = 0x0FFFu;
constexpr uint32_t PLANE_STRIDE_GRANULARITY = 64u;

struct PipeState {
    bool enabled;
    uint32_t pipeconf;
    uint32_t source;
    uint32_t htotal;
    uint32_t vtotal;
    uint32_t ddi_func_ctl;
};

struct PlaneState {
    bool enabled;
    bool linear;
    bool xrgb8888;
    bool rgbx_order;
    uint32_t ctl;
    uint32_t stride;
    uint32_t size;
    uint32_t surf;
    uint32_t surf_live;
    uint32_t offset;
    uint16_t width;
    uint16_t height;
    uint16_t offset_x;
    uint16_t offset_y;
};

struct DriverState {
    bool initialized;
    bool active;
    bool blt_ready;
    pci::PciDevice device;
    const DeviceInfo* info;
    volatile uint8_t* regs;
    uint8_t* scanout_base;
    uint64_t mmio_phys;
    uint64_t aperture_phys;
    uint64_t mmio_size;
    uint64_t aperture_size;
    uint64_t scanout_phys;
    uint64_t scanout_ggtt_offset;
    size_t scanout_bytes;
    uint64_t ggtt_table_phys;
    uint64_t blt_hwsp_phys;
    uint64_t blt_ring_phys;
    uint64_t blt_hwsp_ggtt;
    uint64_t blt_ring_ggtt;
    volatile uint32_t* blt_ring_cpu;
    uint32_t blt_ring_tail;
    uint16_t command;
    PipeState pipes[3];
    PlaneState planes[3];
    int active_pipe;
};

DriverState g_state{};
uint64_t g_map_next_virt = kMapVirtBase;
bool g_disabled = false;

uint64_t align_down_u64(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

uint64_t align_up_u64(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

const DeviceInfo* lookup_device_info(uint16_t device_id) {
    for (size_t i = 0; i < sizeof(kSupportedDevices) / sizeof(kSupportedDevices[0]); ++i) {
        if (kSupportedDevices[i].device_id == device_id) {
            return &kSupportedDevices[i];
        }
    }
    return nullptr;
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

uint64_t pci_bar_size(const pci::PciDevice& device, uint8_t bar_index) {
    if (bar_index >= 6) {
        return 0;
    }

    const uint8_t reg = static_cast<uint8_t>(0x10 + (bar_index * 4));
    const uint32_t original_low = pci::read_config32(device, reg);
    if ((original_low & 0x1u) != 0) {
        return 0;
    }

    const bool is_64 = ((original_low >> 1) & 0x3u) == 0x2u;
    uint32_t original_high = 0;
    if (is_64) {
        if (bar_index + 1 >= 6) {
            return 0;
        }
        original_high = pci::read_config32(device, static_cast<uint8_t>(reg + 4));
    }

    pci::write_config32(device, reg, 0xFFFFFFFFu);
    uint32_t size_low = pci::read_config32(device, reg);
    uint32_t size_high = 0;
    if (is_64) {
        pci::write_config32(device, static_cast<uint8_t>(reg + 4), 0xFFFFFFFFu);
        size_high = pci::read_config32(device, static_cast<uint8_t>(reg + 4));
    }

    pci::write_config32(device, reg, original_low);
    if (is_64) {
        pci::write_config32(device, static_cast<uint8_t>(reg + 4), original_high);
    }

    uint64_t mask = static_cast<uint64_t>(size_low & ~0xFu);
    if (is_64) {
        mask |= static_cast<uint64_t>(size_high) << 32;
    }
    if (mask == 0) {
        return 0;
    }
    return (~mask) + 1ull;
}

uint8_t* map_physical_range(uint64_t phys_base,
                            size_t length,
                            uint64_t page_flags,
                            const char* label) {
    if (phys_base == 0 || length == 0) {
        return nullptr;
    }

    uint64_t page_phys = align_down_u64(phys_base, kPageSize);
    uint64_t page_end = align_up_u64(phys_base + length, kPageSize);
    size_t page_count = static_cast<size_t>((page_end - page_phys) / kPageSize);
    if (page_count == 0) {
        page_count = 1;
    }

    uint64_t virt_base = g_map_next_virt;
    uint64_t virt_end =
        virt_base + static_cast<uint64_t>(page_count) * kPageSize;
    if (virt_end - kMapVirtBase > kMapWindowSize) {
        log_message(LogLevel::Warn,
                    "intel-uhd: virtual window exhausted while mapping %s",
                    (label != nullptr) ? label : "range");
        return nullptr;
    }

    for (size_t i = 0; i < page_count; ++i) {
        uint64_t phys = page_phys + static_cast<uint64_t>(i) * kPageSize;
        uint64_t virt = virt_base + static_cast<uint64_t>(i) * kPageSize;
        if (!paging_map_page(virt,
                             phys,
                             page_flags | PAGE_FLAG_NO_EXECUTE)) {
            log_message(LogLevel::Warn,
                        "intel-uhd: failed to map %s page phys=%016llx",
                        (label != nullptr) ? label : "range",
                        static_cast<unsigned long long>(phys));
            return nullptr;
        }
    }

    g_map_next_virt = virt_end;
    return reinterpret_cast<uint8_t*>(virt_base + (phys_base - page_phys));
}

volatile uint8_t* map_bar_window(const pci::PciDevice& device,
                                 uint8_t bar_index,
                                 size_t length) {
    uint64_t bar_base = pci_bar_base(device, bar_index);
    if (bar_base == 0 || length == 0) {
        return nullptr;
    }

    const uint64_t mmio_flags =
        PAGE_FLAG_WRITE | PAGE_FLAG_WRITE_THROUGH | PAGE_FLAG_CACHE_DISABLE;
    return reinterpret_cast<volatile uint8_t*>(
        map_physical_range(bar_base, length, mmio_flags, "MMIO"));
}

uint32_t mmio_read32(uint32_t reg) {
    if (g_state.regs == nullptr) {
        return 0xFFFFFFFFu;
    }
    return *reinterpret_cast<volatile uint32_t*>(
        const_cast<volatile uint8_t*>(g_state.regs) + reg);
}

void mmio_write32(uint32_t reg, uint32_t value) {
    if (g_state.regs == nullptr) {
        return;
    }
    *reinterpret_cast<volatile uint32_t*>(
        const_cast<volatile uint8_t*>(g_state.regs) + reg) = value;
}

bool wait_for_blt_idle(uint32_t timeout_iterations = 1000000u) {
    if (g_state.regs == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < timeout_iterations; ++i) {
        uint32_t head = mmio_read32(kBltRingBase + kRingHead) & kHeadAddrMask;
        uint32_t tail = mmio_read32(kBltRingBase + kRingTail) & kTailAddrMask;
        uint32_t mode = mmio_read32(kBltRingBase + kRingMiMode);
        if (head == tail && (mode & kModeIdle) != 0) {
            return true;
        }
        asm volatile("pause");
    }
    return false;
}

bool program_ggtt_pages(uint64_t ggtt_offset,
                        uint64_t phys_base,
                        size_t length) {
    if (g_state.ggtt_table_phys == 0 || length == 0) {
        return false;
    }
    const uint64_t first_entry = ggtt_offset / kPageSize;
    const uint64_t page_count = align_up_u64(length, kPageSize) / kPageSize;
    const uint64_t pte_phys = g_state.ggtt_table_phys + first_entry * sizeof(uint64_t);
    auto* ptes = reinterpret_cast<volatile uint64_t*>(
        map_physical_range(pte_phys,
                           static_cast<size_t>(page_count * sizeof(uint64_t)),
                           PAGE_FLAG_WRITE | PAGE_FLAG_WRITE_THROUGH |
                               PAGE_FLAG_CACHE_DISABLE,
                           "ggtt-ptes"));
    if (ptes == nullptr) {
        return false;
    }
    for (uint64_t i = 0; i < page_count; ++i) {
        ptes[i] = (phys_base + i * kPageSize) | kGen8GgttPagePresent;
    }
    mmio_write32(kGfxFlushCntlGen6, kGfxFlushCntlEn);
    (void)mmio_read32(kGfxFlushCntlGen6);
    return true;
}

bool setup_blt_engine() {
    if (g_state.blt_ready) {
        return true;
    }
    if (g_state.scanout_ggtt_offset == 0 || g_state.aperture_size == 0 ||
        g_state.ggtt_table_phys == 0) {
        return false;
    }

    const uint64_t reserve_start =
        align_up_u64(g_state.scanout_ggtt_offset + g_state.scanout_bytes + kPageSize,
                     kPageSize);
    const uint64_t reserve_bytes = kBltHwspSize + kBltRingSize;
    if (reserve_start + reserve_bytes > g_state.aperture_size) {
        log_message(LogLevel::Warn,
                    "intel-uhd: no GGTT space for BLT ring offset=%016llx size=%llu aperture=%llu",
                    static_cast<unsigned long long>(reserve_start),
                    static_cast<unsigned long long>(reserve_bytes),
                    static_cast<unsigned long long>(g_state.aperture_size));
        return false;
    }

    const size_t hwsp_pages = kBltHwspSize / kPageSize;
    const size_t ring_pages = kBltRingSize / kPageSize;
    g_state.blt_hwsp_phys = memory::alloc_kernel_block_pages(hwsp_pages);
    g_state.blt_ring_phys = memory::alloc_kernel_block_pages(ring_pages);
    if (g_state.blt_hwsp_phys == 0 || g_state.blt_ring_phys == 0) {
        log_message(LogLevel::Warn, "intel-uhd: failed to allocate BLT backing pages");
        return false;
    }

    auto* hwsp = static_cast<uint8_t*>(paging_phys_to_virt(g_state.blt_hwsp_phys));
    auto* ring = static_cast<uint32_t*>(paging_phys_to_virt(g_state.blt_ring_phys));
    memset(hwsp, 0, kBltHwspSize);
    memset(ring, 0, kBltRingSize);

    g_state.blt_hwsp_ggtt = reserve_start;
    g_state.blt_ring_ggtt = reserve_start + kBltHwspSize;
    if (!program_ggtt_pages(g_state.blt_hwsp_ggtt, g_state.blt_hwsp_phys, kBltHwspSize) ||
        !program_ggtt_pages(g_state.blt_ring_ggtt, g_state.blt_ring_phys, kBltRingSize)) {
        return false;
    }

    mmio_write32(kBltRingBase + kRingHead, mmio_read32(kBltRingBase + kRingTail));
    (void)mmio_read32(kBltRingBase + kRingHead);
    mmio_write32(kBltRingBase + kRingCtl, 0);
    (void)mmio_read32(kBltRingBase + kRingCtl);
    mmio_write32(kBltRingBase + kRingHead, 0);
    mmio_write32(kBltRingBase + kRingTail, 0);
    mmio_write32(kBltRingBase + kRingHwsPga, static_cast<uint32_t>(g_state.blt_hwsp_ggtt));
    mmio_write32(kBltRingBase + kRingHwstam, 0xFFFFFFFFu);
    mmio_write32(kBltRingBase + kRingStart, static_cast<uint32_t>(g_state.blt_ring_ggtt));
    mmio_write32(kBltRingBase + kRingCtl,
                 static_cast<uint32_t>((kBltRingSize - kPageSize) | kRingValid));
    (void)mmio_read32(kBltRingBase + kRingCtl);
    mmio_write32(kBltRingBase + kRingMiMode, mmio_read32(kBltRingBase + kRingMiMode) & ~kRingStop);
    (void)mmio_read32(kBltRingBase + kRingMiMode);

    g_state.blt_ring_cpu = ring;
    g_state.blt_ring_tail = 0;
    g_state.blt_ready = true;
    log_message(LogLevel::Info,
                "intel-uhd: BLT ring ready ring=%016llx hwsp=%016llx",
                static_cast<unsigned long long>(g_state.blt_ring_ggtt),
                static_cast<unsigned long long>(g_state.blt_hwsp_ggtt));
    return true;
}

bool submit_blt_commands(const uint32_t* commands, size_t dword_count) {
    if (commands == nullptr || dword_count == 0 || g_state.blt_ring_cpu == nullptr) {
        return false;
    }
    const size_t bytes = dword_count * sizeof(uint32_t);
    if (bytes > kBltRingSize) {
        return false;
    }
    if (g_state.blt_ring_tail + bytes > kBltRingSize) {
        if (!wait_for_blt_idle()) {
            return false;
        }
        mmio_write32(kBltRingBase + kRingHead, 0);
        mmio_write32(kBltRingBase + kRingTail, 0);
        g_state.blt_ring_tail = 0;
    }
    for (size_t i = 0; i < dword_count; ++i) {
        g_state.blt_ring_cpu[(g_state.blt_ring_tail / sizeof(uint32_t)) + i] = commands[i];
    }
    asm volatile("mfence" : : : "memory");
    g_state.blt_ring_tail += static_cast<uint32_t>(bytes);
    mmio_write32(kBltRingBase + kRingTail, g_state.blt_ring_tail);
    if (!wait_for_blt_idle()) {
        log_message(LogLevel::Warn, "intel-uhd: BLT command timed out");
        return false;
    }
    return true;
}

uint16_t decode_active_dimension(uint32_t reg_value) {
    return static_cast<uint16_t>(((reg_value >> 16) & 0x1FFFu) + 1u);
}

uint16_t decode_total_dimension(uint32_t reg_value) {
    return static_cast<uint16_t>((reg_value & 0x1FFFu) + 1u);
}

char pipe_name(size_t index) {
    return static_cast<char>('A' + index);
}

uint16_t decode_plane_dimension(uint32_t reg_value, uint32_t shift) {
    return static_cast<uint16_t>(((reg_value >> shift) & 0x0FFFu) + 1u);
}

void capture_pipe_state(size_t pipe_index) {
    if (pipe_index >= 3) {
        return;
    }

    PipeState& pipe = g_state.pipes[pipe_index];
    uint32_t stride = static_cast<uint32_t>(pipe_index) * PIPE_STRIDE;
    pipe.htotal = mmio_read32(TRANS_HTOTAL_A + stride);
    pipe.vtotal = mmio_read32(TRANS_VTOTAL_A + stride);
    pipe.source = mmio_read32(PIPE_SRC_A + stride);
    pipe.pipeconf = mmio_read32(PIPECONF_A + stride);
    pipe.ddi_func_ctl = mmio_read32(TRANS_DDI_FUNC_CTL_A + stride);
    pipe.enabled = ((pipe.pipeconf & PIPECONF_ENABLE) != 0) ||
                   ((pipe.ddi_func_ctl & DDI_FUNC_ENABLE) != 0);
}

void capture_plane_state(size_t pipe_index) {
    if (pipe_index >= 3) {
        return;
    }

    PlaneState& plane = g_state.planes[pipe_index];
    uint32_t stride = static_cast<uint32_t>(pipe_index) * PIPE_STRIDE;
    plane.ctl = mmio_read32(PLANE_CTL_1_A + stride);
    plane.stride = mmio_read32(PLANE_STRIDE_1_A + stride);
    plane.size = mmio_read32(PLANE_SIZE_1_A + stride);
    plane.surf = mmio_read32(PLANE_SURF_1_A + stride);
    plane.surf_live = mmio_read32(PLANE_SURFLIVE_1_A + stride);
    plane.offset = mmio_read32(PLANE_OFFSET_1_A + stride);
    plane.enabled = (plane.ctl & PLANE_CTL_ENABLE) != 0;
    plane.linear = (plane.ctl & PLANE_CTL_TILED_MASK) == PLANE_CTL_TILED_LINEAR;
    plane.xrgb8888 =
        (plane.ctl & PLANE_CTL_FORMAT_MASK) == PLANE_CTL_FORMAT_XRGB_8888;
    plane.rgbx_order = (plane.ctl & PLANE_CTL_ORDER_RGBX) != 0;
    plane.width = decode_plane_dimension(plane.size, 0);
    plane.height = decode_plane_dimension(plane.size, 16);
    plane.offset_x = static_cast<uint16_t>(plane.offset & 0xFFFFu);
    plane.offset_y = static_cast<uint16_t>((plane.offset >> 16) & 0xFFFFu);
}

uint32_t plane_stride_bytes(const PlaneState& plane) {
    return (plane.stride & PLANE_STRIDE_MASK) * PLANE_STRIDE_GRANULARITY;
}

void log_pipe_state(size_t pipe_index) {
    if (pipe_index >= 3) {
        return;
    }

    const PipeState& pipe = g_state.pipes[pipe_index];
    if (!pipe.enabled) {
        log_message(LogLevel::Info, "intel-uhd: pipe %c disabled",
                    static_cast<unsigned int>(pipe_name(pipe_index)));
        return;
    }

    uint16_t width = decode_active_dimension(pipe.source);
    uint16_t height = static_cast<uint16_t>((pipe.source & 0x1FFFu) + 1u);
    uint16_t htotal = decode_total_dimension(pipe.htotal);
    uint16_t hactive = decode_active_dimension(pipe.htotal);
    uint16_t vtotal = decode_total_dimension(pipe.vtotal);
    uint16_t vactive = decode_active_dimension(pipe.vtotal);

    log_message(
        LogLevel::Info,
        "intel-uhd: pipe %c active mode=%ux%u hactive=%u htotal=%u vactive=%u vtotal=%u pipeconf=%08x ddi=%08x",
        static_cast<unsigned int>(pipe_name(pipe_index)),
        static_cast<unsigned int>(width),
        static_cast<unsigned int>(height),
        static_cast<unsigned int>(hactive),
        static_cast<unsigned int>(htotal),
        static_cast<unsigned int>(vactive),
        static_cast<unsigned int>(vtotal),
        static_cast<unsigned int>(pipe.pipeconf),
        static_cast<unsigned int>(pipe.ddi_func_ctl));
}

bool bind_current_scanout() {
    if (g_state.aperture_phys == 0) {
        log_message(LogLevel::Warn, "intel-uhd: missing BAR2 aperture");
        return false;
    }

    const size_t expected_width = 0;
    const size_t expected_height = 0;
    char failure_summary[256];
    size_t failure_used = 0;
    failure_summary[0] = '\0';

    auto append_summary = [&](const char* text) {
        if (text == nullptr || failure_used + 1 >= sizeof(failure_summary)) {
            return;
        }
        while (*text != '\0' && failure_used + 1 < sizeof(failure_summary)) {
            failure_summary[failure_used++] = *text++;
        }
        failure_summary[failure_used] = '\0';
    };

    auto append_char = [&](char ch) {
        if (failure_used + 1 >= sizeof(failure_summary)) {
            return;
        }
        failure_summary[failure_used++] = ch;
        failure_summary[failure_used] = '\0';
    };

    auto append_u32 = [&](uint32_t value) {
        char digits[10];
        size_t count = 0;
        if (value == 0) {
            append_char('0');
            return;
        }
        while (value != 0 && count < sizeof(digits)) {
            digits[count++] = static_cast<char>('0' + (value % 10u));
            value /= 10u;
        }
        while (count > 0) {
            append_char(digits[--count]);
        }
    };

    auto append_pipe_summary = [&](size_t pipe,
                                   const PlaneState& plane,
                                   const PipeState& pipe_state,
                                   const char* reason) {
        const char pipe_char = pipe_name(pipe);
        uint32_t stride_bytes = plane_stride_bytes(plane);
        if (reason == nullptr) {
            return;
        }
        append_char(pipe_char);
        append_summary(":e");
        append_u32(plane.enabled ? 1u : 0u);
        append_summary("/p");
        append_u32(pipe_state.enabled ? 1u : 0u);
        append_summary("/l");
        append_u32(plane.linear ? 1u : 0u);
        append_summary("/x");
        append_u32(plane.xrgb8888 ? 1u : 0u);
        append_char('/');
        append_u32(plane.width);
        append_char('x');
        append_u32(plane.height);
        append_summary("/s");
        append_u32(stride_bytes);
        append_char(':');
        append_summary(reason);
        append_char(' ');
    };

    for (size_t pipe = 0; pipe < 3; ++pipe) {
        const PipeState& pipe_state = g_state.pipes[pipe];
        const PlaneState& plane = g_state.planes[pipe];
        if (!plane.enabled) {
            append_pipe_summary(pipe, plane, pipe_state, "off");
            continue;
        }
        if (!plane.linear || !plane.xrgb8888) {
            append_pipe_summary(pipe, plane, pipe_state, "fmt");
            continue;
        }

        uint32_t stride_bytes = plane_stride_bytes(plane);
        if (stride_bytes == 0 || plane.width == 0 || plane.height == 0) {
            append_pipe_summary(pipe, plane, pipe_state, "geom");
            continue;
        }
        if (expected_width != 0 && expected_height != 0 &&
            (plane.width != expected_width ||
             plane.height != expected_height)) {
            append_pipe_summary(pipe, plane, pipe_state, "size");
            continue;
        }
        if (plane.width > UINT32_MAX / 4u ||
            stride_bytes < plane.width * 4u) {
            append_pipe_summary(pipe, plane, pipe_state, "stride");
            continue;
        }

        uint32_t surf_reg = (plane.surf_live != 0) ? plane.surf_live : plane.surf;
        uint64_t ggtt_offset = static_cast<uint64_t>(surf_reg & 0xFFFFF000u);
        uint64_t y_offset =
            static_cast<uint64_t>(plane.offset_y) * stride_bytes;
        uint64_t x_offset = static_cast<uint64_t>(plane.offset_x) * 4u;
        if (y_offset > UINT64_MAX - ggtt_offset ||
            x_offset > UINT64_MAX - (ggtt_offset + y_offset)) {
            append_pipe_summary(pipe, plane, pipe_state, "offset");
            continue;
        }
        ggtt_offset += y_offset + x_offset;

        if (plane.height > static_cast<size_t>(-1) / stride_bytes) {
            append_pipe_summary(pipe, plane, pipe_state, "bytes");
            continue;
        }
        size_t frame_bytes = static_cast<size_t>(stride_bytes) * plane.height;
        if (g_state.aperture_phys == 0 || g_state.aperture_size == 0 ||
            ggtt_offset > g_state.aperture_size ||
            frame_bytes > g_state.aperture_size - ggtt_offset ||
            ggtt_offset > UINT64_MAX - g_state.aperture_phys) {
            append_pipe_summary(pipe, plane, pipe_state, "aperture");
            continue;
        }
        uint64_t phys = g_state.aperture_phys + ggtt_offset;
        uint8_t* base = map_physical_range(phys, frame_bytes, PAGE_FLAG_WRITE, "scanout");
        if (base == nullptr) {
            return false;
        }
        if (!paging_mark_wc(reinterpret_cast<uint64_t>(base), frame_bytes)) {
            log_message(LogLevel::Warn,
                        "intel-uhd: failed to mark scanout WC (virt=%016llx len=%llu)",
                        reinterpret_cast<unsigned long long>(base),
                        static_cast<unsigned long long>(frame_bytes));
        }

        Framebuffer fb{};
        fb.base = base;
        fb.width = plane.width;
        fb.height = plane.height;
        fb.pitch = stride_bytes;
        fb.bpp = 32;
        if (plane.rgbx_order) {
            fb.red_mask_size = 8;
            fb.red_mask_shift = 0;
            fb.green_mask_size = 8;
            fb.green_mask_shift = 8;
            fb.blue_mask_size = 8;
            fb.blue_mask_shift = 16;
        } else {
            fb.red_mask_size = 8;
            fb.red_mask_shift = 16;
            fb.green_mask_size = 8;
            fb.green_mask_shift = 8;
            fb.blue_mask_size = 8;
            fb.blue_mask_shift = 0;
        }
        descriptor::register_framebuffer_device(fb, phys);
        if (kconsole != nullptr) {
            kconsole->present();
        }

        g_state.scanout_base = base;
        g_state.scanout_phys = phys;
        g_state.scanout_ggtt_offset = ggtt_offset;
        g_state.scanout_bytes = frame_bytes;
        g_state.active_pipe = static_cast<int>(pipe);

        log_message(
            LogLevel::Info,
            "intel-uhd: adopted pipe %c scanout base=%016llx size=%zu stride=%u",
            static_cast<unsigned int>(pipe_name(pipe)),
            static_cast<unsigned long long>(phys),
            frame_bytes,
            static_cast<unsigned int>(stride_bytes));
        if (!pipe_state.enabled) {
            log_message(LogLevel::Warn,
                        "intel-uhd: adopted plane %c.1 even though pipe %c did not report active",
                        static_cast<unsigned int>(pipe_name(pipe)),
                        static_cast<unsigned int>(pipe_name(pipe)));
        }
        return true;
    }

    log_message(LogLevel::Warn,
                "intel-uhd: no adoptable primary plane found expected=%zux%zu %s",
                expected_width,
                expected_height,
                failure_summary);
    return false;
}

void log_device_state() {
    uint16_t gmch = pci::read_config16(g_state.device, REG_GMCH_GMS);
    uint32_t gfx_mode = mmio_read32(REG_GFX_MODE);
    uint32_t ggtt_pat = mmio_read32(REG_GGTT_PAT);
    uint32_t cdclk = mmio_read32(REG_CDCLK_CTL);

    log_message(
        LogLevel::Info,
        "intel-uhd: online at %02u:%02u.%u device=%04x (%s) mmio=%016llx aperture=%016llx cmd=%04x gmch=%04x gfx_mode=%08x ggtt_pat=%08x cdclk=%08x",
        static_cast<unsigned int>(g_state.device.bus),
        static_cast<unsigned int>(g_state.device.slot),
        static_cast<unsigned int>(g_state.device.function),
        static_cast<unsigned int>(g_state.device.device),
        (g_state.info != nullptr) ? g_state.info->name : "unknown",
        static_cast<unsigned long long>(g_state.mmio_phys),
        static_cast<unsigned long long>(g_state.aperture_phys),
        static_cast<unsigned int>(g_state.command),
        static_cast<unsigned int>(gmch),
        static_cast<unsigned int>(gfx_mode),
        static_cast<unsigned int>(ggtt_pat),
        static_cast<unsigned int>(cdclk));

    for (size_t i = 0; i < 3; ++i) {
        log_pipe_state(i);
    }
}

bool init_device(const pci::PciDevice& device) {
    const DeviceInfo* info = lookup_device_info(device.device);
    if (info == nullptr) {
        return false;
    }

    DriverState next{};
    next.initialized = true;
    next.device = device;
    next.info = info;
    next.mmio_phys = pci_bar_base(device, 0);
    next.aperture_phys = pci_bar_base(device, 2);
    next.mmio_size = pci_bar_size(device, 0);
    next.aperture_size = pci_bar_size(device, 2);
    if (next.mmio_phys == 0) {
        log_message(LogLevel::Warn,
                    "intel-uhd: missing BAR0 on %02u:%02u.%u",
                    static_cast<unsigned int>(device.bus),
                    static_cast<unsigned int>(device.slot),
                    static_cast<unsigned int>(device.function));
        return false;
    }

    uint16_t command = pci::read_config16(device, 0x04);
    command |= static_cast<uint16_t>(kPciCommandMemory | kPciCommandBusMaster);
    command &= static_cast<uint16_t>(~kPciCommandIo);
    pci::write_config16(device, 0x04, command);
    next.command = pci::read_config16(device, 0x04);

    next.regs = map_bar_window(device, 0, kMmioMapSize);
    if (next.regs == nullptr) {
        log_message(LogLevel::Warn,
                    "intel-uhd: failed to map BAR0 for %02u:%02u.%u",
                    static_cast<unsigned int>(device.bus),
                    static_cast<unsigned int>(device.slot),
                    static_cast<unsigned int>(device.function));
        return false;
    }

    if (next.mmio_size >= 2 * kPageSize) {
        next.ggtt_table_phys = next.mmio_phys + (next.mmio_size / 2u);
    }

    g_state = next;
    for (size_t i = 0; i < 3; ++i) {
        capture_pipe_state(i);
        capture_plane_state(i);
    }

    (void)bind_current_scanout();
    g_state.active = true;
    log_device_state();
    return true;
}

}  // namespace

void configure() {
    g_disabled =
        kernel_cmdline::has_value("INTEL_UHD", "OFF") ||
        kernel_cmdline::has_flag("INTEL_UHD.DISABLE");
}

void register_driver() {
    if (g_disabled) {
        log_message(LogLevel::Info,
                    "intel-uhd: disabled by kernel command line");
        return;
    }
    (void)driver_registry::register_pci_driver(
        "intel-uhd",
        kPciMatches,
        sizeof(kPciMatches) / sizeof(kPciMatches[0]),
        init);
}

bool register_module() {
    register_driver();
    return true;
}

KERNEL_BUILTIN_MODULE(intel_uhd_module,
                      "intel-uhd",
                      kernel_module::Phase::Driver,
                      register_module,
                      kPciMatches,
                      sizeof(kPciMatches) / sizeof(kPciMatches[0]));

void init() {
    if (g_state.initialized || g_disabled) {
        return;
    }

    g_state.initialized = true;

    const pci::PciDevice* list = pci::devices();
    size_t count = pci::device_count();
    for (size_t i = 0; i < count; ++i) {
        const pci::PciDevice& device = list[i];
        if (device.vendor != kIntelVendorId || device.class_code != 0x03) {
            continue;
        }

        const DeviceInfo* info = lookup_device_info(device.device);
        if (info == nullptr) {
            log_message(LogLevel::Info,
                        "intel-uhd: skipping unsupported Intel graphics device %04x at %02u:%02u.%u",
                        static_cast<unsigned int>(device.device),
                        static_cast<unsigned int>(device.bus),
                        static_cast<unsigned int>(device.slot),
                        static_cast<unsigned int>(device.function));
            continue;
        }

        if (init_device(device)) {
            return;
        }
    }

    log_message(LogLevel::Info, "intel-uhd: no supported device found");
}

bool available() {
    return g_state.active;
}

bool blit_copy(unsigned int src_x,
               unsigned int src_y,
               unsigned int dst_x,
               unsigned int dst_y,
               unsigned int width,
               unsigned int height,
               unsigned int pitch_bytes) {
    if (!g_state.active || width == 0 || height == 0 || pitch_bytes == 0) {
        return false;
    }
    if (src_x == dst_x && src_y == dst_y) {
        return true;
    }
    if (!setup_blt_engine()) {
        return false;
    }

    const uint32_t max_x = static_cast<uint32_t>(g_state.planes[g_state.active_pipe].width);
    const uint32_t max_y = static_cast<uint32_t>(g_state.planes[g_state.active_pipe].height);
    if (src_x + width > max_x || dst_x + width > max_x ||
        src_y + height > max_y || dst_y + height > max_y) {
        return false;
    }

    uint32_t commands[10]{};
    commands[0] = kXySrcCopyBltCmd | kBltWriteRgba | ((8 - 2) + 2);
    commands[1] = kBltDepth32 | kBltRopSrcCopy | pitch_bytes;
    commands[2] = (dst_y << 16) | dst_x;
    commands[3] = ((dst_y + height) << 16) | (dst_x + width);
    commands[4] = static_cast<uint32_t>(g_state.scanout_ggtt_offset & 0xFFFFFFFFu);
    commands[5] = static_cast<uint32_t>(g_state.scanout_ggtt_offset >> 32);
    commands[6] = (src_y << 16) | src_x;
    commands[7] = pitch_bytes;
    commands[8] = static_cast<uint32_t>(g_state.scanout_ggtt_offset & 0xFFFFFFFFu);
    commands[9] = static_cast<uint32_t>(g_state.scanout_ggtt_offset >> 32);
    return submit_blt_commands(commands, sizeof(commands) / sizeof(commands[0]));
}

}  // namespace intel_uhd
