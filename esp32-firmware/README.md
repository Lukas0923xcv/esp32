# Firmware

← [Zurück zur Übersicht](../README.md)

Arduino-Sketch für den LilyGO **T-A7670G R2**. Flashen: siehe
[Inbetriebnahme im Haupt-README](../README.md#1-firmware-flashen).

```
netzwaechter/
├── netzwaechter.ino      Der ganze Sketch
├── secrets.example.h     Vorlage → nach secrets.h kopieren (Admin-Nummer, SIM-PIN)
└── .gitignore            Hält secrets.h aus Git heraus
```

## Was sich anpassen lässt

Oben in `netzwaechter.ino`:

| Konstante | Standard | Bedeutung |
|---|---|---|
| `SMSC` | `+41794999000` | SMS-Zentrale (Sunrise) – bei anderem Anbieter ändern |
| `BT_DEVICE_NAME` | `LilyGO-Netzwaechter` | Bluetooth-Name |
| `MAINS_OFF_VOLT` / `MAINS_ON_VOLT` | 2,5 V / 3,0 V | Schwellen für Strom weg / wieder da (Hysterese) |
| `MAINS_OFF_MS` / `MAINS_ON_MS` | 400 ms / 1000 ms | Entprellung |
| `SMS_POLL_MS` | 15 s | So oft wird nach SMS-Befehlen geschaut |
| `SOLAR_DIVIDER`, `BAT_DIVIDER` | 2,0 | Spannungsteiler an den ADC-Pins |

Alles, was per App oder SMS einstellbar ist (Empfänger, Texte, Verzögerungen …), steht im Flash und
muss nicht im Code geändert werden.

## Pins

| Pin | Funktion |
|---|---|
| 12 | Versorgung des Modems |
| 4 | `PWRKEY` des Modems |
| 25 | `DTR` des Modems |
| 26 / 27 | UART zum Modem (TX / RX) |
| 35 | ADC: 18650-Akku |
| 36 | ADC: JST-Eingang (5-V-Netzteil) |

## Ablauf

- **Core 1** (`loop`): misst Netzteil- und Akkuspannung, erkennt Ausfall und Wiederkehr, schickt alle
  1,5 s die Statuszeile per Bluetooth und nimmt Befehle der App an.
- **Core 0** (eigener Task): steuert das Modem, sendet die SMS und fragt eingegangene SMS-Befehle ab.

Protokoll zur App: [docs/BLUETOOTH-PROTOKOLL.md](../docs/BLUETOOTH-PROTOKOLL.md) ·
SMS-Befehle: [docs/SMS-BEFEHLE.md](../docs/SMS-BEFEHLE.md)
