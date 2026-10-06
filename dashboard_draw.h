#pragma once

#include "dashboard_view.h"

// Drawing shared between display pages.
//
// Deliberately separate from dashboard_view.h. The model is a plain C++ object
// with no ESPHome dependency, which is what lets it be compiled and tested on
// a host. This file cannot be - it uses display::Display, font::Font and Color,
// which only exist inside the generated main.cpp. ESPHome's includes: land well
// after the component headers there, so no #include of them is needed or
// possible; the trade is that a mistake in here is only caught by a full build.
//
// These take a Chart or explicit values rather than the whole DashboardView,
// so a page can render the 6h chart or the selected one through the same code.

inline esphome::Color lighten(esphome::Color c, float amount) {
  float r = c.red   / 255.0f;
  float g = c.green / 255.0f;
  float b = c.blue  / 255.0f;
  int h = 0;
  float s = 0.0f;
  float v = 0.0f;

  esphome::rgb_to_hsv(r, g, b, h, s, v);
  v = v * amount;
  if (v > 1.0f) v = 1.0f;
  if (v < 0.0f) v = 0.0f;

  esphome::hsv_to_rgb(h, s, v, r, g, b);
  return esphome::Color(
      (uint8_t)(r * 255.0f),
      (uint8_t)(g * 255.0f),
      (uint8_t)(b * 255.0f)
  );
}

// Sparkline with a title on the left and an optional peak reading on the right.
//
// Does NOT draw the 0W baseline - the caller does. The trace maps v=0 to
// exactly y1, so a rule the page draws at y1 serves as both the baseline and a
// section divider, instead of the chart drawing its own and landing a few
// pixels from the page's. The caller also chooses its width, which lets a
// full-bleed divider sit under an inset plot.
inline void draw_sparkline(esphome::display::Display &it,
                           const DashboardView::Chart &c,
                           int label_y, int x0, int y0, int x1, int y1,
                           esphome::Color trace, esphome::Color label,
                           esphome::display::BaseFont *label_font,
                           const char *title) {
    esphome::Color fill_color = lighten(trace, 0.35f);
    char buf[16];
    it.printf(x0, label_y, label_font, label, esphome::display::TextAlign::CENTER_LEFT,
              "%s", title);

    if (c.drawable) {
        fmt_power(c.peak_w, buf, sizeof(buf));
        it.printf(x1, label_y, label_font, label, esphome::display::TextAlign::CENTER_RIGHT, "peak %s", buf);
    }

    if (!c.drawable) {
        it.printf((x0 + x1) / 2, (y0 + y1) / 2, label_font, label, esphome::display::TextAlign::CENTER, "collecting data...");
        return;
    }

    int span = c.points - 1;
    for (int i = 0; i < span; i++) {
        float v1, v2;
        // sample() returns false for a bucket with no data. Skipping the segment
        // leaves an honest visual gap rather than a fake dip to a sentinel value.
        if (!c.window.sample(i, v1) || !c.window.sample(i + 1, v2)) continue;

        int px1 = x0 + (x1 - x0) * i / span;
        int px2 = x0 + (x1 - x0) * (i + 1) / span;
        int py1 = y1 - (int) ((v1 / c.scale_max_w) * (y1 - y0));
        int py2 = y1 - (int) ((v2 / c.scale_max_w) * (y1 - y0));

        // 1. Draw the soft fill under the line segment
        // We loop horizontally from px1 to px2 and draw vertical lines down to the bottom (y1)
        for (int x = px1; x <= px2; x++) {
            // Linearly interpolate the height between py1 and py2 for each pixel column
            int y_line = py1 + (py2 - py1) * (x - px1) / (px2 - px1);
            int height = y1 - y_line;
            it.vertical_line(x, y_line, height, fill_color);
        }

        // 2. Draw the main trace line on top of the fill
        it.line(px1, py1, px2, py2, trace);
    }
}

// One stacked flow bar. Segments are drawn in order and clipped to the bar;
// `scale_w` is shared between the two bars by the caller, so the shorter one
// ends short and the gap is the measurement disagreement made visible.
//
// A segment is labelled only if the label actually fits inside it, measured
// rather than estimated from a percentage of the bar. A fixed fraction has to
// be retuned every time the font changes, and gets it wrong in both
// directions: too low and a label spills across its neighbours, too high and
// a segment with plenty of room loses a number it could have shown.
struct FlowSegment {
  float watts;
  esphome::Color color;
};

inline void draw_flow_bar(esphome::display::Display &it,
                          const FlowSegment *segments, int n,
                          float scale_w,
                          int x0, int y, int x1, int h,
                          esphome::Color tail, esphome::Color text,
                          esphome::display::BaseFont *bar_font) {
  int full = x1 - x0;
  int cursor = x0;
  for (int i = 0; i < n; i++) {
    if (segments[i].watts <= 0.0f) continue;
    int w = (int) ((segments[i].watts / scale_w) * full);
    if (w <= 0) continue;
    if (cursor + w > x1) w = x1 - cursor;
    if (w <= 0) break;

    it.filled_rectangle(cursor, y, w, h, lighten(segments[i].color, 0.65f));
    it.filled_rectangle(cursor, y, w, 3, segments[i].color);  // top accent

    char buf[12];
    snprintf(buf, sizeof(buf), "%.0f", segments[i].watts);
    int tx, ty, tw, th;
    it.get_text_bounds(0, 0, buf, bar_font, esphome::display::TextAlign::TOP_LEFT,
                       &tx, &ty, &tw, &th);
    if (tw + 4 <= w) {
      it.printf(cursor + w / 2, y + 3 + (h - 3) / 2, bar_font, text,
                esphome::display::TextAlign::CENTER, "%s", buf);
    }
    cursor += w;
  }
  if (cursor < x1) {
    int w = x1 - cursor;
    it.filled_rectangle(cursor, y, w, h, lighten(tail, 0.35f));
    it.filled_rectangle(cursor, y, w, 3, tail);
  }
}
