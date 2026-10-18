// led.h - the Qingping CGDN1 stock (4.8.5) RGB status LED: level colours, duties, blink.
//
// Pure C++ (no ESPHome includes) so the host tests (tests/test_led.py) compile it with the
// system compiler. The ESPHome glue (3 ledc outputs, 100 ms refresh, the "shown" sensor) is in
// cgdn1.yaml.
//
// Stock source (static RE of CGDN1_ESP_4.8.5.elf):
//  - Hardware: LEDC low-speed timer 0, 10-bit, 5 kHz; R = GPIO25, G = GPIO21, B = GPIO19,
//    active-high (FUN_400e7b5c, timer struct 0x3f408d94). No fades: hard colour switches.
//  - pwm_set FUN_400edac4(gpio, pct): duty = pct ? 1024 / (100 / pct) : 0 -- integer maths, so
//    the duties are quantised (pct 51..100 -> 1024 = 100 %, 34..50 -> 512, 26..33 -> 341 ...).
//  - RGB_LED_Task FUN_400e80e0: one state at a time, refreshed every 100 ms (vTaskDelay(10) at
//    100 Hz). Colour table FUN_400e7f90 (fractions -> pct = (u8)(f * 100)), white FUN_400e7c30,
//    orange blink FUN_400e7c50.
//  - Level of the sensor currently SHOWN on the screen (not a worst-of):
//    calculate_rgb_led_status FUN_400e63a4(type), type 1 PM2.5, 2 PM10, 3 CO2, 4 T, 5 RH.
//    PM tables by ucPm25Standard (settings +0x47): PM2.5 @0x3ffbdcf8, PM10 @0x3ffbdce4.
//    CO2/T/RH use the default co2/temp/humi_led_th (1000/2000/3000 ppm, 23.00/26.50/28.00 C,
//    20/40/60 %RH). Sensor status 0 ("--", no data) -> orange blink; warming up -> off.
#pragma once

#include <cmath>
#include <cstdint>

namespace cgdn1_led {

// Stock sensor type ids (FUN_400e63a4). NONE = no single reading is shown (boot screen).
enum Sensor : uint8_t { SENSOR_NONE = 0, SENSOR_PM25 = 1, SENSOR_PM10 = 2, SENSOR_CO2 = 3, SENSOR_TEMP = 4,
                        SENSOR_HUMI = 5 };

// Stock LED states (event-group bits in brackets).
enum Colour : uint8_t {
  OFF = 0,       // 0x1
  WHITE,         // 0x2      OTA in progress
  ORANGE_BLINK,  // 0x8      no data / sensor error
  BLUE,          // 0x20
  GREEN,         // 0x80
  YELLOW,        // 0x200
  ORANGE,        // 0x800
  RED,           // 0x2000
  DEEP_RED,      // 0x8000   (magenta-ish: R 100 % + B 50 %)
  PURPLE,        // 0x20000
  COLOUR_COUNT
};

// ucPm25Standard 0 (default; 3 and 4 use the same tables), 1, 2.
enum PmStandard : uint8_t { PM_CHINA = 0, PM_US = 1, PM_EU = 2 };

inline const char *colour_name(Colour c) {
  static const char *const N[COLOUR_COUNT] = {"off",    "white", "orange_blink", "blue",     "green",
                                              "yellow", "orange", "red",         "deep_red", "purple"};
  return c < COLOUR_COUNT ? N[c] : "?";
}

// ---- duties --------------------------------------------------------------------------------
static constexpr uint16_t DUTY_FULL = 1024;  // 10-bit LEDC: 1024 = always on

// FUN_400edac4, integer division exactly like stock.
constexpr uint16_t pct_to_duty(int pct) { return pct <= 0 ? 0 : pct >= 100 ? DUTY_FULL : DUTY_FULL / (100 / pct); }

struct Pct {
  uint8_t r, g, b;
};
struct Rgb {
  uint16_t r, g, b;  // 0..1024 (stock 10-bit duty)
  bool operator==(const Rgb &o) const { return r == o.r && g == o.g && b == o.b; }
  bool operator!=(const Rgb &o) const { return !(*this == o); }
};

// The pct each stock state passes to pwm_set (FUN_400e7f90 fractions * 100 truncated; white and
// the blink's on-phase are written directly). ORANGE_BLINK = its on-phase.
static constexpr Pct STOCK_PCT[COLOUR_COUNT] = {
    {0, 0, 0},     // off
    {35, 35, 40},  // white (FUN_400e7c30)
    {70, 30, 9},   // orange blink on-phase (FUN_400e7c50)
    {0, 45, 100},  // blue      0, .45, 1.0
    {4, 100, 2},   // green     .045, 1.0, .025
    {50, 50, 0},   // yellow    .5, .5, 0
    {75, 30, 0},   // orange    .75, .3, 0
    {85, 0, 0},    // red       .85, 0, 0
    {51, 0, 49},   // deep red  .51, 0, .49
    {25, 0, 75},   // purple    .25, 0, .75
};

inline Rgb duty(Colour c) {
  if (c >= COLOUR_COUNT)
    c = OFF;
  const Pct &p = STOCK_PCT[c];
  return {pct_to_duty(p.r), pct_to_duty(p.g), pct_to_duty(p.b)};
}

// ---- level per sensor (FUN_400e63a4, status 2 = valid) --------------------------------------
static constexpr uint16_t PM25_TH[3][5] = {{35, 75, 115, 150, 250},   // China (default)
                                           {12, 35, 55, 150, 250},    // US EPA
                                           {14, 29, 54, 109, 10000}};  // "EU" (purple unreachable)
static constexpr uint16_t PM10_TH[3][5] = {{50, 150, 250, 350, 420},
                                           {54, 154, 254, 354, 424},
                                           {24, 49, 89, 179, 10000}};
static constexpr uint16_t CO2_TH[3] = {1000, 2000, 3000};   // ppm (co2_led_th default)
static constexpr uint16_t TEMP_TH[3] = {2300, 2650, 2800};  // degC * 100 (temp_led_th default)
static constexpr uint16_t HUMI_TH[3] = {2000, 4000, 6000};  // %RH * 100 (humi_led_th default)

// Colour for a valid reading. `v` is the value as displayed (PM/CO2 are compared as the rounded
// integer, like stock's s16/u16 fields; T/RH as float * 100 against the *100 thresholds).
// NaN = no data -> ORANGE_BLINK (stock status 0); SENSOR_NONE -> OFF.
inline Colour level(Sensor s, float v, PmStandard std = PM_CHINA) {
  if (s == SENSOR_NONE)
    return OFF;
  if (std::isnan(v))
    return ORANGE_BLINK;
  const int si = std > PM_EU ? PM_CHINA : std;
  switch (s) {
    case SENSOR_PM25: {  // '<=' (FUN_400e63a4 case 1)
      const auto &t = PM25_TH[si];
      const long x = std::lround(v);
      return x <= t[0] ? GREEN : x <= t[1] ? YELLOW : x <= t[2] ? ORANGE : x <= t[3] ? RED : x <= t[4] ? DEEP_RED : PURPLE;
    }
    case SENSOR_PM10: {  // strict '<' (case 2)
      const auto &t = PM10_TH[si];
      const long x = std::lround(v);
      return x < t[0] ? GREEN : x < t[1] ? YELLOW : x < t[2] ? ORANGE : x < t[3] ? RED : x < t[4] ? DEEP_RED : PURPLE;
    }
    case SENSOR_CO2: {  // '<=' (case 3): no yellow, no purple
      const long x = std::lround(v);
      return x <= CO2_TH[0] ? GREEN : x <= CO2_TH[1] ? ORANGE : x <= CO2_TH[2] ? RED : DEEP_RED;
    }
    case SENSOR_TEMP: {  // float T * 100 vs (float) threshold (case 4)
      const float x = v * 100.0f;
      return x < (float) TEMP_TH[0] ? BLUE : x < (float) TEMP_TH[1] ? GREEN : x < (float) TEMP_TH[2] ? YELLOW : ORANGE;
    }
    case SENSOR_HUMI: {  // case 5
      const float x = v * 100.0f;
      return x < (float) HUMI_TH[0] ? ORANGE : x < (float) HUMI_TH[1] ? YELLOW : x < (float) HUMI_TH[2] ? GREEN : BLUE;
    }
    default:
      return OFF;
  }
}

// ---- state selection ------------------------------------------------------------------------
struct Inputs {
  bool enabled{true};        // HA "LED" switch (not stock: stock's LED is tied to the display)
  bool powering_off{false};  // power-off sequence running (stock FUN_400e9b10 -> off)
  bool display_on{true};     // stock: every page callback forces OFF while the display is off
  bool test_pattern{false};  // display diagnostics frame instead of a reading
  bool ota{false};           // OTA in progress -> white (stock OTA screen FUN_400e2810)
  Sensor sensor{SENSOR_NONE};  // reading currently on screen (NONE = boot screen)
  float value{NAN};            // its value (NaN = no data)
  bool warming_up{false};    // early after boot: no data yet is "warming up" (stock status 1 -> off)
  PmStandard standard{PM_CHINA};
};

// Priority: disabled / power-off / display off > test pattern > OTA > boot screen > level.
inline Colour target(const Inputs &in) {
  if (!in.enabled || in.powering_off || !in.display_on || in.test_pattern)
    return OFF;
  if (in.ota)
    return WHITE;
  if (in.sensor == SENSOR_NONE)
    return OFF;
  if (std::isnan(in.value) && in.warming_up)
    return OFF;
  return level(in.sensor, in.value, in.standard);
}

// RGB_LED_Task: call tick() every 100 ms; it returns the duties to write.
// Orange blink exactly as FUN_400e7c50 (on/off counters 2/2, entered with both = 1): the first
// two ticks after entering keep whatever was lit, tick 2 is off, then on 2 ticks / off 3 ticks
// (200 ms on, 300 ms off, 2 Hz).
class Engine {
 public:
  Rgb tick(const Inputs &in) { return this->step(target(in)); }

  Rgb step(Colour c) {
    if (c == ORANGE_BLINK) {
      if (this->colour_ != ORANGE_BLINK)
        this->blink_n_ = 0;
      const uint32_t n = this->blink_n_++;
      if (n >= 2) {
        const uint32_t phase = (n - 2) % 5;
        this->out_ = (phase == 1 || phase == 2) ? duty(ORANGE_BLINK) : duty(OFF);
      }
    } else {
      this->out_ = duty(c);
    }
    this->colour_ = c;
    return this->out_;
  }

  Colour colour() const { return this->colour_; }
  Rgb output() const { return this->out_; }

 protected:
  Colour colour_{OFF};
  Rgb out_{0, 0, 0};
  uint32_t blink_n_{0};
};

}  // namespace cgdn1_led
