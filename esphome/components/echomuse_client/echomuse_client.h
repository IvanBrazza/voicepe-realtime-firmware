#pragma once

#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"
#include "protocol.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace esphome {
namespace echomuse_client {

class OnLedsTrigger;
class OnVolumeSetTrigger;
class OnPhaseTrigger;

class EchoMuseClient : public Component {
 public:
  void set_host(const std::string &host) { host_ = host; }
  void set_port(uint16_t port) { port_ = port; }
  void set_version(const std::string &version) { version_ = version; }
  void set_microphone(microphone::Microphone *mic) { mic_ = mic; }
  void set_mic_channel(uint8_t channel) { mic_channel_ = channel; }
  void set_speaker(speaker::Speaker *speaker) { speaker_ = speaker; }

  void add_leds_trigger(OnLedsTrigger *trigger) { leds_triggers_.push_back(trigger); }
  void add_volume_trigger(OnVolumeSetTrigger *trigger) { volume_triggers_.push_back(trigger); }
  void add_phase_trigger(OnPhaseTrigger *trigger) { phase_triggers_.push_back(trigger); }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  bool is_connected() const { return control_registered_ && data_connected_; }
  void set_muted(bool muted);
  void report_volume(float pe_volume, bool muted = false);
  void button_down(bool consumed = false);
  void button_up(bool consumed = false);

  // Compatibility surface for the realtime YAML package. EchoMuse owns turn
  // state, so legacy session/enrolment calls either become a button gesture
  // or a deliberate no-op.
  void set_volume(float) {}
  void start_session() {}
  void send_button_cancel() {}
  void send_false_flag() {}
  void send_interrupt() {}
  void enroll_start() {}
  void enroll_stop(bool) {}
  bool enroll_active() const { return false; }
  bool turn_has_reply_audio() const { return playback_active_; }
  uint32_t get_wake_open_delay_ms() const { return 0; }
  void commit_followup_mic() {}

  void on_ws_event(bool control, int32_t event_id, void *event_data);

 public:
  struct WsContext {
    EchoMuseClient *self;
    bool control;
  };

 protected:

  void connect_control_();
  void connect_data_();
  void disconnect_all_();
  void schedule_reconnect_();
  void send_registration_();
  void send_identify_();
  bool send_control_(const std::string &payload);
  void handle_control_(const std::string &payload);
  void handle_data_(const uint8_t *data, size_t len);
  void on_mic_data_(const std::vector<uint8_t> &samples);
  void set_phase_(const std::string &phase);
  void flush_playback_();
  void finish_playback_();
  void fire_leds_(const std::vector<uint8_t> &frame);
  void fire_volume_(float volume);

  bool load_credentials_();
  bool store_credentials_(const std::string &ca, const std::string &token,
                          uint16_t tls_port, const std::string &server_name);
  bool wall_clock_valid_() const;
  std::string ws_uri_(const char *path) const;
  std::string device_id_() const;
  std::string json_escape_(const std::string &value) const;

  std::string host_;
  uint16_t port_{8767};
  std::string version_{"dev"};
  uint8_t mic_channel_{0};
  microphone::Microphone *mic_{nullptr};
  speaker::Speaker *speaker_{nullptr};

  void *control_ws_{nullptr};
  void *data_ws_{nullptr};
  WsContext control_context_{this, true};
  WsContext data_context_{this, false};
  std::atomic<bool> control_connected_{false};
  std::atomic<bool> control_registered_{false};
  std::atomic<bool> data_connected_{false};
  std::atomic<bool> mic_requested_{false};
  std::atomic<bool> muted_{false};
  std::atomic<bool> flush_requested_{false};
  std::atomic<bool> reconnect_requested_{false};
  std::atomic<bool> disconnect_requested_{false};
  std::atomic<bool> intentional_disconnect_{false};
  uint32_t reconnect_at_ms_{0};
  uint32_t reconnect_delay_ms_{1000};
  bool pending_wait_{false};

  protocol::FragmentAccumulator control_fragment_;
  protocol::FragmentAccumulator data_fragment_;

  std::vector<int16_t> mic_packet_;
  uint16_t mic_sequence_{0};

  uint8_t *audio_buf_{nullptr};
  static constexpr size_t kAudioBufBytes = 512 * 1024;
  size_t audio_head_{0};
  size_t audio_tail_{0};
  size_t audio_fill_{0};
  portMUX_TYPE ring_mux_ = portMUX_INITIALIZER_UNLOCKED;
  bool playback_active_{false};
  bool playback_priming_{false};
  bool eos_received_{false};
  protocol::PlaybackDiscardGate playback_gate_;
  bool downstream_was_buffered_{false};
  uint32_t prime_started_ms_{0};
  uint32_t playback_started_ms_{0};
  uint32_t first_recv_ms_{0};
  uint32_t last_recv_ms_{0};
  uint32_t max_gap_ms_{0};
  uint32_t prime_wait_ms_{0};
  uint32_t bytes_received_{0};
  uint32_t playback_periods_{0};
  uint32_t playback_underruns_{0};
  size_t min_depth_{0};

  bool button_active_{false};
  bool button_consumed_{false};
  uint32_t button_down_ms_{0};
  bool button_send_pending_{false};
  uint32_t button_send_at_ms_{0};
  uint32_t button_held_ms_{0};

  std::vector<uint8_t> last_led_frame_;

  bool linked_{false};
  uint16_t tls_port_{8770};
  std::string ca_pem_;
  std::string token_;
  std::string server_name_{"echomuse-controller"};
  std::string headers_;

  std::vector<OnLedsTrigger *> leds_triggers_;
  std::vector<OnVolumeSetTrigger *> volume_triggers_;
  std::vector<OnPhaseTrigger *> phase_triggers_;
  // Start outside an actual phase so the first controller "idle" state is
  // emitted to YAML and clears any inherited boot animation.
  std::string phase_{"boot"};
};

}  // namespace echomuse_client
}  // namespace esphome
