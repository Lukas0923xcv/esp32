/*
  LilyGO-Netzwaechter – LilyGO T-A7670G R2
  - Erkennt Stromausfall am JST-Netzteil-Eingang (Hysterese + Entprellung)
  - Sendet den Status alle 1,5 s per Bluetooth an die Android-App
  - Sendet bei Stromausfall / Wiederkehr eine SMS (A7670G, LTE)
  - Empfaenger, SMS-Texte und Wartezeiten sind per App einstellbar (im Flash gespeichert)
  - Fernsteuerung per SMS: Nachrichten, die mit "/" beginnen, sind Befehle (siehe docs/SMS-BEFEHLE.md)
  Das Modem laeuft in einem eigenen Task auf Core 0, damit die App nie einfriert.

  Bluetooth-Befehle (eine Zeile, mit \n abgeschlossen):
    GET              -> sendet alle Einstellungen als "CFG:KEY=WERT" Zeilen, Ende "CFG:END"
    SET:KEY=WERT     -> speichert eine Einstellung, Antwort "OK:KEY" oder "ERR:KEY"
    TEST             -> sendet eine Test-SMS an alle Empfaenger
  Keys: NUMS, MSGOFF, MSGON, MSGLOW, OFFDELAY, ONDELAY, RESTORE, LOWBAT, SMSCMD
  Platzhalter in SMS-Texten: {akku} {volt} {dauer}
*/

#include <Arduino.h>
#include <Preferences.h>
#include "BluetoothSerial.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#define SMS_TARGET_NUMBER "+41797463235"   // Standard-Empfaenger und Admin (darf immer SMS-Befehle senden)
#define SIM_PIN           "8923"

// ================== Pins (LilyGO T-A7670G) ==================
#define BOARD_POWER_ON    12  // Modem-Versorgung
#define MODEM_PWRKEY_PIN   4
#define MODEM_DTR_PIN     25
#define MODEM_TX_PIN      26
#define MODEM_RX_PIN      27

#define BAT_ADC_PIN       35  // Interner 18650-Akku
#define SOLAR_ADC_PIN     36  // JST-Eingang (5V-Netzteil)

// ================== Einstellungen ==================
const char* BT_DEVICE_NAME = "LilyGO-Netzwaechter";
const char* SMSC           = "+41794999000";   // Sunrise SMS-Zentrale

const float BAT_DIVIDER   = 2.0;
const float SOLAR_DIVIDER = 2.0;
const float REF_VOLT      = 3.3;

const float         MAINS_OFF_VOLT = 2.5;
const float         MAINS_ON_VOLT  = 3.0;
const unsigned long MAINS_OFF_MS   = 400;
const unsigned long MAINS_ON_MS    = 1000;

const unsigned long SMS_RETRY_MS   = 60000;
const unsigned long SMS_POLL_MS    = 15000;   // so oft wird nach eingegangenen SMS-Befehlen geschaut
const unsigned long MAX_PAUSE_S    = 7UL * 86400;

const float SOLAR_ALPHA = 0.30;
const float BAT_ALPHA   = 0.10;

// ================== Konfiguration (per App, im Flash) ==================
const int      MAX_NUMBERS   = 5;
const int      MAX_SMS_LEN   = 160;
const uint32_t MAX_DELAY_S   = 3600;

const char* DEFAULT_MSG_OFF = "ALARM: Stromausfall erkannt! System laeuft auf USV-Akku ({akku}%, {volt}V).";
const char* DEFAULT_MSG_ON  = "ENTWARNUNG: Stromversorgung wiederhergestellt! Akku: {akku}% ({volt}V). Ausfall dauerte {dauer}.";
const char* DEFAULT_MSG_LOW = "WARNUNG: USV-Akku fast leer ({akku}%, {volt}V)! Stromausfall dauert seit {dauer}.";

Preferences prefs;
SemaphoreHandle_t cfgMutex;
String cfgNumbers;                     // kommagetrennt
String cfgMsgOff, cfgMsgOn, cfgMsgLow;
volatile uint32_t cfgOffDelayS  = 180; // so lange muss der Strom weg sein, bevor die Alarm-SMS kommt
volatile uint32_t cfgOnDelayS   = 10;  // so lange muss der Strom wieder da sein, bevor die Entwarnung kommt
volatile bool     cfgRestoreSms = true;
volatile int      cfgLowBatPct  = 0;   // 0 = aus
volatile bool     cfgSmsCmd     = true; // Befehle per SMS erlaubt

BluetoothSerial SerialBT;
HardwareSerial  Modem(1);
SemaphoreHandle_t outMutex;

// ================== Geteilter Zustand ==================
volatile bool          mainsOnline      = true;
volatile unsigned long mainsChangedAt   = 0;
volatile unsigned long outageStartedAt  = 0;
volatile bool          measurementReady = false;
volatile float         vBatShared       = 0;
volatile float         vSolarShared     = 0;
volatile int           battPctShared    = 0;
volatile bool          testRequested    = false;
volatile uint32_t      smsWaitSec       = 0;
volatile unsigned long muteUntil        = 0;   // Alarm-SMS pausiert bis (millis), 0 = keine Pause
volatile bool          rebootRequested  = false;

enum ModemState { MS_START, MS_NO_MODEM, MS_NO_SIM, MS_READY };
enum SmsState   { SMS_NONE, SMS_SENDING, SMS_SENT, SMS_FAILED };
volatile ModemState modemState = MS_START;
volatile bool       registered = false;
volatile int        signalCsq  = 99;
volatile int        lastCereg  = -1;
volatile SmsState   smsState   = SMS_NONE;

// ================== Ausgabe ==================
void sendOutput(const String& msg) {
  xSemaphoreTake(outMutex, portMAX_DELAY);
  Serial.print(msg);
  if (SerialBT.hasClient()) {
    SerialBT.print(msg);
  }
  xSemaphoreGive(outMutex);
}

void logUsb(const String& msg) {
  xSemaphoreTake(outMutex, portMAX_DELAY);
  Serial.print(msg);
  xSemaphoreGive(outMutex);
}

// ================== Konfiguration ==================
// 079... -> +4179..., 0041... -> +41... (das Modem braucht das internationale Format)
String normalizeNumber(String n) {
  n.trim();
  if (n.startsWith("00")) return "+" + n.substring(2);
  if (n.startsWith("0"))  return "+41" + n.substring(1);
  return n;
}

String sanitizeNumbers(const String& in) {
  String out;
  int count = 0;
  String cur;
  for (int i = 0; i <= (int)in.length(); i++) {
    char c = i < (int)in.length() ? in[i] : ',';
    if (c == ',' || c == ';') {
      if (cur.length() >= 3 && count < MAX_NUMBERS) {
        if (out.length()) out += ',';
        out += normalizeNumber(cur);
        count++;
      }
      cur = "";
    } else if (isdigit((unsigned char)c) || (c == '+' && cur.length() == 0)) {
      cur += c;
    }
  }
  return out;
}

String cleanText(const String& in) {
  String t = in;
  t.replace("\r", " ");
  t.replace("\n", " ");
  t.trim();
  return t.substring(0, 300);
}

void loadConfig() {
  prefs.begin("netzw", false);
  cfgNumbers    = prefs.getString("nums", SMS_TARGET_NUMBER);
  cfgMsgOff     = prefs.getString("msgoff", DEFAULT_MSG_OFF);
  cfgMsgOn      = prefs.getString("msgon", DEFAULT_MSG_ON);
  cfgMsgLow     = prefs.getString("msglow", DEFAULT_MSG_LOW);
  cfgOffDelayS  = prefs.getUInt("offdelay", 180);
  cfgOnDelayS   = prefs.getUInt("ondelay", 10);
  cfgRestoreSms = prefs.getBool("restore", true);
  cfgLowBatPct  = prefs.getInt("lowbat", 0);
  cfgSmsCmd     = prefs.getBool("smscmd", true);
}

String cfgGet(const String& which) {
  xSemaphoreTake(cfgMutex, portMAX_DELAY);
  String v = which == "NUMS"   ? cfgNumbers
           : which == "MSGOFF" ? cfgMsgOff
           : which == "MSGON"  ? cfgMsgOn
                               : cfgMsgLow;
  xSemaphoreGive(cfgMutex);
  return v;
}

int getNumbers(String* out) {
  String all = cfgGet("NUMS");
  int n = 0;
  int start = 0;
  while (n < MAX_NUMBERS && start < (int)all.length()) {
    int c = all.indexOf(',', start);
    if (c < 0) c = all.length();
    String num = all.substring(start, c);
    num.trim();
    if (num.length()) out[n++] = normalizeNumber(num);
    start = c + 1;
  }
  return n;
}

void sendConfig() {
  String s;
  s += "CFG:NUMS="     + cfgGet("NUMS")   + "\n";
  s += "CFG:MSGOFF="   + cfgGet("MSGOFF") + "\n";
  s += "CFG:MSGON="    + cfgGet("MSGON")  + "\n";
  s += "CFG:MSGLOW="   + cfgGet("MSGLOW") + "\n";
  s += "CFG:OFFDELAY=" + String((unsigned long)cfgOffDelayS) + "\n";
  s += "CFG:ONDELAY="  + String((unsigned long)cfgOnDelayS) + "\n";
  s += "CFG:RESTORE="  + String(cfgRestoreSms ? 1 : 0) + "\n";
  s += "CFG:LOWBAT="   + String((int)cfgLowBatPct) + "\n";
  s += "CFG:SMSCMD="   + String(cfgSmsCmd ? 1 : 0) + "\n";
  s += "CFG:END\n";
  sendOutput(s);
}

bool applySettingUnlocked(const String& key, const String& value) {
  if (key == "NUMS" || key == "MSGOFF" || key == "MSGON" || key == "MSGLOW") {
    String v = key == "NUMS" ? sanitizeNumbers(value) : cleanText(value);
    if (key != "NUMS" && v.length() == 0) return false;
    xSemaphoreTake(cfgMutex, portMAX_DELAY);
    if (key == "NUMS")        cfgNumbers = v;
    else if (key == "MSGOFF") cfgMsgOff  = v;
    else if (key == "MSGON")  cfgMsgOn   = v;
    else                      cfgMsgLow  = v;
    xSemaphoreGive(cfgMutex);
    String pkey = key;
    pkey.toLowerCase();
    prefs.putString(pkey.c_str(), v);
    return true;
  }
  long n = value.toInt();
  if (key == "OFFDELAY" || key == "ONDELAY") {
    if (n < 0 || n > (long)MAX_DELAY_S) return false;
    if (key == "OFFDELAY") { cfgOffDelayS = n; prefs.putUInt("offdelay", n); }
    else                   { cfgOnDelayS  = n; prefs.putUInt("ondelay", n); }
    return true;
  }
  if (key == "RESTORE") {
    cfgRestoreSms = (n != 0);
    prefs.putBool("restore", cfgRestoreSms);
    return true;
  }
  if (key == "SMSCMD") {
    cfgSmsCmd = (n != 0);
    prefs.putBool("smscmd", cfgSmsCmd);
    return true;
  }
  if (key == "LOWBAT") {
    if (n < 0 || n > 100) return false;
    cfgLowBatPct = n;
    prefs.putInt("lowbat", n);
    return true;
  }
  return false;
}

SemaphoreHandle_t prefsMutex;

bool applySetting(const String& key, const String& value) {
  xSemaphoreTake(prefsMutex, portMAX_DELAY);
  bool ok = applySettingUnlocked(key, value);
  xSemaphoreGive(prefsMutex);
  return ok;
}

void handleCommand(String cmd) {
  cmd.trim();
  if (cmd == "GET") {
    sendConfig();
  } else if (cmd == "TEST") {
    testRequested = true;
    sendOutput("[SMS] Test-SMS angefordert...\n");
  } else if (cmd.startsWith("SET:")) {
    int eq = cmd.indexOf('=');
    String key = eq > 4 ? cmd.substring(4, eq) : cmd.substring(4);
    String value = eq > 4 ? cmd.substring(eq + 1) : "";
    key.trim();
    key.toUpperCase();
    sendOutput((applySetting(key, value) ? "OK:" : "ERR:") + key + "\n");
  }
}

String btBuffer;

void handleBtInput() {
  while (SerialBT.available()) {
    char c = (char)SerialBT.read();
    if (c == '\n' || c == '\r') {
      if (btBuffer.length()) handleCommand(btBuffer);
      btBuffer = "";
    } else if (btBuffer.length() < 500) {
      btBuffer += c;
    }
  }
}

// ================== Messung ==================
int getBatteryPercent(float v) {
  if (v >= 4.18) return 100;
  if (v <= 3.30) return 0;
  if (v > 4.00) return 80 + (int)((v - 4.00) / (4.18 - 4.00) * 20.0);
  if (v > 3.80) return 50 + (int)((v - 3.80) / (4.00 - 3.80) * 30.0);
  if (v > 3.65) return 20 + (int)((v - 3.65) / (3.80 - 3.65) * 30.0);
  return (int)((v - 3.30) / (3.65 - 3.30) * 20.0);
}

float readPinVoltage(int pin, float dividerRatio) {
  uint32_t sum = 0;
  for (int i = 0; i < 30; i++) {
    sum += analogRead(pin);
    delayMicroseconds(150);
  }
  float avg = sum / 30.0;
  return (avg / 4095.0) * REF_VOLT * dividerRatio;
}

// ================== SMS-Texte ==================
String durationText(unsigned long ms) {
  unsigned long s = ms / 1000;
  if (s < 60)   return String(s) + " s";
  if (s < 3600) return String(s / 60) + " min";
  return String(s / 3600) + " h " + String((s % 3600) / 60) + " min";
}

String delayText(unsigned long s) {
  return s == 0 ? String("sofort") : durationText(s * 1000UL);
}

// Umlaute umschreiben (Modem laeuft im GSM-Zeichensatz), nur druckbare Zeichen, auf maxLen kuerzen
String toGsmText(String t, int maxLen) {
  t.replace("ä", "ae"); t.replace("ö", "oe"); t.replace("ü", "ue");
  t.replace("Ä", "Ae"); t.replace("Ö", "Oe"); t.replace("Ü", "Ue");
  t.replace("ß", "ss");
  // Zeichen, die es im GSM-Grundalphabet nicht gibt (das Modem macht sonst z.B. aus | ein oe)
  String out;
  for (int i = 0; i < (int)t.length() && (int)out.length() < maxLen; i++) {
    unsigned char c = t[i];
    switch (c) {
      case '|': case '\\':           out += '/'; break;
      case '[': case '{':             out += '('; break;
      case ']': case '}':             out += ')'; break;
      case '_':                       out += '-'; break;
      case '^': case '~': case '`':
      case '@': case '$':             break;
      default:
        if (c >= 0x20 && c < 0x7F) out += (char)c;
    }
  }
  return out;
}

// Platzhalter ersetzen und auf eine SMS (160 Zeichen) kuerzen
String buildSms(String t, unsigned long durationMs) {
  t.replace("{akku}",  String(battPctShared));
  t.replace("{volt}",  String((float)vBatShared, 2));
  t.replace("{dauer}", durationText(durationMs));
  return toGsmText(t, MAX_SMS_LEN);
}

// ================== Modem ==================
String sendAT(const String& cmd, uint32_t timeout = 2000) {
  while (Modem.available()) Modem.read();
  Modem.print(cmd + "\r\n");
  String resp;
  uint32_t t = millis();
  while (millis() - t < timeout) {
    while (Modem.available()) resp += (char)Modem.read();
    if (resp.indexOf("OK") >= 0 || resp.indexOf("ERROR") >= 0) break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  logUsb(">> " + cmd + "\n" + resp + "\n");
  return resp;
}

bool modemAnswers(int tries) {
  for (int i = 0; i < tries; i++) {
    if (sendAT("AT", 1000).indexOf("OK") >= 0) return true;
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  return false;
}

void hardPowerCycleModem() {
  pinMode(MODEM_PWRKEY_PIN, OUTPUT);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  pinMode(MODEM_DTR_PIN, OUTPUT);
  digitalWrite(MODEM_DTR_PIN, LOW);
  digitalWrite(BOARD_POWER_ON, LOW);
  vTaskDelay(pdMS_TO_TICKS(3000));

  digitalWrite(BOARD_POWER_ON, HIGH);
  vTaskDelay(pdMS_TO_TICKS(500));
  digitalWrite(MODEM_PWRKEY_PIN, HIGH);
  vTaskDelay(pdMS_TO_TICKS(1000));
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  vTaskDelay(pdMS_TO_TICKS(8000));
}

int regStatus(const String& resp, const char* tag) {
  int i = resp.indexOf(tag);
  if (i < 0) return -1;
  int e = resp.indexOf('\n', i);
  String line = resp.substring(i, e < 0 ? resp.length() : e);
  int c = line.indexOf(',');
  if (c < 0) return -1;
  String rest = line.substring(c + 1);
  int c2 = rest.indexOf(',');
  if (c2 >= 0) rest = rest.substring(0, c2);
  rest.trim();
  return rest.toInt();
}

bool isRegistered() {
  int creg  = regStatus(sendAT("AT+CREG?"),  "+CREG:");
  int cereg = regStatus(sendAT("AT+CEREG?"), "+CEREG:");
  lastCereg = cereg;
  return (creg == 1 || creg == 5 || cereg == 1 || cereg == 5);
}

void updateSignal() {
  String r = sendAT("AT+CSQ");
  int i = r.indexOf("+CSQ:");
  if (i >= 0) signalCsq = r.substring(i + 5).toInt();
}

void nudgeNetwork() {
  sendOutput("[MODEM] Einbuchen haengt (Status " + String((int)lastCereg) +
             "), setze Funk zurueck und waehle Sunrise...\n");
  sendAT("AT+CFUN=0", 10000);
  vTaskDelay(pdMS_TO_TICKS(3000));
  sendAT("AT+CFUN=1", 10000);
  vTaskDelay(pdMS_TO_TICKS(5000));
  sendAT("AT+COPS=4,2,\"22802\",7", 90000);
}

bool waitForSim() {
  bool pinTried = false;
  for (int i = 0; i < 30; i++) {
    String r = sendAT("AT+CPIN?");
    if (r.indexOf("READY") >= 0) return true;
    if (r.indexOf("SIM PIN") >= 0 && !pinTried) {
      pinTried = true;
      String p = sendAT(String("AT+CPIN=") + SIM_PIN, 5000);
      if (p.indexOf("OK") < 0) {
        sendOutput("[MODEM] !! PIN falsch - nicht wiederholt neu starten (PUK-Sperre)!\n");
        return false;
      }
      vTaskDelay(pdMS_TO_TICKS(3000));
      sendAT(String("AT+CLCK=\"SC\",0,\"") + SIM_PIN + "\"", 5000);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  return false;
}

bool initModem() {
  modemState = MS_START;
  registered = false;
  sendOutput("[MODEM] Starte A7670G...\n");
  hardPowerCycleModem();

  if (!modemAnswers(30)) {
    modemState = MS_NO_MODEM;
    sendOutput("[MODEM] !! Modem antwortet nicht.\n");
    return false;
  }
  sendAT("ATE0");
  sendAT("AT+CMEE=2");

  if (!waitForSim()) {
    modemState = MS_NO_SIM;
    sendOutput("[MODEM] !! SIM nicht bereit.\n");
    return false;
  }

  sendAT("AT+CNMP=38");
  sendAT("AT+COPS=0", 10000);
  sendAT("AT+CMGF=1");
  sendAT("AT+CSCS=\"GSM\"");
  sendAT("AT+CNMI=2,1,0,0,0");   // eingehende SMS speichern (werden per AT+CMGL abgeholt)
  String sc = sendAT("AT+CSCA?");
  if (sc.indexOf("+41") < 0) sendAT(String("AT+CSCA=\"") + SMSC + "\",145");

  modemState = MS_READY;
  sendOutput("[MODEM] Registriere im LTE-Netz...\n");
  for (int i = 0; i < 90 && !registered; i++) {
    registered = isRegistered();
    if (!registered) {
      if (i == 15 || i == 50) nudgeNetwork();
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
  }
  updateSignal();
  sendOutput(registered ? "[MODEM] Eingebucht und sendebereit!\n"
                        : "[MODEM] !! Noch nicht eingebucht, versuche weiter...\n");
  return true;
}

String compactResp(String r) {
  r.replace("\r", " ");
  r.replace("\n", " ");
  r.replace(">", "");
  r.trim();
  return r.substring(0, 80);
}

bool sendSMS(const String& number, const String& text) {
  if (!registered) {
    sendOutput("[SMS] !! Nicht im Mobilnetz eingebucht (Status " + String((int)lastCereg) +
               ", Signal " + String((int)signalCsq) + ").\n");
    return false;
  }

  sendAT("AT+CMGF=1");
  sendAT("AT+CSCS=\"GSM\"");
  while (Modem.available()) Modem.read();
  Modem.print(String("AT+CMGS=\"") + number + "\"\r");

  String r;
  uint32_t t = millis();
  while (millis() - t < 10000) {
    while (Modem.available()) r += (char)Modem.read();
    if (r.indexOf('>') >= 0 || r.indexOf("ERROR") >= 0) break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (r.indexOf('>') < 0) {
    logUsb("[SMS] Kein '>' Prompt: " + r + "\n");
    sendOutput("[SMS] !! Modem nimmt SMS an " + number + " nicht an: " + compactResp(r) + "\n");
    Modem.write(0x1B);  // eventuell offene Eingabe abbrechen
    return false;
  }

  Modem.print(text);
  vTaskDelay(pdMS_TO_TICKS(100));
  Modem.write(0x1A);

  r = "";
  t = millis();
  while (millis() - t < 60000) {
    while (Modem.available()) r += (char)Modem.read();
    if (r.indexOf("+CMGS") >= 0 && r.indexOf("OK") >= 0) break;
    if (r.indexOf("ERROR") >= 0) break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  logUsb("[SMS] Antwort: " + r + "\n");
  if (r.indexOf("+CMGS") < 0) {
    sendOutput("[SMS] !! Modem-Antwort fuer " + number + ": " +
               (r.length() ? compactResp(r) : String("keine (Timeout)")) + "\n");
    return false;
  }
  return true;
}

// Bitmaske aller aktuell eingestellten Empfaenger
uint8_t allRecipientsMask() {
  String nums[MAX_NUMBERS];
  return (uint8_t)((1 << getNumbers(nums)) - 1);
}

// Sendet an alle Empfaenger in "mask", gibt die Maske der noch fehlgeschlagenen zurueck
uint8_t sendToRecipients(const String& text, uint8_t mask) {
  String nums[MAX_NUMBERS];
  int n = getNumbers(nums);
  mask &= (uint8_t)((1 << n) - 1);
  for (int i = 0; i < n; i++) {
    if (!(mask & (1 << i))) continue;
    sendOutput("[SMS] An " + nums[i] + ": " + text + "\n");
    if (sendSMS(nums[i], text)) mask &= ~(1 << i);
  }
  return mask;
}

// ================== SMS-Befehle (Fernsteuerung) ==================
bool isMuted() {
  unsigned long until = muteUntil;
  return until != 0 && (long)(until - millis()) > 0;
}

// Letzte 9 Ziffern: +41797463235, 0797463235 und 0041797463235 gelten als gleich
String numberTail(const String& n) {
  String d;
  for (int i = 0; i < (int)n.length(); i++) if (isdigit((unsigned char)n[i])) d += n[i];
  return d.length() > 9 ? d.substring(d.length() - 9) : d;
}

bool isAuthorized(const String& sender) {
  String s = numberTail(sender);
  if (s.length() < 9) return false;
  if (s == numberTail(SMS_TARGET_NUMBER)) return true;
  String nums[MAX_NUMBERS];
  int n = getNumbers(nums);
  for (int i = 0; i < n; i++) if (numberTail(nums[i]) == s) return true;
  return false;
}

// "30s", "5m", "5 min", "2h", "1d", "sofort"; ohne Einheit = Minuten
bool parseDuration(String s, unsigned long& out) {
  s.trim();
  s.toLowerCase();
  if (s == "sofort" || s == "0") { out = 0; return true; }
  int i = 0;
  while (i < (int)s.length() && isdigit((unsigned char)s[i])) i++;
  if (i == 0) return false;
  unsigned long v = s.substring(0, i).toInt();
  String u = s.substring(i);
  u.trim();
  unsigned long mult;
  if (u.length() == 0 || u.startsWith("m"))           mult = 60;
  else if (u.startsWith("std") || u.startsWith("h"))  mult = 3600;
  else if (u.startsWith("s"))                         mult = 1;
  else if (u.startsWith("d") || u.startsWith("t"))    mult = 86400;
  else return false;
  out = v * mult;
  return true;
}

// Antwort an den Absender, bei Bedarf auf mehrere SMS verteilt
void sendReply(const String& number, const String& text) {
  String t = toGsmText(text, 3 * MAX_SMS_LEN);
  for (int i = 0; i < (int)t.length(); i += MAX_SMS_LEN) {
    sendSMS(number, t.substring(i, i + MAX_SMS_LEN));
  }
}

String numbersText() {
  String nums[MAX_NUMBERS];
  int n = getNumbers(nums);
  if (n == 0) return "keine";
  String s;
  for (int i = 0; i < n; i++) s += (i ? ", " : "") + String(i + 1) + ") " + nums[i];
  return s;
}

String statusText() {
  String s = String("Netz ") + (mainsOnline ? "ONLINE" : "STROMAUSFALL") +
             ", Netzteil " + String((float)vSolarShared, 2) + "V" +
             ", Akku " + String(battPctShared) + "% (" + String((float)vBatShared, 2) + "V)" +
             ", Signal " + String((int)signalCsq) +
             ", Laufzeit " + durationText(millis());
  if (!mainsOnline) s += ", Ausfall seit " + durationText(millis() - outageStartedAt);
  if (isMuted())    s += ", Pause noch " + durationText(muteUntil - millis());
  return s;
}

String configText() {
  return "Alarm nach " + delayText(cfgOffDelayS) +
         ", Entwarnung " + (cfgRestoreSms ? String("an, nach " + delayText(cfgOnDelayS)) : String("aus")) +
         ", Akku-Warnung " + (cfgLowBatPct > 0 ? String("unter " + String((int)cfgLowBatPct) + "%") : String("aus")) +
         ", Pause " + (isMuted() ? String("noch " + durationText(muteUntil - millis())) : String("aus")) +
         ", Empfaenger: " + numbersText();
}

String runSmsCommand(const String& body) {
  String line = cleanText(body).substring(1);   // fuehrendes "/" weg
  line.trim();
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  cmd.toLowerCase();
  arg.trim();
  String argL = arg;
  argL.toLowerCase();
  unsigned long sec;

  if (cmd == "hilfe" || cmd == "help" || cmd == "?") {
    return "Befehle: /status, /einstellungen, /alarm 5m, /entwarnung an/aus/30s, /akku 20/aus, "
           "/text alarm/entwarnung/akku <Text>, /nummer liste/neu/weg, /pause 2h/aus, /test, /neustart";
  }
  if (cmd == "status") return statusText();
  if (cmd == "einstellungen" || cmd == "config") return configText();

  if (cmd == "alarm") {
    if (!parseDuration(arg, sec) || sec > MAX_DELAY_S) return "Fehler: z.B. /alarm 3m (sofort bis 60m)";
    applySetting("OFFDELAY", String(sec));
    return "OK: Alarm-SMS nach " + delayText(sec) + " Stromausfall.";
  }

  if (cmd == "entwarnung") {
    if (argL == "an" || argL == "ein" || argL == "on") {
      applySetting("RESTORE", "1");
      return "OK: Entwarnungs-SMS an.";
    }
    if (argL == "aus" || argL == "off") {
      applySetting("RESTORE", "0");
      return "OK: Entwarnungs-SMS aus.";
    }
    if (!parseDuration(arg, sec) || sec > MAX_DELAY_S) return "Fehler: /entwarnung an, /entwarnung aus oder /entwarnung 30s";
    applySetting("ONDELAY", String(sec));
    applySetting("RESTORE", "1");
    return "OK: Entwarnung, wenn Strom " + delayText(sec) + " stabil da ist.";
  }

  if (cmd == "akku") {
    if (argL == "aus" || argL == "off" || argL == "0") {
      applySetting("LOWBAT", "0");
      return "OK: Akku-Warnung aus.";
    }
    int pct = argL.toInt();
    if (pct < 1 || pct > 100) return "Fehler: z.B. /akku 20 oder /akku aus";
    applySetting("LOWBAT", String(pct));
    return "OK: Akku-Warnung unter " + String(pct) + "%.";
  }

  if (cmd == "text") {
    int sp2 = arg.indexOf(' ');
    String which = sp2 < 0 ? argL : argL.substring(0, sp2);
    String text = sp2 < 0 ? "" : arg.substring(sp2 + 1);
    text.trim();
    if (which == "standard" || which == "reset") {
      applySetting("MSGOFF", DEFAULT_MSG_OFF);
      applySetting("MSGON",  DEFAULT_MSG_ON);
      applySetting("MSGLOW", DEFAULT_MSG_LOW);
      return "OK: Alle SMS-Texte auf Standard zurueckgesetzt.";
    }
    String key = which == "alarm"      ? "MSGOFF"
               : which == "entwarnung" ? "MSGON"
               : which == "akku"       ? "MSGLOW" : "";
    if (key.length() == 0) return "Fehler: /text alarm, /text entwarnung oder /text akku <Text>, /text standard";
    if (text.length() == 0) return "Aktueller Text (" + which + "): " + cfgGet(key);
    // Im GSM-Zeichensatz kommen { } als Escape-Folge an und werden zu ( ) -> zurueckwandeln
    text = toGsmText(text, 300);
    const char* ph[] = {"akku", "volt", "dauer"};
    for (const char* p : ph) {
      text.replace(String("(") + p + ")", String("{") + p + "}");
      text.replace(String("[") + p + "]", String("{") + p + "}");
    }
    applySetting(key, text);
    return "OK: Neuer Text (" + which + "): " + cfgGet(key);
  }

  if (cmd == "nummer" || cmd == "nummern") {
    int sp2 = argL.indexOf(' ');
    String sub = sp2 < 0 ? argL : argL.substring(0, sp2);
    String val = sp2 < 0 ? "" : arg.substring(sp2 + 1);
    val.trim();
    String nums[MAX_NUMBERS];
    int n = getNumbers(nums);

    if (sub == "" || sub == "liste" || sub == "list") return "Empfaenger: " + numbersText();

    if (sub == "neu" || sub == "add" || sub == "+") {
      String num = sanitizeNumbers(val);
      if (num.length() == 0 || num.indexOf(',') >= 0) return "Fehler: z.B. /nummer neu +41791234567";
      for (int i = 0; i < n; i++) if (numberTail(nums[i]) == numberTail(num)) return "Nummer ist schon eingetragen.";
      if (n >= MAX_NUMBERS) return "Fehler: maximal " + String(MAX_NUMBERS) + " Empfaenger.";
      applySetting("NUMS", cfgGet("NUMS") + "," + num);
      return "OK: Empfaenger: " + numbersText();
    }

    if (sub == "weg" || sub == "del" || sub == "-" || sub == "loeschen") {
      int pos = -1;
      if (val.length() > 0 && val.length() <= 1) pos = val.toInt() - 1;   // Nummer per Platz (1-5)
      String newList;
      bool removed = false;
      for (int i = 0; i < n; i++) {
        bool match = (i == pos) || (pos < 0 && numberTail(nums[i]) == numberTail(val));
        if (match && !removed) { removed = true; continue; }
        if (newList.length()) newList += ',';
        newList += nums[i];
      }
      if (!removed) return "Nummer nicht gefunden. Empfaenger: " + numbersText();
      applySetting("NUMS", newList);
      return "OK: Empfaenger: " + numbersText() +
             (newList.length() ? String("") : String(" - ACHTUNG: keine Alarm-SMS mehr!"));
    }
    return "Fehler: /nummer liste, /nummer neu <Nr>, /nummer weg <Nr oder Platz>";
  }

  if (cmd == "pause") {
    if (argL == "aus" || argL == "off" || argL == "0") {
      muteUntil = 0;
      return "OK: Pause beendet, Alarm-SMS sind wieder aktiv.";
    }
    if (!parseDuration(arg, sec) || sec == 0 || sec > MAX_PAUSE_S) return "Fehler: z.B. /pause 2h (max. 7d) oder /pause aus";
    unsigned long until = millis() + sec * 1000UL;
    muteUntil = until == 0 ? 1 : until;
    return "OK: Alarm-SMS pausiert fuer " + durationText(sec * 1000UL) + ". Mit /pause aus beenden.";
  }

  if (cmd == "test") {
    testRequested = true;
    return "OK: Test-SMS geht an alle Empfaenger.";
  }

  if (cmd == "neustart" || cmd == "reboot") {
    rebootRequested = true;
    return "OK: Netzwaechter startet neu (ca. 1-3 min bis wieder im Netz).";
  }

  return "Unbekannter Befehl: /" + cmd + ". Sende /hilfe fuer alle Befehle.";
}

void handleIncomingSms(const String& sender, const String& body) {
  if (!body.startsWith("/")) {
    sendOutput("[SMS] Empfangen von " + sender + " (kein Befehl): " + toGsmText(body, 60) + "\n");
    return;
  }
  if (!cfgSmsCmd) {
    sendOutput("[SMS] !! Befehl von " + sender + " ignoriert (SMS-Befehle sind aus).\n");
    return;
  }
  if (!isAuthorized(sender)) {
    // Keine Antwort an fremde Nummern
    sendOutput("[SMS] !! Befehl von unbekannter Nummer " + sender + " ignoriert.\n");
    return;
  }
  sendOutput("[SMS] Befehl von " + sender + ": " + toGsmText(body, 80) + "\n");
  String reply = runSmsCommand(body);
  sendOutput("[SMS] Antwort an " + sender + ": " + toGsmText(reply, 120) + "\n");
  sendReply(sender, reply);
}

// Liest auf "OK"/"ERROR" am Ende, damit "OK" im SMS-Text die Antwort nicht abschneidet
String readModemFinal(uint32_t timeout) {
  String r;
  uint32_t t = millis();
  uint32_t lastRx = millis();
  while (millis() - t < timeout) {
    bool got = false;
    while (Modem.available()) { r += (char)Modem.read(); got = true; }
    if (got) lastRx = millis();
    if ((r.endsWith("\r\nOK\r\n") || r.endsWith("ERROR\r\n")) && millis() - lastRx > 150) break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return r;
}

void checkIncomingSms() {
  const int MAX_IN = 8;
  int    idx[MAX_IN];
  String from[MAX_IN];
  String body[MAX_IN];
  int count = 0;

  while (Modem.available()) Modem.read();
  Modem.print("AT+CMGL=\"ALL\"\r\n");
  String r = readModemFinal(8000);
  if (r.indexOf("+CMGL:") < 0) return;
  logUsb(">> AT+CMGL=\"ALL\"\n" + r + "\n");

  // +CMGL: 3,"REC UNREAD","+41791234567","","26/10/28,19:00:00+04"\r\n<Text>\r\n
  int pos = 0;
  while (count < MAX_IN) {
    int h = r.indexOf("+CMGL:", pos);
    if (h < 0) break;
    int eol = r.indexOf('\n', h);
    if (eol < 0) break;
    String header = r.substring(h, eol);
    int q[4];
    int qp = -1;
    bool ok = true;
    for (int k = 0; k < 4; k++) {
      qp = header.indexOf('"', qp + 1);
      if (qp < 0) { ok = false; break; }
      q[k] = qp;
    }
    int next = r.indexOf("+CMGL:", eol);
    int end = next >= 0 ? next : r.lastIndexOf("OK");
    if (end < eol) end = r.length();
    String b = r.substring(eol + 1, end);
    b.trim();

    idx[count]  = header.substring(6).toInt();
    from[count] = ok ? header.substring(q[2] + 1, q[3]) : "";
    body[count] = b;
    count++;
    pos = eol;
  }

  // Erst loeschen, dann ausfuehren: ein /neustart darf nach dem Reboot nicht nochmal laufen
  for (int i = 0; i < count; i++) sendAT("AT+CMGD=" + String(idx[i]), 5000);
  for (int i = 0; i < count; i++) handleIncomingSms(from[i], body[i]);
}

void modemTask(void*) {
  Modem.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

  while (!measurementReady) vTaskDelay(pdMS_TO_TICKS(100));
  // Annahme: vor dem Start war Strom da. Startet das Board waehrend eines
  // Ausfalls neu, geht trotzdem die Alarm-SMS raus.
  bool notifiedState = true;
  bool pendingFor = true;
  uint8_t pendingMask = 0;   // Empfaenger, die die aktuelle Meldung noch nicht haben
  bool lowBatDone = false;
  uint8_t lowBatMask = 0;
  unsigned long lastFailAt = 0;
  bool hadFailure = false;
  unsigned long lastCheck = 0;
  unsigned long lastSmsPoll = 0;
  int lostCount = 0;

  for (;;) {
    if (modemState != MS_READY) {
      smsWaitSec = 0;
      if (!initModem()) { vTaskDelay(pdMS_TO_TICKS(30000)); continue; }
      lostCount = 0;
      lastCheck = millis();
    }

    bool current = mainsOnline;
    bool retryOk = !hadFailure || (millis() - lastFailAt >= SMS_RETRY_MS);
    unsigned long needMs = (current ? cfgOnDelayS : cfgOffDelayS) * 1000UL;
    unsigned long sinceChange = millis() - mainsChangedAt;

    if (current != notifiedState) {
      if (pendingFor != current) {
        pendingFor = current;
        pendingMask = 0;
        hadFailure = false;
        retryOk = true;
      }
      if (sinceChange < needMs) {
        // Kurze Unterbrechungen: erst melden, wenn der Zustand lange genug anhaelt
        smsWaitSec = (needMs - sinceChange + 999) / 1000;
      } else {
        smsWaitSec = 0;
        if (current && !cfgRestoreSms) {
          notifiedState = current;
          sendOutput("[SMS] Entwarnungs-SMS ist deaktiviert.\n");
        } else if (isMuted()) {
          notifiedState = current;
          sendOutput("[SMS] Pause aktiv - keine SMS fuer diesen Wechsel.\n");
        } else if (retryOk) {
          if (pendingMask == 0) pendingMask = allRecipientsMask();
          if (pendingMask == 0) {
            notifiedState = current;
            sendOutput("[SMS] !! Keine Empfaenger eingestellt.\n");
          } else {
            String msg = current
              ? buildSms(cfgGet("MSGON"),  mainsChangedAt - outageStartedAt)
              : buildSms(cfgGet("MSGOFF"), millis() - outageStartedAt);

            smsState = SMS_SENDING;
            pendingMask = sendToRecipients(msg, pendingMask);
            if (pendingMask == 0) {
              notifiedState = current;
              hadFailure = false;
              smsState = SMS_SENT;
              sendOutput("[SMS] Erfolgreich gesendet!\n");
            } else {
              hadFailure = true;
              lastFailAt = millis();
              smsState = SMS_FAILED;
              sendOutput("[SMS] !! Fehler, neuer Versuch in 60 s.\n");
            }
          }
        }
      }
    } else {
      smsWaitSec = 0;
      pendingFor = current;
      pendingMask = 0;
    }

    // Akku-Warnung waehrend eines gemeldeten Stromausfalls (einmal pro Ausfall)
    if (current) {
      lowBatDone = false;
      lowBatMask = 0;
    } else if (cfgLowBatPct > 0 && !notifiedState && !lowBatDone && !isMuted() &&
               battPctShared <= cfgLowBatPct && retryOk) {
      if (lowBatMask == 0) lowBatMask = allRecipientsMask();
      if (lowBatMask == 0) {
        lowBatDone = true;
      } else {
        smsState = SMS_SENDING;
        lowBatMask = sendToRecipients(buildSms(cfgGet("MSGLOW"), millis() - outageStartedAt), lowBatMask);
        if (lowBatMask == 0) {
          lowBatDone = true;
          hadFailure = false;
          smsState = SMS_SENT;
          sendOutput("[SMS] Akku-Warnung gesendet!\n");
        } else {
          hadFailure = true;
          lastFailAt = millis();
          smsState = SMS_FAILED;
          sendOutput("[SMS] !! Akku-Warnung fehlgeschlagen, neuer Versuch in 60 s.\n");
        }
      }
    }

    if (testRequested) {
      testRequested = false;
      uint8_t mask = allRecipientsMask();
      if (mask == 0) {
        sendOutput("[SMS] !! Test: keine Empfaenger eingestellt.\n");
      } else {
        String msg = buildSms(String("TEST vom Netzwaechter: Netz ") +
                              (current ? "ONLINE" : "AUSGEFALLEN") +
                              ", Akku {akku}% ({volt}V).", 0);
        smsState = SMS_SENDING;
        uint8_t left = sendToRecipients(msg, mask);
        smsState = left == 0 ? SMS_SENT : SMS_FAILED;
        sendOutput(left == 0 ? "[SMS] Test-SMS erfolgreich gesendet!\n"
                             : "[SMS] !! Test-SMS fehlgeschlagen.\n");
      }
    }

    if (registered && millis() - lastSmsPoll >= SMS_POLL_MS) {
      lastSmsPoll = millis();
      checkIncomingSms();
    }

    if (rebootRequested) {
      sendOutput("[BOOT] Neustart per SMS-Befehl...\n");
      vTaskDelay(pdMS_TO_TICKS(2000));
      ESP.restart();
    }

    if (millis() - lastCheck >= 30000) {
      lastCheck = millis();
      registered = isRegistered();
      updateSignal();
      if (registered) {
        lostCount = 0;
      } else {
        lostCount++;
        if (lostCount == 2 || lostCount == 6) nudgeNetwork();
        if (lostCount >= 10) {
          sendOutput("[MODEM] Netz seit ~5 min weg - starte Modem neu.\n");
          modemState = MS_START;
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

String modemStatusText() {
  switch (modemState) {
    case MS_START:    return "START";
    case MS_NO_MODEM: return "FEHLER";
    case MS_NO_SIM:   return "KEINE SIM";
    default:          return registered ? "OK (" + String((int)signalCsq) + ")"
                                        : "SUCHE NETZ (Status " + String((int)lastCereg) +
                                          ", Signal " + String((int)signalCsq) + ")";
  }
}

String smsStatusText() {
  if (smsState != SMS_SENDING && isMuted()) {
    return "PAUSE " + durationText(muteUntil - millis());
  }
  if (smsState != SMS_SENDING && smsWaitSec > 0) {
    return "WARTE " + String((unsigned long)smsWaitSec) + "s";
  }
  switch (smsState) {
    case SMS_SENDING: return "SENDE";
    case SMS_SENT:    return "GESENDET";
    case SMS_FAILED:  return "FEHLER";
    default:          return "-";
  }
}

String resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "Strom eingeschaltet";
    case ESP_RST_SW:       return "Software / Upload";
    case ESP_RST_PANIC:    return "Absturz (Panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "Watchdog";
    case ESP_RST_BROWNOUT: return "Spannungseinbruch (Brownout)";
    case ESP_RST_EXT:      return "Reset-Taste";
    default:               return "anderer (" + String((int)esp_reset_reason()) + ")";
  }
}

// ================== Main ==================
unsigned long lastSend = 0;
unsigned long changeSince = 0;
bool pendingChange = false;
float vSolarAvg = 0, vBatAvg = 0;
bool filtersReady = false;

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  setCpuFrequencyMhz(160);

  pinMode(BOARD_POWER_ON, OUTPUT);
  digitalWrite(BOARD_POWER_ON, HIGH);

  Serial.begin(115200);
  delay(200);
  outMutex = xSemaphoreCreateMutex();
  cfgMutex = xSemaphoreCreateMutex();
  prefsMutex = xSemaphoreCreateMutex();
  loadConfig();

  SerialBT.begin(BT_DEVICE_NAME);

  pinMode(BAT_ADC_PIN, INPUT);
  pinMode(SOLAR_ADC_PIN, INPUT);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  sendOutput("\n=== NETZWAECHTER MIT SMS-ALARM GESTARTET ===\n");
  sendOutput("[BOOT] Neustart-Grund: " + resetReasonText() + "\n");
  sendOutput("[BOOT] Empfaenger: " + cfgGet("NUMS") + " | Alarm nach " +
             String((unsigned long)cfgOffDelayS) + " s\n");

  xTaskCreatePinnedToCore(modemTask, "modem", 10240, nullptr, 1, nullptr, 0);
}

void loop() {
  handleBtInput();

  float vSolarRaw = readPinVoltage(SOLAR_ADC_PIN, SOLAR_DIVIDER);
  float vBatRaw   = readPinVoltage(BAT_ADC_PIN, BAT_DIVIDER);

  if (!filtersReady) {
    vSolarAvg = vSolarRaw;
    vBatAvg   = vBatRaw;
    mainsOnline = (vSolarRaw > MAINS_ON_VOLT);
    mainsChangedAt = millis();
    outageStartedAt = millis();
    filtersReady = true;
  } else {
    vSolarAvg += SOLAR_ALPHA * (vSolarRaw - vSolarAvg);
    vBatAvg   += BAT_ALPHA   * (vBatRaw   - vBatAvg);
  }

  int percent = getBatteryPercent(vBatAvg);
  vBatShared = vBatAvg;
  vSolarShared = vSolarAvg;
  battPctShared = percent;
  measurementReady = true;

  bool wantChange = mainsOnline ? (vSolarAvg < MAINS_OFF_VOLT)
                                : (vSolarAvg > MAINS_ON_VOLT);
  if (wantChange) {
    if (!pendingChange) {
      pendingChange = true;
      changeSince = millis();
    } else if (millis() - changeSince >= (mainsOnline ? MAINS_OFF_MS : MAINS_ON_MS)) {
      mainsOnline = !mainsOnline;
      mainsChangedAt = millis();
      pendingChange = false;
      lastSend = 0;
      if (!mainsOnline) {
        outageStartedAt = millis();
        sendOutput("\n[ALARM] 230V-NETZ AUSGEFALLEN! Notbetrieb ueber 18650-Akku!\n");
      } else {
        sendOutput("\n[OK] STROM WIEDER DA! Normalbetrieb aktiv.\n");
      }
    }
  } else {
    pendingChange = false;
  }

  if (lastSend == 0 || millis() - lastSend >= 1500) {
    lastSend = millis();
    String line = "Netz: [" + String(mainsOnline ? "ONLINE" : "STROMAUSFALL") +
                  "] | Netzteil-In: " + String(vSolarAvg, 2) + "V" +
                  " | Akku: " + String(percent) + "% (" + String(vBatAvg, 2) + "V)" +
                  " | Modem: " + modemStatusText() +
                  " | SMS: " + smsStatusText() + "\n";
    sendOutput(line);
  }
}
