# Netzwächter per SMS steuern

Du kannst den Netzwächter von überall aus per SMS einstellen und abfragen, ohne vor Ort zu sein.

## So funktioniert's

1. Schreib eine SMS an die **Nummer der SIM-Karte im Netzwächter**.
2. Die SMS muss mit einem **`/`** anfangen, z. B. `/status`.
3. Nach **15–30 Sekunden** bekommst du eine Antwort per SMS.

**Wer darf Befehle schicken?**
- Alle Nummern, die als **Empfänger** eingetragen sind (in der App oder mit `/nummer neu`).
- Die fest eingestellte **Admin-Nummer** (`SMS_TARGET_NUMBER` im Code). Sie funktioniert immer, damit du dich nicht aussperren kannst.
- SMS von anderen Nummern werden **ignoriert und nicht beantwortet**.

**Gut zu wissen**
- Groß-/Kleinschreibung ist egal: `/Status` = `/status`.
- Pro SMS **ein Befehl**.
- SMS ohne `/` am Anfang werden ignoriert (sie erscheinen nur im Event-Log der App).
- In der App kannst du die SMS-Steuerung ganz ausschalten: **SMS-Alarm → Fernsteuerung per SMS**.

---

## Alle Befehle auf einen Blick

| Befehl | Was er macht | Beispiel |
|---|---|---|
| `/hilfe` | Schickt die Liste aller Befehle | `/hilfe` |
| `/status` | Strom, Akku, Netzteil-Spannung, Signal, Laufzeit | `/status` |
| `/einstellungen` | Alle aktuellen Einstellungen | `/einstellungen` |
| `/alarm <Zeit>` | Wie lange der Strom weg sein muss, bis die Alarm-SMS kommt | `/alarm 3m` |
| `/entwarnung an` | „Strom wieder da“-SMS einschalten | `/entwarnung an` |
| `/entwarnung aus` | „Strom wieder da“-SMS ausschalten | `/entwarnung aus` |
| `/entwarnung <Zeit>` | Wie lange der Strom stabil da sein muss, bis die Entwarnung kommt | `/entwarnung 30s` |
| `/akku <Prozent>` | Warn-SMS, wenn der Akku während eines Ausfalls darunter fällt | `/akku 20` |
| `/akku aus` | Akku-Warnung ausschalten | `/akku aus` |
| `/text alarm <Text>` | Text der Alarm-SMS ändern | `/text alarm Strom weg im Keller!` |
| `/text entwarnung <Text>` | Text der Entwarnungs-SMS ändern | `/text entwarnung Strom ist wieder da.` |
| `/text akku <Text>` | Text der Akku-Warnung ändern | `/text akku Akku nur noch {akku}%!` |
| `/text alarm` | Zeigt den aktuellen Alarm-Text (ohne Text dahinter) | `/text alarm` |
| `/text standard` | Alle drei Texte auf Standard zurücksetzen | `/text standard` |
| `/nummer liste` | Zeigt alle Empfänger | `/nummer liste` |
| `/nummer neu <Nummer>` | Empfänger hinzufügen (max. 5) | `/nummer neu +41791234567` |
| `/nummer weg <Nummer>` | Empfänger entfernen | `/nummer weg +41791234567` |
| `/nummer weg <Platz>` | Empfänger nach Platz in der Liste entfernen | `/nummer weg 2` |
| `/pause <Zeit>` | Alarm-SMS für eine Weile stummschalten (z. B. bei Arbeiten am Strom) | `/pause 2h` |
| `/pause aus` | Pause sofort beenden | `/pause aus` |
| `/test` | Schickt eine Test-SMS an alle Empfänger | `/test` |
| `/neustart` | Startet den Netzwächter neu | `/neustart` |

Englische Kurzformen gehen auch: `/help`, `/config`, `/reboot`, `/nummer add`, `/nummer del`, `on`/`off`.

---

## Zeitangaben

| Du schreibst | Bedeutet |
|---|---|
| `sofort` oder `0` | sofort |
| `30s` | 30 Sekunden |
| `5m` oder `5min` oder einfach `5` | 5 Minuten (**ohne Einheit = Minuten**) |
| `2h` oder `2std` | 2 Stunden |
| `1d` oder `1t` | 1 Tag |

Grenzen: `/alarm` und `/entwarnung` gehen bis **60 Minuten**, `/pause` bis **7 Tage**.

---

## Platzhalter in SMS-Texten

Diese Wörter werden beim Senden durch echte Werte ersetzt:

| Platzhalter | Wird zu | Beispiel |
|---|---|---|
| `{akku}` | Akkustand in % | `84` |
| `{volt}` | Akkuspannung | `3.93` |
| `{dauer}` | Wie lange der Stromausfall schon dauert | `1 h 12 min` |

Beispiel:

```
/text alarm Stromausfall! Akku {akku}% ({volt}V)
```

Wenn dein Handy keine geschweiften Klammern hat, geht auch `(akku)` oder `[akku]`.

**Tipps für Texte**
- Eine SMS hat maximal **160 Zeichen**. Der Befehl selbst zählt mit. Längere Texte besser in der App eingeben.
- **Umlaute vermeiden** (`ae` statt `ä`), ebenso `|`, `@`, `$`, `_` und `~`. Das Modem kann sie nicht sauber senden.

---

## Beispiele aus der Praxis

**Kurz nachschauen, ob alles läuft**
```
/status
```
Antwort:
```
Netz ONLINE, Netzteil 5.02V, Akku 98% (4.15V), Signal 21, Laufzeit 3 h 12 min
```

**Ich arbeite heute an der Elektrik, bitte keine Alarme für 3 Stunden**
```
/pause 3h
```
Danach geht es automatisch wieder los. Früher beenden: `/pause aus`.

**Alarm erst nach 10 Minuten, weil kurze Ausfälle egal sind**
```
/alarm 10m
```

**Nachbar soll auch informiert werden**
```
/nummer neu +41781234567
```

**Prüfen, ob die SMS überhaupt rausgehen**
```
/test
```

---

## Wichtig

- **Die Pause gilt nur bis zum nächsten Neustart.** Nach einem Neustart sind Alarme wieder aktiv.
- Fällt der Strom **während einer Pause** aus, kommt dafür keine Alarm-SMS. Das gilt auch dann, wenn die Pause endet, solange der Strom noch weg ist.
- Alle anderen Einstellungen werden **dauerhaft gespeichert** und gelten auch nach einem Neustart. Die App zeigt sie beim nächsten Öffnen der SMS-Einstellungen an.
- Antworten und Befehle sieht man auch im **Event-Log der App**, wenn sie verbunden ist.
- **Sicherheit:** Der Netzwächter prüft nur die Absender-Nummer. Wer deine Nummer fälschen kann, könnte Befehle schicken. Wenn dir das zu riskant ist, schalte die SMS-Steuerung in der App aus.
- Kommt keine Antwort: Ist der Netzwächter im Mobilnetz (in der App: `Modem: OK`)? Ist deine Nummer als Empfänger eingetragen? Beginnt die SMS mit `/`?

---

## Technik (für später)

- Der Netzwächter schaut alle **15 Sekunden** nach neuen SMS (`AT+CMGL="ALL"`).
- Jede empfangene SMS wird **sofort gelöscht** und erst danach ausgeführt. So läuft ein `/neustart` nach dem Neustart nicht noch einmal.
- Antworten, die länger als 160 Zeichen sind, werden auf bis zu 3 SMS verteilt.
- Die Einstellungen liegen im Flash des ESP32 (`Preferences`, Bereich `netzw`). Die App und die SMS-Befehle ändern dieselben Werte.
