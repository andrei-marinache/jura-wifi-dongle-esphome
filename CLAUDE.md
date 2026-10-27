# CLAUDE.md

ESPHome firmware for the original Jura WiFi Connect dongle (ESP32-WROOM-32D), tested on a Jura Z10
(`EF545`). Start with [README.md](README.md); the protocol is in
[docs/PROTOCOL-Z10.md](docs/PROTOCOL-Z10.md), the circuit in [docs/HARDWARE.md](docs/HARDWARE.md).

## Layout

| Path | Notes |
|---|---|
| `components/jura_link/` | the component; `protocol.h` is the engine (framing, matching, retries), `jura_link.cpp/.h` the ESPHome side |
| `components/jura_link/jura_profiles_blob.h` | generated, never edit by hand |
| `jura-z10.yaml` | example config, Z10-specific (alert bits, product codes, setting values) |
| `card/jura-z10-card.js` | dashboard card; builds the `@TP:` recipe in the browser |
| `profiles/` | J.O.E. XML profiles from AlexxIT/Jura, kept unmodified |
| `tools/pack_profiles.py` | `profiles/` → `jura_profiles_blob.h` |
| `docs/upstream/` | harryzz's docs, unmodified apart from the provenance note on top |

## Checks

Run all of them from the repository root before committing:

```
g++ -std=c++17 -Wall -Wextra -Werror -Icomponents tests/reply_match_test.cpp -o /tmp/reply-match && /tmp/reply-match
g++ -std=c++17 -Wall -Wextra tests/jura_protocol_test.cpp -o /tmp/protocol && /tmp/protocol
g++ -std=c++17 -Wall -Wextra tests/jura_jprf_test.cpp -o /tmp/jprf && /tmp/jprf
node tests/recipe_test.js
```

- **After editing the YAML:** `esphome config jura-z10.yaml`. It needs a `secrets.yaml` with
  `api_key`, `wifi_ssid`, `wifi_password`.
- **After changing `profiles/` or the packer:** regenerate the blob with the command in the README.
  On unchanged profiles the output must be byte-identical to the committed file.

## Rules

- **English only:** code, comments, logs, commits and docs. Comments explain *why*, in one line,
  matching the existing density.
- **Nothing personal in the repo:**
  - no IP addresses, hostnames, network names or user names;
  - never a firmware dump: `*.bin` is ignored because a dump holds the owner's WiFi password.
- **No AI attribution** in commits or PRs.
- **Unlicensed sources:** makefu/jura-connect and Sebbeben's schematic have no license. Use their
  facts in your own words, with credit; never copy their text or files. `profiles/`, `docs/upstream/`,
  `tools/` and the upstream tests are MIT.
- **The firmware drives a real machine:**
  - Never send `@TP:` (brew) or `@TG:` (process) from a test, script or tool run without the owner's
    go-ahead.
  - `@TG:32` is a factory reset without confirmation, and `@TG:30` / `@TG:31` touch accessories:
    never expose or send them.
  - Settings are written only through `write_setting()`: panel lock, checksum, read-back.

## Invariants that are easy to break

- **Session:** the Z10 drops it about 2 s after the handshake unless `@TR:37` follows `@t3`.
- **Replies:**
  - They must match their request: same verb in lower case, plus the echoed argument.
  - A request displaced by an `@TF` / `@TV` push is resent, at most 3 times.
  - `REPLY_GAP_MS` is 10 ms.
  - `tests/reply_match_test.cpp` and `tests/jura_protocol_test.cpp` cover all of this.
- **Switch-off:**
  - Status bit 47 means "switch-off delay"; the machine cuts the dongle's 5 V about 5 min later.
  - The YAML disconnects from the API on purpose after 240 s of bit 47.
  - The empty `deep_sleep:` makes Home Assistant treat that disconnect as expected and keep the
    entities available. Removing either one makes every entity go unavailable each evening.
- **Power-cut detection** arms only after 10 s of a ≥ 4.8 V reading, and needs the GPIO34 divider.
- **Card recipes:**
  - For an untouched product, the card's recipe must equal the firmware's own `recipe_blob()`.
  - Parameters outside `VERIFIED` / `SEND_ALWAYS` go out only when the user changes them.
  - `tests/recipe_test.js` pins the Milkcoffee recipe that was brewed on a Z10.
- **When a *test* parameter is confirmed by a real brew,** update all of these together:
  - `VERIFIED` in the card;
  - the table in the README;
  - section 8 of `docs/PROTOCOL-Z10.md`.

## Releases

- **Installs follow `main`:** use `@main` with `refresh: always`. The default `refresh` (1 day) keeps
  building an older clone after a push. Anything pushed to `main` goes into the next build, so push
  only what passed the checks.
- The example YAML uses `@main`.
- **To freeze a build,** pin the full commit hash instead (a short hash does not fetch).
- Card updates reach browsers only when the dashboard resource URL changes: bump `?v=`.
