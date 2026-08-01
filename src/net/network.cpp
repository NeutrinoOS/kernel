#include "net/network.hpp"

#include "drivers/log/logging.hpp"
#include "fs/vfs.hpp"
#include "kernel/cmdline.hpp"
#include "kernel/config.hpp"
#include "kernel/descriptor.hpp"
#include "kernel/error.hpp"
#include "kernel/string_util.hpp"
#include "kernel/sync.hpp"
#include "lib/mem.hpp"

namespace net {

namespace {

constexpr size_t kMaxLinks = 4;
constexpr size_t kEthernetHeaderSize = 14;
constexpr size_t kEthernetMinimumFrameSize = 60;
constexpr uint32_t kDefaultIpv4Netmask = 0xFFFFFF00u;
constexpr size_t kIpv4MinimumHeaderSize = 20;
constexpr size_t kIcmpMinimumHeaderSize = 8;
constexpr size_t kMaxEthernetPayloadSize = 1500;
constexpr uint16_t kEtherTypeIpv4 = 0x0800;
constexpr uint16_t kEtherTypeArp = 0x0806;
constexpr uint8_t kIpv4ProtocolIcmp = 1;
constexpr uint8_t kIcmpTypeEchoReply = 0;
constexpr uint8_t kIcmpTypeEchoRequest = 8;

struct [[gnu::packed]] EthernetHeader {
    uint8_t destination[6];
    uint8_t source[6];
    uint16_t ether_type;
};

struct [[gnu::packed]] Ipv4Header {
    uint8_t version_ihl;
    uint8_t dscp_ecn;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment_offset;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t header_checksum;
    uint32_t source_address;
    uint32_t destination_address;
};

struct [[gnu::packed]] ArpPacket {
    uint16_t hardware_type;
    uint16_t protocol_type;
    uint8_t hardware_size;
    uint8_t protocol_size;
    uint16_t operation;
    uint8_t sender_hardware_address[6];
    uint8_t sender_protocol_address[4];
    uint8_t target_hardware_address[6];
    uint8_t target_protocol_address[4];
};

LinkDevice* g_links[kMaxLinks];
size_t g_link_count = 0;
bool g_default_ipv4_configured = false;
uint32_t g_default_ipv4_address = 0;

class DeviceGuard {
public:
    explicit DeviceGuard(LinkDevice& device)
        : device_(device), flags_(sync::disable_interrupts()) {
        while (__atomic_test_and_set(&device_.rx_lock, __ATOMIC_ACQUIRE)) {
            asm volatile("pause");
        }
    }
    ~DeviceGuard() {
        __atomic_clear(&device_.rx_lock, __ATOMIC_RELEASE);
        sync::restore_interrupts(flags_);
    }
    DeviceGuard(const DeviceGuard&) = delete;
    DeviceGuard& operator=(const DeviceGuard&) = delete;
private:
    LinkDevice& device_;
    uint64_t flags_;
};

uint16_t load_be16(const void* ptr) {
    const uint8_t* bytes = static_cast<const uint8_t*>(ptr);
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) |
                                 static_cast<uint16_t>(bytes[1]));
}

uint32_t load_be32(const void* ptr) {
    const uint8_t* bytes = static_cast<const uint8_t*>(ptr);
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

void store_be16(void* ptr, uint16_t value) {
    uint8_t* bytes = static_cast<uint8_t*>(ptr);
    bytes[0] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    bytes[1] = static_cast<uint8_t>(value & 0xFFu);
}

void store_be32(void* ptr, uint32_t value) {
    uint8_t* bytes = static_cast<uint8_t*>(ptr);
    bytes[0] = static_cast<uint8_t>((value >> 24) & 0xFFu);
    bytes[1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    bytes[2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    bytes[3] = static_cast<uint8_t>(value & 0xFFu);
}

uint16_t internet_checksum(const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t sum = 0;

    while (length > 1) {
        sum += static_cast<uint32_t>((static_cast<uint16_t>(bytes[0]) << 8) |
                                     static_cast<uint16_t>(bytes[1]));
        bytes += 2;
        length -= 2;
    }

    if (length != 0) {
        sum += static_cast<uint32_t>(static_cast<uint16_t>(bytes[0]) << 8);
    }

    while ((sum >> 16) != 0) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }

    return static_cast<uint16_t>(~sum);
}

char hex_digit(uint8_t value) {
    return (value < 10) ? static_cast<char>('0' + value)
                        : static_cast<char>('a' + (value - 10));
}

void format_mac_dir_name(const uint8_t mac[6], char* out, size_t out_size) {
    if (out == nullptr || out_size < 18) {
        return;
    }
    for (size_t i = 0; i < 6; ++i) {
        uint8_t byte = mac[i];
        out[i * 3] = hex_digit(static_cast<uint8_t>((byte >> 4) & 0x0Fu));
        out[i * 3 + 1] = hex_digit(static_cast<uint8_t>(byte & 0x0Fu));
        if (i != 5) {
            out[i * 3 + 2] = '-';
        }
    }
    out[17] = '\0';
}

void format_ipv4_string(uint32_t address, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return;
    }

    uint8_t octets[4] = {
        static_cast<uint8_t>((address >> 24) & 0xFFu),
        static_cast<uint8_t>((address >> 16) & 0xFFu),
        static_cast<uint8_t>((address >> 8) & 0xFFu),
        static_cast<uint8_t>(address & 0xFFu),
    };

    size_t pos = 0;
    for (size_t i = 0; i < 4 && pos + 1 < out_size; ++i) {
        uint8_t value = octets[i];
        char digits[3];
        size_t count = 0;
        if (value >= 100) {
            digits[count++] = static_cast<char>('0' + (value / 100));
            value %= 100;
            digits[count++] = static_cast<char>('0' + (value / 10));
            digits[count++] = static_cast<char>('0' + (value % 10));
        } else if (value >= 10) {
            digits[count++] = static_cast<char>('0' + (value / 10));
            digits[count++] = static_cast<char>('0' + (value % 10));
        } else {
            digits[count++] = static_cast<char>('0' + value);
        }

        for (size_t d = 0; d < count && pos + 1 < out_size; ++d) {
            out[pos++] = digits[d];
        }
        if (i != 3 && pos + 1 < out_size) {
            out[pos++] = '.';
        }
    }

    out[pos] = '\0';
}

bool parse_ipv4_literal(const char* text, size_t length, uint32_t& out) {
    if (text == nullptr || length == 0) {
        return false;
    }

    uint8_t octets[4] = {};
    size_t octet_index = 0;
    uint32_t current = 0;
    bool have_digit = false;

    for (size_t i = 0; i < length; ++i) {
        char ch = text[i];
        if (ch >= '0' && ch <= '9') {
            current = current * 10u + static_cast<uint32_t>(ch - '0');
            if (current > 255u) {
                return false;
            }
            have_digit = true;
            continue;
        }
        if (ch != '.' || !have_digit || octet_index >= 3) {
            return false;
        }
        octets[octet_index++] = static_cast<uint8_t>(current);
        current = 0;
        have_digit = false;
    }

    if (!have_digit || octet_index != 3) {
        return false;
    }
    octets[3] = static_cast<uint8_t>(current);

    out = (static_cast<uint32_t>(octets[0]) << 24) |
          (static_cast<uint32_t>(octets[1]) << 16) |
          (static_cast<uint32_t>(octets[2]) << 8) |
          static_cast<uint32_t>(octets[3]);
    return true;
}

bool find_cmdline_ipv4(uint32_t& out) {
    for (size_t i = 0; i < kernel_cmdline::token_count(); ++i) {
        const kernel_cmdline::Token* token = kernel_cmdline::token_at(i);
        if (token == nullptr || token->value == nullptr) {
            continue;
        }
        if ((string_util::equals(token->name, "IPV4") ||
             string_util::equals(token->name, "IP")) &&
            parse_ipv4_literal(token->value,
                               string_util::length(token->value),
                               out)) {
            return true;
        }
    }

    return false;
}

void assign_ipv4(LinkDevice& device,
                 uint32_t address,
                 uint32_t netmask,
                 uint32_t gateway,
                 uint32_t dns,
                 bool dhcp,
                 const char* source) {
    device.ipv4_address = address;
    device.ipv4_netmask = netmask;
    device.ipv4_gateway = gateway;
    device.ipv4_dns = dns;
    device.ipv4_dhcp = dhcp;
    device.ipv4_configured = address != 0;

    char address_text[16];
    format_ipv4_string(address, address_text, sizeof(address_text));
    log_message(LogLevel::Info, "net: link %s IPv4=%s (%s)",
                (device.name != nullptr) ? device.name : "(unnamed)",
                address_text,
                (source != nullptr) ? source : "unknown");
}

bool build_interface_config_path(const char* root_mount_path,
                                 const uint8_t mac[6],
                                 char* out,
                                 size_t out_size) {
    if (root_mount_path == nullptr || out == nullptr || out_size == 0) {
        return false;
    }

    char mac_dir[18];
    format_mac_dir_name(mac, mac_dir, sizeof(mac_dir));

    size_t pos = 0;
    auto append = [&](const char* text) -> bool {
        size_t len = string_util::length(text);
        if (pos + len >= out_size) {
            return false;
        }
        memcpy(out + pos, text, len);
        pos += len;
        out[pos] = '\0';
        return true;
    };

    out[0] = '\0';
    return append(root_mount_path) && append("/config/network/") &&
           append(mac_dir) && append("/interface.cfg");
}

void handle_arp(LinkDevice& device, const uint8_t* payload, size_t length) {
    if (!device.ipv4_configured || payload == nullptr || length < sizeof(ArpPacket)) {
        return;
    }

    const ArpPacket* packet = reinterpret_cast<const ArpPacket*>(payload);
    uint16_t hardware_type = load_be16(&packet->hardware_type);
    uint16_t protocol_type = load_be16(&packet->protocol_type);
    uint16_t operation = load_be16(&packet->operation);
    uint32_t target_protocol = load_be32(packet->target_protocol_address);
    if (hardware_type != 0x0001 ||
        protocol_type != kEtherTypeIpv4 ||
        packet->hardware_size != 6 ||
        packet->protocol_size != 4) {
        return;
    }

    if (operation != 0x0001 || target_protocol != device.ipv4_address) {
        return;
    }

    ArpPacket reply{};
    store_be16(&reply.hardware_type, 0x0001);
    store_be16(&reply.protocol_type, kEtherTypeIpv4);
    reply.hardware_size = 6;
    reply.protocol_size = 4;
    store_be16(&reply.operation, 0x0002);
    memcpy(reply.sender_hardware_address, device.mac, 6);
    store_be32(reply.sender_protocol_address, device.ipv4_address);
    memcpy(reply.target_hardware_address, packet->sender_hardware_address, 6);
    memcpy(reply.target_protocol_address,
           packet->sender_protocol_address,
           sizeof(reply.target_protocol_address));

    if (!send_ethernet_frame(device,
                             packet->sender_hardware_address,
                             kEtherTypeArp,
                             &reply,
                             sizeof(reply))) {
        char address_text[16];
        format_ipv4_string(device.ipv4_address, address_text, sizeof(address_text));
        log_message(LogLevel::Warn, "arp: failed to transmit reply on %s for %s",
                    (device.name != nullptr) ? device.name : "(unnamed)",
                    address_text);
    }
}

void handle_icmp_echo(LinkDevice& device,
                      const uint8_t source_mac[6],
                      const uint8_t* packet,
                      size_t length) {
    if (!device.ipv4_configured || source_mac == nullptr || packet == nullptr ||
        length < kIpv4MinimumHeaderSize || length > kMaxEthernetPayloadSize) {
        return;
    }

    const Ipv4Header* header = reinterpret_cast<const Ipv4Header*>(packet);
    uint8_t version = static_cast<uint8_t>(header->version_ihl >> 4);
    uint8_t header_words = static_cast<uint8_t>(header->version_ihl & 0x0Fu);
    size_t header_length = static_cast<size_t>(header_words) * 4;
    if (version != 4 || header_length < kIpv4MinimumHeaderSize ||
        header_length > length) {
        return;
    }

    uint16_t total_length = load_be16(&header->total_length);
    if (total_length < header_length || total_length > length) {
        return;
    }

    uint16_t fragment_bits = load_be16(&header->flags_fragment_offset);
    if ((fragment_bits & 0x1FFFu) != 0 || (fragment_bits & 0x2000u) != 0) {
        return;
    }

    if (load_be32(&header->destination_address) != device.ipv4_address ||
        header->protocol != kIpv4ProtocolIcmp) {
        return;
    }

    if (internet_checksum(packet, header_length) != 0) {
        return;
    }

    size_t icmp_length = static_cast<size_t>(total_length) - header_length;
    if (icmp_length < kIcmpMinimumHeaderSize) {
        return;
    }

    const uint8_t* icmp = packet + header_length;
    if (icmp[0] != kIcmpTypeEchoRequest || icmp[1] != 0) {
        return;
    }

    if (internet_checksum(icmp, icmp_length) != 0) {
        return;
    }

    uint8_t reply[kMaxEthernetPayloadSize];
    memcpy(reply, packet, total_length);

    Ipv4Header* reply_header = reinterpret_cast<Ipv4Header*>(reply);
    uint32_t source_address = load_be32(&header->source_address);
    store_be32(&reply_header->source_address, device.ipv4_address);
    store_be32(&reply_header->destination_address, source_address);
    reply_header->ttl = 64;
    reply_header->header_checksum = 0;

    uint8_t* reply_icmp = reply + header_length;
    reply_icmp[0] = kIcmpTypeEchoReply;
    reply_icmp[1] = 0;
    store_be16(reply_icmp + 2, 0);
    store_be16(reply_icmp + 2, internet_checksum(reply_icmp, icmp_length));
    store_be16(&reply_header->header_checksum, 0);
    store_be16(&reply_header->header_checksum,
               internet_checksum(reply_header, header_length));

    (void)send_ethernet_frame(device, source_mac, kEtherTypeIpv4, reply, total_length);
}

void handle_ipv4(LinkDevice& device,
                 const uint8_t source_mac[6],
                 const uint8_t* payload,
                 size_t length) {
    if (!device.ipv4_configured || source_mac == nullptr || payload == nullptr) {
        return;
    }
    handle_icmp_echo(device, source_mac, payload, length);
}

}  // namespace

void init() {
    g_link_count = 0;
    for (size_t i = 0; i < kMaxLinks; ++i) {
        g_links[i] = nullptr;
    }

    g_default_ipv4_configured = false;
    g_default_ipv4_address = 0;

    uint32_t parsed_ipv4 = 0;
    if (find_cmdline_ipv4(parsed_ipv4)) {
        g_default_ipv4_address = parsed_ipv4;
        g_default_ipv4_configured = true;
        char address_text[16];
        format_ipv4_string(parsed_ipv4, address_text, sizeof(address_text));
        log_message(LogLevel::Info,
                    "net: cmdline fallback IPv4 configured as %s",
                    address_text);
    }
}

void load_config(const char* root_mount_path) {
    if (root_mount_path == nullptr || *root_mount_path == '\0') {
        return;
    }

    bool any_loaded = false;
    for (size_t i = 0; i < g_link_count; ++i) {
        LinkDevice* device = g_links[i];
        if (device == nullptr || !device->up) {
            continue;
        }

        char path[160];
        if (!build_interface_config_path(root_mount_path,
                                         device->mac,
                                         path,
                                         sizeof(path))) {
            log_message(LogLevel::Warn,
                        "net: config path too long for link %s",
                        (device->name != nullptr) ? device->name : "(unnamed)");
            continue;
        }

        uint8_t buffer[512];
        size_t read_size = 0;
        if (!vfs::read_file(path, buffer, sizeof(buffer), read_size)) {
            log_message(LogLevel::Warn, "net: no interface config at %s", path);
            continue;
        }

        config::Table table{};
        if (!config::parse(reinterpret_cast<const char*>(buffer), read_size, table)) {
            log_message(LogLevel::Warn, "net: parse errors in %s", path);
        }

        const char* value = config::get(table, "IPV4.ADDRESS");
        if (value == nullptr) {
            value = config::get(table, "IPV4");
        }
        if (value == nullptr) {
            log_message(LogLevel::Warn, "net: %s missing IPV4.ADDRESS", path);
            continue;
        }

        uint32_t ipv4 = 0;
        if (!parse_ipv4_literal(value, string_util::length(value), ipv4)) {
            log_message(LogLevel::Warn, "net: invalid IPv4 '%s' in %s",
                        value, path);
            continue;
        }

        uint32_t netmask = kDefaultIpv4Netmask;
        uint32_t gateway = 0;
        uint32_t dns = 0;
        const char* mask_value = config::get(table, "IPV4.NETMASK");
        if (mask_value != nullptr) {
            uint32_t parsed_mask = 0;
            if (parse_ipv4_literal(mask_value,
                                   string_util::length(mask_value),
                                   parsed_mask)) {
                netmask = parsed_mask;
            }
        }

        const char* gateway_value = config::get(table, "IPV4.GATEWAY");
        if (gateway_value != nullptr) {
            (void)parse_ipv4_literal(gateway_value,
                                     string_util::length(gateway_value),
                                     gateway);
        }

        const char* dns_value = config::get(table, "IPV4.DNS");
        if (dns_value != nullptr) {
            (void)parse_ipv4_literal(dns_value,
                                     string_util::length(dns_value),
                                     dns);
        }

        assign_ipv4(*device, ipv4, netmask, gateway, dns, false, path);
        any_loaded = true;
    }

    if (!any_loaded) {
        log_message(LogLevel::Warn,
                    "net: no per-interface network config loaded from %s/config/network",
                    root_mount_path);
    }
}

bool register_link(LinkDevice& device,
                   const char* name,
                   void* context,
                   TransmitFn transmit,
                   const uint8_t mac[6]) {
    if (name == nullptr || transmit == nullptr || mac == nullptr ||
        g_link_count >= kMaxLinks) {
        return false;
    }

    device.name = name;
    device.context = context;
    device.transmit = transmit;
    device.index = static_cast<uint32_t>(g_link_count);
    memcpy(device.mac, mac, sizeof(device.mac));
    device.up = true;
    device.ipv4_configured = false;
    device.ipv4_dhcp = false;
    device.ipv4_address = 0;
    device.ipv4_netmask = kDefaultIpv4Netmask;
    device.ipv4_gateway = 0;
    device.ipv4_dns = 0;
    device.rx_head = 0;
    device.rx_tail = 0;
    device.rx_frames_received = 0;
    device.rx_frames_dropped = 0;
    device.rx_lock = 0;
    memset(device.rx_lengths, 0, sizeof(device.rx_lengths));

    if (g_default_ipv4_configured) {
        assign_ipv4(device,
                    g_default_ipv4_address,
                    kDefaultIpv4Netmask,
                    0,
                    0,
                    false,
                    "cmdline");
    }

    g_links[g_link_count++] = &device;
    log_message(LogLevel::Info,
                "net: link %s registered mac=%02x:%02x:%02x:%02x:%02x:%02x",
                name,
                static_cast<unsigned int>(device.mac[0]),
                static_cast<unsigned int>(device.mac[1]),
                static_cast<unsigned int>(device.mac[2]),
                static_cast<unsigned int>(device.mac[3]),
                static_cast<unsigned int>(device.mac[4]),
                static_cast<unsigned int>(device.mac[5]));
    return true;
}

size_t device_count() {
    KERNEL_ASSERT_MSG(g_link_count <= kMaxLinks,
                      "network link count is out of bounds");
    return g_link_count;
}

LinkDevice* device_at(size_t index) {
    KERNEL_ASSERT_MSG(g_link_count <= kMaxLinks,
                      "network link count is out of bounds");
    return (index < g_link_count) ? g_links[index] : nullptr;
}

size_t queued_frame_count(LinkDevice& device) {
    DeviceGuard guard(device);
    KERNEL_ASSERT_MSG(device.rx_head < kMaxQueuedFrames,
                      "network receive queue head is out of bounds");
    KERNEL_ASSERT_MSG(device.rx_tail < kMaxQueuedFrames,
                      "network receive queue tail is out of bounds");
    size_t count = (device.rx_head >= device.rx_tail)
                       ? static_cast<size_t>(device.rx_head - device.rx_tail)
                       : static_cast<size_t>(kMaxQueuedFrames -
                                             (device.rx_tail - device.rx_head));
    return count;
}

int read_frame(LinkDevice& device,
               void* buffer,
               size_t buffer_size,
               size_t& out_size) {
    out_size = 0;
    if (buffer == nullptr || buffer_size == 0) {
        return -1;
    }

    DeviceGuard guard(device);
    KERNEL_ASSERT_MSG(device.rx_head < kMaxQueuedFrames,
                      "network receive queue head is out of bounds");
    KERNEL_ASSERT_MSG(device.rx_tail < kMaxQueuedFrames,
                      "network receive queue tail is out of bounds");
    if (device.rx_head == device.rx_tail) {
        return 0;
    }

    uint16_t slot = device.rx_tail;
    size_t frame_size = device.rx_lengths[slot];
    if (frame_size > buffer_size) {
        return -1;
    }

    memcpy(buffer, device.rx_frames[slot], frame_size);
    device.rx_lengths[slot] = 0;
    device.rx_tail = static_cast<uint16_t>((slot + 1) % kMaxQueuedFrames);

    out_size = frame_size;
    return 1;
}

bool write_frame(LinkDevice& device, const void* frame, size_t length) {
    if (!device.up || frame == nullptr || length == 0) {
        return false;
    }
    return device.transmit(device.context, frame, length);
}

void get_ipv4_config(const LinkDevice& device,
                     bool& enabled,
                     bool& dhcp,
                     uint32_t& address,
                     uint32_t& netmask,
                     uint32_t& gateway,
                     uint32_t& dns) {
    enabled = device.ipv4_configured;
    dhcp = device.ipv4_dhcp;
    address = device.ipv4_address;
    netmask = device.ipv4_netmask;
    gateway = device.ipv4_gateway;
    dns = device.ipv4_dns;
}

void set_ipv4_config(LinkDevice& device,
                     bool enabled,
                     bool dhcp,
                     uint32_t address,
                     uint32_t netmask,
                     uint32_t gateway,
                     uint32_t dns) {
    device.ipv4_configured = enabled && address != 0;
    device.ipv4_dhcp = dhcp;
    device.ipv4_address = device.ipv4_configured ? address : 0;
    device.ipv4_netmask = (netmask != 0) ? netmask : kDefaultIpv4Netmask;
    device.ipv4_gateway = gateway;
    device.ipv4_dns = dns;
}

void receive_frame(LinkDevice* device, const void* frame, size_t length) {
    if (device == nullptr || frame == nullptr || length < sizeof(EthernetHeader)) {
        return;
    }

    if (length <= kMaxQueuedFrameSize) {
        bool queued = false;
        {
            DeviceGuard guard(*device);
            KERNEL_ASSERT_MSG(device->rx_head < kMaxQueuedFrames,
                              "network receive queue head is out of bounds");
            KERNEL_ASSERT_MSG(device->rx_tail < kMaxQueuedFrames,
                              "network receive queue tail is out of bounds");
            ++device->rx_frames_received;
            uint16_t next_head =
                static_cast<uint16_t>((device->rx_head + 1) % kMaxQueuedFrames);
            if (next_head != device->rx_tail) {
                memcpy(device->rx_frames[device->rx_head], frame, length);
                device->rx_lengths[device->rx_head] = static_cast<uint16_t>(length);
                device->rx_head = next_head;
                queued = true;
            } else {
                ++device->rx_frames_dropped;
            }
        }
        if (queued) {
            descriptor::wake_waiters();
        }
    } else {
        DeviceGuard guard(*device);
        ++device->rx_frames_dropped;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(frame);
    const EthernetHeader* header =
        reinterpret_cast<const EthernetHeader*>(bytes);
    uint16_t ether_type = load_be16(&header->ether_type);
    const uint8_t* payload = bytes + sizeof(EthernetHeader);
    size_t payload_length = length - sizeof(EthernetHeader);

    if (ether_type == kEtherTypeArp) {
        handle_arp(*device, payload, payload_length);
    } else if (ether_type == kEtherTypeIpv4) {
        handle_ipv4(*device, header->source, payload, payload_length);
    }
}

bool send_ethernet_frame(LinkDevice& device,
                         const uint8_t destination[6],
                         uint16_t ether_type,
                         const void* payload,
                         size_t payload_length) {
    if (!device.up || destination == nullptr || payload == nullptr ||
        payload_length > kMaxEthernetPayloadSize) {
        return false;
    }

    uint8_t frame[sizeof(EthernetHeader) + kMaxEthernetPayloadSize];
    EthernetHeader* header = reinterpret_cast<EthernetHeader*>(frame);
    memcpy(header->destination, destination, 6);
    memcpy(header->source, device.mac, 6);
    store_be16(&header->ether_type, ether_type);
    memcpy(frame + sizeof(EthernetHeader), payload, payload_length);

    size_t frame_length = sizeof(EthernetHeader) + payload_length;
    if (frame_length < kEthernetMinimumFrameSize) {
        memset(frame + frame_length, 0, kEthernetMinimumFrameSize - frame_length);
        frame_length = kEthernetMinimumFrameSize;
    }

    return device.transmit(device.context, frame, frame_length);
}

}  // namespace net
