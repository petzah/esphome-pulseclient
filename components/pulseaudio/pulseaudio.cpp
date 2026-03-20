#include "pulseaudio.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

namespace esphome {
namespace pulseaudio {

static const char *TAG = "pulseaudio";

// ── Component lifecycle ───────────────────────────────────────────────────────

void PulseAudioComponent::setup() {
    if (volume_number_) volume_number_->set_parent(this);
    if (mute_switch_)   mute_switch_->set_parent(this);
}

void PulseAudioComponent::loop() {
    if (state_ == PAState::Disconnected) {
        connect_();
        return;
    }

    // Check if socket is still alive (non-blocking peek)
    char peek;
    int r = recv(sock_, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
    if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        ESP_LOGW(TAG, "Connection lost, reconnecting");
        disconnect_();
        return;
    }

    drain_rx_();

    if (state_ == PAState::Ready) {
        if (want_volume_) { want_volume_ = false; send_set_volume_(); }
        if (want_mute_)   { want_mute_   = false; send_set_mute_();   }
    }
}

// ── Public command API ────────────────────────────────────────────────────────

void PulseAudioComponent::set_volume(float pct) {
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    want_volume_     = true;
    want_volume_val_ = pct;
    if (state_ == PAState::Ready) { want_volume_ = false; send_set_volume_(); }
}

void PulseAudioComponent::set_mute(bool muted) {
    want_mute_     = true;
    want_mute_val_ = muted;
    if (state_ == PAState::Ready) { want_mute_ = false; send_set_mute_(); }
}

// ── Connection ────────────────────────────────────────────────────────────────

void PulseAudioComponent::connect_() {
    ESP_LOGI(TAG, "Connecting to %s:%u", host_, port_);

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port_);

    if (getaddrinfo(host_, port_str, &hints, &res) != 0 || res == nullptr) {
        ESP_LOGW(TAG, "DNS lookup failed, retrying in 5 s");
        return;
    }

    sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock_ < 0) { freeaddrinfo(res); return; }

    // Non-blocking connect
    fcntl(sock_, F_SETFL, fcntl(sock_, F_GETFL, 0) | O_NONBLOCK);

    int cr = ::connect(sock_, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (cr < 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "connect() failed: %d", errno);
        disconnect_();
        return;
    }

    // Wait up to 3 s for connection
    fd_set wfds;
    FD_ZERO(&wfds); FD_SET(sock_, &wfds);
    struct timeval tv{3, 0};
    if (select(sock_ + 1, nullptr, &wfds, nullptr, &tv) <= 0) {
        ESP_LOGW(TAG, "connect() timed out");
        disconnect_();
        return;
    }

    int err = 0; socklen_t elen = sizeof(err);
    getsockopt(sock_, SOL_SOCKET, SO_ERROR, &err, &elen);
    if (err != 0) {
        ESP_LOGW(TAG, "connect() socket error: %d", err);
        disconnect_();
        return;
    }

    ESP_LOGI(TAG, "Connected — authenticating");
    rx_len_ = 0;
    seq_tag_ = 0;
    state_ = PAState::Authenticating;
    send_auth_();
}

void PulseAudioComponent::disconnect_() {
    if (sock_ >= 0) { close(sock_); sock_ = -1; }
    rx_len_ = 0;
    state_  = PAState::Disconnected;
    seq_tag_ = 0;
}

// ── Send helpers ──────────────────────────────────────────────────────────────

// Decode one hex nibble ('0'-'9', 'a'-'f') → 0-15.
static inline uint8_t hex_nibble(char c) {
    return (c >= '0' && c <= '9') ? (c - '0') : (c - 'a' + 10);
}

void PulseAudioComponent::send_auth_() {
    uint8_t cookie[PA_COOKIE_LENGTH]{};   // zero-initialised (anonymous fallback)

    if (cookie_hex_ != nullptr) {
        // Decode 512 hex chars → 256 bytes
        for (uint32_t i = 0; i < PA_COOKIE_LENGTH; i++) {
            cookie[i] = (hex_nibble(cookie_hex_[i * 2]) << 4) |
                         hex_nibble(cookie_hex_[i * 2 + 1]);
        }
        ESP_LOGD(TAG, "Authenticating with cookie");
    } else {
        ESP_LOGD(TAG, "Authenticating anonymously (no cookie set)");
    }

    PktBuf pkt;
    pkt.putU32(PA_CMD_AUTH);
    pkt.putU32(next_tag_());
    wait_tag_ = seq_tag_;
    pkt.putU32(PA_PROTOCOL_VERSION);
    pkt.putArbitrary(cookie, PA_COOKIE_LENGTH);
    send_packet_(pkt);
}

void PulseAudioComponent::send_set_client_name_() {
    PktBuf pkt;
    pkt.putU32(PA_CMD_SET_CLIENT_NAME);
    pkt.putU32(next_tag_());
    wait_tag_ = seq_tag_;
    pkt.putProplistStr("application.name", "ESPHome-PulseAudio");
    send_packet_(pkt);
}

void PulseAudioComponent::send_get_sink_info_() {
    PktBuf pkt;
    pkt.putU32(PA_CMD_GET_SINK_INFO);
    pkt.putU32(next_tag_());
    wait_tag_ = seq_tag_;
    pkt.putU32(PA_INVALID_INDEX);
    pkt.putStr("@DEFAULT_SINK@");
    send_packet_(pkt);
}

void PulseAudioComponent::send_set_volume_() {
    volume_ = want_volume_val_;
    uint32_t vol = (uint32_t)((volume_ / 100.0f) * PA_VOLUME_NORM);

    PktBuf pkt;
    pkt.putU32(PA_CMD_SET_SINK_VOLUME);
    pkt.putU32(next_tag_());
    pkt.putU32(PA_INVALID_INDEX);
    pkt.putStr("@DEFAULT_SINK@");
    pkt.putCVolume(channels_, vol);
    send_packet_(pkt);

    ESP_LOGI(TAG, "SET_VOLUME %.0f%% (%u PA units, %u ch)", volume_, vol, channels_);
    if (volume_number_) volume_number_->publish_state(volume_);
}

void PulseAudioComponent::send_set_mute_() {
    muted_ = want_mute_val_;

    PktBuf pkt;
    pkt.putU32(PA_CMD_SET_SINK_MUTE);
    pkt.putU32(next_tag_());
    pkt.putU32(PA_INVALID_INDEX);
    pkt.putStr("@DEFAULT_SINK@");
    pkt.putBool(muted_);
    send_packet_(pkt);

    ESP_LOGI(TAG, "SET_MUTE %s", muted_ ? "ON" : "OFF");
    if (mute_switch_) mute_switch_->publish_state(muted_);
}

// ── Framing ───────────────────────────────────────────────────────────────────

void PulseAudioComponent::send_packet_(PktBuf &pkt) {
    uint8_t desc[20]{};
    uint32_t len = pkt.len;
    desc[0] = len >> 24; desc[1] = len >> 16; desc[2] = len >> 8; desc[3] = len;
    desc[4] = 0xFF; desc[5] = 0xFF; desc[6] = 0xFF; desc[7] = 0xFF;
    send(sock_, desc, 20, 0);
    send(sock_, pkt.data, pkt.len, 0);
}

// ── Receive ───────────────────────────────────────────────────────────────────

void PulseAudioComponent::drain_rx_() {
    // Read all available bytes (socket is non-blocking)
    while (rx_len_ < RX_CAP) {
        int n = recv(sock_, rx_ + rx_len_, RX_CAP - rx_len_, MSG_DONTWAIT);
        if (n <= 0) break;
        rx_len_ += n;
    }

    // Dispatch complete packets
    while (true) {
        if (rx_len_ < 20) break;

        uint32_t payload_len = ((uint32_t)rx_[0] << 24) | ((uint32_t)rx_[1] << 16) |
                               ((uint32_t)rx_[2] <<  8) |  (uint32_t)rx_[3];

        if (payload_len > RX_CAP - 20) {
            ESP_LOGE(TAG, "Oversized packet (%u), resetting", payload_len);
            rx_len_ = 0;
            break;
        }
        if (rx_len_ < 20 + payload_len) break;

        process_packet_(rx_ + 20, payload_len);

        if (state_ == PAState::Disconnected) break;   // disconnect_() reset rx_len_

        uint32_t total = 20 + payload_len;
        memmove(rx_, rx_ + total, rx_len_ - total);
        rx_len_ -= total;
    }
}

bool PulseAudioComponent::process_packet_(const uint8_t *payload, uint32_t len) {
    if (len < 8) return false;

    TagReader r(payload, len);
    uint32_t cmd = r.expectU32();
    uint32_t tag = r.expectU32();

    if (cmd == PA_CMD_ERROR) {
        uint32_t code = r.expectU32();
        ESP_LOGE(TAG, "PA error tag=%u code=%u", tag, code);
        if (state_ != PAState::Ready) disconnect_();
        return false;
    }

    if (cmd != PA_CMD_REPLY) return true;

    switch (state_) {
    case PAState::Authenticating:
        if (tag != wait_tag_) break;
        ESP_LOGI(TAG, "Auth OK");
        state_ = PAState::SettingClientName;
        send_set_client_name_();
        break;

    case PAState::SettingClientName:
        if (tag != wait_tag_) break;
        ESP_LOGI(TAG, "Client name set");
        state_ = PAState::GettingSinkInfo;
        send_get_sink_info_();
        break;

    case PAState::GettingSinkInfo:
        if (tag != wait_tag_) break;
        if (parse_sink_info_reply_(payload + r.pos, len - r.pos)) {
            ESP_LOGI(TAG, "Ready — %u ch, vol %.0f%%, mute=%s",
                     channels_, volume_, muted_ ? "on" : "off");
            state_ = PAState::Ready;
            publish_states_();
        } else {
            ESP_LOGW(TAG, "Failed to parse sink info, retrying");
            send_get_sink_info_();
        }
        break;

    case PAState::Ready:
        break;  // ACK for SET commands — nothing to do

    default:
        break;
    }
    return true;
}

bool PulseAudioComponent::parse_sink_info_reply_(const uint8_t *payload, uint32_t len) {
    TagReader r(payload, len);

    r.expectU32();   // sink index
    r.skipStr();     // name
    r.skipStr();     // description

    uint8_t ch = r.readSampleSpec();
    if (ch == 0 || ch > 32) { ESP_LOGE(TAG, "Bad channel count: %u", ch); return false; }
    channels_ = ch;

    r.skipChannelMap();
    r.expectU32();   // module index

    uint32_t avg_vol = r.readCVolume();
    volume_ = (float)((uint64_t)avg_vol * 100 / PA_VOLUME_NORM);

    muted_ = r.readBool();

    return r.ok();
}

void PulseAudioComponent::publish_states_() {
    if (volume_number_) volume_number_->publish_state(volume_);
    if (mute_switch_)   mute_switch_->publish_state(muted_);
}

}  // namespace pulseaudio
}  // namespace esphome
