# ESPHome for the Qingping Air Monitor Lite (CGDN1)

> [!WARNING]
> **Installing this can brick your monitor, and there are no guarantees.** The install replaces
> the firmware over the air with no rollback. If an image fails to boot, the only way back is a
> USB-TTL (3.3 V UART) adapter wired to the ESP32 inside the case. Getting to the board means
> taking out the glued-in screen, which can break it. Use at your own risk: no warranty, no
> support, and no liability for a dead or damaged device.

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
| Bluetooth   | Passive Bluetooth proxy for Home Assistant (advertisements only), switchable; optional BTHome broadcasts (see below) |
| Battery     | Voltage, stock percent table, charge current, charging state                 |
| Display     | Stock pages and digits plus a PM1.0 page, push slide on touch, burn-in drift, brightness, night mode; an info page (version, MAC, WiFi/IP, fallback AP, Bluetooth, uptime) that moves on after 10 s |
| Touch       | Taps/swipes/long touch from the ATtiny814 slider, as HA events               |
| Messages    | Optional: notifications and questions over MQTT, answered by a tap or swipe (see below) |
| LED         | Stock level colours for the reading on screen (PM standard selectable)       |
| Power       | Back button: click toggles the display, long press powers off (on battery)   |
| Safety      | Trial-mode install, "Boot other firmware slot" button                        |
| Portable    | Optional: for a unit carried around and read over BLE, without WiFi or HA (see below) |

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
packages/bthome_*.yaml   optional BTHome v2 BLE broadcasts (substitution bthome)
packages/portable_*.yaml optional portable build (substitutions portable, timezone)
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

<details>
<summary>Example: Home Assistant automation (ask to lock the front door)</summary>

After the door has been unlocked for 10 min it asks on the screen: tap left locks it, tap right
ignores it. If someone locks it before answering, the question is cleared from the screen.
`lock.front_door` is a placeholder.

```yaml
alias: Front door - ask on the air monitor to lock it
mode: single
triggers:
  - trigger: state
    entity_id: lock.front_door
    to: unlocked
    for: "00:10:00"
actions:
  - action: mqtt.publish
    data:
      topic: cgdn1/notify
      payload: >-
        {{ {"id": "front_door",
            "title": "Front door",
            "text": "Unlocked for 10 min. Lock it?",
            "actions": {"tap_left": "Lock", "tap_right": "Ignore"},
            "timeout": 120} | to_json }}
  # An answer on the screen, or someone locking it by hand.
  - wait_for_trigger:
      - trigger: mqtt
        id: answer
        topic: cgdn1/notify/response
        value_template: "{{ value_json.id }}"
        payload: front_door
      - trigger: state
        id: locked
        entity_id: lock.front_door
        to: locked
    timeout: "00:02:30"  # fallback: the device itself reports "timeout" after 120 s
    continue_on_timeout: false
  - choose:
      - conditions: "{{ wait.trigger.id == 'locked' }}"
        sequence:
          - action: mqtt.publish
            data:
              topic: cgdn1/notify/clear
              payload: front_door
      - conditions: "{{ wait.trigger.payload_json.response == 'tap_left' }}"
        sequence:
          - action: lock.lock
            target:
              entity_id: lock.front_door
          - action: mqtt.publish
            data:
              topic: cgdn1/notify
              payload: '{"text": "Front door locked", "timeout": 4}'
```

`to_json` keeps the payload valid whatever the text holds; the `value_template` / `payload` pair
makes the wait react to this question's id only.

</details>

## BTHome broadcasts

Optional, off by default. Build with `esphome -s bthome true compile cgdn1.yaml` and the unit
sends its readings every 5 s as passive [BTHome v2](https://bthome.io/) BLE advertisements:
battery, temperature and humidity (compensated), PM2.5, PM10 and CO2. Any BTHome receiver
nearby can read them, with no pairing or connection: a phone app such as AndroidAQL, or Home
Assistant's BTHome integration through any Bluetooth adapter or proxy. They stop while the
"Bluetooth" switch is off.

- **Unencrypted, from the unit's fixed MAC.** Anyone in range can read the values, and a unit
  carried around can be recognised wherever it goes.
- **Shown twice in Home Assistant** for a unit also added through the ESPHome integration: HA
  discovers the BTHome device too (ignore that discovery to avoid it).

## Portable use

Optional, off by default. By default the unit expects to sit on one WiFi network next to Home
Assistant: after 15 min without WiFi, or without an API client, ESPHome reboots it; out of range
it opens a fallback AP with a captive portal; its clock and time zone come from HA. For a unit
carried around and read mostly over BLE, build with
`esphome -s portable true compile cgdn1.yaml`:

- **No WiFi/API reboot timeouts.** Out of range or without HA is normal. ESPHome keeps retrying
  WiFi, and OTA and the API come back with it. To manage the unit at more than one site, list
  them under `wifi: networks:`.
- **No fallback AP or captive portal.** Out of range it would take radio time from BLE, cost
  battery and offer a firmware upload form wherever the unit goes. In exchange, wrong WiFi
  credentials can't be fixed over the air.
- **Clock from SNTP** (any network with internet access), in UTC unless built with
  `-s timezone <zone>` (e.g. `America/Chicago`). Night mode's hours are in that zone. Until the
  first sync after a boot, night mode does nothing.
- **A restart once a day** (after 24 h of uptime), in place of the reboot timeouts as a
  backstop for a stuck WiFi stack. The self-heating state is saved across it.

In trial mode (`-s trial_mode true`) the 15 min timeouts stay, so a trial build that never gets
on WiFi, or never sees an API client, goes back to stock by itself.

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
