# Pinout

This page shows how the firmware uses the pins of an Adafruit HUZZAH32 (ESP32 Feather). GPIO numbers are as numbered by the ESP32, and the labels are the ones printed on the HUZZAH32.

| Function | GPIO | HUZZAH32 label | Notes |
|---|---|---|---|
| UART TX → F9P RX1 | 1 | none | UART0, also wired to the USB-serial chip |
| UART RX ← F9P TX1 | 3 | none | UART0, also wired to the USB-serial chip |
| UART RTS | 14 | 14 | Only with RTS flow control enabled (off by default) |
| UART CTS | 33 | 33 | Only with CTS flow control enabled (off by default) |
| RGB LED red | 21 | 21 | Active low |
| RGB LED green | 22 | SCL | Active low |
| RGB LED blue | 23 | SDA | Active low |
| WiFi signal LED | 18 | MO | Optional, active high, brighter with a stronger WiFi signal |
| Heartbeat LED | 13 | 13 | Onboard red LED |
| Reserved | 25, 27 | A1, 27 | Driven as LED outputs (see below) |
| Configuration reset | 0 | none | Not broken out on the HUZZAH32 |

## UART
`U0TX` (GPIO1) and `U0RX` (GPIO3) are the main communication port, connected to the F9P's UART1. These are the defaults in the UART section of the web interface, at 115200 baud, 8N1, no flow control.

On the HUZZAH32 these pins are also the USB-serial chip's lines, so a serial terminal on the USB port sees what the ESP32 sends to the F9P, and the USB-serial chip and the F9P both drive GPIO3. Keep this in mind when [flashing over USB](Firmware-Update.md#step-2-connect-the-huzzah32).

The pins, UART number and flow control can be changed in the UART section.

## RGB LED
An external common-anode RGB LED, with each color's cathode connected through a resistor to its GPIO, so a color lights when its pin is low. Its colors are set per feature in the web interface, see [LEDs](Getting-Started.md#leds).

## WiFi signal LED
An optional LED on GPIO18, active high. It shines brighter the stronger the WiFi station's signal is, and is off while WiFi is not connected.

## Heartbeat LED
The HUZZAH32's onboard red LED on GPIO13. It blinks at 1 Hz while the firmware is running, and at 5 Hz for 10 seconds after an unexpected restart.

## Reserved
GPIO25 (A1) and GPIO27 are configured as LED outputs for the original board's `ASSOC` and `SLEEP` LEDs. The firmware does not use these LEDs, but the pins are still driven, so don't connect anything else to them.

## Configuration reset
Holding GPIO0 low for 5 seconds resets the configuration to defaults. GPIO0 is not broken out on the HUZZAH32, so use the other [Full Reset](Getting-Started.md#full-reset) methods instead.

## Original hardware
This firmware was originally written for the ArduSimple WiFi NTRIP Master, an ESP32 board in the XBee form factor ([3D model](../esp32-xbee-board.step)). Its pinout is documented in the [original project's wiki](https://github.com/nebkat/esp32-xbee/wiki/Pinout).
