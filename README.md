# ESP32 NTRIP
Firmware for an ESP32 that sits between a u-blox ZED-F9P GNSS receiver and WiFi, made with [ESP-IDF](https://github.com/espressif/esp-idf). Its main function is to forward the UART of the ESP32 to a variety of protocols over WiFi, most importantly bringing NTRIP corrections to the receiver and sending its position back to the caster.

This is a fork of [nebkat/esp32-xbee](https://github.com/nebkat/esp32-xbee), the original firmware for the ArduSimple [WiFi NTRIP Master](https://www.ardusimple.com/product/wifi-ntrip-master/) XBee board ([3D model](esp32-xbee-board.step)). This fork targets an Adafruit HUZZAH32 (ESP32 Feather) wired to a ZED-F9P, see [Pinout](docs/Pinout.md).

## Features
- WiFi Station, reachable at `http://ntrip.local/` through mDNS
- WiFi Hotspot, which switches itself off once the station is connected and back on when it drops
- Web Interface
- Firmware update from the web interface (OTA)
- UART configuration, with the receiver's fix, satellites, DOP and accuracy shown live
- F9P receiver configuration over the UART: check and write the Dual GPS (moving base) or RS-232 configuration
- NTRIP Client/Server/Caster, with the client's mountpoint picked from the caster's sourcetable
- Distance to the NTRIP base station and correction age
- TCP/UDP Socket Server/Client
- CPU load, heap and uptime, log viewer, core dump download
- Reset to defaults from the web interface

## Help
If you're setting up a new device, check out the [Getting Started](docs/Getting-Started.md) page.

To install the latest firmware check out the [Firmware Update](docs/Firmware-Update.md) page.

If you are experiencing problems with the firmware, please see the [Reporting Crashes](docs/Reporting-Crashes.md) page.

For wiring, see the [Pinout](docs/Pinout.md) page.

## Building
The firmware builds with ESP-IDF v5.4.4, the same version the GitHub Actions build uses.

```sh
git clone --recursive https://github.com/AOG-silrep/esp32-xbee.git
cd esp32-xbee
. $IDF_PATH/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

If you cloned without `--recursive`, run `git submodule update --init` to fetch `components/button`.

The firmware version comes from `git describe`. After creating or moving a tag, run `idf.py reconfigure build`, as a plain `idf.py build` keeps the old version string.
