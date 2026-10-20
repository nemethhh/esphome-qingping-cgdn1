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
| Messages    | Optional: notifications and questions over MQTT, answered by a tap or swipe (see below) |
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
  cgdn1_notify           MQTT screen messages: queue, answers, word wrap
packages/mqtt_*.yaml     optional MQTT screen messages (substitution mqtt_messages)
tools/cgdn1_ota_push.py  push an image to the stock firmware over local MQTT
tools/gen_stock_icons.py regenerate stock_icons.h from a stock firmware image
tests/                   host tests (g++ for the C++ logic, image/partition checks)
docs/                    install guide, hardware map
```

## Screen messages over MQTT

Optional, off by default. Build with `esphome -s mqtt_messages true compile cgdn1.yaml` and
fill in the `mqtt_*` entries in `secrets.yaml`: next to the HA API the device then connects to
that MQTT broker for on-screen messages only (no discovery; HA keeps using the API). Publish,
not retained, to `cgdn1/notify` (the prefix is the device name):

```json
{"id": "door", "title": "Front door", "text": "Open for 10 min. Lock it?",
 "actions": {"tap_left": "Lock", "tap_right": "Ignore"}, "timeout": 60}
```

- Only `text` is needed; a plain-text payload works too. Without `actions` it is a notification
  (10 s, any touch dismisses it); with `actions` it is a question (60 s) that only a mapped
  gesture answers: `tap_left`, `tap_centre`, `tap_right`, `swipe_left` (toward the left end),
  `swipe_right`. The labels show in a hint row at the bottom.
- `timeout` 1-3600 s from when the message is first on screen; `wake` (default `true`) lights a
  dark screen for it. One message shows, up to 4 wait; the same `id` replaces a queued one.
- A message with an `id` reports how it ended on `cgdn1/notify/response`:
  `{"id": "door", "response": "tap_left", "label": "Lock"}`, or `response` `dismissed`,
  `timeout`, `cleared` or `dropped` (queue full).
- `cgdn1/notify/clear`: an id removes that message, an empty payload removes all.
- The text takes the largest font (24/20/17/14 px) that fits it whole; ASCII and Cyrillic.

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
