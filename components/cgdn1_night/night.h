// night.h - the Qingping CGDN1 stock (4.8.5) night mode: display (and so the status LED) off
// inside a daily time window.
//
// Pure C++ (no ESPHome includes) so the host tests (tests/test_night.py) compile it with the
// system compiler. The ESPHome glue (clock, settings entities, 1 s tick, activity hooks) is in
// cgdn1.yaml.
//
// Stock source (static RE of CGDN1_ESP_4.8.5.elf):
//  - Settings night_mode_start_time / night_mode_end_time (config_store 0x13/0x14, settings
//    +0xe/+0x10): minutes after midnight, 0..1440 (cloud JSON parser FUN_400df850 rejects
//    > 1440). Defaults 0/0 (FUN_400d68c4). Either one 0 = night mode off.
//  - Window test FUN_400ea04c(hour, minute, start, end): start/end split into h:m, the window
//    is [start, end) going forward through midnight; start == end = never. (A 1440 = "24:00"
//    value has hour 24, which the hour walk never meets: stock then treats end 24:00 as "all
//    day except before start in the start hour" and start 24:00 as "never" -- a stock quirk,
//    ported as is; the ESPHome time entities only produce 0..1439.)
//  - The 1 s system task FUN_400ea3f0, run only while the device shows sensor pages or the
//    screensaver (dev status 4/5/13), and only once the clock is set (local epoch > threshold):
//      * night off (start or end 0): if the night flag was set, clear it and, if the display is
//        off, turn it on (FUN_400e784c);
//      * outside the window: same (the wake happens on the leaving edge only, so a display the
//        user turned off by day stays off);
//      * inside the window: set the flag; if the display is on and either idle counter is > 4
//        (sys+0xa0 / +0xa4, +1 per second in FUN_400e9654), turn it off (FUN_400e788c).
//    The idle counters are cleared by every touched slider sample (Touch_Task FUN_400e2268) and
//    by the back-button push / click (FUN_400e77d4 / FUN_400e78dc), all via FUN_400e9624(1). So
//    a touch or a click lights the panel (their own handlers do that) and night mode turns it
//    off again ~5 s after the last one.
//  - The status LED needs nothing here: it is off whenever the display is off (stock and
//    cgdn1_led alike).
#pragma once

#include <cstdint>

namespace cgdn1_night {

static constexpr uint16_t DAY_MIN = 1440;
static constexpr uint32_t IDLE_OFF_S = 4;  // stock: off once an idle counter is > 4 (5th second)

// FUN_400ea04c, literally: is hour:minute inside [start, end)?
inline bool in_window(uint8_t hour, uint8_t minute, uint16_t start, uint16_t end) {
  const uint8_t sh = start / 60, sm = start % 60, eh = end / 60, em = end % 60;
  if (sh == hour) {
    if (eh == sh) {
      if (sm < em)
        return sm <= minute && minute < em;
      if (sm != em)
        return sm <= minute || minute < em;  // wraps the whole day
      return false;
    }
    return sm <= minute;
  }
  if (eh == hour)
    return minute < em;
  if (sh == eh)
    return em < sm;  // same hour, wrapping: every other hour is inside
  // Walk the hours after the start hour (at most 24) until the end hour.
  uint8_t h = sh == 23 ? 0 : sh + 1;
  for (int n = 24; n > 0; n--) {
    if (h == eh)
      return false;
    if (h == hour)
      return true;
    h = h == 23 ? 0 : h + 1;
  }
  return false;
}

enum class Action : uint8_t { NONE, DISPLAY_OFF, DISPLAY_ON };

struct Inputs {
  bool enabled{false};      // stock: start != 0 && end != 0 (here: the "Night mode" switch)
  bool pages_shown{false};  // stock dev status 4/5/13 (here: past the boot screen)
  bool clock_valid{false};  // stock: local epoch past the "time set" threshold
  uint8_t hour{0}, minute{0};
  uint16_t start{0}, end{0};
  bool display_on{true};
  uint32_t idle_s{0};       // seconds since the last touch / button push or click
};

// The night branch of the 1 s system task. Call once per second.
class Engine {
 public:
  Action tick(const Inputs &in) {
    if (!in.pages_shown)
      return Action::NONE;
    if (!in.enabled)
      return this->leave_(in);
    if (!in.clock_valid)
      return Action::NONE;
    if (!in_window(in.hour, in.minute, in.start, in.end))
      return this->leave_(in);
    this->active_ = true;
    if (in.display_on && in.idle_s > IDLE_OFF_S)
      return Action::DISPLAY_OFF;
    return Action::NONE;
  }

  /// Inside the window (the stock night flag).
  bool active() const { return this->active_; }

 protected:
  Action leave_(const Inputs &in) {
    if (!this->active_)
      return Action::NONE;
    this->active_ = false;
    return in.display_on ? Action::NONE : Action::DISPLAY_ON;
  }

  bool active_{false};
};

}  // namespace cgdn1_night
