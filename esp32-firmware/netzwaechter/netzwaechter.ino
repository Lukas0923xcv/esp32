/*
  LilyGO-Netzwaechter – LilyGO T-A7670G R2
  - Erkennt Stromausfall am JST-Netzteil-Eingang (Hysterese + Entprellung)
  - Sendet den Status alle 1,5 s per Bluetooth an die Android-App
  - Sendet bei Stromausfall / Wiederkehr eine SMS (A7670G, LTE)
  - Empfaenger, SMS-Texte und Wartezeiten sind per App einstellbar (im Flash gespeichert)
  Das Modem laeuft in einem eigenen Task auf Core 0, damit die App nie einfriert.

  Bluetooth-Befehle (eine Zeile, mit \n abgeschlossen):
    GET              -> sendet alle Einstellungen als "CFG:KEY=WERT" Zeilen, Ende "CFG:END"
    SET:KEY=WERT     -> speichert eine Einstellung, Antwort "OK:KEY" oder "ERR:KEY"
    TEST             -> sendet eine Test-SMS an alle Empfaenger
  Keys: NUMS, MSGOFF, MSGON, MSGLOW, OFFDELAY, ONDELAY, RESTORE, LOWBAT
  Platzhalter in SMS-Texten: {akku} {volt} {dauer}
*/

#include <Arduino.h>
#include <Preferences.h>
#include "BluetoothSerial.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#define SMS_TARGET_NUMBER "+41797463235"   // Standard-Empfaenger, bis in der App etwas anderes gespeichert wird
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

BluetoothSerial SerialBT;
HardwareSerial  Modem(1);
SemaphoreHandle_t outMutex;

// ================== Geteilter Zustand ==================
volatile bool          mainsOnline      = true;
volatile unsigned long mainsChangedAt   = 0;
volatile unsigned long outageStartedAt  = 0;
volatile bool          measurementReady = false;
volatile float         vBatShared       = 0;
volatile int           battPctShared    = 0;
volatile bool          testRequested    = false;
volatile uint32_t      smsWaitSec       = 0;

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
String sanitizeNumbers(const String& in) {
  String out;
  int count = 0;
  String cur;
  for (int i = 0; i <= (int)in.length(); i++) {
    char c = i < (int)in.length() ? in[i] : ',';
    if (c == ',' || c == ';') {
      if (cur.length() >= 3 && count < MAX_NUMBERS) {
        if (out.length()) out += ',';
        out += cur;
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
    if (num.length()) out[n++] = num;
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
  s += "CFG:END\n";
  sendOutput(s);
}

bool applySetting(const String& key, const String& value) {
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
  if (key == "LOWBAT") {
    if (n < 0 || n > 100) return false;
    cfgLowBatPct = n;
    prefs.putInt("lowbat", n);
    return true;
  }
  return false;
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

// Platzhalter ersetzen, Umlaute umschreiben (Modem laeuft im GSM-Zeichensatz), auf 160 Zeichen kuerzen
String buildSms(String t, unsigned long durationMs) {
  t.replace("{akku}",  String(battPctShared));
  t.replace("{volt}",  String((float)vBatShared, 2));
  t.replace("{dauer}", durationText(durationMs));
  t.replace("ä", "ae"); t.replace("ö", "oe"); t.replace("ü", "ue");
  t.replace("Ä", "Ae"); t.replace("Ö", "Oe"); t.replace("Ü", "Ue");
  t.replace("ß", "ss");
  String out;
  for (int i = 0; i < (int)t.length() && (int)out.length() < MAX_SMS_LEN; i++) {
    unsigned char c = t[i];
    if (c >= 0x20 && c < 0x7F) out += (char)c;
  }
  return out;
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

bool sendSMS(const String& number, const String& text) {
  if (!registered) return false;

  sendAT("AT+CMGF=1");
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
  return r.indexOf("+CMGS") >= 0;
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
    } else if (cfgLowBatPct > 0 && !notifiedState && !lowBatDone &&
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
