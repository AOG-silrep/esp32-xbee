#!/usr/bin/env bash
set -e

PORT="${1:-/dev/ttyUSB0}"

esptool --chip esp32 -p "$PORT" -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 4MB --flash_freq 40m \
  0x1000  build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0xf000  build/ota_data_initial.bin \
  0x20000 build/esp32-xbee.bin \
  0x2a0000 build/www.bin
