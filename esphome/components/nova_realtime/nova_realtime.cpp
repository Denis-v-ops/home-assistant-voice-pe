#include "nova_realtime.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include "esphome/components/network/util.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/log.h"

namespace esphome::nova_realtime {

static const char *const TAG = "nova_realtime";
static constexpr size_t MICROPHONE_BUFFER_BYTES = 16000;
static constexpr size_t SPEAKER_BUFFER_BYTES = 19200;
static constexpr uint32_t PLAYED_REPORT_INTERVAL = 960;
static constexpr uint32_t SESSION_START_TIMEOUT_MS = 20000;
// Must cover the longest wake chime (1.7 s) plus the settling delay before
// acceptance, and stay below the gateway's three-second pending window.
static constexpr uint32_t PENDING_WAKE_TIMEOUT_MS = 2800;
static constexpr uint32_t MICROPHONE_DROP_WINDOW_MS = 5000;
static constexpr uint32_t MICROPHONE_DROP_LIMIT = 25;
static constexpr uint32_t FLUSH_QUIET_MS = 10;
static constexpr uint32_t LOOP_BUDGET_US = 2000;
static constexpr size_t LOOP_MESSAGE_BUDGET = 4;
static constexpr size_t SPEAKER_LOOP_FRAME_BUDGET = 4;
static constexpr size_t SPEAKER_START_BUFFER_BYTES = 5 * 960;
static constexpr uint32_t SPEAKER_SOURCE_EMPTY_CONFIRM_MS = 20;
static constexpr UBaseType_t TX_TASK_PRIORITY = 4;
// ESP-IDF defines StackType_t as uint8_t and its task stack depth in bytes.
// Streaming enters the WebSocket send path from this task, so retain ample
// headroom for ESP-IDF 5.5 call frames.
static constexpr uint32_t TX_TASK_STACK_BYTES = 8192;
// esp_websocket_client task_stack is expressed in bytes.
static constexpr int WEBSOCKET_TASK_STACK_BYTES = 16384;

static bool deadline_reached(uint32_t now, uint32_t deadline) { return static_cast<int32_t>(now - deadline) >= 0; }

static void update_atomic_max(std::atomic<uint32_t> &target, uint32_t value) {
  uint32_t current = target.load(std::memory_order_relaxed);
  while (value > current &&
         !target.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
  }
}

static uint32_t read_be32(const uint8_t *data) {
  return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | uint32_t(data[3]);
}

static void write_be32(uint8_t *data, uint32_t value) {
  data[0] = uint8_t(value >> 24);
  data[1] = uint8_t(value >> 16);
  data[2] = uint8_t(value >> 8);
  data[3] = uint8_t(value);
}

void NovaRealtime::setup() {
  if (!this->enabled_) {
    ESP_LOGW(TAG, "NOVA transport disabled by configuration; API and OTA remain available");
    this->publish_wake_status_("Unavailable");
    return;
  }
  this->publish_wake_status_("Starting");
  ESP_LOGI(TAG, "Initializing bounded NOVA transport");
  RAMAllocator<uint8_t> external_allocator(RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  this->incoming_storage_ = external_allocator.allocate(INCOMING_SLOTS * MAX_MESSAGE_BYTES);
  this->fragment_storage_ = external_allocator.allocate(MAX_MESSAGE_BYTES);
  this->microphone_buffer_ = ring_buffer::RingBuffer::create(MICROPHONE_BUFFER_BYTES);
  this->speaker_buffer_ = ring_buffer::RingBuffer::create(SPEAKER_BUFFER_BYTES);
  this->control_queue_ = xQueueCreate(OUTGOING_CONTROL_SLOTS, sizeof(OutgoingControl));
  if (this->incoming_storage_ == nullptr || this->fragment_storage_ == nullptr || this->microphone_buffer_ == nullptr ||
      this->speaker_buffer_ == nullptr || this->control_queue_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate bounded NOVA transport buffers");
    this->mark_failed();
    return;
  }

  this->microphone_->add_data_callback(
      [this](const std::vector<uint8_t> &data) { this->handle_microphone_data_(data); });
  this->speaker_->add_audio_output_callback([this](uint32_t frames, int64_t) {
    this->played_samples_.fetch_add(frames, std::memory_order_relaxed);
    const uint32_t now = millis();
    const uint32_t previous = this->speaker_last_callback_ms_.exchange(now, std::memory_order_relaxed);
    if (previous != 0)
      update_atomic_max(this->speaker_max_callback_gap_ms_, now - previous);
  });

  if (!this->tx_task_handle_.create(NovaRealtime::tx_task_, "nova_tx", TX_TASK_STACK_BYTES, this, TX_TASK_PRIORITY,
                                     false)) {
    ESP_LOGE(TAG, "Could not create NOVA transmit task");
    this->mark_failed();
    return;
  }
  this->next_connect_at_ = millis() + this->connect_delay_ms_;
  ESP_LOGI(TAG, "NOVA transport initialized (TX stack: %u bytes, WebSocket stack: %d bytes)",
           unsigned(TX_TASK_STACK_BYTES), WEBSOCKET_TASK_STACK_BYTES);
}

void NovaRealtime::dump_config() {
  ESP_LOGCONFIG(TAG,
                "NOVA Realtime v2:\n"
                "  Enabled: %s\n"
                "  Initial connect delay: %" PRIu32 " ms\n"
                "  Gateway: %s\n"
                 "  Device ID: %s\n"
                 "  Playback window: 300 ms\n"
                 "  Playback startup buffer: 100 ms\n"
                 "  Speaker buffer: 400 ms\n"
                "  Microphone buffer: 500 ms\n"
                "  Remote wake: gateway-only Hey Nova (English/German)\n"
                "  Transport: trusted LAN WebSocket",
                YESNO(this->enabled_), this->connect_delay_ms_, this->gateway_url_.c_str(), this->device_id_.c_str());
}

void NovaRealtime::on_shutdown() {
  if (!this->enabled_)
    return;
  this->gateway_ready_ = false;
  this->remote_wake_enabled_ = false;
  this->reset_session_("offline");
  this->tx_stop_ = true;
  if (this->tx_task_handle_.is_created()) {
    xTaskNotifyGive(this->tx_task_handle_.get_handle());
    this->tx_task_handle_.deallocate();
  }
  this->destroy_client_();
  if (this->control_queue_ != nullptr) {
    vQueueDelete(this->control_queue_);
    this->control_queue_ = nullptr;
  }
  RAMAllocator<uint8_t> external_allocator(RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  external_allocator.deallocate(this->incoming_storage_, INCOMING_SLOTS * MAX_MESSAGE_BYTES);
  external_allocator.deallocate(this->fragment_storage_, MAX_MESSAGE_BYTES);
  this->incoming_storage_ = nullptr;
  this->fragment_storage_ = nullptr;
}

void NovaRealtime::loop() {
  if (!this->enabled_ || this->is_failed())
    return;
  const uint32_t loop_started = micros();
  const uint32_t now = millis();
  if (network::is_connected() && this->client_ == nullptr && deadline_reached(now, this->next_connect_at_)) {
    this->connect_();
  }
  this->process_socket_event_();

  if (this->incoming_overrun_.exchange(false)) {
    {
      LockGuard lock(this->incoming_mutex_);
      this->incoming_head_ = this->incoming_tail_ = this->incoming_count_ = 0;
    }
    this->send_error_("incoming_overrun", "Gateway receive queue exceeded its safe bound");
    this->stop_session_("incoming_overrun", true);
  }
  if (this->control_overrun_.exchange(false)) {
    this->send_error_("control_overrun", "Gateway transmit control queue exceeded its safe bound");
    this->stop_session_("control_overrun", true);
  }
  if (this->tx_transport_fault_.exchange(false)) {
    this->transport_faults_total_++;
    this->send_error_("transport_send_failed", "Gateway transport send failed");
    this->destroy_client_();
    this->next_connect_at_ = now + 5000;
    if (this->session_state_ != SessionState::IDLE)
      this->stop_session_("transport_send_failed", true);
    else
      this->reset_session_("offline");
  }

  uint32_t pending_drops = this->microphone_drops_pending_.exchange(0);
  if (pending_drops != 0) {
    if (this->microphone_drop_window_started_ == 0 ||
        now - this->microphone_drop_window_started_ > MICROPHONE_DROP_WINDOW_MS) {
      this->microphone_drop_window_started_ = now;
      this->microphone_drop_window_count_ = 0;
    }
    this->microphone_drop_window_count_ += pending_drops;
    this->microphone_drops_total_ += pending_drops;
    if (this->microphone_drop_window_count_ >= MICROPHONE_DROP_LIMIT) {
      this->stop_session_("microphone_backpressure", true);
    }
  }

  size_t processed = 0;
  while (processed < LOOP_MESSAGE_BUDGET && micros() - loop_started < LOOP_BUDGET_US) {
    IncomingMessage message;
    const uint8_t *payload = nullptr;
    {
      LockGuard lock(this->incoming_mutex_);
      if (this->incoming_count_ == 0)
        break;
      message = this->incoming_messages_[this->incoming_head_];
      payload = this->incoming_storage_ + this->incoming_head_ * MAX_MESSAGE_BYTES;
    }
    if (message.binary) {
      this->handle_audio_(payload, message.length);
    } else {
      this->handle_control_(payload, message.length);
    }
    {
      LockGuard lock(this->incoming_mutex_);
      this->incoming_head_ = (this->incoming_head_ + 1) % INCOMING_SLOTS;
      this->incoming_count_--;
    }
    processed++;
  }

  this->process_speaker_();
  this->process_flush_();
  this->process_finish_();
  this->report_played_();

  const uint32_t microphone_stack_margin =
      this->microphone_callback_stack_low_water_bytes_.load(std::memory_order_relaxed);
  if (this->session_active_ && !this->session_stack_reported_ && microphone_stack_margin != 0xFFFFFFFFUL) {
    const uint32_t tx_stack_margin =
        uxTaskGetStackHighWaterMark(this->tx_task_handle_.get_handle());
    ESP_LOGI(TAG, "Session stack margins - microphone callback: %" PRIu32 " bytes, TX: %" PRIu32 " bytes",
             microphone_stack_margin, tx_stack_margin);
    this->session_stack_reported_ = true;
  }

  if (this->session_state_ == SessionState::STARTING && deadline_reached(now, this->session_start_deadline_)) {
    this->send_error_("session_start_timeout", "Gateway did not start the session in time");
    this->stop_session_("session_start_timeout", true);
  }
  if (!this->pending_wake_session_id_.empty() && deadline_reached(now, this->pending_wake_deadline_))
    this->reject_wake("automation_timeout");

  const uint32_t elapsed = micros() - loop_started;
  this->max_loop_us_ = std::max(this->max_loop_us_, elapsed);
}

void NovaRealtime::connect_() {
  ESP_LOGI(TAG, "Connecting to NOVA gateway");
  esp_websocket_client_config_t config{};
  config.uri = this->gateway_url_.c_str();
  config.enable_close_reconnect = true;
  config.reconnect_timeout_ms = 5000;
  config.network_timeout_ms = 5000;
  config.ping_interval_sec = 10;
  config.keep_alive_enable = true;
  config.keep_alive_idle = 5;
  config.keep_alive_interval = 5;
  config.keep_alive_count = 3;
  config.buffer_size = MAX_MESSAGE_BYTES;
  config.task_stack = WEBSOCKET_TASK_STACK_BYTES;
  config.task_core_id_set = false;
  this->client_ = esp_websocket_client_init(&config);
  if (this->client_ == nullptr) {
    this->next_connect_at_ = millis() + 5000;
    this->send_error_("connection_init_failed", "Could not initialize gateway connection");
    return;
  }
  esp_websocket_register_events(this->client_, WEBSOCKET_EVENT_ANY, &NovaRealtime::websocket_event_handler_, this);
  esp_err_t result = esp_websocket_client_start(this->client_);
  if (result != ESP_OK) {
    ESP_LOGW(TAG, "Gateway connection start failed: %s", esp_err_to_name(result));
    this->destroy_client_();
    this->next_connect_at_ = millis() + 5000;
  }
}

void NovaRealtime::destroy_client_() {
  if (this->client_ == nullptr)
    return;
  esp_websocket_client_stop(this->client_);
  esp_websocket_client_destroy(this->client_);
  this->client_ = nullptr;
  this->socket_connected_ = false;
  this->gateway_ready_ = false;
}

void NovaRealtime::disconnect_() {
  const bool notify = this->gateway_ready_.exchange(false);
  this->socket_connected_ = false;
  this->remote_wake_enabled_ = false;
  this->wake_generation_ = 0;
  this->timer_alert_active_ = false;
  this->timer_alert_notification_id_.clear();
  if (this->control_queue_ != nullptr)
    xQueueReset(this->control_queue_);
  {
    LockGuard lock(this->incoming_mutex_);
    this->incoming_head_ = this->incoming_tail_ = this->incoming_count_ = 0;
  }
  this->reset_session_("offline");
  if (notify)
    this->disconnected_trigger_.trigger();
}

void NovaRealtime::websocket_event_handler_(void *handler_args, esp_event_base_t, int32_t event_id, void *event_data) {
  static_cast<NovaRealtime *>(handler_args)->handle_websocket_event_(
      event_id, static_cast<esp_websocket_event_data_t *>(event_data));
}

bool NovaRealtime::enqueue_message_(bool binary, const uint8_t *data, size_t length) {
  if (length == 0 || length > MAX_MESSAGE_BYTES)
    return false;
  LockGuard lock(this->incoming_mutex_);
  if (this->incoming_count_ >= INCOMING_SLOTS) {
    this->incoming_overrun_ = true;
    return false;
  }
  std::memcpy(this->incoming_storage_ + this->incoming_tail_ * MAX_MESSAGE_BYTES, data, length);
  this->incoming_messages_[this->incoming_tail_] = {binary, static_cast<uint16_t>(length)};
  this->incoming_tail_ = (this->incoming_tail_ + 1) % INCOMING_SLOTS;
  this->incoming_count_++;
  this->rx_high_water_bytes_ = std::max<uint32_t>(this->rx_high_water_bytes_, this->incoming_count_ * MAX_MESSAGE_BYTES);
  this->enable_loop_soon_any_context();
  return true;
}

void NovaRealtime::handle_websocket_event_(int32_t event_id, esp_websocket_event_data_t *event) {
  if (event_id == WEBSOCKET_EVENT_CONNECTED) {
    // The main loop publishes the connection only after stale session/control
    // state has been purged, so the TX task cannot precede the new hello.
    this->socket_connected_ = false;
    this->reconnect_count_.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGI(TAG, "Gateway socket connected");
    this->socket_event_ = 1;
    this->enable_loop_soon_any_context();
    return;
  }
  if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED) {
    this->socket_connected_ = false;
    this->socket_event_ = -1;
    this->enable_loop_soon_any_context();
    return;
  }
  if (event_id == WEBSOCKET_EVENT_ERROR) {
    ESP_LOGW(TAG, "Gateway WebSocket error");
    return;
  }
  if (event_id != WEBSOCKET_EVENT_DATA || event == nullptr || event->data_ptr == nullptr || event->data_len <= 0)
    return;

  // ESP-IDF also reports WebSocket close/ping/pong control payloads through
  // WEBSOCKET_EVENT_DATA. They are not application JSON messages.
  if (event->payload_offset == 0 && event->op_code != 0x1 && event->op_code != 0x2)
    return;

  if (event->payload_len <= 0 || static_cast<size_t>(event->payload_len) > MAX_MESSAGE_BYTES) {
    this->incoming_overrun_ = true;
    this->enable_loop_soon_any_context();
    return;
  }
  if (event->payload_offset == 0) {
    this->fragment_length_ = 0;
    this->fragment_expected_ = event->payload_len;
    this->fragment_binary_ = event->op_code == 0x2;
  }
  if (this->fragment_expected_ != static_cast<size_t>(event->payload_len) ||
      this->fragment_length_ + event->data_len > MAX_MESSAGE_BYTES ||
      static_cast<size_t>(event->payload_offset) != this->fragment_length_) {
    this->incoming_overrun_ = true;
    this->enable_loop_soon_any_context();
    return;
  }
  std::memcpy(this->fragment_storage_ + this->fragment_length_, event->data_ptr, event->data_len);
  this->fragment_length_ += event->data_len;
  if (this->fragment_length_ == this->fragment_expected_) {
    this->enqueue_message_(this->fragment_binary_, this->fragment_storage_, this->fragment_length_);
    this->fragment_length_ = this->fragment_expected_ = 0;
  }
}

void NovaRealtime::process_socket_event_() {
  const int8_t event = this->socket_event_.exchange(0);
  if (event < 0) {
    this->hello_pending_ = false;
    this->disconnect_();
    return;
  }
  if (event > 0) {
    this->disconnect_();
    this->socket_connected_ = true;
    this->hello_pending_ = true;
  }
  if (this->hello_pending_ && this->socket_connected_ && this->queue_hello_())
    this->hello_pending_ = false;
}

bool NovaRealtime::queue_hello_() {
  JsonDocument hello;
  hello["type"] = "hello";
  hello["protocol"] = 2;
  hello["device_id"] = this->device_id_;
  char boot_id[9];
  std::snprintf(boot_id, sizeof(boot_id), "%08" PRIx32, esp_random());
  hello["boot_id"] = boot_id;
  hello["capabilities"]["remote_wake"] = 1;
  hello["capabilities"]["timer_alert"] = 1;
  hello["output_flow"]["mode"] = "played_window";
  hello["output_flow"]["max_inflight_samples"] = 7200;
  hello["output_flow"]["played_report_interval_samples"] = PLAYED_REPORT_INTERVAL;
  std::string encoded;
  serializeJson(hello, encoded);
  return this->queue_control_(encoded);
}

bool NovaRealtime::session_matches_(const char *session_id) const {
  return session_id != nullptr && !this->current_session_id_.empty() && this->current_session_id_ == session_id;
}

void NovaRealtime::handle_control_(const uint8_t *payload, size_t length) {
  JsonDocument document;
  DeserializationError error = deserializeJson(document, payload, length);
  if (error) {
    this->send_error_("invalid_gateway_message", error.c_str());
    return;
  }
  const char *type = document["type"] | "";
  if (std::strcmp(type, "hello.ack") == 0) {
    if ((document["protocol"] | 0) != 2 || std::string(document["device_id"] | "") != this->device_id_ ||
        std::string(document["output_flow"]["mode"] | "") != "played_window") {
      this->send_error_("protocol_mismatch", "Gateway hello acknowledgement did not match this device");
      return;
    }
    this->gateway_ready_ = true;
    this->microphone_sequence_ = 0;
    this->microphone_sample_index_ = 0;
    this->expected_speaker_sequence_ = 0;
    this->speaker_sequence_initialized_ = false;
    this->connected_trigger_.trigger();
    JsonObjectConst wake_config = document["remote_wake"].as<JsonObjectConst>();
    if (wake_config.isNull()) {
      this->remote_wake_enabled_ = false;
      this->wake_generation_ = 0;
      this->stop_standby_("Unavailable");
      this->set_state_("offline");
    } else {
      this->apply_wake_configuration_(wake_config);
    }
  } else if (std::strcmp(type, "wake.configure") == 0) {
    this->apply_wake_configuration_(document.as<JsonObjectConst>());
  } else if (std::strcmp(type, "wake.detected") == 0) {
    const uint32_t generation = document["generation"] | uint32_t(0);
    const std::string session_id = document["session_id"] | "";
    const std::string wake_word = document["wake_word"] | "";
    if (generation != this->wake_generation_)
      return;
    if (!this->remote_wake_enabled_ || this->muted_ || this->session_state_ != SessionState::IDLE ||
        !this->pending_wake_session_id_.empty() || session_id.empty() || session_id.size() > 36 ||
        wake_word != "hey_nova") {
      JsonDocument response;
      response["type"] = "wake.rejected";
      response["generation"] = generation;
      response["session_id"] = session_id;
      response["reason"] = "device_busy";
      std::string encoded;
      serializeJson(response, encoded);
      this->queue_control_(encoded);
      return;
    }
    this->stop_standby_("Paused");
    this->pending_wake_session_id_ = session_id;
    this->pending_wake_word_ = wake_word;
    this->pending_wake_deadline_ = millis() + PENDING_WAKE_TIMEOUT_MS;
    this->remote_wake_trigger_.trigger(session_id, wake_word);
  } else if (std::strcmp(type, "wake.cancel") == 0) {
    const uint32_t generation = document["generation"] | uint32_t(0);
    const std::string session_id = document["session_id"] | "";
    if (generation == this->wake_generation_ && session_id == this->pending_wake_session_id_) {
      this->clear_pending_wake_();
      this->start_standby_();
    }
  } else if (std::strcmp(type, "ping") == 0) {
    this->send_pong_(document["timestamp_ms"] | uint64_t(0));
  } else if (std::strcmp(type, "timer.alert") == 0) {
    const std::string notification_id = document["notification_id"] | "";
    if (notification_id.size() != 36) {
      this->send_error_("invalid_timer_alert", "Timer alert notification ID is invalid");
      return;
    }
    if (this->timer_alert_active_ || this->session_state_ != SessionState::IDLE ||
        !this->pending_wake_session_id_.empty()) {
      JsonDocument response;
      response["type"] = "timer.alert_done";
      response["notification_id"] = notification_id;
      response["outcome"] = "failed";
      std::string encoded;
      serializeJson(response, encoded);
      this->queue_control_(encoded);
      return;
    }
    this->timer_alert_active_ = true;
    this->timer_alert_notification_id_ = notification_id;
    this->timer_alert_trigger_.trigger();
  } else if (std::strcmp(type, "state") == 0) {
    const char *session_id = document["session_id"] | "";
    if (this->current_session_id_.empty()) {
      this->stop_standby_("Paused");
      this->current_session_id_ = session_id;
      this->session_state_ = SessionState::STARTING;
      this->session_active_ = true;
      this->session_start_deadline_ = millis() + SESSION_START_TIMEOUT_MS;
      this->acquire_wifi_performance_();
      this->microphone_->start();
    }
    if (this->session_matches_(session_id)) {
      const char *phase = document["phase"] | "unknown";
      this->set_microphone_streaming_(!this->muted_);
      this->publish_wake_status_(this->muted_ ? "Muted" : "Paused");
      this->set_state_(phase);
    }
  } else if (std::strcmp(type, "session.started") == 0) {
    if (!this->session_matches_(document["session_id"] | ""))
      return;
    this->session_state_ = SessionState::ACTIVE;
    this->session_active_ = true;
  } else if (std::strcmp(type, "session.ended") == 0) {
    if (this->session_matches_(document["session_id"] | ""))
      this->reset_session_(this->gateway_ready_ ? "armed" : "offline");
  } else if (std::strcmp(type, "audio.begin") == 0) {
    if (!this->session_matches_(document["session_id"] | ""))
      return;
    const std::string next_item_id = document["item_id"] | "";
    if (next_item_id.empty()) {
      this->stop_session_("invalid_item", true);
      return;
    }
    if (!this->current_item_id_.empty())
      this->speaker_->stop();
    this->speaker_buffer_->reset();
    this->speaker_frame_length_ = this->speaker_frame_offset_ = 0;
    this->current_item_id_ = next_item_id;
    this->played_samples_ = 0;
    this->last_reported_samples_ = 0;
    this->expected_speaker_sample_index_ = 0;
    this->total_speaker_samples_ = 0;
    this->finish_requested_ = this->finish_called_ = this->drained_reported_ = false;
    this->flush_requested_ = false;
    this->speaker_started_ = false;
    this->speaker_source_empty_ = false;
    this->speaker_source_empty_reported_ = false;
    this->speaker_source_empty_since_ = 0;
    this->speaker_start_buffered_bytes_ = 0;
    this->speaker_last_callback_ms_.store(0, std::memory_order_relaxed);
    this->flush_item_id_.clear();
    this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, 24000));
  } else if (std::strcmp(type, "audio.end") == 0) {
    if (!this->session_matches_(document["session_id"] | "") ||
        this->current_item_id_ != std::string(document["item_id"] | "") || this->flush_requested_)
      return;
    const uint32_t total = document["total_samples"] | uint32_t(0);
    if (total != this->expected_speaker_sample_index_) {
      this->stop_session_("speaker_sample_mismatch", true);
      return;
    }
    this->total_speaker_samples_ = total;
    this->finish_requested_ = true;
    this->speaker_source_empty_ = false;
    this->speaker_source_empty_reported_ = false;
    this->speaker_source_empty_since_ = 0;
  } else if (std::strcmp(type, "audio.flush") == 0) {
    if (!this->session_matches_(document["session_id"] | "") ||
        this->current_item_id_ != std::string(document["item_id"] | ""))
      return;
    const std::string item_id = this->current_item_id_;
    this->speaker_buffer_->reset();
    this->speaker_frame_length_ = this->speaker_frame_offset_ = 0;
    this->speaker_->stop();
    this->speaker_started_ = false;
    this->speaker_source_empty_ = false;
    this->speaker_source_empty_reported_ = false;
    this->speaker_source_empty_since_ = 0;
    this->speaker_last_callback_ms_.store(0, std::memory_order_relaxed);
    this->flush_requested_ = true;
    this->flush_item_id_ = item_id;
    this->flush_last_played_samples_ = this->played_samples_.load(std::memory_order_relaxed);
    this->flush_quiet_since_ = millis();
    this->finish_requested_ = this->finish_called_ = false;
  } else if (std::strcmp(type, "error") == 0) {
    const char *session_id = document["session_id"] | "";
    if (*session_id == '\0' || this->session_matches_(session_id)) {
      this->send_error_(document["code"] | "gateway_error", document["message"] | "Gateway error");
      this->reset_session_(this->gateway_ready_ ? "armed" : "offline");
    }
  }
}

void NovaRealtime::handle_audio_(const uint8_t *payload, size_t length) {
  if (length < PROTOCOL_HEADER_SIZE || length > MAX_MESSAGE_BYTES || std::memcmp(payload, "NVR2", 4) != 0 ||
      payload[4] != 2 || payload[5] != 2 || payload[6] != 0 || payload[7] != 0 ||
      ((length - PROTOCOL_HEADER_SIZE) & 1) != 0) {
    this->stop_session_("invalid_audio_frame", true);
    return;
  }
  if (this->current_item_id_.empty() || this->flush_requested_)
    return;
  const uint32_t sequence = read_be32(payload + 8);
  const uint32_t sample_index = read_be32(payload + 12);
  if ((!this->speaker_sequence_initialized_ && sequence != 0) ||
      (this->speaker_sequence_initialized_ && sequence != this->expected_speaker_sequence_) ||
      sample_index != this->expected_speaker_sample_index_) {
    this->stop_session_("speaker_sequence_discontinuity", true);
    return;
  }
  const size_t pcm_size = length - PROTOCOL_HEADER_SIZE;
  if (this->speaker_buffer_->write_without_replacement(payload + PROTOCOL_HEADER_SIZE, pcm_size, 0, false) != pcm_size) {
    this->stop_session_("speaker_overrun", true);
    return;
  }
  this->speaker_sequence_initialized_ = true;
  this->expected_speaker_sequence_ = sequence + 1;
  this->expected_speaker_sample_index_ += pcm_size / 2;
  this->speaker_high_water_bytes_ =
      std::max<uint32_t>(this->speaker_high_water_bytes_, this->speaker_buffer_->available());
}

void NovaRealtime::process_speaker_() {
  if (this->current_item_id_.empty() || this->flush_requested_)
    return;
  if (!this->speaker_started_) {
    const size_t buffered = this->speaker_buffer_->available();
    if (buffered == 0 || (buffered < SPEAKER_START_BUFFER_BYTES && !this->finish_requested_))
      return;
    this->speaker_start_buffered_bytes_ = buffered;
    this->speaker_source_empty_ = false;
    this->speaker_source_empty_reported_ = false;
    this->speaker_source_empty_since_ = 0;
    this->speaker_last_callback_ms_.store(0, std::memory_order_relaxed);
    this->speaker_->start();
    this->speaker_started_ = true;
  }

  const uint32_t started = micros();
  size_t frames_started = this->speaker_frame_length_ > this->speaker_frame_offset_ ? 1 : 0;
  while (frames_started < SPEAKER_LOOP_FRAME_BUDGET && micros() - started < LOOP_BUDGET_US) {
    if (this->speaker_frame_offset_ >= this->speaker_frame_length_) {
      this->speaker_frame_length_ =
          this->speaker_buffer_->read(this->speaker_frame_.data(), SPEAKER_FRAME_BYTES, 0);
      this->speaker_frame_offset_ = 0;
      if (this->speaker_frame_length_ == 0) {
        if (!this->finish_requested_) {
          const uint32_t now = millis();
          if (!this->speaker_source_empty_) {
            this->speaker_source_empty_ = true;
            this->speaker_source_empty_reported_ = false;
            this->speaker_source_empty_since_ = now;
          } else if (!this->speaker_source_empty_reported_ &&
                     now - this->speaker_source_empty_since_ >= SPEAKER_SOURCE_EMPTY_CONFIRM_MS) {
            this->speaker_source_empty_reported_ = true;
            this->speaker_source_empty_transitions_++;
            ESP_LOGD(TAG, "Speaker source empty for at least one frame before audio.end");
          }
        }
        return;
      }
      this->speaker_source_empty_ = false;
      this->speaker_source_empty_reported_ = false;
      this->speaker_source_empty_since_ = 0;
      frames_started++;
    }

    const size_t remaining = this->speaker_frame_length_ - this->speaker_frame_offset_;
    const size_t written =
        this->speaker_->play(this->speaker_frame_.data() + this->speaker_frame_offset_, remaining, 0);
    if (written == 0) {
      this->speaker_zero_writes_++;
      return;
    }
    if (written < remaining)
      this->speaker_partial_writes_++;
    this->speaker_frame_offset_ += written;
    if (this->speaker_frame_offset_ >= this->speaker_frame_length_)
      this->speaker_frame_length_ = this->speaker_frame_offset_ = 0;
    if (written < remaining)
      return;
  }
}

void NovaRealtime::process_flush_() {
  if (!this->flush_requested_ || this->flush_item_id_.empty())
    return;
  const uint32_t now = millis();
  const uint32_t raw_played = this->played_samples_.load(std::memory_order_relaxed);
  if (raw_played != this->flush_last_played_samples_) {
    this->flush_last_played_samples_ = raw_played;
    this->flush_quiet_since_ = now;
  }
  if (!this->speaker_->is_stopped() || now - this->flush_quiet_since_ < FLUSH_QUIET_MS)
    return;
  const uint32_t played = std::min(raw_played, this->expected_speaker_sample_index_);
  JsonDocument response;
  response["type"] = "audio.flushed";
  response["session_id"] = this->current_session_id_;
  response["item_id"] = this->flush_item_id_;
  response["played_samples"] = played;
  std::string encoded;
  serializeJson(response, encoded);
  if (!this->queue_control_(encoded))
    return;
  this->current_item_id_.clear();
  this->flush_item_id_.clear();
  this->flush_requested_ = false;
}

void NovaRealtime::process_finish_() {
  if (!this->finish_requested_ || this->current_item_id_.empty() || this->flush_requested_)
    return;
  if (!this->finish_called_ && this->speaker_buffer_->available() == 0 && this->speaker_frame_length_ == 0) {
    this->speaker_->finish();
    this->finish_called_ = true;
  }
  if (this->finish_called_ && this->speaker_->is_stopped() && !this->drained_reported_) {
    this->speaker_started_ = false;
    this->speaker_source_empty_ = false;
    this->speaker_source_empty_reported_ = false;
    this->speaker_source_empty_since_ = 0;
    this->speaker_last_callback_ms_.store(0, std::memory_order_relaxed);
    // The resampler callback counts source-equivalent DAC frames. Its filter
    // shutdown may under-count the original 24 kHz input by a small amount,
    // even though every buffer has drained. At this point graceful finish is
    // authoritative, so report the canonical input position.
    this->played_samples_.store(this->total_speaker_samples_, std::memory_order_relaxed);
    this->report_played_(true);
    JsonDocument report;
    report["type"] = "audio.drained";
    report["session_id"] = this->current_session_id_;
    report["item_id"] = this->current_item_id_;
    report["played_samples"] = this->total_speaker_samples_;
    std::string encoded;
    serializeJson(report, encoded);
    if (this->queue_control_(encoded)) {
      this->drained_reported_ = true;
      this->current_item_id_.clear();
    }
  }
}

void NovaRealtime::handle_microphone_data_(const std::vector<uint8_t> &data) {
  if (!this->microphone_streaming_ || data.empty() || this->microphone_buffer_ == nullptr)
    return;
  const uint32_t callback_count = this->microphone_callback_count_.fetch_add(1, std::memory_order_relaxed);
  if ((callback_count & 0x3FU) == 0) {
    const uint32_t stack_margin = uxTaskGetStackHighWaterMark(nullptr);
    uint32_t observed_stack_margin =
        this->microphone_callback_stack_low_water_bytes_.load(std::memory_order_relaxed);
    while (stack_margin < observed_stack_margin &&
           !this->microphone_callback_stack_low_water_bytes_.compare_exchange_weak(
               observed_stack_margin, stack_margin, std::memory_order_relaxed)) {
    }
  }
  const uint8_t *source = data.data();
  size_t length = data.size();
  if (length > MICROPHONE_BUFFER_BYTES) {
    source += length - MICROPHONE_BUFFER_BYTES;
    length = MICROPHONE_BUFFER_BYTES;
    this->microphone_discontinuity_ = true;
    this->microphone_drops_pending_.fetch_add(1);
  }
  LockGuard lock(this->microphone_mutex_);
  // The session may have ended or the microphone may have been muted while this
  // callback waited for the buffer.
  if (!this->microphone_streaming_)
    return;
  const size_t free_bytes = this->microphone_buffer_->free();
  if (free_bytes < length) {
    const size_t removed = length - free_bytes;
    this->microphone_discontinuity_ = true;
    this->microphone_drops_pending_.fetch_add((removed + MICROPHONE_FRAME_BYTES - 1) / MICROPHONE_FRAME_BYTES);
  }
  if (this->microphone_buffer_->write(source, length) != length) {
    this->microphone_discontinuity_ = true;
    this->microphone_drops_pending_.fetch_add(1);
  }
  this->microphone_high_water_bytes_ =
      std::max<uint32_t>(this->microphone_high_water_bytes_, this->microphone_buffer_->available());
  if (this->tx_task_handle_.is_created())
    xTaskNotifyGive(this->tx_task_handle_.get_handle());
}

void NovaRealtime::tx_task_(void *parameter) { static_cast<NovaRealtime *>(parameter)->run_tx_task_(); }

void NovaRealtime::run_tx_task_() {
  while (!this->tx_stop_) {
    if (this->control_queue_ != nullptr && xQueueReceive(this->control_queue_, &this->tx_control_, 0) == pdTRUE) {
      if (this->socket_connected_ && this->client_ != nullptr) {
        int result =
            esp_websocket_client_send_text(this->client_, this->tx_control_.data.data(), this->tx_control_.length,
                                           pdMS_TO_TICKS(50));
        if (result < 0) {
          ESP_LOGW(TAG, "Could not send gateway control frame");
          this->socket_connected_ = false;
          this->tx_transport_fault_ = true;
          this->enable_loop_soon_any_context();
        }
      }
      continue;
    }
    bool microphone_ready = false;
    if (this->microphone_buffer_ != nullptr) {
      LockGuard lock(this->microphone_mutex_);
      microphone_ready = this->microphone_buffer_->available() >= MICROPHONE_FRAME_BYTES;
    }
    if (this->gateway_ready_ && this->microphone_streaming_ && this->socket_connected_ &&
        microphone_ready) {
      this->send_audio_from_task_();
      continue;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
  }
  // Keep the task handle valid until StaticTask::deallocate() owns teardown.
  vTaskSuspend(nullptr);
}

void NovaRealtime::send_audio_from_task_() {
  if (!this->microphone_streaming_)
    return;
  auto &frame = this->tx_audio_frame_;
  {
    LockGuard lock(this->microphone_mutex_);
    if (this->microphone_buffer_->read(frame.data() + PROTOCOL_HEADER_SIZE, MICROPHONE_FRAME_BYTES, 0) !=
        MICROPHONE_FRAME_BYTES)
      return;
  }
  std::memcpy(frame.data(), "NVR2", 4);
  frame[4] = 2;
  frame[5] = 1;
  const bool discontinuity = this->microphone_discontinuity_.exchange(false);
  frame[6] = 0;
  frame[7] = discontinuity ? 1 : 0;
  write_be32(frame.data() + 8, this->microphone_sequence_++);
  write_be32(frame.data() + 12, this->microphone_sample_index_);
  this->microphone_sample_index_ += MICROPHONE_FRAME_BYTES / 2;
  int result = esp_websocket_client_send_bin(this->client_, reinterpret_cast<const char *>(frame.data()), frame.size(),
                                             pdMS_TO_TICKS(50));
  if (result < 0) {
    this->microphone_discontinuity_ = true;
    ESP_LOGW(TAG, "Could not send microphone frame");
    this->socket_connected_ = false;
    this->tx_transport_fault_ = true;
    this->enable_loop_soon_any_context();
  }
}

bool NovaRealtime::queue_control_(const std::string &payload) {
  if (this->control_queue_ == nullptr || payload.empty() || payload.size() > MAX_OUTGOING_CONTROL_BYTES ||
      !this->socket_connected_)
    return false;
  OutgoingControl control;
  control.length = payload.size();
  std::memcpy(control.data.data(), payload.data(), payload.size());
  if (xQueueSend(this->control_queue_, &control, 0) != pdTRUE) {
    this->control_overrun_ = true;
    this->enable_loop_soon_any_context();
    return false;
  }
  if (this->tx_task_handle_.is_created())
    xTaskNotifyGive(this->tx_task_handle_.get_handle());
  return true;
}

void NovaRealtime::report_played_(bool force) {
  if (this->current_item_id_.empty() || !this->gateway_ready_)
    return;
  const uint32_t played = std::min(this->played_samples_.load(std::memory_order_relaxed),
                                   this->expected_speaker_sample_index_);
  if (!force && played - this->last_reported_samples_ < PLAYED_REPORT_INTERVAL)
    return;
  JsonDocument report;
  report["type"] = "audio.played";
  report["session_id"] = this->current_session_id_;
  report["item_id"] = this->current_item_id_;
  report["played_samples"] = played;
  std::string encoded;
  serializeJson(report, encoded);
  if (this->queue_control_(encoded))
    this->last_reported_samples_ = played;
}

void NovaRealtime::start_session(const std::string &wake_word) {
  if (!this->enabled_)
    return;
  if (!this->gateway_ready_) {
    this->send_error_("gateway_unavailable", "Realtime gateway is not connected");
    return;
  }
  if (this->session_state_ != SessionState::IDLE || !this->pending_wake_session_id_.empty() ||
      this->timer_alert_active_ || this->muted_)
    return;
  this->stop_standby_("Paused");
  this->current_session_id_ = this->new_session_id_();
  this->session_state_ = SessionState::STARTING;
  this->session_active_ = true;
  this->session_start_deadline_ = millis() + SESSION_START_TIMEOUT_MS;
  {
    LockGuard lock(this->microphone_mutex_);
    this->microphone_buffer_->reset();
  }
  this->microphone_drops_pending_ = 0;
  this->microphone_discontinuity_ = true;
  this->microphone_drop_window_started_ = millis();
  this->microphone_drop_window_count_ = 0;
  this->microphone_callback_count_ = 0;
  this->microphone_callback_stack_low_water_bytes_ = 0xFFFFFFFFUL;
  this->session_stack_reported_ = false;
  this->set_microphone_streaming_(true);
  this->standby_active_ = false;
  this->acquire_wifi_performance_();
  JsonDocument request;
  request["type"] = "session.start";
  request["session_id"] = this->current_session_id_;
  request["wake_word"] = wake_word;
  std::string encoded;
  serializeJson(request, encoded);
  if (!this->queue_control_(encoded)) {
    this->reset_session_("armed");
    return;
  }
  this->microphone_->start();
  this->publish_wake_status_("Paused");
  this->set_state_("connecting");
}

void NovaRealtime::accept_wake() {
  if (this->pending_wake_session_id_.empty() || !this->gateway_ready_ || !this->remote_wake_enabled_ || this->muted_)
    return;
  const std::string session_id = this->pending_wake_session_id_;
  this->current_session_id_ = session_id;
  this->clear_pending_wake_();
  this->session_state_ = SessionState::STARTING;
  this->session_active_ = true;
  this->session_start_deadline_ = millis() + SESSION_START_TIMEOUT_MS;
  {
    LockGuard lock(this->microphone_mutex_);
    this->microphone_buffer_->reset();
  }
  this->microphone_drops_pending_ = 0;
  this->microphone_discontinuity_ = true;
  this->microphone_drop_window_started_ = millis();
  this->microphone_drop_window_count_ = 0;
  this->microphone_callback_count_ = 0;
  this->microphone_callback_stack_low_water_bytes_ = 0xFFFFFFFFUL;
  this->session_stack_reported_ = false;
  JsonDocument response;
  response["type"] = "wake.accepted";
  response["generation"] = this->wake_generation_;
  response["session_id"] = session_id;
  std::string encoded;
  serializeJson(response, encoded);
  if (!this->queue_control_(encoded)) {
    this->reset_session_("armed");
    return;
  }
  this->standby_active_ = false;
  this->set_microphone_streaming_(true);
  this->acquire_wifi_performance_();
  this->microphone_->start();
  this->publish_wake_status_("Paused");
  this->set_state_("connecting");
}

void NovaRealtime::reject_wake(const std::string &reason) {
  if (this->pending_wake_session_id_.empty())
    return;
  JsonDocument response;
  response["type"] = "wake.rejected";
  response["generation"] = this->wake_generation_;
  response["session_id"] = this->pending_wake_session_id_;
  response["reason"] = reason;
  std::string encoded;
  serializeJson(response, encoded);
  this->queue_control_(encoded);
  this->clear_pending_wake_();
  this->start_standby_();
}

void NovaRealtime::stop_session(const std::string &reason) {
  if (!this->pending_wake_session_id_.empty()) {
    this->reject_wake(reason);
    return;
  }
  this->stop_session_(reason, false);
}

void NovaRealtime::complete_timer_alert(const std::string &outcome) {
  if (!this->timer_alert_active_)
    return;
  if (outcome != "played" && outcome != "dismissed" && outcome != "failed") {
    this->send_error_("invalid_timer_alert_outcome", "Timer alert outcome is invalid");
    return;
  }
  this->send_timer_alert_done_(outcome);
}

void NovaRealtime::stop_session_(const std::string &reason, bool error) {
  if (this->session_state_ == SessionState::IDLE)
    return;
  this->session_state_ = SessionState::STOPPING;
  JsonDocument request;
  request["type"] = "session.stop";
  request["session_id"] = this->current_session_id_;
  request["reason"] = reason;
  request["result"] = error ? "error" : "cancelled";
  std::string encoded;
  serializeJson(request, encoded);
  this->queue_control_(encoded);
  this->reset_session_(this->gateway_ready_ ? "armed" : "offline");
}

void NovaRealtime::reset_session_(const std::string &phase) {
  this->session_active_ = false;
  this->session_state_ = SessionState::IDLE;
  this->standby_active_ = false;
  this->set_microphone_streaming_(false);
  if (this->microphone_ != nullptr)
    this->microphone_->stop();
  if (this->speaker_ != nullptr)
    this->speaker_->stop();
  if (this->microphone_buffer_ != nullptr) {
    LockGuard lock(this->microphone_mutex_);
    this->microphone_buffer_->reset();
  }
  if (this->speaker_buffer_ != nullptr)
    this->speaker_buffer_->reset();
  this->speaker_frame_length_ = this->speaker_frame_offset_ = 0;
  this->speaker_started_ = false;
  this->speaker_source_empty_ = false;
  this->speaker_source_empty_reported_ = false;
  this->speaker_source_empty_since_ = 0;
  this->speaker_last_callback_ms_.store(0, std::memory_order_relaxed);
  this->finish_requested_ = this->finish_called_ = this->drained_reported_ = false;
  this->flush_requested_ = false;
  this->flush_item_id_.clear();
  this->current_item_id_.clear();
  this->current_session_id_.clear();
  this->clear_pending_wake_();
  this->release_wifi_performance_();
  if (phase != "offline" && this->gateway_ready_ && this->remote_wake_enabled_ && !this->muted_)
    this->start_standby_();
  else {
    this->publish_wake_status_(this->muted_ ? "Muted" : "Unavailable");
    this->set_state_(phase);
  }
}

void NovaRealtime::start_standby_() {
  if (!this->gateway_ready_ || !this->remote_wake_enabled_) {
    this->stop_standby_("Unavailable");
    this->set_state_("offline");
    return;
  }
  if (this->muted_) {
    this->stop_standby_("Muted");
    return;
  }
  if (this->session_state_ != SessionState::IDLE || !this->pending_wake_session_id_.empty())
    return;
  {
    LockGuard lock(this->microphone_mutex_);
    this->microphone_buffer_->reset();
  }
  this->microphone_discontinuity_ = true;
  this->microphone_drops_pending_ = 0;
  this->microphone_drop_window_started_ = millis();
  this->microphone_drop_window_count_ = 0;
  this->standby_active_ = true;
  this->set_microphone_streaming_(true);
  this->acquire_wifi_performance_();
  this->send_wake_status_("armed");
  this->microphone_->start();
  this->publish_wake_status_("Listening");
  this->set_state_("armed");
}

void NovaRealtime::stop_standby_(const std::string &status) {
  this->standby_active_ = false;
  this->set_microphone_streaming_(false);
  if (!this->session_active_ && this->microphone_ != nullptr)
    this->microphone_->stop();
  if (this->gateway_ready_ && this->wake_generation_ != 0)
    this->send_wake_status_(status == "Unavailable" ? "unavailable" : "suspended");
  if (!this->session_active_ && status != "Paused")
    this->release_wifi_performance_();
  this->publish_wake_status_(status);
}

void NovaRealtime::apply_wake_configuration_(JsonObjectConst config) {
  const uint32_t generation = config["generation"] | uint32_t(0);
  if ((config["version"] | 0) != 1 || generation == 0)
    return;
  if (this->wake_generation_ != 0 && generation <= this->wake_generation_)
    return;
  const bool active_session = this->session_active_;
  if (!active_session) {
    this->set_microphone_streaming_(false);
    if (this->microphone_ != nullptr)
      this->microphone_->stop();
    this->standby_active_ = false;
  }
  this->wake_generation_ = generation;
  JsonArrayConst languages = config["languages"].as<JsonArrayConst>();
  const bool bilingual = languages.size() == 2 && std::string(languages[0] | "") == "en" &&
                         std::string(languages[1] | "") == "de";
  this->remote_wake_enabled_ = bool(config["enabled"] | false) &&
                               std::string(config["wake_word"] | "") == "hey_nova" && bilingual;
  if (!this->remote_wake_enabled_) {
    if (active_session) {
      this->send_wake_status_("unavailable");
      this->publish_wake_status_("Unavailable");
    } else {
      this->stop_standby_("Unavailable");
      this->set_state_("offline");
    }
    return;
  }
  if (active_session) {
    this->send_wake_status_("suspended");
    this->publish_wake_status_(this->muted_ ? "Muted" : "Paused");
    return;
  }
  this->start_standby_();
}

void NovaRealtime::send_wake_status_(const std::string &state) {
  if (!this->socket_connected_ || this->wake_generation_ == 0)
    return;
  JsonDocument status;
  status["type"] = "wake.status";
  status["generation"] = this->wake_generation_;
  status["state"] = state;
  std::string encoded;
  serializeJson(status, encoded);
  this->queue_control_(encoded);
}

void NovaRealtime::publish_wake_status_(const std::string &status) {
  if (status == this->last_wake_status_)
    return;
  this->last_wake_status_ = status;
  if (this->wake_word_status_ != nullptr)
    this->wake_word_status_->publish_state(status);
}

void NovaRealtime::clear_pending_wake_() {
  this->pending_wake_session_id_.clear();
  this->pending_wake_word_.clear();
  this->pending_wake_deadline_ = 0;
}

void NovaRealtime::set_muted(bool muted) {
  if (this->muted_ == muted)
    return;
  this->muted_ = muted;
  if (muted) {
    if (!this->pending_wake_session_id_.empty())
      this->reject_wake("muted");
    this->set_microphone_streaming_(false);
    this->standby_active_ = false;
    if (this->microphone_ != nullptr)
      this->microphone_->stop();
    if (!this->session_active_)
      this->release_wifi_performance_();
    this->send_wake_status_("suspended");
    this->publish_wake_status_("Muted");
    return;
  }
  if (this->session_active_) {
    this->microphone_discontinuity_ = true;
    this->microphone_->start();
    this->set_microphone_streaming_(true);
    this->send_wake_status_("suspended");
    this->publish_wake_status_("Paused");
  } else {
    this->start_standby_();
  }
}

void NovaRealtime::acquire_wifi_performance_() {
  if (this->wifi_performance_owned_ || wifi::global_wifi_component == nullptr)
    return;
  if (wifi::global_wifi_component->request_high_performance()) {
    wifi::global_wifi_component->request_roaming_suppression();
    this->wifi_performance_owned_ = true;
  }
}

void NovaRealtime::release_wifi_performance_() {
  if (!this->wifi_performance_owned_ || wifi::global_wifi_component == nullptr)
    return;
  wifi::global_wifi_component->release_roaming_suppression();
  wifi::global_wifi_component->release_high_performance();
  this->wifi_performance_owned_ = false;
}

std::string NovaRealtime::new_session_id_() {
  uint32_t a = esp_random();
  uint32_t b = esp_random();
  uint32_t c = esp_random();
  uint32_t d = esp_random();
  b = (b & 0xFFFF0FFFU) | 0x00004000U;
  c = (c & 0x3FFFFFFFU) | 0x80000000U;
  char value[37];
  std::snprintf(value, sizeof(value), "%08" PRIx32 "-%04" PRIx16 "-%04" PRIx16 "-%04" PRIx16 "-%04" PRIx16 "%08" PRIx32,
                a, uint16_t(b >> 16), uint16_t(b), uint16_t(c >> 16), uint16_t(c), d);
  return value;
}

void NovaRealtime::send_pong_(uint64_t timestamp_ms) {
  uint32_t rx_high_water_bytes;
  {
    LockGuard lock(this->incoming_mutex_);
    rx_high_water_bytes = this->rx_high_water_bytes_;
  }
  uint32_t microphone_high_water_bytes;
  {
    LockGuard lock(this->microphone_mutex_);
    microphone_high_water_bytes = this->microphone_high_water_bytes_;
  }
  JsonDocument pong;
  pong["type"] = "pong";
  pong["timestamp_ms"] = timestamp_ms;
  JsonObject diagnostics = pong["diagnostics"].to<JsonObject>();
  diagnostics["free_heap_bytes"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  diagnostics["largest_free_block_bytes"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  diagnostics["free_psram_bytes"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  diagnostics["rx_high_water_bytes"] = rx_high_water_bytes;
  diagnostics["speaker_high_water_bytes"] = this->speaker_high_water_bytes_;
  diagnostics["speaker_source_empty_transitions"] = this->speaker_source_empty_transitions_;
  diagnostics["speaker_zero_writes"] = this->speaker_zero_writes_;
  diagnostics["speaker_partial_writes"] = this->speaker_partial_writes_;
  diagnostics["speaker_max_callback_gap_ms"] =
      this->speaker_max_callback_gap_ms_.load(std::memory_order_relaxed);
  diagnostics["speaker_start_buffered_bytes"] = this->speaker_start_buffered_bytes_;
  diagnostics["mic_high_water_bytes"] = microphone_high_water_bytes;
  diagnostics["loop_max_us"] = this->max_loop_us_;
  diagnostics["microphone_drops"] = this->microphone_drops_total_;
  diagnostics["transport_faults"] = this->transport_faults_total_;
  diagnostics["reconnects"] = this->reconnect_count_.load(std::memory_order_relaxed);
  diagnostics["wake_cue"] = this->last_wake_cue_;
  diagnostics["shared_output_volume_per_mille"] = this->shared_output_volume_per_mille_;
  diagnostics["shared_output_muted"] = this->shared_output_muted_ ? 1 : 0;
  diagnostics["tx_stack_low_water_bytes"] =
      this->tx_task_handle_.is_created()
          ? uxTaskGetStackHighWaterMark(this->tx_task_handle_.get_handle())
          : 0;
  const uint32_t microphone_stack_margin =
      this->microphone_callback_stack_low_water_bytes_.load(std::memory_order_relaxed);
  diagnostics["mic_stack_low_water_bytes"] =
      microphone_stack_margin == 0xFFFFFFFFUL ? 0 : microphone_stack_margin;
  diagnostics["uptime_ms"] = millis();
  std::string encoded;
  serializeJson(pong, encoded);
  this->queue_control_(encoded);
}

void NovaRealtime::send_timer_alert_done_(const std::string &outcome) {
  JsonDocument response;
  response["type"] = "timer.alert_done";
  response["notification_id"] = this->timer_alert_notification_id_;
  response["outcome"] = outcome;
  std::string encoded;
  serializeJson(response, encoded);
  this->queue_control_(encoded);
  this->timer_alert_active_ = false;
  this->timer_alert_notification_id_.clear();
}

void NovaRealtime::send_error_(const std::string &code, const std::string &message) {
  ESP_LOGW(TAG, "%s: %s", code.c_str(), message.c_str());
  this->error_trigger_.trigger(code, message);
}

void NovaRealtime::set_microphone_streaming_(bool enabled) {
  if (this->microphone_streaming_.exchange(enabled) == enabled)
    return;
  if (!enabled && this->microphone_buffer_ != nullptr) {
    LockGuard lock(this->microphone_mutex_);
    this->microphone_buffer_->reset();
  }
  if (this->tx_task_handle_.is_created())
    xTaskNotifyGive(this->tx_task_handle_.get_handle());
}

void NovaRealtime::set_state_(const std::string &phase) {
  if (phase == this->last_phase_)
    return;
  this->last_phase_ = phase;
  ESP_LOGD(TAG, "State: %s", phase.c_str());
  this->state_trigger_.trigger(phase);
}

}  // namespace esphome::nova_realtime
