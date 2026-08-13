import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import esp32, microphone, speaker
from esphome.const import CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@IvanBrazza"]
DEPENDENCIES = ["network", "microphone", "speaker"]
AUTO_LOAD = ["json"]

CONF_HOST = "host"
CONF_PORT = "port"
CONF_VERSION = "version"
CONF_MICROPHONE = "microphone"
CONF_MIC_CHANNEL = "mic_channel"
CONF_SPEAKER = "speaker"
CONF_ON_LEDS = "on_leds"
CONF_ON_VOLUME_SET = "on_volume_set"
CONF_ON_PHASE = "on_phase"

ns = cg.esphome_ns.namespace("echomuse_client")
EchoMuseClient = ns.class_("EchoMuseClient", cg.Component)
OnLedsTrigger = ns.class_(
    "OnLedsTrigger", automation.Trigger.template(cg.std_vector.template(cg.uint8))
)
OnVolumeSetTrigger = ns.class_(
    "OnVolumeSetTrigger", automation.Trigger.template(cg.float_)
)
OnPhaseTrigger = ns.class_(
    "OnPhaseTrigger", automation.Trigger.template(cg.std_string)
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(EchoMuseClient),
        cv.Required(CONF_HOST): cv.string_strict,
        cv.Optional(CONF_PORT, default=8767): cv.port,
        cv.Optional(CONF_VERSION, default="dev"): cv.string_strict,
        cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
        cv.Optional(CONF_MIC_CHANNEL, default=0): cv.int_range(min=0, max=1),
        cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Optional(CONF_ON_LEDS): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnLedsTrigger)}
        ),
        cv.Optional(CONF_ON_VOLUME_SET): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnVolumeSetTrigger)}
        ),
        cv.Optional(CONF_ON_PHASE): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnPhaseTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    esp32.add_idf_component(
        name="espressif/esp_websocket_client",
        ref="1.7.0",
    )
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_version(config[CONF_VERSION]))
    cg.add(var.set_mic_channel(config[CONF_MIC_CHANNEL]))
    cg.add(var.set_microphone(await cg.get_variable(config[CONF_MICROPHONE])))
    cg.add(var.set_speaker(await cg.get_variable(config[CONF_SPEAKER])))

    for conf in config.get(CONF_ON_LEDS, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.std_vector.template(cg.uint8), "frame")], conf
        )
    for conf in config.get(CONF_ON_VOLUME_SET, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.float_, "volume")], conf)
    for conf in config.get(CONF_ON_PHASE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "phase")], conf)
