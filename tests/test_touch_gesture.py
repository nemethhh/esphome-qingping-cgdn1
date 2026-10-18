"""Host tests for the CGDN1 touch-slider gesture decoder (components/cgdn1_touch/gesture.h).

A tiny C++ driver is compiled against the header with the system C++ compiler (skipped when
none is installed) and fed (position, touched, t_ms) samples over stdin. The thresholds under
test are the stock 4.8.5 ones (Touch_Task FUN_400e2268, OLED switching page FUN_400e3778, key
timer FUN_400ea1ac); see the header for the addresses.
"""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
TOUCH_DIR = ROOT / "components" / "cgdn1_touch"

DRIVER = r"""
#include "gesture.h"
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
using namespace touch_gesture;
int main() {
  Decoder d;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd; in >> cmd;
    if (cmd == "reset") { d = Decoder(); printf("ok\n"); }
    else if (cmd == "trunc") { d.mark_truncated(); }
    else if (cmd == "s") {
      int pos, touched, status = 0; unsigned long t; in >> pos >> touched >> t >> status;
      Result r = d.feed((uint8_t) pos, touched != 0, (uint32_t) t, (uint8_t) status);
      Stats s = d.stats();
      printf("R %d %d %s %d %d %d %d\n", r.pressed, r.released, gesture_name(r.gesture), s.spos, s.rpos, s.tt, s.speed);
    }
  }
}
"""


@pytest.fixture(scope="module")
def decoder(tmp_path_factory):
    cxx = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        pytest.skip("no C++ compiler on PATH")
    d = tmp_path_factory.mktemp("touch")
    src = d / "drv.cpp"
    src.write_text(DRIVER)
    exe = d / "drv"
    subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Werror", "-I", str(TOUCH_DIR), str(src), "-o", str(exe)],
                   check=True)
    return exe


def run(exe, samples):
    """samples: list of (pos, touched, t[, status]) or "trunc" (mark_truncated() after the previous
    sample). Returns the list of per-sample results."""
    lines = []
    for x in samples:
        if x == "trunc":
            lines.append("trunc\n")
            continue
        p, t, ms, *st = x
        lines.append(f"s {p} {int(t)} {ms} {st[0] if st else 0}\n")
    out = subprocess.run([str(exe)], input="".join(lines), capture_output=True, text=True, check=True).stdout
    res = []
    for line in out.splitlines():
        _, pressed, released, g, spos, rpos, tt, speed = line.split()
        res.append(dict(pressed=pressed == "1", released=released == "1", gesture=g, spos=int(spos),
                        rpos=int(rpos), tt=int(tt), speed=int(speed)))
    return res


def touch(positions, t0=0, dt=10, lift=True):
    """A touch: one sample per position every dt ms, then (optionally) a not-touched sample."""
    s = [(p, True, t0 + i * dt) for i, p in enumerate(positions)]
    if lift:
        s.append((0, False, t0 + len(positions) * dt))
    return s


def gestures(exe, samples):
    return [r["gesture"] for r in run(exe, samples) if r["gesture"] != "none"]


def gesture(exe, positions, **kw):
    g = gestures(exe, touch(positions, **kw))
    assert len(g) <= 1, g
    return g[0] if g else "none"


# ---- taps (FUN_400e2268: SPOS outside 60..185, span <= 24, TT >= 1; zone by SPOS + RPOS) ----

def test_tap_right_end(decoder):
    assert gesture(decoder, [20] * 5) == "tap_right"


def test_tap_left_end(decoder):
    assert gesture(decoder, [220] * 5) == "tap_left"


def test_single_sample_is_no_tap(decoder):
    # TT = 0 (only the touch-down sample): stock emits nothing
    assert gesture(decoder, [20]) == "none"
    assert gesture(decoder, [220]) == "none"
    assert gesture(decoder, [120]) == "none"


def test_zone_sums(decoder):
    # SPOS + RPOS < 120 -> right, < 372 -> stock "chick" (not consumed by stock 4.8.5: nothing;
    # it used to toggle the display), else left
    assert gesture(decoder, [55, 64]) == "tap_right"      # 119
    assert gesture(decoder, [55, 65]) == "none"           # 120
    assert gesture(decoder, [186, 185]) == "none"         # 371 (SPOS 186 is outside the middle)
    assert gesture(decoder, [186, 186]) == "tap_left"     # 372


def test_middle_zone_bounds(decoder):
    # 60..185 = middle (stock: drag; here a still, short middle touch is tap_centre)
    assert gesture(decoder, [59] * 5) == "tap_right"      # edge start, sum 118
    assert gesture(decoder, [60] * 5) == "tap_centre"
    assert gesture(decoder, [185] * 5) == "tap_centre"
    assert gesture(decoder, [186] * 5) == "tap_left"


def test_centre_tap_is_conservative(decoder):
    assert gesture(decoder, [120] * 4) == "tap_centre"           # TT 3
    assert gesture(decoder, [120] * 3) == "none"                 # TT 2: too short to be sure
    assert gesture(decoder, [120, 124, 128, 124]) == "tap_centre"  # span 8
    assert gesture(decoder, [120, 124, 129, 124]) == "none"      # span 9
    assert gesture(decoder, [62, 60, 59, 60, 61]) == "none"      # leaves the middle zone
    assert gesture(decoder, [120] * 101) == "none"               # 1010 ms: a hold, not a tap
    assert gesture(decoder, [120] * 100) == "tap_centre"         # 1000 ms
    # an ATtiny motion bit on any sample: the finger slid
    s = touch([120] * 5)
    s[2] = (121, True, 20, 0x88)
    assert gestures(decoder, s) == []
    # start missed (read error / late first sample): never a tap
    assert gestures(decoder, [(120, True, 0), "trunc"] + touch([120] * 5, t0=10)) == []


def test_tap_span_limit(decoder):
    assert gesture(decoder, [20, 30, 44]) == "tap_right"  # span 24
    assert gesture(decoder, [20, 30, 45]) == "none"       # span 25: neither click nor drag
    assert gesture(decoder, list(range(20, 47, 2))) == "none"  # span 26: slow short drag, snaps back
    assert gesture(decoder, [20, 30, 46]) == "swipe_left"      # span 26 but fast (13 / 10 ms): fling


def test_middle_tap_with_jitter(decoder):
    assert gesture(decoder, [120, 122, 119, 121, 120]) == "tap_centre"
    # out and back within the click span but > CENTRE_MAX_SPAN: ambiguous, nothing
    assert gesture(decoder, [100, 112, 124, 112, 100]) == "none"
    # wider than the click span, slow, small net motion: stock snaps back, nothing
    assert gesture(decoder, [100, 104, 108, 112, 116, 120, 124, 128, 120, 112, 104]) == "none"


def test_position_zero_is_one(decoder):
    r = run(decoder, touch([0, 0, 0]))
    assert r[0]["spos"] == 1 and r[-1]["rpos"] == 1
    assert r[-1]["gesture"] == "tap_right"


# ---- swipes (FUN_400e3778: |speed| >= 6 per 10 ms -> fling; else page snap at 89 units) ------

def test_fast_swipe_left_and_right(decoder):
    assert gesture(decoder, list(range(60, 141, 10))) == "swipe_left"    # +10 / sample
    assert gesture(decoder, list(range(200, 119, -10))) == "swipe_right"


def test_swipe_from_an_end(decoder):
    # starts in the right zone, moves > 25 -> drag
    assert gesture(decoder, list(range(10, 80, 10))) == "swipe_left"
    assert gesture(decoder, list(range(240, 170, -10))) == "swipe_right"


def test_speed_threshold(decoder):
    assert gesture(decoder, [100 + 5 * i for i in range(11)]) == "none"        # speed 5, delta 50
    assert gesture(decoder, [100 + 6 * i for i in range(11)]) == "swipe_left"  # speed 6
    assert gesture(decoder, [150 - 6 * i for i in range(11)]) == "swipe_right"


def test_slow_drag_page_snap(decoder):
    # 0.9 px per unit, half a page = 80 px -> 89 units
    assert gesture(decoder, [60 + 2 * i for i in range(45)] + [148, 149]) == "swipe_left"   # delta 89
    assert gesture(decoder, [60 + 2 * i for i in range(45)] + [148, 148]) == "none"         # delta 88
    assert gesture(decoder, [200 - 2 * i for i in range(45)] + [111, 111]) == "swipe_right" # delta -89


def test_speed_uses_last_ten_intervals(decoder):
    # fast flick, then the finger rests 10+ samples: release speed 0, delta < 89 -> nothing
    pos = [80, 95, 110, 125, 140] + [140] * 11
    assert gesture(decoder, pos) == "none"
    r = run(decoder, touch(pos))
    assert r[-1]["speed"] == 0
    # same flick, released at once: swipe
    assert gesture(decoder, [80, 95, 110, 125, 140]) == "swipe_left"


def test_back_and_forth_uses_speed_sign(decoder):
    # ends where it started (delta 0): the direction is the speed's
    pos = [100, 110, 120, 130, 140, 150, 160] + [160 - 6 * i for i in range(1, 11)]
    r = run(decoder, touch(pos))
    assert r[-1]["speed"] < -5
    assert r[-1]["gesture"] == "swipe_right"


def test_speed_is_time_normalised(decoder):
    # 20 ms samples, +12 each = 6 per 10 ms -> fling; +10 each = 5 per 10 ms -> no fling
    assert gesture(decoder, [100 + 12 * i for i in range(7)], dt=20) == "swipe_left"
    assert gesture(decoder, [100 + 10 * i for i in range(7)], dt=20) == "none"


# ---- long touch (FUN_400ea1ac: span <= 37 for 40 x 180 ms) ---------------------------------

def test_long_touch_fires_once_while_held(decoder):
    n = 800  # 8 s at 10 ms
    r = run(decoder, touch([20] * n))
    fired = [i for i, x in enumerate(r) if x["gesture"] == "long_touch"]
    assert fired == [720]               # t = 7200 ms, still touched
    assert not r[720]["released"]
    assert r[-1]["released"] and r[-1]["gesture"] == "none"  # no tap after a long touch


def test_long_touch_needs_a_still_finger(decoder):
    wobble = [100 + (37 if (i // 50) % 2 else 0) for i in range(800)]
    assert "long_touch" in gestures(decoder, touch(wobble))
    wobble = [100 + (38 if (i // 50) % 2 else 0) for i in range(800)]
    assert "long_touch" not in gestures(decoder, touch(wobble))


def test_long_touch_not_before_7200ms(decoder):
    assert gesture(decoder, [20] * 720) == "tap_right"  # last sample at 7190 ms


# ---- press / release bookkeeping -----------------------------------------------------------

def test_press_release_flags_and_stats(decoder):
    r = run(decoder, touch([30, 31, 32]))
    assert r[0]["pressed"] and not r[0]["released"]
    assert not any(x["pressed"] for x in r[1:])
    assert r[-1]["released"] and r[-1]["spos"] == 30 and r[-1]["rpos"] == 32 and r[-1]["tt"] == 2


def test_idle_samples_do_nothing(decoder):
    r = run(decoder, [(0, False, t) for t in range(0, 100, 10)])
    assert not any(x["pressed"] or x["released"] or x["gesture"] != "none" for x in r)


def test_consecutive_touches_are_independent(decoder):
    s = touch([20] * 5) + touch([220] * 5, t0=100) + touch(list(range(60, 141, 10)), t0=200)
    assert gestures(decoder, s) == ["tap_right", "tap_left", "swipe_left"]


# ---- recorded on the device (2ede230, touch3 log 2026-10-18) ----------------------------------
# Raw lines are logged on change only: "ms:posT[/status]" = touched sample (status byte when not
# 0x80), "ms:pos-" = the release sample. Replayed as 10 ms samples holding the last value.

def replay(spec, truncated=False):
    ch = []
    for tok in spec.split():
        ms, v = tok.split(":")
        touched = "T" in v
        pos, _, st = v.replace("T", "").replace("-", "").partition("/")
        ch.append((int(ms), int(pos), touched, int(st, 16) if st else (0x80 if touched else 0)))
    end = -(-ch[-1][0] // 10) * 10
    out, j = [], 0
    for t in range(0, end, 10):
        while j + 1 < len(ch) - 1 and ch[j + 1][0] <= t:
            j += 1
        out.append((ch[j][1], True, t, ch[j][3]))
        if t == 0 and truncated:
            out.append("trunc")
    out.append((ch[-1][1], False, end, ch[-1][3]))
    return out


DEVICE = [
    # middle taps (display on/off)
    ("0:138T 98:138-", False, "tap_centre"),
    ("0:86T 23:87T 81:87-", False, "tap_centre"),
    ("0:131T 13:135T 63:135-", False, "tap_centre"),
    # edge taps (stock clicks)
    ("0:40T 21:42T 29:43T 128:43-", False, "tap_right"),
    ("0:10T 77:10-", False, "tap_right"),
    ("0:201T 9:205T 187:208T/88 206:211T/88 225:211-", False, "tap_left"),
    ("0:222T 3:223T 90:223-", False, "tap_left"),
    # swipes
    ("0:140T 29:85T/84 39:74T/84 48:64T/84 64:28T/84 69:14T/84 84:14-", False, "swipe_right"),
    ("0:67T 18:68T 71:69T 74:70T 91:72T/88 103:76T/88 111:81T/88 122:82T/88 135:87T/88 144:92T/88 "
     "151:98T/88 161:101T/88 173:113T/88 181:116T/88 192:124T/88 216:138T/88 227:143T/88 238:145T/88 "
     "248:151T/88 258:155T/88 266:157T/88 278:165T/88 285:166T/88 297:171T/88 319:176-", False, "swipe_left"),
    ("0:188T/88 8:201T/88 38:219T/88 48:219-", False, "swipe_left"),
    ("0:140T/88 11:143T/88 77:104T/84 88:94T/84 94:78T/84 120:52T/84 124:47-", False, "swipe_right"),
    # 10:04:16: the first read of this swipe failed (I2C timeout), it was seen from the middle
    # (both samples flagged ->left) and toggled the display as tap_centre
    ("0:142T/88 48:165T/88 139:165-", True, "swipe_left"),
    ("0:142T/88 48:165T/88 139:165-", False, "swipe_left"),
    # 10:03:43: a short slide off the left end, stock "chick" -> was tap_centre (display off)
    ("0:193T 21:192T 53:191T 61:188T 72:184T 81:176T/84 91:176-", False, "none"),
    # slow short drags: stock snaps back to the same page, nothing
    ("0:32T 64:67T/88 70:69T/88 92:92T/88 100:93T/88 205:93-", False, "none"),
    ("0:10T 8:36T 20:37T 31:38T 37:39T 114:40-", False, "none"),
]


@pytest.mark.parametrize("spec,truncated,expected", DEVICE)
def test_device_sequences(decoder, spec, truncated, expected):
    g = gestures(decoder, replay(spec, truncated))
    assert (g[0] if g else "none") == expected


def test_no_swipe_ever_toggles_the_display(decoder):
    # every recorded swipe, also seen from any later sample on (start missed, flagged or not)
    swipes = [spec for spec, _, exp in DEVICE if exp.startswith("swipe")]
    for spec in swipes:
        samples = replay(spec)
        touched = [x for x in samples if x[1]]
        for k in range(len(touched)):
            tail = [(p, True, t - touched[k][2], st) for p, _, t, st in touched[k:]]
            tail.append((samples[-1][0], False, samples[-1][2] - touched[k][2], 0))
            for trunc in (False, True):
                seq = tail[:1] + (["trunc"] if trunc else []) + tail[1:]
                assert "tap_centre" not in gestures(decoder, seq), (spec, k, trunc)
