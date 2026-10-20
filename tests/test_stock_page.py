"""Host tests for the stock sensor page layout (components/cgdn1_display/stock_page.h): HUGE number
placement (stock FUN_400e46e8 / FUN_400e47fc examples), value clamps, page states, exact ink box and the page indicator.

A C++ driver is compiled with the system compiler (skipped when none). Set CGDN1_PREVIEW_DIR to get
ASCII previews of the rendered pages (for human review only, never asserted).
"""
import math
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
DISPLAY_DIR = ROOT / "components" / "cgdn1_display"
KINDS = ["PM25", "PM10", "CO2", "TEMP", "HUMI", "PM1"]
ICONS_H = DISPLAY_DIR / "stock_icons.h"

DRIVER = r"""
#include "stock_page.h"
#include <cstdio>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
using namespace esphome::cgdn1_display;

static PageKind kind_of(int k) { return (PageKind) k; }
static int digit_of(const StockIcon *i) {
  for (int d = 0; d < 10; d++) if (HUGE_DIGITS[d] == i) return d;
  return -1;
}

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd; in >> cmd;
    if (cmd == "huge") {  // huge value tenths
      int v, t; in >> v >> t;
      HugeNumber h = huge_number(v, t != 0);
      printf("%d %d %d", h.width, h.pen, h.n);
      for (int i = 0; i < h.n; i++) printf(" %d@%d", digit_of(h.parts[i].icon), h.parts[i].x);
      printf("\n");
    } else if (cmd == "value") {  // value kind float
      int k; float f; in >> k >> f;
      printf("%d\n", stock_value(kind_of(k), f));
    } else if (cmd == "make") {  // make kind float|nan uptime
      int k; std::string f; unsigned long up; in >> k >> f >> up;
      StockPage p = make_page(kind_of(k), f == "nan" ? NAN : std::stof(f), (uint32_t) up);
      printf("%d %d %d\n", (int) p.state, p.value, p.loader_frame);
    } else if (cmd == "page" || cmd == "ink") {  // page kind state value frame ox oy
      int k, s, v, fr, ox, oy; in >> k >> s >> v >> fr >> ox >> oy;
      StockPage p{kind_of(k), (PageState) s, v, (uint8_t) fr};
      if (cmd == "ink") {
        Rect r = page_ink(p);
        printf("%d %d %d %d\n", r.x0, r.y0, r.x1, r.y1);
      } else {
        std::set<std::pair<int, int>> px;
        draw_page(p, ox, oy, [&](int x, int y) { px.insert({x, y}); });
        printf("%zu", px.size());
        for (auto &q : px) printf(" %d,%d", q.first, q.second);
        printf("\n");
      }
    } else if (cmd == "dots") {  // dots active [count]
      int a, c = 5; in >> a >> c;
      std::set<std::pair<int, int>> px;
      draw_page_dots(a, c, [&](int x, int y) { px.insert({x, y}); });
      printf("%zu", px.size());
      for (auto &q : px) printf(" %d,%d", q.first, q.second);
      printf("\n");
    }
  }
}
"""


@pytest.fixture(scope="module")
def drv(tmp_path_factory):
    cxx = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        pytest.skip("no C++ compiler on PATH")
    d = tmp_path_factory.mktemp("spage")
    src = d / "drv.cpp"
    src.write_text(DRIVER)
    exe = d / "drv"
    subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Werror", "-I", str(DISPLAY_DIR), str(src), "-o", str(exe)],
                   check=True)
    return exe


def run(exe, lines):
    out = subprocess.run([str(exe)], input="".join(l + "\n" for l in lines), capture_output=True, text=True,
                         check=True).stdout
    return out.splitlines()


def huge(drv, value, tenths=False):
    f = run(drv, [f"huge {value} {int(tenths)}"])[0].split()
    return int(f[0]), int(f[1]), [tuple(int(v) for v in p.split("@")) for p in f[3:]]


def pixels(drv, kind, state, value, frame=0, ox=0, oy=0):
    f = run(drv, [f"page {KINDS.index(kind)} {state} {value} {frame} {ox} {oy}"])[0].split()
    return {tuple(int(v) for v in p.split(",")) for p in f[1:]}


def render(drv, kind, state, value, frame=0, ox=0, oy=0):
    px = pixels(drv, kind, state, value, frame, ox, oy)
    rows = ["".join("#" if (x, y) in px else "." for x in range(160)) for y in range(128)]
    pdir = os.environ.get("CGDN1_PREVIEW_DIR")
    if pdir:
        Path(pdir).mkdir(parents=True, exist_ok=True)
        name = f"{kind}_{state}_{value}_{ox}_{oy}.txt"
        ink = [i for i, r in enumerate(rows) if "#" in r]
        (Path(pdir) / name).write_text("\n".join(rows[ink[0]:ink[-1] + 1]) + "\n" if ink else "")
    return rows


def test_huge_number_812(drv):
    w, pen, parts = huge(drv, 812)
    assert w == 102 and parts == [(8, 29), (1, 72), (2, 94)] and pen == 29 + 102


def test_huge_number_23_4(drv):
    w, pen, parts = huge(drv, 234, True)
    assert w == 114 and parts == [(2, 23), (3, 60), (4, 106)]
    # the dot is drawn at (pen - 2, 87): its pixels sit at x 95..102
    px = pixels(drv, "TEMP", 0, 234)
    assert pen - 2 == 95
    dot = {(x, y) for x, y in px if 87 <= y < 95 and 95 <= x < 103}
    assert dot and min(x for x, _ in dot) >= 95 and max(y for _, y in dot) <= 94


def test_huge_number_45(drv):
    w, _, parts = huge(drv, 45)
    assert w == 74 and parts == [(4, 43), (5, 80)]
    assert 80 + (w >> 1) == 117


def test_huge_number_12(drv):
    w, _, parts = huge(drv, 12)
    assert w == 59 and parts == [(1, 51), (2, 73)]


def test_huge_number_1111(drv):
    # Stock: a leading "1" advances 16, every later "1" 22 (drawn 6 px after the pen): width 82,
    # start 80 - (82 >> 1) = 39.
    w, pen, parts = huge(drv, 1111)
    assert w == 82 and parts == [(1, 39), (1, 61), (1, 83), (1, 105)] and pen == 39 + 82


def test_huge_number_0_5(drv):
    w, _, parts = huge(drv, 5, True)
    assert w == 77 and parts[0] == (0, 42)


def test_stock_value(drv):
    cases = [("PM25", 1234, 999), ("PM25", -3, 0), ("PM25", 12.7, 12), ("PM1", 1234, 999), ("PM1", 8.9, 8),
             ("CO2", 12000, 9999),
             ("TEMP", 23.47, 234), ("TEMP", -1.2, 0), ("TEMP", 55, 500),
             ("HUMI", 45.9, 45), ("HUMI", 100, 99), ("HUMI", -1, 0)]
    out = run(drv, [f"value {KINDS.index(k)} {v}" for k, v, _ in cases])
    assert [int(o) for o in out] == [e for _, _, e in cases]


def test_make_page_states(drv):
    k = KINDS.index("TEMP")
    out = run(drv, [f"make {k} nan 10000", f"make {k} nan 299999", f"make {k} nan 300000",
                    f"make {k} 21.0 5", f"make {k} 21.0 400000"])
    assert out[0].split() == ["1", "0", str((10000 // 420) % 3)] and (10000 // 420) % 3 == 2
    assert out[1].split()[0] == "1"
    assert out[2].split()[0] == "2"
    assert out[3].split()[:2] == ["0", "210"] and out[4].split()[:2] == ["0", "210"]


def bbox(px):
    xs, ys = [x for x, _ in px], [y for _, y in px]
    return [min(xs), min(ys), max(xs) + 1, max(ys) + 1]


@pytest.mark.parametrize("kind,state,value,frame", [("CO2", 0, 812, 0), ("CO2", 0, 8888, 0), ("TEMP", 0, 234, 0),
                                                    ("HUMI", 0, 45, 0), ("PM25", 0, 7, 0), ("CO2", 2, 0, 0),
                                                    ("PM10", 2, 0, 0), ("CO2", 1, 0, 0), ("CO2", 1, 0, 2)])
def test_page_ink_matches_pixels(drv, kind, state, value, frame):
    px = pixels(drv, kind, state, value, frame)
    got = [int(v) for v in run(drv, [f"ink {KINDS.index(kind)} {state} {value} {frame} 0 0"])[0].split()]
    assert got == bbox(px)


def test_ink_4_digit(drv):
    x0, _, x1, _ = [int(v) for v in run(drv, [f"ink {KINDS.index('CO2')} 0 8888 0 0 0"])[0].split()]
    assert x0 >= 6 and x1 <= 154


def test_page_dots_1(drv):
    f = run(drv, ["dots 0"])[0].split()[1:]
    px = {tuple(int(v) for v in p.split(",")) for p in f}
    assert all((x, 12) in px for x in range(65, 69))
    assert all(64 <= x <= 95 and 12 <= y <= 15 for x, y in px)


def test_origin_shift(drv):
    for kind, state, value in [("CO2", 0, 812), ("TEMP", 0, 234), ("HUMI", 0, 45), ("CO2", 2, 0)]:
        a = pixels(drv, kind, state, value)
        b = pixels(drv, kind, state, value, 0, 7, -5)
        assert b == {(x + 7, y - 5) for x, y in a}


def test_previews(drv):
    # Not asserted beyond "renders"; CGDN1_PREVIEW_DIR gets the ASCII frames for human review.
    for kind, value in [("CO2", 812), ("TEMP", 234), ("HUMI", 45)]:
        rows = render(drv, kind, 0, value)
        assert len(rows) == 128 and all(len(r) == 160 for r in rows)


def stock_art(title):
    """Lit pixels of a stock icon from its ASCII art in stock_icons.h (`// label "pm10", 56x15`)."""
    lines = ICONS_H.read_text().splitlines()
    i = next(n for n, l in enumerate(lines) if l.startswith(f'// {title}, '))
    rows = []
    for l in lines[i + 1:]:
        if not l.startswith("//   "):
            break
        rows.append(l[5:])
    return {(x, y) for y, r in enumerate(rows) for x, c in enumerate(r) if c == "#"}


def cols(px, x0, x1, to):
    """Pixels of px in columns [x0, x1], moved so column x0 lands on `to`."""
    return {(x - x0 + to, y) for x, y in px if x0 <= x <= x1}


def test_pm1_label_built_from_stock_glyphs(drv):
    # "pm1.0" in the stock outline style: "pm" of pm2.5, "1" and "0" of pm10, the "." of pm2.5,
    # spaced like pm2.5 (4 px after "m", 2 px either side of the "."): 49 px wide, centred on x ~78
    # like the stock labels (pm2.5 ink 51..106).
    pm25, pm10 = stock_art('label "pm2.5"'), stock_art('label "pm10"')
    want = cols(pm25, 0, 26, 0) | cols(pm10, 35, 37, 31) | cols(pm25, 43, 43, 36) | cols(pm10, 42, 51, 39)
    want = {(x + 54, y + 110) for x, y in want}
    label = {(x, y) for x, y in pixels(drv, "PM1", 0, 12) if y >= 110}
    assert label == want


def test_pm1_page_digits_like_pm25(drv):
    a = {p for p in pixels(drv, "PM1", 0, 812) if p[1] < 110}
    b = {p for p in pixels(drv, "PM25", 0, 812) if p[1] < 110}
    assert a == b and a


def dots(drv, active, count):
    f = run(drv, [f"dots {active} {count}"])[0].split()[1:]
    return {tuple(int(v) for v in p.split(",")) for p in f}


@pytest.mark.parametrize("active", range(5))
def test_page_dots_5_are_the_stock_glyphs(drv, active):
    want = {(x + 64, y + 12) for x, y in stock_art(f"page indicator, {'1st 2nd 3rd 4th 5th'.split()[active]} dot active")}
    assert dots(drv, active, 5) == want


def test_page_dots_7(drv):
    # Same 6 px pitch / glyphs, box 6 * 7 + 2 = 44 wide centred like the stock 32 px one (x 64 = 80 - 16).
    for active in range(7):
        px = dots(drv, active, 7)
        outline = {(58 + 6 * active + x, 12 + y) for x in range(1, 5) for y in range(4) if x in (1, 4) or y in (0, 3)}
        small = {(58 + 6 * k + 2 + x, 13 + y) for k in range(7) if k != active for x in range(2) for y in range(2)}
        assert px == outline | small, active
