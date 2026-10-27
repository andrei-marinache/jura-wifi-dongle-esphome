#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include "jura_cipher.h"

// Portable UART transport. No ESPHome, Arduino, network or model dependency.
namespace jura_protocol {
// Query kinds. TYPE/RT_*/RE_* are the clear-text TOP-tronics protocol.
// AT_RAW is the encrypted @-protocol: a fully-built '&' frame (see jura_cipher.h
// and AT-PROTOCOL.md) transmitted verbatim; its reply is a '&' frame.
enum class Query : uint8_t { TYPE, RT_ADDRESS, RE_ADDRESS, RT_COUNTERS, RT_0010, RT_0020, PRODUCTS, MAINTENANCE, MAINTENANCE_PERCENT, AT_RAW };
enum class Event : uint8_t { SENT, REPLY, UNSOLICITED, TIMEOUT, INVALID_FRAME, REJECTED, ERROR_REPLY, RETRY };

inline bool is_rt(Query q) { return q == Query::RT_ADDRESS || q == Query::RT_COUNTERS || q == Query::RT_0010 || q == Query::RT_0020; }
// Queries whose command carries a caller-supplied 16-bit address.
inline bool is_addressed(Query q) { return q == Query::RT_ADDRESS || q == Query::RE_ADDRESS; }
inline const char *command(Query q) {
  switch (q) {
    case Query::TYPE: return "TY:";
    case Query::RT_ADDRESS: return "RT:";
    case Query::RE_ADDRESS: return "RE:";
    case Query::RT_COUNTERS: return "RT:0000";
    case Query::RT_0010: return "RT:0010";
    case Query::RT_0020: return "RT:0020";
    case Query::PRODUCTS: return "TR:32,00";
    case Query::MAINTENANCE: return "TG:43";
    case Query::MAINTENANCE_PERCENT: return "TG:C0";
    case Query::AT_RAW: return "@";
  }
  return "";
}
inline const char *prefix(Query q) {
  switch (q) {
    case Query::TYPE: return "ty:";
    case Query::RE_ADDRESS: return "re:";
    case Query::RT_ADDRESS:
    case Query::RT_COUNTERS:
    case Query::RT_0010:
    case Query::RT_0020: return "rt:";
    case Query::PRODUCTS: return "tr:32";
    case Query::MAINTENANCE: return "tg:43";
    case Query::MAINTENANCE_PERCENT: return "tg:C0";
    case Query::AT_RAW: return "&";
  }
  return "";
}
inline std::array<uint8_t, 4> encode(uint8_t value) {
  std::array<uint8_t, 4> out{};
  for (unsigned i = 0; i < 4; ++i)
    out[i] = 0xDB | (((value >> (2*i)) & 1) << 2) | (((value >> (2*i+1)) & 1) << 5);
  return out;
}
// @-protocol: the machine answers a request "@XY..." with a lower-case "@xy...";
// reads (@TR/@TG/@TM) also echo their argument. Upper-case frames (@TF status,
// @TV progress, @TB, @T2/@T3 prompts) are pushes and never answer a request.
inline char at_lower(char c) { return c >= 'A' && c <= 'Z' ? c + ('a'-'A') : c; }
// Lower-case requests (@t2/@t3) acknowledge a machine prompt and get no answer.
inline bool at_expects_reply(const std::string &req) {
  return req.size() >= 3 && req[0] == '@' && req[1] >= 'A' && req[1] <= 'Z';
}
inline bool at_reply_matches(const std::string &req, const std::string &reply) {
  if (!at_expects_reply(req) || reply.size() < 3 || reply[0] != '@') return false;
  if (reply[1] < 'a' || reply[1] > 'z') return false;
  if (reply[1] != at_lower(req[1]) || at_lower(reply[2]) != at_lower(req[2])) return false;
  const bool echoes = req[1] == 'T' && (req[2] == 'R' || req[2] == 'G' || req[2] == 'M');
  // A setting write "@TM:<arg>,<value><csum>" is acknowledged with just "@tm:<arg>".
  const size_t comma = req[2] == 'M' ? req.find(',') : std::string::npos;
  const size_t echo_end = comma == std::string::npos ? req.size() : comma;
  for (size_t i = 3; echoes && i < echo_end; ++i)
    if (i >= reply.size() || at_lower(reply[i]) != at_lower(req[i])) return false;
  return true;
}
// The machine's periodic status (@TF) and progress (@TV) pushes. A request that
// reaches the machine just before one of them is dropped: the push comes instead
// of the answer (seen on a Z10, 2026-10-27).
inline bool at_is_push(const std::string &plain) {
  return plain.size() >= 3 && plain[0] == '@' && plain[1] == 'T' && (plain[2] == 'F' || plain[2] == 'V');
}
struct Sink {
  virtual ~Sink() = default;
  // Drain transmission and return its completion time on the tick() millisecond clock.
  virtual uint32_t send(const uint8_t *bytes, size_t size, uint32_t now) = 0;
  virtual void event(Event event, Query query, const std::string &payload) = 0;
};

class Engine {
 public:
  static constexpr size_t MAX_FRAME = 1024;
  static constexpr size_t QUEUE_SIZE = 32;   // session start: handshake + 9 banks + 2 maintenance + 5 settings, plus retries
  static constexpr uint32_t FRAME_GAP_MS = 100;
  // Pause after a good reply before the next request; timeouts and errors keep 1 s.
  static constexpr uint32_t REPLY_GAP_MS = 10;
  // Extra millisecond covers truncation of the completion timestamp.
  static constexpr uint32_t TX_IDLE_MS = 9;
  // Re-sends of one request that a push displaced, before it is left to time out.
  static constexpr uint8_t MAX_RETRIES = 3;
  explicit Engine(Sink &sink) : sink_(sink) { frame_.reserve(MAX_FRAME); }
  void set_timeout(uint32_t timeout) { timeout_ = timeout; }
  bool busy() const { return state_ != State::IDLE || count_ != 0; }
  void clear_queue() { count_ = 0; }   // the request already on the wire still completes
  const std::string &active_command() const { return active_command_; }
  bool enqueue_rt(uint16_t address) { return enqueue_addressed(Query::RT_ADDRESS, address); }
  // RE: reads a single word. The S8 reply format is unverified, so validation
  // below deliberately accepts any even-length hex payload rather than
  // assuming four digits; a wrong guess would reject the reply and teach us
  // nothing.
  bool enqueue_re(uint16_t address) { return enqueue_addressed(Query::RE_ADDRESS, address); }
  // Send an encrypted @-protocol command (e.g. "@TF:\r\n"). Read-only unless
  // the caller supplies a state-changing command. key is the per-frame cipher key.
  bool enqueue_at(const std::string &plaintext, uint8_t key=0x2A) {
    if (!enqueue(Query::AT_RAW)) return false;
    const size_t slot = (head_+count_-1)%QUEUE_SIZE;
    raw_[slot] = atproto::build_frame(plaintext, key);
    std::string &plain = plain_[slot];
    plain = plaintext;
    while (!plain.empty() && (plain.back() == '\r' || plain.back() == '\n')) plain.pop_back();
    return true;
  }
  bool enqueue_addressed(Query query, uint16_t address) {
    if (!enqueue(query)) return false;
    addresses_[(head_+count_-1)%QUEUE_SIZE] = address;
    return true;
  }
  bool enqueue(Query query) {
    if (count_ == QUEUE_SIZE) {
      sink_.event(Event::REJECTED, query, "queue full");
      return false;
    }
    queue_[(head_ + count_) % QUEUE_SIZE] = query;
    retries_[(head_ + count_) % QUEUE_SIZE] = 0;
    ++count_;
    return true;
  }
  void feed(uint8_t raw, uint32_t now) {
    expire_frame_(now);
    last_rx_ = now;
    have_rx_ = true;
    if (discard_) return;
    // Some implementations use bit 7 = 0; bits 0,1,3,4,6 must be high.
    if ((raw & 0x5B) != 0x5B) { invalid_(); return; }
    byte_ |= (((raw >> 2) & 1) | (((raw >> 5) & 1) << 1)) << bits_;
    bits_ += 2;
    if (bits_ != 8) return;
    const uint8_t decoded = byte_;
    bits_ = byte_ = 0;
    if (frame_.size() == MAX_FRAME) { invalid_(); return; }
    frame_.push_back(static_cast<char>(decoded));
    // Binary broadcasts can contain CRLF in their payload. Frame them by idle
    // gap, never let their contents satisfy a pending ASCII request.
    if (frame_[0] == '&') return;
    if (decoded != '\r' && decoded != '\n' && (decoded < 32 || decoded > 126)) {
      invalid_(); return;
    }
    if (frame_.size() >= 2 && frame_.compare(frame_.size()-2, 2, "\r\n") == 0) {
      frame_.resize(frame_.size()-2);
      dispatch_(now);
      frame_.clear();
    }
  }
  void tick(uint32_t now) {
    expire_frame_(now);
    if (state_ == State::WAITING && uint32_t(now - wait_start_) >= timeout_) {
      state_ = State::IDLE;
      cooling_ = true;
      cooldown_start_ = now;
      sink_.event(Event::TIMEOUT, active_, "");
    }
    if (state_ == State::IDLE) {
      if (cooling_ && uint32_t(now-cooldown_start_) < cooldown_ms_) return;
      cooling_ = false; cooldown_ms_ = 1000;
      if (!count_ || !frame_.empty() || bits_ || discard_) return;
      if (have_rx_ && uint32_t(now-last_rx_) < FRAME_GAP_MS) return;
      active_ = queue_[head_];
      active_retries_ = retries_[head_];
      active_command_ = command(active_);
      if (is_addressed(active_)) {
        char cmd[8];
        std::snprintf(cmd, sizeof(cmd), "%s%04X", command(active_), unsigned(addresses_[head_]));
        active_command_ = cmd;
      }
      if (active_ == Query::AT_RAW) {
        tx_ = raw_[head_];            // complete '&'..CRLF frame, already built
        active_command_ = plain_[head_];
      } else {
        tx_ = active_command_ + "\r\n";
      }
      head_ = (head_+1) % QUEUE_SIZE;
      --count_;
      tx_pos_ = 0;
      last_tx_ = now-TX_IDLE_MS;
      state_ = State::SENDING;
    }
    if (state_ == State::SENDING && uint32_t(now-last_tx_) >= TX_IDLE_MS) {
      const auto bytes = encode(static_cast<uint8_t>(tx_[tx_pos_++]));
      last_tx_ = sink_.send(bytes.data(), bytes.size(), now);
      if (tx_pos_ == tx_.size()) {
        state_ = State::WAITING;
        wait_start_ = last_tx_;
        sink_.event(Event::SENT, active_, active_command_);
        if (active_ == Query::AT_RAW && !at_expects_reply(active_command_)) {
          state_ = State::IDLE; cooling_ = true; cooldown_start_ = last_tx_; cooldown_ms_ = REPLY_GAP_MS;
        }
      }
    }
  }

 private:
  enum class State : uint8_t { IDLE, SENDING, WAITING };
  static char lower_(char c) { return c >= 'A' && c <= 'Z' ? c + ('a'-'A') : c; }
  static bool starts_(const std::string &s, const char *p) {
    size_t i=0;
    for (; p[i]; ++i)
      if (i >= s.size() || lower_(s[i]) != lower_(p[i])) return false;
    return true;
  }
  bool valid_reply_() const {
    if (!starts_(frame_, prefix(active_))) return false;
    const size_t offset = (active_ == Query::TYPE || is_rt(active_) || active_ == Query::RE_ADDRESS) ? 3 : 5;
    if (frame_.size() <= offset) return false;
    if (active_ == Query::TYPE) return true;
    if (active_ == Query::RT_ADDRESS && frame_.size()-offset != 64) return false;
    if (is_rt(active_) && (frame_.size()-offset) % 4) return false;
    if ((frame_.size()-offset) % 2) return false;
    for (size_t i=offset; i<frame_.size(); ++i) {
      const char c = lower_(frame_[i]);
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
  }
  void dispatch_(uint32_t now) {
    if (state_ == State::WAITING && valid_reply_()) {
      state_ = State::IDLE;
      cooling_ = true;
      cooldown_start_ = now; cooldown_ms_ = REPLY_GAP_MS;
      sink_.event(Event::REPLY, active_, frame_);
    } else if (state_ == State::WAITING &&
               (starts_(frame_, "@an:error") || starts_(frame_, "an:error") || starts_(frame_, "error:"))) {
      state_ = State::IDLE;
      cooling_ = true;
      cooldown_start_ = now;
      sink_.event(Event::ERROR_REPLY, active_, frame_);
    } else {
      sink_.event(Event::UNSOLICITED, active_, frame_);
    }
  }
  // Put the displaced request back at the head of the queue; the push that just
  // ended leaves ~2 s before the next one.
  void retry_active_(uint32_t now) {
    if (count_ == QUEUE_SIZE) return;
    head_ = (head_ + QUEUE_SIZE - 1) % QUEUE_SIZE;
    queue_[head_] = Query::AT_RAW; raw_[head_] = tx_; plain_[head_] = active_command_;
    retries_[head_] = active_retries_ + 1;
    ++count_;
    state_ = State::IDLE; cooling_ = true; cooldown_start_ = now; cooldown_ms_ = REPLY_GAP_MS;
    sink_.event(Event::RETRY, active_, active_command_);
  }
  void invalid_() {
    frame_.clear(); bits_ = byte_ = 0; discard_ = true;
    sink_.event(Event::INVALID_FRAME, active_, "");
  }
  void expire_frame_(uint32_t now) {
    if (!have_rx_ || uint32_t(now-last_rx_) < FRAME_GAP_MS) return;
    if (!discard_ && (!frame_.empty() || bits_)) {
      if (!bits_ && frame_[0] == '&' && frame_.size() >= 3 &&
          frame_.compare(frame_.size()-2, 2, "\r\n") == 0) {
        frame_.resize(frame_.size()-2);
        std::string plain;
        const bool waiting_at = state_ == State::WAITING && active_ == Query::AT_RAW &&
                                atproto::decode_frame(frame_, plain);
        if (waiting_at && at_reply_matches(active_command_, plain)) {
          state_ = State::IDLE; cooling_ = true; cooldown_start_ = now; cooldown_ms_ = REPLY_GAP_MS;
          sink_.event(Event::REPLY, active_, frame_);
        } else {
          sink_.event(Event::UNSOLICITED, active_, frame_);
          if (waiting_at && at_is_push(plain) && active_retries_ < MAX_RETRIES) retry_active_(now);
        }
      } else {
        sink_.event(Event::INVALID_FRAME, active_, "");
      }
    }
    frame_.clear(); bits_ = byte_ = 0; discard_ = false; have_rx_ = false;
  }
  Sink &sink_;
  std::array<Query, QUEUE_SIZE> queue_{};
  std::array<uint16_t, QUEUE_SIZE> addresses_{};
  std::array<std::string, QUEUE_SIZE> raw_{};
  std::array<std::string, QUEUE_SIZE> plain_{};   // AT_RAW plaintext without CRLF, for matching and logs
  std::array<uint8_t, QUEUE_SIZE> retries_{};
  uint8_t active_retries_{0};
  size_t head_{0}, count_{0}, tx_pos_{0};
  Query active_{Query::TYPE};
  State state_{State::IDLE};
  std::string frame_, tx_, active_command_;
  uint8_t byte_{0}, bits_{0};
  bool have_rx_{false}, discard_{false}, cooling_{false};
  uint32_t last_rx_{0}, last_tx_{0}, wait_start_{0}, cooldown_start_{0}, timeout_{3000}, cooldown_ms_{1000};
};
}  // namespace jura_protocol
