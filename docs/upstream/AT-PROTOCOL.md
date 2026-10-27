> Copied from [harryzz/jura-uart-esphome](https://github.com/harryzz/jura-uart-esphome) at commit `23c76ff` (MIT).
> It describes the upstream component (tested on an S8, with MQTT). What this repository changed for the Z10
> is in [../PROTOCOL-Z10.md](../PROTOCOL-Z10.md).

# Jura @-protocol (dongle emulation) — implementation reference

This documents everything the `jura_link` component does to **be the Smart
Connect dongle**: talk the encrypted `@`-protocol to the machine over the
service-port UART, read stats, brew, and publish to Home Assistant.

There are TWO independent protocols on the service port. Do not confuse them:

| | Clear-text TOP-tronics | Encrypted @-protocol (this doc) |
|---|---|---|
| Commands | `TY:`, `RT:`, `RE:`, `FA:`, `FN:` | `@TR:32`, `@TF:`, `@TG:43`, `@TP:`, `@T0..3` |
| On the wire | ASCII + 4:1 line encoding | `&`-framed, **enciphered** + 4:1 |
| Who uses it | juramote/Jutta-Proto, our early work | the Smart Connect dongle, and now us |
| Needs the cipher? | No | Yes (recovered — see below) |

Both share the **4:1 line encoding** (each byte -> 4 UART bytes carrying bits 2
and 5; base 0xDB). That is handled by `protocol.h` `encode()`/`feed()` and is
NOT encryption.

---

## 1. Frame format (`&` frames)

After the 4:1 line decoding, an @-protocol frame is:

```
'&'  <key>  <escaped( cipher(payload) )>  '\r' '\n'
 |    |      |                              |
 |    |      |                              frame terminator
 |    |      enciphered payload, then byte-escaped (see below)
 |    per-frame cipher key (CLEAR, 1 byte, arbitrary — we pick it)
 sync byte 0x26
```

- **payload** is the ASCII command, e.g. `@TR:32,00\r\n` (the inner CRLF is
  part of the payload and is enciphered).
- **cipher(payload)** = the nibble-permutation cipher (section 2), keyed by
  `<key>`.
- **escaping**: reserved bytes `{00, 0A, 0D, 1B, 26}` in the enciphered body
  are emitted as `1B, (b XOR 0x80)` so they cannot be mistaken for sync/CRLF.
  Unescape on receive.

Code: `jura_cipher.h` — `build_frame(plaintext, key)` produces the whole `&..CRLF`
frame; `decode_frame(frame, out)` reverses it.

---

## 2. The cipher (RECOVERED — not from firmware)

Same family as Jura's BT/WiFi ciphers, but with machine-specific S-boxes we
recovered by known-plaintext cryptanalysis of sniffed traffic (every request
starts `@` and ends CRLF -> 510 nibble equations -> solved for the two 16-entry
S-boxes).

```
N1 = {8,14,12,4,3,13,10,11,0,15,6,7,2,5,1,9}   // SBOX A
N2 = {4,11,13,10,0,7,15,5,9,8,3,1,14,2,12,6}   // SBOX B

shuffle(nib, pos, key):        // pos = nibble index (0,1,2,... over the payload)
  ta = key>>4 ; tm = key&15 ; i5 = pos>>4
  a     = N1[(nib + pos + ta) mod 16]
  inner = (i5 + a + tm - pos - ta) mod 16
  outer = (N2[inner] + ta + pos - tm - i5) mod 16
  return (N1[outer] - pos - ta) mod 16
```

**Self-inverse**: applying `shuffle` to a ciphertext nibble with the same
(pos,key) yields the plaintext nibble. So the same `crypt()` both encodes and
decodes. Verified over all 256 keys and against 633 captured frames.

Code: `jura_cipher.h` `_shuffle()` / `crypt()`.

---

## 3. The handshake (WHY the hardcoded values exist)

Before the machine answers any data command, the dongle must complete a fixed
4-step handshake. We captured it identically in **all 11** sessions, so the
values are constant on this machine and are hardcoded. **They may differ on
other models — see "Portability" below.**

```
  us  -> @T0                       we initiate
  M   -> @t0                       machine ack
  us  -> @T1
  M   -> @t1
  M   -> @T2:01C000                machine CHALLENGE ("klein Key" on COM8's whiteboard)
  us  -> @t2:818000                our fixed RESPONSE   <-- HARDCODED, see note
  M   -> @T3:3C16EF536M V02.01     machine sends its model string (3C16 prefix + "EF536M V02.01")
  us  -> @t3                       we ack; handshake complete
  --> data commands (@TR/@TF/@TG/...) now answered
```

### @t2 response: what we know (2026-09-18)

**We send the sniffed value `@t2:818000`** (what the real Smart Connect dongle sends on an S8 / EF536), and we keep it
**until we know what its bits mean**. Live tests on the EF536M (S8) showed the machine accepts several values:

| we send `@t2:` | source | handshake + stats/status reads |
|---|---|---|
| `818000` | sniffed from the real dongle, **in use** | works |
| `812000` | pattern of the EF532M snoop below | works |
| `810000` | all flag bits clear | works |

For reference, Jutta-Proto's EF532M key exchange
(https://github.com/Jutta-Proto/protocol-cpp/blob/main/protocol_snoops/keyexchange.md):
```
EF532M V02.03 : @T2:010001B228 -> real dongle @t2:8120000000   (5 bytes)
EF536M V02.01 : @T2:01C000     -> real dongle @t2:818000       (3 bytes)
```

What this tells us:
- The machine does **not** validate `@t2` as authentication. It is not a checksum or crypto response to the challenge.
- **byte 0 = challenge[0] | 0x80** (0x01 → 0x81) in both machines. It is probably a "reply from the dongle" marker.
- **length = challenge length.**
- The remaining bytes are **most likely a dongle capability / options field**, for example which features the dongle supports
  (product progress, milk, settings tier), timeouts or keep-alive behaviour, or a protocol level. The two real dongles differ
  (`18` vs `20` in byte 1), and so may the dongle generations (BlueFrog BT vs WiFi). **Unknown.** Reads work with any of
  the three values, but a different value might change behaviour we haven't exercised yet: brewing, progress
  reports, settings writes. That is why we keep the real dongle's `818000`.

`@T3` needs no computation either: the machine sends `<2-byte prefix><model>` (3BDE/3C16, **not** a plain CRC16 of the
model, tested), and the dongle just acks `@t3`.

**Possible universal response** (not adopted): `@t2 = (0x80|challenge[0])` + `(challenge_len-1)` zero bytes
(EF536M → `810000`, EF532M → `8100000000`). It would remove the per-machine value, but it has only been proven for reading
on the EF536M. Revisit once the flag bits are understood, for example by sniffing the real dongle on another model, or
by comparing brew/progress behaviour between `818000` and `810000`.

### Where in code
- `jura_link.h` `start_handshake()` sends `@T0` then `@T1`.
- `jura_link.cpp` `publish_at_()` auto-answers the machine's prompts:
  `@T2:*` -> `query_at("@t2:818000")`, `@T3:*` -> `query_at("@t3")`, sets `handshake_done_`, and
  selects the machine profile from the model string in `@T3` (see README §5).

---

## 4. Data commands we use (stats tier)

| Command | Reply | Meaning |
|---|---|---|
| `@TR:32,NN` | `@tr:32,NN,<4 words>` | product counters. **Indexed by XML product `Code`**: flat index = NN*4 + i, so word at index `code` is that product's count. index 0 = grand total. `FFFF` = no value. |
| `@TG:43` | `@tg:43<6 words>` (NO comma) | maintenance counters, in XML `MAINTENANCEPAGE` `TEXTITEM` order (S8: Cleaning, FilterChange, Decalc, CappuRinse, CoffeeRinse, CappuClean). 2 bytes each. |
| `@TG:C0` | `@tg:C0<bytes>` (NO comma) | maintenance % (1 byte each), XML order (S8: Cleaning, FilterChange, Decalc). Published as min(v,100), so `0xFF` → 100, as the BT bridge does. |
| `@TF:` | `@TF:<hex>` broadcast every ~2 s (also a reply) | **alert bitfield: bit i = byte i>>3, MSB first**, names from XML `<ALERT Bit Name Type>`. Example: `@TF:002400000000` = bit 10 `no beans` (display "fill bean container") + bit 13 `coffee ready`. |

Parsing: `jura_link.cpp` `parse_stats_()`. Storage: `counters_[256]` by code, `maint_[8]`, `maint_pct_[8]`, `status_[32]`,
each with a "seen" flag. Accessors: `product_count(code)`, `maint_count(i)`, `maint_percent(i)`. They return NAN only when
the value **hasn't been polled yet**; a reported `FFFF` counter is returned as 0.

**Doubles are FOLDED, not separate.** `@TR:32` reports the single product's
DISPLAY count with doubles already folded in (a 2x espresso increments the
Espresso count by 2). Product codes 0x11/0x12/0x13 ("2 Ristretti/Espressi/
Coffee") therefore always read `FFFF` here, even after you brew doubles.
The SEPARATE double counts exist only in the raw RT EEPROM (`0x00E4` = 2x
espresso, `0x00E5` = 2x coffee; screen = single + 2*double, change-verified) and
are read via the clear-text `RT:`/`RE:` path at per-model addresses, NOT via
`@TR:32`. We publish the 2x products anyway (as 0), like the BT bridge does. The real double counts would need RT
0x00E4/0x00E5 (S8-specific).

Cross-checked live: `@tr:32,00` = `1DD8 0007 00DA 19DD` = total 7640,
Ristretto 7, Espresso 218, Coffee 6621 — matched the machine screen and a
simultaneous BLE read.

---

## 4b. Brew (`@TP:`)

`@TP:<32 hex>` = the 16-byte recipe blob: byte 0 = product code, option `F<n>` at byte n−1, byte 8 = `01`, all else `00`.
Water and bypass are in 5 ml ticks, and the other options are sent as-is. The reply is a bare `@tp` when accepted (then `@TB` and `@TV:` progress, `@TV:3E<code>` when done),
or `@tp:00` when rejected. Source: makefu/jura-connect `docs/PROTOCOL.md` §5.9, verified by brewing through the WiFi dongle on an S8 EB (EF1091)
and an E6. The WiFi dongle relays the same `@T…` commands we send on the service port. Built by `jura_jprf::recipe_blob()`, sent by
`JuraLink::brew_()`.

**Verified on the EF536 (S8) over serial, 2026-09-18 07:32 UTC.** One MQTT `brew/set Coffee` gave:
```
TX  @TP:03000514000001000100000000000000      Coffee, strength 5, 100 ml (0x14 ticks), Normal
M   @TB                                        brew started (no bare @tp ack on serial!)
M   @TV:390305050014FFFFFFFF11FFFF110000      (x2)
M   @TV:3C0305050014FFFFFFFF11FFFF110000 ... @TV:3C0305051414FFFFFFFF11FFFF116400
        byte 4 = water ticks done 00->14, byte 5 = target 14, second-to-last byte = percent 00->64
M   @TV:3E03                                   product 03 finished
```
Counter Coffee 6625 → 6626, total 7644 → 7645. On serial the reply to `@TP` is `@TB`/`@TV`, not the bare `@tp` the WiFi dongle
returns. `@tp:00` (rejected) has not been seen on serial yet.

## 4c. Progress (`@TV:`) and panel lock (`@TS:`)

- `@TV:<state><code><window>`: pushed while the machine works. State `39` = grinding, `3C` = water, `3E` = enjoy (done).
  Byte 14 of a 16-byte frame = whole-product percent. States `FE`/`FF` are other channels (e.g. `@TV:FF5F…` accessory menu).
  Parsed by `parse_progress_()` → `jura/<id>/progress/{percent,state,product}`.
- `@TS:01` locks the front panel and `@TS:00` unlocks it; the reply is `@ts`. Visible as `@TF` bit 39 (`LockedKeys`): `@TF:000400000000` →
  `@TF:000400000100` in a sniff of the real dongle serving the J.O.E. app, which is how the app wraps its reads.
  jura-connect `client.py` `lock_screen()` / `unlock_screen()`. Exposed as the `switch/lock` "Panel Lock" (`allow_lock`).
- `@TB` / `@TS` (no colon) are markers pushed by the machine, not replies.

## 5. Polling & queue

`poll_stats()` (jura_link.h) runs the handshake if needed, then enqueues
`@TR:32,00..0B` (12 banks cover all product codes) + `@TG:43` + `@TG:C0`.
Driven by `update_interval` when `auto_poll: true`.

**Engine queue is `QUEUE_SIZE` (protocol.h, currently 20).** A full poll is ~14
commands; the queue MUST exceed that or the last commands (the `@TG`s) are
dropped — that was a real bug (maintenance counters silently missing).

---

## 6. MQTT (same layout as the jura-mqtt BT bridge)

Full description in `README.md` §6. In short:
- State: `jura/<id>/{availability,connection,rssi,status,active_alerts,total_products}`, `alert/<slug>`,
  `product/<slug>`, `maintenance/<slug>_{counter,percent}`. All retained, published on change, with a full resend every 5 min and on reconnect.
- Discovery: `homeassistant/<platform>/<id>/<component>/config`. The ids and names match
  the jura-mqtt BLE→MQTT bridge.
- `<id>`: the yaml `machine_id`/`machine_model` override, else the slug of the model family stored in the profile DB for the EF code
  (EF536 → `s8`), else the EF code. A yaml override such as `machine_model: "S8 (EA)"` → `s8_ea` pins the ids, for example to keep a BLE bridge's entities.
- Rate limit: 4 discovery / 10 state messages per second. Publishing ~48 at once drops the tail, a real bug we hit.

Broker must be reachable by IP OR a DNS the ESP can resolve — the `.internal`
hostname failed (ESP DNS didn't know it); an IP address is the safe choice.

---

## 7. Portability (universal dongle)

- **Stats and alerts are model-independent**: `@TR:32` is Code-indexed, `@TG` follows the TEXTITEM order, and `@TF` uses the
  ALERT bit numbers, all defined by the machine XML. 75 machines are packed into the JPRF database
  (`../../tools/pack_profiles.py` → `jura_profiles_blob.h`), selected at runtime by the EF code in `@T3`.
- **The handshake `@t2` value** is the sniffed S8 value (§3). The machine accepted every value we tried, so it is unlikely to block
  other models, but its meaning (capabilities/options) is unknown.
- **Cipher S-boxes**: recovered from an S8 (EF536). Believed universal (fixed
  obfuscation, same family as BT/WiFi) but unconfirmed on another machine.

---

## 8. Code & file map

| File | Role |
|---|---|
| `jura_cipher.h` | the recovered cipher: `crypt`, `escape`/`unescape`, `build_frame`, `decode_frame` |
| `jura_jprf.h` / `jura_profiles_blob.h` | packed machine-profile DB (75 machines: products, maintenance labels, alerts, model family) and its reader |
| `protocol.h` | transport engine: 4:1 encoding, framing, clear-text queries, and `AT_RAW` (send a built `&` frame). Queue `QUEUE_SIZE` = 20 |
| `jura_link.h` | adapter API: `query_at`, `start_handshake`, `poll_stats`, stats accessors, MQTT entity table |
| `jura_link.cpp` | event handling, `publish_at_` (decode + auto-handshake), `parse_stats_` (+`@TF`), `mqtt_publish_` |
| `__init__.py` | ESPHome config schema (see README §3) |
| `../../tools/pack_profiles.py` | XMLs + `JOE_MACHINES.TXT` → JPRF DB |

### Example configs
| YAML | Purpose |
|---|---|
| `examples/jura-dongle.yaml` | the dongle: @-protocol + auto-poll + MQTT (+ optional brew / panel lock) |
| `examples/jura-dongle-debug.yaml` | diagnostic sensors + handshake/`@` command buttons, no MQTT |

## 9. Known issues / TODO
- Brew (`@TP:`) is verified on the S8 (§4b). The log reports `BREW started` on `@TB` (the serial link sends no bare `@tp`). The settings tier (`@TM:`) is not implemented.
- Meaning of the `@t2` option bits is unknown (§3).
- `poll_stats()` queues the handshake and data together; `handshake_done_` never resets on timeouts; a failed boot handshake is not
  retried.
- 2x products (codes 0x11-0x13) are folded into singles by @TR:32 (always FFFF, published as 0); the real double counts are
  RT 0x00E4/0x00E5 (clear-text, per-model).
