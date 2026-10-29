# Reporting Crashes

Crashes are indicated after the ESP32 restarts by a fast blinking red RGB LED and the HUZZAH32's onboard red LED blinking at 5 Hz, both for 10 seconds.

If you are experiencing crashes, you can attempt to resolve them with a [Full Reset](Getting-Started.md#full-reset). Always make sure you are running the [latest firmware](Firmware-Update.md), which should have previously discovered problems fixed.

If you have tried both of these steps and are still experiencing problems, you may have found a new bug in the firmware. When you report it to the maintainer, please include:
* Your firmware version, shown at the top of the web interface, e.g. v0.6.4
* The steps to reproduce the crash, e.g. set up NTRIP caster without password, connect to WiFi network, etc.
* [_If possible_] A core dump, which you can download from http://192.168.4.1/core_dump on the device's hotspot, or http://ntrip.local/core_dump on your network. _This will still be available after a Full Reset, which might be necessary if the crash is being caused by your current configuration._
* [_If possible_] A log from the device, from the View log button in the Log & reset section, or http://ntrip.local/log.html.

This will allow the cause of the problem to be identified quickly.
