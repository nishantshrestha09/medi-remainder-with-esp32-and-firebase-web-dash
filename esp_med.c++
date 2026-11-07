/*
  ===============================================================
  MEDBOX - MEDICINE REMINDER (Firebase schedule + offline timer)
  ===============================================================
  Same Firebase style as the Nepali Kansa agriculture node.

  - Dashboard writes  /medbox/box1/schedule/packed  ("09:00,Paracetamol,1;13:00,Paracetamol,1")
  - ESP32 downloads it every 30 s (only while idle), SAVES IT IN FLASH, and runs the
    timer itself, so reminders still work when WiFi / internet is down.
  - ESP32 writes /medbox/box1/status (heartbeat) and /medbox/box1/logs (taken / missed).

  Buttons:
    GREEN held 5 s  -> taken  (green LED double blink + buzzer, then "thank you" audio)
    RED pressed     -> not yet (audio stops, reminds again after 5 min)
    nothing pressed -> audio repeats every 3 min, marked MISSED after 30 min

  DFPlayer SD card (FAT32), folder "mp3":  0001.mp3 = reminder,  0002.mp3 = thank you
  Buzzer: ACTIVE buzzer (on when pin is HIGH).

  secrets.h must define: WIFI_SSID, WIFI_PASSWORD, FIREBASE_AUTH
  Libraries: Firebase ESP32 Client (Mobizt), DFRobotDFPlayerMini
  Pins below are examples for ESP32-S3 CAM free GPIOs. Check your board's pinout.
  ===============================================================
*/

#include <WiFi.h>
#include <FirebaseESP32.h>
#include <DFRobotDFPlayerMini.h>
#include <Preferences.h>
#include <time.h>
#include "secrets.h"

#define FIREBASE_HOST "nxt-date-default-rtdb.asia-southeast1.firebasedatabase.app/"
FirebaseData fbSend, fbGet;
FirebaseConfig config;
FirebaseAuth auth;
const String DB_PATH = "/medbox/box1";

// ---------------- PINS ----------------
#define BTN_GREEN 1
#define BTN_RED   2
#define LED_GREEN 41
#define LED_RED   42
#define BUZZER    14
#define DF_RX     47   // ESP32 RX <- DFPlayer TX
#define DF_TX     21   // ESP32 TX -> DFPlayer RX (through 1K resistor)

// ---------------- SETTINGS ----------------
#define TRACK_REMINDER  1
#define TRACK_THANKS    2
#define REPEAT_MS       180000UL    // repeat audio if no button pressed (3 min)
#define SNOOZE_MS       300000UL    // after RED button (5 min)
#define MISSED_MS       1800000UL   // give up after 30 min
#define HOLD_MS         5000UL      // green hold time
#define NEPAL_OFFSET_S  20700       // UTC+5:45
#define MAX_DOSES       12

struct Dose { char name[25]; uint8_t h, m; bool on; };
Dose doses[MAX_DOSES];
int doseCount = 0;
int lastStamp[MAX_DOSES];           // stops the same dose firing twice in one minute

Preferences prefs;
String packedSched = "-";
HardwareSerial dfSerial(1);
DFRobotDFPlayerMini df;

enum State { IDLE, ALERT };
State st = IDLE;
int cur = -1;
unsigned long alertStart = 0, nextPlay = 0;
unsigned long lastBeat = 0, lastSync = 0, lastCheck = 0, lastWifiTry = 0, lastNtp = 0;
uint32_t beat = 0;

// ---------------- HELPERS ----------------
bool timeValid() { return time(nullptr) > 1700000000; }

void beep(int ms) { digitalWrite(BUZZER, HIGH); delay(ms); digitalWrite(BUZZER, LOW); }

void parseSchedule(const String &s) {
  doseCount = 0;
  if (s.length() == 0 || s == "-") return;
  int start = 0;
  while (start < (int)s.length() && doseCount < MAX_DOSES) {
    int end = s.indexOf(';', start);
    if (end < 0) end = s.length();
    String item = s.substring(start, end);
    int c1 = item.indexOf(','), c2 = item.lastIndexOf(',');
    if (c1 == 5 && c2 > c1) {
      Dose &d = doses[doseCount];
      d.h = item.substring(0, 2).toInt();
      d.m = item.substring(3, 5).toInt();
      item.substring(c1 + 1, c2).toCharArray(d.name, sizeof(d.name));
      d.on = item.substring(c2 + 1).toInt() == 1;
      doseCount++;
    }
    start = end + 1;
  }
  Serial.printf("Schedule loaded: %d doses\n", doseCount);
}

// ---------------- FIREBASE ----------------
void syncSchedule() {
  if (Firebase.getString(fbGet, DB_PATH + "/schedule/packed")) {
    String s = fbGet.stringData();
    if (s.length() && s != packedSched) {
      packedSched = s;
      parseSchedule(s);
      prefs.putString("sched", s);          // survives power cuts
    }
  } else {
    Serial.println("Schedule read failed: " + fbGet.errorReason());
  }
}

void sendHeartbeat() {
  FirebaseJson json;
  json.set("beat", (int)(++beat));
  json.set("rssi", WiFi.RSSI());
  json.set("state", st == ALERT ? "alert" : "idle");
  json.set("dose", (st == ALERT && cur >= 0) ? doses[cur].name : "");
  Firebase.updateNode(fbSend, DB_PATH + "/status", json);
}

void logEvent(const char *status) {
  if (WiFi.status() != WL_CONNECTED || !timeValid() || cur < 0) return;
  struct tm t;
  if (!getLocalTime(&t, 0)) return;
  char d[11], tm[6];
  strftime(d, sizeof(d), "%Y-%m-%d", &t);
  strftime(tm, sizeof(tm), "%H:%M", &t);
  FirebaseJson json;
  json.set("name", doses[cur].name);
  json.set("date", d);
  json.set("time", tm);
  json.set("status", status);
  Firebase.pushJSON(fbSend, DB_PATH + "/logs", json);
}

// ---------------- REMINDER LOGIC ----------------
void endAlert() {
  st = IDLE; cur = -1;
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_GREEN, LOW);
}

void confirmTaken() {
  df.stop();
  for (int i = 0; i < 2; i++) {                 // double blink + buzzer
    digitalWrite(LED_GREEN, HIGH); digitalWrite(BUZZER, HIGH); delay(150);
    digitalWrite(LED_GREEN, LOW);  digitalWrite(BUZZER, LOW);  delay(150);
  }
  df.playMp3Folder(TRACK_THANKS);
  Serial.println("Taken");
  logEvent("taken");
  endAlert();
}

void checkSchedule() {
  if (st != IDLE || !timeValid()) return;
  struct tm t;
  if (!getLocalTime(&t, 0)) return;
  int stamp = t.tm_yday * 1440 + t.tm_hour * 60 + t.tm_min;
  for (int i = 0; i < doseCount; i++) {
    if (doses[i].on && doses[i].h == t.tm_hour && doses[i].m == t.tm_min && lastStamp[i] != stamp) {
      lastStamp[i] = stamp;
      cur = i; st = ALERT;
      alertStart = nextPlay = millis();
      Serial.printf("ALERT: %s\n", doses[i].name);
      return;
    }
  }
}

void runAlert() {
  unsigned long now = millis();
  digitalWrite(LED_RED, (now / 500) % 2);       // red LED blinks while waiting

  if (now >= nextPlay) { df.playMp3Folder(TRACK_REMINDER); nextPlay = now + REPEAT_MS; }

  if (now - alertStart >= MISSED_MS) {
    Serial.println("Missed");
    logEvent("missed");
    endAlert();
    return;
  }

  static bool redPrev = false;                  // RED: not yet
  bool red = !digitalRead(BTN_RED);
  if (red && !redPrev) { df.stop(); beep(100); nextPlay = now + SNOOZE_MS; Serial.println("Not yet"); }
  redPrev = red;

  static unsigned long gStart = 0;              // GREEN: hold to confirm
  if (!digitalRead(BTN_GREEN)) {
    if (!gStart) gStart = now;
    digitalWrite(LED_GREEN, (now / 100) % 2);   // fast blink = "keep holding"
    if (now - gStart >= HOLD_MS) { gStart = 0; confirmTaken(); }
  } else if (gStart) {
    gStart = 0; digitalWrite(LED_GREEN, LOW);
  }
}

// ---------------- SETUP / LOOP ----------------
void setup() {
  Serial.begin(115200);
  pinMode(BTN_GREEN, INPUT_PULLUP);
  pinMode(BTN_RED, INPUT_PULLUP);
  pinMode(LED_GREEN, OUTPUT); pinMode(LED_RED, OUTPUT); pinMode(BUZZER, OUTPUT);
  for (int i = 0; i < MAX_DOSES; i++) lastStamp[i] = -1;

  dfSerial.begin(9600, SERIAL_8N1, DF_RX, DF_TX);
  if (!df.begin(dfSerial)) Serial.println("DFPlayer not found (check wiring / SD card)");
  df.volume(25);

  prefs.begin("medbox", false);                 // load saved schedule first, works with no WiFi
  packedSched = prefs.getString("sched", "-");
  parseSchedule(packedSched);

  beat = esp_random() % 1000000;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {   // don't block forever
    digitalWrite(LED_GREEN, !digitalRead(LED_GREEN)); delay(250);
  }
  digitalWrite(LED_GREEN, LOW);

  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  configTime(NEPAL_OFFSET_S, 0, "pool.ntp.org", "time.google.com");
  beep(150);
  Serial.println("MedBox running.");
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiTry > 10000) { lastWifiTry = now; WiFi.reconnect(); }
  } else if (!timeValid() && now - lastNtp > 10000) {
    lastNtp = now;
    configTime(NEPAL_OFFSET_S, 0, "pool.ntp.org", "time.google.com");
  }

  if (now - lastCheck >= 1000) { lastCheck = now; checkSchedule(); }

  if (st == ALERT) runAlert();

  if (WiFi.status() == WL_CONNECTED) {
    if (now - lastBeat >= 5000) { lastBeat = now; sendHeartbeat(); }
    if (st == IDLE && now - lastSync >= 30000) { lastSync = now; syncSchedule(); }
  }

  delay(20);
}
