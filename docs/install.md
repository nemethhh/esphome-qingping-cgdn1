# Installing ESPHome on a CGDN1

The CGDN1's UART is only reachable inside the case, and opening the case risks damaging the
glued-in screen, so the install goes through the **stock firmware's own OTA**: over a local MQTT connection you tell
the stock firmware to download an image from your PC. It writes the image to its inactive OTA
slot, checks only a CRC-32 trailer and reboots into it. No signature check, no rollback.

**Read the whole page before starting.** An image that is well formed but dies before WiFi
comes up can't be recovered without opening the case.

## What protects you

- **A malformed image** fails the stock bootloader's image check and the bootloader falls back to
  the other slot (stock).
- **Trial mode** (`-s trial_mode true`): every ESPHome boot marks the *other* slot (stock) as the
  next boot, so any reboot, crash, watchdog or power cycle lands back on stock. Use it for the
  first install.
- **Power latch**: `cgdn1_boot` drives the power-hold GPIO high from a global constructor,
  earlier than stock does.
- **"Boot other firmware slot"** button (Home Assistant, config section): boots stock again
  later, as long as no ESPHome OTA update has overwritten it.

What is **not** covered: a crash inside ESP-IDF's own startup, before the trial mark is written.

## Status

Tested on one unit (stock firmware **4.8.5**, panel variant B). On that unit, every feature in
the YAML works, and the trial-mode return path works. The OTA push was done from a patched
stock build whose OTA code is identical to stock's. A push straight from unmodified stock and
the first boot with trial mode off have not been tried yet. Other stock versions and panel
variant A are untested.

## Requirements

- A CGDN1 on stock firmware 4.8.x (Settings → About on the device).
- An MQTT broker on your LAN, and a Linux PC on the same LAN (the push tool uses a Linux socket
  ioctl to confirm delivery).
- Python 3.10+ and `pip install -r requirements.txt` (ESPHome, paho-mqtt).

## 1. Point the stock firmware at your MQTT broker

Pick one:

- **Offline over BLE** with [bitrate16/qingping](https://github.com/bitrate16/qingping)
  (`pip install qingping`). Put the monitor in pairing mode, then:
  ```sh
  python3 -m qingping.bluetooth --mac <BLE MAC> \
      --wifi-ssid "<ssid>" --wifi-password "<password>" \
      --link-token 00112233445566778899AABBCCDDEEFF \
      --mqtt-host <broker> --mqtt-port 1883 --mqtt-login qingping --mqtt-password <pw> \
      --mqtt-client qingping-<WIFIMAC> \
      --mqtt-read qingping/<WIFIMAC>/down --mqtt-write qingping/<WIFIMAC>/up
  ```
  `<WIFIMAC>` is the Wi-Fi MAC without separators, upper case (e.g. `CCB5D1xxxxxx`).
- **Qingping developer portal** ("Private Access Config"), as described in
  [GreyEarl/qingping-air-monitor-mqtt](https://github.com/GreyEarl/qingping-air-monitor-mqtt).
  Note the topics you configure.

Check that the monitor publishes on its "up" topic (a type `13` message about once a minute,
including `sw_version`) before you continue.

## 2. Build the trial image

```sh
cp secrets.yaml.example secrets.yaml   # fill in; api_key: openssl rand -base64 32
esphome -s trial_mode true compile cgdn1.yaml
pytest tests/test_esphome_image.py -rs   # must pass with no skips
```

Push **only** `.esphome/build/cgdn1/build/firmware.ota.bin`, never `firmware.factory.bin`.
Make sure the WiFi credentials in `secrets.yaml` are right: with wrong ones you only get the
`cgdn1 Fallback` access point.

## 3. Push it

Allow TCP 8070 from the monitor in your PC's firewall, then:

```sh
python3 tools/cgdn1_ota_push.py .esphome/build/cgdn1/build/firmware.ota.bin --append-crc \
    --broker <broker> --mac <WIFIMAC> --pw-file <file with the broker password>
```

Use `--topic-down/--topic-up` if your topics differ from `qingping/{mac}/down` and `/up`, and
`--dry-run` first to check the message. Type `flash` at the prompt.

- Wait for `monitor acknowledged all N bytes`. If it reports undelivered bytes instead, the
  monitor stops at 99% and keeps running stock: push again.
- The stock commit can sit at 99–100% for a while. Every state there is safe.
- The tool then watches for a stock "hello" that ESPHome never sends, so "no hello from the new
  firmware yet" is the normal outcome. If it reports the old stock version, the image didn't take.

## 4. Try it out (trial mode)

Within a few seconds the screen shows "ESPHome / Qingping CGDN1 / starting...", then the
readings. Add the device in Home Assistant (ESPHome integration, `api_key` as encryption key).

Check the log first (`esphome logs cgdn1.yaml`): `dump_config` of `cgdn1_boot` must say
`Trial mark result: OK`. If it says FAILED, the next reboot comes back to ESPHome, not stock:
keep the unit powered and don't push anything until you know why.

While trial mode is on, **don't use ESPHome's own OTA** (dashboard Install, `esphome run`/
`upload`, the HA update entity, the captive portal's upload form): it writes the other slot and
overwrites stock.

To go back to stock, press **Restart** in HA, or leave it without an API client for 15 minutes
(ESPHome's `reboot_timeout`).

## 5. Install for good

Back on stock (step 4), build without trial mode and push again. Stock writes its inactive slot,
which is the one the trial image was in:

```sh
esphome compile cgdn1.yaml
python3 tools/cgdn1_ota_push.py .esphome/build/cgdn1/build/firmware.ota.bin --append-crc \
    --broker <broker> --mac <WIFIMAC> --pw-file <file>
```

From now on, update with normal ESPHome OTA. The first OTA update overwrites the stock slot.
Until then, the **Boot other firmware slot** button gets you back to stock. After that, it boots
the previous ESPHome image instead.

## Notes

- **Back button**: a click toggles the display; holding it ≥ 2 s powers the unit off (on battery
  only, like stock). Press it again to power on.
- **NVS** at 0x9000 is shared with the stock firmware's settings (ESPHome uses its own
  namespace). The build stops IDF's WiFi/PHY drivers from writing there, so stock keeps its
  config for the way back.
- `enable_ota_rollback` is off: the stock bootloader has no rollback support.
