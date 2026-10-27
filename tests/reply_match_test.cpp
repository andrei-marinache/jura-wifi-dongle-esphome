// Host test for the @-protocol reply matcher. Build and run:
//   g++ -std=c++17 -Wall -Wextra -Werror -Icomponents tests/reply_match_test.cpp -o /tmp/reply-match && /tmp/reply-match
// (from the repository root)
#include "jura_link/protocol.h"
#include <cassert>
#include <cstdio>
#include <vector>

using jura_protocol::at_expects_reply;
using jura_protocol::at_reply_matches;
using jura_protocol::Event;

// Same formula as JuraLink::setting_checksum_ (that header needs ESPHome to compile).
static uint8_t esphome_free_checksum(const std::string &s) { int sum=0; for (char c : s) sum += (unsigned char) c; return uint8_t((-1 - sum) & 0xFF); }

// Records engine events; send() pretends each 4-byte group takes 1 ms on the wire.
struct FakeSink : jura_protocol::Sink {
  std::vector<std::pair<Event, std::string>> events;
  uint32_t send(const uint8_t *, size_t, uint32_t now) override { return now + 1; }
  void event(Event e, jura_protocol::Query, const std::string &p) override { events.push_back({e, p}); }
};

// Feed one '&' frame from the machine, as 4:1 line-encoded bytes, then let it expire by idle gap.
static void machine_says(jura_protocol::Engine &eng, uint32_t &now, const std::string &plain) {
  for (char c : jura_protocol::atproto::build_frame(plain + "\r\n", 0x5A))
    for (uint8_t b : jura_protocol::encode(static_cast<uint8_t>(c))) eng.feed(b, ++now);
  now += jura_protocol::Engine::FRAME_GAP_MS + 1;
  eng.tick(now);
}

static std::string plain_of(const std::string &frame) {
  std::string p; jura_protocol::atproto::decode_frame(frame, p);
  while (!p.empty() && (p.back() == '\r' || p.back() == '\n')) p.pop_back();
  return p;
}

int main() {
  // Real answers seen on the Z10 (EF545M V02.02), 2026-10-27.
  assert(at_reply_matches("@TR:32,00", "@tr:32,00,0960FFFF005E0007"));
  assert(at_reply_matches("@TR:32,0A", "@tr:32,0A,00000041FFFF0003"));
  assert(at_reply_matches("@TR:37", "@tr:37,4947000244894316"));
  assert(at_reply_matches("@TG:43", "@tg:4300170004000608E4069E0221"));
  assert(at_reply_matches("@TG:C0", "@tg:C014FF0A"));
  assert(at_reply_matches("@T0", "@t0"));
  assert(at_reply_matches("@T1", "@t1"));
  assert(at_reply_matches("@TS:01", "@ts"));
  assert(at_reply_matches("@TP:0300051400", "@tp"));
  assert(at_reply_matches("@TP:0300051400", "@tp:00"));
  // Pushes that arrived while a request was waiting must not count as its answer.
  assert(!at_reply_matches("@TR:32,00", "@TF:00040000004000"));
  assert(!at_reply_matches("@TR:32,0C", "@TV:34040A0A00100000000E000000110085"));
  assert(!at_reply_matches("@TS:01", "@TB"));
  assert(!at_reply_matches("@T1", "@T2:01C041B628"));
  assert(!at_reply_matches("@TR:32,00", "@t0"));            // machine closing the session
  // Another bank's answer is not this bank's answer.
  assert(!at_reply_matches("@TR:32,01", "@tr:32,00,0960FFFF005E0007"));
  assert(!at_reply_matches("@TG:43", "@tg:C014FF0A"));
  // Settings: a read echoes "<arg>,<value><csum>", a write is acknowledged with "@tm:<arg>" alone.
  assert(at_reply_matches("@TM:02", "@tm:02,0110"));
  assert(at_reply_matches("@TM:02,10EF", "@tm:02"));
  assert(!at_reply_matches("@TM:02,10EF", "@tm:13"));
  assert(!at_reply_matches("@TM:02", "@tm:13,1EF9"));
  // Acknowledgements of machine prompts expect nothing.
  assert(!at_expects_reply("@t2:818000"));
  assert(!at_expects_reply("@t3"));
  assert(at_expects_reply("@TR:37"));
  // Engine: a @TF push lands while @TR:32,00 waits; the real answer must still be the REPLY.
  {
    FakeSink sink; jura_protocol::Engine eng(sink); uint32_t now = 1000;
    eng.enqueue_at("@TR:32,00\r\n");
    for (int i = 0; i < 200; ++i) eng.tick(now += 10);           // send it, now waiting
    assert(sink.events.back().first == Event::SENT && sink.events.back().second == "@TR:32,00");
    machine_says(eng, now, "@TF:00040000004000");                 // the push displaces the request...
    assert(sink.events[sink.events.size()-2].first == Event::UNSOLICITED);
    assert(sink.events.back().first == Event::RETRY);
    for (int i = 0; i < 100; ++i) eng.tick(now += 10);           // ...so it goes out again
    assert(sink.events.back().first == Event::SENT && sink.events.back().second == "@TR:32,00");
    machine_says(eng, now, "@tr:32,00,0960FFFF005E0007");
    assert(sink.events.back().first == Event::REPLY);
    assert(plain_of(sink.events.back().second) == "@tr:32,00,0960FFFF005E0007");
  }
  // Engine: retries are capped, then the request is left to time out.
  {
    FakeSink sink; jura_protocol::Engine eng(sink); uint32_t now = 1000;
    eng.enqueue_at("@TR:32,00\r\n");
    for (int i = 0; i < 200; ++i) eng.tick(now += 10);
    int retries = 0;
    for (int k = 0; k < 6; ++k) {
      machine_says(eng, now, "@TF:00040000004000");
      for (int i = 0; i < 100; ++i) eng.tick(now += 10);
    }
    int timeouts = 0, sent = 0;
    for (auto &e : sink.events) { retries += e.first == Event::RETRY; timeouts += e.first == Event::TIMEOUT; sent += e.first == Event::SENT; }
    assert(retries == jura_protocol::Engine::MAX_RETRIES);
    assert(sent == 1 + jura_protocol::Engine::MAX_RETRIES);
    assert(timeouts == 1);
  }
  // Engine: an acknowledgement (@t3) does not wait for an answer.
  {
    FakeSink sink; jura_protocol::Engine eng(sink); uint32_t now = 1000;
    eng.enqueue_at("@t3\r\n"); eng.enqueue_at("@TR:37\r\n");
    for (int i = 0; i < 200; ++i) eng.tick(now += 10);
    int sent = 0; for (auto &e : sink.events) sent += e.first == Event::SENT;
    assert(sent == 2);                                             // @TR:37 went out without a timeout
    for (auto &e : sink.events) assert(e.first != Event::TIMEOUT);
  }
  // Setting checksum (ByteOperations.d): values read from a Z10 in jura-connect's notes.
  assert(esphome_free_checksum("02,01") == 0x10);
  assert(esphome_free_checksum("13,1E") == 0xF9);
  assert(esphome_free_checksum("08,220100") == 0x46);
  assert(esphome_free_checksum("09,02") == 0x08);
  assert(esphome_free_checksum("0A,07") == 0xFB);
  std::puts("reply_match_test: all passed");
  return 0;
}
