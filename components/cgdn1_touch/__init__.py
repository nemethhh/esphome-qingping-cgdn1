"""CGDN1 top touch slider: ATtiny814 capacitive-slider controller @0x0C on I2C bus 1.

Stock 4.8.5 protocol: register-less 2-byte read (byte0 = position,
byte1 bit7 = touched), GPIO32 = data-ready (active low). Gestures are decoded on the ESP with the
stock thresholds (gesture.h, host-tested by tests/test_touch_gesture.py):
tap_left / tap_centre / tap_right / swipe_left / swipe_right / long_touch.

    cgdn1_touch:
      id: touch
      i2c_id: bus_b
      data_ready_pin: {number: GPIO32, mode: INPUT_PULLUP, inverted: true}
      on_touch: ...      # finger down
      on_release: ...    # finger up (after on_gesture)
      on_gesture: ...    # `gesture` (std::string) = one of the names above
"""
from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import i2c
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@nemethhh"]
DEPENDENCIES = ["i2c"]

CONF_DATA_READY_PIN = "data_ready_pin"
CONF_ON_TOUCH = "on_touch"
CONF_ON_RELEASE = "on_release"
CONF_ON_GESTURE = "on_gesture"

cgdn1_touch_ns = cg.esphome_ns.namespace("cgdn1_touch")
Cgdn1Touch = cgdn1_touch_ns.class_("Cgdn1Touch", cg.Component, i2c.I2CDevice)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Cgdn1Touch),
            # Stock: GPIO32 input + pull-up, "ready" = low. Without it the chip is polled (20 ms).
            cv.Optional(CONF_DATA_READY_PIN): pins.internal_gpio_input_pin_schema,
            cv.Optional(CONF_ON_TOUCH): automation.validate_automation({}),
            cv.Optional(CONF_ON_RELEASE): automation.validate_automation({}),
            cv.Optional(CONF_ON_GESTURE): automation.validate_automation({}),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x0C))
)

_CALLBACK_AUTOMATIONS = (
    automation.CallbackAutomation(CONF_ON_TOUCH, "add_on_touch_callback", []),
    automation.CallbackAutomation(CONF_ON_RELEASE, "add_on_release_callback", []),
    automation.CallbackAutomation(
        CONF_ON_GESTURE, "add_on_gesture_callback", [(cg.std_string, "gesture")]
    ),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)
    if CONF_DATA_READY_PIN in config:
        cg.add(var.set_ready_pin(await cg.gpio_pin_expression(config[CONF_DATA_READY_PIN])))
    await automation.build_callback_automations(var, config, _CALLBACK_AUTOMATIONS)
