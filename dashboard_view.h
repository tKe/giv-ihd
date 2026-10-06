#pragma once

#include "solar_history.h"
#include <cmath>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstring>

// Shared view model for the display.
//
// Everything both the renderer and any future alternative page would work out
// identically lives here, derived once per frame by an interval, then rendered
// without further computation. mipi_spi's chunked buffer calls the display
// lambda once per chunk - 8 times per logical refresh - so deriving inside the
// lambda means doing the same work 8 times, and a second page would multiply
// that again.
//
// The stronger reason is consistency, not cost. While the lambda recomputes
// everything per pass it cannot disagree with itself. Once it reads a shared
// model, a rebuild landing between pass 3 and pass 4 would produce a torn
// frame with a visible horizontal seam. That is why rebuild() and
// Display::update() are called back to back from one interval, with the
// display itself set to update_interval: never - all 8 passes happen inside
// that single update() call, so the model is provably frozen for the whole
// frame with no double buffering needed.
//
// WHAT BELONGS HERE: anything derived, and any judgement two pages must agree
// on - what counts as stale, what the link state is, how the chart is scaled.
// WHAT DOES NOT: colours, coordinates, fonts, and formatting choices. The
// model says the battery is charging; the view decides that means green at
// y=96.
//
// Everything a page renders comes from here, including plain sensor values.
// Those are not derived, and carrying them does duplicate a global - but a
// view sourcing half its data from the model and half from id() has no single
// instant at which its frame is consistent, which is precisely the property
// rebuild-then-render exists to provide. Signed readings are carried as
// direction plus magnitude so no page can render a negative watt figure
// beside a direction arrow.

// The battery reading is the inverter's net draw/supply, not cell current, so
// it does not rest at zero - the inverter's own idle overhead (nominally
// ~25W) keeps it off zero even when the cells are doing nothing. Without a
// band around that resting point the direction arrow is not flickering, it is
// simply wrong at rest.
enum class BatteryState : uint8_t {
  Charging,
  Discharging,
  Idle,
};

// Same treatment for the grid: a house sitting near balance crosses zero
// constantly, and an import/export arrow that flips several times a second is
// noise rendered as information.
enum class GridState : uint8_t {
  Importing,
  Exporting,
  Idle,
};

enum class LinkState : uint8_t {
  Connected,
  AccessPoint,  // fallback AP / captive portal is up
  Connecting,   // actively trying
  Failed,       // best effort - ESP8266's WiFi.status() does not cleanly
                // distinguish "still trying" from "given up" in every case
};

// Writes "585W" or "3.8kW" depending on magnitude, for tight spots like the
// sparkline peak label.
//
// Both formatters write into a caller-provided buffer rather than returning
// std::string. At ~40 calls a second, returning by value risks a malloc/free
// cycle per call if this toolchain's std::string lacks small-string
// optimisation - the same churn ha_history's reused record_doc_ avoids (see
// components/ha_history/ha_history.h).
inline void fmt_power(float w, char *buf, size_t n) {
  if (fabsf(w) >= 1000.0f) {
    snprintf(buf, n, "%.1fkW", w / 1000.0f);
  } else {
    snprintf(buf, n, "%.0fW", w);
  }
}

// Writes "6,752W" - full value, no kW abbreviation, comma thousands
// separators. Built by hand rather than a printf locale flag (%'d), which is
// not reliably available on this toolchain.
inline void fmt_power_full(float w, char *buf, size_t n) {
  long val = lroundf(w);
  bool neg = val < 0;
  if (neg) val = -val;
  char digits[24];  // long is 32-bit on ESP8266; sized for 64-bit hosts too
  snprintf(digits, sizeof(digits), "%ld", val);
  int len = (int) strlen(digits);
  size_t o = 0;
  if (neg && o + 1 < n) buf[o++] = '-';
  for (int i = 0; i < len && o + 1 < n; i++) {
    if (i > 0 && (len - i) % 3 == 0 && o + 1 < n) buf[o++] = ',';
    buf[o++] = digits[i];
  }
  if (o + 1 < n) buf[o++] = 'W';
  buf[o] = '\0';
}

// Splits a power figure into value and unit as SEPARATE strings, because the
// Flow page draws them in different fonts at different sizes and cannot use a
// single formatted string.
//
// Four digits is the design limit: 2140W of solar against 1728W of export is
// the realistic worst case and both must fit at once. Above 9999W it switches
// to one decimal of kW, which is also four characters, so the column never
// grows.
inline void fmt_flow(float w, char *val, size_t vn, char *unit, size_t un) {
  float m = fabsf(w);
  if (m > 9999.0f) {
    snprintf(val, vn, "%.1f", m / 1000.0f);
    snprintf(unit, un, "kW");
  } else {
    snprintf(val, vn, "%.0f", m);
    snprintf(unit, un, "W");
  }
}

// Writes "£1.04" or "-£0.67" - sign kept out front of the £ rather than
// between it and the digits (%.2f's own sign would print "£-0.67"), since
// that's how a negative amount conventionally reads.
inline void fmt_gbp(float pounds, char *buf, size_t n) {
  bool neg = pounds < 0.0f;
  snprintf(buf, n, "%s£%.2f", neg ? "-" : "", fabsf(pounds));
}

class DashboardView {
 public:
  // No successful HA fetch within this long marks the data stale.
  static constexpr uint32_t STALE_MS = 45000;
  // Chart scaling floor: stops a near-flat low-solar window rendering as
  // wild noise. Kept separate from the peak label, which must show the
  // genuine reading rather than the floor.
  static constexpr float SCALE_FLOOR_W = 100.0f;
  // Idle band with hysteresis, in watts, applied to both battery and grid.
  // A single threshold is not enough: a value hovering at the boundary would
  // chatter across it. Something active becomes idle only below IDLE_ENTER_W,
  // and something idle becomes active only above IDLE_LEAVE_W, so the gap
  // between them is the noise the display absorbs rather than shows.
  //
  // The gap also removes the need to know exactly where the inverter's idle
  // overhead rests. A battery sitting at a standing +25W draw stays idle,
  // because escaping idle needs 40W, not 26W.
  static constexpr float IDLE_ENTER_W = 25.0f;
  static constexpr float IDLE_LEAVE_W = 40.0f;
  // Below this total, the flow bars stop extending across the full widget
  // width, so a 30W trickle does not fill the same 182px as a 3kW flow and
  // read as equally significant. This governs how far the PAIR extends
  // (flow_active_frac()) - it must NOT also gate the scale the two bars are
  // measured against (flow_scale_w()). Conflating those was a real bug: with
  // scale itself floored, a total below the floor left BOTH bars short of
  // full width, so the floor's own headroom rendered as a second, spurious
  // "mismatch" tail on the bar that actually matched. The invariant a shared
  // scale is supposed to guarantee - exactly one bar reaches full width -
  // only holds if that scale is the true larger of the two, floor or not.
  static constexpr float FLOW_FLOOR_W = 500.0f;

  // GivTCP's sign convention for grid power, in ONE place: this entity reads
  // positive when exporting. The import/export state test and the flow bar
  // split both derive from it, so they cannot end up disagreeing - which is
  // what happened when the convention was written out separately in each spot
  // and only some of them got flipped. Flipping this is now the whole change.
  static constexpr bool GRID_POSITIVE_IS_EXPORT = true;

  // Positive when exporting, whatever the underlying convention.
  static float grid_export_signed(float grid_w) {
    return GRID_POSITIVE_IS_EXPORT ? grid_w : -grid_w;
  }

  // Gathered by the caller because none of it is reachable from a plain C++
  // object - id() and the Arduino WiFi types only exist in the YAML lambda.
  // A struct rather than positional arguments: several of these are adjacent
  // bools, and transposing two of those would compile silently.
  // Everything a sparkline needs, resolved once. Two are built: the one the
  // Sparkline Window select asks for, and a fixed 6h one, so a page can show
  // the full span without reaching past the model for its own window.
  struct Chart {
    SolarHistory::Window window{};
    int points{72};
    int valid{0};
    float peak_w{0.0f};
    float scale_max_w{SCALE_FLOOR_W};
    // Two points are needed to draw a segment; below that a page should say
    // so rather than render an empty chart.
    bool drawable{false};
  };

  struct Inputs {
    time_t now{0};
    bool clock_valid{false};
    // Local (DST-aware) wall-clock hour/minute, straight from ESPTime -
    // NOT derived from `now` here, since `now`/`timestamp` is deliberately
    // UTC-only (see the time: platform comment in givenergy_dashboard.yaml)
    // and reversing that into a local hour without pulling in a full tz
    // database is exactly what ESPHome's own timezone-aware ESPTime
    // breakdown already does for free at the one place that reads it.
    uint8_t hour{0};
    uint8_t minute{0};
    uint32_t now_ms{0};
    uint32_t last_fetch_ms{0};   // millis() of last successful HA fetch
    uint32_t reading_epoch{0};   // inverter's own timestamp, 0 if unknown
    bool polling{false};
    bool link_connected{false};
    bool link_ap{false};
    bool link_trying{false};
    float battery_w{0.0f};
    float grid_w{0.0f};
    float solar_w{0.0f};
    float home_w{0.0f};
    float battery_soc_pct{0.0f};
    float solar_today_kwh{0.0f};
    float home_today_kwh{0.0f};
    float import_today_kwh{0.0f};
    float export_today_kwh{0.0f};
    float inverter_temp_c{0.0f};
    float rate_gbp_per_kwh{0.0f};
    // Export tariff, separate from rate_gbp_per_kwh (import) since the two
    // are set independently (e.g. a flat SEG export rate against a
    // day/night import tariff) and GivTCP exposes them as distinct sensors.
    float export_rate_gbp_per_kwh{0.0f};
    // What GivTCP has already worked out we've spent importing today, in
    // pounds - summed (by the caller, at the HA-sensor layer) from
    // whatever rate buckets the tariff actually has, e.g. day_cost +
    // night_cost for a day/night meter. Carried as one number here because
    // the bucket structure is a tariff-plan detail the model has no
    // business knowing about - see export_income_today_gbp() below for why
    // export income is NOT computed the same way.
    float import_cost_today_gbp{0.0f};
    bool zoom_3h{false};
  };

  void rebuild(const SolarHistory &hist, const Inputs &in) {
    this->now_ = in.now;
    this->clock_valid_ = in.clock_valid;
    this->hour_ = in.hour;
    this->minute_ = in.minute;
    this->show_data_ = in.polling;
    this->reading_epoch_ = in.reading_epoch;

    // Two clocks for two questions, deliberately. Freshness of the READING is
    // wall-clock, because it is about when the inverter produced a value and a
    // view may want to render it as a time of day rather than an age. Whether
    // we have HEARD from HA recently is monotonic, because it must stay
    // answerable when NTP is down and must not lurch when a resync steps the
    // clock.
    this->fetch_age_ms_ = in.now_ms - in.last_fetch_ms;
    this->stale_ = this->fetch_age_ms_ > STALE_MS;

    // Order matters: the AP being up is more specific than being
    // disconnected, so it is tested first.
    if (in.link_ap) {
      this->link_ = LinkState::AccessPoint;
    } else if (in.link_connected) {
      this->link_ = LinkState::Connected;
    } else if (in.link_trying) {
      this->link_ = LinkState::Connecting;
    } else {
      this->link_ = LinkState::Failed;
    }
    this->link_ok_ = in.link_connected;

    // Signed readings are decomposed into direction + magnitude here, and the
    // model exposes only those. Sign convention is GivTCP's. This is the one
    // place raw-looking values are carried, and they are not raw: splitting a
    // signed value into a flag and a magnitude IS the derivation, and leaving
    // half of it to the views means every page must independently remember to
    // strip the sign. One that forgot would render "-800W" next to a charging
    // arrow.
    this->battery_abs_w_ = fabsf(in.battery_w);
    if (!active_with_hysteresis(this->battery_abs_w_,
                                this->battery_ != BatteryState::Idle)) {
      this->battery_ = BatteryState::Idle;
    } else {
      this->battery_ =
          in.battery_w < 0.0f ? BatteryState::Charging : BatteryState::Discharging;
    }

    this->grid_abs_w_ = fabsf(in.grid_w);
    if (!active_with_hysteresis(this->grid_abs_w_, this->grid_ != GridState::Idle)) {
      this->grid_ = GridState::Idle;
    } else {
      this->grid_ = grid_export_signed(in.grid_w) > 0.0f ? GridState::Exporting
                                                         : GridState::Importing;
    }

    // Straight copies. Carried so a page never reaches past the model for
    // something it renders - a view whose data comes half from here and half
    // from id() globals has no single point at which the frame is consistent,
    // which is the property the rebuild-then-render ordering exists to give.
    this->solar_w_ = in.solar_w;
    this->home_w_ = in.home_w;
    this->battery_soc_pct_ = in.battery_soc_pct;
    this->solar_today_kwh_ = in.solar_today_kwh;
    this->home_today_kwh_ = in.home_today_kwh;
    this->import_today_kwh_ = in.import_today_kwh;
    this->export_today_kwh_ = in.export_today_kwh;
    this->inverter_temp_c_ = in.inverter_temp_c;
    // Tariff arrives in pounds per kWh; pence is the conventional display
    // unit, and converting once here keeps every page consistent.
    this->rate_p_per_kwh_ = in.rate_gbp_per_kwh * 100.0f;
    this->export_rate_p_per_kwh_ = in.export_rate_gbp_per_kwh * 100.0f;
    this->import_cost_today_gbp_ = in.import_cost_today_gbp;
    // No GivTCP sensor gives export income the same way it gives import
    // cost (no day/night-style split exists on the export side) - the
    // export tariff is flat, so kWh * rate is exact rather than
    // approximate, unlike a live-rate * kWh estimate would be on the
    // import side with a time-varying tariff.
    this->export_income_today_gbp_ = in.export_today_kwh * in.export_rate_gbp_per_kwh;
    this->net_today_gbp_ = this->export_income_today_gbp_ - this->import_cost_today_gbp_;

    this->zoom_3h_ = in.zoom_3h;
    build_chart(this->chart_selected_, hist, in.now, in.clock_valid,
                in.zoom_3h ? 36 : 72);
    build_chart(this->chart_6h_, hist, in.now, in.clock_valid, 72);

    // Flow bar components. Signs are resolved here so no page has to know
    // which direction a negative reading means.
    //
    // These use the RAW magnitudes, not the idle-banded ones: a bar labelled
    // with a watt total should be a real total, and suppressing sub-40W
    // contributions would make the label disagree with the sum of its parts.
    float grid_out = grid_export_signed(in.grid_w);
    this->export_w_ = grid_out > 0.0f ? grid_out : 0.0f;
    this->import_w_ = grid_out < 0.0f ? -grid_out : 0.0f;
    this->discharge_w_ = in.battery_w > 0.0f ? in.battery_w : 0.0f;
    this->charge_w_ = in.battery_w < 0.0f ? -in.battery_w : 0.0f;
    this->generation_w_ = in.solar_w + this->import_w_ + this->discharge_w_;
    this->consumption_w_ = in.home_w + this->export_w_ + this->charge_w_;
    // One shared divisor for both bars. They are equal by physics but not by
    // measurement - separate CT clamps sampled moments apart disagree by tens
    // of watts - so scaling each to its own total would hide the discrepancy
    // by construction. Scaling both to the larger lets the shorter bar fall
    // visibly short, which is the honest rendering of a real disagreement.
    float larger = this->generation_w_ > this->consumption_w_
                       ? this->generation_w_
                       : this->consumption_w_;
    // The true larger of the two, never floored - this is what a page divides
    // each segment by, so the larger bar's segments always sum to exactly
    // 100% of whatever width it is given. Guarded against 0 only to avoid a
    // divide-by-zero when both totals are genuinely nothing.
    this->flow_scale_w_ = larger > 1.0f ? larger : 1.0f;
    // Separately, how much of the widget's full width that pair of bars
    // should occupy at all - the floor's actual job. 1.0 once the larger
    // total reaches FLOW_FLOOR_W; both bars stop short of the edge together,
    // symmetrically, below it. A page applies this once to compute a shared
    // active width and draws both bars up to that, so the low-flow headroom
    // and the between-bars mismatch are two distinct visual regions rather
    // than one width doing both jobs.
    this->flow_active_frac_ = larger < FLOW_FLOOR_W ? larger / FLOW_FLOOR_W : 1.0f;

    // Live spend/income rate, in pounds per hour - what the Spend page's
    // "Now" reading shows. Only one of import_w_/export_w_ is ever
    // non-zero (see the grid_out split above), so this is never actually
    // "spending and earning at once"; written as a difference anyway so it
    // stays correct if that ever changes. Positive means earning.
    this->live_balance_gbp_per_h_ = (this->export_w_ / 1000.0f) * in.export_rate_gbp_per_kwh -
                                    (this->import_w_ / 1000.0f) * in.rate_gbp_per_kwh;
  }

  // --- Accessors ---------------------------------------------------------

  time_t now() const { return this->now_; }
  bool clock_valid() const { return this->clock_valid_; }
  // Local wall-clock, only meaningful when clock_valid() - see the Inputs
  // comment for why these come straight from ESPTime rather than being
  // derived from now()/timestamp here.
  uint8_t hour() const { return this->hour_; }
  uint8_t minute() const { return this->minute_; }

  // True when there is HA data worth drawing. False means polling is off, so
  // the numbers on screen would be frozen with nothing to say they are.
  bool show_data() const { return this->show_data_; }

  LinkState link() const { return this->link_; }
  bool link_ok() const { return this->link_ok_; }
  bool stale() const { return this->stale_; }

  // Milliseconds since the last successful fetch. Always available.
  uint32_t fetch_age_ms() const { return this->fetch_age_ms_; }

  // The reading's own wall-clock timestamp, or 0 if the clock had not synced
  // when it arrived. Left as an epoch on purpose: an age can only render as
  // an age, whereas an epoch can render as "14:32" as well.
  uint32_t reading_epoch() const { return this->reading_epoch_; }
  bool reading_epoch_known() const {
    return this->clock_valid_ && this->reading_epoch_ != 0;
  }
  uint32_t reading_age_s() const {
    return this->reading_epoch_known()
               ? (uint32_t) ((uint32_t) this->now_ - this->reading_epoch_)
               : 0;
  }

  // Direction and magnitude, never the signed value.
  BatteryState battery_state() const { return this->battery_; }
  float battery_abs_w() const { return this->battery_abs_w_; }
  float battery_soc_pct() const { return this->battery_soc_pct_; }
  GridState grid_state() const { return this->grid_; }
  float grid_abs_w() const { return this->grid_abs_w_; }

  float solar_w() const { return this->solar_w_; }
  float home_w() const { return this->home_w_; }
  float solar_today_kwh() const { return this->solar_today_kwh_; }
  float home_today_kwh() const { return this->home_today_kwh_; }
  float import_today_kwh() const { return this->import_today_kwh_; }
  float export_today_kwh() const { return this->export_today_kwh_; }
  float inverter_temp_c() const { return this->inverter_temp_c_; }
  float rate_p_per_kwh() const { return this->rate_p_per_kwh_; }
  float export_rate_p_per_kwh() const { return this->export_rate_p_per_kwh_; }
  float import_cost_today_gbp() const { return this->import_cost_today_gbp_; }
  float export_income_today_gbp() const { return this->export_income_today_gbp_; }
  float net_today_gbp() const { return this->net_today_gbp_; }
  // Positive means earning (net exporting), negative means spending (net
  // importing) - see the rebuild() comment for why it's never both.
  float live_balance_gbp_per_h() const { return this->live_balance_gbp_per_h_; }

  // The window the Sparkline Window select asks for.
  const Chart &chart_selected() const { return this->chart_selected_; }
  // Always the full 6h span, for a page that shows it regardless of the select.
  const Chart &chart_6h() const { return this->chart_6h_; }
  bool zoom_3h() const { return this->zoom_3h_; }

  // Flow bar components, all non-negative.
  float import_w() const { return this->import_w_; }
  float export_w() const { return this->export_w_; }
  float discharge_w() const { return this->discharge_w_; }
  float charge_w() const { return this->charge_w_; }
  float generation_w() const { return this->generation_w_; }
  float consumption_w() const { return this->consumption_w_; }
  float flow_scale_w() const { return this->flow_scale_w_; }
  float flow_active_frac() const { return this->flow_active_frac_; }

 private:
  time_t now_{0};
  bool clock_valid_{false};
  uint8_t hour_{0};
  uint8_t minute_{0};
  bool show_data_{false};
  LinkState link_{LinkState::Failed};
  bool link_ok_{false};
  bool stale_{true};
  uint32_t fetch_age_ms_{0};
  uint32_t reading_epoch_{0};
  BatteryState battery_{BatteryState::Idle};
  float battery_abs_w_{0.0f};
  float battery_soc_pct_{0.0f};
  GridState grid_{GridState::Idle};
  float grid_abs_w_{0.0f};
  float solar_w_{0.0f};
  float home_w_{0.0f};
  float solar_today_kwh_{0.0f};
  float home_today_kwh_{0.0f};
  float import_today_kwh_{0.0f};
  float export_today_kwh_{0.0f};
  float inverter_temp_c_{0.0f};
  float rate_p_per_kwh_{0.0f};
  float export_rate_p_per_kwh_{0.0f};
  float import_cost_today_gbp_{0.0f};
  float export_income_today_gbp_{0.0f};
  float net_today_gbp_{0.0f};
  float live_balance_gbp_per_h_{0.0f};
  Chart chart_selected_{};
  Chart chart_6h_{};
  bool zoom_3h_{false};
  float import_w_{0.0f};
  float export_w_{0.0f};
  float discharge_w_{0.0f};
  float charge_w_{0.0f};
  float generation_w_{0.0f};
  float consumption_w_{0.0f};
  float flow_scale_w_{FLOW_FLOOR_W};
  float flow_active_frac_{1.0f};

  static void build_chart(Chart &c, const SolarHistory &hist, time_t now,
                          bool clock_valid, int points) {
    c.points = points;
    c.window = hist.window(now, points);
    c.valid = clock_valid ? c.window.valid() : 0;
    c.peak_w = c.valid > 0 ? c.window.peak() : 0.0f;
    c.scale_max_w = c.peak_w < SCALE_FLOOR_W ? SCALE_FLOOR_W : c.peak_w;
    c.drawable = c.valid >= 2;
  }

  // Hysteresis: what counts as active depends on what it was last frame, so
  // rebuild() is deliberately not a pure function of its inputs.
  static bool active_with_hysteresis(float magnitude, bool was_active) {
    return was_active ? magnitude >= IDLE_ENTER_W : magnitude > IDLE_LEAVE_W;
  }
};

DashboardView dashboard_view;
