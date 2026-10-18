---
title: Qingping Air Monitor Lite (CGDN1)
date-published: 2026-10-18
type: sensor
standard: global
board: esp32
project-url: https://github.com/nemethhh/esphome-qingping-cgdn1
difficulty: 3
---

<!-- Draft for a PR to esphome/esphome-devices: src/docs/devices/Qingping-Air-Monitor-Lite-CGDN1/index.md
     Add a photo (Qingping-CGDN1.jpg) next to it before submitting. -->

## Description

The Qingping Air Monitor Lite (CGDN1) measures CO2, PM1.0/2.5/10, temperature and humidity, and
has a 128 × 160 OLED, a capacitive touch slider, an RGB status LED and a battery. The ESP32
("Dany-ESP" hardware, stock firmware 4.8.x) drives everything directly. The configuration
below re-creates the stock screen, touch, LED and battery behaviour with external components
and runs without the Qingping/Xiaomi cloud.

## Installation

The UART is only reachable inside the case, behind a glued-in screen. ESPHome is installed through the stock
firmware's own OTA over a local MQTT connection, with a reversible trial mode first. Follow the
[install guide](https://github.com/nemethhh/esphome-qingping-cgdn1/blob/main/docs/install.md).
The stock partition table must be kept (`cgdn1-partitions.csv`).

## GPIO Pinout

| Pin    | Function                                         |
|--------|--------------------------------------------------|
| GPIO23 | Power-hold latch (must be HIGH at boot)          |
| GPIO13 | Sensor rail enable                               |
| GPIO18 | Sensor rail enable                               |
| GPIO26 | I2C bus 0 SDA (SCD40 0x62, PMSA003I 0x12, SHT4x 0x44) |
| GPIO27 | I2C bus 0 SCL (20 kHz)                           |
| GPIO12 | I2C bus 1 SDA (ATtiny814 touch slider 0x0C)      |
| GPIO14 | I2C bus 1 SCL (400 kHz)                          |
| GPIO32 | Touch data ready (active low)                    |
| GPIO5  | OLED SPI SCLK                                    |
| GPIO4  | OLED SPI MOSI                                    |
| GPIO15 | OLED CS                                          |
| GPIO2  | OLED DC                                          |
| GPIO22 | OLED reset                                       |
| GPIO25 | LED red (LEDC)                                   |
| GPIO21 | LED green (LEDC)                                 |
| GPIO19 | LED blue (LEDC)                                  |
| GPIO36 | Back power button (active low)                   |
| GPIO34 | USB present / charging (active low)              |
| GPIO39 | Battery voltage (ADC, 1:2 divider)               |
| GPIO35 | Charge current sense (ADC)                       |

## Configuration

```yaml url=https://github.com/nemethhh/esphome-qingping-cgdn1/blob/main/cgdn1.yaml
```
