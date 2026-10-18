"""Stock CGDN1 night mode logic (header-only core).

Loading this component (`cgdn1_night:` in the YAML) copies night.h into the build; ESPHome's
generated esphome.h includes it, so the cgdn1.yaml lambdas/globals can use
`cgdn1_night::Engine` / `cgdn1_night::Inputs`. The core (stock 4.8.5 window test and the night
branch of the 1 s system task) has no ESPHome dependencies and is unit-tested on the host
(tests/test_night.py); the clock, settings entities and the 1 s tick are wired up in cgdn1.yaml.
"""
import esphome.config_validation as cv

CODEOWNERS = ["@nemethhh"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass
