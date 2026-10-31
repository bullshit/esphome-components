import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    CONF_TYPE,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_POWER,
)

from . import ups_hid_ns, UpsHidComponent, CONF_UPS_HID_ID

DEPENDENCIES = ["ups_hid"]

UpsHidBinarySensor = ups_hid_ns.class_(
    "UpsHidBinarySensor", binary_sensor.BinarySensor, cg.Component
)

BINARY_SENSOR_TYPES = {
    "online": {
        "device_class": DEVICE_CLASS_CONNECTIVITY,
    },
    "on_battery": {
        "device_class": DEVICE_CLASS_BATTERY,
    },
    "low_battery": {
        "device_class": DEVICE_CLASS_BATTERY,
    },
    "fault": {
        "device_class": DEVICE_CLASS_PROBLEM,
    },
    "overload": {
        "device_class": DEVICE_CLASS_POWER,
    },
    "charging": {
        "device_class": DEVICE_CLASS_BATTERY,
    },
}


def _apply_type_defaults(config):
    # ESPHome no longer has set_device_class(); let new_binary_sensor() apply it.
    defaults = BINARY_SENSOR_TYPES.get(config[CONF_TYPE], {})
    config = config.copy()
    if "device_class" in defaults:
        config.setdefault("device_class", defaults["device_class"])
    return config


CONFIG_SCHEMA = cv.All(
    binary_sensor.binary_sensor_schema(UpsHidBinarySensor).extend(
        {
            cv.GenerateID(CONF_UPS_HID_ID): cv.use_id(UpsHidComponent),
            cv.Required(CONF_TYPE): cv.one_of(*BINARY_SENSOR_TYPES, lower=True),
        }
    ),
    _apply_type_defaults,
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_UPS_HID_ID])
    var = await binary_sensor.new_binary_sensor(config)
    await cg.register_component(var, config)

    sensor_type = config[CONF_TYPE]
    cg.add(var.set_sensor_type(sensor_type))
    cg.add(parent.register_binary_sensor(var, sensor_type))

