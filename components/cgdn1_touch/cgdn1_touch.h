#pragma once
// CGDN1 top touch slider: ATtiny814 capacitive-slider controller @0x0C on I2C bus 1, exactly as the
// stock 4.8.5 app reads it:
//  - register-less 2-byte read (FUN_400e26b8 -> FUN_400eddc0(1, 0x19, buf, 2)); byte0 = position,
//    byte1 bit7 = finger present (bits 2/3: moving toward the right/left end, see gesture.h).
//    The ESP never writes to the chip.
//  - GPIO32 = data-ready, active low, input + pull-up (FUN_400e2678 / FUN_400e26a0); on the device
//    it is low exactly while a finger is on the slider.
//  - "Init" = configure GPIO32, wait, one probe read whose result stock ignores (FUN_400e2268 at
//    0x400e22c4). A failing probe never disables the slider here either.
// Sampling runs like stock's Touch_Task: a dedicated FreeRTOS task ticks every 10 ms (and is woken
// at once by a GPIO32 interrupt), applies the stock read schedule (schedule.h) and does the I2C
// reads; bus 1 has no other device, so no other code touches it after setup. Each sample (with
// its timestamp) goes through a queue to the main loop, which logs, decodes (gesture.h, stock
// thresholds) and fires the automations. Up to 2ede230 the main loop itself polled GPIO32 and read
// the chip, so a busy loop (display redraws, API, WiFi) delayed or missed the first samples of a
// touch: short taps were lost and swipes seen from their middle.
#include <string>

#include "gesture.h"
#include "schedule.h"

#include "esphome/components/i2c/i2c.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace esphome::cgdn1_touch {

class Cgdn1Touch : public Component, public i2c::I2CDevice {
 public:
  void set_ready_pin(InternalGPIOPin *pin) { this->ready_pin_ = pin; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  template<typename F> void add_on_touch_callback(F &&cb) { this->touch_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_release_callback(F &&cb) { this->release_cb_.add(std::forward<F>(cb)); }
  template<typename F> void add_on_gesture_callback(F &&cb) { this->gesture_cb_.add(std::forward<F>(cb)); }

  /// Finger on the slider right now (as far as the main loop has processed the samples).
  bool is_touched() const { return this->decoder_.active(); }
  /// Last gesture name ("none" before the first one).
  const char *last_gesture() const { return touch_gesture::gesture_name(this->last_gesture_); }

 protected:
  /// One record from the sampling task to the main loop.
  struct Rec {
    uint32_t t;          // millis() of the read / report
    uint16_t edge_age;   // ms since the last GPIO32 assert edge (0xFFFF: none / >= 65 s)
    uint16_t err_run;    // consecutive I2C errors including this read (0 = this read OK)
    uint8_t kind;        // Kind
    uint8_t b0, b1;      // raw bytes (READ, OK)
    uint8_t err;         // i2c::ErrorCode (READ)
    uint8_t level;       // GPIO32 electrical level at the read (0/1, 2 = no pin)
    uint8_t note;        // touch_schedule::Note (NOTE)
  };
  enum Kind : uint8_t { REC_READ = 0, REC_DIAG = 1, REC_NOTE = 2 };

  static void task_entry_(void *arg);
  static void gpio_isr_(Cgdn1Touch *self);
  void task_loop_();
  void push_(const Rec &r);
  void handle_(const Rec &r);

  bool ready_() const;
  /// GPIO32 electrical level (0/1), 2 without a data-ready pin.
  uint8_t ready_level_() const;
  void log_raw_(const Rec &r);
  void emit_(touch_gesture::Gesture g);

  InternalGPIOPin *ready_pin_{nullptr};
  touch_gesture::Decoder decoder_;
  touch_gesture::Gesture last_gesture_{touch_gesture::Gesture::NONE};
  CallbackManager<void()> touch_cb_;
  CallbackManager<void()> release_cb_;
  CallbackManager<void(std::string)> gesture_cb_;

  // ---- sampling task (touch_schedule state is task-private) ----
  touch_schedule::Scheduler sched_;
  TaskHandle_t task_{nullptr};
  QueueHandle_t queue_{nullptr};
  volatile uint32_t edge_ms_{0};      // ISR: millis() of the last GPIO32 assert edge
  volatile uint32_t isr_edges_{0};    // ISR: edges seen
  // Written by the task, read by the loop for dump_config / Idle check (plain counters).
  volatile uint32_t reads_{0};
  volatile uint32_t errors_total_{0};
  volatile uint32_t sched_edges_{0};
  volatile uint32_t drops_{0};

  // ---- main loop ----
  uint32_t drops_seen_{0};
  uint8_t last_raw_[2]{0, 0};
  bool raw_valid_{false};
  uint8_t last_err_{0};
  uint32_t err_log_ms_{0};
  uint32_t last_err_t_{0};
  bool had_err_{false};
  uint16_t raw_lines_{0};       // raw DEBUG lines logged for the current touch
  uint16_t first_edge_age_{0};  // the current touch's first-sample latency after GPIO32
  uint32_t touches_{0}, gestures_{0}, truncated_{0};
  uint16_t max_first_age_{0};
};

}  // namespace esphome::cgdn1_touch
