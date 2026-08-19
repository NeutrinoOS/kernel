#pragma once

#include <stddef.h>
#include <stdint.h>

namespace capabilities {

using CapabilityMask = uint64_t;
constexpr CapabilityMask kFullPermissions = ~CapabilityMask{0};

// Compile-time capability kinds understood by the kernel.
enum class CapabilityKind : uint16_t {
    SystemSettings = 0,
    SystemPower = 1,
    FilesystemMount = 2,
    StorageRawRead = 3,
    StorageRawWrite = 4,
    StorageManage = 5,
    ProcessSpawn = 6,
    ProcessInspect = 7,
    ProcessControl = 8,
    ProcessTrace = 9,
    IdentityManage = 10,
    ModuleLoad = 11,
    GraphicalSession = 12,
    InputDevices = 13,
    Audio = 14,
    Network = 15,
    NetworkManage = 16,
    Serial = 17,
    Pci = 18,
    SystemMonitor = 19,
    KernelLog = 20,
    FilesystemOverride = 21,
    Count,
};

static_assert(static_cast<uint16_t>(CapabilityKind::Count) <= 64,
              "capability mask exceeds its 64-bit storage");

struct Principal {
    void* backing_user;  // optional user pointer (users::User*), may be null
    uint64_t backing_generation_snapshot;
    uint64_t generation;
    CapabilityMask allowed_caps;
    uint32_t refcount;
    bool active;
};

struct CapabilityToken {
    Principal* issuer;
    CapabilityKind kind;
    uint64_t generation_snapshot;
    uint32_t refcount;
};

// Per-process handle table entry. Handles are process-local opaque 64-bit ids.
struct CapHandleEntry {
    bool in_use;
    uint64_t handle;
    CapabilityToken* token;
};

constexpr size_t kMaxPrincipals = 64;
constexpr size_t kMaxCapabilityTokens = 256;
constexpr size_t kMaxProcessCapabilities = 32;

void init();

Principal* create_principal(void* backing_user, CapabilityMask allowed_caps);
bool principal_add_ref(Principal* principal);
void principal_release(Principal* principal);
void principal_bump_generation(Principal& principal);
bool principal_allows(const Principal& principal, CapabilityKind kind);
bool principal_is_valid(const Principal* principal);
bool principal_user_id(const Principal* principal,
                       uint64_t& out_machine_id,
                       uint64_t& out_local_id);
Principal* principal_from_handle(uint64_t handle);
// Resolve a handle and retain the principal as one atomic pool operation.
Principal* principal_acquire_from_handle(uint64_t handle);
uint64_t principal_handle(const Principal* principal);
bool capability_from_value(uint64_t value, CapabilityKind& out_kind);
inline uint64_t capability_bit(CapabilityKind kind) {
    return 1ull << static_cast<uint16_t>(kind);
}

inline CapabilityMask normalize_mask(CapabilityMask mask) {
    constexpr CapabilityMask kCurrentAllCapabilities =
        (1ull << static_cast<uint16_t>(CapabilityKind::Count)) - 1;
    return (mask == kCurrentAllCapabilities) ? kFullPermissions : mask;
}

inline bool mask_allows(CapabilityMask mask, CapabilityMask requested_bits) {
    return mask == kFullPermissions ||
           (mask & requested_bits) == requested_bits;
}

inline bool principal_allows_or_unconfined(const Principal* principal,
                                           CapabilityKind kind) {
    return principal == nullptr || principal_allows(*principal, kind);
}

CapabilityToken* issue_token(Principal& issuer, CapabilityKind kind);
void discard_unreferenced_token(CapabilityToken* token);
bool token_valid(const CapabilityToken& token);

bool cap_table_insert(CapHandleEntry* table,
                      size_t capacity,
                      CapabilityToken* token,
                      uint64_t& out_handle);
CapabilityToken* cap_table_lookup(CapHandleEntry* table,
                                  size_t capacity,
                                  uint64_t handle,
                                  bool invalidate_if_stale);
void cap_table_clear(CapHandleEntry* table, size_t capacity);
bool cap_table_copy_handles(CapHandleEntry* dest,
                            size_t dest_capacity,
                            CapHandleEntry* src,
                            size_t src_capacity,
                            const uint64_t* handles,
                            size_t handle_count);

}  // namespace capabilities
