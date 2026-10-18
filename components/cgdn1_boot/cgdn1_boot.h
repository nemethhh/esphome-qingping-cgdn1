#pragma once
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include <cstdint>

#ifdef CGDN1_FLASH_LOG
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "esp_partition.h"
#endif

namespace esphome {
namespace cgdn1_boot {

// Referenced by the generated main.cpp so the linker keeps cgdn1_boot.cpp (and
// its global constructor that holds the power latch) from being dropped from
// the static archive. Optionally (trial_mode: true) marks the other OTA slot as
// the next boot, every boot, and (flash_log: true) persists the ESPHome log, IDF
// driver logs included, to the top 64 KB of the `test` partition for a device
// without UART or network.
// Also owns the one deliberate power-off path (back-button long press).
class Cgdn1Boot : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BUS; }

  // Back-button long press (called from cgdn1.yaml once the button has been held
  // >= 2 s). Starts the stock-like power-off unless it is not armed yet (no
  // post-boot release seen) or the unit is on USB power. Non-blocking: the
  // sequence runs in loop().
  void power_off_long_press();
  bool power_off_armed() const { return this->armed_; }
  // The power-off sequence has started (status LED goes dark, like stock FUN_400e9b10).
  bool powering_off() const { return this->power_state_ != PowerOff::IDLE; }

  // Make the other OTA slot the next boot and reboot into it. Right after the
  // install that slot holds the stock firmware; after an ESPHome OTA update it
  // holds the previous ESPHome image. IDF validates the image first: if it is
  // not bootable nothing changes and this returns false.
  bool boot_other_slot();

#ifdef CGDN1_FLASH_LOG
  // Called from the generated main.cpp right after the logger's pre_setup(),
  // before App.setup(): opens the partition, writes the boot header and hooks
  // the logger, so every later log line (incl. other components' setup) is kept.
  void start_flash_log();
  void on_shutdown() override;
  void on_powerdown() override;
#endif

 protected:
  enum class PowerOff : uint8_t { IDLE, RAIL_OFF, WAIT_RELEASE, AFTER_RELEASE };
  void power_loop_();
  void flash_log_hold_(bool hold);

  PowerOff power_state_{PowerOff::IDLE};
  uint32_t power_t_{0};          // millis() when the current power-off step began
  uint32_t released_since_{0};   // millis() since GPIO36 reads released (0 = pressed)
  bool armed_{false};
  bool disarmed_noted_{false};

#ifdef CGDN1_FLASH_LOG
  static constexpr size_t BUF_SIZE = 4096;  // per half of the double buffer
  static constexpr size_t IDF_PEND = 192;   // IDF line being assembled from pieces
  static int idf_vprintf_hook(const char *fmt, va_list args);
  void on_idf_(const char *piece, size_t len);
  void on_log_(uint8_t level, const char *tag, const char *msg, size_t len);
  void ingest_(uint8_t level, const char *msg, size_t len, bool exempt, bool on_main);
  void append_raw_(const char *data, size_t len);
  void write_(const char *data, size_t len);
  void flush_();
  void flash_loop_();
  void log_status_();

  const esp_partition_t *part_{nullptr};
  uint32_t write_off_{0};
  bool ready_{false};
  bool full_{false};
  bool flushing_{false};
  bool hold_{false};  // power-off in progress: no flash writes while power may drop
  bool loop_seen_{false};
  void *main_task_{nullptr};
  uint32_t last_flush_ms_{0};
  uint32_t last_status_ms_{0};
  uint32_t dropped_{0};
  esp_err_t last_write_err_{0};
  const char *refuse_{"not started"};
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  char buf_[2][BUF_SIZE];
  size_t len_[2]{0, 0};
  uint8_t active_{0};
  char idf_buf_[IDF_PEND];
  size_t idf_len_{0};
  void *idf_task_{nullptr};
#endif
};

}  // namespace cgdn1_boot
}  // namespace esphome
