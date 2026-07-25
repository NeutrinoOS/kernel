#include "../descriptor.hpp"

#include "../../drivers/console/console.hpp"
#include "../process.hpp"
#include "../sync.hpp"
#include "../vm.hpp"
#include "../../lib/mem.hpp"

namespace descriptor {

namespace descriptor_vty {

constexpr size_t kMaxVtys = 8;
constexpr uint32_t kDefaultCols = 80;
constexpr uint32_t kDefaultRows = 25;
constexpr uint32_t kMaxCols = 120;
constexpr uint32_t kMaxRows = 50;
constexpr size_t kInputBufferSize = 256;

struct Vty {
    bool in_use;
    bool allocating;
    uint32_t id;
    uint32_t cols;
    uint32_t rows;
    uint32_t cursor_x;
    uint32_t cursor_y;
    uint32_t flags;
    uint32_t fg;
    uint32_t bg;
    uint8_t text_flags;
    descriptor_defs::VtyCell cells[kMaxCols * kMaxRows];
    uint8_t input[kInputBufferSize];
    size_t input_head;
    size_t input_tail;
    volatile int lock;
};

Vty g_vtys[kMaxVtys]{};
uint32_t g_next_vty_id = 1;
sync::SpinLock g_vty_pool_lock;

inline void lock_vty(Vty& vty) {
    while (__atomic_test_and_set(&vty.lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
}

inline void unlock_vty(Vty& vty) {
    __atomic_clear(&vty.lock, __ATOMIC_RELEASE);
}

size_t cell_index(const Vty& vty, uint32_t x, uint32_t y) {
    return static_cast<size_t>(y) * vty.cols + x;
}

void fill_cell(descriptor_defs::VtyCell& cell,
               uint8_t ch,
               uint32_t fg,
               uint32_t bg,
               uint8_t flags = 0) {
    cell.fg = fg;
    cell.bg = bg;
    cell.ch = ch;
    cell.flags = flags;
    cell.reserved[0] = 0;
    cell.reserved[1] = 0;
}

void clear_row(Vty& vty, uint32_t row) {
    if (row >= vty.rows) {
        return;
    }
    size_t base = static_cast<size_t>(row) * vty.cols;
    for (uint32_t col = 0; col < vty.cols; ++col) {
        fill_cell(vty.cells[base + col], ' ', vty.fg, vty.bg, vty.text_flags);
    }
}

void clear_all(Vty& vty) {
    for (uint32_t row = 0; row < vty.rows; ++row) {
        clear_row(vty, row);
    }
    vty.cursor_x = 0;
    vty.cursor_y = 0;
}

bool set_cursor(Vty& vty, uint32_t x, uint32_t y) {
    if (vty.cols == 0 || vty.rows == 0) {
        return false;
    }
    if (x >= vty.cols) {
        x = vty.cols - 1;
    }
    if (y >= vty.rows) {
        y = vty.rows - 1;
    }
    vty.cursor_x = x;
    vty.cursor_y = y;
    return true;
}

void scroll_up(Vty& vty) {
    if (vty.rows <= 1 || vty.cols == 0) {
        clear_all(vty);
        return;
    }
    size_t row_cells = vty.cols;
    size_t move_cells = static_cast<size_t>(vty.rows - 1) * row_cells;
    memmove_fast(vty.cells,
                 vty.cells + row_cells,
                 move_cells * sizeof(descriptor_defs::VtyCell));
    clear_row(vty, vty.rows - 1);
    vty.cursor_y = vty.rows - 1;
}

void advance_cursor(Vty& vty) {
    ++vty.cursor_x;
    if (vty.cursor_x >= vty.cols) {
        vty.cursor_x = 0;
        ++vty.cursor_y;
        if (vty.cursor_y >= vty.rows) {
            scroll_up(vty);
        }
    }
}

void put_char(Vty& vty, char ch) {
    if (vty.cols == 0 || vty.rows == 0) {
        return;
    }
    if (ch == '\n') {
        vty.cursor_x = 0;
        ++vty.cursor_y;
        if (vty.cursor_y >= vty.rows) {
            scroll_up(vty);
        }
        return;
    }
    if (ch == '\r') {
        vty.cursor_x = 0;
        return;
    }
    if (ch == '\b' || ch == 0x7F) {
        if (vty.cursor_x > 0) {
            --vty.cursor_x;
        } else if (vty.cursor_y > 0) {
            --vty.cursor_y;
            vty.cursor_x = vty.cols - 1;
        } else {
            return;
        }
        size_t idx = cell_index(vty, vty.cursor_x, vty.cursor_y);
        fill_cell(vty.cells[idx], ' ', vty.fg, vty.bg, vty.text_flags);
        return;
    }
    if (ch == '\t') {
        uint32_t spaces = 4 - (vty.cursor_x % 4u);
        for (uint32_t i = 0; i < spaces; ++i) {
            put_char(vty, ' ');
        }
        return;
    }
    if (static_cast<uint8_t>(ch) < 0x20) {
        return;
    }

    size_t idx = cell_index(vty, vty.cursor_x, vty.cursor_y);
    fill_cell(vty.cells[idx],
              static_cast<uint8_t>(ch),
              vty.fg,
              vty.bg,
              vty.text_flags);
    advance_cursor(vty);
}

bool enqueue_input(Vty& vty, uint8_t value) {
    size_t next = (vty.input_head + 1) % kInputBufferSize;
    if (next == vty.input_tail) {
        return false;
    }
    vty.input[vty.input_head] = value;
    vty.input_head = next;
    return true;
}

bool has_input(const Vty& vty) {
    return vty.input_head != vty.input_tail;
}

bool dequeue_input(Vty& vty, uint8_t& out) {
    if (vty.input_head == vty.input_tail) {
        return false;
    }
    out = vty.input[vty.input_tail];
    vty.input_tail = (vty.input_tail + 1) % kInputBufferSize;
    return true;
}

void copy_info(const Vty& vty, descriptor_defs::VtyInfo& info) {
    info.id = vty.id;
    info.cols = vty.cols;
    info.rows = vty.rows;
    info.cursor_x = vty.cursor_x;
    info.cursor_y = vty.cursor_y;
    info.flags = vty.flags;
    info.cell_bytes = sizeof(descriptor_defs::VtyCell);
}

Vty* find_vty_locked(uint32_t id) {
    if (id == 0) {
        return nullptr;
    }
    for (auto& vty : g_vtys) {
        if (vty.in_use && vty.id == id) {
            return &vty;
        }
    }
    return nullptr;
}

Vty* find_vty(uint32_t id) {
    sync::IrqLockGuard guard(g_vty_pool_lock);
    return find_vty_locked(id);
}

Vty* allocate_vty() {
    Vty* selected = nullptr;
    {
        sync::IrqLockGuard guard(g_vty_pool_lock);
        for (auto& vty : g_vtys) {
            if (vty.in_use || vty.allocating) {
                continue;
            }
            vty.allocating = true;
            vty.id = g_next_vty_id++;
            if (vty.id == 0) {
                vty.id = g_next_vty_id++;
            }
            selected = &vty;
            break;
        }
    }
    if (selected == nullptr) {
        return nullptr;
    }

    selected->cols = kDefaultCols;
    selected->rows = kDefaultRows;
    if (selected->cols > kMaxCols) {
        selected->cols = kMaxCols;
    }
    if (selected->rows > kMaxRows) {
        selected->rows = kMaxRows;
    }
    selected->cursor_x = 0;
    selected->cursor_y = 0;
    selected->flags = 0;
    selected->fg = 0xFFFFFFFF;   // white
    selected->bg = 0x00000000;   // black
    selected->text_flags = 0;
    selected->input_head = 0;
    selected->input_tail = 0;
    selected->lock = 0;
    clear_all(*selected);

    {
        sync::IrqLockGuard guard(g_vty_pool_lock);
        selected->in_use = true;
        selected->allocating = false;
    }
    return selected;
}

int64_t vty_read(process::Process& proc,
                 DescriptorEntry& entry,
                 uint64_t user_address,
                 uint64_t length,
                 uint64_t offset) {
    if (offset != 0) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    auto* vty = static_cast<Vty*>(entry.object);
    if (vty == nullptr || !vty->in_use) {
        return -1;
    }
    size_t remaining = static_cast<size_t>(length);
    size_t total = 0;
    uint8_t buffer[64];

    while (remaining > 0) {
        size_t chunk = (remaining > sizeof(buffer)) ? sizeof(buffer) : remaining;
        size_t count = 0;
        lock_vty(*vty);
        while (count < chunk) {
            uint8_t value = 0;
            if (!dequeue_input(*vty, value)) {
                break;
            }
            buffer[count++] = value;
        }
        unlock_vty(*vty);
        if (count == 0) {
            break;
        }
        if (!vm::copy_to_user(proc.cr3,
                              user_address + total,
                              buffer,
                              count)) {
            return (total > 0) ? static_cast<int64_t>(total) : -1;
        }
        total += count;
        remaining -= count;
    }
    return static_cast<int64_t>(total);
}

int64_t vty_write(process::Process& proc,
                  DescriptorEntry& entry,
                  uint64_t user_address,
                  uint64_t length,
                  uint64_t offset) {
    if (offset != 0) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    auto* vty = static_cast<Vty*>(entry.object);
    if (vty == nullptr || !vty->in_use) {
        return -1;
    }
    size_t remaining = static_cast<size_t>(length);
    size_t total = 0;
    uint8_t buffer[128];
    while (remaining > 0) {
        size_t chunk = (remaining > sizeof(buffer)) ? sizeof(buffer) : remaining;
        if (!vm::copy_from_user(proc.cr3,
                                buffer,
                                user_address + total,
                                chunk)) {
            return (total > 0) ? static_cast<int64_t>(total) : -1;
        }
        lock_vty(*vty);
        for (size_t i = 0; i < chunk; ++i) {
            put_char(*vty, static_cast<char>(buffer[i]));
        }
        unlock_vty(*vty);
        total += chunk;
        remaining -= chunk;
    }
    return static_cast<int64_t>(total);
}

int vty_get_property(DescriptorEntry& entry,
                     uint32_t property,
                     void* out,
                     size_t size) {
    auto* vty = static_cast<Vty*>(entry.object);
    if (vty == nullptr || !vty->in_use) {
        return -1;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::VtyInfo)) {
        if (out == nullptr || size < sizeof(descriptor_defs::VtyInfo)) {
            return -1;
        }
        auto* info = reinterpret_cast<descriptor_defs::VtyInfo*>(out);
        lock_vty(*vty);
        copy_info(*vty, *info);
        unlock_vty(*vty);
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::VtyCells)) {
        if (out == nullptr) {
            return -1;
        }
        size_t cells = static_cast<size_t>(vty->cols) * vty->rows;
        size_t required = cells * sizeof(descriptor_defs::VtyCell);
        if (size < required) {
            return -1;
        }
        lock_vty(*vty);
        memcpy(out, vty->cells, required);
        unlock_vty(*vty);
        return 0;
    }
    return -1;
}

int vty_set_property(DescriptorEntry& entry,
                     uint32_t property,
                     const void* in,
                     size_t size) {
    auto* vty = static_cast<Vty*>(entry.object);
    if (vty == nullptr || !vty->in_use) {
        return -1;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::VtyInjectInput)) {
        if (in == nullptr || size == 0) {
            return 0;
        }
        bool changed = false;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(in);
        lock_vty(*vty);
        for (size_t i = 0; i < size; ++i) {
            changed = enqueue_input(*vty, bytes[i]) || changed;
        }
        unlock_vty(*vty);
        if (changed) {
            descriptor::wake_waiters();
        }
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::VtyCursor)) {
        if (in == nullptr || size < sizeof(descriptor_defs::CursorPosition)) {
            return -1;
        }
        auto* pos =
            reinterpret_cast<const descriptor_defs::CursorPosition*>(in);
        lock_vty(*vty);
        bool ok = set_cursor(*vty, pos->x, pos->y);
        unlock_vty(*vty);
        return ok ? 0 : -1;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::VtyClear)) {
        lock_vty(*vty);
        clear_all(*vty);
        unlock_vty(*vty);
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::VtyColor)) {
        if (in == nullptr || size < sizeof(descriptor_defs::ColorPair)) {
            return -1;
        }
        auto* colors =
            reinterpret_cast<const descriptor_defs::ColorPair*>(in);
        lock_vty(*vty);
        vty->fg = colors->fg;
        vty->bg = colors->bg;
        unlock_vty(*vty);
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::VtyTextFlags)) {
        if (in == nullptr || size < sizeof(uint8_t)) {
            return -1;
        }
        lock_vty(*vty);
        vty->text_flags = *reinterpret_cast<const uint8_t*>(in);
        unlock_vty(*vty);
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::VtyCells)) {
        size_t cells = static_cast<size_t>(vty->cols) * vty->rows;
        size_t required = cells * sizeof(descriptor_defs::VtyCell);
        if (in == nullptr || size < required) {
            return -1;
        }
        auto* cells_in = reinterpret_cast<const descriptor_defs::VtyCell*>(in);
        lock_vty(*vty);
        memcpy_fast(vty->cells, cells_in, required);
        unlock_vty(*vty);
        return 0;
    }
    return -1;
}

const Ops kVtyOps{
    .read = vty_read,
    .write = vty_write,
    .get_property = vty_get_property,
    .set_property = vty_set_property,
};

bool query_wait(DescriptorEntry& entry, uint32_t events, uint32_t& revents) {
    revents = 0;
    auto* vty = static_cast<Vty*>(entry.object);
    if (vty == nullptr || !vty->in_use) {
        return false;
    }
    lock_vty(*vty);
    if ((events & descriptor_defs::kWaitRead) != 0 && has_input(*vty)) {
        revents |= descriptor_defs::kWaitRead;
    }
    if ((events & descriptor_defs::kWaitWrite) != 0) {
        revents |= descriptor_defs::kWaitWrite;
    }
    unlock_vty(*vty);
    return true;
}

bool open_vty(process::Process& proc,
              uint64_t resource_selector,
              uint64_t requested_flags,
              uint64_t open_context,
              Allocation& alloc) {
    Vty* vty = nullptr;
    if (resource_selector == 0) {
        vty = allocate_vty();
    } else {
        vty = find_vty(static_cast<uint32_t>(resource_selector));
        // Existing IDs locate a VTY but do not grant access to it. Only a
        // process that inherited or explicitly attached this VTY may reopen.
        if (vty != nullptr && proc.resources->vty_id != vty->id) {
            return false;
        }
    }
    if (vty == nullptr) {
        return false;
    }
    if ((open_context &
         static_cast<uint64_t>(descriptor_defs::VtyOpen::Attach)) != 0) {
        proc.resources->vty_id = vty->id;
    }
    uint64_t flags = static_cast<uint64_t>(Flag::Readable) |
                     static_cast<uint64_t>(Flag::Writable);
    if (requested_flags != 0) {
        flags = requested_flags;
    }
    alloc.type = kTypeVty;
    alloc.flags = flags;
    alloc.extended_flags = 0;
    alloc.has_extended_flags = false;
    alloc.object = vty;
    alloc.subsystem_data = nullptr;
    alloc.name = "vty";
    alloc.ops = &kVtyOps;
    alloc.ext = nullptr;
    alloc.close = nullptr;
    return true;
}

}  // namespace descriptor_vty

bool vty_write(uint32_t id, const char* data, size_t length) {
    if (data == nullptr || length == 0) {
        return true;
    }
    auto* vty = descriptor_vty::find_vty(id);
    if (vty == nullptr) {
        return false;
    }
    descriptor_vty::lock_vty(*vty);
    for (size_t i = 0; i < length; ++i) {
        descriptor_vty::put_char(*vty, data[i]);
    }
    descriptor_vty::unlock_vty(*vty);
    return true;
}

bool register_vty_descriptor() {
    return register_type(kTypeVty,
                         descriptor_vty::open_vty,
                         &descriptor_vty::kVtyOps);
}

bool vty_redraw_console(uint32_t id, Console& console) {
    using namespace descriptor_vty;
    Vty* vty = find_vty(id);
    if (vty == nullptr || !vty->in_use) {
        return false;
    }
    lock_vty(*vty);
    console.redraw_cells(vty->cells,
                         vty->cols,
                         vty->rows,
                         vty->cursor_x,
                         vty->cursor_y,
                         vty->fg,
                         vty->bg,
                         vty->text_flags);
    unlock_vty(*vty);
    return true;
}

int vty_get_property(uint32_t id,
                     uint32_t property,
                     void* out,
                     size_t size) {
    using namespace descriptor_vty;
    Vty* vty = find_vty(id);
    if (vty == nullptr || !vty->in_use) {
        return -1;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::VtyInfo)) {
        if (out == nullptr || size < sizeof(descriptor_defs::VtyInfo)) {
            return -1;
        }
        auto* info = reinterpret_cast<descriptor_defs::VtyInfo*>(out);
        lock_vty(*vty);
        copy_info(*vty, *info);
        unlock_vty(*vty);
        return 0;
    }
    return -1;
}

int vty_set_property(uint32_t id,
                     uint32_t property,
                     const void* in,
                     size_t size) {
    using namespace descriptor_vty;
    Vty* vty = find_vty(id);
    if (vty == nullptr || !vty->in_use) {
        return -1;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::VtyCursor)) {
        if (in == nullptr || size < sizeof(descriptor_defs::CursorPosition)) {
            return -1;
        }
        auto* pos =
            reinterpret_cast<const descriptor_defs::CursorPosition*>(in);
        lock_vty(*vty);
        bool ok = set_cursor(*vty, pos->x, pos->y);
        unlock_vty(*vty);
        return ok ? 0 : -1;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::VtyClear)) {
        lock_vty(*vty);
        clear_all(*vty);
        unlock_vty(*vty);
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::VtyColor)) {
        if (in == nullptr || size < sizeof(descriptor_defs::ColorPair)) {
            return -1;
        }
        auto* colors =
            reinterpret_cast<const descriptor_defs::ColorPair*>(in);
        lock_vty(*vty);
        vty->fg = colors->fg;
        vty->bg = colors->bg;
        unlock_vty(*vty);
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::VtyTextFlags)) {
        if (in == nullptr || size < sizeof(uint8_t)) {
            return -1;
        }
        lock_vty(*vty);
        vty->text_flags = *reinterpret_cast<const uint8_t*>(in);
        unlock_vty(*vty);
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::VtyCells)) {
        size_t cells = static_cast<size_t>(vty->cols) * vty->rows;
        size_t required = cells * sizeof(descriptor_defs::VtyCell);
        if (in == nullptr || size < required) {
            return -1;
        }
        auto* cells_in = reinterpret_cast<const descriptor_defs::VtyCell*>(in);
        lock_vty(*vty);
        memcpy_fast(vty->cells, cells_in, required);
        unlock_vty(*vty);
        return 0;
    }
    if (property ==
        static_cast<uint32_t>(descriptor_defs::Property::VtyInjectInput)) {
        if (in == nullptr || size == 0) {
            return 0;
        }
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(in);
        lock_vty(*vty);
        for (size_t i = 0; i < size; ++i) {
            enqueue_input(*vty, bytes[i]);
        }
        unlock_vty(*vty);
        return 0;
    }
    return -1;
}

}  // namespace descriptor
