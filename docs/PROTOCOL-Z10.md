# The dongle protocol on a Jura Z10

What this firmware relies on, written down from porting it to a Jura Z10 (`EF545M V02.02`).

- **Upstream:** the frame format and the cipher are in
  [upstream/AT-PROTOCOL.md](upstream/AT-PROTOCOL.md) and not repeated here.
- **Credit:** several facts were first documented by
  [makefu/jura-connect](https://github.com/makefu/jura-connect): the `@TP:` recipe layout, the recipe
  verified on a Z10, the settings checksum, and the preselection encoding taken from the J.O.E. app.
- **Measured here:** the rest comes from logs of this firmware talking to a real Z10.

Notation: requests from the dongle are upper case (`@TR:32,00`), the machine's replies are the same
verb in lower case (`@tr:32,00,…`). Values are hex unless a unit is given.

## Contents

1. [Physical layer](#1-physical-layer)
2. [Session](#2-session)
3. [What the machine pushes](#3-what-the-machine-pushes)
4. [Requests and replies](#4-requests-and-replies)
5. [Counters and maintenance](#5-counters-and-maintenance)
6. [Machine settings](#6-machine-settings)
7. [Processes](#7-processes)
8. [Brewing (`@TP:`)](#8-brewing-tp)
9. [Power](#9-power)
10. [The original dongle firmware](#10-the-original-dongle-firmware)

## 1. Physical layer

- **UART:** 9600 baud, 8N1, 5 V logic on the machine side. The dongle shifts it to 3.3 V; see
  [HARDWARE.md](HARDWARE.md).
- **Line encoding:** every byte goes out as 4 UART bytes, each carrying 2 of its bits. This is
  encoding, not encryption.
- **Frames:** on top of the line encoding, every message is a frame: `&` + key + enciphered payload +
  CR LF.
- **Frame end:** a frame counts as complete after 100 ms without new bytes.

## 2. Session

The dongle opens a session with a key exchange:

| Step | Direction | Message on the Z10 |
|---|---|---|
| 1 | dongle → machine | `@T1` (this firmware also sends `@T0` first) |
| 2 | machine → dongle | `@T2:01C041B628`: a 5-byte challenge |
| 3 | dongle → machine | `@t2:818000` |
| 4 | machine → dongle | `@T3:3C80EF545M V02.02`: machine code and firmware version |
| 5 | dongle → machine | `@t3` |
| 6 | dongle → machine | **`@TR:37`**, answered with `@tr:37,4947000244894316` |

**Notes on the steps:**
- **Step 3:** the Z10 accepted every answer tried: `818000`, `8100000000`, `8120000000`, `8180000000`.
- **Step 4:** the code (`EF545`) selects the machine profile. Its firmware version is the `V02.02` part.
- **Step 6 is what makes the Z10 different from the S8.** Without `@TR:37`, the machine pushes one
  status frame, then sends `@t0` about 2 s later and stops answering anything. With it, the session
  stays open indefinitely. The meaning of the `@tr:37` payload is unknown; the original dongle
  firmware sends `@TR:37` at the same point.

**When a session is already open:**
- **Key still valid:** `@T0` / `@T1` are answered with `@t0` / `@t1` and no new `@T2` / `@T3`.
  The firmware reuses the session if `@t1` arrives without `@T2` within 4 s.
- **Machine ends the session:** an unsolicited `@t0` while a session is open means exactly that.
  The firmware runs the handshake again.
- **Retries:** a handshake that does not finish is retried after 10 s.

## 3. What the machine pushes

The machine sends these on its own while a session is open:

| Push | When | Content |
|---|---|---|
| `@TF:<bits>` | about every 2 s | status / alert bits, most significant bit first. Bit numbers and names are the `<ALERT>` list of the XML profile. |
| `@TV:<bytes>` | while something runs | progress of a product or a process: byte 0 = state, byte 1 = product code, byte 14 = percent |
| `@TB` | at a brew start | marks that a product has started |

Two status bits matter for the firmware:
- **Bit 47** ("SwitchOff Delay"): set from the moment the machine is switched off until it cuts the
  dongle's power (see [section 9](#9-power)).
- **Alert bits marked blocking** in the profile: the machine refuses to brew while any of them is set.

## 4. Requests and replies

- **One request at a time.** The next one goes out 10 ms after a reply. Without a reply, it goes out
  after a 3 s timeout.
- **A reply must match its request:** the same verb in lower case, plus the echoed argument for
  `TR`, `TG` and `TM` (`@TR:32,0A` → `@tr:32,0A,…`; `@TM:13` → `@tm:13,…`). `@t2` and `@t3`
  expect no reply.
- **Collisions with pushes:** a request that reaches the machine just before an `@TF` push is
  dropped. The push arrives instead of the reply. The firmware sends the request again right away,
  up to 3 times. Without this, a counter bank was regularly skipped until the next poll.

## 5. Counters and maintenance

**Product counters: `@TR:32,NN`.**
- The reply is `@tr:32,NN,<16 hex digits>`: four big-endian 16-bit counters.
- Counter `i` of bank `NN` belongs to product code `NN × 4 + i`. Code `00` is the grand total.
- `FFFF` means "no such product".
- Example: `@tr:32,00,095FFFFF005E0007` → total 2399, code 01 unused, Espresso (02) 94, Coffee (03) 7.
- The Z10's products go up to code `39`, so banks `00`–`0E` cover everything. The firmware only
  polls banks that contain a product of the machine; on a Z10 a full poll takes about 7 s.

**Maintenance counters: `@TG:43`.**
- The reply is `@tg:43<16-bit counters>`, in the order of the profile's maintenance counter list.
- Example: `@tg:4300170004000608E4069E0221`.

**Maintenance percentages: `@TG:C0`.**
- The reply is `@tg:C0<one byte per item>`: a percentage each for cleaning, filter change and
  descaling.
- `FF` means not applicable. Example: `@tg:C014FF0A` = cleaning 20 %, filter not fitted, descaling 10 %.

## 6. Machine settings

**Read:** `@TM:<arg>` → `@tm:<arg>,<value><checksum>`.

**Write:** wrap the write in a panel lock. Without the lock, the machine acknowledges the write but
keeps the old value.

```
@TS:01
@TM:<arg>,<value><checksum>
@TS:00
```

**Checksum:** one byte, `(−1 − sum of the ASCII codes of "<arg>,<value>") & FF`, written as two hex
digits. This is the J.O.E. app's formula.

**Z10 arguments:** they are listed under `<MACHINESETTINGS>` in the profile.

| Arg | Setting | Values |
|---|---|---|
| `02` | water hardness | one byte, °dH (1–30) |
| `13` | auto off | `0F` 15 min, `1E` 30 min, `213C` 1 h, `2178` 2 h, `21B4` 3 h, `21F0` 4 h, `22012C` 5 h, `220168` 6 h, `2201A4` 7 h, `2201E0` 8 h, `22021C` 9 h |
| `08` | units | `220000` ml, `220100` oz |
| `09` | language | see below |
| `0A` | display brightness | one byte, × 10 % |

**Language codes.** The profile lists Italian, Spanish, Portuguese and Russian on some of these codes.
This Z10's regional variant has other languages there, so the codes were measured on the machine
instead:

| Code | Language | Code | Language |
|---|---|---|---|
| `01` | German | `07` | Bulgarian |
| `02` | English | `08` | Finnish |
| `03` | French | `09` | Swedish |
| `04` | Croatian | `0A` | Polish |
| `05` | Dutch | `0B` | Estonian |
| `06` | Romanian | | |

## 7. Processes

`@TG:<code>` starts a process, as listed under `<PROCESSES>` in the profile. The machine then guides
it on its own display.

| Code | Process |
|---|---|
| `21` | cappu clean |
| `22` | coffee rinse |
| `23` | cappu rinse |
| `24` | cleaning |
| `25` | descaling |
| `26` | filter change |
| `28` | empty system |
| `04` | OK on a prompt / power off |
| `03` | cancel (e.g. a running power-off) |
| `10` | confirm the step the machine waits for |
| `FF` | cancel the running step |
| `30`, `31` | install / disconnect an accessory (not exposed) |
| `32` | **factory reset, no confirmation** (not exposed) |

## 8. Brewing (`@TP:`)

**Request:** `@TP:<recipe>`, 16 bytes as 32 hex digits.

| Byte | Content |
|---|---|
| 0 | product code |
| `n − 1` | parameter `Argument="F<n>"` from the profile |
| 8 | always `01` |
| any other | `00` = not set, the machine uses its own setting for that product |

**Reply:** `@tp` = accepted; the machine then sends `@TB` and `@TV` progress. `@tp:00` = refused.

**Values:**
- Pick lists (strength, temperature) are sent as their `ITEM Value`.
- Water and bypass are sent in units of 5 ml.
- Milk and milk foam are sent in seconds, as they are.

**Verified on a Z10 by brewing:**
- by jura-connect: the Milkcoffee recipe `05000812030202000100000000000000` (strength 8, 90 ml,
  milk 3 s, foam 2 s, temperature high), and with it F3 strength, F4 water, F5 milk, F6 milk foam
  and F7 temperature;
- here: `F2_H` grind as byte 1 = the item value (`10` … `50` for levels 1 … 5). Level 1 ground
  coarse, so 5 is the fine end. Left at `00`, the machine keeps its own grind for the product.
- here: all four preselections: double (byte 0 = the double product's code), cold brew
  (byte 6 = `80`, on a Cappuccino), extra shot (byte 7 = `02`) and ground coffee (byte 2 = `00`).
- here: `F14_1` foam temperature as the high nibble of byte 13. Level 8 instead of the factory 5
  gave noticeably hotter foam.

**Not verified yet:**

*Sub-indexed arguments.* The packed profile and jura-connect skip these, and the card sends them
only when changed. Grind and foam temperature are confirmed (above). One is still a guess:
- `F14_2` milk temperature: the low nibble of byte 13, next to the foam temperature.

*Other unconfirmed parameters:*
- `F8` stroke (Pot): sent only when changed.
- `F10` bypass and `F11` milk break: always sent at their factory value, as the J.O.E. app does,
  but never confirmed on a Z10.

*Preselections.* The Z10's profile has no `IntakeF18` capability and no `<MULTIPLE_PRESELECTS>`, so
the J.O.E. app would use its old encoding, one preselection at a time. All four are confirmed
(above):

| Preselection | Encoding |
|---|---|
| double | byte 0 = the code in `double="…"` (Espresso → `31`, Coffee → `36`, Cafe Barista → `38`, Barista Lungo → `39`) |
| cold brew | byte 6 = `80` |
| extra shot | byte 7 = `02` |
| ground coffee | byte 2 = `00` |
| fake sweet foam | no payload; not offered |

## 9. Power

- **The dongle has no supply of its own:** it runs on the machine's 5 V.
- **After switch-off:** status bit 47 comes on and the machine cuts the 5 V **5 min 3 s** later
  (measured).
  - After 4 minutes of bit 47, the firmware closes the API connection on purpose, so Home Assistant
    keeps the entities available.
  - If the machine is switched on again in the meantime, bit 47 clears and the firmware reboots.
- **Mains loss while the machine is on** gives no warning; see the optional divider in the
  [README](../README.md#4-optional-power-cut-detection).

## 10. The original dongle firmware

These notes come from a full flash dump of one dongle. They help when restoring it, or when comparing
with another unit.

**Firmware:** TT237W V06.11 on ESP-IDF v4.4.1. The loader in `factory` is V02.01. An older public
dump has V05.26 with loader V01.01 on ESP-IDF v3.3.2 and the same partition table.

**Partition table:**

| Partition | Offset | Size | Content |
|---|---|---|---|
| `nvs` | `0x9000` | `0x4000` | WiFi settings, app pairings |
| `otadata` | `0xD000` | `0x2000` | active OTA slot |
| `phy_init` | `0xF000` | `0x1000` | RF calibration of that chip |
| `factory` | `0x10000` | `0x20000` | loader |
| `ota_0` | `0x30000` | `0x180000` | the Jura application |
| `ota_1` | `0x1B0000` | `0x180000` | second OTA slot |
| `storage` | `0x330000` | `0xD0000` | the dongle's own data |

**Its session with the machine:**
1. It sends `TY:`.
2. It sends `@T1`, without an `@T0`.
3. It answers the challenge with `@t2:818851` + 2 computed bytes + `0000`.
4. It sends `@t3`, then `@TR:37`.
5. On an `@t0` from the machine, it restarts the session.
