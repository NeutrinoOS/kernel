#include "acpi.hpp"

#include "arch/x86_64/io.hpp"
#include "arch/x86_64/memory/paging.hpp"
#include "drivers/limine/limine_requests.hpp"
#include "drivers/log/logging.hpp"
#include "drivers/pci/pci.hpp"
#include "kernel/cmdline.hpp"
#include "kernel/interrupts.hpp"
#include "kernel/memory/physical_allocator.hpp"
#include "kernel/sync.hpp"
#include "kernel/time.hpp"
#include "kernel/work.hpp"

extern "C" {
#include <uacpi/kernel_api.h>
#include <uacpi/uacpi.h>
}

void* operator new(size_t, void* storage) noexcept { return storage; }

namespace {

struct Mutex { sync::SpinLock lock; };
struct Event { volatile uint32_t count; };
struct PciHandle { uint8_t bus, slot, function; };
struct Interrupt { uacpi_interrupt_handler handler; uacpi_handle context; uint8_t irq; };

Interrupt* g_interrupts[16]{};
bool g_initialized = false;
bool g_tables_initialized = false;
bool g_entered_acpi_mode = false;
uint64_t g_rsdp_phys = 0;
alignas(void*) unsigned char g_early_table_buffer[4096];

template <unsigned Irq> void interrupt_thunk() {
    Interrupt* entry = g_interrupts[Irq];
    if (entry != nullptr) (void)entry->handler(entry->context);
}
using Thunk = void (*)();
constexpr Thunk kInterruptThunks[] = {
    interrupt_thunk<0>, interrupt_thunk<1>, interrupt_thunk<2>, interrupt_thunk<3>,
    interrupt_thunk<4>, interrupt_thunk<5>, interrupt_thunk<6>, interrupt_thunk<7>,
    interrupt_thunk<8>, interrupt_thunk<9>, interrupt_thunk<10>, interrupt_thunk<11>,
    interrupt_thunk<12>, interrupt_thunk<13>, interrupt_thunk<14>, interrupt_thunk<15>,
};

bool timed_out(uint64_t start, uint16_t timeout_ms) {
    if (timeout_ms == 0xFFFF) return false;
    return timekeeping::nanoseconds_since_boot() - start >=
           static_cast<uint64_t>(timeout_ms) * 1000000ull;
}

}  // namespace

extern "C" uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out_rsdp) {
    if (out_rsdp == nullptr) {
        return UACPI_STATUS_NOT_FOUND;
    }
    if (g_rsdp_phys != 0) {
        *out_rsdp = g_rsdp_phys;
        return UACPI_STATUS_OK;
    }
    if (rsdp_request.response == nullptr ||
        rsdp_request.response->address == 0) {
        return UACPI_STATUS_NOT_FOUND;
    }
    uint64_t address =
        reinterpret_cast<uint64_t>(rsdp_request.response->address);
    uint64_t hhdm = paging_hhdm_offset();
    if (hhdm != 0 && address >= hhdm) address -= hhdm;
    // Limine response structures live in bootloader-reclaimable memory. Keep
    // the physical RSDP address before the page allocator (and now the dynamic
    // disk cache) is allowed to reuse that memory.
    g_rsdp_phys = address;
    *out_rsdp = g_rsdp_phys;
    return UACPI_STATUS_OK;
}

extern "C" void* uacpi_kernel_map(uacpi_phys_addr address, uacpi_size length) {
    constexpr uint64_t kPageSize = 0x1000;
    if (length == 0 || address > UINT64_MAX - length) return UACPI_MAP_FAILED;

    const uint64_t first_phys = address & ~(kPageSize - 1);
    const uint64_t last_phys = (address + length - 1) & ~(kPageSize - 1);
    const uint64_t hhdm = paging_hhdm_offset();
    for (uint64_t phys = first_phys;; phys += kPageSize) {
        const uint64_t virt = hhdm + phys;
        uint64_t resolved_phys = 0;
        // Normal RAM (including ACPI tables) is already present through the
        // HHDM. Device-backed OperationRegions are not, so add a UC mapping.
        if (!paging_resolve_cr3(paging_kernel_cr3(), virt, resolved_phys)) {
            const uint64_t flags = PAGE_FLAG_WRITE | PAGE_FLAG_CACHE_DISABLE |
                                   PAGE_FLAG_WRITE_THROUGH | PAGE_FLAG_NO_EXECUTE;
            if (!paging_map_page(virt, phys, flags)) return UACPI_MAP_FAILED;
        }
        if (phys == last_phys) break;
    }
    return reinterpret_cast<void*>(hhdm + address);
}
extern "C" void uacpi_kernel_unmap(void*, uacpi_size) {}
extern "C" void uacpi_kernel_log(uacpi_log_level level, const uacpi_char* message) {
    LogLevel mapped = level == UACPI_LOG_ERROR ? LogLevel::Error :
                      level == UACPI_LOG_WARN ? LogLevel::Warn :
                      level == UACPI_LOG_INFO ? LogLevel::Info : LogLevel::Debug;

    if (message == nullptr) {
        log_message(mapped, "uACPI: <null>");
        return;
    }

    size_t length = 0;
    while (message[length] != '\0') ++length;
    while (length > 0 && (message[length - 1] == '\n' || message[length - 1] == '\r')) {
        --length;
    }

    char trimmed[256];
    const size_t copy_length = length < sizeof(trimmed) - 1 ? length : sizeof(trimmed) - 1;
    for (size_t i = 0; i < copy_length; ++i) trimmed[i] = message[i];
    trimmed[copy_length] = '\0';
    log_message(mapped, "uACPI: %s", trimmed);
}

extern "C" void* uacpi_kernel_alloc(uacpi_size size) { return memory::alloc_kernel(size); }
extern "C" void uacpi_kernel_free(void* ptr) { memory::free_kernel(ptr); }
extern "C" uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot() {
    return timekeeping::nanoseconds_since_boot();
}
extern "C" void uacpi_kernel_stall(uacpi_u8 usec) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(usec) * 200u; ++i) asm volatile("pause");
}
extern "C" void uacpi_kernel_sleep(uacpi_u64 msec) {
    uint64_t start = timekeeping::nanoseconds_since_boot();
    uint64_t duration = msec * 1000000ull;
    while (timekeeping::nanoseconds_since_boot() - start < duration) asm volatile("pause");
}

extern "C" uacpi_handle uacpi_kernel_create_mutex() {
    void* storage = memory::alloc_kernel(sizeof(Mutex), alignof(Mutex));
    return storage == nullptr ? nullptr : new (storage) Mutex{};
}
extern "C" void uacpi_kernel_free_mutex(uacpi_handle mutex) { memory::free_kernel(mutex); }
extern "C" uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout) {
    if (handle == nullptr) return UACPI_STATUS_INVALID_ARGUMENT;
    Mutex* mutex = static_cast<Mutex*>(handle);
    uint64_t start = timekeeping::nanoseconds_since_boot();
    do {
        if (mutex->lock.try_lock()) return UACPI_STATUS_OK;
        if (timeout == 0 || timed_out(start, timeout)) return UACPI_STATUS_TIMEOUT;
        asm volatile("pause");
    } while (true);
}
extern "C" void uacpi_kernel_release_mutex(uacpi_handle handle) {
    if (handle != nullptr) static_cast<Mutex*>(handle)->lock.unlock();
}

extern "C" uacpi_handle uacpi_kernel_create_event() {
    void* storage = memory::alloc_kernel(sizeof(Event), alignof(Event));
    return storage == nullptr ? nullptr : new (storage) Event{};
}
extern "C" void uacpi_kernel_free_event(uacpi_handle event) { memory::free_kernel(event); }
extern "C" uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout) {
    if (handle == nullptr) return UACPI_FALSE;
    Event* event = static_cast<Event*>(handle);
    uint64_t start = timekeeping::nanoseconds_since_boot();
    for (;;) {
        uint32_t count = __atomic_load_n(&event->count, __ATOMIC_ACQUIRE);
        while (count != 0) {
            if (__atomic_compare_exchange_n(&event->count, &count, count - 1, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return UACPI_TRUE;
        }
        if (timed_out(start, timeout)) return UACPI_FALSE;
        asm volatile("pause");
    }
}
extern "C" void uacpi_kernel_signal_event(uacpi_handle handle) {
    if (handle != nullptr) __atomic_fetch_add(&static_cast<Event*>(handle)->count, 1u, __ATOMIC_RELEASE);
}
extern "C" void uacpi_kernel_reset_event(uacpi_handle handle) {
    if (handle != nullptr) __atomic_store_n(&static_cast<Event*>(handle)->count, 0u, __ATOMIC_RELEASE);
}

extern "C" uacpi_thread_id uacpi_kernel_get_thread_id() { return reinterpret_cast<void*>(1); }
extern "C" uacpi_interrupt_state uacpi_kernel_disable_interrupts() { return sync::disable_interrupts(); }
extern "C" void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) { sync::restore_interrupts(state); }
extern "C" uacpi_handle uacpi_kernel_create_spinlock() {
    void* storage = memory::alloc_kernel(sizeof(sync::SpinLock),
                                         alignof(sync::SpinLock));
    return storage == nullptr ? nullptr : new (storage) sync::SpinLock{};
}
extern "C" void uacpi_kernel_free_spinlock(uacpi_handle lock) { memory::free_kernel(lock); }
extern "C" uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle) {
    if (handle == nullptr) return 0;
    uint64_t flags = sync::disable_interrupts();
    static_cast<sync::SpinLock*>(handle)->lock();
    return flags;
}
extern "C" void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags flags) {
    if (handle != nullptr) static_cast<sync::SpinLock*>(handle)->unlock();
    sync::restore_interrupts(flags);
}

extern "C" uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle* out) {
    if (out == nullptr || address.device > 31 || address.function > 7) return UACPI_STATUS_INVALID_ARGUMENT;
    auto* handle = static_cast<PciHandle*>(memory::alloc_kernel(sizeof(PciHandle)));
    if (handle == nullptr) return UACPI_STATUS_OUT_OF_MEMORY;
    *handle = PciHandle{static_cast<uint8_t>(address.bus), static_cast<uint8_t>(address.device), static_cast<uint8_t>(address.function)};
    *out = handle;
    return UACPI_STATUS_OK;
}
extern "C" void uacpi_kernel_pci_device_close(uacpi_handle handle) { memory::free_kernel(handle); }
#define PCI_ACCESS(width, type) \
extern "C" uacpi_status uacpi_kernel_pci_read##width(uacpi_handle h, uacpi_size o, type* v) { if (!h || !v || o >= 256) return UACPI_STATUS_INVALID_ARGUMENT; auto* p=static_cast<PciHandle*>(h); *v=pci::read_config##width(p->bus,p->slot,p->function,static_cast<uint8_t>(o)); return UACPI_STATUS_OK; } \
extern "C" uacpi_status uacpi_kernel_pci_write##width(uacpi_handle h, uacpi_size o, type v) { if (!h || o >= 256) return UACPI_STATUS_INVALID_ARGUMENT; auto* p=static_cast<PciHandle*>(h); pci::write_config##width(p->bus,p->slot,p->function,static_cast<uint8_t>(o),v); return UACPI_STATUS_OK; }
PCI_ACCESS(8, uacpi_u8)
PCI_ACCESS(16, uacpi_u16)
PCI_ACCESS(32, uacpi_u32)
#undef PCI_ACCESS

extern "C" uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size, uacpi_handle* out) { if (!out || base > 0xFFFF) return UACPI_STATUS_INVALID_ARGUMENT; *out=reinterpret_cast<void*>(static_cast<uintptr_t>(base)); return UACPI_STATUS_OK; }
extern "C" void uacpi_kernel_io_unmap(uacpi_handle) {}
static uint16_t io_port(uacpi_handle h, uacpi_size o) { return static_cast<uint16_t>(reinterpret_cast<uintptr_t>(h) + o); }
extern "C" uacpi_status uacpi_kernel_io_read8(uacpi_handle h,uacpi_size o,uacpi_u8* v) { if(!h||!v) return UACPI_STATUS_INVALID_ARGUMENT; *v=inb(io_port(h,o)); return UACPI_STATUS_OK; }
extern "C" uacpi_status uacpi_kernel_io_read16(uacpi_handle h,uacpi_size o,uacpi_u16* v) { if(!h||!v) return UACPI_STATUS_INVALID_ARGUMENT; *v=inw(io_port(h,o)); return UACPI_STATUS_OK; }
extern "C" uacpi_status uacpi_kernel_io_read32(uacpi_handle h,uacpi_size o,uacpi_u32* v) { if(!h||!v) return UACPI_STATUS_INVALID_ARGUMENT; *v=inl(io_port(h,o)); return UACPI_STATUS_OK; }
extern "C" uacpi_status uacpi_kernel_io_write8(uacpi_handle h,uacpi_size o,uacpi_u8 v) { if(!h) return UACPI_STATUS_INVALID_ARGUMENT; outb(io_port(h,o),v); return UACPI_STATUS_OK; }
extern "C" uacpi_status uacpi_kernel_io_write16(uacpi_handle h,uacpi_size o,uacpi_u16 v) { if(!h) return UACPI_STATUS_INVALID_ARGUMENT; outw(io_port(h,o),v); return UACPI_STATUS_OK; }
extern "C" uacpi_status uacpi_kernel_io_write32(uacpi_handle h,uacpi_size o,uacpi_u32 v) { if(!h) return UACPI_STATUS_INVALID_ARGUMENT; outl(io_port(h,o),v); return UACPI_STATUS_OK; }

extern "C" uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle context, uacpi_handle* out) {
    if (irq >= 16 || handler == nullptr || out == nullptr || g_interrupts[irq] != nullptr) return UACPI_STATUS_INVALID_ARGUMENT;
    auto* entry = static_cast<Interrupt*>(memory::alloc_kernel(sizeof(Interrupt)));
    if (!entry) return UACPI_STATUS_OUT_OF_MEMORY;
    *entry = Interrupt{handler, context, static_cast<uint8_t>(irq)};
    g_interrupts[irq] = entry;
    if (!interrupts::register_isa_irq(static_cast<uint8_t>(irq), kInterruptThunks[irq])) { g_interrupts[irq] = nullptr; memory::free_kernel(entry); return UACPI_STATUS_DENIED; }
    *out = entry;
    return UACPI_STATUS_OK;
}
extern "C" uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler, uacpi_handle handle) {
    if (!handle) return UACPI_STATUS_INVALID_ARGUMENT;
    auto* entry = static_cast<Interrupt*>(handle);
    interrupts::unregister_isa_irq(entry->irq);
    g_interrupts[entry->irq] = nullptr;
    memory::free_kernel(entry);
    return UACPI_STATUS_OK;
}
extern "C" uacpi_status uacpi_kernel_schedule_work(uacpi_work_type, uacpi_work_handler handler, uacpi_handle context) {
    return work::schedule(reinterpret_cast<work::Handler>(handler), context) ? UACPI_STATUS_OK : UACPI_STATUS_OUT_OF_MEMORY;
}
extern "C" uacpi_status uacpi_kernel_wait_for_work_completion() { work::wait(); return UACPI_STATUS_OK; }
extern "C" uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request*) { return UACPI_STATUS_UNIMPLEMENTED; }

namespace acpi {
bool initialize_tables() {
    if (g_tables_initialized) return true;
    uacpi_status status = uacpi_setup_early_table_access(g_early_table_buffer, sizeof(g_early_table_buffer));
    if (status != UACPI_STATUS_OK) {
        log_message(LogLevel::Warn, "uACPI table initialization failed: %s", uacpi_status_to_string(status));
        return false;
    }
    g_tables_initialized = true;
    return true;
}

bool initialize() {
    if (g_initialized) return true;
    if (kernel_cmdline::has_value("ACPI", "OFF")) {
        log_message(LogLevel::Warn,
                    "uACPI: runtime disabled by kernel command line");
        return true;
    }

    const bool no_mode = kernel_cmdline::has_flag("ACPI.NO_MODE");
    const bool no_namespace_load =
        kernel_cmdline::has_flag("ACPI.NO_NAMESPACE_LOAD");
    const bool no_namespace_init = no_namespace_load ||
        kernel_cmdline::has_flag("ACPI.NO_NAMESPACE_INIT");

    if (g_tables_initialized) {
        uacpi_state_reset();
        g_tables_initialized = false;
    }
    log_message(LogLevel::Info,
                "uACPI: runtime init (mode=%s namespace=%s)",
                no_mode ? "unchanged" : "ACPI",
                no_namespace_load ? "off" :
                    (no_namespace_init ? "load-only" : "initialized"));

    uacpi_u64 flags = no_mode ? UACPI_FLAG_NO_ACPI_MODE : 0;
    uacpi_status status = uacpi_initialize(flags);
    if (status == UACPI_STATUS_OK && !no_mode) {
        g_entered_acpi_mode = true;
    }
    if (status == UACPI_STATUS_OK && !no_namespace_load) {
        status = uacpi_namespace_load();
    }
    if (status == UACPI_STATUS_OK && !no_namespace_init) {
        status = uacpi_namespace_initialize();
    }
    if (status != UACPI_STATUS_OK) {
        log_message(LogLevel::Warn, "uACPI initialization failed: %s", uacpi_status_to_string(status));
        return false;
    }
    g_initialized = true;
    log_message(LogLevel::Info, "uACPI initialized");
    return true;
}

bool entered_acpi_mode() {
    return g_entered_acpi_mode;
}
}  // namespace acpi
