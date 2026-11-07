# medi-remainder-with-esp32-and-firebase-web-dash

ESP32-based medicine reminder box with a Firebase Realtime Database backend and a web dashboard.

## Contents

- `MedBox/MedBox.ino` — ESP32 firmware
- `MedBox/secrets.h.example` — copy to `secrets.h` and fill in WiFi/Firebase credentials
- `esp_med.c++` — firmware variant
- `med.html` / `banjara.html` — Firebase web dashboards
- `firebase/database.rules.json` — Realtime Database security rules

## Setup

1. Copy `MedBox/secrets.h.example` to `MedBox/secrets.h` and fill in your credentials.
2. Deploy `firebase/database.rules.json` to your Firebase project.
3. Open `med.html` or `banjara.html` in a browser for the dashboard.
4. Flash the firmware to your ESP32 via Arduino IDE.

`secrets.h` is git-ignored and is never committed.
