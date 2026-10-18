#pragma once
// Pure layout / brightness math for the CGDN1 display: no ESPHome includes, so the host tests
// (tests/test_display_math.py) compile it as-is with the system C++ compiler.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace esphome::cgdn1_display {

/// Half-open pixel rectangle [x0, x1) x [y0, y1). Empty when x1 <= x0 or y1 <= y0.
struct Rect {
  int x0{0}, y0{0}, x1{0}, y1{0};
  bool empty() const { return this->x1 <= this->x0 || this->y1 <= this->y0; }
  int width() const { return this->empty() ? 0 : this->x1 - this->x0; }
  int height() const { return this->empty() ? 0 : this->y1 - this->y0; }
  Rect moved(int dx, int dy) const { return {this->x0 + dx, this->y0 + dy, this->x1 + dx, this->y1 + dy}; }
  Rect united(const Rect &o) const {
    if (this->empty())
      return o;
    if (o.empty())
      return *this;
    return {std::min(this->x0, o.x0), std::min(this->y0, o.y0), std::max(this->x1, o.x1), std::max(this->y1, o.y1)};
  }
};

/// Pong-style mover for one tracked point (x, y): velocity (+-1, +-1) px per step, reflects off the
/// bounds of an explicit [min, max] range per axis. The caller derives the range from what it draws
/// (e.g. the shown page's ink); the range may change between calls. A point left outside it is
/// clamped in at once, or, with max_pull > 0, walked back at most max_pull px per axis per call (a
/// slide into a page with a narrower range: no sideways jump mid-slide). Time-based (whole steps
/// since the last call), so an irregular redraw cadence does not change the speed.
struct Bouncer {
  int x{0}, y{0};
  int vx{1}, vy{1};
  uint32_t t_last{0};
  bool started{false};
  /// Advance to `now` inside [min_x, max_x] x [min_y, max_y] (an axis with max < min pins to min);
  /// the first call starts at the range centre. Returns true if the point moved.
  bool step_range(uint32_t now, int min_x, int max_x, int min_y, int max_y, uint32_t step_ms, int max_pull = 0) {
    max_x = std::max(min_x, max_x);
    max_y = std::max(min_y, max_y);
    const int ox = this->x, oy = this->y;
    if (!this->started) {
      this->started = true;
      this->t_last = now;
      this->x = floor_half(min_x + max_x);
      this->y = floor_half(min_y + max_y);
    }
    uint32_t n = step_ms == 0 ? 0 : (now - this->t_last) / step_ms;
    this->t_last += n * step_ms;
    if (n > 1000)
      n = 1000;  // long stall (display off): no need to replay every step
    // An axis outside its range is only pulled back this call (heading inward); it bounces again
    // from the next call on, once inside.
    const bool pull_x = pull_(this->x, this->vx, min_x, max_x, max_pull);
    const bool pull_y = pull_(this->y, this->vy, min_y, max_y, max_pull);
    for (uint32_t i = 0; i < n; i++) {
      if (!pull_x) {
        this->x += this->vx;
        if (this->x <= min_x || this->x >= max_x)
          this->vx = this->x <= min_x ? 1 : -1;
      }
      if (!pull_y) {
        this->y += this->vy;
        if (this->y <= min_y || this->y >= max_y)
          this->vy = this->y <= min_y ? 1 : -1;
      }
    }
    // Range changed (other page / more digits) with max_pull 0: stay fully inside.
    if (!pull_x)
      this->x = std::max(min_x, std::min(max_x, this->x));
    if (!pull_y)
      this->y = std::max(min_y, std::min(max_y, this->y));
    return this->x != ox || this->y != oy;
  }
  /// Box mover: advance a bw x bh box on a w x h canvas so it lies inside
  /// [margin, w - margin) x [margin, h - margin) (if it fits). Thin wrapper over step_range().
  bool step(uint32_t now, int bw, int bh, int w, int h, int margin, uint32_t step_ms) {
    return this->step_range(now, margin, w - margin - bw, margin, h - margin - bh, step_ms);
  }

 private:
  static int floor_half(int v) { return v >= 0 ? v / 2 : -((1 - v) / 2); }
  /// p outside [lo, hi] and max_pull > 0: move p at most max_pull toward the range, v inward.
  static bool pull_(int &p, int &v, int lo, int hi, int max_pull) {
    if (max_pull <= 0 || (p >= lo && p <= hi))
      return false;
    v = p < lo ? 1 : -1;
    p = p < lo ? std::min(lo, p + max_pull) : std::max(hi, p - max_pull);
    return true;
  }
};

/// The stock page slide, as a time-based state machine.
/// Pages sit PAGE_PX (= the 160 px screen width) apart on a ring of `count` pages. A slide pushes
/// the whole strip a linear STEP_PX per FRAME_MS frame (FUN_400e3e28 next / FUN_400e3c18 prev:
/// vTaskDelayUntil(2) @ 10 ms, no easing) and snaps when <= STEP_PX remain, so one page takes 15
/// frames = 300 ms (the 15th push lands within 10 px and snaps, a 20 px last step). A touch while
/// sliding chains: the target moves one more page the same way. Pages change on a touch only (a
/// deliberate choice): no auto-advance like the stock carousel screensaver (FUN_400e4038).
/// The page indicator (FUN_400e4d48) is shown for INDICATOR_MS after a touch only.
/// The offset is kept unwrapped and the target as an integer page count, so a chain never
/// reverses; both are normalised (mod count) only once settled.
struct PageSlider {
  static constexpr int PAGE_PX = 160, STEP_PX = 10;  // stock: 10 px per frame, snap <= 10
  static constexpr uint32_t FRAME_MS = 20;           // stock vTaskDelayUntil(2) @ 10 ms
  static constexpr uint32_t INDICATOR_MS = 3000;
  static constexpr uint32_t MAX_FRAMES = 64;  // per update(): a stall finishes the slide instantly

  void begin(int count, uint32_t now) {
    this->count_ = std::max(1, count);
    this->off_ = 0;
    this->target_ = 0;
    this->t_frame_ = now;
    this->touched_ = false;
  }
  /// Touch: chain one page forward / back, show the indicator. Ignored
  /// before begin() (a touch on the boot screen or a test pattern: no slide, no 50 Hz redraws).
  void next(uint32_t now) { this->touch_(1, now); }
  void prev(uint32_t now) { this->touch_(-1, now); }
  /// Whole frames since the last call (a slide only); normalised once settled.
  void update(uint32_t now) {
    uint32_t n = (now - this->t_frame_) / FRAME_MS;
    if (n > MAX_FRAMES) {
      // Stall (display off: the lambda is not run): finish any slide now, so the panel wakes
      // to a settled page like stock.
      n = MAX_FRAMES;
      this->t_frame_ = now;
    } else {
      this->t_frame_ += n * FRAME_MS;
    }
    for (uint32_t i = 0; i < n && this->animating(); i++) {
      const int goal = this->target_ * PAGE_PX;
      this->off_ += goal > this->off_ ? STEP_PX : -STEP_PX;  // push first, then snap (stock)
      if (std::abs(goal - this->off_) <= STEP_PX)
        this->off_ = goal;
    }
    if (!this->animating()) {
      this->target_ = mod_(this->target_, this->count_);
      this->off_ = this->target_ * PAGE_PX;
    }
  }
  bool animating() const { return this->off_ != this->target_ * PAGE_PX; }
  /// Page the slide is heading to (0..count-1; == page() when settled): the bounce range comes
  /// from it for the whole slide, so it does not change halfway like page().
  int target_page() const { return mod_(this->target_, this->count_); }
  /// Unwrapped px travelled (negative after prev from page 0); normalised when settled.
  int offset() const { return this->off_; }
  /// Page nearest the screen centre (flips when a slide passes halfway): LED, indicator.
  int page() const { return mod_(floor_div_(this->off_ + PAGE_PX / 2, PAGE_PX), this->count_); }
  /// x of page k's origin on screen, wrapped into (-count*80, count*80]; visible iff |x| < 160.
  int page_x(int k) const {
    const int ring = this->count_ * PAGE_PX;
    int x = mod_(k * PAGE_PX - this->off_, ring);
    if (x > ring / 2)
      x -= ring;
    return x;
  }
  bool indicator_visible(uint32_t now) const { return this->touched_ && now - this->t_touch_ < INDICATOR_MS; }

 private:
  void touch_(int dir, uint32_t now) {
    if (this->count_ <= 1)
      return;  // not begun (or one page: nothing to slide to)
    if (!this->animating())
      this->t_frame_ = now;  // a new slide starts its frame clock here; mid-slide the phase is kept
    this->target_ += dir;
    this->t_touch_ = now;
    this->touched_ = true;
  }
  static int mod_(int v, int m) { return ((v % m) + m) % m; }
  static int floor_div_(int v, int d) { return (v - mod_(v, d)) / d; }
  int count_{1}, off_{0}, target_{0};
  uint32_t t_frame_{0}, t_touch_{0};
  bool touched_{false};
};

/// Brightness steps: 0.75 photographic stop (x 2^-0.75 = x 0.59 segment current) per 20 %, i.e.
/// per "Display brightness" level -- even perceived steps, the dimmest level ~7 % of the top.
static constexpr float BRIGHTNESS_STOPS_PER_PCT = 0.75f / 20.0f;

/// Brightness % (0..100, NaN = 100) -> controller contrast byte (0x81 <byte>; segment current is
/// linear in it on both panel variants): 100 % = `top` (stock: 0xD0), each 20 % less = 0.75 stop
/// less, never below 1 (0 % stays faintly lit; dark is display off).
inline uint8_t contrast_level(float pct, uint8_t top = 0xD0) {
  if (std::isnan(pct))
    pct = 100.0f;
  pct = std::max(0.0f, std::min(100.0f, pct));
  const long v = lroundf(top * exp2f(-(100.0f - pct) * BRIGHTNESS_STOPS_PER_PCT));
  return (uint8_t) std::max(1L, std::min(255L, v));
}

}  // namespace esphome::cgdn1_display
