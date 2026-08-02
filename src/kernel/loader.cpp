#include "loader.hpp"

#include "drivers/log/logging.hpp"
#include "lib/mem.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "fs/vfs.hpp"
#include "kernel/memory/physical_allocator.hpp"
#include "vm.hpp"

namespace {

constexpr uint8_t kElfMagic[4] = {0x7F, 'E', 'L', 'F'};
constexpr uint64_t kPageSize = 0x1000;
// Keep the complete dependency graph bounded so malformed or unusually large
// images cannot exhaust kernel resources while they are being loaded.
constexpr size_t kMaxSharedObjects = 16;
constexpr size_t kDefaultMainStackSize = 256 * 1024;
// A single object may name every other object in the graph.  Do not impose a
// smaller, independent DT_NEEDED limit: it rejects otherwise valid graphs.
constexpr size_t kMaxNeeded = kMaxSharedObjects - 1;
constexpr size_t kMaxSharedObjectName = 64;
constexpr size_t kMaxSharedObjectPath = 128;

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
    ET_EXEC = 2,
    ET_DYN = 3,
    EM_X86_64 = 62,
};

enum : uint32_t {
    PT_LOAD = 1,
    PT_DYNAMIC = 2,
};

enum : uint32_t {
    PF_X = 1,
    PF_W = 2,
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

struct Elf64Phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

struct Elf64Dyn {
    int64_t tag;
    uint64_t val;
};

struct Elf64Rela {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
};

enum : int64_t {
    DT_NULL = 0,
    DT_NEEDED = 1,
    DT_PLTRELSZ = 2,
    DT_PLTGOT = 3,
    DT_HASH = 4,
    DT_STRTAB = 5,
    DT_SYMTAB = 6,
    DT_RELA = 7,
    DT_RELASZ = 8,
    DT_RELAENT = 9,
    DT_STRSZ = 10,
    DT_SYMENT = 11,
    DT_PLTREL = 20,
    DT_JMPREL = 23,
};

enum : uint32_t {
    R_X86_64_64 = 1,
    R_X86_64_GLOB_DAT = 6,
    R_X86_64_JUMP_SLOT = 7,
    R_X86_64_RELATIVE = 8,
};

enum : uint16_t {
    SHN_UNDEF = 0,
};

enum : uint8_t {
    STB_LOCAL = 0,
    STB_GLOBAL = 1,
    STB_WEAK = 2,
};

struct Elf64Sym {
    uint32_t name;
    uint8_t info;
    uint8_t other;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
};

struct DynamicInfo {
    uint64_t needed_offsets[kMaxNeeded];
    size_t needed_count;
    uint64_t rela_addr;
    uint64_t rela_size;
    uint64_t rela_ent;
    uint64_t jmprel_addr;
    uint64_t pltrel_size;
    uint64_t strtab_addr;
    uint64_t strsz;
    uint64_t symtab_addr;
    uint64_t syment;
    size_t dynsym_count;
};

struct LoadedObject {
    const uint8_t* data;
    size_t size;
    char name[kMaxSharedObjectName];
    vm::Region region;
    uint64_t load_bias;
    uint64_t min_vaddr;
    uint64_t max_vaddr;
    uint64_t entry;
    DynamicInfo dynamic;
    bool main_object;
};

constexpr uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

size_t cstring_length(const char* text) {
    if (text == nullptr) {
        return 0;
    }
    size_t len = 0;
    while (text[len] != '\0') {
        ++len;
    }
    return len;
}

bool cstring_equal(const char* lhs, const char* rhs) {
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
    return lhs[i] == rhs[i];
}

bool copy_cstring(char* dest, size_t dest_size, const char* src) {
    if (dest == nullptr || dest_size == 0 || src == nullptr) {
        return false;
    }
    size_t i = 0;
    while (src[i] != '\0') {
        if (i + 1 >= dest_size) {
            dest[0] = '\0';
            return false;
        }
        dest[i] = src[i];
        ++i;
    }
    dest[i] = '\0';
    return true;
}

bool build_library_path(const char* name, char* out, size_t out_size) {
    constexpr const char* prefix = "/library/";
    if (name == nullptr || out == nullptr || out_size == 0 ||
        name[0] == '\0') {
        return false;
    }
    size_t prefix_len = cstring_length(prefix);
    size_t name_len = cstring_length(name);
    if ((name_len == 1 && name[0] == '.') ||
        (name_len == 2 && name[0] == '.' && name[1] == '.')) {
        return false;
    }
    for (size_t i = 0; i < name_len; ++i) {
        unsigned char ch = static_cast<unsigned char>(name[i]);
        if (ch < 0x20 || ch == 0x7F || ch == '/' || ch == '\\') {
            return false;
        }
    }
    if (prefix_len + name_len + 1 > out_size) {
        return false;
    }
    for (size_t i = 0; i < prefix_len; ++i) {
        out[i] = prefix[i];
    }
    for (size_t i = 0; i < name_len; ++i) {
        out[prefix_len + i] = name[i];
    }
    out[prefix_len + name_len] = '\0';
    return true;
}

uint32_t elf_symbol_bind(const Elf64Sym& sym) {
    return static_cast<uint32_t>(sym.info >> 4);
}

const Elf64Phdr* program_header_at(const loader::ProgramImage& image,
                                   const Elf64Ehdr& header,
                                   uint16_t index) {
    if (index >= header.phnum) {
        return nullptr;
    }
    uint64_t ph_offset =
        header.phoff + static_cast<uint64_t>(index) * header.phentsize;
    if (ph_offset + sizeof(Elf64Phdr) > image.size ||
        ph_offset + sizeof(Elf64Phdr) < ph_offset) {
        return nullptr;
    }
    return reinterpret_cast<const Elf64Phdr*>(image.data + ph_offset);
}

const uint8_t* image_ptr_at_vaddr(const loader::ProgramImage& image,
                                  const Elf64Ehdr& header,
                                  uint64_t vaddr,
                                  size_t length) {
    for (uint16_t i = 0; i < header.phnum; ++i) {
        const Elf64Phdr* ph = program_header_at(image, header, i);
        if (ph == nullptr || ph->type != PT_LOAD) {
            continue;
        }
        if (vaddr < ph->vaddr) {
            continue;
        }
        uint64_t offset_in_segment = vaddr - ph->vaddr;
        if (offset_in_segment > ph->filesz) {
            continue;
        }
        if (length > ph->filesz - offset_in_segment) {
            continue;
        }
        uint64_t file_offset = ph->offset + offset_in_segment;
        if (file_offset + length > image.size ||
            file_offset + length < file_offset) {
            return nullptr;
        }
        return image.data + file_offset;
    }
    return nullptr;
}

bool read_shared_object_image(const char* path,
                              loader::ProgramImage& out_image,
                              uint8_t*& out_buffer) {
    out_image = {};
    out_buffer = nullptr;
    if (path == nullptr) {
        return false;
    }
    vfs::FileHandle handle{};
    if (!vfs::open_file(path, handle)) {
        return false;
    }
    if (handle.size == 0 || handle.size > SIZE_MAX) {
        vfs::close_file(handle);
        return false;
    }

    size_t total = static_cast<size_t>(handle.size);
    auto* buffer = static_cast<uint8_t*>(memory::alloc_kernel(total, 16));
    if (buffer == nullptr) {
        vfs::close_file(handle);
        return false;
    }
    size_t offset = 0;
    while (offset < total) {
        size_t read = 0;
        if (!vfs::read_file(handle,
                            offset,
                            buffer + offset,
                            total - offset,
                            read)) {
            vfs::close_file(handle);
            memory::free_kernel(buffer);
            return false;
        }
        if (read == 0) {
            break;
        }
        offset += read;
    }
    vfs::close_file(handle);
    if (offset != total) {
        memory::free_kernel(buffer);
        return false;
    }
    out_buffer = buffer;
    out_image.data = buffer;
    out_image.size = total;
    out_image.entry_offset = 0;
    return true;
}

bool looks_like_elf(const uint8_t* data, size_t size) {
    if (data == nullptr || size < sizeof(Elf64Ehdr)) {
        return false;
    }
    const auto* header = reinterpret_cast<const Elf64Ehdr*>(data);
    return header->ident[0] == kElfMagic[0] &&
           header->ident[1] == kElfMagic[1] &&
           header->ident[2] == kElfMagic[2] &&
           header->ident[3] == kElfMagic[3];
}

bool setup_user_stack(process::Task& proc) {
    // Library code can legitimately use frames larger than the old 16 KiB
    // allocation (enabled FFmpeg routines use up to about 48 KiB apiece).
    // Keep enough headroom for callers without adopting FFmpeg's 1 MiB
    // pthread default for every process. allocate_user_stack() retains an
    // unmapped guard page below this region.
    proc.stack_region = vm::allocate_user_stack(proc.cr3,
                                                 kDefaultMainStackSize);
    if (proc.stack_region.top == 0) {
        log_message(LogLevel::Error,
                    "Loader: failed to allocate stack for process %u",
                    static_cast<unsigned int>(proc.pid));
        return false;
    }
    uint64_t aligned_top = (proc.stack_region.top - 16ull) & ~0xFull;
    proc.user_sp = aligned_top;
    return true;
}

bool load_flat_binary(const loader::ProgramImage& image,
                      process::Task& proc) {
    uint64_t entry_point = 0;
    proc.code_region = vm::map_user_code(proc.cr3,
                                         image.data,
                                         image.size,
                                         image.entry_offset, entry_point);
    if (proc.code_region.base == 0) {
        log_message(LogLevel::Error,
                    "Loader: failed to map flat binary for process %u",
                    static_cast<unsigned int>(proc.pid));
        return false;
    }

    proc.user_ip = entry_point;
    return true;
}

bool validate_elf_header(const Elf64Ehdr& header) {
    if (header.ident[static_cast<size_t>(ElfIdent::Class)] != ELFCLASS64 ||
        header.ident[static_cast<size_t>(ElfIdent::Data)] != ELFDATA2LSB ||
        header.ident[static_cast<size_t>(ElfIdent::Version)] != 1) {
        log_message(LogLevel::Error,
                    "Loader: unsupported ELF identification");
        return false;
    }

    if (header.type != ET_EXEC && header.type != ET_DYN) {
        log_message(LogLevel::Error,
                    "Loader: unsupported ELF type %u",
                    static_cast<unsigned int>(header.type));
        return false;
    }

    if (header.machine != EM_X86_64 || header.version != 1) {
        log_message(LogLevel::Error,
                    "Loader: unsupported ELF target");
        return false;
    }

    if (header.phoff == 0 || header.phnum == 0) {
        log_message(LogLevel::Error,
                    "Loader: ELF missing program headers");
        return false;
    }

    if (header.phentsize != sizeof(Elf64Phdr)) {
        log_message(LogLevel::Error,
                    "Loader: unexpected ELF program header size %u",
                    static_cast<unsigned int>(header.phentsize));
        return false;
    }

    return true;
}

bool parse_dynamic_info(const loader::ProgramImage& image,
                        const Elf64Ehdr& header,
                        const Elf64Phdr* dynamic_phdr,
                        DynamicInfo& info) {
    memset(&info, 0, sizeof(info));
    if (dynamic_phdr == nullptr || dynamic_phdr->filesz == 0) {
        return true;
    }
    if (dynamic_phdr->offset + dynamic_phdr->filesz > image.size ||
        dynamic_phdr->offset + dynamic_phdr->filesz < dynamic_phdr->offset) {
        log_message(LogLevel::Error,
                    "Loader: dynamic table exceeds image");
        return false;
    }

    size_t dyn_count =
        static_cast<size_t>(dynamic_phdr->filesz / sizeof(Elf64Dyn));
    const auto* dyn_table =
        reinterpret_cast<const Elf64Dyn*>(image.data + dynamic_phdr->offset);
    for (size_t i = 0; i < dyn_count; ++i) {
        const Elf64Dyn& dyn = dyn_table[i];
        if (dyn.tag == DT_NULL) {
            break;
        }
        switch (dyn.tag) {
            case DT_NEEDED:
                if (info.needed_count >= kMaxNeeded) {
                    log_message(LogLevel::Error,
                                "Loader: too many shared library dependencies");
                    return false;
                }
                info.needed_offsets[info.needed_count++] = dyn.val;
                break;
            case DT_RELA:
                info.rela_addr = dyn.val;
                break;
            case DT_RELASZ:
                info.rela_size = dyn.val;
                break;
            case DT_RELAENT:
                info.rela_ent = dyn.val;
                break;
            case DT_JMPREL:
                info.jmprel_addr = dyn.val;
                break;
            case DT_PLTRELSZ:
                info.pltrel_size = dyn.val;
                break;
            case DT_STRTAB:
                info.strtab_addr = dyn.val;
                break;
            case DT_STRSZ:
                info.strsz = dyn.val;
                break;
            case DT_SYMTAB:
                info.symtab_addr = dyn.val;
                break;
            case DT_SYMENT:
                info.syment = dyn.val;
                break;
            default:
                break;
        }
    }

    if (info.syment == 0) {
        info.syment = sizeof(Elf64Sym);
    }
    if (info.rela_ent == 0) {
        info.rela_ent = sizeof(Elf64Rela);
    }
    if (info.syment < sizeof(Elf64Sym) ||
        info.rela_ent < sizeof(Elf64Rela)) {
        log_message(LogLevel::Error,
                    "Loader: dynamic entry sizes are too small");
        return false;
    }

    if (info.symtab_addr != 0 && info.strtab_addr > info.symtab_addr) {
        info.dynsym_count =
            static_cast<size_t>((info.strtab_addr - info.symtab_addr) /
                                info.syment);
    }

    if (info.needed_count != 0 && info.strtab_addr == 0) {
        log_message(LogLevel::Error,
                    "Loader: dependencies require a dynamic string table");
        return false;
    }

    (void)header;
    return true;
}

bool image_has_needed_dependencies(const loader::ProgramImage& image) {
    if (!looks_like_elf(image.data, image.size)) {
        return false;
    }
    const auto* header = reinterpret_cast<const Elf64Ehdr*>(image.data);
    if (!validate_elf_header(*header)) {
        return false;
    }

    const Elf64Phdr* dynamic_phdr = nullptr;
    for (uint16_t i = 0; i < header->phnum; ++i) {
        const Elf64Phdr* ph = program_header_at(image, *header, i);
        if (ph != nullptr && ph->type == PT_DYNAMIC) {
            dynamic_phdr = ph;
            break;
        }
    }
    DynamicInfo info{};
    if (!parse_dynamic_info(image, *header, dynamic_phdr, info)) {
        return false;
    }
    return info.needed_count != 0;
}

const char* dynamic_string_at(const LoadedObject& object,
                              uint64_t string_offset,
                              size_t& out_length) {
    out_length = 0;
    if (object.dynamic.strtab_addr == 0 ||
        string_offset >= object.dynamic.strsz ||
        object.dynamic.strtab_addr > UINT64_MAX - string_offset) {
        return nullptr;
    }
    uint64_t remaining64 = object.dynamic.strsz - string_offset;
    if (remaining64 > static_cast<uint64_t>(SIZE_MAX)) {
        return nullptr;
    }
    size_t remaining = static_cast<size_t>(remaining64);
    loader::ProgramImage image{object.data, object.size, 0};
    const auto* header = reinterpret_cast<const Elf64Ehdr*>(object.data);
    const uint8_t* ptr =
        image_ptr_at_vaddr(image,
                           *header,
                           object.dynamic.strtab_addr + string_offset,
                           remaining);
    if (ptr == nullptr) {
        return nullptr;
    }
    while (out_length < remaining) {
        if (ptr[out_length] == '\0') {
            return reinterpret_cast<const char*>(ptr);
        }
        ++out_length;
    }
    return nullptr;
}

bool needed_name_at(const LoadedObject& object,
                    uint64_t needed_offset,
                    char* out,
                    size_t out_size) {
    size_t length = 0;
    const char* value = dynamic_string_at(object, needed_offset, length);
    if (value == nullptr || length + 1 > out_size) {
        return false;
    }
    memcpy(out, value, length + 1);
    return true;
}

bool symbol_name_at(const LoadedObject& object,
                    const Elf64Sym& sym,
                    const char*& out_name) {
    out_name = nullptr;
    size_t ignored_length = 0;
    out_name = dynamic_string_at(object, sym.name, ignored_length);
    return out_name != nullptr;
}

const Elf64Sym* dynsym_at(const LoadedObject& object, size_t index) {
    if (object.dynamic.symtab_addr == 0 ||
        object.dynamic.syment < sizeof(Elf64Sym) ||
        index >= object.dynamic.dynsym_count) {
        return nullptr;
    }
    loader::ProgramImage image{object.data, object.size, 0};
    const auto* header = reinterpret_cast<const Elf64Ehdr*>(object.data);
    const uint8_t* ptr =
        image_ptr_at_vaddr(image,
                           *header,
                           object.dynamic.symtab_addr +
                               index * object.dynamic.syment,
                           sizeof(Elf64Sym));
    return reinterpret_cast<const Elf64Sym*>(ptr);
}

bool resolve_symbol(const char* name,
                    const LoadedObject* objects,
                    size_t object_count,
                    uint64_t& out_value) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    for (size_t obj_index = 0; obj_index < object_count; ++obj_index) {
        const LoadedObject& object = objects[obj_index];
        for (size_t sym_index = 0;
             sym_index < object.dynamic.dynsym_count;
             ++sym_index) {
            const Elf64Sym* sym = dynsym_at(object, sym_index);
            if (sym == nullptr || sym->shndx == SHN_UNDEF ||
                sym->name == 0) {
                continue;
            }
            uint32_t bind = elf_symbol_bind(*sym);
            if (bind != STB_GLOBAL && bind != STB_WEAK) {
                continue;
            }
            const char* candidate = nullptr;
            if (!symbol_name_at(object, *sym, candidate)) {
                continue;
            }
            if (!cstring_equal(name, candidate)) {
                continue;
            }
            out_value = object.load_bias + sym->value;
            return true;
        }
    }
    return false;
}

bool map_elf_object(const loader::ProgramImage& image,
                    process::Task& proc,
                    const char* name,
                    bool main_object,
                    LoadedObject& object) {
    if (image.data == nullptr || image.size < sizeof(Elf64Ehdr)) {
        log_message(LogLevel::Error,
                    "Loader: ELF image too small for object");
        return false;
    }
    const auto* header = reinterpret_cast<const Elf64Ehdr*>(image.data);
    if (!validate_elf_header(*header)) {
        return false;
    }

    uint64_t ph_table_end =
        header->phoff + static_cast<uint64_t>(header->phnum) * header->phentsize;
    if (ph_table_end > image.size || ph_table_end < header->phoff) {
        log_message(LogLevel::Error,
                    "Loader: ELF program headers exceed object image");
        return false;
    }

    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    size_t loadable_segments = 0;
    const Elf64Phdr* dynamic_phdr = nullptr;

    for (uint16_t i = 0; i < header->phnum; ++i) {
        const Elf64Phdr* ph = program_header_at(image, *header, i);
        if (ph == nullptr) {
            return false;
        }
        if (ph->type == PT_DYNAMIC) {
            dynamic_phdr = ph;
        }
        if (ph->type != PT_LOAD) {
            continue;
        }

        ++loadable_segments;
        if (ph->memsz == 0) {
            continue;
        }
        if (ph->filesz > ph->memsz) {
            log_message(LogLevel::Error,
                        "Loader: ELF segment filesz exceeds memsz");
            return false;
        }
        if (ph->offset + ph->filesz > image.size ||
            ph->offset + ph->filesz < ph->offset) {
            log_message(LogLevel::Error,
                        "Loader: ELF segment exceeds image size");
            return false;
        }
        uint64_t seg_start = ph->vaddr;
        uint64_t seg_end = seg_start + ph->memsz;
        if (seg_end < seg_start) {
            log_message(LogLevel::Error,
                        "Loader: ELF segment address overflow");
            return false;
        }
        if (seg_start < min_vaddr) {
            min_vaddr = seg_start;
        }
        if (seg_end > max_vaddr) {
            max_vaddr = seg_end;
        }
    }

    if (loadable_segments == 0 || min_vaddr == UINT64_MAX ||
        max_vaddr <= min_vaddr) {
        log_message(LogLevel::Error,
                    "Loader: ELF object has no loadable segments");
        return false;
    }
    if (main_object &&
        (header->entry < min_vaddr || header->entry >= max_vaddr)) {
        log_message(LogLevel::Error,
                    "Loader: ELF entry point 0x%llx outside load range",
                    static_cast<unsigned long long>(header->entry));
        return false;
    }

    uint64_t aligned_min = align_down(min_vaddr, kPageSize);
    uint64_t aligned_max = align_up(max_vaddr, kPageSize);
    uint64_t aligned_span = aligned_max - aligned_min;
    vm::Region region =
        vm::allocate_user_region(proc.cr3, static_cast<size_t>(aligned_span));
    if (region.base == 0) {
        log_message(LogLevel::Error,
                    "Loader: failed to allocate ELF object region");
        return false;
    }

    uint64_t load_bias = region.base - aligned_min;
    for (uint16_t i = 0; i < header->phnum; ++i) {
        const Elf64Phdr* ph = program_header_at(image, *header, i);
        if (ph == nullptr || ph->type != PT_LOAD || ph->memsz == 0) {
            continue;
        }
        uint64_t dest = load_bias + ph->vaddr;
        if (ph->filesz != 0) {
            if (!vm::copy_to_user(proc.cr3,
                                  dest,
                                  image.data + ph->offset,
                                  static_cast<size_t>(ph->filesz))) {
                log_message(LogLevel::Error,
                            "Loader: failed to copy ELF object segment");
                return false;
            }
        }
        if (ph->memsz > ph->filesz) {
            uint64_t bss_base = dest + ph->filesz;
            size_t bss_len = static_cast<size_t>(ph->memsz - ph->filesz);
            if (!vm::fill_user(proc.cr3, bss_base, 0, bss_len)) {
                log_message(LogLevel::Error,
                            "Loader: failed to clear ELF object segment");
                return false;
            }
        }
    }

    memset(&object, 0, sizeof(object));
    object.data = image.data;
    object.size = image.size;
    object.region = region;
    object.load_bias = load_bias;
    object.min_vaddr = min_vaddr;
    object.max_vaddr = max_vaddr;
    object.entry = header->entry;
    object.main_object = main_object;
    if (!copy_cstring(object.name,
                      sizeof(object.name),
                      name != nullptr ? name : "(main)")) {
        return false;
    }
    if (!parse_dynamic_info(image, *header, dynamic_phdr, object.dynamic)) {
        return false;
    }
    return true;
}

bool protect_elf_object_pages(const LoadedObject& object,
                              process::Task& proc) {
    loader::ProgramImage image{object.data, object.size, 0};
    const auto* header = reinterpret_cast<const Elf64Ehdr*>(object.data);
    uint64_t aligned_min = align_down(object.min_vaddr, kPageSize);
    uint64_t aligned_max = align_up(object.max_vaddr, kPageSize);

    for (uint64_t page = aligned_min; page < aligned_max; page += kPageSize) {
        bool writable = false;
        bool executable = false;
        bool covered = false;

        for (uint16_t i = 0; i < header->phnum; ++i) {
            const Elf64Phdr* ph = program_header_at(image, *header, i);
            if (ph == nullptr || ph->type != PT_LOAD || ph->memsz == 0) {
                continue;
            }
            uint64_t seg_start = align_down(ph->vaddr, kPageSize);
            uint64_t seg_end = align_up(ph->vaddr + ph->memsz, kPageSize);
            if (page < seg_start || page >= seg_end) {
                continue;
            }
            covered = true;
            if ((ph->flags & PF_W) != 0) {
                writable = true;
            }
            if ((ph->flags & PF_X) != 0) {
                executable = true;
            }
        }
        if (!covered) {
            continue;
        }
        if (writable && executable) {
            log_message(LogLevel::Error,
                        "Loader: refusing writable executable ELF object page");
            return false;
        }
        uint64_t address = object.load_bias + page;
        if (!vm::set_user_region_writable(proc.cr3,
                                          address,
                                          kPageSize,
                                          writable) ||
            !vm::set_user_region_executable(proc.cr3,
                                            address,
                                            kPageSize,
                                            executable)) {
            log_message(LogLevel::Error,
                        "Loader: failed to protect ELF object page");
            return false;
        }
    }
    return true;
}

bool apply_relocation(const LoadedObject& object,
                      const Elf64Rela& rela,
                      const LoadedObject* objects,
                      size_t object_count,
                      process::Task& proc) {
    uint32_t type = static_cast<uint32_t>(rela.info & 0xFFFFFFFFu);
    uint32_t sym_index = static_cast<uint32_t>(rela.info >> 32);
    uint64_t value = 0;

    switch (type) {
        case R_X86_64_RELATIVE:
            value = object.load_bias + static_cast<uint64_t>(rela.addend);
            break;
        case R_X86_64_64:
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT: {
            const Elf64Sym* sym = dynsym_at(object, sym_index);
            if (sym == nullptr) {
                log_message(LogLevel::Error,
                            "Loader: relocation references missing symbol");
                return false;
            }
            const char* name = nullptr;
            if (!symbol_name_at(object, *sym, name)) {
                log_message(LogLevel::Error,
                            "Loader: relocation symbol has no name");
                return false;
            }
            if (!resolve_symbol(name, objects, object_count, value)) {
                if (elf_symbol_bind(*sym) == STB_WEAK) {
                    value = 0;
                } else {
                    log_message(LogLevel::Error,
                                "Loader: unresolved symbol %s",
                                name);
                    return false;
                }
            }
            value += static_cast<uint64_t>(rela.addend);
            break;
        }
        default:
            log_message(LogLevel::Error,
                        "Loader: unsupported dynamic relocation type %u",
                        type);
            return false;
    }

    uint64_t target = object.load_bias + rela.offset;
    if (!vm::copy_to_user(proc.cr3, target, &value, sizeof(value))) {
        log_message(LogLevel::Error,
                    "Loader: failed to apply dynamic relocation");
        return false;
    }
    return true;
}

bool apply_relocation_table(const LoadedObject& object,
                            uint64_t rela_addr,
                            uint64_t rela_size,
                            uint64_t rela_ent,
                            const LoadedObject* objects,
                            size_t object_count,
                            process::Task& proc) {
    if (rela_addr == 0 || rela_size == 0) {
        return true;
    }
    if (rela_ent == 0) {
        rela_ent = sizeof(Elf64Rela);
    }
    if (rela_ent < sizeof(Elf64Rela)) {
        log_message(LogLevel::Error,
                    "Loader: relocation entry size too small");
        return false;
    }

    loader::ProgramImage image{object.data, object.size, 0};
    const auto* header = reinterpret_cast<const Elf64Ehdr*>(object.data);
    size_t rela_count = static_cast<size_t>(rela_size / rela_ent);
    for (size_t i = 0; i < rela_count; ++i) {
        const uint8_t* ptr =
            image_ptr_at_vaddr(image,
                               *header,
                               rela_addr + i * rela_ent,
                               sizeof(Elf64Rela));
        if (ptr == nullptr) {
            log_message(LogLevel::Error,
                        "Loader: relocation table exceeds image");
            return false;
        }
        const auto* rela = reinterpret_cast<const Elf64Rela*>(ptr);
        if (!apply_relocation(object,
                              *rela,
                              objects,
                              object_count,
                              proc)) {
            return false;
        }
    }
    return true;
}

bool apply_dynamic_relocations(const LoadedObject* objects,
                               size_t object_count,
                               process::Task& proc) {
    for (size_t i = 0; i < object_count; ++i) {
        const LoadedObject& object = objects[i];
        if (!apply_relocation_table(object,
                                    object.dynamic.rela_addr,
                                    object.dynamic.rela_size,
                                    object.dynamic.rela_ent,
                                    objects,
                                    object_count,
                                    proc)) {
            return false;
        }
        if (!apply_relocation_table(object,
                                    object.dynamic.jmprel_addr,
                                    object.dynamic.pltrel_size,
                                    sizeof(Elf64Rela),
                                    objects,
                                    object_count,
                                    proc)) {
            return false;
        }
    }
    return true;
}

bool object_already_loaded(const LoadedObject* objects,
                           size_t object_count,
                           const char* name) {
    for (size_t i = 0; i < object_count; ++i) {
        if (cstring_equal(objects[i].name, name)) {
            return true;
        }
    }
    return false;
}

bool load_needed_object(const char* name,
                        LoadedObject* objects,
                        size_t& object_count,
                        process::Task& proc) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    if (object_already_loaded(objects, object_count, name)) {
        return true;
    }
    if (object_count >= kMaxSharedObjects) {
        log_message(LogLevel::Error,
                    "Loader: too many shared objects");
        return false;
    }

    char path[kMaxSharedObjectPath];
    if (!build_library_path(name, path, sizeof(path))) {
        log_message(LogLevel::Error,
                    "Loader: shared object name too long");
        return false;
    }

    loader::ProgramImage image{};
    uint8_t* image_buffer = nullptr;
    if (!read_shared_object_image(path, image, image_buffer)) {
        log_message(LogLevel::Error,
                    "Loader: failed to read shared object %s",
                    path);
        return false;
    }
    if (!looks_like_elf(image.data, image.size)) {
        log_message(LogLevel::Error,
                    "Loader: shared object is not ELF: %s",
                    path);
        memory::free_kernel(image_buffer);
        return false;
    }

    LoadedObject object{};
    if (!map_elf_object(image, proc, name, false, object)) {
        memory::free_kernel(image_buffer);
        return false;
    }
    objects[object_count++] = object;

    LoadedObject& loaded = objects[object_count - 1];
    for (size_t i = 0; i < loaded.dynamic.needed_count; ++i) {
        char needed[kMaxSharedObjectName];
        if (!needed_name_at(loaded,
                            loaded.dynamic.needed_offsets[i],
                            needed,
                            sizeof(needed))) {
            log_message(LogLevel::Error,
                        "Loader: failed to read nested dependency name");
            return false;
        }
        if (!load_needed_object(needed, objects, object_count, proc)) {
            return false;
        }
    }
    return true;
}

void free_dependency_images(LoadedObject* objects, size_t object_count) {
    for (size_t i = 1; i < object_count; ++i) {
        memory::free_kernel(const_cast<uint8_t*>(objects[i].data));
        objects[i].data = nullptr;
        objects[i].size = 0;
    }
}

bool load_dynamic_elf_binary(const loader::ProgramImage& image,
                             process::Task& proc) {
    LoadedObject objects[kMaxSharedObjects]{};
    size_t object_count = 0;

    if (!map_elf_object(image, proc, "(main)", true, objects[object_count])) {
        return false;
    }
    ++object_count;

    for (size_t i = 0; i < objects[0].dynamic.needed_count; ++i) {
        char needed[kMaxSharedObjectName];
        if (!needed_name_at(objects[0],
                            objects[0].dynamic.needed_offsets[i],
                            needed,
                            sizeof(needed))) {
            log_message(LogLevel::Error,
                        "Loader: failed to read dependency name");
            free_dependency_images(objects, object_count);
            return false;
        }
        if (!load_needed_object(needed, objects, object_count, proc)) {
            free_dependency_images(objects, object_count);
            return false;
        }
    }

    if (!apply_dynamic_relocations(objects, object_count, proc)) {
        free_dependency_images(objects, object_count);
        return false;
    }
    for (size_t i = 0; i < object_count; ++i) {
        if (!protect_elf_object_pages(objects[i], proc)) {
            free_dependency_images(objects, object_count);
            return false;
        }
    }

    uint64_t entry_va = objects[0].load_bias + objects[0].entry;
    uint64_t entry_page = align_down(entry_va, kPageSize);
    uint64_t entry_phys = 0;
    uint64_t entry_flags = 0;
    if (paging_resolve_cr3(proc.cr3, entry_va, entry_phys) &&
        paging_flags_cr3(proc.cr3, entry_va, entry_flags) &&
        (entry_flags & PAGE_FLAG_WRITE) != 0) {
        log_message(LogLevel::Warn,
                    "Loader: dynamic entry page writable, forcing readonly va=%016llx",
                    static_cast<unsigned long long>(entry_page));
        if (!vm::set_user_region_writable(proc.cr3,
                                          entry_page,
                                          kPageSize,
                                          false)) {
            free_dependency_images(objects, object_count);
            return false;
        }
    }

    proc.code_region = objects[0].region;
    proc.user_ip = entry_va;
    free_dependency_images(objects, object_count);
    return true;
}

bool load_elf_binary(const loader::ProgramImage& image,
                     process::Task& proc) {
    if (image.data == nullptr || image.size < sizeof(Elf64Ehdr)) {
        log_message(LogLevel::Error,
                    "Loader: ELF image too small for header");
        return false;
    }

    const auto* header =
        reinterpret_cast<const Elf64Ehdr*>(image.data);

    if (!validate_elf_header(*header)) {
        return false;
    }

    uint64_t ph_table_end =
        header->phoff + static_cast<uint64_t>(header->phnum) * header->phentsize;
    if (ph_table_end > image.size || ph_table_end < header->phoff) {
        log_message(LogLevel::Error,
                    "Loader: ELF program headers exceed image");
        return false;
    }

    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    size_t loadable_segments = 0;
    const Elf64Phdr* dynamic_phdr = nullptr;

    for (uint16_t i = 0; i < header->phnum; ++i) {
        uint64_t ph_offset = header->phoff + static_cast<uint64_t>(i) * header->phentsize;
        const auto* ph = reinterpret_cast<const Elf64Phdr*>(image.data + ph_offset);
        if (ph->type == PT_DYNAMIC) {
            dynamic_phdr = ph;
        }
        if (ph->type != PT_LOAD) {
            continue;
        }

        ++loadable_segments;
        if (ph->memsz == 0) {
            continue;
        }

        if (ph->filesz > ph->memsz) {
            log_message(LogLevel::Error,
                        "Loader: ELF segment filesz exceeds memsz");
            return false;
        }

        if (ph->offset + ph->filesz > image.size ||
            ph->offset + ph->filesz < ph->offset) {
            log_message(LogLevel::Error,
                        "Loader: ELF segment exceeds image size");
            return false;
        }

        uint64_t seg_start = ph->vaddr;
        uint64_t seg_end = seg_start + ph->memsz;
        if (seg_end < seg_start) {
            log_message(LogLevel::Error,
                        "Loader: ELF segment address overflow");
            return false;
        }

        if (seg_start < min_vaddr) {
            min_vaddr = seg_start;
        }
        if (seg_end > max_vaddr) {
            max_vaddr = seg_end;
        }
    }

    if (loadable_segments == 0 || min_vaddr == UINT64_MAX || max_vaddr <= min_vaddr) {
        log_message(LogLevel::Error,
                    "Loader: ELF has no loadable segments");
        return false;
    }

    uint64_t entry = header->entry;
    if (entry < min_vaddr || entry >= max_vaddr) {
        log_message(LogLevel::Error,
                    "Loader: ELF entry point 0x%llx outside load range",
                    static_cast<unsigned long long>(entry));
        return false;
    }

    uint64_t aligned_min = align_down(min_vaddr, kPageSize);
    uint64_t aligned_max = align_up(max_vaddr, kPageSize);
    uint64_t aligned_span = aligned_max - aligned_min;

    vm::Region region =
        vm::allocate_user_region(proc.cr3, static_cast<size_t>(aligned_span));
    if (region.base == 0) {
        log_message(LogLevel::Error,
                    "Loader: failed to allocate region for ELF process %u",
                    static_cast<unsigned int>(proc.pid));
        return false;
    }

    uint64_t load_bias = region.base - aligned_min;

    for (uint16_t i = 0; i < header->phnum; ++i) {
        uint64_t ph_offset = header->phoff + static_cast<uint64_t>(i) * header->phentsize;
        const auto* ph = reinterpret_cast<const Elf64Phdr*>(image.data + ph_offset);
        if (ph->type != PT_LOAD || ph->memsz == 0) {
            continue;
        }

        uint64_t dest = load_bias + ph->vaddr;
        if (ph->filesz != 0) {
            const uint8_t* src = image.data + ph->offset;
            if (!vm::copy_to_user(proc.cr3,
                                  dest,
                                  src,
                                  static_cast<size_t>(ph->filesz))) {
                log_message(LogLevel::Error,
                            "Loader: failed to copy ELF segment %u",
                            static_cast<unsigned int>(i));
                return false;
            }
        }
        if (ph->memsz > ph->filesz) {
            uint64_t bss_base = dest + ph->filesz;
            size_t bss_len = static_cast<size_t>(ph->memsz - ph->filesz);
            if (!vm::fill_user(proc.cr3, bss_base, 0, bss_len)) {
                log_message(LogLevel::Error,
                            "Loader: failed to clear ELF segment %u",
                            static_cast<unsigned int>(i));
                return false;
            }
        }
    }

    if (dynamic_phdr != nullptr) {
        size_t dyn_count =
            static_cast<size_t>(dynamic_phdr->memsz / sizeof(Elf64Dyn));

        uint64_t rela_addr = 0;
        uint64_t rela_size = 0;
        uint64_t rela_ent = 0;

        for (size_t i = 0; i < dyn_count; ++i) {
            Elf64Dyn dyn{};
            uint64_t dyn_addr =
                load_bias + dynamic_phdr->vaddr + (i * sizeof(Elf64Dyn));
            if (!vm::copy_from_user(proc.cr3,
                                    &dyn,
                                    dyn_addr,
                                    sizeof(Elf64Dyn))) {
                log_message(LogLevel::Error,
                            "Loader: failed to read dynamic table entry");
                return false;
            }
            int64_t tag = dyn.tag;
            if (tag == DT_NULL) {
                break;
            }
            switch (tag) {
                case DT_RELA:
                    rela_addr = dyn.val;
                    break;
                case DT_RELASZ:
                    rela_size = dyn.val;
                    break;
                case DT_RELAENT:
                    rela_ent = dyn.val;
                    break;
                default:
                    break;
            }
        }

        if (rela_addr != 0 && rela_size != 0) {
            if (rela_ent == 0) {
                rela_ent = sizeof(Elf64Rela);
            }
            if (rela_ent < sizeof(Elf64Rela)) {
                log_message(LogLevel::Error,
                            "Loader: relocation entry size too small");
                return false;
            }
            size_t rela_count = static_cast<size_t>(rela_size / rela_ent);
            for (size_t i = 0; i < rela_count; ++i) {
                Elf64Rela rela{};
                uint64_t entry_addr = load_bias + rela_addr + (i * rela_ent);
                if (!vm::copy_from_user(proc.cr3,
                                        &rela,
                                        entry_addr,
                                        sizeof(Elf64Rela))) {
                    log_message(LogLevel::Error,
                                "Loader: failed to read relocation entry");
                    return false;
                }
                uint32_t type = static_cast<uint32_t>(rela.info & 0xFFFFFFFFu);
                switch (type) {
                    case R_X86_64_RELATIVE: {
                        uint64_t value =
                            load_bias + static_cast<uint64_t>(rela.addend);
                        uint64_t target = load_bias + rela.offset;
                        if (!vm::copy_to_user(proc.cr3,
                                              target,
                                              &value,
                                              sizeof(value))) {
                            log_message(LogLevel::Error,
                                        "Loader: failed to apply relocation");
                            return false;
                        }
                        break;
                    }
                    default:
                        log_message(LogLevel::Error,
                                    "Loader: unsupported relocation type %u",
                                    type);
                        return false;
                }
            }
        }
    }

    for (uint64_t page = aligned_min; page < aligned_max; page += kPageSize) {
        bool writable = false;
        bool executable = false;
        bool covered = false;

        for (uint16_t i = 0; i < header->phnum; ++i) {
            uint64_t ph_offset =
                header->phoff + static_cast<uint64_t>(i) * header->phentsize;
            const auto* ph =
                reinterpret_cast<const Elf64Phdr*>(image.data + ph_offset);
            if (ph->type != PT_LOAD || ph->memsz == 0) {
                continue;
            }

            uint64_t seg_start = align_down(ph->vaddr, kPageSize);
            uint64_t seg_end = align_up(ph->vaddr + ph->memsz, kPageSize);
            if (page < seg_start || page >= seg_end) {
                continue;
            }

            covered = true;
            if ((ph->flags & PF_W) != 0) {
                writable = true;
            }
            if ((ph->flags & PF_X) != 0) {
                executable = true;
            }
        }

        if (!covered) {
            continue;
        }

        if (writable && executable) {
            log_message(LogLevel::Error,
                        "Loader: refusing writable executable ELF page");
            return false;
        }

        uint64_t page_addr = load_bias + page;
        if (!vm::set_user_region_writable(proc.cr3,
                                          page_addr,
                                          kPageSize,
                                          writable) ||
            !vm::set_user_region_executable(proc.cr3,
                                            page_addr,
                                            kPageSize,
                                            executable)) {
            log_message(LogLevel::Error,
                        "Loader: failed to protect ELF page 0x%llx",
                        static_cast<unsigned long long>(page_addr));
            return false;
        }
    }

    uint64_t entry_va = load_bias + entry;
    uint64_t entry_page = align_down(entry_va, kPageSize);
    uint64_t entry_phys = 0;
    uint64_t entry_flags = 0;
    if (paging_resolve_cr3(proc.cr3, entry_va, entry_phys) &&
        paging_flags_cr3(proc.cr3, entry_va, entry_flags)) {
        if ((entry_flags & PAGE_FLAG_WRITE) != 0) {
            log_message(LogLevel::Warn,
                        "Loader: entry page still writable, forcing readonly pid=%u va=%016llx flags=%016llx",
                        static_cast<unsigned int>(proc.pid),
                        static_cast<unsigned long long>(entry_page),
                        static_cast<unsigned long long>(entry_flags));
            if (!vm::set_user_region_writable(proc.cr3,
                                              entry_page,
                                              kPageSize,
                                              false)) {
                log_message(LogLevel::Error,
                            "Loader: failed to force entry page readonly va=%016llx",
                            static_cast<unsigned long long>(entry_page));
                return false;
            }
            paging_flags_cr3(proc.cr3, entry_va, entry_flags);
        }
    }

    proc.code_region = region;
    proc.user_ip = load_bias + entry;
    return true;
}

bool read_exact(vfs::FileHandle& file,
                uint64_t offset,
                void* buffer,
                size_t length) {
    auto* bytes = static_cast<uint8_t*>(buffer);
    size_t total = 0;
    while (total < length) {
        if (offset > UINT64_MAX - total) {
            return false;
        }
        size_t read = 0;
        if (!vfs::read_file(file,
                            offset + total,
                            bytes + total,
                            length - total,
                            read) ||
            read == 0) {
            return false;
        }
        total += read;
    }
    return true;
}

bool load_static_elf_file(vfs::FileHandle& file,
                          const Elf64Ehdr& header,
                          const Elf64Phdr* phdrs,
                          process::Task& proc) {
    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;
    size_t loadable_segments = 0;
    const Elf64Phdr* dynamic_phdr = nullptr;

    for (uint16_t i = 0; i < header.phnum; ++i) {
        const Elf64Phdr& ph = phdrs[i];
        if (ph.type == PT_DYNAMIC) {
            dynamic_phdr = &ph;
        }
        if (ph.type != PT_LOAD) {
            continue;
        }
        ++loadable_segments;
        if (ph.filesz > ph.memsz || ph.offset > file.size ||
            ph.filesz > file.size - ph.offset ||
            ph.vaddr > UINT64_MAX - ph.memsz) {
            log_message(LogLevel::Error,
                        "Loader: invalid streamed ELF segment");
            return false;
        }
        if (ph.memsz == 0) {
            continue;
        }
        if (ph.vaddr < min_vaddr) min_vaddr = ph.vaddr;
        if (ph.vaddr + ph.memsz > max_vaddr) {
            max_vaddr = ph.vaddr + ph.memsz;
        }
    }
    if (loadable_segments == 0 || min_vaddr == UINT64_MAX ||
        max_vaddr <= min_vaddr || header.entry < min_vaddr ||
        header.entry >= max_vaddr) {
        log_message(LogLevel::Error,
                    "Loader: invalid streamed ELF load range");
        return false;
    }

    uint64_t aligned_min = align_down(min_vaddr, kPageSize);
    uint64_t aligned_max = align_up(max_vaddr, kPageSize);
    if (aligned_max < max_vaddr || aligned_max <= aligned_min ||
        aligned_max - aligned_min > SIZE_MAX) {
        return false;
    }
    size_t span = static_cast<size_t>(aligned_max - aligned_min);
    vm::Usage usage = vm::usage(proc.cr3);
    uint64_t limit = proc.resources->limits.max_virtual_bytes;
    if (usage.virtual_bytes > limit || span > limit - usage.virtual_bytes) {
        log_message(LogLevel::Error,
                    "Loader: executable exceeds process virtual-memory limit");
        return false;
    }
    vm::Region region = vm::allocate_user_region(proc.cr3, span);
    if (region.base == 0) {
        return false;
    }
    uint64_t load_bias = region.base - aligned_min;

    constexpr size_t kStreamChunk = 4096;
    auto* buffer = static_cast<uint8_t*>(
        memory::alloc_kernel(kStreamChunk, alignof(uint64_t)));
    if (buffer == nullptr) {
        return false;
    }
    bool copied = true;
    for (uint16_t i = 0; i < header.phnum && copied; ++i) {
        const Elf64Phdr& ph = phdrs[i];
        if (ph.type != PT_LOAD || ph.memsz == 0) continue;
        uint64_t copied_bytes = 0;
        while (copied_bytes < ph.filesz) {
            size_t chunk = static_cast<size_t>(ph.filesz - copied_bytes);
            if (chunk > kStreamChunk) chunk = kStreamChunk;
            if (!read_exact(file, ph.offset + copied_bytes, buffer, chunk) ||
                !vm::copy_to_user(proc.cr3,
                                  load_bias + ph.vaddr + copied_bytes,
                                  buffer,
                                  chunk)) {
                copied = false;
                break;
            }
            copied_bytes += chunk;
        }
        if (copied && ph.memsz > ph.filesz &&
            !vm::fill_user(proc.cr3,
                           load_bias + ph.vaddr + ph.filesz,
                           0,
                           static_cast<size_t>(ph.memsz - ph.filesz))) {
            copied = false;
        }
    }
    memory::free_kernel(buffer);
    if (!copied) {
        return false;
    }

    if (dynamic_phdr != nullptr) {
        if (dynamic_phdr->vaddr < min_vaddr ||
            dynamic_phdr->vaddr > max_vaddr ||
            dynamic_phdr->memsz > max_vaddr - dynamic_phdr->vaddr) {
            log_message(LogLevel::Error,
                        "Loader: streamed dynamic table is outside load range");
            return false;
        }
        uint64_t rela_addr = 0;
        uint64_t rela_size = 0;
        uint64_t rela_ent = sizeof(Elf64Rela);
        size_t dyn_count =
            static_cast<size_t>(dynamic_phdr->memsz / sizeof(Elf64Dyn));
        for (size_t i = 0; i < dyn_count; ++i) {
            Elf64Dyn dyn{};
            if (!vm::copy_from_user(
                    proc.cr3,
                    &dyn,
                    load_bias + dynamic_phdr->vaddr + i * sizeof(dyn),
                    sizeof(dyn))) {
                return false;
            }
            if (dyn.tag == DT_NULL) break;
            if (dyn.tag == DT_RELA) rela_addr = dyn.val;
            if (dyn.tag == DT_RELASZ) rela_size = dyn.val;
            if (dyn.tag == DT_RELAENT) rela_ent = dyn.val;
        }
        if (rela_addr != 0 && rela_size != 0) {
            if (rela_ent < sizeof(Elf64Rela) ||
                rela_size / rela_ent > SIZE_MAX) {
                return false;
            }
            size_t count = static_cast<size_t>(rela_size / rela_ent);
            for (size_t i = 0; i < count; ++i) {
                Elf64Rela rela{};
                if (!vm::copy_from_user(proc.cr3,
                                        &rela,
                                        load_bias + rela_addr + i * rela_ent,
                                        sizeof(rela)) ||
                    static_cast<uint32_t>(rela.info) != R_X86_64_RELATIVE) {
                    log_message(LogLevel::Error,
                                "Loader: unsupported streamed ELF relocation");
                    return false;
                }
                if (rela.offset < aligned_min ||
                    rela.offset > aligned_max - sizeof(uint64_t)) {
                    log_message(LogLevel::Error,
                                "Loader: streamed relocation target is outside image");
                    return false;
                }
                uint64_t value =
                    load_bias + static_cast<uint64_t>(rela.addend);
                if (!vm::copy_to_user(proc.cr3,
                                      load_bias + rela.offset,
                                      &value,
                                      sizeof(value))) {
                    return false;
                }
            }
        }
    }

    for (uint64_t page = aligned_min; page < aligned_max; page += kPageSize) {
        bool covered = false;
        bool writable = false;
        bool executable = false;
        for (uint16_t i = 0; i < header.phnum; ++i) {
            const Elf64Phdr& ph = phdrs[i];
            if (ph.type != PT_LOAD || ph.memsz == 0) continue;
            uint64_t start = align_down(ph.vaddr, kPageSize);
            uint64_t end = align_up(ph.vaddr + ph.memsz, kPageSize);
            if (page < start || page >= end) continue;
            covered = true;
            writable = writable || (ph.flags & PF_W) != 0;
            executable = executable || (ph.flags & PF_X) != 0;
        }
        if (!covered) continue;
        if (writable && executable) {
            log_message(LogLevel::Error,
                        "Loader: refusing writable executable streamed ELF page");
            return false;
        }
        uint64_t address = load_bias + page;
        if (!vm::set_user_region_writable(
                proc.cr3, address, kPageSize, writable) ||
            !vm::set_user_region_executable(
                proc.cr3, address, kPageSize, executable)) {
            return false;
        }
    }

    proc.code_region = region;
    proc.user_ip = load_bias + header.entry;
    return true;
}

bool file_has_needed_dependencies(vfs::FileHandle& file,
                                  const Elf64Ehdr& header,
                                  const Elf64Phdr* phdrs) {
    for (uint16_t i = 0; i < header.phnum; ++i) {
        const Elf64Phdr& ph = phdrs[i];
        if (ph.type != PT_DYNAMIC || ph.filesz == 0 ||
            ph.offset > file.size || ph.filesz > file.size - ph.offset) {
            continue;
        }
        size_t count = static_cast<size_t>(ph.filesz / sizeof(Elf64Dyn));
        for (size_t j = 0; j < count; ++j) {
            Elf64Dyn dyn{};
            if (!read_exact(file,
                            ph.offset + j * sizeof(dyn),
                            &dyn,
                            sizeof(dyn)) ||
                dyn.tag == DT_NULL) {
                break;
            }
            if (dyn.tag == DT_NEEDED) return true;
        }
    }
    return false;
}

}  // namespace

namespace loader {

bool load_into_process(const ProgramImage& image, process::Task& proc) {
    bool loaded = false;
    if (looks_like_elf(image.data, image.size)) {
        if (image_has_needed_dependencies(image)) {
            loaded = load_dynamic_elf_binary(image, proc);
        } else {
            loaded = load_elf_binary(image, proc);
        }
    } else {
        loaded = load_flat_binary(image, proc);
    }

    if (!loaded) {
        return false;
    }

    if (!setup_user_stack(proc)) {
        return false;
    }

    proc.has_context = false;
    process::store_state(proc, process::State::Ready);
    return true;
}

bool load_file_into_process(const char* path, process::Task& proc) {
    if (path == nullptr) return false;
    vfs::FileHandle file{};
    if (!vfs::open_file(path, file) || file.size < sizeof(Elf64Ehdr)) {
        vfs::close_file(file);
        return false;
    }
    Elf64Ehdr header{};
    if (!read_exact(file, 0, &header, sizeof(header)) ||
        !looks_like_elf(reinterpret_cast<const uint8_t*>(&header),
                        sizeof(header)) ||
        !validate_elf_header(header) ||
        header.phoff > file.size ||
        static_cast<uint64_t>(header.phnum) >
            (file.size - header.phoff) / sizeof(Elf64Phdr) ||
        header.phnum > SIZE_MAX / sizeof(Elf64Phdr)) {
        vfs::close_file(file);
        return false;
    }
    size_t phdr_bytes = static_cast<size_t>(header.phnum) * sizeof(Elf64Phdr);
    auto* phdrs = static_cast<Elf64Phdr*>(
        memory::alloc_kernel(phdr_bytes, alignof(Elf64Phdr)));
    if (phdrs == nullptr ||
        !read_exact(file, header.phoff, phdrs, phdr_bytes)) {
        memory::free_kernel(phdrs);
        vfs::close_file(file);
        return false;
    }

    bool loaded = false;
    if (file_has_needed_dependencies(file, header, phdrs)) {
        if (file.size <= SIZE_MAX) {
            auto* image_data = static_cast<uint8_t*>(
                memory::alloc_kernel(static_cast<size_t>(file.size), 16));
            if (image_data != nullptr &&
                read_exact(file, 0, image_data, static_cast<size_t>(file.size))) {
                ProgramImage image{
                    image_data, static_cast<size_t>(file.size), 0};
                loaded = load_dynamic_elf_binary(image, proc);
            }
            memory::free_kernel(image_data);
        }
    } else {
        loaded = load_static_elf_file(file, header, phdrs, proc);
    }
    memory::free_kernel(phdrs);
    vfs::close_file(file);
    if (!loaded || !setup_user_stack(proc)) return false;
    proc.has_context = false;
    process::store_state(proc, process::State::Ready);
    return true;
}

}  // namespace loader
