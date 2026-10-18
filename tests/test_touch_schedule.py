"""Host tests for the CGDN1 touch-slider read schedule (components/cgdn1_touch/schedule.h).

A tiny C++ driver is compiled against the header with the system C++ compiler (skipped when none
is installed). It simulates a sampling task that ticks every millisecond and an ATtiny whose
behaviour is scripted per test: GPIO32 (ready) level, touched flag and I2C result over time. Stock
reference: Touch_Task FUN_400e2268 (arming at 0x400e2313, read errors ignored at 0x400e22c4 /
0x400e2618, an untouched sample clears armed at 0x400e2490, release at 0x400e2580 / 0x400e2607).
On the device GPIO32 is low exactly while the touched bit is set (touch3 log).
"""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
TOUCH_DIR = ROOT / "components" / "cgdn1_touch"

# Script lines: "<from_ms> <ready 0/1> <touched 0/1> <i2c_ok 0/1>" (piecewise constant), then
# "run <until_ms> <has_pin 0/1>". Output: one line per read "<t> R|D <ok>", notes "<t> N <1|2>".
DRIVER = r"""
#include "schedule.h"
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
using namespace touch_schedule;
struct Seg { unsigned long from; int ready, touched, ok; };
int main() {
  std::vector<Seg> segs;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd; in >> cmd;
    if (cmd == "run") {
      unsigned long until; int has_pin; in >> until >> has_pin;
      Scheduler s; s.set_has_ready_pin(has_pin != 0);
      bool active = false;
      for (unsigned long t = 1; t <= until; t++) {
        Seg cur{0, 0, 0, 1};
        for (auto &g : segs) if (g.from <= t) cur = g;
        Action a = s.poll((uint32_t) t, cur.ready != 0, active);
        Note n = s.take_note();
        if (n != Note::NONE) printf("%lu N %d\n", t, (int) n);
        if (a == Action::NONE) continue;
        if (a == Action::DIAG) {  // report only, no I2C
          s.diag_done((uint32_t) t);
          printf("%lu D 1\n", t);
          continue;
        }
        bool released = false;
        if (cur.ok) {
          released = active && !cur.touched;
          active = cur.touched != 0;
        }
        s.done((uint32_t) t, a, cur.ok != 0, active, released);
        printf("%lu R %d\n", t, cur.ok);
      }
      printf("end %d %u %u\n", (int) s.state(), (unsigned) s.errors_total(), (unsigned) s.ready_edges());
      segs.clear();
    } else {
      Seg g; g.from = std::stoul(cmd); in >> g.ready >> g.touched >> g.ok; segs.push_back(g);
    }
  }
}
"""


@pytest.fixture(scope="module")
def sim(tmp_path_factory):
    cxx = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        pytest.skip("no C++ compiler on PATH")
    d = tmp_path_factory.mktemp("touch_sched")
    src = d / "drv.cpp"
    src.write_text(DRIVER)
    exe = d / "drv"
    subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Werror", "-I", str(TOUCH_DIR), str(src), "-o", str(exe)],
                   check=True)
    return exe


def run(exe, segs, until, has_pin=True):
    """segs: [(from_ms, ready, touched, ok)]. Returns (reads [(t, kind, ok)], notes [(t, n)], end)."""
    inp = "".join(f"{f} {int(r)} {int(t)} {int(o)}\n" for f, r, t, o in segs) + f"run {until} {int(has_pin)}\n"
    out = subprocess.run([str(exe)], input=inp, capture_output=True, text=True, check=True).stdout
    reads, notes, end = [], [], None
    for line in out.splitlines():
        p = line.split()
        if p[0] == "end":
            end = dict(state=int(p[1]), errors=int(p[2]), edges=int(p[3]))
        elif p[1] == "N":
            notes.append((int(p[0]), int(p[2])))
        else:
            reads.append((int(p[0]), p[1], p[2] == "1"))
    return reads, notes, end


IDLE, ARMED, STUCK = 0, 1, 2


def test_idle_line_no_reads_until_diag(sim):
    reads, notes, end = run(sim, [(0, 0, 0, 1)], 29_999)
    assert reads == [] and notes == [] and end["state"] == IDLE


def test_diag_every_30s_while_unused(sim):
    # a report only: stock never reads an idle chip (on the device that read NACKs and makes the
    # IDF reset the bus), so nothing is read and no error counted
    reads, _, end = run(sim, [(0, 0, 0, 0)], 95_000)
    assert [(t, k) for t, k, _ in reads] == [(30_000, "D"), (60_000, "D"), (90_000, "D")]
    assert end["errors"] == 0


def test_failing_reads_never_disable(sim):
    # chip NACKs for 2 s, then a touch: still read and decoded (8894bc8 marked itself failed)
    segs = [(0, 1, 0, 0), (2000, 1, 1, 1), (2300, 0, 0, 1)]
    reads, _, end = run(sim, segs, 3000)
    ok_reads = [t for t, k, ok in reads if ok]
    assert ok_reads and ok_reads[0] < 2000 + 1100  # within one slow-backoff period
    assert any(2000 <= t < 2300 for t in ok_reads)
    assert end["errors"] > 0


def test_touch_reads_10ms_like_stock(sim):
    # device: GPIO32 low exactly while touched. First read at the edge, 10 ms cadence, the
    # release sample read, then nothing (stock disarms on the untouched sample)
    segs = [(0, 0, 0, 1), (1000, 1, 1, 1), (1123, 0, 0, 1)]
    reads, _, end = run(sim, segs, 2000)
    ts = [t for t, k, _ in reads if k == "R"]
    assert ts[0] == 1000
    assert all(b - a == 10 for a, b in zip(ts, ts[1:]))
    assert ts[-1] == 1130  # the release sample, then disarmed
    assert end["state"] == IDLE and end["edges"] == 1


def test_short_ready_pulse_reads_once_like_stock(sim):
    # GPIO32 low for 3 ms, no touch: one untouched sample, disarmed (stock 0x400e2490); a later
    # finger asserts GPIO32 again and is read from its edge
    segs = [(0, 0, 0, 1), (1000, 1, 0, 1), (1003, 0, 0, 1), (1050, 1, 1, 1), (1200, 0, 0, 1)]
    reads, _, end = run(sim, segs, 2000)
    ts = [t for t, k, _ in reads if k == "R"]
    assert ts[0] == 1000 and ts[1] == 1050
    assert all(b - a == 10 for a, b in zip(ts[1:], ts[2:]))
    assert ts[-1] == 1200
    assert end["state"] == IDLE and end["edges"] == 2


def test_armed_without_touch_disarms(sim):
    # line low without a touch for one tick: one read only (the old loop schedule read 20 times,
    # and reads with GPIO32 high fail on the device)
    segs = [(0, 0, 0, 1), (1000, 1, 0, 1), (1001, 0, 0, 1)]
    reads, notes, end = run(sim, segs, 5000)
    ts = [t for t, k, _ in reads if k == "R"]
    assert ts == [1000]
    assert notes == [] and end["state"] == IDLE and end["edges"] == 1


def test_ready_low_without_touch_reads_until_stuck(sim):
    # stock re-arms every tick while GPIO32 is low: 20 untouched samples, then the stuck poll
    segs = [(0, 0, 0, 1), (1000, 1, 0, 1), (1500, 0, 0, 1)]
    reads, notes, end = run(sim, segs, 2000)
    ts = [t for t, k, _ in reads if k == "R"]
    assert ts[:20] == list(range(1000, 1200, 10))
    assert all(b - a == 100 for a, b in zip(ts[19:], ts[20:]))
    assert [n for _, n in notes] == [1, 2] and end["state"] == IDLE


def test_stuck_ready_line_slow_poll_and_release(sim):
    segs = [(0, 1, 0, 1), (3000, 0, 0, 1)]
    reads, notes, end = run(sim, segs, 4000)
    ts = [t for t, k, _ in reads if k == "R"]
    assert ts[:20] == list(range(10, 210, 10))  # first read SAMPLE_MS after boot
    assert ts[20] - ts[19] == 100 and all(b - a == 100 for a, b in zip(ts[19:], ts[20:]))
    assert notes == [(201, 1), (3000, 2)]  # STUCK (logged on the next loop), UNSTUCK at GPIO32 high
    assert end["state"] == IDLE


def test_touch_while_stuck_is_decoded(sim):
    segs = [(0, 1, 0, 1), (1000, 1, 1, 1), (1300, 1, 0, 1)]
    reads, _, _ = run(sim, segs, 1600)
    ts = [t for t, k, _ in reads]
    in_touch = [t for t in ts if 1000 <= t < 1300]
    assert in_touch and all(b - a == 10 for a, b in zip(in_touch[1:], in_touch[2:]))
    after = [t for t in ts if t >= 1320]
    assert all(b - a == 100 for a, b in zip(after, after[1:]))  # back to the stuck poll


def test_error_backoff(sim):
    segs = [(0, 1, 0, 0)]
    reads, _, end = run(sim, segs, 3000)
    ts = [t for t, k, _ in reads]
    gaps = [b - a for a, b in zip(ts, ts[1:])]
    assert gaps[:4] == [10, 10, 10, 10] and gaps[4] == 1000  # next tick like stock; slow after 5 in a row
    assert end["errors"] == len(ts)


def test_no_ready_pin_polls(sim):
    reads, _, _ = run(sim, [(0, 0, 0, 1)], 200, has_pin=False)
    ts = [t for t, k, _ in reads]
    assert ts[0] == 10 and all(b - a == 20 for a, b in zip(ts, ts[1:]))
    assert all(k == "R" for _, k, _ in reads)
