#include <stddef.h>
#include <stdint.h>

#include "../crt/syscall.hpp"
#include "../desktop_protocol.hpp"
#include "font8x8_basic.hpp"

namespace {

constexpr uint32_t kKeyboardType =
    static_cast<uint32_t>(descriptor_defs::Type::Keyboard);
constexpr uint32_t kTitleHeight = 26;
constexpr uint32_t kMaxWindows = 6;
constexpr int32_t kCursorWidth = 12;
constexpr int32_t kCursorHeight = 18;
constexpr size_t kReceiveBufferSize = 2048;

struct Rect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

struct Theme {
    uint32_t background = 0x16445c;
    uint32_t taskbar = 0x131922;
    uint32_t panel = 0xedf1f4;
    uint32_t title = 0x226084;
    uint32_t accent = 0x40a9d3;
    uint32_t taskbar_height = 42;
    char launcher_label[17] = "DOOM";
    char launcher_path[96] = ".../binary/doom.elf";
    char launcher_args[160] = "-iwad .../doom1.wad";
};

struct Window {
    bool used;
    uint32_t id;
    uint32_t process;
    uint32_t event_pipe;
    uint32_t surface_handle;
    uint64_t token;
    const uint32_t* pixels;
    uint32_t surface_width;
    uint32_t surface_height;
    uint32_t width;
    uint32_t height;
    int32_t x;
    int32_t y;
    char title[49];
};

descriptor_defs::FramebufferInfo g_fb{};
uint32_t g_framebuffer = kInvalidDescriptor;
uint8_t* g_pixels = nullptr;
uint32_t g_bytes_per_pixel = 0;
Theme g_theme{};
Window g_windows[kMaxWindows]{};
uint32_t g_next_window_id = 1;
int32_t g_cursor_x = 0;
int32_t g_cursor_y = 0;
uint8_t g_previous_buttons = 0;
bool g_menu_open = false;
int32_t g_drag_index = -1;
int32_t g_drag_offset_x = 0;
int32_t g_drag_offset_y = 0;
int32_t g_resize_index = -1;
Rect g_clip{};
uint8_t g_receive_buffer[kReceiveBufferSize]{};
size_t g_receive_used = 0;

void copy_text(char* output, size_t capacity, const char* input) {
    if (output == nullptr || capacity == 0) return;
    size_t i = 0;
    if (input != nullptr) {
        while (input[i] != '\0' && i + 1 < capacity) {
            output[i] = input[i];
            ++i;
        }
    }
    output[i] = '\0';
}

void copy_fixed_text(char* output, size_t output_capacity,
                     const char* input, size_t input_capacity) {
    if (output == nullptr || output_capacity == 0) return;
    size_t i = 0;
    while (i < input_capacity && input[i] != '\0' &&
           i + 1 < output_capacity) {
        output[i] = input[i];
        ++i;
    }
    output[i] = '\0';
}

bool equal_text(const char* left, const char* right) {
    if (left == nullptr || right == nullptr) return false;
    while (*left != '\0' && *right != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

uint32_t hex_color(const char* text, uint32_t fallback) {
    uint32_t value = 0;
    for (size_t i = 0; i < 6; ++i) {
        char ch = text[i];
        uint32_t digit = 0;
        if (ch >= '0' && ch <= '9') digit = static_cast<uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') digit = static_cast<uint32_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') digit = static_cast<uint32_t>(ch - 'A' + 10);
        else return fallback;
        value = (value << 4) | digit;
    }
    return text[6] == '\0' ? value : fallback;
}

uint32_t decimal(const char* text, uint32_t fallback) {
    uint32_t value = 0;
    bool have_digit = false;
    while (*text >= '0' && *text <= '9') {
        have_digit = true;
        value = value * 10 + static_cast<uint32_t>(*text++ - '0');
    }
    return have_digit && *text == '\0' ? value : fallback;
}

void apply_setting(const char* key, const char* value) {
    if (equal_text(key, "background")) g_theme.background = hex_color(value, g_theme.background);
    else if (equal_text(key, "taskbar")) g_theme.taskbar = hex_color(value, g_theme.taskbar);
    else if (equal_text(key, "panel")) g_theme.panel = hex_color(value, g_theme.panel);
    else if (equal_text(key, "title")) g_theme.title = hex_color(value, g_theme.title);
    else if (equal_text(key, "accent")) g_theme.accent = hex_color(value, g_theme.accent);
    else if (equal_text(key, "taskbar_height")) {
        uint32_t height = decimal(value, g_theme.taskbar_height);
        if (height >= 30 && height <= 80) g_theme.taskbar_height = height;
    } else if (equal_text(key, "launcher_label")) {
        copy_text(g_theme.launcher_label, sizeof(g_theme.launcher_label), value);
    } else if (equal_text(key, "launcher_path")) {
        copy_text(g_theme.launcher_path, sizeof(g_theme.launcher_path), value);
    } else if (equal_text(key, "launcher_args")) {
        copy_text(g_theme.launcher_args, sizeof(g_theme.launcher_args), value);
    }
}

void load_config() {
    long file = file_open(".../config/desktop.cfg");
    if (file < 0) return;
    char bytes[1024]{};
    long count = file_read(static_cast<uint32_t>(file), bytes, sizeof(bytes) - 1);
    file_close(static_cast<uint32_t>(file));
    if (count <= 0) return;
    bytes[count] = '\0';
    char* line = bytes;
    for (long i = 0; i <= count; ++i) {
        if (bytes[i] != '\n' && bytes[i] != '\r' && bytes[i] != '\0') continue;
        bytes[i] = '\0';
        if (line[0] != '\0' && line[0] != '#') {
            char* separator = line;
            while (*separator != '\0' && *separator != '=') ++separator;
            if (*separator == '=') {
                *separator = '\0';
                apply_setting(line, separator + 1);
            }
        }
        line = bytes + i + 1;
    }
}

uint32_t scale_channel(uint8_t value, uint8_t bits) {
    if (bits == 0) return 0;
    uint32_t maximum = bits >= 32 ? UINT32_MAX : ((1u << bits) - 1u);
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(value) * maximum + 127u) / 255u);
}

uint32_t native_color(uint32_t rgb) {
    uint8_t red = static_cast<uint8_t>(rgb >> 16);
    uint8_t green = static_cast<uint8_t>(rgb >> 8);
    uint8_t blue = static_cast<uint8_t>(rgb);
    return (scale_channel(red, g_fb.red_mask_size) << g_fb.red_mask_shift) |
           (scale_channel(green, g_fb.green_mask_size) << g_fb.green_mask_shift) |
           (scale_channel(blue, g_fb.blue_mask_size) << g_fb.blue_mask_shift);
}

bool intersects(const Rect& left, const Rect& right) {
    return left.x < right.x + right.width && right.x < left.x + left.width &&
           left.y < right.y + right.height && right.y < left.y + left.height;
}

Rect clipped(Rect rect) {
    if (rect.x < 0) {
        rect.width += rect.x;
        rect.x = 0;
    }
    if (rect.y < 0) {
        rect.height += rect.y;
        rect.y = 0;
    }
    int32_t width = static_cast<int32_t>(g_fb.width);
    int32_t height = static_cast<int32_t>(g_fb.height);
    if (rect.x + rect.width > width) rect.width = width - rect.x;
    if (rect.y + rect.height > height) rect.height = height - rect.y;
    if (rect.width < 0) rect.width = 0;
    if (rect.height < 0) rect.height = 0;
    return rect;
}

Rect unite(Rect left, Rect right) {
    left = clipped(left);
    right = clipped(right);
    if (left.width == 0 || left.height == 0) return right;
    if (right.width == 0 || right.height == 0) return left;
    int32_t x = left.x < right.x ? left.x : right.x;
    int32_t y = left.y < right.y ? left.y : right.y;
    int32_t end_x = left.x + left.width > right.x + right.width
                        ? left.x + left.width : right.x + right.width;
    int32_t end_y = left.y + left.height > right.y + right.height
                        ? left.y + left.height : right.y + right.height;
    return {x, y, end_x - x, end_y - y};
}

void put_pixel(int32_t x, int32_t y, uint32_t value) {
    if (x < g_clip.x || y < g_clip.y ||
        x >= g_clip.x + g_clip.width || y >= g_clip.y + g_clip.height) return;
    uint8_t* pixel = g_pixels + static_cast<size_t>(y) * g_fb.pitch +
                     static_cast<size_t>(x) * g_bytes_per_pixel;
    for (uint32_t i = 0; i < g_bytes_per_pixel; ++i) {
        pixel[i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

void fill_rect(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t rgb) {
    Rect rect = clipped({x, y, width, height});
    if (!intersects(rect, g_clip)) return;
    int32_t start_x = rect.x > g_clip.x ? rect.x : g_clip.x;
    int32_t start_y = rect.y > g_clip.y ? rect.y : g_clip.y;
    int32_t end_x = rect.x + rect.width < g_clip.x + g_clip.width
                        ? rect.x + rect.width : g_clip.x + g_clip.width;
    int32_t end_y = rect.y + rect.height < g_clip.y + g_clip.height
                        ? rect.y + rect.height : g_clip.y + g_clip.height;
    uint32_t value = native_color(rgb);
    for (int32_t row = start_y; row < end_y; ++row) {
        for (int32_t column = start_x; column < end_x; ++column) {
            put_pixel(column, row, value);
        }
    }
}

void draw_char(int32_t x, int32_t y, char character, uint32_t foreground,
               uint32_t scale = 1) {
    uint8_t code = static_cast<uint8_t>(character);
    if (code >= 128 || scale == 0) return;
    for (int32_t row = 0; row < 8; ++row) {
        uint8_t glyph_row = font8x8_basic[code][row];
        for (int32_t column = 0; column < 8; ++column) {
            if ((glyph_row & (1u << column)) != 0) {
                fill_rect(x + column * static_cast<int32_t>(scale),
                          y + row * static_cast<int32_t>(scale),
                          static_cast<int32_t>(scale),
                          static_cast<int32_t>(scale), foreground);
            }
        }
    }
}

void draw_text(int32_t x, int32_t y, const char* text, uint32_t foreground,
               uint32_t scale = 1) {
    if (text == nullptr) return;
    while (*text != '\0') {
        draw_char(x, y, *text++, foreground, scale);
        x += static_cast<int32_t>(8u * scale);
    }
}

void draw_frame(int32_t x, int32_t y, int32_t width, int32_t height,
                uint32_t value) {
    fill_rect(x, y, width, 1, value);
    fill_rect(x, y + height - 1, width, 1, value);
    fill_rect(x, y, 1, height, value);
    fill_rect(x + width - 1, y, 1, height, value);
}

Rect window_rect(const Window& window) {
    return {window.x, window.y, static_cast<int32_t>(window.width),
            static_cast<int32_t>(window.height + kTitleHeight)};
}

Rect window_damage_rect(const Window& window) {
    Rect rect = window_rect(window);
    rect.width += 5;
    rect.height += 6;
    return rect;
}

void draw_window(const Window& window, bool focused) {
    int32_t width = static_cast<int32_t>(window.width);
    int32_t height = static_cast<int32_t>(window.height);
    fill_rect(window.x + 5, window.y + 6, width, height + kTitleHeight, 0x0c2a39);
    fill_rect(window.x, window.y, width, kTitleHeight,
              focused ? g_theme.title : 0x596873);
    draw_frame(window.x, window.y, width, height + kTitleHeight, 0x0b1116);
    draw_text(window.x + 8, window.y + 8, window.title, 0xffffff);
    fill_rect(window.x + width - 22, window.y + 5, 16, 16, 0xc74d4d);
    draw_text(window.x + width - 18, window.y + 9, "x", 0xffffff);

    Rect content{window.x, window.y + static_cast<int32_t>(kTitleHeight),
                 width, height};
    if (!intersects(content, g_clip)) return;
    int32_t start_x = content.x > g_clip.x ? content.x : g_clip.x;
    int32_t start_y = content.y > g_clip.y ? content.y : g_clip.y;
    int32_t end_x = content.x + content.width < g_clip.x + g_clip.width
                        ? content.x + content.width : g_clip.x + g_clip.width;
    int32_t end_y = content.y + content.height < g_clip.y + g_clip.height
                        ? content.y + content.height : g_clip.y + g_clip.height;
    for (int32_t y = start_y; y < end_y; ++y) {
        uint32_t source_y =
            static_cast<uint32_t>(y - content.y) * window.surface_height /
            window.height;
        for (int32_t x = start_x; x < end_x; ++x) {
            uint32_t source_x =
                static_cast<uint32_t>(x - content.x) * window.surface_width /
                window.width;
            put_pixel(x, y, native_color(
                window.pixels[source_y * window.surface_width + source_x]));
        }
    }
    for (int32_t inset = 3; inset <= 9; inset += 3) {
        fill_rect(window.x + width - inset - 1,
                  window.y + static_cast<int32_t>(kTitleHeight) + height - 2,
                  inset, 1, 0x65757f);
        fill_rect(window.x + width - 2,
                  window.y + static_cast<int32_t>(kTitleHeight) + height -
                      inset - 1,
                  1, inset, 0x65757f);
    }
}

bool cursor_pixel(int32_t x, int32_t y) {
    if (x == 0 && y < 15) return true;
    if ((y == x * 2 || y == x * 2 + 1) && y < 16) return true;
    if (y >= 10 && y <= 16 && x >= 3 && x <= 5) return true;
    return y >= 14 && y <= 17 && x >= 6 && x <= 8;
}

void draw_cursor() {
    for (int32_t y = 0; y < kCursorHeight; ++y) {
        for (int32_t x = 0; x < kCursorWidth; ++x) {
            if (!cursor_pixel(x, y)) continue;
            bool edge = x == 0 || y == x * 2 || y == x * 2 + 1 ||
                        y == kCursorHeight - 1;
            put_pixel(g_cursor_x + x, g_cursor_y + y,
                      native_color(edge ? 0x000000 : 0xffffff));
        }
    }
}

void render(Rect damage) {
    damage = clipped(damage);
    if (damage.width <= 0 || damage.height <= 0) return;
    g_clip = damage;
    fill_rect(0, 0, static_cast<int32_t>(g_fb.width),
              static_cast<int32_t>(g_fb.height), g_theme.background);

    int32_t taskbar_y =
        static_cast<int32_t>(g_fb.height - g_theme.taskbar_height);
    fill_rect(0, taskbar_y, static_cast<int32_t>(g_fb.width),
              static_cast<int32_t>(g_theme.taskbar_height), g_theme.taskbar);
    fill_rect(8, taskbar_y + 7, 92,
              static_cast<int32_t>(g_theme.taskbar_height) - 14, g_theme.accent);
    draw_text(20, taskbar_y + 13, "NEUTRINO", 0x192026);
    fill_rect(108, taskbar_y + 7, 88,
              static_cast<int32_t>(g_theme.taskbar_height) - 14, 0x35444e);
    draw_text(120, taskbar_y + 13, g_theme.launcher_label, 0xffffff);

    for (uint32_t i = 0; i < kMaxWindows; ++i) {
        if (g_windows[i].used) draw_window(g_windows[i], i == kMaxWindows - 1);
    }
    if (g_menu_open) {
        int32_t menu_y = taskbar_y - 96;
        fill_rect(8, menu_y, 220, 88, g_theme.panel);
        draw_frame(8, menu_y, 220, 88, 0x0b1116);
        fill_rect(9, menu_y + 1, 218, 30, g_theme.title);
        draw_text(20, menu_y + 8, "Applications", 0xffffff);
        draw_text(24, menu_y + 51, g_theme.launcher_label, 0x192026);
    }
    draw_cursor();
    descriptor_defs::FramebufferRect present{
        static_cast<uint32_t>(damage.x), static_cast<uint32_t>(damage.y),
        static_cast<uint32_t>(damage.width), static_cast<uint32_t>(damage.height)};
    (void)framebuffer_present(g_framebuffer, &present);
}

void move_to_front(uint32_t index) {
    if (index >= kMaxWindows || !g_windows[index].used) return;
    Window selected = g_windows[index];
    for (uint32_t i = index; i + 1 < kMaxWindows; ++i) {
        g_windows[i] = g_windows[i + 1];
    }
    g_windows[kMaxWindows - 1] = selected;
}

int32_t window_at(int32_t x, int32_t y) {
    for (int32_t i = static_cast<int32_t>(kMaxWindows) - 1; i >= 0; --i) {
        if (!g_windows[i].used) continue;
        Rect rect = window_rect(g_windows[i]);
        if (x >= rect.x && y >= rect.y &&
            x < rect.x + rect.width && y < rect.y + rect.height) return i;
    }
    return -1;
}

void send_close(Window& window) {
    desktop_protocol::CloseMessage message{
        desktop_protocol::header(desktop_protocol::MessageType::Close,
                                 sizeof(message), window.id, window.token)};
    (void)descriptor_write(window.event_pipe, &message, sizeof(message));
}

void remove_window(uint32_t index) {
    if (index >= kMaxWindows || !g_windows[index].used) return;
    Rect damage = window_damage_rect(g_windows[index]);
    descriptor_close(g_windows[index].event_pipe);
    descriptor_close(g_windows[index].surface_handle);
    for (uint32_t i = index; i > 0; --i) {
        g_windows[i] = g_windows[i - 1];
    }
    g_windows[0] = {};
    render(damage);
}

void launch_configured_app() {
    if (g_theme.launcher_path[0] == '\0') return;
    const char* args = g_theme.launcher_args[0] == '\0'
                           ? nullptr : g_theme.launcher_args;
    (void)child(g_theme.launcher_path, args, 0, "/");
}

bool valid_name(const char* name, size_t capacity) {
    bool terminated = false;
    for (size_t i = 0; i < capacity; ++i) {
        char ch = name[i];
        if (ch == '\0') {
            terminated = i != 0;
            break;
        }
        bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' ||
                    ch == '_';
        if (!safe) return false;
    }
    return terminated;
}

void handle_create(const desktop_protocol::CreateMessage& message) {
    desktop_protocol::CreatedMessage response{
        desktop_protocol::header(desktop_protocol::MessageType::Created,
                                 sizeof(response)), -1, 0};
    uint64_t writable = static_cast<uint64_t>(descriptor_defs::Flag::Writable) |
                        static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long reply = pipe_open_existing(writable, message.reply_pipe_id);
    if (reply < 0) return;
    if (!desktop_protocol::valid(message.header,
                                 desktop_protocol::MessageType::Create,
                                 sizeof(message)) ||
        message.process_id == 0 ||
        message.header.token == 0 ||
        message.width < 64 || message.height < 64 ||
        message.width > desktop_protocol::kMaxSurfaceWidth ||
        message.height > desktop_protocol::kMaxSurfaceHeight ||
        message.pixel_format != desktop_protocol::kPixelFormatArgb8888 ||
        !valid_name(message.surface_name, sizeof(message.surface_name))) {
        descriptor_write(static_cast<uint32_t>(reply), &response, sizeof(response));
        descriptor_close(static_cast<uint32_t>(reply));
        return;
    }
    uint32_t slot = kMaxWindows;
    for (uint32_t i = 0; i < kMaxWindows; ++i) {
        if (!g_windows[i].used) {
            slot = i;
            break;
        }
    }
    size_t surface_bytes = static_cast<size_t>(message.width) * message.height * 4;
    long surface = slot < kMaxWindows
                       ? shared_memory_open(message.surface_name, surface_bytes) : -1;
    descriptor_defs::SharedMemoryInfo info{};
    if (surface < 0 ||
        shared_memory_get_info(static_cast<uint32_t>(surface), &info) != 0 ||
        info.base == 0 || info.length < surface_bytes) {
        if (surface >= 0) descriptor_close(static_cast<uint32_t>(surface));
        descriptor_write(static_cast<uint32_t>(reply), &response, sizeof(response));
        descriptor_close(static_cast<uint32_t>(reply));
        return;
    }
    Window window{};
    window.used = true;
    window.id = g_next_window_id++;
    if (window.id == 0) window.id = g_next_window_id++;
    window.process = message.process_id;
    window.event_pipe = static_cast<uint32_t>(reply);
    window.surface_handle = static_cast<uint32_t>(surface);
    window.token = message.header.token;
    window.pixels = reinterpret_cast<const uint32_t*>(info.base);
    window.surface_width = message.width;
    window.surface_height = message.height;
    window.width = message.width;
    window.height = message.height;
    window.x = (static_cast<int32_t>(g_fb.width) -
                static_cast<int32_t>(message.width)) / 2;
    window.y = (static_cast<int32_t>(g_fb.height - g_theme.taskbar_height) -
                static_cast<int32_t>(message.height + kTitleHeight)) / 2;
    if (window.y < 0) window.y = 0;
    if (message.title[0] == '\0') {
        copy_text(window.title, sizeof(window.title), "Application");
    } else {
        copy_fixed_text(window.title, sizeof(window.title), message.title,
                        sizeof(message.title));
    }
    g_windows[slot] = window;
    move_to_front(slot);
    response.header.window_id = window.id;
    response.header.token = window.token;
    response.status = 0;
    descriptor_write(window.event_pipe, &response, sizeof(response));
    render(window_damage_rect(g_windows[kMaxWindows - 1]));
}

int32_t find_window(uint32_t id) {
    for (uint32_t i = 0; i < kMaxWindows; ++i) {
        if (g_windows[i].used && g_windows[i].id == id) return static_cast<int32_t>(i);
    }
    return -1;
}

void handle_message(const desktop_protocol::MessageHeader& header) {
    if (header.magic != desktop_protocol::kMessageMagic ||
        header.version != desktop_protocol::kVersion) return;
    if (header.type == desktop_protocol::MessageType::Create &&
        header.size == sizeof(desktop_protocol::CreateMessage)) {
        handle_create(reinterpret_cast<const desktop_protocol::CreateMessage&>(header));
        return;
    }
    int32_t index = find_window(header.window_id);
    if (index < 0) return;
    if (header.token == 0 || header.token != g_windows[index].token) return;
    if (header.type == desktop_protocol::MessageType::Damage &&
        header.size == sizeof(desktop_protocol::DamageMessage)) {
        const auto& damage =
            reinterpret_cast<const desktop_protocol::DamageMessage&>(header);
        Window& window = g_windows[index];
        if (damage.x >= window.surface_width || damage.y >= window.surface_height) return;
        uint32_t width = damage.width;
        uint32_t height = damage.height;
        if (width > window.surface_width - damage.x) width = window.surface_width - damage.x;
        if (height > window.surface_height - damage.y) height = window.surface_height - damage.y;
        uint32_t left = damage.x * window.width / window.surface_width;
        uint32_t top = damage.y * window.height / window.surface_height;
        uint32_t right =
            ((damage.x + width) * window.width + window.surface_width - 1) /
            window.surface_width;
        uint32_t bottom =
            ((damage.y + height) * window.height + window.surface_height - 1) /
            window.surface_height;
        render({window.x + static_cast<int32_t>(left),
                window.y + static_cast<int32_t>(kTitleHeight + top),
                static_cast<int32_t>(right - left),
                static_cast<int32_t>(bottom - top)});
    } else if (header.type == desktop_protocol::MessageType::Destroy &&
               header.size == sizeof(desktop_protocol::DestroyMessage)) {
        remove_window(static_cast<uint32_t>(index));
    }
}

void consume_server_pipe(uint32_t pipe) {
    long bytes = descriptor_read(pipe, g_receive_buffer + g_receive_used,
                                 sizeof(g_receive_buffer) - g_receive_used);
    if (bytes <= 0) return;
    g_receive_used += static_cast<size_t>(bytes);
    size_t consumed = 0;
    while (g_receive_used - consumed >= sizeof(desktop_protocol::MessageHeader)) {
        auto* header = reinterpret_cast<const desktop_protocol::MessageHeader*>(
            g_receive_buffer + consumed);
        if (header->size < sizeof(*header) || header->size > kReceiveBufferSize) {
            ++consumed;
            continue;
        }
        if (g_receive_used - consumed < header->size) break;
        handle_message(*header);
        consumed += header->size;
    }
    if (consumed != 0) {
        for (size_t i = consumed; i < g_receive_used; ++i) {
            g_receive_buffer[i - consumed] = g_receive_buffer[i];
        }
        g_receive_used -= consumed;
    }
    if (g_receive_used == sizeof(g_receive_buffer)) g_receive_used = 0;
}

bool handle_keyboard(uint32_t keyboard) {
    descriptor_defs::KeyboardEvent events[16]{};
    long bytes = descriptor_read(keyboard, events, sizeof(events));
    if (bytes <= 0) return true;
    size_t count = static_cast<size_t>(bytes) / sizeof(events[0]);
    for (size_t i = 0; i < count; ++i) {
        bool pressed =
            (events[i].flags & descriptor_defs::kKeyboardFlagPressed) != 0;
        if (pressed && events[i].scancode == 0x58) return false;  // F12
        if (pressed && events[i].scancode == 0x01 && g_menu_open) {
            g_menu_open = false;
            int32_t taskbar_y =
                static_cast<int32_t>(g_fb.height - g_theme.taskbar_height);
            render({8, taskbar_y - 96, 220, 88});
            continue;
        }
        Window& focused = g_windows[kMaxWindows - 1];
        if (!focused.used) continue;
        desktop_protocol::KeyboardMessage message{
            desktop_protocol::header(desktop_protocol::MessageType::Keyboard,
                                     sizeof(message), focused.id, focused.token),
            events[i].scancode, events[i].mods, events[i].flags};
        (void)descriptor_write(focused.event_pipe, &message, sizeof(message));
    }
    return true;
}

void clamp_cursor() {
    if (g_cursor_x < 0) g_cursor_x = 0;
    if (g_cursor_y < 0) g_cursor_y = 0;
    int32_t max_x = static_cast<int32_t>(g_fb.width) - kCursorWidth;
    int32_t max_y = static_cast<int32_t>(g_fb.height) - kCursorHeight;
    if (g_cursor_x > max_x) g_cursor_x = max_x;
    if (g_cursor_y > max_y) g_cursor_y = max_y;
}

void handle_mouse(uint32_t mouse) {
    descriptor_defs::MouseEvent events[24]{};
    long bytes = descriptor_read(mouse, events, sizeof(events));
    if (bytes <= 0) return;
    size_t count = static_cast<size_t>(bytes) / sizeof(events[0]);
    Rect damage{g_cursor_x, g_cursor_y, kCursorWidth, kCursorHeight};
    for (size_t i = 0; i < count; ++i) {
        g_cursor_x += events[i].dx;
        g_cursor_y -= events[i].dy;
        clamp_cursor();
        bool left_pressed = (events[i].buttons & 1u) != 0 &&
                            (g_previous_buttons & 1u) == 0;
        bool left_released = (events[i].buttons & 1u) == 0 &&
                             (g_previous_buttons & 1u) != 0;
        int32_t taskbar_y =
            static_cast<int32_t>(g_fb.height - g_theme.taskbar_height);
        if (left_pressed && g_cursor_y >= taskbar_y + 7) {
            if (g_cursor_x >= 8 && g_cursor_x < 100) {
                g_menu_open = !g_menu_open;
                damage = unite(damage, {8, taskbar_y - 96, 220, 88 +
                                        static_cast<int32_t>(g_theme.taskbar_height)});
            } else if (g_cursor_x >= 108 && g_cursor_x < 196) {
                launch_configured_app();
            }
        } else if (left_pressed && g_menu_open &&
                   g_cursor_x >= 8 && g_cursor_x < 228 &&
                   g_cursor_y >= taskbar_y - 65 && g_cursor_y < taskbar_y - 8) {
            g_menu_open = false;
            launch_configured_app();
            damage = unite(damage, {8, taskbar_y - 96, 220, 88});
        } else if (left_pressed) {
            int32_t index = window_at(g_cursor_x, g_cursor_y);
            if (index >= 0) {
                Rect old = window_damage_rect(g_windows[index]);
                move_to_front(static_cast<uint32_t>(index));
                Window& window = g_windows[kMaxWindows - 1];
                damage = unite(damage, old);
                damage = unite(damage, window_damage_rect(window));
                bool on_resize =
                    g_cursor_x >=
                        window.x + static_cast<int32_t>(window.width) - 14 &&
                    g_cursor_y >=
                        window.y + static_cast<int32_t>(
                                       kTitleHeight + window.height) - 14;
                if (on_resize) {
                    g_resize_index = static_cast<int32_t>(kMaxWindows - 1);
                } else if (g_cursor_y <
                           window.y + static_cast<int32_t>(kTitleHeight)) {
                    if (g_cursor_x >=
                        window.x + static_cast<int32_t>(window.width) - 24) {
                        send_close(window);
                    } else {
                        g_drag_index = static_cast<int32_t>(kMaxWindows - 1);
                        g_drag_offset_x = g_cursor_x - window.x;
                        g_drag_offset_y = g_cursor_y - window.y;
                    }
                }
            }
        }
        if (g_drag_index >= 0 && (events[i].buttons & 1u) != 0) {
            Window& window = g_windows[g_drag_index];
            Rect old = window_damage_rect(window);
            window.x = g_cursor_x - g_drag_offset_x;
            window.y = g_cursor_y - g_drag_offset_y;
            int32_t min_x =
                40 - static_cast<int32_t>(window.width);
            int32_t max_x = static_cast<int32_t>(g_fb.width) - 40;
            if (window.x < min_x) window.x = min_x;
            if (window.x > max_x) window.x = max_x;
            int32_t max_y =
                taskbar_y - static_cast<int32_t>(kTitleHeight) - 100;
            if (window.y < 0) window.y = 0;
            if (window.y > max_y) window.y = max_y;
            damage = unite(damage, unite(old, window_damage_rect(window)));
        }
        if (g_resize_index >= 0 && (events[i].buttons & 1u) != 0) {
            Window& window = g_windows[g_resize_index];
            Rect old = window_damage_rect(window);
            int32_t requested_width = g_cursor_x - window.x + 1;
            int32_t requested_height =
                g_cursor_y - window.y - static_cast<int32_t>(kTitleHeight) + 1;
            int32_t max_width = static_cast<int32_t>(g_fb.width);
            int32_t max_height =
                taskbar_y - window.y - static_cast<int32_t>(kTitleHeight);
            if (requested_width < 160) requested_width = 160;
            if (requested_height < 100) requested_height = 100;
            if (requested_width > max_width) requested_width = max_width;
            if (requested_height > max_height) requested_height = max_height;
            window.width = static_cast<uint32_t>(requested_width);
            window.height = static_cast<uint32_t>(requested_height);
            damage = unite(damage, unite(old, window_damage_rect(window)));
        }
        if (left_released) {
            g_drag_index = -1;
            g_resize_index = -1;
        }
        g_previous_buttons = events[i].buttons;
    }
    damage = unite(damage, {g_cursor_x, g_cursor_y, kCursorWidth, kCursorHeight});
    render(damage);
}

bool initialize_server(uint32_t& pipe_out, uint32_t& registry_out) {
    uint64_t flags = static_cast<uint64_t>(descriptor_defs::Flag::Readable) |
                     static_cast<uint64_t>(descriptor_defs::Flag::Async);
    long pipe = pipe_open_new(flags);
    if (pipe < 0) return false;
    descriptor_defs::PipeInfo pipe_info{};
    if (pipe_get_info(static_cast<uint32_t>(pipe), &pipe_info) != 0 ||
        pipe_info.id == 0) {
        descriptor_close(static_cast<uint32_t>(pipe));
        return false;
    }
    long registry = shared_memory_open(desktop_protocol::kRegistryName,
                                       sizeof(desktop_protocol::Registry));
    descriptor_defs::SharedMemoryInfo info{};
    if (registry < 0 ||
        shared_memory_get_info(static_cast<uint32_t>(registry), &info) != 0 ||
        info.base == 0 || info.length < sizeof(desktop_protocol::Registry)) {
        if (registry >= 0) descriptor_close(static_cast<uint32_t>(registry));
        descriptor_close(static_cast<uint32_t>(pipe));
        return false;
    }
    auto* published = reinterpret_cast<desktop_protocol::Registry*>(info.base);
    *published = {desktop_protocol::kRegistryMagic, desktop_protocol::kVersion,
                  0, pipe_info.id, process_id()};
    pipe_out = static_cast<uint32_t>(pipe);
    registry_out = static_cast<uint32_t>(registry);
    return true;
}

}  // namespace

int main(uint64_t, uint64_t) {
    load_config();
    long session = graphical_session_open();
    if (session < 0) return 1;
    descriptor_defs::GraphicalSessionInfo session_info{};
    if (graphical_session_get_info(static_cast<uint32_t>(session), &session_info) != 0 ||
        session_info.abi_major != descriptor_defs::kGraphicalSessionAbiMajor) {
        descriptor_close(static_cast<uint32_t>(session));
        return 1;
    }
    long framebuffer = framebuffer_open_slot(session_info.display_slot);
    if (framebuffer < 0) {
        descriptor_close(static_cast<uint32_t>(session));
        return 1;
    }
    g_framebuffer = static_cast<uint32_t>(framebuffer);
    if (framebuffer_get_info(g_framebuffer, &g_fb) != 0 ||
        g_fb.virtual_base == 0 || g_fb.width < 320 || g_fb.height < 200 ||
        g_fb.bpp < 16 || g_fb.bpp > 32 || (g_fb.bpp % 8) != 0) {
        descriptor_close(g_framebuffer);
        descriptor_close(static_cast<uint32_t>(session));
        return 1;
    }
    g_pixels = reinterpret_cast<uint8_t*>(g_fb.virtual_base);
    g_bytes_per_pixel = g_fb.bpp / 8u;

    long keyboard = descriptor_open(kKeyboardType, 0);
    long mouse = mouse_open();
    uint32_t server_pipe = kInvalidDescriptor;
    uint32_t registry = kInvalidDescriptor;
    if (keyboard < 0 || mouse < 0 || !initialize_server(server_pipe, registry) ||
        graphical_session_set_active(static_cast<uint32_t>(session), true) != 0) {
        if (registry != kInvalidDescriptor) descriptor_close(registry);
        if (server_pipe != kInvalidDescriptor) descriptor_close(server_pipe);
        if (mouse >= 0) descriptor_close(static_cast<uint32_t>(mouse));
        if (keyboard >= 0) descriptor_close(static_cast<uint32_t>(keyboard));
        descriptor_close(g_framebuffer);
        descriptor_close(static_cast<uint32_t>(session));
        return 1;
    }
    g_cursor_x = static_cast<int32_t>(g_fb.width) / 2;
    g_cursor_y = static_cast<int32_t>(g_fb.height) / 2;
    render({0, 0, static_cast<int32_t>(g_fb.width),
            static_cast<int32_t>(g_fb.height)});

    bool running = true;
    while (running) {
        descriptor_defs::DescriptorWait waits[3]{
            {static_cast<uint32_t>(keyboard), descriptor_defs::kWaitRead, 0, 0},
            {static_cast<uint32_t>(mouse), descriptor_defs::kWaitRead, 0, 0},
            {server_pipe, descriptor_defs::kWaitRead, 0, 0},
        };
        if (descriptor_wait(waits, 3) < 0) {
            yield();
            continue;
        }
        if ((waits[0].revents & descriptor_defs::kWaitRead) != 0) {
            running = handle_keyboard(static_cast<uint32_t>(keyboard));
        }
        if (running && (waits[1].revents & descriptor_defs::kWaitRead) != 0) {
            handle_mouse(static_cast<uint32_t>(mouse));
        }
        if (running && (waits[2].revents & descriptor_defs::kWaitRead) != 0) {
            consume_server_pipe(server_pipe);
        }
        while (process_wait_child(0, true) >= 0) {}
    }

    for (uint32_t i = 0; i < kMaxWindows; ++i) {
        if (g_windows[i].used) send_close(g_windows[i]);
    }
    descriptor_defs::SharedMemoryInfo registry_info{};
    (void)shared_memory_get_info(registry, &registry_info);
    auto* published = reinterpret_cast<desktop_protocol::Registry*>(
        registry_info.base);
    if (published != nullptr) published->server_pipe_id = 0;
    (void)graphical_session_set_active(static_cast<uint32_t>(session), false);
    for (uint32_t i = 0; i < kMaxWindows; ++i) {
        if (g_windows[i].used) {
            descriptor_close(g_windows[i].event_pipe);
            descriptor_close(g_windows[i].surface_handle);
        }
    }
    descriptor_close(registry);
    descriptor_close(server_pipe);
    descriptor_close(static_cast<uint32_t>(mouse));
    descriptor_close(static_cast<uint32_t>(keyboard));
    descriptor_close(g_framebuffer);
    descriptor_close(static_cast<uint32_t>(session));
    return 0;
}
