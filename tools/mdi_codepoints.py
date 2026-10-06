#!/usr/bin/env python3
"""Emit MDI codepoints as an ESPHome substitutions file and a C++ header.

Codepoints move between MDI releases, so they are read from the release you
actually have rather than from a cheatsheet. Point this at whichever of these
sits next to your materialdesignicons-webfont.ttf:

  - css/materialdesignicons.css  (ships beside the TTF in every webfont
                                  distribution, so it is guaranteed to be the
                                  same version as the font - prefer this)
  - meta.json                    (only in @mdi/svg / MaterialDesign-SVG, a
                                  separate package from the webfont)

Both .css and .min.css work, as do the older single-colon (:before) and
current double-colon (::before) selector styles.

Two outputs, from the same table, because YAML substitutions and C++ code
cannot share one: ${mdi_xxx} substitution only resolves in content ESPHome
loads as YAML (inline lambdas, !include'd YAML) - a plain .h file pulled in
via esphome: includes: is copied into the build untouched, so a
substitution placeholder inside one would just be invalid, literal text.

Usage:
  python3 mdi_codepoints.py path/to/materialdesignicons.css mdi_substitutions.yaml mdi_icons.h
  python3 mdi_codepoints.py path/to/meta.json               mdi_substitutions.yaml mdi_icons.h

Pull mdi_substitutions.yaml in as a package, which merges its substitutions
into the main config without colliding with the ones defined there:

  packages:
    mdi_glyphs: !include mdi_substitutions.yaml
  ...
  glyphs: ["${mdi_weather_sunny}"]

Pull mdi_icons.h in via esphome: includes: and use the MDI_* constants
directly from C++ (dashboard_pages.h and similar):

  esphome:
    includes:
      - mdi_icons.h
  ...
  it.printf(..., MDI_WEATHER_SUNNY);
"""
import json
import re
import sys

# Exactly the glyphs the Flow page needs. Anything missing from the bundled
# font is reported by name rather than silently skipped - a missing glyph is a
# blank box at runtime, and a wrong one is worse because it renders silently.
WANTED = [
    "weather-sunny",
    "power-plug",
    "transmission-tower",
    "home-outline",
    "battery-outline",
    "battery-low",
    "battery-medium",
    "battery-high",
    "battery",
    # All four diagonals: the arrows point toward or away from the central
    # house, so which diagonal is needed depends on the quadrant as well as
    # the direction of flow.
    "arrow-top-left",
    "arrow-top-right",
    "arrow-bottom-left",
    "arrow-bottom-right",
    # Directional variants for the totals row: the plain tower is the grid
    # quadrant's icon, but import and export totals want to be told apart.
    "transmission-tower-import",
    "transmission-tower-export",
    "wifi",
    # Flow page header readouts (inverter temp, electricity rate). Plain
    # "cash" rather than "currency-gbp" - the rate value already carries
    # its own unit ("30.5p"), so a currency-specific glyph (implying
    # pounds) paired with a pence figure would assert the wrong one.
    "thermometer",
    "cash",
]


def load(path):
    """Return {icon-name: codepoint-int}."""
    text = open(path, encoding="utf-8").read()
    if path.endswith(".json"):
        return {e["name"]: int(e["codepoint"], 16) for e in json.loads(text)}
    # .mdi-home-outline::before { content: "\F07D0"; }   (current)
    # .mdi-home-outline:before{content:"\F07D0"}          (older / minified)
    found = {}
    for name, cp in re.findall(
        r"\.mdi-([a-z0-9-]+):{1,2}before\s*\{\s*content:\s*[\"']\\([0-9a-fA-F]+)", text
    ):
        found[name] = int(cp, 16)
    return found


def escape_utf8(cp):
    return "\\U%08X" % cp if cp > 0xFFFF else "\\u%04X" % cp


def write_yaml(path, source, table):
    lines = ["# Generated from %s - regenerate if the MDI font is updated." % source,
             "# Include as a package - see tools/mdi_codepoints.py.",
             "substitutions:"]
    for name in WANTED:
        if name in table:
            cp = table[name]
            lines.append('  mdi_%s: "%s"  # mdi-%s U+%04X'
                         % (name.replace("-", "_"), escape_utf8(cp), name, cp))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def write_header(path, source, table):
    lines = ["#pragma once", "",
             "// Generated from %s - regenerate if the MDI font is updated." % source,
             "// See tools/mdi_codepoints.py. Companion to mdi_substitutions.yaml:",
             "// the same codepoints as C++ identifiers, for code in a plain .h file",
             "// (esphome: includes:) that ${mdi_xxx} substitutions cannot reach.",
             ""]
    for name in WANTED:
        if name in table:
            cp = table[name]
            const = "MDI_" + name.upper().replace("-", "_")
            lines.append('constexpr const char *%s = "%s";  // mdi-%s U+%04X'
                         % (const, escape_utf8(cp), name, cp))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    source, yaml_path, header_path = sys.argv[1:4]
    table = load(source)
    if not table:
        sys.exit("No icons parsed - is that a meta.json or materialdesignicons.css?")

    missing = [n for n in WANTED if n not in table]
    write_yaml(yaml_path, source, table)
    write_header(header_path, source, table)
    print("Wrote %s and %s" % (yaml_path, header_path))

    if missing:
        sys.stderr.write("WARNING: %d icon(s) not found in %s: %s\n"
                         % (len(missing), source, ", ".join(missing)))
        sys.stderr.write("Find the current names at https://pictogrammers.com/library/mdi/\n")


if __name__ == "__main__":
    main()
