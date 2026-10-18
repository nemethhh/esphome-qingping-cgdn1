"""Host tests for the CGDN1 night mode logic (components/cgdn1_night/night.h).

A tiny C++ driver is compiled against the header with the system C++ compiler (skipped when none
is installed) and fed commands over stdin. Expected behaviour is the stock 4.8.5 one (window test
FUN_400ea04c, night branch of the 1 s system task FUN_400ea3f0); see the header for details.
"""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
NIGHT_DIR = ROOT / "components" / "cgdn1_night"

DRIVER = r"""
#include "night.h"
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
using namespace cgdn1_night;
int main() {
  Engine e;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd; in >> cmd;
    if (cmd == "reset") { e = Engine(); printf("ok\n"); }
    else if (cmd == "W") {
      // W start end: one char per minute of the day, 1 = inside
      int s, en; in >> s >> en;
      std::string row;
      for (int m = 0; m < DAY_MIN; m++) row += in_window(m / 60, m % 60, s, en) ? '1' : '0';
      printf("%s\n", row.c_str());
    } else if (cmd == "T") {
      int v[9]; for (int &x : v) in >> x;
      Inputs x;
      x.enabled = v[0]; x.pages_shown = v[1]; x.clock_valid = v[2]; x.hour = v[3]; x.minute = v[4];
      x.start = v[5]; x.end = v[6]; x.display_on = v[7]; x.idle_s = v[8];
      static const char *const A[] = {"none", "off", "on"};
      Action a = e.tick(x);
      printf("%s %d\n", A[(int) a], (int) e.active());
    }
  }
}
"""


@pytest.fixture(scope="module")
def drv(tmp_path_factory):
    cxx = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        pytest.skip("no C++ compiler on PATH")
    d = tmp_path_factory.mktemp("night")
    src = d / "drv.cpp"
    src.write_text(DRIVER)
    exe = d / "drv"
    subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Werror", "-I", str(NIGHT_DIR), str(src), "-o", str(exe)],
                   check=True)
    return exe


def run(exe, lines):
    out = subprocess.run([str(exe)], input="".join(l + "\n" for l in lines), capture_output=True, text=True,
                         check=True).stdout
    return out.splitlines()


def circular(start, end):
    """[start, end) forward through midnight, start == end = never."""
    if start == end:
        return "0" * 1440
    return "".join("1" if (m - start) % 1440 < (end - start) % 1440 else "0" for m in range(1440))


# ---- window test (FUN_400ea04c) ----------------------------------------------------------------

def test_window_is_circular_half_open_for_all_clock_values(drv):
    # Every start/end on a 10-minute grid plus hour-boundary neighbours: the literal port of the
    # stock hour/minute walk equals the plain circular [start, end) check for 0..1439.
    marks = sorted({m for h in range(24) for m in (h * 60, h * 60 + 1, h * 60 + 59)} | set(range(0, 1440, 10)))
    pairs = [(s, e) for s in marks[::3] for e in marks[::2]]
    got = run(drv, [f"W {s} {e}" for s, e in pairs])
    for (s, e), row in zip(pairs, got):
        assert row == circular(s, e), (s, e)


def test_window_examples(drv):
    s, e = 22 * 60, 7 * 60  # 22:00 -> 07:00
    row = run(drv, [f"W {s} {e}"])[0]
    assert row[21 * 60 + 59] == "0" and row[22 * 60] == "1" and row[0] == "1"
    assert row[6 * 60 + 59] == "1" and row[7 * 60] == "0"
    assert run(drv, ["W 600 600"])[0] == "0" * 1440


def test_window_stock_quirk_24h(drv):
    # end 1440 ("24:00"): hour 24 never met -> all day except the start hour's minutes before start
    row = run(drv, [f"W {22 * 60 + 30} 1440"])[0]
    assert row[22 * 60 + 29] == "0" and row[22 * 60 + 30] == "1" and row[3 * 60] == "1" and row[22 * 60] == "0"
    # start 1440: hour 24 is never the current hour and the walk from 25 never wraps -> never
    assert run(drv, ["W 1440 600"])[0] == "0" * 1440


# ---- the 1 s tick (FUN_400ea3f0 night branch) --------------------------------------------------

S, E = 22 * 60, 7 * 60


def t(enabled=1, pages=1, clock=1, hh=23, mm=0, start=S, end=E, disp=1, idle=10):
    return f"T {enabled} {pages} {clock} {hh} {mm} {start} {end} {disp} {idle}"


def test_inside_window_turns_display_off_after_idle(drv):
    got = run(drv, [t(idle=0), t(idle=4), t(idle=5), t(disp=0, idle=6)])
    assert got == ["none 1", "none 1", "off 1", "none 1"]


def test_touch_in_night_lights_then_off_again(drv):
    # display woken by a touch (idle reset): stays on while idle <= 4, then off again
    got = run(drv, [t(disp=0, idle=30), t(disp=1, idle=0), t(disp=1, idle=4), t(disp=1, idle=5)])
    assert got == ["none 1", "none 1", "none 1", "off 1"]


def test_leaving_window_wakes_display_once(drv):
    got = run(drv, [t(disp=0), t(hh=7, mm=0, disp=0), t(hh=7, mm=1, disp=0)])
    assert got == ["none 1", "on 0", "none 0"]


def test_display_turned_off_by_day_stays_off(drv):
    assert run(drv, [t(hh=12, disp=0), t(hh=12, disp=0)]) == ["none 0", "none 0"]


def test_leaving_with_display_on_does_nothing(drv):
    assert run(drv, [t(idle=0), t(hh=8, disp=1)]) == ["none 1", "none 0"]


def test_disabling_inside_window_wakes_display(drv):
    assert run(drv, [t(disp=0), t(enabled=0, disp=0), t(enabled=0, disp=0)]) == ["none 1", "on 0", "none 0"]


def test_disabled_never_acts(drv):
    assert run(drv, [t(enabled=0), t(enabled=0, disp=0)]) == ["none 0", "none 0"]


def test_no_clock_does_nothing_and_keeps_flag(drv):
    got = run(drv, [t(clock=0), t(), t(clock=0, hh=12, disp=0), t(hh=12, disp=0)])
    assert got == ["none 0", "off 1", "none 1", "on 0"]


def test_boot_screen_does_nothing(drv):
    got = run(drv, [t(pages=0), t(pages=0, disp=0), t(pages=0, hh=12, disp=0)])
    assert got == ["none 0", "none 0", "none 0"]
