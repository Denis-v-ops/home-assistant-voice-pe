#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "esp_websocket_client.h"

#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/static_task.h"

namespace esphome::nova_realtime {

class NovaRealtime : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_microphone_source(microphone::MicrophoneSource *microphone) { this->microphone_ = microphone; }
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  void set_wake_word_status(text_sensor::TextSensor *sensor) { this->wake_word_status_ = sensor; }
  void set_gateway_url(const std::string &url) { this->gateway_url_ = url; }
  void set_device_id(const std::string &device_id) { this->device_id_ = device_id; }
  void set_enabled(bool enabled) { this->enabled_ = enabled; }
  void set_connect_delay_ms(uint32_t connect_delay_ms) { this->connect_delay_ms_ = connect_delay_ms; }

  void start_session(const std::string &wake_word);
  void stop_session(const std::string &reason = "device_request");
  void accept_wake();
  void reject_wake(const std::string &reason = "device_rejected");
  void set_muted(bool muted);
  bool is_running() const { return this->session_active_.load() || !this->pending_wake_session_id_.empty(); }
  bool is_connected() const { return this->gateway_ready_.load(); }

  Trigger<> *get_connected_trigger() { return &this->connected_trigger_; }
  Trigger<> *get_disconnected_trigger() { return &this->disconnected_trigger_; }
  Trigger<std::string> *get_state_trigger() { return &this->state_trigger_; }
  Trigger<std::string, std::string> *get_error_trigger() { return &this->error_trigger_; }
  Trigger<std::string, std::string> *get_remote_wake_trigger() { return &this->remote_wake_trigger_; }

 protected:
  static constexpr size_t MAX_MESSAGE_BYTES = 2048;
  static constexpr size_t INCOMING_SLOTS = 32;
  static constexpr size_t OUTGOING_CONTROL_SLOTS = 8;
  static constexpr size_t MAX_OUTGOING_CONTROL_BYTES = 768;
  static constexpr size_t PROTOCOL_HEADER_SIZE = 16;
  static constexpr size_t MICROPHONE_FRAME_BYTES = 640;
  static constexpr size_t SPEAKER_FRAME_BYTES = 960;

  enum class SessionState : uint8_t { IDLE, STARTING, ACTIVE, STOPPING };

  struct IncomingMessage {
    bool binary{false};
    uint16_t length{0};
  };

  struct OutgoingControl {
    uint16_t length{0};
    std::array<char, MAX_OUTGOING_CONTROL_BYTES> data{};
  };

  static void websocket_event_handler_(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
  static void tx_task_(void *parameter);

  void handle_websocket_event_(int32_t event_id, esp_websocket_event_data_t *event);
  bool enqueue_message_(bool binary, const uint8_t *data, size_t length);
  void process_socket_event_();
  bool queue_hello_();
  void connect_();
  void disconnect_();
  void destroy_client_();
  void handle_control_(const uint8_t *payload, size_t length);
  void handle_audio_(const uint8_t *payload, size_t length);
  void handle_microphone_data_(const std::vector<uint8_t> &data);
  void process_speaker_();
  void process_flush_();
  void process_finish_();
  void report_played_(bool force = false);
  bool queue_control_(const std::string &payload);
  void send_audio_from_task_();
  void run_tx_task_();
  void send_error_(const std::string &code, const std::string &message);
  void set_state_(const std::string &phase);
  void set_microphone_streaming_(bool enabled);
  void start_standby_();
  void stop_standby_(const std::string &status);
  void apply_wake_configuration_(JsonObjectConst config);
  void send_wake_status_(const std::string &state);
  void publish_wake_status_(const std::string &status);
  void clear_pending_wake_();
  void stop_session_(const std::string &reason, bool error);
  void reset_session_(const std::string &phase);
  void acquire_wifi_performance_();
  void release_wifi_performance_();
  std::string new_session_id_();
  bool session_matches_(const char *session_id) const;
  void send_pong_(uint64_t timestamp_ms);

  microphone::MicrophoneSource *microphone_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  text_sensor::TextSensor *wake_word_status_{nullptr};
  esp_websocket_client_handle_t client_{nullptr};

  std::string gateway_url_;
  std::string device_id_;

  Mutex incoming_mutex_;
  std::array<IncomingMessage, INCOMING_SLOTS> incoming_messages_{};
  uint8_t *incoming_storage_{nullptr};
  uint8_t *fragment_storage_{nullptr};
  size_t incoming_head_{0};
  size_t incoming_tail_{0};
  size_t incoming_count_{0};
  size_t fragment_length_{0};
  size_t fragment_expected_{0};
  bool fragment_binary_{false};

  std::unique_ptr<ring_buffer::RingBuffer> microphone_buffer_;
  Mutex microphone_mutex_;
  std::unique_ptr<ring_buffer::RingBuffer> speaker_buffer_;
  std::array<uint8_t, SPEAKER_FRAME_BYTES> speaker_frame_{};
  size_t speaker_frame_length_{0};
  size_t speaker_frame_offset_{0};

  QueueHandle_t control_queue_{nullptr};
  StaticTask tx_task_handle_;
  // These buffers are deliberately owned by the component instead of living
  // on nova_tx's stack. The websocket send path needs several kilobytes of
  // call-stack headroom on ESP-IDF 5.5.
  OutgoingControl tx_control_{};
  std::array<uint8_t, PROTOCOL_HEADER_SIZE + MICROPHONE_FRAME_BYTES> tx_audio_frame_{};
  std::atomic<bool> tx_stop_{false};

  std::atomic<bool> socket_connected_{false};
  std::atomic<bool> gateway_ready_{false};
  std::atomic<bool> session_active_{false};
  std::atomic<bool> standby_active_{false};
  std::atomic<bool> incoming_overrun_{false};
  std::atomic<bool> control_overrun_{false};
  std::atomic<bool> tx_transport_fault_{false};
  std::atomic<int8_t> socket_event_{0};
  std::atomic<bool> microphone_discontinuity_{false};
  std::atomic<bool> microphone_streaming_{false};
  std::atomic<uint32_t> microphone_drops_pending_{0};
  std::atomic<uint32_t> played_samples_{0};
  std::atomic<uint32_t> microphone_callback_count_{0};
  std::atomic<uint32_t> microphone_callback_stack_low_water_bytes_{0xFFFFFFFFUL};

  SessionState session_state_{SessionState::IDLE};
  uint32_t next_connect_at_{0};
  uint32_t connect_delay_ms_{10000};
  uint32_t session_start_deadline_{0};
  uint32_t pending_wake_deadline_{0};
  uint32_t wake_generation_{0};
  uint32_t microphone_sequence_{0};
  uint32_t microphone_sample_index_{0};
  uint32_t expected_speaker_sequence_{0};
  uint32_t expected_speaker_sample_index_{0};
  uint32_t total_speaker_samples_{0};
  uint32_t last_reported_samples_{0};
  uint32_t microphone_drop_window_started_{0};
  uint32_t microphone_drop_window_count_{0};
  uint32_t microphone_drops_total_{0};
  uint32_t transport_faults_total_{0};
  std::atomic<uint32_t> reconnect_count_{0};
  uint32_t rx_high_water_bytes_{0};
  uint32_t speaker_high_water_bytes_{0};
  uint32_t microphone_high_water_bytes_{0};
  uint32_t max_loop_us_{0};
  bool speaker_sequence_initialized_{false};
  bool finish_requested_{false};
  bool finish_called_{false};
  bool drained_reported_{false};
  bool flush_requested_{false};
  uint32_t flush_last_played_samples_{0};
  uint32_t flush_quiet_since_{0};
  bool wifi_performance_owned_{false};
  bool enabled_{true};
  bool remote_wake_enabled_{false};
  bool muted_{false};
  bool hello_pending_{false};
  bool session_stack_reported_{false};
  std::string flush_item_id_;
  std::string current_session_id_;
  std::string current_item_id_;
  std::string last_phase_;
  std::string pending_wake_session_id_;
  std::string pending_wake_word_;
  std::string last_wake_status_;

  Trigger<> connected_trigger_;
  Trigger<> disconnected_trigger_;
  Trigger<std::string> state_trigger_;
  Trigger<std::string, std::string> error_trigger_;
  Trigger<std::string, std::string> remote_wake_trigger_;
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

template<typename... Ts> class AcceptWakeAction : public Action<Ts...>, public Parented<NovaRealtime> {
 public:
  void play(const Ts &...x) override { this->parent_->accept_wake(); }
};

template<typename... Ts> class RejectWakeAction : public Action<Ts...>, public Parented<NovaRealtime> {
  TEMPLATABLE_VALUE(std::string, reason)

 public:
  void play(const Ts &...x) override { this->parent_->reject_wake(this->reason_.value(x...)); }
};

template<typename... Ts> class SetMutedAction : public Action<Ts...>, public Parented<NovaRealtime> {
  TEMPLATABLE_VALUE(bool, muted)

 public:
  void play(const Ts &...x) override { this->parent_->set_muted(this->muted_.value(x...)); }
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
