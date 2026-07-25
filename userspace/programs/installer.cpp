#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "descriptors.hpp"
#include "keyboard_scancode.hpp"
#include "../auth/password_hash.hpp"
#include "../crt/syscall.hpp"
#include "../helpers/console.hpp"

namespace {

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);
constexpr uint32_t kDescKeyboard =
    static_cast<uint32_t>(descriptor_defs::Type::Keyboard);
constexpr uint32_t kDescBlock =
    static_cast<uint32_t>(descriptor_defs::Type::BlockDevice);

constexpr uint32_t kDefaultFg = 0xFFFFFFFF;
constexpr uint32_t kDefaultBg = 0x00000000;
constexpr long kWouldBlock = -2;
constexpr const char* kInstallerPath = "binary/installer.elf";
constexpr const char* kUserDirectory = "user";
constexpr const char* kRootHomeDirectory = "user/root";
constexpr uint8_t kNeufsMagic[8] = {
    0x4E, 0x45, 0x55, 0x46, 0x53, 0x00, 0x77, 0x42};
constexpr int32_t kNeufsVersion = 1;
constexpr uint8_t kTypeNdir = 0;
constexpr const char* kEspModuleDevice = "MEMDISK_1_0";
constexpr uint64_t kGptEntryCount = 128;
constexpr uint64_t kGptEntrySize = 128;
constexpr uint64_t kGptEntrySectors = 32;
constexpr uint64_t kInstallEspFirstLba = 2048;

#pragma pack(push, 1)
struct NeufsRvt {
    char magic[8];
    int32_t version;
    char name[16];
    uint64_t root;
};

struct NeufsNdir {
    uint8_t type;
    uint8_t reserved[7];
    char name[256];
    int64_t ctime;
    int64_t utime;
    uint64_t acl[32];
    uint64_t parent;
    uint64_t contents[64];
    uint64_t next;
    uint64_t last;
};

struct GptLayout {
    uint64_t esp_first_lba;
    uint64_t esp_last_lba;
    uint64_t root_first_lba;
    uint64_t root_last_lba;
};
#pragma pack(pop)

enum class InstallFs {
    Neufs,
    Fat32,
};

struct Device {
    char name[64];
    descriptor_defs::BlockGeometry geom;
    bool writable;
    bool is_memdisk;
    bool is_usb_mass_storage;
};

struct CopyProgress {
    long console;
    uint64_t total_files;
    uint64_t total_bytes;
    uint64_t copied_files;
    uint64_t copied_bytes;
    uint64_t last_percent;
    uint64_t next_byte_report;
    char current[160];
};

bool set_cursor(long console, uint32_t x, uint32_t y) {
    descriptor_defs::CursorPosition pos{x, y};
    long res = descriptor_set_property(
        static_cast<uint32_t>(console),
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleCursor),
        &pos, sizeof(pos));
    return res == 0;
}

bool clear_console(long console) {
    long res = descriptor_set_property(
        static_cast<uint32_t>(console),
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleClear),
        nullptr, 0);
    return res == 0;
}

bool set_console_color(long console, uint32_t fg, uint32_t bg) {
    descriptor_defs::ColorPair colors{fg, bg};
    long res = descriptor_set_property(
        static_cast<uint32_t>(console),
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleColor),
        &colors, sizeof(colors));
    return res == 0;
}

bool set_kernel_log_console(long console, bool enabled) {
    uint8_t value = enabled ? 1 : 0;
    long res = descriptor_set_property(
        static_cast<uint32_t>(console),
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleKernelLog),
        &value,
        sizeof(value));
    return res == 0;
}

char read_char_blocking(uint32_t keyboard) {
    descriptor_defs::KeyboardEvent events[8]{};
    while (true) {
        long r = descriptor_read(keyboard, events, sizeof(events));
        if (r <= 0) {
            yield();
            continue;
        }
        size_t count = static_cast<size_t>(r) / sizeof(events[0]);
        for (size_t i = 0; i < count; ++i) {
            const auto& ev = events[i];
            if (!keyboard::is_pressed(ev)) {
                continue;
            }
            char c = keyboard::scancode_to_char(ev.scancode, ev.mods);
            if (c != 0) {
                return c;
            }
        }
    }
}

void drain_keyboard(uint32_t keyboard) {
    descriptor_defs::KeyboardEvent events[8];
    while (descriptor_read(keyboard, events, sizeof(events)) > 0) {
        // discard
    }
}

size_t read_line(uint32_t keyboard,
                 long console,
                 char* out,
                 size_t out_cap) {
    if (out == nullptr || out_cap == 0) {
        return 0;
    }
    size_t len = 0;
    while (len + 1 < out_cap) {
        char c = read_char_blocking(keyboard);
        if (c == '\n' || c == '\r') {
            descriptor_write(static_cast<uint32_t>(console), "\n", 1);
            break;
        }
        if (c == '\b') {
            if (len > 0) {
                --len;
                descriptor_write(static_cast<uint32_t>(console), "\b \b", 3);
            }
            continue;
        }
        out[len++] = c;
        descriptor_write(static_cast<uint32_t>(console), &c, 1);
    }
    out[len] = '\0';
    return len;
}

void zero_memory(void* ptr, size_t size) {
    auto* bytes = static_cast<volatile uint8_t*>(ptr);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = 0;
    }
}

bool read_secret_line(uint32_t keyboard,
                      long console,
                      char* out,
                      size_t out_cap) {
    if (out == nullptr || out_cap == 0) {
        return false;
    }
    size_t len = 0;
    out[0] = '\0';
    while (true) {
        char c = read_char_blocking(keyboard);
        if (c == '\n' || c == '\r') {
            descriptor_write(static_cast<uint32_t>(console), "\n", 1);
            out[len] = '\0';
            return true;
        }
        if (c == '\b' || c == 0x7F) {
            if (len > 0) {
                --len;
                out[len] = '\0';
            }
            continue;
        }
        if (c < 0x20 || c > 0x7E || len + 1 >= out_cap) {
            continue;
        }
        out[len++] = c;
        out[len] = '\0';
    }
}

bool provision_root_password(uint32_t keyboard, long console) {
    void* root = user_find("root");
    if (root == nullptr) {
        userspace::write_line(console,
                              "Unable to find the live root account.");
        return false;
    }

    char password[96]{};
    char confirmation[96]{};
    while (true) {
        drain_keyboard(keyboard);
        userspace::write(console, "Set installed root password: ");
        if (!read_secret_line(keyboard,
                              console,
                              password,
                              sizeof(password))) {
            zero_memory(password, sizeof(password));
            return false;
        }
        if (password[0] == '\0') {
            userspace::write_line(console, "Password cannot be empty.");
            continue;
        }

        userspace::write(console, "Confirm root password: ");
        if (!read_secret_line(keyboard,
                              console,
                              confirmation,
                              sizeof(confirmation))) {
            zero_memory(password, sizeof(password));
            zero_memory(confirmation, sizeof(confirmation));
            return false;
        }
        if (strcmp(password, confirmation) == 0) {
            break;
        }
        zero_memory(password, sizeof(password));
        zero_memory(confirmation, sizeof(confirmation));
        userspace::write_line(console, "Passwords do not match. Try again.");
    }

    uint8_t salt[auth::kPasswordSaltSize]{};
    uint8_t hash[auth::kPasswordHashSize]{};
    if (!auth::make_salt("root", salt)) {
        userspace::write_line(console,
                              "Secure random source unavailable.");
        zero_memory(password, sizeof(password));
        zero_memory(confirmation, sizeof(confirmation));
        return false;
    }
    auth::pbkdf2_sha256(password,
                        salt,
                        sizeof(salt),
                        auth::kPasswordIterations,
                        hash);
    zero_memory(password, sizeof(password));
    zero_memory(confirmation, sizeof(confirmation));

    long result =
        user_set_password(root, salt, hash, auth::kPasswordIterations);
    zero_memory(salt, sizeof(salt));
    zero_memory(hash, sizeof(hash));
    if (result < 0) {
        userspace::write_line(console,
                              "Failed to save the root password.");
        return false;
    }
    return true;
}

bool prompt_yes_no_line(uint32_t keyboard, long console, const char* prompt) {
    char buf[8];
    userspace::write(console, prompt);
    userspace::write(console, " [y/N]: ");
    drain_keyboard(keyboard);
    size_t len = read_line(keyboard, console, buf, sizeof(buf));
    if (len == 0) {
        return false;
    }
    char c = buf[0];
    return c == 'y' || c == 'Y';
}

bool parse_uint(const char* text, size_t& out_value) {
    out_value = 0;
    if (text == nullptr || *text == '\0') {
        return false;
    }
    const char* cursor = text;
    while (*cursor != '\0') {
        char c = *cursor++;
        if (c < '0' || c > '9') {
            return false;
        }
        out_value = out_value * 10 + static_cast<size_t>(c - '0');
    }
    return true;
}

uint64_t align_up(uint64_t value, uint64_t alignment) {
    if (alignment == 0) {
        return value;
    }
    uint64_t rem = value % alignment;
    return rem == 0 ? value : value + (alignment - rem);
}

void store_u16_le(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}

void store_u32_le(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

void store_u64_le(uint8_t* out, uint64_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
    out[4] = static_cast<uint8_t>(value >> 32);
    out[5] = static_cast<uint8_t>(value >> 40);
    out[6] = static_cast<uint8_t>(value >> 48);
    out[7] = static_cast<uint8_t>(value >> 56);
}

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t size) {
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (size_t bit = 0; bit < 8; ++bit) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

uint32_t crc32(const uint8_t* data, size_t size) {
    return crc32_update(0, data, size);
}

bool suffix_is_decimal(const char* text, size_t begin, size_t end) {
    if (text == nullptr || begin >= end) {
        return false;
    }
    for (size_t i = begin; i < end; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    return true;
}

bool is_partition_device_name(const char* name) {
    if (name == nullptr) {
        return false;
    }
    size_t len = strlen(name);
    if (len == 0) {
        return false;
    }

    size_t last_sep = len;
    while (last_sep > 0 && name[last_sep - 1] != '_') {
        --last_sep;
    }
    if (last_sep == 0 || !suffix_is_decimal(name, last_sep, len)) {
        return false;
    }
    --last_sep;

    size_t prev_sep = last_sep;
    while (prev_sep > 0 && name[prev_sep - 1] != '_') {
        --prev_sep;
    }
    return prev_sep != 0 && suffix_is_decimal(name, prev_sep, last_sep);
}

bool is_install_target(const Device& dev) {
    return !dev.is_memdisk && !dev.is_usb_mass_storage &&
           !is_partition_device_name(dev.name);
}

size_t collect_install_targets(const Device* devices,
                               size_t device_count,
                               Device* targets,
                               size_t target_capacity) {
    if (devices == nullptr || targets == nullptr) {
        return 0;
    }
    size_t target_count = 0;
    for (size_t i = 0; i < device_count && target_count < target_capacity; ++i) {
        if (!is_install_target(devices[i])) {
            continue;
        }
        targets[target_count++] = devices[i];
    }
    return target_count;
}

bool append_partition_suffix(const char* disk_name,
                             uint32_t partition_index,
                             char* out,
                             size_t out_size) {
    if (disk_name == nullptr || out == nullptr || out_size == 0) {
        return false;
    }
    size_t idx = 0;
    for (size_t i = 0; disk_name[i] != '\0'; ++i) {
        if (idx + 1 >= out_size) {
            return false;
        }
        out[idx++] = disk_name[i];
    }
    if (idx + 1 >= out_size) {
        return false;
    }
    out[idx++] = '_';
    char digits[10];
    size_t digit_count = 0;
    do {
        digits[digit_count++] = static_cast<char>('0' + partition_index % 10);
        partition_index /= 10;
    } while (partition_index != 0 && digit_count < sizeof(digits));
    if (idx + digit_count >= out_size) {
        return false;
    }
    for (size_t i = 0; i < digit_count; ++i) {
        out[idx++] = digits[digit_count - 1 - i];
    }
    out[idx] = '\0';
    return true;
}

void format_size(uint64_t bytes, char* out, size_t out_cap) {
    if (out == nullptr || out_cap == 0) {
        return;
    }
    const uint64_t kGiB = 1024ull * 1024ull * 1024ull;
    const uint64_t kMiB = 1024ull * 1024ull;
    if (bytes >= kGiB) {
        uint64_t whole = bytes / kGiB;
        uint64_t frac = (bytes % kGiB) * 10 / kGiB;
        // format "<whole>.<frac> GiB"
        char buf[32];
        size_t idx = 0;
        // whole part
        char digits[20];
        size_t digit_count = 0;
        uint64_t temp = whole;
        do {
            digits[digit_count++] = static_cast<char>('0' + (temp % 10));
            temp /= 10;
        } while (temp > 0 && digit_count < sizeof(digits));
        for (size_t i = 0; i < digit_count && idx + 1 < sizeof(buf); ++i) {
            buf[idx++] = digits[digit_count - 1 - i];
        }
        if (idx + 3 < sizeof(buf)) {
            buf[idx++] = '.';
            buf[idx++] = static_cast<char>('0' + (frac % 10));
            buf[idx++] = ' ';
            buf[idx++] = 'G';
            buf[idx++] = 'i';
            buf[idx++] = 'B';
            buf[idx] = '\0';
        } else {
            buf[0] = '\0';
        }
        // copy to out
        size_t copy_len = strlen(buf);
        if (copy_len >= out_cap) {
            copy_len = out_cap - 1;
        }
        for (size_t i = 0; i < copy_len; ++i) {
            out[i] = buf[i];
        }
        out[copy_len] = '\0';
        return;
    }
    uint64_t whole = bytes / kMiB;
    char buf[32];
    size_t idx = 0;
    char digits[20];
    size_t digit_count = 0;
    uint64_t temp = whole;
    do {
        digits[digit_count++] = static_cast<char>('0' + (temp % 10));
        temp /= 10;
    } while (temp > 0 && digit_count < sizeof(digits));
    for (size_t i = 0; i < digit_count && idx + 1 < sizeof(buf); ++i) {
        buf[idx++] = digits[digit_count - 1 - i];
    }
    if (idx + 3 < sizeof(buf)) {
        buf[idx++] = ' ';
        buf[idx++] = 'M';
        buf[idx++] = 'i';
        buf[idx++] = 'B';
        buf[idx] = '\0';
    } else {
        buf[0] = '\0';
    }
    size_t copy_len = strlen(buf);
    if (copy_len >= out_cap) {
        copy_len = out_cap - 1;
    }
    for (size_t i = 0; i < copy_len; ++i) {
        out[i] = buf[i];
    }
    out[copy_len] = '\0';
}

uint64_t default_neufs_meta_size(uint64_t total_bytes, uint64_t sector_size) {
    uint64_t suggested = (total_bytes * 225) / 10000;
    const uint64_t min_meta = 256ull * 1024ull * 1024ull;
    const uint64_t max_meta = 16ull * 1024ull * 1024ull * 1024ull;
    if (suggested < min_meta && total_bytes >= min_meta * 2) {
        suggested = min_meta;
    }
    if (suggested > max_meta) {
        suggested = max_meta;
    }
    if (sector_size != 0 && suggested > total_bytes - sector_size) {
        suggested = total_bytes - sector_size;
    } else if (sector_size == 0 && suggested > total_bytes) {
        suggested = total_bytes;
    }
    return align_up(suggested, sector_size);
}

bool fetch_devices(Device* devices, size_t capacity, size_t& count) {
    if (devices == nullptr || capacity == 0) {
        return false;
    }
    count = 0;
    for (size_t idx = 0; idx < capacity; ++idx) {
        long handle = descriptor_open(kDescBlock, 0, idx, 0);
        if (handle < 0) {
            break;
        }

        Device& dev = devices[count];
        for (size_t i = 0; i < sizeof(dev.name); ++i) {
            dev.name[i] = '\0';
        }
        descriptor_get_property(
            static_cast<uint32_t>(handle),
            static_cast<uint32_t>(descriptor_defs::Property::CommonName),
            dev.name, sizeof(dev.name));

        dev.geom = {};
        descriptor_get_property(
            static_cast<uint32_t>(handle),
            static_cast<uint32_t>(descriptor_defs::Property::BlockGeometry),
            &dev.geom, sizeof(dev.geom));

        dev.writable = descriptor_test_flag(
            static_cast<uint32_t>(handle),
            static_cast<uint64_t>(descriptor_defs::Flag::Writable)) == 1;

        dev.is_memdisk = strncmp(dev.name, "MEMDISK_", strlen("MEMDISK_")) == 0;
        dev.is_usb_mass_storage =
            strncmp(dev.name, "USBMS_", strlen("USBMS_")) == 0;

        descriptor_close(static_cast<uint32_t>(handle));
        ++count;
    }
    return count > 0;
}

long read_with_retry(uint32_t handle,
                     void* buffer,
                     size_t length,
                     uint64_t offset) {
    while (true) {
        long r = descriptor_read(handle, buffer, length, offset);
        if (r == kWouldBlock) {
            yield();
            continue;
        }
        return r;
    }
}

long write_with_retry(uint32_t handle,
                      const void* buffer,
                      size_t length,
                      uint64_t offset) {
    while (true) {
        long r = descriptor_write(handle, buffer, length, offset);
        if (r == kWouldBlock) {
            yield();
            continue;
        }
        return r;
    }
}

bool copy_block_device(uint32_t src_handle,
                       uint32_t dst_handle,
                       uint64_t src_offset,
                       uint64_t dst_offset,
                       uint64_t byte_count,
                       uint8_t* buffer,
                       uint64_t buffer_size) {
    while (byte_count > 0) {
        uint64_t chunk = byte_count < buffer_size ? byte_count : buffer_size;
        long read = read_with_retry(src_handle,
                                    buffer,
                                    static_cast<size_t>(chunk),
                                    src_offset);
        if (read != static_cast<long>(chunk)) {
            return false;
        }
        long written = write_with_retry(dst_handle,
                                        buffer,
                                        static_cast<size_t>(chunk),
                                        dst_offset);
        if (written != static_cast<long>(chunk)) {
            return false;
        }
        src_offset += chunk;
        dst_offset += chunk;
        byte_count -= chunk;
    }
    return true;
}

void write_utf16_name(uint8_t* entry, const char* name) {
    uint8_t* out = entry + 56;
    for (size_t i = 0; i < 36; ++i) {
        uint16_t ch = 0;
        if (name != nullptr && name[i] != '\0') {
            ch = static_cast<uint8_t>(name[i]);
        }
        store_u16_le(out + i * 2, ch);
        if (ch == 0) {
            break;
        }
    }
}

void write_gpt_entry(uint8_t* table,
                     size_t index,
                     const uint8_t type_guid[16],
                     const uint8_t unique_guid[16],
                     uint64_t first_lba,
                     uint64_t last_lba,
                     const char* name) {
    uint8_t* entry = table + index * kGptEntrySize;
    memcpy(entry, type_guid, 16);
    memcpy(entry + 16, unique_guid, 16);
    store_u64_le(entry + 32, first_lba);
    store_u64_le(entry + 40, last_lba);
    store_u64_le(entry + 48, 0);
    write_utf16_name(entry, name);
}

void write_gpt_header(uint8_t* sector,
                      uint64_t current_lba,
                      uint64_t backup_lba,
                      uint64_t first_usable_lba,
                      uint64_t last_usable_lba,
                      uint64_t entries_lba,
                      uint32_t entries_crc,
                      const uint8_t disk_guid[16]) {
    memset(sector, 0, 512);
    const uint8_t signature[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    memcpy(sector, signature, sizeof(signature));
    store_u32_le(sector + 8, 0x00010000u);
    store_u32_le(sector + 12, 92);
    store_u64_le(sector + 24, current_lba);
    store_u64_le(sector + 32, backup_lba);
    store_u64_le(sector + 40, first_usable_lba);
    store_u64_le(sector + 48, last_usable_lba);
    memcpy(sector + 56, disk_guid, 16);
    store_u64_le(sector + 72, entries_lba);
    store_u32_le(sector + 80, static_cast<uint32_t>(kGptEntryCount));
    store_u32_le(sector + 84, static_cast<uint32_t>(kGptEntrySize));
    store_u32_le(sector + 88, entries_crc);
    store_u32_le(sector + 16, 0);
    uint32_t header_crc = crc32(sector, 92);
    store_u32_le(sector + 16, header_crc);
}

bool write_protective_mbr(uint32_t handle, uint64_t total_sectors, uint8_t* sector) {
    memset(sector, 0, 512);
    sector[446 + 4] = 0xEE;
    store_u32_le(sector + 446 + 8, 1);
    uint64_t protected_sectors = total_sectors > 0 ? total_sectors - 1 : 0;
    if (protected_sectors > 0xFFFFFFFFull) {
        protected_sectors = 0xFFFFFFFFull;
    }
    store_u32_le(sector + 446 + 12, static_cast<uint32_t>(protected_sectors));
    sector[510] = 0x55;
    sector[511] = 0xAA;
    return write_with_retry(handle, sector, 512, 0) == 512;
}

bool write_gpt(uint32_t handle,
               uint64_t total_sectors,
               uint64_t esp_sectors,
               GptLayout& layout,
               uint8_t* scratch,
               uint64_t scratch_size,
               long console) {
    if (scratch == nullptr || scratch_size < 512 ||
        total_sectors < 4096 || esp_sectors == 0) {
        return false;
    }

    layout.esp_first_lba = kInstallEspFirstLba;
    layout.esp_last_lba = layout.esp_first_lba + esp_sectors - 1;
    layout.root_first_lba = align_up(layout.esp_last_lba + 1, 2048);
    uint64_t backup_header_lba = total_sectors - 1;
    uint64_t backup_entries_lba = backup_header_lba - kGptEntrySectors;
    layout.root_last_lba = backup_entries_lba - 1;
    if (layout.root_first_lba >= layout.root_last_lba) {
        userspace::write_line(console, "target too small for ESP + NEUFS GPT layout");
        return false;
    }

    if (!write_protective_mbr(handle, total_sectors, scratch)) {
        return false;
    }

    uint64_t entries_bytes = kGptEntryCount * kGptEntrySize;
    if (scratch_size < entries_bytes) {
        return false;
    }
    memset(scratch, 0, static_cast<size_t>(entries_bytes));

    const uint8_t esp_type[16] = {
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};
    const uint8_t linux_type[16] = {
        0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
        0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4};
    const uint8_t disk_guid[16] = {
        0x4E, 0x54, 0x52, 0x4E, 0x49, 0x4E, 0x4F, 0x00,
        0x90, 0x01, 0x12, 0x34, 0x56, 0x78, 0x00, 0x01};
    const uint8_t esp_guid[16] = {
        0x4E, 0x54, 0x52, 0x4E, 0x45, 0x53, 0x50, 0x00,
        0x90, 0x01, 0x12, 0x34, 0x56, 0x78, 0x00, 0x02};
    const uint8_t root_guid[16] = {
        0x4E, 0x54, 0x52, 0x4E, 0x52, 0x4F, 0x4F, 0x54,
        0x90, 0x01, 0x12, 0x34, 0x56, 0x78, 0x00, 0x03};

    write_gpt_entry(scratch,
                    0,
                    esp_type,
                    esp_guid,
                    layout.esp_first_lba,
                    layout.esp_last_lba,
                    "EFI System");
    write_gpt_entry(scratch,
                    1,
                    linux_type,
                    root_guid,
                    layout.root_first_lba,
                    layout.root_last_lba,
                    "Neutrino Root");

    uint32_t entries_crc = crc32(scratch, static_cast<size_t>(entries_bytes));
    uint64_t primary_entries_lba = 2;
    uint64_t sector_size = 512;
    if (write_with_retry(handle,
                         scratch,
                         static_cast<size_t>(entries_bytes),
                         primary_entries_lba * sector_size) !=
        static_cast<long>(entries_bytes)) {
        return false;
    }
    if (write_with_retry(handle,
                         scratch,
                         static_cast<size_t>(entries_bytes),
                         backup_entries_lba * sector_size) !=
        static_cast<long>(entries_bytes)) {
        return false;
    }

    write_gpt_header(scratch,
                     1,
                     backup_header_lba,
                     34,
                     layout.root_last_lba,
                     primary_entries_lba,
                     entries_crc,
                     disk_guid);
    if (write_with_retry(handle, scratch, 512, 512) != 512) {
        return false;
    }

    write_gpt_header(scratch,
                     backup_header_lba,
                     1,
                     34,
                     layout.root_last_lba,
                     backup_entries_lba,
                     entries_crc,
                     disk_guid);
    if (write_with_retry(handle,
                         scratch,
                         512,
                         backup_header_lba * sector_size) != 512) {
        return false;
    }

    return true;
}

void render_table(long console,
                  const Device* devices,
                  size_t count,
                  size_t highlight_row,
                  size_t first_data_row) {
    set_cursor(console, 0, static_cast<uint32_t>(first_data_row));
    for (size_t i = 0; i < count; ++i) {
        char size_buf[32];
        uint64_t bytes =
            devices[i].geom.sector_size * devices[i].geom.sector_count;
        format_size(bytes, size_buf, sizeof(size_buf));

        if (i == highlight_row) {
            set_console_color(console, 0xFF000000, 0xFFFFFFFF);
        } else {
            set_console_color(console, kDefaultFg, kDefaultBg);
        }

        userspace::write(console, " ");
        char idx_buf[6];
        idx_buf[0] = '#';
        idx_buf[1] = '\0';
        size_t idx_len = 1;
        size_t idx = i;
        char digits[10];
        size_t digit_count = 0;
        do {
            digits[digit_count++] = static_cast<char>('0' + (idx % 10));
            idx /= 10;
        } while (idx > 0 && digit_count < sizeof(digits));
        for (size_t d = 0; d < digit_count && idx_len + 1 < sizeof(idx_buf);
             ++d) {
            idx_buf[idx_len++] = digits[digit_count - 1 - d];
        }
        idx_buf[idx_len] = '\0';
        userspace::write(console, idx_buf);
        userspace::write(console, "  ");
        userspace::write(console, devices[i].name);
        userspace::write(console, "  ");
        userspace::write(console, size_buf);
        userspace::write(console, "  ");
        userspace::write(console, devices[i].writable ? "[rw]" : "[ro]");
        if (devices[i].is_memdisk) {
            userspace::write(console, "  (source)");
        }
        descriptor_write(static_cast<uint32_t>(console), "\n", 1);
    }
    set_console_color(console, kDefaultFg, kDefaultBg);
}

bool copy_image(const char* src_name,
                const Device& src,
                const char* dst_name,
                const Device& dst,
                long console) {
    if (src_name == nullptr || dst_name == nullptr) {
        return false;
    }

    if (dst.geom.sector_size == 0 || src.geom.sector_size == 0 ||
        src.geom.sector_size != dst.geom.sector_size) {
        userspace::write_line(console, "sector size mismatch");
        return false;
    }
    if (dst.geom.sector_count < src.geom.sector_count) {
        userspace::write_line(console, "destination too small for image");
        return false;
    }
    uint64_t sector_size = src.geom.sector_size;
    uint64_t total_sectors = src.geom.sector_count;

    long src_handle = descriptor_open(
        kDescBlock,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(src_name)),
        0, 0);
    long dst_handle = descriptor_open(
        kDescBlock,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(dst_name)),
        0, 0);
    if (src_handle < 0 || dst_handle < 0) {
        userspace::write_line(console, "failed to open block devices");
        if (src_handle >= 0) descriptor_close(static_cast<uint32_t>(src_handle));
        if (dst_handle >= 0) descriptor_close(static_cast<uint32_t>(dst_handle));
        return false;
    }

    const uint64_t kChunkSectors = 1024;  // ~512KiB per transfer
    uint64_t chunk_bytes = kChunkSectors * sector_size;
    uint8_t* buffer = static_cast<uint8_t*>(
        map_anonymous(static_cast<size_t>(chunk_bytes), MAP_WRITE));
    if (buffer == nullptr) {
        userspace::write_line(console, "failed to allocate buffer");
        descriptor_close(static_cast<uint32_t>(src_handle));
        descriptor_close(static_cast<uint32_t>(dst_handle));
        return false;
    }

    uint64_t copied = 0;
    uint64_t last_pct = UINT64_MAX;
    userspace::write_line(console, "Starting install...");
    while (copied < total_sectors) {
        uint64_t batch = kChunkSectors;
        if (batch > total_sectors - copied) {
            batch = total_sectors - copied;
        }
        uint64_t bytes = batch * sector_size;
        uint64_t offset = copied * sector_size;

        long r = read_with_retry(static_cast<uint32_t>(src_handle),
                                 buffer,
                                 static_cast<size_t>(bytes),
                                 offset);
        if (r < 0 || static_cast<uint64_t>(r) != bytes) {
            userspace::write_line(console, "read failed");
            unmap(buffer, static_cast<size_t>(chunk_bytes));
            descriptor_close(static_cast<uint32_t>(src_handle));
            descriptor_close(static_cast<uint32_t>(dst_handle));
            return false;
        }
        long w = write_with_retry(static_cast<uint32_t>(dst_handle),
                                  buffer,
                                  static_cast<size_t>(bytes),
                                  offset);
        if (w < 0 || static_cast<uint64_t>(w) != bytes) {
            userspace::write_line(console, "write failed");
            unmap(buffer, static_cast<size_t>(chunk_bytes));
            descriptor_close(static_cast<uint32_t>(src_handle));
            descriptor_close(static_cast<uint32_t>(dst_handle));
            return false;
        }
        copied += batch;

        uint64_t percent = (copied * 100) / total_sectors;
        if (percent != last_pct) {
            last_pct = percent;
            set_cursor(console, 0, 6);
            userspace::write(console, "Installing... ");
            char pct[8];
            pct[0] = static_cast<char>('0' + (percent / 10));
            pct[1] = static_cast<char>('0' + (percent % 10));
            pct[2] = '%';
            pct[3] = '\0';
            userspace::write(console, pct);
            descriptor_write(static_cast<uint32_t>(console), "   \r", 4);
            yield();
        }
    }

    unmap(buffer, static_cast<size_t>(chunk_bytes));
    descriptor_close(static_cast<uint32_t>(src_handle));
    descriptor_close(static_cast<uint32_t>(dst_handle));
    set_cursor(console, 0, 6);
    userspace::write_line(console, "Install complete.              ");
    return true;
}

bool zero_region(uint32_t handle,
                 uint64_t offset,
                 uint64_t size,
                 uint8_t* scratch,
                 uint64_t scratch_size,
                 long console) {
    memset(scratch, 0, static_cast<size_t>(scratch_size));
    uint64_t done = 0;
    uint64_t total = size;
    uint64_t next_mib = 1024ull * 1024ull;
    while (size > 0) {
        uint64_t chunk = size < scratch_size ? size : scratch_size;
        long written = write_with_retry(handle,
                                        scratch,
                                        static_cast<size_t>(chunk),
                                        offset);
        if (written != static_cast<long>(chunk)) {
            return false;
        }
        offset += chunk;
        size -= chunk;
        done += chunk;
        if (done >= next_mib || size == 0) {
            set_cursor(console, 0, 6);
            userspace::write(console, "Formatting NEUFS... ");
            char size_buf[32];
            format_size(done, size_buf, sizeof(size_buf));
            userspace::write(console, size_buf);
            userspace::write(console, " / ");
            format_size(total, size_buf, sizeof(size_buf));
            userspace::write(console, size_buf);
            userspace::write(console, "       ");
            while (next_mib <= done) {
                next_mib += 1024ull * 1024ull;
            }
        }
    }
    descriptor_write(static_cast<uint32_t>(console), "\n", 1);
    return true;
}

bool write_bytes(uint32_t handle,
                 uint64_t offset,
                 const void* data,
                 size_t size,
                 uint8_t* sector,
                 uint64_t sector_size) {
    const uint8_t* src = static_cast<const uint8_t*>(data);
    while (size > 0) {
        uint64_t sector_offset = (offset / sector_size) * sector_size;
        size_t in_sector = static_cast<size_t>(offset - sector_offset);
        size_t chunk = static_cast<size_t>(sector_size) - in_sector;
        if (chunk > size) {
            chunk = size;
        }
        if (read_with_retry(handle,
                            sector,
                            static_cast<size_t>(sector_size),
                            sector_offset) != static_cast<long>(sector_size)) {
            return false;
        }
        memcpy(sector + in_sector, src, chunk);
        if (write_with_retry(handle,
                             sector,
                             static_cast<size_t>(sector_size),
                             sector_offset) != static_cast<long>(sector_size)) {
            return false;
        }
        offset += chunk;
        src += chunk;
        size -= chunk;
    }
    return true;
}

bool bitmap_set_range(uint8_t* sector,
                      uint32_t handle,
                      uint64_t bitmap_offset,
                      uint64_t start_bit,
                      uint64_t count,
                      uint64_t sector_size) {
    uint64_t end_bit = start_bit + count;
    uint64_t current_bit = start_bit;
    while (current_bit < end_bit) {
        uint64_t byte_offset = bitmap_offset + (current_bit / 8);
        uint64_t sector_offset = (byte_offset / sector_size) * sector_size;
        if (read_with_retry(handle,
                            sector,
                            static_cast<size_t>(sector_size),
                            sector_offset) != static_cast<long>(sector_size)) {
            return false;
        }
        while (current_bit < end_bit) {
            uint64_t current_byte = bitmap_offset + (current_bit / 8);
            if ((current_byte / sector_size) * sector_size != sector_offset) {
                break;
            }
            size_t in_sector = static_cast<size_t>(current_byte - sector_offset);
            uint8_t bit_in_byte = static_cast<uint8_t>(current_bit % 8);
            uint8_t mask = 0;
            while (bit_in_byte < 8 && current_bit < end_bit) {
                mask |= static_cast<uint8_t>(1u << bit_in_byte);
                ++bit_in_byte;
                ++current_bit;
            }
            sector[in_sector] |= mask;
        }
        if (write_with_retry(handle,
                             sector,
                             static_cast<size_t>(sector_size),
                             sector_offset) != static_cast<long>(sector_size)) {
            return false;
        }
    }
    return true;
}

bool format_neufs_device(const Device& dst, long console) {
    if (dst.geom.sector_size == 0 || dst.geom.sector_count == 0 ||
        dst.geom.sector_size > 4096) {
        userspace::write_line(console, "unsupported geometry for NEUFS");
        return false;
    }
    uint64_t total_bytes = dst.geom.sector_size * dst.geom.sector_count;
    uint64_t meta_size =
        default_neufs_meta_size(total_bytes, dst.geom.sector_size);
    if (meta_size >= total_bytes) {
        userspace::write_line(console, "target too small for NEUFS metadata area");
        return false;
    }

    uint64_t data_bitmap_offset = align_up(sizeof(NeufsRvt), 8);
    uint64_t data_bitmap_size = (dst.geom.sector_count + 7) / 8;
    uint64_t meta_blocks = meta_size / 8;
    uint64_t meta_bitmap_offset =
        align_up(data_bitmap_offset + data_bitmap_size, 8);
    uint64_t meta_bitmap_size = (meta_blocks + 7) / 8;
    uint64_t root_offset =
        align_up(meta_bitmap_offset + meta_bitmap_size, dst.geom.sector_size);
    if (root_offset + sizeof(NeufsNdir) > meta_size) {
        userspace::write_line(console, "metadata area cannot fit NEUFS root directory");
        return false;
    }

    long handle = descriptor_open(
        kDescBlock,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(dst.name)),
        0,
        0);
    if (handle < 0) {
        userspace::write_line(console, "failed to open target for formatting");
        return false;
    }

    uint64_t scratch_size = dst.geom.sector_size * 255;
    uint8_t* sector = static_cast<uint8_t*>(
        map_anonymous(static_cast<size_t>(scratch_size), MAP_WRITE));
    if (sector == nullptr) {
        descriptor_close(static_cast<uint32_t>(handle));
        userspace::write_line(console, "failed to allocate format buffer");
        return false;
    }

    bool ok = zero_region(static_cast<uint32_t>(handle),
                          0,
                          meta_size,
                          sector,
                          scratch_size,
                          console);
    if (ok) {
        NeufsRvt rvt{};
        for (size_t i = 0; i < sizeof(kNeufsMagic); ++i) {
            rvt.magic[i] = static_cast<char>(kNeufsMagic[i]);
        }
        rvt.version = kNeufsVersion;
        memcpy(rvt.name, "neutrino", 8);
        rvt.root = root_offset;
        ok = write_bytes(static_cast<uint32_t>(handle),
                         0,
                         &rvt,
                         sizeof(rvt),
                         sector,
                         dst.geom.sector_size);
    }
    if (ok) {
        NeufsNdir root{};
        root.type = kTypeNdir;
        root.name[0] = '/';
        ok = write_bytes(static_cast<uint32_t>(handle),
                         root_offset,
                         &root,
                         sizeof(root),
                         sector,
                         dst.geom.sector_size);
    }
    if (ok) {
        uint64_t used_data_sectors = align_up(meta_size, dst.geom.sector_size) /
                                     dst.geom.sector_size;
        ok = bitmap_set_range(sector,
                              static_cast<uint32_t>(handle),
                              data_bitmap_offset,
                              0,
                              used_data_sectors,
                              dst.geom.sector_size);
    }
    if (ok) {
        uint64_t rvt_blocks = align_up(sizeof(NeufsRvt), 8) / 8;
        uint64_t data_bitmap_blocks = align_up(data_bitmap_size, 8) / 8;
        uint64_t meta_bitmap_blocks = align_up(meta_bitmap_size, 8) / 8;
        uint64_t root_blocks = align_up(sizeof(NeufsNdir), 8) / 8;
        ok = bitmap_set_range(sector,
                              static_cast<uint32_t>(handle),
                              meta_bitmap_offset,
                              0,
                              rvt_blocks,
                              dst.geom.sector_size) &&
             bitmap_set_range(sector,
                              static_cast<uint32_t>(handle),
                              meta_bitmap_offset,
                              data_bitmap_offset / 8,
                              data_bitmap_blocks,
                              dst.geom.sector_size) &&
             bitmap_set_range(sector,
                              static_cast<uint32_t>(handle),
                              meta_bitmap_offset,
                              meta_bitmap_offset / 8,
                              meta_bitmap_blocks,
                              dst.geom.sector_size) &&
             bitmap_set_range(sector,
                              static_cast<uint32_t>(handle),
                              meta_bitmap_offset,
                              root_offset / 8,
                              root_blocks,
                              dst.geom.sector_size);
    }

    unmap(sector, static_cast<size_t>(scratch_size));
    descriptor_close(static_cast<uint32_t>(handle));
    if (!ok) {
        userspace::write_line(console, "NEUFS format failed");
    }
    return ok;
}

bool append_path(const char* base,
                 const char* name,
                 char* out,
                 size_t out_size) {
    if (base == nullptr || name == nullptr || out == nullptr || out_size == 0) {
        return false;
    }
    size_t idx = 0;
    for (size_t i = 0; base[i] != '\0'; ++i) {
        if (idx + 1 >= out_size) {
            return false;
        }
        out[idx++] = base[i];
    }
    if (idx > 0 && out[idx - 1] != '/') {
        if (idx + 1 >= out_size) {
            return false;
        }
        out[idx++] = '/';
    }
    for (size_t i = 0; name[i] != '\0'; ++i) {
        if (idx + 1 >= out_size) {
            return false;
        }
        out[idx++] = name[i];
    }
    out[idx] = '\0';
    return true;
}

bool ends_with(const char* text, const char* suffix) {
    if (text == nullptr || suffix == nullptr) {
        return false;
    }
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > text_len) {
        return false;
    }
    return strcmp(text + text_len - suffix_len, suffix) == 0;
}

bool is_dot_dir_entry(const char* name) {
    return name != nullptr &&
           ((name[0] == '.' && name[1] == '\0') ||
            (name[0] == '.' && name[1] == '.' && name[2] == '\0'));
}

void render_copy_progress(CopyProgress& progress, bool force) {
    uint64_t percent = 0;
    if (progress.total_bytes != 0) {
        percent = (progress.copied_bytes * 100) / progress.total_bytes;
    } else if (progress.total_files != 0) {
        percent = (progress.copied_files * 100) / progress.total_files;
    } else {
        percent = 100;
    }
    if (percent > 100) {
        percent = 100;
    }
    if (!force && percent == progress.last_percent) {
        return;
    }
    progress.last_percent = percent;

    set_cursor(progress.console, 0, 7);
    userspace::write(progress.console, "Copying live system... ");
    userspace::write_u64(progress.console, percent);
    userspace::write(progress.console, "%  files ");
    userspace::write_u64(progress.console, progress.copied_files);
    userspace::write(progress.console, "/");
    userspace::write_u64(progress.console, progress.total_files);
    userspace::write(progress.console, "  bytes ");
    char size_buf[32];
    format_size(progress.copied_bytes, size_buf, sizeof(size_buf));
    userspace::write(progress.console, size_buf);
    userspace::write(progress.console, "/");
    format_size(progress.total_bytes, size_buf, sizeof(size_buf));
    userspace::write(progress.console, size_buf);
    userspace::write(progress.console, "          ");

    set_cursor(progress.console, 0, 8);
    userspace::write(progress.console, "Current: ");
    userspace::write(progress.console, progress.current);
    userspace::write(progress.console,
          "                                                            ");
    yield();
}

void set_copy_status(CopyProgress* progress, const char* action, const char* path) {
    if (progress == nullptr) {
        return;
    }
    if (action == nullptr) {
        action = "";
    }
    if (path == nullptr) {
        path = "";
    }

    size_t idx = 0;
    for (size_t i = 0; action[i] != '\0' && idx + 1 < sizeof(progress->current); ++i) {
        progress->current[idx++] = action[i];
    }
    if (idx + 2 < sizeof(progress->current)) {
        progress->current[idx++] = ':';
        progress->current[idx++] = ' ';
    }
    for (size_t i = 0; path[i] != '\0' && idx + 1 < sizeof(progress->current); ++i) {
        progress->current[idx++] = path[i];
    }
    progress->current[idx] = '\0';
    render_copy_progress(*progress, true);
}

bool scan_tree(const char* source_dir,
               uint64_t& total_files,
               uint64_t& total_bytes,
               long console,
               size_t depth = 0) {
    if (depth > 16) {
        userspace::write_line(console, "copy depth exceeded while scanning");
        return false;
    }
    long dir = directory_open(source_dir);
    if (dir < 0) {
        userspace::write(console, "directory scan failed: ");
        userspace::write_line(console, source_dir);
        return false;
    }

    DirEntry entry{};
    bool ok = true;
    while (directory_read(static_cast<uint32_t>(dir), &entry) > 0) {
        if (entry.name[0] == '\0' || is_dot_dir_entry(entry.name)) {
            continue;
        }
        char source_path[160];
        if (!append_path(source_dir, entry.name, source_path, sizeof(source_path))) {
            ok = false;
            break;
        }
        if (ends_with(source_path, kInstallerPath)) {
            continue;
        }
        if ((entry.flags & DIR_ENTRY_FLAG_DIRECTORY) != 0) {
            if (!scan_tree(source_path,
                           total_files,
                           total_bytes,
                           console,
                           depth + 1)) {
                ok = false;
                break;
            }
        } else {
            ++total_files;
            total_bytes += entry.size;
        }
    }
    directory_close(static_cast<uint32_t>(dir));
    return ok;
}

bool copy_file_path(const char* source,
                    const char* dest,
                    long console,
                    CopyProgress* progress) {
    set_copy_status(progress, "open", source);

    long in = file_open(source);
    if (in < 0) {
        userspace::write(console, "open failed: ");
        userspace::write_line(console, source);
        return false;
    }
    long out = file_create(dest);
    if (out < 0) {
        file_close(static_cast<uint32_t>(in));
        userspace::write(console, "create failed: ");
        userspace::write_line(console, dest);
        return false;
    }
    uint8_t buffer[4096];
    bool ok = true;
    while (true) {
        set_copy_status(progress, "read", source);
        long r = file_read(static_cast<uint32_t>(in), buffer, sizeof(buffer));
        if (r < 0) {
            ok = false;
            break;
        }
        if (r == 0) {
            break;
        }
        size_t written = 0;
        while (written < static_cast<size_t>(r)) {
            set_copy_status(progress, "write", dest);
            long w = file_write(static_cast<uint32_t>(out),
                                buffer + written,
                                static_cast<size_t>(r) - written);
            if (w <= 0) {
                ok = false;
                break;
            }
            written += static_cast<size_t>(w);
        }
        if (!ok) {
            break;
        }
        if (progress != nullptr) {
            progress->copied_bytes += static_cast<uint64_t>(r);
            set_copy_status(progress, "copied", source);
            bool force = progress->copied_bytes >= progress->next_byte_report;
            if (force) {
                constexpr uint64_t mib = 1024ull * 1024ull;
                while (progress->next_byte_report <= progress->copied_bytes) {
                    progress->next_byte_report += mib;
                }
            }
            if (!force) {
                render_copy_progress(*progress, false);
            }
        }
    }
    file_close(static_cast<uint32_t>(in));
    file_close(static_cast<uint32_t>(out));
    if (ok && progress != nullptr) {
        ++progress->copied_files;
        set_copy_status(progress, "done", source);
    }
    return ok;
}

bool write_all(uint32_t file, const char* text) {
    if (text == nullptr) {
        return true;
    }
    size_t len = strlen(text);
    size_t written = 0;
    while (written < len) {
        long w = file_write(file, text + written, len - written);
        if (w <= 0) {
            return false;
        }
        written += static_cast<size_t>(w);
    }
    return true;
}

const char* basename_of(const char* path) {
    if (path == nullptr) {
        return "";
    }
    const char* name = path;
    for (size_t i = 0; path[i] != '\0'; ++i) {
        if (path[i] == '/') {
            name = path + i + 1;
        }
    }
    return name;
}

bool write_installed_module_loads(const char* target_root, long console) {
    char loads_path[160];
    if (!append_path(target_root, "modules/loads.txt", loads_path, sizeof(loads_path))) {
        return false;
    }

    file_remove(loads_path);
    long file = file_create(loads_path);
    if (file < 0) {
        userspace::write_line(console, "warning: unable to write module load list");
        return false;
    }

    bool ok = true;
    long count = module_count();
    if (count < 0) {
        ok = false;
    }
    for (long i = 0; ok && i < count; ++i) {
        ModuleInfo info{};
        if (module_info(static_cast<size_t>(i), &info) != 0 ||
            (info.flags & kModuleInfoDynamic) == 0) {
            continue;
        }
        const char* name = basename_of(info.path[0] != '\0' ? info.path : info.name);
        if (name[0] == '\0') {
            continue;
        }
        ok = write_all(static_cast<uint32_t>(file), name) &&
             write_all(static_cast<uint32_t>(file), "\n");
    }
    if (ok && file_sync(static_cast<uint32_t>(file)) != 0) {
        ok = false;
    }
    file_close(static_cast<uint32_t>(file));
    if (!ok) {
        userspace::write_line(console, "warning: module load list was not refreshed");
    }
    return ok;
}

bool ensure_installed_directory(const char* target_root,
                                const char* relative_path,
                                long console) {
    char path[160];
    if (!append_path(target_root, relative_path, path, sizeof(path))) {
        userspace::write_line(console, "installed directory path too long");
        return false;
    }

    long directory = directory_open(path);
    if (directory >= 0) {
        directory_close(static_cast<uint32_t>(directory));
        return true;
    }
    if (directory_create(path) >= 0) {
        return true;
    }

    userspace::write(console, "failed to create installed directory: ");
    userspace::write_line(console, path);
    return false;
}

bool ensure_installed_root_home(const char* target_root, long console) {
    return ensure_installed_directory(target_root, kUserDirectory, console) &&
           ensure_installed_directory(target_root, kRootHomeDirectory, console);
}

bool write_config_chunk(uint32_t file,
                        const char* text,
                        const char* label,
                        long console) {
    if (write_all(file, text)) {
        return true;
    }
    userspace::write(console, "failed to write installed ESP config ");
    userspace::write_line(console, label);
    return false;
}

bool write_installed_esp_config(const char* esp_name,
                                const char* root_name,
                                long console) {
    if (esp_name == nullptr || root_name == nullptr) {
        return false;
    }

    long esp = descriptor_open(
        kDescBlock,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(esp_name)),
        0,
        0);
    if (esp < 0) {
        userspace::write_line(console, "failed to open installed ESP");
        return false;
    }
    bool mounted = mount_descriptor(static_cast<uint32_t>(esp), nullptr) == 0;
    descriptor_close(static_cast<uint32_t>(esp));
    if (!mounted) {
        userspace::write_line(console, "failed to mount installed ESP");
        return false;
    }

    char config_path[96];
    config_path[0] = '/';
    config_path[1] = '\0';
    strlcpy(config_path + 1, esp_name, sizeof(config_path) - 1);
    size_t len = strlen(config_path);
    if (len + strlen("/limine.conf") + 1 >= sizeof(config_path)) {
        userspace::write_line(console, "installed ESP config path too long");
        return false;
    }
    strlcpy(config_path + len, "/limine.conf", sizeof(config_path) - len);

    long config = file_open(config_path);
    if (config < 0) {
        strlcpy(config_path + len, "/LIMINE.CNF", sizeof(config_path) - len);
        config = file_open(config_path);
    }
    if (config < 0) {
        file_remove(config_path);
        config = file_create(config_path);
    }
    if (config < 0) {
        userspace::write_line(console, "failed to create installed ESP config");
        return false;
    }

    uint32_t config_handle = static_cast<uint32_t>(config);
    bool ok =
        write_config_chunk(config_handle, "timeout: 1\n\n", "timeout", console) &&
        write_config_chunk(config_handle, "/Neutrino\n", "entry", console) &&
        write_config_chunk(config_handle, "    protocol: limine\n", "protocol", console) &&
        write_config_chunk(config_handle,
                           "    path: boot():/boot/kernel.elf\n",
                           "kernel path",
                           console) &&
        write_config_chunk(config_handle, "    cmdline: ROOT=", "cmdline", console) &&
        write_config_chunk(config_handle, root_name, "root device", console) &&
        write_config_chunk(config_handle, "\n", "newline", console);
    if (ok && file_sync(config_handle) != 0) {
        userspace::write_line(console,
                              "warning: installed ESP config sync reported an error");
    }
    file_close(config_handle);
    if (!ok) {
        return false;
    }
    return true;
}

bool copy_tree(const char* source_dir,
               const char* dest_dir,
               long console,
               CopyProgress* progress,
               size_t depth = 0) {
    if (depth > 16) {
        userspace::write_line(console, "copy depth exceeded");
        return false;
    }
    long dir = directory_open(source_dir);
    if (dir < 0) {
        userspace::write(console, "directory open failed: ");
        userspace::write_line(console, source_dir);
        return false;
    }
    if (depth != 0) {
        set_copy_status(progress, "mkdir", dest_dir);
        if (directory_create(dest_dir) < 0) {
            descriptor_close(static_cast<uint32_t>(dir));
            userspace::write(console, "directory create failed: ");
            userspace::write_line(console, dest_dir);
            return false;
        }
    }

    DirEntry entry{};
    bool ok = true;
    while (directory_read(static_cast<uint32_t>(dir), &entry) > 0) {
        if (entry.name[0] == '\0' || is_dot_dir_entry(entry.name)) {
            continue;
        }
        char source_path[160];
        char dest_path[160];
        if (!append_path(source_dir, entry.name, source_path, sizeof(source_path)) ||
            !append_path(dest_dir, entry.name, dest_path, sizeof(dest_path))) {
            ok = false;
            break;
        }
        if (ends_with(source_path, kInstallerPath)) {
            continue;
        }
        if ((entry.flags & DIR_ENTRY_FLAG_DIRECTORY) != 0) {
            set_copy_status(progress, "enter", source_path);
            if (!copy_tree(source_path,
                           dest_path,
                           console,
                           progress,
                           depth + 1)) {
                ok = false;
                break;
            }
        } else if (!copy_file_path(source_path, dest_path, console, progress)) {
            ok = false;
            break;
        }
    }
    directory_close(static_cast<uint32_t>(dir));
    return ok;
}

bool protect_installed_user_store(const char* target_root, long console) {
    void* root_user = user_find("root");
    UserInfo root_info{};
    if (root_user == nullptr || user_info(root_user, &root_info) != 0 ||
        root_info.id_local == 0) {
        userspace::write_line(console,
                              "failed to resolve root user for credential ACL");
        return false;
    }

    FileAclEntry acl{};
    acl.machine_id = root_info.id_machine;
    acl.local_id = root_info.id_local;
    acl.write = AclValue::Allow;
    acl.read = AclValue::Allow;
    acl.delete_permission = AclValue::Allow;
    acl.edit = AclValue::Allow;

    const char* candidates[] = {"/system/users.ntd", "/users.ntd"};
    bool found = false;
    for (const char* suffix : candidates) {
        char path[160];
        if (!append_path(target_root, suffix + 1, path, sizeof(path))) {
            return false;
        }
        long handle = file_open(path);
        if (handle < 0) {
            continue;
        }
        file_close(static_cast<uint32_t>(handle));
        found = true;
        if (file_set_acl(path, &acl, 1) != 0) {
            userspace::write(console, "failed to protect credential store: ");
            userspace::write_line(console, path);
            return false;
        }
    }
    if (!found) {
        userspace::write_line(console,
                              "installed credential store was not found");
    }
    return found;
}

bool install_neufs(const Device& src, const Device& dst, long console) {
    clear_console(console);
    set_cursor(console, 0, 0);
    userspace::write_line(console, "Neutrino Installer");
    userspace::write_line(console, "Installing to UEFI GPT + NEUFS target.");
    userspace::write(console, "Target: ");
    userspace::write_line(console, dst.name);

    char esp_name[64];
    char root_name[64];
    if (is_partition_device_name(dst.name) ||
        !append_partition_suffix(dst.name, 0, esp_name, sizeof(esp_name)) ||
        !append_partition_suffix(dst.name, 1, root_name, sizeof(root_name))) {
        userspace::write_line(console, "target name is not a whole-disk install target");
        return false;
    }

    long esp = descriptor_open(
        kDescBlock,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(kEspModuleDevice)),
        0,
        0);
    if (esp < 0) {
        userspace::write_line(console, "missing installer ESP image module");
        return false;
    }
    descriptor_defs::BlockGeometry esp_geom{};
    if (descriptor_get_property(
            static_cast<uint32_t>(esp),
            static_cast<uint32_t>(descriptor_defs::Property::BlockGeometry),
            &esp_geom,
            sizeof(esp_geom)) != 0 ||
        esp_geom.sector_size == 0 || esp_geom.sector_count == 0) {
        descriptor_close(static_cast<uint32_t>(esp));
        userspace::write_line(console, "invalid installer ESP image geometry");
        return false;
    }

    long disk = descriptor_open(
        kDescBlock,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(dst.name)),
        0,
        0);
    if (disk < 0) {
        descriptor_close(static_cast<uint32_t>(esp));
        userspace::write_line(console, "failed to open target disk");
        return false;
    }

    uint64_t scratch_size = dst.geom.sector_size * 255;
    if (scratch_size < kGptEntryCount * kGptEntrySize) {
        scratch_size = kGptEntryCount * kGptEntrySize;
    }
    uint8_t* scratch = static_cast<uint8_t*>(
        map_anonymous(static_cast<size_t>(scratch_size), MAP_WRITE));
    if (scratch == nullptr) {
        descriptor_close(static_cast<uint32_t>(disk));
        descriptor_close(static_cast<uint32_t>(esp));
        userspace::write_line(console, "failed to allocate install buffer");
        return false;
    }

    GptLayout layout{};
    set_cursor(console, 0, 6);
    userspace::write_line(console, "Writing GPT...");
    bool ok = write_gpt(static_cast<uint32_t>(disk),
                        dst.geom.sector_count,
                        esp_geom.sector_count,
                        layout,
                        scratch,
                        scratch_size,
                        console);
    if (ok) {
        set_cursor(console, 0, 6);
        userspace::write_line(console, "Writing EFI system partition...");
        ok = copy_block_device(static_cast<uint32_t>(esp),
                               static_cast<uint32_t>(disk),
                               0,
                               layout.esp_first_lba * dst.geom.sector_size,
                               esp_geom.sector_count * esp_geom.sector_size,
                               scratch,
                               scratch_size);
    }

    unmap(scratch, static_cast<size_t>(scratch_size));
    descriptor_close(static_cast<uint32_t>(disk));
    descriptor_close(static_cast<uint32_t>(esp));
    if (!ok) {
        userspace::write_line(console, "failed to install GPT/ESP");
        return false;
    }

    if (rescan_block_devices() != 0) {
        userspace::write_line(console, "failed to rescan GPT partitions");
        return false;
    }
    if (!write_installed_esp_config(esp_name, root_name, console)) {
        return false;
    }

    Device root{};
    strlcpy(root.name, root_name, sizeof(root.name));
    root.geom.sector_size = dst.geom.sector_size;
    root.geom.sector_count = layout.root_last_lba - layout.root_first_lba + 1;
    root.writable = true;
    root.is_memdisk = false;

    if (!format_neufs_device(root, console)) {
        return false;
    }
    long target = descriptor_open(
        kDescBlock,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(root.name)),
        0,
        0);
    if (target < 0) {
        userspace::write_line(console, "failed to open formatted target");
        return false;
    }
    bool mounted = mount_descriptor(static_cast<uint32_t>(target), nullptr) == 0;
    descriptor_close(static_cast<uint32_t>(target));
    if (!mounted) {
        userspace::write_line(console, "failed to mount formatted NEUFS target");
        return false;
    }
    char source_root[80];
    char target_root[80];
    source_root[0] = '/';
    source_root[1] = '\0';
    strlcpy(source_root + 1, src.name, sizeof(source_root) - 1);
    target_root[0] = '/';
    target_root[1] = '\0';
    strlcpy(target_root + 1, root.name, sizeof(target_root) - 1);

    userspace::write_line(console, "Scanning live system...");
    uint64_t total_files = 0;
    uint64_t total_bytes = 0;
    if (!scan_tree(source_root, total_files, total_bytes, console)) {
        return false;
    }

    CopyProgress progress{};
    progress.console = console;
    progress.total_files = total_files;
    progress.total_bytes = total_bytes;
    progress.last_percent = UINT64_MAX;
    progress.next_byte_report = 1024ull * 1024ull;
    strlcpy(progress.current, source_root, sizeof(progress.current));
    render_copy_progress(progress, true);

    ok = copy_tree(source_root, target_root, console, &progress) &&
         ensure_installed_root_home(target_root, console) &&
         protect_installed_user_store(target_root, console);
    if (ok) {
        (void)write_installed_module_loads(target_root, console);
        progress.copied_files = progress.total_files;
        if (progress.total_bytes > progress.copied_bytes) {
            progress.copied_bytes = progress.total_bytes;
        }
        strlcpy(progress.current, "complete", sizeof(progress.current));
        render_copy_progress(progress, true);
        set_cursor(console, 0, 10);
        userspace::write_line(console, "NEUFS install complete.");
    }
    return ok;
}

bool finalize_cloned_target(const Device& dst, long console) {
    long target = descriptor_open(
        kDescBlock,
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(dst.name)),
        0,
        0);
    if (target < 0) {
        return false;
    }
    bool mounted = mount_descriptor(static_cast<uint32_t>(target), nullptr) == 0;
    descriptor_close(static_cast<uint32_t>(target));
    if (!mounted) {
        return false;
    }

    char target_root[96];
    target_root[0] = '/';
    target_root[1] = '\0';
    strlcpy(target_root + 1, dst.name, sizeof(target_root) - 1);

    char user_store_path[160];
    if (!append_path(target_root,
                     "system/users.ntd",
                     user_store_path,
                     sizeof(user_store_path))) {
        return false;
    }
    if (file_remove(user_store_path) != 0 ||
        !copy_file_path("/system/users.ntd",
                        user_store_path,
                        console,
                        nullptr)) {
        userspace::write_line(
            console,
            "Failed to provision credentials on cloned target.");
        return false;
    }

    char installer_path[160];
    if (!append_path(target_root,
                     kInstallerPath,
                     installer_path,
                     sizeof(installer_path))) {
        return false;
    }
    return file_remove(installer_path) == 0;
}

InstallFs choose_filesystem(uint32_t keyboard, long console) {
    userspace::write_line(console, "");
    userspace::write_line(console, "Choose target filesystem:");
    userspace::write_line(console, "  1. NEUFS (recommended)");
    userspace::write_line(console, "  2. FAT32 (legacy image clone, discouraged)");
    userspace::write(console, "Selection [1]: ");
    char input[8];
    drain_keyboard(keyboard);
    size_t len = read_line(keyboard, console, input, sizeof(input));
    if (len == 1 && input[0] == '2') {
        return InstallFs::Fat32;
    }
    return InstallFs::Neufs;
}

}  // namespace

int main(uint64_t, uint64_t) {
    long console = descriptor_open(kDescConsole, 0);
    long keyboard = descriptor_open(kDescKeyboard, 0);
    if (console < 0 || keyboard < 0) {
        return 1;
    }

    clear_console(console);
    set_console_color(console, kDefaultFg, kDefaultBg);
    set_kernel_log_console(console, false);
    userspace::write_line(console, "Neutrino Installer");
    userspace::write_line(console, "Live installer for MEMDISK boot sessions.");

    Device devices[16];
    size_t device_count = 0;
    if (!fetch_devices(devices, 16, device_count) || device_count == 0) {
        userspace::write_line(console, "No block devices detected.");
        return 1;
    }

    size_t source_index = device_count;
    for (size_t i = 0; i < device_count; ++i) {
        if (devices[i].is_memdisk) {
            source_index = i;
            break;
        }
    }
    if (source_index == device_count) {
        userspace::write_line(console, "Installer is only available from a live MEMDISK root.");
        userspace::write_line(console, "This system appears to be running from an installed disk.");
        return 1;
    }

    Device targets[16];
    size_t target_count =
        collect_install_targets(devices, device_count, targets, 16);
    if (target_count == 0) {
        userspace::write_line(console, "No installable whole disks detected.");
        return 1;
    }

    while (true) {
        set_cursor(console, 0, 3);
        userspace::write_line(console, "Available drives:");
        render_table(console, targets, target_count, target_count, 4);

        userspace::write_line(console, "");
        userspace::write(console, "Enter target index (or q to quit): ");
        char input[8];
        size_t len = read_line(static_cast<uint32_t>(keyboard),
                               console,
                               input,
                               sizeof(input));
        if (len == 1 && (input[0] == 'q' || input[0] == 'Q')) {
            return 0;
        }
        size_t choice = 0;
        if (!parse_uint(input, choice) || choice >= target_count) {
            userspace::write_line(console, "Invalid selection.");
            continue;
        }
        if (!targets[choice].writable) {
            userspace::write_line(console, "Drive is read-only.");
            continue;
        }

        bool proceed = prompt_yes_no_line(static_cast<uint32_t>(keyboard),
                                          console,
                                          "This will erase the drive. Proceed");
        if (!proceed) {
            userspace::write_line(console, "Cancelled.");
            continue;
        }

        InstallFs fs = choose_filesystem(static_cast<uint32_t>(keyboard),
                                         console);
        userspace::write_line(console, "");
        if (!provision_root_password(static_cast<uint32_t>(keyboard),
                                     console)) {
            userspace::write_line(
                console,
                "Installation cancelled: root password was not set.");
            continue;
        }
        bool ok = false;
        if (fs == InstallFs::Neufs) {
            ok = install_neufs(devices[source_index], targets[choice], console);
        } else {
            clear_console(console);
            set_cursor(console, 0, 0);
            userspace::write_line(console, "Neutrino Installer");
            userspace::write_line(console, "Installing FAT32 by cloning the live image...");
            ok = copy_image(devices[source_index].name,
                            devices[source_index],
                            targets[choice].name,
                            targets[choice],
                            console);
            if (ok) {
                ok = finalize_cloned_target(targets[choice], console);
            }
        }
        if (!ok) {
            userspace::write_line(console, "Install failed.");
        }

        bool again = prompt_yes_no_line(static_cast<uint32_t>(keyboard),
                                        console,
                                        "Install another drive");
        if (!again) {
            break;
        }
    }

    return 0;
}
