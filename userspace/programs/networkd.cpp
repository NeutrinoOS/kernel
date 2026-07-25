#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../crt/syscall.hpp"
#include "../net/ipv4.hpp"
#include "../net/network_protocol.hpp"
#include "../net/tcp.hpp"
#include "../net/udp.hpp"

namespace {

constexpr size_t kMaxBindings = 16;
constexpr size_t kMaxArpEntries = 16;
constexpr size_t kMaxPendingPings = 8;
constexpr uint32_t kDeviceWriteRetryLimit = 100000;
constexpr uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct ArpEntry {
    bool in_use;
    uint8_t ip[4];
    uint8_t mac[6];
};

struct PendingPing {
    bool in_use;
    uint16_t identifier;
    uint16_t sequence;
    uint32_t pipe_handle;
};

struct Binding {
    bool in_use;
    uint8_t protocol;
    uint16_t port;
    uint32_t pipe_id;
    uint32_t pipe_handle;
};

struct ServerContext {
    usernet::Device device;
    uint32_t server_pipe;
    networkd_protocol::Registry* registry;
    Binding bindings[kMaxBindings];
    ArpEntry arp_entries[kMaxArpEntries];
    PendingPing pending_pings[kMaxPendingPings];
};

constexpr uint8_t kBindingProtocolUdp = 17;
constexpr uint8_t kBindingProtocolTcp = 6;

networkd_protocol::Message* g_control_message_buffer = nullptr;
networkd_protocol::Message* g_tx_message_buffer = nullptr;
uint8_t* g_frame_buffer = nullptr;

bool poll_network(ServerContext& ctx);

void print(const char* text) {
    static int32_t console = -1;
    if (console < 0) {
        console = static_cast<int32_t>(
            descriptor_open(static_cast<uint32_t>(descriptor_defs::Type::Console), 0));
        if (console < 0) {
            return;
        }
    }
    if (text == nullptr) {
        return;
    }
    size_t len = 0;
    while (text[len] != '\0') {
        ++len;
    }
    if (len != 0) {
        descriptor_write(static_cast<uint32_t>(console), text, len);
    }
}

void print_line(const char* text) {
    print(text);
    print("\n");
}

void print_u32(uint32_t value) {
    char buffer[16];
    size_t length = 0;
    if (value == 0) {
        buffer[length++] = '0';
    } else {
        while (value != 0 && length < sizeof(buffer)) {
            buffer[length++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
    }
    for (size_t i = 0; i < length / 2; ++i) {
        char tmp = buffer[i];
        buffer[i] = buffer[length - 1 - i];
        buffer[length - 1 - i] = tmp;
    }
    buffer[length] = '\0';
    print(buffer);
}

descriptor_defs::SharedMemoryInfo* allocate_shm_info_buffer() {
    auto* info = static_cast<descriptor_defs::SharedMemoryInfo*>(
        map_anonymous(sizeof(descriptor_defs::SharedMemoryInfo), MAP_WRITE));
    if (info != nullptr) {
        info->base = 0;
        info->length = 0;
    }
    return info;
}

descriptor_defs::PipeInfo* allocate_pipe_info_buffer() {
    auto* info = static_cast<descriptor_defs::PipeInfo*>(
        map_anonymous(sizeof(descriptor_defs::PipeInfo), MAP_WRITE));
    if (info != nullptr) {
        info->id = 0;
        info->flags = 0;
    }
    return info;
}

networkd_protocol::Message* reset_message_buffer(
    networkd_protocol::Message*& slot) {
    if (slot == nullptr) {
        slot = static_cast<networkd_protocol::Message*>(
            map_anonymous(sizeof(networkd_protocol::Message), MAP_WRITE));
    }
    if (slot != nullptr) {
        for (size_t i = 0; i < sizeof(*slot); ++i) {
            reinterpret_cast<uint8_t*>(slot)[i] = 0;
        }
    }
    return slot;
}

networkd_protocol::Message* allocate_control_message_buffer() {
    return reset_message_buffer(g_control_message_buffer);
}

networkd_protocol::Message* allocate_tx_message_buffer() {
    return reset_message_buffer(g_tx_message_buffer);
}

uint8_t* allocate_frame_buffer() {
    if (g_frame_buffer == nullptr) {
        g_frame_buffer = static_cast<uint8_t*>(
            map_anonymous(usernet::kMaxFrameSize, MAP_WRITE));
    }
    if (g_frame_buffer != nullptr) {
        for (size_t i = 0; i < usernet::kMaxFrameSize; ++i) {
            g_frame_buffer[i] = 0;
        }
    }
    return g_frame_buffer;
}

Binding* find_binding(ServerContext& ctx, uint8_t protocol, uint16_t port) {
    for (size_t i = 0; i < kMaxBindings; ++i) {
        if (ctx.bindings[i].in_use &&
            ctx.bindings[i].protocol == protocol &&
            ctx.bindings[i].port == port) {
            return &ctx.bindings[i];
        }
    }
    return nullptr;
}

Binding* allocate_binding(ServerContext& ctx) {
    for (size_t i = 0; i < kMaxBindings; ++i) {
        if (!ctx.bindings[i].in_use) {
            return &ctx.bindings[i];
        }
    }
    return nullptr;
}

void release_binding(Binding& binding) {
    if (binding.pipe_handle != kInvalidDescriptor) {
        descriptor_close(binding.pipe_handle);
    }
    binding.in_use = false;
    binding.protocol = 0;
    binding.port = 0;
    binding.pipe_id = 0;
    binding.pipe_handle = kInvalidDescriptor;
}

ArpEntry* find_arp_entry(ServerContext& ctx, const uint8_t ip[4]) {
    for (size_t i = 0; i < kMaxArpEntries; ++i) {
        if (!ctx.arp_entries[i].in_use) {
            continue;
        }
        bool match = true;
        for (size_t j = 0; j < 4; ++j) {
            if (ctx.arp_entries[i].ip[j] != ip[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            return &ctx.arp_entries[i];
        }
    }
    return nullptr;
}

ArpEntry* allocate_arp_entry(ServerContext& ctx) {
    for (size_t i = 0; i < kMaxArpEntries; ++i) {
        if (!ctx.arp_entries[i].in_use) {
            return &ctx.arp_entries[i];
        }
    }
    return &ctx.arp_entries[0];
}

void record_arp(ServerContext& ctx, const uint8_t ip[4], const uint8_t mac[6]) {
    ArpEntry* entry = find_arp_entry(ctx, ip);
    if (entry == nullptr) {
        entry = allocate_arp_entry(ctx);
    }
    entry->in_use = true;
    for (size_t i = 0; i < 4; ++i) {
        entry->ip[i] = ip[i];
    }
    for (size_t i = 0; i < 6; ++i) {
        entry->mac[i] = mac[i];
    }
}

PendingPing* allocate_pending_ping(ServerContext& ctx) {
    for (size_t i = 0; i < kMaxPendingPings; ++i) {
        if (!ctx.pending_pings[i].in_use) {
            return &ctx.pending_pings[i];
        }
    }
    return nullptr;
}

PendingPing* find_pending_ping(ServerContext& ctx,
                               uint16_t identifier,
                               uint16_t sequence) {
    for (size_t i = 0; i < kMaxPendingPings; ++i) {
        if (ctx.pending_pings[i].in_use &&
            ctx.pending_pings[i].identifier == identifier &&
            ctx.pending_pings[i].sequence == sequence) {
            return &ctx.pending_pings[i];
        }
    }
    return nullptr;
}

bool load_ipv4_config(ServerContext& ctx, descriptor_defs::NetIpv4Config& out) {
    return net_device_get_ipv4_config(ctx.device.handle, &out) == 0 &&
           (out.flags & descriptor_defs::kNetIpv4FlagEnabled) != 0;
}

bool write_device_frame(ServerContext& ctx, const uint8_t* frame, size_t frame_length) {
    if (frame == nullptr || frame_length == 0) {
        return false;
    }
    for (uint32_t attempts = 0; attempts < kDeviceWriteRetryLimit; ++attempts) {
        long written = descriptor_write(ctx.device.handle, frame, frame_length);
        if (written == static_cast<long>(frame_length)) {
            if (ctx.registry != nullptr) {
                ++ctx.registry->net_tx_frames;
            }
            return true;
        }
        if (written == kDescriptorWouldBlock) {
            yield();
            continue;
        }
        if (ctx.registry != nullptr) {
            ++ctx.registry->net_tx_failures;
        }
        return false;
    }
    if (ctx.registry != nullptr) {
        ++ctx.registry->net_tx_failures;
    }
    return false;
}

bool resolve_next_hop(ServerContext& ctx,
                      const uint8_t destination_ip[4],
                      uint8_t next_hop_ip[4]) {
    descriptor_defs::NetIpv4Config cfg{};
    if (!load_ipv4_config(ctx, cfg)) {
        return false;
    }
    const uint8_t* target = destination_ip;
    if (!usernet::ipv4_same_subnet(cfg.address, destination_ip, cfg.netmask) &&
        !usernet::ipv4_is_zero(cfg.gateway)) {
        target = cfg.gateway;
    }
    for (size_t i = 0; i < 4; ++i) {
        next_hop_ip[i] = target[i];
    }
    return true;
}

bool send_arp_request(ServerContext& ctx, const uint8_t target_ip[4]) {
    descriptor_defs::NetIpv4Config cfg{};
    if (!load_ipv4_config(ctx, cfg)) {
        return false;
    }
    auto* frame = allocate_frame_buffer();
    if (frame == nullptr) {
        return false;
    }
    size_t frame_length = 0;
    if (!usernet::build_arp_request_frame(frame,
                                          usernet::kMaxFrameSize,
                                          frame_length,
                                          ctx.device.info.mac,
                                          cfg.address,
                                          target_ip)) {
        return false;
    }
    if (ctx.registry != nullptr) {
        ++ctx.registry->arp_requests;
    }
    return write_device_frame(ctx, frame, frame_length);
}

bool lookup_or_resolve_mac(ServerContext& ctx,
                           const uint8_t destination_ip[4],
                           uint8_t out_mac[6]);

int g_registry_fail_reason = 0;

bool populate_registry(ServerContext& ctx, uint32_t server_pipe_id) {
    g_registry_fail_reason = 0;
    long handle =
        shared_memory_open(networkd_protocol::kRegistryName,
                           sizeof(networkd_protocol::Registry));
    if (handle < 0) {
        g_registry_fail_reason = 1;
        return false;
    }
    auto* info = allocate_shm_info_buffer();
    if (info == nullptr) {
        g_registry_fail_reason = 5;
        descriptor_close(static_cast<uint32_t>(handle));
        return false;
    }
    long info_result =
        shared_memory_get_info(static_cast<uint32_t>(handle), info);
    if (info_result != 0) {
        g_registry_fail_reason = 2;
        unmap(info, sizeof(*info));
        descriptor_close(static_cast<uint32_t>(handle));
        return false;
    }
    uint64_t info_base = info->base;
    uint64_t info_length = info->length;
    unmap(info, sizeof(*info));
    if (info_base == 0) {
        g_registry_fail_reason = 3;
        descriptor_close(static_cast<uint32_t>(handle));
        return false;
    }
    if (info_length < sizeof(networkd_protocol::Registry)) {
        g_registry_fail_reason = 4;
        descriptor_close(static_cast<uint32_t>(handle));
        return false;
    }

    auto* registry = reinterpret_cast<networkd_protocol::Registry*>(info_base);
    memset(registry, 0, sizeof(*registry));
    registry->magic = networkd_protocol::kRegistryMagic;
    registry->version = networkd_protocol::kRegistryVersion;
    registry->server_pipe_id = server_pipe_id;
    registry->networkd_state = networkd_protocol::kStateStarting;
    registry->dhcp_state = networkd_protocol::kStateIdle;
    registry->dhcp_last_error = networkd_protocol::kErrorNone;
    ctx.registry = registry;
    return true;
}

void send_bind_response(uint32_t handle,
                        uint16_t type,
                        uint16_t port,
                        int32_t status) {
    auto* message = allocate_tx_message_buffer();
    if (message == nullptr) {
        return;
    }
    networkd_protocol::init_message(*message, type);
    if (type == networkd_protocol::kBindTcpResponse) {
        message->bind_tcp_response.status = status;
        message->bind_tcp_response.port = port;
    } else {
        message->bind_response.status = status;
        message->bind_response.port = port;
    }
    (void)networkd_protocol::write_message(handle, *message);
}

void send_udp_packet(Binding& binding, const usernet::UdpPacketView& packet) {
    if (binding.pipe_handle == kInvalidDescriptor ||
        packet.payload_length > networkd_protocol::kMaxUdpPayload) {
        return;
    }

    auto* message = allocate_tx_message_buffer();
    if (message == nullptr) {
        return;
    }
    networkd_protocol::init_message(*message,
                                    networkd_protocol::kUdpPacketEvent);
    message->udp_event.source_port = packet.source_port;
    message->udp_event.destination_port = packet.destination_port;
    message->udp_event.payload_length =
        static_cast<uint16_t>(packet.payload_length);
    for (size_t i = 0; i < 4; ++i) {
        message->udp_event.source_ip[i] = packet.source_ip[i];
        message->udp_event.destination_ip[i] = packet.destination_ip[i];
    }
    if (packet.payload_length != 0) {
        memcpy(message->udp_event.payload, packet.payload, packet.payload_length);
    }
    (void)networkd_protocol::write_message(binding.pipe_handle, *message);
}

void send_tcp_segment(Binding& binding, const usernet::TcpSegmentView& segment) {
    if (binding.pipe_handle == kInvalidDescriptor ||
        segment.options_length > networkd_protocol::kMaxTcpOptionBytes ||
        segment.payload_length > networkd_protocol::kMaxTcpPayload) {
        return;
    }

    auto* message = allocate_tx_message_buffer();
    if (message == nullptr) {
        return;
    }
    networkd_protocol::init_message(*message, networkd_protocol::kTcpSegmentEvent);
    message->tcp_event.source_port = segment.source_port;
    message->tcp_event.destination_port = segment.destination_port;
    message->tcp_event.flags = segment.flags;
    message->tcp_event.options_length = static_cast<uint16_t>(segment.options_length);
    message->tcp_event.payload_length = static_cast<uint16_t>(segment.payload_length);
    message->tcp_event.window_size = segment.window_size;
    message->tcp_event.sequence_number = segment.sequence_number;
    message->tcp_event.acknowledgment_number = segment.acknowledgment_number;
    for (size_t i = 0; i < 4; ++i) {
        message->tcp_event.source_ip[i] = segment.source_ip[i];
        message->tcp_event.destination_ip[i] = segment.destination_ip[i];
    }
    if (segment.options_length != 0) {
        memcpy(message->tcp_event.options, segment.options, segment.options_length);
    }
    if (segment.payload_length != 0) {
        memcpy(message->tcp_event.payload, segment.payload, segment.payload_length);
    }
    (void)networkd_protocol::write_message(binding.pipe_handle, *message);
}

void send_icmp_reply(PendingPing& ping, const usernet::IcmpEchoReplyView& reply) {
    if (ping.pipe_handle == kInvalidDescriptor ||
        reply.payload_length > networkd_protocol::kMaxUdpPayload) {
        return;
    }
    auto* message = allocate_tx_message_buffer();
    if (message == nullptr) {
        return;
    }
    networkd_protocol::init_message(*message, networkd_protocol::kIcmpEchoReplyEvent);
    message->icmp_event.identifier = reply.identifier;
    message->icmp_event.sequence = reply.sequence;
    message->icmp_event.payload_length = static_cast<uint16_t>(reply.payload_length);
    message->icmp_event.ttl = reply.ttl;
    for (size_t i = 0; i < 4; ++i) {
        message->icmp_event.source_ip[i] = reply.source_ip[i];
        message->icmp_event.destination_ip[i] = reply.destination_ip[i];
    }
    if (reply.payload_length != 0) {
        memcpy(message->icmp_event.payload, reply.payload, reply.payload_length);
    }
    (void)networkd_protocol::write_message(ping.pipe_handle, *message);
}

void handle_bind_request(ServerContext& ctx,
                         uint8_t protocol,
                         uint32_t reply_pipe_id,
                         uint16_t port,
                         uint16_t response_type) {
    print("networkd: bind request proto=");
    print_u32(protocol);
    print(" reply_pipe=");
    print_u32(reply_pipe_id);
    print(" port=");
    print_u32(port);
    print("\n");
    if (ctx.registry != nullptr) {
        ctx.registry->dhcp_state = networkd_protocol::kStateBindSeen;
    }
    if (reply_pipe_id == 0 || port == 0) {
        print_line("networkd: bind request rejected (zero reply pipe or port)");
        return;
    }

    Binding* existing = find_binding(ctx, protocol, port);
    if (existing != nullptr) {
        uint64_t flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                         static_cast<uint64_t>(descriptor_defs::Flag::Async);
        long handle = pipe_open_existing(flags, reply_pipe_id);
        if (handle >= 0) {
            send_bind_response(static_cast<uint32_t>(handle),
                               response_type,
                               port,
                               networkd_protocol::kStatusInUse);
            if (ctx.registry != nullptr) {
                ctx.registry->dhcp_state = networkd_protocol::kStateBindReplySent;
            }
            descriptor_close(static_cast<uint32_t>(handle));
            print_line("networkd: bind reply sent (in use)");
        } else {
            print_line("networkd: failed to open bind reply pipe (in use)");
        }
        return;
    }

    Binding* binding = allocate_binding(ctx);
    if (binding == nullptr) {
        print_line("networkd: no binding slots available");
        return;
    }

    uint64_t flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                     static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long handle = pipe_open_existing(flags, reply_pipe_id);
    if (handle < 0) {
        print_line("networkd: failed to open bind reply pipe");
        return;
    }

    binding->in_use = true;
    binding->protocol = protocol;
    binding->port = port;
    binding->pipe_id = reply_pipe_id;
    binding->pipe_handle = static_cast<uint32_t>(handle);
    send_bind_response(binding->pipe_handle,
                       response_type,
                       binding->port,
                       networkd_protocol::kStatusOk);
    if (ctx.registry != nullptr) {
        ctx.registry->dhcp_state = networkd_protocol::kStateBindReplySent;
    }
    print_line("networkd: bind reply sent");
}

void handle_udp_bind_request(ServerContext& ctx,
                             const networkd_protocol::BindUdpRequest& request) {
    handle_bind_request(ctx,
                        kBindingProtocolUdp,
                        request.reply_pipe_id,
                        request.port,
                        networkd_protocol::kBindUdpResponse);
}

void handle_udp_unbind_request(ServerContext& ctx,
                               const networkd_protocol::UnbindUdpRequest& request) {
    if (request.port == 0) {
        return;
    }
    Binding* binding = find_binding(ctx, kBindingProtocolUdp, request.port);
    if (binding != nullptr) {
        release_binding(*binding);
    }
}

void handle_tcp_bind_request(ServerContext& ctx,
                             const networkd_protocol::BindTcpRequest& request) {
    handle_bind_request(ctx,
                        kBindingProtocolTcp,
                        request.reply_pipe_id,
                        request.port,
                        networkd_protocol::kBindTcpResponse);
}

void handle_send_request(ServerContext& ctx,
                         const networkd_protocol::SendUdpRequest& request) {
    if (request.source_port == 0 ||
        request.destination_port == 0 ||
        request.payload_length > networkd_protocol::kMaxUdpPayload ||
        find_binding(ctx, kBindingProtocolUdp, request.source_port) == nullptr) {
        return;
    }

    uint8_t destination_mac_storage[6];
    const uint8_t* destination_mac = nullptr;
    if ((request.flags & networkd_protocol::kSendFlagBroadcast) != 0) {
        destination_mac = kBroadcastMac;
    } else if (lookup_or_resolve_mac(ctx, request.destination_ip, destination_mac_storage)) {
        destination_mac = destination_mac_storage;
    } else {
        return;
    }

    auto* frame = allocate_frame_buffer();
    if (frame == nullptr) {
        return;
    }
    size_t frame_length = 0;
    if (!usernet::build_udp_ipv4_frame(frame,
                                       usernet::kMaxFrameSize,
                                       frame_length,
                                       ctx.device.info.mac,
                                       destination_mac,
                                       request.source_ip,
                                       request.destination_ip,
                                       request.source_port,
                                       request.destination_port,
                                       request.payload,
                                       request.payload_length)) {
        return;
    }

    if (ctx.registry != nullptr) {
        ++ctx.registry->net_tx_udp;
    }
    (void)write_device_frame(ctx, frame, frame_length);
}

void handle_tcp_send_request(ServerContext& ctx,
                             const networkd_protocol::SendTcpRequest& request) {
    if (request.source_port == 0 ||
        request.destination_port == 0 ||
        request.options_length > networkd_protocol::kMaxTcpOptionBytes ||
        (request.options_length % 4) != 0 ||
        request.payload_length > networkd_protocol::kMaxTcpPayload ||
        find_binding(ctx, kBindingProtocolTcp, request.source_port) == nullptr) {
        return;
    }

    uint8_t destination_mac_storage[6];
    const uint8_t* destination_mac = nullptr;
    if ((request.flags & networkd_protocol::kSendFlagBroadcast) != 0) {
        destination_mac = kBroadcastMac;
    } else if (lookup_or_resolve_mac(ctx, request.destination_ip, destination_mac_storage)) {
        destination_mac = destination_mac_storage;
    } else {
        return;
    }

    auto* frame = allocate_frame_buffer();
    if (frame == nullptr) {
        return;
    }
    size_t frame_length = 0;
    if (!usernet::build_tcp_ipv4_frame(frame,
                                       usernet::kMaxFrameSize,
                                       frame_length,
                                       ctx.device.info.mac,
                                       destination_mac,
                                       request.source_ip,
                                       request.destination_ip,
                                       request.source_port,
                                       request.destination_port,
                                       request.sequence_number,
                                       request.acknowledgment_number,
                                       request.flags,
                                       request.window_size,
                                       request.options,
                                       request.options_length,
                                       request.payload,
                                       request.payload_length)) {
        return;
    }

    if (ctx.registry != nullptr) {
        ++ctx.registry->net_tx_tcp;
    }
    (void)write_device_frame(ctx, frame, frame_length);
}

void handle_icmp_request(ServerContext& ctx,
                         const networkd_protocol::SendIcmpEchoRequest& request) {
    if (request.reply_pipe_id == 0 ||
        request.payload_length > networkd_protocol::kMaxUdpPayload) {
        return;
    }
    descriptor_defs::NetIpv4Config cfg{};
    if (!load_ipv4_config(ctx, cfg)) {
        return;
    }
    uint64_t flags = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                     static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long handle = pipe_open_existing(flags, request.reply_pipe_id);
    if (handle < 0) {
        return;
    }
    PendingPing* pending = allocate_pending_ping(ctx);
    if (pending == nullptr) {
        descriptor_close(static_cast<uint32_t>(handle));
        return;
    }
    uint8_t destination_mac[6];
    if (!lookup_or_resolve_mac(ctx, request.destination_ip, destination_mac)) {
        descriptor_close(static_cast<uint32_t>(handle));
        return;
    }
    auto* frame = allocate_frame_buffer();
    if (frame == nullptr) {
        descriptor_close(static_cast<uint32_t>(handle));
        return;
    }
    size_t frame_length = 0;
    if (!usernet::build_icmp_echo_frame(frame,
                                        usernet::kMaxFrameSize,
                                        frame_length,
                                        ctx.device.info.mac,
                                        destination_mac,
                                        cfg.address,
                                        request.destination_ip,
                                        request.identifier,
                                        request.sequence,
                                        request.payload,
                                        request.payload_length)) {
        descriptor_close(static_cast<uint32_t>(handle));
        return;
    }
    pending->in_use = true;
    pending->identifier = request.identifier;
    pending->sequence = request.sequence;
    pending->pipe_handle = static_cast<uint32_t>(handle);
    (void)write_device_frame(ctx, frame, frame_length);
}

bool poll_control(ServerContext& ctx) {
    bool did_work = false;
    for (;;) {
        auto* message = allocate_control_message_buffer();
        if (message == nullptr) {
            return did_work;
        }
        if (!networkd_protocol::read_message(ctx.server_pipe, *message)) {
            return did_work;
        }
        did_work = true;
        if (message->type == networkd_protocol::kBindUdpRequest) {
            handle_udp_bind_request(ctx, message->bind_request);
        } else if (message->type == networkd_protocol::kUnbindUdpRequest) {
            handle_udp_unbind_request(ctx, message->unbind_request);
        } else if (message->type == networkd_protocol::kBindTcpRequest) {
            handle_tcp_bind_request(ctx, message->bind_tcp_request);
        } else if (message->type == networkd_protocol::kSendUdpRequest) {
            handle_send_request(ctx, message->send_request);
        } else if (message->type == networkd_protocol::kSendTcpRequest) {
            handle_tcp_send_request(ctx, message->send_tcp_request);
        } else if (message->type == networkd_protocol::kSendIcmpEchoRequest) {
            handle_icmp_request(ctx, message->icmp_request);
        } else if (ctx.registry != nullptr) {
            ++ctx.registry->control_invalid;
        }
    }
}

bool lookup_or_resolve_mac(ServerContext& ctx,
                           const uint8_t destination_ip[4],
                           uint8_t out_mac[6]) {
    uint8_t next_hop_ip[4];
    if (!resolve_next_hop(ctx, destination_ip, next_hop_ip)) {
        return false;
    }
    ArpEntry* entry = find_arp_entry(ctx, next_hop_ip);
    if (entry != nullptr) {
        if (ctx.registry != nullptr) {
            ++ctx.registry->arp_cache_hits;
        }
        for (size_t i = 0; i < 6; ++i) {
            out_mac[i] = entry->mac[i];
        }
        return true;
    }
    if (!send_arp_request(ctx, next_hop_ip)) {
        return false;
    }
    for (size_t i = 0; i < 5000; ++i) {
        poll_network(ctx);
        entry = find_arp_entry(ctx, next_hop_ip);
        if (entry != nullptr) {
            for (size_t j = 0; j < 6; ++j) {
                out_mac[j] = entry->mac[j];
            }
            return true;
        }
        yield();
    }
    if (ctx.registry != nullptr) {
        ++ctx.registry->arp_timeouts;
    }
    return false;
}

bool poll_network(ServerContext& ctx) {
    auto* frame = allocate_frame_buffer();
    if (frame == nullptr) {
        return false;
    }
    bool did_work = false;
    for (;;) {
        long result =
            descriptor_read(ctx.device.handle, frame, usernet::kMaxFrameSize);
        if (result == kDescriptorWouldBlock || result <= 0) {
            return did_work;
        }
        did_work = true;
        if (ctx.registry != nullptr) {
            ++ctx.registry->net_rx_frames;
        }

        usernet::ArpPacketView arp{};
        if (usernet::parse_arp_frame(frame, static_cast<size_t>(result), arp)) {
            if (ctx.registry != nullptr) {
                ++ctx.registry->net_rx_arp;
            }
            if (arp.operation == 0x0001 || arp.operation == 0x0002) {
                record_arp(ctx, arp.sender_ip, arp.sender_mac);
            }
            continue;
        }

        usernet::UdpPacketView packet{};
        if (usernet::parse_udp_frame(frame, static_cast<size_t>(result), packet)) {
            if (ctx.registry != nullptr) {
                ++ctx.registry->net_rx_udp;
            }

            Binding* binding =
                find_binding(ctx, kBindingProtocolUdp, packet.destination_port);
            if (binding == nullptr) {
                if (ctx.registry != nullptr) {
                    ++ctx.registry->net_rx_no_binding;
                }
                continue;
            }
            if (ctx.registry != nullptr) {
                ++ctx.registry->net_rx_delivered;
            }
            send_udp_packet(*binding, packet);
            continue;
        }

        usernet::IcmpEchoReplyView icmp{};
        if (usernet::parse_icmp_echo_reply_frame(frame,
                                                 static_cast<size_t>(result),
                                                 icmp)) {
            if (ctx.registry != nullptr) {
                ++ctx.registry->net_rx_icmp;
            }
            PendingPing* pending =
                find_pending_ping(ctx, icmp.identifier, icmp.sequence);
            if (pending == nullptr) {
                continue;
            }
            send_icmp_reply(*pending, icmp);
            descriptor_close(pending->pipe_handle);
            pending->in_use = false;
            pending->pipe_handle = kInvalidDescriptor;
            continue;
        }

        usernet::TcpSegmentView segment{};
        if (!usernet::parse_tcp_frame(frame, static_cast<size_t>(result), segment)) {
            if (ctx.registry != nullptr) {
                ++ctx.registry->net_rx_unrecognized;
            }
            continue;
        }
        if (ctx.registry != nullptr) {
            ++ctx.registry->net_rx_tcp;
        }

        Binding* binding =
            find_binding(ctx, kBindingProtocolTcp, segment.destination_port);
        if (binding == nullptr) {
            if (ctx.registry != nullptr) {
                ++ctx.registry->net_rx_no_binding;
            }
            continue;
        }
        if (ctx.registry != nullptr) {
            ++ctx.registry->net_rx_delivered;
        }
        send_tcp_segment(*binding, segment);
    }
}

}  // namespace

int main(uint64_t, uint64_t) {
    ServerContext ctx{};
    descriptor_defs::DescriptorWait waits[2]{};
    print_line("networkd: start");

    uint64_t net_flags = static_cast<uint64_t>(descriptor_defs::Flag::Async);
    if (!usernet::open_device(ctx.device, 0, net_flags)) {
        print_line("networkd: failed to open net device 0");
        return 11;
    }
    print_line("networkd: device open ok");

    uint64_t server_flags = static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
                            static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long server_pipe = pipe_open_new(server_flags);
    if (server_pipe < 0) {
        print_line("networkd: failed to create server pipe");
        return 12;
    }
    ctx.server_pipe = static_cast<uint32_t>(server_pipe);
    print_line("networkd: server pipe created");

    auto* info = allocate_pipe_info_buffer();
    if (info == nullptr) {
        print_line("networkd: failed to allocate pipe info buffer");
        return 13;
    }
    long info_result = pipe_get_info(ctx.server_pipe, info);
    uint32_t server_pipe_id = info->id;
    unmap(info, sizeof(*info));
    if (info_result != 0 || server_pipe_id == 0) {
        print_line("networkd: failed to query server pipe");
        return 13;
    }
    print_line("networkd: server pipe info ok");
    if (!populate_registry(ctx, server_pipe_id)) {
        print_line("networkd: failed to publish registry");
        return 140 + g_registry_fail_reason;
    }
    print_line("networkd: registry published");
    ctx.registry->networkd_state = networkd_protocol::kStateReady;

    print_line("networkd: ready");

    for (;;) {
        bool did_work = false;
        did_work = poll_control(ctx) || did_work;
        did_work = poll_network(ctx) || did_work;
        if (did_work) {
            continue;
        }

        waits[0].handle = ctx.server_pipe;
        waits[0].events = descriptor_defs::kWaitRead;
        waits[0].revents = 0;
        waits[0].reserved = 0;
        waits[1].handle = ctx.device.handle;
        waits[1].events = descriptor_defs::kWaitRead;
        waits[1].revents = 0;
        waits[1].reserved = 0;
        (void)descriptor_wait(waits, 2);
        // A descriptor can be immediately readable because its peer closed.
        // Yield even in that case so kernel waiter and device-poll workers are
        // not starved by a service loop that otherwise never blocks.
        yield();
    }
}
