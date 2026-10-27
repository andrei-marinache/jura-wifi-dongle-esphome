# Hardware

How the Jura WiFi Connect dongle is wired, as far as this firmware depends on it.

- **Sources:** the signal paths come from the KiCad schematic in
  [Sebbeben/Jura-Smart-Connect-Wifi-pcb-3d-model](https://github.com/Sebbeben/Jura-Smart-Connect-Wifi-pcb-3d-model).
  The working firmware confirms them.
- **Measured:** the machine-side voltages were measured on a Z10.

## The dongle

| | |
|---|---|
| Board | TOP-tronic TT237XX-PRD-10 V02 |
| Module | ESP32-WROOM-32D: classic ESP32, 4 MB flash. The unit used here is silicon rev 1, with no flash encryption and no secure boot. |
| Other microcontrollers | none: the Jura firmware runs on the ESP32 itself |
| Supply | 5 V from the machine, regulated to 3.3 V on the board |

**ESP32 pins:**

| ESP32 pin | Used for | Circuit |
|---|---|---|
| GPIO17 | UART TX → machine RX | MOSFET level shifter (gate on 3.3 V, pull-up R12 to 3.3 V), then R9 / R11 to connector pin J1.3. Not inverted. |
| GPIO16 | UART RX ← machine TX | from connector pin J1.5 through the divider R7 / R6 and R5. Not inverted. |
| GPIO2 | status LED | LED to GND through R1, active high |
| EN | reset | pull-up R15 to 3.3 V and C7 to GND: a clean reset at every power-up. Leave it unconnected when flashing. |
| GPIO0 | boot mode | low at reset = serial download mode. The schematic also shows a button on it. |
| GPIO1 / GPIO3 | UART0: programming and log | test pads TXD0 / RXD0 on the back |
| GPIO34 | free | module pin 6, input only, ADC1 (works with WiFi on). Used for the optional supply divider, see the [README](../README.md#4-optional-power-cut-detection). |

GPIO16 / GPIO17 are the default UART pins of Espressif's ESP-AT firmware, which the original Jura
firmware is built on.

**Test pads on the back:** GND, TXD0 (module pin 35), RXD0 (module pin 34), GPIO0 (module pin 25),
EN (module pin 3), 3.3 V. See the photo in the [README](../README.md#pinout).

**Connector J1:** 5 pins.

| Pin | Signal |
|---|---|
| J1.3 | machine RX |
| J1.5 | machine TX |
| J1.2 | unused on the dongle: tied to 3.3 V through R10 (0 Ω) and R8 (100 Ω) |
| the other two | 5 V and GND |

## The machine's service socket

You only need this if you build your own adapter instead of reusing the dongle. The socket has 7
positions. Measured on a Z10 against position 3, with nothing plugged in:

| Position | Signal |
|---|---|
| 2 | machine TX, idles at about 5 V |
| 3 | GND |
| 4 | machine RX, pulled up to about 5 V inside the machine |
| 6 | 5 V supply |
| 1, 5, 7 | not used |

The position numbering follows [Jutta-Proto](https://github.com/Jutta-Proto)'s serial-snooper
pinout.

**Before you wire anything:**
- Check that position 3 → position 6 reads +5 V. This also confirms you counted from the right end.
- Both data lines are 5 V, and an ESP32 is not 5 V tolerant, so a bare ESP32 board needs a level
  shifter on both lines. The dongle already has one.

The machine cuts the 5 V about 5 minutes after it is switched off. The current the 5 V line can
supply is not documented.
