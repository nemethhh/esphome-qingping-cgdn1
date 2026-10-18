"""Stock CGDN1 temperature/humidity self-heating compensation (header-only core).

Loading this component (`cgdn1_tcomp:` in the YAML) copies tcomp.h into the build;
ESPHome's generated esphome.h includes it, so the cgdn1.yaml lambdas/globals can use
`tcomp::Engine` / `tcomp::Saved`. The core has no ESPHome dependencies and is unit-tested
on the host (tests/test_tcomp.py).
"""
import esphome.config_validation as cv

CODEOWNERS = ["@nemethhh"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass
