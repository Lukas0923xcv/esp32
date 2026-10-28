/*
  LilyGO-Netzwaechter – LilyGO T-A7670G R2
  - Erkennt Stromausfall am JST-Netzteil-Eingang (Hysterese + Entprellung)
  - Sendet den Status alle 1,5 s per Bluetooth an die Android-App
  - Sendet bei Stromausfall / Wiederkehr eine SMS (A7670G, LTE)

  Das Modem laeuft in einem eigenen Task auf Core 0. Dadurch blockiert ein
  langsames Modem (Einbuchen, SMS-Versand bis 60 s) nie die Bluetooth-Ausgabe.

  Vor dem Hochladen: secrets.example.h nach secrets.h kopieren und ausfuellen.
*/

#include <Arduino.h>
#include "BluetoothSerial.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "secrets.h"   // SMS_TARGET_NUMBER, SIM_PIN

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

// Netzerkennung mit Hysterese + Entprellung
const float         MAINS_OFF_VOLT = 2.5;   // darunter gilt Netz als weg
const float         MAINS_ON_VOLT  = 3.0;   // darueber gilt Netz als da
const unsigned long MAINS_OFF_MS   = 400;   // so lange muss Spannung niedrig bleiben
const unsigned long MAINS_ON_MS    = 1000;  // so lange muss Spannung hoch bleiben

// SMS erst senden, wenn der neue Zustand so lange stabil ist (verhindert SMS-Flut)
const unsigned long SMS_CONFIRM_MS = 3000;
const unsigned long SMS_RETRY_MS   = 60000;

// Glaettung (exponentieller Mittelwert), kleiner = ruhiger
const float SOLAR_ALPHA = 0.30;
const float BAT_ALPHA   = 0.10;

BluetoothSerial SerialBT;
HardwareSerial  Modem(1);
SemaphoreHandle_t outMutex;

// ================== Geteilter Zustand (Loop <-> Modem-Task) ==================
volatile bool          mainsOnline     = true;
volatile unsigned long mainsChangedAt  = 0;
volatile bool          measurementReady = false;
volatile float         vBatShared      = 0;
volatile int           battPctShared   = 0;

enum ModemState { MS_START, MS_NO_MODEM, MS_NO_SIM, MS_READY };
enum SmsState   { SMS_NONE, SMS_SENDING, SMS_SENT, SMS_FAILED };
volatile ModemState modemState = MS_START;
volatile bool       registered = false;
volatile int        signalCsq  = 99;
volatile int        lastCereg  = -1;
volatile SmsState   smsState   = SMS_NONE;

// ================== Ausgabe ==================
void sendOutput(const String& msg) {          // USB + Bluetooth (App)
  xSemaphoreTake(outMutex, portMAX_DELAY);
  Serial.print(msg);
  if (SerialBT.hasClient()) {
    SerialBT.print(msg);
  }
  xSemaphoreGive(outMutex);
}

void logUsb(const String& msg) {              // nur USB (AT-Debug)
  xSemaphoreTake(outMutex, portMAX_DELAY);
  Serial.print(msg);
  xSemaphoreGive(outMutex);
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

// ================== Modem (nur im Modem-Task benutzen) ==================
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

// Kompletter Strom-Zyklus, wie Ausstecken (sonst startet das Modem nach Reset oft nicht)
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

// Haengt das Modem in "nur Notruf" (Status 11) oder sucht zu lange:
// Funk aus/ein und Sunrise direkt waehlen (Fallback: automatisch).
void nudgeNetwork() {
  sendOutput("[MODEM] Einbuchen haengt (Status " + String((int)lastCereg) +
             "), setze Funk zurueck und waehle Sunrise...\n");
  sendAT("AT+CFUN=0", 10000);
  vTaskDelay(pdMS_TO_TICKS(3000));
  sendAT("AT+CFUN=1", 10000);
  vTaskDelay(pdMS_TO_TICKS(5000));
  sendAT("AT+COPS=4,2,\"22802\",7", 90000);   // Sunrise LTE, sonst automatisch
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
      sendAT(String("AT+CLCK=\"SC\",0,\"") + SIM_PIN + "\"", 5000);  // PIN dauerhaft aus
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

  sendAT("AT+CNMP=38");           // nur LTE (kein 2G/3G mehr in CH)
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
      if (i == 15 || i == 50) nudgeNetwork();   // nach ~1 min und ~3 min nachhelfen
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
  }
  updateSignal();
  sendOutput(registered ? "[MODEM] Eingebucht und sendebereit!\n"
                        : "[MODEM] !! Noch nicht eingebucht, versuche weiter...\n");
  return true;
}

bool sendSMS(const String& text) {
  if (!registered) return false;

  sendAT("AT+CMGF=1");
  while (Modem.available()) Modem.read();
  Modem.print(String("AT+CMGS=\"") + SMS_TARGET_NUMBER + "\"\r");

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
  Modem.write(0x1A);              // Strg+Z = senden

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

void modemTask(void*) {
  Modem.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

  while (!measurementReady) vTaskDelay(pdMS_TO_TICKS(100));
  // Annahme: vor dem Start war Strom da. Startet das Board waehrend eines
  // Ausfalls neu (z.B. Spannungseinbruch beim Umschalten auf Akku), geht
  // trotzdem die Alarm-SMS raus.
  bool notifiedState = true;
  unsigned long lastFailAt = 0;
  bool hadFailure = false;
  unsigned long lastCheck = 0;
  int lostCount = 0;

  for (;;) {
    if (modemState != MS_READY) {
      if (!initModem()) { vTaskDelay(pdMS_TO_TICKS(30000)); continue; }
      lostCount = 0;
      lastCheck = millis();
    }

    // SMS, sobald der neue Netzzustand stabil ist
    bool current = mainsOnline;
    bool retryOk = !hadFailure || (millis() - lastFailAt >= SMS_RETRY_MS);
    if (current != notifiedState &&
        millis() - mainsChangedAt >= SMS_CONFIRM_MS && retryOk) {
      String msg = current
        ? "ENTWARNUNG: Stromversorgung wiederhergestellt! Akku: " +
            String(battPctShared) + "% (" + String((float)vBatShared, 2) + "V)."
        : "ALARM: Stromausfall erkannt! System laeuft auf USV-Akku (" +
            String(battPctShared) + "%, " + String((float)vBatShared, 2) + "V).";

      smsState = SMS_SENDING;
      sendOutput("[SMS] Sende: " + msg + "\n");
      if (sendSMS(msg)) {
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

    // Alle 30 s Netz + Signal pruefen, bei laengerem Verlust Modem neu starten
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
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // Brownout-Reset sperren (Umschalten Netz/Akku)
  setCpuFrequencyMhz(160);

  pinMode(BOARD_POWER_ON, OUTPUT);
  digitalWrite(BOARD_POWER_ON, HIGH);

  Serial.begin(115200);
  delay(200);
  outMutex = xSemaphoreCreateMutex();

  SerialBT.begin(BT_DEVICE_NAME);

  pinMode(BAT_ADC_PIN, INPUT);
  pinMode(SOLAR_ADC_PIN, INPUT);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  sendOutput("\n=== NETZWAECHTER MIT SMS-ALARM GESTARTET ===\n");
  sendOutput("[BOOT] Neustart-Grund: " + resetReasonText() + "\n");

  // Modem-Task auf Core 0, Arduino-Loop laeuft auf Core 1
  xTaskCreatePinnedToCore(modemTask, "modem", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  float vSolarRaw = readPinVoltage(SOLAR_ADC_PIN, SOLAR_DIVIDER);
  float vBatRaw   = readPinVoltage(BAT_ADC_PIN, BAT_DIVIDER);

  if (!filtersReady) {
    vSolarAvg = vSolarRaw;
    vBatAvg   = vBatRaw;
    mainsOnline = (vSolarRaw > MAINS_ON_VOLT);
    mainsChangedAt = millis();
    filtersReady = true;
  } else {
    vSolarAvg += SOLAR_ALPHA * (vSolarRaw - vSolarAvg);
    vBatAvg   += BAT_ALPHA   * (vBatRaw   - vBatAvg);
  }

  int percent = getBatteryPercent(vBatAvg);
  vBatShared = vBatAvg;
  battPctShared = percent;
  measurementReady = true;

  // Statuswechsel nur, wenn der Zustand mit Hysterese lange genug anhaelt
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
      lastSend = 0;  // Statuszeile sofort an die App senden
      if (!mainsOnline) {
        sendOutput("\n[ALARM] 230V-NETZ AUSGEFALLEN! Notbetrieb ueber 18650-Akku!\n");
      } else {
        sendOutput("\n[OK] STROM WIEDER DA! Normalbetrieb aktiv.\n");
      }
    }
  } else {
    pendingChange = false;
  }

  // Statuszeile fuer die App (Format wird von der App per Regex gelesen,
  // Modem/SMS-Angaben am Ende werden ignoriert bzw. als Rohzeile angezeigt)
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
