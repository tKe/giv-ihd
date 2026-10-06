// Host-side renderer for the Flow/Detail display pages, for fast layout
// iteration without an OTA flash + ESP_LOGI cycle. See esphome_shim.h and
// fake_display.h for how the ESPHome symbols dashboard_draw.h/
// dashboard_pages.h expect are provided here, and
// tools/extract_preview_fonts.py for where generated_fonts.h comes from.
//
// Usage: render_preview [now_ms]
//   now_ms - optional virtual time-of-day in ms, per DemoData::DAY_MS
//            (default: mid-morning, an arbitrary but representative frame).
// Writes flow.bmp and detail.bmp to the current directory.

#include "esphome_shim.h"
#include "fake_display.h"
#include "generated_fonts.h"

#include "../../dashboard_draw.h"
#include "../../dashboard_pages.h"
#include "../../demo_data.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
  uint32_t now_ms = 45u * 60u * 1000u;
  if (argc > 1)
    now_ms = (uint32_t) strtoul(argv[1], nullptr, 10);

  // clock_valid=false so DemoData synthesizes its own sane epoch
  // (1700000000 + now_ms/1000) instead of us needing a real one - passing
  // true with real_now=0 runs the whole SolarHistory window at Unix epoch
  // 0, which starves the sparkline of valid buckets.
  DashboardView::Inputs in;
  DemoData::fill(in, demo_history, now_ms, /*clock_valid=*/false, /*real_now=*/0);

  DashboardView view;
  view.rebuild(demo_history, in);

  esphome::display::Display flow_disp;
  draw_page_flow(flow_disp, view);
  flow_disp.save_bmp("flow.bmp");
  printf("Wrote flow.bmp (now_ms=%u)\n", now_ms);

  esphome::display::Display detail_disp;
  draw_page_detail(detail_disp, view);
  detail_disp.save_bmp("detail.bmp");
  printf("Wrote detail.bmp (now_ms=%u)\n", now_ms);

  esphome::display::Display spend_disp;
  draw_page_spend(spend_disp, view);
  spend_disp.save_bmp("spend.bmp");
  printf("Wrote spend.bmp (now_ms=%u)\n", now_ms);

  return 0;
}
