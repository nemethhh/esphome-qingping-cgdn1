#pragma once
// Source selection is independent per measurement. The HA feed contains a
// heartbeat timestamp even when both numbers are unchanged; value-change time
// alone is not a useful connection-health check for a stable room temperature.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace esphome::cgdn1_display {
enum class DisplaySource { NONE, LOCAL, HA };
struct DisplayValues {
  float co2{NAN}, temperature{NAN};
  DisplaySource co2_source{DisplaySource::NONE}, temperature_source{DisplaySource::NONE};
};
inline const char *source_name(DisplaySource source) {
  return source == DisplaySource::HA ? "HA" : source == DisplaySource::LOCAL ? "local" : "unavailable";
}
class DisplaySources {
 public:
  static constexpr uint32_t MAX_AGE_MS = 180000;
  void local_co2_seen(uint32_t now) { co2_seen_ = true; co2_ms_ = now; }
  void local_temperature_seen(uint32_t now) { temp_seen_ = true; temp_ms_ = now; }
  void clear() { remote_seen_ = false; remote_epoch_ = 0; }
  // Wire format: unix_seconds|CO2_or_nan|temperature_or_nan. A malformed packet
  // invalidates the entire feed; a valid 'nan' invalidates only its measurement.
  bool ingest(const char *packet, uint32_t now, int64_t wall_now) {
    unsigned long long epoch;
    char co2[32], temp[32]; int used = 0;
    if (std::sscanf(packet, "%llu|%31[^|]|%31s%n", &epoch, co2, temp, &used) != 3 || packet[used] != '\0' || std::strchr(temp, '|') ||
        epoch < 1577836800ULL || epoch > 4102444800ULL || wall_now < 1577836800 ||
        int64_t(epoch) > wall_now + 60 || wall_now - int64_t(epoch) > 180) {
      clear(); return false;
    }
    // Duplicate/replayed snapshots must not keep an otherwise dead feed fresh.
    if (remote_seen_ && epoch <= remote_epoch_) return false;
    remote_co2_ = parse_(co2, 0, 10000);
    remote_temp_ = parse_(temp, -40, 125);
    remote_epoch_ = epoch; remote_ms_ = now; remote_seen_ = true;
    return true;
  }
  DisplayValues choose(uint32_t now, int64_t wall_now, float local_co2, float local_temp) const {
    DisplayValues out;
    const bool remote = remote_seen_ && uint32_t(now - remote_ms_) <= MAX_AGE_MS &&
        wall_now >= 1577836800 && int64_t(remote_epoch_) <= wall_now + 60 &&
        wall_now - int64_t(remote_epoch_) <= 180;
    if (remote && std::isfinite(remote_co2_)) { out.co2 = remote_co2_; out.co2_source = DisplaySource::HA; }
    else if (co2_seen_ && uint32_t(now - co2_ms_) <= MAX_AGE_MS && std::isfinite(local_co2)) {
      out.co2 = local_co2; out.co2_source = DisplaySource::LOCAL;
    }
    if (remote && std::isfinite(remote_temp_)) { out.temperature = remote_temp_; out.temperature_source = DisplaySource::HA; }
    else if (temp_seen_ && uint32_t(now - temp_ms_) <= MAX_AGE_MS && std::isfinite(local_temp)) {
      out.temperature = local_temp; out.temperature_source = DisplaySource::LOCAL;
    }
    return out;
  }
 protected:
  static float parse_(const char *text, float lo, float hi) {
    char *end; const float value = std::strtof(text, &end);
    return end != text && *end == '\0' && std::isfinite(value) && value >= lo && value <= hi ? value : NAN;
  }
  float remote_co2_{NAN}, remote_temp_{NAN};
  uint64_t remote_epoch_{0};
  uint32_t remote_ms_{0}, co2_ms_{0}, temp_ms_{0};
  bool remote_seen_{false}, co2_seen_{false}, temp_seen_{false};
};
}  // namespace esphome::cgdn1_display
