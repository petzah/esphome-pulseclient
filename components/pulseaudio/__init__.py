import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number, switch
from esphome.const import CONF_ID

CONF_HOST = "host"
CONF_PORT = "port"

CODEOWNERS = ["@you"]
DEPENDENCIES = ["network"]
AUTO_LOAD = ["number", "switch"]

pulseaudio_ns = cg.esphome_ns.namespace("pulseaudio")
PulseAudioComponent = pulseaudio_ns.class_(
    "PulseAudioComponent", cg.Component, cg.Nameable
)
PulseAudioVolumeNumber = pulseaudio_ns.class_(
    "PulseAudioVolumeNumber", number.Number
)
PulseAudioMuteSwitch = pulseaudio_ns.class_(
    "PulseAudioMuteSwitch", switch.Switch
)

CONF_VOLUME = "volume"
CONF_MUTE   = "mute"
CONF_COOKIE = "cookie"

# A PulseAudio cookie is exactly 256 bytes, represented as 512 hex characters.
def validate_cookie(value):
    value = cv.string(value)
    value = value.replace(" ", "").replace(":", "").lower()
    if len(value) != 512:
        raise cv.Invalid(
            f"cookie must be exactly 512 hex characters (256 bytes), got {len(value)}"
        )
    try:
        bytes.fromhex(value)
    except ValueError:
        raise cv.Invalid("cookie must contain only hex characters (0-9, a-f)")
    return value

VOLUME_SCHEMA = number.NUMBER_SCHEMA.extend(
    {cv.GenerateID(): cv.declare_id(PulseAudioVolumeNumber)}
)
MUTE_SCHEMA = switch.SWITCH_SCHEMA.extend(
    {cv.GenerateID(): cv.declare_id(PulseAudioMuteSwitch)}
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PulseAudioComponent),
        cv.Required(CONF_HOST): cv.string,
        cv.Optional(CONF_PORT, default=4713): cv.port,
        # 512-char hex string (256 bytes).  Omit to use anonymous auth (requires
        # auth-anonymous=true on the server).  See DOCUMENTATION.md for how to
        # extract the cookie from the PipeWire host.
        cv.Optional(CONF_COOKIE): validate_cookie,
        cv.Optional(CONF_VOLUME): VOLUME_SCHEMA,
        cv.Optional(CONF_MUTE):   MUTE_SCHEMA,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_port(config[CONF_PORT]))

    if CONF_COOKIE in config:
        cg.add(var.set_cookie_hex(config[CONF_COOKIE]))

    if CONF_VOLUME in config:
        vol = await number.new_number(
            config[CONF_VOLUME],
            min_value=0,
            max_value=100,
            step=1,
        )
        cg.add(var.set_volume_number(vol))

    if CONF_MUTE in config:
        mute = await switch.new_switch(config[CONF_MUTE])
        cg.add(var.set_mute_switch(mute))
