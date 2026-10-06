#pragma once

#include "dashboard_view.h"
#define DEMO_DATA 1

// Synthetic data for demonstrating and eyeballing the display without HA.
//
// Deliberately injected at ONE point: the view interval builds a
// DashboardView::Inputs, and in demo mode that struct is filled from here
// instead of from the globals. Nothing in the model, the pages or the polling
// path knows demo mode exists, and no real value is overwritten - flip the
// switch off and the next frame is live data again.
//
// Stateless by design. Everything is a function of a virtual time of day, so
// there are no accumulators to drift, the sequence repeats exactly, and the
// cost is a few hundred flops once per second.
//
// It runs a whole virtual day in DAY_MS so every state the pages can render
// turns up within a couple of minutes: solar ramping, midday export, battery
// charging then discharging, an idle battery at the top of the charge, night
// import, and the 9999W/kW formatting switchover at the solar peak.
class DemoData {
 public:
  static constexpr uint32_t DAY_MS = 120000;   // one virtual day, in real ms
  static constexpr float PV_PEAK_W = 4200.0f;
  static constexpr float BATT_KWH = 13.5f;

  // Virtual time of day, 0.0 at midnight to 1.0 at the next midnight.
  static float phase_of(uint32_t now_ms) {
    return (float) (now_ms % DAY_MS) / (float) DAY_MS;
  }

  // Sine hump across daylight hours, zero outside them.
  static float solar_w(float p) {
    const float dawn = 0.25f, dusk = 0.79f;   // ~06:00 to ~19:00
    if (p <= dawn || p >= dusk) return 0.0f;
    return PV_PEAK_W * sinf((float) M_PI * (p - dawn) / (dusk - dawn));
  }

  // Base load with a morning and an evening bump, so the consumption bar has
  // something to do and the house corner is not a flat line.
  static float load_w(float p) {
    float w = 260.0f;
    w += 1150.0f * bump(p, 0.31f, 0.035f);   // morning
    w += 2600.0f * bump(p, 0.78f, 0.030f);   // cooking
    w += 520.0f * bump(p, 0.55f, 0.06f);     // afternoon oddments
    return w;
  }

  // Everything at a given virtual time, integrated from midnight. Battery
  // responds to the surplus rather than following an SoC curve open-loop: an
  // open-loop battery keeps discharging at a fixed rate through the night and
  // the residual lands on the grid, which renders as exporting in the dark.
  struct Snapshot {
    float solar, load, batt, grid_out, soc;   // grid_out > 0 means exporting
    float solar_kwh, load_kwh, imp_kwh, exp_kwh;
    float import_cost_gbp;   // accrued at the same day/night split as
                             // rate_gbp_per_kwh below, so it agrees with it
  };

  static Snapshot walk(float p) {
    const int STEPS = 96;                       // 15 virtual minutes each
    const float h = 24.0f / STEPS;
    Snapshot s{};
    s.soc = 24.0f;   // low enough that the small hours hit the floor and import
    for (int i = 0; i < STEPS; i++) {
      float q = (float) i / STEPS;
      if (q >= p) break;
      float solar = solar_w(q), load = load_w(q);
      float batt = battery_for(solar - load, s.soc);
      float out = (solar - load) + batt;
      s.soc -= (batt / 1000.0f) * h / BATT_KWH * 100.0f;
      s.soc = clampf(s.soc, SOC_MIN, 100.0f);
      s.solar_kwh += solar * h / 1000.0f;
      s.load_kwh += load * h / 1000.0f;
      if (out > 0) {
        s.exp_kwh += out * h / 1000.0f;
      } else {
        float step_kwh = -out * h / 1000.0f;
        s.imp_kwh += step_kwh;
        // Same cheap-overnight threshold as the rate assigned below, so
        // the accrued cost and the displayed rate can't disagree.
        s.import_cost_gbp += step_kwh * ((q < 0.23f) ? 0.0712f : 0.2847f);
      }
    }
    // Live values at the exact phase, using the SoC the walk arrived at, so
    // the corners move smoothly rather than stepping every 15 virtual minutes.
    s.solar = solar_w(p);
    s.load = load_w(p);
    s.batt = battery_for(s.solar - s.load, s.soc);
    s.grid_out = (s.solar - s.load) + s.batt;
    return s;
  }

  static void fill(DashboardView::Inputs &in, SolarHistory &hist,
                   uint32_t now_ms, bool clock_valid, time_t real_now) {
    float p = phase_of(now_ms);
    Snapshot s = walk(p);

    in.solar_w = s.solar;
    in.home_w = s.load;
    in.battery_w = s.batt;
    in.battery_soc_pct = s.soc;
    // Converted back through the model's own convention rather than assuming
    // one, so demo data cannot be the thing that disagrees about which way
    // round the grid reads.
    in.grid_w = DashboardView::GRID_POSITIVE_IS_EXPORT ? s.grid_out : -s.grid_out;

    in.solar_today_kwh = s.solar_kwh;
    in.home_today_kwh = s.load_kwh;
    in.import_today_kwh = s.imp_kwh;
    in.export_today_kwh = s.exp_kwh;
    in.inverter_temp_c = 27.0f + 15.0f * (s.solar / PV_PEAK_W);
    in.rate_gbp_per_kwh = (p < 0.23f) ? 0.0712f : 0.2847f;   // cheap overnight
    in.export_rate_gbp_per_kwh = 0.15f;   // flat SEG-style export rate
    in.import_cost_today_gbp = s.import_cost_gbp;

    // Demo supplies its own time base so the sparkline works with no SNTP at
    // all - which is the situation demo mode is most useful in.
    time_t t = clock_valid ? real_now : (time_t) (1700000000u + now_ms / 1000u);
    in.now = t;
    in.clock_valid = true;
    // p (0 at virtual midnight) mapped onto a real 24h clock, so the
    // status strip's time readout moves through a full day right along
    // with everything else demo mode animates.
    uint32_t total_minutes = (uint32_t) (p * 24.0f * 60.0f) % (24 * 60);
    in.hour = (uint8_t) (total_minutes / 60);
    in.minute = (uint8_t) (total_minutes % 60);
    in.now_ms = now_ms;
    in.last_fetch_ms = now_ms;          // never stale, never a red border
    in.reading_epoch = (uint32_t) t;
    in.polling = true;                  // never "Polling Disabled"
    in.link_connected = true;
    in.link_ap = false;
    in.link_trying = false;

    fill_history(hist, t, p);
  }

 private:
  static constexpr float SOC_MIN = 12.0f;
  static constexpr float CHARGE_MAX_W = 3300.0f;
  // Deliberately below the evening load peak, and the starting SoC below what
  // the night needs. A battery that covers every deficit means the grid corner
  // never imports and half the page never exercises - the demo exists to show
  // each state, not to model an ideal installation.
  static constexpr float DISCHARGE_MAX_W = 1800.0f;

  static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
  }

  static float lerp(float a, float b, float t) { return a + (b - a) * t; }

  // Gaussian-ish bump, 1.0 at centre.
  static float bump(float p, float centre, float width) {
    float d = (p - centre) / width;
    return expf(-0.5f * d * d);
  }

  // Negative charging, positive discharging - the model's convention. Returns
  // 0 at the rails, which is what puts the battery corner into its idle state
  // for a good part of a sunny afternoon.
  static float battery_for(float surplus, float soc) {
    if (surplus > 0.0f && soc < 100.0f) {
      float c = surplus < CHARGE_MAX_W ? surplus : CHARGE_MAX_W;
      return -c;
    }
    if (surplus < 0.0f && soc > SOC_MIN) {
      float d = -surplus < DISCHARGE_MAX_W ? -surplus : DISCHARGE_MAX_W;
      return d;
    }
    return 0.0f;
  }

  // Paints the whole 6h window every frame. 72 writes a second is nothing, and
  // regenerating beats filling once because the window slides on the real
  // clock - a one-shot fill would grow a gap at the right-hand edge.
  static void fill_history(SolarHistory &hist, time_t now, float p) {
    hist.advance_to(now);
    uint32_t newest = SolarHistory::bucket_of(now);
    // 72 buckets span 6 real hours; compress that to 6 virtual hours so the
    // trace matches the solar figure in the corner.
    const float span = 0.25f;
    for (int i = 0; i < SolarHistory::SLOTS; i++) {
      float q = p - span * (float) (SolarHistory::SLOTS - 1 - i) /
                        (float) SolarHistory::SLOTS;
      if (q < 0.0f) q += 1.0f;
      uint32_t bucket = newest - (uint32_t) ((SolarHistory::SLOTS - 1 - i) *
                                             SolarHistory::BUCKET_S);
      hist.store_bucket(bucket, solar_w(q), 1);
    }
  }
};

// Its own history instance, so demo mode cannot overwrite six hours of real
// readings that backfill would then have to rebuild. Costs 72 floats.
SolarHistory demo_history;
