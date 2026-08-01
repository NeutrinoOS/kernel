#include "kernel/module.hpp"

#include "arch/x86_64/memory/paging.hpp"
#include "arch/x86_64/lapic.hpp"
#include "drivers/driver_registry.hpp"
#include "drivers/log/logging.hpp"
#include "drivers/pci/pci.hpp"
#include "fs/vfs.hpp"
#include "kernel/error.hpp"
#include "kernel/interrupts.hpp"
#include "kernel/memory/physical_allocator.hpp"
#include "kernel/scheduler.hpp"
#include "lib/mem.hpp"
#include "net/network.hpp"

extern "C" {
extern const kernel_module::Descriptor __kernel_modules_start[];
extern const kernel_module::Descriptor __kernel_modules_end[];
}

namespace kernel_module {
namespace {

constexpr size_t kMaxInitializedModules = 64;
constexpr size_t kMaxLoadedModules = 16;
constexpr size_t kMaxModuleImageSize = 1024 * 1024;
constexpr size_t kMaxSections = 96;
constexpr uint64_t kPageSize = 0x1000;
constexpr uint64_t kModuleVirtBase = 0xFFFFFFFF90000000ull;
constexpr uint64_t kModuleVirtLimit = 0xFFFFFFFFA0000000ull;
const Descriptor* g_initialized[kMaxInitializedModules]{};
size_t g_initialized_count = 0;
alignas(16) uint8_t g_module_image[kMaxModuleImageSize];
uint64_t g_next_module_virt = kModuleVirtBase;
bool g_module_load_active = false;

class ModuleLoadGuard {
public:
    ModuleLoadGuard() {
        bool expected = false;
        acquired_ = __atomic_compare_exchange_n(&g_module_load_active,
                                                &expected,
                                                true,
                                                false,
                                                __ATOMIC_ACQUIRE,
                                                __ATOMIC_RELAXED);
    }

    ~ModuleLoadGuard() {
        if (acquired_) {
            __atomic_store_n(&g_module_load_active, false, __ATOMIC_RELEASE);
        }
    }

    explicit operator bool() const { return acquired_; }

private:
    bool acquired_ = false;
};

enum class ElfIdent : size_t {
    Class = 4,
    Data = 5,
    Version = 6,
};

enum : uint8_t {
    ELFCLASS64 = 2,
    ELFDATA2LSB = 1,
};

enum : uint16_t {
    ET_REL = 1,
    EM_X86_64 = 62,
    SHN_UNDEF = 0,
    SHN_ABS = 0xFFF1,
};

enum : uint32_t {
    SHT_NULL = 0,
    SHT_PROGBITS = 1,
    SHT_SYMTAB = 2,
    SHT_STRTAB = 3,
    SHT_RELA = 4,
    SHT_NOBITS = 8,
};

enum : uint64_t {
    SHF_WRITE = 1ull << 0,
    SHF_ALLOC = 1ull << 1,
    SHF_EXECINSTR = 1ull << 2,
};

enum : uint32_t {
    R_X86_64_NONE = 0,
    R_X86_64_64 = 1,
    R_X86_64_PC32 = 2,
    R_X86_64_PLT32 = 4,
    R_X86_64_32 = 10,
    R_X86_64_32S = 11,
};

enum : uint8_t {
    STB_LOCAL = 0,
    STB_GLOBAL = 1,
    STB_WEAK = 2,
};

struct Elf64Ehdr {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct Elf64Shdr {
    uint32_t name;
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t addralign;
    uint64_t entsize;
};

struct Elf64Sym {
    uint32_t name;
    uint8_t info;
    uint8_t other;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
};

struct Elf64Rela {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
};

struct LoadedDiskModule {
    bool used;
    bool initialized;
    char path[128];
    uint64_t phys;
    uint8_t* image;
    size_t image_size;
};

LoadedDiskModule g_loaded_modules[kMaxLoadedModules]{};

bool module_slot_used(const LoadedDiskModule& module) {
    return __atomic_load_n(&module.used, __ATOMIC_ACQUIRE);
}

constexpr bool is_power_of_two(uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

bool align_up_checked(uint64_t value,
                      uint64_t alignment,
                      uint64_t& out_value) {
    if (!is_power_of_two(alignment)) {
        return false;
    }
    uint64_t mask = alignment - 1;
    if (value > UINT64_MAX - mask) {
        return false;
    }
    out_value = (value + mask) & ~mask;
    return true;
}

uint32_t elf_relocation_type(const Elf64Rela& rela) {
    return static_cast<uint32_t>(rela.info & 0xFFFFFFFFu);
}

uint32_t elf_relocation_symbol(const Elf64Rela& rela) {
    return static_cast<uint32_t>(rela.info >> 32);
}

uint8_t elf_symbol_bind(const Elf64Sym& sym) {
    return static_cast<uint8_t>(sym.info >> 4);
}

bool strings_equal(const char* lhs, const char* rhs) {
    if (lhs == nullptr || rhs == nullptr) {
        return lhs == rhs;
    }
    size_t i = 0;
    while (lhs[i] != '\0' && rhs[i] != '\0') {
        if (lhs[i] != rhs[i]) {
            return false;
        }
        ++i;
    }
    return lhs[i] == '\0' && rhs[i] == '\0';
}

void copy_string(char* dest, size_t dest_size, const char* src) {
    if (dest == nullptr || dest_size == 0) {
        return;
    }
    size_t i = 0;
    while (src != nullptr && src[i] != '\0' && i + 1 < dest_size) {
        dest[i] = src[i];
        ++i;
    }
    dest[i] = '\0';
}

void publish_loaded_module(LoadedDiskModule& module,
                           const char* path,
                           bool initialized) {
    copy_string(module.path, sizeof(module.path), path);
    module.initialized = initialized;
    __atomic_store_n(&module.used, true, __ATOMIC_RELEASE);
}

void api_log(uint8_t level, const char* message) {
    LogLevel log_level = LogLevel::Info;
    switch (level) {
        case 0: log_level = LogLevel::Debug; break;
        case 1: log_level = LogLevel::Info; break;
        case 2: log_level = LogLevel::Warn; break;
        case 3: log_level = LogLevel::Error; break;
        default: break;
    }
    log_message(log_level, "%s", message != nullptr ? message : "(null)");
}

bool api_register_pci_driver(const char* name,
                             const PciMatch* matches,
                             size_t match_count,
                             PciDriverInitFn init) {
    return driver_registry::register_pci_driver(name,
                                                matches,
                                                match_count,
                                                init);
}

const Api g_disk_module_api{
    .abi_version = kDescriptorAbiVersion,
    .log = api_log,
    .register_pci_driver = api_register_pci_driver,
};

bool match_field(uint32_t actual, uint32_t expected, uint32_t any_value) {
    return expected == any_value || actual == expected;
}

bool pci_device_matches(const pci::PciDevice& device, const PciMatch& match) {
    return match_field(device.vendor, match.vendor, kAnyVendor) &&
           match_field(device.device, match.device, kAnyDevice) &&
           match_field(device.class_code, match.class_code, kAnyClass) &&
           match_field(device.subclass, match.subclass, kAnySubclass) &&
           match_field(device.prog_if, match.prog_if, kAnyProgIf);
}

bool module_matches_any_pci_device(const Descriptor& module) {
    if (module.pci_matches == nullptr || module.pci_match_count == 0) {
        return true;
    }

    const pci::PciDevice* devices = pci::devices();
    size_t device_count = pci::device_count();
    for (size_t i = 0; i < device_count; ++i) {
        for (size_t j = 0; j < module.pci_match_count; ++j) {
            if (pci_device_matches(devices[i], module.pci_matches[j])) {
                return true;
            }
        }
    }
    return false;
}

bool is_initialized(const Descriptor& module) {
    for (size_t i = 0; i < g_initialized_count; ++i) {
        if (g_initialized[i] == &module) {
            return true;
        }
    }
    return false;
}

void mark_initialized(const Descriptor& module) {
    if (g_initialized_count >= kMaxInitializedModules) {
        log_message(LogLevel::Warn,
                    "Module: initialized module table is full");
        return;
    }
    g_initialized[g_initialized_count++] = &module;
}

const char* phase_name(Phase phase) {
    switch (phase) {
        case Phase::Core: return "core";
        case Phase::Bus: return "bus";
        case Phase::Driver: return "driver";
        case Phase::Late: return "late";
    }
    return "unknown";
}

bool read_file_image(const char* path, uint8_t*& out_data, size_t& out_size) {
    out_data = nullptr;
    out_size = 0;
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    vfs::FileHandle handle{};
    if (!vfs::open_file(path, handle)) {
        log_message(LogLevel::Warn, "Module: failed to open %s", path);
        return false;
    }
    if (handle.size == 0 || handle.size > kMaxModuleImageSize) {
        log_message(LogLevel::Warn,
                    "Module: %s has unsupported size %llu",
                    path,
                    static_cast<unsigned long long>(handle.size));
        vfs::close_file(handle);
        return false;
    }

    size_t total = static_cast<size_t>(handle.size);
    size_t offset = 0;
    while (offset < total) {
        size_t read = 0;
        if (!vfs::read_file(handle,
                            offset,
                            g_module_image + offset,
                            total - offset,
                            read)) {
            vfs::close_file(handle);
            return false;
        }
        if (read == 0) {
            break;
        }
        offset += read;
    }
    vfs::close_file(handle);
    if (offset != total) {
        log_message(LogLevel::Warn, "Module: short read from %s", path);
        return false;
    }
    out_data = g_module_image;
    out_size = total;
    return true;
}

bool validate_relocatable_header(const uint8_t* data,
                                 size_t size,
                                 const Elf64Ehdr*& out_header) {
    out_header = nullptr;
    if (data == nullptr || size < sizeof(Elf64Ehdr)) {
        return false;
    }
    const auto* header = reinterpret_cast<const Elf64Ehdr*>(data);
    if (header->ident[0] != 0x7F || header->ident[1] != 'E' ||
        header->ident[2] != 'L' || header->ident[3] != 'F') {
        log_message(LogLevel::Warn, "Module: image is not ELF");
        return false;
    }
    if (header->ident[static_cast<size_t>(ElfIdent::Class)] != ELFCLASS64 ||
        header->ident[static_cast<size_t>(ElfIdent::Data)] != ELFDATA2LSB ||
        header->ident[static_cast<size_t>(ElfIdent::Version)] != 1 ||
        header->type != ET_REL ||
        header->machine != EM_X86_64 ||
        header->version != 1 ||
        header->shoff == 0 ||
        header->shnum == 0 ||
        header->shentsize != sizeof(Elf64Shdr)) {
        log_message(LogLevel::Warn,
                    "Module: unsupported ELF relocatable object");
        return false;
    }
    uint64_t section_table_end =
        header->shoff +
        static_cast<uint64_t>(header->shnum) * header->shentsize;
    if (section_table_end > size || section_table_end < header->shoff ||
        header->shnum > kMaxSections) {
        log_message(LogLevel::Warn, "Module: invalid section table");
        return false;
    }
    out_header = header;
    return true;
}

const Elf64Shdr* section_at(const uint8_t* data,
                            const Elf64Ehdr& header,
                            size_t index) {
    if (index >= header.shnum) {
        return nullptr;
    }
    return reinterpret_cast<const Elf64Shdr*>(
        data + header.shoff + index * header.shentsize);
}

const char* string_at(const uint8_t* data,
                      size_t size,
                      const Elf64Shdr& strings,
                      uint32_t offset) {
    if (strings.type != SHT_STRTAB ||
        offset >= strings.size ||
        strings.offset + strings.size > size ||
        strings.offset + strings.size < strings.offset) {
        return nullptr;
    }
    const char* value =
        reinterpret_cast<const char*>(data + strings.offset + offset);
    size_t remaining = static_cast<size_t>(strings.size - offset);
    for (size_t i = 0; i < remaining; ++i) {
        if (value[i] == '\0') {
            return value;
        }
    }
    return nullptr;
}

bool section_data_valid(const Elf64Shdr& section, size_t image_size) {
    if (section.type == SHT_NOBITS || section.size == 0) {
        return true;
    }
    return section.offset + section.size <= image_size &&
           section.offset + section.size >= section.offset;
}

bool allocate_module_sections(const uint8_t* data,
                              size_t image_size,
                              const Elf64Ehdr& header,
                              uint64_t (&section_addrs)[kMaxSections],
                              LoadedDiskModule& loaded) {
    uint64_t required = 0;
    for (size_t i = 0; i < header.shnum; ++i) {
        const Elf64Shdr* section = section_at(data, header, i);
        if (section == nullptr) {
            return false;
        }
        if ((section->flags & SHF_ALLOC) == 0 || section->size == 0) {
            continue;
        }
        if (!section_data_valid(*section, image_size)) {
            log_message(LogLevel::Warn, "Module: section exceeds image");
            return false;
        }
        if (section->addralign != 0 &&
            !is_power_of_two(section->addralign)) {
            log_message(LogLevel::Warn,
                        "Module: section has invalid alignment");
            return false;
        }
        if (section->addralign > kPageSize) {
            log_message(LogLevel::Warn,
                        "Module: section alignment exceeds loader support");
            return false;
        }
        uint64_t alignment = section->addralign == 0 ? 1 : section->addralign;
        if (alignment < kPageSize) {
            alignment = kPageSize;
        }
        if (!align_up_checked(required, alignment, required)) {
            return false;
        }
        section_addrs[i] = required;
        if (section->size > UINT64_MAX - required) {
            return false;
        }
        required += section->size;
    }
    if (required == 0) {
        log_message(LogLevel::Warn, "Module: no allocatable sections");
        return false;
    }

    uint64_t rounded_required = 0;
    if (!align_up_checked(required, kPageSize, rounded_required)) {
        return false;
    }
    if (rounded_required > kModuleVirtLimit - kModuleVirtBase) {
        log_message(LogLevel::Warn, "Module: allocatable image is too large");
        return false;
    }
    size_t pages = static_cast<size_t>(rounded_required / kPageSize);
    uint64_t phys = memory::alloc_kernel_block_pages(pages);
    if (phys == 0) {
        log_message(LogLevel::Warn, "Module: failed to allocate memory");
        return false;
    }

    uint64_t virt = 0;
    if (!align_up_checked(g_next_module_virt, kPageSize, virt)) {
        memory::free_kernel_block(phys);
        return false;
    }
    uint64_t bytes = rounded_required;
    if (virt + bytes > kModuleVirtLimit || virt + bytes < virt) {
        log_message(LogLevel::Warn, "Module: virtual module window is full");
        memory::free_kernel_block(phys);
        return false;
    }
    size_t mapped_pages = 0;
    for (; mapped_pages < pages; ++mapped_pages) {
        size_t page = mapped_pages;
        if (!paging_map_page(virt + page * kPageSize,
                             phys + page * kPageSize,
                             PAGE_FLAG_WRITE | PAGE_FLAG_GLOBAL |
                                 PAGE_FLAG_NO_EXECUTE)) {
            log_message(LogLevel::Warn, "Module: failed to map module page");
            for (size_t mapped = 0; mapped < mapped_pages; ++mapped) {
                uint64_t ignored = 0;
                paging_unmap_page(virt + mapped * kPageSize, ignored);
            }
            memory::free_kernel_block(phys);
            return false;
        }
    }
    g_next_module_virt = virt + bytes;

    auto* base = reinterpret_cast<uint8_t*>(virt);
    memset(base, 0, bytes);

    for (size_t i = 0; i < header.shnum; ++i) {
        const Elf64Shdr* section = section_at(data, header, i);
        if (section == nullptr ||
            (section->flags & SHF_ALLOC) == 0 ||
            section->size == 0) {
            continue;
        }
        section_addrs[i] += reinterpret_cast<uint64_t>(base);
        if (section->type != SHT_NOBITS) {
            memcpy(reinterpret_cast<void*>(section_addrs[i]),
                   data + section->offset,
                   static_cast<size_t>(section->size));
        }
    }

    loaded.phys = phys;
    loaded.image = base;
    loaded.image_size = bytes;
    return true;
}

void release_module_image(LoadedDiskModule& loaded) {
    if (loaded.image != nullptr) {
        uint64_t base = reinterpret_cast<uint64_t>(loaded.image);
        for (size_t offset = 0; offset < loaded.image_size;
             offset += kPageSize) {
            uint64_t ignored = 0;
            paging_unmap_page(base + offset, ignored);
        }
        if (loaded.image_size <= UINT64_MAX - base &&
            g_next_module_virt == base + loaded.image_size) {
            g_next_module_virt = base;
        }
    }
    if (loaded.phys != 0) {
        memory::free_kernel_block(loaded.phys);
    }
    loaded.phys = 0;
    loaded.image = nullptr;
    loaded.image_size = 0;
}

bool protect_module_sections(const uint8_t* data,
                             const Elf64Ehdr& header,
                             const uint64_t (&section_addrs)[kMaxSections]) {
    uint64_t cr3 = paging_kernel_cr3();
    for (size_t i = 0; i < header.shnum; ++i) {
        const Elf64Shdr* section = section_at(data, header, i);
        if (section == nullptr ||
            (section->flags & SHF_ALLOC) == 0 ||
            section->size == 0) {
            continue;
        }
        bool writable = (section->flags & SHF_WRITE) != 0;
        bool executable = (section->flags & SHF_EXECINSTR) != 0;
        if (writable && executable) {
            log_message(LogLevel::Warn,
                        "Module: refusing writable executable section");
            return false;
        }
        if (section->size > UINT64_MAX - section_addrs[i]) {
            return false;
        }
        uint64_t start = section_addrs[i] & ~(kPageSize - 1);
        uint64_t end = 0;
        if (!align_up_checked(section_addrs[i] + section->size,
                              kPageSize,
                              end)) {
            return false;
        }
        for (uint64_t page = start; page < end; page += kPageSize) {
            if (!paging_set_writable_cr3(cr3, page, writable) ||
                !paging_set_executable_cr3(cr3, page, executable)) {
                log_message(LogLevel::Warn,
                            "Module: failed to apply section permissions");
                return false;
            }
        }
    }
    return true;
}

bool resolve_external_symbol(const char* name, uint64_t& out_value) {
    struct Export {
        const char* name;
        uint64_t value;
    };
    const Export exports[] = {
        {"_Z11log_message8LogLevelPKcz",
         reinterpret_cast<uint64_t>(&log_message)},
        {"_Z15paging_map_pagemmm",
         reinterpret_cast<uint64_t>(&paging_map_page)},
        {"_Z19paging_phys_to_virtm",
         reinterpret_cast<uint64_t>(&paging_phys_to_virt)},
        {"_ZN10interrupts15allocate_vectorEv",
         reinterpret_cast<uint64_t>(&interrupts::allocate_vector)},
        {"_ZN10interrupts15register_vectorEhPFvvE",
         reinterpret_cast<uint64_t>(&interrupts::register_vector)},
        {"_ZN10interrupts17unregister_vectorEh",
         reinterpret_cast<uint64_t>(&interrupts::unregister_vector)},
        {"_ZN3net13receive_frameEPNS_10LinkDeviceEPKvm",
         reinterpret_cast<uint64_t>(&net::receive_frame)},
        {"_ZN3net13register_linkERNS_10LinkDeviceEPKcPvPFbS4_PKvmEPKh",
         reinterpret_cast<uint64_t>(&net::register_link)},
        {"_ZN3pci10enable_msiERKNS_9PciDeviceEhh",
         reinterpret_cast<uint64_t>(&pci::enable_msi)},
        {"_ZN3pci12device_countEv",
         reinterpret_cast<uint64_t>(&pci::device_count)},
        {"_ZN3pci13read_config16ERKNS_9PciDeviceEh",
         reinterpret_cast<uint64_t>(
             static_cast<uint16_t (*)(const pci::PciDevice&, uint8_t)>(
                 &pci::read_config16))},
        {"_ZN3pci13read_config32ERKNS_9PciDeviceEh",
         reinterpret_cast<uint64_t>(
             static_cast<uint32_t (*)(const pci::PciDevice&, uint8_t)>(
                 &pci::read_config32))},
        {"_ZN3pci15find_capabilityERKNS_9PciDeviceEh",
         reinterpret_cast<uint64_t>(&pci::find_capability)},
        {"_ZN3pci14write_config16ERKNS_9PciDeviceEht",
         reinterpret_cast<uint64_t>(
             static_cast<void (*)(const pci::PciDevice&, uint8_t, uint16_t)>(
                 &pci::write_config16))},
        {"_ZN3pci7devicesEv",
         reinterpret_cast<uint64_t>(&pci::devices)},
        {"_ZN5lapic2idEv",
         reinterpret_cast<uint64_t>(&lapic::id)},
        {"_ZN6memory17alloc_kernel_pageEv",
         reinterpret_cast<uint64_t>(&memory::alloc_kernel_page)},
        {"_ZN6memory24alloc_kernel_block_pagesEm",
         reinterpret_cast<uint64_t>(&memory::alloc_kernel_block_pages)},
        {"_ZN6memory16free_kernel_pageEm",
         reinterpret_cast<uint64_t>(&memory::free_kernel_page)},
        {"_ZN6memory17free_kernel_blockEm",
         reinterpret_cast<uint64_t>(&memory::free_kernel_block)},
        {"_ZN9scheduler13register_pollEPFvvE",
         reinterpret_cast<uint64_t>(&scheduler::register_poll)},
        {"kernel_assertion_failed",
         reinterpret_cast<uint64_t>(&kernel_assertion_failed)},
        {"memcpy", reinterpret_cast<uint64_t>(&memcpy)},
        {"memset", reinterpret_cast<uint64_t>(&memset)},
    };

    for (const Export& export_entry : exports) {
        if (strings_equal(name, export_entry.name)) {
            out_value = export_entry.value;
            return true;
        }
    }

    if (strings_equal(name, "memcpy")) {
        out_value = reinterpret_cast<uint64_t>(&memcpy);
        return true;
    }
    if (strings_equal(name, "memset")) {
        out_value = reinterpret_cast<uint64_t>(&memset);
        return true;
    }
    return false;
}

bool resolve_symbol_value(const uint8_t* data,
                          size_t image_size,
                          const Elf64Ehdr& header,
                          const Elf64Sym& sym,
                          const Elf64Shdr& string_table,
                          const uint64_t (&section_addrs)[kMaxSections],
                          uint64_t& out_value) {
    if (sym.shndx == SHN_UNDEF) {
        const char* name = string_at(data, image_size, string_table, sym.name);
        if (resolve_external_symbol(name, out_value)) {
            return true;
        }
        if (elf_symbol_bind(sym) == STB_WEAK) {
            out_value = 0;
            return true;
        }
        log_message(LogLevel::Warn,
                    "Module: unresolved symbol %s",
                    name != nullptr ? name : "(unnamed)");
        return false;
    }
    if (sym.shndx == SHN_ABS) {
        out_value = sym.value;
        return true;
    }
    if (sym.shndx >= header.shnum || section_addrs[sym.shndx] == 0) {
        return false;
    }
    const Elf64Shdr* section = section_at(data, header, sym.shndx);
    if (section == nullptr || sym.value > section->size ||
        sym.size > section->size - sym.value ||
        sym.value > UINT64_MAX - section_addrs[sym.shndx]) {
        return false;
    }
    out_value = section_addrs[sym.shndx] + sym.value;
    return true;
}

bool apply_relocation(uint64_t place,
                      uint32_t type,
                      uint64_t symbol_value,
                      int64_t addend) {
    uint64_t value = symbol_value + static_cast<uint64_t>(addend);
    switch (type) {
        case R_X86_64_NONE:
            return true;
        case R_X86_64_64:
            *reinterpret_cast<uint64_t*>(place) = value;
            return true;
        case R_X86_64_PC32:
        case R_X86_64_PLT32: {
            int64_t rel = static_cast<int64_t>(value - place);
            if (rel < INT32_MIN || rel > INT32_MAX) {
                return false;
            }
            *reinterpret_cast<int32_t*>(place) = static_cast<int32_t>(rel);
            return true;
        }
        case R_X86_64_32:
            if (value > UINT32_MAX) {
                return false;
            }
            *reinterpret_cast<uint32_t*>(place) = static_cast<uint32_t>(value);
            return true;
        case R_X86_64_32S: {
            int64_t signed_value = static_cast<int64_t>(value);
            if (signed_value < INT32_MIN || signed_value > INT32_MAX) {
                return false;
            }
            *reinterpret_cast<int32_t*>(place) =
                static_cast<int32_t>(signed_value);
            return true;
        }
        default:
            log_message(LogLevel::Warn,
                        "Module: unsupported relocation type %u",
                        static_cast<unsigned int>(type));
            return false;
    }
}

size_t relocation_width(uint32_t type) {
    switch (type) {
        case R_X86_64_NONE:
            return 0;
        case R_X86_64_64:
            return 8;
        case R_X86_64_PC32:
        case R_X86_64_PLT32:
        case R_X86_64_32:
        case R_X86_64_32S:
            return 4;
        default:
            return 0;
    }
}

bool apply_module_relocations(const uint8_t* data,
                              size_t image_size,
                              const Elf64Ehdr& header,
                              const uint64_t (&section_addrs)[kMaxSections]) {
    for (size_t i = 0; i < header.shnum; ++i) {
        const Elf64Shdr* rela_section = section_at(data, header, i);
        if (rela_section == nullptr || rela_section->type != SHT_RELA) {
            continue;
        }
        if (rela_section->info >= header.shnum ||
            rela_section->link >= header.shnum ||
            rela_section->entsize < sizeof(Elf64Rela) ||
            !section_data_valid(*rela_section, image_size) ||
            rela_section->size % rela_section->entsize != 0) {
            return false;
        }
        uint64_t target_base = section_addrs[rela_section->info];
        if (target_base == 0) {
            continue;
        }
        const Elf64Shdr* symtab = section_at(data, header, rela_section->link);
        if (symtab == nullptr || symtab->type != SHT_SYMTAB ||
            symtab->link >= header.shnum ||
            symtab->entsize < sizeof(Elf64Sym) ||
            !section_data_valid(*symtab, image_size) ||
            symtab->size % symtab->entsize != 0) {
            return false;
        }
        const Elf64Shdr* string_table = section_at(data, header, symtab->link);
        if (string_table == nullptr || string_table->type != SHT_STRTAB) {
            return false;
        }

        size_t rela_count =
            static_cast<size_t>(rela_section->size / rela_section->entsize);
        size_t sym_count = static_cast<size_t>(symtab->size / symtab->entsize);
        for (size_t rela_index = 0; rela_index < rela_count; ++rela_index) {
            const auto* rela = reinterpret_cast<const Elf64Rela*>(
                data + rela_section->offset +
                rela_index * rela_section->entsize);
            uint32_t sym_index = elf_relocation_symbol(*rela);
            if (sym_index >= sym_count) {
                return false;
            }
            const auto* sym = reinterpret_cast<const Elf64Sym*>(
                data + symtab->offset + sym_index * symtab->entsize);
            uint64_t symbol_value = 0;
            if (!resolve_symbol_value(data,
                                      image_size,
                                      header,
                                      *sym,
                                      *string_table,
                                      section_addrs,
                                      symbol_value)) {
                return false;
            }
            uint32_t relocation_type = elf_relocation_type(*rela);
            size_t width = relocation_width(relocation_type);
            uint64_t place = target_base + rela->offset;
            const Elf64Shdr* target = section_at(data, header, rela_section->info);
            if (target == nullptr ||
                rela->offset > target->size ||
                width > target->size - rela->offset) {
                return false;
            }
            if (!apply_relocation(place,
                                  relocation_type,
                                  symbol_value,
                                  rela->addend)) {
                return false;
            }
        }
    }
    return true;
}

bool find_symbol(const uint8_t* data,
                 size_t image_size,
                 const Elf64Ehdr& header,
                 const uint64_t (&section_addrs)[kMaxSections],
                 const char* name,
                 uint64_t& out_value) {
    for (size_t i = 0; i < header.shnum; ++i) {
        const Elf64Shdr* symtab = section_at(data, header, i);
        if (symtab == nullptr || symtab->type != SHT_SYMTAB ||
            symtab->link >= header.shnum ||
            symtab->entsize < sizeof(Elf64Sym) ||
            !section_data_valid(*symtab, image_size) ||
            symtab->size % symtab->entsize != 0) {
            continue;
        }
        const Elf64Shdr* string_table = section_at(data, header, symtab->link);
        if (string_table == nullptr || string_table->type != SHT_STRTAB) {
            continue;
        }
        size_t sym_count = static_cast<size_t>(symtab->size / symtab->entsize);
        for (size_t sym_index = 0; sym_index < sym_count; ++sym_index) {
            const auto* sym = reinterpret_cast<const Elf64Sym*>(
                data + symtab->offset + sym_index * symtab->entsize);
            if (sym->shndx == SHN_UNDEF || sym->name == 0) {
                continue;
            }
            const char* candidate =
                string_at(data, image_size, *string_table, sym->name);
            if (!strings_equal(candidate, name)) {
                continue;
            }
            if (sym->shndx >= header.shnum) {
                return false;
            }
            const Elf64Shdr* target = section_at(data, header, sym->shndx);
            if (target == nullptr ||
                (target->flags & (SHF_ALLOC | SHF_EXECINSTR)) !=
                    (SHF_ALLOC | SHF_EXECINSTR) ||
                sym->value >= target->size) {
                return false;
            }
            return resolve_symbol_value(data,
                                        image_size,
                                        header,
                                        *sym,
                                        *string_table,
                                        section_addrs,
                                        out_value);
        }
    }
    return false;
}

LoadedDiskModule* allocate_loaded_module_slot() {
    for (size_t i = 0; i < kMaxLoadedModules; ++i) {
        if (!module_slot_used(g_loaded_modules[i])) {
            return &g_loaded_modules[i];
        }
    }
    return nullptr;
}

LoadedDiskModule* find_loaded_module(const char* path) {
    for (size_t i = 0; i < kMaxLoadedModules; ++i) {
        if (module_slot_used(g_loaded_modules[i]) &&
            strings_equal(g_loaded_modules[i].path, path)) {
            return &g_loaded_modules[i];
        }
    }
    return nullptr;
}

}  // namespace

size_t count() {
    return static_cast<size_t>(__kernel_modules_end - __kernel_modules_start);
}

bool initialize_phase(Phase phase) {
    bool ok = true;
    size_t initialized = 0;
    size_t skipped = 0;

    for (const Descriptor* module = __kernel_modules_start;
         module < __kernel_modules_end;
         ++module) {
        if (module->abi_version != kDescriptorAbiVersion ||
            module->name == nullptr ||
            module->init == nullptr ||
            module->phase != phase ||
            is_initialized(*module)) {
            continue;
        }

        if (!module_matches_any_pci_device(*module)) {
            ++skipped;
            log_message(LogLevel::Debug,
                        "Module: skipping %s (no PCI match)",
                        module->name);
            continue;
        }

        log_message(LogLevel::Info,
                    "Module: initializing %s",
                    module->name);
        if (!module->init()) {
            ok = false;
            log_message(LogLevel::Warn,
                        "Module: %s initialization failed",
                        module->name);
        }
        mark_initialized(*module);
        ++initialized;
    }

    log_message(LogLevel::Info,
                "Module: %s phase complete (%zu initialized, %zu skipped)",
                phase_name(phase),
                initialized,
                skipped);
    return ok;
}

size_t loaded_count() {
    size_t total = g_initialized_count;
    for (size_t i = 0; i < kMaxLoadedModules; ++i) {
        if (module_slot_used(g_loaded_modules[i])) {
            ++total;
        }
    }
    return total;
}

bool info_at(size_t index, ModuleInfo& out_info) {
    memset(&out_info, 0, sizeof(out_info));

    if (index < g_initialized_count) {
        const Descriptor* module = g_initialized[index];
        if (module == nullptr || module->name == nullptr) {
            return false;
        }
        copy_string(out_info.name, sizeof(out_info.name), module->name);
        out_info.flags = kModuleInfoBuiltin;
        return true;
    }

    index -= g_initialized_count;
    for (size_t i = 0; i < kMaxLoadedModules; ++i) {
        const LoadedDiskModule& module = g_loaded_modules[i];
        if (!module_slot_used(module)) {
            continue;
        }
        if (index != 0) {
            --index;
            continue;
        }
        const char* name = module.path;
        for (size_t j = 0; module.path[j] != '\0'; ++j) {
            if (module.path[j] == '/') {
                name = module.path + j + 1;
            }
        }
        copy_string(out_info.name, sizeof(out_info.name), name);
        copy_string(out_info.path, sizeof(out_info.path), module.path);
        out_info.image_size = module.image_size;
        out_info.flags = kModuleInfoDynamic;
        return true;
    }

    return false;
}

bool load_from_file(const char* path) {
    ModuleLoadGuard load_guard;
    if (!load_guard) {
        log_message(LogLevel::Warn, "Module: another load is in progress");
        return false;
    }

    if (LoadedDiskModule* existing = find_loaded_module(path);
        existing != nullptr) {
        log_message(LogLevel::Info,
                    "Module: %s is already resident%s",
                    path,
                    existing->initialized ? "" : " after a failed init");
        return existing->initialized;
    }

    uint8_t* data = nullptr;
    size_t image_size = 0;
    if (!read_file_image(path, data, image_size)) {
        return false;
    }

    const Elf64Ehdr* header = nullptr;
    if (!validate_relocatable_header(data, image_size, header)) {
        return false;
    }

    LoadedDiskModule* loaded = allocate_loaded_module_slot();
    if (loaded == nullptr) {
        log_message(LogLevel::Warn, "Module: loaded module table is full");
        return false;
    }

    uint64_t section_addrs[kMaxSections]{};
    if (!allocate_module_sections(data,
                                  image_size,
                                  *header,
                                  section_addrs,
                                  *loaded)) {
        return false;
    }
    if (!apply_module_relocations(data, image_size, *header, section_addrs)) {
        log_message(LogLevel::Warn, "Module: failed to relocate %s", path);
        release_module_image(*loaded);
        return false;
    }
    if (!protect_module_sections(data, *header, section_addrs)) {
        release_module_image(*loaded);
        return false;
    }

    uint64_t init_addr = 0;
    if (!find_symbol(data,
                     image_size,
                     *header,
                     section_addrs,
                     "neutrino_module_init",
                     init_addr)) {
        log_message(LogLevel::Warn,
                    "Module: %s does not export neutrino_module_init",
                    path);
        release_module_image(*loaded);
        return false;
    }

    using DiskModuleInitFn = bool (*)(const Api*);
    auto init = reinterpret_cast<DiskModuleInitFn>(init_addr);
    log_message(LogLevel::Info, "Module: loading %s", path);
    if (!init(&g_disk_module_api)) {
        log_message(LogLevel::Warn,
                    "Module: %s initialization failed; keeping its image "
                    "resident because callbacks may have escaped",
                    path);
        publish_loaded_module(*loaded, path, false);
        return false;
    }

    publish_loaded_module(*loaded, path, true);
    log_message(LogLevel::Info, "Module: loaded %s", path);
    return true;
}

}  // namespace kernel_module
