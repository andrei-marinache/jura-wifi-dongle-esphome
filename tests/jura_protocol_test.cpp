#include "../components/jura_link/protocol.h"
#include <cassert>
#include <iostream>
#include <vector>
#include <tuple>
using namespace jura_protocol;
struct Fake : Sink {
  std::vector<uint8_t> tx;
  std::vector<std::tuple<Event,Query,std::string>> events;
  uint32_t drain_ms=0;
  uint32_t send(const uint8_t *p,size_t n,uint32_t now) override { tx.insert(tx.end(),p,p+n); return now+drain_ms; }
  void event(Event e,Query q,const std::string &s) override { events.emplace_back(e,q,s); }
  size_t count(Event e) const { size_t n=0; for(const auto &v:events) n+=std::get<0>(v)==e; return n; }
};
void feed(Engine &e,const std::string &s,uint32_t now) {
  for(unsigned char c:s) for(auto raw:encode(c)) e.feed(raw,now);
}
void sent(Engine &e,uint32_t start=0) { for(unsigned i=0;i<10;++i) e.tick(start+i*9); }
int main() {
  { // Addressed reads preserve each queued address and require all 32 bytes.
    Fake s; Engine e(s); e.enqueue_rt(0x0010); e.enqueue_rt(0xFFF0); sent(e);
    assert(std::get<2>(s.events.back())=="RT:0010");
    feed(e,"rt:0001\r\n",100); assert(s.count(Event::REPLY)==0);
    feed(e,"rt:"+std::string(64,'0')+"\r\n",200); assert(s.count(Event::REPLY)==1);
    sent(e,1300); assert(std::get<2>(s.events.back())=="RT:FFF0");
  }

  { // @-protocol: encrypted command TX and reply decode.
    Fake s; Engine e(s);
    assert(e.enqueue_at("@TF:\r\n", 0x2A));
    sent(e);                                   // transmits the & frame
    assert(s.count(Event::SENT)==1);
    // machine replies with an encrypted & frame; build one with the same cipher
    std::string body; body.push_back((char)0xA0);
    body += jura_protocol::atproto::crypt("@tf:000400000800\r\n", 0xA0);
    std::string wire = std::string("&") + jura_protocol::atproto::escape(body) + "\r\n";
    feed(e, wire, 100);
    e.tick(250);                               // idle gap -> reply emitted
    assert(s.count(Event::REPLY)==1);
    std::string pt;
    assert(jura_protocol::atproto::decode_frame(std::get<2>(s.events.back()), pt));
    assert(pt=="@tf:000400000800\r\n");
  }
  { // RE: single-word reads. Address is formatted and preserved like RT, but
    // the reply is accepted permissively because the S8 format is unverified:
    // any even-length hex payload, not an assumed four digits.
    Fake s; Engine e(s); e.enqueue_re(0x0013); e.enqueue_re(0x01B1); sent(e);
    assert(std::get<2>(s.events.back())=="RE:0013");
    feed(e,"re:00Z1\r\n",100); assert(s.count(Event::REPLY)==0);   // non-hex
    feed(e,"re:001\r\n",200);  assert(s.count(Event::REPLY)==0);   // odd length
    feed(e,"re:\r\n",300);     assert(s.count(Event::REPLY)==0);   // empty
    feed(e,"rt:0022\r\n",400); assert(s.count(Event::REPLY)==0);   // wrong family
    feed(e,"re:0022\r\n",500); assert(s.count(Event::REPLY)==1);
    assert(std::get<2>(s.events.back())=="re:0022");
    sent(e,1600); assert(std::get<2>(s.events.back())=="RE:01B1");
  }
  { // A longer-than-expected RE payload is still captured rather than dropped.
    Fake s; Engine e(s); e.enqueue_re(0x0000); sent(e);
    feed(e,"re:00220033\r\n",100); assert(s.count(Event::REPLY)==1);
  }
  { // RT and RE replies must not satisfy each other's pending request.
    Fake s; Engine e(s); e.enqueue_rt(0x0000); sent(e);
    feed(e,"re:0022\r\n",100); assert(s.count(Event::REPLY)==0);
    assert(s.count(Event::UNSOLICITED)==1);
  }

  for (auto q : {Query::RT_COUNTERS, Query::RT_0010, Query::RT_0020}) { // RT fields.
    Fake s; Engine e(s); e.enqueue(q); sent(e);
    feed(e,"rt:000\r\n",100);
    assert(s.count(Event::REPLY)==0);
    feed(e,"rt:000Z\r\n",110);
    assert(s.count(Event::REPLY)==0);
    feed(e,"rt:00010002FFFF\r\n",120);
    assert(s.count(Event::REPLY)==1);
  }

  { // The idle interval starts at TX completion, including across clock rollover.
    for (uint32_t start : {0u, 0xFFFFFFFCu}) {
      Fake s; s.drain_ms=5; Engine e(s); e.enqueue(Query::TYPE);
      e.tick(start); assert(s.tx.size()==4);
      e.tick(start+8); assert(s.tx.size()==4);
      e.tick(start+13); assert(s.tx.size()==4);
      e.tick(start+14); assert(s.tx.size()==8);
    }
  }
  { // Captured live TY command and reply, with an unrelated broadcast first.
    Fake s; Engine e(s); e.enqueue(Query::TYPE); sent(e);
    const std::vector<uint8_t> expected={0xDB,0xDF,0xDF,0xDF,0xDF,0xFB,0xDF,0xDF,0xFB,0xFB,0xFF,0xDB,0xDF,0xFF,0xDB,0xDB,0xFB,0xFB,0xDB,0xDB};
    assert(s.tx==expected);
    feed(e,std::string("&\0\r\nXYZ\r\n",9),100);
    e.tick(201); assert(s.count(Event::UNSOLICITED)==1); assert(s.count(Event::REPLY)==0);
    feed(e,"ty:EF536M V02.01\r\n",250);
    assert(s.count(Event::REPLY)==1); assert(!e.busy());
  }
  { // Unrelated, malformed and mismatched bank replies never complete a query.
    Fake s; Engine e(s); e.enqueue(Query::MAINTENANCE); sent(e);
    feed(e,"tg:C000FF46\r\n",100); feed(e,"tg:43ZZ\r\n",110);
    assert(s.count(Event::REPLY)==0);
    feed(e,"tg:43000100020003000400050006\r\n",120);
    assert(s.count(Event::REPLY)==1);
  }
  { // All 256 values survive encoding, including bit 7 low on raw input.
    for(unsigned c=0;c<256;++c) {
      auto raw=encode(c); unsigned value=0;
      for(unsigned i=0;i<4;++i) value|=(((raw[i]>>2)&1)|(((raw[i]>>5)&1)<<1))<<(2*i);
      assert(value==c);
    }
    Fake s; Engine e(s); e.enqueue(Query::TYPE); sent(e);
    for(unsigned char c:std::string("ty:EF536M\r\n")) for(auto r:encode(c)) e.feed(r&0x7F,100);
    assert(s.count(Event::REPLY)==1);
  }
  { // Timeout is bounded even under continuous traffic. A late reply is unsolicited.
    Fake s; Engine e(s); e.set_timeout(200); e.enqueue(Query::TYPE); sent(e);
    e.tick(240); assert(s.count(Event::TIMEOUT)==1);
    feed(e,"ty:LATE\r\n",260); assert(s.count(Event::REPLY)==0);
    e.tick(1000); assert(s.count(Event::TIMEOUT)==1);
  }
  { // Truncated symbol, invalid raw byte, oversized frame; then recovery.
    Fake s; Engine e(s); e.feed(0xDB,1); e.tick(102);
    assert(s.count(Event::INVALID_FRAME)==1);
    e.feed(0x00,200); e.tick(301);
    assert(s.count(Event::INVALID_FRAME)==2);
    feed(e,std::string(Engine::MAX_FRAME+1,'a'),400); e.tick(501);
    assert(s.count(Event::INVALID_FRAME)==3);
    e.enqueue(Query::TYPE); sent(e,600); feed(e,"ty:RECOVERED\r\n",700);
    assert(s.count(Event::REPLY)==1);
  }
  { // Bounded FIFO and no transmission before the active request completes.
    Fake s; Engine e(s);
    for(size_t i=0;i<Engine::QUEUE_SIZE;++i) assert(e.enqueue(Query::TYPE));
    assert(!e.enqueue(Query::TYPE)); sent(e); assert(s.count(Event::SENT)==1);
    e.tick(500); assert(s.count(Event::SENT)==1);
    // This fork waits REPLY_GAP_MS (10 ms) after a reply, upstream waited 1 s.
    feed(e,"ty:ONE\r\n",600); e.tick(650); assert(s.count(Event::SENT)==1);
    sent(e,701); assert(s.count(Event::SENT)==2);
  }
  { // Millis rollover during a transaction.
    Fake s; Engine e(s); e.set_timeout(200); e.enqueue(Query::TYPE);
    sent(e,0xFFFFFFE0u); e.tick(205); assert(s.count(Event::TIMEOUT)==1);
  }
  std::cout << "PASS: captured frames, binary separation, matching, encoding, timeouts, recovery, bounds, rollover\n";
}
