#pragma once
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/pmsa003i/pmsa003i.h"
#include <cmath>

namespace esphome::cgdn1_pm {
static constexpr const char *TAG = "cgdn1_pm";

// This controls the module's physical supply, not just its polling interval.
// The stock power latch remains owned by cgdn1_boot. GPIO18 is deliberately not
// touched: the two independent CGDN1 projects disagree on its exact function.
class Cgdn1Pm : public Component {
 public:
  void set_reader(pmsa003i::PMSA003IComponent *reader) { reader_ = reader; }
  void set_enable_pin(GPIOPin *pin) { enable_pin_ = pin; }
  void set_sensors(sensor::Sensor *pm1, sensor::Sensor *pm25, sensor::Sensor *pm10) {
    sensors_[0] = pm1; sensors_[1] = pm25; sensors_[2] = pm10;
  }
  void set_warmup_time(uint32_t ms) { warmup_ms_ = ms; }
  // The template switch restores before sensor setup. Save its request without
  // cutting the supply under PMSA003I::setup(), whose failed probe is permanent.
  void set_enabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (setup_done_) apply_();
  }
  bool enabled() const { return enabled_; }
  bool ready() const { return ready_; }
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }
  void setup() override {
    // cgdn1_boot already held the rail high for the reader's initial probe.
    // Preserve that level while configuring the output; then apply restoration.
    enable_pin_->digital_write(true);
    enable_pin_->setup();
    setup_done_ = true;
    apply_();
  }
  void dump_config() override {
    ESP_LOGCONFIG("cgdn1_pm", "PM module power: %s; readings: %s; warmup: %u ms",
                  enabled_ ? "ON" : "OFF", ready_ ? "ready" : "unavailable", (unsigned) warmup_ms_);
    LOG_PIN("  Enable pin: ", enable_pin_);
  }

 protected:
  void invalidate_() {
    // HA must see unknown/unavailable, never a convincing stale value or zero.
    for (auto *s : sensors_) if (s != nullptr) s->publish_state(NAN);
  }
  void apply_() {
    cancel_timeout("pm_warmup");
    reader_->stop_poller();
    ready_ = false;
    invalidate_();
    enable_pin_->digital_write(enabled_);
    ESP_LOGI("cgdn1_pm", "PM module power %s", enabled_ ? "ON (warming up)" : "OFF");
    if (!enabled_) {
      reader_->status_clear_warning();  // deliberately powered off is not an I2C failure
      return;
    }
    // Each wake starts a new warmup. Turning off cancels it, so an old callback
    // cannot restart polling after a quick ON/OFF or ON/OFF/ON sequence.
    set_timeout("pm_warmup", warmup_ms_, [this]() {
      if (!enabled_) return;
      if (reader_->is_failed()) {
        status_set_warning();
        ESP_LOGW("cgdn1_pm", "Reader failed its boot probe; keep readings unavailable");
        return;
      }
      ready_ = true;
      reader_->update();
      reader_->start_poller();
      status_clear_warning();
      ESP_LOGI("cgdn1_pm", "PM warmup complete; polling resumed");
    });
  }
  pmsa003i::PMSA003IComponent *reader_{nullptr};
  GPIOPin *enable_pin_{nullptr};
  sensor::Sensor *sensors_[3]{};
  uint32_t warmup_ms_{30000};
  bool enabled_{true}, ready_{false}, setup_done_{false};
};
}  // namespace esphome::cgdn1_pm
