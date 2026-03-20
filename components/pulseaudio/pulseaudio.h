#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/number/number.h"
#include "esphome/components/switch/switch.h"

#include <lwip/sockets.h>
#include <lwip/netdb.h>

namespace esphome {
namespace pulseaudio {

// ── Protocol constants ────────────────────────────────────────────────────────

static constexpr uint32_t PA_PROTOCOL_VERSION = 35;
static constexpr uint32_t PA_INVALID_INDEX    = 0xFFFFFFFFU;
static constexpr uint32_t PA_VOLUME_NORM      = 0x10000U;
static constexpr uint32_t PA_COOKIE_LENGTH    = 256;

static constexpr uint32_t PA_CMD_REPLY            = 2;
static constexpr uint32_t PA_CMD_ERROR            = 0;
static constexpr uint32_t PA_CMD_AUTH             = 8;
static constexpr uint32_t PA_CMD_SET_CLIENT_NAME  = 9;
static constexpr uint32_t PA_CMD_GET_SINK_INFO    = 21;
static constexpr uint32_t PA_CMD_SET_SINK_VOLUME  = 37;
static constexpr uint32_t PA_CMD_SET_SINK_MUTE    = 40;

static constexpr uint8_t PA_TAG_U32         = 'L';
static constexpr uint8_t PA_TAG_ARBITRARY   = 'x';
static constexpr uint8_t PA_TAG_STRING      = 't';
static constexpr uint8_t PA_TAG_STRING_NULL = 'N';
static constexpr uint8_t PA_TAG_BOOL_TRUE   = '1';
static constexpr uint8_t PA_TAG_BOOL_FALSE  = '0';
static constexpr uint8_t PA_TAG_SAMPLE_SPEC = 'a';
static constexpr uint8_t PA_TAG_CHANNEL_MAP = 'm';
static constexpr uint8_t PA_TAG_CVOLUME     = 'v';
static constexpr uint8_t PA_TAG_PROPLIST    = 'P';

// ── Packet builder ────────────────────────────────────────────────────────────

struct PktBuf {
    static constexpr size_t CAP = 600;
    uint8_t  data[CAP];
    uint32_t len = 0;

    void reset() { len = 0; }

    void putRawU8(uint8_t v)  { if (len < CAP) data[len++] = v; }
    void putRawU32(uint32_t v) {
        putRawU8(v >> 24); putRawU8(v >> 16); putRawU8(v >> 8); putRawU8(v);
    }

    void putU32(uint32_t v) { putRawU8(PA_TAG_U32); putRawU32(v); }
    void putBool(bool v)    { putRawU8(v ? PA_TAG_BOOL_TRUE : PA_TAG_BOOL_FALSE); }

    void putStr(const char *s) {
        putRawU8(PA_TAG_STRING);
        while (*s) putRawU8((uint8_t)*s++);
        putRawU8(0);
    }
    void putStrNull() { putRawU8(PA_TAG_STRING_NULL); }

    void putArbitrary(const uint8_t *buf, uint32_t n) {
        putRawU8(PA_TAG_ARBITRARY);
        putRawU32(n);
        for (uint32_t i = 0; i < n && len < CAP; i++) data[len++] = buf[i];
    }

    void putProplistStr(const char *key, const char *value) {
        putRawU8(PA_TAG_PROPLIST);
        putStr(key);
        uint32_t vlen = strlen(value) + 1;
        putU32(vlen);                              // length before the arbitrary blob
        putArbitrary((const uint8_t *)value, vlen);
        putStrNull();
    }

    void putCVolume(uint8_t ch, uint32_t vol) {
        putRawU8(PA_TAG_CVOLUME);
        putRawU8(ch);
        for (uint8_t i = 0; i < ch; i++) putRawU32(vol);
    }
};

// ── Tag reader ────────────────────────────────────────────────────────────────

struct TagReader {
    const uint8_t *data;
    uint32_t       len;
    uint32_t       pos = 0;

    TagReader(const uint8_t *d, uint32_t l) : data(d), len(l) {}

    bool ok() const { return pos <= len; }

    uint8_t readRawU8() { return (pos < len) ? data[pos++] : 0; }
    uint32_t readRawU32() {
        if (pos + 4 > len) { pos = len; return 0; }
        uint32_t v = ((uint32_t)data[pos] << 24) | ((uint32_t)data[pos+1] << 16) |
                     ((uint32_t)data[pos+2] << 8)  |  (uint32_t)data[pos+3];
        pos += 4;
        return v;
    }

    uint32_t expectU32() { return readRawU8() == PA_TAG_U32 ? readRawU32() : 0; }
    bool     readBool()  { return readRawU8() == PA_TAG_BOOL_TRUE; }

    void skipStr() {
        uint8_t t = readRawU8();
        if (t == PA_TAG_STRING) { while (pos < len && data[pos++] != 0); }
    }

    uint8_t readSampleSpec() {
        if (readRawU8() != PA_TAG_SAMPLE_SPEC) return 0;
        readRawU8();        // format
        uint8_t ch = readRawU8();
        readRawU32();       // rate
        return ch;
    }

    void skipChannelMap() {
        if (readRawU8() != PA_TAG_CHANNEL_MAP) return;
        uint8_t n = readRawU8();
        pos += n;
    }

    uint32_t readCVolume() {
        if (readRawU8() != PA_TAG_CVOLUME) return PA_VOLUME_NORM;
        uint8_t n = readRawU8();
        if (n == 0) return PA_VOLUME_NORM;
        uint64_t sum = 0;
        for (uint8_t i = 0; i < n; i++) sum += readRawU32();
        return (uint32_t)(sum / n);
    }
};

// ── Forward declarations ──────────────────────────────────────────────────────

class PulseAudioVolumeNumber;
class PulseAudioMuteSwitch;

// ── Main component ────────────────────────────────────────────────────────────

enum class PAState : uint8_t {
    Disconnected,
    Authenticating,
    SettingClientName,
    GettingSinkInfo,
    Ready,
};

class PulseAudioComponent : public Component {
public:
    void set_host(const char *host)         { host_ = host; }
    void set_port(uint16_t port)            { port_ = port; }
    // 512-char lowercase hex string (256 bytes).  If not set, zeros are sent
    // and the server must have auth-anonymous=true.
    void set_cookie_hex(const char *hex)    { cookie_hex_ = hex; }
    void set_volume_number(PulseAudioVolumeNumber *n) { volume_number_ = n; }
    void set_mute_switch(PulseAudioMuteSwitch *s)     { mute_switch_ = s; }

    void setup() override;
    void loop() override;
    float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

    // Called by Number/Switch entities when HA sends a command
    void set_volume(float pct);
    void set_mute(bool muted);

    bool is_ready() const { return state_ == PAState::Ready; }

protected:
    const char *host_       = nullptr;
    uint16_t    port_       = 4713;
    const char *cookie_hex_ = nullptr;   // 512 hex chars = 256 bytes; nullptr → zeros
    int         sock_       = -1;
    PAState     state_ = PAState::Disconnected;
    uint32_t    seq_tag_ = 0;
    uint32_t    wait_tag_ = 0;

    uint8_t  channels_ = 2;
    float    volume_   = 50.0f;   // 0-100
    bool     muted_    = false;

    bool     want_volume_     = false;
    float    want_volume_val_ = 0;
    bool     want_mute_       = false;
    bool     want_mute_val_   = false;

    static constexpr size_t RX_CAP = 4096;
    uint8_t  rx_[RX_CAP];
    uint32_t rx_len_ = 0;

    PulseAudioVolumeNumber *volume_number_ = nullptr;
    PulseAudioMuteSwitch   *mute_switch_  = nullptr;

    void connect_();
    void disconnect_();

    void send_auth_();
    void send_set_client_name_();
    void send_get_sink_info_();
    void send_set_volume_();
    void send_set_mute_();

    void send_packet_(PktBuf &pkt);
    void drain_rx_();
    bool process_packet_(const uint8_t *payload, uint32_t len);
    bool parse_sink_info_reply_(const uint8_t *payload, uint32_t len);

    uint32_t next_tag_() { return ++seq_tag_; }

    void publish_states_();
};

// ── Number entity (volume) ────────────────────────────────────────────────────

class PulseAudioVolumeNumber : public number::Number {
public:
    void set_parent(PulseAudioComponent *p) { parent_ = p; }

protected:
    void control(float value) override {
        if (parent_) parent_->set_volume(value);
    }
    PulseAudioComponent *parent_ = nullptr;
};

// ── Switch entity (mute) ──────────────────────────────────────────────────────

class PulseAudioMuteSwitch : public switch_::Switch {
public:
    void set_parent(PulseAudioComponent *p) { parent_ = p; }

protected:
    void write_state(bool state) override {
        if (parent_) parent_->set_mute(state);
    }
    PulseAudioComponent *parent_ = nullptr;
};

}  // namespace pulseaudio
}  // namespace esphome
