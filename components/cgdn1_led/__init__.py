"""Stock CGDN1 RGB status LED logic (header-only core).

Loading this component (`cgdn1_led:` in the YAML) copies led.h into the build; ESPHome's
generated esphome.h includes it, so the cgdn1.yaml lambdas/globals can use
`cgdn1_led::Engine` / `cgdn1_led::Inputs`. The core (stock 4.8.5 level thresholds, duties,
orange blink) has no ESPHome dependencies and is unit-tested on the host (tests/test_led.py);
the three ledc outputs and the 100 ms refresh are wired up in cgdn1.yaml.
"""
import esphome.config_validation as cv

CODEOWNERS = ["@nemethhh"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass
