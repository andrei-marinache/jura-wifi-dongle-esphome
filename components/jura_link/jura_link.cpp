#include "jura_link.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#ifdef USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#endif
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#include "jura_profiles_blob.h"

namespace esphome { namespace jura_link {
static const char *const TAG = "jura_link";
// Same idle gap the engine uses to delimit frames. Shorter gaps can split a
// frame mid-group when the loop stalls and misalign the 4:1 decoding after it.
static const uint32_t SNIFF_GAP_MS = 100;
static const size_t SNIFF_MAX_RAW = 4096;
static const size_t SNIFF_CHUNK = 48;
void JuraLink::setup() {
  for (int i=0;i<256;++i) counters_[i]=0xFFFF;
  for (int i=0;i<8;++i){ maint_[i]=0xFFFF; maint_pct_[i]=0xFF; }
  profiles_ = jura_jprf::Profiles(JURA_PROFILES, JURA_PROFILES_LEN);
  for (auto &v : req_num_) v = -1;
  if (connected_) connected_->publish_state(false);
  if (last_error_) last_error_->publish_state("None");
  publish_counters_();
  query_type();
}
void JuraLink::dump_config() {
  ESP_LOGCONFIG(TAG, "Jura link: TX drain + 8 ms minimum idle, read-only; binary broadcasts separated from replies");
  ESP_LOGCONFIG(TAG, "Listen only: %s", listen_only_ ? "YES (TX disabled)" : "NO");
  ESP_LOGCONFIG(TAG, "Sniff: %s, tag %s", sniff_ ? "YES (raw bursts logged, engine bypassed)" : "NO", sniff_tag_.c_str());
  check_uart_settings(9600);
  LOG_UPDATE_INTERVAL(this);
}
void JuraLink::update() {
  ESP_LOGD(TAG, "update() busy=%d auto_poll=%d hs=%d listen=%d", engine_.busy(), auto_poll_, handshake_done_, listen_only_);
  if (listen_only_||engine_.busy()) return; if (auto_poll_) poll_stats(); else query_type();
}
void JuraLink::loop() {
  uint8_t raw;
  if (sniff_) {
    for (size_t i=0; i<1024 && available(); ++i) {
      if (!read_byte(&raw)) break;
      const uint32_t now = millis();
      if (!sniff_raw_.empty() && uint32_t(now-sniff_last_) >= SNIFF_GAP_MS) flush_sniff_();
      if (sniff_raw_.empty()) sniff_start_ = now;
      if (sniff_raw_.size() < SNIFF_MAX_RAW) sniff_raw_.push_back(raw); else ++sniff_overflow_;
      sniff_last_ = now;
    }
    if (!sniff_raw_.empty() && uint32_t(millis()-sniff_last_) >= SNIFF_GAP_MS) flush_sniff_();
    if (dirty_ && uint32_t(millis()-last_publish_) >= 1000) publish_counters_();
    return;
  }
  // Bound per-loop work, including if a wire or device continuously sends noise.
  for (size_t i=0; i<128 && available(); ++i) {
    if (!read_byte(&raw)) break;
    engine_.feed(raw, millis());
  }
  engine_.tick(millis());
  if (hs_started_ && !session_ready_ && !engine_.busy()) {
    const uint32_t now = millis();
    // @T2 normally follows @t1 within ~2 s; none after 4 s means the machine kept the old key.
    if (handshake_done_ && !t2_seen_ && uint32_t(now-t1_at_) >= 4000) {
      ESP_LOGI(TAG, "@t1 without a new key exchange, session reused"); on_session_ready_();
    } else if (uint32_t(now-hs_started_) >= 10000) {
      ESP_LOGW(TAG, "handshake not finished after 10 s, retrying"); start_handshake();
    }
  }
  if (dirty_ && uint32_t(millis()-last_publish_) >= 1000) publish_counters_();
  if (uint32_t(millis()-mqtt_tick_) >= 1000) { mqtt_tick_ = millis(); mqtt_publish_(); }
  if (prog_state_ >= 0 && uint32_t(millis()-last_tv_) > 20000) {   // no @TV: for 20 s -> idle
    prog_state_ = -1; prog_code_ = -1; prog_pct_ = 0;
  }
  if (!pending_brew_.empty() && session_ready_) {   // brew requested before the handshake finished
    ESP_LOGW(TAG, "BREW (after handshake) %s", pending_brew_.c_str());
    brew_sent_ = millis() | 1;
    query_at(pending_brew_); pending_brew_.clear();
  }
}
void JuraLink::flush_sniff_() {
  static const char hex[] = "0123456789ABCDEF";
  const uint32_t seq = ++sniff_seq_;
  size_t bad = 0, bit7 = 0;
  for (auto r : sniff_raw_) {
    if ((r & 0x5B) != 0x5B) ++bad;
    if (r & 0x80) ++bit7;
  }
  // Decode whole 4-byte groups: payload bit pairs sit in raw bits 2 and 5.
  std::string dec;
  const size_t groups = sniff_raw_.size() / 4, tail = sniff_raw_.size() % 4;
  for (size_t g=0; g<groups; ++g) {
    uint8_t b = 0;
    for (unsigned i=0; i<4; ++i) {
      const uint8_t r = sniff_raw_[g*4+i];
      b |= (((r >> 2) & 1) | (((r >> 5) & 1) << 1)) << (2*i);
    }
    dec.push_back(static_cast<char>(b));
  }
  ESP_LOGI(TAG, "SNIFF[%s] #%u t=%u raw=%u dec=%u bad=%u bit7=%u tail=%u ovf=%u", sniff_tag_.c_str(), seq, sniff_start_,
           static_cast<unsigned>(sniff_raw_.size()), static_cast<unsigned>(dec.size()),
           static_cast<unsigned>(bad), static_cast<unsigned>(bit7), static_cast<unsigned>(tail), sniff_overflow_);
  const size_t dparts = (dec.size() + SNIFF_CHUNK - 1) / SNIFF_CHUNK;
  for (size_t p=0; p<dparts; ++p) {
    std::string hx, asc;
    for (size_t i=p*SNIFF_CHUNK; i<dec.size() && i<(p+1)*SNIFF_CHUNK; ++i) {
      const auto b = static_cast<uint8_t>(dec[i]);
      hx += hex[b >> 4]; hx += hex[b & 15];
      asc += (b >= 32 && b < 127 && b != '|') ? static_cast<char>(b) : '.';
    }
    ESP_LOGI(TAG, "SNIFFDEC[%s] #%u %u/%u %s |%s|", sniff_tag_.c_str(), seq, static_cast<unsigned>(p+1), static_cast<unsigned>(dparts), hx.c_str(), asc.c_str());
  }
  // Raw bytes too: decoding discards bits 0,1,3,4,6,7, and the encoding may not be what we assume.
  const size_t rparts = (sniff_raw_.size() + SNIFF_CHUNK - 1) / SNIFF_CHUNK;
  for (size_t p=0; p<rparts; ++p) {
    std::string hx;
    for (size_t i=p*SNIFF_CHUNK; i<sniff_raw_.size() && i<(p+1)*SNIFF_CHUNK; ++i) {
      hx += hex[sniff_raw_[i] >> 4]; hx += hex[sniff_raw_[i] & 15];
    }
    ESP_LOGI(TAG, "SNIFFRAW[%s] #%u %u/%u %s", sniff_tag_.c_str(), seq, static_cast<unsigned>(p+1), static_cast<unsigned>(rparts), hx.c_str());
  }
  ++unsolicited_count_;
  if (bad || tail) ++framing_count_;
  dirty_ = true;
  sniff_raw_.clear();
  sniff_overflow_ = 0;
}
uint32_t JuraLink::send(const uint8_t *data, size_t size, uint32_t) {
  write_array(data, size);
  flush();  // Wait for all four raw bytes to leave the UART before timing idle.
  return millis();
}
void JuraLink::publish_counters_() {
  if (requests_) requests_->publish_state(requests_count_);
  if (replies_) replies_->publish_state(replies_count_);
  if (timeouts_) timeouts_->publish_state(timeouts_count_);
  if (unsolicited_frames_) unsolicited_frames_->publish_state(unsolicited_count_);
  if (framing_errors_) framing_errors_->publish_state(framing_count_);
  if (rejected_requests_) rejected_requests_->publish_state(rejected_count_);
  dirty_ = false;
  last_publish_ = millis();
}
void JuraLink::publish_at_(const std::string &ampframe, bool reply) {
  std::string plain;
  if (!jura_protocol::atproto::decode_frame(ampframe, plain)) return;
  // strip trailing CRLF for display
  while (!plain.empty() && (plain.back()=='\r' || plain.back()=='\n')) plain.pop_back();
  ESP_LOGI(TAG, "@%s %s", reply ? "reply" : "bcast", plain.c_str());
  last_rx_ = millis();
  if (at_decoded_) at_decoded_->publish_state(plain);
  // --- Dongle handshake auto-response (see AT-PROTOCOL.md section 3) ---
  // After we send @T0/@T1, the machine drives the rest and expects the dongle
  // to answer two prompts:
  //   machine "@T2:01C000"         -> dongle "@t2:818000" (sniffed from the real dongle)
  //   machine "@T3:<model string>" -> dongle "@t3"
  // @t2 is not authentication: 818000, 812000 and 810000 are all accepted. Byte 0
  // is challenge[0]|0x80; the rest is probably a dongle capabilities/options field
  // (meaning unknown), so we keep the real dongle's value.
  if (!listen_only_) {
    if (plain.rfind("@T2:",0)==0) { t2_seen_=true; query_at("@t2:" + t2_reply_); }   // default sniffed from the S8 dongle; settable to test other machines
    else if (plain.rfind("@T3:",0)==0) {
      // The Z10 ends the session with @t0 ~2 s after the handshake unless the dongle
      // follows @t3 with @TR:37, as the original TT237W firmware does.
      query_at("@t3");   // the machine repeats @T3 until it gets this
      if (!session_ready_) {
        auto ef = plain.find("EF"); if (ef!=std::string::npos) select_profile_(plain.substr(ef));
        on_session_ready_();
      }
    }
    // @t0 while a session is open means the machine dropped it: start over.
    else if (plain == "@t0" && session_ready_) {
      ESP_LOGW(TAG, "machine closed the session (@t0), redoing the handshake");
      start_handshake();
    }
  }
  if (plain.rfind("@t1",0)==0) { handshake_done_=true; t1_at_=millis(); }
  parse_stats_(plain);
}
// Session is up (after @t3, or @t1 with no new key exchange): keep it open with @TR:37,
// as the original TT237W firmware does, and poll right away instead of at the next update().
void JuraLink::on_session_ready_() {
  query_at("@TR:37"); handshake_done_=true; session_ready_=true; hs_started_=0; settings_countdown_=0;
  poll_stats();
}
// Parse two ASCII hex chars -> nibble; four -> a 16-bit big-endian word.
static int hex4(const char *p){int v=0;for(int i=0;i<4;++i){char c=p[i];int d=(c<='9')?c-'0':(c|0x20)-'a'+10;v=(v<<4)|(d&0xF);}return v;}
void JuraLink::parse_stats_(const std::string &plain) {
  // Setting read: "@tm:<arg>,<value><csum>"; the checksum covers "<arg>,<value>".
  if (plain.rfind("@tm:",0)==0 && plain.size() >= 10 && plain[6]==',') {
    const std::string arg = plain.substr(4,2), body = plain.substr(4, plain.size()-6);
    const uint8_t cs = uint8_t(strtol(plain.substr(plain.size()-2).c_str(), nullptr, 16));
    for (size_t i=0; i<setting_args_.size(); ++i) {
      if (setting_args_[i] != arg) continue;
      if (cs != setting_checksum_(body)) { ESP_LOGW(TAG, "setting %s: bad checksum in %s", arg.c_str(), plain.c_str()); return; }
      setting_vals_[i] = body.substr(3);
      ESP_LOGI(TAG, "setting %s = %s", arg.c_str(), setting_vals_[i].c_str());
    }
    return;
  }
  // @TB marks a brew start (and a @TS:01 lock). Over serial it is the first answer to @TP:.
  if (plain == "@TB") {
    if (brew_sent_ && uint32_t(millis()-brew_sent_) < 60000) { ESP_LOGW(TAG, "BREW started"); brew_sent_ = 0; }
    return;
  }
  if (plain == "@ts") { ESP_LOGI(TAG, "panel lock command acknowledged"); return; }
  if (plain.rfind("@TV:",0)==0) { parse_progress_(plain); return; }
  // @TP: brew reply: bare "@tp" = accepted (then @TB + @TV progress), "@tp:00" = rejected/ignored.
  if (plain == "@tp") { ESP_LOGW(TAG, "BREW accepted by the machine"); brew_sent_ = 0; return; }
  if (plain.rfind("@tp:",0)==0) { ESP_LOGW(TAG, "BREW rejected by the machine (%s)", plain.c_str()); return; }
  // @TF:<hex> status broadcast (~every 2 s): alert bit i = byte i>>3, MSB first.
  if (plain.rfind("@TF:",0)==0) {
    const size_t n = std::min<size_t>((plain.size()-4)/2, sizeof(status_));
    for (size_t i=0; i<n; ++i) status_[i] = (uint8_t)strtol(plain.substr(4+i*2,2).c_str(), nullptr, 16);
    status_len_ = uint8_t(n); status_seen_ = true;
    return;
  }
  // @tr:32,NN,WWWW... (4 big-endian 16-bit words per bank, flat index = NN*4+i by product code)
  if (plain.rfind("@tr:32,",0)==0 && plain.size()>=9) {
    int nn = (int)strtol(plain.substr(7,2).c_str(), nullptr, 16);
    const char *w = plain.c_str()+10;   // after "@tr:32,NN,"
    size_t avail = plain.size()>10 ? (plain.size()-10) : 0;
    for (int i=0; i*4+4<=(int)avail && nn*4+i<256; ++i) { counters_[nn*4+i] = (uint16_t)hex4(w+i*4); counters_seen_[nn*4+i] = true; }
    dirty_ = true;
  } else if (plain.rfind("@tg:43",0)==0) {           // @tg:43<hex>, no comma
    const char *w = plain.c_str()+6;
    for (int i=0; i<8 && (int)(6+i*4+4)<=(int)plain.size(); ++i) { maint_[i]=(uint16_t)hex4(w+i*4); maint_seen_[i]=true; }
    dirty_ = true;
  } else if (plain.rfind("@tg:C0",0)==0 || plain.rfind("@tg:c0",0)==0) {  // @tg:C0<hex>
    const char *w = plain.c_str()+6;
    for (int i=0; i<8 && (int)(6+i*2+2)<=(int)plain.size(); ++i) {
      char b[3]={w[i*2],w[i*2+1],0}; maint_pct_[i]=(uint8_t)strtol(b,nullptr,16); maint_pct_seen_[i]=true;
    }
    dirty_ = true;
  }
}
std::string JuraLink::slug_(const std::string &s) {
  std::string o;
  for (char ch : s) {
    if ((ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')) o+=ch;
    else if (ch>='A'&&ch<='Z') o+=char(ch+32);
    else if (!o.empty() && o.back()!='_') o+='_';
  }
  while(!o.empty() && o.back()=='_') o.pop_back();
  return o;
}
// "FilterChange" -> "Filter Change", "ABCWord" -> "ABC Word" (jura-mqtt split_camel_case).
std::string JuraLink::camel_words_(const std::string &s) {
  auto up=[](char c){ return c>='A'&&c<='Z'; };
  auto lo=[](char c){ return c>='a'&&c<='z'; };
  auto dg=[](char c){ return c>='0'&&c<='9'; };
  std::string o;
  for (size_t i=0; i<s.size(); ++i) {
    const char c=s[i];
    bool start=false;
    if (up(c) && i>0) {
      const char p=s[i-1];
      if (lo(p)||dg(p)) start=true;
      else if (up(p) && i+1<s.size() && lo(s[i+1])) start=true;
    }
    if (start && !o.empty()) o+=' ';
    o+=c;
  }
  return o;
}
// "empty grounds" -> "Empty Grounds"
std::string JuraLink::title_(const std::string &s) {
  std::string o; bool first=true;
  for (char c : s) {
    if (c==' ') { if (!o.empty() && o.back()!=' ') o+=' '; first=true; continue; }
    o += (first && c>='a'&&c<='z') ? char(c-32) : c;
    first=false;
  }
  while (!o.empty() && o.back()==' ') o.pop_back();
  return o;
}
static std::string json_str(const std::string &s) {
  std::string o="\"";
  for (char c : s) { if (c=='"'||c=='\\') o+='\\'; o+=c; }
  return o+"\"";
}
// jura-mqtt Attr slugs / labels / units, indexed by jura_jprf::Attr.
static const char *const ATTR_SLUG[] = {"coffee_strength", "grinder_ratio", "temperature", "water_amount",
                                        "milk_amount", "milk_foam_amount", "bypass", "milk_break"};
static const char *const ATTR_LABEL[] = {"Coffee Strength", "Grinder Ratio", "Temperature", "Water Amount",
                                         "Milk Amount", "Milk Foam Amount", "Bypass", "Milk Break"};
static const char *const ATTR_UNIT[] = {"", "", "", "mL", "mL", "mL", "mL", "s"};
// @TV: progress-state codes (J.O.E. ProgressState, via jura-connect PROTOCOL.md 5.10).
struct ProgState { uint8_t code; const char *name; };
static const ProgState PROG_STATES[] = {
  {0x01,"insert_tray"},{0x02,"fill_watertank"},{0x03,"empty_grounds"},{0x04,"empty_tray"},{0x08,"insert_grounds_box"},
  {0x09,"close_nozzle_cover"},{0x0A,"close_bean_cover"},{0x0C,"close_tap"},{0x0D,"open_tap"},{0x0E,"alarm"},
  {0x0F,"close_powder_cover"},{0x10,"add_powder_coffee"},{0x11,"filling_process"},{0x12,"system_emptying"},
  {0x13,"add_beans"},{0x14,"not_enough_powder"},{0x15,"waiting"},{0x16,"remove_watertank"},{0x19,"smart_alert"},
  {0x1E,"place_cup_for_coffee"},{0x1F,"place_cup_for_cleaning"},{0x20,"startup"},{0x21,"heating_up"},
  {0x23,"rinse_process"},{0x30,"popup_window"},{0x31,"milk_foam_bean_amount"},{0x32,"milk_foam_milk_volume"},
  {0x33,"milk_foam_pause"},{0x34,"milk_foam_volume"},{0x37,"milk_foam_water_volume"},{0x38,"milk_foam_no_adjustment"},
  {0x39,"coffee_bean_amount"},{0x3C,"coffee_water_amount"},{0x3D,"coffee_no_adjustment"},{0x3E,"enjoy"},
  {0x40,"hotwater_temperature"},{0x41,"hotwater_volume"},{0x42,"steam_time"},{0x43,"steam_temperature"},
  {0x4B,"grinder_setting_request"},{0x50,"descale_start"},{0x51,"descale_materials"},{0x52,"descale_empty_tray"},
  {0x53,"descale_add_fluid"},{0x54,"descale_process"},{0x55,"descale_rinse_watertank"},{0x56,"descale_finish"},
  {0x60,"filter_rinse_start"},{0x61,"filter_rinse_materials"},{0x62,"filter_rinse_change"},{0x63,"filter_rinse_process"},
  {0x65,"filter_rinse_finish"},{0x66,"filter_rinse_remove_filter"},{0x67,"filter_rinse_insert"},{0x70,"cleaning_start"},
  {0x71,"cleaning_materials"},{0x72,"cleaning_empty_tray"},{0x73,"cleaning_press_rotary"},{0x74,"cleaning_process"},
  {0x75,"cleaning_add_tablet"},{0x76,"cleaning_finish"},
};
// Maintenance processes started by <PROCESS ExecuteCommand="@TG:xx">; xx is the @TV: byte 1.
static const char *process_name(uint8_t code) {
  switch (code) {
    case 0x21: return "CappuClean"; case 0x22: return "CoffeeRinse"; case 0x23: return "CappuRinse";
    case 0x24: return "Cleaning";   case 0x25: return "Decalc";      case 0x26: return "FilterChange";
    default: return nullptr;
  }
}
bool JuraLink::alert_ignored_(uint16_t bit) const {
  for (auto b : ignore_bits_) if (b==bit) return true;
  return false;
}
void JuraLink::select_profile_(const std::string &model) {
  if (!profiles_.ok() || profiles_.selected()) return;
  if (!profiles_.select(model.c_str())) { ESP_LOGW(TAG, "no profile for %s", model.c_str()); return; }
  sw_version_ = model;
  ef_code_ = model.substr(0, model.find_first_not_of("EF0123456789"));
  // Model: YAML override > family from the profile DB ("S8") > EF code.
  const char *family = profiles_.model_name();
  machine_model_ = !machine_model_cfg_.empty() ? machine_model_cfg_ : (family[0] ? std::string(family) : ef_code_);
  machine_id_ = machine_id_cfg_.empty() ? slug_(machine_model_) : machine_id_cfg_;
  if (machine_id_.empty()) machine_id_ = "jura";
  build_entities_();
  poll_banks_ = 1u;   // bank 0: total
  // Every product of the profile, inactive ones too (e.g. Z10 "2 Coffee"): they count in the per-type totals.
  for (uint8_t i=0; i<profiles_.product_count(); ++i) {
    const auto p = profiles_.product(i);
    if ((p.code>>2) < STAT_BANKS) poll_banks_ |= 1u << (p.code>>2);
  }
  ESP_LOGI(TAG, "polling @TR:32 banks mask 0x%04X", static_cast<unsigned>(poll_banks_));
  ESP_LOGI(TAG, "profile %s: %d products, %d maint, %d alerts -> MQTT %s (%u entities)", model.c_str(),
           profiles_.product_count(), profiles_.maint_counter_count(), profiles_.alert_count(),
           base_().c_str(), static_cast<unsigned>(ents_.size()));
}
void JuraLink::build_entities_() {
  ents_.clear();
  const std::string b = base_();
  lock_bit_ = -1;
  for (uint16_t i=0; i<profiles_.alert_count(); ++i)
    if (slug_(profiles_.alert_name(i)) == "lockedkeys") lock_bit_ = int16_t(profiles_.alert_bit(i));
  auto add=[&](Ent k, uint8_t idx, bool disc, const std::string &id, const std::string &topic){
    ents_.push_back(MqttEnt{k, idx, disc, id, topic, std::string()});
  };
  add(Ent::CONNECTION, 0, true, "connection", b+"/connection");
  add(Ent::RSSI, 0, true, "rssi", b+"/rssi");
  add(Ent::STATUS, 0, true, "status", b+"/status");
  add(Ent::ACTIVE_ALERTS, 0, true, "active_alerts", b+"/active_alerts");
  for (uint16_t i=0; i<profiles_.alert_count(); ++i) {
    if (alert_ignored_(profiles_.alert_bit(i))) continue;
    const std::string s = slug_(profiles_.alert_name(i));
    if (!s.empty()) add(Ent::ALERT, uint8_t(i), true, "alert_"+s, b+"/alert/"+s);
  }
  add(Ent::TOTAL, 0, true, "total_products", b+"/total_products");
  // Every product gets a state topic; only active ones get an HA entity.
  for (uint8_t i=0; i<profiles_.product_count(); ++i) {
    const auto p = profiles_.product(i);
    const std::string s = slug_(p.name);
    if (!s.empty()) add(Ent::PRODUCT, i, p.active(), "count_"+s, b+"/product/"+s);
  }
  for (uint8_t i=0; i<profiles_.maint_pct_count(); ++i) {
    const std::string s = slug_(camel_words_(profiles_.maint_pct(i)));
    add(Ent::MAINT_PCT, i, true, "maintenance_"+s+"_percent", b+"/maintenance/"+s+"_percent");
  }
  for (uint8_t i=0; i<profiles_.maint_counter_count(); ++i) {
    const std::string s = slug_(camel_words_(profiles_.maint_counter(i)));
    add(Ent::MAINT_CNT, i, true, "maintenance_"+s+"_counter", b+"/maintenance/"+s+"_counter");
  }
  add(Ent::PROG_PERCENT, 0, true, "progress", b+"/progress/percent");
  add(Ent::PROG_STATE, 0, true, "progress_state", b+"/progress/state");
  add(Ent::PROG_PRODUCT, 0, true, "progress_product", b+"/progress/product");
  if (allow_brew_) {
    add(Ent::SEL_PRODUCT, 0, true, "product", b+"/select/product");
    std::vector<std::string> ch; uint16_t mn, mx, st;
    for (uint8_t a=0; a<jura_jprf::ATTR_COUNT; ++a) {
      choices_(a, ch);
      if (!ch.empty()) add(Ent::SEL_ATTR, a, true, ATTR_SLUG[a], b+"/select/"+ATTR_SLUG[a]);
    }
    for (uint8_t a=0; a<jura_jprf::ATTR_COUNT; ++a)
      if (bounds_(a, mn, mx, st)) add(Ent::NUM_ATTR, a, true, ATTR_SLUG[a], b+"/number/"+ATTR_SLUG[a]);
    add(Ent::BREW, 0, true, "brew", "");
  }
  if (allow_lock_) add(Ent::LOCK, 0, true, "lock", b+"/lock");
  add(Ent::REFRESH, 0, true, "refresh", "");
  disc_cursor_ = 0; state_cursor_ = 0; disc_done_ = false; mqtt_subscribed_ = false;
}
// Current state payload; "" = unknown, nothing is published.
std::string JuraLink::ent_value_(const MqttEnt &e) const {
  char b[16];
  auto num=[&](float v)->std::string { if (std::isnan(v)) return ""; snprintf(b,sizeof(b),"%.0f",v); return b; };
  switch (e.kind) {
    case Ent::CONNECTION: return link_up_() ? "ON" : "OFF";
    case Ent::RSSI:
#ifdef USE_WIFI
      if (wifi::global_wifi_component && wifi::global_wifi_component->is_connected()) {
        snprintf(b,sizeof(b),"%d",wifi::global_wifi_component->wifi_rssi()); return b;
      }
#endif
      return "";
    case Ent::STATUS: {
      if (status_seen_)
        for (uint16_t i=0; i<profiles_.alert_count(); ++i) {
          const uint16_t bit = profiles_.alert_bit(i);
          if (profiles_.alert_blocking(i) && !alert_ignored_(bit) && alert_active_(bit)) return "blocked";
        }
      return link_up_() ? "ready" : "idle";
    }
    case Ent::ACTIVE_ALERTS: {
      if (!status_seen_) return "";
      std::string o;
      for (uint16_t i=0; i<profiles_.alert_count(); ++i) {
        const uint16_t bit = profiles_.alert_bit(i);
        if (alert_ignored_(bit) || !alert_active_(bit)) continue;
        if (!o.empty()) o += ", ";
        o += profiles_.alert_name(i);
      }
      return o.empty() ? "none" : o;
    }
    case Ent::ALERT:
      if (!status_seen_) return "";
      return alert_active_(profiles_.alert_bit(e.idx)) ? "ON" : "OFF";
    case Ent::TOTAL: return num(product_count(0));
    case Ent::PRODUCT: return num(product_count(profiles_.product(e.idx).code));
    case Ent::MAINT_PCT:
      if (e.idx<8 && maint_pct_seen_[e.idx] && maint_pct_[e.idx]==0xFF) return "n/a";   // e.g. no filter fitted
      return num(maint_percent(e.idx));
    case Ent::MAINT_CNT: return num(maint_count(e.idx));
    case Ent::SEL_PRODUCT:
      return sel_prod_ >= 0 ? std::string(profiles_.product(uint8_t(sel_prod_)).name) : "";
    case Ent::SEL_ATTR: {
      if (!req_choice_[e.idx].empty()) return req_choice_[e.idx];
      if (sel_prod_ < 0) return "";
      const auto o = profiles_.find_option(profiles_.product(uint8_t(sel_prod_)), e.idx);   // product default
      if (!o.valid()) return "";
      for (uint8_t k=0; k<o.n_items; ++k) if (o.item_value(k) == o.def) return profiles_.item_name(o, k);
      return "";
    }
    case Ent::NUM_ATTR: {
      if (req_num_[e.idx] >= 0) return num(float(req_num_[e.idx]));
      if (sel_prod_ < 0) return "";
      const auto o = profiles_.find_option(profiles_.product(uint8_t(sel_prod_)), e.idx);
      return o.valid() && !o.is_list() ? num(float(o.def)) : "";
    }
    case Ent::LOCK:
      if (lock_bit_ >= 0 && status_seen_) return alert_active_(uint16_t(lock_bit_)) ? "ON" : "OFF";
      return lock_cmd_ < 0 ? "" : (lock_cmd_ ? "ON" : "OFF");
    case Ent::PROG_PERCENT: return num(float(prog_pct_));
    case Ent::PROG_STATE: {
      if (prog_state_ < 0) return "idle";
      for (const auto &ps : PROG_STATES) if (ps.code == prog_state_) return ps.name;
      snprintf(b, sizeof(b), "state_%02X", prog_state_); return b;
    }
    case Ent::PROG_PRODUCT: return prog_state_ < 0 ? "none" : progress_name_();
    case Ent::BREW: case Ent::REFRESH: return "";
  }
  return "";
}
// Home Assistant / openHAB discovery payload, matching jura-mqtt's component style.
std::string JuraLink::ent_discovery_(const MqttEnt &e, std::string &topic) const {
  const char *platform = "sensor";
  std::string name, c;
  auto st=[&]{ c += ",\"state_topic\":" + json_str(e.topic); };
  switch (e.kind) {
    case Ent::CONNECTION:
      platform="binary_sensor"; name="Connection"; st();
      c += ",\"device_class\":\"connectivity\",\"entity_category\":\"diagnostic\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"";
      break;
    case Ent::RSSI:
      name="Signal Strength"; st();
      c += ",\"device_class\":\"signal_strength\",\"unit_of_measurement\":\"dBm\",\"state_class\":\"measurement\",\"entity_category\":\"diagnostic\"";
      break;
    case Ent::STATUS: name="Status"; st(); c += ",\"icon\":\"mdi:coffee-maker\""; break;
    case Ent::ACTIVE_ALERTS: name="Active Alerts"; st(); c += ",\"icon\":\"mdi:alert-circle-outline\""; break;
    case Ent::ALERT:
      platform="binary_sensor"; name=title_(profiles_.alert_name(e.idx)); st();
      c += ",\"device_class\":\"problem\",\"entity_category\":\"diagnostic\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"";
      break;
    case Ent::TOTAL:
      name="Total Products"; st();
      c += ",\"state_class\":\"total_increasing\",\"unit_of_measurement\":\"products\",\"icon\":\"mdi:coffee\"";
      break;
    case Ent::PRODUCT:
      name=std::string(profiles_.product(e.idx).name)+" Count"; st();
      c += ",\"state_class\":\"total_increasing\",\"unit_of_measurement\":\"products\",\"icon\":\"mdi:coffee-outline\"";
      break;
    case Ent::MAINT_PCT:
      name=title_(camel_words_(profiles_.maint_pct(e.idx)))+" Progress"; st();
      c += ",\"unit_of_measurement\":\"%\",\"state_class\":\"measurement\",\"entity_category\":\"diagnostic\",\"icon\":\"mdi:progress-wrench\"";
      c += ",\"value_template\":\"{{ 'None' if value == 'n/a' else value }}\"";
      break;
    case Ent::MAINT_CNT:
      name=title_(camel_words_(profiles_.maint_counter(e.idx)))+" Counter"; st();
      c += ",\"state_class\":\"measurement\",\"unit_of_measurement\":\"products\",\"entity_category\":\"diagnostic\",\"icon\":\"mdi:counter\"";
      break;
    case Ent::SEL_PRODUCT: {
      platform="select"; name="Product"; st();
      c += ",\"command_topic\":" + json_str(e.topic+"/set") + ",\"options\":[";
      bool first=true;
      for (uint8_t i=0; i<profiles_.product_count(); ++i) {
        const auto p = profiles_.product(i);
        if (!p.active()) continue;
        c += (first ? "" : ",") + json_str(p.name); first=false;
      }
      c += "],\"icon\":\"mdi:coffee-maker\"";
      break;
    }
    case Ent::SEL_ATTR: {
      platform="select"; name=ATTR_LABEL[e.idx]; st();
      std::vector<std::string> ch; choices_(e.idx, ch);
      c += ",\"command_topic\":" + json_str(e.topic+"/set") + ",\"options\":[";
      for (size_t k=0; k<ch.size(); ++k) c += (k ? "," : "") + json_str(ch[k]);
      c += "]";
      break;
    }
    case Ent::NUM_ATTR: {
      platform="number"; name=ATTR_LABEL[e.idx]; st();
      uint16_t mn=0, mx=0, sp=1; bounds_(e.idx, mn, mx, sp);
      char b[80]; snprintf(b, sizeof(b), ",\"min\":%u,\"max\":%u,\"step\":%u,\"mode\":\"slider\"", mn, mx, sp);
      c += ",\"command_topic\":" + json_str(e.topic+"/set") + b;
      if (ATTR_UNIT[e.idx][0]) c += std::string(",\"unit_of_measurement\":\"") + ATTR_UNIT[e.idx] + "\"";
      break;
    }
    case Ent::LOCK:
      platform="switch"; name="Panel Lock"; st();
      c += ",\"command_topic\":" + json_str(e.topic+"/set") + ",\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"icon\":\"mdi:lock\"";
      break;
    case Ent::PROG_PERCENT:
      name="Progress"; st(); c += ",\"unit_of_measurement\":\"%\",\"icon\":\"mdi:progress-clock\"";
      break;
    case Ent::PROG_STATE: name="Progress State"; st(); c += ",\"icon\":\"mdi:coffee-maker-outline\""; break;
    case Ent::PROG_PRODUCT: name="Progress Product"; st(); c += ",\"icon\":\"mdi:coffee-outline\""; break;
    case Ent::BREW:
      platform="button"; name="Brew";
      c += ",\"command_topic\":" + json_str(base_()+"/brew/set") + ",\"payload_press\":\"PRESS\",\"icon\":\"mdi:coffee-to-go\"";
      break;
    case Ent::REFRESH:
      platform="button"; name="Refresh";
      c += ",\"command_topic\":" + json_str(base_()+"/refresh/set") +
           ",\"payload_press\":\"PRESS\",\"entity_category\":\"diagnostic\",\"icon\":\"mdi:refresh\"";
      break;
  }
  topic = discovery_prefix_ + "/" + platform + "/" + machine_id_ + "/" + e.id + "/config";
  std::string dev = "{\"identifiers\":[" + json_str(machine_id_) + "],\"name\":" + json_str("JURA "+machine_model_) +
    ",\"manufacturer\":\"JURA\",\"model\":" + json_str(machine_model_) + ",\"model_id\":" + json_str(ef_code_);
  if (!sw_version_.empty()) dev += ",\"sw_version\":" + json_str(sw_version_);
  dev += "}";
  std::string avail = ",\"availability_topic\":" + json_str(base_()+"/availability");
  if (e.kind == Ent::MAINT_PCT)   // "n/a" (0xFF from the machine) makes this one entity unavailable
    avail = ",\"availability\":[{\"topic\":" + json_str(base_()+"/availability") + "},{\"topic\":" + json_str(e.topic) +
            ",\"value_template\":\"{{ 'offline' if value == 'n/a' else 'online' }}\"}],\"availability_mode\":\"all\"";
  return "{\"name\":" + json_str(name) + ",\"unique_id\":" + json_str(machine_id_+"_"+e.id) + c + avail +
    ",\"payload_available\":\"online\",\"payload_not_available\":\"offline\",\"device\":" + dev +
    ",\"origin\":{\"name\":\"jura-link\",\"url\":\"https://github.com/Jutta-Proto/protocol-bt-cpp\"}}";
}
// @TV:<state><code><window...>: byte 0 = progress state, byte 1 = product (or process) code,
// the whole-product percentage is the second-to-last byte of a 16-byte frame (0x00..0x64).
// State FE (aroma preselection) / FF (P-mode, accessory menus) and unknown codes are not progress.
void JuraLink::parse_progress_(const std::string &plain) {
  const size_t n = (plain.size()-4)/2;
  if (n < 2) return;
  uint8_t v[32]; const size_t m = std::min<size_t>(n, sizeof(v));
  for (size_t i=0; i<m; ++i) v[i] = (uint8_t)strtol(plain.substr(4+i*2,2).c_str(), nullptr, 16);
  if (v[0] == 0xFE || v[0] == 0xFF) return;
  bool product = false;
  for (uint8_t i=0; i<profiles_.product_count(); ++i) if (profiles_.product(i).code == v[1]) product = true;
  const bool process = !product && process_name(v[1]) != nullptr;
  if (!product && !process) return;
  const bool changed = prog_state_ != v[0] || prog_code_ != v[1];
  prog_state_ = v[0]; prog_code_ = v[1]; prog_process_ = process; last_tv_ = millis();
  if (m == 16 && v[14] <= 0x64) prog_pct_ = v[14];
  if (v[0] == 0x3E) {   // ENJOY: finished
    prog_pct_ = 100;
    if (changed) ESP_LOGW(TAG, "BREW finished: %s", progress_name_().c_str());
  } else if (changed) {
    ESP_LOGI(TAG, "progress: %s state %02X, %d%%", progress_name_().c_str(), v[0], prog_pct_);
  }
}
std::string JuraLink::progress_name_() const {
  if (prog_code_ < 0) return "none";
  if (prog_process_) return process_name(uint8_t(prog_code_));
  for (uint8_t i=0; i<profiles_.product_count(); ++i)
    if (profiles_.product(i).code == prog_code_) return profiles_.product(i).name;
  return "unknown";
}
void JuraLink::choices_(uint8_t attr, std::vector<std::string> &out) const {
  out.clear();
  for (uint8_t i=0; i<profiles_.product_count(); ++i) {
    const auto p = profiles_.product(i);
    if (!p.active()) continue;
    const auto o = profiles_.find_option(p, attr);
    for (uint8_t k=0; o.valid() && k<o.n_items; ++k) {
      const std::string n = profiles_.item_name(o, k);
      if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
  }
}
bool JuraLink::bounds_(uint8_t attr, uint16_t &mn, uint16_t &mx, uint16_t &st) const {
  bool any = false;
  for (uint8_t i=0; i<profiles_.product_count(); ++i) {
    const auto p = profiles_.product(i);
    if (!p.active()) continue;
    const auto o = profiles_.find_option(p, attr);
    if (!o.valid() || o.is_list()) continue;
    if (!any) { mn=o.min; mx=o.max; st=o.step; any=true; }
    else { mn=std::min(mn,o.min); mx=std::max(mx,o.max); st=std::max(st,o.step); }
  }
  return any;
}
static bool ieq(const std::string &a, const char *b) {
  if (a.size() != std::strlen(b)) return false;
  for (size_t i=0; i<a.size(); ++i) if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
  return true;
}
bool JuraLink::select_product_(const std::string &name) {
  for (uint8_t i=0; i<profiles_.product_count(); ++i) {
    if (!ieq(name, profiles_.product(i).name)) continue;
    sel_prod_ = i;
    // A new product has its own defaults: drop overrides made for the previous one.
    for (auto &c : req_choice_) c.clear();
    for (auto &v : req_num_) v = -1;
    ESP_LOGI(TAG, "brew: product %s selected", profiles_.product(i).name);
    return true;
  }
  ESP_LOGW(TAG, "brew: unknown product '%s'", name.c_str());
  return false;
}
void JuraLink::brew_recipe(const std::string &hex) {
  if (!allow_brew_) { ESP_LOGW(TAG, "brew ignored: allow_brew is off"); return; }
  if (!profiles_.selected()) { ESP_LOGW(TAG, "brew: machine not identified yet"); return; }
  if (hex.size() != 32 || hex.find_first_not_of("0123456789ABCDEFabcdef") != std::string::npos) {
    ESP_LOGW(TAG, "brew: recipe must be 32 hex chars, got '%s'", hex.c_str()); return;
  }
  const uint8_t code = uint8_t(strtol(hex.substr(0,2).c_str(), nullptr, 16));
  if (hex.substr(16,2) != "01") { ESP_LOGW(TAG, "brew: recipe byte 8 must be 01: %s", hex.c_str()); return; }
  const char *name = nullptr;   // inactive products count: doubles are sent under their own code
  for (uint8_t i=0; i<profiles_.product_count(); ++i) if (profiles_.product(i).code == code) name = profiles_.product(i).name;
  if (!name) { ESP_LOGW(TAG, "brew: product %02X is not in this machine's profile", code); return; }
  std::string cmd = "@TP:";
  for (char c : hex) cmd += char(std::toupper((unsigned char) c));
  send_brew_(name, cmd);
}
void JuraLink::brew_() {
  if (sel_prod_ < 0) { ESP_LOGW(TAG, "brew requested but no product is selected"); return; }
  const auto p = profiles_.product(uint8_t(sel_prod_));
  int32_t req[jura_jprf::ATTR_COUNT];
  for (uint8_t a=0; a<jura_jprf::ATTR_COUNT; ++a) {
    req[a] = req_num_[a];
    if (req_choice_[a].empty()) continue;
    const auto o = profiles_.find_option(p, a);   // choice names map to this product's ITEM values
    for (uint8_t k=0; o.valid() && k<o.n_items; ++k)
      if (ieq(req_choice_[a], profiles_.item_name(o, k))) req[a] = o.item_value(k);
  }
  uint8_t blob[16];
  jura_jprf::recipe_blob(profiles_, p, req, blob);
  static const char hx[] = "0123456789ABCDEF";
  std::string cmd = "@TP:";
  for (auto b : blob) { cmd += hx[b>>4]; cmd += hx[b&15]; }
  send_brew_(p.name, cmd);
}
void JuraLink::send_brew_(const char *name, const std::string &cmd) {
  if (!session_ready_) {
    ESP_LOGW(TAG, "BREW %s %s: waiting for the handshake", name, cmd.c_str());
    pending_brew_ = cmd;
    start_handshake();
    return;
  }
  ESP_LOGW(TAG, "BREW %s %s", name, cmd.c_str());
  brew_sent_ = millis() | 1;
  query_at(cmd);
}
// Command topics, jura-mqtt layout: <base>/refresh/set, <base>/brew/set,
// <base>/select/<product|attr>/set, <base>/number/<attr>/set.
void JuraLink::mqtt_command_(const std::string &topic, const std::string &payload) {
  const std::string b = base_() + "/";
  if (topic.rfind(b, 0) != 0 || topic.size() < b.size()+4 || topic.compare(topic.size()-4, 4, "/set") != 0) return;
  const std::string path = topic.substr(b.size(), topic.size()-b.size()-4);
  ESP_LOGI(TAG, "MQTT command %s = %s", path.c_str(), payload.c_str());
  if (path == "refresh") { if (!engine_.busy()) poll_stats(); return; }
  if (path == "lock" && allow_lock_) {
    std::string v = payload; for (auto &ch : v) ch = char(std::toupper((unsigned char)ch));
    const bool on = (v == "ON" || v == "LOCK" || v == "1" || v == "TRUE");
    if (!on && !(v == "OFF" || v == "UNLOCK" || v == "0" || v == "FALSE")) { ESP_LOGW(TAG, "lock expects ON or OFF, got '%s'", payload.c_str()); return; }
    ESP_LOGW(TAG, "PANEL %s (@TS:%s)", on ? "LOCK" : "UNLOCK", on ? "01" : "00");
    lock_cmd_ = on;
    if (!session_ready_) start_handshake();
    query_at(on ? "@TS:01" : "@TS:00");
    return;
  }
  if (!allow_brew_) return;
  if (path == "brew") {
    // A product name as payload selects and brews in one message (jura-mqtt behaviour).
    if (!payload.empty() && payload != "PRESS") select_product_(payload);
    brew_();
    return;
  }
  if (path == "select/product") { select_product_(payload); return; }
  for (uint8_t a=0; a<jura_jprf::ATTR_COUNT; ++a) {
    if (path == std::string("select/")+ATTR_SLUG[a]) { req_choice_[a] = payload; return; }
    if (path == std::string("number/")+ATTR_SLUG[a]) {
      char *end = nullptr;
      const double v = strtod(payload.c_str(), &end);   // openHAB sends "100.0"
      if (end == payload.c_str() || !(v >= 0)) { ESP_LOGW(TAG, "number %s expects a value, got '%s'", ATTR_SLUG[a], payload.c_str()); return; }
      req_num_[a] = int32_t(v + 0.5);
      return;
    }
  }
}
void JuraLink::mqtt_publish_() {
#ifdef USE_MQTT
  auto *cl = mqtt::global_mqtt_client;
  if (!mqtt_discovery_ || cl==nullptr || ents_.empty()) return;
  const std::string avail = base_()+"/availability";
  if (cl->get_availability().topic != avail) {
    // The machine id is known only after the handshake, so point birth/will/shutdown
    // at its availability topic now and reconnect once so the broker holds the new will.
    ESP_LOGI(TAG, "MQTT availability -> %s (reconnecting)", avail.c_str());
    cl->set_birth_message(mqtt::MQTTMessage{avail, "online", 1, true});
    cl->set_last_will(mqtt::MQTTMessage{avail, "offline", 1, true});
    cl->set_shutdown_message(mqtt::MQTTMessage{avail, "offline", 1, true});
    if (cl->is_connected()) { cl->disable(); cl->enable(); }
    mqtt_was_connected_ = false;
    return;
  }
  const bool up = cl->is_connected();
  if (!up) { mqtt_was_connected_ = false; return; }
  if (!mqtt_was_connected_) {          // (re)connected: announce, rediscover, resend all state
    mqtt_was_connected_ = true;
    cl->publish(base_()+"/availability", std::string("online"), 0, true);
    disc_cursor_ = 0; disc_done_ = false;
    for (auto &e : ents_) e.last.clear();
  }
  if (!mqtt_subscribed_) {
    mqtt_subscribed_ = true;
    auto cb = [this](const std::string &t, const std::string &p) { mqtt_command_(t, p); };
    cl->subscribe(base_()+"/+/set", cb);
    if (allow_brew_) cl->subscribe(base_()+"/+/+/set", cb);
  }
  // Phase 1: retained discovery, a few per tick so the TX buffer never overruns.
  if (!disc_done_) {
    std::string topic;
    for (int n=0; n<4 && disc_cursor_<ents_.size(); ++disc_cursor_) {
      const auto &e = ents_[disc_cursor_];
      if (!e.discover) continue;
      const std::string cfg = ent_discovery_(e, topic);
      cl->publish(topic, cfg, 0, true);
      ++n;
    }
    if (disc_cursor_ >= ents_.size()) { disc_done_ = true; mqtt_heartbeat_ = millis(); }
    return;
  }
  // Phase 2: retained state on change; every 5 min resend everything in case
  // the broker lost its retained store.
  if (uint32_t(millis()-mqtt_heartbeat_) >= 300000) {
    for (auto &e : ents_) e.last.clear();
    mqtt_heartbeat_ = millis();
  }
  int sent = 0;
  for (size_t step=0; step<ents_.size() && sent<10; ++step) {
    auto &e = ents_[state_cursor_];
    state_cursor_ = (state_cursor_ + 1) % ents_.size();
    if (e.topic.empty()) continue;
    const std::string v = ent_value_(e);
    if (v.empty() || v == e.last) continue;
    cl->publish(e.topic, v, 0, true);
    e.last = v; ++sent;
  }
#endif
}
void JuraLink::event(jura_protocol::Event e, jura_protocol::Query q, const std::string &payload) {
  using jura_protocol::Event;
  dirty_ = true;
  switch (e) {
    case Event::SENT:
      ++requests_count_;
      ESP_LOGD(TAG, "TX %s", engine_.active_command().c_str());
      break;
    case Event::REPLY:
      ++replies_count_;
      ESP_LOGI(TAG, "RX %s", payload.c_str());
      if (connected_) connected_->publish_state(true);
      if (last_reply_) last_reply_->publish_state(payload);
      if (last_error_) last_error_->publish_state("None");
      if (q == jura_protocol::Query::TYPE) { if (machine_type_) machine_type_->publish_state(payload.substr(3)); select_profile_(payload.substr(3)); }
      if (!payload.empty() && payload[0]=='&') publish_at_(payload, true);
      break;
    case Event::UNSOLICITED: {
      ++unsolicited_count_;
      // A bounded hex preview avoids publishing raw binary/NUL in web JSON.
      static const char hex[] = "0123456789ABCDEF";
      std::string preview;
      for (size_t i=0; i<payload.size() && i<96; ++i) {
        if (i) preview += ' ';
        const auto b=static_cast<uint8_t>(payload[i]);
        preview += hex[b>>4]; preview += hex[b&15];
      }
      if (payload.size()>96) preview += " ...";
      if (listen_only_) {
        ESP_LOGD(TAG, "Passive RX (%u bytes): %s", static_cast<unsigned>(payload.size()), preview.c_str());
      } else {
        ESP_LOGV(TAG, "Unsolicited (%u bytes): %s", static_cast<unsigned>(payload.size()), preview.c_str());
      }
      if (last_broadcast_) last_broadcast_->publish_state(preview);
      if (!payload.empty() && payload[0]=='&') publish_at_(payload, false);
      break;
    }
    case Event::TIMEOUT:
      ++timeouts_count_;
      ESP_LOGW(TAG, "Timeout: %s", engine_.active_command().c_str());
      if (q == jura_protocol::Query::TYPE && connected_) connected_->publish_state(false);
      if (last_error_) last_error_->publish_state(std::string("Timeout: ")+engine_.active_command().c_str());
      break;
    case Event::ERROR_REPLY:
      ESP_LOGW(TAG, "Machine rejected %s: %s", engine_.active_command().c_str(), payload.c_str());
      if (last_error_) last_error_->publish_state(payload);
      break;
    case Event::RETRY:
      ESP_LOGD(TAG, "%s displaced by a push, sending it again", payload.c_str());
      break;
    case Event::INVALID_FRAME:
      ++framing_count_;
      ESP_LOGV(TAG, "Invalid/oversized/incomplete frame; resynchronizing at idle gap");
      break;
    case Event::REJECTED:
      ++rejected_count_;
      ESP_LOGW(TAG, "Request rejected: %s", payload.c_str());
      break;
  }
}
} }
