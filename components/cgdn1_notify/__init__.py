"""MQTT notifications and questions on the CGDN1 screen (header-only core, ours).

Loading this component (`cgdn1_notify:` in the YAML) copies notify.h into the build; ESPHome's
generated esphome.h includes it, so the cgdn1.yaml lambdas/globals can use
`cgdn1_notify::Queue` / `cgdn1_notify::Message`. The core (queue, timeouts, answers, word wrap,
hint row) has no ESPHome dependencies and is unit-tested on the host (tests/test_notify.py); the
MQTT topics, JSON parsing, touch slider and display lambda are wired up in cgdn1.yaml.
"""
import esphome.config_validation as cv

CODEOWNERS = []

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass
