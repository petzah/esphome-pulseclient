# ESP32 Remote PipeWire Control — AI Reconstruction Guide

This document contains everything needed to recreate this project from scratch.
The project is an **ESPHome external component** that lets an ESP32 control
PipeWire/PulseAudio volume and mute over WiFi using the PA native binary protocol,
exposing a Number and Switch entity to Home Assistant with no server-side software.

---

## Project structure to create

```
remote-pipewire-control/
├── components/
│   └── pulseaudio/
│       ├── __init__.py        ← ESPHome codegen (Python)
│       ├── pulseaudio.h       ← C++ header: PktBuf, TagReader, entities, component
│       └── pulseaudio.cpp     ← C++ impl: connect/handshake/send/recv/parse
├── server/
│   └── pipewire-pulse-tcp.conf  ← drop-in config for the Linux host
├── pipewire-control.yaml      ← example ESPHome YAML
└── DOCUMENTATION.md           ← user-facing docs (see below)
```

---

## Architecture

```
┌─────────────────────────┐        TCP :4713        ┌─────────────────────┐
│  ESP32 (ESPHome)        │ ──── PA native proto ──▶ │  Linux host         │
│                         │                          │  pipewire-pulse     │
│  rotary encoder ──┐     │ ◀── HA ESPHome API ───── │  ↕                  │
│  mute button   ──▶│ PA  │                          │  PipeWire           │
│                   │ clt │                          │  (audio subsystem)  │
└───────────────────┴─────┘                          └─────────────────────┘
```

- **Server**: pipewire-pulse with `module-native-protocol-tcp` on TCP port 4713
- **Client**: ESPHome external component speaking the PA native binary protocol
- **Hardware**: rotary encoder + mute button wired directly via ESPHome YAML (not C++)
- **HA integration**: Number entity (volume 0–100) + Switch entity (mute)

---

## PA Protocol — critical implementation details

### Packet framing (20-byte descriptor before every payload)

| Bytes | Field      | Value                       |
|-------|------------|-----------------------------|
| 0–3   | length     | payload length (big-endian) |
| 4–7   | channel    | `0xFFFFFFFF` (control chan) |
| 8–11  | offset_hi  | `0`                         |
| 12–15 | offset_lo  | `0`                         |
| 16–19 | flags      | `0`                         |

### Tag types (single byte prefix before each value)

| Constant          | Byte | Meaning                          |
|-------------------|------|----------------------------------|
| PA_TAG_U32        | 'L'  | followed by 4-byte big-endian u32|
| PA_TAG_ARBITRARY  | 'x'  | followed by 4-byte len + raw bytes|
| PA_TAG_STRING     | 't'  | followed by null-terminated string|
| PA_TAG_STRING_NULL| 'N'  | null/empty string                |
| PA_TAG_BOOL_TRUE  | '1'  | boolean true                     |
| PA_TAG_BOOL_FALSE | '0'  | boolean false                    |
| PA_TAG_SAMPLE_SPEC| 'a'  | format(u8) + channels(u8) + rate(u32)|
| PA_TAG_CHANNEL_MAP| 'm'  | n(u8) + n channel bytes          |
| PA_TAG_CVOLUME    | 'v'  | n(u8) + n * u32 volumes          |
| PA_TAG_PROPLIST   | 'P'  | see proplist wire format below   |

### Proplist wire format (critical — easy to get wrong)

Each proplist entry on the wire is:
```
'P'                         ← TAG_PROPLIST (once, at start)
't' + key_str + '\0'        ← key as TAG_STRING
'L' + u32(value_len)        ← value length as TAG_U32  ← REQUIRED, easy to miss
'x' + u32(value_len) + data ← value as TAG_ARBITRARY
'N'                         ← TAG_STRING_NULL terminator
```

The `TAG_U32` length field before `TAG_ARBITRARY` is **mandatory**. Omitting it causes
`PA_ERR_PROTOCOL (code 7)` on `SET_CLIENT_NAME`.

### Command codes

| Constant              | Value |
|-----------------------|-------|
| PA_CMD_ERROR          | 0     |
| PA_CMD_REPLY          | 2     |
| PA_CMD_AUTH           | 8     |
| PA_CMD_SET_CLIENT_NAME| 9     |
| PA_CMD_GET_SINK_INFO  | 21    |
| PA_CMD_SET_SINK_VOLUME| **36** |
| PA_CMD_SET_SINK_MUTE  | **39** |

> **Note**: values 36/39 confirmed empirically against pipewire-pulse 1.4.2.
> The enum has more entries between 32 and 36 than commonly documented.

### Protocol constants

```cpp
PA_PROTOCOL_VERSION = 35
PA_INVALID_INDEX    = 0xFFFFFFFF  // used as "default sink" index
PA_VOLUME_NORM      = 0x10000     // 65536 = 100% volume
PA_COOKIE_LENGTH    = 256         // bytes; 512 hex chars in YAML
```

### Handshake sequence

```
CLIENT                              SERVER
  │── AUTH (version=35, cookie) ──▶ │   payload: U32(CMD_AUTH) U32(tag) U32(ver) ARB(256 bytes cookie)
  │ ◀─────────────── REPLY (ver) ── │
  │── SET_CLIENT_NAME (proplist) ──▶ │   payload: U32(CMD_SET_CLIENT_NAME) U32(tag) PROPLIST("application.name","ESPHome-PulseAudio") STRING_NULL
  │ ◀──────────────────── REPLY ─── │
  │── GET_SINK_INFO (@DEFAULT_SINK@)▶│   payload: U32(CMD_GET_SINK_INFO) U32(tag) U32(0xFFFFFFFF) STR("@DEFAULT_SINK@")
  │ ◀── REPLY (ch, volume, muted) ── │   → parse_sink_info_reply_
  │                                  │     index(u32) name(str) desc(str) sample_spec(a) channel_map(m) module_idx(u32) cvolume(v) muted(bool)
  │── SET_SINK_VOLUME ──────────────▶│   U32(CMD_SET_SINK_VOLUME) U32(tag) U32(sink_index_) STR_NULL CVOLUME(channels, vol)
  │── SET_SINK_MUTE ────────────────▶│   U32(CMD_SET_SINK_MUTE)   U32(tag) U32(sink_index_) STR_NULL BOOL(muted)
```

- Cookie is 256 zero bytes for anonymous auth (server must have `auth-anonymous=true`)
- With cookie auth: decode 512 hex chars from YAML config into 256 bytes
- Volume conversion: `pa_vol = (uint32_t)((pct / 100.0f) * 65536)`
- Reverse: `pct = (float)((uint64_t)avg_vol * 100 / 65536)`
- For multi-channel: average all channel volumes when reading, set all to same value when writing
- Socket is non-blocking (O_NONBLOCK), recv uses MSG_DONTWAIT
- **`@DEFAULT_SINK@` works for GET_SINK_INFO but NOT for SET commands** — pipewire-pulse returns
  `PA_ERR_NOENTITY (code 5)`. Store `sink_index_` from the GET reply and use it for all SET commands.

### State machine

```
Disconnected → Authenticating → SettingClientName → GettingSinkInfo → Ready
```

- Each state sends one command and stores `wait_tag_` = seq_tag_
- On REPLY: verify `tag == wait_tag_`, advance state, send next command
- On ERROR: log code, if not Ready → disconnect and retry
- On Ready: process SET_VOLUME / SET_MUTE commands from HA

---

## File 1: `components/pulseaudio/__init__.py`

ESPHome codegen. Key points:
- `DEPENDENCIES = ["network"]`, `AUTO_LOAD = ["number", "switch"]`
- Classes: `PulseAudioComponent(Component)`, `PulseAudioVolumeNumber(Number)`, `PulseAudioMuteSwitch(Switch)`
- Config schema: `host` (required string), `port` (optional, default 4713), `cookie` (optional, 512 hex chars validated), `volume`, `mute`
- Cookie validation: strip spaces/colons, lowercase, must be exactly 512 hex chars
- `to_code`: register component, call `set_host`, `set_port`, optionally `set_cookie_hex`
- Volume number: `new_number(config, min_value=0, max_value=100, step=1)` then `var.set_volume_number(vol)`
- Mute switch: `new_switch(config)` then `var.set_mute_switch(mute)`

**ESPHome 2026.3.0+ API requirements (these replaced older APIs that no longer exist):**
- `CONF_HOST`, `CONF_PORT` removed from `esphome.const` → define locally as `"host"`, `"port"`
- `ICON_*`, `UNIT_*`, `DEVICE_CLASS_*` removed → don't import them (unused anyway)
- `cg.Nameable` removed from `esphome.codegen` → omit it, `Component` is sufficient
- `number.NUMBER_SCHEMA` / `switch.SWITCH_SCHEMA` removed → use `number.number_schema(ClassName)` / `switch.switch_schema(ClassName)`

```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number, switch
from esphome.const import CONF_ID

CONF_HOST = "host"   # removed from esphome.const in 2026.3.0
CONF_PORT = "port"   # removed from esphome.const in 2026.3.0

CODEOWNERS = ["@you"]
DEPENDENCIES = ["network"]
AUTO_LOAD = ["number", "switch"]

pulseaudio_ns = cg.esphome_ns.namespace("pulseaudio")
PulseAudioComponent = pulseaudio_ns.class_("PulseAudioComponent", cg.Component)  # Nameable removed in 2026.3.0
PulseAudioVolumeNumber = pulseaudio_ns.class_("PulseAudioVolumeNumber", number.Number)
PulseAudioMuteSwitch = pulseaudio_ns.class_("PulseAudioMuteSwitch", switch.Switch)

CONF_VOLUME = "volume"
CONF_MUTE   = "mute"
CONF_COOKIE = "cookie"

def validate_cookie(value):
    value = cv.string(value)
    value = value.replace(" ", "").replace(":", "").lower()
    if len(value) != 512:
        raise cv.Invalid(f"cookie must be exactly 512 hex characters (256 bytes), got {len(value)}")
    try:
        bytes.fromhex(value)
    except ValueError:
        raise cv.Invalid("cookie must contain only hex characters (0-9, a-f)")
    return value

# number_schema()/switch_schema() replace NUMBER_SCHEMA/SWITCH_SCHEMA removed in 2026.3.0
VOLUME_SCHEMA = number.number_schema(PulseAudioVolumeNumber)
MUTE_SCHEMA   = switch.switch_schema(PulseAudioMuteSwitch)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(PulseAudioComponent),
    cv.Required(CONF_HOST): cv.string,
    cv.Optional(CONF_PORT, default=4713): cv.port,
    cv.Optional(CONF_COOKIE): validate_cookie,
    cv.Optional(CONF_VOLUME): VOLUME_SCHEMA,
    cv.Optional(CONF_MUTE):   MUTE_SCHEMA,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_port(config[CONF_PORT]))
    if CONF_COOKIE in config:
        cg.add(var.set_cookie_hex(config[CONF_COOKIE]))
    if CONF_VOLUME in config:
        vol = await number.new_number(config[CONF_VOLUME], min_value=0, max_value=100, step=1)
        cg.add(var.set_volume_number(vol))
    if CONF_MUTE in config:
        mute = await switch.new_switch(config[CONF_MUTE])
        cg.add(var.set_mute_switch(mute))
```

---

## File 2: `components/pulseaudio/pulseaudio.h`

C++ header. Contains:

### PktBuf (packet builder, stack-allocated, 600-byte cap)
- `putRawU8(u8)`, `putRawU32(u32)` — raw bytes, big-endian
- `putU32(u32)` — TAG_U32 + raw u32
- `putBool(bool)` — TAG_BOOL_TRUE or TAG_BOOL_FALSE
- `putStr(const char*)` — TAG_STRING + null-terminated bytes
- `putStrNull()` — TAG_STRING_NULL
- `putArbitrary(buf, n)` — TAG_ARBITRARY + u32 len + raw bytes
- `putProplistStr(key, value)` — TAG_PROPLIST + str(key) + arbitrary(value+null) + STRING_NULL
- `putCVolume(channels, vol)` — TAG_CVOLUME + u8(n) + n * u32(vol)

### TagReader (packet reader)
- `expectU32()` — reads TAG_U32 byte, returns raw u32
- `readBool()` — reads tag byte, returns true if TAG_BOOL_TRUE
- `skipStr()` — skips TAG_STRING (reads until null) or handles TAG_STRING_NULL
- `readSampleSpec()` — reads TAG_SAMPLE_SPEC, returns channel count (skips format byte and rate)
- `skipChannelMap()` — reads TAG_CHANNEL_MAP, skips n position bytes
- `readCVolume()` — reads TAG_CVOLUME, averages all n channel volumes, returns average

### PAState enum
`Disconnected, Authenticating, SettingClientName, GettingSinkInfo, Ready`

### PulseAudioComponent : public Component
- `setup_priority`: AFTER_WIFI
- Members: `host_`, `port_`, `cookie_hex_` (nullable, 512 hex chars), `sock_`, `state_`, `seq_tag_`, `wait_tag_`, `channels_`, `volume_`, `muted_`, `want_volume_`/`want_volume_val_`, `want_mute_`/`want_mute_val_`, `rx_[4096]`, `rx_len_`, `volume_number_*`, `mute_switch_*`
- Public: `set_volume(float pct)`, `set_mute(bool muted)`, `is_ready()`
- Protected: `connect_()`, `disconnect_()`, `send_auth_()`, `send_set_client_name_()`, `send_get_sink_info_()`, `send_set_volume_()`, `send_set_mute_()`, `send_packet_(PktBuf&)`, `drain_rx_()`, `process_packet_(payload, len)`, `parse_sink_info_reply_(payload, len)`, `next_tag_()`, `publish_states_()`

### PulseAudioVolumeNumber : public number::Number
- `control(float value)` calls `parent_->set_volume(value)`

### PulseAudioMuteSwitch : public switch_::Switch
- `write_state(bool state)` calls `parent_->set_mute(state)`

---

## File 3: `components/pulseaudio/pulseaudio.cpp`

### `setup()`
Set parent pointer on volume_number_ and mute_switch_.

### `loop()`
1. If Disconnected: call `connect_()`
2. Peek socket (MSG_PEEK|MSG_DONTWAIT): if `r==0` or error (not EAGAIN/EWOULDBLOCK): `disconnect_()`
3. Call `drain_rx_()`
4. If Ready: flush `want_volume_` then `want_mute_`

### `set_volume(float pct)` / `set_mute(bool muted)`
Clamp, set want flags. If already Ready: send immediately.

### `connect_()`
- `getaddrinfo` → `socket` → `fcntl(O_NONBLOCK)` → `connect` (expect EINPROGRESS)
- `select` with 3s timeout on write-fd
- `getsockopt(SO_ERROR)` to verify
- On success: `state_ = Authenticating`, `send_auth_()`

### `disconnect_()`
`close(sock_)`, reset sock_/-1, rx_len_=0, state_=Disconnected, seq_tag_=0

### `send_auth_()`
- Decode `cookie_hex_` (512 hex chars) into 256 bytes, or use 256 zeros
- Build: `U32(CMD_AUTH)` `U32(tag)` `U32(PA_PROTOCOL_VERSION)` `ARB(256, cookie)`

### `send_set_client_name_()`
- Build: `U32(CMD_SET_CLIENT_NAME)` `U32(tag)` `PROPLIST("application.name","ESPHome-PulseAudio")` `STRING_NULL`

### `send_get_sink_info_()`
- Build: `U32(CMD_GET_SINK_INFO)` `U32(tag)` `U32(0xFFFFFFFF)` `STR("@DEFAULT_SINK@")`

### `send_set_volume_()`
- `vol = (uint32_t)((volume_ / 100.0f) * PA_VOLUME_NORM)`
- Build: `U32(CMD_SET_SINK_VOLUME)` `U32(tag)` `U32(sink_index_)` `STR_NULL` `CVOLUME(channels_, vol)`
- **Use `sink_index_` (from GET_SINK_INFO reply) + null string, NOT `PA_INVALID_INDEX` + `"@DEFAULT_SINK@"`**
  — pipewire-pulse does not resolve `@DEFAULT_SINK@` alias in SET commands (returns PA_ERR_NOENTITY)
- `publish_state(volume_)` on volume_number_

### `send_set_mute_()`
- Build: `U32(CMD_SET_SINK_MUTE)` `U32(tag)` `U32(sink_index_)` `STR_NULL` `BOOL(muted_)`
- Same note: use real `sink_index_`, not `@DEFAULT_SINK@`
- `publish_state(muted_)` on mute_switch_

### `send_packet_(PktBuf&)`
- Build 20-byte descriptor: bytes 0-3 = payload len BE, bytes 4-7 = 0xFF 0xFF 0xFF 0xFF, rest = 0
- `send(sock_, desc, 20, 0)` then `send(sock_, pkt.data, pkt.len, 0)`

### `drain_rx_()`
- `recv` loop with MSG_DONTWAIT until exhausted, accumulate into `rx_[4096]`
- Inner loop: need ≥20 bytes for header; read payload_len from rx_[0..3] BE
- If complete packet: `process_packet_(rx_+20, payload_len)`, then **check `state_ == Disconnected` before memmove** — `process_packet_` can call `disconnect_()` which resets `rx_len_=0`, and `rx_len_ - total` would unsigned-underflow to ~4 billion, crashing memmove. Break the loop if disconnected.

### `process_packet_(payload, len)`
- `TagReader r`; read `cmd` and `tag` via `expectU32()`
- If CMD_ERROR: read error code, log, if not Ready → disconnect_()
- If CMD_REPLY: dispatch by state, match `tag == wait_tag_`
  - Authenticating → SettingClientName → send_set_client_name_()
  - SettingClientName → GettingSinkInfo → send_get_sink_info_()
  - GettingSinkInfo → parse_sink_info_reply_ → Ready + publish_states_() (or retry)
  - Ready: no-op (ACKs for SET commands)

### `parse_sink_info_reply_(payload, len)`
- `sink_index_ = expectU32()` — **store the real sink index** for use in SET commands
- `skipStr()` (name), `skipStr()` (description)
- `channels_ = readSampleSpec()` — validate 1–32
- `skipChannelMap()`, `expectU32()` (module index)
- `avg_vol = readCVolume()` → `volume_ = (float)((uint64_t)avg_vol * 100 / PA_VOLUME_NORM)`
- `muted_ = readBool()`
- Return `r.ok()`

---

## File 4: `server/pipewire-pulse-tcp.conf`

Drop into `~/.config/pipewire/pipewire-pulse.conf.d/tcp.conf`.
Then `systemctl --user restart pipewire-pulse`.

```
# Mode A: Cookie auth (recommended)
pulse.modules = [
  {
    name = module-native-protocol-tcp
    args = {
      port        = 4713
      auth-ip-acl = 127.0.0.1;192.168.0.0/16;10.0.0.0/8;172.16.0.0/12
    }
  }
]

# Mode B: Anonymous auth (uncomment to use instead)
# pulse.modules = [
#   {
#     name = module-native-protocol-tcp
#     args = {
#       port            = 4713
#       auth-anonymous  = true
#       auth-ip-acl     = 127.0.0.1;192.168.0.0/16;10.0.0.0/8;172.16.0.0/12
#     }
#   }
# ]
```

---

## File 5: `pipewire-control.yaml` (example ESPHome YAML)

```yaml
substitutions:
  device_name: pipewire-control

esphome:
  name: ${device_name}

esp32:
  board: esp32dev
  framework:
    type: esp-idf

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ap:
    ssid: "${device_name} Fallback"
    password: !secret ap_password

logger:
api:
ota:
  platform: esphome

external_components:
  - source:
      type: local
      path: components

pulseaudio:
  host: "192.168.1.100"
  port: 4713
  # cookie: !secret pa_cookie   # omit for anonymous auth
  volume:
    name: "Living Room Volume"
    icon: mdi:volume-high
  mute:
    name: "Living Room Mute"
    icon: mdi:volume-off

sensor:
  - platform: rotary_encoder
    name: "Volume Encoder"
    pin_a: GPIO18
    pin_b: GPIO19
    resolution: 1
    on_clockwise:
      - number.increment:
          id: living_room_volume
          cycle: false
    on_counter_clockwise:
      - number.decrement:
          id: living_room_volume
          cycle: false

binary_sensor:
  - platform: gpio
    pin:
      number: GPIO21
      mode: INPUT_PULLUP
      inverted: true
    name: "Mute Button"
    filters:
      - delayed_on: 10ms
    on_press:
      - switch.toggle: living_room_mute
```

Note: The `volume` and `mute` entities need `id:` fields if referenced elsewhere (e.g. `living_room_volume`, `living_room_mute`).

---

## Hardware wiring

```
ESP32 GPIO18 ──── Encoder CLK (A)
ESP32 GPIO19 ──── Encoder DT  (B)
ESP32 GPIO21 ──── Mute button (other leg → GND)
ESP32 GND    ──── Encoder GND, button GND
ESP32 3V3    ──── Encoder VCC (if module needs it)
```

Internal pull-up used for button (INPUT_PULLUP), no external resistor needed.

---

## Cookie extraction (for cookie auth mode)

```bash
# On the Linux host, export the 256-byte cookie as hex:
xxd -p ~/.config/pulse/cookie | tr -d '\n'
# Outputs 512 hex chars — paste into secrets.yaml as pa_cookie
```

---

## Troubleshooting quick-reference

| Symptom | Cause | Fix |
|---------|-------|-----|
| `PA error code=4` | Cookie mismatch | Re-extract cookie |
| `PA error code=5` | IP not in ACL | Add ESP32 IP/subnet to auth-ip-acl |
| DNS lookup failed | Wrong host value | Use IP address |
| Stuck in GettingSinkInfo | No default sink | Check `pactl info` on host |
| Volume jumps | resolution too high | Set `resolution: 1` |

---

## Key design decisions

1. **No server-side software**: speaks PA native protocol directly to pipewire-pulse
2. **Non-blocking I/O**: `O_NONBLOCK` + `MSG_DONTWAIT` — never blocks ESPHome loop()
3. **Want flags**: `want_volume_` / `want_mute_` queue commands if not yet Ready; sent in loop()
4. **Anonymous auth**: zero cookie works when `auth-anonymous=true` on server
5. **Sink index for SET commands**: `@DEFAULT_SINK@` is resolved only for `GET_SINK_INFO`. Store the returned `sink_index_` and use it with a null name for all `SET_SINK_VOLUME` / `SET_SINK_MUTE` commands — pipewire-pulse returns `PA_ERR_NOENTITY` otherwise.
6. **Channel preservation**: reads channel count from sink info, uses it for SET_SINK_VOLUME
7. **PktBuf capacity 600 bytes**: sufficient for all packets (auth packet is largest at ~280 bytes)
8. **RX buffer 4096 bytes**: sufficient for sink info reply which can be ~500 bytes
9. **Disconnect guard in drain_rx_**: after `process_packet_()`, check `state_ == Disconnected` before memmove — `disconnect_()` resets `rx_len_=0` and the unsigned subtraction would underflow

## Known bugs fixed (lessons learned)

| Bug | Symptom | Root cause | Fix |
|-----|---------|------------|-----|
| Proplist missing length tag | `PA_ERR_PROTOCOL (7)` on `SET_CLIENT_NAME` | Proplist value wire format is `U32(len) + ARBITRARY(len, data)`, not just `ARBITRARY` | Add `putU32(vlen)` before `putArbitrary()` in `putProplistStr` |
| memmove crash after disconnect | `LoadStoreError` on boot | `process_packet_` calls `disconnect_()` → `rx_len_=0`; `rx_len_ - total` underflows | Check `state_ == Disconnected` before memmove |
| Wrong SET command codes | `PA_ERR_PROTOCOL (7)` on SET_SINK_VOLUME | SET_SINK_VOLUME=36, SET_SINK_MUTE=39 (not 37/40) | Correct constants |
| `@DEFAULT_SINK@` in SET commands | `PA_ERR_NOENTITY (5)` on SET_SINK_MUTE/VOLUME | pipewire-pulse only resolves alias in GET, not SET | Store real `sink_index_` from GET reply |
