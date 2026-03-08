#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <time.h>

// =====================
// Configuration
// =====================
#define TX_PIN 5          // GPIO5
#define SYMBOL 640        // us

#define HAUT 0x2
#define STOP 0x1
#define BAS  0x4
#define PROG 0x8

#define EEPROM_ADDRESS 0
#define EEPROM_SIZE 1024
#define VERSION 5

// =====================
// Configuration OTA
// =====================
#define OTA_HOSTNAME  "somfy-rts"
#define OTA_PASSWORD  "somfy1234"

// =====================
// Configuration NTP
// =====================
#define NTP_SERVER  "pool.ntp.org"
#define TZ_INFO     "CET-1CEST,M3.5.0,M10.5.0/3"  // Europe/Paris

// =====================
// Configuration Solaire
// =====================
// Colomiers - 10 chemin d'en sigal
#define VILLE "Colomiers"
#define LATITUDE    43.607063348076395f // Latitude
#define LONGITUDE    1.3329200924239f   // Longitude

#ifndef DEG_TO_RAD
#define DEG_TO_RAD 0.017453293f
#endif
#ifndef RAD_TO_DEG
#define RAD_TO_DEG 57.295779513f
#endif

// =====================
// Configuration LEDs
// =====================
#define LED_POWER_PIN    4   // GPIO4 - LED fonctionnement (toujours allumée)
#define LED_UP_PIN       14  // GPIO14 - LED montée
#define LED_DOWN_PIN     12  // GPIO12 - LED descente

#define BLINK_DURATION   100 // ms
#define BLINK_INTERVAL   200 // ms
#define BLINK_COUNT      6

// =====================
// Configuration Volets/Groupes
// =====================
#define MAX_VOLETS   8
#define MAX_GROUPES  6
#define NAME_LEN    20   // 19 chars utiles + '\0'

// Log minimal (0 = muet)
#ifndef SOMFY_LOG
#define SOMFY_LOG 1
#endif

#if SOMFY_LOG
  #define LOG(x)        Serial.print(x)
  #define LOGLN(x)      Serial.println(x)
  #define LOGLN2(x,b)   Serial.println(x,b)
#else
  #define LOG(x)        do{}while(0)
  #define LOGLN(x)      do{}while(0)
  #define LOGLN2(x,b)   do{}while(0)
#endif

// Wi-Fi — valeurs par défaut (déclenchent le portail de config)
#define WIFI_SSID_DEFAULT "a renseigner"
#define WIFI_PASS_DEFAULT "a renseigner"

// #define USE_BASIC_AUTH 1
#if defined(USE_BASIC_AUTH)
static const char* HTTP_USER = "admin";
static const char* HTTP_PASS = "admin";
#endif

// =====================
// Gestion des LEDs
// =====================
struct LEDState {
  uint8_t pin;
  uint8_t blinkCount;
  uint32_t lastToggle;
  bool isBlinking;
  bool state;
};

LEDState ledPower = {LED_POWER_PIN, 0, 0, false, false};
LEDState ledUp    = {LED_UP_PIN,    0, 0, false, false};
LEDState ledDown  = {LED_DOWN_PIN,  0, 0, false, false};

static bool ledPowerEnabled = true;

static void initLEDs() {
  pinMode(LED_POWER_PIN, OUTPUT);
  pinMode(LED_UP_PIN, OUTPUT);
  pinMode(LED_DOWN_PIN, OUTPUT);

  if (ledPowerEnabled) {
    digitalWrite(LED_POWER_PIN, HIGH);
    ledPower.state = true;
  }
  digitalWrite(LED_UP_PIN, LOW);
  digitalWrite(LED_DOWN_PIN, LOW);
  LOGLN(F("LEDs initialisées"));
}

static void setLEDState(LEDState &led, bool state) {
  if (led.state != state) {
    digitalWrite(led.pin, state ? HIGH : LOW);
    led.state = state;
  }
}

static void startBlink(LEDState &led) {
  led.blinkCount = BLINK_COUNT * 2;
  led.isBlinking = true;
  led.lastToggle = millis();
  setLEDState(led, true);
}

static void updateLED(LEDState &led) {
  if (!led.isBlinking) return;

  uint32_t now = millis();
  if (now - led.lastToggle >= BLINK_INTERVAL) {
    led.lastToggle = now;
    setLEDState(led, !led.state);
    led.blinkCount--;

    if (led.blinkCount == 0) {
      led.isBlinking = false;
      setLEDState(led, false);
    }
  }
}

static void updateAllLEDs() {
  updateLED(ledUp);
  updateLED(ledDown);
}

// =====================
// Modèle multi-volets
// =====================
struct Remote {
  uint32_t remoteID;
  uint16_t rollingCode;
};

// =====================
// Scènes / Horaires
// =====================
#define NB_SCENES 8

struct Scene {
  uint8_t hour;          // 0-23 (ignoré si triggerType != 0)
  uint8_t minute;        // 0-59 (ignoré si triggerType != 0)
  uint8_t days;          // bitmask: bit0=Lun, bit1=Mar, ..., bit6=Dim (0x00 = désactivé)
  uint8_t targetType;    // 0 = volet, 1 = groupe
  uint8_t targetId;
  uint8_t cmd;           // HAUT / STOP / BAS
  uint8_t enabled;       // 0 ou 1
  uint8_t triggerType;   // 0=heure fixe, 1=lever soleil, 2=coucher soleil
  int8_t  offsetMinutes; // décalage ±127 min par rapport au lever/coucher
};

struct VoletConfig {
  char    name[NAME_LEN]; // 20 bytes — 19 chars + '\0'
  uint8_t remoteIndex;    //  1 byte  — index dans remotes[16]
  uint8_t enabled;        //  1 byte
};  // 22 bytes × 8 = 176 bytes

struct GroupeConfig {
  char     name[NAME_LEN]; // 20 bytes
  uint16_t members;        //  2 bytes — bitmask: bit i = volet slot i
  uint8_t  enabled;        //  1 byte
};  // 23 bytes × 6 = 138 bytes

struct Persist {
  int          appVersion;           //   4 bytes
  Remote       remotes[16];          //  96 bytes
  Scene        scenes[NB_SCENES];    //  72 bytes
  VoletConfig  volets[MAX_VOLETS];   // 176 bytes
  GroupeConfig groupes[MAX_GROUPES]; // 138 bytes
  char         wifiSSID[33];         //  33 bytes
  char         wifiPass[64];         //  64 bytes
  // Total : 583 bytes (< 1024)
};

// =====================
// État / Serveur
// =====================
Persist somfy;
ESP8266WebServer server(8090);

// Anti-double-déclenchement des scènes (timestamp en minutes)
static uint32_t lastFiredMinute[NB_SCENES];

// Cache lever/coucher du soleil (recalculé une fois par jour)
static uint8_t sunCacheDay = 0xFF;
static uint8_t sunRiseH = 6,  sunRiseM = 0;
static uint8_t sunSetH  = 20, sunSetM  = 0;

// =====================
// GPIO FAST (ESP8266)
// =====================
static uint32_t txMask = 0;
static bool txStateLow = true;

static inline void IRAM_ATTR txHigh() { GPOS = txMask; txStateLow = false; }
static inline void IRAM_ATTR txLow()  { GPOC = txMask; txStateLow = true; }
static inline void IRAM_ATTR txToggle(){ if (txStateLow) txHigh(); else txLow(); }

// =====================
// Somfy RTS
// =====================
static byte frame[7];

static void BuildFrame(uint32_t remoteID, uint16_t rollingCode, byte *frame, byte button) {
  frame[0] = 0xA7;
  frame[1] = (button << 4);
  frame[2] = (byte)(rollingCode >> 8);
  frame[3] = (byte)(rollingCode & 0xFF);
  frame[4] = (byte)(remoteID >> 16);
  frame[5] = (byte)(remoteID >> 8);
  frame[6] = (byte)(remoteID & 0xFF);

  byte checksum = 0;
  for (byte i = 0; i < 7; i++) checksum ^= frame[i] ^ (frame[i] >> 4);
  checksum &= 0x0F;
  frame[1] |= checksum;

  for (byte i = 1; i < 7; i++) frame[i] ^= frame[i - 1];
}

static void SendCommand(byte *frame, byte sync) {
  if (sync == 2) {
    txHigh(); delayMicroseconds(9415);
    txLow();  delayMicroseconds(89565);
  }

  for (int i = 0; i < sync; i++) {
    txHigh(); delayMicroseconds(4 * SYMBOL);
    txLow();  delayMicroseconds(4 * SYMBOL);
  }

  txHigh(); delayMicroseconds(4550);
  txLow();  delayMicroseconds(SYMBOL);

  for (byte i = 0; i < 56; i++) {
    bool bit = ((frame[i / 8] >> (7 - (i % 8))) & 1) != 0;
    if (bit) {
      txLow(); delayMicroseconds(SYMBOL);
      txToggle(); delayMicroseconds(SYMBOL);
    } else {
      txHigh(); delayMicroseconds(SYMBOL);
      txToggle(); delayMicroseconds(SYMBOL);
    }
  }

  txLow();
  delayMicroseconds(30415);
}

static void SendSomfy(uint8_t remoteIndex, byte button) {
  if (remoteIndex >= (sizeof(somfy.remotes) / sizeof(somfy.remotes[0]))) return;

  Remote &r = somfy.remotes[remoteIndex];
  BuildFrame(r.remoteID, r.rollingCode, frame, button);

  SendCommand(frame, 2);
  SendCommand(frame, 7);
  SendCommand(frame, 7);

  r.rollingCode++;
  EEPROM.put(EEPROM_ADDRESS, somfy);
  EEPROM.commit();
}

// =====================
// OTA
// =====================
static void initOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    LOGLN(F("OTA start"));
    setLEDState(ledPower, true);
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static uint8_t lastSlice = 0xFF;
    uint8_t slice = (uint8_t)(progress * 10 / total);
    if (slice != lastSlice) {
      lastSlice = slice;
      setLEDState(ledPower, !ledPower.state);
    }
  });
  ArduinoOTA.onEnd([]() {
    LOGLN(F("OTA end"));
    setLEDState(ledPower, true);
  });
  ArduinoOTA.onError([](ota_error_t e) {
    LOGLN(F("OTA error"));
    for (uint8_t i = 0; i < 6; i++) {
      setLEDState(ledPower, !ledPower.state);
      delay(100);
    }
    setLEDState(ledPower, ledPowerEnabled);
  });
  ArduinoOTA.begin();
  LOGLN(F("OTA ready"));
}

// =====================
// NTP
// =====================
static void initNTP() {
  configTzTime(TZ_INFO, NTP_SERVER);
  LOGLN(F("NTP init"));
}

// =====================
// Calcul solaire (algorithme NOAA simplifié, précision ±2 min)
// =====================
static void computeSunTimes(time_t utcNow,
                             uint8_t *riseH, uint8_t *riseM,
                             uint8_t *setH,  uint8_t *setM) {
  // Noon UTC of today, days since J2000.0 (2000-01-01T12:00Z = unix 946728000)
  struct tm tg;
  gmtime_r(&utcNow, &tg);
  time_t dayStart = utcNow - ((time_t)tg.tm_hour * 3600 + tg.tm_min * 60 + tg.tm_sec);
  float d = (float)(dayStart + 43200 - 946728000L) / 86400.0f;

  float g = fmodf(357.529f + 0.98560028f * d, 360.0f);  // anomalie moyenne
  float q = fmodf(280.459f + 0.98564736f * d, 360.0f);  // longitude moyenne
  float L = fmodf(q + 1.915f * sinf(g * DEG_TO_RAD)
                    + 0.020f * sinf(2.0f * g * DEG_TO_RAD), 360.0f);
  float e     = 23.439f - 3.6e-7f * d;                  // obliquité
  float sinL  = sinf(L * DEG_TO_RAD);
  float RA    = atan2f(cosf(e * DEG_TO_RAD) * sinL, cosf(L * DEG_TO_RAD)) * RAD_TO_DEG / 15.0f;
  if (RA < 0) RA += 24.0f;

  float sinDec = sinf(e * DEG_TO_RAD) * sinL;
  float dec    = asinf(sinDec);

  float EqT = q / 15.0f - RA;  // équation du temps (heures)
  if (EqT >  12.0f) EqT -= 24.0f;
  if (EqT < -12.0f) EqT += 24.0f;

  float noonUTC = 12.0f - EqT - LONGITUDE / 15.0f;

  float cosH = (sinf(-0.8333f * DEG_TO_RAD) - sinf(LATITUDE * DEG_TO_RAD) * sinDec)
               / (cosf(LATITUDE * DEG_TO_RAD) * cosf(dec));
  float H;
  if      (cosH <= -1.0f) H = 12.0f;  // soleil de minuit
  else if (cosH >=  1.0f) H =  0.0f;  // nuit polaire
  else                    H = acosf(cosH) * RAD_TO_DEG / 15.0f;

  float riseUTC = noonUTC - H;
  float setUTC  = noonUTC + H;

  // Conversion UTC → heure locale
  struct tm tloc, tgmt;
  localtime_r(&utcNow, &tloc);
  gmtime_r(&utcNow, &tgmt);
  float utcOffH = (float)(tloc.tm_hour - tgmt.tm_hour)
                + (float)(tloc.tm_min  - tgmt.tm_min) / 60.0f;
  if (utcOffH >  12.0f) utcOffH -= 24.0f;
  if (utcOffH < -12.0f) utcOffH += 24.0f;

  float riseLocal = riseUTC + utcOffH;
  float setLocal  = setUTC  + utcOffH;
  while (riseLocal <  0) riseLocal += 24.0f;
  while (riseLocal > 24) riseLocal -= 24.0f;
  while (setLocal  <  0) setLocal  += 24.0f;
  while (setLocal  > 24) setLocal  -= 24.0f;

  *riseH = (uint8_t)riseLocal;
  *riseM = (uint8_t)roundf((riseLocal - *riseH) * 60.0f);
  *setH  = (uint8_t)setLocal;
  *setM  = (uint8_t)roundf((setLocal  - *setH)  * 60.0f);
  if (*riseM >= 60) { (*riseH)++; *riseM -= 60; }
  if (*setM  >= 60) { (*setH)++;  *setM  -= 60; }
  *riseH %= 24; *setH %= 24;
}

static void refreshSunCache(struct tm* t, time_t utcNow) {
  if (sunCacheDay == (uint8_t)t->tm_mday) return;
  computeSunTimes(utcNow, &sunRiseH, &sunRiseM, &sunSetH, &sunSetM);
  sunCacheDay = (uint8_t)t->tm_mday;
  LOG(F("Soleil lever ")); LOG(sunRiseH); LOG(F(":")); LOG(sunRiseM);
  LOG(F(" coucher "));     LOG(sunSetH);  LOG(F(":")); LOGLN(sunSetM);
}

// =====================
// Scènes — helpers
// =====================
static const char* cmdToStr(uint8_t cmd) {
  if (cmd == HAUT) return "up";
  if (cmd == BAS)  return "down";
  return "stop";
}

static void checkSchedules() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  if (t->tm_year < 120) return; // NTP pas encore synchronisé (< 2020)

  uint32_t nowMinute = (uint32_t)(now / 60);

  // tm_wday: 0=Dim, 1=Lun...6=Sam → bitmask: bit0=Lun...bit6=Dim
  uint8_t wday = t->tm_wday;
  uint8_t dayBit = (wday == 0) ? (1 << 6) : (1 << (wday - 1));

  // Mise à jour du cache solaire si nouveau jour
  refreshSunCache(t, now);

  for (uint8_t i = 0; i < NB_SCENES; i++) {
    Scene& s = somfy.scenes[i];
    if (!s.enabled) continue;
    if (!(s.days & dayBit)) continue;
    if (lastFiredMinute[i] == nowMinute) continue; // déjà déclenché cette minute

    // Calcul de l'heure cible selon le type de déclencheur
    uint8_t targetH, targetM;
    if (s.triggerType == 0) {
      targetH = s.hour;
      targetM = s.minute;
    } else {
      int base = (s.triggerType == 1)
        ? (sunRiseH * 60 + sunRiseM)
        : (sunSetH  * 60 + sunSetM);
      int adjusted = base + (int)s.offsetMinutes;
      if (adjusted <    0) adjusted += 1440;
      if (adjusted >= 1440) adjusted -= 1440;
      targetH = (uint8_t)(adjusted / 60);
      targetM = (uint8_t)(adjusted % 60);
    }

    if (targetH != (uint8_t)t->tm_hour) continue;
    if (targetM != (uint8_t)t->tm_min)  continue;

    lastFiredMinute[i] = nowMinute;
    byte button = s.cmd;

    if (s.targetType == 0) {
      // Volet individuel
      if (s.targetId < MAX_VOLETS && somfy.volets[s.targetId].enabled) {
        SendSomfy(somfy.volets[s.targetId].remoteIndex, button);
        if (button == HAUT) startBlink(ledUp);
        else if (button == BAS) startBlink(ledDown);
      }
    } else {
      // Groupe
      if (s.targetId < MAX_GROUPES && somfy.groupes[s.targetId].enabled) {
        if (button == HAUT) startBlink(ledUp);
        else if (button == BAS) startBlink(ledDown);
        for (uint8_t j = 0; j < MAX_VOLETS; j++) {
          if (!(somfy.groupes[s.targetId].members & (1 << j))) continue;
          if (!somfy.volets[j].enabled) continue;
          SendSomfy(somfy.volets[j].remoteIndex, button);
          delay(120);
        }
      }
    }
    LOG(F("Scene fired: ")); LOGLN(i);
  }
}

// =====================
// Helpers API
// =====================
static bool ensureAuth() {
#if defined(USE_BASIC_AUTH)
  if (!server.authenticate(HTTP_USER, HTTP_PASS)) {
    server.requestAuthentication();
    return false;
  }
#endif
  return true;
}

static bool parseCmd(const String& cmd, byte &buttonOut) {
  if (cmd == "up")   { buttonOut = HAUT; return true; }
  if (cmd == "down") { buttonOut = BAS;  return true; }
  if (cmd == "stop") { buttonOut = STOP; return true; }
  if (cmd == "prog") { buttonOut = PROG; return true; }
  return false;
}

static String jsonEscape(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\"') o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else o += c;
  }
  return o;
}

// =====================
// Handlers REST
// =====================
static void handleStatus() {
  if (!ensureAuth()) return;

  String out;
  out.reserve(1024);
  out += "{";

  out += "\"wifi\":{";
  out += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  out += "\"rssi\":" + String(WiFi.RSSI());
  out += "},";

  // Heure NTP
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char tbuf[20];
  if (t->tm_year >= 120) {
    snprintf(tbuf, sizeof(tbuf), "%04d-%02d-%02dT%02d:%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min);
  } else {
    snprintf(tbuf, sizeof(tbuf), "sync...");
  }
  out += "\"time\":\"";
  out += tbuf;
  out += "\",";

  // Volets (uniquement les actifs)
  out += "\"volets\":[";
  bool firstVolet = true;
  for (uint8_t i = 0; i < MAX_VOLETS; i++) {
    if (!somfy.volets[i].enabled) continue;
    uint8_t ri = somfy.volets[i].remoteIndex;
    if (!firstVolet) out += ",";
    firstVolet = false;
    out += "{";
    out += "\"id\":"           + String(i)                                       + ",";
    out += "\"name\":\""       + jsonEscape(somfy.volets[i].name)                + "\",";
    out += "\"remoteIndex\":"  + String(ri)                                      + ",";
    out += "\"remoteID\":\"0x" + String(somfy.remotes[ri].remoteID, HEX)         + "\",";
    out += "\"rolling\":"      + String(somfy.remotes[ri].rollingCode);
    out += "}";
  }
  out += "],";

  // Groupes (uniquement les actifs)
  out += "\"groupes\":[";
  bool firstGroupe = true;
  for (uint8_t g = 0; g < MAX_GROUPES; g++) {
    if (!somfy.groupes[g].enabled) continue;
    if (!firstGroupe) out += ",";
    firstGroupe = false;
    out += "{";
    out += "\"id\":"     + String(g)                         + ",";
    out += "\"name\":\"" + jsonEscape(somfy.groupes[g].name) + "\",";
    out += "\"members\":[";
    bool firstMember = true;
    for (uint8_t j = 0; j < MAX_VOLETS; j++) {
      if (!(somfy.groupes[g].members & (1 << j))) continue;
      if (!firstMember) out += ",";
      firstMember = false;
      out += String(j);
    }
    out += "]";
    out += "}";
  }
  out += "]";

  out += "}";
  server.send(200, "application/json", out);
}

static void handleVolet() {
  if (!ensureAuth()) return;

  if (!server.hasArg("id") || !server.hasArg("cmd")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing id/cmd\"}");
    return;
  }

  int id = server.arg("id").toInt();
  String cmd = server.arg("cmd");
  byte button;
  if (id < 0 || id >= MAX_VOLETS || !somfy.volets[id].enabled) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid id\"}");
    return;
  }
  if (!parseCmd(cmd, button)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid cmd\"}");
    return;
  }

  uint8_t remoteIndex = somfy.volets[id].remoteIndex;
  SendSomfy(remoteIndex, button);

  if (cmd == "up") {
    startBlink(ledUp);
  } else if (cmd == "down") {
    startBlink(ledDown);
  } else if (cmd == "prog") {
    startBlink(ledUp);
    startBlink(ledDown);
  }

  String out = "{\"ok\":true,\"target\":\"volet\",\"id\":" + String(id) + ",\"cmd\":\"" + cmd + "\"}";
  server.send(200, "application/json", out);
}

static void handleGroupe() {
  if (!ensureAuth()) return;

  if (!server.hasArg("id") || !server.hasArg("cmd")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing id/cmd\"}");
    return;
  }

  int gid = server.arg("id").toInt();
  String cmd = server.arg("cmd");
  byte button;
  if (gid < 0 || gid >= MAX_GROUPES || !somfy.groupes[gid].enabled) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid group id\"}");
    return;
  }
  if (!parseCmd(cmd, button)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid cmd\"}");
    return;
  }

  if (cmd == "up") {
    startBlink(ledUp);
  } else if (cmd == "down") {
    startBlink(ledDown);
  } else if (cmd == "prog") {
    startBlink(ledUp);
    startBlink(ledDown);
  }

  for (uint8_t i = 0; i < MAX_VOLETS; i++) {
    if (!(somfy.groupes[gid].members & (1 << i))) continue;
    if (!somfy.volets[i].enabled) continue;
    uint8_t ri = somfy.volets[i].remoteIndex;
    SendSomfy(ri, button);
    delay(120);
  }

  String out = "{\"ok\":true,\"target\":\"groupe\",\"id\":" + String(gid) + ",\"cmd\":\"" + cmd + "\"}";
  server.send(200, "application/json", out);
}

static void handleVoletConfig() {
  if (!ensureAuth()) return;

  if (server.method() == HTTP_DELETE) {
    if (!server.hasArg("id")) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing id\"}"); return; }
    int id = server.arg("id").toInt();
    if (id < 0 || id >= MAX_VOLETS) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid id\"}"); return; }
    somfy.volets[id].enabled = 0;
    EEPROM.put(EEPROM_ADDRESS, somfy); EEPROM.commit();
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }

  if (server.method() == HTTP_GET) {
    String out; out.reserve(512);
    out += "{\"volets\":[";
    for (uint8_t i = 0; i < MAX_VOLETS; i++) {
      out += "{";
      out += "\"id\":"          + String(i)                              + ",";
      out += "\"name\":\""      + jsonEscape(somfy.volets[i].name)       + "\",";
      out += "\"remoteIndex\":" + String(somfy.volets[i].remoteIndex)    + ",";
      out += "\"enabled\":"     + String(somfy.volets[i].enabled);
      out += "}";
      if (i + 1 < MAX_VOLETS) out += ",";
    }
    out += "]}";
    server.send(200, "application/json", out);
    return;
  }

  // POST
  if (!server.hasArg("id")) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing id\"}"); return; }
  int id = server.arg("id").toInt();
  if (id < 0 || id >= MAX_VOLETS) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid id\"}"); return; }

  VoletConfig& v = somfy.volets[id];
  if (server.hasArg("name")) {
    strncpy(v.name, server.arg("name").c_str(), NAME_LEN - 1);
    v.name[NAME_LEN - 1] = '\0';
  }
  if (server.hasArg("remoteIndex")) v.remoteIndex = (uint8_t)constrain(server.arg("remoteIndex").toInt(), 0, 15);
  if (server.hasArg("enabled"))     v.enabled     = (server.arg("enabled") == "1") ? 1 : 0;

  EEPROM.put(EEPROM_ADDRESS, somfy); EEPROM.commit();
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleGroupeConfig() {
  if (!ensureAuth()) return;

  if (server.method() == HTTP_DELETE) {
    if (!server.hasArg("id")) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing id\"}"); return; }
    int id = server.arg("id").toInt();
    if (id < 0 || id >= MAX_GROUPES) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid id\"}"); return; }
    somfy.groupes[id].enabled = 0;
    EEPROM.put(EEPROM_ADDRESS, somfy); EEPROM.commit();
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }

  if (server.method() == HTTP_GET) {
    String out; out.reserve(384);
    out += "{\"groupes\":[";
    for (uint8_t i = 0; i < MAX_GROUPES; i++) {
      out += "{";
      out += "\"id\":"      + String(i)                              + ",";
      out += "\"name\":\"" + jsonEscape(somfy.groupes[i].name)       + "\",";
      out += "\"members\":" + String(somfy.groupes[i].members)       + ",";
      out += "\"enabled\":" + String(somfy.groupes[i].enabled);
      out += "}";
      if (i + 1 < MAX_GROUPES) out += ",";
    }
    out += "]}";
    server.send(200, "application/json", out);
    return;
  }

  // POST
  if (!server.hasArg("id")) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing id\"}"); return; }
  int id = server.arg("id").toInt();
  if (id < 0 || id >= MAX_GROUPES) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid id\"}"); return; }

  GroupeConfig& g = somfy.groupes[id];
  if (server.hasArg("name")) {
    strncpy(g.name, server.arg("name").c_str(), NAME_LEN - 1);
    g.name[NAME_LEN - 1] = '\0';
  }
  if (server.hasArg("members")) g.members = (uint16_t)(server.arg("members").toInt() & 0xFF);
  if (server.hasArg("enabled")) g.enabled = (server.arg("enabled") == "1") ? 1 : 0;

  EEPROM.put(EEPROM_ADDRESS, somfy); EEPROM.commit();
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleLedConfig() {
  if (!ensureAuth()) return;
  if (server.method() == HTTP_GET) {
    server.send(200, "application/json",
      String("{\"ledPower\":") + (ledPowerEnabled ? "1" : "0") + "}");
    return;
  }
  // POST
  if (server.hasArg("power")) {
    ledPowerEnabled = (server.arg("power") == "1");
    setLEDState(ledPower, ledPowerEnabled);
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleScenes() {
  if (!ensureAuth()) return;

  String out;
  out.reserve(640);
  out += "{\"scenes\":[";
  for (uint8_t i = 0; i < NB_SCENES; i++) {
    Scene& s = somfy.scenes[i];
    out += "{";
    out += "\"id\":"           + String(i)               + ",";
    out += "\"enabled\":"      + String(s.enabled)        + ",";
    out += "\"hour\":"         + String(s.hour)           + ",";
    out += "\"min\":"          + String(s.minute)         + ",";
    out += "\"days\":"         + String(s.days)           + ",";
    out += "\"targetType\":"   + String(s.targetType)     + ",";
    out += "\"targetId\":"     + String(s.targetId)       + ",";
    out += "\"cmd\":\""        + String(cmdToStr(s.cmd))  + "\",";
    out += "\"triggerType\":"  + String(s.triggerType)    + ",";
    out += "\"offset\":"       + String((int)s.offsetMinutes);
    out += "}";
    if (i + 1 < NB_SCENES) out += ",";
  }
  out += "]}";
  server.send(200, "application/json", out);
}

static void handleSun() {
  if (!ensureAuth()) return;
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"rise\":\"%02u:%02u\",\"set\":\"%02u:%02u\"}",
           sunRiseH, sunRiseM, sunSetH, sunSetM);
  server.send(200, "application/json", buf);
}

static void handleScenePost() {
  if (!ensureAuth()) return;

  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing id\"}");
    return;
  }
  int id = server.arg("id").toInt();
  if (id < 0 || id >= NB_SCENES) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid id\"}");
    return;
  }

  Scene& s = somfy.scenes[id];
  if (server.hasArg("hour"))        s.hour          = (uint8_t)constrain(server.arg("hour").toInt(),        0,  23);
  if (server.hasArg("min"))         s.minute        = (uint8_t)constrain(server.arg("min").toInt(),         0,  59);
  if (server.hasArg("days"))        s.days          = (uint8_t)(server.arg("days").toInt() & 0x7F);
  if (server.hasArg("type"))        s.targetType    = (uint8_t)(server.arg("type").toInt() & 1);
  if (server.hasArg("targetId"))    s.targetId      = (uint8_t)server.arg("targetId").toInt();
  if (server.hasArg("cmd")) {
    byte btn;
    if (parseCmd(server.arg("cmd"), btn)) s.cmd = btn;
  }
  if (server.hasArg("enabled"))     s.enabled       = (server.arg("enabled") == "1") ? 1 : 0;
  if (server.hasArg("triggerType")) s.triggerType   = (uint8_t)constrain(server.arg("triggerType").toInt(), 0, 2);
  if (server.hasArg("offset"))      s.offsetMinutes = (int8_t)constrain(server.arg("offset").toInt(),     -127, 127);

  EEPROM.put(EEPROM_ADDRESS, somfy);
  EEPROM.commit();
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleSceneDelete() {
  if (!ensureAuth()) return;

  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing id\"}");
    return;
  }
  int id = server.arg("id").toInt();
  if (id < 0 || id >= NB_SCENES) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid id\"}");
    return;
  }
  somfy.scenes[id].enabled = 0;
  EEPROM.put(EEPROM_ADDRESS, somfy);
  EEPROM.commit();
  server.send(200, "application/json", "{\"ok\":true}");
}

// =====================
// Page Web
// =====================
static const char PAGE_INDEX[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Somfy RTS - Volets</title>
<style>
body{font-family:system-ui,Arial;margin:16px;max-width:960px}
.card{border:1px solid #ddd;border-radius:10px;padding:12px;margin:10px 0}
.row{display:flex;gap:8px;flex-wrap:wrap}
button{padding:10px 12px;border-radius:8px;border:1px solid #bbb;background:#f7f7f7;cursor:pointer}
button:active{transform:translateY(1px)}
button.danger{background:#fee;border-color:#f99}
button.small{padding:5px 8px;font-size:0.85em}
small{color:#555}
table{border-collapse:collapse;width:100%}
td,th{border:1px solid #ddd;padding:6px 10px;text-align:left}
th{background:#f5f5f5}
select,input[type=number],input[type=time],input[type=text]{padding:6px;border-radius:6px;border:1px solid #bbb}
.form-row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:8px 0}
hr{border:none;border-top:1px solid #eee;margin:12px 0}
.tabs{display:flex;gap:0;margin:12px 0 0}
.tab-btn{padding:8px 18px;border:1px solid #bbb;border-bottom:none;background:#f5f5f5;cursor:pointer;border-radius:6px 6px 0 0;font-size:.95em}
.tab-btn.active{background:#fff;font-weight:bold;position:relative;margin-bottom:-1px;border-bottom:1px solid #fff}
.tab-pane{display:none;border:1px solid #bbb;border-radius:0 6px 6px 6px;padding:12px}
.tab-pane.active{display:block}
</style></head><body>
<h1>Somfy RTS - Multi-volets</h1>
<div id="meta"><small>Chargement…</small></div>
<div style="margin:4px 0"><button id="btn-led" onclick="toggleLed()" style="padding:4px 10px;font-size:0.85em">LED fonct. &#9679;</button></div>

<div class="tabs">
  <button class="tab-btn active" onclick="showTab('usage')">Utilisation</button>
  <button class="tab-btn" onclick="showTab('config')">Configuration</button>
</div>

<div id="tab-usage" class="tab-pane active">
<h2>Groupes</h2>
<div id="groups"></div>

<h2>Volets</h2>
<div id="volets"></div>

<h2>Scènes / Horaires</h2>
<div class="card">
  <b>Nouvelle scène</b>
  <div class="form-row">
    <label>Déclencheur
      <select id="sc-trigger" onchange="onTriggerChange()">
        <option value="0">Heure fixe</option>
        <option value="1">Lever du soleil</option>
        <option value="2">Coucher du soleil</option>
      </select>
    </label>
    <span id="sc-time-wrap"><label>Heure <input type="time" id="sc-time" value="08:00"></label></span>
    <label>Décalage (min) <input type="number" id="sc-offset" value="0" min="-127" max="127" style="width:70px"></label>
  </div>
  <div class="form-row">
    <span>
      <label><input type="checkbox" class="dc" value="1"> Lun</label>
      <label><input type="checkbox" class="dc" value="2"> Mar</label>
      <label><input type="checkbox" class="dc" value="4"> Mer</label>
      <label><input type="checkbox" class="dc" value="8"> Jeu</label>
      <label><input type="checkbox" class="dc" value="16"> Ven</label>
      <label><input type="checkbox" class="dc" value="32"> Sam</label>
      <label><input type="checkbox" class="dc" value="64"> Dim</label>
    </span>
  </div>
  <div class="form-row">
    <label>Type <select id="sc-type" onchange="updateTargetList()"><option value="0">Volet</option><option value="1">Groupe</option></select></label>
    <label>Cible <select id="sc-target"></select></label>
    <label>Commande <select id="sc-cmd"><option value="up">Monter</option><option value="down">Descendre</option><option value="stop">Stop</option></select></label>
    <label>Slot (0-7) <input type="number" id="sc-id" min="0" max="7" value="0" style="width:60px"></label>
    <button onclick="saveScene()">Enregistrer</button>
  </div>
</div>
<div id="scenes-list"></div>
</div><!-- /tab-usage -->

<div id="tab-config" class="tab-pane">
<h2>Volets</h2>
<div class="card">
  <b>Volets</b>
  <div id="volets-config"></div>
  <hr>
  <b>Nouveau / modifier volet</b>
  <div class="form-row">
    <label>Slot (0-7) <input type="number" id="vc-id" min="0" max="7" value="0" style="width:60px"></label>
    <label>Nom <input type="text" id="vc-name" maxlength="19" style="width:150px" placeholder="ex: Salon"></label>
    <label>Remote (0-15) <input type="number" id="vc-remote" min="0" max="15" value="0" style="width:60px"></label>
    <button onclick="saveVoletConfig()">Enregistrer</button>
  </div>
</div>
<h2>Groupes</h2>
<div class="card">
  <b>Groupes</b>
  <div id="groupes-config"></div>
  <hr>
  <b>Nouveau / modifier groupe</b>
  <div class="form-row">
    <label>Slot (0-5) <input type="number" id="gc-id" min="0" max="5" value="0" style="width:60px"></label>
    <label>Nom <input type="text" id="gc-name" maxlength="19" style="width:150px" placeholder="ex: Chambres"></label>
  </div>
  <div class="form-row" id="gc-members"><small>Chargement volets…</small></div>
  <div class="form-row">
    <button onclick="saveGroupeConfig()">Enregistrer</button>
  </div>
</div>
</div><!-- /tab-config -->

<script>
const DAY_NAMES=["Lun","Mar","Mer","Jeu","Ven","Sam","Dim"];
const TRIGGER_LABELS=["","Lever","Coucher"];
let statusData={volets:[],groupes:[]};
let configData={volets:[],groupes:[]};

async function api(path,opts){
  const r=await fetch(path,{cache:"no-store",...(opts||{})});
  return r.json();
}
function btn(label,onclick,cls){
  const b=document.createElement("button");
  b.textContent=label; b.onclick=onclick;
  if(cls) b.className=cls;
  return b;
}
function card(title){
  const d=document.createElement("div"); d.className="card";
  const h=document.createElement("div"); h.innerHTML="<b>"+title+"</b>";
  d.appendChild(h); return d;
}
function daysStr(mask){
  if(!mask) return "—";
  if(mask===127) return "Tous";
  return DAY_NAMES.filter((_,i)=>mask&(1<<i)).join(", ");
}
function triggerStr(s){
  if(s.triggerType===0) return String(s.hour).padStart(2,"0")+":"+String(s.min).padStart(2,"0");
  const base=TRIGGER_LABELS[s.triggerType];
  const off=s.offset;
  return off===0?base:base+(off>0?"+":"")+off+"min";
}
function onTriggerChange(){
  const t=parseInt(document.getElementById("sc-trigger").value);
  document.getElementById("sc-time-wrap").style.display=t===0?"":"none";
}
function updateTargetList(){
  const type=parseInt(document.getElementById("sc-type").value);
  const sel=document.getElementById("sc-target");
  sel.innerHTML="";
  const list=type===0?statusData.volets:statusData.groupes;
  list.forEach(x=>{
    const o=document.createElement("option");
    o.value=x.id; o.textContent=x.name; sel.appendChild(o);
  });
}
async function saveScene(){
  const [hh,mm]=document.getElementById("sc-time").value.split(":").map(Number);
  let days=0;
  document.querySelectorAll(".dc:checked").forEach(c=>days|=parseInt(c.value));
  const type=document.getElementById("sc-type").value;
  const targetId=document.getElementById("sc-target").value;
  const cmd=document.getElementById("sc-cmd").value;
  const id=document.getElementById("sc-id").value;
  const triggerType=document.getElementById("sc-trigger").value;
  const offset=document.getElementById("sc-offset").value;
  const p=new URLSearchParams({id,hour:hh,min:mm,days,type,targetId,cmd,enabled:1,triggerType,offset});
  await api("/api/scene?"+p.toString(),{method:"POST"});
  await refreshScenes();
}
async function deleteScene(id){
  await api("/api/scene?id="+id,{method:"DELETE"});
  await refreshScenes();
}
async function refreshScenes(){
  const data=await api("/api/scenes");
  const div=document.getElementById("scenes-list");
  const active=data.scenes.filter(s=>s.enabled);
  if(!active.length){div.innerHTML="<small>Aucune scène active.</small>";return;}
  let html="<table><tr><th>#</th><th>Déclencheur</th><th>Jours</th><th>Cible</th><th>Commande</th><th></th></tr>";
  active.forEach(s=>{
    const vmap=Object.fromEntries(statusData.volets.map(v=>[v.id,v.name]));
    const gmap=Object.fromEntries(statusData.groupes.map(g=>[g.id,g.name]));
    const tname=s.targetType===0?(vmap[s.targetId]||"?"):(gmap[s.targetId]||"?");
    const cname={up:"Monter",down:"Descendre",stop:"Stop"}[s.cmd]||s.cmd;
    const ttype=s.targetType===0?"Volet":"Groupe";
    html+=`<tr><td>${s.id}</td><td>${triggerStr(s)}</td><td>${daysStr(s.days)}</td><td>${ttype}: ${tname}</td><td>${cname}</td><td><button class="danger small" onclick="deleteScene(${s.id})">Supprimer</button></td></tr>`;
  });
  html+="</table>";
  div.innerHTML=html;
}

// ---- Configuration volets ----
async function saveVoletConfig(){
  const id=document.getElementById("vc-id").value;
  const name=document.getElementById("vc-name").value;
  const remoteIndex=document.getElementById("vc-remote").value;
  const p=new URLSearchParams({id,name,remoteIndex,enabled:1});
  await api("/api/volet-config?"+p.toString(),{method:"POST"});
  await refresh(); await refreshConfig();
}
async function deleteVoletConfig(id){
  if(!confirm("Désactiver le volet slot "+id+" ?")) return;
  await api("/api/volet-config?id="+id,{method:"DELETE"});
  await refresh(); await refreshConfig();
}

// ---- Configuration groupes ----
async function saveGroupeConfig(){
  const id=document.getElementById("gc-id").value;
  const name=document.getElementById("gc-name").value;
  let members=0;
  document.querySelectorAll(".gm:checked").forEach(c=>members|=(1<<parseInt(c.value)));
  const p=new URLSearchParams({id,name,members,enabled:1});
  await api("/api/groupe-config?"+p.toString(),{method:"POST"});
  await refresh(); await refreshConfig();
}
async function deleteGroupeConfig(id){
  if(!confirm("Désactiver le groupe slot "+id+" ?")) return;
  await api("/api/groupe-config?id="+id,{method:"DELETE"});
  await refresh(); await refreshConfig();
}

async function refreshConfig(){
  const [vc,gc]=await Promise.all([api("/api/volet-config"),api("/api/groupe-config")]);
  configData=vc;

  // Table volets config
  let html="<table><tr><th>Slot</th><th>Nom</th><th>Remote</th><th>Actif</th><th></th></tr>";
  vc.volets.forEach(v=>{
    html+=`<tr>
      <td>${v.id}</td>
      <td><input type="text" value="${v.name}" id="vn-${v.id}" maxlength="19" style="width:130px"></td>
      <td><input type="number" value="${v.remoteIndex}" id="vr-${v.id}" min="0" max="15" style="width:55px"></td>
      <td>${v.enabled?"✓":"—"}</td>
      <td style="white-space:nowrap">
        <button class="small" onclick="saveVoletSlot(${v.id})">Sauver</button>
        ${v.enabled?`<button class="danger small" onclick="deleteVoletConfig(${v.id})">Suppr.</button>`:""}
      </td></tr>`;
  });
  html+="</table>";
  document.getElementById("volets-config").innerHTML=html;

  // Table groupes config
  html="<table><tr><th>Slot</th><th>Nom</th><th>Membres (volets actifs)</th><th>Actif</th><th></th></tr>";
  gc.groupes.forEach(g=>{
    const vmap=Object.fromEntries(statusData.volets.map(v=>[v.id,v.name]));
    const memberNames=statusData.volets.filter(v=>g.members&(1<<v.id)).map(v=>v.name).join(", ")||"—";
    html+=`<tr>
      <td>${g.id}</td>
      <td>${g.name}</td>
      <td>${memberNames}</td>
      <td>${g.enabled?"✓":"—"}</td>
      <td style="white-space:nowrap">
        ${g.enabled?`<button class="danger small" onclick="deleteGroupeConfig(${g.id})">Suppr.</button>`:""}
      </td></tr>`;
  });
  html+="</table>";
  document.getElementById("groupes-config").innerHTML=html;

  // Checkboxes membres groupe (volets actifs)
  const membersDiv=document.getElementById("gc-members");
  if(statusData.volets.length===0){
    membersDiv.innerHTML="<small>Aucun volet actif.</small>";
  } else {
    membersDiv.innerHTML="<b style='margin-right:8px'>Volets:</b>";
    statusData.volets.forEach(v=>{
      const lbl=document.createElement("label");
      lbl.innerHTML=`<input type="checkbox" class="gm" value="${v.id}"> ${v.name}`;
      membersDiv.appendChild(lbl);
    });
  }
}

async function saveVoletSlot(id){
  const name=document.getElementById("vn-"+id).value;
  const remoteIndex=document.getElementById("vr-"+id).value;
  const p=new URLSearchParams({id,name,remoteIndex,enabled:1});
  await api("/api/volet-config?"+p.toString(),{method:"POST"});
  await refresh(); await refreshConfig();
}

async function toggleLed(){
  const cur=await api("/api/led");
  const next=cur.ledPower===1?0:1;
  await api("/api/led?power="+next,{method:"POST"});
  document.getElementById("btn-led").style.color=next?"green":"#aaa";
}
async function refresh(){
  const [st,sun,led]=await Promise.all([api("/api/status"),api("/api/sun"),api("/api/led")]);
  statusData=st;
  document.getElementById("btn-led").style.color=led.ledPower?"green":"#aaa";
  document.querySelector("#meta").innerHTML=
    "<small>IP: "+st.wifi.ip+" | RSSI: "+st.wifi.rssi+" dBm | "+st.time
    +" | Colomiers : 🌅 Lever: "+sun.rise+" :: 🌙 Coucher: "+sun.set+"</small>";

  const g=document.querySelector("#groups"); g.innerHTML="";
  st.groupes.forEach(gr=>{
    const c=card("Groupe #"+gr.id+" — "+gr.name);
    const r=document.createElement("div"); r.className="row";
    r.appendChild(btn("Monter",   ()=>api("/api/groupe?id="+gr.id+"&cmd=up")));
    r.appendChild(btn("Stop",     ()=>api("/api/groupe?id="+gr.id+"&cmd=stop")));
    r.appendChild(btn("Descendre",()=>api("/api/groupe?id="+gr.id+"&cmd=down")));
    c.appendChild(r);
    c.appendChild(document.createElement("div")).innerHTML=
      "<small>Membres: "+gr.members.join(", ")+"</small>";
    g.appendChild(c);
  });

  const v=document.querySelector("#volets"); v.innerHTML="";
  st.volets.forEach(vo=>{
    const c=card("Volet #"+vo.id+" — "+vo.name);
    const r=document.createElement("div"); r.className="row";
    r.appendChild(btn("Monter",   ()=>api("/api/volet?id="+vo.id+"&cmd=up")));
    r.appendChild(btn("Stop/MY",  ()=>api("/api/volet?id="+vo.id+"&cmd=stop")));
    r.appendChild(btn("Descendre",()=>api("/api/volet?id="+vo.id+"&cmd=down")));
    r.appendChild(btn("Prog", async ()=>{
      if(confirm("Confirmer PROG pour '"+vo.name+"' ?"))
        await api("/api/volet?id="+vo.id+"&cmd=prog");
    }));
    c.appendChild(r);
    c.appendChild(document.createElement("div")).innerHTML=
      "<small>RemoteIndex: "+vo.remoteIndex+" | RemoteID: "+vo.remoteID+" | Rolling: "+vo.rolling+"</small>";
    v.appendChild(c);
  });

  updateTargetList();
  await refreshScenes();
}
function showTab(name){
  document.querySelectorAll(".tab-pane").forEach(p=>p.classList.remove("active"));
  document.querySelectorAll(".tab-btn").forEach(b=>b.classList.remove("active"));
  document.getElementById("tab-"+name).classList.add("active");
  const labels={usage:"Utilisation",config:"Configuration"};
  document.querySelectorAll(".tab-btn").forEach(b=>{
    if(b.textContent.trim()===labels[name]) b.classList.add("active");
  });
  if(name==="config") refreshConfig();
}
refresh();
</script>
</body></html>
)HTML";

static void handleIndex() {
  if (!ensureAuth()) return;
  server.send(200, "text/html; charset=utf-8", FPSTR(PAGE_INDEX));
}

// =====================
// Portail de configuration WiFi
// =====================
static const char CONFIG_PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Somfy - Config WiFi</title>
<style>body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 16px}
input,select{width:100%;padding:8px;margin:6px 0;box-sizing:border-box;border-radius:6px;border:1px solid #bbb}
button{width:100%;padding:10px;background:#4a90d9;color:#fff;border:none;border-radius:6px;font-size:1em;cursor:pointer}
h2{color:#333}p{color:#666;font-size:.9em}
</style></head><body>
<h2>Configuration WiFi</h2>
<p>Connectez-vous au reseau <b>SomfyConfig</b> depuis un autre appareil puis configurez le WiFi.</p>
<label>Reseau WiFi</label>
<select id=ssid><option>Scan en cours...</option></select>
<label>Mot de passe</label>
<input type=password id=pass placeholder="Mot de passe WiFi">
<br><br>
<button onclick=save()>Enregistrer et redemarrer</button>
<script>
fetch("/scan").then(r=>r.json()).then(list=>{
  const s=document.getElementById("ssid");
  s.innerHTML=list.map(n=>"<option>"+n+"</option>").join("");
}).catch(()=>{document.getElementById("ssid").innerHTML="<option>Erreur scan</option>";});
function save(){
  const ssid=document.getElementById("ssid").value;
  const pass=document.getElementById("pass").value;
  if(!ssid){alert("Selectionnez un reseau");return;}
  const p=new URLSearchParams({ssid,pass});
  fetch("/wifi?"+p.toString(),{method:"POST"}).then(r=>r.text()).then(t=>{document.body.innerHTML=t});
}
</script></body></html>
)rawliteral";

static bool isInvalidRemoteID(uint32_t id) {
  return (id == 0xFFFFFFFFu) || (id == 0x00000000u);
}

static void fixupEEPROM() {
  bool changed = false;
  for (int i = 0; i < 16; i++) {
    if (isInvalidRemoteID(somfy.remotes[i].remoteID)) {
      somfy.remotes[i].remoteID    = 0;
      somfy.remotes[i].rollingCode = 0;
      changed = true;
    }
  }
  if (changed) {
    EEPROM.put(EEPROM_ADDRESS, somfy);
    EEPROM.commit();
  }
}

// Valeurs par défaut pour la migration / init complète
static void initDefaultVoletsGroupes() {
  struct { const char* name; uint8_t remoteIndex; } dv[] = {
    {"Salon",           0},
    {"Cuisine",         1},
    {"Chambre Charles", 2},
    {"Chambre Louis",   3},
    {"Chambre Parent",  4},
    {"Salle de bain",   5}
  };
  const uint8_t NB_DV = sizeof(dv) / sizeof(dv[0]);

  for (uint8_t i = 0; i < MAX_VOLETS; i++) {
    memset(&somfy.volets[i], 0, sizeof(VoletConfig));
    if (i < NB_DV) {
      strncpy(somfy.volets[i].name, dv[i].name, NAME_LEN - 1);
      somfy.volets[i].remoteIndex = dv[i].remoteIndex;
      somfy.volets[i].enabled = 1;
    }
  }

  // bitmasks: bit i = volet slot i
  // "Tous"={0..5}=0x3F, "Chambres"={2,3,4}=0x1C, "Arriere"={1,5}=0x22, "Arriere+Chambres"={1..5}=0x3E
  struct { const char* name; uint16_t members; } dg[] = {
    {"Tous",               0x3F},
    {"Chambres",           0x1C},
    {"Arriere",            0x22},
    {"Arriere + Chambres", 0x3E}
  };
  const uint8_t NB_DG = sizeof(dg) / sizeof(dg[0]);

  for (uint8_t i = 0; i < MAX_GROUPES; i++) {
    memset(&somfy.groupes[i], 0, sizeof(GroupeConfig));
    if (i < NB_DG) {
      strncpy(somfy.groupes[i].name, dg[i].name, NAME_LEN - 1);
      somfy.groupes[i].members = dg[i].members;
      somfy.groupes[i].enabled = 1;
    }
  }
}

// =====================
// Init EEPROM
// =====================
static void initEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(EEPROM_ADDRESS, somfy);

  if (somfy.appVersion == 4) {
    // Migration VERSION 4 → 5 : ajout wifiSSID/wifiPass en EEPROM
    LOGLN(F("EEPROM migration v4->v5"));
    strncpy(somfy.wifiSSID, WIFI_SSID_DEFAULT, sizeof(somfy.wifiSSID) - 1);
    somfy.wifiSSID[sizeof(somfy.wifiSSID) - 1] = '\0';
    strncpy(somfy.wifiPass, WIFI_PASS_DEFAULT, sizeof(somfy.wifiPass) - 1);
    somfy.wifiPass[sizeof(somfy.wifiPass) - 1] = '\0';
    somfy.appVersion = VERSION;
    EEPROM.put(EEPROM_ADDRESS, somfy);
    EEPROM.commit();

  } else if (somfy.appVersion == 3) {
    // Migration VERSION 3 → 5 : ajout volets/groupes + wifiSSID/wifiPass
    LOGLN(F("EEPROM migration v3->v5"));
    initDefaultVoletsGroupes();
    strncpy(somfy.wifiSSID, WIFI_SSID_DEFAULT, sizeof(somfy.wifiSSID) - 1);
    somfy.wifiSSID[sizeof(somfy.wifiSSID) - 1] = '\0';
    strncpy(somfy.wifiPass, WIFI_PASS_DEFAULT, sizeof(somfy.wifiPass) - 1);
    somfy.wifiPass[sizeof(somfy.wifiPass) - 1] = '\0';
    somfy.appVersion = VERSION;
    EEPROM.put(EEPROM_ADDRESS, somfy);
    EEPROM.commit();

  } else if (somfy.appVersion == 2) {
    // Migration VERSION 2 → 5 : Scene agrandie (7→9 bytes) + wifiSSID/wifiPass
    LOGLN(F("EEPROM migration v2->v5"));
    struct OldScene { uint8_t hour, minute, days, targetType, targetId, cmd, enabled; };
    const int scenesBase = (int)sizeof(int) + 16 * (int)sizeof(Remote); // offset 100
    for (uint8_t i = 0; i < NB_SCENES; i++) {
      OldScene old;
      EEPROM.get(scenesBase + i * (int)sizeof(OldScene), old);
      somfy.scenes[i] = {old.hour, old.minute, old.days,
                         old.targetType, old.targetId, old.cmd,
                         old.enabled, 0, 0};
    }
    initDefaultVoletsGroupes();
    strncpy(somfy.wifiSSID, WIFI_SSID_DEFAULT, sizeof(somfy.wifiSSID) - 1);
    somfy.wifiSSID[sizeof(somfy.wifiSSID) - 1] = '\0';
    strncpy(somfy.wifiPass, WIFI_PASS_DEFAULT, sizeof(somfy.wifiPass) - 1);
    somfy.wifiPass[sizeof(somfy.wifiPass) - 1] = '\0';
    somfy.appVersion = VERSION;
    EEPROM.put(EEPROM_ADDRESS, somfy);
    EEPROM.commit();

  } else if (somfy.appVersion == 1) {
    // Migration VERSION 1 → 5 : remoteIDs préservés + wifiSSID/wifiPass
    LOGLN(F("EEPROM migration v1->v5"));
    for (uint8_t i = 0; i < NB_SCENES; i++) {
      somfy.scenes[i] = {0, 0, 0, 0, 0, STOP, 0, 0, 0};
    }
    initDefaultVoletsGroupes();
    strncpy(somfy.wifiSSID, WIFI_SSID_DEFAULT, sizeof(somfy.wifiSSID) - 1);
    somfy.wifiSSID[sizeof(somfy.wifiSSID) - 1] = '\0';
    strncpy(somfy.wifiPass, WIFI_PASS_DEFAULT, sizeof(somfy.wifiPass) - 1);
    somfy.wifiPass[sizeof(somfy.wifiPass) - 1] = '\0';
    somfy.appVersion = VERSION;
    EEPROM.put(EEPROM_ADDRESS, somfy);
    EEPROM.commit();

  } else if (somfy.appVersion != VERSION) {
    // Initialisation complète (premier boot ou version inconnue)
    LOGLN(F("EEPROM init"));
    somfy.appVersion = VERSION;
    fixupEEPROM();

    somfy.remotes[0].remoteID = 0x5A7B2C;  // Salon
    somfy.remotes[1].remoteID = 0xC3E916;  // Cuisine
    somfy.remotes[2].remoteID = 0x2F4D8A;  // Chambre Charles
    somfy.remotes[3].remoteID = 0xD6F541;  // Chambre Louis
    somfy.remotes[4].remoteID = 0x8A1BE7;  // Chambre Parent
    somfy.remotes[5].remoteID = 0x47C36D;  // Salle de bain

    for (uint8_t i = 0; i < NB_SCENES; i++) {
      somfy.scenes[i] = {0, 0, 0, 0, 0, STOP, 0, 0, 0};
    }
    initDefaultVoletsGroupes();
    strncpy(somfy.wifiSSID, WIFI_SSID_DEFAULT, sizeof(somfy.wifiSSID) - 1);
    somfy.wifiSSID[sizeof(somfy.wifiSSID) - 1] = '\0';
    strncpy(somfy.wifiPass, WIFI_PASS_DEFAULT, sizeof(somfy.wifiPass) - 1);
    somfy.wifiPass[sizeof(somfy.wifiPass) - 1] = '\0';

    EEPROM.put(EEPROM_ADDRESS, somfy);
    EEPROM.commit();
  }
}

static void initRadioPin() {
  pinMode(TX_PIN, OUTPUT);
  digitalWrite(TX_PIN, LOW);
  txMask = (1u << TX_PIN);
  txStateLow = true;
}

static void startConfigPortal() {
  LOGLN(F("Mode config WiFi (AP)"));
  setLEDState(ledPower, true);

  WiFi.mode(WIFI_AP);
  WiFi.softAP("SomfyConfig");
  IPAddress apIP(192, 168, 4, 1);

  DNSServer dnsServer;
  dnsServer.start(53, "*", apIP);

  ESP8266WebServer cfgServer(80);

  cfgServer.on("/", HTTP_GET, [&cfgServer]() {
    cfgServer.send_P(200, "text/html; charset=utf-8", CONFIG_PORTAL_HTML);
  });

  cfgServer.on("/scan", HTTP_GET, [&cfgServer]() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
      if (i) json += ",";
      json += "\"" + WiFi.SSID(i) + "\"";
    }
    json += "]";
    cfgServer.send(200, "application/json", json);
  });

  cfgServer.on("/wifi", HTTP_POST, [&cfgServer]() {
    if (cfgServer.hasArg("ssid") && cfgServer.hasArg("pass")) {
      strncpy(somfy.wifiSSID, cfgServer.arg("ssid").c_str(), sizeof(somfy.wifiSSID) - 1);
      somfy.wifiSSID[sizeof(somfy.wifiSSID) - 1] = '\0';
      strncpy(somfy.wifiPass, cfgServer.arg("pass").c_str(), sizeof(somfy.wifiPass) - 1);
      somfy.wifiPass[sizeof(somfy.wifiPass) - 1] = '\0';
      EEPROM.put(EEPROM_ADDRESS, somfy);
      EEPROM.commit();
      cfgServer.send(200, "text/html; charset=utf-8",
        "<html><body><h2>Credentials sauvegardes, redemarrage...</h2></body></html>");
      delay(1500);
      ESP.restart();
    }
    cfgServer.send(400, "text/plain", "Parametres manquants");
  });

  cfgServer.onNotFound([&cfgServer]() {
    cfgServer.sendHeader("Location", "http://192.168.4.1/");
    cfgServer.send(302, "text/plain", "");
  });

  cfgServer.begin();
  LOGLN(F("Portail config demarre: SomfyConfig (192.168.4.1)"));

  uint32_t lastBlink = 0;
  while (true) {
    dnsServer.processNextRequest();
    cfgServer.handleClient();
    if (millis() - lastBlink > 800) {
      lastBlink = millis();
      setLEDState(ledPower, !ledPower.state);
    }
    yield();
  }
}

static void initWiFi() {
  if (strcmp(somfy.wifiSSID, WIFI_SSID_DEFAULT) == 0 || somfy.wifiSSID[0] == '\0') {
    startConfigPortal();  // bloquant — reboot à la fin
    return;               // jamais atteint
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.begin(somfy.wifiSSID, somfy.wifiPass);

  LOGLN(F("WiFi connexion..."));
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    LOG(".");
  }
  LOGLN("");

  if (WiFi.status() == WL_CONNECTED) {
    LOG(F("WiFi OK IP=")); LOGLN(WiFi.localIP().toString());
  } else {
    LOGLN(F("WiFi ECHEC (serveur quand meme demarre)"));
  }
}

static void initServer() {
  server.on("/",                    handleIndex);
  server.on("/api/status",          handleStatus);
  server.on("/api/volet",           handleVolet);
  server.on("/api/groupe",          handleGroupe);
  server.on("/api/scenes",          handleScenes);
  server.on("/api/scene",  HTTP_POST,   handleScenePost);
  server.on("/api/scene",  HTTP_DELETE, handleSceneDelete);
  server.on("/api/sun",             handleSun);
  server.on("/api/volet-config",    handleVoletConfig);
  server.on("/api/groupe-config",   handleGroupeConfig);
  server.on("/api/led",             handleLedConfig);
  server.onNotFound([](){
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
  });
  server.begin();
  LOGLN(F("HTTP server started"));
}

// =====================
// Arduino
// =====================
void setup() {
  Serial.begin(115200);
  Serial.println();

  initLEDs();
  initEEPROM();
  initRadioPin();
  initWiFi();
  initOTA();
  initNTP();
  initServer();

  memset(lastFiredMinute, 0xFF, sizeof(lastFiredMinute));
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  checkSchedules();
  updateAllLEDs();
}
