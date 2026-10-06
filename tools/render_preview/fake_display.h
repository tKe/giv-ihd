#pragma once

// The one genuinely custom piece of this harness (see esphome_shim.h for
// what's lifted verbatim vs. new). Everything below `draw_pixel_at` is
// copied near-verbatim from esphome/components/display/display.cpp: those
// functions only ever call `this->draw_pixel_at(...)`, so reusing the real
// algorithm here means lines/rectangles/text land on exactly the same
// pixels as the real Display class would compute, just written into an
// in-memory buffer instead of driven out over SPI.
//
// Deliberately NOT a subclass of ESPHome's real Display (that class
// inherits PollingComponent and pulls in the scheduler, automation, and
// several other components' headers - none of it reachable from
// draw_page_detail/draw_page_flow, all of it dead weight for a host binary
// whose only job is to call draw_pixel_at into a buffer).

#include <cstdio>
#include <cstdlib>
#include "esphome_shim.h"

namespace esphome::display {

class Display {
 public:
  static constexpr int WIDTH = 240;
  static constexpr int HEIGHT = 240;

  Display() { this->fill(Color::BLACK); }

  int get_width() { return WIDTH; }
  int get_height() { return HEIGHT; }

  void draw_pixel_at(int x, int y, Color color) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT)
      return;
    auto &px = this->buffer_[y][x];
    px[0] = color.r;
    px[1] = color.g;
    px[2] = color.b;
  }

  // --- Geometry, verbatim from Display:: in display.cpp -----------------

  void fill(Color color) { this->filled_rectangle(0, 0, this->get_width(), this->get_height(), color); }

  void HOT line(int x1, int y1, int x2, int y2, Color color) {
    const int32_t dx = std::abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    const int32_t dy = -std::abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int32_t err = dx + dy;

    while (true) {
      this->draw_pixel_at(x1, y1, color);
      if (x1 == x2 && y1 == y2)
        break;
      int32_t e2 = 2 * err;
      if (e2 >= dy) { err += dy; x1 += sx; }
      if (e2 <= dx) { err += dx; y1 += sy; }
    }
  }

  void HOT horizontal_line(int x, int y, int width, Color color) {
    for (int i = x; i < x + width; i++) this->draw_pixel_at(i, y, color);
  }

  void HOT vertical_line(int x, int y, int height, Color color) {
    for (int i = y; i < y + height; i++) this->draw_pixel_at(x, i, color);
  }

  void rectangle(int x1, int y1, int width, int height, Color color) {
    this->horizontal_line(x1, y1, width, color);
    this->horizontal_line(x1, y1 + height - 1, width, color);
    this->vertical_line(x1, y1, height, color);
    this->vertical_line(x1 + width - 1, y1, height, color);
  }

  void filled_rectangle(int x1, int y1, int width, int height, Color color) {
    for (int i = y1; i < y1 + height; i++) this->horizontal_line(x1, i, width, color);
  }

  // --- Text, verbatim from Display:: in display.cpp ---------------------

  void get_text_bounds(int x, int y, const char *text, BaseFont *font, TextAlign align, int *x1, int *y1,
                       int *width, int *height) {
    int x_offset, baseline;
    font->measure(text, width, &x_offset, &baseline, height);

    auto x_align = TextAlign(int(align) & 0x18);
    auto y_align = TextAlign(int(align) & 0x07);

    switch (x_align) {
      case TextAlign::RIGHT: *x1 = x - *width - x_offset; break;
      case TextAlign::CENTER_HORIZONTAL: *x1 = x - (*width + x_offset) / 2; break;
      case TextAlign::LEFT:
      default: *x1 = x; break;
    }
    switch (y_align) {
      case TextAlign::BOTTOM: *y1 = y - *height; break;
      case TextAlign::BASELINE: *y1 = y - baseline; break;
      case TextAlign::CENTER_VERTICAL: *y1 = y - (*height) / 2; break;
      case TextAlign::TOP:
      default: *y1 = y; break;
    }
  }

  void print(int x, int y, BaseFont *font, Color color, TextAlign align, const char *text, Color background) {
    int x_start, y_start, width, height;
    this->get_text_bounds(x, y, text, font, align, &x_start, &y_start, &width, &height);
    font->print(x_start, y_start, this, color, text, background);
  }
  void print(int x, int y, BaseFont *font, Color color, const char *text, Color background) {
    this->print(x, y, font, color, TextAlign::TOP_LEFT, text, background);
  }
  void print(int x, int y, BaseFont *font, TextAlign align, const char *text) {
    this->print(x, y, font, COLOR_ON, align, text, COLOR_OFF);
  }
  void print(int x, int y, BaseFont *font, const char *text) {
    this->print(x, y, font, COLOR_ON, TextAlign::TOP_LEFT, text, COLOR_OFF);
  }

  void vprintf_(int x, int y, BaseFont *font, Color color, Color background, TextAlign align, const char *format,
               va_list arg) {
    char buffer[256];
    int ret = vsnprintf(buffer, sizeof(buffer), format, arg);
    if (ret > 0)
      this->print(x, y, font, color, align, buffer, background);
  }
  void printf(int x, int y, BaseFont *font, Color color, Color background, TextAlign align, const char *format,
             ...) {
    va_list arg; va_start(arg, format);
    this->vprintf_(x, y, font, color, background, align, format, arg);
    va_end(arg);
  }
  void printf(int x, int y, BaseFont *font, Color color, TextAlign align, const char *format, ...) {
    va_list arg; va_start(arg, format);
    this->vprintf_(x, y, font, color, COLOR_OFF, align, format, arg);
    va_end(arg);
  }
  void printf(int x, int y, BaseFont *font, Color color, const char *format, ...) {
    va_list arg; va_start(arg, format);
    this->vprintf_(x, y, font, color, COLOR_OFF, TextAlign::TOP_LEFT, format, arg);
    va_end(arg);
  }
  void printf(int x, int y, BaseFont *font, TextAlign align, const char *format, ...) {
    va_list arg; va_start(arg, format);
    this->vprintf_(x, y, font, COLOR_ON, COLOR_OFF, align, format, arg);
    va_end(arg);
  }
  void printf(int x, int y, BaseFont *font, const char *format, ...) {
    va_list arg; va_start(arg, format);
    this->vprintf_(x, y, font, COLOR_ON, COLOR_OFF, TextAlign::TOP_LEFT, format, arg);
    va_end(arg);
  }

  // --- Harness-only: not part of the real Display API --------------------

  // Uncompressed 24-bit BMP, bottom-up row order per the format spec - zero
  // dependencies, no vendored image library needed for something this small.
  bool save_bmp(const char *path) const {
    FILE *f = fopen(path, "wb");
    if (f == nullptr) return false;

    const int row_bytes = WIDTH * 3;
    const int padding = (4 - (row_bytes % 4)) % 4;
    const int stride = row_bytes + padding;
    const uint32_t pixel_data_size = (uint32_t) (stride * HEIGHT);
    const uint32_t file_size = 54 + pixel_data_size;

    uint8_t header[54] = {0};
    header[0] = 'B'; header[1] = 'M';
    *(uint32_t *) &header[2] = file_size;
    *(uint32_t *) &header[10] = 54;         // pixel data offset
    *(uint32_t *) &header[14] = 40;         // DIB header size
    *(int32_t *) &header[18] = WIDTH;
    *(int32_t *) &header[22] = HEIGHT;
    *(uint16_t *) &header[26] = 1;          // planes
    *(uint16_t *) &header[28] = 24;         // bpp
    *(uint32_t *) &header[34] = pixel_data_size;
    fwrite(header, 1, sizeof(header), f);

    uint8_t pad[3] = {0, 0, 0};
    for (int y = HEIGHT - 1; y >= 0; y--) {
      for (int x = 0; x < WIDTH; x++) {
        auto &px = this->buffer_[y][x];
        uint8_t bgr[3] = {px[2], px[1], px[0]};  // BMP stores BGR
        fwrite(bgr, 1, 3, f);
      }
      if (padding > 0) fwrite(pad, 1, padding, f);
    }
    fclose(f);
    return true;
  }

 private:
  uint8_t buffer_[HEIGHT][WIDTH][3];
};

}  // namespace esphome::display

namespace esphome::font {

// font::Font::print needs the concrete Display type above, so it's defined
// here rather than in esphome_shim.h. Verbatim from font.cpp.
inline void Font::print(int x_start, int y_start, display::Display *display, Color color, const char *text,
                        Color background) {
  int x_at = x_start;
  for (;;) {
    size_t length;
    auto code_point = extract_unicode_codepoint(text, &length);
    if (length == 0) break;
    text += length;
    auto *glyph = this->find_glyph(code_point);
    if (glyph == nullptr) {
      if (!this->glyphs_.empty()) {
        uint8_t glyph_width = this->glyphs_[0].advance;
        display->rectangle(x_at, y_start, glyph_width, this->height_, color);
        x_at += glyph_width;
      }
      continue;
    }

    const uint8_t *data = glyph->data;
    const int max_x = x_at + glyph->offset_x + glyph->width;
    const int max_y = y_start + glyph->offset_y + glyph->height;

    uint8_t bitmask = 0;
    uint8_t pixel_data = 0;
    uint8_t bpp_max = (1 << this->bpp_) - 1;
    auto diff_r = (float) color.r - (float) background.r;
    auto diff_g = (float) color.g - (float) background.g;
    auto diff_b = (float) color.b - (float) background.b;
    auto diff_w = (float) color.w - (float) background.w;
    auto b_r = (float) background.r;
    auto b_g = (float) background.g;
    auto b_b = (float) background.b;
    auto b_w = (float) background.w;
    for (int glyph_y = y_start + glyph->offset_y; glyph_y != max_y; glyph_y++) {
      for (int glyph_x = x_at + glyph->offset_x; glyph_x != max_x; glyph_x++) {
        uint8_t pixel = 0;
        for (uint8_t bit_num = 0; bit_num != this->bpp_; bit_num++) {
          if (bitmask == 0) {
            pixel_data = progmem_read_byte(data++);
            bitmask = 0x80;
          }
          pixel <<= 1;
          if ((pixel_data & bitmask) != 0) pixel |= 1;
          bitmask >>= 1;
        }
        if (pixel == bpp_max) {
          display->draw_pixel_at(glyph_x, glyph_y, color);
        } else if (pixel != 0) {
          auto on = (float) pixel / (float) bpp_max;
          auto blended = Color((uint8_t) (diff_r * on + b_r), (uint8_t) (diff_g * on + b_g),
                               (uint8_t) (diff_b * on + b_b), (uint8_t) (diff_w * on + b_w));
          display->draw_pixel_at(glyph_x, glyph_y, blended);
        }
      }
    }
    x_at += glyph->advance;
  }
}

}  // namespace esphome::font
