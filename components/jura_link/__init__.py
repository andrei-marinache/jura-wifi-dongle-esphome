import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart, sensor, text_sensor, binary_sensor
from esphome.const import CONF_ID, ENTITY_CATEGORY_DIAGNOSTIC

DEPENDENCIES = ["uart"]
# One instance per UART, so a sniffer can watch both directions of the link.
MULTI_CONF = True
AUTO_LOAD = ["sensor", "text_sensor", "binary_sensor"]
ns = cg.esphome_ns.namespace("jura_link")
JuraLink = ns.class_("JuraLink", cg.PollingComponent, uart.UARTDevice)
TEXTS = ("machine_type", "last_reply", "last_broadcast", "last_error", "at_decoded")
COUNTERS = ("requests", "replies", "timeouts", "unsolicited_frames", "framing_errors", "rejected_requests")
CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(JuraLink),
    cv.Optional("listen_only", default=False): cv.boolean,
    cv.Optional("sniff", default=False): cv.boolean,
    cv.Optional("auto_poll", default=False): cv.boolean,
    cv.Optional("mqtt_discovery", default=False): cv.boolean,
    # MQTT layout of the jura-mqtt BT bridge: <base_topic>/<machine_id>/...
    cv.Optional("base_topic", default="jura"): cv.string_strict,
    cv.Optional("discovery_prefix", default="homeassistant"): cv.string_strict,
    # Auto-detected per machine: model = family from the profile DB for the EF code
    # the machine reports (EF536 -> "S8", topics jura/s8/...), else the EF code.
    # Override only to pin a name, e.g. machine_model "S8 (EA)" -> machine_id "s8_ea"
    # to keep the jura-mqtt BT bridge's topics. machine_id defaults to slug(model).
    cv.Optional("machine_model", default=""): cv.string,
    cv.Optional("machine_id", default=""): cv.string,
    # Brew over MQTT (product/option selects, Brew button). Off by default.
    cv.Optional("allow_brew", default=False): cv.boolean,
    # Panel lock switch over MQTT (@TS:01 lock / @TS:00 unlock). Off by default.
    cv.Optional("allow_lock", default=False): cv.boolean,
    # Machine setting P_Arguments to read (hex), e.g. ["02", "13"] from the machine XML <MACHINESETTINGS>.
    cv.Optional("settings", default=[]): cv.ensure_list(cv.All(cv.string_strict, cv.Length(min=2, max=2))),
    cv.Optional("ignore_alert_bits", default=[12, 13, 36, 37, 148, 149, 150, 151]): cv.ensure_list(cv.int_range(min=0, max=0x7FFF)),
    cv.Optional("sniff_tag", default="rx"): cv.All(cv.string_strict, cv.Length(min=1, max=8)),
    cv.Optional("response_timeout", default="3s"): cv.All(cv.positive_time_period_milliseconds, cv.Range(min=cv.TimePeriod(milliseconds=100), max=cv.TimePeriod(seconds=30))),
    cv.Optional("connected"): binary_sensor.binary_sensor_schema(device_class="connectivity", entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
    **{cv.Optional(k): text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC) for k in TEXTS},
    **{cv.Optional(k): sensor.sensor_schema(accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC) for k in COUNTERS},
}).extend(cv.polling_component_schema("30s")).extend(uart.UART_DEVICE_SCHEMA)

def _final_validate(config):
    # Sniffing needs no TX pin: leaving it unconfigured keeps the line high-impedance.
    return uart.final_validate_device_schema("jura_link", baud_rate=9600, require_tx=not config["sniff"], require_rx=True, data_bits=8, parity="NONE", stop_bits=1)(config)

FINAL_VALIDATE_SCHEMA = _final_validate

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_listen_only(config["listen_only"] or config["sniff"]))
    cg.add(var.set_sniff(config["sniff"]))
    cg.add(var.set_sniff_tag(config["sniff_tag"]))
    cg.add(var.set_auto_poll(config["auto_poll"]))
    cg.add(var.set_mqtt_discovery(config["mqtt_discovery"]))
    cg.add(var.set_base_topic(config["base_topic"]))
    cg.add(var.set_discovery_prefix(config["discovery_prefix"]))
    cg.add(var.set_machine_model(config["machine_model"]))
    cg.add(var.set_machine_id(config["machine_id"]))
    cg.add(var.set_ignore_alert_bits(config["ignore_alert_bits"]))
    cg.add(var.set_allow_brew(config["allow_brew"]))
    cg.add(var.set_allow_lock(config["allow_lock"]))
    for arg in config["settings"]:
        cg.add(var.add_setting(arg.upper()))
    cg.add(var.set_response_timeout(config["response_timeout"].total_milliseconds))
    for key in TEXTS:
        if key in config:
            cg.add(getattr(var, "set_"+key)(await text_sensor.new_text_sensor(config[key])))
    for key in COUNTERS:
        if key in config:
            cg.add(getattr(var, "set_"+key)(await sensor.new_sensor(config[key])))
    if "connected" in config:
        cg.add(var.set_connected(await binary_sensor.new_binary_sensor(config["connected"])))
