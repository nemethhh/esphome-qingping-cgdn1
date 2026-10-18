# CGDN1 hardware map

Everything here was recovered by static analysis of the stock 4.8.5 ESP32 app image
(`CGDN1_ESP_4.8.5_*.bin`, model "Dany-ESP") and confirmed on one unit running this firmware.
Function addresses (`FUN_400xxxxx`) in the YAML and component comments refer to that image.

## SoC and flash

- ESP32 (ESP32-WROVER-B module), 4 MB flash, DIO @ 80 MHz. Stock bootloader is ESP-IDF 4.0.3.
- No secure boot, no flash encryption, no image signature check.
- The factory-MAC eFuse fails its CRC on this product line; stock (a patched IDF fork) ignores
  it. ESPHome needs `ignore_efuse_mac_crc: true` or WiFi runs with an all-zero MAC.
- No UART pads are broken out and the screen is glued to the case: in practice the only way in
  is the stock OTA (see [install.md](install.md)).

### Partition table (stock, kept as is: `cgdn1-partitions.csv`)

| Name        | Type | Subtype  | Offset   | Size     | Notes                                   |
|-------------|------|----------|----------|----------|-----------------------------------------|
| nvs         | data | nvs      | 0x9000   | 0x4000   | stock WiFi/MQTT config + ESPHome prefs  |
| otadata     | data | ota      | 0xd000   | 0x2000   |                                         |
| phy_init    | data | phy      | 0xf000   | 0x1000   |                                         |
| ota_0       | app  | ota_0    | 0x10000  | 0x1E0000 |                                         |
| ota_1       | app  | ota_1    | 0x1f0000 | 0x1E0000 |                                         |
| test        | app  | test     | 0x3d0000 | 0x23000  | factory info (0x3d2000), never written  |
| factory_nvs | data | nvs      | 0x3f3000 | 0x4000   |                                         |
| nvs_keys    | data | nvs_keys | 0x3f7000 | 0x1000   |                                         |
| minvs       | data | 0xfe     | 0x3f8000 | 0x4000   |                                         |

The stock commit-time image check hangs on images with 9 or more segments; ESPHome's
image has 8 or fewer (checked by `tests/test_esphome_image.py`).

## Power

| GPIO | Direction | Function                                                              |
|------|-----------|-----------------------------------------------------------------------|
| 23   | out       | **Power-hold latch.** Must go HIGH at boot or the unit switches off.  |
| 13   | out       | Peripheral/sensor rail enable (HIGH)                                  |
| 18   | out       | Peripheral/sensor rail enable (HIGH)                                  |
| 36   | in        | Back power button, active low, external pull-up                       |
| 34   | in        | USB present / charging, active low                                    |
| 39   | ADC1 ch3  | Battery voltage, 1:2 divider                                          |
| 35   | ADC1 ch7  | Charger current sense                                                 |

`cgdn1_boot` drives 23/13/18 HIGH from a global constructor, before ESPHome starts. The only
path that drives GPIO23 low is the back-button long press (≥ 2 s, on battery), like stock.

## I2C bus 0: SDA=26, SCL=27, 20 kHz

| Addr | Part                         | ESPHome platform |
|------|------------------------------|------------------|
| 0x62 | Sensirion SCD40 (CO2)        | `scd4x`          |
| 0x12 | Plantower PMSA003I (PM)      | `pmsa003i`       |
| 0x44 | Sensirion SHT4x (T/RH)       | `sht4x`          |
| 0x10 | Apple MFi auth coprocessor   | (not used)       |

The SHT4x sits next to the board's heat sources and reads about 6 °C high once warm. Stock
corrects this with a warm-up model, ported exactly in `components/cgdn1_tcomp`.

## I2C bus 1: SDA=12, SCL=14, 400 kHz

- ATtiny814 @ 0x0C: capacitive touch-slider controller only. Read-only 2-byte read
  (byte 0 = position, byte 1 bit 7 = finger present), data-ready on **GPIO32** (active low).
  Gestures are decoded on the ESP32 (`components/cgdn1_touch`).

## OLED (SPI2/HSPI, mode 0)

- SCLK=5, MOSI=4, CS=15, DC=2, RST=22, no MISO. 128 × 160 GUI, mounted rotated (upright
  landscape = `rotation: 90`).
- Two factory panel variants, picked by the byte at flash 0x3d205d (stock "lcd id"):
  1 = variant B (CH1120-family, 4-bpp grayscale, 8 MHz), anything else = variant A
  (SH1107/SH1108-style 1-bpp, 2 MHz). Only variant B has been tested on hardware.
- CS must stay low after init (stock never raises it).

## RGB status LED

- LEDC, 10 bit, 5 kHz: R=GPIO25, G=GPIO21, B=GPIO19, active high.
- Colour = level of the reading on screen (stock thresholds and duties in `components/cgdn1_led/led.h`).
