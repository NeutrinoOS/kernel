#include "kernel/descriptor.hpp"

#include "drivers/log/logging.hpp"
#include "kernel/process.hpp"
#include "kernel/string_util.hpp"
#include "kernel/sync.hpp"
#include "kernel/vm.hpp"

namespace descriptor {
namespace service_registry_descriptor {

constexpr size_t kMaxOffers = 32;
constexpr size_t kMaxLookups = 64;

struct Offer {
    bool in_use;
    char service[descriptor_defs::kServiceIdLength];
    char provider[descriptor_defs::kServiceProviderLength];
    uint32_t abi_version;
    uint32_t pipe_id;
    uint32_t provider_pid;
    DescriptorEntry* registrar;
};

struct LookupState {
    bool in_use;
    descriptor_defs::ServiceBinding binding;
};

Offer g_offers[kMaxOffers]{};
LookupState g_lookups[kMaxLookups]{};
sync::SpinLock g_lock;

bool valid_service_id(const char* service) {
    if (service == nullptr || service[0] == '\0') {
        return false;
    }
    for (size_t i = 0; service[i] != '\0'; ++i) {
        char ch = service[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                  ch == '.' || ch == '-' || ch == '_';
        if (!ok || i + 1 >= descriptor_defs::kServiceIdLength) {
            return false;
        }
    }
    return true;
}

Offer* find_offer_locked(const char* service, uint32_t abi_version) {
    for (auto& offer : g_offers) {
        if (offer.in_use && offer.abi_version == abi_version &&
            string_util::equals(offer.service, service)) {
            return &offer;
        }
    }
    return nullptr;
}

void copy_binding(const Offer& offer, descriptor_defs::ServiceBinding& out) {
    string_util::copy(out.service, sizeof(out.service), offer.service);
    string_util::copy(out.provider, sizeof(out.provider), offer.provider);
    out.abi_version = offer.abi_version;
    out.pipe_id = offer.pipe_id;
    out.provider_pid = offer.provider_pid;
    out.reserved = 0;
}

void drop_registrar_offers(DescriptorEntry& registrar) {
    sync::LockGuard guard(g_lock);
    for (auto& offer : g_offers) {
        if (offer.in_use && offer.registrar == &registrar) {
            offer.in_use = false;
            offer.service[0] = '\0';
            offer.provider[0] = '\0';
            offer.abi_version = 0;
            offer.pipe_id = 0;
            offer.provider_pid = 0;
            offer.registrar = nullptr;
        }
    }
}

int64_t read(process::Task&, DescriptorEntry&, uint64_t, uint64_t, uint64_t) {
    return -1;
}

int64_t write(process::Task&, DescriptorEntry&, uint64_t, uint64_t, uint64_t) {
    return -1;
}

int get_property(DescriptorEntry& entry,
                 uint32_t property,
                 void* out,
                 size_t size) {
    if (property !=
            static_cast<uint32_t>(descriptor_defs::Property::ServiceBinding) ||
        out == nullptr || size < sizeof(descriptor_defs::ServiceBinding)) {
        return -1;
    }
    auto* state = static_cast<LookupState*>(entry.subsystem_data);
    if (state == nullptr) {
        return -1;
    }
    *static_cast<descriptor_defs::ServiceBinding*>(out) = state->binding;
    return 0;
}

int set_property(DescriptorEntry& entry,
                 uint32_t property,
                 const void* in,
                 size_t size) {
    if (entry.subsystem_data != nullptr) {
        return -1;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::ServiceRegister)) {
        if (in == nullptr || size < sizeof(descriptor_defs::ServiceOffer)) {
            return -1;
        }
        const auto& offer =
            *static_cast<const descriptor_defs::ServiceOffer*>(in);
        if (!valid_service_id(offer.service) || offer.pipe_id == 0) {
            return -1;
        }
        auto* proc = process::current();
        uint32_t pid = proc != nullptr ? proc->pid : 0;
        sync::LockGuard guard(g_lock);
        if (find_offer_locked(offer.service, offer.abi_version) != nullptr) {
            return -1;
        }
        Offer* slot = nullptr;
        for (auto& candidate : g_offers) {
            if (!candidate.in_use) {
                slot = &candidate;
                break;
            }
        }
        if (slot == nullptr) {
            return -1;
        }
        slot->in_use = true;
        string_util::copy(slot->service, sizeof(slot->service), offer.service);
        string_util::copy(slot->provider, sizeof(slot->provider), offer.provider);
        slot->abi_version = offer.abi_version;
        slot->pipe_id = offer.pipe_id;
        slot->provider_pid = pid;
        slot->registrar = &entry;
        log_message(LogLevel::Info,
                    "Service: registered %s abi %u pipe %u (%s pid %u)",
                    slot->service,
                    static_cast<unsigned int>(slot->abi_version),
                    static_cast<unsigned int>(slot->pipe_id),
                    slot->provider[0] != '\0' ? slot->provider : "anonymous",
                    static_cast<unsigned int>(pid));
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::ServiceUnregister)) {
        if (in == nullptr || size < sizeof(descriptor_defs::ServiceQuery)) {
            return -1;
        }
        const auto& query =
            *static_cast<const descriptor_defs::ServiceQuery*>(in);
        sync::LockGuard guard(g_lock);
        Offer* offer = find_offer_locked(query.service, query.abi_version);
        if (offer == nullptr || offer->registrar != &entry) {
            return -1;
        }
        offer->in_use = false;
        offer->registrar = nullptr;
        return 0;
    }
    return -1;
}

void close_registrar(DescriptorEntry& entry) {
    drop_registrar_offers(entry);
}

void close_lookup(DescriptorEntry& entry) {
    auto* state = static_cast<LookupState*>(entry.subsystem_data);
    if (state != nullptr) {
        state->binding = {};
        state->in_use = false;
        entry.subsystem_data = nullptr;
    }
}

const Ops kRegistrarOps{
    .read = read,
    .write = write,
    .get_property = nullptr,
    .set_property = set_property,
};

const Ops kLookupOps{
    .read = read,
    .write = write,
    .get_property = get_property,
    .set_property = nullptr,
};

bool open(process::Task& proc,
          uint64_t name_ptr,
          uint64_t abi_version,
          uint64_t,
          Allocation& allocation) {
    if (name_ptr == 0) {
        allocation.type = kTypeServiceRegistry;
        allocation.flags = static_cast<uint64_t>(Flag::Writable);
        allocation.extended_flags = 0;
        allocation.has_extended_flags = false;
        allocation.object = nullptr;
        allocation.subsystem_data = nullptr;
        allocation.name = "service-registrar";
        allocation.ops = &kRegistrarOps;
        allocation.ext = nullptr;
        allocation.close = close_registrar;
        return true;
    }

    char service[descriptor_defs::kServiceIdLength];
    if (!vm::copy_user_string(proc.cr3,
                              reinterpret_cast<const char*>(name_ptr),
                              service,
                              sizeof(service)) ||
        !valid_service_id(service)) {
        return false;
    }

    descriptor_defs::ServiceBinding binding{};
    {
        sync::LockGuard guard(g_lock);
        Offer* offer =
            find_offer_locked(service, static_cast<uint32_t>(abi_version));
        if (offer == nullptr) {
            return false;
        }
        copy_binding(*offer, binding);
    }

    LookupState* state = nullptr;
    {
        sync::LockGuard guard(g_lock);
        for (auto& candidate : g_lookups) {
            if (!candidate.in_use) {
                state = &candidate;
                break;
            }
        }
        if (state != nullptr) {
            state->in_use = true;
            state->binding = binding;
        }
    }
    if (state == nullptr) {
        return false;
    }
    allocation.type = kTypeServiceRegistry;
    allocation.flags = static_cast<uint64_t>(Flag::Readable);
    allocation.extended_flags = 0;
    allocation.has_extended_flags = false;
    allocation.object = nullptr;
    allocation.subsystem_data = state;
    allocation.name = "service-binding";
    allocation.ops = &kLookupOps;
    allocation.ext = nullptr;
    allocation.close = close_lookup;
    return true;
}

}  // namespace service_registry_descriptor

bool register_service_registry_descriptor() {
    return register_type(kTypeServiceRegistry,
                         service_registry_descriptor::open,
                         &service_registry_descriptor::kRegistrarOps);
}

}  // namespace descriptor
