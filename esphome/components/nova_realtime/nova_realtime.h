#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "esp_websocket_client.h"

#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

namespace esphome::nova_realtime {

class NovaRealtime : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_microphone_source(microphone::MicrophoneSource *microphone) { this->microphone_ = microphone; }
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  void set_gateway_url(const std::string &url) { this->gateway_url_ = url; }
  void set_device_id(const std::string &device_id) { this->device_id_ = device_id; }

  void start_session(const std::string &wake_word);
  void stop_session(const std::string &reason = "device_request");
  bool is_running() const { return this->session_active_.load(); }
  bool is_connected() const { return this->gateway_ready_.load(); }

  Trigger<> *get_connected_trigger() { return &this->connected_trigger_; }
  Trigger<> *get_disconnected_trigger() { return &this->disconnected_trigger_; }
  Trigger<std::string> *get_state_trigger() { return &this->state_trigger_; }
  Trigger<std::string, std::string> *get_error_trigger() { return &this->error_trigger_; }

 protected:
  struct IncomingMessage {
    bool binary;
    std::vector<uint8_t> data;
  };

  static void websocket_event_handler_(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
  void handle_websocket_event_(int32_t event_id, esp_websocket_event_data_t *event);
  void connect_();
  void disconnect_();
  void handle_control_(const std::string &payload);
  void handle_audio_(const std::vector<uint8_t> &payload);
  void handle_microphone_data_(const std::vector<uint8_t> &data);
  void process_microphone_();
  void process_speaker_();
  void report_played_(bool force = false);
  void send_control_(const std::string &payload);
  void send_audio_(const uint8_t *pcm, size_t length);
  void send_error_(const std::string &code, const std::string &message);
  void set_state_(const std::string &phase);

  microphone::MicrophoneSource *microphone_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  esp_websocket_client_handle_t client_{nullptr};

  std::string gateway_url_;
  std::string device_id_;

  Mutex incoming_mutex_;
  std::deque<IncomingMessage> incoming_;
  std::vector<uint8_t> fragmented_message_;
  bool fragmented_binary_{false};

  Mutex microphone_mutex_;
  std::vector<uint8_t> microphone_buffer_;
  std::vector<uint8_t> speaker_pending_;
  size_t speaker_pending_offset_{0};

  std::atomic<bool> socket_connected_{false};
  std::atomic<bool> gateway_ready_{false};
  std::atomic<bool> session_active_{false};
  std::atomic<bool> incoming_overrun_{false};
  uint32_t next_connect_at_{0};
  uint32_t microphone_sequence_{0};
  uint32_t microphone_sample_index_{0};
  std::string current_item_id_;
  std::atomic<uint32_t> played_samples_{0};
  uint32_t last_reported_samples_{0};

  Trigger<> connected_trigger_;
  Trigger<> disconnected_trigger_;
  Trigger<std::string> state_trigger_;
  Trigger<std::string, std::string> error_trigger_;
};

template<typename... Ts> class StartAction : public Action<Ts...>, public Parented<NovaRealtime> {
  TEMPLATABLE_VALUE(std::string, wake_word)

 public:
  void play(const Ts &...x) override { this->parent_->start_session(this->wake_word_.value(x...)); }
};

template<typename... Ts> class StopAction : public Action<Ts...>, public Parented<NovaRealtime> {
 public:
  void play(const Ts &...x) override { this->parent_->stop_session(); }
};

template<typename... Ts> class IsRunningCondition : public Condition<Ts...>, public Parented<NovaRealtime> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_running(); }
};

template<typename... Ts> class IsConnectedCondition : public Condition<Ts...>, public Parented<NovaRealtime> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_connected(); }
};

}  // namespace esphome::nova_realtime
