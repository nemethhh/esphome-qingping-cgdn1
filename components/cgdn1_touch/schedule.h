#pragma once
// When to read the ATtiny814 touch controller: the stock 4.8.5 Touch_Task (FUN_400e2268) read
// schedule. Pure C++ (no ESPHome headers), host-tested by tests/test_touch_schedule.py; driven by
// the component's own 10 ms sampling task (cgdn1_touch.cpp), not by the ESPHome main loop.
//
// Stock, per 10 ms tick (vTaskDelayUntil(1) at 0x400e2487):
//  - `armed |= GPIO32 low` (0x400e2309..0x400e2313: FUN_400e26a0 ready, `or a2, a2, ready`);
//  - armed -> one 2-byte read (FUN_400e26b8). A read error only logs "Touch Read Error"
//    (0x400e2618) and stays armed (next tick reads again);
//  - an untouched sample clears `armed` (`movi a2, 0` at 0x400e2490, before the "touch in
//    progress?" test), so with no touch stock reads only while GPIO32 is low; a touched sample
//    keeps it, so a touch is read every tick until the release sample (armed = 0 again at
//    0x400e2580 / 0x400e2607). On the device GPIO32 is low exactly while byte1 bit7 (touched) is
//    set (touch3 log).
//  - the "init" probe read at 0x400e22c4 is not checked at all ("Attiny814 IIC Init Fail" is the
//    gpio_config result of FUN_400e2678), so a failing read never disables the slider.
//  - stock never reads while GPIO32 is high and no touch is in progress. On the device such a read
//    fails (the idle ATtiny NACKs / stretches SCL -> "I2C hardware timeout", IDF bus reset), and
//    once (touch3 log, 10:04:15) the touch right after such a failed idle read lost its first
//    sample. So the idle diagnostic (DIAG, every DIAG_MS without reads) does NOT read the chip.
// Here: the same arming and disarming; an armed run with GPIO32 held low and no touch is bounded
// (STUCK_READS samples, then a slow STUCK_POLL_MS poll until the line goes high). I2C errors retry
// on the next tick like stock, and back off after ERR_SLOW_AFTER in a row.
#include <cstdint>

namespace touch_schedule {

static constexpr uint32_t SAMPLE_MS = 10;       // Touch_Task period (1 tick at 100 Hz)
static constexpr uint16_t STUCK_READS = 20;     // untouched samples per armed run (200 ms)
static constexpr uint32_t STUCK_POLL_MS = 100;  // GPIO32 asserted without a touch: poll period
static constexpr uint32_t IDLE_POLL_MS = 20;    // no data-ready pin configured: poll period
static constexpr uint32_t ERR_RETRY_MS = 10;    // after an I2C error (stock: next tick) ...
static constexpr uint32_t ERR_SLOW_AFTER = 5;   // ... and after this many in a row:
static constexpr uint32_t ERR_SLOW_MS = 1000;
static constexpr uint32_t DIAG_MS = 30000;      // idle diagnostic report period (no I2C)

enum class Action : uint8_t { NONE, READ, DIAG };
enum class State : uint8_t { IDLE, ARMED, STUCK };
enum class Note : uint8_t { NONE, STUCK, UNSTUCK };

class Scheduler {
 public:
  void set_has_ready_pin(bool v) { this->has_pin_ = v; }

  /// Called every tick. `ready` = data-ready asserted since the last tick (level or edge; ignored
  /// without a pin), `touching` = the last good sample was touched. Returns what to do now: READ
  /// = one I2C sample, then call done(); DIAG = report only (no I2C), then call diag_done().
  Action poll(uint32_t now, bool ready, bool touching) {
    if (!this->has_pin_)
      ready = false;
    if (ready && !this->ready_)
      this->edges_++;
    this->ready_ = ready;
    if (!touching) {
      if (this->state_ == State::ARMED && this->untouched_ && !ready) {
        this->state_ = State::IDLE;  // stock: the untouched sample cleared armed, line high
        this->idle_reads_ = 0;
      }
      if (this->state_ == State::STUCK && !ready) {
        this->state_ = State::IDLE;
        this->note_ = Note::UNSTUCK;
      }
      if (ready && this->state_ == State::IDLE) {
        this->state_ = State::ARMED;
        this->idle_reads_ = 0;
      }
    }
    if ((int32_t) (now - this->next_read_) < 0)
      return Action::NONE;  // error backoff / slow poll
    if (touching || this->state_ != State::IDLE || !this->has_pin_)
      return (now - this->last_read_ >= SAMPLE_MS) ? Action::READ : Action::NONE;
    if (now - this->last_read_ >= DIAG_MS && now - this->last_diag_ >= DIAG_MS)
      return Action::DIAG;
    return Action::NONE;
  }

  /// After a READ: `ok` = I2C success, `active` = this sample (or, on an error, the last good
  /// one) is touched, `released` = this sample ended a touch.
  void done(uint32_t now, Action a, bool ok, bool active, bool released) {
    (void) a;
    this->last_read_ = now;
    this->reads_++;
    this->untouched_ = ok && !active;
    if (!ok) {
      this->errors_++;
      this->errors_total_++;
      if (!active)
        this->idle_step_(now);  // a failing chip counts as an untouched sample
      const uint32_t back = now + (this->errors_ >= ERR_SLOW_AFTER ? ERR_SLOW_MS : ERR_RETRY_MS);
      if ((int32_t) (back - this->next_read_) > 0)
        this->next_read_ = back;
      return;
    }
    this->errors_ = 0;
    if (active) {
      this->idle_reads_ = 0;
      return;
    }
    if (released || !this->ready_) {  // stock: an untouched sample clears armed (re-armed while low)
      this->idle_reads_ = 0;
      if (this->state_ == State::ARMED)
        this->state_ = State::IDLE;
      this->set_slow_poll_(now);
      return;
    }
    this->idle_step_(now);  // GPIO32 still low without a touch: stuck-line accounting
  }

  /// After a DIAG report.
  void diag_done(uint32_t now) { this->last_diag_ = now; }

  State state() const { return this->state_; }
  /// Reads are due on the fixed SAMPLE_MS cadence (armed or touching); otherwise the task waits
  /// for the GPIO32 interrupt (at most one tick).
  bool fast(bool touching) const { return touching || this->state_ == State::ARMED; }
  /// Consecutive I2C errors (0 after a good read).
  uint32_t errors() const { return this->errors_; }
  uint32_t errors_total() const { return this->errors_total_; }
  uint32_t reads() const { return this->reads_; }
  uint32_t ready_edges() const { return this->edges_; }
  /// A state change worth one log line (returned once).
  Note take_note() {
    const Note n = this->note_;
    this->note_ = Note::NONE;
    return n;
  }

 protected:
  void set_slow_poll_(uint32_t now) {
    if (!this->has_pin_)
      this->next_read_ = now + IDLE_POLL_MS;
    else if (this->state_ == State::STUCK)
      this->next_read_ = now + STUCK_POLL_MS;
  }

  void idle_step_(uint32_t now) {
    if (!this->has_pin_ || this->state_ == State::STUCK) {
      this->set_slow_poll_(now);
      return;
    }
    if (this->state_ != State::ARMED)
      return;  // a diagnostic read
    if (++this->idle_reads_ < STUCK_READS)
      return;
    this->idle_reads_ = 0;
    if (this->ready_) {
      this->state_ = State::STUCK;
      this->note_ = Note::STUCK;
      this->set_slow_poll_(now);
    } else {
      this->state_ = State::IDLE;
    }
  }

  bool has_pin_{false};
  bool ready_{false};
  bool untouched_{false};  // the last read was OK and untouched
  State state_{State::IDLE};
  Note note_{Note::NONE};
  uint16_t idle_reads_{0};
  uint32_t last_read_{0};
  uint32_t last_diag_{0};
  uint32_t next_read_{0};
  uint32_t errors_{0};
  uint32_t errors_total_{0};
  uint32_t reads_{0};
  uint32_t edges_{0};
};

}  // namespace touch_schedule
