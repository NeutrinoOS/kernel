#pragma once

#include <stdint.h>

namespace desktop_protocol {

constexpr const char* kRegistryName = "neutrino.desktop";
constexpr uint32_t kRegistryMagic = 0x4E44534Bu;  // NDSK
constexpr uint16_t kVersion = 1;
constexpr uint32_t kMessageMagic = 0x4E44534Du;  // NDSM
constexpr uint32_t kMaxSurfaceWidth = 1920;
constexpr uint32_t kMaxSurfaceHeight = 1080;
constexpr uint32_t kPixelFormatArgb8888 = 1;

struct Registry {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t server_pipe_id;
    uint32_t desktop_process_id;
};

enum class MessageType : uint16_t {
    Create = 1,
    Damage = 2,
    Destroy = 3,
    Created = 4,
    Keyboard = 5,
    Close = 6,
};

struct MessageHeader {
    uint32_t magic;
    uint16_t version;
    MessageType type;
    uint32_t size;
    uint32_t window_id;
    uint64_t token;
};

struct CreateMessage {
    MessageHeader header;
    uint32_t process_id;
    uint32_t reply_pipe_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t reserved;
    char surface_name[48];
    char title[48];
};

struct DamageMessage {
    MessageHeader header;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct DestroyMessage {
    MessageHeader header;
};

struct CreatedMessage {
    MessageHeader header;
    int32_t status;
    uint32_t reserved;
};

struct KeyboardMessage {
    MessageHeader header;
    uint16_t scancode;
    uint16_t mods;
    uint32_t flags;
};

struct CloseMessage {
    MessageHeader header;
};

inline MessageHeader header(MessageType type,
                            uint32_t size,
                            uint32_t window_id = 0,
                            uint64_t token = 0) {
    return {kMessageMagic, kVersion, type, size, window_id, token};
}

inline bool valid(const MessageHeader& value, MessageType type, uint32_t size) {
    return value.magic == kMessageMagic && value.version == kVersion &&
           value.type == type && value.size == size;
}

}  // namespace desktop_protocol
