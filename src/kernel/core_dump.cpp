#include "core_dump.hpp"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/isr.hpp"
#include "drivers/log/logging.hpp"
#include "fs/vfs.hpp"
#include "kernel/capabilities.hpp"
#include "kernel/cmdline.hpp"
#include "kernel/process.hpp"
#include "kernel/string_util.hpp"
#include "kernel/vm.hpp"
#include "kernel/work.hpp"
#include "lib/mem.hpp"

namespace {

constexpr size_t kMaxJobs = 4;
constexpr size_t kMaxAreas = 128;
constexpr size_t kPageSize = 4096;
constexpr uint64_t kMaxDumpBytes = 64ull * 1024 * 1024;
constexpr uint16_t kElfTypeCore = 4;
constexpr uint16_t kElfMachineX86_64 = 62;
constexpr uint32_t kProgramTypeLoad = 1;
constexpr uint32_t kProgramTypeNote = 4;
constexpr uint32_t kProgramFlagExecute = 1;
constexpr uint32_t kProgramFlagWrite = 2;
constexpr uint32_t kProgramFlagRead = 4;
constexpr uint32_t kNotePrStatus = 1;
constexpr uint32_t kNotePrPsInfo = 3;

struct ElfHeader {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t program_header_offset;
    uint64_t section_header_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_header_size;
    uint16_t program_header_count;
    uint16_t section_header_size;
    uint16_t section_header_count;
    uint16_t section_name_index;
};

struct ProgramHeader {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
};

struct NoteHeader {
    uint32_t name_size;
    uint32_t descriptor_size;
    uint32_t type;
};

struct TimeValue {
    int64_t seconds;
    int64_t microseconds;
};

struct ElfPrStatus {
    int32_t signal_number;
    int32_t signal_code;
    int32_t signal_errno;
    int16_t current_signal;
    uint16_t padding;
    uint64_t pending_signals;
    uint64_t held_signals;
    int32_t pid;
    int32_t parent_pid;
    int32_t process_group;
    int32_t session;
    TimeValue user_time;
    TimeValue system_time;
    TimeValue child_user_time;
    TimeValue child_system_time;
    uint64_t registers[27];
    int32_t fp_valid;
    int32_t tail_padding;
};

struct ElfPrPsInfo {
    uint8_t state;
    char state_name;
    uint8_t zombie;
    int8_t nice;
    uint32_t padding;
    uint64_t flags;
    uint32_t uid;
    uint32_t gid;
    int32_t pid;
    int32_t parent_pid;
    int32_t process_group;
    int32_t session;
    char file_name[16];
    char arguments[80];
};

struct NeutrinoException {
    uint64_t vector;
    uint64_t error_code;
    uint64_t fault_address;
    uint64_t instruction_pointer;
};

static_assert(sizeof(ElfHeader) == 64);
static_assert(sizeof(ProgramHeader) == 56);
static_assert(sizeof(ElfPrStatus) == 336);
static_assert(sizeof(ElfPrPsInfo) == 136);

struct Job {
    bool in_use;
    process::Task* task;
    InterruptFrame frame;
    uint64_t fault_address;
    uint32_t tid;
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t process_group;
    uint32_t session;
    uint64_t owner_machine;
    uint64_t owner_local;
    bool have_owner;
    char image_path[path_util::kMaxPathLength];
};

Job g_jobs[kMaxJobs]{};
uint64_t g_sequence = 0;
// Deferred work is serviced serially on CPU 0. Keep the large dump buffers out
// of the poll worker's bounded 16 KiB kernel stack.
vm::AreaInfo g_areas[kMaxAreas]{};
ProgramHeader g_programs[kMaxAreas + 1]{};
uint8_t g_notes[kPageSize]{};
uint8_t g_page[kPageSize]{};

uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

int signal_for_vector(uint64_t vector) {
    switch (vector) {
        case 0:
        case 16:
        case 19: return 8;   // SIGFPE
        case 1:
        case 3: return 5;    // SIGTRAP
        case 6: return 4;    // SIGILL
        case 17: return 7;   // SIGBUS
        default: return 11;  // SIGSEGV
    }
}

bool append_decimal(char* output,
                    size_t capacity,
                    size_t& position,
                    uint64_t value) {
    char digits[20];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    if (position + count >= capacity) {
        return false;
    }
    while (count != 0) {
        output[position++] = digits[--count];
    }
    output[position] = '\0';
    return true;
}

bool append_text(char* output,
                 size_t capacity,
                 size_t& position,
                 const char* value) {
    if (value == nullptr) {
        return false;
    }
    while (*value != '\0') {
        if (position + 1 >= capacity) {
            return false;
        }
        output[position++] = *value++;
    }
    output[position] = '\0';
    return true;
}

bool write_all(vfs::FileHandle& file,
               uint64_t offset,
               const void* data,
               size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t done = 0;
    while (done < length) {
        size_t written = 0;
        if (!vfs::write_file(file,
                             offset + done,
                             bytes + done,
                             length - done,
                             written) ||
            written == 0) {
            return false;
        }
        done += written;
    }
    return true;
}

void sort_areas(vm::AreaInfo* areas, size_t count) {
    for (size_t i = 1; i < count; ++i) {
        vm::AreaInfo value = areas[i];
        size_t at = i;
        while (at != 0 && areas[at - 1].base > value.base) {
            areas[at] = areas[at - 1];
            --at;
        }
        areas[at] = value;
    }
}

void prioritize_area(vm::AreaInfo* areas,
                     size_t count,
                     uint64_t address,
                     size_t target) {
    if (address == 0 || target >= count) {
        return;
    }
    size_t found = count;
    for (size_t i = 0; i < count; ++i) {
        if (address >= areas[i].base &&
            address - areas[i].base < areas[i].length) {
            found = i;
            break;
        }
    }
    if (found >= count || found <= target) {
        return;
    }
    const vm::AreaInfo value = areas[found];
    while (found > target) {
        areas[found] = areas[found - 1];
        --found;
    }
    areas[target] = value;
}

bool dumpable(const vm::AreaInfo& area) {
    return area.length != 0 && area.kind != vm::MappingKind::Shared &&
           area.kind != vm::MappingKind::Device;
}

void fill_registers(const InterruptFrame& frame, uint64_t (&out)[27]) {
    out[0] = frame.r15;
    out[1] = frame.r14;
    out[2] = frame.r13;
    out[3] = frame.r12;
    out[4] = frame.rbp;
    out[5] = frame.rbx;
    out[6] = frame.r11;
    out[7] = frame.r10;
    out[8] = frame.r9;
    out[9] = frame.r8;
    out[10] = frame.rax;
    out[11] = frame.rcx;
    out[12] = frame.rdx;
    out[13] = frame.rsi;
    out[14] = frame.rdi;
    out[15] = UINT64_MAX;
    out[16] = frame.rip;
    out[17] = frame.cs;
    out[18] = frame.rflags;
    out[19] = frame.rsp;
    out[20] = frame.ss;
}

size_t append_note(uint8_t* buffer,
                   size_t capacity,
                   size_t offset,
                   const char* owner,
                   uint32_t type,
                   const void* descriptor,
                   size_t descriptor_size) {
    if (owner == nullptr) {
        return 0;
    }
    const size_t owner_size = string_util::length(owner) + 1;
    const size_t owner_padded = align_up(owner_size, 4);
    const size_t padded = align_up(descriptor_size, 4);
    const size_t required = sizeof(NoteHeader) + owner_padded + padded;
    if (offset > capacity || required > capacity - offset) {
        return 0;
    }
    const NoteHeader header{
        .name_size = static_cast<uint32_t>(owner_size),
        .descriptor_size = static_cast<uint32_t>(descriptor_size),
        .type = type,
    };
    memcpy(buffer + offset, &header, sizeof(header));
    offset += sizeof(header);
    memset(buffer + offset, 0, owner_padded);
    memcpy(buffer + offset, owner, owner_size);
    offset += owner_padded;
    memcpy(buffer + offset, descriptor, descriptor_size);
    if (padded != descriptor_size) {
        memset(buffer + offset + descriptor_size,
               0,
               padded - descriptor_size);
    }
    return offset + padded;
}

void base_name(const char* path, char* output, size_t output_size) {
    const char* name = path;
    if (path != nullptr) {
        for (const char* cursor = path; *cursor != '\0'; ++cursor) {
            if (*cursor == '/') {
                name = cursor + 1;
            }
        }
    }
    string_util::copy(output, output_size, name);
}

bool protect_file(const Job& job, const char* path) {
    if (!vfs::acl_supported(path)) {
        return true;
    }
    if (!job.have_owner) {
        return false;
    }
    vfs::AclEntry acl{};
    acl.machine_id = job.owner_machine;
    acl.local_id = job.owner_local;
    acl.read = vfs::AclValue::Allow;
    acl.write = vfs::AclValue::Allow;
    acl.delete_permission = vfs::AclValue::Allow;
    acl.edit = vfs::AclValue::Allow;
    return vfs::set_acl(path, &acl, 1);
}

bool write_dump(Job& job, const char* path) {
    size_t area_count = vm::snapshot_areas(job.task->cr3,
                                           g_areas,
                                           kMaxAreas);
    sort_areas(g_areas, area_count);
    // Preserve the most useful mappings when the dump is capped: the
    // faulting stack first, followed by the instruction mapping.
    prioritize_area(g_areas, area_count, job.frame.rsp, 0);
    prioritize_area(g_areas, area_count, job.frame.rip, 1);

    memset(g_programs, 0, sizeof(g_programs));
    size_t program_count = 1;
    uint64_t memory_bytes = 0;
    for (size_t i = 0; i < area_count; ++i) {
        if (!dumpable(g_areas[i]) || memory_bytes == kMaxDumpBytes) {
            continue;
        }
        uint64_t file_size = g_areas[i].length;
        const uint64_t remaining = kMaxDumpBytes - memory_bytes;
        if (file_size > remaining) {
            file_size = remaining;
        }
        if (file_size == 0) {
            continue;
        }
        uint32_t flags = kProgramFlagRead;
        if ((g_areas[i].flags & vm::kMapWrite) != 0) {
            flags |= kProgramFlagWrite;
        }
        if ((g_areas[i].flags & vm::kMapExecute) != 0) {
            flags |= kProgramFlagExecute;
        }
        g_programs[program_count++] = ProgramHeader{
            .type = kProgramTypeLoad,
            .flags = flags,
            .offset = 0,
            .virtual_address = g_areas[i].base,
            .physical_address = 0,
            .file_size = file_size,
            .memory_size = g_areas[i].length,
            .alignment = kPageSize,
        };
        memory_bytes += file_size;
    }

    memset(g_notes, 0, sizeof(g_notes));
    ElfPrStatus status{};
    const int signal = signal_for_vector(job.frame.int_no);
    status.signal_number = signal;
    status.current_signal = static_cast<int16_t>(signal);
    status.pid = static_cast<int32_t>(job.tid);
    status.parent_pid = static_cast<int32_t>(job.parent_pid);
    status.process_group = static_cast<int32_t>(job.process_group);
    status.session = static_cast<int32_t>(job.session);
    fill_registers(job.frame, status.registers);
    size_t notes_size = append_note(g_notes,
                                    sizeof(g_notes),
                                    0,
                                    "CORE",
                                    kNotePrStatus,
                                    &status,
                                    sizeof(status));

    ElfPrPsInfo info{};
    info.state_name = 'R';
    info.pid = static_cast<int32_t>(job.pid);
    info.parent_pid = static_cast<int32_t>(job.parent_pid);
    info.process_group = static_cast<int32_t>(job.process_group);
    info.session = static_cast<int32_t>(job.session);
    base_name(job.image_path, info.file_name, sizeof(info.file_name));
    string_util::copy(info.arguments, sizeof(info.arguments), job.image_path);
    notes_size = append_note(g_notes,
                             sizeof(g_notes),
                             notes_size,
                             "CORE",
                             kNotePrPsInfo,
                             &info,
                             sizeof(info));
    const NeutrinoException exception{
        .vector = job.frame.int_no,
        .error_code = job.frame.err_code,
        .fault_address = job.fault_address,
        .instruction_pointer = job.frame.rip,
    };
    notes_size = append_note(g_notes,
                             sizeof(g_notes),
                             notes_size,
                             "NEUTRINO",
                             1,
                             &exception,
                             sizeof(exception));
    if (notes_size == 0) {
        return false;
    }

    const uint64_t notes_offset = align_up(
        sizeof(ElfHeader) + program_count * sizeof(ProgramHeader),
        kPageSize);
    uint64_t data_offset = align_up(notes_offset + notes_size, kPageSize);
    g_programs[0] = ProgramHeader{
        .type = kProgramTypeNote,
        .flags = 0,
        .offset = notes_offset,
        .virtual_address = 0,
        .physical_address = 0,
        .file_size = notes_size,
        .memory_size = 0,
        .alignment = 4,
    };
    for (size_t i = 1; i < program_count; ++i) {
        g_programs[i].offset = data_offset;
        data_offset = align_up(data_offset + g_programs[i].file_size,
                               kPageSize);
    }

    ElfHeader header{};
    header.ident[0] = 0x7f;
    header.ident[1] = 'E';
    header.ident[2] = 'L';
    header.ident[3] = 'F';
    header.ident[4] = 2;
    header.ident[5] = 1;
    header.ident[6] = 1;
    header.type = kElfTypeCore;
    header.machine = kElfMachineX86_64;
    header.version = 1;
    header.program_header_offset = sizeof(ElfHeader);
    header.header_size = sizeof(ElfHeader);
    header.program_header_size = sizeof(ProgramHeader);
    header.program_header_count = static_cast<uint16_t>(program_count);

    vfs::FileHandle file{};
    if (!vfs::create_file(path, file)) {
        return false;
    }
    bool ok = protect_file(job, path) &&
              write_all(file, 0, &header, sizeof(header)) &&
              write_all(file,
                        sizeof(header),
                        g_programs,
                        program_count * sizeof(ProgramHeader)) &&
              write_all(file, notes_offset, g_notes, notes_size);
    for (size_t i = 1; ok && i < program_count; ++i) {
        uint64_t copied = 0;
        while (copied < g_programs[i].file_size) {
            size_t chunk = kPageSize;
            if (g_programs[i].file_size - copied < chunk) {
                chunk = static_cast<size_t>(g_programs[i].file_size - copied);
            }
            memset(g_page, 0, sizeof(g_page));
            (void)vm::copy_from_user_present(job.task->cr3,
                                             g_page,
                                             g_programs[i].virtual_address + copied,
                                             chunk);
            if (!write_all(file,
                           g_programs[i].offset + copied,
                           g_page,
                           chunk)) {
                ok = false;
                break;
            }
            copied += chunk;
        }
    }
    vfs::close_file(file);
    if (!ok) {
        (void)vfs::remove_file(path);
    }
    return ok;
}

void service_job(void* context) {
    auto& job = *static_cast<Job*>(context);
    char path[path_util::kMaxPathLength]{};
    (void)vfs::create_directory("/cores");
    bool have_path = false;
    // The sequence restarts at boot, while /cores may be persistent. Find an
    // unused name instead of overwriting or failing on an earlier boot's dump.
    for (size_t attempt = 0; attempt < 1024 && !have_path; ++attempt) {
        size_t position = 0;
        const uint64_t sequence =
            __atomic_add_fetch(&g_sequence, 1, __ATOMIC_RELAXED);
        const bool built =
            append_text(path, sizeof(path), position, "/cores/core.") &&
            append_decimal(path, sizeof(path), position, job.pid) &&
            append_text(path, sizeof(path), position, ".") &&
            append_decimal(path, sizeof(path), position, job.tid) &&
            append_text(path, sizeof(path), position, ".") &&
            append_decimal(path, sizeof(path), position, sequence);
        if (!built) {
            break;
        }
        vfs::FileHandle existing{};
        if (!vfs::open_file(path, existing)) {
            have_path = true;
        } else {
            vfs::close_file(existing);
        }
    }
    const bool ok = have_path && write_dump(job, path);
    if (ok) {
        log_message(LogLevel::Info,
                    "Core dump: wrote %s for exception #%u at %016llx (fault=%016llx)",
                    path,
                    static_cast<unsigned int>(job.frame.int_no),
                    static_cast<unsigned long long>(job.frame.rip),
                    static_cast<unsigned long long>(job.fault_address));
    } else {
        log_message(LogLevel::Warn,
                    "Core dump: failed for tid=%u image=%s",
                    static_cast<unsigned int>(job.tid),
                    job.image_path[0] != '\0' ? job.image_path : "(unknown)");
    }
    process::Task* task = job.task;
    __atomic_store_n(&job.in_use, false, __ATOMIC_RELEASE);
    __atomic_store_n(&task->core_dump_pending, false, __ATOMIC_RELEASE);
}

bool enabled() {
    return kernel_cmdline::has_flag("COREDUMP") ||
           kernel_cmdline::has_value("COREDUMP", "ON");
}

}  // namespace

namespace core_dump {

bool schedule(process::Task& task,
              const InterruptFrame& frame,
              uint64_t fault_address) {
    if (!enabled() || task.is_kernel_task ||
        __atomic_exchange_n(&task.core_dump_pending,
                            true,
                            __ATOMIC_ACQ_REL)) {
        return false;
    }
    Job* job = nullptr;
    for (auto& candidate : g_jobs) {
        bool expected = false;
        if (__atomic_compare_exchange_n(&candidate.in_use,
                                        &expected,
                                        true,
                                        false,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            job = &candidate;
            break;
        }
    }
    if (job == nullptr) {
        __atomic_store_n(&task.core_dump_pending, false, __ATOMIC_RELEASE);
        return false;
    }

    job->task = &task;
    job->frame = frame;
    job->fault_address = fault_address;
    job->tid = task.tid;
    job->pid = task.pid != 0 ? task.pid : task.tid;
    job->parent_pid = task.resources != nullptr
                          ? task.resources->parent_process_id
                          : 0;
    job->process_group = task.resources != nullptr
                             ? task.resources->process_group_id
                             : 0;
    job->session = task.resources != nullptr
                       ? task.resources->session_id
                       : 0;
    job->owner_machine = 0;
    job->owner_local = 0;
    job->have_owner = task.resources != nullptr &&
                      capabilities::principal_user_id(
                          task.resources->principal,
                          job->owner_machine,
                          job->owner_local);
    string_util::copy(job->image_path,
                      sizeof(job->image_path),
                      task.image_path);

    if (!work::schedule(service_job, job)) {
        __atomic_store_n(&job->in_use, false, __ATOMIC_RELEASE);
        __atomic_store_n(&task.core_dump_pending, false, __ATOMIC_RELEASE);
        return false;
    }
    return true;
}

}  // namespace core_dump
