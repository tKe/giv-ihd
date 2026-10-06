# GivEnergy display

An [ESPHome](https://esphome.io/)-driven status display for a GivEnergy solar/battery inverter,
running on a cheap ESP8266 "smart clock" board. Pulls live and
historical data from Home Assistant (via the [GivTCP](https://github.com/britkat1980/givtcp)
integration) and renders three pages — a flow diagram, a spend summary, and a
detail view — with a rolling solar-output sparkline backed by a local history
buffer.

## Hardware

- ESP12F (ESP8266), 80KB RAM, no PSRAM.
- 240x240 ST7789V SPI panel (`mipi_spi` platform) — this is the panel found
  in GeekMagic SmallTV-Ultra clones. Pinout is in `givenergy_dashboard.yaml`;
  verified against the board, not just the datasheet. See the pinout comment
  there for the source of the reverse-engineered mapping.
- Backlight is a PWM-dimmable LED, active-low on GPIO5.
- Flashing: [slavka08/esp-mini_screen](https://github.com/slavka08/esp-mini_screen)
  (the source for the reverse-engineered pinout above) documents soldering a
  CH340C onto the back of the board for USB flashing. This build didn't do
  that — instead, wire spurs were soldered to the relevant pads and a
  separate USB-to-TTL adapter was used to flash it.

## Setup

1. Copy `secrets.yaml.example` to `secrets.yaml` and fill in real values —
   WiFi credentials, a fallback-AP password, a generated native-API
   encryption key, and a Home Assistant long-lived access token.
2. In `givenergy_dashboard.yaml`, set the `ha_entity_prefix` substitution to
   match your own GivTCP entity IDs (check **Developer Tools -> States** in
   Home Assistant — it's a prefix like `sensor.givaio_<your inverter's
   serial-derived id>`, and every sensor this dashboard reads is
   `${ha_entity_prefix}_<metric>`, e.g. `_pv_power`, `_soc`, `_grid_power`).
   `current_rate`/`export_rate` were matched by naming pattern rather than
   confirmed against GivTCP's own docs — check those two against your
   instance's actual entity names before trusting the rate readout.
3. First flash: these boards ship with their own stock firmware (a basic
   weather clock, in this case), not ESPHome, so OTA isn't an option yet.
   Wire up a USB-to-TTL adapter to the flashing pads (see Hardware above) and use
   [web.esphome.io](https://web.esphome.io/) to compile and flash over that
   serial connection. Every flash after this one can go over OTA instead,
   via the `ota:` component already configured in `givenergy_dashboard.yaml`
   — `esphome run givenergy_dashboard.yaml` picks the device up on the
   network automatically.

HA itself needs no configuration changes beyond having GivTCP set up and
creating the long-lived access token — everything else is read-only HTTP
calls into HA's REST API and history endpoint.

## How it's put together

- `givenergy_dashboard.yaml` is the real config: platform, WiFi, the
  live-poll/backfill scripts, and page wiring. `fonts.yaml` and
  `mdi_substitutions.yaml` are included from it.
- `dashboard_view.h` is a shared view model, rebuilt once per frame from raw
  sensor globals, so both pages render from one consistent snapshot rather
  than re-deriving (and potentially disagreeing on) things like battery
  state or staleness independently.
- `dashboard_pages.h` / `dashboard_draw.h` are the actual page layouts, as
  real C++ functions rather than inline YAML lambdas.
- `components/ha_history/` is a local external component that streams and
  parses Home Assistant's `/api/history/period` response one JSON record at
  a time, rather than buffering the whole body — this is what makes backfill
  of the solar sparkline's 6-hour window fast and cheap on an ESP8266 with no
  PSRAM. Its assumptions about HA's response shape are spot-checked, not
  exhaustively tested — see the header comment in `ha_history.cpp` for the
  known gaps (an over-256-byte record is dropped with a warning, not a hard
  failure; no coverage for a sensor with zero history in the window).
- `solar_history.h` holds the rolling sparkline buffer — a plain C++ global
  rather than an ESPHome `globals:` entry, since a raw `float[]` array can't
  go through that component.
- `demo_data.h` drives a "Demo Mode" switch that swaps the view model's
  inputs for synthetic data, for taking screenshots or checking layouts
  without live sensor data.

## Host-side render preview

`tools/render_preview/` renders the display pages to a BMP file on your
machine, without needing to flash the device — useful for checking layout
changes quickly. It's a thin shim that fakes just enough of ESPHome's display
API to compile `dashboard_pages.h` as a normal C++ program against stb-style
font rendering.

Build on Windows with MSVC (`tools/render_preview/build.bat`, adjust the
Visual Studio path at the top if yours differs), then run the resulting
`render_preview.exe` from that directory. `generated_fonts.h` is checked in
pre-generated (via `tools/extract_preview_fonts.py`) so the harness builds
without a full `esphome compile` first; regenerate it if you change
`fonts.yaml`.

## MDI icons

`mdi_substitutions.yaml` maps icon names to codepoints in the bundled
Material Design Icons webfont (`fonts/materialdesignicons-webfont.ttf`).
Codepoints move between MDI releases, so regenerate after updating the font:

```
python3 tools/mdi_codepoints.py <path>/materialdesignicons.css mdi_substitutions.yaml mdi_icons.h
```
