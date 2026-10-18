#pragma once
// CGDN1 on-board OLED, SPI. Two factory panel variants, exactly as the stock 4.8.5 app drives them
// (CGDN1_ESP_4.8.5.elf):
//
// Probe (oled_drive_init FUN_400f12a4): `spi_flash_read(0x3d205d, &lcd_id, 1)` -- a factory byte
// in the 0x3d2000 factory-info block (byte 0x3d2000 == 'Z' marks it valid). lcd_id == 1 selects
// variant B, anything else variant A. No GPIO strap, no MISO read. The author's unit holds 1 -> B.
//
// Common: HSPI mode 0, SCLK=5 MOSI=4, CS=15 / DC=2 / RST=22 as plain GPIOs (stock: spics_io_num=-1),
// DC low = command (one byte per transaction, FUN_400f13b8), DC high = data. Reset: RST high, low
// 5 ticks, high, 1 tick. CS: stock drives GPIO15 low once at the start of the init sequence
// (FUN_400f1730: gpio_set_level(15, 0)) and never raises it again -- every later command and pixel
// byte goes out with CS held low. This driver does the same (one enable() in setup(), never a
// disable()). Raising CS after every byte (the earlier driver) evidently cost every parameter
// byte (run as a command of its own; inferred from the unit, not bench-proven): 0x81 changed
// nothing, `DA C0` ran C0 = COM scan opposite to stock (the vertical mirror the yaml then patched
// with flip_x), and contrast 0x80 / 1/160 duty (reset defaults instead of 0xD0 / A8 7F) made the
// panel visibly dimmer than stock with an identical byte stream. Datasheet (CH1120): "The SPI is
// also able to work properly when the CS always keep low".
// Display on/off = AF/AE (FUN_400f198c/FUN_400f1998), contrast 0x81 0xD0 at init (stock never
// changes it at runtime). 0x81 <level> sits in BOTH init sequences at the same place, so
// set_brightness() resends just those two command bytes on either variant.
//
// Variant A (lcd_id != 1): 1 bpp, 2 MHz, SH1108-style.
//   init AE 20 81 D0 A0 A6 A9 02 AD 80 C0 D5 F0 D9 13 DB 2B DC 35 30, [frame], AF
//   frame (FUN_400f159c): per page p = 0..19: cmd {B0, p, 00, 11}, then 128 data bytes = GUI
//   page p (byte = 8 vertical px, LSB = top).
// Variant B (lcd_id == 1): 4 bpp grayscale, 8 MHz. Controller: Chipwealth CH1120 family (160 SEG x
//   160 COM, 16 gray; 20h addressing mode, 81h contrast, ADh IREF select, A8h/D3h/D5h/D9h/DBh/DAh,
//   00-0F/10-14 column address, 160 x 128 = u8g2's "ch1120 128x160" with D3 10 / A8 7F).
//   init AE 20 00 81 D0 AD 00 A0 C8 A8 7F D3 10 D5 07 D9 40 40 40 05 98 00 DB 2B DA C0 A4 A6
//   (every byte proven: each call's a10 is a movi immediate, or -- the three 00s at 0x400f175e /
//   0x400f1787 / 0x400f1825 -- the previous call's return value, which the `bnez a10` right before
//   the call has just proven to be 0), then a GRAM clear (FUN_400f149c: cmd 00 10, 128 x 80 zero
//   data bytes), [frame], AF.
//   frame (FUN_400f1410): cmd 00, 10, then 127 lines of 80 data bytes; line j carries GUI column
//   X = 127 - j (GUI X = 0 is never sent), byte i = GUI rows 2i (low nibble) / 2i+1 (high nibble),
//   lit pixel = 0xF.
//
// Both variants show the same GUI: 128 (X) x 160 (Y), kept here as an SSD1306-style 1-bpp buffer
// (byte = x + (y / 8) * 128, LSB = top) -- stock's own framebuffer layout. The glass is mounted
// rotated: `rotation: 90` gives an upright 160 x 128 landscape canvas (derived from the stock
// glyph bitmaps).
#include <string>

#include "display_math.h"
#include "stock_page.h"

#include "esphome/components/display/display_buffer.h"
#include "esphome/components/spi/spi.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"

namespace esphome::cgdn1_display {

static constexpr int PANEL_W = 128;                       // GUI X (controller columns)
static constexpr int PANEL_H = 160;                       // GUI Y (20 pages)
static constexpr size_t FB_SIZE = PANEL_W * PANEL_H / 8;  // 2560 bytes
static constexpr int B_LINE_BYTES = PANEL_H / 2;          // 80: 160 px at 4 bpp
static constexpr int B_LINES = PANEL_W - 1;               // 127: stock never sends GUI X = 0
static constexpr size_t B_STREAM_SIZE = B_LINES * B_LINE_BYTES;  // 10160 bytes
static constexpr uint32_t LCD_ID_FLASH_ADDR = 0x3d205d;

enum Variant : uint8_t { VARIANT_AUTO = 0, VARIANT_A = 1, VARIANT_B = 2 };
enum TestPattern : uint8_t {
  TEST_PATTERN_OFF = 0,
  TEST_PATTERN_ORIENTATION,  // double border, 4 distinct corner markers, up arrow, "TOP", L/R, variant
  TEST_PATTERN_CHECKERBOARD,  // 8 x 8 px squares, top-left square lit
  TEST_PATTERN_WHITE,         // every pixel lit
};

class Cgdn1Display : public display::DisplayBuffer,
                     public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                           spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_2MHZ> {
 public:
  void set_dc_pin(GPIOPin *pin) { this->dc_pin_ = pin; }
  void set_reset_pin(GPIOPin *pin) { this->reset_pin_ = pin; }
  void set_contrast_init(uint8_t c) {
    this->contrast_ = c;
    this->level_ = c;
  }
  void set_flip(bool fx, bool fy) {
    this->flip_x_ = fx;
    this->flip_y_ = fy;
  }
  void set_variant(Variant v) { this->variant_cfg_ = v; }

  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }

  void fill(Color color) override;
  display::DisplayType get_display_type() override { return display::DisplayType::DISPLAY_TYPE_BINARY; }

  /// Panel lit (stock: AF) / dark (stock: AE). Safe to call before setup().
  void turn_on();
  void turn_off();
  bool is_on() const { return this->on_; }
  /// Send 0x81 <c> now (and use c from the next init on); `contrast:` (the 100 % level) unchanged.
  void set_contrast(uint8_t c);
  uint8_t get_contrast() const { return this->level_; }
  /// Brightness in % (0..100), applied at once (no re-init); before setup() it is kept and used
  /// from the first frame on. Both variants: controller contrast 0x81 <contrast_level(%, top)>,
  /// top = `contrast:` (stock 0xD0) at 100 %, 0.75 stop (x0.59 current) less per 20 % down to
  /// ~7 % of it at 0 % (still lit; dark is turn_off()). Lit pixels stay 0xF like stock.
  void set_brightness(float pct);
  /// Contrast byte currently in use (the init value until set_brightness()/set_contrast()).
  uint8_t level() const { return this->level_; }

  /// Diagnostic frame drawn instead of the lambda. Redraws at once.
  void set_test_pattern(TestPattern p);
  /// Same, by name ("off", "orientation", "checkerboard", "white"); false if unknown.
  bool set_test_pattern(const char *name);
  TestPattern get_test_pattern() const { return this->pattern_; }

  /// Variant in use ('A'/'B'; '?' before setup), the probed factory lcd_id (-1 = not read).
  char variant_letter() const;
  int lcd_id() const { return this->lcd_id_; }
  /// e.g. "B (4-bpp, 8 MHz), lcd_id=1 auto" -- for a text sensor / logs.
  std::string variant_summary() const;

 protected:
  int get_width_internal() override { return PANEL_W; }
  int get_height_internal() override { return PANEL_H; }
  void draw_absolute_pixel_internal(int x, int y, Color color) override;

  void resolve_variant_();
  void reset_controller_();
  void command_(uint8_t b);
  void data_(const uint8_t *buf, size_t len);
  void clear_gram_b_();
  void render_();
  void draw_test_pattern_();
  void draw_glyph_(int x, int y, char c, int scale);
  void write_frame_(bool force);
  void write_frame_a_();
  void write_frame_b_();
  void apply_brightness_();
  void log_stats_();

  GPIOPin *dc_pin_{nullptr};
  GPIOPin *reset_pin_{nullptr};
  uint8_t contrast_{0xD0};  // `contrast:` = the 100 % brightness level (stock 0xD0)
  uint8_t level_{0xD0};     // contrast byte in use (init + runtime 0x81)
  float brightness_{NAN};   // set_brightness() value, % (NaN = never set: contrast_)
  bool flip_x_{false};
  bool flip_y_{false};
  Variant variant_cfg_{VARIANT_AUTO};
  Variant variant_{VARIANT_AUTO};  // resolved in setup(): A or B
  int lcd_id_{-1};
  TestPattern pattern_{TEST_PATTERN_OFF};
  bool on_{true};      // requested state (on at boot)
  bool ready_{false};  // controller initialised
  // Loop-cost bookkeeping (logged once a minute at DEBUG): redraws, frames sent, update() time.
  uint32_t stat_t0_{0};
  uint32_t stat_updates_{0};
  uint32_t stat_frames_{0};
  uint32_t stat_us_sum_{0};
  uint32_t stat_us_max_{0};
  uint32_t sent_hash_{0};
  bool sent_valid_{false};
};

}  // namespace esphome::cgdn1_display
