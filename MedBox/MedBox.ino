#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <RTClib.h>
#include <DFRobotDFPlayerMini.h>
#include <Preferences.h>
#include <FirebaseESP32.h>
#include <addons/TokenHelper.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdio.h>

#include "secrets.h"

#define FW_VERSION "1.0.0"
#define DB_PATH "/medbox/box1"
#define NEPAL_OFFSET_S 20700UL
#define MAX_DOSES 32
#define OUTBOX_MAX 24
#define DEBOUNCE_MS 40UL
#define HEARTBEAT_MS 30000UL
#define SCHEDULE_PULL_MS 30000UL
#define SETTINGS_PULL_MS 60000UL
#define COMMAND_PULL_MS 15000UL
#define DF_RETRY_MS 30000UL
#define TRACK_REMINDER 1
#define TRACK_THANKS 2

#define I2C_SDA 21
#define I2C_SCL 22
#define DF_RX 16
#define DF_TX 17
#define BTN_RED 32
#define BTN_GREEN 33
#define LED_RED 25
#define LED_GREEN 26

const uint32_t MIN_VALID_EPOCH = 1700000000UL;

struct Dose {
  char name[25];
  uint8_t h;
  uint8_t m;
  bool on;
  uint32_t until;
  uint16_t track;
};

struct Pending {
  uint8_t kind;
  uint8_t status;
  uint32_t epoch;
  char name[25];
};

struct Button {
  uint8_t pin;
  bool raw;
  bool stable;
  bool pressedEvent;
  bool longFired;
  unsigned long lastChange;
  unsigned long pressAt;
};

enum BoxState { IDLE, ALERT };
enum PendingKind { PENDING_LOG, PENDING_EVENT };
enum LogStatus { LOG_TAKEN, LOG_MISSED };
enum EventType { EVENT_START, EVENT_ACK, EVENT_SNOOZE, EVENT_RTC_SET, EVENT_ERROR };

RTC_DS3231 rtc;
Preferences prefs;
HardwareSerial dfSerial(1);
DFRobotDFPlayerMini df;
FirebaseData fbGet;
FirebaseData fbSend;
FirebaseAuth auth;
FirebaseConfig config;

Dose doses[MAX_DOSES];
int doseCount = 0;
String packedSched = "-";

Button redBtn = {BTN_RED, false, false, false, false, 0, 0};
Button greenBtn = {BTN_GREEN, false, false, false, false, 0, 0};

BoxState state = IDLE;
int cur = -1;
bool rtcOk = false;
bool dfReady = false;
bool cloudReady = false;
bool statusPending = true;
bool forceSync = false;
bool resumeChecked = false;

int repeatMin = 3;
int snoozeMin = 5;
int missedMin = 30;
int holdSec = 5;
int volume = 25;

uint32_t currentUtc = 0;
uint32_t alertStartEpoch = 0;
uint32_t deadlineEpoch = 0;
uint32_t lastPlayEpoch = 0;
uint32_t snoozeUntilEpoch = 0;
uint32_t nextPlayEpoch = 0;
uint32_t beat = 0;
uint32_t lastRtcCommand = 0;

unsigned long alertStartMs = 0;
unsigned long confirmFlashUntil = 0;
unsigned long lastClockRead = 0;
unsigned long lastBeatMs = 0;
unsigned long lastSchedulePull = 0;
unsigned long lastSettingsPull = 0;
unsigned long lastCommandPull = 0;
unsigned long lastFlushMs = 0;
unsigned long lastDfTry = 0;
unsigned long lastDfPoll = 0;
unsigned long lastWifiTry = 0;
unsigned long lastNtpTry = 0;

Pending outbox[OUTBOX_MAX];
uint8_t outboxCount = 0;

char serialLine[120];
uint8_t serialLen = 0;

uint32_t fnv1a(const char *s) {
  uint32_t h = 2166136261UL;
  while (*s) {
    h ^= (uint8_t)*s++;
    h *= 16777619UL;
  }
  return h;
}

uint32_t doseKey(const Dose &d) {
  char buf[40];
  snprintf(buf, sizeof(buf), "%s|%02u:%02u", d.name, d.h, d.m);
  return fnv1a(buf);
}

void keyForDose(const Dose &d, char *key, size_t len) {
  snprintf(key, len, "lf%08lx", (unsigned long)doseKey(d));
}

void setSystemTime(uint32_t epoch) {
  timeval tv;
  tv.tv_sec = (time_t)epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

uint32_t readUtc() {
  if (!rtcOk) return 0;
  DateTime now = rtc.now();
  uint32_t e = now.unixtime();
  if (e < MIN_VALID_EPOCH) return 0;
  return e;
}

void localTm(uint32_t utc, struct tm &t) {
  time_t shifted = (time_t)(utc + NEPAL_OFFSET_S);
  gmtime_r(&shifted, &t);
}

uint32_t dayKeyFromTm(const struct tm &t) {
  return (uint32_t)(t.tm_year + 1900) * 10000UL + (uint32_t)(t.tm_mon + 1) * 100UL + (uint32_t)t.tm_mday;
}

void formatDate(uint32_t utc, char *out, size_t len) {
  struct tm t;
  localTm(utc, t);
  snprintf(out, len, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}

void formatHm(uint32_t utc, char *out, size_t len) {
  struct tm t;
  localTm(utc, t);
  snprintf(out, len, "%02d:%02d", t.tm_hour, t.tm_min);
}

long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (long)era * 146097L + (long)doe - 719468L;
}

bool parseNepalTime(const char *s, uint32_t &epoch) {
  int y, mo, d, h, mi, se;
  if (sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return false;
  if (y < 2024 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
  if (h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 59) return false;
  long days = daysFromCivil(y, (unsigned)mo, (unsigned)d);
  long sec = days * 86400L + h * 3600L + mi * 60L + se - (long)NEPAL_OFFSET_S;
  if (sec < (long)MIN_VALID_EPOCH) return false;
  epoch = (uint32_t)sec;
  return true;
}

bool parseDoseItem(String item, Dose &d) {
  item.trim();
  if (item.length() < 9) return false;
  int c1 = item.indexOf(',');
  if (c1 != 5) return false;
  String tm = item.substring(0, 5);
  if (tm.charAt(2) != ':') return false;
  int hh = tm.substring(0, 2).toInt();
  int mm = tm.substring(3, 5).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;

  int c2 = item.indexOf(',', c1 + 1);
  if (c2 < 0) return false;
  String name = item.substring(c1 + 1, c2);
  name.trim();
  if (name.length() == 0) return false;
  name.replace(",", " ");
  name.replace(";", " ");
  if (name.length() > 24) name = name.substring(0, 24);

  int c3 = item.indexOf(',', c2 + 1);
  int c4 = (c3 >= 0) ? item.indexOf(',', c3 + 1) : -1;

  String onText = (c3 >= 0) ? item.substring(c2 + 1, c3) : item.substring(c2 + 1);
  String untilText = (c3 >= 0) ? item.substring(c3 + 1, (c4 >= 0) ? c4 : item.length()) : "0";
  String trackText = (c4 >= 0) ? item.substring(c4 + 1) : "1";

  d.h = (uint8_t)hh;
  d.m = (uint8_t)mm;
  d.on = onText.toInt() == 1;
  d.until = (uint32_t)untilText.toInt();
  int tr = trackText.toInt();
  if (tr < 1 || tr > 2999) tr = TRACK_REMINDER;
  d.track = (uint16_t)tr;
  strncpy(d.name, name.c_str(), sizeof(d.name) - 1);
  d.name[sizeof(d.name) - 1] = 0;
  return true;
}

void parseSchedule(const String &s) {
  doseCount = 0;
  if (s.length() == 0 || s == "-") return;
  int start = 0;
  while (start < (int)s.length() && doseCount < MAX_DOSES) {
    int end = s.indexOf(';', start);
    if (end < 0) end = s.length();
    String item = s.substring(start, end);
    Dose d;
    if (parseDoseItem(item, d)) doses[doseCount++] = d;
    start = end + 1;
  }
  Serial.printf("Schedule loaded: %d dose(s)\n", doseCount);
}

String buildPacked() {
  String out;
  for (int i = 0; i < doseCount; i++) {
    const Dose &d = doses[i];
    char buf[80];
    snprintf(buf, sizeof(buf), "%02u:%02u,%s,%d,%lu,%u",
             d.h, d.m, d.name, d.on ? 1 : 0,
             (unsigned long)d.until, d.track);
    if (out.length()) out += ';';
    out += buf;
  }
  if (out.length() == 0) out = "-";
  return out;
}

void saveOutbox() {
  if (outboxCount == 0) {
    prefs.remove("outbox");
    return;
  }
  prefs.putBytes("outbox", outbox, (size_t)outboxCount * sizeof(Pending));
}

void loadOutbox() {
  outboxCount = 0;
  size_t n = prefs.getBytes("outbox", outbox, sizeof(outbox));
  if (n >= sizeof(Pending)) {
    outboxCount = (uint8_t)(n / sizeof(Pending));
    if (outboxCount > OUTBOX_MAX) outboxCount = OUTBOX_MAX;
  }
}

void enqueue(uint8_t kind, uint8_t status, const char *name, uint32_t epoch) {
  if (epoch < MIN_VALID_EPOCH) return;
  if (outboxCount >= OUTBOX_MAX) {
    memmove(&outbox[0], &outbox[1], sizeof(Pending) * (OUTBOX_MAX - 1));
    outboxCount = OUTBOX_MAX - 1;
  }
  Pending &p = outbox[outboxCount++];
  memset(&p, 0, sizeof(p));
  p.kind = kind;
  p.status = status;
  p.epoch = epoch;
  if (name) {
    strncpy(p.name, name, sizeof(p.name) - 1);
    p.name[sizeof(p.name) - 1] = 0;
  }
  saveOutbox();
}

void queueLog(uint8_t status, const char *name, uint32_t epoch) {
  enqueue(PENDING_LOG, status, name, epoch);
}

void queueEvent(uint8_t type, const char *name, uint32_t epoch) {
  enqueue(PENDING_EVENT, type, name, epoch);
}

const char *logStatusName(uint8_t s) {
  return s == LOG_TAKEN ? "taken" : "missed";
}

const char *eventTypeName(uint8_t t) {
  switch (t) {
    case EVENT_START: return "reminder_started";
    case EVENT_ACK: return "ack";
    case EVENT_SNOOZE: return "snooze";
    case EVENT_RTC_SET: return "rtc_set";
    default: return "error";
  }
}

void saveSettings() {
  prefs.putInt("rep", repeatMin);
  prefs.putInt("snz", snoozeMin);
  prefs.putInt("msd", missedMin);
  prefs.putInt("hld", holdSec);
  prefs.putInt("vol", volume);
}

void loadSettings() {
  repeatMin = prefs.getInt("rep", 3);
  snoozeMin = prefs.getInt("snz", 5);
  missedMin = prefs.getInt("msd", 30);
  holdSec = prefs.getInt("hld", 5);
  volume = prefs.getInt("vol", 25);
  if (repeatMin < 1 || repeatMin > 30) repeatMin = 3;
  if (snoozeMin < 1 || snoozeMin > 60) snoozeMin = 5;
  if (missedMin < 5 || missedMin > 180) missedMin = 30;
  if (holdSec < 3 || holdSec > 10) holdSec = 5;
  if (volume < 0 || volume > 30) volume = 25;
}

void saveActive() {
  prefs.putUInt("aKey", (uint32_t)doseKey(doses[cur]));
  prefs.putUInt("aStart", alertStartEpoch);
  prefs.putUInt("aDeadline", deadlineEpoch);
  prefs.putUInt("aLastPlay", lastPlayEpoch);
  prefs.putUInt("aSnooze", snoozeUntilEpoch);
}

void clearActive() {
  prefs.remove("aKey");
  prefs.remove("aStart");
  prefs.remove("aDeadline");
  prefs.remove("aLastPlay");
  prefs.remove("aSnooze");
}

void endAlert() {
  state = IDLE;
  cur = -1;
  snoozeUntilEpoch = 0;
  statusPending = true;
  clearActive();
}

void playReminder() {
  if (!dfReady) return;
  df.playMp3Folder(TRACK_REMINDER);
}

void startAlert(int index, uint32_t utc) {
  state = ALERT;
  cur = index;
  alertStartEpoch = utc;
  deadlineEpoch = utc + (uint32_t)missedMin * 60UL;
  lastPlayEpoch = 0;
  snoozeUntilEpoch = 0;
  nextPlayEpoch = utc;
  alertStartMs = millis();
  statusPending = true;

  char key[12];
  keyForDose(doses[index], key, sizeof(key));
  prefs.putUInt(key, utc / 60UL);

  saveActive();
  queueEvent(EVENT_START, doses[index].name, utc);
  playReminder();
  lastPlayEpoch = utc;
  nextPlayEpoch = utc + (uint32_t)repeatMin * 60UL;
  saveActive();

  Serial.printf("ALERT: %s\n", doses[index].name);
}

void markMissed() {
  if (cur < 0) {
    endAlert();
    return;
  }
  uint32_t when = deadlineEpoch ? deadlineEpoch : currentUtc;
  queueLog(LOG_MISSED, doses[cur].name, when);
  queueEvent(EVENT_ERROR, "missed", when);
  Serial.printf("MISSED: %s\n", doses[cur].name);
  endAlert();
}

void confirmTaken() {
  uint32_t when = currentUtc ? currentUtc : readUtc();
  if (cur >= 0 && when >= MIN_VALID_EPOCH) {
    queueLog(LOG_TAKEN, doses[cur].name, when);
    queueEvent(EVENT_ACK, doses[cur].name, when);
  }
  if (dfReady) {
    df.stop();
    df.playMp3Folder(TRACK_THANKS);
  }
  confirmFlashUntil = millis() + 600UL;
  Serial.println("TAKEN");
  endAlert();
}

void checkSchedule(uint32_t utc) {
  if (!rtcOk || utc < MIN_VALID_EPOCH || state != IDLE) return;
  struct tm t;
  localTm(utc, t);
  uint32_t day = dayKeyFromTm(t);
  uint32_t minute = utc / 60UL;

  for (int i = 0; i < doseCount; i++) {
    const Dose &d = doses[i];
    if (!d.on) continue;
    if (d.until && day > d.until) continue;
    if (d.h != t.tm_hour || d.m != t.tm_min) continue;

    char key[12];
    keyForDose(d, key, sizeof(key));
    if (prefs.getUInt(key, 0) == minute) continue;

    startAlert(i, utc);
    return;
  }
}

void runAlert(uint32_t utc, unsigned long ms) {
  if (utc < MIN_VALID_EPOCH) return;

  if (utc >= deadlineEpoch) {
    markMissed();
    return;
  }

  if (snoozeUntilEpoch && utc >= snoozeUntilEpoch) {
    snoozeUntilEpoch = 0;
    nextPlayEpoch = utc;
    saveActive();
  }

  if (utc >= nextPlayEpoch) {
    playReminder();
    lastPlayEpoch = utc;
    nextPlayEpoch = utc + (uint32_t)repeatMin * 60UL;
    saveActive();
  }

  if (redBtn.pressedEvent) {
    redBtn.pressedEvent = false;
    if (snoozeUntilEpoch == 0) {
      if (dfReady) df.stop();
      snoozeUntilEpoch = utc + (uint32_t)snoozeMin * 60UL;
      nextPlayEpoch = snoozeUntilEpoch;
      saveActive();
      queueEvent(EVENT_SNOOZE, doses[cur].name, utc);
      statusPending = true;
      Serial.println("SNOOZE");
    }
  }

  if (greenBtn.stable && !greenBtn.longFired && state == ALERT) {
    if (ms - greenBtn.pressAt >= (unsigned long)holdSec * 1000UL) {
      greenBtn.longFired = true;
      confirmTaken();
    }
  }
}

void resumeActive(uint32_t utc) {
  if (!rtcOk || utc < MIN_VALID_EPOCH) return;
  uint32_t key = prefs.getUInt("aKey", 0);
  if (key == 0) return;

  int found = -1;
  for (int i = 0; i < doseCount; i++) {
    if (doseKey(doses[i]) == key) {
      found = i;
      break;
    }
  }
  if (found < 0) {
    clearActive();
    return;
  }

  uint32_t deadline = prefs.getUInt("aDeadline", 0);
  if (deadline == 0 || utc >= deadline) {
    queueLog(LOG_MISSED, doses[found].name, deadline ? deadline : utc);
    Serial.printf("MISSED (resume): %s\n", doses[found].name);
    clearActive();
    return;
  }

  state = ALERT;
  cur = found;
  alertStartEpoch = prefs.getUInt("aStart", utc);
  deadlineEpoch = deadline;
  lastPlayEpoch = prefs.getUInt("aLastPlay", 0);
  snoozeUntilEpoch = prefs.getUInt("aSnooze", 0);
  alertStartMs = millis();
  if (snoozeUntilEpoch) nextPlayEpoch = snoozeUntilEpoch;
  else if (lastPlayEpoch) nextPlayEpoch = lastPlayEpoch + (uint32_t)repeatMin * 60UL;
  else nextPlayEpoch = utc;
  Serial.printf("RESUME ALERT: %s\n", doses[found].name);
}

void updateButton(Button &b) {
  bool now = digitalRead(b.pin) == LOW;
  unsigned long ms = millis();
  if (now != b.raw) {
    b.raw = now;
    b.lastChange = ms;
  }
  if (ms - b.lastChange >= DEBOUNCE_MS && b.raw != b.stable) {
    b.stable = b.raw;
    if (b.stable) {
      b.pressAt = ms;
      b.longFired = false;
      b.pressedEvent = true;
    }
  }
}

void updateLeds() {
  unsigned long ms = millis();
  bool red = false;
  bool green = false;

  if (!rtcOk) {
    red = ((ms / 250UL) % 2UL) == 0;
  } else if (state == ALERT) {
    red = ((ms / 500UL) % 2UL) == 0;
    if (greenBtn.stable && !greenBtn.longFired) {
      green = ((ms / 100UL) % 2UL) == 0;
    }
  } else if (ms < confirmFlashUntil) {
    green = (((confirmFlashUntil - ms) / 150UL) % 2UL) == 0;
  } else {
    green = true;
  }

  digitalWrite(LED_RED, red ? HIGH : LOW);
  digitalWrite(LED_GREEN, green ? HIGH : LOW);
}

void handleSerialLine(char *line) {
  String raw(line);
  raw.trim();
  if (raw.length() == 0) return;
  String upper = raw;
  upper.toUpperCase();

  if (upper.startsWith("SETTIME ")) {
    uint32_t epoch = 0;
    if (parseNepalTime(raw.substring(8).c_str(), epoch)) {
      rtc.adjust(DateTime(epoch));
      rtcOk = true;
      currentUtc = epoch;
      setSystemTime(epoch);
      resumeChecked = false;
      queueEvent(EVENT_RTC_SET, "serial", epoch);
      statusPending = true;
      Serial.printf("RTC set: %s Nepal\n", raw.substring(8).c_str());
    } else {
      Serial.println("ERR use SETTIME YYYY-MM-DD HH:MM:SS");
    }
    return;
  }

  if (upper.startsWith("SETUTCTIME ")) {
    uint32_t epoch = (uint32_t)strtoul(raw.substring(11).c_str(), nullptr, 10);
    if (epoch >= MIN_VALID_EPOCH) {
      rtc.adjust(DateTime(epoch));
      rtcOk = true;
      currentUtc = epoch;
      setSystemTime(epoch);
      resumeChecked = false;
      queueEvent(EVENT_RTC_SET, "serial", epoch);
      statusPending = true;
      Serial.println("RTC set from UTC epoch");
    } else {
      Serial.println("ERR invalid UTC epoch");
    }
    return;
  }

  if (upper == "SCHED LIST") {
    Serial.println(packedSched);
    return;
  }

  if (upper == "SCHED CLEAR") {
    doseCount = 0;
    packedSched = "-";
    prefs.putString("sched", packedSched);
    Serial.println("Schedule cleared");
    return;
  }

  if (upper.startsWith("SCHED ADD ")) {
    Dose d;
    if (!parseDoseItem(raw.substring(10), d)) {
      Serial.println("ERR use SCHED ADD HH:MM,Name,1,YYYYMMDD,1");
      return;
    }
    if (doseCount >= MAX_DOSES) {
      Serial.println("ERR schedule full");
      return;
    }
    doses[doseCount++] = d;
    packedSched = buildPacked();
    prefs.putString("sched", packedSched);
    Serial.println(packedSched);
    return;
  }

  if (upper == "STATUS") {
    char date[11];
    char hm[6];
    formatDate(currentUtc, date, sizeof(date));
    formatHm(currentUtc, hm, sizeof(hm));
    Serial.printf("state=%s rtc=%s df=%s utc=%lu nepal=%s %s doses=%d pending=%u\n",
                  state == ALERT ? "ALERT" : "IDLE",
                  rtcOk ? "ok" : "bad",
                  dfReady ? "ok" : "bad",
                  (unsigned long)currentUtc, date, hm, doseCount, outboxCount);
    if (cur >= 0) Serial.printf("current=%s\n", doses[cur].name);
    return;
  }

  if (upper == "HELP") {
    Serial.println("SETTIME YYYY-MM-DD HH:MM:SS | SETUTCTIME epoch");
    Serial.println("SCHED ADD HH:MM,Name,1,YYYYMMDD,1 | SCHED LIST | SCHED CLEAR | STATUS");
    return;
  }

  Serial.println("ERR unknown command, type HELP");
}

void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialLen > 0) {
        serialLine[serialLen] = 0;
        handleSerialLine(serialLine);
        serialLen = 0;
      }
    } else if (serialLen < sizeof(serialLine) - 1) {
      serialLine[serialLen++] = c;
    }
  }
}

bool sendStatus() {
  if (!cloudReady) return false;
  FirebaseJson json;
  json.set("beat", (int)(++beat));
  json.set("state", state == ALERT ? "alert" : "idle");
  json.set("dose", (state == ALERT && cur >= 0) ? doses[cur].name : "");
  json.set("rssi", WiFi.RSSI());
  String ip = WiFi.localIP().toString();
  json.set("ip", ip.c_str());
  json.set("rtcOk", rtcOk);
  json.set("rtcEpoch", (int)currentUtc);
  json.set("pending", outboxCount);
  json.set("dfOk", dfReady);
  json.set("fw", FW_VERSION);
  bool ok = Firebase.updateNode(fbSend, String(DB_PATH) + "/status", json);
  if (!ok) Serial.printf("status write failed: %s\n", fbSend.errorReason().c_str());
  return ok;
}

bool flushOutbox() {
  if (!cloudReady || outboxCount == 0) return false;
  Pending &p = outbox[0];
  char date[11];
  char hm[6];
  formatDate(p.epoch, date, sizeof(date));
  formatHm(p.epoch, hm, sizeof(hm));

  FirebaseJson json;
  json.set("name", p.name);
  json.set("date", date);
  json.set("time", hm);
  json.set("epoch", (int)p.epoch);
  json.set("source", "device");

  bool ok = false;
  if (p.kind == PENDING_LOG) {
    json.set("status", logStatusName(p.status));
    ok = Firebase.pushJSON(fbSend, String(DB_PATH) + "/logs", json);
  } else {
    json.set("type", eventTypeName(p.status));
    ok = Firebase.pushJSON(fbSend, String(DB_PATH) + "/events", json);
  }

  if (!ok) {
    Serial.printf("outbox write failed: %s\n", fbSend.errorReason().c_str());
    return false;
  }

  memmove(&outbox[0], &outbox[1], sizeof(Pending) * (outboxCount - 1));
  outboxCount--;
  saveOutbox();
  return true;
}

bool pullSchedule() {
  String path = String(DB_PATH) + "/schedule/packed";
  if (!Firebase.getString(fbGet, path)) {
    Serial.printf("schedule read failed: %s\n", fbGet.errorReason().c_str());
    return false;
  }
  String s = fbGet.stringData();
  if (s.length() == 0) s = "-";
  if (s == packedSched) return true;

  String old = packedSched;
  parseSchedule(s);
  if (doseCount == 0 && s != "-" && s.length() > 1) {
    packedSched = old;
    Serial.println("schedule rejected: no valid doses");
    return true;
  }
  packedSched = s;
  prefs.putString("sched", packedSched);
  Serial.println("Schedule updated from Firebase");
  return true;
}

bool pullSettings() {
  bool changed = false;
  int v = 0;
  String base = String(DB_PATH) + "/settings/";

  if (Firebase.getInt(fbGet, base + "repeat", &v) && v >= 1 && v <= 30 && v != repeatMin) {
    repeatMin = v;
    changed = true;
  }
  if (Firebase.getInt(fbGet, base + "snooze", &v) && v >= 1 && v <= 60 && v != snoozeMin) {
    snoozeMin = v;
    changed = true;
  }
  if (Firebase.getInt(fbGet, base + "missed", &v) && v >= 5 && v <= 180 && v != missedMin) {
    missedMin = v;
    changed = true;
  }
  if (Firebase.getInt(fbGet, base + "hold", &v) && v >= 3 && v <= 10 && v != holdSec) {
    holdSec = v;
    changed = true;
  }
  if (Firebase.getInt(fbGet, base + "volume", &v) && v >= 0 && v <= 30 && v != volume) {
    volume = v;
    changed = true;
  }

  if (changed) {
    saveSettings();
    if (dfReady) df.volume((uint8_t)volume);
    Serial.println("Settings updated from Firebase");
  }
  return true;
}

void applyRtcEpoch(uint32_t epoch) {
  if (epoch < MIN_VALID_EPOCH) return;
  rtc.adjust(DateTime(epoch));
  rtcOk = true;
  currentUtc = epoch;
  setSystemTime(epoch);
  resumeChecked = false;
  queueEvent(EVENT_RTC_SET, "dashboard", epoch);
  statusPending = true;
  Serial.println("RTC set from dashboard");
}

bool pullCommands() {
  int cmd = 0;
  String base = String(DB_PATH) + "/commands/";
  if (Firebase.getInt(fbGet, base + "setTime", &cmd)) {
    uint32_t epoch = (uint32_t)cmd;
    if (epoch >= MIN_VALID_EPOCH && epoch != lastRtcCommand) {
      lastRtcCommand = epoch;
      applyRtcEpoch(epoch);
      Firebase.setInt(fbSend, base + "setTime", 0);
    }
  }

  bool req = false;
  if (Firebase.getBool(fbGet, base + "requestSync", &req) && req) {
    forceSync = true;
    Firebase.setBool(fbSend, base + "requestSync", false);
  }
  return true;
}

void syncCloud(unsigned long ms) {
  if (forceSync || ms - lastSchedulePull >= SCHEDULE_PULL_MS) {
    if (pullSchedule()) lastSchedulePull = ms;
    forceSync = false;
  }
  if (ms - lastSettingsPull >= SETTINGS_PULL_MS) {
    if (pullSettings()) lastSettingsPull = ms;
  }
  if (ms - lastCommandPull >= COMMAND_PULL_MS) {
    if (pullCommands()) lastCommandPull = ms;
  }
}

void pollDf() {
  unsigned long ms = millis();
  if (ms - lastDfPoll < 100UL) return;
  lastDfPoll = ms;
  if (!dfReady) return;
  if (df.available()) {
    uint8_t type = df.readType();
    uint16_t value = df.read();
    if (type == DFPlayerError) {
      char msg[20];
      snprintf(msg, sizeof(msg), "df_error_%u", value);
      queueEvent(EVENT_ERROR, msg, currentUtc);
      Serial.printf("DFPlayer error: %u\n", value);
    }
  }
}

void ensureDf() {
  if (dfReady) return;
  if (millis() - lastDfTry < DF_RETRY_MS) return;
  lastDfTry = millis();
  if (df.begin(dfSerial)) {
    dfReady = true;
    df.volume((uint8_t)volume);
    Serial.println("DFPlayer ready");
  } else {
    Serial.println("DFPlayer not found");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN_RED, INPUT_PULLUP);
  pinMode(BTN_GREEN, INPUT_PULLUP);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_GREEN, HIGH);

  prefs.begin("medbox", false);
  loadSettings();
  loadOutbox();
  packedSched = prefs.getString("sched", "-");
  parseSchedule(packedSched);

  Wire.begin(I2C_SDA, I2C_SCL);
  if (rtc.begin()) {
    if (rtc.lostPower()) {
      rtcOk = false;
      Serial.println("RTC lost power; set time by serial or NTP");
    } else {
      uint32_t e = rtc.now().unixtime();
      rtcOk = e >= MIN_VALID_EPOCH;
      if (rtcOk) setSystemTime(e);
    }
  } else {
    rtcOk = false;
    Serial.println("RTC not found");
  }

  dfSerial.begin(9600, SERIAL_8N1, DF_RX, DF_TX);
  lastDfTry = millis();
  if (df.begin(dfSerial)) {
    dfReady = true;
    df.volume((uint8_t)volume);
    Serial.println("DFPlayer ready");
  } else {
    Serial.println("DFPlayer not found; retrying in background");
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  lastWifiTry = millis();
  lastNtpTry = millis();

  config.api_key = FIREBASE_API_KEY;
  config.database_url = FIREBASE_DATABASE_URL;
  auth.user.email = FIREBASE_USER_EMAIL;
  auth.user.password = FIREBASE_USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;
  config.timeout.networkReconnect = 15000;
  config.timeout.socketConnection = 10000;
  config.timeout.serverResponse = 10000;
  fbGet.setBSSLBufferSize(4096, 1024);
  fbSend.setBSSLBufferSize(4096, 1024);
  Firebase.reconnectNetwork(true);
  Firebase.begin(&config, &auth);
  Firebase.setDoubleDigits(5);

  currentUtc = rtcOk ? readUtc() : 0;
  if (rtcOk) {
    resumeActive(currentUtc);
    resumeChecked = true;
  }

  beat = esp_random() % 1000000UL;
  Serial.printf("\nMedBox %s booted. RTC=%s DF=%s\n", FW_VERSION, rtcOk ? "ok" : "bad", dfReady ? "ok" : "bad");
  Serial.println("Type HELP for serial commands.");
}

void loop() {
  unsigned long ms = millis();

  handleSerial();
  updateButton(redBtn);
  updateButton(greenBtn);

  if (WiFi.status() != WL_CONNECTED) {
    if (ms - lastWifiTry >= 15000UL) {
      lastWifiTry = ms;
      WiFi.reconnect();
    }
  } else if (ms - lastNtpTry >= 10000UL) {
    lastNtpTry = ms;
    time_t nt = time(nullptr);
    if (nt > (time_t)MIN_VALID_EPOCH && !rtcOk) {
      applyRtcEpoch((uint32_t)nt);
      Serial.println("RTC set from NTP");
    }
  }

  if (ms - lastClockRead >= 500UL) {
    lastClockRead = ms;
    uint32_t u = readUtc();
    if (u >= MIN_VALID_EPOCH) currentUtc = u;
    if (state == IDLE) {
      if (!resumeChecked && rtcOk) {
        resumeActive(currentUtc);
        resumeChecked = true;
      }
      checkSchedule(currentUtc);
    }
  }

  if (state == ALERT) runAlert(currentUtc, ms);

  ensureDf();
  pollDf();

  cloudReady = Firebase.ready();
  if (cloudReady) {
    if (state == ALERT) {
      if ((statusPending && ms - alertStartMs > 3000UL) || ms - lastBeatMs >= HEARTBEAT_MS) {
        if (sendStatus()) {
          statusPending = false;
          lastBeatMs = ms;
        }
      }
    } else {
      if (statusPending || ms - lastBeatMs >= HEARTBEAT_MS) {
        if (sendStatus()) {
          statusPending = false;
          lastBeatMs = ms;
        }
      }
      if (ms - lastFlushMs >= 2000UL) {
        lastFlushMs = ms;
        flushOutbox();
      }
      syncCloud(ms);
    }
  }

  if (state != ALERT) {
    redBtn.pressedEvent = false;
  }

  updateLeds();
  delay(5);
}
