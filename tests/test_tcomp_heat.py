"""Host tests for the display + Bluetooth self-heat model (components/cgdn1_tcomp/heat.h).

A tiny C++ driver is compiled against heat.h (and tcomp.h, which wires it into the Engine)
with the system C++ compiler (skipped when none is installed) and scripted over stdin.
The expected curves are recomputed here from the same recurrence, independently of the C++.
"""
import math
import re
import shutil
import subprocess

import pytest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TCOMP_DIR = ROOT / "components" / "cgdn1_tcomp"

DRIVER = r"""
#include "heat.h"
#include "tcomp.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <iostream>
#include <sstream>
static float rd(std::istringstream &in) { std::string v; in >> v; return std::stof(v); }
static void pparams(const tcomp::heat::Params &p) {
  printf("P %.9g %.9g %.9g %.9g %.9g %.9g %d\n", p.a6, p.a1, p.tau_d1, p.tau_d2, p.a_bt, p.tau_bt, p.bt_comp ? 1 : 0);
}
static void pstate(const tcomp::heat::State &s) { printf("S %.9g %.9g %.9g %.9g %.9g\n", s.p_a, s.p_b, s.q_a, s.q_b, s.b_bt); }
static void pengine(const tcomp::Engine &e, bool ok) {
  const tcomp::heat::State &h = e.heat_state();
  printf("E %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %d %d %d %d\n", e.out().t, e.out().rh, e.out().t_h,
         e.display_heat(), e.bt_heat(), h.p_a, h.p_b, h.q_a, h.q_b, h.b_bt, e.state().bacc, e.state().on, ok ? 1 : 0,
         e.heat_restored() ? 1 : 0);
}
static void rdsaved(std::istringstream &in, tcomp::heat::Saved &v) {  // magic p_a p_b q_a q_b b_bt (raw int16)
  v.magic = (uint16_t) rd(in); v.p_a = (int16_t) rd(in); v.p_b = (int16_t) rd(in); v.q_a = (int16_t) rd(in);
  v.q_b = (int16_t) rd(in); v.b_bt = (int16_t) rd(in);
}
int main() {
  using namespace tcomp::heat;
  Params p; State s; Saved sv;
  tcomp::Engine eng[2]; tcomp::Inputs ein[2]; tcomp::Saved tsv; tcomp::heat::Saved hsv;
  bool eok[2] = {false, false};
  for (auto &i : ein) { i.t_raw = 29.1f; i.rh_raw = 33.0f; i.usb = true; }
  tcomp::State bs; bool bs_init = false;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd; in >> cmd;
    if (cmd == "reset") { p = Params(); s = State(); sv = Saved(); printf("ok\n"); }
    else if (cmd == "params") {  // params a6 a1 tau_d1 tau_d2 a_bt tau_bt bt_comp (nan allowed)
      p.a6 = rd(in); p.a1 = rd(in); p.tau_d1 = rd(in); p.tau_d2 = rd(in); p.a_bt = rd(in); p.tau_bt = rd(in);
      int c; in >> c; p.bt_comp = c != 0; pparams(p); }
    else if (cmd == "sanitize") { p = sanitize(p); pparams(p); }
    else if (cmd == "defaults") { pparams(Params()); }
    else if (cmd == "weight") { printf("W %.9g\n", level_weight(rd(in))); }
    else if (cmd == "run") {  // run N display_on level ble -> final heats, peak and minimum display heat
      long n; int on, ble; in >> n >> on; float lv = rd(in); in >> ble;
      float peak = 0, mn = INFINITY;
      for (long i = 0; i < n; i++) {
        step(s, p, on != 0, lv, ble != 0);
        peak = std::fmax(peak, display_heat(s, p)); mn = std::fmin(mn, display_heat(s, p)); }
      printf("R %.9g %.9g %.9g %.9g\n", display_heat(s, p), bt_heat(s, p), peak, mn); }
    else if (cmd == "state") pstate(s);
    else if (cmd == "snap") { sv = snapshot(s); printf("V %d %d %d %d %d %d\n", sv.magic, sv.p_a, sv.p_b, sv.q_a, sv.q_b, sv.b_bt); }
    else if (cmd == "svset") { rdsaved(in, sv); printf("ok\n"); }
    else if (cmd == "restore") { s = restore(sv, rd(in)); pstate(s); }
    else if (cmd == "ereset") { eng[0] = tcomp::Engine(); eng[1] = tcomp::Engine(); tsv = tcomp::Saved(); hsv = tcomp::heat::Saved();
      bs_init = false; for (auto &i : ein) { i = tcomp::Inputs(); i.t_raw = 29.1f; i.rh_raw = 33.0f; i.usb = true; } printf("ok\n"); }
    else if (cmd == "eset") {  // eset idx name value
      int i; std::string k; in >> i >> k; float v = rd(in); tcomp::Inputs &x = ein[i];
      if (k == "t") x.t_raw = v; else if (k == "rh") x.rh_raw = v; else if (k == "bright") x.bright_target = (int) v; else if (k == "level") x.level = v;
      else if (k == "ble") x.ble = v != 0; else if (k == "measured") x.measured = v != 0; else if (k == "a6") x.heat.a6 = v;
      else if (k == "a1") x.heat.a1 = v; else if (k == "abt") x.heat.a_bt = v; else if (k == "btcomp") x.heat.bt_comp = v != 0;
      else if (k == "tau1") x.heat.tau_d1 = v; else if (k == "tau2") x.heat.tau_d2 = v; else if (k == "taubt") x.heat.tau_bt = v;
      else { printf("? %s\n", k.c_str()); continue; }
      printf("ok\n"); }
    else if (cmd == "etick") {  // etick idx n [hs]: hs=0 passes no heat record
      int i; long n; in >> i >> n; int hs = 1; in >> hs;
      for (long j = 0; j < n; j++) eok[i] = eng[i].tick(ein[i], tsv, hs ? &hsv : nullptr);
      pengine(eng[i], eok[i]); }
    else if (cmd == "esnap") { int i; in >> i; tsv = eng[i].snapshot(); hsv = eng[i].heat_snapshot(); printf("V %d %d\n", tsv.magic, hsv.magic); }
    else if (cmd == "efresh") { int i, hm; in >> i >> hm; eng[i] = tcomp::Engine(); if (!hm) hsv.magic = 0; printf("ok\n"); }
    else if (cmd == "hsvset") { rdsaved(in, hsv); printf("ok\n"); }
    else if (cmd == "tsvon") { tsv.on = (int16_t) rd(in); printf("ok\n"); }
    else if (cmd == "bare") {  // bare N: old-signature step() on a cold-start state, same inputs as engine 0
      long n; in >> n; const tcomp::Inputs &x = ein[0];
      if (!bs_init) { tcomp::init(bs, 0, 0, tcomp::Engine::t100(x.t_raw), -1, tcomp::Engine::t100(x.t_raw)); bs_init = true; }
      tcomp::Out o{};
      for (long j = 0; j < n; j++) o = tcomp::step(bs, x.t_raw, x.rh_raw, 2, x.bright_target, 100);
      printf("B %.9g %.9g %.9g\n", o.t, o.rh, o.t_h); }
    else if (cmd == "curves") { printf("C %.9g %.9g\n", tcomp::preheating(8000), tcomp::chargingdone(13000)); }
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
    d = tmp_path_factory.mktemp("tcomp_heat")
    src = d / "driver.cpp"
    src.write_text(DRIVER)
    exe = d / "driver"
    r = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Werror", "-I", str(TCOMP_DIR), str(src), "-o", str(exe)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    p = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def q(cmd):
        p.stdin.write(cmd + "\n")
        p.stdin.flush()
        out = p.stdout.readline().split()
        assert out and out[0] != "?", f"driver rejected {cmd!r}"
        return [float(v) for v in out[1:]]

    q("reset")
    yield q
    p.stdin.close()
    p.wait(timeout=5)


# ---- Python reference (independent of the C++) ----------------------------------------------
def heat_defaults():
    """heat::Params defaults as written in heat.h."""
    src = (TCOMP_DIR / "heat.h").read_text()
    body = src[src.index("struct Params {"):]
    body = body[:body.index("};")]
    return {k: float(v) for k, v in re.findall(r"float (\w+) = ([0-9.]+)f;", body)}


D = heat_defaults()
CONTRAST = (0x0F, 0x1A, 0x2C, 0x4A, 0x7C, 0xD0)  # OLED contrast byte for "Display brightness" 1..6


def w(level):
    """Level weight: 0 at brightness 1, 1 at brightness 6, linear in the contrast byte."""
    return (CONTRAST[level - 1] - 0x0F) / (0xD0 - 0x0F)


def cascade(n, u=1.0, a=0.0, b=0.0, tau1=D["tau_d1"], tau2=D["tau_d2"]):
    for _ in range(n):
        a += (u - a) / tau1
        b += (a - b) / tau2
    return a, b


def model(n, on, level, a6=D["a6"], a1=D["a1"], st=(0.0, 0.0, 0.0, 0.0), tau1=D["tau_d1"], tau2=D["tau_d2"]):
    """Superposition model: unit cascades p (drive on) and q (drive on * w(level)),
    display heat = a1 * p_b + (a6 - a1) * q_b. Returns (state, heat)."""
    pa, pb = cascade(n, 1.0 if on else 0.0, st[0], st[1], tau1, tau2)
    qa, qb = cascade(n, w(level) if on else 0.0, st[2], st[3], tau1, tau2)
    return (pa, pb, qa, qb), a1 * pb + (a6 - a1) * qb


def test_step_response_matches_cascade(drv):
    drv("reset")
    done = 0
    for t in (60, 300, 900, 1800):
        got = drv(f"run {t - done} 1 6 0")[0]
        done = t
        assert got == pytest.approx(D["a6"] * cascade(t)[1], abs=1e-4), t
        assert got == pytest.approx(model(t, 1, 6)[1], abs=1e-4), t
    b = got / D["a6"]
    assert b == pytest.approx(cascade(1800)[1], abs=1e-4)
    assert 0.75 < b < 0.95


def test_step_response_mid_level_matches_reference(drv):
    drv("reset")
    drv("params 2.0 0.5 90 800 0 800 1")
    st = (0.0, 0.0, 0.0, 0.0)
    for n, on, level in ((600, 1, 4), (900, 1, 2), (300, 0, 2), (1200, 1, 5)):
        st, ref = model(n, on, level, a6=2.0, a1=0.5, st=st, tau1=90.0, tau2=800.0)
        assert drv(f"run {n} {on} {level} 0")[0] == pytest.approx(ref, abs=1e-4), (n, on, level)
    assert drv("state")[:4] == pytest.approx(list(st), abs=1e-5)


def test_off_decays_symmetrically(drv):
    drv("reset")
    drv("run 10800 1 6 0")
    off = drv("run 900 0 6 0")[0]
    drv("reset")
    on = drv("run 900 1 6 0")[0]
    assert off + on == pytest.approx(D["a6"], abs=0.02)


def test_short_pulse_small(drv):
    drv("reset")
    peak = drv("run 30 1 6 0")[2]
    assert peak < 0.1
    back = drv("run 3600 0 6 0")[0]
    assert back < 0.01


def test_level_weight(drv):
    assert drv("weight 6")[0] == pytest.approx(1.0, abs=1e-6)
    assert drv("weight 1")[0] == pytest.approx(0.0, abs=1e-6)
    assert drv("weight 4")[0] == pytest.approx((0x4A - 0x0F) / (0xD0 - 0x0F), abs=1e-6)
    assert drv("weight nan")[0] == pytest.approx(1.0, abs=1e-6)
    assert drv("weight 9")[0] == pytest.approx(1.0, abs=1e-6)
    assert drv("weight 0")[0] == pytest.approx(0.0, abs=1e-6)
    for level in range(1, 7):
        assert drv(f"weight {level}")[0] == pytest.approx(w(level), abs=1e-6), level
    # steady state at level 4 interpolates between the heat at level 1 and at level 6
    drv("reset")
    drv("params 2.0 0.5 90 800 0 800 1")
    assert drv("run 20000 1 4 0")[0] == pytest.approx(0.5 + 1.5 * w(4), abs=1e-3)


def test_zero_and_inverted_heat(drv):
    # a6 = 0 does not zero the level-1 term: at level 1 the heat is a1
    drv("reset")
    drv("params 0 1.1 90 800 0 800 1")
    heat, _, _, mn = drv("run 20000 1 1 0")
    assert math.isfinite(heat) and heat == pytest.approx(1.1, abs=1e-3)
    assert mn >= 0.0
    heat, _, _, mn = drv("run 20000 1 6 0")
    assert math.isfinite(heat) and heat == pytest.approx(0.0, abs=1e-3)
    assert mn >= -1e-6
    # a1 > a6 (brightness 1 hotter than 6) is allowed: finite and never negative
    drv("reset")
    drv("params 2 3 90 800 0 800 1")
    heat, _, _, mn = drv("run 20000 1 1 0")
    assert math.isfinite(heat) and heat == pytest.approx(3.0, abs=1e-3) and mn >= 0.0
    heat, _, _, mn = drv("run 20000 1 6 0")
    assert math.isfinite(heat) and heat == pytest.approx(2.0, abs=1e-3) and mn >= 0.0
    heat, _, _, mn = drv("run 7200 0 6 0")
    assert math.isfinite(heat) and mn >= 0.0


def test_slider_rescale_one_step_any_level(drv):
    for level in (1, 3):
        drv("reset")
        drv("params 2.2 1.1 90 800 0 800 1")
        drv(f"run 20000 1 {level} 0")
        for a6, a1 in ((3.0, 1.1), (3.0, 0.4), (1.5, 2.0)):
            drv(f"params {a6} {a1} 90 800 0 800 1")
            want = a1 + (a6 - a1) * w(level)
            assert drv(f"run 1 1 {level} 0")[0] == pytest.approx(want, abs=0.01), (level, a6, a1)
            heat, _, peak, mn = drv(f"run 1800 1 {level} 0")
            assert heat == pytest.approx(want, abs=0.01), (level, a6, a1)
            assert peak <= want + 0.01 and mn >= want - 0.01, (level, a6, a1)


def test_a6_zero_and_back_no_reset(drv):
    drv("reset")
    drv("params 2.2 1.1 90 800 0 800 1")
    drv("run 10800 1 6 0")
    drv("params 0 1.1 90 800 0 800 1")
    heat, _, _, mn = drv("run 1800 1 6 0")
    assert math.isfinite(heat) and heat == pytest.approx(0.0, abs=0.01) and mn >= -0.01
    drv("params 2.2 1.1 90 800 0 800 1")
    assert drv("run 1 1 6 0")[0] == pytest.approx(2.2, abs=0.01)


def test_defaults_parsed_from_heat_h(drv):
    got = drv("defaults")
    assert got[:6] == pytest.approx([D[k] for k in ("a6", "a1", "tau_d1", "tau_d2", "a_bt", "tau_bt")], abs=1e-6)
    assert got[6] == 1


def test_sanitize(drv):
    drv("reset")
    defaults = drv("defaults")
    drv("params nan nan nan nan nan nan 1")
    assert drv("sanitize") == defaults
    drv("params 2.2 1.1 0 0 0 0 1")
    out = drv("sanitize")
    assert out[2] == 1 and out[3] == 1 and out[5] == 1
    drv("params -1 -1 90 800 -2 800 1")
    out = drv("sanitize")
    assert out[0] == 0 and out[1] == 0 and out[4] == 0


def test_ble_term(drv):
    drv("reset")
    drv("params 2.2 1.1 90 800 0.5 800 1")
    bt = drv("run 10800 0 6 1")[1]
    assert bt == pytest.approx(0.5, abs=0.01)
    drv("reset")
    drv("params 2.2 1.1 90 800 0.5 800 0")
    bt = drv("run 10800 0 6 1")[1]
    assert bt == 0.0
    assert drv("state")[4] > 0.9


def test_snapshot_restore(drv):
    drv("reset")
    drv("params 2.2 1.1 90 800 0.5 800 1")
    drv("run 1000 1 3 1")
    st = drv("state")
    snap = drv("snap")
    assert snap[0] == 0x4E02
    assert snap[1:] == pytest.approx([v * 10000 for v in st], abs=1)
    for scale in (1.0, 0.5):
        got = drv(f"restore {scale}")
        assert got == pytest.approx([v * scale for v in st], abs=1e-4)
    # the state is a unit fraction whatever the heat sizes: no int16 saturation at a1 >> a6
    drv("reset")
    drv("params 0.1 5 90 800 0 800 1")
    drv("run 20000 1 1 0")
    st = drv("state")
    drv("snap")
    assert drv("restore 1") == pytest.approx(st, abs=1e-4)


def test_restore_clamps(drv):
    drv("reset")
    # out-of-range fractions -> [0, 1], q never above p (q_b <= p_b keeps the heat >= 0)
    drv(f"svset {0x4E02} 15000 -5000 20000 3000 9000")
    assert drv("restore 1") == pytest.approx([1.0, 0.0, 1.0, 0.0, 0.9], abs=1e-6)
    assert drv("restore 1.5") == pytest.approx([1.0, 0.0, 1.0, 0.0, 0.9], abs=1e-6)
    assert drv("restore -1") == [0.0] * 5
    assert drv("restore nan") == [0.0] * 5
    drv(f"svset {0x4E02} 8000 6000 4000 3000 2000")
    assert drv("restore 2") == pytest.approx([0.8, 0.6, 0.4, 0.3, 0.2], abs=1e-6)


def eset(drv, i, **kw):
    for k, v in kw.items():
        drv(f"eset {i} {k} {v}")


ETICK_KEYS = ("t", "rh", "th", "dh", "bt", "pa", "pb", "qa", "qb", "c", "bacc", "on", "ok", "hr")


def etick(drv, i, n, hs=1):
    return dict(zip(ETICK_KEYS, drv(f"etick {i} {n} {hs}"), strict=True))


def test_stock_path_identical(drv):
    drv("ereset")
    eset(drv, 0, bright=100)
    r = etick(drv, 0, 3 * 3600)
    ref = drv("bare 10800")
    assert r["t"] == pytest.approx(ref[0], abs=1e-6)
    assert r["rh"] == pytest.approx(ref[1], abs=1e-6)  # stock RH transfer untouched
    assert r["th"] == pytest.approx(ref[2], abs=1e-6)
    assert r["t"] == pytest.approx(22.12, abs=0.01)


def rh_air(rh_raw, t_raw, t_air):
    """Vapour pressure conserved: RH_raw * e_s(T_raw) / e_s(T_air), Magnus over water (Sensirion)."""
    es = lambda t: math.exp(17.62 * t / (243.12 + t))  # noqa: E731
    return min(rh_raw * es(t_raw) / es(t_air), 99.9)


@pytest.mark.parametrize("rh_raw", (20.0, 33.0, 54.9, 55.1, 70.0))
def test_measured_rh_conserves_vapour_pressure(drv, rh_raw):
    drv("ereset")
    eset(drv, 0, bright=100, measured=1, rh=rh_raw)
    r = etick(drv, 0, 3 * 3600)
    # USB + charge done: the humidity transfer uses the full model T, charging term included
    assert r["th"] == pytest.approx(r["t"], abs=1e-4)
    assert r["rh"] == pytest.approx(rh_air(rh_raw, 29.1, r["t"]), abs=0.01)


def test_measured_rh_no_055_step(drv):
    out = {}
    for rh_raw in (54.99, 55.01):
        drv("ereset")
        eset(drv, 0, bright=100, measured=1, rh=rh_raw)
        out[rh_raw] = etick(drv, 0, 3 * 3600)["rh"]
    assert out[55.01] / out[54.99] == pytest.approx(55.01 / 54.99, abs=1e-4)


def test_measured_rh_below_stock(drv):
    """Stock's lambda' = 220.12 reads ~4-6 %RH above the conserved vapour pressure."""
    drv("ereset")
    eset(drv, 0, bright=100, measured=1)
    r = etick(drv, 0, 3 * 3600)
    stock = 33.0 * math.exp((29.1 - r["th"]) * 4283.78 / (r["th"] + 220.12) / (29.1 + 220.12))
    assert r["rh"] < stock - 4.0


def test_measured_replaces_bright(drv):
    drv("ereset")
    eset(drv, 0, bright=100, measured=1)
    r = etick(drv, 0, 3 * 3600)
    pre, cdn = drv("curves")
    assert r["dh"] == pytest.approx(D["a6"], abs=0.01)
    assert r["t"] == pytest.approx(29.1 + pre + cdn - r["dh"], abs=0.01)
    assert r["t"] - drv("bare 10800")[0] == pytest.approx(1.0 - D["a6"], abs=0.02)  # stock bright() = -1 C


def test_model_switch_no_reset(drv):
    drv("ereset")
    for i in (0, 1):
        eset(drv, i, bright=100)
    eset(drv, 0, measured=1)
    etick(drv, 0, 3600)
    etick(drv, 1, 3600)
    before = etick(drv, 0, 1)
    etick(drv, 1, 1)
    eset(drv, 0, measured=0)
    sw = etick(drv, 0, 1)
    ref = etick(drv, 1, 1)
    assert sw["t"] == pytest.approx(ref["t"], abs=1e-4)  # stock output, bright() term back
    assert sw["pb"] == pytest.approx(before["pb"], abs=1e-3)  # heat state kept running, not reset
    assert sw["pb"] > 0.3
    eset(drv, 0, measured=1)
    back = etick(drv, 0, 1)
    assert back["t"] == pytest.approx(before["t"], abs=0.01)


def test_slider_rescale(drv):
    drv("ereset")
    eset(drv, 0, bright=100, measured=1)
    r = etick(drv, 0, 3 * 3600)
    a6 = D["a6"] + 0.3
    eset(drv, 0, a6=a6)
    r2 = etick(drv, 0, 1)
    assert r2["pb"] == pytest.approx(r["pb"], abs=1e-3)
    assert r2["qb"] == pytest.approx(r["qb"], abs=1e-3)
    assert r2["dh"] == pytest.approx(D["a1"] * r2["pb"] + (a6 - D["a1"]) * r2["qb"], abs=1e-5)
    assert r2["dh"] == pytest.approx(a6, abs=0.02)
    assert math.isfinite(r2["t"]) and r2["t"] == pytest.approx(r["t"] - 0.3, abs=0.02)


def test_restore_heat_record(drv):
    drv("ereset")
    eset(drv, 0, bright=100, measured=1)
    r = etick(drv, 0, 3 * 3600)
    assert r["hr"] == 0  # first start: no records -> seeded
    assert drv("esnap 0") == [0x7C01, 0x4E02]
    # same raw T: no stock scaling
    drv("efresh 0 1")
    f = etick(drv, 0, 1)
    assert f["hr"] == 1
    for k in ("pa", "pb", "qa", "qb"):
        assert f[k] == pytest.approx(r[k], abs=1e-3), k
    # raw 1 C lower: stock init shortens `on`; heat record scaled by the same factor
    drv("efresh 0 1")
    eset(drv, 0, t=28.1)
    f = etick(drv, 0, 1)
    assert f["hr"] == 1
    scale = 1 - (8000 - (f["on"] - 1)) / 8000.0
    assert 0.0 < scale < 1.0
    assert f["pb"] == pytest.approx(r["pb"] * scale, abs=2e-3)
    a0 = r["pa"] * scale  # the first tick then relaxes p_a one step toward on = 1
    assert f["pa"] == pytest.approx(a0 + (1.0 - a0) / D["tau_d1"], abs=1e-3)
    # heat record magic 0: seed from bacc (level 6 -> q = p)
    drv("efresh 0 0")
    eset(drv, 0, t=29.1)
    f = etick(drv, 0, 1)
    assert f["hr"] == 0
    for k in ("pa", "pb", "qa", "qb"):
        assert f[k] == pytest.approx(f["bacc"] / 2000.0, abs=1e-3), k
    assert f["c"] == 0.0


def test_seed_at_level3_no_heat_record(drv):
    drv("ereset")
    eset(drv, 0, bright=100, measured=1)
    etick(drv, 0, 3 * 3600)
    drv("esnap 0")
    drv("efresh 0 0")
    eset(drv, 0, level=3)
    f = etick(drv, 0, 1)
    assert f["hr"] == 0
    assert f["pb"] == pytest.approx(f["bacc"] / 2000.0, abs=1e-3)
    assert f["pb"] > 0.9
    assert f["qa"] == pytest.approx(f["pa"] * w(3), abs=1e-5)
    assert f["qb"] == pytest.approx(f["pb"] * w(3), abs=1e-5)
    assert f["dh"] == pytest.approx(D["a1"] + (D["a6"] - D["a1"]) * w(3), abs=0.01)


def test_restore_scale_and_fractions_clamped(drv):
    drv("ereset")
    eset(drv, 0, bright=100, measured=1)
    etick(drv, 0, 3 * 3600)
    drv("esnap 0")
    # corrupted stock record: saved on 20000 > ON_MAX -> init caps on at 8000 -> raw scale -0.5
    drv("tsvon 20000")
    drv("efresh 0 1")
    f = etick(drv, 0, 1)
    assert f["hr"] == 1
    assert f["pa"] == pytest.approx(1.0 / D["tau_d1"], abs=1e-5)  # scale 0, then one step toward 1
    assert 0.0 <= f["pb"] < 1e-4
    assert 0.0 <= f["dh"] < 1e-3
    # corrupted heat record fractions -> [0, 1], q <= p
    drv("esnap 0")
    drv(f"hsvset {0x4E02} 30000 -30000 30000 30000 -5")
    drv("efresh 0 1")
    f = etick(drv, 0, 1)
    assert f["hr"] == 1
    assert f["pa"] == pytest.approx(1.0, abs=1e-6)
    assert f["pb"] == pytest.approx(1.0 / D["tau_d2"], abs=1e-6)
    assert f["qa"] == pytest.approx(1.0, abs=1e-6)
    assert f["qb"] == pytest.approx(1.0 / D["tau_d2"], abs=1e-6)
    assert f["c"] == 0.0 and f["dh"] >= 0.0


def test_engine_nan_heat_params_equal_defaults(drv):
    drv("ereset")
    for i in (0, 1):
        eset(drv, i, bright=100, measured=1, ble=1, level=3)
    eset(drv, 0, a6="nan", a1="nan", tau1="nan", tau2="nan", abt="nan", taubt="nan")
    for n in (1, 600, 3600):
        a, b = etick(drv, 0, n), etick(drv, 1, n)
        assert a == b, n
        assert all(math.isfinite(v) for v in a.values()), n


def test_engine_heat_frozen_without_reading(drv):
    drv("ereset")
    eset(drv, 0, bright=100, measured=1)
    r = etick(drv, 0, 600)
    assert r["ok"] == 1
    eset(drv, 0, t="nan")
    f = etick(drv, 0, 300)
    assert f["ok"] == 0
    for k in ("pa", "pb", "qa", "qb", "c", "dh", "bt", "bacc", "on"):
        assert f[k] == r[k], k
    eset(drv, 0, t=29.1)
    g = etick(drv, 0, 1)
    assert g["ok"] == 1 and g["pa"] > r["pa"]


def test_yaml_initial_values_equal_heat_h_defaults():
    """cgdn1.yaml number initial_values = heat::Params defaults (first boot uses the YAML ones)."""
    yaml = (ROOT / "cgdn1.yaml").read_text()
    ids = {"hc_a6": "a6", "hc_a1": "a1", "hc_tau_d1": "tau_d1", "hc_tau_d2": "tau_d2", "hc_a_bt": "a_bt",
           "hc_tau_bt": "tau_bt"}
    for eid, k in ids.items():
        block = yaml[yaml.index(f"    id: {eid}\n"):]
        m = re.search(r"initial_value: ([0-9.]+)", block)
        assert m and float(m.group(1)) == pytest.approx(D[k], abs=1e-6), eid
