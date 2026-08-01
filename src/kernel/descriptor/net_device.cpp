#include "../descriptor.hpp"

#include "../../net/network.hpp"
#include "../string_util.hpp"
#include "../../lib/mem.hpp"

namespace descriptor {

namespace descriptor_net_device {

int64_t net_device_read(process::Task&,
                        DescriptorEntry& entry,
                        uint64_t user_address,
                        uint64_t length,
                        uint64_t offset) {
    if (offset != 0 || user_address == 0 || length == 0) {
        return (offset == 0 && length == 0) ? 0 : -1;
    }

    auto* device = static_cast<net::LinkDevice*>(entry.object);
    if (device == nullptr) {
        return -1;
    }

    size_t out_size = 0;
    int result = net::read_frame(*device,
                                 reinterpret_cast<void*>(user_address),
                                 static_cast<size_t>(length),
                                 out_size);
    if (result == 0) {
        if (has_flag(entry.flags, Flag::Async)) {
            return kWouldBlock;
        }
        return kWouldBlock;
    }
    if (result < 0) {
        return -1;
    }
    return static_cast<int64_t>(out_size);
}

int64_t net_device_write(process::Task&,
                         DescriptorEntry& entry,
                         uint64_t user_address,
                         uint64_t length,
                         uint64_t offset) {
    if (offset != 0 || user_address == 0 || length == 0) {
        return (offset == 0 && length == 0) ? 0 : -1;
    }

    auto* device = static_cast<net::LinkDevice*>(entry.object);
    if (device == nullptr) {
        return -1;
    }

    if (!net::write_frame(*device,
                          reinterpret_cast<const void*>(user_address),
                          static_cast<size_t>(length))) {
        return kWouldBlock;
    }
    return static_cast<int64_t>(length);
}

int get_property(DescriptorEntry& entry,
                 uint32_t property,
                 void* out,
                 size_t size) {
    auto* device = static_cast<net::LinkDevice*>(entry.object);
    if (device == nullptr || out == nullptr) {
        return -1;
    }

    switch (static_cast<descriptor_defs::Property>(property)) {
        case descriptor_defs::Property::NetDeviceInfo: {
            if (size < sizeof(descriptor_defs::NetDeviceInfo)) {
                return -1;
            }
            auto* info = reinterpret_cast<descriptor_defs::NetDeviceInfo*>(out);
            info->index = device->index;
            info->flags = descriptor_defs::kNetDeviceFlagRawEthernet;
            if (device->up) {
                info->flags |= descriptor_defs::kNetDeviceFlagUp;
            }
            if (device->ipv4_configured) {
                info->flags |= descriptor_defs::kNetDeviceFlagIpv4Configured;
            }
            info->rx_queued = static_cast<uint32_t>(net::queued_frame_count(*device));
            info->mtu = static_cast<uint16_t>(net::kEthernetMtu);
            info->reserved0 = 0;
            for (size_t i = 0; i < 6; ++i) {
                info->mac[i] = device->mac[i];
            }
            info->reserved1[0] = 0;
            info->reserved1[1] = 0;
            return 0;
        }
        case descriptor_defs::Property::NetIpv4Config: {
            if (size < sizeof(descriptor_defs::NetIpv4Config)) {
                return -1;
            }
            auto* cfg = reinterpret_cast<descriptor_defs::NetIpv4Config*>(out);
            bool enabled = false;
            bool dhcp = false;
            uint32_t address = 0;
            uint32_t netmask = 0;
            uint32_t gateway = 0;
            uint32_t dns = 0;
            net::get_ipv4_config(*device, enabled, dhcp, address, netmask, gateway, dns);
            cfg->address[0] = static_cast<uint8_t>((address >> 24) & 0xFFu);
            cfg->address[1] = static_cast<uint8_t>((address >> 16) & 0xFFu);
            cfg->address[2] = static_cast<uint8_t>((address >> 8) & 0xFFu);
            cfg->address[3] = static_cast<uint8_t>(address & 0xFFu);
            cfg->netmask[0] = static_cast<uint8_t>((netmask >> 24) & 0xFFu);
            cfg->netmask[1] = static_cast<uint8_t>((netmask >> 16) & 0xFFu);
            cfg->netmask[2] = static_cast<uint8_t>((netmask >> 8) & 0xFFu);
            cfg->netmask[3] = static_cast<uint8_t>(netmask & 0xFFu);
            cfg->gateway[0] = static_cast<uint8_t>((gateway >> 24) & 0xFFu);
            cfg->gateway[1] = static_cast<uint8_t>((gateway >> 16) & 0xFFu);
            cfg->gateway[2] = static_cast<uint8_t>((gateway >> 8) & 0xFFu);
            cfg->gateway[3] = static_cast<uint8_t>(gateway & 0xFFu);
            cfg->dns[0] = static_cast<uint8_t>((dns >> 24) & 0xFFu);
            cfg->dns[1] = static_cast<uint8_t>((dns >> 16) & 0xFFu);
            cfg->dns[2] = static_cast<uint8_t>((dns >> 8) & 0xFFu);
            cfg->dns[3] = static_cast<uint8_t>(dns & 0xFFu);
            cfg->flags = enabled ? descriptor_defs::kNetIpv4FlagEnabled : 0u;
            if (dhcp) {
                cfg->flags |= descriptor_defs::kNetIpv4FlagDhcp;
            }
            return 0;
        }
        case descriptor_defs::Property::NetDeviceDebug: {
            if (size < sizeof(descriptor_defs::NetDeviceDebug)) {
                return -1;
            }
            auto* debug =
                reinterpret_cast<descriptor_defs::NetDeviceDebug*>(out);
            memset(debug, 0, sizeof(*debug));
            debug->rx_queued = static_cast<uint32_t>(net::queued_frame_count(*device));
            debug->rx_frames_received = device->rx_frames_received;
            debug->rx_frames_dropped = device->rx_frames_dropped;
            return 0;
        }
        default:
            return -1;
    }
}

int set_property(DescriptorEntry& entry,
                 uint32_t property,
                 const void* in,
                 size_t size) {
    auto* device = static_cast<net::LinkDevice*>(entry.object);
    if (device == nullptr || in == nullptr) {
        return -1;
    }

    if (static_cast<descriptor_defs::Property>(property) !=
            descriptor_defs::Property::NetIpv4Config ||
        size < sizeof(descriptor_defs::NetIpv4Config)) {
        return -1;
    }

    const auto* cfg =
        reinterpret_cast<const descriptor_defs::NetIpv4Config*>(in);
    uint32_t address = (static_cast<uint32_t>(cfg->address[0]) << 24) |
                       (static_cast<uint32_t>(cfg->address[1]) << 16) |
                       (static_cast<uint32_t>(cfg->address[2]) << 8) |
                       static_cast<uint32_t>(cfg->address[3]);
    uint32_t netmask = (static_cast<uint32_t>(cfg->netmask[0]) << 24) |
                       (static_cast<uint32_t>(cfg->netmask[1]) << 16) |
                       (static_cast<uint32_t>(cfg->netmask[2]) << 8) |
                       static_cast<uint32_t>(cfg->netmask[3]);
    uint32_t gateway = (static_cast<uint32_t>(cfg->gateway[0]) << 24) |
                       (static_cast<uint32_t>(cfg->gateway[1]) << 16) |
                       (static_cast<uint32_t>(cfg->gateway[2]) << 8) |
                       static_cast<uint32_t>(cfg->gateway[3]);
    uint32_t dns = (static_cast<uint32_t>(cfg->dns[0]) << 24) |
                   (static_cast<uint32_t>(cfg->dns[1]) << 16) |
                   (static_cast<uint32_t>(cfg->dns[2]) << 8) |
                   static_cast<uint32_t>(cfg->dns[3]);
    bool enabled =
        (cfg->flags & descriptor_defs::kNetIpv4FlagEnabled) != 0;
    bool dhcp = (cfg->flags & descriptor_defs::kNetIpv4FlagDhcp) != 0;
    net::set_ipv4_config(*device, enabled, dhcp, address, netmask, gateway, dns);
    return 0;
}

const Ops kNetDeviceOps{
    .read = net_device_read,
    .write = net_device_write,
    .get_property = get_property,
    .set_property = set_property,
};

bool open_net_device(process::Task&,
                     uint64_t resource_selector,
                     uint64_t requested_flags,
                     uint64_t,
                     Allocation& alloc) {
    net::LinkDevice* device = net::device_at(static_cast<size_t>(resource_selector));
    if (device == nullptr) {
        return false;
    }

    alloc.type = kTypeNetDevice;
    alloc.flags = static_cast<uint64_t>(Flag::Readable) |
                  static_cast<uint64_t>(Flag::Writable);
    if ((requested_flags & static_cast<uint64_t>(Flag::Async)) != 0) {
        alloc.flags |= static_cast<uint64_t>(Flag::Async);
    }
    alloc.extended_flags = static_cast<uint64_t>(Flag::Async) |
                           static_cast<uint64_t>(Flag::EventSource) |
                           static_cast<uint64_t>(Flag::Device);
    alloc.has_extended_flags = true;
    alloc.object = device;
    alloc.subsystem_data = nullptr;
    alloc.close = nullptr;
    alloc.name = device->name;
    alloc.ops = &kNetDeviceOps;
    return true;
}

bool query_wait(DescriptorEntry& entry, uint32_t events, uint32_t& revents) {
    revents = 0;
    auto* device = static_cast<net::LinkDevice*>(entry.object);
    if (device == nullptr) {
        return false;
    }
    if ((events & descriptor_defs::kWaitRead) != 0 &&
        net::queued_frame_count(*device) != 0) {
        revents |= descriptor_defs::kWaitRead;
    }
    if ((events & descriptor_defs::kWaitWrite) != 0 && device->up) {
        revents |= descriptor_defs::kWaitWrite;
    }
    return true;
}

}  // namespace descriptor_net_device

bool register_net_device_descriptor() {
    return register_type(kTypeNetDevice,
                         descriptor_net_device::open_net_device,
                         &descriptor_net_device::kNetDeviceOps);
}

}  // namespace descriptor
