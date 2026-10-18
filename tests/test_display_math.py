"""Host tests for the CGDN1 display math (components/cgdn1_display/display_math.h): the Bouncer
clamp / explicit-range step, the PageSlider (stock slide, carousel, touch chaining) and the
brightness mapping (contrast byte per "Display brightness" level, both panel variants).

A tiny C++ driver is compiled against the header with the system C++ compiler (skipped when none is
installed).
"""
import random
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
DISPLAY_DIR = ROOT / "components" / "cgdn1_display"

DRIVER = r"""
#include "display_math.h"
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
using namespace esphome::cgdn1_display;

int main() {
  std::string line;
  Bouncer b;
  PageSlider sl;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd; in >> cmd;
    if (cmd == "breset") {
      b = Bouncer();
      printf("ok\n");
    } else if (cmd == "bstep") {  // bstep now bw bh w h margin step_ms
      unsigned long now; int bw, bh, w, h, m, st; in >> now >> bw >> bh >> w >> h >> m >> st;
      b.step((uint32_t) now, bw, bh, w, h, m, (uint32_t) st);
      printf("%d %d\n", b.x, b.y);
    } else if (cmd == "brstep") {  // brstep now min_x max_x min_y max_y step_ms [max_pull]
      unsigned long now; int x0, x1, y0, y1, st, pull = 0; in >> now >> x0 >> x1 >> y0 >> y1 >> st >> pull;
      b.step_range((uint32_t) now, x0, x1, y0, y1, (uint32_t) st, pull);
      printf("%d %d\n", b.x, b.y);
    } else if (cmd == "bset") {  // bset x y vx vy: place the point (as a range change would leave it)
      in >> b.x >> b.y >> b.vx >> b.vy;
      printf("ok\n");
    } else if (cmd == "sreset") {  // a default-constructed slider (begin() not called yet)
      sl = PageSlider();
      printf("ok\n");
    } else if (cmd == "sstate") {  // sstate now -> offset page animating indicator target_page (no update)
      unsigned long now; in >> now;
      printf("%d %d %d %d %d\n", sl.offset(), sl.page(), (int) sl.animating(),
             (int) sl.indicator_visible((uint32_t) now), sl.target_page());
    } else if (cmd == "sbegin") {  // sbegin count now
      int c; unsigned long now; in >> c >> now;
      sl.begin(c, (uint32_t) now);
      printf("ok\n");
    } else if (cmd == "snext" || cmd == "sprev") {  // snext now
      unsigned long now; in >> now;
      if (cmd == "snext") sl.next((uint32_t) now); else sl.prev((uint32_t) now);
      printf("ok\n");
    } else if (cmd == "supdate") {  // supdate now -> offset page animating indicator page_x(0..4)
      unsigned long now; in >> now;
      sl.update((uint32_t) now);
      printf("%d %d %d %d", sl.offset(), sl.page(), (int) sl.animating(), (int) sl.indicator_visible((uint32_t) now));
      for (int k = 0; k < 5; k++) printf(" %d", sl.page_x(k));
      printf("\n");
    } else if (cmd == "contrast") {  // contrast pct [top]
      std::string v; in >> v;
      int top = -1; in >> top;
      float pct = v == "nan" ? NAN : std::stof(v);
      printf("%d\n", top < 0 ? contrast_level(pct) : contrast_level(pct, (uint8_t) top));
    }
  }
}
"""


@pytest.fixture(scope="module")
def drv(tmp_path_factory):
    cxx = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        pytest.skip("no C++ compiler on PATH")
    d = tmp_path_factory.mktemp("dmath")
    src = d / "drv.cpp"
    src.write_text(DRIVER)
    exe = d / "drv"
    subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Werror", "-I", str(DISPLAY_DIR), str(src), "-o", str(exe)],
                   check=True)
    return exe


# The display lambda's page + bounce logic (cgdn1.yaml) with the real page ink.
SIM = r"""
#include "display_math.h"
#include "stock_page.h"
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
using namespace esphome::cgdn1_display;

int main() {
  std::string line;
  PageSlider sl;
  Bouncer pos;
  float vals[5] = {NAN, NAN, NAN, NAN, NAN};
  int pull = BOUNCE_SLIDE_PULL;
  auto range = [&](int k, uint32_t now) {
    return bounce_range(page_ink(make_page((PageKind) k, vals[k], now)), 160, 128);
  };
  sl.begin(5, 0);
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd; in >> cmd;
    if (cmd == "vals") {  // vals v0..v4 (nan = no reading)
      for (auto &v : vals) { std::string f; in >> f; v = f == "nan" ? NAN : std::stof(f); }
      printf("ok\n");
    } else if (cmd == "pull") {  // pull n: override the slide pull (0 = the old hard clamp)
      in >> pull;
      printf("ok\n");
    } else if (cmd == "next") {
      unsigned long now; in >> now;
      sl.next((uint32_t) now);
      printf("ok\n");
    } else if (cmd == "park") {  // park cx cy: bouncer to a corner of the target page's range, heading out
      int cx, cy; unsigned long now; in >> cx >> cy >> now;
      const BounceRange r = range(sl.target_page(), (uint32_t) now);
      pos.x = cx ? r.max_x : r.min_x; pos.y = cy ? r.max_y : r.min_y;
      pos.vx = cx ? 1 : -1; pos.vy = cy ? 1 : -1;
      printf("%d %d\n", pos.x, pos.y);
    } else if (cmd == "frame") {  // frame now -> x y animating page min_x max_x min_y max_y ink(x0 y0 x1 y1)
      unsigned long now; in >> now;
      sl.update((uint32_t) now);  // as the lambda: update, range of the TARGET page, pull while sliding
      const int t = sl.target_page();
      const BounceRange r = range(t, (uint32_t) now);
      pos.step_range((uint32_t) now, r.min_x, r.max_x, r.min_y, r.max_y, 125, sl.animating() ? pull : 0);
      const Rect ink = page_ink(make_page((PageKind) t, vals[t], (uint32_t) now)).moved(pos.x, pos.y);
      printf("%d %d %d %d %d %d %d %d %d %d %d %d\n", pos.x, pos.y, (int) sl.animating(), sl.page(), r.min_x,
             r.max_x, r.min_y, r.max_y, ink.x0, ink.y0, ink.x1, ink.y1);
    }
  }
}
"""


@pytest.fixture(scope="module")
def sim(tmp_path_factory):
    cxx = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        pytest.skip("no C++ compiler on PATH")
    d = tmp_path_factory.mktemp("dsim")
    src = d / "sim.cpp"
    src.write_text(SIM)
    exe = d / "sim"
    subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Werror", "-I", str(DISPLAY_DIR), str(src), "-o", str(exe)],
                   check=True)
    return exe


def run(exe, lines):
    out = subprocess.run([str(exe)], input="".join(l + "\n" for l in lines), capture_output=True, text=True,
                         check=True).stdout
    return out.splitlines()


def ints(s):
    return [int(v) for v in s.replace("|", " ").split()]


# ---- Bouncer: the drawn box stays inside the margin, whatever its size does ----

def test_bouncer_stays_inside_with_changing_box(drv):
    rnd = random.Random(7)
    W, H, M = 160, 128, 3
    lines, sizes, now = ["breset"], [], 0
    for i in range(3000):
        now += rnd.choice([0, 60, 125, 125, 125, 250, 1000, 5000])
        bw, bh = (rnd.randint(30, 140), rnd.randint(40, 90)) if i % 40 == 0 or not sizes else sizes[-1]
        sizes.append((bw, bh))
        lines.append(f"bstep {now} {bw} {bh} {W} {H} {M} 125")
    out = run(drv, lines)[1:]
    for (bw, bh), xy in zip(sizes, out):
        x, y = ints(xy)
        assert M <= x and x + bw <= W - M, (bw, x)
        assert M <= y and y + bh <= H - M, (bh, y)


def test_bouncer_reaches_both_extremes(drv):
    W, H, M, bw, bh = 160, 128, 3, 100, 65
    lines = ["breset"] + [f"bstep {t * 125} {bw} {bh} {W} {H} {M} 125" for t in range(400)]
    xs, ys = zip(*(ints(o) for o in run(drv, lines)[1:]))
    assert (min(xs), max(xs)) == (M, W - M - bw)
    assert (min(ys), max(ys)) == (M, H - M - bh)


# ---- Bouncer::step_range: explicit [min, max] range ----

def test_bouncer_range_reaches_extremes(drv):
    lines = ["breset"] + [f"brstep {t * 125} -20 25 -30 5 125" for t in range(400)]
    xs, ys = zip(*(ints(o) for o in run(drv, lines)[1:]))
    assert (min(xs), max(xs)) == (-20, 25)
    assert (min(ys), max(ys)) == (-30, 5)


def test_bouncer_range_wider_than_canvas_pins_to_min(drv):
    lines = ["breset"] + [f"brstep {t * 125} 4 -9 -30 5 125" for t in range(100)]
    xs, ys = zip(*(ints(o) for o in run(drv, lines)[1:]))
    assert set(xs) == {4}
    assert (min(ys), max(ys)) == (-30, 5)


def test_bouncer_range_shrinks_clamps(drv):
    lines = ["breset"] + [f"brstep {t * 125} -20 25 -30 5 125" for t in range(57)]
    lines.append("brstep 7125 -3 2 -3 2 125")
    x, y = ints(run(drv, lines)[-1])
    assert -3 <= x <= 2 and -3 <= y <= 2


def test_bouncer_range_pull_limited(drv):
    # max_pull > 0 (a slide is running): a point 20 px outside the range walks back <= 3 px per
    # call per axis, monotonically, instead of jumping; once inside it bounces normally.
    lines = ["breset", "brstep 0 -20 25 -30 5 125", "bset 45 -50 1 -1"]
    lines += [f"brstep {20 * i} -20 25 -30 5 125 3" for i in range(1, 30)]
    pts = [(45, -50)] + [tuple(ints(o)) for o in run(drv, lines)[3:]]
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        assert 0 <= ax - bx <= 3 and 0 <= by - ay <= 3, pts
    assert pts[7] == (25, -30)  # 20 px at 3 px per call: inside after ceil(20 / 3) = 7 calls
    x, y = pts[-1]
    assert -20 <= x <= 25 and -30 <= y <= 5
    assert pts[-1] != pts[7]  # normal stepping resumed inside


def test_bouncer_range_pull_zero_clamps(drv):
    # max_pull 0 (settled, the default): the same point is clamped in one call.
    out = run(drv, ["breset", "brstep 0 -20 25 -30 5 125", "bset 45 -50 1 -1", "brstep 20 -20 25 -30 5 125 0",
                    "bset 45 -50 1 -1", "brstep 40 -20 25 -30 5 125"])
    assert ints(out[3]) == [25, -30] and ints(out[5]) == [25, -30]


# ---- PageSlider: stock slide (10 px / 20 ms), 5.5 s carousel, touch chaining ----

def slide(drv, cmds, times):
    """Run commands (list), then supdate at each time; returns [(offset, page, anim, ind, px*5)]."""
    out = run(drv, ["sbegin 5 0"] + cmds + [f"supdate {t}" for t in times])
    return [ints(o) for o in out[1 + len(cmds):]]


def test_slider_next_stock_timing(drv):
    r = slide(drv, ["snext 0"], [20, 140, 280, 300])
    assert [v[0] for v in r] == [10, 70, 140, 160]
    assert [v[2] for v in r] == [1, 1, 1, 0]
    assert r[3][1] == 1


def test_slider_prev_wraps(drv):
    r = slide(drv, ["sprev 0"], [100, 300])
    assert (r[0][4], r[0][8]) == (50, -110)
    assert r[1][1] == 4 and r[1][8] == 0


def test_slider_page_x_mid_slide(drv):
    v = slide(drv, ["snext 0"], [140])[0]
    px = v[4:]
    assert px[0] == -70 and px[1] == 90
    assert all(abs(x) >= 160 for x in px[2:])


def test_slider_wrap_forward(drv):
    out = run(drv, ["sbegin 5 0", "snext 0", "snext 0", "snext 0", "snext 0", "supdate 1300", "snext 1300",
                    "supdate 1440", "supdate 1600"])
    settled, mid, end = ints(out[5]), ints(out[7]), ints(out[8])
    assert settled[1] == 4 and not settled[2]
    assert mid[4] > 0  # page 0 comes in from the right
    assert end[1] == 0 and end[4] == 0 and not end[2]


def test_slider_chain(drv):
    cmds = ["snext 0", "snext 0"]
    out = run(drv, ["sbegin 5 0"] + cmds + [f"supdate {t}" for t in range(0, 640, 20)])[3:]
    offs = [ints(o)[0] for o in out]
    assert offs == sorted(offs)
    assert slide(drv, cmds, [620])[0][1:3] == [2, 0]


def test_next_then_prev_mid_slide(drv):
    times = list(range(0, 700, 20))
    out = run(drv, ["sbegin 5 0", "snext 0"] + [f"supdate {t}" for t in times if t <= 100] + ["sprev 100"]
              + [f"supdate {t}" for t in times if t > 100])[2:]
    offs = [ints(o)[0] for o in out if o.count(" ") > 2]
    # Continuous: a 10 px push per frame; only the final stock snap (<= 10 px left after a push) is 20.
    d = [abs(a - b) for a, b in zip(offs, offs[1:])]
    assert max(d) <= 20 and sum(v > 10 for v in d) <= 1, offs
    assert all(v <= 10 for v in d[:d.index(max(d))]) and not any(d[d.index(max(d)) + 1:]), offs
    last = ints(out[-1])
    assert last[1] == 0 and not last[2]


def test_slider_page_jumps_halfway(drv):
    r = slide(drv, ["snext 0"], [140, 180])
    assert (r[0][0], r[0][1]) == (70, 0)
    assert (r[1][0], r[1][1]) == (90, 1)


def tick(t0, t1):
    """supdate every 125 ms in [t0, t1): the display update_interval."""
    return [f"supdate {t}" for t in range(t0, t1, 125)]


def test_slider_no_auto_advance(drv):
    # Pages change on a touch only: a minute of redraws, before and after a touch, never slides.
    out = [ints(o) for o in run(drv, ["sbegin 5 0"] + tick(0, 60000)) if o != "ok"]
    assert all(v[1] == 0 and not v[2] for v in out)
    out = [ints(o) for o in run(drv, ["sbegin 5 0", "snext 0"] + tick(0, 60000)) if o != "ok"]
    assert all(v[1] == 1 and not v[2] for v in out[3:])  # settled on page 1 by 300 ms, stays


def test_slider_indicator(drv):
    # Off after begin(), 3 s after a touch only.
    out = [ints(o) for o in run(drv, ["sbegin 5 0", "supdate 0"] + tick(125, 10000)
                                + ["snext 10000", "supdate 10000", "supdate 12999", "supdate 13000"])
           if o != "ok"]
    assert not any(v[3] for v in out[:-3])
    assert [v[3] for v in out[-3:]] == [1, 1, 0]


def test_slider_target_page(drv):
    def tgt(cmds):
        return [ints(o)[1:] for o in run(drv, ["sbegin 5 0"] + cmds) if o.count(" ") == 4]

    # settled: target == page(); next: the target flips at once, page() only at the halfway point
    r = tgt(["sstate 0", "snext 0", "sstate 0", "supdate 140", "sstate 140", "supdate 300", "sstate 300"])
    assert [(v[0], v[1], v[3]) for v in r] == [(0, 0, 0), (0, 1, 1), (0, 1, 1), (1, 0, 1)]
    assert tgt(["sprev 0", "sstate 0"])[0][3] == 4  # wraps
    assert tgt(["snext 0", "snext 0", "supdate 140", "sstate 140"])[0][3] == 2  # chain
    assert tgt(["snext 0", "supdate 100", "sprev 100", "sstate 100"])[0][3] == 0  # reversed


def test_slider_touch_before_begin_ignored(drv):
    # A touch on the boot screen / a test pattern reaches a slider that was never begun: it must
    # not start a slide (animating() stuck true = 50 Hz redraws) nor arm the indicator.
    for cmd in ("snext", "sprev"):
        v = ints(run(drv, ["sreset", f"{cmd} 100", "sstate 100"])[2])
        assert v[0] == 0 and not v[2] and not v[3], (cmd, v)


def test_long_stall_bounded(drv):
    # A stall (display off) finishes the slide at once: the panel wakes to the settled page and
    # stays there (stock wakes to a static page).
    out = run(drv, ["sbegin 5 0", "snext 0", "supdate 3600000"] + tick(3600125, 3660000)
              + ["breset", "brstep 0 -20 25 -30 5 125", "brstep 3600000 -20 25 -30 5 125"])
    assert all(ints(o)[1] == 1 and not ints(o)[2] for o in out[2:-3])
    x, y = ints(out[-1])
    assert -20 <= x <= 25 and -30 <= y <= 5


# ---- the lambda's bounce over the real pages: no jump mid-slide, indicator kept clear ----

def slide_into(sim, vals, corner, pull=None):
    """Settle on page 1 (PM10), park the bouncer in a corner of its range, touch to slide to page 2
    (CO2) at 5500 ms; returns the frames (20 ms apart) from the last frame before that slide on."""
    cx, cy = corner
    lines = ([f"vals {vals}"] + ([f"pull {pull}"] if pull is not None else []) + ["next 0"]
             + [f"frame {t}" for t in range(0, 1000, 20)] + [f"park {cx} {cy} 1000"]
             + [f"frame {t}" for t in range(1000, 5500, 20)] + ["next 5500"]
             + [f"frame {t}" for t in range(5500, 6200, 20)])
    out = run(sim, lines)
    frames = [ints(o) for o in out if o.count(" ") == 11]
    return frames[(5500 - 1000) // 20 + 50 - 1:]  # from the last frame before the slide


@pytest.mark.parametrize("vals", ["5 12 8888 21.5 40", "5 nan 8888 21.5 40"])  # x 14-19 px, y 18 px narrower
@pytest.mark.parametrize("corner", [(0, 0), (0, 1), (1, 0), (1, 1)])
def test_slide_into_narrower_range_no_jump(sim, vals, corner):
    f = slide_into(sim, vals, corner)
    assert not f[0][2] and f[1][2]  # f[0] settled on PM10, f[1] the first slide frame
    end = next(i for i in range(1, len(f)) if not f[i][2])
    assert f[end][3] == 2
    for a, b in zip(f[:end + 1], f[1:end + 1]):
        assert abs(a[0] - b[0]) <= 3 and abs(a[1] - b[1]) <= 3, (a, b)
    x, y, _, _, x0, x1, y0, y1 = f[end][:8]
    assert x0 <= x <= x1 and y0 <= y <= y1  # inside CO2's range on arrival: no residual clamp
    # Negative control: the scenario is a real range change (the old hard clamp jumps).
    g = slide_into(sim, vals, corner, pull=0)
    assert max(max(abs(a[0] - b[0]), abs(a[1] - b[1])) for a, b in zip(g, g[1:])) > 3


def test_bounce_clears_page_indicator(sim):
    # Settled pages over a minute (loader, values, 4-digit CO2): ink >= 17 px from the top (the
    # fixed indicator at y 12..15 + 1 px), >= 3 px from the other edges; the top edge is reached.
    lines = ["vals 5 nan 8888 21.5 40"]
    for p in range(5):
        lines += [f"frame {p * 60000 + t}" for t in range(0, 60000, 125)] + [f"next {p * 60000 + 59999}"]
    frames = [ints(o) for o in run(sim, lines) if o.count(" ") == 11]
    settled = [v for v in frames if not v[2]]
    assert all(v[8] >= 3 and v[9] >= 17 and v[10] <= 157 and v[11] <= 125 for v in settled)
    assert min(v[9] for v in settled) == 17


# ---- brightness ----

def test_contrast_level_mapping(drv):
    # "Display brightness" levels 1..6 = 0..100 %: level 6 = stock's 0xD0, 0.75 stop per level.
    levels = [int(v) for v in run(drv, [f"contrast {p}" for p in (100, 80, 60, 40, 20, 0)])]
    assert levels == [0xD0, 0x7C, 0x4A, 0x2C, 0x1A, 0x0F]
    for hi, lo in zip(levels, levels[1:]):
        assert 0.55 < lo / hi < 0.65  # even perceived steps
    out = [int(v) for v in run(drv, ["contrast nan", "contrast 150", "contrast -5", "contrast 100 255",
                                     "contrast 80 255", "contrast 0 0", "contrast 0 255"])]
    assert out == [0xD0, 0xD0, 0x0F, 255, 152, 1, 19]
    curve = [int(v) for v in run(drv, [f"contrast {p}" for p in range(101)])]
    assert curve == sorted(curve) and curve[0] >= 1  # monotonic, never fully dark
