# Cloud Lamp — Firmware Install Guide

This puts the lamp software onto the ESP32 board. You do it once per lamp.
Budget about 30 minutes the first time (most of it is downloads).

You need:
- The lamp, with its ESP32 board reachable by a USB cable
- A USB data cable (some cheap cables are charge-only — if the board never shows
  up as a port later, try a different cable first)
- A computer (Mac or Windows) with internet

---

## 1. Install the Arduino IDE

1. Go to <https://www.arduino.cc/en/software>
2. Download **Arduino IDE 2.x** for your operating system and install it like any
   other app.
3. Open it once so it finishes first-time setup, then continue below.

---

## 2. Add ESP32 board support

The ESP32 isn't known to the IDE out of the box.

1. Open **Arduino IDE → Settings** (Windows: **File → Preferences**).
2. In **“Additional boards manager URLs”**, paste this and click OK:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
3. Open **Tools → Board → Boards Manager…** (or the board icon in the left
   sidebar).
4. Search **esp32**, find **“esp32” by Espressif Systems**, and click
   **Install** (use version **2.0.17** or any newer 2.0.x; 3.x also works).
   This is a big download — let it finish.

---

## 3. Install the libraries

Open **Tools → Manage Libraries…** (or the books icon in the left sidebar).
Search for each of these by name and click **Install**. If a pop-up asks to also
install dependencies, click **“Install All”**.

| Search for | Author |
|---|---|
| **Adafruit NeoPixel** | Adafruit |
| **Adafruit GFX Library** | Adafruit *(also installs “Adafruit BusIO” — allow it)* |
| **Adafruit ILI9341** | Adafruit |
| **Adafruit TouchScreen** | Adafruit |
| **PubSubClient** | Nick O’Leary |
| **ArduinoJson** | Benoit Blanchon *(latest is fine; if you see yellow “StaticJsonDocument is deprecated” warnings while compiling, ignore them — it still works)* |

You do **not** need to install WiFi, WiFiClientSecure, time, or Preferences —
those come with the ESP32 board package from step 2.

---

## 4. Open the sketch and set up credentials

1. Unzip the folder I sent you. Inside is a folder called **`cloud_lamp`**
   containing `cloud_lamp.ino`.
2. Double-click **`cloud_lamp.ino`** — it opens in the Arduino IDE.
   (The folder name and the `.ino` name must match, so keep them together.)
3. In the same folder, find **`secrets.h`**.
   - If `secrets.h` is already there and filled in, you're set.
   - If you only see **`secrets.h.example`**, make a copy of it named exactly
     **`secrets.h`** (same folder), then open it in a text editor and fill in:
     Wi-Fi name/password, the MQTT username/password, this lamp's ID, and the
     time zone line. Save it.
   - The bottom of the file has optional, commented-out personalization
     settings (anniversary date, day/night hours for the LED dial, LED ring
     orientation, startup brightness) — uncomment and edit only the ones you
     want to change from their defaults.
4. Back in the Arduino IDE, the `secrets.h` tab should appear next to the sketch.

---

## 5. Pick the board and port

Plug the lamp's ESP32 into the computer with the USB cable.

- **Tools → Board → esp32 → “ESP32 Dev Module”**
- **Tools → Port →** pick the new entry that appeared when you plugged it in
  (looks like `/dev/cu.usbserial-XXXX` or `/dev/cu.SLAB_USBtoUART` on Mac,
  `COM3` / `COM4` on Windows).
- Leave every other setting under **Tools** at its default.

**No port shows up?** It's almost always the cable or the USB-serial driver:
- Try a different USB cable (must be a data cable).
- Some boards need a driver for their USB chip:
  - CP2102: <https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers>
  - CH340: <https://www.wch-ic.com/downloads/CH341SER_MAC_ZIP.html>
  Install, unplug/replug, check **Tools → Port** again.

---

## 6. Upload

1. Click the **→ (Upload)** arrow button, top-left.
2. It compiles, then flashes. The bottom of the window shows progress.
3. If it stalls on **“Connecting….\_\_\_\_”**: press and hold the **BOOT** button
   on the ESP32 board until connecting starts, then release.
4. If upload fails partway: set **Tools → Upload Speed → 115200** and try again.

“**Done uploading**” at the bottom means it worked.

---

## 7. Check that it's alive

- The screen should light up and show a clock within a minute (it needs Wi-Fi to
  set the time first).
- Open **Tools → Serial Monitor**, set the dropdown to **115200 baud**. You
  should see lines like:
  ```
  WiFi: "YourNetwork"
  WiFi connected to "YourNetwork", IP: 192.168.x.x
  MQTT connected
  ```
- Tap the screen — the LED ring flashes and the other lamp gets a “blink”.
- Receiving a blink shows “I love you” on the screen and flashes the ring pink.

---

## Repeat for the second lamp

Steps 1–3 are already done. For the other lamp you only need to:
- Edit `secrets.h` so `LAMP_ID` / `OTHER_LAMP_ID` are swapped, the MQTT
  username/password are that lamp's, and `LAMP_TZ` matches where it lives.
- Do steps 5–7 again with the other board plugged in.

---

## Common issues

| Symptom | Fix |
|---|---|
| `secrets.h: No such file or directory` | You didn't create `secrets.h` (step 4). Copy `secrets.h.example` to `secrets.h`. |
| Screen works, clock never appears | Wi-Fi didn't connect. Check the SSID/password in `secrets.h`; watch Serial Monitor. |
| Clock works, `MQTT connect failed` in Serial | Wrong MQTT username/password, or that HiveMQ account isn't set up. |
| LED ring never lights, everything else works | Wiring to the ring (power, ground, or data). Not a software problem. |
| Upload error `A fatal error occurred: Failed to connect` | Hold **BOOT** during upload; try Upload Speed 115200; try another cable. |
