#include "../descriptor.hpp"

#include "../../drivers/console/console.hpp"
#include "../../drivers/log/logging.hpp"
#include "../../lib/mem.hpp"
#include "../process.hpp"
#include "../settings.hpp"

namespace descriptor {
int vty_get_property(uint32_t id,
                     uint32_t property,
                     void* out,
                     size_t size);
int vty_set_property(uint32_t id,
                     uint32_t property,
                     const void* in,
                     size_t size);
bool vty_redraw_console(uint32_t id, Console& console);
}

namespace descriptor {

namespace console_descriptor {

extern const Ops kConsoleOps;

constexpr uintptr_t kRedirectStdoutTag = 1ull << 63;

bool is_stdout_redirect(const DescriptorEntry& entry) {
    return (reinterpret_cast<uintptr_t>(entry.subsystem_data) &
            kRedirectStdoutTag) != 0;
}

uint32_t stdout_redirect_handle(const DescriptorEntry& entry) {
    return static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(entry.subsystem_data) &
        ~kRedirectStdoutTag);
}

void fill_stdout_redirect(process::Task& proc, Allocation& alloc) {
    alloc.type = kTypeConsole;
    alloc.flags = static_cast<uint64_t>(Flag::Writable);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = nullptr;
    alloc.close = nullptr;
    alloc.name = "console-stdout";
    alloc.subsystem_data =
        reinterpret_cast<void*>(kRedirectStdoutTag |
                                proc.resources->standard_descriptors[1]);
    alloc.ops = &kConsoleOps;
}

bool can_redirect_to_stdout(process::Task& proc) {
    uint32_t handle = proc.resources->standard_descriptors[1];
    if (handle == kInvalidHandle) {
        return false;
    }
    uint64_t flags = 0;
    if (!get_flags(proc.resources->descriptors, handle, false, flags)) {
        return false;
    }
    return (flags & static_cast<uint64_t>(Flag::Writable)) != 0;
}

int console_set_property(DescriptorEntry& entry,
                         uint32_t property,
                         const void* in,
                         size_t size) {
    if (is_stdout_redirect(entry)) {
        if (property ==
                static_cast<uint32_t>(descriptor_defs::Property::ConsoleCursor) ||
            property ==
                static_cast<uint32_t>(descriptor_defs::Property::ConsoleClear) ||
            property ==
                static_cast<uint32_t>(descriptor_defs::Property::ConsoleColor) ||
            property ==
                static_cast<uint32_t>(descriptor_defs::Property::ConsoleTextFlags) ||
            property ==
                static_cast<uint32_t>(descriptor_defs::Property::ConsoleKernelLog) ||
            property ==
                static_cast<uint32_t>(descriptor_defs::Property::ConsoleUpdate) ||
            property ==
                static_cast<uint32_t>(descriptor_defs::Property::ConsoleScale) ||
            property ==
                static_cast<uint32_t>(descriptor_defs::Property::ConsoleFont)) {
            return 0;
        }
        return -1;
    }

    auto* console = static_cast<Console*>(entry.object);
    if (console == nullptr) {
        return -1;
    }
    uint32_t vty_id = 0;
    if (entry.subsystem_data != nullptr) {
        vty_id = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(entry.subsystem_data));
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleCursor)) {
        if (in == nullptr || size < sizeof(descriptor_defs::CursorPosition)) {
            return -1;
        }
        auto* pos =
            reinterpret_cast<const descriptor_defs::CursorPosition*>(in);
        console->set_cursor(pos->x, pos->y);
        if (vty_id != 0) {
            return vty_set_property(
                vty_id,
                static_cast<uint32_t>(descriptor_defs::Property::VtyCursor),
                in,
                size);
        }
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleClear)) {
        console->clear();
        if (vty_id != 0) {
            return vty_set_property(
                vty_id,
                static_cast<uint32_t>(descriptor_defs::Property::VtyClear),
                nullptr,
                0);
        }
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleColor)) {
        if (in == nullptr || size < sizeof(descriptor_defs::ColorPair)) {
            return -1;
        }
        auto* colors =
            reinterpret_cast<const descriptor_defs::ColorPair*>(in);
        console->set_color(colors->fg, colors->bg);
        if (vty_id != 0) {
            return vty_set_property(
                vty_id,
                static_cast<uint32_t>(descriptor_defs::Property::VtyColor),
                in,
                size);
        }
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleTextFlags)) {
        if (in == nullptr || size < sizeof(uint8_t)) {
            return -1;
        }
        uint8_t flags = *reinterpret_cast<const uint8_t*>(in);
        console->set_text_flags(flags);
        if (vty_id != 0) {
            return vty_set_property(
                vty_id,
                static_cast<uint32_t>(descriptor_defs::Property::VtyTextFlags),
                in,
                size);
        }
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleKernelLog)) {
        if (in == nullptr || size < sizeof(uint8_t)) {
            return -1;
        }
        uint8_t enabled = *reinterpret_cast<const uint8_t*>(in);
        log_set_console_enabled(enabled != 0);
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleUpdate)) {
        if (in == nullptr || size < sizeof(uint8_t)) {
            return -1;
        }
        uint8_t deferred = *reinterpret_cast<const uint8_t*>(in);
        console->set_update_deferred(deferred != 0);
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleScale)) {
        if (in == nullptr || size != sizeof(uint32_t)) {
            return -1;
        }
        uint32_t scale = *reinterpret_cast<const uint32_t*>(in);
        bool changed = console->get_scale() != scale;
        if (!console->set_scale(scale)) {
            return -1;
        }
        if (changed) {
            settings::persist_console_scale(scale);
            if (vty_id != 0) {
                return vty_redraw_console(vty_id, *console) ? 0 : -1;
            }
            console->clear();
        }
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleFont)) {
        if (in == nullptr || size < sizeof(descriptor_defs::ConsoleFont)) {
            return -1;
        }
        auto* font = reinterpret_cast<const descriptor_defs::ConsoleFont*>(in);
        if (font->data_size > descriptor_defs::kConsoleMaxFontDataSize ||
            size != descriptor_defs::console_font_payload_size(*font) ||
            !console->set_font(*font,
                               reinterpret_cast<const uint8_t*>(font + 1))) {
            return -1;
        }
        settings::persist_console_font(*font,
                                       reinterpret_cast<const uint8_t*>(font + 1));
        if (vty_id != 0) {
            return vty_redraw_console(vty_id, *console) ? 0 : -1;
        }
        console->clear();
        return 0;
    }
    return -1;
}

int console_get_property(DescriptorEntry& entry,
                         uint32_t property,
                         void* out,
                         size_t size) {
    if (is_stdout_redirect(entry)) {
        return -1;
    }

    auto* console = static_cast<Console*>(entry.object);
    if (console == nullptr) {
        return -1;
    }

    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleScale)) {
        if (out == nullptr || size != sizeof(uint32_t)) {
            return -1;
        }
        *reinterpret_cast<uint32_t*>(out) = console->get_scale();
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleFont)) {
        if (out == nullptr || size < sizeof(descriptor_defs::ConsoleFont)) {
            return -1;
        }
        auto* font = reinterpret_cast<descriptor_defs::ConsoleFont*>(out);
        *font = console->get_font_info();
        if (size == sizeof(*font)) {
            return 0;
        }
        if (size != descriptor_defs::console_font_payload_size(*font)) {
            return -1;
        }
        memcpy(font + 1, console->get_font_data(), font->data_size);
        return 0;
    }

    uint32_t vty_id = 0;
    if (entry.subsystem_data != nullptr) {
        vty_id = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(entry.subsystem_data));
    }
    if (vty_id != 0 &&
        property == static_cast<uint32_t>(descriptor_defs::Property::VtyInfo)) {
        return vty_get_property(vty_id, property, out, size);
    }
    return -1;
}

int64_t console_read(process::Task&,
                     DescriptorEntry&,
                     uint64_t,
                     uint64_t,
                     uint64_t) {
    return -1;
}

int64_t console_write(process::Task& proc,
                      DescriptorEntry& entry,
                      uint64_t user_address,
                      uint64_t length,
                      uint64_t offset) {
    if (offset != 0) {
        return -1;
    }
    if (is_stdout_redirect(entry)) {
        return write(proc,
                     proc.resources->descriptors,
                     stdout_redirect_handle(entry),
                     user_address,
                     length,
                     offset);
    }
    if (proc.resources->vty_id != 0) {
        const char* data = reinterpret_cast<const char*>(user_address);
        if (data == nullptr || length == 0) {
            return 0;
        }
        if (vty_write(proc.resources->vty_id, data, static_cast<size_t>(length))) {
            return static_cast<int64_t>(length);
        }
    }
    auto* console = static_cast<Console*>(entry.object);
    if (console == nullptr) {
        return -1;
    }
    const char* data = reinterpret_cast<const char*>(user_address);
    if (data == nullptr || length == 0) {
        return 0;
    }
    size_t to_write = static_cast<size_t>(length);
    for (size_t i = 0; i < to_write; ++i) {
        console->putc(data[i]);
    }
    return static_cast<int64_t>(to_write);
}

const Ops kConsoleOps{
    .read = console_read,
    .write = console_write,
    .get_property = console_get_property,
    .set_property = console_set_property,
};

process::Task* g_console_owner = nullptr;
size_t g_console_refcount = 0;

void close_console(DescriptorEntry&) {
    if (g_console_refcount > 0) {
        --g_console_refcount;
    }
    if (g_console_refcount == 0) {
        g_console_owner = nullptr;
    }
}

bool open_console(process::Task& proc,
                  uint64_t,
                  uint64_t,
                  uint64_t,
                  Allocation& alloc) {
    if (proc.resources->vty_id != 0) {
        alloc.type = kTypeConsole;
        alloc.flags = static_cast<uint64_t>(Flag::Writable);
        alloc.extended_flags = 0;
        alloc.has_extended_flags = false;
        alloc.object = kconsole;
        alloc.close = nullptr;
        alloc.name = "console";
        alloc.subsystem_data =
            reinterpret_cast<void*>(static_cast<uintptr_t>(proc.resources->vty_id));
        alloc.ops = &kConsoleOps;
        return true;
    }
    if ((kconsole == nullptr ||
         (g_console_owner != nullptr && g_console_owner != &proc)) &&
        can_redirect_to_stdout(proc)) {
        fill_stdout_redirect(proc, alloc);
        return true;
    }
    if (kconsole == nullptr) {
        return false;
    }
    if (g_console_owner != nullptr && g_console_owner != &proc) {
        return false;
    }
    g_console_owner = &proc;
    ++g_console_refcount;
    alloc.type = kTypeConsole;
    alloc.flags = static_cast<uint64_t>(Flag::Writable);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = kconsole;
    alloc.close = close_console;
    alloc.name = "console";
    alloc.ops = &kConsoleOps;
    return true;
}

}  // namespace console_descriptor

bool register_console_descriptor() {
    return register_type(kTypeConsole, console_descriptor::open_console, &console_descriptor::kConsoleOps);
}

bool transfer_console_owner(process::Task& from, process::Task& to) {
    if (console_descriptor::g_console_owner != &from &&
        (console_descriptor::g_console_owner == nullptr ||
         console_descriptor::g_console_owner->resources != from.resources)) {
        return false;
    }
    console_descriptor::g_console_owner = &to;
    console_descriptor::g_console_refcount = 0;
    return true;
}

void restore_console_owner(process::Task& proc) {
    console_descriptor::g_console_owner = &proc;
    ++console_descriptor::g_console_refcount;
}

bool console_is_owner(const process::Task& proc) {
    return console_descriptor::g_console_owner == &proc ||
           (console_descriptor::g_console_owner != nullptr &&
            (console_descriptor::g_console_owner->resources == proc.resources ||
             (console_descriptor::g_console_owner->resources != nullptr &&
              proc.resources != nullptr &&
              console_descriptor::g_console_owner->resources->session_id ==
                  proc.resources->session_id &&
              process::is_foreground(proc))));
}

}  // namespace descriptor
