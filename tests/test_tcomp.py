"""Host tests for the stock self-heating compensation port (components/cgdn1_tcomp/tcomp.h).

A tiny C++ driver is compiled against the header with the system C++ compiler (skipped
when none is installed) and scripted over stdin. Expected values are recomputed here
from the constants in the static RE (stock 4.8.5, FUN_400e8d9c and callees), so the C++
port is checked against an independent transcription of the formulas.
"""
import math
import shutil
import subprocess

import pytest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TCOMP_DIR = ROOT / "components" / "cgdn1_tcomp"

# ---- reference curves (DROM 0x3f4070e8.., literal 0x400d19ac): (k + a1 x + a2 x^2 + a3 x^3)/1e4
PRE = (1200, (10.288467313132514, -24778.832215649065),
       ((-63.89875545624044, 0.038722865402681175, -9.578169525070734e-06),
        (-14.76911061946425, 0.002048246327797731, -9.423988878154432e-08)))
CHG = (2000, (-5.895304541018959, -50136.197864496324),
       ((-111.93648927230642, 0.09037562449514912, -2.8655718423920007e-05),
        (-6.100628223515754, 0.0006464288432308507, 0.0)))
CDN = (6000, (-60292.9664820073, -302448.55028207874),
       ((6.841134842508077, -0.0016753264405149615, 2.2600454574472906e-07),
        (84.8684530552672, -0.007906957677746823, 2.438464861143075e-07)))


def curve(c, x):
    x0, k, a = c
    s = 1 if x > x0 else 0
    return (k[s] + a[s][0] * x + a[s][1] * x * x + a[s][2] * x ** 3) / 10000


def dis(x):
    x = min(x, 3600)
    return (16.32204328517696 + 8.456100837633594 * x - 0.004002620029362993 * x * x
            + 5.993886915689473e-07 * x ** 3) / 10000


def rh_ref(rh_raw, t_raw, t_h):
    rh = rh_raw * math.exp((t_raw - t_h) * 4283.78 / (t_h + 220.12) / (t_raw + 220.12))
    if rh_raw > 55.0:
        rh *= 0.9
    return min(rh, 99.9)


DRIVER = r"""
#include "tcomp.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <iostream>
#include <sstream>
using namespace tcomp;
static void pstate(const State &s) {
  printf("S %d %d %d %d %d %d %.6f\n", s.on, s.chg, s.dis, s.bacc, s.cst, s.bst, s.last);
}
int main() {
  State s; Engine e; Saved sv; Out o{0, 0, 0};
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd; in >> cmd;
    if (cmd == "reset") { s = State(); e = Engine(); sv = Saved(); printf("ok\n"); }
    else if (cmd == "init") { int a, b, c, d, t; in >> a >> b >> c >> d >> t; init(s, a, b, c, d, t); pstate(s); }
    else if (cmd == "set") { int on, chg, dis, bacc, cst, bst; float last;
      in >> on >> chg >> dis >> bacc >> cst >> bst >> last;
      s.on = on; s.chg = chg; s.dis = dis; s.bacc = bacc; s.cst = cst; s.bst = bst; s.last = last; pstate(s); }
    else if (cmd == "run") {  // run N t rh cs bt pct [toff tzoom rhoff rhzoom]
      long n; float t, rh; int cs, bt, pct; Calib c; in >> n >> t >> rh >> cs >> bt >> pct;
      in >> c.t_off_c100 >> c.t_zoom >> c.rh_off_d10 >> c.rh_zoom;
      for (long i = 0; i < n; i++) o = step(s, t, rh, cs, bt, pct, c);
      printf("O %.6f %.6f %.6f\n", o.t, o.rh, o.t_h); }
    else if (cmd == "state") pstate(s);
    else if (cmd == "cs") { int prev, usb; std::string v, i; in >> prev >> usb >> v >> i;
      printf("C %d\n", charge_state(prev, usb != 0, std::stof(v), std::stof(i))); }
    else if (cmd == "curve") { std::string n; int x; in >> n >> x;
      double y = n == "pre" ? preheating(x) : n == "chg" ? charging(x) : n == "cdn" ? chargingdone(x)
               : n == "dis" ? discharging(x) : bright(x);
      printf("Y %.9f\n", y); }
    else if (cmd == "conv") { int x; in >> x; printf("V %d %d\n", conv_c2d(x), conv_d2c(x)); }
    else if (cmd == "etick") {  // etick N t rh usb vbat_mv ichg pct  (NaN allowed as 'nan')
      long n; std::string t, rh, v, i, p; int usb; in >> n >> t >> rh >> usb >> v >> i >> p;
      Inputs x; x.t_raw = std::stof(t); x.rh_raw = std::stof(rh); x.usb = usb != 0;
      x.vbat_mv = std::stof(v); x.ichg_ma = std::stof(i); x.batt_pct = std::stof(p);
      bool ok = false;
      for (long k = 0; k < n; k++) ok = e.tick(x, sv);
      printf("E %d %d %d %.6f %.6f\n", ok ? 1 : 0, e.ready() ? 1 : 0, e.cstate(), e.out().t, e.out().rh); }
    else if (cmd == "estate") pstate(e.state());
    else if (cmd == "save") { sv = e.snapshot(); printf("V %d %d %d %d %d\n", sv.magic, sv.on, sv.bright, sv.t100, sv.chg); }
    else if (cmd == "fresh") { e = Engine(); printf("ok\n"); }
    else printf("? %s\n", cmd.c_str());
    fflush(stdout);
  }
}
"""


@pytest.fixture(scope="module")
def drv(tmp_path_factory):
    cxx = next((c for c in ("c++", "g++", "clang++") if shutil.which(c)), None)
    if cxx is None:
        pytest.skip("no C++ compiler on PATH")
    d = tmp_path_factory.mktemp("tcomp")
    src = d / "driver.cpp"
    src.write_text(DRIVER)
    exe = d / "driver"
    subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Werror", "-I", str(TCOMP_DIR), str(src), "-o", str(exe)],
                   check=True, capture_output=True, text=True)
    p = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def q(cmd):
        p.stdin.write(cmd + "\n")
        p.stdin.flush()
        out = p.stdout.readline().split()
        assert out and out[0] != "?", f"driver rejected {cmd!r}"
        return [float(v) for v in out[1:]]

    yield q
    p.stdin.close()
    p.wait(timeout=5)


def run(q, n, t, rh, cs, bt=0, pct=100, cal=(0, 0, 0, 0)):
    return q(f"run {n} {t} {rh} {cs} {bt} {pct} " + " ".join(map(str, cal)))


def state(q):
    on, chg, dis_, bacc, cst, bst, last = q("state")
    return dict(on=on, chg=chg, dis=dis_, bacc=bacc, cst=cst, bst=bst, last=last)


# ---------------------------------------------------------------------------------------------
def test_curves_match_findings_table(drv):
    # values listed in the RE findings (section 3)
    table = {"pre": {0: 0.00, 60: -0.37, 300: -1.59, 600: -2.65, 1200: -3.75, 1201: -3.97,
                     1800: -4.53, 3600: -5.58, 6000: -6.00, 8000: -6.01},
             "chg": {500: -3.70, 1000: -5.02, 2000: -9.16, 2001: -5.98, 4000: -6.42, 7000: -6.12},
             "cdn": {0: -6.03, 3000: -4.88, 6000: -3.07, 6001: -2.52, 8000: -0.47, 13000: 0.03},
             "dis": {0: 0.00, 300: 0.22, 600: 0.38, 1200: 0.54, 3600: 0.65, 9999: 0.65},
             "bright": {0: 0.0, 50: -0.5, 100: -1.0, 150: -1.0}}
    ref = {"pre": lambda x: curve(PRE, x), "chg": lambda x: curve(CHG, x),
           "cdn": lambda x: curve(CDN, x), "dis": dis, "bright": lambda b: -min(b, 100) / 100}
    for name, pts in table.items():
        for x, y in pts.items():
            got = drv(f"curve {name} {x}")[0]
            assert got == pytest.approx(y, abs=0.006), (name, x)
            assert got == pytest.approx(ref[name](x), abs=1e-9), (name, x)


def test_steady_state_usb_full_display_off(drv):
    """Reference case: on USB, charge done, battery full, no display, raw 29.1 C / 33 %RH."""
    drv("reset")
    drv("init 0 0 2910 -1 2910")  # nothing saved -> cold
    t, rh, th = run(drv, 3 * 3600, 29.1, 33.0, 2)
    exp_th = 29.1 + curve(PRE, 8000) + dis(0)
    exp_t = exp_th + curve(CDN, 13000)
    assert exp_t == pytest.approx(23.12, abs=0.005)
    assert t == pytest.approx(exp_t, abs=0.005)
    assert t == pytest.approx(23.12, abs=0.01)
    assert th == pytest.approx(exp_th, abs=0.005)
    assert rh == pytest.approx(rh_ref(33.0, 29.1, exp_th), abs=0.02)
    assert rh == pytest.approx(50.5, abs=0.3)
    s = state(drv)
    assert (s["on"], s["chg"], s["dis"], s["cst"]) == (8000, 0, 0, 2)


def test_steady_state_display_on_matches_stock(drv):
    drv("reset")
    drv("init 0 100 2910 -1 2910")
    t, rh, _ = run(drv, 3 * 3600, 29.1, 33.0, 2, bt=100)
    assert t == pytest.approx(22.12, abs=0.01)  # = raw - 6.98 (display -1.00 C)
    assert rh == pytest.approx(54.3, abs=0.2)


def test_warmup_curve_usb(drv):
    drv("reset")
    drv("init 0 0 2910 -1 2910")
    done = 0
    for t_s in (60, 600, 1200, 1800, 3600, 7200):
        t, _, _ = run(drv, t_s - done, 29.1, 33.0, 2)
        done = t_s
        assert t == pytest.approx(29.1 + curve(PRE, t_s) + curve(CDN, 13000), abs=0.01), t_s


def test_on_battery_curve(drv):
    drv("reset")
    drv("init 0 0 2500 -1 2500")
    done = 0
    for t_s in (60, 600, 1800, 3600, 7200):
        t, _, th = run(drv, t_s - done, 25.0, 40.0, 0)
        done = t_s
        exp_th = 25.0 + curve(PRE, min(t_s, 8000)) + dis(min(t_s, 3600))
        assert th == pytest.approx(exp_th, abs=0.005), t_s
        assert t == pytest.approx(exp_th + curve(CDN, 13000), abs=0.01), t_s
    s = state(drv)
    assert (s["dis"], s["cst"]) == (3600, 0)
    # back on USB (charge done): dis counts down 1/s
    run(drv, 600, 25.0, 40.0, 2)
    assert state(drv)["dis"] == 3000


def test_charging_term_and_bst(drv):
    drv("reset")
    drv("set 8000 0 3600 0 0 0 -100")  # warm, long on battery
    run(drv, 1, 25.0, 40.0, 1, pct=40)  # battery -> charging at 40 %: bst latched (x=13000 > 9100)
    s = state(drv)
    assert s["bst"] == 40 and s["cst"] == 1
    # chg grows by trunc(200/(200-40)) = 1 per s (from the d2c-converted start)
    c0 = s["chg"]
    run(drv, 1000, 25.0, 40.0, 1, pct=40)
    s = state(drv)
    assert s["chg"] == c0 + 1000
    assert s["dis"] == 3600 - 2 * 1001  # dis counts down 2/s while charging
    t, _, th = run(drv, 1, 25.0, 40.0, 1, pct=40)
    chg = state(drv)["chg"]
    assert t == pytest.approx(th + (300 - 40) * curve(CHG, chg) / 300, abs=0.005)
    # charging -> done converts the counter into the charge-done domain
    run(drv, 1, 25.0, 40.0, 2)
    s = state(drv)
    assert s["cst"] == 2 and 0 <= s["chg"] <= 7000


def test_domain_conversions(drv):
    c2d, d2c = drv("conv 7000")
    assert c2d == 7000
    c2d, d2c = drv("conv 13000")
    assert d2c == 0
    # d2c(x): first u (step 10) with charging(u) < chargingdone(x)
    x = 6000
    v = curve(CDN, x)
    exp = next(u for u in range(0, 7000, 10) if curve(CHG, u) < v)
    assert drv(f"conv {x}")[1] == exp


def test_slew_limit(drv):
    drv("reset")
    drv("init 0 0 2910 -1 2910")
    t0, _, _ = run(drv, 9000, 29.1, 33.0, 2)  # settled at ~23.12
    t1, _, th1 = run(drv, 1, 35.0, 33.0, 2)  # +5.9 C jump -> only +0.3
    assert t1 == pytest.approx(t0 + 0.3, abs=1e-4)
    target = th1 + curve(CDN, 13000)
    seq = [t1]
    for _ in range(12):
        seq.append(run(drv, 1, 35.0, 33.0, 2)[0])
    steps = [b - a for a, b in zip(seq, seq[1:])]
    n_limited = sum(1 for d in steps if d == pytest.approx(0.3, abs=1e-4))
    assert n_limited >= 9  # 0.3 C/s while the gap is > 3 C ...
    assert seq[-1] == pytest.approx(target, abs=1e-3)  # ... then it snaps to the target
    # same downwards
    t2, _, _ = run(drv, 1, 25.0, 33.0, 2)
    assert t2 == pytest.approx(seq[-1] - 0.3, abs=1e-4)
    # a jump of <= 3 C passes through unlimited
    run(drv, 100, 25.0, 33.0, 2)
    t3, _, th3 = run(drv, 1, 27.5, 33.0, 2)
    assert t3 == pytest.approx(th3 + curve(CDN, 13000), abs=1e-3)


def test_clamps(drv):
    drv("reset")
    drv("init 8000 0 2910 -1 2910")
    assert run(drv, 1, 60.0, 30.0, 2)[0] == 40.0  # on USB max 40
    drv("reset")
    drv("init 8000 0 2910 -1 2910")
    assert run(drv, 1, 60.0, 30.0, 0)[0] == 50.0  # on battery max 50
    drv("reset")
    drv("init 8000 0 2910 -1 2910")
    assert run(drv, 1, -10.0, 30.0, 2)[0] == 0.0  # never below 0
    drv("reset")
    drv("init 8000 0 2910 -1 2910")
    assert run(drv, 1, 29.1, 95.0, 2)[1] == pytest.approx(99.9, abs=1e-4)  # RH max 99.9


def test_rh_formula_and_055_step(drv):
    t_raw = 29.1
    for rh_in in (20.0, 33.0, 50.0, 55.0, 55.01, 56.0, 70.0):
        drv("reset")
        drv("init 8000 0 2910 -1 2910")
        _, rh, th = run(drv, 1, t_raw, rh_in, 2)
        assert th == pytest.approx(t_raw + curve(PRE, 8000) + dis(0), abs=1e-4)
        assert rh == pytest.approx(rh_ref(rh_in, t_raw, th), abs=0.01), rh_in
    # hand-computed: T_raw 29.10, T_h = 29.10 - 6.0093 + 0.0016 = 23.0922;
    # ratio = exp(6.0078*4283.78/(243.2122*249.22)) = 1.5290
    # 33.00 % -> 50.46 %; 55.00 % -> 84.09 %; 56.00 % (> 55: x0.9) -> 77.06 %
    for rh_in, rh_out in ((33.0, 50.46), (55.0, 84.09), (56.0, 77.06)):
        drv("reset")
        drv("init 8000 0 2910 -1 2910")
        assert run(drv, 1, 29.1, rh_in, 2)[1] == pytest.approx(rh_out, abs=0.02), rh_in


def test_user_calibration(drv):
    drv("reset")
    drv("init 8000 0 2910 -1 2910")
    t, rh, _ = run(drv, 1, 29.1, 33.0, 2)
    drv("reset")
    drv("init 8000 0 2910 -1 2910")
    t2, rh2, _ = run(drv, 1, 29.1, 33.0, 2, cal=(150, 0, -25, 0))  # +1.50 C, -2.5 %RH
    assert t2 == pytest.approx(t + 1.5, abs=1e-4)
    assert rh2 == pytest.approx(rh - 2.5, abs=1e-4)
    drv("reset")
    drv("init 8000 0 2910 -1 2910")
    t3, _, _ = run(drv, 1, 29.1, 33.0, 2, cal=(150, 10, 0, 0))  # zoom replaces the offset
    assert t3 == pytest.approx(t + (10 * t / 100 + 5) / 10, abs=1e-4)


def test_boot_reinit_rule(drv):
    p8000 = curve(PRE, 8000)
    # cooled by 2 C (< |preheat|): restart at the first 60 s grid t with preheat(t) < p - dT
    on, chg, _, bacc, cst, bst, last = drv("init 8000 100 2910 -1 2710")
    exp_on = next(i for i in range(0, 8000, 60) if curve(PRE, i) < p8000 + 2.0)
    assert on == exp_on and 1200 < on < 1500
    assert bacc == int((1 - (8000 - exp_on) / 8000) * 2000)
    assert (chg, cst, bst, last) == (0, 2, 0, -100)
    # cooled by more than the self-heat: cold
    assert drv("init 8000 100 2910 -1 2010")[0] == 0
    # warmer than at save: fully restored
    on, chg, _, bacc, cst, _, _ = drv("init 8000 100 2910 -1 2950")
    assert (on, bacc, cst) == (8000, 2000, 2)
    # charging counter restored (positive = was charging), minus the "lost" seconds
    on, chg, _, _, cst, _, _ = drv("init 8000 0 2910 5000 2810")
    assert cst == 1 and chg == max(0, 5000 - (8000 - on))
    on, chg, _, _, cst, _, _ = drv("init 3000 0 2910 4000 2910")
    assert (on, chg, cst) == (3000, 4000, 1)


def test_charge_state_derivation(drv):
    cs = lambda prev, usb, v, i: int(drv(f"cs {prev} {usb} {v} {i}")[0])
    assert cs(1, 0, 3900, 300) == 0  # no USB -> battery
    assert cs(0, 1, 3900, 300) == 1  # USB, Vbat <= 4050, current flowing -> charging
    assert cs(1, 1, 4050, 300) == 1
    assert cs(1, 1, 4060, 300) == 2  # above 4.05 V -> done (stock)
    assert cs(2, 1, 4000, 300) == 2  # hysteresis: after done, only <= 3949 mV restarts
    assert cs(2, 1, 3949, 300) == 1
    assert cs(0, 1, 3900, 5) == 2  # no measurable current -> done (stock: GPIO35 high)
    # "Charge current" is floor-corrected in the YAML (raw ADC floor 39 mA -> 0): an idle charger
    # reads 0 and must not count as charging; real current just above the 20 mA cut does.
    assert cs(0, 1, 3900, 0) == 2
    assert cs(0, 1, 3900, 21) == 1
    assert cs(0, 1, float("nan"), 300) == 2  # Vbat unknown -> stock default 2
    assert cs(0, 1, 3900, float("nan")) == 1  # current unknown -> Vbat-only


def test_engine_nan_restore_and_snapshot(drv):
    drv("reset")
    ok, ready, *_ = drv("etick 5 nan nan 1 4150 0 100")
    assert (ok, ready) == (0, 0)  # no reading: nothing stepped, nothing published
    ok, ready, cst, t, rh = drv("etick 7200 29.1 33 1 4150 0 100")
    assert (ok, ready, cst) == (1, 1, 2)
    assert t == pytest.approx(29.1 + curve(PRE, 7200) + curve(CDN, 13000), abs=0.01)
    magic, on, bright, t100, chg = drv("save")
    assert (magic, on, bright, t100, chg) == (0x7C01, 7200, 0, 2910, -1)
    # reboot after cooling 1 C: restored warm-ish, not cold
    drv("fresh")
    drv("etick 1 28.1 33 1 4150 0 100")
    on = drv("estate")[0]
    exp = next(i for i in range(0, 7200, 60) if curve(PRE, i) < curve(PRE, 7200) + 1.0)
    assert on == exp + 1
    # NaN after start: no step, but the engine stays ready
    ok, ready, *_ = drv("etick 1 nan nan 1 4150 0 100")
    assert (ok, ready) == (0, 1)
