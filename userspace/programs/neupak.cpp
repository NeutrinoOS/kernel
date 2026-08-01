#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <neutrino.h>

#include "../auth/password_hash.hpp"
#include "../crt/syscall.hpp"
#include "../helpers/args.hpp"
#include "../helpers/text.hpp"

namespace {

constexpr size_t kMaxText = 256 * 1024;
constexpr size_t kMaxPath = 256;
constexpr size_t kMaxName = 64;
constexpr size_t kMaxUrl = 512;
constexpr size_t kMaxVersion = 32;
constexpr size_t kMaxDescription = 160;
constexpr size_t kMaxRepos = 12;
constexpr size_t kMaxPackages = 96;
constexpr size_t kMaxDeps = 16;
constexpr size_t kMaxFiles = 256;
constexpr size_t kMaxInstallQueue = 96;
constexpr size_t kCopyBufferSize = 4096;
constexpr uint64_t kLockStaleSeconds = 30ull;

constexpr const char* kSysrootPrefix = "...";
constexpr const char* kConfigDir = ".../config/neupak";
constexpr const char* kCacheDir = ".../config/neupak/cache";
constexpr const char* kRepoCacheDir = ".../config/neupak/cache/repos";
constexpr const char* kPackageCacheDir = ".../config/neupak/cache/packages";
constexpr const char* kExtractDir = ".../config/neupak/cache/extract";
constexpr const char* kManifestCachePath = ".../config/neupak/cache/manifest.tmp";
constexpr const char* kReposPath = ".../config/neupak/repos.cfg";
constexpr const char* kInstalledPath = ".../config/neupak/install.db";
constexpr const char* kFilesPath = ".../config/neupak/files.db";
constexpr const char* kLockPath = ".../config/neupak/db.lck";
constexpr const char* kDownloadPath = ".../binary/download.elf";

long g_console = -1;
long g_lock = -1;
uint8_t g_copy_buffer[kCopyBufferSize];

struct StringList {
    char values[kMaxDeps][kMaxName];
    size_t count = 0;
};

struct Repo {
    char name[kMaxName];
    char indexurl[kMaxUrl];
};

struct Package {
    char name[kMaxName];
    char package[kMaxName];
    char package_url[kMaxUrl];
    char version[kMaxVersion];
    char sha256[65];
    uint64_t size = 0;
    char strategy[32];
    char description[kMaxDescription];
    StringList depends;
    bool present = false;
};

struct PackageSet {
    Package packages[kMaxPackages];
    size_t count = 0;
};

struct InstalledPackage {
    char name[kMaxName];
    char version[kMaxVersion];
};

struct InstalledDb {
    InstalledPackage packages[kMaxPackages];
    size_t count = 0;
};

struct FileOwner {
    char path[kMaxPath];
};

struct PackageFiles {
    char name[kMaxName];
    FileOwner files[kMaxFiles];
    size_t file_count = 0;
};

struct FilesDb {
    PackageFiles packages[kMaxPackages];
    size_t count = 0;
};

struct Args {
    char words[8][kMaxPath];
    size_t count = 0;
};

struct ZipEntry {
    char archive_path[kMaxPath];
    char final_path[kMaxPath];
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
    uint16_t method = 0;
    bool is_dir = false;
};

struct ZipPlan {
    ZipEntry entries[kMaxFiles];
    size_t count = 0;
};

Repo g_repos[kMaxRepos];
PackageSet g_index;
PackageSet g_repo_set;
PackageSet g_check_set;
PackageSet g_manifest_set;
InstalledDb g_installed;
FilesDb g_files;
ZipPlan g_plan;
char g_install_queue[kMaxInstallQueue][kMaxName];

void print(const char* text) {
    if (g_console < 0) {
        g_console = neutrino_open_stdout();
    }
    neutrino_write(g_console, text);
}

void print_line(const char* text) {
    if (g_console < 0) {
        g_console = neutrino_open_stdout();
    }
    neutrino_write_line(g_console, text);
}

void print_u64(uint64_t value) {
    char digits[24];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10u));
        value /= 10u;
    } while (value != 0 && count < sizeof(digits));
    while (count != 0) {
        char ch = digits[--count];
        descriptor_write(static_cast<uint32_t>(g_console), &ch, 1);
    }
}

bool append_u64(char* out, size_t out_size, size_t& len, uint64_t value) {
    char digits[24];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10u));
        value /= 10u;
    } while (value != 0 && count < sizeof(digits));
    while (count != 0) {
        if (!userspace::text::append_char(out, out_size, len, digits[--count])) {
            return false;
        }
    }
    return true;
}

bool sysroot_path_for_logical(const char* logical_path, char* out, size_t out_size) {
    if (logical_path == nullptr || logical_path[0] != '/') {
        return false;
    }
    size_t len = 0;
    out[0] = '\0';
    return userspace::text::append_text(out, out_size, len, kSysrootPrefix) &&
           userspace::text::append_text(out, out_size, len, logical_path);
}

bool parse_args(const char* raw, Args& args) {
    const char* cursor = userspace::skip_spaces(raw);
    while (cursor != nullptr && *cursor != '\0') {
        if (args.count >= 8) {
            return false;
        }
        size_t len = 0;
        while (cursor[len] != '\0' && !userspace::is_space(cursor[len])) {
            if (len + 1 >= kMaxPath) {
                return false;
            }
            args.words[args.count][len] = cursor[len];
            ++len;
        }
        args.words[args.count][len] = '\0';
        ++args.count;
        cursor = userspace::skip_spaces(cursor + len);
    }
    return true;
}

bool ensure_dir(const char* path) {
    long dir = directory_open(path);
    if (dir >= 0) {
        directory_close(static_cast<uint32_t>(dir));
        return true;
    }
    return directory_create(path) == 0;
}

bool ensure_dir_tree(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    char partial[kMaxPath];
    size_t len = 0;
    size_t i = 0;
    if (userspace::text::starts_with(path, kSysrootPrefix) && path[3] == '/') {
        if (!userspace::text::append_text(partial, sizeof(partial), len, kSysrootPrefix)) {
            return false;
        }
        i = 3;
    } else if (path[0] == '/') {
        partial[len++] = '/';
        partial[len] = '\0';
        i = 1;
    } else {
        return false;
    }
    for (; path[i] != '\0'; ++i) {
        if (!userspace::text::append_char(partial, sizeof(partial), len, path[i])) {
            return false;
        }
        if (path[i] == '/') {
            if (len > 1 && strcmp(partial, ".../") != 0) {
                partial[len - 1] = '\0';
                if (!ensure_dir(partial)) {
                    return false;
                }
                partial[len - 1] = '/';
            }
        }
    }
    if (len > 1 && partial[len - 1] != '/') {
        return ensure_dir(partial);
    }
    return true;
}

bool ensure_parent_dir(const char* path) {
    char parent[kMaxPath];
    strlcpy(parent, path, sizeof(parent));
    size_t len = strlen(parent);
    while (len > 0 && parent[len - 1] != '/') {
        --len;
    }
    if (len <= 1 || (len == 4 && strncmp(parent, ".../", 4) == 0)) {
        return true;
    }
    parent[len - 1] = '\0';
    return ensure_dir_tree(parent);
}

bool prepare_dirs() {
    return ensure_dir_tree(kConfigDir) &&
           ensure_dir_tree(kCacheDir) &&
           ensure_dir_tree(kRepoCacheDir) &&
           ensure_dir_tree(kPackageCacheDir) &&
           ensure_dir_tree(kExtractDir);
}

bool read_file_limited(const char* path, char*& out, size_t& out_len) {
    out = nullptr;
    out_len = 0;
    long file = file_open(path);
    if (file < 0) {
        return false;
    }
    char* buffer = static_cast<char*>(map_anonymous(kMaxText + 1, MAP_WRITE));
    if (buffer == nullptr) {
        file_close(static_cast<uint32_t>(file));
        return false;
    }
    size_t used = 0;
    while (true) {
        if (used >= kMaxText) {
            file_close(static_cast<uint32_t>(file));
            unmap(buffer, kMaxText + 1);
            return false;
        }
        long got = file_read(static_cast<uint32_t>(file), buffer + used, kMaxText - used);
        if (got < 0) {
            file_close(static_cast<uint32_t>(file));
            unmap(buffer, kMaxText + 1);
            return false;
        }
        if (got == 0) {
            break;
        }
        used += static_cast<size_t>(got);
    }
    file_close(static_cast<uint32_t>(file));
    buffer[used] = '\0';
    out = buffer;
    out_len = used;
    return true;
}

bool write_all(uint32_t file, const void* data, size_t len) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t written = 0;
    while (written < len) {
        long got = file_write(file, bytes + written, len - written);
        if (got <= 0) {
            return false;
        }
        written += static_cast<size_t>(got);
    }
    return true;
}

bool replace_file(const char* path, const char* text) {
    ensure_parent_dir(path);
    long existing = file_open(path);
    if (existing >= 0) {
        file_close(static_cast<uint32_t>(existing));
        if (file_remove(path) < 0) {
            return false;
        }
    }
    long file = file_create(path);
    if (file < 0) {
        return false;
    }
    bool ok = write_all(static_cast<uint32_t>(file), text, strlen(text));
    file_close(static_cast<uint32_t>(file));
    if (!ok) {
        file_remove(path);
    }
    return ok;
}

bool parse_section_header(char* line, char* out, size_t out_size) {
    char* cursor = line;
    while (userspace::is_space(*cursor)) {
        ++cursor;
    }
    if (*cursor != '[') {
        return false;
    }
    ++cursor;
    bool quoted = false;
    if (*cursor == '"') {
        quoted = true;
        ++cursor;
    }
    size_t len = 0;
    while (*cursor != '\0' && ((quoted && *cursor != '"') ||
                               (!quoted && *cursor != ']'))) {
        if (len + 1 >= out_size) {
            return false;
        }
        out[len++] = *cursor++;
    }
    if (quoted) {
        if (*cursor != '"') {
            return false;
        }
        ++cursor;
        while (userspace::is_space(*cursor)) {
            ++cursor;
        }
    }
    if (*cursor != ']') {
        return false;
    }
    out[len] = '\0';
    return len != 0;
}

void strip_comment(char* line) {
    bool quoted = false;
    for (size_t i = 0; line[i] != '\0'; ++i) {
        if (line[i] == '"') {
            quoted = !quoted;
        } else if (!quoted && line[i] == '#') {
            line[i] = '\0';
            return;
        }
    }
}

char* trim(char* line) {
    while (userspace::is_space(*line)) {
        ++line;
    }
    size_t len = strlen(line);
    while (len != 0 && userspace::is_space(line[len - 1])) {
        line[--len] = '\0';
    }
    return line;
}

bool split_key_value(char* line, char*& key, char*& value) {
    char* equals = line;
    while (*equals != '\0' && *equals != '=') {
        ++equals;
    }
    if (*equals != '=') {
        return false;
    }
    *equals = '\0';
    key = trim(line);
    value = trim(equals + 1);
    return key[0] != '\0';
}

bool parse_quoted(const char* value, char* out, size_t out_size) {
    value = userspace::skip_spaces(value);
    if (value == nullptr || *value != '"') {
        return false;
    }
    ++value;
    size_t len = 0;
    while (*value != '\0' && *value != '"') {
        if (len + 1 >= out_size) {
            return false;
        }
        out[len++] = *value++;
    }
    if (*value != '"') {
        return false;
    }
    out[len] = '\0';
    return true;
}

bool parse_u64(const char* value, uint64_t& out) {
    value = userspace::skip_spaces(value);
    if (value == nullptr || *value == '\0') {
        return false;
    }
    uint64_t result = 0;
    while (*value >= '0' && *value <= '9') {
        result = result * 10u + static_cast<uint64_t>(*value - '0');
        ++value;
    }
    value = userspace::skip_spaces(value);
    if (value == nullptr || *value != '\0') {
        return false;
    }
    out = result;
    return true;
}

bool valid_name(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    for (size_t i = 0; name[i] != '\0'; ++i) {
        char ch = name[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
                  ch == '+' || ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool valid_semver(const char* version) {
    int dots = 0;
    size_t i = 0;
    for (int part = 0; part < 3; ++part) {
        if (version[i] < '0' || version[i] > '9') {
            return false;
        }
        while (version[i] >= '0' && version[i] <= '9') {
            ++i;
        }
        if (part != 2) {
            if (version[i] != '.') {
                return false;
            }
            ++dots;
            ++i;
        }
    }
    if (version[i] == '\0') {
        return dots == 2;
    }
    if (version[i] != '-' && version[i] != '+') {
        return false;
    }
    for (; version[i] != '\0'; ++i) {
        char ch = version[i];
        bool ok = (ch >= '0' && ch <= '9') ||
                  (ch >= 'A' && ch <= 'Z') ||
                  (ch >= 'a' && ch <= 'z') ||
                  ch == '-' || ch == '+' || ch == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool valid_sha256_hex(const char* text) {
    if (strlen(text) != 64) {
        return false;
    }
    for (size_t i = 0; text[i] != '\0'; ++i) {
        char ch = text[i];
        bool ok = (ch >= '0' && ch <= '9') ||
                  (ch >= 'a' && ch <= 'f') ||
                  (ch >= 'A' && ch <= 'F');
        if (!ok) {
            return false;
        }
    }
    return true;
}

char ascii_lower(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

bool contains_case_insensitive(const char* text, const char* needle) {
    if (text == nullptr || needle == nullptr) {
        return false;
    }
    if (needle[0] == '\0') {
        return true;
    }
    for (size_t i = 0; text[i] != '\0'; ++i) {
        size_t j = 0;
        while (needle[j] != '\0' && text[i + j] != '\0' &&
               ascii_lower(text[i + j]) == ascii_lower(needle[j])) {
            ++j;
        }
        if (needle[j] == '\0') {
            return true;
        }
    }
    return false;
}

bool parse_string_list_line(const char* value, StringList& out) {
    out.count = 0;
    value = userspace::skip_spaces(value);
    if (value == nullptr || *value != '[') {
        return false;
    }
    ++value;
    while (true) {
        value = userspace::skip_spaces(value);
        if (*value == ']') {
            return true;
        }
        if (*value != '"') {
            return false;
        }
        ++value;
        if (out.count >= kMaxDeps) {
            return false;
        }
        size_t len = 0;
        while (*value != '\0' && *value != '"') {
            if (len + 1 >= kMaxName) {
                return false;
            }
            out.values[out.count][len++] = *value++;
        }
        if (*value != '"') {
            return false;
        }
        out.values[out.count][len] = '\0';
        ++out.count;
        ++value;
        value = userspace::skip_spaces(value);
        if (*value == ',') {
            ++value;
            continue;
        }
        if (*value == ']') {
            return true;
        }
        return false;
    }
}

bool contains_char(const char* text, char needle) {
    while (text != nullptr && *text != '\0') {
        if (*text++ == needle) {
            return true;
        }
    }
    return false;
}

bool collect_array_value(char* value,
                         char*& next_line,
                         const char* text_end,
                         char* out,
                         size_t out_size) {
    size_t len = 0;
    out[0] = '\0';
    if (!userspace::text::append_text(out, out_size, len, value)) {
        return false;
    }
    while (!contains_char(out, ']')) {
        if (next_line >= text_end) {
            return false;
        }
        char* line = next_line;
        char* next = line;
        while (next < text_end && *next != '\n') {
            ++next;
        }
        if (next < text_end) {
            *next++ = '\0';
        }
        strip_comment(line);
        char* cleaned = trim(line);
        if (!userspace::text::append_char(out, out_size, len, ' ') ||
            !userspace::text::append_text(out, out_size, len, cleaned)) {
            return false;
        }
        next_line = next;
    }
    return true;
}

bool parse_file_array(const char* value, PackageFiles& out) {
    out.file_count = 0;
    value = userspace::skip_spaces(value);
    if (value == nullptr || *value != '[') {
        return false;
    }
    ++value;
    while (true) {
        value = userspace::skip_spaces(value);
        if (*value == ']') {
            return true;
        }
        if (*value == ',') {
            ++value;
            continue;
        }
        if (*value != '"') {
            return false;
        }
        ++value;
        if (out.file_count >= kMaxFiles) {
            return false;
        }
        size_t out_len = 0;
        while (*value != '\0' && *value != '"') {
            if (out_len + 1 >= kMaxPath) {
                return false;
            }
            out.files[out.file_count].path[out_len++] = *value++;
        }
        if (*value != '"') {
            return false;
        }
        out.files[out.file_count].path[out_len] = '\0';
        ++out.file_count;
        ++value;
    }
}

Repo* add_repo(Repo* repos, size_t& repo_count, const char* name) {
    if (repo_count >= kMaxRepos || !valid_name(name)) {
        return nullptr;
    }
    for (size_t i = 0; i < repo_count; ++i) {
        if (strcmp(repos[i].name, name) == 0) {
            return nullptr;
        }
    }
    Repo* repo = &repos[repo_count++];
    memset(repo, 0, sizeof(*repo));
    strlcpy(repo->name, name, sizeof(repo->name));
    return repo;
}

bool parse_repos_cfg(Repo* repos, size_t& repo_count) {
    repo_count = 0;
    char* text = nullptr;
    size_t len = 0;
    if (!read_file_limited(kReposPath, text, len)) {
        print_line("neupak: missing .../config/neupak/repos.cfg");
        return false;
    }
    Repo* current = nullptr;
    char* line = text;
    while (line < text + len) {
        char* next = line;
        while (next < text + len && *next != '\n') {
            ++next;
        }
        if (next < text + len) {
            *next++ = '\0';
        }
        strip_comment(line);
        char* cleaned = trim(line);
        if (cleaned[0] == '\0') {
            line = next;
            continue;
        }
        char section[kMaxName];
        if (parse_section_header(cleaned, section, sizeof(section))) {
            current = add_repo(repos, repo_count, section);
            if (current == nullptr) {
                unmap(text, kMaxText + 1);
                print_line("neupak: invalid or duplicate repo section");
                return false;
            }
        } else if (current != nullptr) {
            char* key = nullptr;
            char* value = nullptr;
            if (split_key_value(cleaned, key, value) && strcmp(key, "indexurl") == 0) {
                if (!parse_quoted(value, current->indexurl, sizeof(current->indexurl))) {
                    unmap(text, kMaxText + 1);
                    print_line("neupak: invalid repo indexurl");
                    return false;
                }
            }
        }
        line = next;
    }
    unmap(text, kMaxText + 1);
    for (size_t i = 0; i < repo_count; ++i) {
        if (repos[i].indexurl[0] == '\0') {
            print_line("neupak: repo missing indexurl");
            return false;
        }
    }
    return repo_count != 0;
}

Package* find_package(PackageSet& set, const char* name) {
    for (size_t i = 0; i < set.count; ++i) {
        if (strcmp(set.packages[i].name, name) == 0) {
            return &set.packages[i];
        }
    }
    return nullptr;
}

Package* add_package(PackageSet& set, const char* name) {
    if (set.count >= kMaxPackages || !valid_name(name)) {
        return nullptr;
    }
    if (find_package(set, name) != nullptr) {
        return nullptr;
    }
    Package* pkg = &set.packages[set.count++];
    memset(pkg, 0, sizeof(*pkg));
    pkg->present = true;
    strlcpy(pkg->name, name, sizeof(pkg->name));
    return pkg;
}

bool validate_package(const Package& pkg, bool require_hash) {
    if (!valid_name(pkg.name)) {
        print("neupak: invalid package name ");
        print_line(pkg.name);
        return false;
    }
    if (!valid_semver(pkg.version)) {
        print("neupak: invalid semver for ");
        print(pkg.name);
        print(": ");
        print_line(pkg.version);
        return false;
    }
    if (require_hash && !valid_name(pkg.package)) {
        print("neupak: invalid package filename for ");
        print_line(pkg.name);
        return false;
    }
    if (strcmp(pkg.strategy, "over_root") != 0) {
        print("neupak: unsupported strategy for ");
        print(pkg.name);
        print(": ");
        print_line(pkg.strategy);
        return false;
    }
    if (require_hash && !valid_sha256_hex(pkg.sha256)) {
        print("neupak: invalid sha256 for ");
        print_line(pkg.name);
        return false;
    }
    for (size_t i = 0; i < pkg.depends.count; ++i) {
        if (!valid_name(pkg.depends.values[i])) {
            print("neupak: invalid dependency for ");
            print(pkg.name);
            print(": ");
            print_line(pkg.depends.values[i]);
            return false;
        }
    }
    return true;
}

bool same_deps(const StringList& a, const StringList& b) {
    if (a.count != b.count) {
        return false;
    }
    for (size_t i = 0; i < a.count; ++i) {
        if (strcmp(a.values[i], b.values[i]) != 0) {
            return false;
        }
    }
    return true;
}

bool same_manifest_package(const Package& index_pkg, const Package& manifest_pkg) {
    if (strcmp(index_pkg.version, manifest_pkg.version) != 0) {
        print("neupak: manifest version mismatch: ");
        print(manifest_pkg.version);
        print(" != ");
        print_line(index_pkg.version);
        return false;
    }
    if (strcmp(index_pkg.strategy, manifest_pkg.strategy) != 0) {
        print("neupak: manifest strategy mismatch: ");
        print(manifest_pkg.strategy);
        print(" != ");
        print_line(index_pkg.strategy);
        return false;
    }
    if (strcmp(index_pkg.description, manifest_pkg.description) != 0) {
        print_line("neupak: manifest description mismatch");
        return false;
    }
    if (!same_deps(index_pkg.depends, manifest_pkg.depends)) {
        print_line("neupak: manifest dependency list mismatch");
        return false;
    }
    return true;
}

bool parse_package_toml(const char* path, PackageSet& set, bool require_hash) {
    char* text = nullptr;
    size_t len = 0;
    if (!read_file_limited(path, text, len)) {
        return false;
    }
    Package* current = nullptr;
    char* line = text;
    size_t line_number = 0;
    while (line < text + len) {
        ++line_number;
        char* next = line;
        while (next < text + len && *next != '\n') {
            ++next;
        }
        if (next < text + len) {
            *next++ = '\0';
        }
        strip_comment(line);
        char* cleaned = trim(line);
        if (cleaned[0] == '\0') {
            line = next;
            continue;
        }
        char section[kMaxName];
        if (parse_section_header(cleaned, section, sizeof(section))) {
            current = add_package(set, section);
            if (current == nullptr) {
                print("neupak: bad or duplicate package section at line ");
                print_u64(line_number);
                print(": ");
                print_line(section);
                unmap(text, kMaxText + 1);
                return false;
            }
        } else if (current != nullptr) {
            char* key = nullptr;
            char* value = nullptr;
            if (!split_key_value(cleaned, key, value)) {
                print("neupak: bad key/value at line ");
                print_u64(line_number);
                print("\n");
                unmap(text, kMaxText + 1);
                return false;
            }
            if (strcmp(key, "version") == 0) {
                if (!parse_quoted(value, current->version, sizeof(current->version))) {
                    print("neupak: bad version string at line ");
                    print_u64(line_number);
                    print("\n");
                    unmap(text, kMaxText + 1);
                    return false;
                }
            } else if (strcmp(key, "package") == 0) {
                if (!parse_quoted(value, current->package, sizeof(current->package))) {
                    print("neupak: bad package string at line ");
                    print_u64(line_number);
                    print("\n");
                    unmap(text, kMaxText + 1);
                    return false;
                }
            } else if (strcmp(key, "sha256") == 0) {
                if (!parse_quoted(value, current->sha256, sizeof(current->sha256))) {
                    print("neupak: bad sha256 string at line ");
                    print_u64(line_number);
                    print("\n");
                    unmap(text, kMaxText + 1);
                    return false;
                }
            } else if (strcmp(key, "size") == 0) {
                if (!parse_u64(value, current->size)) {
                    print("neupak: bad size at line ");
                    print_u64(line_number);
                    print("\n");
                    unmap(text, kMaxText + 1);
                    return false;
                }
            } else if (strcmp(key, "strategy") == 0) {
                if (!parse_quoted(value, current->strategy, sizeof(current->strategy))) {
                    print("neupak: bad strategy string at line ");
                    print_u64(line_number);
                    print("\n");
                    unmap(text, kMaxText + 1);
                    return false;
                }
            } else if (strcmp(key, "description") == 0) {
                if (!parse_quoted(value, current->description, sizeof(current->description))) {
                    print("neupak: bad description string at line ");
                    print_u64(line_number);
                    print("\n");
                    unmap(text, kMaxText + 1);
                    return false;
                }
            } else if (strcmp(key, "depends") == 0) {
                char array_text[1024];
                if (!collect_array_value(value, next, text + len, array_text, sizeof(array_text)) ||
                    !parse_string_list_line(array_text, current->depends)) {
                    print("neupak: bad depends array at line ");
                    print_u64(line_number);
                    print("\n");
                    unmap(text, kMaxText + 1);
                    return false;
                }
            }
        }
        line = next;
    }
    unmap(text, kMaxText + 1);
    for (size_t i = 0; i < set.count; ++i) {
        if (!validate_package(set.packages[i], require_hash)) {
            return false;
        }
    }
    return true;
}

bool repo_cache_path(const char* repo, char* out, size_t out_size) {
    size_t len = 0;
    out[0] = '\0';
    return userspace::text::append_text(out, out_size, len, kRepoCacheDir) &&
           userspace::text::append_char(out, out_size, len, '/') &&
           userspace::text::append_text(out, out_size, len, repo) &&
           userspace::text::append_text(out, out_size, len, ".idx");
}

uint32_t fnv1a32(const char* text) {
    uint32_t hash = 2166136261u;
    while (text != nullptr && *text != '\0') {
        hash ^= static_cast<uint8_t>(*text++);
        hash *= 16777619u;
    }
    return hash;
}

char hex_nibble(uint32_t value) {
    value &= 0xFu;
    return static_cast<char>(value < 10 ? ('0' + value) : ('A' + value - 10));
}

bool append_hex8(char* out, size_t out_size, size_t& len, uint32_t value) {
    for (int shift = 28; shift >= 0; shift -= 4) {
        if (!userspace::text::append_char(out, out_size, len, hex_nibble(value >> shift))) {
            return false;
        }
    }
    return true;
}

bool package_cache_path(const char* package, char* out, size_t out_size) {
    size_t len = 0;
    out[0] = '\0';
    return userspace::text::append_text(out, out_size, len, kPackageCacheDir) &&
           userspace::text::append_char(out, out_size, len, '/') &&
           append_hex8(out, out_size, len, fnv1a32(package)) &&
           userspace::text::append_text(out, out_size, len, ".zip");
}

bool derive_package_url(const char* indexurl,
                        const char* package,
                        const char* sha256,
                        char* out,
                        size_t out_size) {
    if (indexurl == nullptr || package == nullptr || !valid_name(package) ||
        sha256 == nullptr || !valid_sha256_hex(sha256)) {
        return false;
    }
    size_t url_len = strlen(indexurl);
    size_t base_len = url_len;
    while (base_len > 0 && indexurl[base_len - 1] != '/') {
        --base_len;
    }
    if (base_len == 0) {
        return false;
    }
    size_t len = 0;
    out[0] = '\0';
    for (size_t i = 0; i < base_len - 1; ++i) {
        if (!userspace::text::append_char(out, out_size, len, indexurl[i])) {
            return false;
        }
    }
    return userspace::text::append_text(out, out_size, len, "/package/") &&
           userspace::text::append_text(out, out_size, len, package) &&
           userspace::text::append_text(out, out_size, len, "?sha256=") &&
           userspace::text::append_text(out, out_size, len, sha256);
}

bool run_download(const char* url, const char* output, bool quiet) {
    if (!ensure_parent_dir(output)) {
        print_line("neupak: invalid download output path");
        return false;
    }
    char args[900];
    size_t len = 0;
    args[0] = '\0';
    if (!userspace::text::append_text(args,
                                      sizeof(args),
                                      len,
                                      "--require-valid-cert --compact ") ||
        (quiet &&
         !userspace::text::append_text(args, sizeof(args), len, "--quiet ")) ||
        !userspace::text::append_text(args, sizeof(args), len, url) ||
        !userspace::text::append_char(args, sizeof(args), len, ' ') ||
        !userspace::text::append_text(args, sizeof(args), len, output)) {
        print_line("neupak: download arguments too long");
        return false;
    }
    long result = exec(kDownloadPath, args, 0, nullptr);
    return result == 0;
}

bool load_indexes(PackageSet& set) {
    set.count = 0;
    size_t repo_count = 0;
    if (!parse_repos_cfg(g_repos, repo_count)) {
        return false;
    }
    for (size_t i = 0; i < repo_count; ++i) {
        char path[kMaxPath];
        if (!repo_cache_path(g_repos[i].name, path, sizeof(path))) {
            return false;
        }
        g_repo_set.count = 0;
        if (!parse_package_toml(path, g_repo_set, true)) {
            print("neupak: unable to read cached repo ");
            print_line(g_repos[i].name);
            return false;
        }
        for (size_t j = 0; j < g_repo_set.count; ++j) {
            if (find_package(set, g_repo_set.packages[j].name) == nullptr) {
                if (set.count >= kMaxPackages) {
                    return false;
                }
                set.packages[set.count++] = g_repo_set.packages[j];
                if (!derive_package_url(g_repos[i].indexurl,
                                        set.packages[set.count - 1].package,
                                        set.packages[set.count - 1].sha256,
                                        set.packages[set.count - 1].package_url,
                                        sizeof(set.packages[set.count - 1].package_url))) {
                    print("neupak: unable to derive package URL for ");
                    print_line(set.packages[set.count - 1].name);
                    return false;
                }
            }
        }
    }
    return true;
}

bool update_index() {
    if (!prepare_dirs()) {
        print_line("neupak: unable to create config directories");
        return false;
    }
    size_t repo_count = 0;
    if (!parse_repos_cfg(g_repos, repo_count)) {
        return false;
    }
    for (size_t i = 0; i < repo_count; ++i) {
        char path[kMaxPath];
        if (!repo_cache_path(g_repos[i].name, path, sizeof(path))) {
            return false;
        }
        print("neupak: updating ");
        print_line(g_repos[i].name);
        if (!run_download(g_repos[i].indexurl, path, false)) {
            print("neupak: failed to download ");
            print_line(g_repos[i].name);
            return false;
        }
        g_check_set.count = 0;
        if (!parse_package_toml(path, g_check_set, true)) {
            print("neupak: invalid index from ");
            print_line(g_repos[i].name);
            return false;
        }
    }
    print_line("neupak: indexes updated");
    return true;
}

InstalledPackage* find_installed(InstalledDb& db, const char* name) {
    for (size_t i = 0; i < db.count; ++i) {
        if (strcmp(db.packages[i].name, name) == 0) {
            return &db.packages[i];
        }
    }
    return nullptr;
}

bool load_installed(InstalledDb& db) {
    db.count = 0;
    char* text = nullptr;
    size_t len = 0;
    if (!read_file_limited(kInstalledPath, text, len)) {
        return true;
    }
    InstalledPackage* current = nullptr;
    char* line = text;
    while (line < text + len) {
        char* next = line;
        while (next < text + len && *next != '\n') {
            ++next;
        }
        if (next < text + len) {
            *next++ = '\0';
        }
        strip_comment(line);
        char* cleaned = trim(line);
        if (cleaned[0] == '\0') {
            line = next;
            continue;
        }
        char section[kMaxName];
        if (parse_section_header(cleaned, section, sizeof(section))) {
            if (db.count >= kMaxPackages || !valid_name(section)) {
                unmap(text, kMaxText + 1);
                return false;
            }
            current = &db.packages[db.count++];
            memset(current, 0, sizeof(*current));
            strlcpy(current->name, section, sizeof(current->name));
        } else if (current != nullptr) {
            char* key = nullptr;
            char* value = nullptr;
            if (split_key_value(cleaned, key, value) && strcmp(key, "version") == 0) {
                if (!parse_quoted(value, current->version, sizeof(current->version))) {
                    unmap(text, kMaxText + 1);
                    return false;
                }
            }
        }
        line = next;
    }
    unmap(text, kMaxText + 1);
    return true;
}

PackageFiles* find_package_files(FilesDb& db, const char* name) {
    for (size_t i = 0; i < db.count; ++i) {
        if (strcmp(db.packages[i].name, name) == 0) {
            return &db.packages[i];
        }
    }
    return nullptr;
}

bool load_files_db(FilesDb& db) {
    db.count = 0;
    char* text = nullptr;
    size_t len = 0;
    if (!read_file_limited(kFilesPath, text, len)) {
        return true;
    }
    PackageFiles* current = nullptr;
    char* line = text;
    while (line < text + len) {
        char* next = line;
        while (next < text + len && *next != '\n') {
            ++next;
        }
        if (next < text + len) {
            *next++ = '\0';
        }
        strip_comment(line);
        char* cleaned = trim(line);
        if (cleaned[0] == '\0') {
            line = next;
            continue;
        }
        char section[kMaxName];
        if (parse_section_header(cleaned, section, sizeof(section))) {
            if (db.count >= kMaxPackages || !valid_name(section)) {
                unmap(text, kMaxText + 1);
                return false;
            }
            current = &db.packages[db.count++];
            memset(current, 0, sizeof(*current));
            strlcpy(current->name, section, sizeof(current->name));
        } else if (current != nullptr && userspace::text::starts_with(cleaned, "files")) {
            char* key = nullptr;
            char* value = nullptr;
            if (!split_key_value(cleaned, key, value)) {
                continue;
            }
            char array_text[8192];
            if (!collect_array_value(value, next, text + len, array_text, sizeof(array_text)) ||
                !parse_file_array(array_text, *current)) {
                unmap(text, kMaxText + 1);
                return false;
            }
        }
        line = next;
    }
    unmap(text, kMaxText + 1);
    return true;
}

bool write_installed_db(const InstalledDb& db) {
    char* text = static_cast<char*>(map_anonymous(kMaxText, MAP_WRITE));
    if (text == nullptr) {
        return false;
    }
    size_t len = 0;
    text[0] = '\0';
    for (size_t i = 0; i < db.count; ++i) {
        if (!userspace::text::append_char(text, kMaxText, len, '[') ||
            !userspace::text::append_text(text, kMaxText, len, db.packages[i].name) ||
            !userspace::text::append_text(text, kMaxText, len, "]\nversion = \"") ||
            !userspace::text::append_text(text, kMaxText, len, db.packages[i].version) ||
            !userspace::text::append_text(text, kMaxText, len, "\"\n\n")) {
            unmap(text, kMaxText);
            return false;
        }
    }
    bool ok = replace_file(kInstalledPath, text);
    unmap(text, kMaxText);
    return ok;
}

bool write_files_db(const FilesDb& db) {
    char* text = static_cast<char*>(map_anonymous(kMaxText, MAP_WRITE));
    if (text == nullptr) {
        return false;
    }
    size_t len = 0;
    text[0] = '\0';
    for (size_t i = 0; i < db.count; ++i) {
        if (!userspace::text::append_char(text, kMaxText, len, '[') ||
            !userspace::text::append_text(text, kMaxText, len, db.packages[i].name) ||
            !userspace::text::append_text(text, kMaxText, len, "]\nfiles = [\n")) {
            unmap(text, kMaxText);
            return false;
        }
        for (size_t j = 0; j < db.packages[i].file_count; ++j) {
            if (!userspace::text::append_text(text, kMaxText, len, "    \"") ||
                !userspace::text::append_text(text, kMaxText, len, db.packages[i].files[j].path) ||
                !userspace::text::append_text(text, kMaxText, len, "\",\n")) {
                unmap(text, kMaxText);
                return false;
            }
        }
        if (!userspace::text::append_text(text, kMaxText, len, "]\n\n")) {
            unmap(text, kMaxText);
            return false;
        }
    }
    bool ok = replace_file(kFilesPath, text);
    unmap(text, kMaxText);
    return ok;
}

void remove_installed_record(InstalledDb& db, const char* name) {
    for (size_t i = 0; i < db.count; ++i) {
        if (strcmp(db.packages[i].name, name) == 0) {
            for (size_t j = i + 1; j < db.count; ++j) {
                db.packages[j - 1] = db.packages[j];
            }
            --db.count;
            return;
        }
    }
}

void remove_files_record(FilesDb& db, const char* name) {
    for (size_t i = 0; i < db.count; ++i) {
        if (strcmp(db.packages[i].name, name) == 0) {
            for (size_t j = i + 1; j < db.count; ++j) {
                db.packages[j - 1] = db.packages[j];
            }
            --db.count;
            return;
        }
    }
}

bool package_is_installed(const InstalledDb& db, const char* name) {
    for (size_t i = 0; i < db.count; ++i) {
        if (strcmp(db.packages[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

void release_lock();

bool current_time_seconds(uint64_t& out) {
    NeutrinoWallTime now{};
    if (!neutrino_get_time(&now)) {
        return false;
    }
    out = now.unix_seconds;
    return true;
}

bool write_lock_metadata(uint32_t lock) {
    uint64_t now = 0;
    bool has_time = current_time_seconds(now);

    char text[80];
    size_t len = 0;
    text[0] = '\0';
    if (!userspace::text::append_text(text, sizeof(text), len, "neupak lock\nstarted=")) {
        return false;
    }
    if (has_time) {
        if (!append_u64(text, sizeof(text), len, now)) {
            return false;
        }
    } else if (!userspace::text::append_text(text, sizeof(text), len, "unknown")) {
        return false;
    }
    if (!userspace::text::append_char(text, sizeof(text), len, '\n') ||
        !userspace::text::append_char(text, sizeof(text), len, '\n')) {
        return false;
    }
    return file_write(lock, text, len) == static_cast<long>(len) &&
           file_sync(lock) == 0;
}

bool read_lock_started(uint64_t& started, bool& legacy_empty) {
    started = 0;
    legacy_empty = false;

    long file = file_open(kLockPath);
    if (file < 0) {
        return false;
    }

    char text[96];
    long got = file_read(static_cast<uint32_t>(file), text, sizeof(text) - 1);
    file_close(static_cast<uint32_t>(file));
    if (got < 0) {
        return false;
    }
    if (got == 0) {
        legacy_empty = true;
        return true;
    }
    text[static_cast<size_t>(got)] = '\0';

    const char* key = "started=";
    const char* cursor = text;
    while (*cursor != '\0') {
        if (strncmp(cursor, key, strlen(key)) == 0) {
            return parse_u64(cursor + strlen(key), started);
        }
        while (*cursor != '\0' && *cursor != '\n') {
            ++cursor;
        }
        if (*cursor == '\n') {
            ++cursor;
        }
    }
    return false;
}

bool lock_is_stale() {
    uint64_t started = 0;
    bool legacy_empty = false;
    if (!read_lock_started(started, legacy_empty)) {
        return false;
    }
    if (legacy_empty) {
        return true;
    }

    uint64_t now = 0;
    if (!current_time_seconds(now)) {
        return false;
    }
    return started > now || now - started >= kLockStaleSeconds;
}

bool acquire_lock() {
    for (size_t attempt = 0; attempt < 2; ++attempt) {
        long lock = file_create(kLockPath);
        if (lock >= 0) {
            g_lock = lock;
            if (!write_lock_metadata(static_cast<uint32_t>(g_lock))) {
                release_lock();
                print_line("neupak: failed to write package database lock");
                return false;
            }
            return true;
        }

        if (attempt == 0 && lock_is_stale()) {
            print_line("neupak: removing stale package database lock");
            file_remove(kLockPath);
            system_sync();
            continue;
        }
        break;
    }

    print_line("neupak: package database is locked");
    return false;
}

void release_lock() {
    if (g_lock >= 0) {
        file_sync(static_cast<uint32_t>(g_lock));
        file_close(static_cast<uint32_t>(g_lock));
        g_lock = -1;
    }
    if (file_remove(kLockPath) == 0) {
        system_sync();
    }
}

bool hex_digit(char ch, uint8_t& value) {
    if (ch >= '0' && ch <= '9') {
        value = static_cast<uint8_t>(ch - '0');
        return true;
    }
    if (ch >= 'a' && ch <= 'f') {
        value = static_cast<uint8_t>(ch - 'a' + 10);
        return true;
    }
    if (ch >= 'A' && ch <= 'F') {
        value = static_cast<uint8_t>(ch - 'A' + 10);
        return true;
    }
    return false;
}

bool parse_sha256_hex(const char* text, uint8_t out[32]) {
    if (strlen(text) != 64) {
        return false;
    }
    for (size_t i = 0; i < 32; ++i) {
        uint8_t hi = 0;
        uint8_t lo = 0;
        if (!hex_digit(text[i * 2], hi) || !hex_digit(text[i * 2 + 1], lo)) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool verify_file_sha256(const char* path, const char* expected_hex, uint64_t expected_size) {
    uint8_t expected[32];
    if (!parse_sha256_hex(expected_hex, expected)) {
        return false;
    }
    long file = file_open(path);
    if (file < 0) {
        return false;
    }
    auth::Sha256 ctx;
    auth::sha256_init(ctx);
    uint64_t total = 0;
    while (true) {
        long got = file_read(static_cast<uint32_t>(file), g_copy_buffer, sizeof(g_copy_buffer));
        if (got < 0) {
            file_close(static_cast<uint32_t>(file));
            return false;
        }
        if (got == 0) {
            break;
        }
        total += static_cast<uint64_t>(got);
        auth::sha256_update(ctx, g_copy_buffer, static_cast<size_t>(got));
    }
    file_close(static_cast<uint32_t>(file));
    uint8_t actual[32];
    auth::sha256_final(ctx, actual);
    return total == expected_size && auth::constant_time_equal(expected, actual, sizeof(actual));
}

uint16_t load_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

uint32_t load_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool read_exact(uint32_t file, uint8_t* out, size_t len) {
    size_t got_total = 0;
    while (got_total < len) {
        long got = file_read(file, out + got_total, len - got_total);
        if (got <= 0) {
            return false;
        }
        got_total += static_cast<size_t>(got);
    }
    return true;
}

bool skip_bytes(uint32_t file, uint32_t count) {
    while (count != 0) {
        size_t want = count < sizeof(g_copy_buffer) ? static_cast<size_t>(count)
                                                    : sizeof(g_copy_buffer);
        if (!read_exact(file, g_copy_buffer, want)) {
            return false;
        }
        count -= static_cast<uint32_t>(want);
    }
    return true;
}

bool archive_path_to_final(const char* archive_path, char* out, size_t out_size) {
    if (archive_path == nullptr || archive_path[0] == '\0' ||
        archive_path[0] == '/' || archive_path[0] == '\\') {
        return false;
    }
    size_t len = 0;
    out[0] = '\0';
    if (!userspace::text::append_char(out, out_size, len, '/')) {
        return false;
    }
    const char* component = archive_path;
    while (*component != '\0') {
        const char* end = component;
        while (*end != '\0' && *end != '/' && *end != '\\') {
            ++end;
        }
        size_t clen = static_cast<size_t>(end - component);
        if (clen == 0 || (clen == 2 && component[0] == '.' && component[1] == '.') ||
            (clen == 3 && component[0] == '.' && component[1] == '.' && component[2] == '.')) {
            return false;
        }
        for (size_t i = 0; i < clen; ++i) {
            if (!userspace::text::append_char(out, out_size, len, component[i])) {
                return false;
            }
        }
        if (*end == '\0') {
            return true;
        }
        if (!userspace::text::append_char(out, out_size, len, '/')) {
            return false;
        }
        component = end + 1;
    }
    return true;
}

bool add_zip_entry(ZipPlan& plan, const ZipEntry& entry) {
    if (entry.is_dir) {
        return true;
    }
    if (strcmp(entry.archive_path, "manifest.toml") == 0) {
        return true;
    }
    if (plan.count >= kMaxFiles) {
        return false;
    }
    for (size_t i = 0; i < plan.count; ++i) {
        if (strcmp(plan.entries[i].final_path, entry.final_path) == 0) {
            return false;
        }
    }
    plan.entries[plan.count++] = entry;
    return true;
}

bool read_local_zip_entry(uint32_t file, ZipEntry& entry, bool& end) {
    end = false;
    uint8_t header[30];
    long first = file_read(file, header, 4);
    if (first == 0) {
        end = true;
        return true;
    }
    if (first != 4) {
        return false;
    }
    uint32_t sig = load_le32(header);
    if (sig == 0x02014b50u || sig == 0x06054b50u) {
        end = true;
        return true;
    }
    if (sig != 0x04034b50u || !read_exact(file, header + 4, sizeof(header) - 4)) {
        return false;
    }
    uint16_t flags = load_le16(header + 6);
    entry.method = load_le16(header + 8);
    entry.compressed_size = load_le32(header + 18);
    entry.uncompressed_size = load_le32(header + 22);
    uint16_t name_len = load_le16(header + 26);
    uint16_t extra_len = load_le16(header + 28);
    if ((flags & 0x0008u) != 0 || name_len == 0 || name_len >= kMaxPath) {
        return false;
    }
    uint8_t name_buf[kMaxPath];
    if (!read_exact(file, name_buf, name_len)) {
        return false;
    }
    name_buf[name_len] = '\0';
    if (!skip_bytes(file, extra_len)) {
        return false;
    }
    memset(&entry, 0, sizeof(entry));
    strlcpy(entry.archive_path, reinterpret_cast<const char*>(name_buf), sizeof(entry.archive_path));
    entry.method = load_le16(header + 8);
    entry.compressed_size = load_le32(header + 18);
    entry.uncompressed_size = load_le32(header + 22);
    size_t path_len = strlen(entry.archive_path);
    entry.is_dir = path_len != 0 && entry.archive_path[path_len - 1] == '/';
    if (!archive_path_to_final(entry.archive_path, entry.final_path, sizeof(entry.final_path))) {
        return false;
    }
    return true;
}

bool plan_zip(const char* path, ZipPlan& plan) {
    plan.count = 0;
    long file = file_open(path);
    if (file < 0) {
        return false;
    }
    while (true) {
        ZipEntry entry{};
        bool end = false;
        if (!read_local_zip_entry(static_cast<uint32_t>(file), entry, end)) {
            file_close(static_cast<uint32_t>(file));
            return false;
        }
        if (end) {
            break;
        }
        if (entry.method != 0) {
            file_close(static_cast<uint32_t>(file));
            print_line("neupak: compressed zip entries are not supported yet");
            return false;
        }
        if (!add_zip_entry(plan, entry) || !skip_bytes(static_cast<uint32_t>(file), entry.compressed_size)) {
            file_close(static_cast<uint32_t>(file));
            return false;
        }
    }
    file_close(static_cast<uint32_t>(file));
    return plan.count != 0;
}

bool copy_zip_payload_to_file(uint32_t zip, const char* output_path, uint32_t size) {
    ensure_parent_dir(output_path);
    long existing = file_open(output_path);
    if (existing >= 0) {
        file_close(static_cast<uint32_t>(existing));
        if (file_remove(output_path) < 0) {
            return false;
        }
    }
    long out = file_create(output_path);
    if (out < 0) {
        return false;
    }
    uint32_t remaining = size;
    while (remaining != 0) {
        size_t want = remaining < sizeof(g_copy_buffer) ? static_cast<size_t>(remaining)
                                                        : sizeof(g_copy_buffer);
        if (!read_exact(zip, g_copy_buffer, want) ||
            !write_all(static_cast<uint32_t>(out), g_copy_buffer, want)) {
            file_close(static_cast<uint32_t>(out));
            return false;
        }
        remaining -= static_cast<uint32_t>(want);
    }
    file_close(static_cast<uint32_t>(out));
    return true;
}

bool extract_manifest_package(const char* zip_path, const char* expected_name, Package& out) {
    long file = file_open(zip_path);
    if (file < 0) {
        return false;
    }
    bool found = false;
    while (true) {
        ZipEntry entry{};
        bool end = false;
        if (!read_local_zip_entry(static_cast<uint32_t>(file), entry, end)) {
            file_close(static_cast<uint32_t>(file));
            return false;
        }
        if (end) {
            break;
        }
        if (strcmp(entry.archive_path, "manifest.toml") == 0) {
            if (entry.method != 0 ||
                !copy_zip_payload_to_file(static_cast<uint32_t>(file),
                                          kManifestCachePath,
                                          entry.uncompressed_size)) {
                file_close(static_cast<uint32_t>(file));
                return false;
            }
            found = true;
        } else if (!skip_bytes(static_cast<uint32_t>(file), entry.compressed_size)) {
            file_close(static_cast<uint32_t>(file));
            return false;
        }
    }
    file_close(static_cast<uint32_t>(file));
    if (!found) {
        print_line("neupak: package archive has no manifest.toml");
        return false;
    }

    g_manifest_set.count = 0;
    if (!parse_package_toml(kManifestCachePath, g_manifest_set, false)) {
        return false;
    }
    Package* manifest_pkg = expected_name == nullptr ? nullptr
                                                     : find_package(g_manifest_set, expected_name);
    if (manifest_pkg == nullptr && g_manifest_set.count == 1) {
        manifest_pkg = &g_manifest_set.packages[0];
    }
    if (manifest_pkg == nullptr) {
        if (expected_name == nullptr) {
            print_line("neupak: local package manifest must contain exactly one package section");
        } else {
            print("neupak: manifest missing package section ");
            print_line(expected_name);
        }
        return false;
    }
    out = *manifest_pkg;
    return true;
}

bool extract_manifest(const char* zip_path, const Package& index_pkg) {
    Package manifest_pkg{};
    if (!extract_manifest_package(zip_path, index_pkg.name, manifest_pkg)) {
        return false;
    }
    return same_manifest_package(index_pkg, manifest_pkg);
}

bool owned_by_other(const FilesDb& db, const char* package, const char* path, const char*& owner) {
    owner = nullptr;
    for (size_t i = 0; i < db.count; ++i) {
        for (size_t j = 0; j < db.packages[i].file_count; ++j) {
            if (strcmp(db.packages[i].files[j].path, path) == 0) {
                owner = db.packages[i].name;
                return strcmp(db.packages[i].name, package) != 0;
            }
        }
    }
    return false;
}

bool temp_payload_path(const char* package, const char* final_path, char* out, size_t out_size);

bool files_equal(const char* first_path, const char* second_path) {
    long first = file_open(first_path);
    if (first < 0) {
        return false;
    }
    long second = file_open(second_path);
    if (second < 0) {
        file_close(static_cast<uint32_t>(first));
        return false;
    }

    bool equal = true;
    uint8_t second_buffer[kCopyBufferSize];
    while (true) {
        long first_got = file_read(static_cast<uint32_t>(first),
                                   g_copy_buffer,
                                   sizeof(g_copy_buffer));
        long second_got = file_read(static_cast<uint32_t>(second),
                                    second_buffer,
                                    sizeof(second_buffer));
        if (first_got < 0 || second_got < 0 || first_got != second_got) {
            equal = false;
            break;
        }
        if (first_got == 0) {
            break;
        }
        if (memcmp(g_copy_buffer, second_buffer, static_cast<size_t>(first_got)) != 0) {
            equal = false;
            break;
        }
    }
    file_close(static_cast<uint32_t>(first));
    file_close(static_cast<uint32_t>(second));
    return equal;
}

bool check_conflicts(const ZipPlan& plan, const FilesDb& files, const char* package, bool force) {
    for (size_t i = 0; i < plan.count; ++i) {
        const char* owner = nullptr;
        if (owned_by_other(files, package, plan.entries[i].final_path, owner)) {
            if (!force) {
                print("neupak: file conflict with ");
                print(owner);
                print(": ");
                print_line(plan.entries[i].final_path);
                return false;
            }
            continue;
        }
        if (owner == nullptr) {
            char physical_path[kMaxPath];
            if (!sysroot_path_for_logical(plan.entries[i].final_path,
                                          physical_path,
                                          sizeof(physical_path))) {
                return false;
            }
            long existing = file_open(physical_path);
            if (existing >= 0) {
                file_close(static_cast<uint32_t>(existing));
                char package_path[kMaxPath];
                if (!temp_payload_path(package,
                                       plan.entries[i].final_path,
                                       package_path,
                                       sizeof(package_path))) {
                    return false;
                }
                bool preinstalled_library =
                    userspace::text::starts_with(plan.entries[i].final_path, "/library/");
                if (!force && !preinstalled_library &&
                    !files_equal(physical_path, package_path)) {
                    print("neupak: refusing to overwrite unowned file ");
                    print_line(plan.entries[i].final_path);
                    return false;
                }
            }
        }
    }
    return true;
}

bool temp_payload_path(const char* package, const char* final_path, char* out, size_t out_size) {
    size_t len = 0;
    out[0] = '\0';
    return userspace::text::append_text(out, out_size, len, kExtractDir) &&
           userspace::text::append_char(out, out_size, len, '/') &&
           append_hex8(out, out_size, len, fnv1a32(package)) &&
           userspace::text::append_text(out, out_size, len, final_path);
}

bool copy_entry_data_to_path(uint32_t zip, const ZipEntry& entry, const char* path) {
    if (!ensure_parent_dir(path)) {
        print("neupak: unable to create temp parent for ");
        print_line(path);
        return false;
    }
    long existing = file_open(path);
    if (existing >= 0) {
        file_close(static_cast<uint32_t>(existing));
        if (file_remove(path) < 0) {
            print("neupak: unable to replace temp file ");
            print_line(path);
            return false;
        }
    }
    long out = file_create(path);
    if (out < 0) {
        print("neupak: unable to create temp file ");
        print_line(path);
        return false;
    }
    uint32_t remaining = entry.uncompressed_size;
    while (remaining != 0) {
        size_t want = remaining < sizeof(g_copy_buffer) ? static_cast<size_t>(remaining)
                                                        : sizeof(g_copy_buffer);
        if (!read_exact(zip, g_copy_buffer, want) ||
            !write_all(static_cast<uint32_t>(out), g_copy_buffer, want)) {
            file_close(static_cast<uint32_t>(out));
            print("neupak: unable to write temp file ");
            print_line(path);
            return false;
        }
        remaining -= static_cast<uint32_t>(want);
    }
    file_close(static_cast<uint32_t>(out));
    return true;
}

bool copy_file_replace(const char* source, const char* dest) {
    if (!ensure_parent_dir(dest)) {
        return false;
    }
    long in = file_open(source);
    if (in < 0) {
        return false;
    }
    long existing = file_open(dest);
    if (existing >= 0) {
        file_close(static_cast<uint32_t>(existing));
        if (file_remove(dest) < 0) {
            file_close(static_cast<uint32_t>(in));
            return false;
        }
    }
    long out = file_create(dest);
    if (out < 0) {
        file_close(static_cast<uint32_t>(in));
        return false;
    }
    bool ok = true;
    while (true) {
        long got = file_read(static_cast<uint32_t>(in), g_copy_buffer, sizeof(g_copy_buffer));
        if (got < 0) {
            ok = false;
            break;
        }
        if (got == 0) {
            break;
        }
        if (!write_all(static_cast<uint32_t>(out), g_copy_buffer, static_cast<size_t>(got))) {
            ok = false;
            break;
        }
    }
    file_close(static_cast<uint32_t>(in));
    file_close(static_cast<uint32_t>(out));
    return ok;
}

bool extract_zip_to_temp(const char* path, const char* package) {
    long file = file_open(path);
    if (file < 0) {
        return false;
    }
    while (true) {
        ZipEntry entry{};
        bool end = false;
        if (!read_local_zip_entry(static_cast<uint32_t>(file), entry, end)) {
            file_close(static_cast<uint32_t>(file));
            return false;
        }
        if (end) {
            break;
        }
        char temp_path[kMaxPath];
        if (!temp_payload_path(package, entry.final_path, temp_path, sizeof(temp_path))) {
            print("neupak: temp path too long for ");
            print_line(entry.final_path);
            file_close(static_cast<uint32_t>(file));
            return false;
        }
        if (entry.is_dir) {
            if (!ensure_dir_tree(temp_path)) {
                print("neupak: unable to create temp dir ");
                print_line(temp_path);
                file_close(static_cast<uint32_t>(file));
                return false;
            }
        } else if (strcmp(entry.archive_path, "manifest.toml") == 0) {
            if (!skip_bytes(static_cast<uint32_t>(file), entry.compressed_size)) {
                file_close(static_cast<uint32_t>(file));
                return false;
            }
        } else if (entry.method == 0) {
            if (!copy_entry_data_to_path(static_cast<uint32_t>(file), entry, temp_path)) {
                file_close(static_cast<uint32_t>(file));
                return false;
            }
        } else {
            file_close(static_cast<uint32_t>(file));
            return false;
        }
    }
    file_close(static_cast<uint32_t>(file));
    return true;
}

bool copy_temp_into_place(const char* package, const ZipPlan& plan) {
    for (size_t i = 0; i < plan.count; ++i) {
        char temp_path[kMaxPath];
        char dest_path[kMaxPath];
        if (!temp_payload_path(package, plan.entries[i].final_path, temp_path, sizeof(temp_path)) ||
            !sysroot_path_for_logical(plan.entries[i].final_path, dest_path, sizeof(dest_path)) ||
            !copy_file_replace(temp_path, dest_path)) {
            return false;
        }
    }
    return true;
}

bool update_db_for_package(InstalledDb& installed,
                           FilesDb& files,
                           const Package& pkg,
                           const ZipPlan& plan) {
    InstalledPackage* installed_pkg = find_installed(installed, pkg.name);
    if (installed_pkg == nullptr) {
        if (installed.count >= kMaxPackages) {
            return false;
        }
        installed_pkg = &installed.packages[installed.count++];
        memset(installed_pkg, 0, sizeof(*installed_pkg));
        strlcpy(installed_pkg->name, pkg.name, sizeof(installed_pkg->name));
    }
    strlcpy(installed_pkg->version, pkg.version, sizeof(installed_pkg->version));

    PackageFiles* package_files = find_package_files(files, pkg.name);
    if (package_files == nullptr) {
        if (files.count >= kMaxPackages) {
            return false;
        }
        package_files = &files.packages[files.count++];
        memset(package_files, 0, sizeof(*package_files));
        strlcpy(package_files->name, pkg.name, sizeof(package_files->name));
    }
    package_files->file_count = 0;
    for (size_t i = 0; i < plan.count; ++i) {
        if (package_files->file_count >= kMaxFiles) {
            return false;
        }
        strlcpy(package_files->files[package_files->file_count++].path,
                plan.entries[i].final_path,
                kMaxPath);
    }
    return true;
}

bool queue_contains(char names[kMaxInstallQueue][kMaxName], size_t count, const char* name) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

bool installed_version_matches(const InstalledDb& installed, const Package& pkg) {
    for (size_t i = 0; i < installed.count; ++i) {
        if (strcmp(installed.packages[i].name, pkg.name) == 0) {
            return strcmp(installed.packages[i].version, pkg.version) == 0;
        }
    }
    return false;
}

bool add_install_with_deps(PackageSet& index,
                           const char* name,
                           const InstalledDb& installed,
                           bool is_dependency,
                           char queue[kMaxInstallQueue][kMaxName],
                           size_t& queue_count) {
    Package* pkg = find_package(index, name);
    if (pkg == nullptr) {
        print("neupak: package not found: ");
        print_line(name);
        return false;
    }
    if (is_dependency && installed_version_matches(installed, *pkg)) {
        return true;
    }
    if (queue_contains(queue, queue_count, name)) {
        return true;
    }
    for (size_t i = 0; i < pkg->depends.count; ++i) {
        if (!add_install_with_deps(index,
                                   pkg->depends.values[i],
                                   installed,
                                   true,
                                   queue,
                                   queue_count)) {
            return false;
        }
    }
    if (queue_count >= kMaxInstallQueue) {
        return false;
    }
    strlcpy(queue[queue_count++], name, kMaxName);
    return true;
}

bool check_installed_dependencies(const Package& pkg, const InstalledDb& installed) {
    for (size_t i = 0; i < pkg.depends.count; ++i) {
        bool found = false;
        for (size_t j = 0; j < installed.count; ++j) {
            if (strcmp(installed.packages[j].name, pkg.depends.values[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            print("neupak: missing dependency for ");
            print(pkg.name);
            print(": ");
            print_line(pkg.depends.values[i]);
            return false;
        }
    }
    return true;
}

bool check_reverse_dependencies(const char* name, const InstalledDb& installed) {
    g_index.count = 0;
    if (!load_indexes(g_index)) {
        print_line("neupak: warning: unable to check reverse dependencies");
        return true;
    }
    for (size_t i = 0; i < installed.count; ++i) {
        if (strcmp(installed.packages[i].name, name) == 0) {
            continue;
        }
        Package* pkg = find_package(g_index, installed.packages[i].name);
        if (pkg == nullptr) {
            continue;
        }
        for (size_t j = 0; j < pkg->depends.count; ++j) {
            if (strcmp(pkg->depends.values[j], name) == 0) {
                print("neupak: refusing to uninstall ");
                print(name);
                print("; required by ");
                print_line(installed.packages[i].name);
                return false;
            }
        }
    }
    return true;
}

bool list_package_files(const char* name) {
    if (!load_installed(g_installed) || !load_files_db(g_files)) {
        print_line("neupak: unable to read package databases");
        return false;
    }
    if (!package_is_installed(g_installed, name)) {
        print("neupak: package is not installed: ");
        print_line(name);
        return false;
    }
    PackageFiles* package_files = find_package_files(g_files, name);
    if (package_files == nullptr || package_files->file_count == 0) {
        print("neupak: no files recorded for ");
        print_line(name);
        return true;
    }
    for (size_t i = 0; i < package_files->file_count; ++i) {
        print_line(package_files->files[i].path);
    }
    return true;
}

bool remove_package_payload(const PackageFiles& package_files) {
    for (size_t i = 0; i < package_files.file_count; ++i) {
        char physical_path[kMaxPath];
        if (!sysroot_path_for_logical(package_files.files[i].path,
                                      physical_path,
                                      sizeof(physical_path))) {
            return false;
        }
        long existing = file_open(physical_path);
        if (existing < 0) {
            continue;
        }
        file_close(static_cast<uint32_t>(existing));
        if (file_remove(physical_path) < 0) {
            print("neupak: unable to remove ");
            print_line(package_files.files[i].path);
            return false;
        }
    }
    return true;
}

bool uninstall_package(const char* name) {
    if (!load_installed(g_installed) || !load_files_db(g_files)) {
        print_line("neupak: unable to read package databases");
        return false;
    }
    if (!package_is_installed(g_installed, name)) {
        print("neupak: package is not installed: ");
        print_line(name);
        return false;
    }
    if (!check_reverse_dependencies(name, g_installed)) {
        return false;
    }

    if (!acquire_lock()) {
        return false;
    }
    bool ok = true;
    if (!load_installed(g_installed) || !load_files_db(g_files)) {
        print_line("neupak: failed to reload package databases");
        ok = false;
    } else if (!package_is_installed(g_installed, name)) {
        print("neupak: package is not installed: ");
        print_line(name);
        ok = false;
    } else if (!check_reverse_dependencies(name, g_installed)) {
        ok = false;
    } else {
        PackageFiles package_files{};
        PackageFiles* found_files = find_package_files(g_files, name);
        if (found_files != nullptr) {
            package_files = *found_files;
        } else {
            strlcpy(package_files.name, name, sizeof(package_files.name));
        }
        if (!remove_package_payload(package_files)) {
            ok = false;
        } else {
            remove_installed_record(g_installed, name);
            remove_files_record(g_files, name);
            if (!write_installed_db(g_installed)) {
                print_line("neupak: failed to write installed database");
                ok = false;
            } else if (!write_files_db(g_files)) {
                print_line("neupak: failed to write file database");
                ok = false;
            } else if (!neutrino_sync()) {
                print_line("neupak: failed to sync package database");
                ok = false;
            }
        }
    }
    release_lock();
    if (!ok) {
        print("neupak: uninstall failed for ");
        print_line(name);
        return false;
    }
    print("neupak: uninstalled ");
    print_line(name);
    return true;
}

void print_package_summary(const Package& pkg) {
    print(pkg.name);
    print(" ");
    print(pkg.version);
    if (pkg.description[0] != '\0') {
        print(" - ");
        print(pkg.description);
    }
    print("\n");
}

bool search_packages(const char* query) {
    if (!prepare_dirs()) {
        return false;
    }
    if (!load_indexes(g_index)) {
        print_line("neupak: run `neupak update-index` first");
        return false;
    }
    size_t matches = 0;
    for (size_t i = 0; i < g_index.count; ++i) {
        const Package& pkg = g_index.packages[i];
        if (contains_case_insensitive(pkg.name, query) ||
            contains_case_insensitive(pkg.description, query)) {
            print_package_summary(pkg);
            ++matches;
        }
    }
    if (matches == 0) {
        print_line("neupak: no matching packages");
    }
    return true;
}

bool install_zip_package(const char* zip_path, const Package& pkg, bool force) {
    if (!plan_zip(zip_path, g_plan)) {
        print("neupak: invalid package archive for ");
        print_line(pkg.name);
        return false;
    }

    if (!load_installed(g_installed) || !load_files_db(g_files)) {
        return false;
    }
    if (!check_installed_dependencies(pkg, g_installed)) {
        return false;
    }
    if (!acquire_lock()) {
        return false;
    }
    bool ok = true;
    if (!load_installed(g_installed)) {
        print_line("neupak: failed to reload installed database");
        ok = false;
    } else if (!load_files_db(g_files)) {
        print_line("neupak: failed to reload file database");
        ok = false;
    } else if (!check_installed_dependencies(pkg, g_installed)) {
        ok = false;
    } else if (!extract_zip_to_temp(zip_path, pkg.name)) {
        print_line("neupak: failed to extract package to temp");
        ok = false;
    } else if (!check_conflicts(g_plan, g_files, pkg.name, force)) {
        ok = false;
    } else if (!copy_temp_into_place(pkg.name, g_plan)) {
        print_line("neupak: failed to copy package files into place");
        ok = false;
    } else if (!update_db_for_package(g_installed, g_files, pkg, g_plan)) {
        print_line("neupak: failed to update package databases in memory");
        ok = false;
    } else if (!write_installed_db(g_installed)) {
        print_line("neupak: failed to write installed database");
        ok = false;
    } else if (!write_files_db(g_files)) {
        print_line("neupak: failed to write file database");
        ok = false;
    } else if (!neutrino_sync()) {
        print_line("neupak: failed to sync package database");
        ok = false;
    }
    release_lock();
    if (!ok) {
        print("neupak: install failed for ");
        print_line(pkg.name);
        return false;
    }
    print("neupak: installed ");
    print(pkg.name);
    print(" ");
    print_line(pkg.version);
    return true;
}

bool install_one(PackageSet& index, const char* name, bool force) {
    Package* pkg = find_package(index, name);
    if (pkg == nullptr) {
        return false;
    }
    char zip_path[kMaxPath];
    if (!package_cache_path(pkg->name, zip_path, sizeof(zip_path))) {
        return false;
    }
    print("neupak: downloading ");
    print_line(pkg->name);
    if (!run_download(pkg->package_url, zip_path, false)) {
        return false;
    }
    if (!verify_file_sha256(zip_path, pkg->sha256, pkg->size)) {
        print("neupak: checksum failed for ");
        print_line(pkg->name);
        return false;
    }
    if (!extract_manifest(zip_path, *pkg)) {
        print("neupak: manifest validation failed for ");
        print_line(pkg->name);
        return false;
    }
    return install_zip_package(zip_path, *pkg, force);
}

bool install_local_package(const char* zip_path, bool force) {
    if (!prepare_dirs()) {
        return false;
    }
    long file = file_open(zip_path);
    if (file < 0) {
        print("neupak: unable to open local package ");
        print_line(zip_path);
        return false;
    }
    file_close(static_cast<uint32_t>(file));

    Package pkg{};
    if (!extract_manifest_package(zip_path, nullptr, pkg)) {
        print_line("neupak: local package manifest validation failed");
        return false;
    }

    print("neupak: installing local package ");
    print(pkg.name);
    print_line(" (no index checksum)");
    return install_zip_package(zip_path, pkg, force);
}

bool install_package(const char* name, bool force) {
    if (!prepare_dirs()) {
        return false;
    }
    if (!load_indexes(g_index)) {
        print_line("neupak: run `neupak update-index` first");
        return false;
    }
    if (!load_installed(g_installed)) {
        print_line("neupak: unable to read installed database");
        return false;
    }
    size_t queue_count = 0;
    if (!add_install_with_deps(g_index,
                               name,
                               g_installed,
                               false,
                               g_install_queue,
                               queue_count)) {
        return false;
    }
    for (size_t i = 0; i < queue_count; ++i) {
        if (!install_one(g_index, g_install_queue[i], force)) {
            return false;
        }
    }
    return true;
}

void usage() {
    print_line("usage: neupak update-index");
    print_line("       neupak search <query>");
    print_line("       neupak install [--force-overwrite] <package>");
    print_line("       neupak install-local [--force-overwrite] <zip-path>");
    print_line("       neupak files <package>");
    print_line("       neupak uninstall <package>");
}

}  // namespace

int main(uint64_t arg_ptr, uint64_t) {
    g_console = neutrino_open_stdout();

    Args args{};
    if (!parse_args(reinterpret_cast<const char*>(arg_ptr), args) || args.count == 0) {
        usage();
        return 1;
    }

    if (strcmp(args.words[0], "update-index") == 0) {
        return update_index() ? 0 : 1;
    }

    if (strcmp(args.words[0], "search") == 0) {
        if (args.count != 2) {
            usage();
            return 1;
        }
        return search_packages(args.words[1]) ? 0 : 1;
    }

    if (strcmp(args.words[0], "files") == 0 ||
        strcmp(args.words[0], "list-files") == 0) {
        if (args.count != 2 || !valid_name(args.words[1])) {
            usage();
            return 1;
        }
        return list_package_files(args.words[1]) ? 0 : 1;
    }

    if (strcmp(args.words[0], "uninstall") == 0) {
        if (args.count != 2 || !valid_name(args.words[1])) {
            usage();
            return 1;
        }
        return uninstall_package(args.words[1]) ? 0 : 1;
    }

    if (strcmp(args.words[0], "install") == 0 ||
        strcmp(args.words[0], "install-local") == 0) {
        bool local = strcmp(args.words[0], "install-local") == 0;
        bool force = false;
        const char* package = nullptr;
        for (size_t i = 1; i < args.count; ++i) {
            if (strcmp(args.words[i], "--force-overwrite") == 0) {
                force = true;
            } else if (package == nullptr) {
                package = args.words[i];
            } else {
                usage();
                return 1;
            }
        }
        if (package == nullptr) {
            usage();
            return 1;
        }
        if (local) {
            return install_local_package(package, force) ? 0 : 1;
        }
        if (!valid_name(package)) {
            usage();
            return 1;
        }
        return install_package(package, force) ? 0 : 1;
    }

    usage();
    return 1;
}
