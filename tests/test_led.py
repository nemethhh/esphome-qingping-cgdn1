"""Host tests for the CGDN1 RGB status LED logic (components/cgdn1_led/led.h).

A tiny C++ driver is compiled against the header with the system C++ compiler (skipped when none
is installed) and fed commands over stdin. Expected values are the stock 4.8.5 ones
(calculate_rgb_led_status FUN_400e63a4, colour table FUN_400e7f90, pwm_set FUN_400edac4,
orange blink FUN_400e7c50); see the header for the addresses.
"""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
LED_DIR = ROOT / "components" / "cgdn1_led"

DRIVER = r"""
#include "led.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
using namespace cgdn1_led;
static float num(std::istringstream &in) { std::string t; in >> t; return strtof(t.c_str(), nullptr); }
static int inum(std::istringstream &in) { int v; in >> v; return v; }
int main() {
  Engine e;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd; in >> cmd;
    if (cmd == "reset") { e = Engine(); printf("ok\n"); }
    else if (cmd == "P") { printf("%d\n", pct_to_duty(inum(in))); }
    else if (cmd == "L") {
      int s = inum(in); float v = num(in); int st = inum(in);
      printf("%s\n", colour_name(level((Sensor) s, v, (PmStandard) st)));
    } else if (cmd == "D") {
      Rgb d = duty((Colour) inum(in));
      printf("%d %d %d\n", d.r, d.g, d.b);
    } else if (cmd == "S") {
      Rgb d = e.step((Colour) inum(in));
      printf("%s %d %d %d\n", colour_name(e.colour()), d.r, d.g, d.b);
    } else if (cmd == "T") {
      Inputs x;
      x.enabled = inum(in); x.powering_off = inum(in); x.display_on = inum(in); x.test_pattern = inum(in);
      x.ota = inum(in); x.sensor = (Sensor) inum(in); x.value = num(in); x.warming_up = inum(in);
      x.standard = (PmStandard) inum(in);
      Rgb d = e.tick(x);
      printf("%s %d %d %d\n", colour_name(e.colour()), d.r, d.g, d.b);
    }
  }
}
"""

NONE, PM25, PM10, CO2, TEMP, HUMI, PM1 = range(7)
CHINA, US, EU = range(3)
COLOURS = ["off", "white", "orange_blink", "blue", "green", "yellow", "orange", "red", "deep_red", "purple"]
C = {name: i for i, name in enumerate(COLOURS)}


@pytest.fixture(scope="module")
def drv(tmp_path_factory):
    cxx = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        pytest.skip("no C++ compiler on PATH")
    d = tmp_path_factory.mktemp("led")
    src = d / "drv.cpp"
    src.write_text(DRIVER)
    exe = d / "drv"
    subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Werror", "-I", str(LED_DIR), str(src), "-o", str(exe)],
                   check=True)
    return exe


def run(exe, lines):
    out = subprocess.run([str(exe)], input="".join(l + "\n" for l in lines), capture_output=True, text=True,
                         check=True).stdout
    return out.splitlines()


def levels(exe, cases):
    """cases: list of (sensor, value, standard) -> list of colour names."""
    return run(exe, [f"L {s} {v} {st}" for s, v, st in cases])


def check(exe, sensor, std, expected):
    """expected: list of (value, colour)."""
    got = levels(exe, [(sensor, v, std) for v, _ in expected])
    assert got == [c for _, c in expected], list(zip([v for v, _ in expected], got))


# ---- duties (FUN_400edac4: pct ? 1024 / (100 / pct) : 0) --------------------------------------

def test_pct_to_duty_integer_division(drv):
    pcts = [0, 1, 2, 4, 9, 25, 26, 30, 33, 34, 35, 40, 45, 49, 50, 51, 70, 100]
    got = [int(x) for x in run(drv, [f"P {p}" for p in pcts])]
    assert got == [0 if p == 0 else 1024 // (100 // p) for p in pcts]
    assert dict(zip(pcts, got))[50] == 512 and dict(zip(pcts, got))[51] == 1024


def test_colour_duties_match_stock(drv):
    expected = {
        "off": (0, 0, 0),
        "white": (512, 512, 512),          # 35, 35, 40 %
        "orange_blink": (1024, 341, 93),   # on-phase 70, 30, 9 %
        "blue": (0, 512, 1024),            # 0, 45, 100
        "green": (40, 1024, 20),           # 4, 100, 2
        "yellow": (512, 512, 0),           # 50, 50, 0
        "orange": (1024, 341, 0),          # 75, 30, 0
        "red": (1024, 0, 0),               # 85, 0, 0
        "deep_red": (1024, 0, 512),        # 51, 0, 49
        "purple": (256, 0, 1024),          # 25, 0, 75
    }
    got = run(drv, [f"D {C[n]}" for n in COLOURS])
    assert {n: tuple(map(int, g.split())) for n, g in zip(COLOURS, got)} == expected


def test_esphome_13bit_scaling_is_exact():
    # cgdn1.yaml writes d/1024 to 13-bit ledc outputs: round(d/1024 * 8191) must be 8*d (or full on).
    for d in (20, 40, 93, 256, 341, 512):
        assert round(d / 1024 * 8191) == 8 * d


# ---- level thresholds (FUN_400e63a4) ----------------------------------------------------------

def test_pm25_china(drv):
    check(drv, PM25, CHINA, [(0, "green"), (35, "green"), (36, "yellow"), (75, "yellow"), (76, "orange"),
                             (115, "orange"), (116, "red"), (150, "red"), (151, "deep_red"), (250, "deep_red"),
                             (251, "purple"), (999, "purple")])


def test_pm25_us(drv):
    check(drv, PM25, US, [(12, "green"), (13, "yellow"), (35, "yellow"), (36, "orange"), (55, "orange"),
                          (56, "red"), (150, "red"), (151, "deep_red"), (250, "deep_red"), (251, "purple")])


def test_pm25_eu(drv):
    check(drv, PM25, EU, [(14, "green"), (15, "yellow"), (29, "yellow"), (30, "orange"), (54, "orange"),
                          (55, "red"), (109, "red"), (110, "deep_red"), (999, "deep_red"),
                          (10000, "deep_red"), (10001, "purple")])


def test_pm25_rounds_like_display(drv):
    check(drv, PM25, CHINA, [(35.4, "green"), (35.5, "yellow")])


def test_pm1_uses_pm25_thresholds(drv):
    # PM1.0 is ours (stock shows no PM1.0): no standard of its own, so the PM2.5 table ('<=').
    check(drv, PM1, CHINA, [(35, "green"), (36, "yellow"), (250, "deep_red"), (251, "purple")])
    check(drv, PM1, US, [(12, "green"), (13, "yellow"), (55, "orange"), (56, "red")])
    check(drv, PM1, EU, [(14, "green"), (15, "yellow"), (109, "red"), (999, "deep_red")])


def test_pm10_china_strict(drv):
    check(drv, PM10, CHINA, [(0, "green"), (49, "green"), (50, "yellow"), (149, "yellow"), (150, "orange"),
                             (249, "orange"), (250, "red"), (349, "red"), (350, "deep_red"), (419, "deep_red"),
                             (420, "purple")])


def test_pm10_us(drv):
    check(drv, PM10, US, [(53, "green"), (54, "yellow"), (153, "yellow"), (154, "orange"), (253, "orange"),
                          (254, "red"), (353, "red"), (354, "deep_red"), (423, "deep_red"), (424, "purple")])


def test_pm10_eu(drv):
    check(drv, PM10, EU, [(23, "green"), (24, "yellow"), (48, "yellow"), (49, "orange"), (88, "orange"),
                          (89, "red"), (178, "red"), (179, "deep_red"), (9999, "deep_red"), (10000, "purple")])


def test_unknown_standard_falls_back_to_china(drv):
    assert levels(drv, [(PM25, 36, 7), (PM10, 50, 7)]) == ["yellow", "yellow"]


def test_co2(drv):
    check(drv, CO2, CHINA, [(400, "green"), (1000, "green"), (1001, "orange"), (2000, "orange"),
                            (2001, "red"), (3000, "red"), (3001, "deep_red"), (40000, "deep_red")])
    # PM standard does not affect CO2
    check(drv, CO2, US, [(1000, "green"), (1001, "orange")])


def test_temperature(drv):
    check(drv, TEMP, CHINA, [(-5, "blue"), (22.99, "blue"), (23.0, "green"), (26.49, "green"), (26.5, "yellow"),
                             (27.99, "yellow"), (28.0, "orange"), (40, "orange")])


def test_humidity(drv):
    check(drv, HUMI, CHINA, [(0, "orange"), (19.99, "orange"), (20.0, "yellow"), (39.99, "yellow"),
                             (40.0, "green"), (59.99, "green"), (60.0, "blue"), (99.9, "blue")])


def test_nan_is_orange_blink(drv):
    assert levels(drv, [(s, "nan", CHINA) for s in (PM25, PM10, CO2, TEMP, HUMI)]) == ["orange_blink"] * 5


def test_no_sensor_is_off(drv):
    assert levels(drv, [(NONE, 500, CHINA), (NONE, "nan", CHINA)]) == ["off", "off"]


# ---- state priority -----------------------------------------------------------------------------

def tick(enabled=1, poff=0, disp=1, tp=0, ota=0, sensor=CO2, value=500, warm=0, std=CHINA):
    return f"T {enabled} {poff} {disp} {tp} {ota} {sensor} {value} {warm} {std}"


def colours(exe, ticks):
    return [l.split()[0] for l in run(exe, ["reset"] + ticks)[1:]]


def test_priority(drv):
    assert colours(drv, [
        tick(),                              # level
        tick(ota=1),                         # OTA beats the level
        tick(ota=1, tp=1),                   # test pattern beats OTA
        tick(ota=1, tp=1, disp=0),           # display off
        tick(ota=1, poff=1),                 # power-off
        tick(ota=1, enabled=0),              # LED switch off
        tick(ota=1, sensor=NONE),            # OTA on the boot screen: white
        tick(sensor=NONE),                   # boot screen: off
        tick(value=1500),
    ]) == ["green", "white", "off", "off", "off", "off", "white", "off", "orange"]


def test_warming_up(drv):
    assert colours(drv, [tick(value="nan", warm=1), tick(value=900, warm=1), tick(value="nan", warm=0)]) == [
        "off", "green", "orange_blink"]


def test_standard_passed_through(drv):
    assert colours(drv, [tick(sensor=PM25, value=13, std=CHINA), tick(sensor=PM25, value=13, std=US)]) == [
        "green", "yellow"]


def test_tick_outputs_duties(drv):
    out = run(drv, ["reset", tick(sensor=TEMP, value=20), tick(sensor=HUMI, value=10), tick(disp=0)])[1:]
    assert out == ["blue 0 512 1024", "orange 1024 341 0", "off 0 0 0"]


# ---- orange blink (FUN_400e7c50) --------------------------------------------------------------

def test_blink_timing(drv):
    # Enter from green: 2 ticks keep green, tick 2 off, then on 2 / off 3 (200 ms on, 300 ms off).
    ON, OFF, GREEN = "1024 341 93", "0 0 0", "40 1024 20"
    out = run(drv, ["reset", f"S {C['green']}"] + [f"S {C['orange_blink']}"] * 17)[2:]
    duties = [" ".join(l.split()[1:]) for l in out]
    assert duties[:2] == [GREEN, GREEN]
    assert duties[2] == OFF
    cycle = [ON, ON, OFF, OFF, OFF]
    assert duties[3:] == (cycle * 3)[:14]
    # 100 ms ticks: period 500 ms, 40 % on
    assert duties[3:8].count(ON) == 2


def test_blink_restarts_on_reentry(drv):
    ON, OFF = "1024 341 93", "0 0 0"
    seq = ["reset"] + [f"S {C['orange_blink']}"] * 4 + [f"S {C['off']}"] + [f"S {C['orange_blink']}"] * 4
    duties = [" ".join(l.split()[1:]) for l in run(drv, seq)[1:]]
    # from off: off, off, off, ON; leave; re-enter from off -> same pattern again
    assert duties[:4] == [OFF, OFF, OFF, ON]
    assert duties[4] == OFF
    assert duties[5:] == [OFF, OFF, OFF, ON]


def test_blink_via_nan_tick(drv):
    out = colours(drv, [tick(value="nan")] * 3)
    assert out == ["orange_blink"] * 3

