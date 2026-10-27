#pragma once
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/optional.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "protocol.h"
#include "jura_jprf.h"
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>
#include <initializer_list>

namespace esphome { namespace jura_link {
class JuraLink : public PollingComponent, public uart::UARTDevice, public jura_protocol::Sink {
 public:
  JuraLink() : engine_(*this) {}
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  void set_listen_only(bool v) { listen_only_ = v; }
  // Passive capture: no TX pin, no engine. Logs every idle-gap burst losslessly.
  void set_sniff(bool v) { sniff_ = v; if (v) listen_only_ = true; }
  // Labels log lines so two instances on different UARTs stay distinguishable.
  void set_sniff_tag(const std::string &tag) { sniff_tag_ = tag; }
  void set_auto_poll(bool v) { auto_poll_ = v; }
  void set_mqtt_discovery(bool v) { mqtt_discovery_ = v; }
  void set_base_topic(const std::string &v) { base_topic_ = v; }
  void set_discovery_prefix(const std::string &v) { discovery_prefix_ = v; }
  void set_machine_id(const std::string &v) { machine_id_cfg_ = v; }
  void set_machine_model(const std::string &v) { machine_model_cfg_ = v; }
  void set_ignore_alert_bits(const std::vector<uint16_t> &v) { ignore_bits_ = v; }
  // Brewing over MQTT. Off by default: a Brew press makes the machine pour.
  void set_allow_brew(bool v) { allow_brew_ = v; }
  // Panel lock switch over MQTT (@TS:01 / @TS:00). Off by default.
  void set_allow_lock(bool v) { allow_lock_ = v; }
  void set_response_timeout(uint32_t v) { engine_.set_timeout(v); }
  void query_type() { if (!listen_only_) engine_.enqueue(jura_protocol::Query::TYPE); }
  void query_rt_address(uint16_t address) { if (!listen_only_) engine_.enqueue_rt(address); }
  void query_re_address(uint16_t address) { if (!listen_only_) engine_.enqueue_re(address); }
  // Send an encrypted @-protocol command (default read-only intent). Appends
  // CRLF if the caller omitted it. Blocked in listen_only mode.
  // Start the dongle handshake (AT-PROTOCOL.md section 3): we send @T0 then @T1;
  // the machine then prompts @T2/@T3 which publish_at_() answers automatically.
  // Must complete before the machine will answer any data command.
  void start_handshake() { if (listen_only_) return; handshake_done_=false; session_ready_=false; t2_seen_=false; hs_started_=millis(); engine_.clear_queue(); query_at("@T0"); query_at("@T1"); }
  // Poll the statistics banks (@TR:32,00..0F product counters, @TG:43/C0
  // maintenance). Read-only. Runs the handshake first if not yet done.
  void poll_stats() {
    if (listen_only_) return;
    // No session: (re)start the handshake only; the poll follows from on_session_ready_().
    if (!session_ready_) { if (!hs_started_) start_handshake(); return; }
    for (int nn=0; nn<STAT_BANKS; ++nn) {
      if (!(poll_banks_ & (1u<<nn))) continue;
      char c[12]; std::snprintf(c,sizeof(c),"@TR:32,%02X",nn); query_at(c);
    }
    query_at("@TG:43"); query_at("@TG:C0");
    // Settings change rarely: read them in the first poll of a session, then every 10th.
    if (settings_countdown_-- == 0) { for (const auto &a : setting_args_) read_setting(a); settings_countdown_ = 9; }
  }
  // --- Machine settings (@TM:<P_Argument>, jura-connect PROTOCOL.md 5.7) ---
  void add_setting(const std::string &arg) { setting_args_.push_back(arg); setting_vals_.emplace_back(); }
  void read_setting(const std::string &arg) { query_at("@TM:" + arg); }
  // Write <value_hex> to setting <arg>: wrapped in a panel lock, as the J.O.E. app does
  // (without it the machine acknowledges but ignores the value), then read back.
  void write_setting(const std::string &arg, const std::string &value_hex) {
    if (listen_only_ || value_hex.empty() || value_hex.size() % 2 ||
        value_hex.find_first_not_of("0123456789ABCDEF") != std::string::npos) {
      ESP_LOGW("jura_link", "setting %s: refusing value '%s'", arg.c_str(), value_hex.c_str()); return;
    }
    const std::string body = arg + "," + value_hex;
    char cs[3]; std::snprintf(cs, sizeof(cs), "%02X", setting_checksum_(body));
    ESP_LOGW("jura_link", "setting %s <- %s", arg.c_str(), value_hex.c_str());
    query_at("@TS:01"); query_at("@TM:" + body + cs); query_at("@TS:00"); read_setting(arg);
  }
  // Last value read for <arg> (hex, checksum stripped); empty until read.
  optional<std::string> setting(const std::string &arg) const {
    for (size_t i=0; i<setting_args_.size(); ++i) if (setting_args_[i]==arg && !setting_vals_[i].empty()) return setting_vals_[i];
    return {};
  }
  // Position of the current value of <arg> in <values> (a select's value list). A one-byte
  // tagged value "21xx" reads back as "xx"; the two-byte form "22xxxx" reads back whole.
  optional<size_t> setting_index(const std::string &arg, const std::vector<std::string> &values) const {
    auto v = setting(arg);
    if (!v) return {};
    for (size_t i=0; i<values.size(); ++i) {
      const std::string &w = values[i];
      if (*v == w || (w.size() == 4 && w.compare(0, 2, "21") == 0 && *v == w.substr(2))) return i;
    }
    return {};
  }
  // ByteOperations.d in the J.O.E. app: (-1 - sum of the characters) & 0xFF.
  static uint8_t setting_checksum_(const std::string &s) { int sum=0; for (char c : s) sum += (unsigned char) c; return uint8_t((-1 - sum) & 0xFF); }
  bool handshake_done() const { return handshake_done_; }
  // Brew a ready-made 16-byte @TP: recipe (32 hex chars), built by the dashboard card from
  // the machine XML. Checked for shape only: byte 0 a product of this machine, byte 8 = 01.
  void brew_recipe(const std::string &hex);
  // Hex payload for the @t2 answer; takes effect at the next handshake.
  void set_t2_reply(const std::string &hex) { t2_reply_ = hex; }
  // code 0 = grand total; other codes per the machine XML profile.
  // NAN = not polled yet. A reported 0xFFFF counter means "never used" -> 0;
  // percentages clamp to 100 (0xFF -> 100), as the jura-mqtt BT bridge does.
  float product_count(int code) const { if(!counters_seen_[code&0xFF]) return NAN; uint16_t v=counters_[code&0xFF]; return v==0xFFFF?0:v; }
  float maint_count(int i) const { if(i<0||i>=8||!maint_seen_[i]) return NAN; uint16_t v=maint_[i]; return v==0xFFFF?0:v; }
  float maint_percent(int i) const { if(i<0||i>=8||!maint_pct_seen_[i]) return NAN; uint8_t v=maint_pct_[i]; return v>100?100:v; }
  // --- Values for native ESPHome entities (template lambdas). Empty = not read yet, so nothing is published. ---
  optional<float> product(uint8_t code) const { float v=product_count(code); if (std::isnan(v)) return {}; return v; }
  // Sum of several product counters (the Bluetooth integration's per-type totals); empty until all were read.
  optional<float> product_sum(std::initializer_list<uint8_t> codes) const {
    float sum = 0;
    for (uint8_t c : codes) { float v = product_count(c); if (std::isnan(v)) return {}; sum += v; }
    return sum;
  }
  optional<float> maint_counter(int i) const { float v=maint_count(i); if (std::isnan(v)) return {}; return v; }
  // 0xFF means the counter does not apply (e.g. no filter fitted): NAN, shown as unknown.
  optional<float> maint_pct(int i) const {
    if (i<0||i>=8||!maint_pct_seen_[i]) return {};
    return maint_pct_[i]==0xFF ? NAN : maint_percent(i);
  }
  optional<bool> alert(uint16_t bit) const { if (!status_seen_) return {}; return alert_active_(bit); }
  bool link_up() const { return link_up_(); }
  // Value of a profile entity by its MQTT-layout id ("status", "active_alerts", "progress_state", ...).
  optional<std::string> entity_value(const std::string &id) const {
    for (const auto &e : ents_) if (e.id == id) { std::string v = ent_value_(e); if (v.empty()) return {}; return v; }
    return {};
  }
  // Send one encrypted @-command. Builds the '&' frame (cipher+escape) and
  // queues it for transmission. `key` is the per-frame cipher key (any value;
  // the machine reads it from the frame). CRLF appended if missing.
  // NOTE: read-only unless you pass a state-changing command (@TM/@TP).
  void query_at(std::string cmd, int key=0x2A) {
    if (listen_only_) return;
    if (cmd.size()<2 || cmd.compare(cmd.size()-2,2,"\r\n")!=0) cmd += "\r\n";
    engine_.enqueue_at(cmd, static_cast<uint8_t>(key));
  }
  void query_rt_counters() { if (!listen_only_) engine_.enqueue(jura_protocol::Query::RT_COUNTERS); }
  void query_rt_0010() { if (!listen_only_) engine_.enqueue(jura_protocol::Query::RT_0010); }
  void query_rt_0020() { if (!listen_only_) engine_.enqueue(jura_protocol::Query::RT_0020); }
  void query_products() { if (!listen_only_) engine_.enqueue(jura_protocol::Query::PRODUCTS); }
  void query_maintenance() { if (!listen_only_) engine_.enqueue(jura_protocol::Query::MAINTENANCE); }
  void query_maintenance_percent() { if (!listen_only_) engine_.enqueue(jura_protocol::Query::MAINTENANCE_PERCENT); }
  void set_connected(binary_sensor::BinarySensor *s) { connected_ = s; }
#define JURA_TEXT(name) void set_##name(text_sensor::TextSensor *s) { name##_ = s; }
  JURA_TEXT(machine_type) JURA_TEXT(last_reply) JURA_TEXT(last_broadcast) JURA_TEXT(last_error) JURA_TEXT(at_decoded)
#undef JURA_TEXT
#define JURA_SENSOR(name) void set_##name(sensor::Sensor *s) { name##_ = s; }
  JURA_SENSOR(requests) JURA_SENSOR(replies) JURA_SENSOR(timeouts)
  JURA_SENSOR(unsolicited_frames) JURA_SENSOR(framing_errors) JURA_SENSOR(rejected_requests)
#undef JURA_SENSOR
  uint32_t send(const uint8_t *data, size_t size, uint32_t now) override;
  void event(jura_protocol::Event event, jura_protocol::Query query, const std::string &payload) override;
 protected:
  void publish_counters_();
  void publish_at_(const std::string &ampframe, bool reply);
  void parse_stats_(const std::string &plain);
  void select_profile_(const std::string &model);   // pick machine by EF### string
  jura_jprf::Profiles profiles_{};
  static constexpr int STAT_BANKS = 15;   // 15 banks reach code 0x3B (Z10 profile goes up to 0x39)
  bool handshake_done_{false};
  bool auto_poll_{false};
  // --- MQTT: same topic layout and discovery as the jura-mqtt BT bridge ---
  //   <base>/<id>/{availability,connection,rssi,status,active_alerts,total_products}
  //   <base>/<id>/alert/<slug>, product/<slug>, maintenance/<slug>_{percent,counter}
  //   <prefix>/<platform>/<id>/<component>/config
  enum class Ent : uint8_t { CONNECTION, RSSI, STATUS, ACTIVE_ALERTS, ALERT, TOTAL, PRODUCT, MAINT_PCT, MAINT_CNT,
                             SEL_PRODUCT, SEL_ATTR, NUM_ATTR, BREW, LOCK, PROG_PERCENT, PROG_STATE, PROG_PRODUCT, REFRESH };
  struct MqttEnt { Ent kind; uint8_t idx; bool discover; std::string id, topic, last; };
  void mqtt_publish_();
  void build_entities_();
  std::string ent_value_(const MqttEnt &e) const;
  std::string ent_discovery_(const MqttEnt &e, std::string &topic) const;
  bool alert_ignored_(uint16_t bit) const;
  // --- Brew (allow_brew): jura-mqtt's select/number/button entities -> @TP: recipe blob ---
  void mqtt_command_(const std::string &topic, const std::string &payload);
  bool select_product_(const std::string &name);
  void brew_();
  void send_brew_(const char *name, const std::string &cmd);
  void choices_(uint8_t attr, std::vector<std::string> &out) const;          // union over active products
  bool bounds_(uint8_t attr, uint16_t &mn, uint16_t &mx, uint16_t &st) const;  // union over active products
  bool allow_brew_{false};
  int sel_prod_{-1};                                    // index into profiles_ products
  std::string req_choice_[jura_jprf::ATTR_COUNT];       // pick-list overrides, by ITEM name
  int32_t req_num_[jura_jprf::ATTR_COUNT];              // range overrides (ml / s), -1 = default
  std::string pending_brew_;                            // @TP: command waiting for the handshake
  bool session_ready_{false};                           // @T3 seen since the last start_handshake()
  std::string t2_reply_{"818000"};                      // hex payload answered to the machine's @T2 challenge
  std::vector<std::string> setting_args_, setting_vals_;
  int settings_countdown_{0};
  uint32_t hs_started_{0};                              // millis() of the last start_handshake(), 0 = none running
  uint32_t t1_at_{0};                                   // millis() of the last @t1
  bool t2_seen_{false};                                 // @T2 key exchange seen since start_handshake()
  uint32_t poll_banks_{(1u<<STAT_BANKS)-1};             // @TR:32 banks to poll; narrowed to the profile's products
  void on_session_ready_();
  uint32_t brew_sent_{0};                               // millis() of the last @TP:, 0 = none
  // --- Product/process progress from @TV: pushes (layout: jura-connect PROTOCOL.md 5.10) ---
  void parse_progress_(const std::string &plain);
  std::string progress_name_() const;                   // product / process of the running progress
  int16_t prog_state_{-1};                              // progress-state code, -1 = idle
  int16_t prog_code_{-1};                               // byte 1: product or process code
  int16_t prog_pct_{0};
  bool prog_process_{false};
  uint32_t last_tv_{0};
  // --- Panel lock (allow_lock): @TS:01 / @TS:00, state from the LockedKeys alert bit ---
  bool allow_lock_{false};
  int16_t lock_bit_{-1};                                // alert bit named LockedKeys, -1 = unknown
  int8_t lock_cmd_{-1};                                 // last commanded state when the bit is unknown
  bool alert_active_(uint16_t bit) const { return (bit>>3) < status_len_ && (status_[bit>>3] >> (7-(bit&7)) & 1); }
  bool link_up_() const { return last_rx_ != 0 && uint32_t(millis()-last_rx_) < 10000; }
  std::string base_() const { return base_topic_ + "/" + machine_id_; }
  static std::string slug_(const std::string &s);
  static std::string camel_words_(const std::string &s);
  static std::string title_(const std::string &s);
  bool mqtt_discovery_{false};
  std::string base_topic_{"jura"}, discovery_prefix_{"homeassistant"};
  std::string machine_id_cfg_, machine_model_cfg_;       // from YAML; empty = derive from EF code
  std::string machine_id_, machine_model_, ef_code_, sw_version_;
  std::vector<uint16_t> ignore_bits_;
  std::vector<MqttEnt> ents_;
  size_t disc_cursor_{0}, state_cursor_{0};
  bool disc_done_{false}, mqtt_was_connected_{false}, mqtt_subscribed_{false};
  uint32_t mqtt_tick_{0}, mqtt_heartbeat_{0}, last_rx_{0};
  uint8_t status_[32]{};   // @TF bytes; alert bit i = byte i>>3, MSB first
  uint8_t status_len_{0};
  bool status_seen_{false};
  uint16_t counters_[256];   // product counter by XML Code; [0]=total
  uint16_t maint_[8];
  uint8_t maint_pct_[8];
  bool maint_pct_seen_[8]{};
  bool counters_seen_[256]{};
  bool maint_seen_[8]{};
  void flush_sniff_();
  jura_protocol::Engine engine_;
  binary_sensor::BinarySensor *connected_{nullptr};
  text_sensor::TextSensor *machine_type_{nullptr}, *last_reply_{nullptr}, *last_broadcast_{nullptr}, *last_error_{nullptr}, *at_decoded_{nullptr};
  sensor::Sensor *requests_{nullptr}, *replies_{nullptr}, *timeouts_{nullptr}, *unsolicited_frames_{nullptr}, *framing_errors_{nullptr}, *rejected_requests_{nullptr};
  uint32_t requests_count_{0}, replies_count_{0}, timeouts_count_{0}, unsolicited_count_{0}, framing_count_{0}, rejected_count_{0};
  uint32_t last_publish_{0};
  bool dirty_{false};
  bool listen_only_{false};
  bool sniff_{false};
  std::vector<uint8_t> sniff_raw_;
  std::string sniff_tag_{"rx"};
  uint32_t sniff_start_{0}, sniff_last_{0}, sniff_seq_{0}, sniff_overflow_{0};
};
} }
