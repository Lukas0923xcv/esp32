#include <Arduino.h>
#include "BluetoothSerial.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#define BAT_ADC_PIN    35  // Interner 18650-Akku Pin
#define SOLAR_ADC_PIN  36  // Interner Solar-/JST-Buchsen-Messpin
#define BOARD_POWER_ON 12  // Modem/Board aktiv halten

BluetoothSerial SerialBT;
const char* BT_DEVICE_NAME = "LilyGO-Netzwaechter";

// LilyGO On-Board Spannungsteiler-Faktoren
const float BAT_DIVIDER   = 2.0;
const float SOLAR_DIVIDER = 2.0;
const float REF_VOLT      = 3.3;

// Netzerkennung mit Hysterese + Entprellung (verhindert zufaelliges Umschalten)
const float          MAINS_OFF_VOLT = 2.5;   // darunter gilt Netz als weg
const float          MAINS_ON_VOLT  = 3.0;   // darueber gilt Netz als da
const unsigned long  MAINS_OFF_MS   = 400;   // so lange muss Spannung niedrig bleiben
const unsigned long  MAINS_ON_MS    = 1000;  // so lange muss Spannung hoch bleiben

// Glaettung (exponentieller Mittelwert), kleiner = ruhiger
const float SOLAR_ALPHA = 0.30;
const float BAT_ALPHA   = 0.10;

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

void sendOutput(const String& msg) {
  Serial.print(msg);
  if (SerialBT.hasClient()) {
    SerialBT.print(msg);
    SerialBT.flush();
  }
}

bool mainsOnline = true;
unsigned long lastSend = 0;
unsigned long changeSince = 0;   // seit wann weicht der Messwert vom Status ab
bool pendingChange = false;
float vSolarAvg = 0, vBatAvg = 0;
bool filtersReady = false;

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Brownout Reset sperren
  setCpuFrequencyMhz(160);

  pinMode(BOARD_POWER_ON, OUTPUT);
  digitalWrite(BOARD_POWER_ON, HIGH);

  Serial.begin(115200);
  delay(200);

  SerialBT.begin(BT_DEVICE_NAME);

  pinMode(BAT_ADC_PIN, INPUT);
  pinMode(SOLAR_ADC_PIN, INPUT);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  sendOutput("\n=== NETZWAECHTER VIA JST-SOLARPORT GESTARTET ===\n");
}

void loop() {
  float vSolarRaw = readPinVoltage(SOLAR_ADC_PIN, SOLAR_DIVIDER);
  float vBatRaw   = readPinVoltage(BAT_ADC_PIN, BAT_DIVIDER);

  if (!filtersReady) {
    vSolarAvg = vSolarRaw;
    vBatAvg = vBatRaw;
    mainsOnline = (vSolarRaw > MAINS_ON_VOLT);
    filtersReady = true;
  } else {
    vSolarAvg += SOLAR_ALPHA * (vSolarRaw - vSolarAvg);
    vBatAvg   += BAT_ALPHA   * (vBatRaw   - vBatAvg);
  }

  // Statuswechsel nur, wenn der Zustand mit Hysterese lange genug anhaelt
  bool wantChange = mainsOnline ? (vSolarAvg < MAINS_OFF_VOLT)
                                : (vSolarAvg > MAINS_ON_VOLT);
  if (wantChange) {
    if (!pendingChange) {
      pendingChange = true;
      changeSince = millis();
    } else if (millis() - changeSince >= (mainsOnline ? MAINS_OFF_MS : MAINS_ON_MS)) {
      mainsOnline = !mainsOnline;
      pendingChange = false;
      lastSend = 0;  // Statuszeile sofort senden, nicht erst im naechsten 1,5-s-Takt
      if (!mainsOnline) {
        sendOutput("\n🚨 [ALARM] 230V-NETZ AUSGEFALLEN! Notbetrieb ueber 18650-Akku!\n");
      } else {
        sendOutput("\n✅ [OK] STROM WIEDER DA! Normalbetrieb aktiv.\n");
      }
    }
  } else {
    pendingChange = false;
  }

  // Zyklischer Statusbericht alle 1,5 Sekunden
  if (millis() - lastSend >= 1500) {
    lastSend = millis();
    int percent = getBatteryPercent(vBatAvg);

    String line = "Netz: [" + String(mainsOnline ? "ONLINE" : "STROMAUSFALL") +
                  "] | Netzteil-In: " + String(vSolarAvg, 2) + "V" +
                  " | Akku: " + String(percent) + "% (" + String(vBatAvg, 2) + "V)\n";
    sendOutput(line);
  }
}
