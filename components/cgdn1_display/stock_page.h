#pragma once
// Layout and drawing of one stock-firmware sensor page (HUGE outline digits, label icon, "%", ".",
// "E2", loader), the PM1.0 page and the page indicator. Pure: no ESPHome includes, drawing goes through a
// plot(x, y) callback, so the host tests (tests/test_stock_page.py) compile it with plain g++.
// All coordinates are upright logical canvas coordinates (160 x 128), relative to the page origin
// (the stock origin (Y0, X0)); the stock +2 px panel shift is ignored here.
#include <cmath>
#include <cstdint>

#include "display_math.h"
#include "stock_icons.h"

namespace esphome::cgdn1_display {

/// Stock sensor type ids 1..5 (sensor page order), then our PM1.0 (stock shows none).
enum class PageKind : uint8_t { PM25, PM10, CO2, TEMP, HUMI, PM1 };
enum class PageState : uint8_t { VALUE, LOADING, ERROR };

/// What one page shows: `value` is the stock-clamped integer (TEMP: tenths of a degree).
struct StockPage {
  PageKind kind;
  PageState state;
  int value;
  uint8_t loader_frame;
};

/// A centred HUGE number: digit glyphs only (all drawn at y 39), x relative to the page origin.
/// `pen` is the pen after the last integer digit; the temperature dot is drawn at (pen - 2, 87).
struct HugeNumber {
  struct Part {
    const StockIcon *icon;
    int x;
  };
  Part parts[6];
  int n;
  int width;  // sum of the stock per-digit advances (the trailing 5 px gap included)
  int pen;
};

static constexpr int HUGE_Y = 39;      // FUN_400e46b4: HUGE digits top (GUI X = 33)
static constexpr int HUGE_DOT_Y = 87;  // 0x3f4052c4, 8x8: ink on y 91..93
static constexpr int PERCENT_Y = 79;   // FUN_400e4ae8 / 0x3f40515c
static constexpr int LABEL_Y = 110;    // FUN_400e4a74
static constexpr int E2_X = 44, E2_Y = 40;       // FUN_400e4b94(fb, 1, ...), 0x3f404dbb
static constexpr int LOADER_X = 60, LOADER_Y = 57;  // FUN_400e4bd0
static constexpr int DOTS_X = 64, DOTS_Y = 12;   // page indicator

// label "pm1.0", 49x15 (not in stock): "pm" and "." of pm2.5, "1" and "0" of pm10, spaced like pm2.5.
//   ##########....#############....###.....##########
//   #........#....#.....#.....#......#.....#........#
//   #........#....#.....#.....#......#.....#........#
//   #........#....#.....#.....#......#.....#........#
//   #........#....#.....#.....#......#.....#........#
//   #........#....#.....#.....#......#.....#........#
//   #........#....#.....#.....#......#.....#........#
//   #........#....#.....#.....#......#.....#........#
//   ##########....#.....#.....#......#.....#........#
//   #.............#.....#.....#......#.....#........#
//   #.............#.....#.....#......#.....#........#
//   #.............#.....#.....#......#.....#........#
//   #.............#.....#.....#......#.....#........#
//   #.............#.....#.....#......#.....#........#
//   #.............#.....#.....#......#..#..##########
inline constexpr uint8_t ICON_PM1_DATA[] = {
    0xFF, 0xC3, 0xFF, 0xE1, 0xC1, 0xFF, 0x80, 0x80, 0x42, 0x08, 0x20, 0x41, 0x00, 0x80, 0x80, 0x42, 0x08, 0x20,
    0x41, 0x00, 0x80, 0x80, 0x42, 0x08, 0x20, 0x41, 0x00, 0x80, 0x80, 0x42, 0x08, 0x20, 0x41, 0x00, 0x80, 0x80,
    0x42, 0x08, 0x20, 0x41, 0x00, 0x80, 0x80, 0x42, 0x08, 0x20, 0x41, 0x00, 0x80, 0x80, 0x42, 0x08, 0x20, 0x41,
    0x00, 0x80, 0xFF, 0xC2, 0x08, 0x20, 0x41, 0x00, 0x80, 0x80, 0x02, 0x08, 0x20, 0x41, 0x00, 0x80, 0x80, 0x02,
    0x08, 0x20, 0x41, 0x00, 0x80, 0x80, 0x02, 0x08, 0x20, 0x41, 0x00, 0x80, 0x80, 0x02, 0x08, 0x20, 0x41, 0x00,
    0x80, 0x80, 0x02, 0x08, 0x20, 0x41, 0x00, 0x80, 0x80, 0x02, 0x08, 0x20, 0x49, 0xFF, 0x80};
inline constexpr StockIcon ICON_PM1 = {49, 15, ICON_PM1_DATA};

/// Stock gives up on a missing first reading after this long and shows "E2".
static constexpr uint32_t LOADING_MAX_MS = 300000;
/// FUN_400e4bd0 advances the loader every > 40 ticks of the 20 ms loop.
static constexpr uint32_t LOADER_FRAME_MS = 420;

/// Lay out a non-negative number the way FUN_400e46e8 (integer) / FUN_400e47fc (1 decimal) do:
/// digits are summed MSD first into `width`, then the pen starts at 80 - (width >> 1) and the
/// glyphs are placed with the same advances. tenths: `value` is x10 and a value < 1.0 gets a
/// leading "0" (as the stock float path does).
inline HugeNumber huge_number(int value, bool tenths) {
  HugeNumber h{};
  if (value < 0)
    value = 0;
  int tenth = 0;
  if (tenths) {
    tenth = value % 10;
    value /= 10;
  }
  int digits[4], nd = 0;  // LSD first; stock shows at most 4
  do {
    digits[nd++] = value % 10;
    value /= 10;
  } while (value > 0 && nd < 4);

  // Pass 1: width = sum of advances (FUN_400e46e8 scans from the MSD).
  int adv[4];
  for (int i = 0; i < nd; i++) {
    const int d = digits[nd - 1 - i];
    const bool prev1 = i > 0 && digits[nd - i] == 1;
    adv[i] = i == 0 ? (d == 1 ? 16 : 37) : d == 1 ? 22 : prev1 ? 43 : 37;
  }
  int width = 0;
  for (int i = 0; i < nd; i++)
    width += adv[i];
  if (tenths)
    width += tenth == 1 ? 19 : 40;

  // Pass 2: placement. A "1" (unless first) and a digit right after a "1" are drawn 6 px after the
  // pen; the advance already includes that pad.
  int pen = 80 - (width >> 1);
  for (int i = 0; i < nd; i++) {
    const int d = digits[nd - 1 - i];
    const bool prev1 = i > 0 && digits[nd - i] == 1;
    const int x = pen + ((i > 0 && (d == 1 || prev1)) ? 6 : 0);
    h.parts[h.n++] = {HUGE_DIGITS[d], x};
    pen += adv[i];
  }
  h.pen = pen;
  if (tenths)
    h.parts[h.n++] = {HUGE_DIGITS[tenth], pen + 9};  // dot at pen - 2, digit at pen + 9 (FUN_400e47fc)
  h.width = width;
  return h;
}

/// Stock clamp + truncation of a reading: PM2.5 / PM10 0..999, CO2 0..9999, humidity
/// trunc 0..99, temperature trunc(T * 10) 0..500 (no minus sign anywhere; the multiply is in float
/// like the stock code, so 23.4f -> 233 is faithful).
inline int stock_value(PageKind kind, float v) {
  if (std::isnan(v))
    return 0;
  const float scaled = kind == PageKind::TEMP ? v * 10.0f : v;
  const float t = std::trunc(scaled);
  int max;
  switch (kind) {
    case PageKind::PM25:
    case PageKind::PM10:
    case PageKind::PM1:
      max = 999;
      break;
    case PageKind::CO2:
      max = 9999;
      break;
    case PageKind::TEMP:
      max = 500;
      break;
    default:
      max = 99;
      break;
  }
  return t <= 0.0f ? 0 : t >= (float) max ? max : (int) t;
}

/// Page state for a reading: a number shows its value; NaN shows the loader for the first
/// LOADING_MAX_MS since boot (status 1) and "E2" after that. One global loader clock (stock keeps
/// one per sensor, irrelevant here).
inline StockPage make_page(PageKind kind, float v, uint32_t uptime_ms) {
  StockPage p{kind, PageState::VALUE, 0, 0};
  if (std::isnan(v)) {
    p.state = uptime_ms < LOADING_MAX_MS ? PageState::LOADING : PageState::ERROR;
    p.loader_frame = (uint8_t) ((uptime_ms / LOADER_FRAME_MS) % 3);
  } else {
    p.value = stock_value(kind, v);
  }
  return p;
}

/// Calls plot(x, y) for every lit pixel of `icon` placed with its top-left at (x, y): upright,
/// row-major, MSB = leftmost, rows padded to whole bytes.
template<typename Plot> void draw_stock_icon(const StockIcon &icon, int x, int y, Plot plot) {
  const int stride = (icon.width + 7) / 8;
  for (int r = 0; r < icon.height; r++)
    for (int c = 0; c < icon.width; c++)
      if (icon.data[r * stride + c / 8] & (0x80 >> (c & 7)))
        plot(x + c, y + r);
}

/// Label icon and its x offset from 80 (FUN_400e4a74): thermometer -5, droplet -9, CO2 -21,
/// pm2.5 / pm10 -29 (the icons are 8 / 16 / 40 / 56 px wide, so they are centred on ~x 79); our
/// 49 px pm1.0 -26.
inline const StockIcon &page_label(PageKind kind, int *dx) {
  switch (kind) {
    case PageKind::PM25:
      *dx = -29;
      return ICON_PM25;
    case PageKind::PM10:
      *dx = -29;
      return ICON_PM10;
    case PageKind::PM1:
      *dx = -26;
      return ICON_PM1;
    case PageKind::CO2:
      *dx = -21;
      return ICON_CO2;
    case PageKind::TEMP:
      *dx = -5;
      return ICON_THERMOMETER;
    default:
      *dx = -9;
      return ICON_DROPLET;
  }
}

/// One page at origin (ox, oy). The label is common to every state; the middle is the HUGE
/// number (+ "." for temperature, + "%" for humidity), the loader (FUN_400e4bd0) or "E2"
/// (FUN_400e4b94).
template<typename Plot> void draw_page(const StockPage &p, int ox, int oy, Plot plot) {
  int dx;
  const StockIcon &label = page_label(p.kind, &dx);
  draw_stock_icon(label, ox + 80 + dx, oy + LABEL_Y, plot);
  if (p.state == PageState::LOADING) {
    draw_stock_icon(*LOADER_FRAMES[p.loader_frame % 3], ox + LOADER_X, oy + LOADER_Y, plot);
  } else if (p.state == PageState::ERROR) {
    draw_stock_icon(ICON_E2, ox + E2_X, oy + E2_Y, plot);
  } else {
    const bool temp = p.kind == PageKind::TEMP;
    const HugeNumber h = huge_number(p.value, temp);
    for (int i = 0; i < h.n; i++)
      draw_stock_icon(*h.parts[i].icon, ox + h.parts[i].x, oy + HUGE_Y, plot);
    if (temp)
      draw_stock_icon(ICON_HUGE_DOT, ox + h.pen - 2, oy + HUGE_DOT_Y, plot);
    if (p.kind == PageKind::HUMI)
      draw_stock_icon(ICON_PERCENT, ox + 80 + (h.width >> 1), oy + PERCENT_Y, plot);
  }
}

/// Exact lit-pixel bounding box of draw_page at origin (0, 0), scanned from the glyph bitmaps
/// (not their rectangles: the glyphs have empty margins). Feeds the bounce range.
inline Rect page_ink(const StockPage &p) {
  Rect r;
  bool any = false;
  draw_page(p, 0, 0, [&](int x, int y) {
    const Rect px{x, y, x + 1, y + 1};
    r = any ? r.united(px) : px;
    any = true;
  });
  return r;
}

/// Bounce margins, lit ink to the glass edge: 3 px (the ink was seen running off the glass at 1).
/// The top one keeps the digits off the page indicator, which does not bounce: its rows y 12..15
/// (DOTS_Y + the 4 px glyph) plus a 1 px gap, so ink starts at y >= 17.
static constexpr int BOUNCE_MARGIN = 3;
static constexpr int BOUNCE_TOP_MARGIN = DOTS_Y + 4 + 1;
/// While a slide runs the bounce range is the arriving page's from the first frame; a point left
/// outside it is walked in at most this many px per redraw (Bouncer::step_range max_pull), i.e.
/// up to 45 px over the 15-frame slide, instead of a jump.
static constexpr int BOUNCE_SLIDE_PULL = 3;

/// Range of the bounce offset for a page with ink `ink` (page_ink) on a w x h canvas.
struct BounceRange {
  int min_x, max_x, min_y, max_y;
};
inline BounceRange bounce_range(const Rect &ink, int w, int h) {
  return {BOUNCE_MARGIN - ink.x0, w - BOUNCE_MARGIN - ink.x1, BOUNCE_TOP_MARGIN - ink.y0,
          h - BOUNCE_MARGIN - ink.y1};
}

/// The page indicator for `count` pages, drawn like the stock 5-dot 32x4 glyphs at (64, 12)
/// (which it reproduces for 5): a 2x2 dot per page on a 6 px pitch, the active one a 4x4 ring, the
/// 6 * count + 2 px box centred on the stock 32 px one.
template<typename Plot> void draw_page_dots(int active, int count, Plot plot) {
  if (active < 0 || active >= count)
    return;
  const int x0 = DOTS_X + (32 - (6 * count + 2)) / 2;
  for (int k = 0; k < count; k++) {
    const int x = x0 + 6 * k;
    if (k == active) {
      for (int i = 0; i < 4; i++) {
        plot(x + 1 + i, DOTS_Y);
        plot(x + 1 + i, DOTS_Y + 3);
      }
      for (int j = 1; j < 3; j++) {
        plot(x + 1, DOTS_Y + j);
        plot(x + 4, DOTS_Y + j);
      }
    } else {
      for (int j = 1; j < 3; j++) {
        plot(x + 2, DOTS_Y + j);
        plot(x + 3, DOTS_Y + j);
      }
    }
  }
}

}  // namespace esphome::cgdn1_display
