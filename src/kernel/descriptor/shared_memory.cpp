#include "../descriptor.hpp"

#include "arch/x86_64/memory/paging.hpp"
#include "../../drivers/log/logging.hpp"
#include "../../lib/mem.hpp"
#include "../process.hpp"
#include "../memory/physical_allocator.hpp"
#include "../string_util.hpp"
#include "../vm.hpp"

namespace descriptor {

namespace shared_memory_descriptor {

constexpr size_t kMaxSegments = 32;
constexpr size_t kMaxNameLength = 48;
constexpr size_t kDefaultSegmentSize = 0x1000;
constexpr size_t kPageSize = 0x1000;
constexpr size_t kMaxSegmentPages = 4096;  // Allow larger shared buffers (e.g., full-screen surfaces).

struct SegmentMapping {
    process::Process* proc;
    vm::Region region;
    uint32_t refcount;
};

struct SharedSegment {
    bool in_use;
    char name[kMaxNameLength];
    size_t length;
    size_t page_count;
    uint64_t pages[kMaxSegmentPages];
    SegmentMapping mappings[process::kMaxProcesses];
    uint32_t refcount;
};

SharedSegment g_segments[kMaxSegments];
volatile int g_segments_lock = 0;

void lock_segments() {
    while (__atomic_test_and_set(&g_segments_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

void unlock_segments() {
    __atomic_clear(&g_segments_lock, __ATOMIC_RELEASE);
}

class SegmentGuard {
public:
    SegmentGuard() { lock_segments(); }
    ~SegmentGuard() { unlock_segments(); }
};

void reset_segment(SharedSegment& segment) {
    segment.in_use = false;
    segment.name[0] = '\0';
    segment.length = 0;
    segment.page_count = 0;
    for (auto& mapping : segment.mappings) {
        mapping.proc = nullptr;
        mapping.region = vm::Region{0, 0};
        mapping.refcount = 0;
    }
    segment.refcount = 0;
}

SharedSegment* find_segment_locked(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return nullptr;
    }
    for (auto& segment : g_segments) {
        if (!segment.in_use) {
            continue;
        }
        if (string_util::equals(segment.name, name)) {
            return &segment;
        }
    }
    return nullptr;
}

SegmentMapping* find_mapping(SharedSegment& segment, process::Process& proc) {
    for (auto& mapping : segment.mappings) {
        if (mapping.proc == &proc) {
            return &mapping;
        }
    }
    return nullptr;
}

SegmentMapping* allocate_mapping(SharedSegment& segment,
                                 process::Process& proc) {
    for (auto& mapping : segment.mappings) {
        if (mapping.proc == nullptr) {
            mapping.proc = &proc;
            mapping.region = vm::Region{0, 0};
            mapping.refcount = 0;
            return &mapping;
        }
    }
    return nullptr;
}

bool map_segment_into_process(SharedSegment& segment,
                              process::Process& proc,
                              SegmentMapping& mapping) {
    if (segment.length == 0 || segment.page_count == 0) {
        return false;
    }
    if (proc.cr3 == 0) {
        return false;
    }
    if (mapping.region.base == 0 || mapping.region.length == 0) {
        mapping.region = vm::reserve_user_region(
            proc.cr3,
            segment.length,
            vm::MappingKind::Shared);
        if (mapping.region.base == 0 || mapping.region.length < segment.length) {
            mapping.region = vm::Region{0, 0};
            return false;
        }
    }
    for (size_t i = 0; i < segment.page_count; ++i) {
        uint64_t virt = mapping.region.base + (i * kPageSize);
        uint64_t phys = segment.pages[i];
        if (!paging_map_page_cr3(proc.cr3,
                                 virt,
                                 phys,
                                 PAGE_FLAG_WRITE | PAGE_FLAG_USER |
                                     PAGE_FLAG_NO_EXECUTE)) {
            log_message(LogLevel::Error,
                        "SHM map failed pid=%u virt=%llx phys=%llx",
                        static_cast<unsigned int>(proc.pid),
                        static_cast<unsigned long long>(virt),
                        static_cast<unsigned long long>(phys));
            for (size_t rollback = 0; rollback < i; ++rollback) {
                uint64_t ignored = 0;
                (void)paging_unmap_page_cr3(
                    proc.cr3,
                    mapping.region.base + (rollback * kPageSize),
                    ignored);
            }
            vm::release_external_region(proc.cr3, mapping.region);
            mapping.region = vm::Region{0, 0};
            return false;
        }
    }
    if (!vm::mark_region_resident(proc.cr3,
                                  mapping.region,
                                  segment.page_count)) {
        for (size_t i = 0; i < segment.page_count; ++i) {
            uint64_t ignored = 0;
            (void)paging_unmap_page_cr3(
                proc.cr3,
                mapping.region.base + (i * kPageSize),
                ignored);
        }
        vm::release_external_region(proc.cr3, mapping.region);
        mapping.region = vm::Region{0, 0};
        return false;
    }
    return true;
}

void unmap_segment_from_process(SharedSegment& segment,
                                process::Process& proc,
                                SegmentMapping& mapping) {
    if (mapping.region.base == 0 || segment.page_count == 0 || proc.cr3 == 0) {
        return;
    }
    for (size_t i = 0; i < segment.page_count; ++i) {
        uint64_t virt = mapping.region.base + (i * kPageSize);
        uint64_t phys = 0;
        paging_unmap_page_cr3(proc.cr3, virt, phys);
    }
    vm::release_external_region(proc.cr3, mapping.region);
}

void release_segment_pages(SharedSegment& segment) {
    for (size_t i = 0; i < segment.page_count; ++i) {
        memory::free_user_page(segment.pages[i]);
    }
    segment.page_count = 0;
}

SharedSegment* allocate_segment_locked(const char* name,
                                       size_t requested_length) {
    size_t length = (requested_length == 0) ? kDefaultSegmentSize
                                            : requested_length;
    size_t padded = static_cast<size_t>((length + kPageSize - 1) & ~(kPageSize - 1));
    size_t pages = padded / kPageSize;
    if (pages == 0 || pages > kMaxSegmentPages) {
        log_message(LogLevel::Warn,
                    "SharedMemory: request %zu bytes (%zu pages) exceeds limit %zu",
                    length,
                    pages,
                    kMaxSegmentPages);
        return nullptr;
    }
    SharedSegment* slot = nullptr;
    for (auto& segment : g_segments) {
        if (!segment.in_use) {
            slot = &segment;
            break;
        }
    }
    if (slot == nullptr) {
        log_message(LogLevel::Warn,
                    "SharedMemory: no free segment slots for '%s'",
                    name);
        return nullptr;
    }

    reset_segment(*slot);
    slot->in_use = true;
    slot->length = padded;
    slot->page_count = pages;
    for (size_t i = 0; i < pages; ++i) {
        uint64_t phys = memory::alloc_user_page();
        if (phys == 0) {
            log_message(LogLevel::Warn,
                        "SharedMemory: alloc_user_page failed at %zu/%zu",
                        i + 1,
                        pages);
            for (size_t j = 0; j < i; ++j) {
                memory::free_user_page(slot->pages[j]);
            }
            reset_segment(*slot);
            return nullptr;
        }
        auto* page = static_cast<uint8_t*>(paging_phys_to_virt(phys));
        memset(page, 0, kPageSize);
        slot->pages[i] = phys;
    }
    string_util::copy(slot->name, sizeof(slot->name), name);
    return slot;
}

int64_t shared_memory_read(process::Process&,
                           DescriptorEntry& entry,
                           uint64_t user_address,
                           uint64_t length,
                           uint64_t offset) {
    auto* segment = static_cast<SharedSegment*>(entry.object);
    if (segment == nullptr || !segment->in_use) {
        return -1;
    }
    auto* mapping = static_cast<SegmentMapping*>(entry.subsystem_data);
    if (mapping == nullptr || mapping->region.base == 0 ||
        segment->length == 0 || user_address == 0) {
        return -1;
    }
    if (offset >= segment->length) {
        return 0;
    }
    uint64_t remaining = segment->length - offset;
    uint64_t to_copy = (length > remaining) ? remaining : length;
    if (to_copy == 0) {
        return 0;
    }
    process::Process* proc = process::current();
    if (proc == nullptr || proc->cr3 == 0 ||
        !vm::is_user_range(user_address, to_copy)) {
        return -1;
    }
    if (!vm::copy_to_user(proc->cr3,
                          user_address,
                          reinterpret_cast<const void*>(mapping->region.base + offset),
                          static_cast<size_t>(to_copy))) {
        return -1;
    }
    return static_cast<int64_t>(to_copy);
}

int64_t shared_memory_write(process::Process& proc,
                            DescriptorEntry& entry,
                            uint64_t user_address,
                            uint64_t length,
                            uint64_t offset) {
    auto* segment = static_cast<SharedSegment*>(entry.object);
    if (segment == nullptr || !segment->in_use) {
        return -1;
    }
    auto* mapping = static_cast<SegmentMapping*>(entry.subsystem_data);
    if (mapping == nullptr || mapping->region.base == 0 ||
        segment->length == 0 || user_address == 0) {
        return -1;
    }
    if (offset >= segment->length) {
        return 0;
    }
    uint64_t remaining = segment->length - offset;
    uint64_t to_copy = (length > remaining) ? remaining : length;
    if (to_copy == 0) {
        return 0;
    }
    if (proc.cr3 == 0 || !vm::is_user_range(user_address, to_copy)) {
        return -1;
    }
    if (!vm::copy_from_user(proc.cr3,
                            reinterpret_cast<void*>(mapping->region.base + offset),
                            user_address,
                            static_cast<size_t>(to_copy))) {
        return -1;
    }
    return static_cast<int64_t>(to_copy);
}

int shared_memory_get_property(DescriptorEntry& entry,
                               uint32_t property,
                               void* out,
                               size_t size) {
    auto* segment = static_cast<SharedSegment*>(entry.object);
    if (segment == nullptr || !segment->in_use) {
        return -1;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::SharedMemoryInfo)) {
        if (out == nullptr || size < sizeof(descriptor_defs::SharedMemoryInfo)) {
            return -1;
        }
        if (!vm::is_user_range(reinterpret_cast<uint64_t>(out),
                               sizeof(descriptor_defs::SharedMemoryInfo))) {
            return -1;
        }
        process::Process* proc = process::current();
        if (proc == nullptr || proc->cr3 == 0) {
            return -1;
        }
        auto* mapping = static_cast<SegmentMapping*>(entry.subsystem_data);
        if (mapping == nullptr || mapping->region.base == 0) {
            return -1;
        }
        descriptor_defs::SharedMemoryInfo info{};
        info.base = mapping->region.base;
        info.length = segment->length;
        if (!vm::copy_to_user(proc->cr3,
                              reinterpret_cast<uint64_t>(out),
                              &info,
                              sizeof(info))) {
            return -1;
        }
        return 0;
    }
    return -1;
}

void shared_memory_close(DescriptorEntry& entry) {
    auto* segment = static_cast<SharedSegment*>(entry.object);
    if (segment == nullptr) {
        return;
    }
    SegmentGuard guard;
    auto* mapping = static_cast<SegmentMapping*>(entry.subsystem_data);
    if (mapping != nullptr && mapping->proc != nullptr) {
        if (mapping->refcount > 0) {
            --mapping->refcount;
        }
        if (mapping->refcount == 0) {
            unmap_segment_from_process(*segment, *mapping->proc, *mapping);
            mapping->proc = nullptr;
            mapping->region = vm::Region{0, 0};
        }
    }
    if (segment->refcount > 0) {
        --segment->refcount;
    }
    if (segment->refcount == 0 && segment->in_use) {
        release_segment_pages(*segment);
        reset_segment(*segment);
    }
}

const Ops kSharedMemoryOps{
    .read = shared_memory_read,
    .write = shared_memory_write,
    .get_property = shared_memory_get_property,
    .set_property = nullptr,
};

bool open_shared_memory(process::Process& proc,
                        uint64_t name_ptr,
                        uint64_t length,
                        uint64_t,
                        Allocation& alloc) {
    if (name_ptr == 0) {
        return false;
    }
    const char* user_name = reinterpret_cast<const char*>(name_ptr);
    char name_buffer[kMaxNameLength];
    if (!vm::copy_user_string(proc.cr3,
                              user_name,
                              name_buffer,
                              sizeof(name_buffer))) {
        return false;
    }
    if (name_buffer[0] == '\0') {
        return false;
    }
    size_t requested = static_cast<size_t>(length);
    SharedSegment* segment = nullptr;
    SegmentMapping* mapping = nullptr;
    bool created = false;
    {
        SegmentGuard guard;
        segment = find_segment_locked(name_buffer);
        if (segment == nullptr) {
            segment = allocate_segment_locked(name_buffer, requested);
            if (segment == nullptr) {
                log_message(LogLevel::Warn,
                            "SharedMemory: failed to create '%s' (%zu bytes)",
                            name_buffer,
                            requested);
                return false;
            }
            created = true;
        } else if (requested != 0 && segment->length < requested) {
            log_message(LogLevel::Warn,
                        "SharedMemory: '%s' existing size %zu < requested %zu",
                        name_buffer,
                        static_cast<size_t>(segment->length),
                        requested);
            return false;
        }
        mapping = find_mapping(*segment, proc);
        if (mapping == nullptr) {
            mapping = allocate_mapping(*segment, proc);
            if (mapping == nullptr) {
                return false;
            }
        }
        if (mapping->refcount == 0) {
            if (!map_segment_into_process(*segment, proc, *mapping)) {
                mapping->proc = nullptr;
                mapping->region = vm::Region{0, 0};
                mapping->refcount = 0;
                if (created) {
                    release_segment_pages(*segment);
                    reset_segment(*segment);
                }
                return false;
            }
        }
        ++mapping->refcount;
        ++segment->refcount;
    }
    alloc.type = kTypeSharedMemory;
    alloc.flags = static_cast<uint64_t>(Flag::Readable) |
                  static_cast<uint64_t>(Flag::Writable) |
                  static_cast<uint64_t>(Flag::Mappable);
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = segment;
    alloc.subsystem_data = mapping;
    alloc.name = segment->name;
    alloc.ops = &kSharedMemoryOps;
    alloc.close = shared_memory_close;
    return true;
}

}  // namespace shared_memory_descriptor

bool register_shared_memory_descriptor() {
    return register_type(kTypeSharedMemory,
                         shared_memory_descriptor::open_shared_memory,
                         &shared_memory_descriptor::kSharedMemoryOps);
}

}  // namespace descriptor
