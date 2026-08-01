#pragma once

#include <stddef.h>
#include <stdint.h>

namespace net {

using TransmitFn = bool (*)(void* context, const void* data, size_t length);

constexpr size_t kMaxQueuedFrames = 128;
constexpr size_t kMaxQueuedFrameSize = 1600;
constexpr size_t kEthernetMtu = 1500;

struct LinkDevice {
    const char* name;
    void* context;
    TransmitFn transmit;
    uint32_t index;
    uint8_t mac[6];
    bool up;
    bool ipv4_configured;
    bool ipv4_dhcp;
    uint32_t ipv4_address;
    uint32_t ipv4_netmask;
    uint32_t ipv4_gateway;
    uint32_t ipv4_dns;
    uint16_t rx_head;
    uint16_t rx_tail;
    uint16_t rx_lengths[kMaxQueuedFrames];
    uint8_t rx_frames[kMaxQueuedFrames][kMaxQueuedFrameSize];
    uint32_t rx_frames_received;
    uint32_t rx_frames_dropped;
    volatile int rx_lock;
};

void init();
void load_config(const char* root_mount_path);

bool register_link(LinkDevice& device,
                   const char* name,
                   void* context,
                   TransmitFn transmit,
                   const uint8_t mac[6]);

size_t device_count();
LinkDevice* device_at(size_t index);
size_t queued_frame_count(LinkDevice& device);
int read_frame(LinkDevice& device,
               void* buffer,
               size_t buffer_size,
               size_t& out_size);
bool write_frame(LinkDevice& device, const void* frame, size_t length);
void get_ipv4_config(const LinkDevice& device,
                     bool& enabled,
                     bool& dhcp,
                     uint32_t& address,
                     uint32_t& netmask,
                     uint32_t& gateway,
                     uint32_t& dns);
void set_ipv4_config(LinkDevice& device,
                     bool enabled,
                     bool dhcp,
                     uint32_t address,
                     uint32_t netmask,
                     uint32_t gateway,
                     uint32_t dns);

void receive_frame(LinkDevice* device, const void* frame, size_t length);

bool send_ethernet_frame(LinkDevice& device,
                         const uint8_t destination[6],
                         uint16_t ether_type,
                         const void* payload,
                         size_t payload_length);

}  // namespace net
