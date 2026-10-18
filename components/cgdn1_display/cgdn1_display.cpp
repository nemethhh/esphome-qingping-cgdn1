#include "cgdn1_display.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "esphome/core/log.h"

#ifdef USE_ESP32
#include <esp_flash.h>
#endif

// Fixed .bss arrays (not heap) so a host tool can find them by symbol, e.g. QEMU HMP
// `pmemsave <&cgdn1_display_fb> 2560 frame.bin`.
//  - cgdn1_display_fb: the 1-bpp GUI frame (both variants).
//  - cgdn1_display_stream: the last variant-B data stream sent (127 lines x 80 B, 4 bpp).
extern "C" {
alignas(4) __attribute__((used)) uint8_t cgdn1_display_fb[esphome::cgdn1_display::FB_SIZE];
alignas(4) __attribute__((used)) uint8_t cgdn1_display_stream[esphome::cgdn1_display::B_STREAM_SIZE];
}

namespace esphome::cgdn1_display {

static const char *const TAG = "cgdn1_display";

// Stock init sequences (FUN_400f1730). Display-on (AF) is sent after the first frame.
static const uint8_t INIT_A[] = {0xAE, 0x20, 0x81, 0xD0, 0xA0, 0xA6, 0xA9, 0x02, 0xAD, 0x80,
                                 0xC0, 0xD5, 0xF0, 0xD9, 0x13, 0xDB, 0x2B, 0xDC, 0x35, 0x30};
static constexpr size_t INIT_A_CONTRAST_IDX = 3;
static const uint8_t INIT_B[] = {0xAE, 0x20, 0x00, 0x81, 0xD0, 0xAD, 0x00, 0xA0, 0xC8, 0xA8,
                                 0x7F, 0xD3, 0x10, 0xD5, 0x07, 0xD9, 0x40, 0x40, 0x40, 0x05,
                                 0x98, 0x00, 0xDB, 0x2B, 0xDA, 0xC0, 0xA4, 0xA6};
static constexpr size_t INIT_B_CONTRAST_IDX = 4;

static const char *const PATTERN_NAMES[] = {"off", "orientation", "checkerboard", "white"};

void Cgdn1Display::resolve_variant_() {
  if (this->variant_cfg_ != VARIANT_AUTO) {
    this->variant_ = this->variant_cfg_;
    return;
  }
  // Stock oled_drive_init: spi_flash_read(0x3d205d, &lcd_id, 1); lcd_id == 1 -> variant B.
#ifdef USE_ESP32
  uint8_t id = 0xFF;
  const esp_err_t err = esp_flash_read(esp_flash_default_chip, &id, LCD_ID_FLASH_ADDR, 1);
  if (err == ESP_OK) {
    this->lcd_id_ = id;
    this->variant_ = id == 1 ? VARIANT_B : VARIANT_A;
    return;
  }
  ESP_LOGW(TAG, "lcd_id flash read failed (%d); assuming variant B", (int) err);
#endif
  this->variant_ = VARIANT_B;  // the only variant tested on hardware
}

void Cgdn1Display::setup() {
  this->buffer_ = cgdn1_display_fb;
  memset(this->buffer_, 0, FB_SIZE);
  this->resolve_variant_();
  const bool b = this->variant_ == VARIANT_B;
  this->apply_brightness_();  // restored "Display brightness" -> the init contrast byte

  this->set_data_rate(b ? 8000000 : 2000000);  // stock: 8 MHz for lcd_id 1, else 2 MHz
  this->spi_setup();
  this->dc_pin_->setup();
  this->dc_pin_->digital_write(true);
  this->reset_controller_();
  // Stock FUN_400f1730 starts with gpio_set_level(15, 0) and never raises CS again: assert it once
  // here and keep it low for good (command_()/data_() never toggle it), see cgdn1_display.h.
  this->enable();

  const uint8_t *seq = b ? INIT_B : INIT_A;
  const size_t n = b ? sizeof(INIT_B) : sizeof(INIT_A);
  const size_t ci = b ? INIT_B_CONTRAST_IDX : INIT_A_CONTRAST_IDX;
  for (size_t i = 0; i < n; i++)
    this->command_(i == ci ? this->level_ : seq[i]);
  if (b)
    this->clear_gram_b_();  // stock clears the whole GRAM right after the B init
  this->ready_ = true;

  // First frame right away (overwrites everything the stock firmware left), then light up.
  this->render_();
  this->write_frame_(true);
  this->command_(this->on_ ? 0xAF : 0xAE);
  ESP_LOGI(TAG, "panel %s", this->variant_summary().c_str());
}

void Cgdn1Display::reset_controller_() {
  if (this->reset_pin_ == nullptr)
    return;
  // Stock: RST/DC/CS high, 2 ticks, RST low 5 ticks, RST high, 1 tick (1 tick <= 10 ms).
  // Generous here: the controller must really restart (registers to defaults).
  this->reset_pin_->setup();
  this->reset_pin_->digital_write(true);
  delay(20);
  this->reset_pin_->digital_write(false);
  delay(50);
  this->reset_pin_->digital_write(true);
  delay(20);
}

char Cgdn1Display::variant_letter() const {
  return this->variant_ == VARIANT_A ? 'A' : this->variant_ == VARIANT_B ? 'B' : '?';
}

std::string Cgdn1Display::variant_summary() const {
  char buf[64];
  const bool b = this->variant_ == VARIANT_B;
  if (this->lcd_id_ >= 0) {
    snprintf(buf, sizeof(buf), "%c (%s), lcd_id=%d auto", this->variant_letter(),
             b ? "4-bpp, 8 MHz" : "1-bpp, 2 MHz", this->lcd_id_);
  } else {
    snprintf(buf, sizeof(buf), "%c (%s), %s", this->variant_letter(), b ? "4-bpp, 8 MHz" : "1-bpp, 2 MHz",
             this->variant_cfg_ == VARIANT_AUTO ? "auto, lcd_id unread" : "forced");
  }
  return buf;
}

void Cgdn1Display::dump_config() {
  LOG_DISPLAY("", "CGDN1 OLED (128x160 GUI)", this);
  ESP_LOGCONFIG(TAG,
                "  Panel: %s\n"
                "  Contrast: 0x%02X in use, 0x%02X at 100 %%\n"
                "  Flip X/Y: %s/%s\n"
                "  Test pattern: %s\n"
                "  Framebuffer: %p (%u B), B stream: %p (%u B)\n"
                "  State: %s",
                this->variant_summary().c_str(), this->level_, this->contrast_, YESNO(this->flip_x_), YESNO(this->flip_y_),
                PATTERN_NAMES[this->pattern_], (void *) cgdn1_display_fb, (unsigned) FB_SIZE,
                (void *) cgdn1_display_stream, (unsigned) B_STREAM_SIZE, this->on_ ? "on" : "off");
  LOG_PIN("  CS Pin: ", this->cs_);
  LOG_PIN("  DC Pin: ", this->dc_pin_);
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  LOG_UPDATE_INTERVAL(this);
}

void Cgdn1Display::update() {
  if (!this->ready_ || !this->on_)
    return;  // dark panel: nothing to draw; turn_on() redraws immediately
  const uint32_t t0 = micros();
  this->render_();
  this->write_frame_(false);
  const uint32_t us = micros() - t0;
  this->stat_updates_++;
  this->stat_us_sum_ += us;
  if (us > this->stat_us_max_)
    this->stat_us_max_ = us;
  this->log_stats_();
}

void Cgdn1Display::log_stats_() {
  // Once a minute: how much of the main loop the display costs (render + SPI, blocking).
  const uint32_t now = millis();
  if (this->stat_t0_ == 0)
    this->stat_t0_ = now;
  if (now - this->stat_t0_ < 60000 || this->stat_updates_ == 0)
    return;
  ESP_LOGD(TAG, "last %" PRIu32 " s: %" PRIu32 " redraws, %" PRIu32 " frames sent; update avg %" PRIu32
                " us, max %" PRIu32 " us",
           (now - this->stat_t0_) / 1000, this->stat_updates_, this->stat_frames_,
           this->stat_us_sum_ / this->stat_updates_, this->stat_us_max_);
  this->stat_t0_ = now;
  this->stat_updates_ = this->stat_frames_ = this->stat_us_sum_ = this->stat_us_max_ = 0;
}

void Cgdn1Display::render_() {
  if (this->pattern_ != TEST_PATTERN_OFF) {
    this->draw_test_pattern_();
    return;
  }
  this->do_update_();
}

void Cgdn1Display::turn_on() {
  this->on_ = true;
  if (!this->ready_)
    return;
  this->render_();
  this->write_frame_(true);
  this->command_(0xAF);
  ESP_LOGD(TAG, "display on");
}

void Cgdn1Display::turn_off() {
  this->on_ = false;
  if (!this->ready_)
    return;
  this->command_(0xAE);
  ESP_LOGD(TAG, "display off");
}

void Cgdn1Display::set_contrast(uint8_t c) {
  this->level_ = c;
  if (!this->ready_)
    return;
  this->command_(0x81);  // CS stays low across both bytes, like stock's init
  this->command_(c);
}

void Cgdn1Display::set_brightness(float pct) {
  this->brightness_ = std::isnan(pct) ? 100.0f : std::max(0.0f, std::min(100.0f, pct));
  if (!this->ready_) {
    ESP_LOGI(TAG, "brightness %.0f %% (applied at the display init)", this->brightness_);
    return;  // setup() applies it before the init sequence
  }
  this->apply_brightness_();
}

void Cgdn1Display::apply_brightness_() {
  if (std::isnan(this->brightness_))
    return;  // never set: `contrast:`
  const uint8_t level = contrast_level(this->brightness_, this->contrast_);
  ESP_LOGI(TAG, "brightness %.0f %% -> contrast 0x%02X (top 0x%02X)%s", this->brightness_, level, this->contrast_,
           this->ready_ ? "" : " (init value)");
  this->set_contrast(level);
}

void Cgdn1Display::set_test_pattern(TestPattern p) {
  if (p > TEST_PATTERN_WHITE)
    p = TEST_PATTERN_OFF;
  this->pattern_ = p;
  ESP_LOGI(TAG, "test pattern: %s", PATTERN_NAMES[p]);
  if (this->ready_ && this->on_) {
    this->render_();
    this->write_frame_(true);
  }
}

bool Cgdn1Display::set_test_pattern(const char *name) {
  for (uint8_t i = 0; i <= TEST_PATTERN_WHITE; i++) {
    if (strcmp(name, PATTERN_NAMES[i]) == 0) {
      this->set_test_pattern(static_cast<TestPattern>(i));
      return true;
    }
  }
  ESP_LOGW(TAG, "unknown test pattern '%s'", name);
  return false;
}

// 5x7 bitmap glyphs for the orientation pattern (row-major, bit 4 = leftmost column).
struct Glyph {
  char c;
  uint8_t rows[7];
};
static const Glyph GLYPHS[] = {
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}}, {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}}, {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}}, {'?', {0x0E, 0x11, 0x01, 0x06, 0x04, 0x00, 0x04}},
};

void Cgdn1Display::draw_glyph_(int x, int y, char c, int scale) {
  for (const auto &g : GLYPHS) {
    if (g.c != c)
      continue;
    for (int r = 0; r < 7; r++)
      for (int col = 0; col < 5; col++)
        if (g.rows[r] & (0x10 >> col))
          this->filled_rectangle(x + col * scale, y + r * scale, scale, scale);
    return;
  }
}

void Cgdn1Display::draw_test_pattern_() {
  // Drawn on the rotated (logical) canvas, so it shows what the firmware thinks is "up".
  const int w = this->get_width(), h = this->get_height();
  this->clear();
  if (this->pattern_ == TEST_PATTERN_WHITE) {
    this->fill(display::COLOR_ON);
    return;
  }
  if (this->pattern_ == TEST_PATTERN_CHECKERBOARD) {
    for (int y = 0; y < h; y += 8)
      for (int x = 0; x < w; x += 8)
        if (((x + y) / 8) % 2 == 0)
          this->filled_rectangle(x, y, 8, 8);
    return;
  }
  // Orientation: outer border on the extreme pixels + inner border 2 px in (coverage), four
  // different corner markers (mirror/rotation), "TOP" + up arrow, L / R, variant letter.
  this->rectangle(0, 0, w, h);
  this->rectangle(2, 2, w - 4, h - 4);
  this->filled_rectangle(5, 5, 12, 12);  // top-left: solid square
  this->rectangle(w - 17, 5, 12, 12);    // top-right: hollow square (2 px wall)
  this->rectangle(w - 16, 6, 10, 10);
  this->filled_triangle(5, h - 6, 5, h - 17, 16, h - 6);  // bottom-left: solid triangle
  for (int d = 0; d < 2; d++) {                            // bottom-right: X
    this->line(w - 17 + d, h - 17, w - 6 + d, h - 6);
    this->line(w - 17 + d, h - 6, w - 6 + d, h - 17);
  }
  const int cx = w / 2;
  this->draw_glyph_(cx - 25, 8, 'T', 3);
  this->draw_glyph_(cx - 7, 8, 'O', 3);
  this->draw_glyph_(cx + 11, 8, 'P', 3);
  this->filled_triangle(cx, 34, cx - 20, 56, cx + 20, 56);  // arrow head, points up
  this->filled_rectangle(cx - 5, 56, 10, h - 30 - 56);      // shaft
  this->draw_glyph_(9, h / 2 - 7, 'L', 2);
  this->draw_glyph_(w - 19, h / 2 - 7, 'R', 2);
  this->draw_glyph_(cx - 5, h - 24, this->variant_letter(), 2);
}

void Cgdn1Display::command_(uint8_t b) {
  // Stock FUN_400f13b8: DC low, one byte per transaction; CS stays low (asserted once in setup()).
  this->dc_pin_->digital_write(false);
  this->write_byte(b);
}

void Cgdn1Display::data_(const uint8_t *buf, size_t len) {
  this->dc_pin_->digital_write(true);
  this->write_array(buf, len);
}

void Cgdn1Display::clear_gram_b_() {
  // Stock FUN_400f149c: column 0, then 128 x 80 zero bytes = the whole 4-bpp GRAM.
  static const uint8_t ZERO[B_LINE_BYTES] = {};
  this->command_(0x00);
  this->command_(0x10);
  for (int line = 0; line < PANEL_W; line++)
    this->data_(ZERO, sizeof(ZERO));
}

void Cgdn1Display::write_frame_(bool force) {
  // Skip identical frames: values change every few seconds at most, the poll is 1 s.
  uint32_t h = 2166136261u;  // FNV-1a
  for (size_t i = 0; i < FB_SIZE; i++)
    h = (h ^ this->buffer_[i]) * 16777619u;
  if (!force && this->sent_valid_ && h == this->sent_hash_)
    return;
  if (this->variant_ == VARIANT_B) {
    this->write_frame_b_();
  } else {
    this->write_frame_a_();
  }
  this->sent_hash_ = h;
  this->sent_valid_ = true;
  this->stat_frames_++;
}

void Cgdn1Display::write_frame_a_() {
  // Stock FUN_400f159c: per page {B0, page, 00, 11} as commands, then 128 data bytes.
  for (uint8_t page = 0; page < PANEL_H / 8; page++) {
    const uint8_t cmd[4] = {0xB0, page, 0x00, 0x11};
    this->dc_pin_->digital_write(false);
    this->write_array(cmd, sizeof(cmd));
    this->data_(this->buffer_ + page * PANEL_W, PANEL_W);
  }
}

void Cgdn1Display::write_frame_b_() {
  // Stock FUN_400f1410: cmd 00, 10; then for GUI column x = 127 .. 1 one 80-byte line holding
  // that column's 160 rows at 4 bpp: byte i = rows 2i (low nibble) and 2i+1 (high nibble).
  // 127 x 80 B at 8 MHz ~ 10 ms.
  // Lit pixel = 0xF like stock ("Display brightness" dims through the contrast byte instead).
  const uint8_t lo = 0x0F, hi = 0xF0;
  this->command_(0x00);
  this->command_(0x10);
  for (int j = 0; j < B_LINES; j++) {
    const int x = PANEL_W - 1 - j;
    uint8_t *line = cgdn1_display_stream + j * B_LINE_BYTES;
    for (int i = 0; i < B_LINE_BYTES; i++) {
      const int y0 = 2 * i, y1 = 2 * i + 1;
      const uint8_t p0 = (this->buffer_[x + (y0 / 8) * PANEL_W] >> (y0 & 7)) & 1;
      const uint8_t p1 = (this->buffer_[x + (y1 / 8) * PANEL_W] >> (y1 & 7)) & 1;
      line[i] = (p0 ? lo : 0x00) | (p1 ? hi : 0x00);
    }
    this->data_(line, B_LINE_BYTES);
  }
}

void Cgdn1Display::fill(Color color) {
  if (this->get_clipping().is_set()) {
    Display::fill(color);
    return;
  }
  memset(this->buffer_, color.is_on() ? 0xFF : 0x00, FB_SIZE);
}

void HOT Cgdn1Display::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (this->flip_x_)
    x = PANEL_W - 1 - x;
  if (this->flip_y_)
    y = PANEL_H - 1 - y;
  if (x < 0 || x >= PANEL_W || y < 0 || y >= PANEL_H)
    return;
  uint8_t *p = this->buffer_ + x + (y / 8) * PANEL_W;
  const uint8_t bit = 1u << (y & 7);
  if (color.is_on()) {
    *p |= bit;
  } else {
    *p &= ~bit;
  }
}

}  // namespace esphome::cgdn1_display
