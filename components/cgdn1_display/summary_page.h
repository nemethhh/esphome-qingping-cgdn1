#pragma once
// Fixed CO2 + temperature idle page, on the physical 160 x 128 canvas. Pure
// bitmap rendering makes layout testable on the host without an OLED or fonts.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace esphome::cgdn1_display {
// Five columns, seven rows, MSB at the top. No smoothing or fractional pixels.
inline const uint8_t *summary_glyph(char ch) {
  static constexpr uint8_t digits[10][5] = {
      {0x3e,0x45,0x49,0x51,0x3e},
      {0x00,0x21,0x7f,0x01,0x00},
      {0x21,0x43,0x45,0x49,0x31},
      {0x41,0x49,0x49,0x49,0x36},
      {0x0c,0x14,0x24,0x7f,0x04},
      {0x79,0x49,0x49,0x49,0x46},
      {0x3e,0x49,0x49,0x49,0x06},
      {0x40,0x47,0x48,0x50,0x60},
      {0x36,0x49,0x49,0x49,0x36},
      {0x30,0x49,0x49,0x49,0x3e}};
  static constexpr uint8_t c[] = {0x3e,0x41,0x41,0x41,0x41};
  static constexpr uint8_t o[] = {0x3e,0x41,0x41,0x41,0x3e};
  static constexpr uint8_t p[] = {0x1f,0x14,0x14,0x14,0x08};
  static constexpr uint8_t m[] = {0x1f,0x10,0x0f,0x10,0x1f};
  static constexpr uint8_t dot[] = {0,0,0x03,0,0};
  static constexpr uint8_t dash[] = {0x08,0x08,0x08,0x08,0x08};
  static constexpr uint8_t degree[] = {0x20,0x50,0x50,0x20,0};
  static constexpr uint8_t blank[] = {0,0,0,0,0};
  if (ch >= '0' && ch <= '9') return digits[ch - '0'];
  switch (ch) {
    case 'C': return c; case 'O': return o; case 'p': return p; case 'm': return m;
    case '.': return dot; case '-': return dash; case '*': return degree;
    default: return blank;
  }
}
template<typename Plot> void summary_text(const char *text, int cx, int y, int scale, Plot plot) {
  const int width = (int(strlen(text)) * 6 - 1) * scale;
  const int left = cx - width / 2;
  for (int i = 0; text[i]; i++) {
    const auto *g = summary_glyph(text[i]);
    for (int x = 0; x < 5; x++) for (int row = 0; row < 7; row++) {
      if (!(g[x] & (1 << (6 - row)))) continue;
      for (int dx = 0; dx < scale; dx++) for (int dy = 0; dy < scale; dy++)
        plot(left + (i * 6 + x) * scale + dx, y + row * scale + dy);
    }
  }
}
inline bool summary_due(uint32_t now, uint32_t last_activity, uint32_t delay_ms) {
  return uint32_t(now - last_activity) >= delay_ms;  // wrap-safe millis interval
}
template<typename Plot> void draw_summary(float co2, float temperature, Plot plot) {
  char c[12], t[16];
  if (std::isfinite(co2)) snprintf(c, sizeof(c), "%d", int(std::lround(std::clamp(co2, 0.0f, 9999.0f))));
  else snprintf(c, sizeof(c), "--");
  if (std::isfinite(temperature)) snprintf(t, sizeof(t), "%.1f *C", double(std::clamp(temperature, -40.0f, 125.0f)));
  else snprintf(t, sizeof(t), "--.- *C");
  summary_text("CO2", 80, 8, 1, plot);
  summary_text(c, 80, 24, strlen(c) < 4 ? 5 : 4, plot);
  summary_text("ppm", 80, 65, 1, plot);
  summary_text(t, 80, 91, strlen(t) > 8 ? 2 : 3, plot);
}
}  // namespace esphome::cgdn1_display
