#pragma once

#include "dashboard_draw.h"
#include "mdi_icons.h"

// The two display pages, as real functions instead of inline YAML lambdas.
//
// Same reasoning as dashboard_draw.h for why this can't be host-tested: it
// uses display::Display, the font/icon globals (font_10, icon_13, ...) and
// Color, none of which exist outside the generated main.cpp. Must be
// #include'd (via esphome: includes:) AFTER dashboard_draw.h and after
// ESPHome's own component globals, for the same reason - the font/icon
// objects referenced here by plain name (no id() - that's YAML-lambda sugar
// ESPHome strips to the bare identifier at codegen time; it doesn't exist as
// a construct in a plain .h file) must already be declared.
//
// MDI_* constants come from mdi_icons.h, not ${mdi_xxx} YAML substitutions:
// substitution only resolves in content ESPHome loads as YAML, and this
// file's text is copied into the build untouched - a placeholder in here
// would just be invalid, literal text. See mdi_icons.h and
// tools/mdi_codepoints.py.

// Detail page: stacked readouts (solar/house/battery/grid), a sparkline and
// today's totals. The original single-page layout, unchanged in content.
// Its sparkline goes through draw_sparkline (dashboard_draw.h) so a fix
// lands on both pages at once.
inline void draw_page_detail(esphome::display::Display &it, const DashboardView &v) {
  // First line of every page function: without it, WiFi drops during this
  // page's SPI writes - see the wifi: on_connect comment in
  // givenergy_dashboard.yaml for the full story.
  yield();

  using esphome::Color;
  using esphome::display::TextAlign;

  // Colors
  auto yellow = Color(244, 208, 63);
  auto teal   = Color(26, 188, 156);
  auto green  = Color(46, 204, 113);
  auto red    = Color(231, 76, 60);
  auto white  = Color(255, 255, 255);
  auto grey   = Color(150, 150, 150);
  auto dgrey  = Color(40, 40, 40);
  auto blue   = Color(52, 152, 219);
  auto orange = Color(230, 126, 34);

  static char pbuf[20];

  it.fill(Color::BLACK);

  // --- Header ---
  it.printf(6, 11, font_10, grey, TextAlign::CENTER_LEFT, "GivEnergy AIO");

  // Freshness readout. The model carries the reading's epoch rather than
  // an age, so choosing between "45s ago" and a clock time is a decision
  // made here. Falls back to time since the last fetch when the clock has
  // not synced, since an epoch cannot render at all in that state.
  if (v.reading_epoch_known()) {
    it.printf(213, 11, font_10, grey, TextAlign::CENTER_RIGHT,
              "%us ago", v.reading_age_s());
  } else {
    it.printf(213, 11, font_10, grey, TextAlign::CENTER_RIGHT,
              "%.0fs ago", v.fetch_age_ms() / 1000.0f);
  }

  // WiFi signal icon - plain rectangles (bars), not diagonals/curves,
  // since fine curved detail doesn't survive well at this resolution.
  // The red border below remains the primary disconnect indicator; this
  // is a secondary one that also hints at *why* it's not connected.
  {
    Color wifi_color;
    switch (v.link()) {
      case LinkState::AccessPoint: wifi_color = blue;   break;
      case LinkState::Connected:   wifi_color = green;  break;
      case LinkState::Connecting:  wifi_color = orange; break;
      default:                     wifi_color = dgrey;  break;
    }
    it.filled_rectangle(219, 12, 3, 4, wifi_color);
    it.filled_rectangle(224, 9, 3, 7, wifi_color);
    it.filled_rectangle(229, 6, 3, 10, wifi_color);
  }
  it.horizontal_line(6, 18, 228, dgrey);

  // Everything below this point is HA data, which is not being refreshed
  // while polling is off - drawing it would show stale numbers with no
  // indication they're frozen. The header and WiFi icon above are drawn
  // first deliberately, so the AP-fallback/connecting state stays visible
  // here.
  if (!v.show_data()) {
    it.print(120, 120, font_20, TextAlign::CENTER, "Polling Disabled");
    return;
  }

  // --- Live readings ---
  it.printf(8, 34, font_14, yellow, TextAlign::CENTER_LEFT, "Solar");
  fmt_power_full(v.solar_w(), pbuf, sizeof(pbuf));
  it.printf(232, 34, font_20, yellow, TextAlign::CENTER_RIGHT, "%s", pbuf);

  it.printf(8, 58, font_14, teal, TextAlign::CENTER_LEFT, "House");
  fmt_power_full(v.home_w(), pbuf, sizeof(pbuf));
  it.printf(232, 58, font_20, teal, TextAlign::CENTER_RIGHT, "%s", pbuf);

  // No arrow while idle. The inverter's own overhead keeps the reading off
  // zero at rest, so an arrow there would assert a direction that isn't
  // real - see IDLE_ENTER_W in dashboard_view.h.
  const char *batt_arrow = "";
  if (v.battery_state() == BatteryState::Charging)         batt_arrow = "↑";
  else if (v.battery_state() == BatteryState::Discharging) batt_arrow = "↓";
  it.printf(8, 82, font_14, green, TextAlign::CENTER_LEFT, "Batt");
  fmt_power_full(v.battery_abs_w(), pbuf, sizeof(pbuf));
  it.printf(232, 82, font_20, green, TextAlign::CENTER_RIGHT,
            "%.0f%% %s%s", v.battery_soc_pct(), batt_arrow, pbuf);

  it.printf(8, 106, font_14, red, TextAlign::CENTER_LEFT, "Grid");
  fmt_power_full(v.grid_abs_w(), pbuf, sizeof(pbuf));
  const char *grid_arrow = "";
  if (v.grid_state() == GridState::Importing)      grid_arrow = "→";
  else if (v.grid_state() == GridState::Exporting) grid_arrow = "←";
  it.printf(232, 106, font_20, red, TextAlign::CENTER_RIGHT,
            "%s%s", grid_arrow, pbuf);

  it.horizontal_line(6, 118, 228, dgrey);

  // Shared with the Flow page - see draw_sparkline in dashboard_draw.h.
  // Uses the chart the Sparkline Window select asks for. The 0W baseline
  // is drawn by the caller, not the helper, so a page can merge it with a
  // section divider (as the Flow page does) instead of ending up with two
  // rules a few pixels apart.
  draw_sparkline(it, v.chart_selected(), 128, 8, 138, 232, 172,
                 yellow, grey, font_10,
                 v.zoom_3h() ? "Solar - last 3h" : "Solar - last 6h");
  it.horizontal_line(8, 172, 224, dgrey);

  it.horizontal_line(6, 180, 228, dgrey);

  // --- Today's totals (kWh), colour-coded. Each value is short enough
  // (max ~2 digits + 1 decimal) to fit a single row of four.
  it.printf(120, 190, font_10, grey, TextAlign::CENTER, "Today (kWh)");
  it.printf(35,  208, font_16, yellow, TextAlign::CENTER, "%.1f", v.solar_today_kwh());
  it.printf(92,  208, font_16, blue,   TextAlign::CENTER, "%.1f", v.home_today_kwh());
  it.printf(148, 208, font_16, red,    TextAlign::CENTER, "%.1f", v.import_today_kwh());
  it.printf(206, 208, font_16, orange, TextAlign::CENTER, "%.1f", v.export_today_kwh());

  it.horizontal_line(6, 218, 228, dgrey);

  // --- Footer: inverter temp + current rate ---
  it.printf(8, 229, font_10, grey, TextAlign::CENTER_LEFT,
            "Inverter %.0fC", v.inverter_temp_c());
  it.printf(234, 229, font_10, grey, TextAlign::CENTER_RIGHT,
            "Rate %.1fp/kWh", v.rate_p_per_kwh());

  // --- Error state: WiFi down, or no successful HA fetch recently.
  // Both are model judgements, so a second page cannot disagree about
  // when the border goes red.
  if (!v.link_ok() || v.stale()) {
    for (int i = 0; i < 4; i++) {
      it.rectangle(i, i, 240 - 2 * i, 240 - 2 * i, red);
    }
  }
}

// One icon+value pair for the status strip's temp/rate readouts, right-
// aligned so pairs chain leftward from the screen edge without drifting
// apart if either value's width changes. Returns the x of the icon's own
// left edge, so the next pair over can anchor its right edge there minus
// a gap - same measure-then-place idiom as draw_flow_quad's unit suffix.
inline int draw_flow_header_readout(esphome::display::Display &it, int right_x, int y,
                                    const char *icon, const char *text, esphome::Color c) {
  using esphome::display::TextAlign;
  int tx, ty, tw, th;
  it.get_text_bounds(right_x, y, text, font_14, TextAlign::CENTER_RIGHT, &tx, &ty, &tw, &th);
  it.printf(right_x, y, font_14, c, TextAlign::CENTER_RIGHT, "%s", text);
  int icon_right_x = tx - 3;
  int itx, ity, itw, ith;
  it.get_text_bounds(icon_right_x, y, icon, icon_13, TextAlign::CENTER_RIGHT, &itx, &ity, &itw, &ith);
  it.printf(icon_right_x, y, icon_13, c, TextAlign::CENTER_RIGHT, "%s", icon);
  return itx;
}

// Mirror of draw_flow_header_readout for chaining rightward from a left
// edge instead of leftward from a right edge (the status strip's time/
// rate pairing grows this way, left of centre, while temp/rate on the
// right still grows the other way) - icon first, value after, both
// CENTER_LEFT. Returns the value's own right edge, so a caller chaining
// further right can anchor there plus a gap.
inline int draw_flow_header_readout_left(esphome::display::Display &it, int left_x, int y,
                                         const char *icon, const char *text, esphome::Color c) {
  using esphome::display::TextAlign;
  it.printf(left_x, y, icon_13, c, TextAlign::CENTER_LEFT, "%s", icon);
  int itx, ity, itw, ith;
  it.get_text_bounds(left_x, y, icon, icon_13, TextAlign::CENTER_LEFT, &itx, &ity, &itw, &ith);
  int text_x = itx + itw + 3;
  it.printf(text_x, y, font_14, c, TextAlign::CENTER_LEFT, "%s", text);
  int ttx, tty, ttw, tth;
  it.get_text_bounds(text_x, y, text, font_14, TextAlign::CENTER_LEFT, &ttx, &tty, &ttw, &tth);
  return ttx + ttw;
}

// Flow page status strip: wifi link icon, reading freshness (age or fetch
// age), rate and inverter temp (each with its own icon - font_14, up from
// font_9, the row grew taller specifically to make room for this), a
// divider, and a bar showing how overdue the current reading is (up to
// 15s, colour-graded amber/red as it edges past the expected ~10s
// cadence, and growing outward from the centre rather than left-to-right
// so it reads as a pulse under the divider rather than a loading bar).
// hair_c/chrome_c are passed in rather than redeclared here since
// draw_page_flow also uses them below the strip (dividers, bar totals) -
// one definition, no risk of drift.
//
// Draws starting at y (the icon row sits at y+10, the divider/age bar at
// y+21) and returns the height rendered, so a caller can reposition the
// whole strip - or stack something above/below it - by adjusting one
// number instead of every literal inside here.
//
// Time is font_14_bold - its own font (see fonts.yaml, this font
// component has no per-call style flag), just for the time, chosen over
// bolding the whole strip after comparing both via the harness: bolding
// everything read as heavier overall with no clear anchor, while bolding
// only time made it the strip's obvious anchor point without dragging in
// every other place font_14 is used (Detail page labels, Flow page's
// footer/sparkline title, Spend page's rate figure, the OTA screen).
inline int draw_flow_status_strip(esphome::display::Display &it, const DashboardView &v,
                                  int y, esphome::Color hair_c, esphome::Color chrome_c) {
  using esphome::Color;
  using esphome::display::TextAlign;

  int row_y = y + 10;

  // WiFi measured (not yet drawn - order doesn't matter for the pixels,
  // only for what's known when) before anything else: its corner spot is
  // fixed and excluded from the four-way split below, so its width has
  // to be known first to work out what's left to divide.
  Color wifi_c;
  switch (v.link()) {
    case LinkState::AccessPoint: wifi_c = Color(52, 152, 219); break;
    case LinkState::Connected:   wifi_c = Color(46, 204, 113);  break;
    case LinkState::Connecting:  wifi_c = Color(230, 126, 34);  break;
    default:                     wifi_c = hair_c;               break;
  }
  int wtx, wty, wtw, wth;
  it.get_text_bounds(238, row_y, MDI_WIFI, icon_13, TextAlign::CENTER_RIGHT, &wtx, &wty, &wtw, &wth);

  // The other four components (time, rate, age, temp) split whatever's
  // left of WiFi's spot into four equal shares - age centres in the
  // third share rather than on the screen's literal midpoint, since
  // WiFi's own footprint in the corner would otherwise skew that
  // midpoint off from where these four actually sit.
  const int CONTENT_LEFT = 2;
  int content_right = wtx - 6;
  int slot_w = (content_right - CONTENT_LEFT) / 4;
  int age_cx = CONTENT_LEFT + 2 * slot_w + slot_w / 2;

  // Left side: time, then rate chained after it - the two loosely relate
  // (rate is "what time-of-day period am I in"), and grouping them left
  // of centre balances temp+WiFi (both "status", not readings) on the
  // right of centre. Time omitted (not a stale/zeroed "00:00") when the
  // clock hasn't synced - there's no meaningful fallback for an absolute
  // time the way age has fetch_age_ms.
  int time_right_x = CONTENT_LEFT;
  if (v.clock_valid()) {
    it.printf(CONTENT_LEFT, row_y, font_14_bold, chrome_c, TextAlign::CENTER_LEFT,
              "%02u:%02u", v.hour(), v.minute());
    int ttx, tty, ttw, tth;
    it.get_text_bounds(CONTENT_LEFT, row_y, "00:00", font_14_bold, TextAlign::CENTER_LEFT, &ttx, &tty, &ttw, &tth);
    time_right_x = ttx + ttw;
  }
  char rate_buf[8];
  snprintf(rate_buf, sizeof(rate_buf), "%.1fp", v.rate_p_per_kwh());
  draw_flow_header_readout_left(it, time_right_x + 8, row_y, MDI_CASH, rate_buf, chrome_c);

  // Centre share: reading age - the one figure here that changes every
  // second, deliberately centred (in its own share, see above) so it
  // sits directly above the age bar's own centre-out pulse below
  // (divider_y, further down) rather than off to one side of it.
  if (v.reading_epoch_known()) {
    it.printf(age_cx, row_y, font_14, chrome_c, TextAlign::CENTER,
              "%us", v.reading_age_s());
  } else {
    it.printf(age_cx, row_y, font_14, chrome_c, TextAlign::CENTER,
              "%.0fs", v.fetch_age_ms() / 1000.0f);
  }

  // Right side: WiFi in the corner (drawn now, alongside temp chained
  // inward from it) - both are "status" rather than a reading, same
  // reasoning that groups them together on this side. WiFi in the corner
  // rather than leading the strip also matches the usual phone-status-bar
  // convention (connectivity is secondary, usually green and ignorable).
  it.printf(238, row_y, icon_13, wifi_c, TextAlign::CENTER_RIGHT, "%s", MDI_WIFI);

  char temp_buf[8];
  snprintf(temp_buf, sizeof(temp_buf), "%.0f°C", v.inverter_temp_c());
  draw_flow_header_readout(it, wtx - 6, row_y, MDI_THERMOMETER, temp_buf, chrome_c);

  int divider_y = y + 21;
  it.horizontal_line(0, divider_y, 240, hair_c);
  if (v.reading_epoch_known()) {
    int reading_age_s = v.reading_age_s();
    // we expect a reading every 10 seconds.
    // overlay a bar over the hori-line representing up to 15 seconds to allow for late readings
    Color age_bar_c = Color(84, 102, 128);
    if (reading_age_s >= 15) { // late
      age_bar_c = Color(231, 76, 60);
      reading_age_s = 15;
    }
    else if(reading_age_s >= 12) age_bar_c = Color(230, 126, 34); // overdue
    else if(reading_age_s >= 8) age_bar_c = Color(194, 178, 63); // soon
    int half_w = (int) (120.0 * reading_age_s / 15);
    it.filled_rectangle(120 - half_w, divider_y, half_w * 2, 1, age_bar_c);
  }
  return divider_y + 1 - y;
}

// One Flow-page quadrant: an icon, the formatted value+unit anchored to the
// outer edge, and an optional directional arrow pointing toward (or away
// from) the house. `left` mirrors the whole layout - icon/arrow order and
// text alignment both flip - since the unit is positioned by measuring the
// value's width rather than assuming one (mono digits keep the advance
// fixed, but the string itself is 3 or 4 characters depending on magnitude).
//
// `y` is the row's vertical center (everything here is CENTER/BASELINE
// aligned around it, not top-aligned).
inline void draw_flow_quad(esphome::display::Display &it, bool left, int y,
                           const char *icon, esphome::Color c,
                           const char *arrow, float watts) {
  using esphome::display::TextAlign;
  char vbuf[8], ubuf[4];
  fmt_flow(watts, vbuf, sizeof(vbuf), ubuf, sizeof(ubuf));
  int base = y + 7;
  int tx, ty, tw, th;
  if (left) {
    it.printf(2, y, icon_24, c, TextAlign::CENTER_LEFT, "%s", icon);
    it.printf(28, base, font_mono_20, c, TextAlign::BASELINE_LEFT,
              "%s", vbuf);
    it.get_text_bounds(28, base, vbuf, font_mono_20,
                       TextAlign::BASELINE_LEFT, &tx, &ty, &tw, &th);
    it.printf(28 + tw + 2, base, font_9, c,
              TextAlign::BASELINE_LEFT, "%s", ubuf);
    if (arrow != nullptr)
      it.printf(110, y, icon_19, c, TextAlign::CENTER_RIGHT, "%s", arrow);
  } else {
    it.printf(238, y, icon_24, c, TextAlign::CENTER_RIGHT, "%s", icon);
    it.printf(210, base, font_9, c, TextAlign::BASELINE_RIGHT,
              "%s", ubuf);
    it.get_text_bounds(210, base, ubuf, font_9,
                       TextAlign::BASELINE_RIGHT, &tx, &ty, &tw, &th);
    it.printf(210 - tw - 2, base, font_mono_20, c,
              TextAlign::BASELINE_RIGHT, "%s", vbuf);
    if (arrow != nullptr)
      it.printf(129, y, icon_19, c, TextAlign::CENTER_LEFT, "%s", arrow);
  }
}

// Flow page footer: today's four totals (solar/house/import/export) as
// icon+value pairs across the bottom row, vertically centered in [y0, y1)
// - the leftover space below the last divider - rather than pinned to a
// fixed offset. Colours are passed in rather than recomputed so they can
// never drift from the matching quadrant/bar above - export_c in
// particular is shared with draw_flow_bars' consumption segment, the same
// orange in both places. Returns the taller of the two fonts' measured
// heights, for a caller that wants to know roughly how much of the band
// this used - it doesn't feed the row's own positioning any more (see
// below).
inline int draw_flow_today_totals(esphome::display::Display &it, const DashboardView &v,
                                  esphome::Color solar_c, esphome::Color load_c,
                                  esphome::Color grid_c, esphome::Color export_c,
                                  int y0, int y1) {
  using esphome::display::TextAlign;
  const int cx[4] = {30, 90, 150, 210};
  const char *icons[4] = {MDI_WEATHER_SUNNY, MDI_POWER_PLUG,
                          MDI_TRANSMISSION_TOWER_IMPORT,
                          MDI_TRANSMISSION_TOWER_EXPORT};
  const esphome::Color cols[4] = {solar_c, load_c, grid_c, export_c};
  const float vals[4] = {v.solar_today_kwh(), v.home_today_kwh(),
                         v.import_today_kwh(), v.export_today_kwh()};

  // Used to be top-aligned to a shared y computed from these two - in
  // theory a shared top edge reads as one row, but on the physical
  // screen the icon sat visibly higher than the numerals: icon fonts and
  // text fonts don't carry the same top-side-bearing, so "same nominal
  // top" is not "same visual top". CENTER_LEFT/CENTER_RIGHT anchored on
  // one shared y (same idiom draw_flow_header_readout above already uses
  // for the same icon+value pairing) lines them up by where the ink
  // actually sits instead of by font metrics.
  int tx, ty, tw, icon_h, font_h;
  it.get_text_bounds(0, 0, MDI_WEATHER_SUNNY, icon_13, TextAlign::TOP_LEFT, &tx, &ty, &tw, &icon_h);
  it.get_text_bounds(0, 0, "0.0", font_14, TextAlign::TOP_LEFT, &tx, &ty, &tw, &font_h);
  int content_h = icon_h > font_h ? icon_h : font_h;
  int center_y = (y0 + y1) / 2;

  for (int i = 0; i < 4; i++) {
    it.printf(cx[i] - 22, center_y, icon_13, cols[i],
              TextAlign::CENTER_LEFT, "%s", icons[i]);
    it.printf(cx[i] + 24, center_y, font_14, cols[i],
              TextAlign::CENTER_RIGHT, "%.1f", vals[i]);
  }
  return content_h;
}

// Flow page quadrant block: four metered quantities in a 2x2 grid around a
// house glyph, with diagonal arrows pointing toward the house for energy
// entering the property and away for energy leaving it, so direction reads
// without parsing the number. Solar and load have fixed directions;
// battery and grid follow the model's hysteresis-banded state. An idle
// corner draws no arrow at all - an absent arrow reads as "nothing
// happening" at a glance, whereas a 0W arrow has to be read.
//
// Does NOT draw the SoC readout under the house - see draw_flow_battery_soc
// below, factored out separately since it's a state rather than a flow and
// doesn't fit the icon/watts/direction grammar the corners use.
//
// y is the top of the block; the corners (y+21), hub icon (y+35, sized
// from its ink box rather than its em box - icon fonts carry 10-15% side
// bearing, and the house is bottom-heavy so its optical centre sits above
// its geometric one) and second row (y+53) sit at fixed offsets from it -
// unchanged from before this was factored out, not rederived, since
// retuning them isn't safe without eyes on the physical screen.
//
// Returns 66, the height of what THIS drew (the second row's ink bottom)
// and nothing past it. It used to return 87, which was actually "second
// row bottom, plus room for the SoC line below it, plus the gap before
// the divider after that" - none of which is this component's content.
// Whatever sits below (SoC line, a divider, anything else) is the caller's
// layout decision, not baked in here.
//
// 66 is a measured constant (logged from the device, not calculated: 53 +
// half of font_mono_20's runtime ink height), not derived at draw time -
// simpler, and font_mono_20 isn't expected to change. If it ever does
// (different font, different size), re-measure rather than guess: a
// throwaway ESP_LOGI of the old dynamic calculation is what produced this
// number.
inline int draw_flow_quadrants(esphome::display::Display &it, const DashboardView &v, int y,
                               esphome::Color solar_c, esphome::Color load_c,
                               esphome::Color batt_c, esphome::Color grid_c,
                               esphome::Color solar_idle_c, esphome::Color load_idle_c,
                               esphome::Color grid_idle_c, esphome::Color batt_idle_c,
                               esphome::Color hub_c) {
  using esphome::Color;
  using esphome::display::TextAlign;

  bool solar_idle = v.solar_w() < DashboardView::IDLE_ENTER_W;
  bool load_idle  = v.home_w() < DashboardView::IDLE_ENTER_W;
  draw_flow_quad(it, true,  y + 21, MDI_WEATHER_SUNNY,
                 solar_idle ? solar_idle_c : solar_c,
                 solar_idle ? nullptr : MDI_ARROW_BOTTOM_RIGHT, v.solar_w());
  draw_flow_quad(it, false, y + 21, MDI_POWER_PLUG,
                 load_idle ? load_idle_c : load_c,
                 load_idle ? nullptr : MDI_ARROW_TOP_RIGHT, v.home_w());

  // Battery glyph carries coarse SoC, so the corner still says
  // something while idle and the hub number has a redundant cue.
  const char *batt_icon = MDI_BATTERY_OUTLINE;
  if (v.battery_soc_pct() >= 98)      batt_icon = MDI_BATTERY;
  else if (v.battery_soc_pct() >= 70) batt_icon = MDI_BATTERY_HIGH;
  else if (v.battery_soc_pct() >= 40) batt_icon = MDI_BATTERY_MEDIUM;
  else if (v.battery_soc_pct() >= 10) batt_icon = MDI_BATTERY_LOW;

  const char *batt_arrow = nullptr;
  Color batt_draw_c = batt_idle_c;
  if (v.battery_state() == BatteryState::Discharging) {
    batt_arrow = MDI_ARROW_TOP_RIGHT;   // inward: into the house
    batt_draw_c = batt_c;
  } else if (v.battery_state() == BatteryState::Charging) {
    batt_arrow = MDI_ARROW_BOTTOM_LEFT;  // outward: out of the house
    batt_draw_c = batt_c;
  }
  draw_flow_quad(it, true, y + 53, batt_icon, batt_draw_c, batt_arrow, v.battery_abs_w());

  // Geometric mapping: the grid is the bottom-right corner, so importing
  // points up-left toward the house and exporting points down-right away
  // from it. If the panel shows these reversed, the fault is the glyph
  // rather than the logic - see the note on the substitutions file.
  const char *grid_arrow = nullptr;
  Color grid_draw_c = grid_idle_c;
  if (v.grid_state() == GridState::Importing) {
    grid_arrow = MDI_ARROW_TOP_LEFT;
    grid_draw_c = grid_c;
  } else if (v.grid_state() == GridState::Exporting) {
    grid_arrow = MDI_ARROW_BOTTOM_RIGHT;
    grid_draw_c = grid_c;
  }
  draw_flow_quad(it, false, y + 53, MDI_TRANSMISSION_TOWER,
                 grid_draw_c, grid_arrow, v.grid_abs_w());

  it.printf(120, y + 35, icon_34, hub_c, TextAlign::CENTER, "%s", MDI_HOME_OUTLINE);

  return 66;
}

// Battery SoC readout under the house glyph. Sits under the house rather
// than in the battery corner because it is the only value on the page
// that is a state rather than a flow, and it does not fit the
// icon/watts/direction grammar the quadrants use - drawn as its own
// component even though it shares the quadrant block's visual footprint.
// font_16 rather than larger because the battery quad's arrow spans
// x 129-145 and this spans roughly 102-138 - they overlap horizontally,
// so only vertical clearance (the caller's choice of y) keeps them apart.
//
// y is the TOP of the text, like every other section here - TOP_CENTER
// rather than the plain CENTER every other draw call in this component
// uses, so this composes into a top-down layout the same way they do
// instead of needing its own vertical-centering math at the call site.
//
// Returns 19, font_16's measured ink height (logged from the device, not
// calculated) - a fixed constant rather than a per-call measurement,
// since font_16 isn't expected to change. Re-measure if it ever does.
inline int draw_flow_battery_soc(esphome::display::Display &it, const DashboardView &v,
                                 int y, esphome::Color batt_c) {
  it.printf(120, y, font_16, batt_c, esphome::display::TextAlign::TOP_CENTER,
            "%.0f%%", v.battery_soc_pct());
  return 19;
}

// Flow page generation/consumption bars: two stacked segmented bars
// (draw_flow_bar above) sharing one x-scale, each with its total wattage
// printed to the right. Stacked with a shared edge rather than a gap -
// comparing the two lengths is the bars' whole job, and a common baseline
// makes that easier to read than two separated objects. export_c is
// passed in rather than declared here (and reused by
// draw_flow_today_totals) so the export segment's colour can't drift from
// the footer's.
//
// y is the top of the first bar; the second bar (y+21) and both totals
// (y+10, y+31) sit at fixed offsets from it - unchanged from before this
// was factored out, not rederived (see draw_flow_quadrants above for
// why). Returns 41 - the second bar's own bottom edge (21 + its 20px
// height) - not the gap before whatever the caller draws next.
inline int draw_flow_bars(esphome::display::Display &it, const DashboardView &v, int y,
                          esphome::Color solar_c, esphome::Color load_c,
                          esphome::Color batt_c, esphome::Color grid_c,
                          esphome::Color export_c, esphome::Color hair_c,
                          esphome::Color chrome_c) {
  using esphome::Color;
  using esphome::display::TextAlign;
  char tbuf[16];

  FlowSegment gen[3] = {{v.solar_w(), solar_c},
                        {v.discharge_w(), batt_c},
                        {v.import_w(), grid_c}};
  FlowSegment con[3] = {{v.home_w(), load_c},
                        {v.charge_w(), batt_c},
                        {v.export_w(), export_c}};
  // Shared active width, computed once for the pair. This is what
  // keeps a quiet house from filling the widget with a 30W trickle -
  // it does NOT feed the scale below, which is deliberately never
  // floored: flooring the scale as well was the earlier bug, where
  // a total below the floor left both bars short and the floor's
  // own headroom read as a second, spurious mismatch tail on the
  // bar that actually matched.
  int bar_x1 = 2 + (int) (182 * v.flow_active_frac());
  draw_flow_bar(it, gen, 3, v.flow_scale_w(),
                2, y, bar_x1, 20, hair_c, Color::BLACK, font_16);
  fmt_power(v.generation_w(), tbuf, sizeof(tbuf));
  it.printf(238, y + 10, font_16, chrome_c, TextAlign::CENTER_RIGHT,
            "%s", tbuf);
  draw_flow_bar(it, con, 3, v.flow_scale_w(),
                2, y + 21, bar_x1, 20, hair_c, Color::BLACK, font_16);
  fmt_power(v.consumption_w(), tbuf, sizeof(tbuf));
  it.printf(238, y + 31, font_16, chrome_c, TextAlign::CENTER_RIGHT,
            "%s", tbuf);

  return 41;
}

// Flow page: the same model in a spatial layout - see draw_flow_quadrants
// and draw_flow_battery_soc above for the corners/hub and the SoC readout.
inline void draw_page_flow(esphome::display::Display &it, const DashboardView &v) {
  // yield() first, same WiFi-stability reason as draw_page_detail.
  yield();

  using esphome::Color;
  using esphome::display::TextAlign;

  auto solar_c  = Color(242, 193, 78);
  auto load_c   = Color(79, 195, 247);
  auto batt_c   = Color(95, 214, 138);
  auto grid_c   = Color(255, 107, 107);
  auto hub_c    = Color(200, 208, 216);
  auto chrome_c = Color(126, 138, 153);
  auto hair_c   = Color(42, 51, 64);
  auto batt_idle_c = Color(49, 84, 63);
  // Muted variants for the idle state. Nothing reflows when a corner
  // goes idle - only the colour changes and the arrow disappears.
  auto solar_idle_c = Color(97, 77, 31);
  auto load_idle_c  = Color(32, 78, 99);
  auto grid_idle_c  = Color(102, 43, 43);
  // Shared between the consumption bar's export segment and the footer's
  // export total, so they can't end up two different shades of orange.
  auto export_c = Color(230, 126, 34);

  it.fill(Color::BLACK);

  // --- Layout -----------------------------------------------------------
  // Every section below draws starting at `y` and returns the height it
  // actually rendered; the gaps between sections are explicit constants
  // right here rather than baked into any one section's return value, so
  // reordering, removing, or inserting a section is just moving, deleting,
  // or adding a block - the sections don't know about each other.
  //
  // Row 2's ink runs to about y=81 relative to the quadrant block's own
  // start (quad_h=66 is 53 + half of font_mono_20's 26px ink height); the
  // old CENTER-aligned SoC line's ink started about 2.5px before that.
  // The original hand-tuned layout was never really a clean
  // non-overlapping stack, it just didn't need to be, since CENTER-
  // aligned glyphs carry whitespace around their ink. QUAD_TO_SOC_GAP
  // being negative reclaims that same overlap deliberately.
  //
  // DIVIDER_TO_QUAD_GAP is negative for a different reason: it moves the
  // whole quadrant block up independent of the header, so the gap
  // between the header's divider and row 1 can be tuned without pulling
  // apart row 1/hub/row 2's spacing relative to each other (a rhythm
  // tuned as one unit). Every constant here shifts the ones after it,
  // so growing this one (making it less negative) borrows from whatever
  // else changes below it in this same layout pass.
  //
  // All four constants, and the resulting above/below split at each
  // divider, were tuned by measuring rendered pixels via
  // tools/render_preview (see its header comment for how), not by font
  // metrics. Quads/divider2/bars were then shifted down 2px as one group
  // (DIVIDER_TO_QUAD_GAP up, BARS_TO_DIVIDER_GAP down, by 2 each so
  // divider1 and divider3 - fixed points on either side of the group -
  // don't move) because divider3's above-gap (6px) read as noticeably
  // more than divider2's below-gap (4px), i.e. the bars sat closer to
  // the divider above them than the one below. Current split: divider1
  // 3px/8px, divider2 4px/4px, divider3 4px/4px against the sparkline
  // title (fixed, outside this cursor).
  const int DIVIDER_TO_QUAD_GAP = -4;
  const int QUAD_TO_SOC_GAP = -3;
  const int SOC_TO_DIVIDER_GAP = 0;
  const int DIVIDER_TO_BARS_GAP = 4;
  const int BARS_TO_DIVIDER_GAP = 3;

  int y = draw_flow_status_strip(it, v, 0, hair_c, chrome_c);

  if (!v.show_data()) {
    it.print(120, 120, font_20, TextAlign::CENTER, "Polling Disabled");
    return;
  }

  int quad_y = y + DIVIDER_TO_QUAD_GAP;
  int quad_h = draw_flow_quadrants(it, v, quad_y, solar_c, load_c, batt_c, grid_c,
                                   solar_idle_c, load_idle_c, grid_idle_c,
                                   batt_idle_c, hub_c);
  y = quad_y + quad_h + QUAD_TO_SOC_GAP;

  // SoC in battery green: the colour is what ties it to the corner.
  int soc_h = draw_flow_battery_soc(it, v, y, batt_c);
  y += soc_h + SOC_TO_DIVIDER_GAP;

  it.horizontal_line(0, y, 240, hair_c);
  y += DIVIDER_TO_BARS_GAP;

  int bars_h = draw_flow_bars(it, v, y, solar_c, load_c, batt_c, grid_c,
                              export_c, hair_c, chrome_c);
  y += bars_h + BARS_TO_DIVIDER_GAP;

  it.horizontal_line(0, y, 240, hair_c);

  // --- Sparkline: always the full 6h span on this page, regardless of
  // the Sparkline Window select, which belongs to the Detail page. Fixed
  // geometry, deliberately not part of the cursor above - it lives in the
  // page's lower third alongside the footer, not stacked below the bars.
  //
  // The rule below is both the chart's 0W baseline and the divider
  // above the totals row. The trace maps 0W to exactly y=212, so one
  // full-bleed line does both jobs and there is no pair of rules a few
  // pixels apart.
  draw_sparkline(it, v.chart_6h(), 159, 4, 168, 236, 212,
                 solar_c, chrome_c, font_14, "6h");
  it.horizontal_line(0, 212, 240, hair_c);

  // --- Today totals -------------------------------------------------
  // 212 is the divider just drawn above; 240 is the screen bottom.
  draw_flow_today_totals(it, v, solar_c, load_c, grid_c, export_c, 212, 240);
}

// Spend page summary block: a small "Today" label over import/export
// (font_16, left) and a small live rate over a bold net total (right).
// Replaces an earlier three-column "totals row" plus a separate "Now"
// hero row - collapsed into one block, then re-anchored bottom-up so
// export and net share one literal baseline instead of merely a matched
// centre (a centre match looks aligned only when both fonts have similar
// proportions - font_16 next to net_font's much larger, bolder glyphs
// visibly didn't), and every other line packs tight against its
// neighbour instead of spreading out to fill whatever height the caller
// reserved.
//
// Built entirely bottom-up: every element's height is measured FIRST
// (font metrics only - none of this depends on which digits are actually
// drawn, so it can all happen before anything is drawn), which fixes the
// block's total height, and everything is then placed working upward
// from baseline_y = y + block_h. That is what makes "sits on the bottom
// of the component" literal rather than approximate: the values that
// need to line up (export, net) are drawn with BASELINE alignment at the
// exact same y, not CENTER-matched at their respective midpoints.
//
// Left column rows keep the icon CENTER_LEFT + value BASELINE_RIGHT
// idiom from draw_flow_quad (icon centred on the value's own ink, not on
// its font's nominal em box - text and icon fonts don't share a
// top/bottom-side-bearing, so centring on the actual ink is what keeps
// them looking aligned regardless of font).
//
// Right column: live rate is font_14+font_10 (small, a secondary readout
// now that net is the headline, also BASELINE-paired with each other
// rather than CENTER-paired, for the same reason), pulled down to sit
// tight above net rather than floating near the top - on request, prefer
// one dense block over parts spread across the reserved space. net uses
// net_font - font_24_bold, the caller's choice (its own font, see
// fonts.yaml, since this font component has no separate style flag),
// picked by comparing 20/22/24pt renders side by side; worst case
// ("-£99.99", unlikely but checked) still clears the left column by 23px
// at 24pt.
//
// Direction (the rate row's colour) comes from grid_state(), not
// live_balance_gbp_per_h()'s own sign: import_w()/export_w() are RAW
// magnitudes, so a few idle watts of grid noise can flip the sign while
// the quads below still show idle - see the removed draw_spend_now, which
// first hit this rendering the harness's near-idle demo frame.
//
// y is the block's top. Returns block_h, the tight bottom-up height -
// smaller than what earlier versions of this block used, on purpose: the
// caller reserves exactly this much and lets whatever's above (the
// sparkline) claim the rest, rather than this block padding itself out
// to fill a fixed allowance.
inline int draw_spend_summary(esphome::display::Display &it, const DashboardView &v, int y,
                              esphome::Color import_c, esphome::Color export_c,
                              esphome::Color earn_c, esphome::Color spend_c,
                              esphome::Color idle_c, esphome::Color label_c,
                              esphome::display::BaseFont *net_font) {
  using esphome::display::TextAlign;
  char buf[12];

  // --- Pass 1: measure every element's own height, nothing drawn yet --
  const int LABEL_TO_ROWS_GAP = 2;
  const int ROW_GAP = 2;
  const int NET_TO_RATE_GAP = 2;

  int tx, ty, tw, label_h, icon_h, val_h, rate_h, net_h;
  it.get_text_bounds(0, 0, "Today", font_10, TextAlign::TOP_LEFT, &tx, &ty, &tw, &label_h);
  it.get_text_bounds(0, 0, MDI_TRANSMISSION_TOWER_IMPORT, icon_13, TextAlign::TOP_LEFT, &tx, &ty, &tw, &icon_h);
  it.get_text_bounds(0, 0, "£0.00", font_16, TextAlign::TOP_LEFT, &tx, &ty, &tw, &val_h);
  it.get_text_bounds(0, 0, "0.0", font_14, TextAlign::TOP_LEFT, &tx, &ty, &tw, &rate_h);
  it.get_text_bounds(0, 0, "£0.00", net_font, TextAlign::TOP_LEFT, &tx, &ty, &tw, &net_h);
  int row_h = icon_h > val_h ? icon_h : val_h;

  int left_h = label_h + LABEL_TO_ROWS_GAP + row_h + ROW_GAP + row_h;
  int right_h = rate_h + NET_TO_RATE_GAP + net_h;
  int block_h = left_h > right_h ? left_h : right_h;
  int baseline_y = y + block_h;

  // --- Pass 2: draw right column bottom-up (net, then rate above it) -
  bool net_pos = v.net_today_gbp() >= 0.0f;
  esphome::Color net_c = net_pos ? earn_c : spend_c;
  fmt_gbp(v.net_today_gbp(), buf, sizeof(buf));
  it.printf(232, baseline_y, net_font, net_c, TextAlign::BASELINE_RIGHT, "%s", buf);

  GridState gs = v.grid_state();
  esphome::Color rate_c = gs == GridState::Exporting ? earn_c
                         : gs == GridState::Importing ? spend_c
                         : idle_c;
  float mag_p = fabsf(v.live_balance_gbp_per_h()) * 100.0f;
  int rate_baseline_y = baseline_y - net_h - NET_TO_RATE_GAP;
  int rtx, rty, rtw, rth;
  it.printf(232, rate_baseline_y, font_10, rate_c, TextAlign::BASELINE_RIGHT, "p/h");
  it.get_text_bounds(232, rate_baseline_y, "p/h", font_10, TextAlign::BASELINE_RIGHT, &rtx, &rty, &rtw, &rth);
  it.printf(rtx - 2, rate_baseline_y, font_14, rate_c, TextAlign::BASELINE_RIGHT, "%.1f", mag_p);

  // --- Pass 3: draw left column bottom-up (export, import, label) ----
  int export_baseline_y = baseline_y;  // shares net's baseline - the point
  fmt_gbp(v.export_income_today_gbp(), buf, sizeof(buf));
  it.printf(112, export_baseline_y, font_16, export_c, TextAlign::BASELINE_RIGHT, "%s", buf);
  int etx, ety, etw, eth;
  it.get_text_bounds(112, export_baseline_y, buf, font_16, TextAlign::BASELINE_RIGHT, &etx, &ety, &etw, &eth);
  it.printf(8, ety + eth / 2, icon_13, export_c, TextAlign::CENTER_LEFT, "%s", MDI_TRANSMISSION_TOWER_EXPORT);

  int import_baseline_y = export_baseline_y - row_h - ROW_GAP;
  fmt_gbp(v.import_cost_today_gbp(), buf, sizeof(buf));
  it.printf(112, import_baseline_y, font_16, import_c, TextAlign::BASELINE_RIGHT, "%s", buf);
  int itx, ity, itw, ith;
  it.get_text_bounds(112, import_baseline_y, buf, font_16, TextAlign::BASELINE_RIGHT, &itx, &ity, &itw, &ith);
  it.printf(8, ity + ith / 2, icon_13, import_c, TextAlign::CENTER_LEFT, "%s", MDI_TRANSMISSION_TOWER_IMPORT);

  int label_y = (import_baseline_y - row_h) - LABEL_TO_ROWS_GAP - label_h;
  it.printf(8, label_y, font_10, label_c, TextAlign::TOP_LEFT, "Today");

  return block_h;
}

// Spend page: today's money picture - live spend/income rate, today's
// import/export/net totals, then (space permitting) the same flow
// quadrants and sparkline the Flow page uses, reused rather than
// reimplemented so the two pages can't disagree about what "importing" or
// "the 6h trend" looks like. Status strip is reused for the same reason -
// freshness/link/rate/temp are judgements every page should agree on, not
// redraw independently.
inline void draw_page_spend(esphome::display::Display &it, const DashboardView &v) {
  // yield() first, same WiFi-stability reason as the other pages.
  yield();

  using esphome::Color;
  using esphome::display::TextAlign;

  // Same palette as the Flow page for the quadrant block, so a value reads
  // the same colour on both pages. earn_c/spend_c match Detail page's
  // green/red exactly, for the same reason.
  auto solar_c  = Color(242, 193, 78);
  auto load_c   = Color(79, 195, 247);
  auto batt_c   = Color(95, 214, 138);
  auto grid_c   = Color(255, 107, 107);
  auto hub_c    = Color(200, 208, 216);
  auto chrome_c = Color(126, 138, 153);
  auto hair_c   = Color(42, 51, 64);
  auto batt_idle_c  = Color(49, 84, 63);
  auto solar_idle_c = Color(97, 77, 31);
  auto load_idle_c  = Color(32, 78, 99);
  auto grid_idle_c  = Color(102, 43, 43);
  auto export_c = Color(230, 126, 34);
  auto earn_c   = Color(46, 204, 113);
  auto spend_c  = Color(231, 76, 60);

  it.fill(Color::BLACK);

  // --- Layout -------------------------------------------------------
  // Same discipline as draw_page_flow: each section draws at a y it's
  // given and returns only the height it drew; gaps live here as named
  // constants. Initial values below, to be corrected against rendered
  // pixels via tools/render_preview rather than left as guesses - see
  // draw_page_flow's layout comment for why.
  //
  // Order (on request, no functional reason behind any prior order):
  // quads -> SoC -> sparkline -> summary. The summary block is a fixed
  // height regardless of its values (font metrics only, nothing in it
  // measures per-digit) - SUMMARY_H is that height, measured via the
  // harness rather than guessed (see draw_flow_quadrants' own "measured
  // constant" comment for why this is preferred over recomputing it at
  // draw time). Reserving it bottom-up, BEFORE laying out the sparkline,
  // is what lets the sparkline claim "whatever's actually left" instead
  // of a fixed size of its own - the one variable-height section here,
  // since draw_sparkline already takes explicit y0/y1 rather than an
  // aspect ratio.
  const int STRIP_TO_QUAD_GAP  = -4;  // matches the Flow page's own
                                      // DIVIDER_TO_QUAD_GAP - same status
                                      // strip, same quad block, same
                                      // reason to pull it up slightly.
  const int QUAD_TO_SOC_GAP    = -3;  // same reclaimed-overlap reasoning
                                      // as the Flow page - see its layout
                                      // comment.
  const int SOC_TO_DIV_GAP     = 3;
  const int DIV_TO_SPARK_GAP   = 4;
  const int SPARK_TO_DIV_GAP   = 4;
  const int DIV_TO_SUMMARY_GAP = 5;
  const int BOTTOM_MARGIN      = 4;
  const int SUMMARY_H          = 54;

  int y = draw_flow_status_strip(it, v, 0, hair_c, chrome_c);

  if (!v.show_data()) {
    it.print(120, 120, font_20, TextAlign::CENTER, "Polling Disabled");
    return;
  }

  int quad_y = y + STRIP_TO_QUAD_GAP;
  int quad_h = draw_flow_quadrants(it, v, quad_y, solar_c, load_c, batt_c, grid_c,
                                   solar_idle_c, load_idle_c, grid_idle_c,
                                   batt_idle_c, hub_c);
  y = quad_y + quad_h + QUAD_TO_SOC_GAP;

  // Battery green, same as the Flow page - the colour is what ties it to
  // the battery corner above.
  int soc_h = draw_flow_battery_soc(it, v, y, batt_c);
  y += soc_h + SOC_TO_DIV_GAP;

  it.horizontal_line(0, y, 240, hair_c);
  y += DIV_TO_SPARK_GAP;

  int summary_y = 240 - BOTTOM_MARGIN - SUMMARY_H;
  int div2_y = summary_y - DIV_TO_SUMMARY_GAP;
  int chart_y1 = div2_y - SPARK_TO_DIV_GAP;
  int label_y = y + 8;
  int chart_y0 = y + 18;
  draw_sparkline(it, v.chart_6h(), label_y, 8, chart_y0, 232, chart_y1,
                 solar_c, chrome_c, font_10,
                 "Solar - last 6h");

  it.horizontal_line(0, div2_y, 240, hair_c);

  draw_spend_summary(it, v, summary_y, spend_c, earn_c, earn_c, spend_c, chrome_c, chrome_c, font_24_bold);
}
