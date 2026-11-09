# LilyGO-Netzwächter

Stromausfall-Melder auf Basis eines **LilyGO T-A7670G R2** (ESP32 + LTE-Modem A7670G + 18650-Akku).
Fällt der Netzstrom aus, schickt der Netzwächter eine **SMS** an bis zu fünf Nummern und meldet sich
wieder, wenn der Strom zurück ist. Eine **Android-App** zeigt den Live-Status per Bluetooth und dient
zum Einstellen; unterwegs lässt er sich auch **per SMS-Befehl** abfragen und konfigurieren.

## So funktioniert es

```
5V-Netzteil ──JST──▶ LilyGO T-A7670G ──LTE/SMS──▶ Handys der Empfänger
                     │  18650-Akku als USV
                     └──Bluetooth (SPP)──▶ Android-App (Live-Status, Einstellungen)
```

- Das Board hängt an einem 5-V-Netzteil am **JST-Eingang** (beim Board als Solar-Eingang bezeichnet,
  ADC-Pin 36). Der eingebaute **18650-Akku** hält es bei einem Ausfall am Laufen.
- Fällt die Spannung am JST-Eingang unter 2,5 V (gemessen hinter dem Spannungsteiler), gilt der Strom als weg;
  ab 3,0 V wieder als da (Hysterese und Entprellung gegen Flackern).
- Bleibt der Strom länger als die eingestellte **Alarm-Verzögerung** weg (Standard 3 min), geht die
  Alarm-SMS raus. Ist er wieder stabil da (Standard 10 s), folgt die Entwarnung. Optional gibt es eine
  Warnung, wenn der Akku während eines Ausfalls unter eine Schwelle fällt.
- Das Modem läuft in einem eigenen Task auf Core 0, damit Bluetooth und App nie hängen.
- Alle Einstellungen liegen im Flash des ESP32 und überleben einen Neustart.

## Inhalt

```
esp32-firmware/netzwaechter/   Arduino-Sketch (netzwaechter.ino) und secrets.example.h
android-app/                   Flutter-App (Bluetooth-Dashboard, SMS-Einstellungen, Event-Log)
docs/SMS-BEFEHLE.md            Alle SMS-Befehle, Zeitangaben und Platzhalter
.github/workflows/             Baut die App-APK bei jedem Push auf main
```

## Hardware

- LilyGO **T-A7670G R2** mit 18650-Akku
- 5-V-Netzteil am JST-Eingang (das ist der Sensor für den Netzstrom)
- Nano-SIM mit SMS-Guthaben

> [!NOTE]
> Die Firmware ist auf die Schweiz zugeschnitten: Die SMS-Zentrale ist fest auf **Sunrise**
> (`SMSC = "+41794999000"`) eingestellt, und Nummern ohne Ländervorwahl (`079…`) werden als `+41…`
> gelesen. Bei einem anderen Anbieter oder Land beides oben in `netzwaechter.ino` anpassen.

## 1. Firmware flashen

1. Arduino IDE mit dem Board-Paket **esp32 by Espressif** installieren.
2. Im Ordner `esp32-firmware/netzwaechter/` die Vorlage kopieren und ausfüllen:

   ```sh
   cp secrets.example.h secrets.h
   ```

   ```c
   #define SMS_TARGET_NUMBER "+41790000000"   // Standard-Empfänger und Admin-Nummer
   #define SIM_PIN           "0000"           // PIN der SIM, leer lassen wenn keine PIN
   ```

   `secrets.h` ist in `.gitignore` und landet nicht auf GitHub. Die **Admin-Nummer** darf immer
   SMS-Befehle schicken, damit du dich nicht aussperren kannst. Die SIM-PIN wird beim ersten Start
   deaktiviert. Ohne `secrets.h` lässt sich der Sketch trotzdem bauen, dann aber ohne Admin-Nummer
   und ohne PIN.
3. `netzwaechter.ino` öffnen, das Board auswählen (LilyGO empfiehlt für die T-A76xx-Serie
   *ESP32 Wrover Module*) und hochladen. Meldet die IDE, dass der Sketch zu groß ist, ein
   Partitionsschema mit mehr Platz für die App wählen (z. B. *Huge APP*), denn Bluetooth Classic
   braucht viel Flash.
4. Im seriellen Monitor (115200 Baud) siehst du, ob Modem und SIM bereit sind.

## 2. Android-App

**Fertige APK:** [Releases → latest → `app-release.apk`](https://github.com/Lukas0923xcv/esp32/releases/tag/latest).
Sie wird bei jedem Push auf `main` automatisch neu gebaut.

Einrichten:

1. Unter Android den Netzwächter (`LilyGO-Netzwaechter`) in den Bluetooth-Einstellungen koppeln.
2. App öffnen, Berechtigungen (Bluetooth, Standort, Benachrichtigungen) erlauben, Gerät wählen, **Verbinden**.
3. Unter **SMS-Alarm einstellen** Empfänger, Texte, Verzögerungen und die Akku-Warnung setzen.

Die App zeigt Netzstatus, Netzteil-Spannung, Akkustand, SMS-Status, eine Diagnose und ein Event-Log
der Statuswechsel. Bei einem Ausfall gibt es zusätzlich eine Benachrichtigung auf dem Handy, solange
es verbunden ist. Mit dem **Demo-Modus** lässt sich die App ohne Hardware ausprobieren.

Selbst bauen (Flutter 3, Java 17):

```sh
cd android-app
flutter pub get
flutter build apk --release
```

`flutter_bluetooth_serial` muss mit neueren Android-Gradle-Plugins gepatcht werden (Namespace und
`compileSdkVersion`); wie, steht im Workflow
[`.github/workflows/build-apk.yml`](.github/workflows/build-apk.yml). Ohne das Secret `KEYSTORE_BASE64`
wird die APK mit dem Debug-Schlüssel signiert.

## 3. Per SMS steuern

Eine SMS, die mit `/` beginnt, ist ein Befehl, zum Beispiel `/status`, `/alarm 10m`, `/pause 3h`
oder `/nummer neu +41781234567`. Antworten kommen nach 15–30 Sekunden. Befehle werden nur von
eingetragenen Empfängern und der Admin-Nummer angenommen.

Alle Befehle: **[docs/SMS-BEFEHLE.md](docs/SMS-BEFEHLE.md)**

> [!WARNING]
> Der Netzwächter prüft nur die Absender-Nummer, und die lässt sich fälschen. Wer das nicht will,
> schaltet die SMS-Steuerung in der App ab (**SMS-Alarm → Fernsteuerung per SMS**).

## Bluetooth-Protokoll

Der Netzwächter schickt alle 1,5 s eine Statuszeile, z. B.
`Netz: [ONLINE] | Netzteil: 5.02V | Akku: 98% (4.15V)`. Befehle an ihn sind einzelne Zeilen mit `\n`:

| Befehl | Antwort |
|---|---|
| `GET` | Alle Einstellungen als `CFG:KEY=WERT`, Ende mit `CFG:END` |
| `SET:KEY=WERT` | `OK:KEY` oder `ERR:KEY` |
| `TEST` | Schickt eine Test-SMS an alle Empfänger |

Keys: `NUMS`, `MSGOFF`, `MSGON`, `MSGLOW`, `OFFDELAY`, `ONDELAY`, `RESTORE`, `LOWBAT`, `SMSCMD`.
Platzhalter in SMS-Texten: `{akku}`, `{volt}`, `{dauer}`.
