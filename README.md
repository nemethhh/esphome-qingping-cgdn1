# ESPHome for the Qingping Air Monitor Lite (CGDN1)

ESPHome firmware for the Qingping Air Monitor Lite (CGDN1, ESP32 "Dany-ESP" hardware), with
the stock screen, touch slider, status LED and battery handling re-created. It runs fully
local, with no cloud connection.

Installed over the air from the stock firmware, without opening the case. See
**[docs/install.md](docs/install.md)** before trying.

## Features

| Area        | What you get                                                                 |
|-------------|------------------------------------------------------------------------------|
| Sensors     | CO2 (SCD40), PM1.0/2.5/10 (PMSA003I), temperature/humidity (SHT4x)           |
| T/RH        | Stock self-heating compensation (`cgdn1_tcomp`) with a measured display + Bluetooth heat model, tunable from HA ("Heat model" Stock/Measured), plus offsets |
| Bluetooth   | Passive Bluetooth proxy for Home Assistant (advertisements only), switchable |
| Battery     | Voltage, stock percent table, charge current, charging state                 |
| Display     | Stock pages and digits plus a PM1.0 page, push slide on touch, burn-in drift, brightness, night mode; an info page (version, MAC, WiFi/IP, fallback AP, Bluetooth, uptime) that moves on after 10 s |
| Touch       | Taps/swipes/long touch from the ATtiny814 slider, as HA events               |
| LED         | Stock level colours for the reading on screen (PM standard selectable)       |
| Power       | Back button: click toggles the display, long press powers off (on battery)   |
| Safety      | Trial-mode install, "Boot other firmware slot" button                        |

## Layout

```
cgdn1.yaml               device configuration
components/              external components
  cgdn1_boot             power latch, stock partition table, power-off, trial mode, flash log
  cgdn1_display          OLED driver (two panel variants) + stock page renderer
  cgdn1_touch            ATtiny814 touch slider + gesture decoder
  cgdn1_tcomp            stock T/RH self-heating compensation + display/Bluetooth heat model
  cgdn1_led              stock RGB status LED logic
  cgdn1_night            stock night mode window
tools/cgdn1_ota_push.py  push an image to the stock firmware over local MQTT
tools/gen_stock_icons.py regenerate stock_icons.h from a stock firmware image
tests/                   host tests (g++ for the C++ logic, image/partition checks)
docs/                    install guide, hardware map
```

## Development

```sh
pip install -r requirements.txt
cp secrets.yaml.example secrets.yaml
esphome compile cgdn1.yaml
pytest -rs
```

The C++ logic in the components (`*.h` with no ESPHome includes) is unit-tested on the host
with the system C++ compiler.

## Third-party material

`components/cgdn1_display/stock_icons.h` contains bitmaps (outline digits, labels, page icons)
extracted from Qingping's stock 4.8.5 firmware, for an identical look. They are not covered by
the MIT license. `tools/gen_stock_icons.py` regenerates the header from your own copy of the
stock image.

Qingping is a trademark of its owner. This project is not affiliated with or endorsed by
Qingping.

## License

MIT, see [LICENSE](LICENSE), except as noted above.
