# Getting Started

This guide describes how to connect to and configure the ESP32 NTRIP firmware on an Adafruit HUZZAH32 wired to a u-blox ZED-F9P.

You will need:
* An Adafruit HUZZAH32 (ESP32 Feather) with this firmware installed. If it doesn't have it yet, follow the [USB flash](Firmware-Update.md#usb-flash) steps first.
* A ZED-F9P with its UART1 wired to the ESP32's UART0 (GPIO1/GPIO3).
* Optionally, an RGB status LED on GPIO21/22/23.

See [Pinout](Pinout.md) for the wiring.

### Step 1: Connect to the Hotspot
Power the HUZZAH32 and, using your phone or PC, connect to its WiFi Hotspot, which will be called `ESP_XBee_XXXXXX` where `XXXXXX` are some letters/numbers unique to your device. The hotspot is open (no password) until you change it in [Step 4](#step-4-improve-security).

### Step 2: Browse to the Configuration Page
Open your browser and navigate to http://192.168.4.1/.

### Step 3: Configure WiFi
Enable the WiFi section, press Scan to search for networks, choose your home WiFi network (or phone hotspot) and enter the password.

_The ESP32 does not have very good WiFi reception, so make sure you are relatively close to the network you are connecting to, especially when indoors, to avoid problems._

Press the Submit button, and follow the instructions on screen. You may need to reconnect to the hotspot after it restarts.

The WiFi section will now show its connection status and IP address. Once it is connected, switch your phone/PC back to your home network and open either:
* http://ntrip.local/, the name the device announces with mDNS. This works on Windows 10+, macOS, iOS and most Linux desktops, while Android support varies.
* The IP address shown in the WiFi section.

#### Internet health
The bottom of the WiFi section shows whether the device can reach the Internet, with the round-trip time and the host it checked:
* With the NTRIP client on, it pings the caster. This needs DNS (UDP 53, unless the caster is entered as an IP address), ping (ICMP) to the caster, and the caster's own port (usually 2101). No web ports are used, so it works on networks that block browsing.
* With the NTRIP client off, it does a web check against `connectivitycheck.gstatic.com`, then `cp.cloudflare.com`, over HTTP (TCP 80) or, if that is blocked, a plain connection on TCP 443. It checks every minute, or every 10 seconds while the Internet is down. **Login page / captive portal** means port 80 answered with a page, so the network probably wants you to sign in.

Some casters and networks drop pings, all the time or now and then, in which case it shows **No ping reply from caster** until the NTRIP client connects, and **Internet** once corrections are flowing.

#### The hotspot switches itself off
When both the WiFi Hotspot and WiFi sections are enabled, the hotspot switches off 5 minutes after WiFi gets an IP address. If a device is still connected to the hotspot at that point, it waits another 5 minutes and checks again. The WiFi Hotspot section is grayed out while the hotspot is off.

If the WiFi connection is lost, the hotspot switches back on by itself, so you can always get back in through http://192.168.4.1/.

#### Alternatively
If you are connected to the HUZZAH32's USB port with a serial terminal at 115200 baud, the device will output information about its WiFi connection.

```
$PESP,WIFI,STA,CONNECTING,MyNetwork,P,F*23
$PESP,WIFI,STA,CONNECTED,MyNetwork*74
$PESP,WIFI,STA,IP,192.168.1.100/24,192.168.1.1*5D
$PESP,WIFI,STA,INTERNET,OK,rtk2go.com*6A
```

The new IP address will be the first address in the 3rd line as above, i.e. `192.168.1.100`.

### Step 4: Improve Security
#### Hotspot
To prevent others from accessing the device, you may want to adjust the settings of the WiFi hotspot section.

There are three ways to do this:
1. Change from Open security to WPA/2-PSK and enter a password (recommended).
2. Hide the SSID by toggling the Hidden setting. _This will not prevent connections, but will hide the hotspot from WiFi scans on other devices._
3. Disable the hotspot entirely by toggling the WiFi hotspot section. _You will not be able to access the device if there is a problem with its connection to your home WiFi network, unless you perform a [Full Reset](#full-reset). Since the hotspot already [switches itself off](#the-hotspot-switches-itself-off) while WiFi is connected, option 1 is usually the better choice._

#### Configuration
If you would like to prevent others on your network from modifying the configuration, you can also adjust the settings of the Admin section.

You can choose between only allowing devices connected to the hotspot, or a username/password.

These settings will have no effect on access to the data sockets or NTRIP caster, which will still be available to all devices on the network.

### Step 5: Connect the F9P
#### UART
The UART section defaults to UART0 on TX GPIO1 / RX GPIO3 at 115200 baud, 8N1, no flow control. This matches the F9P's UART1 in the configurations written by the F9P receiver section below, so you normally don't need to change it.

Once the receiver is sending GGA, the bottom of the UART section shows its fix type, satellites and HDOP, plus VDOP from GSA and the 1-sigma latitude/longitude/altitude error from GST.

#### F9P receiver
The F9P receiver section reads and writes the receiver's configuration over the UART. Choose a mode:
* **Dual GPS**: for the moving base (right) receiver of a dual-antenna setup. UART1 to NTRIP: NAV-PVT and GGA at 10 Hz, VTG, GSA and GST at 1 Hz. UART2: RTCM3 to the second F9P at 460800 baud.
* **RS-232**: for a single receiver. UART1 to NTRIP: GGA at 10 Hz, VTG, GSA and GST at 1 Hz. UART2 (and USB): NMEA with the rates and baud rate you choose below the mode.

Press **Check F9P** to compare the receiver's active and saved configuration with the selected mode, or **Write to F9P** to write it and save it in the receiver. NTRIP corrections to the receiver pause for a few seconds while it is written.

The configurations are built from the u-center files in [`tools/f9p`](../tools/f9p).

### Step 6: Set up Protocols
You can now proceed to configure the available protocols.

_Do not enable all protocols at once. The ESP32 is not able to handle an unlimited amount of open sockets/connections, so only enable the protocols you are actually using._

#### NTRIP client
Enter the caster's host, port, username and password, then press **Scan** beside Mountpoint. The device downloads the caster's sourcetable and lists its mountpoints, split into open ones and ones that need a login, so you can pick one.

While the client is running, the bottom of the NTRIP client section shows the base station ID from its RTCM 1005/1006 position, the 3D, horizontal and vertical distance from the base antenna, and the age of the corrections at the receiver.

The client doesn't connect until the receiver sends a GGA with a position (any fix, not older than 10 seconds), since an NTRIP mountpoint may send nothing until it gets one, and a caster may stop answering a client that keeps reconnecting. Until then the bottom of the section shows **Waiting for a GGA position from the receiver** and the serial port shows `$PESP,NTRIP,CLI,WAITING_GGA`. If the position is lost while connected and the NTRIP mountpoint stops sending, the client disconnects after 10 seconds and waits again.

Before each connection attempt the client looks up and pings the caster. If the caster's name doesn't resolve after 3 tries, the client waits for the Internet instead of retrying (`$PESP,NTRIP,CLI,WAITING_INTERNET`) and connects as soon as the name resolves again. A missing ping reply doesn't stop it trying, since some networks drop pings while the caster works fine. Failed connections are retried at most a minute apart.

#### LEDs
The small color selector beside each section's toggle button sets the color of the RGB LED for that feature, so that you can keep track of its status. Setting the color to black disables the LED for that feature. Typically, a fading LED means the feature is working correctly/connected, while a blinking LED or no LED means the feature is awaiting a connection or could not connect to its target.

The defaults are red for the WiFi Hotspot, blue for WiFi and green for the NTRIP client. All other sections start with their LED off.

The HUZZAH32's onboard red LED (GPIO13) blinks at 1 Hz while the firmware is running. After an unexpected restart, such as a crash, it blinks at 5 Hz for 10 seconds, together with a fast red blink on the RGB LED.

### Step 7: Start Using!
Your device is now fully configured!

#### Full Reset
If you ever experience any problems, there are a few ways to reset the device to its default configuration:
* In the web interface, press **Reset to defaults** in the Log & reset section. This keeps the WiFi hotspot settings, so you can still reach the device on its own hotspot, and keeps the WiFi network and password unless you tick the box to clear them.
* If you are unable to connect to the device at all, erase the whole configuration over USB with `esptool erase_region 0x9000 0x6000`, as described on the [Firmware Update](Firmware-Update.md#resetting-the-configuration) page.
* On boards with a BOOT button on GPIO0, hold it for 5 seconds. _The HUZZAH32 does not have one, as GPIO0 is not broken out._

A reset does not delete the last core dump, see [Reporting Crashes](Reporting-Crashes.md).
