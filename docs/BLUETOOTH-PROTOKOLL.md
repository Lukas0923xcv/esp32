# Bluetooth-Protokoll

← [Zurück zur Übersicht](../README.md)

Der Netzwächter meldet sich als Bluetooth-Classic-Gerät (SPP) mit dem Namen **`LilyGO-Netzwaechter`**.
Die Android-App nutzt genau dieses Protokoll; jedes andere Programm mit serieller Bluetooth-Verbindung
(z. B. ein Terminal am PC) kann es ebenso.

## Statuszeile

Alle **1,5 s** schickt der Netzwächter eine Zeile, zum Beispiel:

```
Netz: [ONLINE] | Netzteil-In: 5.02V | Akku: 98% (4.15V) | Modem: … | SMS: …
```

| Feld | Bedeutung |
|---|---|
| `Netz` | `ONLINE` oder `STROMAUSFALL` |
| `Netzteil-In` | Spannung am JST-Eingang (hinter dem Spannungsteiler) |
| `Akku` | Ladestand und Spannung des 18650-Akkus |
| `Modem` | Zustand von Modem, SIM und Netz |
| `SMS` | Zustand der letzten SMS |

Daneben erscheinen Ereigniszeilen wie `[OK] STROM WIEDER DA! …` oder `[BOOT] …`.

## Befehle an den Netzwächter

Jeder Befehl ist eine Zeile, abgeschlossen mit `\n`.

| Befehl | Antwort |
|---|---|
| `GET` | Alle Einstellungen als `CFG:KEY=WERT`, Ende mit `CFG:END` |
| `SET:KEY=WERT` | `OK:KEY` oder `ERR:KEY` |
| `TEST` | Schickt eine Test-SMS an alle Empfänger |

## Einstellungen (Keys)

| Key | Wert | Standard |
|---|---|---|
| `NUMS` | Empfänger, kommagetrennt (max. 5) | Admin-Nummer aus `secrets.h` |
| `MSGOFF` | Text der Alarm-SMS (max. 160 Zeichen) | `ALARM: Stromausfall erkannt! …` |
| `MSGON` | Text der Entwarnungs-SMS | `ENTWARNUNG: Stromversorgung wiederhergestellt! …` |
| `MSGLOW` | Text der Akku-Warnung | `WARNUNG: USV-Akku fast leer …` |
| `OFFDELAY` | Sekunden bis zur Alarm-SMS (0–3600) | `180` |
| `ONDELAY` | Sekunden bis zur Entwarnung (0–3600) | `10` |
| `RESTORE` | Entwarnung senden: `1` / `0` | `1` |
| `LOWBAT` | Akku-Warnung ab Prozent, `0` = aus (0–100) | `0` |
| `SMSCMD` | SMS-Befehle erlaubt: `1` / `0` | `1` |

Platzhalter in den SMS-Texten: `{akku}` (Prozent), `{volt}` (Akku-Spannung), `{dauer}` (Dauer des Ausfalls).

Beispiel:

```
> SET:OFFDELAY=300
< OK:OFFDELAY
> GET
< CFG:NUMS=+41790000000
< CFG:OFFDELAY=300
< …
< CFG:END
```
