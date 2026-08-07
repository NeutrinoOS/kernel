#pragma once
#include <stddef.h>
#include <stdint.h>

#include "lib/font8x8_basic.hpp"
#include "kernel/descriptor.hpp"

struct Framebuffer {
    uint8_t* base;
    size_t width;
    size_t height;
    size_t pitch;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
};

class Console {
public:
    explicit Console(uint32_t framebuffer_handle);

    void putc(char c);
    void puts(const char* s);
    void write(const char* data, size_t length);
    void printf(const char* fmt, ...);
    void clear();
    void set_cursor(size_t x, size_t y);
    void set_cursor_blink(bool enabled);
    void tick_cursor(uint64_t tick);
    void get_dimensions(size_t& out_cols, size_t& out_rows) const;

    void set_color(uint32_t fg, uint32_t bg);
    void set_text_flags(uint8_t flags);
    void set_update_deferred(bool deferred);
    bool set_scale(uint32_t scale);
    uint32_t get_scale() const;
    bool set_font(const descriptor_defs::ConsoleFont& font,
                  const uint8_t* data);
    descriptor_defs::ConsoleFont get_font_info() const;
    const uint8_t* get_font_data() const;
    void redraw_cells(const descriptor_defs::VtyCell* cells,
                      size_t source_cols,
                      size_t source_rows,
                      size_t source_cursor_x,
                      size_t source_cursor_y,
                      uint32_t final_fg,
                      uint32_t final_bg,
                      uint8_t final_flags);

    bool enable_back_buffer();
    void present();

private:
    uint32_t framebuffer_handle;
    Framebuffer primary_fb;
    size_t cursor_x;
    size_t cursor_y;
    uint32_t fg_color;
    uint32_t bg_color;
    uint8_t text_flags;
    uint32_t font_scale;
    descriptor_defs::ConsoleFont font_info;
    const uint8_t* font_data;
    uint64_t font_data_phys;
    size_t columns;
    size_t rows;
    size_t text_width;
    size_t text_height;
    Framebuffer back_fb;
    uint8_t* back_buffer;
    size_t frame_bytes;
    size_t back_buffer_capacity;
    size_t update_depth;
    bool cursor_blink_enabled;
    bool cursor_drawn;
    uint64_t cursor_last_toggle_tick;
    size_t cursor_update_depth;

    bool refresh_framebuffer_info();
    bool allocate_back_buffer();
    Framebuffer* draw_target();
    void flush_region(size_t x, size_t y, size_t width, size_t height);
    void flush_all();
    void update_geometry();
    size_t cell_width_px() const;
    size_t cell_height_px() const;
    size_t line_spacing_px() const;
    bool font_pixel(uint8_t glyph, size_t x, size_t y) const;

    void draw_char(char c, size_t x, size_t y);
    void clear_without_cursor();
    void toggle_cursor();
    void begin_cursor_update();
    void end_cursor_update();
    void putc_without_cursor(char c);
    void scroll();
    void print_dec(uint64_t n);
    void print_hex(uint64_t n, bool pad16);
};

extern Console* kconsole;
