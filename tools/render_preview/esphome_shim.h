#pragma once

// Minimal stand-in for the ESPHome symbols dashboard_draw.h/dashboard_pages.h
// reference, so those two production files can be #include'd completely
// unmodified in a host binary.
//
// This is NOT a from-scratch reimplementation of font/text handling: the
// font::Glyph/font::Font classes below, and the Display geometry/text
// methods in fake_display.h, are lifted near-verbatim from ESPHome's real
// esphome/components/font/font.{h,cpp} and esphome/components/display/
// display.{h,cpp} (MIT-licensed, same project). That's deliberate - the
// whole point of this harness is that get_text_bounds()/print() give the
// SAME answers as the physical device, not an approximation. The only
// genuinely custom piece is FakeDisplay's draw_pixel_at (fake_display.h),
// which writes into an in-memory buffer instead of driving SPI.
//
// What's intentionally left out: ESPHome's Component/PollingComponent
// scheduling base classes, automation/trigger machinery, and the LVGL-font
// code path (#ifdef USE_LVGL_FONT, never defined here) - none of that is
// reachable from draw_page_detail/draw_page_flow.

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <algorithm>

// Real macro is `__attribute__((hot))`; harmless but irrelevant on a host
// build done for correctness, not speed.
#define HOT
// Real font/icon data is `PROGMEM` (flash-resident) on the ESP8266; on a
// host build there's no separate flash address space, so this is empty
// rather than reimplemented.
#define PROGMEM
#define ESP_LOGW(tag, ...) ((void) 0)

inline void yield() {}  // Arduino global, called at the top of every page function.

namespace esphome {

// --- Color -------------------------------------------------------------
// Subset of esphome/core/color.h: dashboard_draw.h/dashboard_pages.h only
// ever construct and pass these around, never use its arithmetic operators.
struct Color {
  // Same red/r etc. dual-naming as the real struct (there via an anonymous
  // union; dashboard_draw.h's lighten() uses the long names, dashboard_
  // pages.h uses the short ones).
  union { uint8_t r; uint8_t red; };
  union { uint8_t g; uint8_t green; };
  union { uint8_t b; uint8_t blue; };
  union { uint8_t w; uint8_t white; };
  constexpr Color() : r(0), g(0), b(0), w(0) {}
  constexpr Color(uint8_t red_, uint8_t green_, uint8_t blue_) : r(red_), g(green_), b(blue_), w(0) {}
  constexpr Color(uint8_t red_, uint8_t green_, uint8_t blue_, uint8_t white_)
      : r(red_), g(green_), b(blue_), w(white_) {}
  static const Color BLACK;
  static const Color WHITE;
};
inline constexpr Color Color::BLACK{0, 0, 0, 0};
inline constexpr Color Color::WHITE{255, 255, 255, 255};

// Verbatim from esphome/core/helpers.{h,cpp} - standard RGB<->HSV, used by
// dashboard_draw.h's lighten() for the sparkline fill and flow-bar tails.
inline void rgb_to_hsv(float red, float green, float blue, int &hue, float &saturation, float &value) {
  float max_color_value = std::max({red, green, blue});
  float min_color_value = std::min({red, green, blue});
  float delta = max_color_value - min_color_value;

  if (delta == 0) {
    hue = 0;
  } else if (max_color_value == red) {
    hue = int(fmodf((60.0f * ((green - blue) / delta)) + 360.0f, 360.0f));
  } else if (max_color_value == green) {
    hue = int(fmodf((60.0f * ((blue - red) / delta)) + 120.0f, 360.0f));
  } else if (max_color_value == blue) {
    hue = int(fmodf((60.0f * ((red - green) / delta)) + 240.0f, 360.0f));
  }

  saturation = (max_color_value == 0) ? 0 : delta / max_color_value;
  value = max_color_value;
}

inline void hsv_to_rgb(int hue, float saturation, float value, float &red, float &green, float &blue) {
  float chroma = value * saturation;
  float hue_prime = fmodf(hue / 60.0f, 6.0f);
  float intermediate = chroma * (1.0f - fabsf(fmodf(hue_prime, 2.0f) - 1.0f));
  float delta = value - chroma;

  if (0 <= hue_prime && hue_prime < 1) {
    red = chroma; green = intermediate; blue = 0;
  } else if (1 <= hue_prime && hue_prime < 2) {
    red = intermediate; green = chroma; blue = 0;
  } else if (2 <= hue_prime && hue_prime < 3) {
    red = 0; green = chroma; blue = intermediate;
  } else if (3 <= hue_prime && hue_prime < 4) {
    red = 0; green = intermediate; blue = chroma;
  } else if (4 <= hue_prime && hue_prime < 5) {
    red = intermediate; green = 0; blue = chroma;
  } else if (5 <= hue_prime && hue_prime < 6) {
    red = chroma; green = 0; blue = intermediate;
  } else {
    red = 0; green = 0; blue = 0;
  }

  red += delta; green += delta; blue += delta;
}

inline uint8_t progmem_read_byte(const uint8_t *addr) { return *addr; }  // no flash/RAM split on host

// --- ConstVector ---------------------------------------------------------
// Verbatim from esphome/core/helpers.h.
template<typename T> class ConstVector {
 public:
  constexpr ConstVector(const T *data, size_t size) : data_(data), size_(size) {}
  const constexpr T &operator[](size_t i) const { return data_[i]; }
  constexpr size_t size() const { return size_; }
  constexpr bool empty() const { return size_ == 0; }

 protected:
  const T *data_;
  size_t size_;
};

namespace display {

// Verbatim bit layout from esphome/components/display/display.h - the mask
// values in Display::get_text_bounds (fake_display.h) depend on these exact
// numbers, not just the combined names.
enum class TextAlign {
  TOP = 0x00,
  CENTER_VERTICAL = 0x01,
  BASELINE = 0x02,
  BOTTOM = 0x04,

  LEFT = 0x00,
  CENTER_HORIZONTAL = 0x08,
  RIGHT = 0x10,

  TOP_LEFT = TOP | LEFT,
  TOP_CENTER = TOP | CENTER_HORIZONTAL,
  TOP_RIGHT = TOP | RIGHT,

  CENTER_LEFT = CENTER_VERTICAL | LEFT,
  CENTER = CENTER_VERTICAL | CENTER_HORIZONTAL,
  CENTER_RIGHT = CENTER_VERTICAL | RIGHT,

  BASELINE_LEFT = BASELINE | LEFT,
  BASELINE_CENTER = BASELINE | CENTER_HORIZONTAL,
  BASELINE_RIGHT = BASELINE | RIGHT,

  BOTTOM_LEFT = BOTTOM | LEFT,
  BOTTOM_CENTER = BOTTOM | CENTER_HORIZONTAL,
  BOTTOM_RIGHT = BOTTOM | RIGHT,
};

class Display;

class BaseFont {
 public:
  virtual void print(int x, int y, Display *display, Color color, const char *text, Color background) = 0;
  virtual void measure(const char *str, int *width, int *x_offset, int *baseline, int *height) = 0;
  virtual ~BaseFont() = default;
};

inline constexpr Color COLOR_OFF(0, 0, 0, 0);
inline constexpr Color COLOR_ON(255, 255, 255, 255);

}  // namespace display

namespace font {

// Verbatim from esphome/components/font/font.h (LVGL member functions
// omitted - USE_LVGL_FONT is never defined here).
class Glyph final {
 public:
  constexpr Glyph(uint32_t code_point, const uint8_t *data, int advance, int offset_x, int offset_y, int width,
                  int height)
      : code_point(code_point), data(data), advance(advance), offset_x(offset_x), offset_y(offset_y), width(width),
        height(height) {}

  bool is_less_or_equal(uint32_t other) const { return this->code_point <= other; }

  const uint32_t code_point;
  const uint8_t *data;
  int advance;
  int offset_x;
  int offset_y;
  int width;
  int height;
};

class Font final : public display::BaseFont {
 public:
  Font(const Glyph *data, int data_nr, int baseline, int height, int descender, int xheight, int capheight,
       uint8_t bpp = 1);

  const Glyph *find_glyph(uint32_t codepoint) const;

  void print(int x_start, int y_start, display::Display *display, Color color, const char *text,
             Color background) override;
  void measure(const char *str, int *width, int *x_offset, int *baseline, int *height) override;

  inline int get_baseline() { return this->baseline_; }
  inline int get_height() { return this->height_; }
  inline int get_descender() { return this->descender_; }
  inline int get_xheight() { return this->xheight_; }
  inline int get_capheight() { return this->capheight_; }
  inline int get_bpp() { return this->bpp_; }

  const ConstVector<Glyph> &get_glyphs() const { return glyphs_; }

 protected:
  ConstVector<Glyph> glyphs_;
  int baseline_;
  int height_;
  int descender_;
  int linegap_;
  int xheight_;
  int capheight_;
  uint8_t bpp_;
};

// --- Font implementation (verbatim from font.cpp, minus LVGL) -----------

inline uint32_t extract_unicode_codepoint(const char *utf8_str, size_t *length) {
  const uint8_t *current = reinterpret_cast<const uint8_t *>(utf8_str);
  uint32_t code_point = 0;
  uint8_t c1 = *current++;

  if (c1 == 0) {
    *length = 0;
    return 0;
  }
  if (c1 < 0x80) {
    code_point = c1;
  } else if ((c1 & 0xE0) == 0xC0) {
    uint8_t c2 = *current++;
    if ((c2 & 0xC0) != 0x80) { *length = 0; return 0; }
    code_point = (c1 & 0x1F) << 6;
    code_point |= (c2 & 0x3F);
    if (code_point <= 0x7F) { *length = 0; return 0; }
  } else if ((c1 & 0xF0) == 0xE0) {
    uint8_t c2 = *current++;
    uint8_t c3 = *current++;
    if (((c2 & 0xC0) != 0x80) || ((c3 & 0xC0) != 0x80)) { *length = 0; return 0; }
    code_point = (c1 & 0x0F) << 12;
    code_point |= (c2 & 0x3F) << 6;
    code_point |= (c3 & 0x3F);
    if (code_point <= 0x7FF || (code_point >= 0xD800 && code_point <= 0xDFFF)) { *length = 0; return 0; }
  } else if ((c1 & 0xF8) == 0xF0) {
    uint8_t c2 = *current++;
    uint8_t c3 = *current++;
    uint8_t c4 = *current++;
    if (((c2 & 0xC0) != 0x80) || ((c3 & 0xC0) != 0x80) || ((c4 & 0xC0) != 0x80)) { *length = 0; return 0; }
    code_point = (c1 & 0x07) << 18;
    code_point |= (c2 & 0x3F) << 12;
    code_point |= (c3 & 0x3F) << 6;
    code_point |= (c4 & 0x3F);
    if (code_point <= 0xFFFF || code_point > 0x10FFFF) { *length = 0; return 0; }
  } else {
    *length = 0;
    return 0;
  }
  *length = current - reinterpret_cast<const uint8_t *>(utf8_str);
  return code_point;
}

inline Font::Font(const Glyph *data, int data_nr, int baseline, int height, int descender, int xheight,
                  int capheight, uint8_t bpp)
    : glyphs_(ConstVector<Glyph>(data, data_nr)),
      baseline_(baseline),
      height_(height),
      descender_(descender),
      linegap_(height - baseline - descender),
      xheight_(xheight),
      capheight_(capheight),
      bpp_(bpp) {}

inline const Glyph *Font::find_glyph(uint32_t codepoint) const {
  int lo = 0;
  int hi = (int) this->glyphs_.size() - 1;
  while (lo != hi) {
    int mid = (lo + hi + 1) / 2;
    if (this->glyphs_[mid].is_less_or_equal(codepoint)) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  auto *result = &this->glyphs_[lo];
  if (result->code_point == codepoint)
    return result;
  return nullptr;
}

inline void Font::measure(const char *str, int *width, int *x_offset, int *baseline, int *height) {
  *baseline = this->baseline_;
  *height = this->height_;
  int min_x = 0;
  bool has_char = false;
  int x = 0;
  for (;;) {
    size_t length;
    auto code_point = extract_unicode_codepoint(str, &length);
    if (length == 0)
      break;
    str += length;
    auto *glyph = this->find_glyph(code_point);
    if (glyph == nullptr) {
      if (!this->glyphs_.empty())
        x += this->glyphs_[0].advance;
      continue;
    }
    if (!has_char) {
      min_x = glyph->offset_x;
    } else {
      min_x = std::min(min_x, x + glyph->offset_x);
    }
    x += glyph->advance;
    has_char = true;
  }
  *x_offset = min_x;
  *width = x - min_x;
}

}  // namespace font
}  // namespace esphome
