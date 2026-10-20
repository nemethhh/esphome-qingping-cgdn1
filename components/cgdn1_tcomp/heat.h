// heat.h - measured display + Bluetooth self-heating of the Qingping CGDN1 (our model, not stock).
//
// Pure C++ (no ESPHome includes) so the host tests (tests/test_tcomp_heat.py) compile it with
// the system compiler. Wired into the tcomp.h Engine, stepped once per second.
//
// Why: the stock compensation (tcomp.h) only knows the panel on/off, as a -1 C linear ramp.
// Measured on a unit by switching the display, its brightness and Bluetooth on and off: the lit
// display warms the SHT4x by ~1.5 C at "Display brightness" 6 and the running BLE stack by
// ~1.6 C, each through a two-stage lag (a first-order stage of ~110 s feeding a second of
// ~1100 s); the first stage reproduces the ~1.5 min before the sensor feels a change.
//
// Superposition model. The heat at a brightness level follows the OLED contrast byte
// (cgdn1.yaml "Display brightness": 0x0F 0x1A 0x2C 0x4A 0x7C 0xD0 for levels 1..6), linearly
// between the heat at level 1 (a1) and at level 6 (a6):
//   A(level) = a1 + (a6 - a1) * w(level),  w(level) = (c(level) - 0x0F) / (0xD0 - 0x0F)  (0..1)
// Two unit-drive cascades of two first-order lags (tau_d1 then tau_d2) carry the physics:
//   p: driven by on = display on ? 1 : 0;  q: driven by g = on * w(level)
//   x_a += (in - x_a) / tau_d1;  x_b += (x_a - x_b) / tau_d2   (x = p, q)
//   heat_display = a1 * p_b + (a6 - a1) * q_b
// which equals a cascade driven by on * A(level) for constant a1/a6 (linearity), but the state
// does not depend on a1/a6: moving either slider rescales the heat to its new value in one
// step at any level, a6 = 0 keeps the a1 term and does not decay the state, and the state stays
// a unit fraction (0 <= q <= p <= 1, so heat >= 0 for a1, a6 >= 0, even with a1 > a6).
// A brightness change still lags physically, through the q cascade.
// Bluetooth (BLE stack running) is a single lag toward 0/1:
//   b_bt += (ble - b_bt) / tau_bt;  heat_bt = a_bt * b_bt   (only when bt_comp)
#pragma once

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace tcomp {
namespace heat {

// Defaults: fit of an on/off test of one unit (display, brightness and Bluetooth switched
// hourly), rounded to the HA entity steps; same values as the cgdn1.yaml number
// initial_values. Other units may differ: tune them from HA.
// The display sizes are the Bluetooth-on operating point (the BLE proxy runs permanently):
// the Bluetooth-on display edges fitted 1.50-1.54 C, the one Bluetooth-off edge 1.82 C.
// That display x Bluetooth interaction is not modelled.
struct Params {
  float a6 = 1.5f;         // display heat at brightness 6 [degC]
  float a1 = 1.25f;        // display heat at brightness 1 [degC]
  float tau_d1 = 110.0f;   // first display lag [s]
  float tau_d2 = 1130.0f;  // second display lag [s]
  float a_bt = 1.6f;       // Bluetooth heat [degC]
  float tau_bt = 1070.0f;  // Bluetooth lag [s]
  bool bt_comp = true;     // apply the Bluetooth term
};

namespace detail {
inline float nz(float v, float def) { return std::isnan(v) ? def : v; }
inline float unit(float v) { return std::fmin(std::fmax(v, 0.0f), 1.0f); }  // NaN -> 0
static const int CONTRAST[6] = {0x0F, 0x1A, 0x2C, 0x4A, 0x7C, 0xD0};
}  // namespace detail

// NaN (entity without a value yet) -> that field's default; taus >= 1, heats >= 0.
inline Params sanitize(const Params &p) {
  const Params d;
  Params r = p;
  r.a6 = std::fmax(detail::nz(p.a6, d.a6), 0.0f);
  r.a1 = std::fmax(detail::nz(p.a1, d.a1), 0.0f);
  r.a_bt = std::fmax(detail::nz(p.a_bt, d.a_bt), 0.0f);
  r.tau_d1 = std::fmax(detail::nz(p.tau_d1, d.tau_d1), 1.0f);
  r.tau_d2 = std::fmax(detail::nz(p.tau_d2, d.tau_d2), 1.0f);
  r.tau_bt = std::fmax(detail::nz(p.tau_bt, d.tau_bt), 1.0f);
  return r;
}

struct State {
  float p_a = 0, p_b = 0;  // unit cascade driven by display on (0/1)
  float q_a = 0, q_b = 0;  // unit cascade driven by on * level_weight(level)
  float b_bt = 0;          // Bluetooth lag, 0..1
};

// Level weight w(level): 0 at brightness 1, 1 at 6, by contrast byte (NaN -> 6, rounded, 1..6).
inline float level_weight(float level) {
  int l = std::isnan(level) ? 6 : (int) std::lround(std::fmin(std::fmax(level, 0.0f), 9.0f));
  l = l < 1 ? 1 : l > 6 ? 6 : l;
  return (float) (detail::CONTRAST[l - 1] - 0x0F) / (float) (0xD0 - 0x0F);
}

// One 1 s step. p must be sanitized.
inline void step(State &s, const Params &p, bool display_on, float level, bool ble_active) {
  const float on = display_on ? 1.0f : 0.0f;
  const float g = on * level_weight(level);
  s.p_a += (on - s.p_a) / p.tau_d1;
  s.p_b += (s.p_a - s.p_b) / p.tau_d2;
  s.q_a += (g - s.q_a) / p.tau_d1;
  s.q_b += (s.q_a - s.q_b) / p.tau_d2;
  s.b_bt += ((ble_active ? 1.0f : 0.0f) - s.b_bt) / p.tau_bt;
}

inline float display_heat(const State &s, const Params &p) { return p.a1 * s.p_b + (p.a6 - p.a1) * s.q_b; }
inline float bt_heat(const State &s, const Params &p) { return p.bt_comp ? p.a_bt * s.b_bt : 0.0f; }

// ---- persistence (ESPHome restore_value global: raw bytes in NVS, keep trivially copyable) ----
static constexpr uint16_t HEAT_MAGIC = 0x4E02;
struct Saved {
  uint16_t magic = 0;
  int16_t p_a = 0, p_b = 0, q_a = 0, q_b = 0, b_bt = 0;  // fractions x 10000
};
static_assert(std::is_trivially_copyable<Saved>::value && sizeof(Saved) == 12, "heat::Saved: POD, no padding");

namespace detail {
inline int16_t q16(float f) { return (int16_t) std::nearbyint(unit(f) * 10000.0f); }
inline float uq16(int16_t v) { return unit((float) v / 10000.0f); }
}  // namespace detail

inline Saved snapshot(const State &s) {
  Saved v;
  v.magic = HEAT_MAGIC;
  v.p_a = detail::q16(s.p_a);
  v.p_b = detail::q16(s.p_b);
  v.q_a = detail::q16(s.q_a);
  v.q_b = detail::q16(s.q_b);
  v.b_bt = detail::q16(s.b_bt);
  return v;
}

// Fractions x scale (the caller checks v.magic == HEAT_MAGIC). Scale and every fraction are
// clamped to [0, 1] and q to <= p (the invariant that keeps the heat >= 0), so a corrupt
// record cannot produce a negative or runaway heat.
inline State restore(const Saved &v, float scale) {
  const float k = detail::unit(scale);
  State s;
  s.p_a = detail::uq16(v.p_a) * k;
  s.p_b = detail::uq16(v.p_b) * k;
  s.q_a = std::fmin(detail::uq16(v.q_a) * k, s.p_a);
  s.q_b = std::fmin(detail::uq16(v.q_b) * k, s.p_b);
  s.b_bt = detail::uq16(v.b_bt) * k;
  return s;
}

}  // namespace heat
}  // namespace tcomp
