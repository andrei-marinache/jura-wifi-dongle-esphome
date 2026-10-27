#pragma once
// Reader for the packed JPRF machine-profile blob (see dongle/pack_profiles.py).
// One embedded blob holds every supported machine; look one up by its EF code
// (from TY:/@T3, e.g. "EF536"). No XML, no decompression — direct field access.
//
// Layout (little-endian), from pack_profiles.py:
//   header: 'JPRF'(4) u16 version(=4), u16 machine_count, u32 strpool_off
//   index:  machine_count * { u16 ef_str, u16 name_str, u32 rec_off }
//   record @rec_off:
//     u8 n_products
//     product[]: u8 code, u16 name, u8 flags(bit0 active, bit1 double), u8 n_opts,
//                opt[]: u8 attr, u8 arg, u16 def, u16 min, u16 max, u16 step, u8 n_items,
//                       item[]: u8 value, u16 name
//     u8 n_mc;  mc[]:  u16 label
//     u8 n_mp;  mp[]:  u16 label
//     u16 n_alerts; alert[]: u16 bit (bit15 = blocking), u16 name
//   strpool @strpool_off: NUL-terminated UTF-8; refs are u16 offsets into it.
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace jura_jprf {
static const uint16_t VERSION = 4;
static const size_t IDX_SZ = 8;       // index entry: u16 ef, u16 name, u32 rec_off
static const size_t PROD_HDR = 5;     // u8 code, u16 name, u8 flags, u8 n_opts
static const size_t OPT_HDR = 11;     // u8 attr, u8 arg, u16 def, min, max, step, u8 n_items
static const size_t ITEM_SZ = 3;      // u8 value, u16 name
static const uint8_t MAX_PRODUCTS = 64;

// Brew option attributes, in jura-mqtt's Attr order.
enum Attr : uint8_t { COFFEE_STRENGTH, GRINDER_RATIO, TEMPERATURE, WATER_AMOUNT,
                      MILK_AMOUNT, MILK_FOAM_AMOUNT, BYPASS, MILK_BREAK, ATTR_COUNT };

// Unaligned little-endian reads (records are packed, not aligned).
inline uint16_t rd16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24); }

// One brew parameter of a product. Pick lists have items; ranges have min/max/step
// in the XML's units (ml, or seconds for milk).
struct Option {
  uint8_t attr{ATTR_COUNT}, arg{0}, n_items{0};
  uint16_t def{0}, min{0}, max{0}, step{1};
  const uint8_t *items{nullptr};   // n_items * {u8 value, u16 name}
  bool valid() const { return attr < ATTR_COUNT; }
  bool is_list() const { return n_items > 0; }
  uint8_t item_value(uint8_t k) const { return items[k*ITEM_SZ]; }
  uint16_t item_name_ref(uint8_t k) const { return rd16(items + k*ITEM_SZ + 1); }
};

struct Product {
  uint8_t code{0}, flags{0}, n_opts{0};
  const char *name{""};
  const uint8_t *opts{nullptr};
  bool active() const { return flags & 1; }
  bool is_double() const { return flags & 2; }
};

class Profiles {
 public:
  Profiles() : d_(nullptr), len_(0) {}
  Profiles(const uint8_t *blob, size_t len) : d_(blob), len_(len) {
    ok_ = len >= 12 && std::memcmp(blob, "JPRF", 4) == 0;
    if (!ok_) return;
    count_ = rd16(d_ + 6);
    strpool_ = rd32(d_ + 8);
    ok_ = strpool_ <= len_ && rd16(d_ + 4) == VERSION;
  }
  bool ok() const { return ok_; }
  uint16_t machine_count() const { return count_; }

  const char *str(uint16_t off) const {
    const size_t p = strpool_ + off;
    return (p < len_) ? reinterpret_cast<const char *>(d_ + p) : "";
  }

  // Select a machine by EF code (e.g. "EF536"). Matches by prefix so
  // "EF536M V02.01" or "EF536" both resolve; the longest matching code wins
  // ("EF532V2..." picks EF532V2, not EF532). Returns false if not found.
  bool select(const char *ef) {
    size_t best = 0; uint32_t rec = 0; uint16_t name = 0;
    for (uint16_t i = 0; i < count_; ++i) {
      const uint8_t *e = d_ + 12 + i * IDX_SZ;
      const char *code = str(rd16(e));
      const size_t nlen = std::strlen(code);
      if (nlen > best && std::strncmp(ef, code, nlen) == 0) { best = nlen; rec = rd32(e + 4); name = rd16(e + 2); }
    }
    if (!best || rec >= strpool_) return false;
    rec_ = rec; name_ = name;
    // Products are variable-length: remember where each one starts.
    n_prod_ = d_[rec_] < MAX_PRODUCTS ? d_[rec_] : MAX_PRODUCTS;
    size_t o = rec_ + 1;
    for (uint8_t i = 0; i < d_[rec_]; ++i) {
      if (i < MAX_PRODUCTS) prod_off_[i] = uint32_t(o);
      const uint8_t n_opts = d_[o + 4];
      o += PROD_HDR;
      for (uint8_t j = 0; j < n_opts; ++j) o += OPT_HDR + size_t(d_[o + 10]) * ITEM_SZ;
    }
    mc_off_ = o;
    return true;
  }
  // Model family of the selected machine from JOE_MACHINES.TXT ("S8"), "" if unknown.
  const char *model_name() const { return rec_ ? str(name_) : ""; }
  bool selected() const { return rec_ != 0; }

  // --- accessors for the selected machine ---
  uint8_t product_count() const { return rec_ ? n_prod_ : 0; }
  Product product(uint8_t i) const {
    Product p{};
    if (i >= product_count()) return p;
    const uint8_t *r = d_ + prod_off_[i];
    p.code = r[0]; p.name = str(rd16(r + 1)); p.flags = r[3]; p.n_opts = r[4]; p.opts = r + PROD_HDR;
    return p;
  }
  // j-th brew option of a product.
  Option option(const Product &p, uint8_t j) const {
    Option o{};
    if (j >= p.n_opts) return o;
    const uint8_t *r = p.opts;
    for (uint8_t k = 0; k < j; ++k) r += OPT_HDR + size_t(r[10]) * ITEM_SZ;
    o.attr = r[0]; o.arg = r[1]; o.def = rd16(r + 2); o.min = rd16(r + 4); o.max = rd16(r + 6);
    o.step = rd16(r + 8) ? rd16(r + 8) : 1; o.n_items = r[10]; o.items = r + OPT_HDR;
    return o;
  }
  // The product's option for `attr`, or an invalid Option if it has none.
  Option find_option(const Product &p, uint8_t attr) const {
    for (uint8_t j = 0; j < p.n_opts; ++j) { Option o = option(p, j); if (o.attr == attr) return o; }
    return Option{};
  }
  const char *item_name(const Option &o, uint8_t k) const { return k < o.n_items ? str(o.item_name_ref(k)) : ""; }

  // maintenance-counter labels (@TG:43 order)
  uint8_t maint_counter_count() const { return rec_ ? d_[mc_off_] : 0; }
  const char *maint_counter(uint8_t i) const { return str(rd16(d_ + mc_off_ + 1 + i*2)); }
  // maintenance-percent labels (@TG:C0 order)
  uint8_t maint_pct_count() const { return rec_ ? d_[mp_off_()] : 0; }
  const char *maint_pct(uint8_t i) const { return str(rd16(d_ + mp_off_() + 1 + i*2)); }
  // alerts
  uint16_t alert_count() const { return rec_ ? rd16(d_ + al_off_()) : 0; }
  uint16_t alert_bit(uint16_t i) const { return rd16(d_ + al_off_() + 2 + i*4) & 0x7FFF; }
  // Type="block" in the XML: the machine refuses to brew while it is set.
  bool alert_blocking(uint16_t i) const { return rd16(d_ + al_off_() + 2 + i*4) & 0x8000; }
  const char *alert_name(uint16_t i) const { return str(rd16(d_ + al_off_() + 2 + i*4 + 2)); }

 private:
  size_t mp_off_() const { return mc_off_ + 1 + size_t(d_[mc_off_]) * 2; }
  size_t al_off_() const { return mp_off_() + 1 + size_t(d_[mp_off_()]) * 2; }

  const uint8_t *d_; size_t len_;
  bool ok_ = false; uint16_t count_ = 0; uint32_t strpool_ = 0; uint32_t rec_ = 0; uint16_t name_ = 0;
  uint8_t n_prod_ = 0; uint32_t prod_off_[MAX_PRODUCTS]{}; size_t mc_off_ = 0;
};

// Build the 16-byte @TP: recipe blob for `p` (format verified by brewing, see
// makefu/jura-connect docs/PROTOCOL.md §5.9): byte 0 = product code, each option at
// byte arg-1, byte 8 = 0x01, everything else 0x00. `req[attr]` is the requested value
// (a pick-list ITEM value, or ml/seconds for ranges); a negative entry uses the
// product's default. Ranges are clamped and snapped to the product's step. Water and
// bypass travel in 5 ml ticks, everything else as-is.
inline void recipe_blob(const Profiles &prof, const Product &p, const int32_t req[ATTR_COUNT], uint8_t out[16]) {
  std::memset(out, 0, 16);
  out[0] = p.code;
  out[8] = 0x01;
  for (uint8_t j = 0; j < p.n_opts; ++j) {
    const Option o = prof.option(p, j);
    if (!o.valid() || o.arg < 2 || o.arg > 16 || o.arg == 9) continue;   // byte 8 is fixed
    int32_t v = (req && req[o.attr] >= 0) ? req[o.attr] : int32_t(o.def);
    uint8_t b;
    if (o.is_list()) {
      bool known = false;
      for (uint8_t k = 0; k < o.n_items; ++k) if (o.item_value(k) == v) known = true;
      b = uint8_t(known ? v : o.def);
    } else {
      if (v < o.min) v = o.min;
      if (v > o.max) v = o.max;
      v = o.min + (v - o.min) / o.step * o.step;
      if (o.attr == WATER_AMOUNT || o.attr == BYPASS) v /= 5;
      b = uint8_t(v > 255 ? 255 : v);
    }
    out[o.arg - 1] = b;
  }
}

}  // namespace jura_jprf
