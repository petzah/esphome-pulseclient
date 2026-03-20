# ESPHome PulseAudio Component

Control the default audio sink of a PipeWire/PulseAudio server from an ESP32
over WiFi — no extra software on the server, no MQTT broker, no intermediary.

The ESP32 speaks the native PulseAudio binary protocol (TCP port 4713) directly
to `pipewire-pulse`, and exposes a **Number** (volume 0–100 %) and a **Switch**
(mute) to Home Assistant via the ESPHome API.

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

---

## Server setup

### 1. Enable TCP in pipewire-pulse

Copy the provided config file:

```bash
mkdir -p ~/.config/pipewire/pipewire-pulse.conf.d
cp server/pipewire-pulse-tcp.conf \
   ~/.config/pipewire/pipewire-pulse.conf.d/tcp.conf
```

Edit the file and choose an authentication mode (see [Authentication](#authentication)
below), then restart:

```bash
systemctl --user restart pipewire-pulse
```

Verify the module loaded:

```bash
pactl list modules short | grep native-protocol-tcp
# Expected output:
# 42   module-native-protocol-tcp   port=4713 auth-ip-acl=...
```

### 2. Allow the port through the firewall (if applicable)

```bash
# ufw example
sudo ufw allow from 192.168.0.0/16 to any port 4713 proto tcp
```

---

## Authentication

The PulseAudio native protocol supports two authentication modes for TCP
connections. Choose one and configure both the server config and the ESPHome
YAML to match.

### Mode A — Cookie authentication (recommended)

The server and client share a 256-byte secret cookie. This is the same
mechanism used by `pactl` and other local PA clients.

**Server config** (`tcp.conf`):
```
pulse.modules = [
  {
    name = module-native-protocol-tcp
    args = {
      port        = 4713
      auth-ip-acl = 127.0.0.1;192.168.0.0/16
    }
  }
]
```

**Getting the cookie from the host:**

```bash
# Hex-encode the 256-byte cookie file (outputs 512 hex chars, no spaces)
xxd -p ~/.config/pulse/cookie | tr -d '\n'
```

Copy the output and paste it into your ESPHome YAML:

```yaml
pulseaudio:
  host: "192.168.1.100"
  cookie: "1a2b3c...  # 512 hex chars"
```

> The cookie is stored in `~/.config/pulse/cookie`. If it does not exist yet,
> connect any PA client (e.g. `pactl info`) to generate it.

### Mode B — Anonymous authentication

No cookie required. Any client that can reach port 4713 is accepted.
Suitable for isolated home networks.

**Server config** (`tcp.conf`):
```
pulse.modules = [
  {
    name = module-native-protocol-tcp
    args = {
      port            = 4713
      auth-anonymous  = true
      auth-ip-acl     = 127.0.0.1;192.168.0.0/16
    }
  }
]
```

**ESPHome YAML** — omit the `cookie` key entirely:

```yaml
pulseaudio:
  host: "192.168.1.100"
  # no cookie: key → anonymous auth
```

---

## ESPHome configuration reference

### Component

```yaml
pulseaudio:
  host: "192.168.1.100"   # Required. IP or hostname of the PipeWire host.
  port: 4713              # Optional. Default: 4713.
  cookie: "<512 hex>"     # Optional. Omit for anonymous auth.
  volume:
    name: "Living Room Volume"
  mute:
    name: "Living Room Mute"
```

### `volume` entity (Number)

Exposes the master sink volume as a Home Assistant Number entity (0–100, step 1).
Supports all standard ESPHome Number options (`name`, `icon`, `id`, etc.).

### `mute` entity (Switch)

Exposes the master sink mute state as a Home Assistant Switch entity.
Supports all standard ESPHome Switch options.

---

## Full YAML example

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
  cookie: !secret pa_cookie   # paste 512-char hex here, or use secrets.yaml
  volume:
    name: "Living Room Volume"
    id: living_room_volume
    icon: mdi:volume-high
  mute:
    name: "Living Room Mute"
    id: living_room_mute
    icon: mdi:volume-off

# ── Rotary encoder → volume ────────────────────────────────────────────────────
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

# ── Mute button ────────────────────────────────────────────────────────────────
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

`secrets.yaml`:

```yaml
wifi_ssid: "MyNetwork"
wifi_password: "hunter2"
ap_password: "fallback123"
pa_cookie: "1a2b3c..."   # output of: xxd -p ~/.config/pulse/cookie | tr -d '\n'
```

---

## Hardware wiring

```
ESP32 GPIO18 ──── Encoder CLK (A)
ESP32 GPIO19 ──── Encoder DT  (B)
ESP32 GPIO21 ──── Mute button (other leg → GND)
ESP32 GND    ──── Encoder GND, button GND
ESP32 3V3    ──── Encoder VCC (if required by module)
```

Most rotary encoder modules already include a pull-up resistor on CLK and DT.
If using a bare encoder (no module), add 10 kΩ pull-ups to 3V3 on both pins.

The mute button uses the ESP32's internal pull-up (`INPUT_PULLUP`), so no
external resistor is required.

---

## Protocol handshake

The component implements a subset of the PulseAudio native binary protocol:

```
CLIENT                              SERVER
  │── AUTH (version=35, cookie) ──▶ │
  │ ◀─────────────── REPLY (ver) ── │
  │── SET_CLIENT_NAME (proplist) ──▶ │
  │ ◀──────────────────── REPLY ─── │
  │── GET_SINK_INFO (@DEFAULT_SINK@)▶│
  │ ◀── REPLY (ch, volume, muted) ── │  ← component enters Ready state,
  │                                  │    publishes initial HA states
  │── SET_SINK_VOLUME ──────────────▶│  ← on volume change
  │── SET_SINK_MUTE ────────────────▶│  ← on mute toggle
```

**Packet framing** — every packet is preceded by a 20-byte descriptor:

| Bytes | Field      | Value                      |
|-------|------------|----------------------------|
| 0–3   | length     | payload length (big-endian)|
| 4–7   | channel    | `0xFFFFFFFF` (control)     |
| 8–11  | offset\_hi | `0`                        |
| 12–15 | offset\_lo | `0`                        |
| 16–19 | flags      | `0`                        |

**Volume units** — PulseAudio uses `PA_VOLUME_NORM = 0x10000` (65536) for 100 %.
The component converts linearly: `pa_vol = (percent / 100) × 65536`.

---

## ESPHome version compatibility

This component requires **ESPHome 2026.3.0 or later**. The following ESPHome
API changes are already handled in the current codebase:

| Removed API | Replacement used |
|---|---|
| `CONF_HOST`, `CONF_PORT` from `esphome.const` | Defined locally as `"host"`, `"port"` |
| `ICON_*`, `UNIT_*`, `DEVICE_CLASS_*` from `esphome.const` | Removed (were unused) |
| `cg.Nameable` from `esphome.codegen` | Removed (not needed for this component) |
| `number.NUMBER_SCHEMA` / `switch.SWITCH_SCHEMA` | `number.number_schema()` / `switch.switch_schema()` |

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `PA error code=4` on connect | Cookie mismatch | Re-extract cookie with `xxd -p ~/.config/pulse/cookie \| tr -d '\n'` |
| `PA error code=5` on connect | IP not in ACL | Add ESP32's IP or subnet to `auth-ip-acl` in `tcp.conf` |
| `DNS lookup failed` | Wrong `host` value | Use IP address instead of hostname, or ensure mDNS resolves |
| Component stuck in `GettingSinkInfo` | No default sink set | Run `pactl info` on host and confirm `Default Sink` is not empty |
| Volume jumps on encoder | `resolution` too high | Set `resolution: 1` in `rotary_encoder` |
| `Unable to import component` | ESPHome API changed | Ensure you are using the latest version from the repository |
