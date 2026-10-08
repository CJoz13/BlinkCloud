// Cloud Lamp - MVP firmware
// Screen shows current time. Tap anywhere to send a "blink" to the paired lamp
// over MQTT. Sending or receiving a blink plays a flash animation on the
// NeoPixel ring. When idle, the ring acts as a 24-hour sun/moon dial showing
// what time it currently is at the OTHER lamp (each of the 24 LEDs = 1 hour).
//
// Not yet implemented (see README.md "Status"):
// weather-driven modes, BLE presence, missed-blink backlog, multi-button UI,
// captive-portal wifi login.
//
// Wiring (see README.md "Signal wiring" for the full table):
//   TFT:   CLK18 MISO19 MOSI23 CS15 DC2 RST4 (hardware VSPI)
//   Touch: X+32 Y+33 X-21 Y-22
//   LED:   GPIO13 -> 330ohm resistor -> NeoPixel ring DIN (24x RGBW)

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <TouchScreen.h>
#include <Adafruit_NeoPixel.h>

#include "secrets.h"

// Optional open (passwordless) network, tried before the WPA2 one. Define it in
// secrets.h; leaving it undefined or "" just skips straight to WIFI_SSID.
#ifndef WIFI_SSID_OPEN
#define WIFI_SSID_OPEN ""
#endif

// ---- Pins ----
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  4

#define TS_XP 32
#define TS_YP 33
#define TS_XM 21
#define TS_YM 22

#define LED_PIN     13
#define NUM_PIXELS  24

// ---- Touch calibration (raw ADC values; recalibrate for your panel) ----
#define TS_MINX 150
#define TS_MAXX 920
#define TS_MINY 120
#define TS_MAXY 940
// setupTouch() forces analogReadResolution(10), so these are back on the library's
// native 10-bit scale. Tune with TOUCH_DEBUG if a firm tap still won't register.
#define MINPRESSURE 10
#define MAXPRESSURE 1200

// ---- Timing (internal tuning, not meant to be overridden per-lamp) ----
static const uint32_t WIFI_RECONNECT_INTERVAL_MS = 10000;
static const uint32_t WIFI_JOIN_TIMEOUT_MS        = 12000; // per-network wait during boot
static const uint32_t MQTT_RECONNECT_INTERVAL_MS  = 5000;
static const uint32_t STATUS_PUBLISH_INTERVAL_MS  = 30000;
static const uint32_t TOUCH_DEBOUNCE_MS           = 800;
static const uint32_t BLINK_ANIM_DURATION_MS      = 1600;
static const uint32_t HOLIDAY_BLINK_DURATION_MS   = 5000; // themed holiday blink runs longer
static const uint32_t LOVE_MSG_DURATION_MS        = 8000; // "I love you" hold on the TFT
static const uint32_t OTHER_STATUS_STALE_MS       = 5UL * 60UL * 1000UL;

// If the WiFi AP is down for days straight, the ESP32's WiFi driver can wedge
// into a state where plain WiFi.begin() retries never rejoin even once the AP
// is back (seen reconnecting lamp2 to its open network after a multi-day
// outage). These two thresholds self-heal that without affecting normal,
// short reconnects:
static const uint32_t WIFI_HARD_RESET_INTERVAL_MS = 5UL * 60UL * 1000UL;        // full radio off/on cycle if still offline this long since the last one
static const uint32_t WIFI_RESTART_AFTER_MS       = 3UL * 60UL * 60UL * 1000UL; // reboot if WiFi has been down this long straight (last-resort)

// ---- Per-lamp personalization ----
// Every value below can be overridden by defining it in secrets.h (same pattern
// as WIFI_SSID_OPEN above) -- nothing personal to a given lamp/couple needs to be
// edited in this shared source file.

// POSIX TZ string for THIS lamp's physical location. Set per-device in secrets.h
// (LAMP_TZ) so each lamp keeps its own wall-clock time and broadcasts the correct
// local hour to its partner. Falls back to America/Detroit if secrets.h omits it.
//   Michigan (America/Detroit): "EST5EDT,M3.2.0,M11.1.0"
//   Vienna   (Europe/Vienna):   "CET-1CEST,M3.5.0,M10.5.0/3"
#ifndef LAMP_TZ
#define LAMP_TZ "EST5EDT,M3.2.0,M11.1.0"
#endif
static const char* TZ_INFO = LAMP_TZ;

// On this month/day the TFT shows "Happy N years" under the clock, where
// N = current year - ANNIVERSARY_START_YEAR (so Jan 2 2026 -> "Happy 3 years").
// Month/day default to 0, which never matches a real calendar date, so the
// banner stays off until secrets.h defines a real date.
#ifndef ANNIVERSARY_MONTH
#define ANNIVERSARY_MONTH 0   // 1-12, 0 = disabled
#endif
#ifndef ANNIVERSARY_DAY
#define ANNIVERSARY_DAY 0     // 1-31, 0 = disabled
#endif
#ifndef ANNIVERSARY_START_YEAR
#define ANNIVERSARY_START_YEAR 2023
#endif

// Local hours during which the idle LED ring treats the *other* lamp as
// "daytime" for its sun/moon dial colors.
#ifndef DAY_START_HOUR
#define DAY_START_HOUR 7
#endif
#ifndef DAY_END_HOUR
#define DAY_END_HOUR 20
#endif

// Which physical NeoPixel sits at hour 0 (midnight). Adjust so the dial lines up
// with however the ring is glued into the lid -- e.g. set to 12 to put noon at
// pixel 0 / "top". Wraps mod 24.
#ifndef HOUR_LED_OFFSET
#define HOUR_LED_OFFSET 0
#endif

// Startup LED brightness (0-255). MAX_BRIGHTNESS is a firmware-enforced safety
// ceiling for future brightness controls and is intentionally not overridable.
#ifndef DEFAULT_BRIGHTNESS
#define DEFAULT_BRIGHTNESS 40
#endif
static const uint8_t MAX_BRIGHTNESS = 120;

// ---- Globals ----
Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);
// Adafruit_TouchScreen must analogRead() its "yp" and "xm" args, so BOTH of
// those must land on ESP32 ADC1 pins (ADC2 is unusable while WiFi is on).
// Wiring is Y+ =GPIO33, X+ =GPIO32 (both ADC1), X- =GPIO21, Y- =GPIO22 (no ADC).
// So we hand the library GPIO32 as its "xm" (it only senses, doesn't care which
// X terminal) and the real X- (GPIO21) as its "xp". Args: (xp, yp, xm, ym).
TouchScreen ts(TS_XM, TS_YP, TS_XP, TS_YM, 300);

// Set true to stream raw touch x/y/z over Serial for calibrating the constants
// above (TS_MIN*/TS_MAX*/MINPRESSURE/MAXPRESSURE).
#define TOUCH_DEBUG false
Adafruit_NeoPixel pixels(NUM_PIXELS, LED_PIN, NEO_GRBW + NEO_KHZ800);

WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

String cmdTopicSelf;
String statusTopicSelf;
String statusTopicOther;

uint32_t lastWifiAttemptMillis = 0;
uint32_t lastWifiConnectedMillis = 0;  // last millis() WiFi.status() read WL_CONNECTED
uint32_t lastWifiHardResetMillis = 0;  // last millis() the WiFi radio was fully cycled while offline
uint32_t lastMqttAttemptMillis = 0;
uint32_t lastStatusPublishMillis = 0;
uint32_t lastTouchMillis = 0;

// WiFi networks, tried in order. pass == nullptr / "" means an open network.
// (Declared up here so the .ino auto-prototype pass sees the type.)
struct WifiNet { const char* ssid; const char* pass; };
static const WifiNet WIFI_NETWORKS[] = {
  { WIFI_SSID_OPEN, nullptr },        // open network, tried first
  { WIFI_SSID,      WIFI_PASSWORD },  // WPA2 fallback
};
static const int WIFI_NETWORK_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);
static int wifiNetIdx = -1; // index of the network we last tried / are joined to

int lastDrawnMinuteOfDay = -1;

// When >0, the TFT is showing the "I love you" message until this millis().
uint32_t loveMsgUntilMillis = 0;
void showLoveMessage(); // defined in the Display section, called from mqttCallback

// True while the TFT is in anniversary layout (clock raised, banner beneath).
bool anniversaryScreenActive = false;

bool otherStatusEverReceived = false;
uint32_t lastOtherStatusMillis = 0;
uint8_t otherHour = 12;
bool otherIsDaytime = true;

enum BlinkAnimKind { BLINK_NONE, BLINK_SENT, BLINK_RECEIVED };
BlinkAnimKind blinkAnim = BLINK_NONE;
uint32_t blinkAnimStartMillis = 0;

// Calendar events that get their own LED blink animation (and, for the
// anniversary, a TFT banner). Detected from local date at blink start.
enum CalEvent {
  CAL_NONE,
  CAL_ANNIVERSARY,   // ANNIVERSARY_MONTH/DAY
  CAL_NEW_YEAR,      // Dec 31 - Jan 1
  CAL_VALENTINES,    // Feb 14
  CAL_ST_PATRICKS,   // Mar 17
  CAL_JULY_4,        // Jul 4
  CAL_HALLOWEEN,     // Oct 31
  CAL_THANKSGIVING,  // 4th Thursday of November
  CAL_CHRISTMAS,     // Dec 24 - 25
};
CalEvent blinkEvent = CAL_NONE; // event captured when the current blink started

// ---- Time helpers ----

bool getLocalTm(struct tm &timeinfo) {
  return getLocalTime(&timeinfo, 100);
}

bool isDaytimeHour(int hour) {
  return hour >= DAY_START_HOUR && hour < DAY_END_HOUR;
}

// Is date the Nth given weekday of its month? weekday: 0=Sun..6=Sat, nth is 1-based.
static bool isNthWeekdayOfMonth(const struct tm &t, int weekday, int nth) {
  if (t.tm_wday != weekday) return false;
  return ((t.tm_mday - 1) / 7 + 1) == nth;
}

CalEvent currentCalEvent(const struct tm &t) {
  int mon = t.tm_mon + 1; // 1-12
  int day = t.tm_mday;

  if (mon == ANNIVERSARY_MONTH && day == ANNIVERSARY_DAY) return CAL_ANNIVERSARY;
  if ((mon == 1 && day == 1) || (mon == 12 && day == 31)) return CAL_NEW_YEAR;
  if (mon == 2 && day == 14) return CAL_VALENTINES;
  if (mon == 3 && day == 17) return CAL_ST_PATRICKS;
  if (mon == 7 && day == 4)  return CAL_JULY_4;
  if (mon == 10 && day == 31) return CAL_HALLOWEEN;
  if (mon == 11 && isNthWeekdayOfMonth(t, 4, 4)) return CAL_THANKSGIVING;
  if (mon == 12 && (day == 24 || day == 25)) return CAL_CHRISTMAS;
  return CAL_NONE;
}

int anniversaryYears(const struct tm &t) {
  return (t.tm_year + 1900) - ANNIVERSARY_START_YEAR;
}

// ---- Wifi ----

static bool wifiSlotUsable(const WifiNet& n) { return n.ssid && n.ssid[0]; }
static bool wifiSlotOpen(const WifiNet& n)   { return !(n.pass && n.pass[0]); }

static void wifiBeginNet(int idx) {
  const WifiNet& n = WIFI_NETWORKS[idx];
  Serial.printf("WiFi: \"%s\"%s\n", n.ssid, wifiSlotOpen(n) ? " (open)" : "");
  WiFi.disconnect();
  if (wifiSlotOpen(n)) WiFi.begin(n.ssid);
  else                 WiFi.begin(n.ssid, n.pass);
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  Serial.print("ESP32 MAC address: ");
  Serial.println(WiFi.macAddress());

  for (int i = 0; i < WIFI_NETWORK_COUNT; i++) {
    if (!wifiSlotUsable(WIFI_NETWORKS[i])) continue;
    wifiBeginNet(i);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_JOIN_TIMEOUT_MS) {
      delay(250);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      wifiNetIdx = i;
      Serial.printf("WiFi connected to \"%s\", IP: %s\n",
                    WIFI_NETWORKS[i].ssid, WiFi.localIP().toString().c_str());
      return;
    }
  }
  Serial.println("WiFi: no network joined; will keep retrying in loop().");
}

void maintainWiFi() {
  uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    lastWifiConnectedMillis = now;
    return;
  }

  // Last resort: WiFi has been down for hours straight. Rather than trust that
  // any software-level recovery below has actually cleared the driver's state,
  // just reboot -- the lamp has no state a restart loses, and this guarantees
  // recovery from whatever multi-day-retry-loop wedge caused the outage.
  if (now - lastWifiConnectedMillis > WIFI_RESTART_AFTER_MS) {
    Serial.println("WiFi: no connection for hours straight, restarting...");
    ESP.restart();
  }

  if (now - lastWifiAttemptMillis < WIFI_RECONNECT_INTERVAL_MS) return;
  lastWifiAttemptMillis = now;

  // Periodically fully power-cycle the WiFi radio (not just disconnect/begin)
  // before retrying. Long unbroken retry loops can leave the driver's internal
  // state (scan cache, association state machine) stale enough that plain
  // retries never rejoin even after the AP comes back -- this clears it out.
  if (now - lastWifiHardResetMillis > WIFI_HARD_RESET_INTERVAL_MS) {
    lastWifiHardResetMillis = now;
    Serial.println("WiFi: cycling radio before retry (stale-state recovery)");
    WiFi.mode(WIFI_OFF);
    delay(100);
    WiFi.mode(WIFI_STA);
  }

  // Non-blocking: kick off a connect to the next usable network and return.
  // The next call (WIFI_RECONNECT_INTERVAL_MS later) checks whether it worked
  // and, if not, moves on to the following network.
  for (int step = 1; step <= WIFI_NETWORK_COUNT; step++) {
    int cand = (wifiNetIdx + step) % WIFI_NETWORK_COUNT;
    if (!wifiSlotUsable(WIFI_NETWORKS[cand])) continue;
    wifiNetIdx = cand;
    Serial.print("WiFi retry -> ");
    wifiBeginNet(cand);
    return;
  }
}

// ---- Time sync ----

void setupTime() {
  configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  Serial.print("Waiting for NTP time sync");
  uint32_t start = millis();
  while (!getLocalTm(timeinfo) && millis() - start < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
}

// ---- MQTT ----

void publishBlink() {
  struct tm timeinfo;
  getLocalTm(timeinfo);

  StaticJsonDocument<128> doc;
  doc["type"] = "blink";
  doc["from"] = LAMP_ID;
  doc["timestamp"] = (uint32_t)time(nullptr);

  char buf[128];
  size_t n = serializeJson(doc, buf);
  String cmdTopicOther = String("cloudlamp/") + OTHER_LAMP_ID + "/command";
  mqttClient.publish(cmdTopicOther.c_str(), (uint8_t*)buf, n, false);
  Serial.println("Published blink to " + cmdTopicOther);
}

void publishStatus() {
  struct tm timeinfo;
  if (!getLocalTm(timeinfo)) return;

  StaticJsonDocument<128> doc;
  doc["lamp"] = LAMP_ID;
  doc["hour"] = timeinfo.tm_hour;
  doc["isDaytime"] = isDaytimeHour(timeinfo.tm_hour);
  doc["epoch"] = (uint32_t)time(nullptr);

  char buf[128];
  size_t n = serializeJson(doc, buf);
  mqttClient.publish(statusTopicSelf.c_str(), (uint8_t*)buf, n, true);
}

void startBlinkAnim(BlinkAnimKind kind) {
  blinkAnim = kind;
  blinkAnimStartMillis = millis();
  struct tm t;
  blinkEvent = getLocalTm(t) ? currentCalEvent(t) : CAL_NONE;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<192> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.println("MQTT payload parse failed: " + String(err.c_str()));
    return;
  }

  String topicStr(topic);

  if (topicStr == cmdTopicSelf) {
    const char* type = doc["type"] | "";
    if (strcmp(type, "blink") == 0) {
      Serial.println("Blink received from " + String((const char*)(doc["from"] | "?")));
      startBlinkAnim(BLINK_RECEIVED);
      showLoveMessage();
    }
    return;
  }

  if (topicStr == statusTopicOther) {
    otherHour = doc["hour"] | otherHour;
    otherIsDaytime = doc["isDaytime"] | otherIsDaytime;
    otherStatusEverReceived = true;
    lastOtherStatusMillis = millis();
    return;
  }
}

void maintainMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) {
    mqttClient.loop();
    return;
  }

  uint32_t now = millis();
  if (now - lastMqttAttemptMillis < MQTT_RECONNECT_INTERVAL_MS) return;
  lastMqttAttemptMillis = now;

  Serial.println("Connecting to MQTT...");
  if (mqttClient.connect(LAMP_ID, MQTT_USER, MQTT_PASSWORD)) {
    Serial.println("MQTT connected");
    mqttClient.subscribe(cmdTopicSelf.c_str());
    mqttClient.subscribe(statusTopicOther.c_str());
    publishStatus();
    lastStatusPublishMillis = millis();
  } else {
    Serial.println("MQTT connect failed, rc=" + String(mqttClient.state()));
  }
}

void publishStatusIfDue() {
  if (!mqttClient.connected()) return;
  uint32_t now = millis();
  if (now - lastStatusPublishMillis < STATUS_PUBLISH_INTERVAL_MS) return;
  lastStatusPublishMillis = now;
  publishStatus();
}

// ---- Display ----

void setupDisplay() {
  // Long/loose F/F jumper wires can't reliably carry the library's default
  // near-max SPI clock; slow it down to rule out signal integrity issues.
  tft.begin(16000000);
  tft.setRotation(3);  // landscape, flipped 180 from rotation 1
  tft.fillScreen(ILI9341_BLACK);
}

static void drawCenteredText(const char* s, int16_t yCenter, uint8_t size,
                             uint16_t color) {
  int16_t x1, y1;
  uint16_t w, h;
  tft.setTextSize(size);
  tft.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  tft.setTextColor(color);
  tft.setCursor((tft.width() - w) / 2, yCenter - h / 2);
  tft.print(s);
}

static void drawHeart(int16_t cx, int16_t cy, int16_t s, uint16_t color) {
  tft.fillCircle(cx - s / 2, cy - s / 4, s / 2, color);
  tft.fillCircle(cx + s / 2, cy - s / 4, s / 2, color);
  tft.fillTriangle(cx - s, cy - s / 6, cx + s, cy - s / 6, cx, cy + s, color);
}

void drawClockScreen(struct tm &timeinfo) {
  int displayHour = timeinfo.tm_hour % 12;
  if (displayHour == 0) displayHour = 12;
  const char* ampm = timeinfo.tm_hour >= 12 ? "PM" : "AM";

  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d %s", displayHour, timeinfo.tm_min, ampm);

  int16_t x1, y1;
  uint16_t w, h;
  tft.setTextSize(4);
  tft.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  int16_t x = (tft.width() - w) / 2;
  // Raise the clock on anniversary day to make room for the banner below it.
  int16_t yCenter = anniversaryScreenActive ? tft.height() / 2 - 40 : tft.height() / 2;
  int16_t y = yCenter - h / 2;

  tft.fillRect(0, y - 4, tft.width(), h + 8, ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(x, y);
  tft.print(buf);
}

void drawAnniversaryBanner(struct tm &timeinfo) {
  char msg[24];
  snprintf(msg, sizeof(msg), "Happy %d years", anniversaryYears(timeinfo));
  const uint16_t pink = tft.color565(255, 90, 140);
  int16_t midY = tft.height() / 2;
  drawCenteredText(msg, midY + 34, 3, pink);
  drawHeart(tft.width() / 2, midY + 78, 16, pink);
}

void drawLoveScreen() {
  const uint16_t pink = tft.color565(255, 90, 140);
  tft.fillScreen(ILI9341_BLACK);
  int16_t midY = tft.height() / 2;
  drawCenteredText("I love you", midY - 24, 4, pink);
  drawHeart(tft.width() / 2, midY + 46, 26, pink);
}

// Called from the MQTT callback when a blink arrives. Shows the message now and
// keeps it up for LOVE_MSG_DURATION_MS before the clock is allowed back.
void showLoveMessage() {
  loveMsgUntilMillis = millis() + LOVE_MSG_DURATION_MS;
  drawLoveScreen();
}

void updateDisplayIfNeeded() {
  if (loveMsgUntilMillis != 0) {
    if ((int32_t)(millis() - loveMsgUntilMillis) < 0) return; // still holding message
    loveMsgUntilMillis = 0;
    tft.fillScreen(ILI9341_BLACK);    // wipe the message
    anniversaryScreenActive = false; // force the banner to be re-evaluated/redrawn
    lastDrawnMinuteOfDay = -1;
  }

  struct tm timeinfo;
  if (!getLocalTm(timeinfo)) return;

  // Enter/leave the anniversary layout when the date rolls over.
  bool anniv = (currentCalEvent(timeinfo) == CAL_ANNIVERSARY);
  if (anniv != anniversaryScreenActive) {
    anniversaryScreenActive = anniv;
    tft.fillScreen(ILI9341_BLACK);
    lastDrawnMinuteOfDay = -1;
    if (anniv) drawAnniversaryBanner(timeinfo); // banner is static; drawn once here
  }

  int minuteOfDay = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  if (minuteOfDay == lastDrawnMinuteOfDay) return;
  lastDrawnMinuteOfDay = minuteOfDay;
  drawClockScreen(timeinfo);
}

// ---- Touch ----

void setupTouch() {
  // Adafruit_TouchScreen does its coordinate/pressure math assuming a 10-bit ADC
  // (constants of 1023/1024). The ESP32 defaults to 12-bit (0-4095), which makes
  // the pressure value ~16x too large so a touch only clears MAXPRESSURE near one
  // edge of the panel (the "only the top quarter works" symptom). Force 10-bit so
  // the library's assumptions hold.
  analogReadResolution(10);
  // (ESP32 Arduino already defaults the ADC to the full ~0-3.3V input range.)
}

void handleTouch() {
  TSPoint p = ts.getPoint();
  // getPoint() leaves the shared X-/Y+ pins as inputs; nothing else uses them,
  // but restore a defined state anyway.
  pinMode(TS_XM, OUTPUT);
  pinMode(TS_YP, OUTPUT);

#if TOUCH_DEBUG
  if (p.z > 0) {
    Serial.printf("touch raw  x=%4d  y=%4d  z=%4d\n", p.x, p.y, p.z);
  }
#endif

  if (p.z < MINPRESSURE || p.z > MAXPRESSURE) return;

  uint32_t now = millis();
  if (now - lastTouchMillis < TOUCH_DEBOUNCE_MS) return;
  lastTouchMillis = now;

  Serial.println("Touch detected, sending blink");
  publishBlink();
  startBlinkAnim(BLINK_SENT);
}

// ---- LED animation ----

uint32_t scaleColor(uint8_t r, uint8_t g, uint8_t b, uint8_t w, float scale) {
  scale = constrain(scale, 0.0f, 1.0f);
  return pixels.Color((uint8_t)(r * scale), (uint8_t)(g * scale), (uint8_t)(b * scale), (uint8_t)(w * scale));
}

// 0-255 hue around the color wheel (RGB only), scaled by `scale`.
uint32_t wheelColor(uint8_t pos, float scale) {
  pos = 255 - pos;
  if (pos < 85)  return scaleColor(255 - pos * 3, 0, pos * 3, 0, scale);
  if (pos < 170) { pos -= 85;  return scaleColor(0, pos * 3, 255 - pos * 3, 0, scale); }
  pos -= 170;    return scaleColor(pos * 3, 255 - pos * 3, 0, 0, scale);
}

void renderIdleTimeRing() {
  bool haveStatus = otherStatusEverReceived &&
                     (millis() - lastOtherStatusMillis < OTHER_STATUS_STALE_MS);

  if (!haveStatus) {
    // No (fresh) data from the other lamp yet: gentle neutral breathing white.
    float breath = 0.5f + 0.5f * sinf(millis() / 1200.0f);
    uint32_t c = scaleColor(200, 200, 200, 80, 0.15f + 0.15f * breath);
    for (int i = 0; i < NUM_PIXELS; i++) pixels.setPixelColor(i, c);
    pixels.show();
    return;
  }

  uint8_t sunR = 255, sunG = 130, sunB = 0, sunW = 60;
  uint8_t moonR = 40, moonG = 60, moonB = 120, moonW = 140;
  uint8_t ambR = otherIsDaytime ? 40 : 0;
  uint8_t ambG = otherIsDaytime ? 20 : 0;
  uint8_t ambB = otherIsDaytime ? 0  : 30;
  uint8_t ambW = otherIsDaytime ? 4  : 6;

  float breath = 0.85f + 0.15f * sinf(millis() / 1500.0f);

  int hourPixel = ((int)otherHour + HOUR_LED_OFFSET) % NUM_PIXELS;
  if (hourPixel < 0) hourPixel += NUM_PIXELS;

  for (int i = 0; i < NUM_PIXELS; i++) {
    int dist = abs(i - hourPixel);
    dist = min(dist, NUM_PIXELS - dist); // wrap around the ring
    float falloff = dist == 0 ? 1.0f : (dist == 1 ? 0.5f : (dist == 2 ? 0.2f : 0.0f));

    uint32_t color;
    if (falloff > 0.0f) {
      float scale = falloff * (dist == 0 ? breath : 1.0f);
      color = otherIsDaytime ? scaleColor(sunR, sunG, sunB, sunW, scale)
                              : scaleColor(moonR, moonG, moonB, moonW, scale);
    } else {
      color = scaleColor(ambR, ambG, ambB, ambW, 0.3f);
    }
    pixels.setPixelColor(i, color);
  }
  pixels.show();
}

// ---- Holiday blink animations ----
// Each takes the animation's elapsed ms and a 1->0 fade envelope. All are
// single-pass and non-blocking; called once per loop while a blink is active.

static void blinkAnniversary(uint32_t elapsed, float fade) {
  // Warm rose-gold breathing with two gold sparkles orbiting the ring.
  float breath = 0.4f + 0.6f * (0.5f + 0.5f * sinf(elapsed / 350.0f));
  int sparkle = (elapsed / 130) % NUM_PIXELS;
  for (int i = 0; i < NUM_PIXELS; i++) {
    uint32_t c = scaleColor(255, 120, 70, 90, fade * breath * 0.7f);
    if (i == sparkle || i == (sparkle + NUM_PIXELS / 2) % NUM_PIXELS)
      c = scaleColor(255, 215, 130, 160, fade);
    pixels.setPixelColor(i, c);
  }
}

static void blinkNewYear(uint32_t elapsed, float fade) {
  // Rotating rainbow with a champagne twinkle.
  int spin = elapsed / 20;
  for (int i = 0; i < NUM_PIXELS; i++) {
    uint8_t hue = (uint8_t)((i * 256 / NUM_PIXELS) + spin);
    bool twinkle = ((i * 7 + elapsed / 50) % 5) != 0;
    pixels.setPixelColor(i, twinkle ? wheelColor(hue, fade) : 0);
  }
}

static void blinkValentines(uint32_t elapsed, float fade) {
  // Pulsing pink/red with a brighter pair chasing around the ring.
  float pulse = 0.35f + 0.65f * (0.5f + 0.5f * sinf(elapsed / 180.0f));
  int head = (elapsed / 70) % NUM_PIXELS;
  for (int i = 0; i < NUM_PIXELS; i++) {
    int d = abs(i - head);
    d = min(d, NUM_PIXELS - d);
    float s = d <= 1 ? 1.0f : 0.4f;
    pixels.setPixelColor(i, scaleColor(255, 20, 80, 10, fade * pulse * s));
  }
}

static void blinkStPatricks(uint32_t elapsed, float fade) {
  // Green comet with a fading tail.
  int head = (elapsed / 45) % NUM_PIXELS;
  for (int i = 0; i < NUM_PIXELS; i++) {
    int d = (head - i + NUM_PIXELS) % NUM_PIXELS;
    float s = d < 6 ? (1.0f - d / 6.0f) : 0.08f;
    pixels.setPixelColor(i, scaleColor(0, 255, 40, 0, fade * s));
  }
}

static void blinkJuly4(uint32_t elapsed, float fade) {
  // Red / white / blue bands that rotate, with occasional full white sparkle.
  int phase = (elapsed / 250) % 3;
  bool flash = (elapsed / 120) % 7 == 0;
  for (int i = 0; i < NUM_PIXELS; i++) {
    int band = (i * 3 / NUM_PIXELS + phase) % 3;
    uint32_t c = band == 0 ? scaleColor(255, 0, 0, 0, fade)
               : band == 1 ? scaleColor(0, 0, 0, 180, fade)
                           : scaleColor(0, 40, 255, 0, fade);
    if (flash) c = scaleColor(120, 120, 160, 200, fade);
    pixels.setPixelColor(i, c);
  }
}

static void blinkHalloween(uint32_t elapsed, float fade) {
  // Flickering orange with a purple wisp drifting around.
  float flicker = 0.5f + 0.5f * sinf(elapsed / 40.0f) * sinf(elapsed / 13.0f);
  int ghost = (elapsed / 100) % NUM_PIXELS;
  for (int i = 0; i < NUM_PIXELS; i++) {
    uint32_t c = scaleColor(255, 90, 0, 0, fade * (0.3f + 0.7f * flicker));
    if (i == ghost) c = scaleColor(150, 0, 200, 0, fade);
    pixels.setPixelColor(i, c);
  }
}

static void blinkThanksgiving(uint32_t elapsed, float fade) {
  // Slow warm autumn shimmer (amber/orange/red).
  for (int i = 0; i < NUM_PIXELS; i++) {
    float ph = sinf((i * 0.5f) + elapsed / 300.0f);
    int r = constrain(200 + (int)(55 * ph), 0, 255);
    int g = constrain(70 + (int)(40 * ph), 0, 255);
    pixels.setPixelColor(i, scaleColor(r, g, 0, 20, fade * (0.6f + 0.4f * (0.5f + 0.5f * ph))));
  }
}

static void blinkChristmas(uint32_t elapsed, float fade) {
  // Alternating red/green candy-cane stripes rotating around the ring.
  int spin = elapsed / 90;
  for (int i = 0; i < NUM_PIXELS; i++) {
    bool red = (((i + spin) / 2) % 2) == 0;
    pixels.setPixelColor(i, red ? scaleColor(255, 0, 0, 0, fade)
                                : scaleColor(0, 255, 0, 0, fade));
  }
}

void renderBlinkAnim() {
  uint32_t dur = (blinkEvent == CAL_NONE) ? BLINK_ANIM_DURATION_MS
                                          : HOLIDAY_BLINK_DURATION_MS;
  uint32_t elapsed = millis() - blinkAnimStartMillis;
  if (elapsed >= dur) {
    blinkAnim = BLINK_NONE;
    blinkEvent = CAL_NONE;
    return;
  }

  float fade = 1.0f - (float)elapsed / (float)dur;

  switch (blinkEvent) {
    case CAL_ANNIVERSARY:  blinkAnniversary(elapsed, fade); pixels.show(); return;
    case CAL_NEW_YEAR:     blinkNewYear(elapsed, fade);     pixels.show(); return;
    case CAL_VALENTINES:   blinkValentines(elapsed, fade);  pixels.show(); return;
    case CAL_ST_PATRICKS:  blinkStPatricks(elapsed, fade);  pixels.show(); return;
    case CAL_JULY_4:       blinkJuly4(elapsed, fade);       pixels.show(); return;
    case CAL_HALLOWEEN:    blinkHalloween(elapsed, fade);   pixels.show(); return;
    case CAL_THANKSGIVING: blinkThanksgiving(elapsed, fade);pixels.show(); return;
    case CAL_CHRISTMAS:    blinkChristmas(elapsed, fade);   pixels.show(); return;
    case CAL_NONE:         break;
  }

  // Everyday flash: warm gold when sent, pink when received.
  uint32_t color = (blinkAnim == BLINK_SENT) ? scaleColor(255, 200, 60, 120, fade)
                                             : scaleColor(255, 30, 90, 20, fade);
  for (int i = 0; i < NUM_PIXELS; i++) pixels.setPixelColor(i, color);
  pixels.show();
}

void updateLEDAnimation() {
  if (blinkAnim != BLINK_NONE) {
    renderBlinkAnim();
  } else {
    renderIdleTimeRing();
  }
}

// ---- Setup / loop ----

void setup() {
  Serial.begin(115200);

  pixels.begin();
  pixels.setBrightness(DEFAULT_BRIGHTNESS);
  pixels.clear();
  pixels.show();

  setupDisplay();
  setupTouch();

  cmdTopicSelf = String("cloudlamp/") + LAMP_ID + "/command";
  statusTopicSelf = String("cloudlamp/") + LAMP_ID + "/status";
  statusTopicOther = String("cloudlamp/") + OTHER_LAMP_ID + "/status";

  setupWiFi();
  setupTime();

  // MVP: skip TLS cert validation against HiveMQ Cloud's root CA.
  // Hardening TODO: espClient.setCACert(<HiveMQ/ISRG root CA PEM>) instead.
  espClient.setInsecure();
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
}

void loop() {
  maintainWiFi();
  maintainMQTT();
  publishStatusIfDue();
  handleTouch();
  updateLEDAnimation();
  updateDisplayIfNeeded();
}
