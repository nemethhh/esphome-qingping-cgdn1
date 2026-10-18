"""`display: - platform: cgdn1_display` -- the CGDN1 OLED (see cgdn1_display.h for the RE)."""
from esphome import pins
import esphome.codegen as cg
from esphome.components import display, spi
import esphome.config_validation as cv
from esphome.const import (
    CONF_CONTRAST,
    CONF_DC_PIN,
    CONF_FLIP_X,
    CONF_FLIP_Y,
    CONF_ID,
    CONF_LAMBDA,
    CONF_PAGES,
    CONF_RESET_PIN,
    CONF_ROTATION,
)

DEPENDENCIES = ["spi"]

CONF_VARIANT = "variant"
CONF_TEST_PATTERN = "test_pattern"

cgdn1_display_ns = cg.esphome_ns.namespace("cgdn1_display")
Cgdn1Display = cgdn1_display_ns.class_(
    "Cgdn1Display", cg.PollingComponent, display.DisplayBuffer, spi.SPIDevice
)
Variant = cgdn1_display_ns.enum("Variant")
VARIANTS = {"AUTO": Variant.VARIANT_AUTO, "A": Variant.VARIANT_A, "B": Variant.VARIANT_B}
TestPattern = cgdn1_display_ns.enum("TestPattern")
TEST_PATTERNS = {
    "off": TestPattern.TEST_PATTERN_OFF,
    "orientation": TestPattern.TEST_PATTERN_ORIENTATION,
    "checkerboard": TestPattern.TEST_PATTERN_CHECKERBOARD,
    "white": TestPattern.TEST_PATTERN_WHITE,
}


def _test_pattern(value):
    # A bare YAML `off` arrives as boolean False.
    if value is False:
        value = "off"
    return cv.enum(TEST_PATTERNS, lower=True)(value)


def _default_rotation(config):
    # Upright landscape on the glass = 90 (derived from the stock glyph bitmaps, see
    # cgdn1_display.h). Set `rotation:` in the YAML to override (0/90/180/270).
    config.setdefault(CONF_ROTATION, 90)
    return config


CONFIG_SCHEMA = cv.All(
    display.FULL_DISPLAY_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(Cgdn1Display),
            cv.Required(CONF_DC_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_RESET_PIN): pins.gpio_output_pin_schema,
            # Panel variant (stock "lcd id"): AUTO reads the factory byte at flash 0x3d205d like
            # stock (1 -> B: 4-bpp grayscale @ 8 MHz, else A: 1-bpp SH1108-style @ 2 MHz).
            # The SPI clock follows the variant; `data_rate` is ignored.
            cv.Optional(CONF_VARIANT, default="AUTO"): cv.enum(VARIANTS, upper=True),
            # Diagnostic frame instead of the lambda: off / orientation / checkerboard / white.
            cv.Optional(CONF_TEST_PATTERN, default="off"): _test_pattern,
            # Contrast at 100 % brightness (and the init value); stock init sends 0x81 0xD0.
            # set_brightness(%) scales it down (0x81 at runtime, no re-init); set_contrast() raw.
            cv.Optional(CONF_CONTRAST, default=0xD0): cv.int_range(min=0, max=255),
            # Mirror the 128x160 GUI axes (before rotation). Not needed per the RE; kept as an
            # escape hatch should the first look at the glass disagree.
            cv.Optional(CONF_FLIP_X, default=False): cv.boolean,
            cv.Optional(CONF_FLIP_Y, default=False): cv.boolean,
        }
    )
    .extend(cv.polling_component_schema("1s"))
    .extend(spi.spi_device_schema(cs_pin_required=True, default_data_rate=2e6, default_mode="MODE0")),
    cv.has_at_most_one_key(CONF_PAGES, CONF_LAMBDA),
    _default_rotation,
)

FINAL_VALIDATE_SCHEMA = spi.final_validate_device_schema(
    "cgdn1_display", require_miso=False, require_mosi=True
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)
    await spi.register_spi_device(var, config, write_only=True)

    cg.add(var.set_dc_pin(await cg.gpio_pin_expression(config[CONF_DC_PIN])))
    if CONF_RESET_PIN in config:
        cg.add(var.set_reset_pin(await cg.gpio_pin_expression(config[CONF_RESET_PIN])))
    cg.add(var.set_contrast_init(config[CONF_CONTRAST]))
    cg.add(var.set_flip(config[CONF_FLIP_X], config[CONF_FLIP_Y]))
    cg.add(var.set_variant(config[CONF_VARIANT]))
    cg.add(var.set_test_pattern(config[CONF_TEST_PATTERN]))
    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(
            config[CONF_LAMBDA], [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(lambda_))
