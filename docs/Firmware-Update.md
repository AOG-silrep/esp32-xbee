# Firmware Update

_Firmware updates bring bug fixes and new features, and in most cases it is recommended that you update to the latest version. If the correct operation of the device is critical to you and you are happy with your current version, feel free to keep it. You can always flash an older version back with the [USB flash](#usb-flash) steps._

There are two ways to update:
* [Web update](#web-update): from the web interface, for devices already running this firmware (v0.6.1 or later).
* [USB flash](#usb-flash): for a first install, for devices running the original [nebkat/esp32-xbee](https://github.com/nebkat/esp32-xbee) firmware, or to recover a device that won't start.

## Web update
### Step 1: Download Latest Firmware
Go to the [latest firmware release](https://github.com/AOG-silrep/esp32-xbee/releases/latest) and download `esp32-xbee.bin`.

### Step 2: Upload
Open the device's web interface (http://ntrip.local/ or http://192.168.4.1/), choose the file in the Firmware update section and press Update. The device restarts with the new firmware when the upload completes, and the version is shown at the top of the page.

_The web update only replaces the firmware itself. There is no automatic rollback, so if the new firmware does not start, use the [USB flash](#usb-flash) steps to recover._

_v0.7.0 gave the firmware more space in flash (1856 KB instead of 1280 KB per firmware slot). A web update from v0.6.x installs the new firmware but keeps the old layout. To switch to the new layout, do a [USB flash](#usb-flash) once. Future firmware larger than 1280 KB will need it._

## USB flash
### Step 1: Download Latest Firmware
Go to the [latest firmware release](https://github.com/AOG-silrep/esp32-xbee/releases/latest) and download:
* `bootloader.bin`
* `partition-table.bin`
* `ota_data_initial.bin`
* `esp32-xbee.bin`

_`www.bin` is no longer needed: since v0.6.4 the web interface is built into `esp32-xbee.bin`._

### Step 2: Connect the HUZZAH32
Connect the HUZZAH32 to your PC with a USB cable. It switches into firmware download mode by itself when the flashing tool asks, so there are no buttons to press.

**_The F9P's UART1 output shares GPIO3 with the HUZZAH32's USB-serial chip, and can interfere with flashing. If the flashing tool can't connect, disconnect the F9P's TX line or power the F9P off while flashing._**

Proceed to the [Linux/macOS](#linuxmacos) or [Windows](#windows) section.

### Linux/macOS
#### Step 3: Install Flashing Tool
If you have not already done so, install Espressif's flashing tool, `esptool`:

`pipx install esptool` (or `pip install esptool`)

#### Step 4: Flash Firmware
Navigate to the directory where you downloaded the firmware files and run (adjust the port, e.g. `/dev/cu.usbserial-*` on macOS):

```
esptool --chip esp32 -p /dev/ttyUSB0 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 4MB --flash_freq 40m \
  0x1000  bootloader.bin \
  0x8000  partition-table.bin \
  0xf000  ota_data_initial.bin \
  0x20000 esp32-xbee.bin
```

This is the same command as [`linux_upload.sh`](../linux_upload.sh) in this repository, which you can copy next to the downloaded files and run as `./linux_upload.sh /dev/ttyUSB0`.

The HUZZAH32 restarts with the new firmware when flashing completes.

### Windows
The HUZZAH32's USB-serial chip needs the Silicon Labs [CP210x driver](https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers), which Windows usually installs by itself.

#### Step 3: Choose a Flashing Tool
Either:
* Open [ESP Tool](https://espressif.github.io/esptool-js/) in Chrome or Edge, Espressif's browser version of `esptool`, and press Connect. It also works on Linux and macOS.
* Or download "Flash Download Tools" from [Espressif](https://www.espressif.com/en/support/download/other-tools), extract and run it, and choose ChipType ESP32, WorkMode Develop, LoadMode UART.

#### Step 4: Load Firmware
Add the downloaded files with these offsets. _It is important that the offsets exactly match the files._

| File | Offset |
|---|---|
| `bootloader.bin` | `0x1000` |
| `partition-table.bin` | `0x8000` |
| `ota_data_initial.bin` | `0xf000` |
| `esp32-xbee.bin` | `0x20000` |

In Flash Download Tools, tick each file and also set SPI SPEED 40MHz, SPI MODE DIO and, if your version shows it, a 4MB flash size.

#### Step 5: Flash Firmware
Select the COM port and press Program (ESP Tool) or START (Flash Download Tools), then wait for flashing to complete. Press the HUZZAH32's RESET button if it does not restart by itself.

## Resetting the configuration
To erase the device's configuration over USB, for example when you can no longer reach its web interface, run:

`esptool --chip esp32 -p /dev/ttyUSB0 erase_region 0x9000 0x6000`

This erases only the configuration (the `nvs` partition) and leaves the firmware and the last core dump in place. On the next start the device uses its default settings, as on a new device. You can run it before or after the USB flash above. In ESP Tool, the same is available with Erase Flash, which erases everything, so flash the firmware again afterwards.

_Older instructions used a `wipe_config.bin` file for this, which is no longer needed._
