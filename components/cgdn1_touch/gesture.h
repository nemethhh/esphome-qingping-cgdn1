// gesture.h - the Qingping CGDN1 stock (4.8.5) touch-slider gesture decoder.
//
// Pure C++ (no ESPHome includes) so the host tests (tests/test_touch_gesture.py) compile it with
// the system compiler. The ESPHome glue (I2C reads of the ATtiny814, GPIO32 data-ready) is in
// cgdn1_touch.h/.cpp.
//
// Stock source (static RE of CGDN1_ESP_4.8.5.elf):
//  - Touch_Task FUN_400e2268 samples the ATtiny once per FreeRTOS tick (vTaskDelayUntil(1) at
//    100 Hz = 10 ms) from the first GPIO32 data-ready until the finger lifts. Sample =
//    FUN_400e26b8: byte0 = position (0 is stored as 1), byte1 bit7 = touched.
//  - On touch-down it records SPOS (start position), min = max = SPOS, clears a 10-entry
//    per-sample delta history (FUN_400e2170); every further touched sample adds its delta
//    (FUN_400e21a0), updates min/max and TT (touched samples after the first, cap 400).
//    "release_speed" = mean of the last <= 10 deltas, integer division (FUN_400e2238).
//  - Click (decided at release, needs TT >= 1): SPOS outside the middle zone 60..185
//    (`(SPOS - 0x3c) & 0xff < 0x7e` is the middle) and max - min <= 24 (`0x18 < span` = no click).
//    Zone from SPOS + RPOS (RPOS = last touched position): < 0x78 (120) "right click"
//    (event bit 0x10), < 0x174 (372) "chick" = centre (bit 0x100), else "left click" (bit 8).
//    So position counts up from the RIGHT end of the slider to the LEFT end.
//  - Drag (event bit 4): set at touch-down when SPOS is in the middle zone, or later once
//    max - min > 25. The sensor page then follows the finger (FUN_400e3778: page offset +=
//    0.9 * delta, DAT_400d14dc = 0x3f666666; pages are 160 px apart). At release
//    |release_speed| >= 6 (per 10 ms sample) flings one page in the direction of RPOS - SPOS
//    (equal: the sign of the speed); slower, it snaps to the nearest page (FUN_40225eec), i.e.
//    it changes page once the finger moved >= 80 / 0.9 -> 89 position units.
//    Position increasing (finger moving toward the LEFT end) = next page (FUN_400e5d14).
//  - Long touch: Touch_Task keeps key-group bit 2 while max - min <= 37 (`0x25 < span` clears
//    it); FUN_400ea1ac, every 18 ticks (180 ms), counts it and fires at 40 -> 7.2 s, while held
//    (stock then starts the network reset, "触发重置网络操作"; not reproduced here).
//
// ATtiny status byte (device logs 2026-10-18, touch3): byte1 = 0x80 touched, plus bit 2 (0x04)
// while the finger moves toward the RIGHT end (position decreasing) and bit 3 (0x08) while it
// moves toward the LEFT end (increasing); idle 0x00 / 0x12. Stock ignores bits 0..6.
//
// Differences from stock, on purpose:
//  - TAP_CENTRE (display on/off here; stock has no centre action) is conservative, so a swipe can
//    never toggle the display: the touch starts in the middle zone, never leaves it, moves at most
//    CENTRE_MAX_SPAN, has >= CENTRE_MIN_TT samples after the first, lasts <= CENTRE_MAX_MS, no
//    sample carries an ATtiny motion bit and the start was not missed (mark_truncated()). Device
//    log: a swipe whose first read failed (I2C timeout) was seen as 142 -> 165 (span 23, both
//    samples flagged 0x88) and toggled the display.
//  - Stock's "chick" (edge start, centre SPOS + RPOS: a short slide off an end) is not consumed
//    by stock 4.8.5; here it is NONE too (it used to be TAP_CENTRE: 193 -> 176 turned the panel
//    off).
//  - A touch whose start was missed (first sample already flagged as moving, or
//    mark_truncated()) yields no tap at all; it is a swipe only by the stock swipe rules or, when
//    its first sample is flagged, in that flag's direction if the net motion goes that way.
//  - Time-based: the speed is normalised to position units per 10 ms over the last <= 10
//    sample intervals, so a late sample does not inflate it.
//  - After LONG_TOUCH the release yields no further gesture.
#pragma once

#include <cstdint>
#include <cstdlib>

namespace touch_gesture {

enum class Gesture : uint8_t {
  NONE = 0,
  TAP_LEFT,
  TAP_CENTRE,
  TAP_RIGHT,
  SWIPE_LEFT,   // finger moved toward the left end (position increasing)
  SWIPE_RIGHT,  // finger moved toward the right end (position decreasing)
  LONG_TOUCH,
};

inline const char *gesture_name(Gesture g) {
  switch (g) {
    case Gesture::TAP_LEFT:
      return "tap_left";
    case Gesture::TAP_CENTRE:
      return "tap_centre";
    case Gesture::TAP_RIGHT:
      return "tap_right";
    case Gesture::SWIPE_LEFT:
      return "swipe_left";
    case Gesture::SWIPE_RIGHT:
      return "swipe_right";
    case Gesture::LONG_TOUCH:
      return "long_touch";
    default:
      return "none";
  }
}

// ---- stock thresholds (see above) ----------------------------------------------------------
static constexpr uint32_t SAMPLE_MS = 10;       // Touch_Task period: 1 tick at 100 Hz
static constexpr int MID_LO = 60;               // middle zone 0x3c ..
static constexpr int MID_HI = 185;              //   .. 0x3c + 0x7e - 1
static constexpr int TAP_MAX_SPAN = 24;         // click needs max - min <= 0x18
static constexpr int DRAG_SPAN = 25;            // drag once max - min > 0x19
static constexpr int STILL_SPAN = 37;           // long-touch candidate while max - min <= 0x25
static constexpr int RIGHT_SUM = 120;           // SPOS + RPOS < 0x78 -> right
static constexpr int LEFT_SUM = 372;            // SPOS + RPOS >= 0x174 -> left (else centre)
static constexpr int FLING_SPEED = 6;           // |release_speed| >= 6 per sample -> fling
static constexpr int SPEED_WINDOW = 10;         // deltas averaged (FUN_400e21a0 history)
static constexpr int PAGE_SNAP_DELTA = 89;      // ceil(80 px / 0.9 px per unit)
static constexpr uint32_t LONG_TOUCH_MS = 7200; // 40 x 180 ms (FUN_400ea1ac)
// ATtiny status bits (byte1) besides 0x80 = touched.
static constexpr uint8_t MOVE_RIGHT = 0x04;     // moving toward the right end (position down)
static constexpr uint8_t MOVE_LEFT = 0x08;      // moving toward the left end (position up)
static constexpr uint8_t MOVE_MASK = MOVE_RIGHT | MOVE_LEFT;
// Centre tap (not stock): still, short, inside the middle zone throughout. Device taps: span
// 0..4, TT 4..12, 55..140 ms.
static constexpr int CENTRE_MAX_SPAN = 8;
static constexpr int CENTRE_MIN_TT = 3;
static constexpr uint32_t CENTRE_MAX_MS = 1000;

/// What one sample produced. `pressed`: this sample started a touch; `released`: it ended one
/// (gesture, if any, in `gesture`). LONG_TOUCH arrives while still touching.
struct Result {
  bool pressed{false};
  bool released{false};
  Gesture gesture{Gesture::NONE};
};

/// Release-time numbers, kept for logging ("release SPOS RPOS TT speed" like stock).
struct Stats {
  int spos{0};
  int rpos{0};
  int tt{0};      // touched samples after the first (stock TT, capped at 400)
  int speed{0};   // position units per 10 ms, mean over the last <= 10 intervals
  int span{0};    // max - min
  uint32_t duration_ms{0};
  bool truncated{false};  // start missed (mark_truncated() or first sample already moving)
  bool moved{false};      // some sample carried an ATtiny motion bit
};

class Decoder {
 public:
  /// Feed one sample (position byte, touched flag, timestamp in ms, ATtiny status byte: only its
  /// MOVE_* bits are used).
  Result feed(uint8_t raw_pos, bool touched, uint32_t t_ms, uint8_t status = 0) {
    const uint8_t move = status & MOVE_MASK;
    Result r;
    if (!touched) {
      if (this->active_) {
        r.released = true;
        r.gesture = this->classify_(t_ms);
        this->active_ = false;
      }
      return r;
    }
    const int pos = raw_pos == 0 ? 1 : raw_pos;  // stock: position 0 is stored as 1
    if (!this->active_) {
      this->active_ = true;
      this->long_fired_ = false;
      this->spos_ = this->last_ = this->min_ = this->max_ = pos;
      this->t0_ = t_ms;
      this->tt_ = 0;
      this->hist_n_ = 0;
      this->hist_i_ = 0;
      this->t_last_ = t_ms;
      this->truncated_ = false;
      this->first_move_ = move;
      this->moved_ = move != 0;
      r.pressed = true;
      return r;
    }
    if (move != 0)
      this->moved_ = true;
    // Delta history (ring of the last SPEED_WINDOW intervals).
    this->hist_dp_[this->hist_i_] = pos - this->last_;
    this->hist_dt_[this->hist_i_] = t_ms - this->t_last_;
    this->hist_i_ = (this->hist_i_ + 1) % SPEED_WINDOW;
    if (this->hist_n_ < SPEED_WINDOW)
      this->hist_n_++;
    this->last_ = pos;
    this->t_last_ = t_ms;
    if (pos < this->min_)
      this->min_ = pos;
    if (pos > this->max_)
      this->max_ = pos;
    if (this->tt_ < 400)
      this->tt_++;
    if (!this->long_fired_ && this->max_ - this->min_ <= STILL_SPAN && t_ms - this->t0_ >= LONG_TOUCH_MS) {
      this->long_fired_ = true;
      r.gesture = Gesture::LONG_TOUCH;
    }
    return r;
  }

  /// Drop the current touch without a gesture (e.g. the controller stopped answering).
  void abort() { this->active_ = false; }
  /// The start of the current touch was missed (late first sample, read error just before it):
  /// no tap from it, only a swipe.
  void mark_truncated() {
    if (this->active_)
      this->truncated_ = true;
  }
  /// The current / last touch was truncated or started mid-motion.
  bool truncated() const { return this->truncated_ || this->first_move_ != 0; }

  bool active() const { return this->active_; }
  /// Numbers of the touch in progress / the last released one.
  Stats stats() const {
    Stats s;
    s.spos = this->spos_;
    s.rpos = this->last_;
    s.tt = this->tt_;
    s.speed = this->speed_();
    s.span = this->max_ - this->min_;
    s.duration_ms = this->t_last_ - this->t0_;
    s.truncated = this->truncated();
    s.moved = this->moved_;
    return s;
  }

 protected:
  /// Mean delta per SAMPLE_MS over the last <= SPEED_WINDOW intervals, truncated toward 0
  /// (stock: integer mean of per-tick deltas).
  int speed_() const {
    if (this->hist_n_ == 0)
      return 0;
    long dp = 0, dt = 0;
    for (int i = 0; i < this->hist_n_; i++) {
      dp += this->hist_dp_[i];
      dt += (long) this->hist_dt_[i];
    }
    if (dt <= 0)
      dt = (long) SAMPLE_MS * this->hist_n_;
    return (int) (dp * (long) SAMPLE_MS / dt);
  }

  Gesture classify_(uint32_t t_ms) const {
    if (this->long_fired_)
      return Gesture::NONE;
    const int span = this->max_ - this->min_;
    const int delta = this->last_ - this->spos_;
    const int speed = this->speed_();
    const bool mid_start = this->spos_ >= MID_LO && this->spos_ <= MID_HI;
    const bool dragging = mid_start || span > DRAG_SPAN;
    if (dragging && (std::abs(speed) >= FLING_SPEED || std::abs(delta) >= PAGE_SNAP_DELTA)) {
      const int dir = delta != 0 ? delta : speed;
      return dir > 0 ? Gesture::SWIPE_LEFT : Gesture::SWIPE_RIGHT;
    }
    if (this->first_move_ != 0) {
      // Already sliding at the first sample: the start of a swipe was missed. Its direction is
      // the ATtiny's, if the motion we did see does not contradict it.
      const int dir = this->first_move_ == MOVE_LEFT ? 1 : this->first_move_ == MOVE_RIGHT ? -1 : 0;
      if (dir != 0 && delta * dir > 0)
        return dir > 0 ? Gesture::SWIPE_LEFT : Gesture::SWIPE_RIGHT;
      return Gesture::NONE;
    }
    if (this->truncated_)
      return Gesture::NONE;
    if (dragging) {
      // Not in stock (there: snap back, no action). Ambiguous -> nothing.
      const bool inside = this->min_ >= MID_LO && this->max_ <= MID_HI;
      if (mid_start && inside && !this->moved_ && span <= CENTRE_MAX_SPAN && this->tt_ >= CENTRE_MIN_TT &&
          t_ms - this->t0_ <= CENTRE_MAX_MS && std::abs(speed) <= 1)
        return Gesture::TAP_CENTRE;
      return Gesture::NONE;
    }
    if (span > TAP_MAX_SPAN || this->tt_ < 1)
      return Gesture::NONE;
    const int sum = this->spos_ + this->last_;
    if (sum < RIGHT_SUM)
      return Gesture::TAP_RIGHT;
    if (sum >= LEFT_SUM)
      return Gesture::TAP_LEFT;
    return Gesture::NONE;  // stock "chick": not consumed by stock 4.8.5
  }

  bool active_{false};
  bool long_fired_{false};
  bool truncated_{false};
  bool moved_{false};       // some sample carried an ATtiny motion bit
  uint8_t first_move_{0};   // motion bits of the touch-down sample
  int spos_{0}, last_{0}, min_{0}, max_{0};
  int tt_{0};
  uint32_t t0_{0}, t_last_{0};
  int hist_dp_[SPEED_WINDOW]{};
  uint32_t hist_dt_[SPEED_WINDOW]{};
  int hist_n_{0}, hist_i_{0};
};

}  // namespace touch_gesture
