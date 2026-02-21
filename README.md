# funnyportal -- ESP32 WiFi Captive Portal with OLED Display

A self-contained ESP32 sketch that displays status on a 0.96" SSD1306 OLED and
lets you configure WiFi credentials through a captive portal served from an open
access point.

## Wiring

| OLED Pin | ESP32 GPIO |
|----------|------------|
| SDA      | GPIO 18    |
| SCL      | GPIO 33    |
| VCC      | 3.3 V      |
| GND      | GND        |

## Required Libraries

Install these through the Arduino IDE **Library Manager** (Sketch > Include Library > Manage Libraries):

| Library            | Author     | Notes                          |
|--------------------|------------|--------------------------------|
| Adafruit GFX       | Adafruit   | Library Manager                |
| Adafruit SSD1306   | Adafruit   | Library Manager                |

The following libraries need to be installed **manually** (download the ZIP from
GitHub, then Sketch > Include Library > Add .ZIP Library):

| Library             | Source                                                  |
|---------------------|---------------------------------------------------------|
| ESPAsyncWebServer   | https://github.com/me-no-dev/ESPAsyncWebServer         |
| AsyncTCP            | https://github.com/me-no-dev/AsyncTCP                  |

`WiFi`, `Wire`, `FS`, `SPIFFS`, and `DNSServer` are bundled with the ESP32
Arduino core and do not need separate installation.

### ESP32 Board Support

If you haven't already, add ESP32 board support to the Arduino IDE:

1. Open **File > Preferences**.
2. In *Additional Board Manager URLs* add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Open **Tools > Board > Boards Manager**, search for **esp32** and install
   *esp32 by Espressif Systems*.

## Uploading the Sketch

1. Open `funnyportal.ino` in the Arduino IDE.
2. Select **Tools > Board > ESP32 Dev Module**.
3. Set **Partition Scheme** to *Default 4MB with spiffs* (or any scheme that
   includes a SPIFFS partition).
4. Select the correct **Port** (COM port of your ESP32).
5. Click **Upload**.

## Uploading SPIFFS Data (config.txt)

The `data/` folder contains a blank `config.txt` that must be written to the
ESP32's SPIFFS partition so the device boots into AP mode on first run.

1. Install the **ESP32 Sketch Data Upload** plugin for the Arduino IDE:
   - Arduino IDE 1.x: https://github.com/me-no-dev/arduino-esp32fs-plugin
   - Arduino IDE 2.x: Use the *LittleFS/SPIFFS Upload* extension or the
     `esptool.py` command line.
2. Make sure the `data/` folder sits next to the `.ino` file:
   ```
   funnyportal/
   ├── funnyportal.ino
   └── data/
       └── config.txt
   ```
3. In the Arduino IDE: **Tools > ESP32 Sketch Data Upload**.
4. Wait for the upload to finish.

## How It Works

```
Boot
 │
 ├─ Read /config.txt
 │   ├─ Credentials found ──► Try WiFi STA (20 retries)
 │   │                            ├─ Connected ──► Show IP on OLED, done
 │   │                            └─ Failed ──────┐
 │   └─ No credentials ──────────────────────────►│
 │                                                 ▼
 └──────────────────────────────────────► Start AP "SetupWiFi"
                                          Start DNS catch-all
                                          Start web server
                                          Show "AP Mode" on OLED
```

1. **Power on without config** -- the ESP32 starts an open AP called
   `SetupWiFi`.
2. **Connect a phone/laptop** to `SetupWiFi`. The captive portal should
   auto-open (or navigate to `192.168.4.1`).
3. **Log in** with username `admin` and password `esp32admin`.
4. **Enter your WiFi SSID and password**, then tap *Save & Restart*.
5. The ESP32 reboots and connects to your WiFi network. The OLED shows the
   assigned IP address.

## Viewing Logs

Device connect/disconnect events and configuration changes are appended to
`/log.txt` on SPIFFS. You can read them by:

- Watching the **Serial Monitor** at 115200 baud (events are echoed there in
  real time).
- Adding a simple `/log` web route in a future version to serve the file over
  HTTP.

## Security Notice

The HTTP Basic Auth credentials (`admin` / `esp32admin`) are hardcoded and
transmitted in **plaintext** over the open AP. This is acceptable for a
demo/portfolio project on your own devices but is **not suitable for
production**. For a real deployment you would use WPA2 on the AP, HTTPS (TLS),
and stored hashed passwords.
