from pathlib import Path

import esphome.codegen as cg
from esphome.components import esp32, logger
import esphome.config_validation as cv
from esphome.const import CONF_ID
import esphome.final_validate as fv

CODEOWNERS = ["@nemethhh"]

CONF_FLASH_LOG = "flash_log"
CONF_TRIAL_MODE = "trial_mode"
CONF_FLASH_SIZE = "flash_size"
CONF_PARTITIONS = "partitions"

# The stock CGDN1 partition table. It has to match what is on the device (the
# stock bootloader and the stock firmware in the other slot use it), so this
# component always brings it and `esp32: partitions:` must not be set.
PARTITIONS_CSV = Path(__file__).parent / "cgdn1-partitions.csv"

cgdn1_boot_ns = cg.esphome_ns.namespace("cgdn1_boot")
Cgdn1Boot = cgdn1_boot_ns.class_("Cgdn1Boot", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Cgdn1Boot),
        # Every boot, mark the other OTA slot (the firmware ESPHome was installed
        # from, e.g. stock) as the next boot, so any reboot, crash or power cycle
        # returns to it. For a first, reversible try-out. Never use ESPHome's own
        # OTA while this is on: it overwrites that other slot.
        cv.Optional(CONF_TRIAL_MODE, default=False): cv.boolean,
        # Persist the ESPHome log (IDF driver logs included) to the top 64 KB of
        # the `test` partition (0x3e3000-0x3f2fff), for when neither UART nor
        # network is available. Reading it back needs a flash-read tool on the device side.
        # Opt-in. Refuses to touch the region unless it is blank or our own log.
        cv.Optional(CONF_FLASH_LOG, default=False): cv.All(
            cv.boolean, cv.requires_component("logger")
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config):
    esp32_conf = fv.full_config.get().get("esp32", {})
    if CONF_PARTITIONS in esp32_conf:
        raise cv.Invalid(
            "cgdn1_boot brings the stock CGDN1 partition table: remove `partitions:` from `esp32:`"
        )
    if esp32_conf.get(CONF_FLASH_SIZE) != "4MB":
        raise cv.Invalid("the CGDN1 has 4MB of flash: set `flash_size: 4MB` in `esp32:`")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    esp32.add_extra_build_file("partitions.csv", PARTITIONS_CSV)
    cg.add_global(cg.RawStatement('#include "esphome/components/cgdn1_boot/cgdn1_boot.h"'))
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    if config[CONF_TRIAL_MODE]:
        cg.add_define("CGDN1_TRIAL_MODE")
    if config[CONF_FLASH_LOG]:
        logger.request_log_listener()
        cg.add_define("CGDN1_FLASH_LOG")
        # Emitted into main.cpp setup() after the logger's pre_setup() (logger
        # to_code runs at EARLY_INIT priority), i.e. before App.setup().
        cg.add(var.start_flash_log())
