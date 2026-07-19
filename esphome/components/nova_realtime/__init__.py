import re

from esphome import automation
import esphome.codegen as cg
from esphome.components import esp32, microphone, speaker, wifi
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_SPEAKER


DEPENDENCIES = ["esp32", "microphone", "network", "speaker", "wifi"]
AUTO_LOAD = ["audio", "json", "ring_buffer"]
CODEOWNERS = []

CONF_GATEWAY_URL = "gateway_url"
CONF_DEVICE_ID = "device_id"
CONF_ON_CONNECTED = "on_connected"
CONF_ON_DISCONNECTED = "on_disconnected"
CONF_ON_STATE = "on_state"
CONF_ON_ERROR = "on_error"
CONF_WAKE_WORD = "wake_word"

nova_ns = cg.esphome_ns.namespace("nova_realtime")
NovaRealtime = nova_ns.class_("NovaRealtime", cg.Component)
StartAction = nova_ns.class_(
    "StartAction", automation.Action, cg.Parented.template(NovaRealtime)
)
StopAction = nova_ns.class_(
    "StopAction", automation.Action, cg.Parented.template(NovaRealtime)
)
IsRunningCondition = nova_ns.class_(
    "IsRunningCondition", automation.Condition, cg.Parented.template(NovaRealtime)
)
IsConnectedCondition = nova_ns.class_(
    "IsConnectedCondition", automation.Condition, cg.Parented.template(NovaRealtime)
)


def _lan_ws_url(value):
    value = cv.url(value)
    if not value.startswith("ws://"):
        raise cv.Invalid("NOVA trusted-LAN gateway URL must use ws://")
    return value


def _device_id(value):
    value = cv.string_strict(value)
    if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", value) is None:
        raise cv.Invalid("NOVA device ID may contain letters, digits, dot, dash, and underscore")
    return value

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(NovaRealtime),
        cv.Required(CONF_MICROPHONE): microphone.microphone_source_schema(
            min_bits_per_sample=16,
            max_bits_per_sample=16,
            min_channels=1,
            max_channels=1,
        ),
        cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Required(CONF_GATEWAY_URL): _lan_ws_url,
        cv.Required(CONF_DEVICE_ID): _device_id,
        cv.Optional(CONF_ON_CONNECTED): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_DISCONNECTED): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_STATE): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_ERROR): automation.validate_automation(single=True),
    }
).extend(cv.COMPONENT_SCHEMA)

FINAL_VALIDATE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_MICROPHONE): microphone.final_validate_microphone_source_schema(
            "nova_realtime", sample_rate=16000
        )
    },
    extra=cv.ALLOW_EXTRA,
)


async def to_code(config):
    # Opt into ESPHome's reference-counted runtime controls. Without these
    # requests the public C++ APIs are intentionally compiled out.
    wifi.enable_runtime_power_save_control()
    wifi.enable_runtime_roaming_suppression()

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # NOVA participates in the physical capture lifecycle so conversion work is
    # gated by its session and listener counting keeps shared capture active.
    mic_source = await microphone.microphone_source_to_code(
        config[CONF_MICROPHONE], passive=False
    )
    cg.add(var.set_microphone_source(mic_source))
    output = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_speaker(output))
    cg.add(var.set_gateway_url(config[CONF_GATEWAY_URL]))
    cg.add(var.set_device_id(config[CONF_DEVICE_ID]))

    if CONF_ON_CONNECTED in config:
        await automation.build_automation(var.get_connected_trigger(), [], config[CONF_ON_CONNECTED])
    if CONF_ON_DISCONNECTED in config:
        await automation.build_automation(
            var.get_disconnected_trigger(), [], config[CONF_ON_DISCONNECTED]
        )
    if CONF_ON_STATE in config:
        await automation.build_automation(
            var.get_state_trigger(), [(cg.std_string, "phase")], config[CONF_ON_STATE]
        )
    if CONF_ON_ERROR in config:
        await automation.build_automation(
            var.get_error_trigger(),
            [(cg.std_string, "code"), (cg.std_string, "message")],
            config[CONF_ON_ERROR],
        )

    esp32.add_idf_component(
        name="espressif/esp_websocket_client",
        ref="1.7.0",
    )


NOVA_SCHEMA = cv.Schema({cv.GenerateID(): cv.use_id(NovaRealtime)})


@automation.register_action(
    "nova_realtime.start",
    StartAction,
    NOVA_SCHEMA.extend(
        {cv.Optional(CONF_WAKE_WORD, default=""): cv.templatable(cv.string)}
    ),
    synchronous=True,
)
async def start_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    wake_word = await cg.templatable(config[CONF_WAKE_WORD], args, cg.std_string)
    cg.add(var.set_wake_word(wake_word))
    return var


@automation.register_action(
    "nova_realtime.stop", StopAction, NOVA_SCHEMA, synchronous=True
)
async def stop_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_condition("nova_realtime.is_running", IsRunningCondition, NOVA_SCHEMA)
async def is_running_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_condition("nova_realtime.connected", IsConnectedCondition, NOVA_SCHEMA)
async def is_connected_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
