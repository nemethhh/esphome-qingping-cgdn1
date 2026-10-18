#include "cgdn1_boot.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"

#ifdef CGDN1_FLASH_LOG
#include <cstdio>
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "esphome/core/application.h"
#ifndef USE_LOGGER
#error "cgdn1_boot flash_log needs the logger component"
#endif
#include "esphome/components/logger/logger.h"
#ifdef USE_WIFI
#include "esp_wifi.h"
#include "esphome/components/wifi/wifi_component.h"
#endif
#endif

// SAFETY CRITICAL: GPIO23 is the board power-hold latch. If it is not driven
// HIGH early the device powers itself off, and there is no UART to recover.
// Nothing in this file may drive GPIO23 low, with exactly ONE deliberate
// exception: the back-button long-press power-off (power_off_long_press() /
// power_loop_()). It is armed only after GPIO36 has read released for 200 ms
// at >= 5 s uptime (so the power-on hold can never trigger it), is refused while
// GPIO34 reads LOW (USB/charging), and starts only while GPIO36 reads pressed
// (the held button keeps the board powered until its release); if the CPU is
// still running 10 s after the release, GPIO23/13 are driven HIGH again. Never add another path that drives GPIO23 low.

namespace esphome {
namespace cgdn1_boot {

static const char *const TAG = "cgdn1_boot";

#ifdef CGDN1_TRIAL_MODE
// Result of the constructor-time mark, logged again from setup() once the
// ESPHome logger is up. ESP_ERR_INVALID_STATE = the mark never ran.
static esp_err_t s_mark_err = ESP_ERR_INVALID_STATE;
#endif

static void drive_high(gpio_num_t pin) {
  // Latch HIGH in the output register first, then enable the output, so the
  // pin never glitches low.
  gpio_set_level(pin, 1);
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << pin;
  cfg.mode = GPIO_MODE_OUTPUT;
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&cfg);
  gpio_set_level(pin, 1);
}

// Runs from do_global_ctors() (esp_system/startup.c): after the CORE init stage
// (heap, flash driver, partition/MMU support are up), before app_main and before
// esp_ipc_init. The OTA mark needs no IPC, so this is safe that early. No claim
// is made about the order relative to other constructors.
__attribute__((constructor(101))) static void cgdn1_power_latch() {
  drive_high(GPIO_NUM_23);  // power-hold
  drive_high(GPIO_NUM_13);
  drive_high(GPIO_NUM_18);

#ifdef CGDN1_TRIAL_MODE
  // Trial policy (no health check, no confirm): make the other OTA slot the next
  // boot, every boot. The other slot holds the firmware ESPHome was installed from
  // (e.g. stock), so any later crash or reboot lands back on it. One attempt only;
  // never retry, never reboot on failure.
  // Logging here must use IDF's ESP_EARLY_LOGx: ESPHome redefines ESP_LOGx to go
  // through logger::global_logger, which does not exist yet (nullptr).
  s_mark_err = esp_ota_set_boot_partition(esp_ota_get_next_update_partition(nullptr));
  if (s_mark_err != ESP_OK) {
    ESP_EARLY_LOGE(TAG, "trial mark failed: %s", esp_err_to_name(s_mark_err));
  }
#endif
}

// Back power button and charge sense; same pins as the cgdn1.yaml binary
// sensors (stock 4.8.5). Both are input-only pins with EXTERNAL pull-ups
// (GPIO34-39 have no internal pulls): button pressed = LOW, USB/charging = LOW.
static constexpr gpio_num_t BUTTON_PIN = GPIO_NUM_36;
static constexpr gpio_num_t CHARGE_PIN = GPIO_NUM_34;
static constexpr uint32_t ARM_MIN_UPTIME_MS = 5000;   // never arm earlier than this
static constexpr uint32_t ARM_RELEASED_MS = 200;      // released this long -> armed
static constexpr uint32_t DISARMED_NOTE_MS = 30000;   // one "still disarmed" note
static constexpr uint32_t RAIL_SETTLE_MS = 300;       // GPIO13 low -> GPIO23 low
static constexpr uint32_t RELEASE_STABLE_MS = 100;    // button counts as released
static constexpr uint32_t FALLBACK_MS = 10000;        // still running -> re-latch

static void config_input(gpio_num_t pin) {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << pin;
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&cfg);
}

static bool on_usb_power() { return gpio_get_level(CHARGE_PIN) == 0; }

// Backstop re-assert (never drives low), then report the mark result.
void Cgdn1Boot::setup() {
  drive_high(GPIO_NUM_23);
  drive_high(GPIO_NUM_13);
  drive_high(GPIO_NUM_18);
  // Readable even if the YAML sensors change (ESPHome's gpio binary sensors set
  // these up again later, identically: input, no pulls).
  config_input(BUTTON_PIN);
  config_input(CHARGE_PIN);
#ifdef CGDN1_TRIAL_MODE
  if (s_mark_err == ESP_OK) {
    ESP_LOGI(TAG, "trial mode: next boot marked: the other slot");
  } else {
    ESP_LOGE(TAG, "trial mark FAILED (%s): the next boot is ESPHome again", esp_err_to_name(s_mark_err));
  }
#endif
#ifdef CGDN1_FLASH_LOG
  if (this->ready_) {
    ESP_LOGI(TAG, "flash log: appending at 0x%06" PRIX32, this->part_->address + this->write_off_);
  } else {
    ESP_LOGE(TAG, "flash log OFF: %s", this->refuse_);
  }
#endif
}

void Cgdn1Boot::dump_config() {
  ESP_LOGCONFIG(TAG, "CGDN1 boot:");
  const esp_partition_t *running = esp_ota_get_running_partition();
  ESP_LOGCONFIG(TAG, "  Running from: %s", running != nullptr ? running->label : "?");
#ifdef CGDN1_TRIAL_MODE
  ESP_LOGCONFIG(TAG, "  Trial mark result: %s (%s)", s_mark_err == ESP_OK ? "OK" : "FAILED", esp_err_to_name(s_mark_err));
#else
  ESP_LOGCONFIG(TAG, "  Trial mode: off");
#endif
#ifdef CGDN1_FLASH_LOG
  ESP_LOGCONFIG(TAG, "  Flash log: %s", this->ready_ ? "on" : this->refuse_);
#endif
  ESP_LOGCONFIG(TAG, "  Long-press power-off: %s",
                this->armed_ ? "armed (on battery only)" : "disarmed (needs a post-boot button release)");
}

// ---------------------------------------------------------------------------
// Back-button power-off (stock FUN_400e7990/FUN_400e9b10): GPIO13 rail low,
// short wait, GPIO23 latch low ("System_Power_Off"), wait for the release. The
// button itself powers the board while held, so power drops on the release.
// Stock does not power off on USB; it shows a charge-while-off screen instead
// (not implemented here: the request is simply ignored).
// ---------------------------------------------------------------------------

void Cgdn1Boot::power_off_long_press() {
  if (this->power_state_ != PowerOff::IDLE)
    return;  // already in progress
  if (!this->armed_) {
    ESP_LOGW(TAG, "long press ignored: power-off disarmed (back button not seen released since boot)");
    return;
  }
  if (gpio_get_level(BUTTON_PIN) != 0) {
    ESP_LOGW(TAG, "long press ignored: GPIO36 no longer reads pressed");
    return;
  }
  if (on_usb_power()) {
    ESP_LOGW(TAG, "on USB power: power-off ignored");
    return;
  }
  ESP_LOGW(TAG, "power off requested (back button long press)");
  this->flash_log_hold_(false);  // synchronous flush: the line is in flash now
  gpio_set_level(GPIO_NUM_13, 0);
  this->power_t_ = millis();
  this->power_state_ = PowerOff::RAIL_OFF;
}

bool Cgdn1Boot::boot_other_slot() {
  const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
  if (other == nullptr) {
    ESP_LOGE(TAG, "boot other slot: no other OTA slot");
    return false;
  }
  const esp_err_t err = esp_ota_set_boot_partition(other);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "boot other slot: %s holds no bootable image (%s)", other->label, esp_err_to_name(err));
    return false;
  }
  ESP_LOGW(TAG, "next boot: %s; rebooting", other->label);
  App.safe_reboot();
  return true;
}

void Cgdn1Boot::power_loop_() {
  const uint32_t now = millis();
  if (gpio_get_level(BUTTON_PIN) == 0) {
    this->released_since_ = 0;
  } else if (this->released_since_ == 0) {
    this->released_since_ = now != 0 ? now : 1;
  }
  const uint32_t released_for = this->released_since_ != 0 ? now - this->released_since_ : 0;

  switch (this->power_state_) {
    case PowerOff::IDLE:
      if (this->armed_)
        break;
      if (now >= ARM_MIN_UPTIME_MS && released_for >= ARM_RELEASED_MS) {
        this->armed_ = true;
        ESP_LOGI(TAG, "power-off armed: back button seen released (hold >= 2 s on battery = power off)");
      } else if (!this->disarmed_noted_ && now >= DISARMED_NOTE_MS) {
        this->disarmed_noted_ = true;
        ESP_LOGW(TAG, "power-off disarmed: back button (GPIO36) not seen released since boot");
      }
      break;

    case PowerOff::RAIL_OFF:
      if (now - this->power_t_ < RAIL_SETTLE_MS)
        break;
      if (on_usb_power()) {  // USB plugged in during the wait: back out
        drive_high(GPIO_NUM_13);
        this->power_state_ = PowerOff::IDLE;
        ESP_LOGW(TAG, "on USB power: power-off aborted, GPIO13 rail restored");
        break;
      }
      ESP_LOGW(TAG, "power off: GPIO13 low, GPIO23 low now; power drops when the button is released");
      // Flush, then keep the flash log in RAM: no flash write may be in flight
      // when power drops (a torn page would make the next boot refuse the log).
      this->flash_log_hold_(true);
      gpio_set_level(GPIO_NUM_23, 0);
      this->power_t_ = now;
      this->power_state_ = PowerOff::WAIT_RELEASE;
      break;

    case PowerOff::WAIT_RELEASE:
      if (released_for >= RELEASE_STABLE_MS) {
        ESP_LOGI(TAG, "power off: button released");
        this->power_t_ = now;
        this->power_state_ = PowerOff::AFTER_RELEASE;
      }
      break;

    case PowerOff::AFTER_RELEASE:
      if (now - this->power_t_ < FALLBACK_MS)
        break;
      drive_high(GPIO_NUM_23);
      drive_high(GPIO_NUM_13);
      this->power_state_ = PowerOff::IDLE;
      this->flash_log_hold_(false);
      ESP_LOGE(TAG, "power-off did not take; latch re-asserted");
      break;
  }
}

// hold=false: release any hold and flush now. hold=true: flush now, then keep
// later lines in RAM until released.
void Cgdn1Boot::flash_log_hold_(bool hold) {
#ifdef CGDN1_FLASH_LOG
  this->hold_ = false;
  this->flush_();
  this->hold_ = hold;
#endif
}

void Cgdn1Boot::loop() {
  this->power_loop_();
#ifdef CGDN1_FLASH_LOG
  this->flash_loop_();
#endif
}

#ifdef CGDN1_FLASH_LOG
// ---------------------------------------------------------------------------
// Flash-backed log in the top 64 KB of the `test` partition (0x3e3000-0x3f2fff).
//
// Layout: plain printable-ASCII text appended across boots; erased flash (0xFF)
// marks the end. Ownership interlock: see start_flash_log().
// Each boot starts with "\n=== CGDN1LOG boot <n> ... ===". The text never
// contains 0xFF, so the first 0xFF byte is the append position.
//
// Sources: ESPHome lines via the logger's log callback (main task; other tasks'
// lines are replayed by the logger from its task log buffer), and IDF lines
// (wifi/phy driver) via our own esp_log vprintf hook chained in front of
// ESPHome's, on whatever task logs them. Both only copy into a RAM double
// buffer under a spinlock. Flash is written only on the main task:
//   - from loop(): every 500 ms or once 1 KB is pending (plus a status line / 10 s);
//   - synchronously from the callback ONLY on the main task and only (a) until
//     our first loop() -- during App.setup() loop() does not run at all, so a
//     crash in a later component's setup (WiFi start) would otherwise lose
//     everything -- or (b) for WARN/ERROR lines;
//   - from on_shutdown()/on_powerdown() before a reboot.
// Our own flush path never logs; a line arriving on the main task while we are
// flushing (e.g. an IDF flash error) is dropped (no recursion).
//
// Budget: a partition fill during a boot stops writing (earliest lines kept) and
// leaves one "=== log full ===" marker; never erases mid-boot. At boot the
// region is erased only when it is provably ours and < 8 KB is free (noted in
// the header). Spam guard: consecutive lines that differ only in digits are
// collapsed into one "[repeated N times]" line, and a byte token bucket (16 KB
// burst, 64 B/s) drops excess lines (counted) -- header/status lines bypass it.
// ---------------------------------------------------------------------------

// Log region inside `test`: 0x3e3000..0x3f2fff (top 64 KB). The low part of
// `test` is NOT free: the stock firmware keeps config_store A/B (0x3d0000/
// 0x3d1000: broker config + calibration), the device id/serial (0x3d2000) and a
// sensor-history ring (index 0x3d3000, records from 0x3d4000) there.
static constexpr uint32_t REGION_OFF = 0x13000;
static constexpr uint32_t REGION_SIZE = 0x10000;
static constexpr uint32_t MIN_FREE = 8 * 1024;
static constexpr uint32_t FLUSH_MS = 500;
static constexpr size_t FLUSH_BYTES = 1024;
static constexpr uint32_t STATUS_MS = 10000;
static constexpr size_t MAX_LINE = 320;
static constexpr int32_t BUCKET_CAP = 16 * 1024;
static constexpr int32_t BUCKET_RATE = 64;  // bytes per second
static const char FULL_MARKER[] = "\n=== log full ===\n";
static constexpr uint32_t FULL_RESERVE = sizeof(FULL_MARKER) - 1;
static const char HEADER_PAT[] = "\n=== CGDN1LOG";

// Rate-limit / repeat state (touched only under mux_ in on_log_).
static uint32_t s_last_hash = 0;
static uint32_t s_repeats = 0;
static int32_t s_tokens = BUCKET_CAP;
static uint32_t s_bucket_ms = 0;
static uint32_t s_limited = 0;

static Cgdn1Boot *s_self = nullptr;
static vprintf_like_t s_prev_vprintf = nullptr;

// True for the only bytes our log ever contains (format_line guarantees it).
static inline bool is_log_byte(uint8_t c) { return c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F); }

static uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

static const char *reset_name(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "OTHER";
  }
}

// "[1234.567] " + message with ANSI escapes stripped, CR dropped, inner newlines
// as spaces, other control bytes and 0xFF as '?', trailing newline. Returns length.
// *hash = FNV-1a over the body ignoring digits (for repeat collapsing).
static size_t format_line(char *out, size_t cap, const char *msg, size_t len, uint32_t *hash) {
  uint32_t ms = now_ms();
  int n = snprintf(out, cap, "[%" PRIu32 ".%03" PRIu32 "] ", ms / 1000, ms % 1000);
  const size_t prefix = n > 0 ? static_cast<size_t>(n) : 0;
  size_t o = prefix;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len && o + 1 < cap; i++) {
    uint8_t c = static_cast<uint8_t>(msg[i]);
    if (c == 0) break;
    if (c == 0x1B) {  // ESC [ params final-byte
      if (i + 1 < len && msg[i + 1] == '[') {
        i += 2;
        while (i < len && !(msg[i] >= 0x40 && msg[i] <= 0x7E)) i++;
      }
      continue;
    }
    if (c == '\r') continue;
    if (c == '\n') c = ' ';
    else if (!is_log_byte(c)) c = '?';  // control bytes, 0xFF and non-ASCII
    out[o++] = static_cast<char>(c);
    if (c < '0' || c > '9') h = (h ^ c) * 16777619u;
  }
  while (o > prefix && out[o - 1] == ' ') o--;  // trailing newline(s) became spaces
  if (o == prefix)
    return 0;  // nothing but whitespace/escapes (e.g. a lone IDF "\n")
  out[o++] = '\n';
  *hash = h;
  return o;
}

void Cgdn1Boot::start_flash_log() {
  this->main_task_ = xTaskGetCurrentTaskHandle();
  this->part_ = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_TEST, nullptr);
  if (this->part_ == nullptr) {
    this->refuse_ = "no test partition";
    return;
  }
  if (this->part_->size < REGION_OFF + REGION_SIZE) {
    this->refuse_ = "test partition smaller than the log region";
    return;
  }

  // Ownership interlock. The region is used only if it is entirely erased, or
  // holds nothing but our own log text (starting with our header) followed by
  // erased bytes. Anything else (e.g. the stock firmware's data store growing
  // into it) -> refuse: no write, no erase, this boot is not logged.
  uint8_t blk[512];
  uint32_t off = 0, headers = 0;
  size_t match = 0;
  bool in_text = true, foreign = false, read_err = false;
  this->write_off_ = REGION_OFF + REGION_SIZE;
  while (off < REGION_SIZE && !foreign) {
    uint32_t n = REGION_SIZE - off < sizeof(blk) ? REGION_SIZE - off : sizeof(blk);
    if (esp_partition_read(this->part_, REGION_OFF + off, blk, n) != ESP_OK) {
      read_err = true;
      break;
    }
    for (uint32_t i = 0; i < n; i++) {
      uint8_t c = blk[i];
      if (!in_text) {
        if (c != 0xFF) {
          foreign = true;
          break;
        }
        continue;
      }
      if (c == 0xFF) {
        this->write_off_ = REGION_OFF + off + i;
        in_text = false;
        continue;
      }
      if (!is_log_byte(c) || (off + i < sizeof(HEADER_PAT) - 1 && c != static_cast<uint8_t>(HEADER_PAT[off + i]))) {
        foreign = true;
        break;
      }
      if (c == static_cast<uint8_t>(HEADER_PAT[match])) {
        if (++match == sizeof(HEADER_PAT) - 1) {
          headers++;
          match = 0;
        }
      } else {
        match = c == '\n' ? 1 : 0;
      }
    }
    off += n;
  }
  if (read_err) {
    this->refuse_ = "read error while checking the log region";
    return;
  }
  if (foreign) {
    this->refuse_ = "log region holds foreign data (not erased, not our log): left untouched";
    return;
  }

  // The region is provably ours (or blank): only now may it be erased.
  bool erased = false;
  if (REGION_OFF + REGION_SIZE - this->write_off_ < MIN_FREE) {
    if (esp_partition_erase_range(this->part_, REGION_OFF, REGION_SIZE) != ESP_OK) {
      this->refuse_ = "erase failed";
      return;
    }
    erased = true;
    this->write_off_ = REGION_OFF;
    headers = 0;
  }

  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_reset_reason_t rr = esp_reset_reason();
  char hdr[224];
  int n = snprintf(hdr, sizeof(hdr),
                   "\n=== CGDN1LOG boot %" PRIu32 " reset_reason=%d(%s) running=%s mark=%s erased=%s at=0x%06" PRIX32
                   " ===\n",
                   headers, static_cast<int>(rr), reset_name(rr), running ? running->label : "?",
#ifdef CGDN1_TRIAL_MODE
                   esp_err_to_name(s_mark_err),
#else
                   "off",
#endif
                   erased ? "yes(<8KB free)" : "no",
                   this->part_->address + this->write_off_);
  this->ready_ = true;
  this->write_(hdr, n > 0 ? static_cast<size_t>(n) : 0);
  s_bucket_ms = now_ms();

  // ESPHome lines (main task, or other tasks' lines replayed by the logger).
  if (logger::global_logger != nullptr) {
    logger::global_logger->add_log_callback(
        this, [](void *self, uint8_t level, const char *tag, const char *message, size_t message_len) {
          static_cast<Cgdn1Boot *>(self)->on_log_(level, tag, message, message_len);
        });
  }
  // IDF lines (wifi/phy driver ...) are taken straight from the vprintf hook,
  // chained in front of ESPHome's: the logger's 768-byte task log buffer
  // overflows during wifi init and its overflow path bypasses log callbacks.
  s_self = this;
  s_prev_vprintf = esp_log_set_vprintf(idf_vprintf_hook);
}

// Runs on any task (IDF logs from the wifi task etc.): small stack frame, no
// flash access, no logging; only the RAM buffer under the spinlock.
int Cgdn1Boot::idf_vprintf_hook(const char *fmt, va_list args) {
  Cgdn1Boot *self = s_self;
  if (self != nullptr) {
    char piece[128];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(piece, sizeof(piece), fmt, copy);
    va_end(copy);
    if (n > 0)
      self->on_idf_(piece, static_cast<size_t>(n) < sizeof(piece) ? static_cast<size_t>(n) : sizeof(piece) - 1);
  }
  return s_prev_vprintf != nullptr ? s_prev_vprintf(fmt, args) : 0;
}

// The wifi blob emits one line as several vprintf calls ("I (12) wifi:",
// "text", "\n"): join the pieces of one task until a newline.
void Cgdn1Boot::on_idf_(const char *piece, size_t len) {
  void *me = xTaskGetCurrentTaskHandle();
  const bool on_main = me == this->main_task_;
  if (on_main && this->flushing_)
    return;
  const bool ends_line = len > 0 && piece[len - 1] == '\n';
  char line[IDF_PEND];
  size_t line_len = 0;
  bool standalone = false;
  portENTER_CRITICAL(&this->mux_);
  if (this->idf_task_ != nullptr && this->idf_task_ != me) {
    standalone = true;  // another task's line is half-assembled: don't mix
  } else {
    size_t room = IDF_PEND - this->idf_len_;
    size_t k = len < room ? len : room;
    memcpy(this->idf_buf_ + this->idf_len_, piece, k);
    this->idf_len_ += k;
    if (ends_line || this->idf_len_ == IDF_PEND) {
      line_len = this->idf_len_;
      memcpy(line, this->idf_buf_, line_len);
      this->idf_len_ = 0;
      this->idf_task_ = nullptr;
    } else {
      this->idf_task_ = me;
    }
  }
  portEXIT_CRITICAL(&this->mux_);
  const char *body = standalone ? piece : line;
  size_t body_len = standalone ? len : line_len;
  if (body_len == 0)
    return;
  // IDF level letter after any colour escape: "\033[0;31mE (123) ..."
  const char *p = body;
  if (body_len > 1 && p[0] == 0x1B) {
    const char *m = static_cast<const char *>(memchr(p, 'm', body_len));
    if (m != nullptr && m + 1 < body + body_len)
      p = m + 1;
  }
  uint8_t level = *p == 'E' ? ESPHOME_LOG_LEVEL_ERROR : *p == 'W' ? ESPHOME_LOG_LEVEL_WARN : ESPHOME_LOG_LEVEL_INFO;
  this->ingest_(level, body, body_len, false, on_main);
}

void Cgdn1Boot::on_log_(uint8_t level, const char *tag, const char *msg, size_t len) {
  if (tag != nullptr && strcmp(tag, "esp-idf") == 0)
    return;  // already captured (complete) by idf_vprintf_hook
  const bool on_main = xTaskGetCurrentTaskHandle() == this->main_task_;
  if (on_main && this->flushing_)
    return;  // something logged from inside our own flush: drop, never recurse
  const bool exempt = tag != nullptr && strcmp(tag, TAG) == 0;  // status/markers
  this->ingest_(level, msg, len, exempt, on_main);
}

void Cgdn1Boot::ingest_(uint8_t level, const char *msg, size_t len, bool exempt, bool on_main) {
  char line[MAX_LINE];
  uint32_t hash;
  size_t n = format_line(line, sizeof(line), msg, len, &hash);
  if (n == 0)
    return;

  uint32_t repeats = 0, limited = 0;
  bool keep = true;
  portENTER_CRITICAL(&this->mux_);
  if (!exempt && hash == s_last_hash) {
    s_repeats++;
    keep = false;
  } else {
    repeats = s_repeats;
    s_repeats = 0;
    if (!exempt)
      s_last_hash = hash;
    uint32_t now = now_ms();
    s_tokens += static_cast<int32_t>((now - s_bucket_ms) * BUCKET_RATE / 1000);
    s_bucket_ms = now;
    if (s_tokens > BUCKET_CAP)
      s_tokens = BUCKET_CAP;
    if (!exempt && s_tokens < static_cast<int32_t>(n)) {
      s_limited++;
      keep = false;
    } else {
      if (!exempt)
        s_tokens -= static_cast<int32_t>(n);
      limited = s_limited;
      s_limited = 0;
    }
  }
  portEXIT_CRITICAL(&this->mux_);

  char note[64];
  if (repeats > 0) {
    int k = snprintf(note, sizeof(note), "[...previous line repeated %" PRIu32 " more times]\n", repeats);
    this->append_raw_(note, k > 0 ? static_cast<size_t>(k) : 0);
  }
  if (limited > 0) {
    int k = snprintf(note, sizeof(note), "[...rate limit: %" PRIu32 " lines dropped]\n", limited);
    this->append_raw_(note, k > 0 ? static_cast<size_t>(k) : 0);
  }
  if (!keep)
    return;
  this->append_raw_(line, n);

  if (on_main && this->ready_ && (!this->loop_seen_ || level <= ESPHOME_LOG_LEVEL_WARN))
    this->flush_();
}

void Cgdn1Boot::append_raw_(const char *data, size_t len) {
  portENTER_CRITICAL(&this->mux_);
  uint8_t a = this->active_;
  if (this->len_[a] + len <= BUF_SIZE) {
    memcpy(this->buf_[a] + this->len_[a], data, len);
    this->len_[a] += len;
  } else {
    this->dropped_++;
  }
  portEXIT_CRITICAL(&this->mux_);
}

// Main task only. Never logs.
void Cgdn1Boot::write_(const char *data, size_t len) {
  if (!this->ready_ || this->full_ || len == 0)
    return;
  const uint32_t limit = REGION_OFF + REGION_SIZE - FULL_RESERVE;
  bool fill = false;
  if (this->write_off_ + len > limit) {
    len = this->write_off_ < limit ? limit - this->write_off_ : 0;
    fill = true;
  }
  if (len > 0) {
    esp_err_t err = esp_partition_write(this->part_, this->write_off_, data, len);
    if (err != ESP_OK)
      this->last_write_err_ = err;
    this->write_off_ += len;
  }
  if (fill) {
    // Keep the earliest lines of this boot; one marker, then stop for the boot.
    esp_partition_write(this->part_, this->write_off_, FULL_MARKER, FULL_RESERVE);
    this->write_off_ += FULL_RESERVE;
    this->full_ = true;
  }
}

void Cgdn1Boot::flush_() {
  if (!this->ready_ || this->flushing_ || this->hold_)
    return;
  this->flushing_ = true;
  portENTER_CRITICAL(&this->mux_);
  uint8_t idx = this->active_;
  this->active_ ^= 1;  // the other half is empty: only this function drains it
  uint32_t dropped = this->dropped_;
  this->dropped_ = 0;
  portEXIT_CRITICAL(&this->mux_);
  this->write_(this->buf_[idx], this->len_[idx]);
  this->len_[idx] = 0;
  if (dropped) {
    char note[64];
    int k = snprintf(note, sizeof(note), "[...ram buffer overflow: %" PRIu32 " lines lost]\n", dropped);
    this->write_(note, k > 0 ? static_cast<size_t>(k) : 0);
  }
  this->last_flush_ms_ = millis();
  this->flushing_ = false;
}

void Cgdn1Boot::log_status_() {
  uint32_t up = millis() / 1000;
#ifdef USE_WIFI
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_err_t me = esp_wifi_get_mode(&mode);
  wifi_ap_record_t ap = {};
  esp_err_t ae = esp_wifi_sta_get_ap_info(&ap);
  uint8_t ch = 0;
  wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
  esp_err_t ce = esp_wifi_get_channel(&ch, &sec);
  wifi::WiFiComponent *w = wifi::global_wifi_component;
  TaskHandle_t wt = xTaskGetHandle("wifi");
  unsigned wifi_stack = wt != nullptr ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(wt)) : 0;
  unsigned main_stack = static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr));
  ESP_LOGI(TAG,
           "status up=%" PRIu32 "s heap=%" PRIu32 " min=%" PRIu32 " setup_done=%d wifi_mode=%d(%s) sta_ap_info=%s"
           " rssi=%d ch=%u(%s) comp=%s cstate=0x%02X conn=%d has_sta=%d has_ap=%d stack_free main=%u wifi=%u"
           " flash_at=0x%06" PRIX32 "%s",
           up, static_cast<uint32_t>(esp_get_free_heap_size()), static_cast<uint32_t>(esp_get_minimum_free_heap_size()),
           App.is_setup_complete() ? 1 : 0, static_cast<int>(mode), esp_err_to_name(me), esp_err_to_name(ae),
           ae == ESP_OK ? ap.rssi : 0, ch, esp_err_to_name(ce), w ? "yes" : "null", w ? w->get_component_state() : 0,
           w ? w->is_connected() : 0, w ? w->has_sta() : 0, w ? w->has_ap() : 0, main_stack, wifi_stack,
           this->part_->address + this->write_off_,
           this->last_write_err_ != ESP_OK ? " WRITE_ERR" : "");
#else
  ESP_LOGI(TAG, "status up=%" PRIu32 "s heap=%" PRIu32 " (no wifi)", up, static_cast<uint32_t>(esp_get_free_heap_size()));
#endif
}

void Cgdn1Boot::flash_loop_() {
  this->loop_seen_ = true;
  if (!this->ready_)
    return;
  uint32_t now = millis();
  if (this->last_status_ms_ == 0 || now - this->last_status_ms_ >= STATUS_MS) {
    this->last_status_ms_ = now ? now : 1;
    this->log_status_();
  }
  size_t pending = this->len_[this->active_];  // racy read is fine: only a trigger
  if (pending >= FLUSH_BYTES || (pending > 0 && now - this->last_flush_ms_ >= FLUSH_MS) || this->dropped_ > 0)
    this->flush_();
}

void Cgdn1Boot::on_shutdown() {
  ESP_LOGI(TAG, "shutdown: flushing flash log");
  this->flush_();
}

void Cgdn1Boot::on_powerdown() { this->flush_(); }
#endif  // CGDN1_FLASH_LOG

}  // namespace cgdn1_boot
}  // namespace esphome
