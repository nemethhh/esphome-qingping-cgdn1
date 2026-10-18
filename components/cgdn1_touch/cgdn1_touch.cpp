#include "cgdn1_touch.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome::cgdn1_touch {

static const char *const TAG = "cgdn1_touch";

using touch_gesture::Gesture;
using touch_schedule::Action;
using touch_schedule::Note;

// I2C errors are logged at most once per ERR_LOG_MS (plus the first of a run).
static constexpr uint32_t ERR_LOG_MS = 60000;
// Sample queue task -> loop: 256 x 16 B = 2.56 s of continuous touch at 10 ms.
static constexpr UBaseType_t QUEUE_LEN = 256;
static constexpr uint32_t TASK_STACK = 6144;  // IDF I2C + its error logging (bus reset)
static constexpr UBaseType_t TASK_PRIO = 5;  // above loopTask (1), far below WiFi/lwIP
// A touch is "truncated" (no tap from it, gesture.h) when its first sample came later than LATE_MS
// after the GPIO32 edge, or a read failed within ERR_TRUNC_MS before it.
static constexpr uint16_t LATE_MS = 30;
static constexpr uint32_t ERR_TRUNC_MS = 100;
// Raw DEBUG lines per touch (the rest of a long touch is not logged).
static constexpr uint16_t RAW_LINES_MAX = 48;

void Cgdn1Touch::setup() {
  if (this->ready_pin_ != nullptr)
    this->ready_pin_->setup();
  this->sched_.set_has_ready_pin(this->ready_pin_ != nullptr);
  // Stock "Attiny814 IIC Init": configure GPIO32, then one probe read whose result is ignored
  // (0x400e22c4). On the device it fails while idle (8894bc8), so this is information only.
  uint8_t buf[2] = {0, 0};
  const i2c::ErrorCode err = this->read(buf, 2);
  if (err != i2c::ERROR_OK) {
    ESP_LOGI(TAG, "ATtiny814 at 0x%02X: probe read I2C error %d, GPIO32=%u (stock ignores it too)", this->address_,
             (int) err, this->ready_level_());
  } else {
    ESP_LOGI(TAG, "ATtiny814 at 0x%02X: pos=%u status=0x%02X, GPIO32=%u", this->address_, buf[0], buf[1],
             this->ready_level_());
  }

  this->queue_ = xQueueCreate(QUEUE_LEN, sizeof(Rec));
  if (this->queue_ == nullptr ||
      xTaskCreate(&Cgdn1Touch::task_entry_, "cgdn1_touch", TASK_STACK, this, TASK_PRIO, &this->task_) != pdPASS) {
    ESP_LOGE(TAG, "Cannot start the sampling task");
    this->mark_failed();
    return;
  }
  // Wake the task at once when GPIO32 is asserted (logical rising = physical falling edge with
  // `inverted: true`): the first sample of a touch is read within ~1 ms, not at the next tick.
  if (this->ready_pin_ != nullptr)
    this->ready_pin_->attach_interrupt(&Cgdn1Touch::gpio_isr_, this, gpio::INTERRUPT_RISING_EDGE);
}

void IRAM_ATTR Cgdn1Touch::gpio_isr_(Cgdn1Touch *self) {
  self->edge_ms_ = millis();
  self->isr_edges_ = self->isr_edges_ + 1;
  if (self->task_ == nullptr)
    return;
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(self->task_, &woken);
  portYIELD_FROM_ISR(woken);
}

void Cgdn1Touch::task_entry_(void *arg) { static_cast<Cgdn1Touch *>(arg)->task_loop_(); }

// Stock Touch_Task, one tick = SAMPLE_MS: while armed or touching a fixed 10 ms cadence
// (vTaskDelayUntil), otherwise wait up to one tick for the GPIO32 interrupt.
void Cgdn1Touch::task_loop_() {
  const TickType_t period = pdMS_TO_TICKS(touch_schedule::SAMPLE_MS);
  TickType_t wake = xTaskGetTickCount();
  bool touching = false;
  for (;;) {
    bool edge;
    if (this->sched_.fast(touching)) {
      vTaskDelayUntil(&wake, period);
      edge = ulTaskNotifyTake(pdTRUE, 0) > 0;
    } else {
      edge = ulTaskNotifyTake(pdTRUE, period) > 0;
      wake = xTaskGetTickCount();
    }
    const uint32_t now = millis();
    // An edge since the last tick arms like a low level would (a very short pulse is not lost).
    const Action action = this->sched_.poll(now, this->ready_() || edge, touching);
    this->sched_edges_ = this->sched_.ready_edges();

    const Note note = this->sched_.take_note();
    if (note != Note::NONE) {
      Rec r{};
      r.t = now;
      r.kind = REC_NOTE;
      r.note = (uint8_t) note;
      r.level = this->ready_level_();
      this->push_(r);
    }
    if (action == Action::NONE)
      continue;

    Rec r{};
    r.t = now;
    r.level = this->ready_level_();
    const uint32_t age = now - this->edge_ms_;
    r.edge_age = (this->isr_edges_ == 0 || age >= 0xFFFF) ? 0xFFFF : (uint16_t) age;
    if (action == Action::DIAG) {
      // Report only: stock never reads an idle chip, and on the device that read fails.
      r.kind = REC_DIAG;
      this->sched_.diag_done(now);
      this->push_(r);
      continue;
    }

    uint8_t buf[2] = {0, 0};
    // Stock FUN_400e26b8 -> FUN_400eddc0(1, 0x19, buf, 2): START, 0x19 (= 0x0C read), 1 byte ACK,
    // 1 byte NACK, STOP; no register byte, never a write. read() is exactly that transaction.
    const i2c::ErrorCode err = this->read(buf, 2);
    const bool ok = err == i2c::ERROR_OK;
    bool released = false;
    if (ok) {
      const bool t = (buf[1] & 0x80) != 0;
      released = touching && !t;  // stock disarms after the release sample
      touching = t;
    }
    this->sched_.done(now, action, ok, touching, released);
    if (!ok) {
      this->errors_total_ = this->errors_total_ + 1;
      if (this->sched_.errors() >= touch_schedule::ERR_SLOW_AFTER)
        touching = false;  // the loop drops the touch in progress too
    }
    this->reads_ = this->reads_ + 1;
    r.kind = REC_READ;
    r.b0 = buf[0];
    r.b1 = buf[1];
    r.err = (uint8_t) err;
    r.err_run = (uint16_t) std::min<uint32_t>(this->sched_.errors(), 0xFFFF);
    this->push_(r);
  }
}

void Cgdn1Touch::push_(const Rec &r) {
  if (xQueueSend(this->queue_, &r, 0) != pdTRUE)
    this->drops_ = this->drops_ + 1;
  App.wake_loop_threadsafe();
}

void Cgdn1Touch::dump_config() {
  ESP_LOGCONFIG(TAG, "CGDN1 touch slider (ATtiny814):");
  LOG_I2C_DEVICE(this);
  LOG_PIN("  Data-ready pin: ", this->ready_pin_);
  ESP_LOGCONFIG(TAG,
                "  GPIO32 level: %u\n"
                "  Sampling task: %s, 10 ms ticks, GPIO32 interrupt\n"
                "  Reads: %" PRIu32 ", I2C errors: %" PRIu32 " (last %u), ready edges: %" PRIu32 " (ISR %" PRIu32
                "), queue drops: %" PRIu32 "\n"
                "  Touches: %" PRIu32 ", gestures: %" PRIu32 ", truncated: %" PRIu32
                ", max first-sample latency: %u ms\n"
                "  Last raw: %02X %02X, last gesture: %s",
                this->ready_level_(), this->task_ != nullptr ? "running" : "NOT running", (uint32_t) this->reads_,
                (uint32_t) this->errors_total_, this->last_err_, (uint32_t) this->sched_edges_,
                (uint32_t) this->isr_edges_, (uint32_t) this->drops_, this->touches_, this->gestures_,
                this->truncated_, this->max_first_age_, this->last_raw_[0], this->last_raw_[1], this->last_gesture());
}

bool Cgdn1Touch::ready_() const { return this->ready_pin_ != nullptr && this->ready_pin_->digital_read(); }

uint8_t Cgdn1Touch::ready_level_() const {
  if (this->ready_pin_ == nullptr)
    return 2;
  return this->ready_pin_->digital_read() != this->ready_pin_->is_inverted() ? 1 : 0;
}

void Cgdn1Touch::log_raw_(const Rec &r) {
  const bool ok = r.err == i2c::ERROR_OK;
  const bool changed =
      r.err != this->last_err_ || (ok && (!this->raw_valid_ || r.b0 != this->last_raw_[0] || r.b1 != this->last_raw_[1]));
  if (changed && this->raw_lines_ < RAW_LINES_MAX) {
    this->raw_lines_++;
    if (ok) {
      ESP_LOGD(TAG, "raw %02X %02X (pos %u%s%s) t=%" PRIu32 " GPIO32=%u", r.b0, r.b1, r.b0, (r.b1 & 0x80) ? ", touched" : "",
               (r.b1 & touch_gesture::MOVE_LEFT) ? ", ->left" : (r.b1 & touch_gesture::MOVE_RIGHT) ? ", ->right" : "",
               r.t, r.level);
    } else {
      ESP_LOGD(TAG, "raw read: I2C error %u t=%" PRIu32 " GPIO32=%u", r.err, r.t, r.level);
    }
  }
  this->last_err_ = r.err;
  if (ok) {
    this->last_raw_[0] = r.b0;
    this->last_raw_[1] = r.b1;
    this->raw_valid_ = true;
  }
}

void Cgdn1Touch::emit_(Gesture g) {
  this->last_gesture_ = g;
  this->gestures_++;
  const auto s = this->decoder_.stats();
  ESP_LOGI(TAG, "%s (SPOS %d RPOS %d TT %d speed %d span %d, %" PRIu32 " ms, first +%u ms%s%s)",
           touch_gesture::gesture_name(g), s.spos, s.rpos, s.tt, s.speed, s.span, s.duration_ms, this->first_edge_age_,
           s.moved ? ", moving" : "", s.truncated ? ", truncated" : "");
  this->gesture_cb_.call(std::string(touch_gesture::gesture_name(g)));
}

void Cgdn1Touch::loop() {
  if (this->queue_ == nullptr)
    return;
  Rec r;
  // Bounded per loop() so a flood can never starve the other components.
  for (int n = 0; n < 64 && xQueueReceive(this->queue_, &r, 0) == pdTRUE; n++)
    this->handle_(r);
}

void Cgdn1Touch::handle_(const Rec &r) {
  if (this->drops_ != this->drops_seen_) {
    ESP_LOGW(TAG, "Sample queue overflow (%" PRIu32 " dropped): touch in progress discarded",
             (uint32_t) this->drops_ - this->drops_seen_);
    this->drops_seen_ = this->drops_;
    if (this->decoder_.active()) {
      this->decoder_.abort();
      this->release_cb_.call();
    }
    this->had_err_ = true;  // the next touch-down may be mid-touch: no tap from it
    this->last_err_t_ = r.t;
  }

  if (r.kind == REC_NOTE) {
    if (r.note == (uint8_t) Note::STUCK) {
      ESP_LOGW(TAG, "Data-ready line asserted without a touch: polling every %" PRIu32 " ms",
               touch_schedule::STUCK_POLL_MS);
    } else if (r.note == (uint8_t) Note::UNSTUCK) {
      ESP_LOGI(TAG, "Data-ready line released");
    }
    return;
  }
  if (r.kind == REC_DIAG) {
    // Rate-limited (one per DIAG_MS without reads) proof of life for device tests. No I2C read.
    ESP_LOGI(TAG,
             "Idle check: GPIO32=%u; reads %" PRIu32 ", I2C errors %" PRIu32 ", ready edges %" PRIu32 " (ISR %" PRIu32
             "), touches %" PRIu32 ", gestures %" PRIu32 ", truncated %" PRIu32 ", max first-sample latency %u ms, "
             "drops %" PRIu32 ", last gesture %s",
             r.level, (uint32_t) this->reads_, (uint32_t) this->errors_total_, (uint32_t) this->sched_edges_,
             (uint32_t) this->isr_edges_, this->touches_, this->gestures_, this->truncated_, this->max_first_age_,
             (uint32_t) this->drops_, this->last_gesture());
    return;
  }

  const bool ok = r.err == i2c::ERROR_OK;
  if (ok && (r.b1 & 0x80) && !this->decoder_.active())
    this->raw_lines_ = 0;  // a new touch: its raw lines are logged again
  this->log_raw_(r);
  if (!ok) {
    if (r.err_run == 1 || r.t - this->err_log_ms_ >= ERR_LOG_MS) {
      ESP_LOGW(TAG, "Touch read error %u (%u in a row, %" PRIu32 " total), GPIO32=%u", r.err, r.err_run,
               (uint32_t) this->errors_total_, r.level);
      this->err_log_ms_ = r.t;
    }
    if (r.err_run == 1)
      this->status_set_warning();
    this->had_err_ = true;
    this->last_err_t_ = r.t;
    if (r.err_run >= touch_schedule::ERR_SLOW_AFTER && this->decoder_.active()) {
      this->decoder_.abort();  // a touch in progress is dropped (no gesture)
      this->release_cb_.call();
    }
    return;
  }
  if (this->status_has_warning()) {
    ESP_LOGI(TAG, "Touch reads OK again");
    this->status_clear_warning();
  }

  const auto res = this->decoder_.feed(r.b0, (r.b1 & 0x80) != 0, r.t, r.b1);
  if (res.pressed) {
    this->touches_++;
    this->first_edge_age_ = r.edge_age == 0xFFFF ? 0 : r.edge_age;
    if (r.edge_age != 0xFFFF && r.edge_age > this->max_first_age_ && r.edge_age < 1000)
      this->max_first_age_ = r.edge_age;
    // The start of this touch was missed: read error just before it, or the first sample came
    // late after a recent GPIO32 edge. No tap from it (gesture.h), only a swipe.
    const bool err_before = this->had_err_ && r.t - this->last_err_t_ <= ERR_TRUNC_MS;
    const bool late = r.edge_age != 0xFFFF && r.edge_age > LATE_MS && r.edge_age < 1000;
    if (err_before || late)
      this->decoder_.mark_truncated();
    if (this->decoder_.truncated())
      this->truncated_++;
    this->touch_cb_.call();
  }
  if (res.released) {
    this->had_err_ = false;
    if (res.gesture != Gesture::NONE) {
      this->emit_(res.gesture);  // one log line per touch: the gesture with its numbers ...
    } else {                     // ... or stock's release line
      const auto s = this->decoder_.stats();
      ESP_LOGD(TAG, "release SPOS:%d RPOS:%d TT:%d release_speed:%d span:%d, %" PRIu32 " ms, first +%u ms%s%s (no gesture)",
               s.spos, s.rpos, s.tt, s.speed, s.span, s.duration_ms, this->first_edge_age_, s.moved ? ", moving" : "",
               s.truncated ? ", truncated" : "");
    }
    this->release_cb_.call();
  } else if (res.gesture != Gesture::NONE) {
    this->emit_(res.gesture);  // LONG_TOUCH, while held
  }
}

}  // namespace esphome::cgdn1_touch
