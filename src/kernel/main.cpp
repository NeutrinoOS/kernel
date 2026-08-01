#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../drivers/console/console.hpp"
#include "../drivers/driver_registry.hpp"
#include "../drivers/fs/mount_manager.hpp"
#include "../drivers/gpu/intel_uhd.hpp"
#include "../drivers/input/keyboard.hpp"
#include "../drivers/input/mouse.hpp"
#include "../drivers/interrupts/ioapic.hpp"
#include "../drivers/interrupts/pic.hpp"
#include "../drivers/limine/limine_requests.hpp"
#include "drivers/acpi/acpi.hpp"
#include "../drivers/log/logging.hpp"
#include "../drivers/pci/pci.hpp"
#include "../drivers/sensors/acpi_thermal.hpp"
#include "../drivers/sensors/it87.hpp"
#include "../drivers/timer/pit.hpp"
#include "../fs/vfs.hpp"
#include "../net/network.hpp"
#include "arch/x86_64/gdt.hpp"
#include "arch/x86_64/cpu_features.hpp"
#include "arch/x86_64/idt.hpp"
#include "arch/x86_64/io.hpp"
#include "arch/x86_64/lapic.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "arch/x86_64/mtrr.hpp"
#include "arch/x86_64/pat.hpp"
#include "arch/x86_64/percpu.hpp"
#include "arch/x86_64/smp.hpp"
#include "arch/x86_64/syscall.hpp"
#include "arch/x86_64/tss.hpp"
#include "config.hpp"
#include "descriptor.hpp"
#include "capabilities.hpp"
#include "cmdline.hpp"
#include "debug_heartbeat.hpp"
#include "users.hpp"
#include "error.hpp"
#include "loader.hpp"
#include "memory/physical_allocator.hpp"
#include "module.hpp"
#include "path_util.hpp"
#include "process.hpp"
#include "random.hpp"
#include "scheduler.hpp"
#include "settings.hpp"
#include "string_util.hpp"
#include "time.hpp"

static void hcf(void) {
    for (;;) asm("hlt");
}

constexpr size_t kMaxMountSpecs = 16;
constexpr size_t kMaxSpecLen = 32;

constexpr size_t BOOTSTRAP_STACK_SIZE = 0x8000;
alignas(16) static uint8_t bootstrap_stack[BOOTSTRAP_STACK_SIZE];

static void kernel_main_stage2();

static void collect_boot_specs(char (&root_spec)[kMaxSpecLen],
                               char (&mount_buffers)[kMaxMountSpecs][kMaxSpecLen],
                               const char* (&mount_specs)[kMaxMountSpecs],
                               size_t& mount_spec_count) {
    const size_t requested_root_count = kernel_cmdline::value_count("ROOT");
    for (size_t requested = 0; requested < requested_root_count; ++requested) {
        const char* value = kernel_cmdline::value_at("ROOT", requested);
        if (value != nullptr && value[0] != '\0') {
            string_util::copy(root_spec, kMaxSpecLen, value);
        }
    }

    const size_t requested_mount_count = kernel_cmdline::value_count("MOUNT");
    for (size_t requested = 0; requested < requested_mount_count; ++requested) {
        const char* value = kernel_cmdline::value_at("MOUNT", requested);
        if (value == nullptr || value[0] == '\0') {
            continue;
        }
        bool duplicate = (root_spec[0] != '\0' &&
                          string_util::equals(root_spec, value));
        for (size_t i = 0; i < mount_spec_count && !duplicate; ++i) {
            if (string_util::equals(mount_buffers[i], value)) {
                duplicate = true;
            }
        }
        if (duplicate) {
            continue;
        }
        if (mount_spec_count < kMaxMountSpecs) {
            string_util::copy(mount_buffers[mount_spec_count],
                              kMaxSpecLen,
                              value);
            mount_specs[mount_spec_count] = mount_buffers[mount_spec_count];
            ++mount_spec_count;
        } else {
            log_message(LogLevel::Warn,
                        "Boot: ignoring extra MOUNT=%s (limit %zu)",
                        value,
                        static_cast<size_t>(kMaxMountSpecs));
        }
    }
}

static bool append_decimal(char* out, size_t out_size, size_t& index, size_t value) {
    char digits[20];
    size_t digit_count = 0;
    do {
        digits[digit_count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && digit_count < sizeof(digits));

    if (index + digit_count >= out_size) {
        return false;
    }
    for (size_t i = 0; i < digit_count; ++i) {
        out[index++] = digits[digit_count - 1 - i];
    }
    return true;
}

static bool ends_with(const char* text, const char* suffix) {
    if (text == nullptr || suffix == nullptr) {
        return false;
    }
    size_t text_len = string_util::length(text);
    size_t suffix_len = string_util::length(suffix);
    if (suffix_len > text_len) {
        return false;
    }
    return string_util::equals(text + text_len - suffix_len, suffix);
}

static bool default_root_from_rootfs_module(char (&root_spec)[kMaxSpecLen]) {
    size_t module_count = preserved_limine_module_count();
    if (module_count == 0) {
        log_message(LogLevel::Warn,
                    "boot: no Limine modules available for live root lookup");
        return false;
    }
    size_t fallback_index = module_count;
    for (size_t i = 0; i < module_count; ++i) {
        const PreservedLimineModule* module = preserved_limine_module(i);
        if (module == nullptr) {
            continue;
        }
        if (fallback_index == module_count &&
            module->address != nullptr && module->size != 0) {
            fallback_index = i;
        }
        const char* module_string = module->string;
        const char* module_path = module->path;
        log_message(LogLevel::Info,
                    "boot: module %zu path=%s string=%s size=%llu",
                    i,
                    module_path != nullptr ? module_path : "(null)",
                    module_string != nullptr ? module_string : "(null)",
                    static_cast<unsigned long long>(module->size));
        bool is_rootfs_module =
            (module_string != nullptr &&
             string_util::equals(module_string, "rootfs")) ||
            ends_with(module_path, "rootfs.img");
        if (!is_rootfs_module) {
            continue;
        }

        const char prefix[] = "MEMDISK_";
        size_t index = 0;
        for (size_t j = 0; prefix[j] != '\0'; ++j) {
            if (index + 1 >= kMaxSpecLen) {
                return false;
            }
            root_spec[index++] = prefix[j];
        }
        if (!append_decimal(root_spec, kMaxSpecLen, index, static_cast<size_t>(i))) {
            return false;
        }
        if (index + 3 >= kMaxSpecLen) {
            return false;
        }
        root_spec[index++] = '_';
        root_spec[index++] = '0';
        root_spec[index] = '\0';
        return true;
    }
    if (fallback_index < module_count) {
        log_message(LogLevel::Warn,
                    "boot: rootfs module not labelled, falling back to module %zu",
                    fallback_index);
        const char prefix[] = "MEMDISK_";
        size_t index = 0;
        for (size_t j = 0; prefix[j] != '\0'; ++j) {
            if (index + 1 >= kMaxSpecLen) {
                return false;
            }
            root_spec[index++] = prefix[j];
        }
        if (!append_decimal(root_spec,
                            kMaxSpecLen,
                            index,
                            static_cast<size_t>(fallback_index))) {
            return false;
        }
        if (index + 3 >= kMaxSpecLen) {
            return false;
        }
        root_spec[index++] = '_';
        root_spec[index++] = '0';
        root_spec[index] = '\0';
        return true;
    }
    return false;
}

static bool module_config_key(const char* key) {
    if (key == nullptr) {
        return false;
    }
    const char prefix[] = "KERNEL.MODULE";
    if (!string_util::starts_with(key, prefix)) {
        return false;
    }
    const char* suffix = key + sizeof(prefix) - 1;
    if (*suffix == '\0') {
        return true;
    }
    while (*suffix != '\0') {
        if (*suffix < '0' || *suffix > '9') {
            return false;
        }
        ++suffix;
    }
    return true;
}

static void load_configured_kernel_modules(const config::Table& kernel_config,
                                           const char* boot_cwd) {
    for (size_t i = 0; i < kernel_config.count; ++i) {
        const config::Entry& entry = kernel_config.entries[i];
        if (!module_config_key(entry.key) || entry.value[0] == '\0') {
            continue;
        }

        char path[path_util::kMaxPathLength];
        if (!path_util::build_absolute_path(boot_cwd, entry.value, path)) {
            log_message(LogLevel::Warn,
                        "Module: invalid path in %s: %s",
                        entry.key,
                        entry.value);
            continue;
        }
        if (!kernel_module::load_from_file(path)) {
            log_message(LogLevel::Warn,
                        "Module: failed to load %s",
                        path);
        }
    }
}

static bool is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static bool build_module_load_path(const char* boot_cwd,
                                   const char* entry,
                                   char (&out)[path_util::kMaxPathLength]) {
    if (entry == nullptr || entry[0] == '\0') {
        return false;
    }
    if (entry[0] == '/' || string_util::starts_with(entry, ".../")) {
        return path_util::build_absolute_path(boot_cwd, entry, out);
    }

    char relative[path_util::kMaxPathLength];
    const char prefix[] = ".../modules/";
    size_t used = sizeof(prefix) - 1;
    if (used >= sizeof(relative)) {
        return false;
    }
    for (size_t i = 0; i < used; ++i) {
        relative[i] = prefix[i];
    }
    for (size_t i = 0; entry[i] != '\0'; ++i) {
        if (used + 1 >= sizeof(relative)) {
            return false;
        }
        relative[used++] = entry[i];
    }
    relative[used] = '\0';
    return path_util::build_absolute_path(boot_cwd, relative, out);
}

static void load_module_list_file(const char* boot_cwd) {
    char list_path[path_util::kMaxPathLength];
    if (!path_util::build_absolute_path(boot_cwd,
                                        ".../modules/loads.txt",
                                        list_path)) {
        return;
    }

    uint8_t buffer[2048];
    size_t file_size = 0;
    if (!vfs::read_file(list_path, buffer, sizeof(buffer), file_size)) {
        log_message(LogLevel::Debug, "Module: %s not present", list_path);
        return;
    }

    log_message(LogLevel::Info, "Module: loading list %s", list_path);
    size_t offset = 0;
    while (offset < file_size) {
        char line[path_util::kMaxPathLength];
        size_t len = 0;
        while (offset < file_size &&
               buffer[offset] != '\n' &&
               len + 1 < sizeof(line)) {
            line[len++] = static_cast<char>(buffer[offset++]);
        }
        while (offset < file_size && buffer[offset] != '\n') {
            ++offset;
        }
        if (offset < file_size && buffer[offset] == '\n') {
            ++offset;
        }
        line[len] = '\0';

        size_t start = 0;
        while (is_space(line[start])) {
            ++start;
        }
        size_t end = start;
        while (line[end] != '\0' && line[end] != '#') {
            ++end;
        }
        while (end > start && is_space(line[end - 1])) {
            --end;
        }
        if (end == start) {
            continue;
        }
        line[end] = '\0';

        char path[path_util::kMaxPathLength];
        if (!build_module_load_path(boot_cwd, line + start, path)) {
            log_message(LogLevel::Warn,
                        "Module: invalid loads.txt entry '%s'",
                        line + start);
            continue;
        }
        if (!kernel_module::load_from_file(path)) {
            log_message(LogLevel::Warn,
                        "Module: failed to load %s from loads.txt",
                        path);
        }
    }
}

extern "C" void kernel_main(void) {
    uint8_t* stack_top = bootstrap_stack + BOOTSTRAP_STACK_SIZE;
    asm volatile(
        "mov %0, %%rsp\n"
        "xor %%rbp, %%rbp\n"
        :
        : "r"(stack_top)
        : "memory");
    kernel_main_stage2();
}

static void kernel_main_stage2() {
    if (framebuffer_request.response == nullptr ||
        framebuffer_request.response->framebuffer_count == 0) {
        // limine didn't give us a framebuffer
        hcf(); // normally we'd want to use error_screen::display but that depends on fbcon
        // so we can't actually use it without a framebuffer
    }

    // Limine owns the response storage. Preserve the command line before the
    // physical allocator and PCI drivers can reuse bootloader-reclaimable
    // memory; losing this pointer later made ROOT= appear intermittently
    // absent on real machines.
    const char* early_cmdline = nullptr;
    if (cmdline_request.response != nullptr &&
        cmdline_request.response->cmdline != nullptr) {
        early_cmdline = cmdline_request.response->cmdline;
    }
    if (early_cmdline == nullptr && kernel_file_request.response != nullptr) {
#if LIMINE_API_REVISION >= 3
        if (kernel_file_request.response->executable_file != nullptr) {
            early_cmdline =
                kernel_file_request.response->executable_file->string;
        }
#else
        if (kernel_file_request.response->kernel_file != nullptr) {
            early_cmdline = kernel_file_request.response->kernel_file->cmdline;
        }
#endif
    }
    kernel_cmdline::initialize(early_cmdline);
    preserve_limine_modules();
    intel_uhd::configure();

    auto fb = *framebuffer_request.response->framebuffers[0];
    uint8_t* fb_virtual = static_cast<uint8_t*>(fb.address);
    uint64_t fb_phys_addr = reinterpret_cast<uint64_t>(fb.address);
    if (hhdm_request.response != nullptr &&
        hhdm_request.response->offset != 0 &&
        fb_phys_addr >= hhdm_request.response->offset) {
        fb_phys_addr -= hhdm_request.response->offset;
    }
    uint64_t fb_length =
        static_cast<uint64_t>(fb.pitch) * static_cast<uint64_t>(fb.height);

    Framebuffer framebuffer = {fb_virtual,
                               static_cast<size_t>(fb.width),
                               static_cast<size_t>(fb.height),
                               static_cast<size_t>(fb.pitch),
                               static_cast<uint16_t>(fb.bpp),
                               fb.memory_model,
                               fb.red_mask_size,
                               fb.red_mask_shift,
                               fb.green_mask_size,
                               fb.green_mask_shift,
                               fb.blue_mask_size,
                               fb.blue_mask_shift};

    debug_heartbeat::init(framebuffer);

    descriptor::init();
    descriptor::register_builtin_types();
    capabilities::init();
    users::init();

    descriptor::register_framebuffer_device(framebuffer, fb_phys_addr);
    uint32_t framebuffer_handle =
        descriptor::open_kernel(descriptor::kTypeFramebuffer, 0, 0, 0);
    if (framebuffer_handle == descriptor::kInvalidHandle) {
        log_message(LogLevel::Warn,
                    "Console: failed to open framebuffer descriptor");
    }
    Console console = Console(framebuffer_handle);
    kconsole = &console;  // HAVE AT THEE

    log_init();

    log_message(LogLevel::Info, "Console online");
    if (kernel_cmdline::truncated()) {
        log_message(
            LogLevel::Warn,
            "boot: kernel command line exceeded 255 bytes and was truncated");
    }

    log_message(LogLevel::Info, "Welcome to Neutrino");

    const char* compiler_string =
#if defined(__clang__)
        "Clang/LLVM " __clang_version__;
#elif defined(__GNUC__) || defined(__GNUG__)
        "GCC " __VERSION__;
#elif defined(_MSC_VER)
        "MSVC" __MSC_FULL_VER_STR;
#else
        "Unknown compiler";
#endif

    log_message(LogLevel::Info, "Compiler: %s", compiler_string);

    if (!cpu::init_boot_features()) {
        hcf();
    }

    log_message(LogLevel::Info, "Installing IDT");
    idt_install();
    log_message(LogLevel::Info, "IDT installed");

    log_message(LogLevel::Info, "Initializing per-CPU state (BSP)");
    uint32_t bsp_lapic = lapic::id();
    percpu::Cpu* bsp_cpu = percpu::find_by_lapic(bsp_lapic);
    if (bsp_cpu == nullptr) {
        bsp_cpu = percpu::register_cpu(bsp_lapic, 0);
    }
    percpu::set_current_cpu(bsp_cpu);
    percpu::setup_cpu_tss(*bsp_cpu);
    percpu::setup_cpu_gdt(*bsp_cpu);
    scheduler::register_cpu(bsp_cpu);
    log_message(LogLevel::Info, "BSP per-CPU GDT/TSS installed (LAPIC=%u)",
                bsp_lapic);

    log_message(LogLevel::Info, "Initializing syscall interface");
    syscall::init();
    log_message(LogLevel::Info, "Syscall interface initialized");

    log_message(LogLevel::Info, "Initializing paging");
    paging_init();
    log_message(LogLevel::Info, "Paging initialized");

    lapic::init(hhdm_request.response->offset);
    log_message(LogLevel::Info, "Local APIC initialized");

    (void)acpi::initialize_tables();
    ioapic::init(hhdm_request.response->offset, bsp_lapic);
    (void)ioapic::route_isa_irq(1, 32 + 1);
    (void)ioapic::route_isa_irq(12, 32 + 12);

    log_message(LogLevel::Info, "Initializing PIC");
    pic::init();
    log_message(LogLevel::Info, "PIC initialized");

    log_message(LogLevel::Info, "Initializing keyboard");
    keyboard::init();
    log_message(LogLevel::Info, "Keyboard initialized");

    log_message(LogLevel::Info, "Initializing mouse");
    mouse::init();
    log_message(LogLevel::Info, "Mouse initialized");

    log_message(LogLevel::Info, "Configuring PIT");
    pit::init(1000);
    log_message(LogLevel::Info, "PIT configured");

    log_message(LogLevel::Info, "Probing ISA hardware-monitor adapters");
    it87::init();

    log_message(
        LogLevel::Debug, "Kernel phys base addr: %016x",
        (unsigned long long)kernel_addr_request.response->physical_base);
    log_message(LogLevel::Debug, "Kernel virt base addr: %016x",
                (unsigned long long)kernel_addr_request.response->virtual_base);
    log_message(
        LogLevel::Debug, "Kernel size: %u KB (%x)",
#if LIMINE_API_REVISION >= 2
        (unsigned int)(kernel_file_request.response->executable_file->size / 1024),
        (unsigned int)kernel_file_request.response->executable_file->size);
#else
        (unsigned int)(kernel_file_request.response->kernel_file->size / 1024),
        (unsigned int)kernel_file_request.response->kernel_file->size);
#endif

    log_message(LogLevel::Debug, "HHDM offset: %016x",
                (unsigned long long)hhdm_request.response->offset);

    // Limine's APs are still executing from bootloader-reclaimable low memory.
    // Release them before the physical allocator is allowed to reuse it.
    log_message(LogLevel::Info, "Starting SMP bring-up");
    smp::init();
    log_message(LogLevel::Info, "SMP bring-up complete");

    log_message(LogLevel::Info, "Initializing physical memory pools");
    memory::init();

    uint64_t entropy_probe = 0;
    bool secure_random_available =
        kernel_random::secure_fill(&entropy_probe, sizeof(entropy_probe));
    entropy_probe = 0;
    log_message(secure_random_available ? LogLevel::Info : LogLevel::Warn,
                secure_random_available
                    ? "Random: hardware entropy source available"
                    : "Random: no secure hardware entropy source");

    // Keep timer interrupts gated out of timekeeping until the complete RTC
    // snapshot has been committed by init_from_rtc().
    log_message(LogLevel::Info, "Initializing wall clock");
    if (!timekeeping::init_from_rtc(1000)) {
        log_message(LogLevel::Warn, "Wall clock unavailable");
    }

    if (kconsole != nullptr) {
        log_message(LogLevel::Info, "Enabling console back buffer");
        kconsole->enable_back_buffer();
        log_message(LogLevel::Info, "Console back buffer enabled");
    }
    bool pat_ok = cpu::configure_pat_write_combining();
    bool wc_pages = false;
    if (pat_ok && fb_virtual != nullptr) {
        wc_pages =
            paging_mark_wc(reinterpret_cast<uint64_t>(fb_virtual), fb_length);
    }
    bool mtrr_ok = cpu::configure_write_combining(fb_phys_addr, fb_length);

    if (!pat_ok) {
        log_message(LogLevel::Warn,
                    "PAT: failed to configure write-combining entry");
    } else if (!wc_pages) {
        log_message(
            LogLevel::Warn,
            "PAT: failed to mark framebuffer pages WC (virt=%016llx len=%llu) (ancient CPU?)",
            reinterpret_cast<unsigned long long>(fb_virtual),
            static_cast<unsigned long long>(fb_length));
    }

    if (!mtrr_ok) {
        log_message(
            LogLevel::Warn,
            "Framebuffer WC configuration failed (phys=%016llx len=%llu) (ancient CPU?)",
            static_cast<unsigned long long>(fb_phys_addr),
            static_cast<unsigned long long>(fb_length));
    }

    net::init();

    log_message(LogLevel::Info, "Initializing PCI subsystem");
    pci::init();
    log_message(LogLevel::Info, "PCI subsystem initialized");

    log_message(LogLevel::Info, "Module: discovered %zu built-in modules",
                kernel_module::count());
    (void)kernel_module::initialize_phase(kernel_module::Phase::Bus);
    (void)kernel_module::initialize_phase(kernel_module::Phase::Driver);

    vfs::init();

    char root_spec[kMaxSpecLen] = {0};
    char mount_buffers[kMaxMountSpecs][kMaxSpecLen] = {{0}};
    const char* mount_specs[kMaxMountSpecs];
    for (size_t i = 0; i < kMaxMountSpecs; ++i) {
        mount_specs[i] = nullptr;
    }
    size_t mount_spec_count = 0;

    collect_boot_specs(root_spec,
                     mount_buffers,
                     mount_specs,
                     mount_spec_count);
    if (root_spec[0] == '\0' && default_root_from_rootfs_module(root_spec)) {
        log_message(LogLevel::Warn,
                    "boot: ROOT= missing, defaulting to %s from rootfs module",
                    root_spec);
    }

    if (root_spec[0] == '\0') {
        log_message(LogLevel::Warn,
                    "boot: ROOT= not specified on kernel command line");
        error_screen::display("NO_ROOT_SPECIFIED", nullptr, nullptr);
    } else {
        log_message(LogLevel::Info, "boot: ROOT=%s", root_spec);
    }

    size_t mounted_count = 0;
    const char* root_ptr = (root_spec[0] != '\0') ? root_spec : nullptr;
    bool root_ok = fs::mount_requested_filesystems(
        root_ptr, mount_specs, mount_spec_count, mounted_count);
    if (root_ptr != nullptr && !root_ok) {
        log_message(LogLevel::Warn,
                    "Boot: root filesystem '%s' was not mounted", root_ptr);
        error_screen::display("FAILED_ROOT_MOUNT_", root_ptr, nullptr);
    }

    char boot_mount_name[64] = {0};
    if (root_ptr != nullptr && root_ok) {
        string_util::copy(boot_mount_name, sizeof(boot_mount_name), root_ptr);
        vfs::set_root_mount(root_ptr);
        settings::set_storage_root(root_ptr);
        settings::load_from_disk();
        if (kconsole != nullptr) {
            settings::apply_console_preferences(*kconsole);
        }
        // Configure user storage path under the root mount.
        char user_path[128];
        user_path[0] = '\0';
        string_util::copy(user_path, sizeof(user_path), root_ptr);
        size_t len = string_util::length(user_path);
        if (len + 1 < sizeof(user_path)) {
            user_path[len++] = '/';
            const char suffix[] = "system/users.ntd";
            string_util::copy(user_path + len,
                              sizeof(user_path) - len,
                              suffix);
            users::set_storage_path(user_path);
            users::load_from_disk();
        }

        net::load_config(root_ptr);
    }
    char boot_cwd[128];
    boot_cwd[0] = '/';
    boot_cwd[1] = '\0';

    config::Table kernel_config{};
    bool kernel_config_loaded = false;
    char init_task_path[path_util::kMaxPathLength] = {0};
    bool init_task_path_valid = false;

    constexpr size_t kMountQueryLimit = 16;
    size_t total_mounts = vfs::enumerate_mounts(nullptr, 0);
    size_t fetch_mounts =
        (total_mounts < kMountQueryLimit) ? total_mounts : kMountQueryLimit;

    const char* mount_names[kMountQueryLimit];
    if (fetch_mounts > 0) {
        vfs::enumerate_mounts(mount_names, fetch_mounts);
    }

    log_message(LogLevel::Info, "VFS: mounted filesystems: %u",
                static_cast<unsigned int>(mounted_count));
    log_message(LogLevel::Info, "VFS: available mounts: %u",
                static_cast<unsigned int>(total_mounts));
    for (size_t i = 0; i < fetch_mounts; ++i) {
        log_message(LogLevel::Info, "  %s/", mount_names[i]);
    }
    if (fetch_mounts < total_mounts) {
        log_message(LogLevel::Info,
                    "VFS: additional mounts not listed due to buffer size");
    }

    if (boot_mount_name[0] == '\0') {
        for (size_t i = 0; i < fetch_mounts; ++i) {
            if (mount_names[i] != nullptr) {
                string_util::copy(boot_mount_name, sizeof(boot_mount_name),
                                  mount_names[i]);
                break;
            }
        }
    }

    if (boot_mount_name[0] != '\0') {
        size_t idx = 1;
        size_t mount_len = string_util::length(boot_mount_name);
        if (mount_len + 1 >= sizeof(boot_cwd)) {
            mount_len = sizeof(boot_cwd) - 2;
        }
        for (size_t i = 0; i < mount_len; ++i) {
            boot_cwd[idx++] = boot_mount_name[i];
        }
        boot_cwd[idx] = '\0';
    } else {
        boot_cwd[1] = '\0';
    }

    if (root_ptr != nullptr && root_ok) {
        vfs::DirEntry entries[32];
        size_t entry_count = 0;
        if (vfs::list(root_ptr, entries, 32, entry_count)) {
            log_message(LogLevel::Info, "VFS: %s contains %u entries", root_ptr,
                        static_cast<unsigned int>(entry_count));
        }

        char path[path_util::kMaxPathLength];
        if (path_util::build_absolute_path(boot_cwd, ".../KERNEL.CFG", path)) {
                uint8_t file_buffer[1024];
                size_t file_size = 0;
                if (vfs::read_file(path, file_buffer, sizeof(file_buffer),
                                   file_size)) {
                    log_message(LogLevel::Info, "VFS: read %s (%u bytes)", path,
                                (unsigned int)file_size);
                    bool parse_ok = config::parse(
                        reinterpret_cast<const char*>(file_buffer), file_size,
                        kernel_config);
                    kernel_config_loaded = true;
                    if (!parse_ok) {
                        log_message(LogLevel::Warn,
                                    "Boot: KERNEL.CFG parse reported errors");
                    }
                    const char* init_spec = nullptr;
                    if (config::get(kernel_config, "KERNEL.INIT_TASK",
                                    init_spec)) {
                        if (path_util::build_absolute_path(boot_cwd,
                                                           init_spec,
                                                           init_task_path)) {
                            init_task_path_valid = true;
                            log_message(LogLevel::Info,
                                        "Boot: init task set to %s",
                                        init_task_path);
                        } else {
                            log_message(
                                LogLevel::Warn,
                                "Boot: invalid KERNEL.INIT_TASK value '%s'",
                                init_spec);
                        }
                    }
                } else {
                    log_message(LogLevel::Debug,
                                "VFS: %s not present or read failed", path);
                }
        }
    } else {
        log_message(LogLevel::Warn,
                    "Boot: skipping KERNEL.CFG lookup (root not mounted)");
    }

    if (root_ptr != nullptr && root_ok && !kernel_config_loaded) {
        log_message(LogLevel::Warn, "Boot: KERNEL.CFG not found on '%s'",
                    root_ptr);
    }

    if (kernel_config_loaded) {
        load_configured_kernel_modules(kernel_config, boot_cwd);
    }
    if (root_ptr != nullptr && root_ok) {
        load_module_list_file(boot_cwd);
    }

    process::init();
    scheduler::init();
    descriptor::start_waiter_worker();
    // Namespace initialization may install SCI handlers and queue deferred AML
    // work, so it must follow process and scheduler initialization.
    if (!acpi::initialize()) {
        log_message(LogLevel::Warn, "ACPI runtime unavailable");
    } else {
        acpi_thermal::init();
    }

    log_message(LogLevel::Info, "DriverRegistry: probing PCI drivers");
    driver_registry::probe_pci_drivers();
    log_message(LogLevel::Info, "DriverRegistry: PCI probe complete");

    constexpr size_t kInitMaxSize = 64 * 1024;
    alignas(16) static uint8_t init_buffer[kInitMaxSize];
    size_t init_size = 0;

    constexpr size_t kMaxInitCandidates = 4;
    const char* init_candidates[kMaxInitCandidates];
    size_t init_candidate_count = 0;

    if (init_task_path_valid) {
        init_candidates[init_candidate_count++] = init_task_path;
    }

    char default_init_paths[2][path_util::kMaxPathLength] = {{0}};
    size_t default_paths_used = 0;

    if (root_ptr != nullptr) {
        constexpr const char* kDefaultInitFiles[] = {
            ".../init.elf",
            ".../init.bin",
        };

        for (const char* fallback : kDefaultInitFiles) {
            if (default_paths_used >= 2) {
                break;
            }

            auto& buffer = default_init_paths[default_paths_used];
            if (!path_util::build_absolute_path(boot_cwd, fallback, buffer)) {
                log_message(LogLevel::Warn,
                            "Boot: invalid init fallback path '%s'",
                            fallback);
                continue;
            }

            bool duplicate = init_task_path_valid &&
                             string_util::equals(init_task_path, buffer);
            if (!duplicate && init_candidate_count < kMaxInitCandidates) {
                init_candidates[init_candidate_count++] = buffer;
            }
            ++default_paths_used;
        }
    }

    bool init_loaded = false;
    const char* init_path_used = nullptr;

    for (size_t i = 0; i < init_candidate_count; ++i) {
        const char* candidate = init_candidates[i];
        if (candidate == nullptr) {
            continue;
        }
        if (vfs::read_file(candidate, init_buffer, sizeof(init_buffer),
                           init_size)) {
            init_loaded = true;
            init_path_used = candidate;
            break;
        }
        log_message(LogLevel::Warn, "Boot: init task not found at %s",
                    candidate);
    }

    if (init_loaded) {
        loader::ProgramImage image{init_buffer, init_size, 0};
        process::Task* proc = process::allocate_init_task();
        bool init_started = false;
        if (proc != nullptr) {
            string_util::copy(proc->resources->cwd, sizeof(proc->resources->cwd), boot_cwd);
            if (init_path_used != nullptr) {
                string_util::copy(proc->image_path,
                                  sizeof(proc->image_path),
                                  init_path_used);
            }
            if (loader::load_into_process(image, *proc)) {
                // Keep the bootstrap process on the BSP. AP run queues are
                // not yet reliable during early userspace startup; placing
                // init there can leave the kernel alive (including CPU-0
                // poll workers) while login never executes. Children inherit
                // this affinity through the process syscall path.
                proc->preferred_cpu = 0;
                log_message(LogLevel::Info,
                            "Boot: launched init task from %s (%x bytes) on CPU 0",
                            init_path_used,
                            init_size);
                scheduler::enqueue(proc);
                init_started = true;
            } else {
                process::reclaim(*proc);
            }
        }
        if (!init_started) {
            log_message(
                LogLevel::Error, "Boot: failed to start init task (%s)",
                init_path_used != nullptr ? init_path_used : "(unknown)");
            error_screen::display("FAILED_INIT_START", nullptr, nullptr);
        }
    } else if (init_candidate_count > 0) {
        log_message(LogLevel::Error, "Boot: no init task could be loaded");
        error_screen::display("FAILED_INIT_LOAD", nullptr, nullptr);
    } else {
        log_message(LogLevel::Warn,
                    "Boot: init task not attempted (no path configured)");
        error_screen::display("FAILED_INIT_PATH", nullptr, nullptr);
    }

    // Userspace now owns the interactive console. Deferred driver work (most
    // notably xHCI port enumeration) must not write over login or shell
    // prompts. Logging continues to serial and the kernel ring buffer, where
    // it remains available through dmesg.
    log_set_console_enabled(false);
    smp::release_aps();
    scheduler::run();

    error_screen::display("SCHEDULER_EXITED", nullptr, nullptr);
}
