"""Optional power control around ESPHome's unmodified PMSA003I reader.

The component intentionally takes an explicit board-verified power pin. It never
touches the power-hold latch or assumes that an I2C sleep command exists.
"""
from esphome import pins
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.components.pmsa003i.sensor import PMSA003IComponent
from esphome.const import CONF_ID, CONF_PM_1_0, CONF_PM_2_5, CONF_PM_10_0

# PMSA003I is a sensor platform, not a top-level YAML component. Its exact
# reader type is enforced by use_id below; the board-specific controller is ESP32.
DEPENDENCIES = ["esp32"]
CONF_READER_ID = "reader_id"
CONF_ENABLE_PIN = "enable_pin"
CONF_WARMUP_TIME = "warmup_time"
ns = cg.esphome_ns.namespace("cgdn1_pm")
Cgdn1Pm = ns.class_("Cgdn1Pm", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(Cgdn1Pm),
    cv.Required(CONF_READER_ID): cv.use_id(PMSA003IComponent),
    cv.Required(CONF_ENABLE_PIN): pins.gpio_output_pin_schema,
    cv.Required(CONF_PM_1_0): cv.use_id(sensor.Sensor),
    cv.Required(CONF_PM_2_5): cv.use_id(sensor.Sensor),
    cv.Required(CONF_PM_10_0): cv.use_id(sensor.Sensor),
    cv.Optional(CONF_WARMUP_TIME, default="30s"): cv.All(
        cv.positive_time_period_milliseconds, cv.Range(min=cv.TimePeriod(milliseconds=30000))
    ),
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_reader(await cg.get_variable(config[CONF_READER_ID])))
    cg.add(var.set_enable_pin(await cg.gpio_pin_expression(config[CONF_ENABLE_PIN])))
    cg.add(var.set_sensors(*[await cg.get_variable(config[key]) for key in
                            (CONF_PM_1_0, CONF_PM_2_5, CONF_PM_10_0)]))
    cg.add(var.set_warmup_time(config[CONF_WARMUP_TIME].total_milliseconds))
