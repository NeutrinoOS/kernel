#include "console.hpp"

#include <stdarg.h>

#include "lib/mem.hpp"
#include "../../arch/x86_64/memory/paging.hpp"
#include "kernel/memory/physical_allocator.hpp"
#include "kernel/time.hpp"

namespace {

constexpr uint32_t kDefaultScale = 2;
constexpr uint16_t kDefaultGlyphWidth = 8;
constexpr uint16_t kDefaultGlyphHeight = 8;
constexpr uint16_t kDefaultGlyphCount = 128;
constexpr uint8_t kMemoryModelRgb = 1;
constexpr size_t kPageSize = 0x1000;
constexpr uint64_t kCursorBlinkMilliseconds = 500;

size_t bytes_per_pixel(const Framebuffer* fb) {
    if (fb == nullptr || fb->bpp == 0) {
        return 4;
    }
    return static_cast<size_t>((fb->bpp + 7) / 8);
}

uint32_t scale_component(uint8_t value, uint8_t bits) {
    if (bits == 0) {
        return 0;
    }
    if (bits >= 8) {
        return static_cast<uint32_t>(value) << (bits - 8);
    }
    uint32_t max_value = (1u << bits) - 1u;
    return (static_cast<uint32_t>(value) * max_value + 127u) / 255u;
}

uint64_t pack_color(const Framebuffer* fb, uint32_t argb) {
    if (fb == nullptr) {
        return argb;
    }

    if (fb->memory_model != kMemoryModelRgb) {
        return argb;
    }

    uint8_t red = static_cast<uint8_t>((argb >> 16) & 0xFF);
    uint8_t green = static_cast<uint8_t>((argb >> 8) & 0xFF);
    uint8_t blue = static_cast<uint8_t>(argb & 0xFF);

    uint64_t packed = 0;
    packed |= static_cast<uint64_t>(
                  scale_component(red, fb->red_mask_size))
              << fb->red_mask_shift;
    packed |= static_cast<uint64_t>(
                  scale_component(green, fb->green_mask_size))
              << fb->green_mask_shift;
    packed |= static_cast<uint64_t>(
                  scale_component(blue, fb->blue_mask_size))
              << fb->blue_mask_shift;
    return packed;
}

inline void store_pixel(uint8_t* dst, size_t bpp, uint64_t packed_color) {
    for (size_t i = 0; i < bpp; ++i) {
        dst[i] = static_cast<uint8_t>((packed_color >> (8 * i)) & 0xFFu);
    }
}

void fill_span(uint8_t* dst,
               size_t pixel_count,
               size_t bpp,
               uint64_t packed_color) {
    if (dst == nullptr || pixel_count == 0 || bpp == 0) {
        return;
    }

    size_t total_bytes = pixel_count * bpp;
    store_pixel(dst, bpp, packed_color);
    size_t filled = bpp;

    while (filled < total_bytes) {
        size_t copy = (filled < total_bytes - filled) ? filled
                                                      : (total_bytes - filled);
        memcpy(dst + filled, dst, copy);
        filled += copy;
    }
}


void fill_rect(Framebuffer* fb,
               size_t x,
               size_t y,
               size_t width,
               size_t height,
               uint32_t color) {
    if (fb == nullptr || fb->base == nullptr) {
        return;
    }
    if (width == 0 || height == 0) {
        return;
    }

    size_t bpp = bytes_per_pixel(fb);
    if (bpp == 0) {
        return;
    }

    if (x >= fb->width || y >= fb->height) {
        return;
    }

    if (width > fb->width - x) {
        width = fb->width - x;
    }

    uint64_t packed = pack_color(fb, color);
    for (size_t row = 0; row < height; ++row) {
        size_t py = y + row;
        if (py >= fb->height) {
            break;
        }

        uint8_t* dst = fb->base + py * fb->pitch + x * bpp;
        size_t max_bytes = fb->pitch - x * bpp;
        size_t row_pixels = width;
        size_t needed_bytes = row_pixels * bpp;
        if (needed_bytes > max_bytes) {
            row_pixels = max_bytes / bpp;
            if (row_pixels == 0) {
                continue;
            }
        }

        fill_span(dst, row_pixels, bpp, packed);
    }
}

}  // namespace

Console* kconsole = nullptr;
uint32_t DEFAULT_BG = 0x00000000;

bool Console::allocate_back_buffer() {
    if (back_buffer != nullptr) {
        return true;
    }
    if (primary_fb.base == nullptr || frame_bytes == 0) {
        return false;
    }

    size_t pages = (frame_bytes + kPageSize - 1) / kPageSize;
    uint64_t phys = memory::alloc_kernel_block_pages(pages);
    if (phys == 0) {
        return false;
    }
    auto* start = static_cast<uint8_t*>(paging_phys_to_virt(phys));

    back_buffer = start;
    back_buffer_capacity = pages * kPageSize;
    back_fb = primary_fb;
    back_fb.base = back_buffer;
    return true;
}

Framebuffer* Console::draw_target() {
    if (back_buffer != nullptr) {
        return &back_fb;
    }
    return &primary_fb;
}

bool Console::enable_back_buffer() {
    if (back_buffer != nullptr) {
        return true;
    }
    if (!allocate_back_buffer()) {
        return false;
    }
    if (frame_bytes == 0 || primary_fb.base == nullptr) {
        return true;
    }

    size_t bytes = frame_bytes;
    if (bytes > back_buffer_capacity) {
        bytes = back_buffer_capacity;
    }
    if (bytes == 0) {
        return true;
    }

    memcpy(back_buffer, primary_fb.base, bytes);
    return true;
}

void Console::flush_region(size_t x, size_t y, size_t width, size_t height) {
    refresh_framebuffer_info();
    if (back_buffer == nullptr || primary_fb.base == nullptr) {
        return;
    }
    if (!descriptor::framebuffer_is_active(0)) {
        return;
    }
    if (update_depth != 0) {
        return;
    }
    if (width == 0 || height == 0) {
        return;
    }
    if (x >= primary_fb.width || y >= primary_fb.height) {
        return;
    }

    size_t bpp = bytes_per_pixel(&primary_fb);
    if (bpp == 0) {
        return;
    }

    size_t max_width = primary_fb.width - x;
    size_t copy_width = width > max_width ? max_width : width;
    size_t max_height = primary_fb.height - y;
    size_t copy_height = height > max_height ? max_height : height;

    size_t row_bytes = copy_width * bpp;

    for (size_t row = 0; row < copy_height; ++row) {
        size_t offset = (y + row) * primary_fb.pitch + x * bpp;
        if (offset >= frame_bytes || offset >= back_buffer_capacity) {
            break;
        }

        size_t usable = frame_bytes - offset;
        size_t cap_remaining = back_buffer_capacity - offset;
        if (usable > cap_remaining) {
            usable = cap_remaining;
        }

        size_t to_copy = row_bytes;
        if (to_copy > usable) {
            to_copy = usable;
        }
        if (to_copy == 0) {
            break;
        }

        memcpy_simd(primary_fb.base + offset, back_buffer + offset, to_copy);
    }
}

void Console::flush_all() {
    refresh_framebuffer_info();
    if (back_buffer == nullptr || primary_fb.base == nullptr) {
        return;
    }
    if (!descriptor::framebuffer_is_active(0)) {
        return;
    }
    if (update_depth != 0) {
        return;
    }

    if (frame_bytes == 0 || back_buffer_capacity == 0) {
        return;
    }

    size_t bytes = frame_bytes;
    if (bytes > back_buffer_capacity) {
        bytes = back_buffer_capacity;
    }
    if (bytes == 0) {
        return;
    }

    memcpy_simd(primary_fb.base, back_buffer, bytes);
}

void Console::present() {
    size_t saved_depth = update_depth;
    update_depth = 0;
    flush_all();
    update_depth = saved_depth;
}

void Console::set_update_deferred(bool deferred) {
    if (deferred) {
        ++update_depth;
        return;
    }
    if (update_depth == 0) {
        return;
    }
    --update_depth;
    if (update_depth == 0) {
        flush_all();
    }
}

void Console::get_dimensions(size_t& out_cols, size_t& out_rows) const {
    out_cols = columns;
    out_rows = rows;
}

void Console::set_cursor(size_t x, size_t y) {
    begin_cursor_update();
    if (x >= columns) {
        x = columns ? columns - 1 : 0;
    }
    if (y >= rows) {
        y = rows ? rows - 1 : 0;
    }
    cursor_x = x;
    cursor_y = y;
    end_cursor_update();
}

bool Console::refresh_framebuffer_info() {
    descriptor_defs::FramebufferInfo info{};
    int result = descriptor::get_property_kernel(
        framebuffer_handle,
        static_cast<uint32_t>(descriptor_defs::Property::FramebufferInfo),
        &info,
        sizeof(info));
    if (result != 0) {
        primary_fb = {};
        frame_bytes = 0;
        return false;
    }

    primary_fb.base = reinterpret_cast<uint8_t*>(info.virtual_base);
    primary_fb.width = static_cast<size_t>(info.width);
    primary_fb.height = static_cast<size_t>(info.height);
    primary_fb.pitch = static_cast<size_t>(info.pitch);
    primary_fb.bpp = info.bpp;
    primary_fb.memory_model = info.memory_model;
    primary_fb.red_mask_size = info.red_mask_size;
    primary_fb.red_mask_shift = info.red_mask_shift;
    primary_fb.green_mask_size = info.green_mask_size;
    primary_fb.green_mask_shift = info.green_mask_shift;
    primary_fb.blue_mask_size = info.blue_mask_size;
    primary_fb.blue_mask_shift = info.blue_mask_shift;

    frame_bytes = (primary_fb.pitch != 0)
                      ? primary_fb.pitch * primary_fb.height
                      : 0;
    if (back_buffer != nullptr) {
        back_fb = primary_fb;
        back_fb.base = back_buffer;
    }
    return primary_fb.base != nullptr;
}

Console::Console(uint32_t framebuffer_handle)
    : framebuffer_handle(framebuffer_handle),
      primary_fb{},
      cursor_x(0),
      cursor_y(0),
      fg_color(0xFFFFFFFF),
      bg_color(0x00000000),
      text_flags(0),
      font_scale(kDefaultScale),
      font_info{kDefaultGlyphWidth,
                kDefaultGlyphHeight,
                kDefaultGlyphCount,
                1,
                sizeof(font8x8_basic),
                0},
      font_data(&font8x8_basic[0][0]),
      font_data_phys(0),
      columns(0),
      rows(0),
      text_width(0),
      text_height(0),
      back_fb{},
      back_buffer(nullptr),
      frame_bytes(0),
      back_buffer_capacity(0),
      update_depth(0),
      cursor_blink_enabled(false),
      cursor_drawn(false),
      cursor_last_toggle_tick(0),
      cursor_update_depth(0) {  // white on black
    refresh_framebuffer_info();

    update_geometry();
}

size_t Console::cell_width_px() const {
    return static_cast<size_t>(font_info.width) * font_scale;
}

size_t Console::line_spacing_px() const {
    return static_cast<size_t>(3) * font_scale;
}

size_t Console::cell_height_px() const {
    return static_cast<size_t>(font_info.height) * font_scale +
           line_spacing_px();
}

void Console::update_geometry() {
    size_t cw = cell_width_px();
    size_t ch = cell_height_px();
    if (cw == 0) {
        cw = 1;
    }
    if (ch == 0) {
        ch = 1;
    }
    columns = (primary_fb.width != 0) ? (primary_fb.width / cw) : 0;
    rows = (primary_fb.height != 0) ? (primary_fb.height / ch) : 0;
    text_width = columns * cw;
    text_height = rows * ch;

    if (columns == 0) {
        columns = 1;
        text_width = cw;
    }
    if (rows == 0) {
        rows = 1;
        text_height = ch;
    }

    if (frame_bytes == 0 && primary_fb.pitch != 0) {
        frame_bytes = primary_fb.pitch * primary_fb.height;
    }
}

void Console::draw_char(char c, size_t x, size_t y) {
    Framebuffer* target = draw_target();
    if (target == nullptr || target->base == nullptr) {
        return;
    }
    if (x >= columns || y >= rows) {
        return;
    }
    uint8_t uc = static_cast<uint8_t>(c);
    if (uc >= font_info.glyph_count) return;

    size_t glyph_width = cell_width_px();
    size_t glyph_height = static_cast<size_t>(font_info.height) * font_scale;
    size_t base_px = x * glyph_width;
    size_t base_py = y * cell_height_px();
    if (base_px >= text_width || base_py >= target->height) {
        return;
    }

    size_t bpp = bytes_per_pixel(target);
    if (bpp == 0) {
        return;
    }

    size_t max_draw_width = text_width - base_px;
    if (max_draw_width == 0) {
        return;
    }

    size_t glyph_draw_width = glyph_width;
    if (glyph_draw_width > max_draw_width) {
        glyph_draw_width = max_draw_width;
    }
    if (glyph_draw_width == 0) {
        return;
    }

    uint64_t packed_fg = pack_color(target, fg_color);
    uint64_t packed_bg = pack_color(target, bg_color);

    for (size_t row = 0; row < font_info.height; ++row) {
        for (uint32_t dy = 0; dy < font_scale; ++dy) {
            size_t py = base_py + static_cast<size_t>(row) * font_scale + dy;
            if (py >= target->height) {
                continue;
            }

            uint8_t* dst = target->base + py * target->pitch + base_px * bpp;
            size_t px_offset = 0;
            for (size_t col = 0;
                 col < font_info.width && px_offset < glyph_draw_width;
                 ++col) {
                bool bit_set = font_pixel(uc, col, row);
                size_t span = static_cast<size_t>(font_scale);
                if (px_offset + span > glyph_draw_width) {
                    span = glyph_draw_width - px_offset;
                }
                fill_span(dst + px_offset * bpp,
                          span,
                          bpp,
                          bit_set ? packed_fg : packed_bg);
                px_offset += span;
            }
            if (px_offset < glyph_draw_width) {
                fill_span(dst + px_offset * bpp,
                          glyph_draw_width - px_offset,
                          bpp,
                          packed_bg);
            }
        }
    }

    // fill line spacing with background colour
    size_t gap_start_y = base_py + glyph_height;
    size_t line_spacing = line_spacing_px();
    if (line_spacing > 0 && gap_start_y < target->height && glyph_draw_width > 0) {
        fill_rect(target,
                  base_px,
                  gap_start_y,
                  glyph_draw_width,
                  line_spacing,
                  bg_color);
    }

    if ((text_flags & descriptor_defs::kTextCellUnderline) != 0) {
        size_t underline_y = base_py + glyph_height;
        if (underline_y >= target->height && target->height != 0) {
            underline_y = target->height - 1;
        }
        if (underline_y < target->height) {
            size_t underline_h = static_cast<size_t>(font_scale);
            if (underline_y + underline_h > target->height) {
                underline_h = target->height - underline_y;
            }
            fill_rect(target,
                      base_px,
                      underline_y,
                      glyph_draw_width,
                      underline_h,
                      fg_color);
        }
    }

    if (back_buffer != nullptr) {
        size_t flush_height = glyph_height + line_spacing;
        size_t remaining_height = (base_py < target->height)
                                      ? (target->height - base_py)
                                      : 0;
        if (flush_height > remaining_height) {
            flush_height = remaining_height;
        }
        if (flush_height == 0) {
            flush_height = glyph_height;
        }
        size_t flush_width = glyph_draw_width;
        if (flush_width == 0) {
            flush_width = glyph_width;
        }
        flush_region(base_px, base_py, flush_width, flush_height);
    }
}

void Console::set_color(uint32_t fg, uint32_t bg) {
    fg_color = fg;
    bg_color = bg;
}

void Console::set_text_flags(uint8_t flags) {
    text_flags = flags;
}

bool Console::set_scale(uint32_t new_scale) {
    if (new_scale < descriptor_defs::kConsoleMinScale ||
        new_scale > descriptor_defs::kConsoleMaxScale) {
        return false;
    }
    if (font_scale == new_scale) {
        return true;
    }
    begin_cursor_update();
    font_scale = new_scale;
    update_geometry();
    end_cursor_update();
    return true;
}

uint32_t Console::get_scale() const {
    return font_scale;
}

bool Console::font_pixel(uint8_t glyph, size_t x, size_t y) const {
    if (font_data == nullptr || glyph >= font_info.glyph_count ||
        x >= font_info.width || y >= font_info.height) {
        return false;
    }
    size_t glyph_bytes = static_cast<size_t>(font_info.height) *
                         font_info.bytes_per_row;
    size_t offset = static_cast<size_t>(glyph) * glyph_bytes +
                    y * font_info.bytes_per_row + x / 8;
    size_t bit = x % 8;
    if ((font_info.flags & descriptor_defs::kConsoleFontMsbFirst) != 0) {
        bit = 7 - bit;
    }
    return (font_data[offset] & (1u << bit)) != 0;
}

bool Console::set_font(const descriptor_defs::ConsoleFont& new_font,
                       const uint8_t* data) {
    if (data == nullptr || new_font.width == 0 || new_font.height == 0 ||
        new_font.glyph_count == 0 || new_font.glyph_count > 256 ||
        new_font.width > 64 || new_font.height > 64 ||
        new_font.bytes_per_row != (new_font.width + 7u) / 8u ||
        (new_font.flags & ~descriptor_defs::kConsoleFontMsbFirst) != 0) {
        return false;
    }
    size_t expected_size = static_cast<size_t>(new_font.glyph_count) *
                           new_font.height * new_font.bytes_per_row;
    if (new_font.data_size != expected_size ||
        expected_size > descriptor_defs::kConsoleMaxFontDataSize) {
        return false;
    }

    size_t pages = (expected_size + kPageSize - 1) / kPageSize;
    uint64_t new_phys = memory::alloc_kernel_block_pages(pages);
    if (new_phys == 0) {
        return false;
    }
    auto* new_data = static_cast<uint8_t*>(paging_phys_to_virt(new_phys));
    memcpy(new_data, data, expected_size);

    begin_cursor_update();
    uint64_t old_phys = font_data_phys;
    font_info = new_font;
    font_data = new_data;
    font_data_phys = new_phys;
    update_geometry();
    if (old_phys != 0) {
        memory::free_kernel_block(old_phys);
    }
    end_cursor_update();
    return true;
}

descriptor_defs::ConsoleFont Console::get_font_info() const {
    return font_info;
}

const uint8_t* Console::get_font_data() const {
    return font_data;
}

void Console::redraw_cells(const descriptor_defs::VtyCell* cells,
                           size_t source_cols,
                           size_t source_rows,
                           size_t source_cursor_x,
                           size_t source_cursor_y,
                           uint32_t final_fg,
                           uint32_t final_bg,
                           uint8_t final_flags) {
    if (cells == nullptr || source_cols == 0 || source_rows == 0) {
        clear();
        return;
    }

    begin_cursor_update();
    set_update_deferred(true);
    fg_color = final_fg;
    bg_color = final_bg;
    text_flags = final_flags;
    clear_without_cursor();

    size_t draw_rows = source_rows < rows ? source_rows : rows;
    size_t source_start_y = 0;
    if (source_rows > draw_rows && source_cursor_y >= draw_rows) {
        source_start_y = source_cursor_y - draw_rows + 1;
        size_t max_start = source_rows - draw_rows;
        if (source_start_y > max_start) {
            source_start_y = max_start;
        }
    }
    size_t draw_cols = source_cols < columns ? source_cols : columns;
    for (size_t y = 0; y < draw_rows; ++y) {
        size_t source_y = source_start_y + y;
        for (size_t x = 0; x < draw_cols; ++x) {
            const auto& cell = cells[source_y * source_cols + x];
            fg_color = cell.fg;
            bg_color = cell.bg;
            text_flags = cell.flags;
            draw_char(static_cast<char>(cell.ch), x, y);
        }
    }

    fg_color = final_fg;
    bg_color = final_bg;
    text_flags = final_flags;
    cursor_x = source_cursor_x < columns ? source_cursor_x
                                         : (columns ? columns - 1 : 0);
    cursor_y = source_cursor_y >= source_start_y
                   ? source_cursor_y - source_start_y
                   : 0;
    if (cursor_y >= rows) {
        cursor_y = rows ? rows - 1 : 0;
    }
    set_update_deferred(false);
    end_cursor_update();
}

void Console::scroll() {
    size_t row_height = cell_height_px();
    Framebuffer* target = draw_target();
    if (row_height == 0 || target == nullptr || target->base == nullptr) {
        return;
    }
    if (text_height == 0) {
        text_height = target->height - (target->height % row_height);
    }
    if (row_height >= text_height) {
        fill_rect(target, 0, 0, target->width, target->height, bg_color);
        if (back_buffer != nullptr) {
            flush_all();
        }
        cursor_y = 0;
        return;
    }

    size_t rows_to_copy = text_height - row_height;
    size_t bytes_to_copy = rows_to_copy * target->pitch;
    if (bytes_to_copy > 0) {
        memmove_simd(target->base,
                     target->base + row_height * target->pitch,
                     bytes_to_copy);
    }

    size_t copy_width = text_width > 0 ? text_width : target->width;
    if (copy_width > target->width) {
        copy_width = target->width;
    }

    fill_rect(target,
              0,
              text_height - row_height,
              copy_width,
              row_height,
              bg_color);

    if (cursor_y > 0) cursor_y--;

    if (back_buffer != nullptr) {
        flush_all();
    }
}

void Console::putc_without_cursor(char c) {
    Framebuffer* target = draw_target();
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        if (target != nullptr &&
            (cursor_y + 1) * cell_height_px() >= target->height) {
            scroll();
        }
        return;
    }
    if (c == '\r') {
        cursor_x = 0;
        return;
    }
    if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
        } else if (cursor_y > 0) {
            size_t max_cols = target != nullptr ? (target->width / cell_width_px())
                                                : 0;
            if (max_cols > 0) {
                cursor_y--;
                cursor_x = max_cols - 1;
            }
        }
        draw_char(' ', cursor_x, cursor_y);
        return;
    }

    draw_char(c, cursor_x, cursor_y);
    cursor_x++;
    if (cursor_x >= columns) {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= rows) scroll();
    }
}

void Console::putc(char c) {
    begin_cursor_update();
    putc_without_cursor(c);
    end_cursor_update();
}

void Console::puts(const char* s) {
    begin_cursor_update();
    while (*s) putc_without_cursor(*s++);
    end_cursor_update();
}

void Console::write(const char* data, size_t length) {
    if (data == nullptr || length == 0) {
        return;
    }
    begin_cursor_update();
    for (size_t i = 0; i < length; ++i) {
        putc_without_cursor(data[i]);
    }
    end_cursor_update();
}

void Console::clear() {
    begin_cursor_update();
    clear_without_cursor();
    end_cursor_update();
}

void Console::clear_without_cursor() {
    Framebuffer* target = draw_target();
    if (target == nullptr || target->base == nullptr) {
        return;
    }
    fill_rect(target, 0, 0, target->width, target->height, bg_color);
    if (back_buffer != nullptr) {
        flush_all();
    }
    cursor_x = cursor_y = 0;
}

void Console::toggle_cursor() {
    Framebuffer* target = draw_target();
    if (target == nullptr || target->base == nullptr || columns == 0 ||
        rows == 0 || cursor_x >= columns || cursor_y >= rows) {
        cursor_drawn = false;
        return;
    }

    const size_t width = cell_width_px();
    size_t height = font_scale;
    const size_t base_x = cursor_x * width;
    const size_t glyph_height = static_cast<size_t>(font_info.height) * font_scale;
    const size_t base_y = cursor_y * cell_height_px() + glyph_height - height;
    if (base_x >= target->width || base_y >= target->height) {
        cursor_drawn = false;
        return;
    }
    size_t draw_width = width;
    if (draw_width > target->width - base_x) {
        draw_width = target->width - base_x;
    }
    if (height > target->height - base_y) {
        height = target->height - base_y;
    }
    const size_t bpp = bytes_per_pixel(target);
    if (bpp == 0 || base_x > target->pitch / bpp ||
        draw_width > target->pitch / bpp - base_x) {
        cursor_drawn = false;
        return;
    }
    for (size_t row = 0; row < height; ++row) {
        uint8_t* pixel = target->base + (base_y + row) * target->pitch +
                         base_x * bpp;
        for (size_t byte = 0; byte < draw_width * bpp; ++byte) {
            pixel[byte] ^= 0xFFu;
        }
    }
    cursor_drawn = !cursor_drawn;
    if (back_buffer != nullptr) {
        if (descriptor::framebuffer_is_active(0) && primary_fb.base != nullptr &&
            primary_fb.pitch == target->pitch &&
            base_y + height <= primary_fb.height) {
            const size_t row_bytes = draw_width * bpp;
            for (size_t row = 0; row < height; ++row) {
                const size_t offset = (base_y + row) * target->pitch +
                                      base_x * bpp;
                memcpy_simd(primary_fb.base + offset,
                            target->base + offset,
                            row_bytes);
            }
        }
    }
}

void Console::begin_cursor_update() {
    while (__atomic_exchange_n(&cursor_update_depth,
                               1,
                               __ATOMIC_ACQUIRE) != 0) {
        asm volatile("pause");
    }
    if (cursor_drawn) {
        toggle_cursor();
    }
}

void Console::end_cursor_update() {
    if (cursor_blink_enabled && !cursor_drawn) {
        toggle_cursor();
        cursor_last_toggle_tick = timekeeping::tick_count();
    }
    __atomic_store_n(&cursor_update_depth, 0, __ATOMIC_RELEASE);
}

void Console::set_cursor_blink(bool enabled) {
    begin_cursor_update();
    cursor_blink_enabled = enabled;
    end_cursor_update();
}

void Console::tick_cursor(uint64_t tick) {
    if (!cursor_blink_enabled || update_depth != 0 ||
        __atomic_exchange_n(&cursor_update_depth,
                            1,
                            __ATOMIC_ACQUIRE) != 0) {
        return;
    }
    const uint64_t interval = timekeeping::ticks_for_duration_ns(
        kCursorBlinkMilliseconds * 1000000ull);
    if (cursor_blink_enabled && update_depth == 0 &&
        tick - cursor_last_toggle_tick >= interval) {
        toggle_cursor();
        cursor_last_toggle_tick = tick;
    }
    __atomic_store_n(&cursor_update_depth, 0, __ATOMIC_RELEASE);
}

void Console::print_dec(uint64_t n) {
    char buf[21];
    int i = 0;
    if (n == 0) {
        putc_without_cursor('0');
        return;
    }
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i--) putc_without_cursor(buf[i]);
}

void Console::print_hex(uint64_t n, bool pad16) {
    char buf[17];
    int i = 0;

    // handle zero explicitly
    if (n == 0) {
        const char* zero = pad16 ? "0x0000000000000000" : "0x0";
        while (*zero != '\0') {
            putc_without_cursor(*zero++);
        }
        return;
    }

    // convert to hex string (reversed)
    while (n > 0 && i < 16) {
        uint8_t d = n & 0xF;
        buf[i++] = (d < 10) ? ('0' + d) : ('a' + (d - 10));
        n >>= 4;
    }

    putc_without_cursor('0');
    putc_without_cursor('x');

    // pad to 16 digits if requested
    if (pad16 && i < 16) {
        for (int j = 0; j < 16 - i; j++)
            putc_without_cursor('0');
    }

    // print reversed buffer
    while (i--)
        putc_without_cursor(buf[i]);
}


void Console::printf(const char* fmt, ...) {
    if (fmt == nullptr) {
        return;
    }
    begin_cursor_update();
    va_list args;
    va_start(args, fmt);
    while (*fmt) {
        if (*fmt != '%') {
            putc_without_cursor(*fmt++);
            continue;
        }
        fmt++;

        bool pad16 = false;

        // check for "%016x"
        if (*fmt == '0' && *(fmt + 1) == '1' && *(fmt + 2) == '6') {
            fmt += 3;
            if (*fmt == 'x') {
                pad16 = true;
            }
        }

        switch (*fmt++) {
            case 'd':
                print_dec(va_arg(args, int));
                break;
            case 'x':
                print_hex(va_arg(args, unsigned long long), pad16);
                break;
            case 's':
                if (const char* text = va_arg(args, const char*)) {
                    while (*text != '\0') {
                        putc_without_cursor(*text++);
                    }
                }
                break;
            case 'c':
                putc_without_cursor((char)va_arg(args, int));
                break;
            case '%':
                putc_without_cursor('%');
                break;
            default:
                putc_without_cursor('?');
                break;
        }
    }
    va_end(args);
    end_cursor_update();
}
