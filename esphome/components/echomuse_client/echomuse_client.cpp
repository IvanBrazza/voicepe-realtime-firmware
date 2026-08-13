#include "echomuse_client.h"
#include "automation.h"

#include "esphome/components/audio/audio.h"
#include "esphome/components/network/util.h"
#include "esphome/core/log.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <sstream>

#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_websocket_client.h>
#include <nvs.h>

namespace esphome {
namespace echomuse_client {

static const char *const TAG = "echomuse_client";
static constexpr size_t PLAYBACK_PRIME_BYTES = 48000 * 2;

static void echomuse_ws_event(void *arg, esp_event_base_t, int32_t event_id,
                              void *event_data) {
  auto *ctx = static_cast<EchoMuseClient::WsContext *>(arg);
  if (ctx != nullptr && ctx->self != nullptr)
    ctx->self->on_ws_event(ctx->control, event_id, event_data);
}

static bool nvs_read_string(nvs_handle_t handle, const char *key,
                            std::string &value) {
  size_t len = 0;
  if (nvs_get_str(handle, key, nullptr, &len) != ESP_OK || len == 0)
    return false;
  std::vector<char> buffer(len);
  if (nvs_get_str(handle, key, buffer.data(), &len) != ESP_OK)
    return false;
  value.assign(buffer.data());
  return true;
}

void EchoMuseClient::setup() {
  ESP_LOGCONFIG(TAG, "Setting up EchoMuse satellite client");
  linked_ = load_credentials_();
  if (mic_ != nullptr) {
    mic_->add_data_callback(
        [this](const std::vector<uint8_t> &data) { on_mic_data_(data); });
  }
  mic_packet_.reserve(protocol::MIC_SAMPLES_PER_PACKET);

  audio_buf_ = static_cast<uint8_t *>(
      heap_caps_malloc(kAudioBufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (audio_buf_ == nullptr) {
    mark_failed();
    ESP_LOGE(TAG, "Could not allocate %u-byte PSRAM playback ring",
             static_cast<unsigned>(kAudioBufBytes));
    return;
  }
  if (speaker_ != nullptr) {
    speaker_->set_audio_stream_info(
        audio::AudioStreamInfo(16, 1, 48000));
    speaker_->start();
  }
  reconnect_requested_ = true;
  reconnect_at_ms_ = millis() + 250;
}

void EchoMuseClient::dump_config() {
  ESP_LOGCONFIG(TAG, "EchoMuse satellite:");
  ESP_LOGCONFIG(TAG, "  Controller: %s:%u", host_.c_str(),
                static_cast<unsigned>(linked_ ? tls_port_ : port_));
  ESP_LOGCONFIG(TAG, "  Secure credentials: %s", YESNO(linked_));
  ESP_LOGCONFIG(TAG, "  Device id: %s", device_id_().c_str());
  ESP_LOGCONFIG(TAG, "  Playback ring: %u bytes",
                static_cast<unsigned>(kAudioBufBytes));
}

void EchoMuseClient::loop() {
  if (flush_requested_.exchange(false))
    flush_playback_();

  if (disconnect_requested_.exchange(false)) {
    reconnect_requested_ = false;
    disconnect_all_();
    schedule_reconnect_();
  }

  if (reconnect_requested_ && static_cast<int32_t>(millis() - reconnect_at_ms_) >= 0) {
    // esp_websocket_client_start() may wait for its first TCP connection.
    // Starting it while ESPHome's Wi-Fi component is still scanning blocks
    // loopTask long enough for the ESP-IDF task watchdog to reset the PE.
    // setup priority orders components but does not mean Wi-Fi has an IP yet.
    if (!network::is_connected()) {
      reconnect_at_ms_ = millis() + 1000;
      return;
    }
    reconnect_requested_ = false;
    disconnect_all_();
    if (!linked_ || wall_clock_valid_()) {
      connect_control_();
    } else {
      ESP_LOGD(TAG, "Waiting for SNTP before opening secure WebSockets");
      reconnect_requested_ = true;
      reconnect_at_ms_ = millis() + 1000;
    }
  }

  if (button_send_pending_ &&
      static_cast<int32_t>(millis() - button_send_at_ms_) >= 0) {
    button_send_pending_ = false;
    const char *muted = muted_ ? "true" : "false";
    send_control_(std::string("{\"type\":\"button\",\"clickType\":138,") +
                  "\"down\":true,\"muted\":" + muted + "}");
    send_control_(std::string("{\"type\":\"button\",\"clickType\":138,") +
                  "\"down\":false,\"heldMs\":" +
                  std::to_string(button_held_ms_) + ",\"muted\":" + muted + "}");
  }

  if (speaker_ == nullptr || audio_buf_ == nullptr)
    return;

  size_t fill;
  size_t head;
  portENTER_CRITICAL(&ring_mux_);
  fill = audio_fill_;
  head = audio_head_;
  portEXIT_CRITICAL(&ring_mux_);

  if (playback_active_ && fill > 0) {
    if (playback_priming_) {
      if (fill < PLAYBACK_PRIME_BYTES && !eos_received_ &&
          millis() - prime_started_ms_ < 1500)
        return;
      playback_priming_ = false;
      prime_wait_ms_ = millis() - prime_started_ms_;
      playback_started_ms_ = millis();
    }

    if (!speaker_->has_buffered_data() && downstream_was_buffered_)
      playback_underruns_++;
    downstream_was_buffered_ = speaker_->has_buffered_data();

    size_t contiguous = std::min(fill, kAudioBufBytes - head);
    size_t accepted = speaker_->play(audio_buf_ + head, contiguous);
    if (accepted > 0) {
      portENTER_CRITICAL(&ring_mux_);
      audio_head_ = (audio_head_ + accepted) % kAudioBufBytes;
      audio_fill_ -= accepted;
      min_depth_ = std::min(min_depth_, audio_fill_);
      portEXIT_CRITICAL(&ring_mux_);
      downstream_was_buffered_ = true;
    }
  }

  portENTER_CRITICAL(&ring_mux_);
  fill = audio_fill_;
  portEXIT_CRITICAL(&ring_mux_);
  if (playback_active_ && eos_received_ && fill == 0 &&
      !speaker_->has_buffered_data())
    finish_playback_();
}

void EchoMuseClient::connect_control_() {
  std::string uri = ws_uri_("/control");
  esp_websocket_client_config_t cfg = {};
  cfg.uri = uri.c_str();
  cfg.disable_auto_reconnect = true;
  cfg.ping_interval_sec = 10;
  cfg.pingpong_timeout_sec = 20;
  if (linked_) {
    headers_ = "X-EM-Token: " + token_ + "\r\n";
    cfg.cert_pem = ca_pem_.c_str();
    cfg.cert_common_name = server_name_.c_str();
    cfg.headers = headers_.c_str();
  }
  auto handle = esp_websocket_client_init(&cfg);
  if (handle == nullptr) {
    schedule_reconnect_();
    return;
  }
  control_ws_ = handle;
  esp_websocket_register_events(handle, WEBSOCKET_EVENT_ANY,
                                echomuse_ws_event, &control_context_);
  ESP_LOGI(TAG, "Connecting control plane to %s", uri.c_str());
  if (esp_websocket_client_start(handle) != ESP_OK)
    schedule_reconnect_();
}

void EchoMuseClient::connect_data_() {
  if (!control_registered_ || data_ws_ != nullptr)
    return;
  std::string uri = ws_uri_("/data");
  esp_websocket_client_config_t cfg = {};
  cfg.uri = uri.c_str();
  cfg.disable_auto_reconnect = true;
  cfg.ping_interval_sec = 10;
  cfg.pingpong_timeout_sec = 20;
  if (linked_) {
    cfg.cert_pem = ca_pem_.c_str();
    cfg.cert_common_name = server_name_.c_str();
    cfg.headers = headers_.c_str();
  }
  auto handle = esp_websocket_client_init(&cfg);
  if (handle == nullptr) {
    schedule_reconnect_();
    return;
  }
  data_ws_ = handle;
  esp_websocket_register_events(handle, WEBSOCKET_EVENT_ANY,
                                echomuse_ws_event, &data_context_);
  if (esp_websocket_client_start(handle) != ESP_OK)
    schedule_reconnect_();
}

void EchoMuseClient::disconnect_all_() {
  intentional_disconnect_ = true;
  control_connected_ = false;
  control_registered_ = false;
  data_connected_ = false;
  mic_requested_ = false;
  mic_packet_.clear();
  if (mic_ != nullptr)
    mic_->stop();
  flush_playback_();
  set_phase_("idle");
  auto destroy = [](void *&opaque) {
    if (opaque == nullptr)
      return;
    auto handle = static_cast<esp_websocket_client_handle_t>(opaque);
    esp_websocket_unregister_events(handle, WEBSOCKET_EVENT_ANY,
                                    echomuse_ws_event);
    esp_websocket_client_stop(handle);
    esp_websocket_client_destroy(handle);
    opaque = nullptr;
  };
  destroy(data_ws_);
  destroy(control_ws_);
  intentional_disconnect_ = false;
  control_fragment_.clear();
  data_fragment_.clear();
}

void EchoMuseClient::schedule_reconnect_() {
  if (reconnect_requested_)
    return;
  reconnect_requested_ = true;
  reconnect_at_ms_ = millis() + (pending_wait_ ? 30000 : reconnect_delay_ms_);
  pending_wait_ = false;
  if (reconnect_delay_ms_ < 2000)
    reconnect_delay_ms_ = 2000;
  else if (reconnect_delay_ms_ < 5000)
    reconnect_delay_ms_ = 5000;
  else
    reconnect_delay_ms_ = 10000;
}

void EchoMuseClient::on_ws_event(bool control, int32_t event_id,
                                 void *event_data) {
  auto *event = static_cast<esp_websocket_event_data_t *>(event_data);
  if (event_id == WEBSOCKET_EVENT_CONNECTED) {
    if (control) {
      control_connected_ = true;
      reconnect_delay_ms_ = 1000;
      send_registration_();
    } else {
      data_connected_ = true;
      send_identify_();
    }
    return;
  }
  if (event_id == WEBSOCKET_EVENT_DATA && event != nullptr &&
      event->data_ptr != nullptr && event->data_len > 0) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(event->data_ptr);
    const size_t len = static_cast<size_t>(event->data_len);
    const size_t total = static_cast<size_t>(event->payload_len);
    const size_t offset = static_cast<size_t>(event->payload_offset);
    if (control) {
      if (control_fragment_.append(offset, total, bytes, len)) {
        const auto &message = control_fragment_.bytes();
        handle_control_(std::string(message.begin(), message.end()));
      }
    } else {
      if (data_fragment_.append(offset, total, bytes, len)) {
        const auto &message = data_fragment_.bytes();
        handle_data_(message.data(), message.size());
        data_fragment_.clear();
      }
    }
    return;
  }
  if (event_id == WEBSOCKET_EVENT_DISCONNECTED ||
      event_id == WEBSOCKET_EVENT_CLOSED || event_id == WEBSOCKET_EVENT_ERROR) {
    if (intentional_disconnect_)
      return;
    if (control) {
      control_connected_ = false;
      control_registered_ = false;
      data_connected_ = false;
      mic_requested_ = false;
      disconnect_requested_ = true;
    } else {
      data_connected_ = false;
      mic_requested_ = false;
      disconnect_requested_ = true;
    }
  }
}

void EchoMuseClient::send_registration_() {
  std::ostringstream out;
  out << "{\"type\":\"register\",\"device_id\":\"" << device_id_()
      << "\",\"version\":\"" << json_escape_(version_)
      << "\",\"device_type\":\"voice_pe\","
         "\"device_model\":\"Home Assistant Voice PE\","
         "\"capabilities\":[\"mic\",\"speaker\",\"leds\",\"buttons\","
         "\"button_hold\",\"hardware_mute\",\"volume\"]}";
  send_control_(out.str());
}

void EchoMuseClient::send_identify_() {
  if (data_ws_ == nullptr)
    return;
  std::string msg = "{\"type\":\"identify\",\"device_id\":\"" +
                    device_id_() + "\"}";
  esp_websocket_client_send_text(
      static_cast<esp_websocket_client_handle_t>(data_ws_), msg.c_str(),
      msg.size(), 100 / portTICK_PERIOD_MS);
}

bool EchoMuseClient::send_control_(const std::string &payload) {
  if (!control_connected_ || control_ws_ == nullptr)
    return false;
  return esp_websocket_client_send_text(
             static_cast<esp_websocket_client_handle_t>(control_ws_),
             payload.c_str(), payload.size(), 100 / portTICK_PERIOD_MS) >= 0;
}

void EchoMuseClient::handle_control_(const std::string &payload) {
  JsonDocument doc;
  if (deserializeJson(doc, payload) != DeserializationError::Ok)
    return;
  JsonObject root = doc.as<JsonObject>();
  const char *type = root["type"] | "";
  if (strcmp(type, "ack") == 0) {
    control_registered_ = true;
    pending_wait_ = false;
    reconnect_delay_ms_ = 1000;
    set_phase_("idle");
    defer([this]() { connect_data_(); });
  } else if (strcmp(type, "pending") == 0) {
    pending_wait_ = true;
  } else if (strcmp(type, "mic_start") == 0) {
    mic_requested_ = true;
    if (mic_ != nullptr)
      mic_->start();
    // EchoMuse uses mic_start for its always-on controller-side wake-word
    // capture.  It is not the "the user is speaking" state: showing the
    // inherited PE listening spinner here leaves a white ring rotating at
    // rest.  Turn visuals come from controller LEDs frames instead.
    set_phase_("idle");
  } else if (strcmp(type, "mic_stop") == 0) {
    mic_requested_ = false;
    mic_packet_.clear();
    if (mic_ != nullptr)
      mic_->stop();
    set_phase_(playback_active_ ? "replying" : "idle");
  } else if (strcmp(type, "speaker_flush") == 0) {
    playback_gate_.flush();
    flush_requested_ = true;
  } else if (strcmp(type, "ping") == 0) {
    std::ostringstream reply;
    reply << "{\"type\":\"pong\"";
    if (root["id"].is<int>())
      reply << ",\"id\":" << root["id"].as<int>();
    reply << "}";
    send_control_(reply.str());
  } else if (strcmp(type, "volume_set") == 0) {
    fire_volume_(protocol::level_to_pe_volume(root["level"] | 85));
  } else if (strcmp(type, "leds") == 0) {
    std::vector<uint8_t> frame(36, 0);
    JsonArray leds = root["leds"].as<JsonArray>();
    for (JsonObject led : leds) {
      int id = led["id"] | -1;
      if (id < 0 || id >= 12)
        continue;
      frame[id * 3] = static_cast<uint8_t>(std::max(0, std::min(255, led["r"] | 0)));
      frame[id * 3 + 1] = static_cast<uint8_t>(std::max(0, std::min(255, led["g"] | 0)));
      frame[id * 3 + 2] = static_cast<uint8_t>(std::max(0, std::min(255, led["b"] | 0)));
    }
    fire_leds_(frame);
  } else if (strcmp(type, "link_credentials") == 0) {
    const std::string request_id = root["request_id"] | "";
    const std::string ca = root["ca_pem"] | "";
    const std::string token = root["token"] | "";
    const std::string server_name = root["server_name"] | "echomuse-controller";
    const uint16_t tls_port = root["tls_port"] | 8770;
    const bool ok = !request_id.empty() && store_credentials_(
        ca, token, tls_port, server_name);
    std::string reply = "{\"type\":\"link_credentials_ack\",\"request_id\":\"" +
                        json_escape_(request_id) + "\",\"ok\":" +
                        (ok ? "true" : "false");
    if (!ok)
      reply += ",\"error\":\"credential storage failed\"";
    reply += "}";
    send_control_(reply);
    if (ok) {
      linked_ = true;
      reconnect_requested_ = true;
      reconnect_at_ms_ = millis() + 500;
    }
  }
}

void EchoMuseClient::handle_data_(const uint8_t *data, size_t len) {
  if (data == nullptr || len == 0)
    return;
  const bool accept = playback_gate_.accept(data[0]);
  if (data[0] == 0x03) {
    eos_received_ = true;
    return;
  }
  if (!accept || len <= 1 || audio_buf_ == nullptr)
    return;
  const uint8_t *pcm = data + 1;
  size_t pcm_len = len - 1;
  const uint32_t now = millis();
  if (!playback_active_) {
    playback_active_ = true;
    playback_priming_ = true;
    eos_received_ = false;
    prime_started_ms_ = now;
    first_recv_ms_ = now;
    last_recv_ms_ = 0;
    max_gap_ms_ = prime_wait_ms_ = bytes_received_ = playback_periods_ =
        playback_underruns_ = 0;
    min_depth_ = kAudioBufBytes;
    set_phase_("replying");
  }
  if (last_recv_ms_ != 0)
    max_gap_ms_ = std::max(max_gap_ms_, now - last_recv_ms_);
  last_recv_ms_ = now;
  bytes_received_ += pcm_len;
  playback_periods_++;

  portENTER_CRITICAL(&ring_mux_);
  if (!protocol::ring_write(audio_buf_, kAudioBufBytes, audio_tail_, audio_fill_,
                            pcm, pcm_len)) {
    playback_underruns_++;
  }
  portEXIT_CRITICAL(&ring_mux_);
}

void EchoMuseClient::on_mic_data_(const std::vector<uint8_t> &samples) {
  if (!data_connected_ || !mic_requested_ || muted_ || data_ws_ == nullptr)
    return;
  const size_t frame_bytes = sizeof(int32_t) * 2;
  const size_t frames = samples.size() / frame_bytes;
  for (size_t i = 0; i < frames; i++) {
    mic_packet_.push_back(protocol::stereo_s32_to_mono_s16(
        samples.data() + i * frame_bytes, mic_channel_));
    if (mic_packet_.size() == protocol::MIC_SAMPLES_PER_PACKET) {
      std::vector<uint8_t> packet(3 + protocol::MIC_BYTES_PER_PACKET);
      protocol::write_mic_header(packet.data(), mic_sequence_);
      memcpy(packet.data() + 3, mic_packet_.data(), protocol::MIC_BYTES_PER_PACKET);
      int sent = esp_websocket_client_send_bin(
          static_cast<esp_websocket_client_handle_t>(data_ws_),
          reinterpret_cast<const char *>(packet.data()), packet.size(),
          20 / portTICK_PERIOD_MS);
      if (sent >= 0)
        mic_sequence_ = protocol::next_sequence(mic_sequence_);
      mic_packet_.clear();
    }
  }
}

void EchoMuseClient::flush_playback_() {
  portENTER_CRITICAL(&ring_mux_);
  audio_head_ = audio_tail_ = audio_fill_ = 0;
  portEXIT_CRITICAL(&ring_mux_);
  if (speaker_ != nullptr)
    speaker_->stop();
  playback_active_ = false;
  playback_priming_ = false;
  eos_received_ = false;
  set_phase_(mic_requested_ ? "listening" : "idle");
}

void EchoMuseClient::finish_playback_() {
  playback_active_ = false;
  const uint32_t recv_span = last_recv_ms_ >= first_recv_ms_
                                 ? last_recv_ms_ - first_recv_ms_ : 0;
  std::ostringstream msg;
  msg << "{\"type\":\"playback_stats\",\"periods\":" << playback_periods_
      << ",\"underruns\":" << playback_underruns_
      << ",\"bytesRecv\":" << bytes_received_
      << ",\"primeWaitMs\":" << prime_wait_ms_
      << ",\"recvSpanMs\":" << recv_span
      << ",\"maxGapMs\":" << max_gap_ms_
      << ",\"minDepth\":" << min_depth_ << "}";
  send_control_(msg.str());
  set_phase_(mic_requested_ ? "listening" : "idle");
}

void EchoMuseClient::set_muted(bool muted) {
  muted_ = muted;
  if (muted)
    mic_packet_.clear();
  send_control_(std::string("{\"type\":\"mute_state\",\"muted\":") +
                (muted ? "true}" : "false}"));
  if (!muted && !last_led_frame_.empty())
    fire_leds_(last_led_frame_);
}

void EchoMuseClient::report_volume(float pe_volume, bool muted) {
  const int level = protocol::pe_volume_to_level(pe_volume, muted);
  send_control_("{\"type\":\"volume_state\",\"level\":" +
                std::to_string(level) + "}");
}

void EchoMuseClient::button_down(bool consumed) {
  button_active_ = true;
  button_consumed_ = consumed;
  button_down_ms_ = millis();
}

void EchoMuseClient::button_up(bool consumed) {
  if (!button_active_)
    return;
  button_active_ = false;
  button_consumed_ = button_consumed_ || consumed;
  uint32_t held = millis() - button_down_ms_;
  if (button_consumed_)
    return;
  button_held_ms_ = held;
  // The inherited PE UI recognises its local multi-click/chime after release.
  // Send after that short window so the controller never records the chime as
  // the start of a button turn.
  button_send_pending_ = true;
  button_send_at_ms_ = millis() + 900;
}

void EchoMuseClient::set_phase_(const std::string &phase) {
  if (phase == phase_)
    return;
  phase_ = phase;
  defer([this, phase]() {
    for (auto *trigger : phase_triggers_)
      trigger->trigger(phase);
  });
}

void EchoMuseClient::fire_leds_(const std::vector<uint8_t> &frame) {
  last_led_frame_ = frame;
  if (muted_)
    return;
  defer([this, frame]() {
    for (auto *trigger : leds_triggers_)
      trigger->trigger(frame);
  });
}

void EchoMuseClient::fire_volume_(float volume) {
  defer([this, volume]() {
    for (auto *trigger : volume_triggers_)
      trigger->trigger(volume);
  });
}

bool EchoMuseClient::load_credentials_() {
  nvs_handle_t handle;
  if (nvs_open("echomuse", NVS_READONLY, &handle) != ESP_OK)
    return false;
  uint8_t linked = 0;
  uint16_t port = 0;
  bool ok = nvs_get_u8(handle, "linked", &linked) == ESP_OK && linked == 1 &&
            nvs_read_string(handle, "ca", ca_pem_) &&
            nvs_read_string(handle, "token", token_) &&
            nvs_read_string(handle, "server", server_name_) &&
            nvs_get_u16(handle, "port", &port) == ESP_OK;
  nvs_close(handle);
  if (ok)
    tls_port_ = port;
  return ok;
}

bool EchoMuseClient::store_credentials_(const std::string &ca,
                                        const std::string &token,
                                        uint16_t tls_port,
                                        const std::string &server_name) {
  if (ca.empty() || token.empty() || server_name.empty() || tls_port == 0)
    return false;
  nvs_handle_t handle;
  if (nvs_open("echomuse", NVS_READWRITE, &handle) != ESP_OK)
    return false;
  esp_err_t err = nvs_set_str(handle, "ca", ca.c_str());
  if (err == ESP_OK) err = nvs_set_str(handle, "token", token.c_str());
  if (err == ESP_OK) err = nvs_set_str(handle, "server", server_name.c_str());
  if (err == ESP_OK) err = nvs_set_u16(handle, "port", tls_port);
  if (err == ESP_OK) err = nvs_set_u8(handle, "linked", 1);
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK)
    return false;
  ca_pem_ = ca;
  token_ = token;
  server_name_ = server_name;
  tls_port_ = tls_port;
  return true;
}

bool EchoMuseClient::wall_clock_valid_() const {
  return std::time(nullptr) > 1700000000;
}

std::string EchoMuseClient::ws_uri_(const char *path) const {
  return std::string(linked_ ? "wss://" : "ws://") + host_ + ":" +
         std::to_string(linked_ ? tls_port_ : port_) + path;
}

std::string EchoMuseClient::device_id_() const {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char value[32];
  snprintf(value, sizeof(value), "voice-pe-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return value;
}

std::string EchoMuseClient::json_escape_(const std::string &value) const {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    if (c == '\\' || c == '"') out.push_back('\\');
    if (c == '\n') { out += "\\n"; continue; }
    if (c == '\r') { out += "\\r"; continue; }
    out.push_back(c);
  }
  return out;
}

}  // namespace echomuse_client
}  // namespace esphome
