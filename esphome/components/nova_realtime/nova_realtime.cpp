#include "nova_realtime.h"

#include <algorithm>
#include <cstring>

#include <ArduinoJson.h>
#include <mbedtls/md.h>

#include "esphome/components/network/util.h"
#include "esphome/core/log.h"

namespace esphome::nova_realtime {

static const char *const TAG = "nova_realtime";
static constexpr size_t PROTOCOL_HEADER_SIZE = 16;
static constexpr size_t MICROPHONE_FRAME_BYTES = 640;  // 20 ms, 16 kHz, mono PCM16
static constexpr size_t MAX_MICROPHONE_BUFFER = 16000;  // 500 ms startup prebuffer
static constexpr size_t MAX_INCOMING_MESSAGES = 32;
static constexpr size_t MAX_SPEAKER_BUFFER = 96000;  // two seconds at 24 kHz PCM16
static constexpr uint32_t PLAYED_REPORT_INTERVAL = 2400;  // 100 ms at 24 kHz

static void write_be32(uint8_t *data, uint32_t value) {
  data[0] = uint8_t(value >> 24);
  data[1] = uint8_t(value >> 16);
  data[2] = uint8_t(value >> 8);
  data[3] = uint8_t(value);
}

void NovaRealtime::setup() {
  this->microphone_->add_data_callback(
      [this](const std::vector<uint8_t> &data) { this->handle_microphone_data_(data); });
  this->speaker_->add_audio_output_callback(
      [this](uint32_t frames, int64_t) { this->played_samples_.fetch_add(frames, std::memory_order_relaxed); });
}

void NovaRealtime::dump_config() {
  ESP_LOGCONFIG(TAG, "NOVA Realtime:");
  ESP_LOGCONFIG(TAG, "  Gateway: %s", this->gateway_url_.c_str());
  ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
  ESP_LOGCONFIG(TAG, "  TLS CA configured: %s", YESNO(!this->ca_certificate_.empty()));
}

void NovaRealtime::loop() {
  if (network::is_connected() && this->client_ == nullptr && millis() >= this->next_connect_at_) {
    this->connect_();
  }

  if (this->incoming_overrun_.exchange(false)) {
    {
      LockGuard lock(this->incoming_mutex_);
      this->incoming_.clear();
    }
    this->send_error_("incoming_overrun", "Gateway receive queue exceeded its safe bound");
    this->stop_session("incoming_overrun");
  }

  while (true) {
    IncomingMessage message;
    {
      LockGuard lock(this->incoming_mutex_);
      if (this->incoming_.empty())
        break;
      message = std::move(this->incoming_.front());
      this->incoming_.pop_front();
    }
    if (message.binary) {
      this->handle_audio_(message.data);
    } else {
      this->handle_control_(std::string(message.data.begin(), message.data.end()));
    }
  }

  this->process_speaker_();
  this->process_microphone_();
  this->report_played_();
}

void NovaRealtime::connect_() {
  esp_websocket_client_config_t config{};
  config.uri = this->gateway_url_.c_str();
  if (!this->ca_certificate_.empty())
    config.cert_pem = this->ca_certificate_.c_str();
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
    esp_websocket_client_destroy(this->client_);
    this->client_ = nullptr;
    this->next_connect_at_ = millis() + 5000;
  }
}

void NovaRealtime::disconnect_() {
  bool notify = this->authenticated_;
  this->socket_connected_ = false;
  this->authenticated_ = false;
  if (this->session_active_) {
    this->session_active_ = false;
    this->microphone_->stop();
    this->speaker_->stop();
    this->current_item_id_.clear();
    this->set_state_("offline");
  }
  if (notify)
    this->disconnected_trigger_.trigger();
}

void NovaRealtime::websocket_event_handler_(void *handler_args, esp_event_base_t, int32_t event_id, void *event_data) {
  static_cast<NovaRealtime *>(handler_args)->handle_websocket_event_(
      event_id, static_cast<esp_websocket_event_data_t *>(event_data));
}

void NovaRealtime::handle_websocket_event_(int32_t event_id, esp_websocket_event_data_t *event) {
  if (event_id == WEBSOCKET_EVENT_CONNECTED) {
    this->socket_connected_ = true;
    ESP_LOGI(TAG, "Gateway socket connected");
    return;
  }
  if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED) {
    this->socket_connected_ = false;
    LockGuard lock(this->incoming_mutex_);
    static const char disconnected[] = "{\"type\":\"__socket.disconnected\"}";
    this->incoming_.push_back({false, std::vector<uint8_t>(disconnected, disconnected + sizeof(disconnected) - 1)});
    this->enable_loop_soon_any_context();
    return;
  }
  if (event_id == WEBSOCKET_EVENT_ERROR) {
    ESP_LOGW(TAG, "Gateway WebSocket error");
    return;
  }
  if (event_id != WEBSOCKET_EVENT_DATA || event == nullptr || event->data_ptr == nullptr || event->data_len <= 0)
    return;

  LockGuard lock(this->incoming_mutex_);
  if (event->payload_offset == 0) {
    this->fragmented_message_.clear();
    this->fragmented_message_.reserve(event->payload_len);
    this->fragmented_binary_ = event->op_code == 0x2;
  }
  const auto *begin = reinterpret_cast<const uint8_t *>(event->data_ptr);
  this->fragmented_message_.insert(this->fragmented_message_.end(), begin, begin + event->data_len);
  if (event->payload_offset + event->data_len >= event->payload_len) {
    if (this->incoming_.size() >= MAX_INCOMING_MESSAGES) {
      this->incoming_overrun_ = true;
    } else {
      this->incoming_.push_back({this->fragmented_binary_, std::move(this->fragmented_message_)});
    }
    this->fragmented_message_.clear();
    this->enable_loop_soon_any_context();
  }
}

void NovaRealtime::handle_control_(const std::string &payload) {
  JsonDocument document;
  DeserializationError error = deserializeJson(document, payload);
  if (error) {
    this->send_error_("invalid_gateway_message", error.c_str());
    return;
  }
  std::string type = document["type"] | "";
  if (type == "__socket.disconnected") {
    this->disconnect_();
  } else if (type == "auth.challenge") {
    std::string challenge = document["challenge"] | "";
    JsonDocument response;
    response["type"] = "auth.response";
    response["device_id"] = this->device_id_;
    response["digest"] = this->authentication_digest_(challenge);
    std::string encoded;
    serializeJson(response, encoded);
    this->send_control_(encoded);
  } else if (type == "auth.ok") {
    this->authenticated_ = true;
    this->microphone_sequence_ = 0;
    this->microphone_sample_index_ = 0;
    this->send_control_("{\"type\":\"hello\",\"protocol\":1}");
    this->connected_trigger_.trigger();
    this->set_state_("armed");
  } else if (type == "ping") {
    this->send_control_("{\"type\":\"pong\"}");
  } else if (type == "session.started") {
    this->session_active_ = true;
  } else if (type == "session.ended") {
    this->report_played_(true);
    this->session_active_ = false;
    this->microphone_->stop();
    this->speaker_->stop();
    this->current_item_id_.clear();
    this->set_state_("armed");
  } else if (type == "state") {
    this->set_state_(document["phase"] | "unknown");
  } else if (type == "audio.begin") {
    std::string next_item_id = document["item_id"] | "";
    if (!this->current_item_id_.empty() && this->current_item_id_ != next_item_id)
      this->speaker_->stop();
    this->current_item_id_ = std::move(next_item_id);
    this->played_samples_.store(0, std::memory_order_relaxed);
    this->last_reported_samples_ = 0;
    this->speaker_pending_.clear();
    this->speaker_pending_offset_ = 0;
    this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, 24000));
    this->speaker_->start();
  } else if (type == "audio.end") {
    this->report_played_(true);
    this->speaker_->finish();
  } else if (type == "audio.flush") {
    std::string item_id = document["item_id"] | this->current_item_id_;
    this->speaker_pending_.clear();
    this->speaker_pending_offset_ = 0;
    this->speaker_->stop();
    JsonDocument response;
    response["type"] = "audio.flushed";
    response["item_id"] = item_id;
    response["played_samples"] = this->played_samples_.load(std::memory_order_relaxed);
    std::string encoded;
    serializeJson(response, encoded);
    this->send_control_(encoded);
    this->current_item_id_.clear();
  } else if (type == "error") {
    this->send_error_(document["code"] | "gateway_error", document["message"] | "Gateway error");
    if (this->session_active_) {
      this->session_active_ = false;
      this->microphone_->stop();
      this->speaker_->stop();
    }
  }
}

void NovaRealtime::handle_audio_(const std::vector<uint8_t> &payload) {
  if (payload.size() < PROTOCOL_HEADER_SIZE || payload.size() > 2048 ||
      std::memcmp(payload.data(), "NVR1", 4) != 0 || payload[4] != 1 || payload[5] != 2 ||
      ((payload.size() - PROTOCOL_HEADER_SIZE) & 1) != 0) {
    this->send_error_("invalid_audio_frame", "Rejected malformed speaker frame");
    return;
  }
  if (this->current_item_id_.empty())
    return;
  const size_t pcm_size = payload.size() - PROTOCOL_HEADER_SIZE;
  const size_t pending_size = this->speaker_pending_.size() - this->speaker_pending_offset_;
  if (pending_size + pcm_size > MAX_SPEAKER_BUFFER) {
    this->send_error_("speaker_overrun", "Speaker buffer exceeded two seconds");
    this->stop_session("speaker_overrun");
    return;
  }
  if (this->speaker_pending_offset_ > 0) {
    this->speaker_pending_.erase(this->speaker_pending_.begin(),
                                 this->speaker_pending_.begin() + this->speaker_pending_offset_);
    this->speaker_pending_offset_ = 0;
  }
  this->speaker_pending_.insert(this->speaker_pending_.end(), payload.begin() + PROTOCOL_HEADER_SIZE, payload.end());
  this->process_speaker_();
}

void NovaRealtime::process_speaker_() {
  if (this->speaker_pending_.empty())
    return;
  size_t written = this->speaker_->play(this->speaker_pending_.data() + this->speaker_pending_offset_,
                                        this->speaker_pending_.size() - this->speaker_pending_offset_,
                                        pdMS_TO_TICKS(20));
  this->speaker_pending_offset_ += written;
  if (this->speaker_pending_offset_ >= this->speaker_pending_.size()) {
    this->speaker_pending_.clear();
    this->speaker_pending_offset_ = 0;
  }
}

void NovaRealtime::handle_microphone_data_(const std::vector<uint8_t> &data) {
  if (!this->session_active_)
    return;
  LockGuard lock(this->microphone_mutex_);
  size_t overflow = this->microphone_buffer_.size() + data.size() > MAX_MICROPHONE_BUFFER
                        ? this->microphone_buffer_.size() + data.size() - MAX_MICROPHONE_BUFFER
                        : 0;
  if (overflow >= this->microphone_buffer_.size()) {
    this->microphone_buffer_.clear();
  } else if (overflow > 0) {
    this->microphone_buffer_.erase(this->microphone_buffer_.begin(), this->microphone_buffer_.begin() + overflow);
  }
  this->microphone_buffer_.insert(this->microphone_buffer_.end(), data.begin(), data.end());
  this->enable_loop_soon_any_context();
}

void NovaRealtime::process_microphone_() {
  if (!this->authenticated_ || !this->session_active_ || !this->socket_connected_)
    return;
  uint8_t frame[MICROPHONE_FRAME_BYTES];
  {
    LockGuard lock(this->microphone_mutex_);
    if (this->microphone_buffer_.size() < MICROPHONE_FRAME_BYTES)
      return;
    std::memcpy(frame, this->microphone_buffer_.data(), MICROPHONE_FRAME_BYTES);
    this->microphone_buffer_.erase(this->microphone_buffer_.begin(),
                                   this->microphone_buffer_.begin() + MICROPHONE_FRAME_BYTES);
  }
  this->send_audio_(frame, sizeof(frame));
}

void NovaRealtime::start_session(const std::string &wake_word) {
  if (!this->authenticated_) {
    this->send_error_("gateway_unavailable", "Realtime gateway is not connected");
    return;
  }
  if (this->session_active_)
    return;
  this->session_active_ = true;
  {
    LockGuard lock(this->microphone_mutex_);
    this->microphone_buffer_.clear();
  }
  this->microphone_->start();
  this->set_state_("connecting");
  JsonDocument request;
  request["type"] = "session.start";
  request["wake_word"] = wake_word;
  std::string encoded;
  serializeJson(request, encoded);
  this->send_control_(encoded);
}

void NovaRealtime::stop_session(const std::string &reason) {
  if (!this->session_active_)
    return;
  JsonDocument request;
  request["type"] = "session.stop";
  request["reason"] = reason;
  std::string encoded;
  serializeJson(request, encoded);
  this->send_control_(encoded);
  this->session_active_ = false;
  this->microphone_->stop();
  this->speaker_->stop();
  this->current_item_id_.clear();
  this->set_state_(this->authenticated_ ? "armed" : "offline");
}

void NovaRealtime::report_played_(bool force) {
  if (this->current_item_id_.empty() || !this->authenticated_)
    return;
  uint32_t played = this->played_samples_.load(std::memory_order_relaxed);
  if (!force && played - this->last_reported_samples_ < PLAYED_REPORT_INTERVAL)
    return;
  JsonDocument report;
  report["type"] = "audio.played";
  report["item_id"] = this->current_item_id_;
  report["played_samples"] = played;
  std::string encoded;
  serializeJson(report, encoded);
  this->send_control_(encoded);
  this->last_reported_samples_ = played;
}

void NovaRealtime::send_control_(const std::string &payload) {
  if (this->client_ == nullptr || !this->socket_connected_)
    return;
  int result = esp_websocket_client_send_text(this->client_, payload.c_str(), payload.size(), pdMS_TO_TICKS(100));
  if (result < 0)
    ESP_LOGW(TAG, "Could not send gateway control frame");
}

void NovaRealtime::send_audio_(const uint8_t *pcm, size_t length) {
  std::vector<uint8_t> frame(PROTOCOL_HEADER_SIZE + length);
  std::memcpy(frame.data(), "NVR1", 4);
  frame[4] = 1;
  frame[5] = 1;
  frame[6] = 0;
  frame[7] = 0;
  write_be32(frame.data() + 8, this->microphone_sequence_++);
  write_be32(frame.data() + 12, this->microphone_sample_index_);
  this->microphone_sample_index_ += length / 2;
  std::memcpy(frame.data() + PROTOCOL_HEADER_SIZE, pcm, length);
  int result = esp_websocket_client_send_bin(this->client_, reinterpret_cast<const char *>(frame.data()), frame.size(),
                                             pdMS_TO_TICKS(100));
  if (result < 0)
    ESP_LOGW(TAG, "Could not send microphone frame");
}

void NovaRealtime::send_error_(const std::string &code, const std::string &message) {
  ESP_LOGW(TAG, "%s: %s", code.c_str(), message.c_str());
  this->error_trigger_.trigger(code, message);
}

void NovaRealtime::set_state_(const std::string &phase) {
  ESP_LOGD(TAG, "State: %s", phase.c_str());
  this->state_trigger_.trigger(phase);
}

std::string NovaRealtime::authentication_digest_(const std::string &challenge) const {
  const std::string material = "nova-v1\n" + challenge;
  unsigned char digest[32];
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(info, reinterpret_cast<const unsigned char *>(this->pre_shared_key_.data()),
                  this->pre_shared_key_.size(), reinterpret_cast<const unsigned char *>(material.data()),
                  material.size(), digest);
  static const char hex[] = "0123456789abcdef";
  std::string output(64, '0');
  for (size_t index = 0; index < sizeof(digest); index++) {
    output[index * 2] = hex[digest[index] >> 4];
    output[index * 2 + 1] = hex[digest[index] & 0x0F];
  }
  return output;
}

}  // namespace esphome::nova_realtime
