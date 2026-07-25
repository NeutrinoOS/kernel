#pragma once

#include <stdint.h>
#include <stddef.h>

namespace descriptor_defs {

enum class Type : uint16_t {
    Console     = 0x001,
    Serial      = 0x002,
    Keyboard    = 0x003,
    Mouse       = 0x004,
    Framebuffer = 0x010,
    GraphicalSession = 0x011,
    BlockDevice = 0x020,
    Disk        = 0x021,
    Partition   = 0x022,
    Pipe        = 0x030,
    SharedMemory = 0x040,
    Vty         = 0x050,
    CpuStats    = 0x060,
    TaskStats   = 0x061,
    KernelLog   = 0x062,
    NetDevice   = 0x070,
    NetEndpoint = 0x071,
    Pci         = 0x080,
    AudioOutput = 0x090,
    Sensor      = 0x0A0,
};

enum class Flag : uint64_t {
    Readable    = 1ull << 0,
    Writable    = 1ull << 1,
    Seekable    = 1ull << 2,
    Mappable    = 1ull << 3,
    Async       = 1ull << 8,
    EventSource = 1ull << 9,
    Device      = 1ull << 10,
    Block       = 1ull << 11,
    Stream      = 1ull << 12,
};

enum class Property : uint32_t {
    CommonName        = 0x00000001,
    ConsoleCursor     = 0x00000002,
    ConsoleClear      = 0x00000003,
    ConsoleColor      = 0x00000004,
    ConsoleTextFlags  = 0x00000005,
    ConsoleKernelLog  = 0x00000006,
    ConsoleUpdate     = 0x00000007,
    ConsoleScale      = 0x00000008,
    ConsoleFont       = 0x00000009,
    FramebufferInfo   = 0x00010001,
    FramebufferPresent= 0x00010002,
    GraphicalSessionInfo = 0x00011001,
    GraphicalSessionControl = 0x00011002,
    BlockGeometry     = 0x00020001,
    DiskInfo          = 0x00020002,
    PartitionInfo     = 0x00020003,
    SharedMemoryInfo  = 0x00030001,
    PipeInfo          = 0x00040001,
    VtyInfo           = 0x00050001,
    VtyCells          = 0x00050002,
    VtyInjectInput    = 0x00050003,
    VtyCursor         = 0x00050004,
    VtyClear          = 0x00050005,
    VtyColor          = 0x00050006,
    VtyTextFlags      = 0x00050007,
    NetDeviceInfo     = 0x00060001,
    NetIpv4Config     = 0x00060002,
    NetDeviceDebug    = 0x00060003,
    NetEndpointInfo   = 0x00070001,
    AudioFormat       = 0x00080001,
    AudioStatus       = 0x00080002,
    AudioControl      = 0x00080003,
    SensorInfo        = 0x00090001,
};

enum class SensorKind : uint16_t {
    Temperature = 1,
    Voltage = 2,
    Fan = 3,
};

enum class SensorUnit : uint16_t {
    MilliCelsius = 1,
    Millivolt = 2,
    Rpm = 3,
};

enum SensorSampleFlag : uint32_t {
    kSensorSampleValid = 1u << 0,
};

struct SensorInfo {
    char name[32];
    char adapter[32];
    SensorKind kind;
    SensorUnit unit;
    uint32_t index;
};

struct SensorSample {
    int64_t value;
    uint32_t flags;
    uint32_t reserved;
};

static_assert(sizeof(SensorInfo) == 72, "SensorInfo size mismatch");
static_assert(sizeof(SensorSample) == 16, "SensorSample size mismatch");

struct AudioFormatInfo {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t frame_bytes;
    uint32_t reserved;
};

enum AudioCommand : uint32_t {
    kAudioCommandPause = 1,
    kAudioCommandResume = 2,
    kAudioCommandFlush = 3,
    kAudioCommandSetVolume = 4,
};

enum AudioStatusFlag : uint32_t {
    kAudioStatusPaused = 1u << 0,
    kAudioStatusRunning = 1u << 1,
};

struct AudioControlInfo {
    uint32_t command;
    int32_t value;
};

struct AudioStatusInfo {
    uint64_t queued_bytes;
    uint32_t flags;
    uint32_t volume;
};

enum DiskFlag : uint32_t {
    kDiskFlagRemovable = 1u << 0,
};

enum KeyboardEventFlag : uint8_t {
    kKeyboardFlagPressed = 1u << 0,
    kKeyboardFlagExtended = 1u << 1,
};

enum KeyboardMod : uint8_t {
    kKeyboardModShift = 1u << 0,
    kKeyboardModCtrl = 1u << 1,
    kKeyboardModAlt = 1u << 2,
    kKeyboardModCaps = 1u << 3,
};

struct KeyboardEvent {
    uint8_t scancode;
    uint8_t flags;
    uint8_t mods;
    uint8_t reserved;
};


struct FramebufferInfo {
    uint64_t physical_base;
    uint64_t virtual_base;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t reserved;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
};

struct FramebufferRect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

// Versioned kernel contract held by the trusted userspace display/session
// service. The descriptor is the capability: its owner may activate only the
// display slot assigned here and receives input for that same slot.
constexpr uint16_t kGraphicalSessionAbiMajor = 1;
constexpr uint16_t kGraphicalSessionAbiMinor = 0;

enum GraphicalSessionFeature : uint32_t {
    kGraphicalSessionFeatureDisplayLease = 1u << 0,
    kGraphicalSessionFeatureInputSeat = 1u << 1,
    kGraphicalSessionFeatureDamagePresent = 1u << 2,
    kGraphicalSessionFeatureSharedSurfaces = 1u << 3,
};

enum GraphicalSessionFlag : uint32_t {
    kGraphicalSessionFlagActive = 1u << 0,
    kGraphicalSessionFlagFramebufferOpen = 1u << 1,
};

enum GraphicalSessionCommand : uint32_t {
    kGraphicalSessionCommandActivate = 1,
    kGraphicalSessionCommandDeactivate = 2,
};

struct GraphicalSessionInfo {
    uint16_t abi_major;
    uint16_t abi_minor;
    uint16_t display_slot;
    uint16_t reserved0;
    uint32_t session_id;
    uint32_t flags;
    uint32_t features;
    uint32_t reserved1;
};

struct GraphicalSessionControl {
    uint32_t command;
    uint32_t flags;
};

static_assert(sizeof(GraphicalSessionInfo) == 24,
              "GraphicalSessionInfo size mismatch");
static_assert(sizeof(GraphicalSessionControl) == 8,
              "GraphicalSessionControl size mismatch");

struct BlockGeometry {
    uint64_t sector_size;
    uint64_t sector_count;
};

struct DiskInfo {
    char name[32];
    uint32_t partition_count;
    uint32_t flags;
};

struct PartitionInfo {
    char name[32];
    uint64_t start_lba;
    uint64_t sector_count;
    uint32_t index;
    uint8_t type;
    uint8_t reserved[3];
};

struct SharedMemoryInfo {
    uint64_t base;
    uint64_t length;
};

struct PipeInfo {
    uint32_t id;
    uint32_t flags;
};

struct MouseEvent {
    int8_t dx;
    int8_t dy;
    uint8_t buttons;
    uint8_t reserved;
};

struct ColorPair {
    uint32_t fg;
    uint32_t bg;
};

constexpr uint32_t kConsoleMinScale = 1;
constexpr uint32_t kConsoleMaxScale = 8;
constexpr uint32_t kConsoleMaxFontDataSize = 64 * 1024;

enum ConsoleFontFlag : uint32_t {
    kConsoleFontMsbFirst = 1u << 0,
};

struct ConsoleFont {
    uint16_t width;
    uint16_t height;
    uint16_t glyph_count;
    uint16_t bytes_per_row;
    uint32_t data_size;
    uint32_t flags;
};

static_assert(sizeof(ConsoleFont) == 16, "ConsoleFont size mismatch");

// Packed glyph data immediately follows this header. Glyphs are stored in
// code-point order and row by row. By default the leftmost pixel is in the low
// bit; kConsoleFontMsbFirst selects the high bit instead.
constexpr size_t console_font_payload_size(const ConsoleFont& font) {
    return sizeof(ConsoleFont) + static_cast<size_t>(font.data_size);
}

enum class VtyOpen : uint64_t {
    Attach = 1ull << 0,
};

struct VtyInfo {
    uint32_t id;
    uint32_t cols;
    uint32_t rows;
    uint32_t cursor_x;
    uint32_t cursor_y;
    uint32_t flags;
    uint32_t cell_bytes;
};

struct VtyCell {
    uint32_t fg;
    uint32_t bg;
    uint8_t ch;
    uint8_t flags;
    uint8_t reserved[2];
};

enum TextCellFlag : uint8_t {
    kTextCellUnderline = 1u << 0,
};

struct CursorPosition {
    uint32_t x;
    uint32_t y;
};

struct CpuUsage {
    uint32_t cpu_index;
    uint32_t reserved;
    uint64_t user_ticks;
    uint64_t kernel_ticks;
    uint64_t idle_ticks;
    uint64_t irq_ticks;
};

enum TaskStatFlag : uint32_t {
    kTaskStatFlagKernel = 1u << 0,
    kTaskStatFlagExited = 1u << 1,
    kTaskStatFlagThread = 1u << 2,
};

enum WaitFlag : uint32_t {
    kWaitRead = 1u << 0,
    kWaitWrite = 1u << 1,
};

struct DescriptorWait {
    uint32_t handle;
    uint32_t events;
    uint32_t revents;
    uint32_t reserved;
};

struct TaskUsage {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t state;
    uint32_t flags;
    uint32_t preferred_cpu;
    uint32_t process_id;
    uint64_t user_ticks;
    uint64_t kernel_ticks;
    uint64_t virtual_bytes;
    uint64_t resident_bytes;
    uint64_t shared_bytes;
    uint64_t file_bytes;
    char image_path[64];
};

struct PciDeviceInfo {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint8_t reserved;
};

static_assert(sizeof(PciDeviceInfo) == 12, "PciDeviceInfo size mismatch");

enum NetDeviceInfoFlag : uint32_t {
    kNetDeviceFlagUp = 1u << 0,
    kNetDeviceFlagIpv4Configured = 1u << 1,
    kNetDeviceFlagRawEthernet = 1u << 2,
};

enum NetIpv4ConfigFlag : uint32_t {
    kNetIpv4FlagEnabled = 1u << 0,
    kNetIpv4FlagDhcp = 1u << 1,
};

struct NetDeviceInfo {
    uint32_t index;
    uint32_t flags;
    uint32_t rx_queued;
    uint16_t mtu;
    uint16_t reserved0;
    uint8_t mac[6];
    uint8_t reserved1[2];
};

struct NetIpv4Config {
    uint8_t address[4];
    uint8_t netmask[4];
    uint8_t gateway[4];
    uint8_t dns[4];
    uint32_t flags;
};

struct NetDeviceDebug {
    uint32_t status;
    uint32_t rctl;
    uint32_t tctl;
    uint32_t rdh;
    uint32_t rdt;
    uint32_t tdh;
    uint32_t tdt;
    uint32_t tx_submitted;
    uint32_t tx_completed;
    uint32_t rx_desc_seen;
    uint32_t rx_frames_passed;
    uint32_t rx_queued;
    uint32_t rx_frames_received;
    uint32_t rx_frames_dropped;
    uint32_t reserved;
};

enum NetEndpointOpenFlag : uint64_t {
    kNetEndpointOpenService = 1ull << 0,
};

struct NetEndpointInfo {
    uint32_t id;
    uint32_t flags;
    uint32_t role;
    uint32_t reserved;
};

}  // namespace descriptor_defs
