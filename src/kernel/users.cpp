#include "users.hpp"

#include <stddef.h>
#include <stdint.h>

#include "capabilities.hpp"
#include "string_util.hpp"
#include "sync.hpp"
#include "time.hpp"
#include "../fs/vfs.hpp"
#include "../lib/mem.hpp"
#include "../drivers/log/logging.hpp"
#include "../kernel/path_util.hpp"

namespace {

users::User g_users[users::kMaxUsers];
char g_storage_path[128];
char g_storage_path_fallback[128];
bool g_has_storage_path = false;
bool g_user_database_loading = false;
users::UserId g_machine_id = {0, 0};
uint64_t g_next_user_id = 1;
sync::SpinLock g_user_lock;
sync::SpinLock g_persist_lock;

class LockGuard {
public:
    explicit LockGuard(sync::SpinLock& lock) : lock_(lock) { lock_.lock(); }
    ~LockGuard() { lock_.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    sync::SpinLock& lock_;
};

class UserLoadGuard {
public:
    UserLoadGuard() {
        __atomic_store_n(&g_user_database_loading, true, __ATOMIC_RELEASE);
    }
    ~UserLoadGuard() {
        __atomic_store_n(&g_user_database_loading, false, __ATOMIC_RELEASE);
    }
    UserLoadGuard(const UserLoadGuard&) = delete;
    UserLoadGuard& operator=(const UserLoadGuard&) = delete;
};

bool user_database_loading() {
    return __atomic_load_n(&g_user_database_loading, __ATOMIC_ACQUIRE);
}

constexpr uint32_t kMagic = 0x4E544455;  // 'NTDU'
constexpr uint16_t kVersion = 4;
constexpr uint16_t kLegacyVersion = 2;
constexpr uint16_t kLegacyVersion1 = 1;
constexpr uint64_t kLegacyAllCapabilities = (1ull << 3) - 1;
constexpr uint32_t kMaxPasswordIterations = 1000000;

struct PackedUserV1 {
    char name[32];
    uint64_t allowed_caps;
    uint64_t generation;
    uint8_t active;
    uint8_t reserved[7];
};

struct PackedUser {
    char name[32];
    uint64_t allowed_caps;
    uint64_t generation;
    uint8_t active;
    uint8_t password_set;
    uint16_t password_algorithm;
    uint32_t password_iterations;
    uint8_t password_salt[16];
    uint8_t password_hash[32];
    uint64_t machine_id;
    uint64_t local_id;
    uint8_t reserved[8];
};

static_assert(sizeof(PackedUserV1) == 56, "PackedUserV1 size changed");
static_assert(sizeof(PackedUser) == 128, "PackedUser size changed");

struct LegacyHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t entry_size;
    uint32_t count;
};

static_assert(sizeof(LegacyHeader) == 12, "LegacyHeader size changed");

struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t entry_size;
    uint32_t count;
    uint64_t next_user_id;
    uint64_t machine_id;
};

static_assert(sizeof(Header) == 32, "Header size changed");

struct UserStore {
    Header header;
    PackedUser users[users::kMaxUsers];
};

constexpr size_t kUserStoreBufferSize =
    sizeof(Header) + sizeof(PackedUser) * users::kMaxUsers;
static_assert(sizeof(UserStore) == kUserStoreBufferSize,
              "UserStore layout changed");

bool valid_user_name(const char* name) {
    if (name == nullptr) {
        return false;
    }
    size_t length = 0;
    while (length < sizeof(users::User::name) && name[length] != '\0') {
        unsigned char ch = static_cast<unsigned char>(name[length]);
        if (ch < 0x20 || ch == 0x7F || ch == '/' || ch == '\\') {
            return false;
        }
        ++length;
    }
    if (length == 0 || length >= sizeof(users::User::name)) {
        return false;
    }
    return !(length == 1 && name[0] == '.') &&
           !(length == 2 && name[0] == '.' && name[1] == '.');
}

bool copy_packed_user_name(const char* source,
                           size_t source_size,
                           char (&out)[sizeof(users::User::name)]) {
    if (source == nullptr || source_size == 0) {
        return false;
    }
    size_t length = 0;
    while (length < source_size && source[length] != '\0') {
        ++length;
    }
    if (length == source_size || length >= sizeof(out)) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        out[i] = source[i];
    }
    out[length] = '\0';
    return valid_user_name(out);
}

bool user_index(const users::User* user, size_t& out_index) {
    if (user == nullptr) {
        return false;
    }
    uintptr_t address = reinterpret_cast<uintptr_t>(user);
    uintptr_t begin = reinterpret_cast<uintptr_t>(&g_users[0]);
    uintptr_t end =
        reinterpret_cast<uintptr_t>(&g_users[users::kMaxUsers]);
    if (address < begin || address >= end) {
        return false;
    }
    uintptr_t offset = address - begin;
    if ((offset % sizeof(users::User)) != 0) {
        return false;
    }
    out_index = static_cast<size_t>(offset / sizeof(users::User));
    return true;
}

users::User* find_name_locked(const char* name) {
    for (size_t i = 0; i < users::kMaxUsers; ++i) {
        users::User& user = g_users[i];
        if (__atomic_load_n(&user.active, __ATOMIC_ACQUIRE) &&
            string_util::equals(user.name, name)) {
            return &user;
        }
    }
    return nullptr;
}

static bool path_valid() {
    return g_has_storage_path && g_storage_path[0] != '\0';
}

bool build_root_relative_path(const char* suffix,
                              char* out,
                              size_t out_size) {
    if (!path_valid() || suffix == nullptr || out == nullptr || out_size == 0) {
        return false;
    }
    size_t root_len = 0;
    while (g_storage_path[root_len] != '\0' && g_storage_path[root_len] != '/') {
        ++root_len;
    }
    if (root_len == 0 || root_len + 1 >= out_size) {
        return false;
    }
    for (size_t i = 0; i < root_len; ++i) {
        out[i] = g_storage_path[i];
    }
    size_t idx = root_len;
    out[idx++] = '/';
    for (size_t i = 0; suffix[i] != '\0'; ++i) {
        if (idx + 1 >= out_size) {
            return false;
        }
        out[idx++] = suffix[i];
    }
    out[idx] = '\0';
    return true;
}

bool machine_id_path(char* out, size_t out_size) {
    return build_root_relative_path("system/machine.id", out, out_size);
}

void ensure_parent_directories(const char* path);

uint64_t random_u64() {
    uint64_t seed = timekeeping::tick_count();
    seed ^= reinterpret_cast<uintptr_t>(&seed);
    if (seed == 0) {
        seed = 0xD0C0FFEE12345678ull;
    }
    uint64_t x = seed;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    return x * 0x2545F4914F6CDD1Dull;
}

bool persist_machine_id() {
    if (!path_valid()) {
        return false;
    }
    char path[128];
    if (!machine_id_path(path, sizeof(path))) {
        return false;
    }
    ensure_parent_directories(path);
    vfs::FileHandle handle{};
    if (!vfs::create_file(path, handle)) {
        if (!vfs::open_file(path, handle)) {
            return false;
        }
    }
    size_t written = 0;
    bool ok = vfs::write_file(handle,
                              0,
                              reinterpret_cast<const uint8_t*>(&g_machine_id.machine),
                              sizeof(g_machine_id.machine),
                              written);
    vfs::close_file(handle);
    return ok && written == sizeof(g_machine_id.machine);
}

bool load_machine_id() {
    if (!path_valid()) {
        return false;
    }
    char path[128];
    if (!machine_id_path(path, sizeof(path))) {
        return false;
    }
    vfs::FileHandle handle{};
    size_t read_size = 0;
    if (vfs::open_file(path, handle)) {
        uint8_t buffer[sizeof(g_machine_id.machine)];
        if (vfs::read_file(handle, 0, buffer, sizeof(buffer), read_size) &&
            read_size == sizeof(buffer)) {
            memcpy(&g_machine_id.machine, buffer, sizeof(g_machine_id.machine));
            vfs::close_file(handle);
            if (g_machine_id.machine != 0) {
                return true;
            }
        }
        vfs::close_file(handle);
    }
    g_machine_id.machine = random_u64();
    if (g_machine_id.machine == 0) {
        g_machine_id.machine = 1;
    }
    return persist_machine_id();
}

void ensure_directory(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    vfs::DirectoryHandle handle{};
    if (vfs::open_directory(path, handle)) {
        vfs::close_directory(handle);
        return;
    }
    if (!vfs::create_directory(path)) {
        log_message(LogLevel::Warn, "Users: failed to create directory '%s'", path);
    }
}

void ensure_parent_directories(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return;
    }

    char current[path_util::kMaxPathLength];
    size_t path_len = string_util::length(path);
    if (path_len >= sizeof(current)) {
        path_len = sizeof(current) - 1;
    }
    for (size_t i = 0; i < path_len; ++i) {
        current[i] = path[i];
    }
    current[path_len] = '\0';

    int last_slash = -1;
    for (size_t i = 0; current[i] != '\0'; ++i) {
        if (current[i] == '/') {
            last_slash = static_cast<int>(i);
        }
    }
    if (last_slash <= 0) {
        return;
    }
    current[last_slash] = '\0';

    char partial[path_util::kMaxPathLength];
    size_t out = 0;
    for (size_t i = 0; current[i] != '\0' && out + 1 < sizeof(partial); ++i) {
        partial[out++] = current[i];
        partial[out] = '\0';
        if (current[i] == '/') {
            continue;
        }
        char next = current[i + 1];
        if (next == '/' || next == '\0') {
            ensure_directory(partial);
        }
    }
}

void ensure_home_directory(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return;
    }

    char user_root[128];
    if (!build_root_relative_path("user", user_root, sizeof(user_root))) {
        return;
    }
    ensure_directory(user_root);

    char user_home[128];
    size_t len = string_util::length(user_root);
    if (len + 1 >= sizeof(user_home)) {
        return;
    }
    string_util::copy(user_home, sizeof(user_home), user_root);
    user_home[len++] = '/';
    user_home[len] = '\0';
    string_util::copy(user_home + len, sizeof(user_home) - len, name);
    ensure_directory(user_home);
}

uint64_t normalize_stored_caps(uint64_t stored_caps,
                               bool legacy_encoding,
                               bool& upgraded) {
    if (stored_caps == capabilities::kFullPermissions) {
        return stored_caps;
    }
    if (legacy_encoding && stored_caps == kLegacyAllCapabilities) {
        upgraded = true;
        return capabilities::kFullPermissions;
    }
    uint64_t normalized = capabilities::normalize_mask(stored_caps);
    if (normalized != stored_caps) {
        upgraded = true;
    }
    return normalized;
}

void reset_user_pool() {
    sync::IrqLockGuard guard(g_user_lock);
    for (size_t i = 0; i < users::kMaxUsers; ++i) {
        __atomic_store_n(&g_users[i].active, false, __ATOMIC_RELEASE);
        __atomic_store_n(&g_users[i].allowed_caps, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_users[i].generation, 0, __ATOMIC_RELAXED);
        g_users[i].password_iterations = 0;
        g_users[i].password_set = false;
        memset(g_users[i].password_salt,
               0,
               sizeof(g_users[i].password_salt));
        memset(g_users[i].password_hash,
               0,
               sizeof(g_users[i].password_hash));
        g_users[i].name[0] = '\0';
        g_users[i].id.machine = 0;
        g_users[i].id.local = 0;
    }
    if (g_next_user_id == 0) {
        g_next_user_id = 1;
    }
}

users::User* create_in_memory(const char* name,
                              uint64_t allowed_caps,
                              uint64_t machine) {
    sync::IrqLockGuard guard(g_user_lock);
    if (find_name_locked(name) != nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < users::kMaxUsers; ++i) {
        users::User& user = g_users[i];
        if (__atomic_load_n(&user.active, __ATOMIC_ACQUIRE)) {
            continue;
        }
        string_util::copy(user.name, sizeof(user.name), name);
        __atomic_store_n(&user.allowed_caps,
                         allowed_caps,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&user.generation, 1, __ATOMIC_RELAXED);
        user.password_iterations = 0;
        user.password_set = false;
        memset(user.password_salt, 0, sizeof(user.password_salt));
        memset(user.password_hash, 0, sizeof(user.password_hash));
        user.id.machine = machine;
        user.id.local = g_next_user_id++;
        __atomic_store_n(&user.active, true, __ATOMIC_RELEASE);
        return &user;
    }
    return nullptr;
}

}  // namespace

namespace users {

static bool persist_locked();

void init() {
    LockGuard persist_guard(g_persist_lock);
    reset_user_pool();
    if (g_machine_id.machine == 0 && path_valid()) {
        load_machine_id();
    }
}

User* create(const char* name, uint64_t allowed_caps) {
    if (!valid_user_name(name)) {
        return nullptr;
    }
    allowed_caps = capabilities::normalize_mask(allowed_caps);
    LockGuard persist_guard(g_persist_lock);
    if (g_machine_id.machine == 0 && path_valid()) {
        load_machine_id();
    }
    User* out = create_in_memory(name, allowed_caps, g_machine_id.machine);
    if (out == nullptr) {
        return nullptr;
    }
    ensure_home_directory(out->name);
    if (!persist_locked()) {
        log_message(LogLevel::Warn, "Users: persist failed after create");
    }
    return out;
}

User* find(const char* name) {
    if (name == nullptr || user_database_loading()) {
        return nullptr;
    }
    sync::IrqLockGuard guard(g_user_lock);
    return find_name_locked(name);
}

User* find(const UserId& id) {
    if (id.local == 0 || user_database_loading()) {
        return nullptr;
    }
    sync::IrqLockGuard guard(g_user_lock);
    for (size_t i = 0; i < kMaxUsers; ++i) {
        User& u = g_users[i];
        if (!__atomic_load_n(&u.active, __ATOMIC_ACQUIRE)) {
            continue;
        }
        if (u.id.machine == id.machine && u.id.local == id.local) {
            return &u;
        }
    }
    return nullptr;
}

uint64_t handle_for(const User* user) {
    if (user_database_loading()) {
        return 0;
    }
    sync::IrqLockGuard guard(g_user_lock);
    size_t index = 0;
    if (!user_index(user, index) ||
        !__atomic_load_n(&user->active, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    uint64_t generation =
        __atomic_load_n(&user->generation, __ATOMIC_ACQUIRE);
    if (generation == 0) {
        return 0;
    }
    return (generation << 32) |
           (static_cast<uint64_t>(index) + 1u);
}

User* from_handle(uint64_t handle) {
    if (user_database_loading()) {
        return nullptr;
    }
    uint32_t encoded_index = static_cast<uint32_t>(handle);
    uint64_t generation = handle >> 32;
    if (encoded_index == 0 || generation == 0) {
        return nullptr;
    }
    size_t index = static_cast<size_t>(encoded_index - 1u);
    if (index >= kMaxUsers) {
        return nullptr;
    }
    sync::IrqLockGuard guard(g_user_lock);
    User& user = g_users[index];
    if (!__atomic_load_n(&user.active, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&user.generation, __ATOMIC_ACQUIRE) != generation) {
        return nullptr;
    }
    return &user;
}

UserId machine_id() {
    LockGuard persist_guard(g_persist_lock);
    return g_machine_id;
}

void bump_generation(User& user) {
    if (user_database_loading()) {
        return;
    }
    sync::IrqLockGuard guard(g_user_lock);
    size_t index = 0;
    if (!user_index(&user, index) ||
        !__atomic_load_n(&g_users[index].active, __ATOMIC_ACQUIRE)) {
        return;
    }
    uint64_t next =
        __atomic_load_n(&g_users[index].generation, __ATOMIC_RELAXED) + 1;
    if (next == 0) {
        next = 1;
    }
    __atomic_store_n(&g_users[index].generation, next, __ATOMIC_RELEASE);
    // No persistence required; tokens are ephemeral across restarts.
}

bool allows(const User& user, uint64_t cap_bitmask) {
    uint64_t generation = 0;
    if (!active_generation(user, generation)) {
        return false;
    }
    return generation_allows(user, generation, cap_bitmask);
}

bool active_generation(const User& user, uint64_t& out_generation) {
    if (user_database_loading() ||
        !__atomic_load_n(&user.active, __ATOMIC_ACQUIRE)) {
        return false;
    }
    uint64_t generation =
        __atomic_load_n(&user.generation, __ATOMIC_ACQUIRE);
    if (generation == 0 ||
        !__atomic_load_n(&user.active, __ATOMIC_ACQUIRE)) {
        return false;
    }
    out_generation = generation;
    return true;
}

bool generation_allows(const User& user,
                       uint64_t expected_generation,
                       uint64_t cap_bitmask) {
    if (user_database_loading() || expected_generation == 0 ||
        !__atomic_load_n(&user.active, __ATOMIC_ACQUIRE)) {
        return false;
    }
    uint64_t generation_before =
        __atomic_load_n(&user.generation, __ATOMIC_ACQUIRE);
    uint64_t allowed_caps =
        __atomic_load_n(&user.allowed_caps, __ATOMIC_ACQUIRE);
    uint64_t generation_after =
        __atomic_load_n(&user.generation, __ATOMIC_ACQUIRE);
    return generation_before == expected_generation &&
           generation_after == expected_generation &&
           __atomic_load_n(&user.active, __ATOMIC_ACQUIRE) &&
           capabilities::mask_allows(allowed_caps, cap_bitmask);
}

bool snapshot_info(const User& user, UserInfo& out_info) {
    if (user_database_loading()) {
        return false;
    }
    sync::IrqLockGuard guard(g_user_lock);
    size_t index = 0;
    if (!user_index(&user, index)) {
        return false;
    }
    const User& stored = g_users[index];
    if (!__atomic_load_n(&stored.active, __ATOMIC_ACQUIRE)) {
        return false;
    }
    out_info.id_machine = stored.id.machine;
    out_info.id_local = stored.id.local;
    string_util::copy(out_info.name, sizeof(out_info.name), stored.name);
    out_info.allowed_caps =
        __atomic_load_n(&stored.allowed_caps, __ATOMIC_ACQUIRE);
    out_info.generation =
        __atomic_load_n(&stored.generation, __ATOMIC_ACQUIRE);
    out_info.password_set = stored.password_set ? 1u : 0u;
    out_info.active = 1;
    return true;
}

bool set_password(User& user,
                  const uint8_t* salt,
                  const uint8_t* hash,
                  uint32_t iterations) {
    if (user_database_loading() || salt == nullptr || hash == nullptr ||
        iterations == 0 ||
        iterations > kMaxPasswordIterations) {
        return false;
    }
    LockGuard persist_guard(g_persist_lock);
    if (user_database_loading()) {
        return false;
    }
    {
        sync::IrqLockGuard guard(g_user_lock);
        size_t index = 0;
        if (!user_index(&user, index) ||
            !__atomic_load_n(&g_users[index].active, __ATOMIC_ACQUIRE)) {
            return false;
        }
        User& stored = g_users[index];
        memcpy(stored.password_salt, salt, sizeof(stored.password_salt));
        memcpy(stored.password_hash, hash, sizeof(stored.password_hash));
        stored.password_iterations = iterations;
        stored.password_set = true;
    }
    return persist_locked();
}

void set_storage_path(const char* path) {
    LockGuard persist_guard(g_persist_lock);
    if (path == nullptr) {
        g_has_storage_path = false;
        g_storage_path[0] = '\0';
        g_storage_path_fallback[0] = '\0';
        return;
    }
    string_util::copy(g_storage_path, sizeof(g_storage_path), path);
    // Build fallback without the /system/ component.
    g_storage_path_fallback[0] = '\0';
    const char* system_pos = nullptr;
    for (size_t i = 0; g_storage_path[i] != '\0'; ++i) {
        const char* tail = g_storage_path + i;
        const char match[] = "/system/";
        size_t m = 0;
        while (match[m] != '\0' && tail[m] == match[m]) {
            ++m;
        }
        if (match[m] == '\0') {
            system_pos = g_storage_path + i;
            break;
        }
    }
    if (system_pos != nullptr) {
        size_t prefix = static_cast<size_t>(system_pos - g_storage_path);
        if (prefix + 1 < sizeof(g_storage_path_fallback)) {
            for (size_t i = 0; i < prefix; ++i) {
                g_storage_path_fallback[i] = g_storage_path[i];
            }
            size_t idx = prefix;
            g_storage_path_fallback[idx++] = '/';
            const char suffix[] = "users.ntd";
            string_util::copy(g_storage_path_fallback + idx,
                              sizeof(g_storage_path_fallback) - idx,
                              suffix);
        }
    }
    g_has_storage_path = (g_storage_path[0] != '\0');
}

static bool protect_store_acl_locked(const char* path) {
    if (!vfs::acl_supported(path)) {
        return true;
    }
    UserId root_id{};
    {
        sync::IrqLockGuard guard(g_user_lock);
        User* root = find_name_locked("root");
        if (root != nullptr) {
            root_id = root->id;
        }
    }
    if (root_id.local == 0) {
        return false;
    }
    vfs::AclEntry acl{};
    acl.machine_id = root_id.machine;
    acl.local_id = root_id.local;
    acl.write = vfs::AclValue::Allow;
    acl.read = vfs::AclValue::Allow;
    acl.delete_permission = vfs::AclValue::Allow;
    acl.edit = vfs::AclValue::Allow;
    return vfs::set_acl(path, &acl, 1);
}

static bool persist_locked() {
    if (!path_valid()) {
        return false;
    }
    if (g_machine_id.machine == 0) {
        load_machine_id();
    }
    ensure_parent_directories(g_storage_path);
    if (g_storage_path_fallback[0] != '\0') {
        ensure_parent_directories(g_storage_path_fallback);
    }
    UserStore store{};
    Header& hdr = store.header;
    hdr.magic = kMagic;
    hdr.version = kVersion;
    hdr.entry_size = static_cast<uint16_t>(sizeof(PackedUser));
    hdr.count = 0;

    {
        sync::IrqLockGuard guard(g_user_lock);
        hdr.next_user_id = g_next_user_id;
        hdr.machine_id = g_machine_id.machine;
        for (size_t i = 0; i < kMaxUsers; ++i) {
            const User& user = g_users[i];
            if (!__atomic_load_n(&user.active, __ATOMIC_ACQUIRE)) {
                continue;
            }
            PackedUser& packed_user = store.users[hdr.count++];
            string_util::copy(packed_user.name,
                              sizeof(packed_user.name),
                              user.name);
            packed_user.allowed_caps =
                __atomic_load_n(&user.allowed_caps, __ATOMIC_ACQUIRE);
            packed_user.generation =
                __atomic_load_n(&user.generation, __ATOMIC_ACQUIRE);
            packed_user.active = 1;
            packed_user.password_set = user.password_set ? 1 : 0;
            packed_user.password_algorithm = user.password_set ? 1 : 0;
            packed_user.password_iterations = user.password_iterations;
            memcpy(packed_user.password_salt,
                   user.password_salt,
                   sizeof(packed_user.password_salt));
            memcpy(packed_user.password_hash,
                   user.password_hash,
                   sizeof(packed_user.password_hash));
            packed_user.machine_id = user.id.machine;
            packed_user.local_id = user.id.local;
        }
    }

    size_t total = sizeof(Header) + hdr.count * sizeof(PackedUser);

    auto write_path = [&](const char* p) -> bool {
        vfs::FileHandle handle{};
        if (!vfs::create_file(p, handle)) {
            if (!vfs::open_file(p, handle)) {
                return false;
            }
        }
        size_t written = 0;
        bool ok = vfs::write_file(
            handle,
            0,
            reinterpret_cast<const uint8_t*>(&store),
            total,
            written);
        vfs::close_file(handle);
        if (!ok || written != total) {
            return false;
        }
        return protect_store_acl_locked(p);
    };

    if (write_path(g_storage_path)) {
        return true;
    }

    if (g_storage_path_fallback[0] != '\0' &&
        write_path(g_storage_path_fallback)) {
        log_message(LogLevel::Info,
                    "Users: wrote store to fallback path '%s' (primary '%s' missing?)",
                    g_storage_path_fallback, g_storage_path);
        return true;
    }

    log_message(LogLevel::Warn,
                "Users: persist failed for '%s'%s",
                g_storage_path,
                (g_storage_path_fallback[0] != '\0') ? " and fallback" : "");
    return false;
}

bool persist() {
    LockGuard persist_guard(g_persist_lock);
    return persist_locked();
}

bool load_from_disk() {
    LockGuard persist_guard(g_persist_lock);
    if (!path_valid()) {
        return false;
    }
    UserLoadGuard load_guard;
    load_machine_id();

    uint8_t buffer[kUserStoreBufferSize];
    size_t read_size = 0;
    const char* loaded_path = nullptr;
    auto try_load = [&](const char* p) -> bool {
        LegacyHeader raw_hdr{};
        vfs::FileHandle h{};
        size_t local_read_size = 0;
        if (!vfs::open_file(p, h)) {
            return false;
        }
        bool ok = vfs::read_file(h, 0, buffer, sizeof(buffer), local_read_size);
        vfs::close_file(h);
        if (!ok) {
            return false;
        }
        if (local_read_size < sizeof(LegacyHeader)) {
            return false;
        }
        memcpy(&raw_hdr, buffer, sizeof(LegacyHeader));
        if (raw_hdr.magic != kMagic) {
            return false;
        }
        bool current = raw_hdr.version == kVersion &&
                       raw_hdr.entry_size == sizeof(PackedUser);
        bool legacy = raw_hdr.version == kLegacyVersion &&
                      raw_hdr.entry_size == sizeof(PackedUser);
        bool legacy_v1 = raw_hdr.version == kLegacyVersion1 &&
                         raw_hdr.entry_size == sizeof(PackedUserV1);
        if (!current && !legacy && !legacy_v1) {
            return false;
        }
        if (raw_hdr.count > kMaxUsers) {
            return false;
        }
        size_t expected = sizeof(LegacyHeader) +
                          static_cast<size_t>(raw_hdr.count) * raw_hdr.entry_size;
        if (current) {
            expected = sizeof(Header) +
                       static_cast<size_t>(raw_hdr.count) * raw_hdr.entry_size;
        }
        if (local_read_size < expected) {
            return false;
        }
        read_size = local_read_size;
        loaded_path = p;
        return true;
    };

    bool loaded = try_load(g_storage_path);
    if (!loaded && g_storage_path_fallback[0] != '\0') {
        loaded = try_load(g_storage_path_fallback);
        if (loaded) {
            log_message(LogLevel::Warn,
                        "Users: loaded from fallback path '%s'",
                        g_storage_path_fallback);
        }
    }
    if (!loaded) {
        // Missing and malformed credential stores must remain distinguishable
        // from an explicitly provisioned empty store.  In particular, do not
        // overwrite corruption with a passwordless bootstrap database.
        return false;
    }
    if (read_size < sizeof(LegacyHeader)) {
        return false;
    }

    LegacyHeader raw_hdr{};
    memcpy(&raw_hdr, buffer, sizeof(LegacyHeader));
    Header hdr{};
    if (raw_hdr.version == kVersion) {
        if (read_size < sizeof(Header)) {
            return false;
        }
        memcpy(&hdr, buffer, sizeof(Header));
        if (hdr.machine_id == 0) {
            hdr.machine_id = g_machine_id.machine;
        }
    } else {
        hdr.magic = raw_hdr.magic;
        hdr.version = raw_hdr.version;
        hdr.entry_size = raw_hdr.entry_size;
        hdr.count = raw_hdr.count;
        hdr.next_user_id = 1;
        hdr.machine_id = g_machine_id.machine;
    }

    size_t entries_offset = (hdr.version == kVersion)
                                ? sizeof(Header)
                                : sizeof(LegacyHeader);
    for (size_t i = 0; i < hdr.count; ++i) {
        char name[sizeof(User::name)];
        const char* packed_name = nullptr;
        const PackedUser* packed_user = nullptr;
        if (hdr.version == kLegacyVersion1) {
            const auto* entries = reinterpret_cast<const PackedUserV1*>(
                buffer + entries_offset);
            packed_name = entries[i].name;
        } else {
            const auto* entries = reinterpret_cast<const PackedUser*>(
                buffer + entries_offset);
            packed_user = &entries[i];
            packed_name = packed_user->name;
        }
        if (!copy_packed_user_name(
                packed_name, sizeof(PackedUser::name), name)) {
            return false;
        }
        if (packed_user != nullptr && packed_user->password_set != 0 &&
            (packed_user->password_algorithm != 1 ||
             packed_user->password_iterations == 0 ||
             packed_user->password_iterations > kMaxPasswordIterations)) {
            return false;
        }
    }

    reset_user_pool();
    {
        sync::IrqLockGuard guard(g_user_lock);
        g_next_user_id = (hdr.next_user_id == 0) ? 1 : hdr.next_user_id;
    }
    bool upgraded_legacy_entries = false;
    if (hdr.version == kLegacyVersion1) {
        const PackedUserV1* entries =
            reinterpret_cast<const PackedUserV1*>(buffer + sizeof(LegacyHeader));
        for (size_t i = 0; i < hdr.count && i < kMaxUsers; ++i) {
            const PackedUserV1& p = entries[i];
            char name[sizeof(User::name)];
            if (!copy_packed_user_name(p.name, sizeof(p.name), name)) {
                continue;
            }
            uint64_t allowed_caps =
                normalize_stored_caps(p.allowed_caps,
                                      true,
                                      upgraded_legacy_entries);
            User* u =
                create_in_memory(name, allowed_caps, g_machine_id.machine);
            if (u != nullptr) {
                {
                    sync::IrqLockGuard guard(g_user_lock);
                    __atomic_store_n(&u->active, false, __ATOMIC_RELEASE);
                    __atomic_store_n(&u->generation,
                                     p.generation,
                                     __ATOMIC_RELAXED);
                    if (u->id.local == 0) {
                        u->id.local = g_next_user_id++;
                    }
                    if (u->id.machine == 0) {
                        u->id.machine = hdr.machine_id;
                    }
                    __atomic_store_n(&u->active,
                                     p.active != 0,
                                     __ATOMIC_RELEASE);
                }
                ensure_home_directory(u->name);
            }
        }
    } else {
        const PackedUser* entries =
            reinterpret_cast<const PackedUser*>(buffer +
                                               ((hdr.version == kVersion)
                                                    ? sizeof(Header)
                                                    : sizeof(LegacyHeader)));
        for (size_t i = 0; i < hdr.count && i < kMaxUsers; ++i) {
            const PackedUser& p = entries[i];
            char name[sizeof(User::name)];
            if (!copy_packed_user_name(p.name, sizeof(p.name), name)) {
                continue;
            }
            uint64_t allowed_caps =
                normalize_stored_caps(p.allowed_caps,
                                      hdr.version != kVersion,
                                      upgraded_legacy_entries);
            User* u =
                create_in_memory(name, allowed_caps, g_machine_id.machine);
            if (u != nullptr) {
                {
                    sync::IrqLockGuard guard(g_user_lock);
                    __atomic_store_n(&u->active, false, __ATOMIC_RELEASE);
                    __atomic_store_n(&u->generation,
                                     p.generation,
                                     __ATOMIC_RELAXED);
                    if (p.password_set != 0 && p.password_algorithm == 1 &&
                        p.password_iterations != 0) {
                        u->password_set = true;
                        u->password_iterations = p.password_iterations;
                        memcpy(u->password_salt,
                               p.password_salt,
                               sizeof(u->password_salt));
                        memcpy(u->password_hash,
                               p.password_hash,
                               sizeof(u->password_hash));
                    }
                    u->id.machine =
                        (p.machine_id != 0) ? p.machine_id : hdr.machine_id;
                    u->id.local = p.local_id;
                    if (u->id.local == 0) {
                        u->id.local = g_next_user_id++;
                    }
                    if (u->id.machine == 0) {
                        u->id.machine = hdr.machine_id;
                    }
                    if (u->id.local >= g_next_user_id) {
                        g_next_user_id = u->id.local + 1;
                    }
                    __atomic_store_n(&u->active,
                                     p.active != 0,
                                     __ATOMIC_RELEASE);
                }
                ensure_home_directory(u->name);
            }
        }
    }

    log_message(LogLevel::Info,
                "Users: loaded %u entr%s from '%s'",
                static_cast<unsigned int>(hdr.count),
                (hdr.count == 1) ? "y" : "ies",
                (loaded_path != nullptr) ? loaded_path : "(unknown)");
    if (upgraded_legacy_entries || hdr.version != kVersion) {
        log_message(LogLevel::Info,
                    "Users: upgraded legacy user store to current format");
    }
    if (upgraded_legacy_entries || hdr.version != kVersion) {
        persist_locked();
    }
    if (!protect_store_acl_locked(loaded_path)) {
        log_message(LogLevel::Error,
                    "Users: failed to protect store ACL for '%s'",
                    (loaded_path != nullptr) ? loaded_path : "(unknown)");
        return false;
    }
    return true;
}

}  // namespace users
