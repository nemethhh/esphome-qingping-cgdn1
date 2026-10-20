// tcomp.h - the Qingping CGDN1 stock (4.8.5) temperature/humidity self-heating compensation.
//
// Pure C++ (no ESPHome includes) so the host tests (tests/test_tcomp.py) compile it with
// the system compiler. ESPHome glue lives in cgdn1.yaml (1 s interval + globals).
//
// Stock source (static RE of CGDN1_ESP_4.8.5.elf): task "Sensor_Temp_Humi_Temp_Fix_Task"
// FUN_400e8d9c steps the model once per SECOND (vTaskDelay(100) at 100 Hz):
//   FUN_400e8380 -> FUN_40225f40 (on++), FUN_400e8adc (charge counters), FUN_400e8d10
//   (brightness ramp), FUN_400e85dc (T sum + slew limiter); humidity / clamps / user
//   calibration inline in FUN_400e8d9c. Boot restore FUN_400e8330 (FUN_400e8960,
//   FUN_400e8c6c, FUN_4022603c). Curves FUN_400e844c, evaluators FUN_400f22f8/FUN_400f2214.
// Every constant below was read back from the image (DROM tables 0x3f4070e8..0x3f4071c0,
// literal pool 0x400d19ac..0x400d1a84). Float/double mixing follows the stock code (each
// sum done in double, stored as float); differences are < 0.01 C.
#pragma once

#include <cmath>
#include <cstdint>

#include "heat.h"

namespace tcomp {

// ---- curves: y(x) = (k + a1*x + a2*x^2 + a3*x^3) / 10000  [degC] -------------------------
// Two-segment cubic: seg0 for x <= x0, seg1 for x > x0.
struct Cubic2 {
  int x0;
  double k[2];
  double a[2][3];
};
// "preheating" (x = seconds powered on, max 8000): DROM 0x3f4070e8 / 0x3f4070f0 / 0x3f407100
static const Cubic2 PREHEAT = {1200,
                               {10.288467313132514, -24778.832215649065},
                               {{-63.89875545624044, 0.038722865402681175, -9.578169525070734e-06},
                                {-14.76911061946425, 0.002048246327797731, -9.423988878154432e-08}}};
// "charging" (x = charge counter, max 7000): 0x3f407130 / 0x3f407138 / 0x3f407148
static const Cubic2 CHARGING = {2000,
                                {-5.895304541018959, -50136.197864496324},
                                {{-111.93648927230642, 0.09037562449514912, -2.8655718423920007e-05},
                                 {-6.100628223515754, 0.0006464288432308507, 0.0}}};
// "chargingdone" (x = 13000 - chg*13000/7000): 0x3f407178 / 0x3f407180 / 0x3f407190
static const Cubic2 CHGDONE = {6000,
                               {-60292.9664820073, -302448.55028207874},
                               {{6.841134842508077, -0.0016753264405149615, 2.2600454574472906e-07},
                                {84.8684530552672, -0.007906957677746823, 2.438464861143075e-07}}};

static constexpr int ON_MAX = 8000;    // S+0x08
static constexpr int CHG_MAX = 7000;   // S+0x0a
static constexpr int CHG_K = 13000;    // S+0x04 (0x400d19e8)
static constexpr int DIS_MAX = 3600;   // S+0x0c (0x400d10e4)
static constexpr int BACC_MAX = 2000;  // brightness accumulator, brightness = bacc/20

inline double cubic2(const Cubic2 &c, int x) {
  const int s = x > c.x0 ? 1 : 0;
  const double d = x;
  return (c.k[s] + c.a[s][0] * d + c.a[s][1] * d * d + c.a[s][2] * d * d * d) / 10000.0;
}
inline double preheating(int x) { return cubic2(PREHEAT, x); }
inline double charging(int x) { return cubic2(CHARGING, x); }
inline double chargingdone(int x) { return cubic2(CHGDONE, x); }
// "discharging" single cubic, x clamped to 3600 (FUN_400f2214/FUN_400f2164; K 0x400d19ac,
// coefficients DROM 0x3f4071c0)
inline double discharging(int x) {
  if (x > DIS_MAX) x = DIS_MAX;
  const double d = x;
  return (16.32204328517696 + 8.456100837633594 * d - 0.004002620029362993 * d * d +
          5.993886915689473e-07 * d * d * d) /
         10000.0;
}
// "bright": (0 - 100*b)/10000, b clamped to 100 -> -1.00 C with the display fully ramped
inline double bright(int b) {
  if (b > 100) b = 100;
  return (-100.0 * b) / 10000.0;
}

// ---- model state (stock struct @0x3ffc5d88 + slew memory 0x3ffbdd18) ----------------------
struct State {
  int16_t on = 0;        // +0x00 seconds powered on ("preheating"), max 8000
  int16_t chg = 0;       // +0x02 charge-heat counter, max 7000
  int16_t dis = 0;       // +0x06 seconds on battery ("discharging"), max 3600
  int16_t bacc = 0;      // +0x0e brightness accumulator 0..2000 (20 s per brightness %)
  uint8_t cst = 2;       // +0x10 previous charge state: 0 battery, 1 charging, 2 USB + done
  uint8_t bst = 0;       // +0x11 battery % when charging began
  float last = -100.0f;  // previous compensated T (slew limiter); -100 = none yet
};

// Charge-done-domain x for the curve lookup (FUN_400e8920)
inline int16_t chg_x(const State &s) {
  if (s.cst == 1) return s.chg;
  return (int16_t) (float) ((float) CHG_K - (float) (s.chg * CHG_K) / (float) CHG_MAX);
}
// FUN_400e89e4: charging-domain counter -> equivalent charge-done counter
inline int16_t conv_c2d(int16_t x) {
  if (x >= CHG_MAX) return CHG_MAX;
  const float v = (float) charging(x);
  for (int u = 0; u < CHG_K; u += 10)
    if (chargingdone(u) > (double) v) return (int16_t) ((float) (CHG_K - u) * (float) CHG_MAX / (float) CHG_K);
  return x;
}
// FUN_400e8a74: charge-done-domain x -> equivalent charging counter
inline int16_t conv_d2c(int16_t x) {
  if (x >= CHG_K) return 0;
  const float v = (float) chargingdone(x);
  for (int u = 0; u < CHG_MAX; u += 10)
    if (charging(u) < (double) v) return (int16_t) u;
  return x;
}
template<typename T> inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---- boot re-init (FUN_400e8330 + FUN_400e8960 / FUN_400e8c6c / FUN_4022603c) ------------
// saved_on / saved_bright (0..100) / saved_t100 (raw T*100 at save) / saved_chg (+chg if it
// was charging, else -max(chg,1)) are stock config_store ids 0x16/0x17/0x18/0x1a.
// t100_now: current raw T*100. There is no wall clock: the remaining self-heat is estimated
// from how far the raw temperature fell while off. If it fell by less than |preheat(saved)|,
// `on` restarts at the first t (60 s grid) with preheat(t) < preheat(saved) - dT; if it fell
// more, the unit starts cold (on = 0); if it did not fall at all, it is restored fully warm.
inline void init(State &s, int saved_on, int saved_bright, int saved_t100, int saved_chg, int t100_now) {
  const float dT = (float) ((double) (t100_now - saved_t100) / 100.0);
  int on = saved_on;
  if (dT < 0.0f) {
    const float p = (float) preheating(saved_on);
    on = 0;
    if (p - dT < 0.0f) {
      on = saved_on;
      for (int i = 0; i < saved_on; i += 60)
        if (preheating(i) < (double) (p - dT)) {
          on = i;
          break;
        }
    }
  }
  s.on = (int16_t) (on > ON_MAX ? ON_MAX : on);
  int acc = saved_bright * 20;
  if (s.on < saved_on) acc = (int) ((1.0 - (double) (saved_on - s.on) / (double) ON_MAX) * (double) acc);
  s.bacc = (int16_t) (acc > BACC_MAX ? BACC_MAX : acc);
  int v = saved_chg;
  s.cst = 1;
  if (v < 0) {
    v = -v;
    s.cst = 2;
  }
  if (s.on < saved_on) v -= saved_on - s.on;
  s.chg = (int16_t) clampv(v, 0, CHG_MAX);
  s.dis = 0;
  s.bst = 0;
  s.last = -100.0f;
}
// Value stock saves under id 0x1a
inline int save_chg(const State &s) { return s.cst == 1 ? s.chg : -(s.chg == 0 ? 1 : s.chg); }

// User calibration (stock config_store 0x43..0x46, cloud keys temperature_offset /
// temperature_zoom / humidity_offset / humidity_zoom). A non-zero zoom replaces the offset.
struct Calib {
  int t_off_c100 = 0;  // 0.01 C units
  int t_zoom = 0;
  int rh_off_d10 = 0;  // 0.1 %RH units
  int rh_zoom = 0;
};

struct Out {
  float t;    // compensated temperature, degC (after clamps and calibration)
  float rh;   // compensated humidity, %RH
  float t_h;  // temperature used for the humidity transfer (no charging term, no slew limit)
};

// ---- one 1-second step ------------------------------------------------------------------
// t_raw / rh_raw: SHT4x degC / %RH. cstate: 0 battery, 1 charging, 2 USB + charge done.
// bright_target: 100 while the display is on, else 0. batt_pct: 0..100.
// heat_c: measured display + Bluetooth heat [degC] (heat.h). NaN -> stock bright() term.
inline Out step(State &s, float t_raw, float rh_raw, int cstate, int bright_target, int batt_pct,
                const Calib &cal = Calib(), float heat_c = NAN) {
  // FUN_40225f40(S, 1)
  s.on = (int16_t) (s.on + 1 > ON_MAX ? ON_MAX : s.on + 1);
  // FUN_400e8adc(S, cstate, 1, batt_pct); x is taken from the PREVIOUS state
  const int16_t x = chg_x(s);
  if (cstate == 0) {
    if ((double) x > CHG_K * 0.8) s.bst = 0;
    if (s.cst == 1) s.chg = (int16_t) clampv<int>(conv_c2d(x), -32768, CHG_MAX);
    s.dis = (int16_t) (s.dis + 1 > DIS_MAX ? DIS_MAX : s.dis + 1);
    s.chg = (int16_t) (s.chg - 1 < 0 ? 0 : s.chg - 1);
  } else if (cstate == 1) {
    if (s.cst != 1) {
      if (s.cst == 0 && (double) x > CHG_K * 0.7) s.bst = (uint8_t) clampv(batt_pct, 0, 100);
      s.chg = (int16_t) clampv<int>(conv_d2c(x), -32768, CHG_MAX);
    }
    // FUN_400e88a4: chg += 200/(200 - bst) per second (1/s at bst 0, 2/s at bst 100)
    const int c = (int) (1.0 * 200.0 / (double) (200 - s.bst) + (double) s.chg);
    s.chg = (int16_t) (c > CHG_MAX ? CHG_MAX : c);
    s.dis = (int16_t) (s.dis - 2 < 0 ? 0 : s.dis - 2);
  } else if (cstate == 2) {
    if ((double) x > CHG_K * 0.8) s.bst = 0;
    if (s.cst == 1) s.chg = (int16_t) clampv<int>(conv_c2d(x), -32768, CHG_MAX);
    s.chg = (int16_t) (s.chg - 1 < 0 ? 0 : s.chg - 1);
    s.dis = (int16_t) (s.dis - 1 < 0 ? 0 : s.dis - 1);
  }
  s.cst = (uint8_t) cstate;
  // FUN_400e8d10(S, bright_target, 1): linear ramp, 0 -> 100 in 2000 s
  if (s.bacc / 20 < bright_target) s.bacc++;
  if (bright_target < s.bacc / 20) s.bacc--;
  s.bacc = (int16_t) clampv<int>(s.bacc, 0, BACC_MAX);
  const int bs = clampv<int>(s.bst, 0, 100);  // FUN_40226084
  // FUN_400e85dc. Stock's raw T is int16 T*100 (SHT4x conversion truncated to 0.01 C).
  const float tr = (float) ((double) (int) (t_raw * 100.0f) / 100.0);
  const int16_t xc = chg_x(s);
  const double cc = s.cst == 1 ? charging(xc) : chargingdone(xc);
  float t = (float) ((double) tr + preheating(s.on));
  t = (float) ((double) t + discharging(s.dis));
  // Measured mode: heat_c (heat.h) replaces the stock bright() ramp; bacc still advances above.
  t = (float) ((double) t + (std::isnan(heat_c) ? bright(s.bacc / 20) : -(double) heat_c));
  const float t_h = t;
  t = (float) ((double) (300 - bs) * cc / 300.0 + (double) t);
  if (s.last > -100.0f) {  // slew limiter: a jump of more than 3 C moves only 0.3 C per second
    if (s.last > t && s.last - t > 3.0f)
      t = (float) ((double) s.last - 0.3);
    else if (t > s.last && t - s.last > 3.0f)
      t = (float) ((double) s.last + 0.3);
  }
  s.last = t;
  // FUN_400e8d9c: move RH to the compensated temperature (Magnus-type saturation-pressure
  // ratio; b*c = 4283.78 = 17.62*243.12 but c' = 220.12 as compiled), then a hard x0.9
  // whenever RAW RH > 55.00 %, clamp 99.9.
  const double trd = tr, thd = t_h;
  const double ex = std::exp((trd - thd) * 4283.78 / (thd + 220.12) / (trd + 220.12));
  // Stock holds RH*100 as u16; a (physically impossible) negative SHT4x RH would wrap there.
  // We clamp it to 0 instead.
  const int rh100 = rh_raw > 0.0f ? (int) (rh_raw * 100.0f) : 0;
  float rh = (float) ((double) rh100 / 100.0 * ex);
  if (rh100 > 5500) rh = (float) ((double) rh * 0.9);
  if (rh > 99.9f) rh = 99.9f;
  if (t < 0.0f)
    t = 0.0f;
  else {
    const float mx = cstate != 0 ? 40.0f : 50.0f;
    if (t > mx) t = mx;
  }
  // user calibration, then T >= 0 and RH in 0..99.9
  float tf = cal.t_zoom == 0 ? (float) ((double) cal.t_off_c100 / 100.0 + (double) t)
                             : ((float) cal.t_zoom * t / 100.0f + 5.0f) / 10.0f + t;
  float rf = cal.rh_zoom == 0 ? (float) ((double) cal.rh_off_d10 / 10.0 + (double) rh)
                              : ((float) cal.rh_zoom * rh / 100.0f + 5.0f) / 10.0f + rh;
  if (tf < 0.0f) tf = 0.0f;
  rf = clampv(rf, 0.0f, 99.9f);
  return {tf, rf, t_h};
}

// ---- charge state (stock Battery_Task FUN_400f1c90 writes sysparams+0x08) -----------------
// Stock: no USB (GPIO34 high) -> 0; on USB -> 1 ("charging") iff Vbat <= 4050 mV (<= 3949 mV
// once it has reported 2, i.e. ~100 mV recharge hysteresis) AND gpio_get_level(35) == 0,
// else 2 ("charge done"). GPIO35 is the charger's PROG/ISET pin, which we sample as an ADC
// ("Charge current", mA = pin_mV/2), so its digital level is not available. Stand-in: the
// charger is "active" when the measured charge current exceeds ICHG_ACTIVE_MA. The YAML
// subtracts the ADC's ~39 mA floor (read with USB unplugged) and clamps at 0, so "no current"
// arrives here as ~0, not 39 (with the floor this test could never fail). Both stock
// conditions stay ANDed, so the Vbat thresholds and hysteresis are exactly stock's, and an
// ADC offset on GPIO35 cannot keep a full battery in "charging" (which would subtract the
// ~6 C charging term on top of the self-heat). Unknown Vbat -> 2 (stock's default cstate);
// unknown current -> Vbat-only.
static constexpr float VBAT_CHARGING_MV = 4050.0f;    // 0x400d23c8
static constexpr float VBAT_RECHARGE_MV = 3949.0f;    // 0x400d23cc
static constexpr float ICHG_ACTIVE_MA = 20.0f;
inline int charge_state(int prev, bool usb, float vbat_mv, float ichg_ma) {
  if (!usb) return 0;
  if (std::isnan(vbat_mv)) return 2;
  const bool v_ok = vbat_mv <= (prev == 2 ? VBAT_RECHARGE_MV : VBAT_CHARGING_MV);
  const bool i_ok = std::isnan(ichg_ma) || ichg_ma > ICHG_ACTIVE_MA;
  return v_ok && i_ok ? 1 : 2;
}

// ---- persisted record (our stand-in for stock config_store 0x16/0x17/0x18/0x1a) ---------
// Stock keeps these in its raw-flash KV at 0x3d0000; we must not write there, so ESPHome
// keeps this record in NVS (a restore_value global). magic != SAVED_MAGIC -> nothing saved
// yet -> start cold (on 0, display 0, USB + charge done).
static constexpr uint16_t SAVED_MAGIC = 0x7C01;
struct Saved {
  uint16_t magic = 0;
  int16_t on = 0;
  int16_t bright = 0;
  int16_t t100 = 0;
  int16_t chg = 0;
};

// ---- per-second driver used by the ESPHome glue ----------------------------------------
struct Inputs {
  float t_raw = NAN;      // degC
  float rh_raw = NAN;     // %RH
  bool usb = false;       // USB present (GPIO34 low)
  float vbat_mv = NAN;    // battery voltage, mV
  float ichg_ma = NAN;    // charge current, mA
  float batt_pct = NAN;   // battery level, %
  int bright_target = 0;  // 100 while the display is on, else 0
  Calib cal;
  float level = 6;         // "Display brightness" 1..6 (heat model drive)
  bool ble = false;        // Bluetooth proxy active
  bool measured = false;   // true: Measured heat model replaces stock bright(); false: Stock
  heat::Params heat;       // heat model parameters (NaN fields -> defaults)
};

class Engine {
 public:
  // Call once per second. Returns false (and steps nothing) while T or RH is not available.
  // The first valid call restores the model from `saved` (stock FUN_400e8330).
  // The heat model (heat.h) advances on every valid tick, whichever mode is selected, so
  // switching Stock <-> Measured never resets it; `heat_saved` is its restore record.
  bool tick(const Inputs &in, const Saved &saved, const heat::Saved *heat_saved = nullptr) {
    this->ticks_++;
    if (std::isnan(in.t_raw) || std::isnan(in.rh_raw)) return false;
    if (!this->ready_) {
      if (saved.magic == SAVED_MAGIC)
        init(this->s_, saved.on, saved.bright, saved.t100, saved.chg, t100(in.t_raw));
      else
        init(this->s_, 0, 0, t100(in.t_raw), -1, t100(in.t_raw));
      // Heat state: with a valid stock record AND a heat record, restore it scaled like stock
      // init() scales bacc (a shorter `on` = more of the self-heat has decayed; scale clamped to
      // [0, 1] against a corrupt record); otherwise seed it from the stock brightness
      // accumulator (0 on a cold start): p = bacc/2000, q = p * level weight of this tick.
      this->heat_restored_ =
          saved.magic == SAVED_MAGIC && heat_saved != nullptr && heat_saved->magic == heat::HEAT_MAGIC;
      if (this->heat_restored_) {
        const float scale =
            this->s_.on < saved.on ? 1.0f - (float) (saved.on - this->s_.on) / (float) ON_MAX : 1.0f;
        this->hs_ = heat::restore(*heat_saved, clampv(scale, 0.0f, 1.0f));
      } else {
        this->hs_ = heat::State();
        this->hs_.p_a = this->hs_.p_b = (float) this->s_.bacc / (float) BACC_MAX;
        this->hs_.q_a = this->hs_.q_b = this->hs_.p_a * heat::level_weight(in.level);
      }
      this->cstate_ = this->s_.cst;
      this->ready_ = true;
    }
    this->hp_ = heat::sanitize(in.heat);
    heat::step(this->hs_, this->hp_, in.bright_target > 0, in.level, in.ble);
    this->cstate_ = charge_state(this->cstate_, in.usb, in.vbat_mv, in.ichg_ma);
    const int pct = std::isnan(in.batt_pct) ? 100 : (int) in.batt_pct;  // stock default 100
    this->t100_ = t100(in.t_raw);
    this->out_ = step(this->s_, in.t_raw, in.rh_raw, this->cstate_, in.bright_target, pct, in.cal,
                          in.measured ? heat::display_heat(this->hs_, this->hp_) + heat::bt_heat(this->hs_, this->hp_)
                                      : NAN);
    return true;
  }
  bool ready() const { return this->ready_; }
  const Out &out() const { return this->out_; }
  const State &state() const { return this->s_; }
  int cstate() const { return this->cstate_; }
  const heat::State &heat_state() const { return this->hs_; }
  // true: the heat state came from the heat record; false: seeded (cold start or no/unknown record)
  bool heat_restored() const { return this->heat_restored_; }
  float display_heat() const { return heat::display_heat(this->hs_, this->hp_); }
  float bt_heat() const { return heat::bt_heat(this->hs_, this->hp_); }
  heat::Saved heat_snapshot() const { return heat::snapshot(this->hs_); }
  uint32_t ticks() const { return this->ticks_; }
  // What stock's save (FUN_400e9224) would write now.
  Saved snapshot() const {
    Saved v;
    v.magic = SAVED_MAGIC;
    v.on = this->s_.on;
    v.bright = (int16_t) (this->s_.bacc / 20);
    v.t100 = (int16_t) this->t100_;
    v.chg = (int16_t) save_chg(this->s_);
    return v;
  }

  static int t100(float t) { return (int) (t * 100.0f); }

 private:
  State s_;
  heat::State hs_;
  heat::Params hp_;
  Out out_{NAN, NAN, NAN};
  int cstate_ = 2;
  int t100_ = 0;
  uint32_t ticks_ = 0;
  bool ready_ = false;
  bool heat_restored_ = false;
};

}  // namespace tcomp
