#pragma once
// JURA service-port @-protocol cipher (dongle<->machine), recovered 2026-09-17.
// Same nibble-permutation family as the BT/WiFi ciphers; self-inverse.
// Portable: no ESPHome/Arduino dependency.
#include <cstdint>
#include <string>

namespace jura_protocol {
namespace atproto {

// Machine-recovered S-boxes (see research/sniff/CIPHER-BROKEN.md).
static const uint8_t N1[16] = {8,14,12,4,3,13,10,11,0,15,6,7,2,5,1,9};
static const uint8_t N2[16] = {4,11,13,10,0,7,15,5,9,8,3,1,14,2,12,6};

inline uint8_t shuffle(uint8_t nib, int pos, uint8_t key) {
  const int ta = key >> 4, tm = key & 15, i5 = (pos >> 4) & 15;
  const int a = N1[((nib + pos + ta) % 16 + 16) % 16];
  const int inner = ((i5 + a + tm - pos - ta) % 16 + 16) % 16;
  const int outer = ((N2[inner] + ta + pos - tm - i5) % 16 + 16) % 16;
  return static_cast<uint8_t>(((N1[outer] - pos - ta) % 16 + 16) % 16);
}

// Self-inverse per-nibble transform (encode == decode).
inline std::string crypt(const std::string &data, uint8_t key) {
  std::string out;
  out.reserve(data.size());
  for (size_t i = 0; i < data.size(); ++i) {
    const uint8_t b = static_cast<uint8_t>(data[i]);
    out.push_back(static_cast<char>((shuffle(b >> 4, 2*i, key) << 4) |
                                     shuffle(b & 15, 2*i+1, key)));
  }
  return out;
}

inline bool is_reserved(uint8_t b) {
  return b==0x00 || b==0x0A || b==0x0D || b==0x1B || b==0x26;
}
inline std::string escape(const std::string &b) {
  std::string o;
  for (char c : b) {
    const uint8_t v = static_cast<uint8_t>(c);
    if (is_reserved(v)) { o.push_back(0x1B); o.push_back(static_cast<char>(v ^ 0x80)); }
    else o.push_back(static_cast<char>(v));
  }
  return o;
}
inline std::string unescape(const std::string &b) {
  std::string o;
  for (size_t i = 0; i < b.size(); ++i) {
    if (static_cast<uint8_t>(b[i]) == 0x1B && i+1 < b.size()) {
      o.push_back(static_cast<char>(static_cast<uint8_t>(b[++i]) ^ 0x80));
    } else o.push_back(b[i]);
  }
  return o;
}

// Build a wire frame from a plaintext @-command (e.g. "@TR:32,00\r\n").
// Result is the decoded-byte frame '&' + escape(key + cipher(body)) + CRLF,
// ready to be 4:1 line-encoded and transmitted.
inline std::string build_frame(const std::string &plaintext, uint8_t key) {
  std::string body;
  body.push_back(static_cast<char>(key));
  body += crypt(plaintext, key);
  return std::string("&") + escape(body) + "\r\n";
}

// Decode a received '&'...CRLF frame (decoded bytes) to its plaintext command.
// Returns false if the frame is not a well-formed '&' frame.
inline bool decode_frame(const std::string &frame_in, std::string &plaintext) {
  std::string frame = frame_in;
  // Accept frames with or without the trailing CRLF (the transport engine
  // strips it before handing us the '&' payload).
  while (!frame.empty() && (frame.back()=='\r' || frame.back()=='\n')) frame.pop_back();
  if (frame.size() < 2 || frame[0] != '&') return false;
  const std::string body = unescape(frame.substr(1));
  if (body.empty()) return false;
  plaintext = crypt(body.substr(1), static_cast<uint8_t>(body[0]));
  return true;
}

}  // namespace atproto
}  // namespace jura_protocol
