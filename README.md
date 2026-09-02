# PIR Notifier — LoLin NodeMCU v3 (ESP8266)

A self-contained sketch for a NodeMCU + PIR sensor that:

- Connects to your WiFi automatically once configured.
- If it can't connect (first boot, or WiFi unreachable), opens its own **open** WiFi hotspot so you can set it up from a phone or laptop.
- Serves a web page to scan and select your WiFi network, choose which pin the PIR is wired to (default **D0**), and set a URL to call when motion is detected.
- On motion: sends an HTTP GET to your configured URL and flashes the onboard LED.

## 1. Hardware

- LoLin NodeMCU v3 (ESP8266, e.g. CH340 variant)
- A PIR motion sensor (e.g. HC-SR501) with digital output that goes **HIGH** on motion
- Micro-USB cable

### Wiring

| PIR pin | NodeMCU pin |
|---|---|
| VCC | 3V3 or 5V (check your PIR module's spec — most HC-SR501 boards accept 5V from VIN, and their signal pin is 3.3V-safe) |
| GND | GND |
| OUT | D0 (default; changeable later from the web page) |

> **Note:** the onboard status LED is fixed to **D4 (GPIO2)**. Avoid selecting D4 for the PIR to prevent a conflict. Also, D3 (GPIO0) and D8 (GPIO15) are "boot strapping" pins on the ESP8266 — they work fine as a PIR input once the board has booted, but if the PIR happens to be pulling them a certain way *during power-on*, it can occasionally interfere with boot. D0 (the default) doesn't have this issue and is a safe default.

## 2. Arduino IDE setup

1. Install the [Arduino IDE](https://www.arduino.cc/en/software) (2.x recommended).
2. Add the ESP8266 board package:
   - **File → Preferences → Additional Board Manager URLs**, add:
     `http://arduino.esp8266.com/stable/package_esp8266com_index.json`
   - **Tools → Board → Boards Manager**, search `esp8266`, install **"esp8266 by ESP8266 Community"**.
3. Select the board:
   - **Tools → Board → ESP8266 Boards → NodeMCU 1.0 (ESP8266-12E Module)**
4. Set the flash size so there's room for the LittleFS filesystem used to store your config:
   - **Tools → Flash Size → 4MB (FS:2MB OTA:~1019KB)** (any option that reserves an FS partition works)
5. Install the one required library:
   - **Tools → Manage Libraries…**, search **ArduinoJson** (by Benoit Blanchon), install the latest **6.x** version.
6. Select the correct **Port** for your NodeMCU (install the CH340 USB driver first if the board doesn't show up).

## 3. Flash the sketch

1. Open `pir_notifier.ino` in the Arduino IDE.
2. Click **Upload** (the arrow icon). The IDE will compile and flash the board.
3. Open **Tools → Serial Monitor** at 115200 baud if you want to watch boot messages (optional — not required for setup, everything is done over WiFi).

## 4. First-time setup

1. After flashing, the board has no saved WiFi credentials, so it starts its own **open** hotspot named:
   ```
   PIR-Setup
   ```
2. On your phone or laptop, connect to the **PIR-Setup** WiFi network (no password).
3. Open a browser and go to:
   ```
   http://192.168.4.1
   ```
4. Click **Scan networks**, click your network in the list (this fills in the SSID field), enter your WiFi password, and click **Save WiFi & Reboot**.
5. The board reboots and connects to your network. It will then be reachable at whatever IP your router assigns it — check your router's device list, or reopen the Serial Monitor at boot to see the printed IP (you can also add a static DHCP reservation in your router for convenience).

## 5. Configure the PIR pin and trigger URL

1. Visit the board's page again (now on your normal WiFi, at its new IP address instead of 192.168.4.1).
2. Under **PIR / Trigger settings**:
   - Choose which pin the PIR is wired to (defaults to **D0**).
   - Enter the **Trigger URL** to call when motion is detected, e.g. `http://homeassistant.local:8123/api/webhook/...` or any HTTP/HTTPS endpoint.
3. Click **Save Settings**. This applies immediately — no reboot needed.

Now, whenever the PIR triggers:
- The board sends an **HTTP GET** to your configured URL.
- The onboard LED flashes a few times.
- There's a 5-second cooldown between triggers to avoid spamming the URL.

## 6. Resetting configuration

To wipe the saved WiFi and settings and go back into setup mode:

1. Power off the board.
2. Press and hold the onboard **FLASH** button (labeled FLASH, next to the USB port — this is GPIO0 / D3).
3. While still holding it, power the board back on (plug in USB), then release the button after a second or two.
4. The saved config is deleted and the board comes back up as the open **PIR-Setup** hotspot.

## Notes / limitations

- The setup hotspot is intentionally **open** (no password) for easy first-time access — don't leave it exposed long-term; it's normally only active until you save WiFi credentials, after which the board switches to station mode.
- The web configuration page itself has no login — anyone on your network (or connected to the setup hotspot) can change settings. Fine for a home project; add your own auth if you need more security.
- HTTPS trigger URLs are supported but certificate validation is disabled (`setInsecure()`) to keep things simple — traffic is encrypted but the server identity isn't verified.
- The PIR is read by polling in the main loop (not an interrupt), which works reliably on every GPIO including D0 (which doesn't support interrupts on the ESP8266 anyway).
