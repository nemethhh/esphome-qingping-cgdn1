"""Compile real pure headers: freshness, independent fallback and fixed pixel bounds."""
from pathlib import Path
import shutil
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[1]

def test_sources_and_layout(tmp_path):
    cc = shutil.which("c++")
    assert cc, "a host C++ compiler is required for the safety tests"
    source = tmp_path / "test.cpp"
    source.write_text(r'''
#include "display_sources.h"
#include "summary_page.h"
#include <cassert>
#include <limits>
#include <set>
#include <utility>
using namespace esphome::cgdn1_display;
int main() {
  const int64_t epoch = 1793323200;
  DisplaySources s;
  assert(std::isnan(s.choose(0, epoch, 700, 23).co2)); // no first local sample
  s.local_co2_seen(10); s.local_temperature_seen(10);
  auto x = s.choose(10, epoch, 700, 23);
  assert(x.co2 == 700 && x.temperature == 23 && x.co2_source == DisplaySource::LOCAL);
  assert(s.ingest("1793323200|800|24.5", 20, epoch));
  x = s.choose(20, epoch, 700, 23);
  assert(x.co2 == 800 && x.temperature == 24.5 && x.co2_source == DisplaySource::HA);
  // One invalid HA field does not discard the other one.
  assert(s.ingest("1793323201|unavailable|24.5", 30, epoch+1));
  x = s.choose(30, epoch+1, 700, 23);
  assert(x.co2 == 700 && x.co2_source == DisplaySource::LOCAL && x.temperature == 24.5);
  assert(s.ingest("1793323202|800|nan", 40, epoch+2));
  x = s.choose(40, epoch+2, 700, 23); assert(x.co2 == 800 && x.temperature == 23);
  assert(!s.ingest("1793323202|900|25", 150000, epoch+150)); // replay cannot extend TTL
  s.local_co2_seen(180050); s.local_temperature_seen(180050);
  x = s.choose(180050, epoch+181, 701, 23.1);
  assert(x.co2 == 701 && x.co2_source == DisplaySource::LOCAL);
  s.clear(); x = s.choose(180060, epoch+181, 702, 23.2); assert(x.co2 == 702);
  assert(s.ingest("1793323382|810|24.6", 180070, epoch+182)); // recovery
  assert(s.choose(180070, epoch+182, 700, 23).co2 == 810);
  // Stale/future/malformed packets and an unsynchronised clock fail closed.
  for (const auto *p : {"1793323200|800|24.5", "1793330000|800|24.5", "bad", "1793323382|800|24.5junk|x"}) {
    assert(!s.ingest(p, 180080, epoch+182));
    assert(s.choose(180080, epoch+182, 700, 23).co2_source == DisplaySource::LOCAL);
  }
  assert(!s.ingest("1793323382|800|24.5", 180080, 0));
  // Out of range numbers are not trusted, and old local samples also expire.
  assert(s.ingest("1793323383|inf|-999", 180090, epoch+183));
  x = s.choose(400000, epoch+400, 700, 23); assert(std::isnan(x.co2) && std::isnan(x.temperature));
  // Millis wrap does not break the local or remote timeout.
  DisplaySources wrap; wrap.local_co2_seen(0xfffffff0U);
  assert(wrap.choose(0x20U, epoch, 720, NAN).co2 == 720);
  assert(summary_due(0x20U, 0xfffffff0U, 48));
  assert(!summary_due(99, 0, 100));
  // Render every digit, transitions to four digits, missing/negative/max values.
  for (float c : {0.f, 111.f, 735.f, 999.f, 1000.f, 2500.f, 9999.f, NAN}) {
    for (float t : {-40.f, 0.f, 23.8f, 35.f, 125.f, NAN}) {
      std::set<std::pair<int,int>> a, b;
      auto plot = [&](int x,int y) { assert(x>=0 && x<160 && y>=0 && y<128); a.emplace(x,y); };
      draw_summary(c,t,plot);
      draw_summary(c,t,[&](int x,int y){b.emplace(x,y);});
      assert(!a.empty() && a == b); // no drift, regardless of redraw count
    }
  }
}
''')
    exe = tmp_path / "test"
    subprocess.run([cc, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(source),
                    "-I", str(ROOT / "components/cgdn1_display"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
