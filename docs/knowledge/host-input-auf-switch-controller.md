# Host-Eingaben auf Switch-Controller abbilden (Tastatur, Gamepads, Browser)

Spielübergreifendes Wissen aus P5.1 Teil 2. Der Code steht in `platform/common/src/pad_input.cpp` (Abbildung, ohne Geräte), `platform/desktop/src/sdl_input.cpp` (SDL3) und `platform/web/src/web_input.cpp` (Browser). Tests: `platform.pad_input` (nativ und Node), `platform.sdl_input` (SDL-Gamepad ohne Gerät), `browser.web_input` (Chromium über Playwright). Wie die Zustände danach ins Shared Memory kommen, beschreibt [`horizon-hid-npad-shared-memory.md`](horizon-hid-npad-shared-memory.md).

## Aufbau

- **Geräte-Backends liefern nur `HostPad`s**: Tasten schon als Switch-Bits (nach Beschriftung, siehe unten), Sticks −1…1 mit „oben positiv“, analoge Trigger 0…1, Bewegungsdaten in den Einheiten von `SixAxisSensorState` (G, Umdrehungen/s) und ein Flag „Wurfgeste“.
- **`ControllerMapper`** macht daraus den `ControllerState` *eines* Switch-Controllertyps: radiale Totzone (Standard 0,15) und äußere Sättigung (ab Länge 0,9 voller Ausschlag) mit Neuskalierung dazwischen, Trigger → ZL/ZR ab 0,5, Tastenteilmenge des Typs, synthetische Bewegung.
- **`PadInput`** ist das `IInput` dazu: Slot *i* hat einen festen Typ (`MapperConfig`), Backend-Pad *i* liegt auf Slot *i* (in SDL nach `PlayerPick`, siehe unten), die Tastatur kommt zu Slot 0 dazu. `sample()` und `rumble()` des Backends laufen unter derselben Sperre.
- Standard für LGP: Slot 0 = Handheld (Spieler 1 auf dem Handheld-Npad), Slot 1 = einzelner rechter Joy-Con (Spieler 2 auf Npad No2). Welches Host-Pad auf Slot 0 liegt, entscheidet der erste Tastendruck (unten, „Wer Spieler 1 ist“). Das Startprogramm wählt Spieler 1 mit `--controller handheld|joycon-right|joycon-left`. Dieses und `--stick`, `--throw`, `--motion-frame` gelten für SDL und den Browser (`WebInput`, über `?args=`); das Startprogramm baut die Slots einmal und gibt sie an `boot::host_devices(slots)`.

## Wer Spieler 1 ist (mehrere Controller)

- An der Konsole meldet sich im Controller-Bildschirm eines Spiels der Controller an, an dem gedrückt wird (Joy-Con mit SL+SR, Handheld mit A); er bekommt Npad No1. Ein Host-Pad kann sich nicht anmelden. Früher war deshalb immer das zuerst verbundene Pad Spieler 1, egal an welchem Pad A gedrückt wurde.
- **`PlayerPick`** (`pad_input.hpp`, Test `platform.pad_input`) ersetzt das Anmelden durch einen Tastendruck: Das erste Pad, an dem nach dem Start eine Taste *neu* gedrückt wird, rückt auf Slot 0; die übrigen behalten ihre Reihenfolge dahinter. Die Wahl hält, bis das Pad abgesteckt wird oder neu gewählt wird. Eine Taste, die beim Verbinden schon gehalten wird, zählt nicht. Bis zum ersten Druck gilt die Reihenfolge des Backends, also bleibt Slot 0 belegt und das Spiel sieht vom Start an einen Controller.
- **Neu wählen**: (1) Trennt das Spiel Npad No1 (`DisconnectNpad`, LGP im Ein-Joy-Con-Modus am Controller-Bildschirm), meldet hid das dem Backend (`IInput::npad_disconnected`); der nächste neue Tastendruck an irgendeinem Pad wählt Spieler 1 und meldet das Npad im selben Takt wieder an (siehe `horizon-hid-npad-shared-memory.md`). (2) In der App: Einstellungen → Controller → „Spieler 1 neu wählen“ (nur im Spiel), danach wählt der nächste Druck nach dem Schließen des Menüs. Tastendrücke im Menü erreichen das Spiel nicht und wählen deshalb nicht.
- Abschalten: Einstellungen „Spieler 1 per Tastendruck wählen“, `settings.ini` `player1 = press|order`, Startprogramm `--player1 press|order` (Standard `press`). `order` ist das alte Verhalten (Verbindungsreihenfolge).
- Wechselt das Pad auf einem Slot, folgt ihm die Vibration, die das Spiel für den Slot angefordert hat. Das alte Pad hört auf zu vibrieren. `ControllerState::device` nennt hid das Pad: Nach `DisconnectNpad` zählen beim neuen Pad auch Tasten als neuer Druck, die das alte Pad beim Trennen hielt (Test `runtime.hle_hid`).
- Tastatur, Touch-Steuerung und TV-Fernbedienungen gehören weiter fest zu Slot 0 und nehmen an der Wahl nicht teil.
- Log: `lgp: player 1: gamepad … (button press), slot 0`, nach einem Neuwählen `lgp: player 1: … the next button press on a gamepad chooses`.
- Umgesetzt für SDL (`SdlInput`, Test `platform.sdl_input` mit zwei virtuellen Pads). Der Browser (`WebInput`) nimmt noch die Reihenfolge der Gamepad-API.
- **Nicht am Gerät geprüft (Verdacht):** dass der erste neue Tastendruck nach dem Start in LGP immer der im Controller-Bildschirm ist. Läuft vorher die App (Startmenü), erreichen deren Drücke `SdlInput` nicht, weil es erst danach angelegt wird. Ob LGP im Handheld-Modus später noch einmal `DisconnectNpad` ruft, ist nicht gemessen; im Ein-Joy-Con-Modus tut es das am Controller-Bildschirm.

## Tasten nach Beschriftung, nicht nach Position

- Die Switch hat A rechts und B unten, Xbox-Pads haben A unten. Wer mit einem Xbox-Pad spielt, erwartet „A = bestätigen“. Deshalb gehen Tasten nach **Beschriftung**: Xbox-A → Switch-A, PlayStation-Kreuz → A (Xbox-Position), Kreis → B, Viereck → X, Dreieck → Y.
- SDL3 kennt die Beschriftung (`SDL_GetGamepadButtonLabel`). Für Nintendo-Controller ergibt das automatisch die gedruckte Belegung.
- Die Browser-Gamepad-API kennt **keine Beschriftung**, nur die W3C-„standard“-Positionen (0 unten, 1 rechts, 2 links, 3 oben, 4/5 Schultern, 6/7 Trigger, 8 Select, 9 Start, 10/11 Stickklick, 12–15 Steuerkreuz). Das Backend nimmt Xbox-Positionen an und dreht A/B und X/Y, wenn die Gamepad-ID „Nintendo“, „057e“ (Nintendos USB-Hersteller-ID), „Pro Controller“ oder „Joy-Con“ enthält. Pads ohne „standard“-Mapping werden ignoriert.

## Im Startprogramm

- Desktop: Mit `--graphics hle --renderer window` öffnet das Startprogramm `SdlInput` (abschaltbar mit `--input none`).
- Browser: Ohne Fenster nimmt das Startprogramm die Geräte des Hosts (`boot::host_devices()` in `platform/web/src/boot_host.cpp`), also `WebInput` für hid und Web Audio für audout. Die Spielseite aus P6.1 bekommt damit Tastatur, Gamepads und Ton ohne weitere Optionen.

## Tastatur

- Tasten werden als **USB-HID-Usage-IDs** geführt. SDL3-Scancodes haben dieselben Nummern. Im Browser wird `KeyboardEvent.code` übersetzt (`hid_usage_from_dom_code`), nicht `key`, denn `code` ist positionsbezogen und unabhängig vom Tastaturlayout.
- Positionen folgen dem US-Layout. Auf einer deutschen Tastatur liegt „Z“ (Switch-A) also auf der Taste mit der Aufschrift Y.
- Standardbelegung, angelehnt an die Ryujinx-Voreinstellung (dort nur nachgesehen, nicht übernommen):

| Switch | Taste |
|---|---|
| linker Stick | W A S D, Klick F |
| rechter Stick | I J K L, Klick H |
| Steuerkreuz | Pfeiltasten |
| A / B / X / Y | Z / X / C / V |
| L / ZL | E / Q |
| R / ZR | U / O |
| Plus | `=`, Ziffernblock `+`, Enter |
| Minus | `-`, Ziffernblock `-` |
| Wurfgeste (nur einzelner Joy-Con) | Leertaste |

- Die Tastatur zählt als verbundener Controller auf Slot 0, solange sie aktiv ist. Verliert das Fenster den Fokus, gelten alle Tasten als losgelassen, weil die Key-Ups dann an ein anderes Fenster gehen.

## Einzelner Joy-Con quer

- **Rechter Joy-Con**: Sein Stick ist der *rechte* Stick des Npad-Zustands. A/B/X/Y und Plus bleiben, Host-L/ZL wird SL, Host-R/ZR wird SR, beide Stickklicks werden Stickklick rechts, Minus wird Plus. Als Stick nimmt der Mapper den linken Host-Stick, fällt auf den rechten zurück, wenn der linke ruht, und auf das Steuerkreuz (volle Auslenkung, Diagonalen auf dem Einheitskreis), wenn beide ruhen. Ein rechter Joy-Con hat kein Steuerkreuz, so bleibt es nicht ungenutzt.
- **Linker Joy-Con**: Seine Gesichtstasten sind die Steuerkreuz-Tasten. Host-A/B/X/Y landen nach Himmelsrichtung auf Rechts/Unten/Oben/Links. SL/SR wie oben.
- **Echter einzelner Joy-Con hochkant**: SDL meldet einen einzelnen Joy-Con von sich aus quer als Mini-Gamepad. Dann
  sind die Gesichtstasten gedreht (physisches X kommt als A an) und die Sensorachsen x/z getauscht (Paul 2026-10-22,
  echter rechter Joy-Con am PC). LGP hält ihn hochkant. `SdlInput::create` setzt deshalb
  `SDL_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS=1`; die Umgebung gewinnt (`=0` gibt das alte Verhalten). Hochkant kommt SR als
  rechtes Paddle 1 an, also als SR; SL bleibt ungenutzt. Paare sind nicht betroffen.
- **Nicht gedreht**: Der Mapper liefert Stick und Tasten so, wie der Spieler sie sieht. Am PC mit LGP geprüft (2026-10-13, `docs/analysis/11-pc-abnahme-p5.md`): Stick oben bewegt die Figur nach oben, rechts nach rechts. Das Drehen übernimmt also nicht der Mapper.

## Bewegung

- **Echte Sensoren** reicht das Backend durch. Das gilt für SDL-Gamepads mit Gyro *und* Beschleunigungssensor: DualShock 4, DualSense, Switch-Controller über SDLs HIDAPI-Treiber. SDL liefert rad/s und m/s² im Rahmen „x rechts, y oben, z zum Spieler“; aufrecht gehalten zeigt die Schwerkraft nach +y.
- Die Umrechnung in den Switch-Rahmen (flach liegend zeigt die Schwerkraft nach −z) ist im Code `(x, y, z)_Switch = (−x, z, −y)_SDL`, geteilt durch 9,80665 bzw. 2π. Am PC mit LGP und einem Pro Controller im Handheld-Stil geprüft (2026-10-13): Die Ansicht folgt der Drehung richtig. Spielt ein quer gehaltenes Pad einen *einzelnen Joy-Con*, reagiert das Zielen in LGP nur sehr schwach. Verdacht, nicht geprüft: Das Spiel erwartet den Joy-Con aufrecht, die Achsen müssten für diesen Fall um 90° gedreht werden. Die Browser-Gamepad-API hat keine Bewegungsdaten.
- **Synthetische Bewegung** gibt es nur für einen einzelnen Joy-Con ohne Sensor:
  - In Ruhe meldet der Mapper „liegt flach“: Beschleunigung (0, 0, −1) G, Drehrate 0.
  - Die Wurfgeste ist eine Sinusperiode über 400 ms. Die Beschleunigung entlang +y steigt bis 6 G, die Drehrate um x bis 5 U/s.
  - Ausgelöst wird sie von der Leertaste oder dem Klick des rechten Sticks. Gedrückthalten wirft nur einmal, erst Loslassen und erneutes Drücken wirft wieder. Ohne Fenster per Eingabeskript: Taste `throw` in `--press` (z. B. `--press 30s:throw`, nur mit `--controller joycon-right|joycon-left`, `--throw` gilt auch hier). Der Mapper läuft dann auf der Gastzeit, sodass Läufe mit `--clock virtual` sich wiederholen. Zwischen den Würfen meldet der Joy-Con nur die Schwerkraft (Test `boot.press_script`, `test_scripted_throw`).
  - Am PC mit LGP abgestimmt (2026-10-13): 300 ms / 4 G / 3 U/s erkennt das Spiel nicht, 400 ms / 6 G / 5 U/s wirft geradeaus. Wie LGP den Wurf erkennt, ist nicht analysiert; es fehlte Stärke bzw. Dauer, keine andere Achse. Die Werte stehen in `MapperConfig`, im Startprogramm einstellbar mit `--throw ms=…,g=…,rps=…,accel=x|y|z|all,gyro=x|y|z|all,rest=[+-]x|y|z` (`parse_throw_spec`).
- Im Handheld-Modus wirft LGP per Taste. Dort erfindet der Mapper keine Bewegung: Ohne Sensor bleibt der Zustand „nicht verbunden“, wie im hid-Dokument beschrieben.
- **Richtung und Stärke** (Wisch-Würfe, siehe Touchscreen): `HostPad::throw_side` (−1 links … 1 rechts) und `throw_strength` (1…2) liest der Mapper beim Start der Geste. Die Seite dreht den Schwung um +y (`throw_side_gyro_axis`, `throw_side_gain` 1; die linke Seite lässt sich mit `throw_side_left_*` getrennt einstellen); eine seitliche Beschleunigung (`throw_side_accel_axis`) ist ab Werk aus. Die Stärke skaliert beide Spitzenwerte, wird aber ab Werk auf 1 gekappt (`throw_strength_max` = 1). Das Wischtempo wirkt stattdessen über die Länge des Schwungs (`HostPad::throw_reach` −1…1, Pauls Wahl 2026-10-21: schnell wischen wirft etwas weiter, langsam etwas kürzer): 300 ms bei −1 (`throw_reach_short_ms`), 400 bei 0, 550 bei 1 (`throw_reach_long_ms`). **Gemessen am PC (2026-10-21, `--press …:throw`, virtuelle Uhr, `joycon-right`, Ball in der ersten Fangszene, 1280×720; Wiederholung bitgleich):** Bei `sidegain=1` und voller Seite lenken alle drei Anteile, + ist rechts. Drehung um z versetzt die Ballbahn um etwa +270 px (rechts) bzw. −140…−370 px (links), Drehung um y um +35…+90 px, Beschleunigung entlang x um +55…+120 px und biegt die Bahn nach unten. Doppelte Spitzen (`strength=2`, 12 G / 10 U/s) werfen keinen weiteren Ball, sondern einen Steilwurf, der zu kurz fällt. Zweiter Lauf (Seite × Gain = s): um z trifft s = −0,5 (Kurve links oben), aber jedes s ≥ +0,5 taucht rechts unten ab und verfehlt, s = +0,25 wirkt kaum; um y trifft s = +1 (Kurve rechts oben, Ende etwa +35…+75 px), s = ±2 taucht auf beiden Seiten ab. Stärke 1,25 wirkt nicht, ab 1,5 Steilwurf. Dritter Lauf (530e0cde): links um y mit Gain 1 trifft wie z bei −0,5 (Kurve links oben, Ende etwa −330 px), z bei −0,25 zeigt nichts, y bei +0,5 kaum etwas. Daraus beide Seiten um y mit Gain 1. Läufe 4 und 5 (Wurfhöhe, nur ein Zubat in einer Entfernung): Spitzen zwischen 0,8× und 1,45× werfen bytegleich, erst g und rps zusammen ab 1,5× geben einen Steilwurf (an dieser Entfernung zu kurz; ob er bei weiten oder hohen Pokémon trifft, ist offen). Die Dauer ändert die Bahn: 300 ms etwas kürzer, 550 ms etwas weiter und minimal höher (Ende x 684/746 gegen 714), 250 ms wie 300, 700 ms biegt nach links. Halbe Seite (Wischen etwa 22° schräg) wirkt also kaum; eine Kennlinie, die kleine Winkel verstärkt, ist ungeprüft. Zum Abstimmen: `--throw side=[+-]x|y|z|none,yaw=[+-]x|y|z|none,sidegain=F,leftyaw=…,leftgain=F,sideaccel=F,leftaccel=F,dir=-1..1,strength=1..2,tempo=1..2,reach=-1..1,short=MS,long=MS` (`yaw`/`sidegain` gelten für beide Seiten, `leftyaw`/`leftgain` danach nur links) (`sideaccel`/`leftaccel`: eigener Gain der Seitenbeschleunigung, −10…10, links nicht gespiegelt, + = Plusseite der Achse; ohne sie folgt sie `sidegain` und spiegelt links). **Echte Joy-Con-Würfe (PC, 2026-10-22, hochkant):** gedreht wird vor allem um x, dazu um z bis ±10 U/s mit gegenläufiger Beschleunigung auf x, um y kaum. Hochkant auf den Bildschirm gerichtet ist links/rechts eine Drehung um z. Auftrag 10 (um z, Gain 1, `side=…` mit voller Seite): rechts ohne x etwa 220 px weiter rechts als um y, aber der Ball taucht ab; mit −x taucht er ebenso ab (nur +56 px); mit +x schießt er über das Bild. Links mit +x (Lz) trifft er wie um y. Auftrag 11 (`sideaccel` 0,25/0,5/0,75 mit `side=x`): rechts landet der Ball je Stufe etwa 95 px weiter rechts (Ende bei Bild 3034 x 1056/1154/1230 gegen 738 um y), taucht aber jedes Mal ab (y 410–440) und verfehlt; links trifft 0,5. Die Beschleunigung skaliert also nur die Weite, das Abtauchen rechts hängt an der Drehung +z selbst. Ergebnis: Um z mit Seitenbeschleunigung gibt es rechts keinen Treffer, der Standard bleibt um y mit Gain 1 (rechts nur etwa +100 px, links etwa −235 px). Was einen echten Rechtswurf oben hält, ist offen (Verdacht: ein Anteil um y, den ein einzelnes Drehachsen-Feld nicht nachbilden kann) (`dir`/`strength` geben jedem Wurf der Leertaste bzw. von `throw` im Eingabeskript diese Richtung/Stärke, `tempo` erlaubt Wisch-Stärken bis zu diesem Wert) (Test `platform.pad_input`, `test_throw_swipe`). Liefert der Slot echte Sensordaten (Geräte-Gyro des Handys auf Slot 0, #329), legt der Mapper den Schwung eines Wurfs auf die echten Werte statt ihn zu verwerfen; vorher warf das Wischen im Joy-Con-Stil mit Geräte-Gyro gar nicht.

## Vibration

- hid gibt pro Npad zwei Amplituden weiter (tief, hoch). SDL: `SDL_RumbleGamepad` mit 250 ms Dauer, alle 150 ms erneuert, solange das Spiel vibrieren lässt; der Aufruf geschieht im Haupt-Thread (`SdlInput::service`). Browser: `vibrationActuator.playEffect("dual-rumble")` (Chrome), stark = tief, schwach = hoch.
- Das Spiel schickt pro Vibrationsgerät einen `VibrationValue` aus vier Floats: Amplitude und Frequenz des tiefen Bands, Amplitude und Frequenz des hohen Bands (Standard 160 Hz/320 Hz). hid reicht nur die beiden Amplituden weiter; die Frequenzen gehen verloren.
- **Kurze Stöße:** `SdlInput::service` läuft einmal pro Durchlauf der Hauptschleife (höchstens etwa 10 ms Abstand, unter Last auch mehr). Früher sah es nur den *aktuellen* Wert; ein Stoß, der zwischen zwei Durchläufen begann und endete, ging ganz verloren. Jetzt merkt sich das Backend den stärksten Wert seit dem letzten Durchlauf und schickt ihn einmal (Test `platform.sdl_input`). Ob das Spiel solche kurzen Stöße schickt, ist nicht gemessen.
- **SDLs Switch-Treiber (HIDAPI, SDL 3.4.16, `SDL_hidapi_switch.c`)**, nachgelesen: Er kodiert beide Amplituden mit der Tabelle von dekuNukem (`rumble_data_table.md`) *linear* (65535 = Amplitude 1,0, 514 ≈ 0,0078) und schickt sie mit festen Frequenzen (beide Bänder ≈ 150 Hz, Codes 0x74/0x3D). Er schreibt höchstens alle 30 ms (`RUMBLE_WRITE_FREQUENCY_MS`); Werte dazwischen fasst er zusammen, wobei der größere gewinnt, und eine Null schickt er nachgereicht. Leise Werte kommen also in richtiger Stärke an, nur nicht in der Frequenz des Spiels. Echte HD-Vibration mit den Frequenzen des Spiels ginge über `SDL_SendGamepadEffect` mit einem rohen Paket (Typ 0x10, zwei mal vier Byte), ist aber nicht eingebaut.
- Die Rohformel für die Frequenz passt zu SDLs Neutralwerten (`00 01 40 40`): Code = round(32 · log2(f/10)), hohes Band (Code − 0x60) · 4, tiefes Band Code − 0x40. 160 Hz → 0x40, 320 Hz → 0x100.

## Stick am Anschlag

- SDLs Switch-Treiber (HIDAPI, `SDL_hidapi_switch.c`, nachgelesen) skaliert jede Stick-Achse einzeln: Anfangs gelten 70 % der Werkskalibrierung als Vollausschlag, und jeder größere Rohwert erweitert diesen Bereich dauerhaft. Am physischen Anschlag liefert ein Pro Controller deshalb nicht verlässlich 1,0, sondern schwankt etwas darunter.
- Liegt die Schwelle eines Spiels (z. B. Gehen → Rennen) nahe am Vollausschlag, flackert die Figur dann zwischen beiden. In LGP am PC mit Pro Controller beobachtet (2026-10-13): Am Anschlag rennt die Figur nur zeitweise und fällt immer wieder ins Gehen. Das Log zeigte kaum Stick-Einbrüche, also kein Wertverlust. Ob die Schwankung am Anschlag die Ursache ist, ist noch nicht bestätigt (Verdacht).
- Gegenmittel: äußere Sättigung im Mapper (`MapperConfig::stick_outer`, Standard 0,9). Ab dieser Länge meldet der Stick 32767; zwischen Totzone und 0,9 wird linear neu skaliert. Einstellbar mit `--stick TOTZONE,AUSSEN` (z. B. `0.15,0.85`). Gilt für alle Backends, auch den Browser.
- Nachlaufen der Figur nach Loslassen (ebenfalls beobachtet) trat nach der Sättigung nicht mehr auf (Paul, 2026-10-13); ein eigener Zusammenhang wurde nicht untersucht.
- Gegenbefund (Paul, 2026-10-13, vor der Sättigung): Ein Gamepad-Tester zeigte am Anschlag nach oben 1,0 (quer 0,04), die Figur ging trotzdem; der Xbox-Controller zeigte in derselben Sitzung das Fehlerbild nie. Das sprach zunächst gegen die Schwelle; der Nachtest mit Sättigung ab 0,9 behob das Stocken aber („gefixt“, Paul 2026-10-13). Lehre: Ein Gamepad-Tester im Browser liest das Gerät über einen anderen Treiber als SDL und taugt nicht als Beleg für SDLs Werte. Zuvor erwogener Verdacht: Zittern. Der Pro Controller liefert über SDL laufend leicht wechselnde Werte, ein gehaltener Xbox-Stick steht still; ein Spiel, das bei Richtungswechseln abbremst, könnte das als Lenken lesen (unbelegt). Versuch: Hysterese `--stick TOTZONE,AUSSEN,HALTEN` (`MapperConfig::stick_hold`, z. B. `0.15,0.9,0.05`): Ein gehaltener Stick behält seinen gemeldeten Wert, solange sich keine Achse um mehr als HALTEN bewegt; Loslassen geht immer sofort durch. Ob die Konsole selbst so filtert, ist nicht geprüft.

## Touchscreen (Handy, Tablet)

`TouchPad` (`platform/common`, Test `platform.touch_pad`) macht aus Fingern auf dem Fenster einen `HostPad` für
Spieler 1 und liefert die Kreise, die `WebGpuRenderer::set_overlay` über das Bild zeichnet. Links ein schwebender
Stick (Mitte dort, wo der Finger aufsetzt, voller Ausschlag nach 0,16 Fensterhöhen), rechts A/B/X/Y in der
Switch-Raute (A rechts, B unten), oben L/ZL und R/ZR, Mitte oben Minus/Plus; ein Finger darf auf benachbarte Tasten
rutschen. Maße in Fensterhöhen, damit die Tasten auf jedem Seitenverhältnis gleich groß bleiben. Since 2026-10-26 (Paul: smaller, with labels, stick hidden) the
buttons are drawn smaller (A/B/X/Y radius 0.058 instead of 0.075) while the touch area stays about the same (1.7 times
the radius instead of 1.35); each button is a dark disc with a light ring and its name (A, ZL, + …,
`OverlayCircle::label`, strokes from `overlay_label_strokes`), pressed a light disc with a dark name. L/R and ZL/ZR share one rounded rectangle (`OverlayCircle::half_w`,
`half_h`, `corner`; the touch areas stay circles). Ring and name are faint (Paul: unobtrusive but readable). The stick is not
drawn and works as before. `overlay_shade` (`lgp_platform`) is the CPU reference of the shader `kOverlayWgsl`; pictures
without a device with `lgp_touch_preview --size WxH --insets L,R,T,B [--press a,zr] [--hidden] --out FILE.ppm` (test
`platform.touch_preview`, `--points H` for the window height in points). Tall windows (tablets) keep the size
the controls have at 480 points (`TouchPad::set_window_points`, `kComfortHeight`; SDL: pixel height over
`SDL_GetWindowDisplayScale`), so on the iPad (834 points) A is about 56 points wide instead of 97; phones stay as they were. `SdlInput` reicht SDL-Touch-Ereignisse durch (`SdlInputOptions::touch`, Standard an für
Android und iOS, aber nicht auf Android TV (`SDL_IsTV()`), Startprogramm `--touch on|off`; Fernbedienung und
Zurück-Taste auf Android: `android-ndk-build.md`); ist ein Controller verbunden, verschwindet das Overlay, bis der
Bildschirm wieder berührt wird (10 s). Einstellbar (für das App-Menü): `set_scale` (0,5–1,4, darüber trifft X auf ZR; Größe und
Randabstand aller Tasten samt Stick-Radius, Minus/Plus bleiben auf ihrer Höhe und rücken nie näher an die Mitte, wo
der Pause-Knopf des App-Menüs sitzt; auf schmalen Fenstern wirkt höchstens `size()`, auf 4:3 etwa 1,34, damit
Minus/Plus nicht in ZL/ZR laufen, `scale()` behält den eingestellten Wert), `set_opacity` (0–1, landet als `OverlayCircle::alpha` im Renderer) und
`set_mirrored` (Stick rechts, A/B/X/Y links; L/ZL bleiben links, R/ZR rechts). Ungeprüft am Gerät: Größe/Lage der Tasten für Daumen, Mehrfingerverhalten.

**Freie Finger und der Touchscreen des Spiels.** Ein Finger, der weder auf einer Taste noch als Stick aufsetzt, ist frei.
Nur freie Finger gehen an den HID-Touchscreen (`TouchPad::take_touches` → `PadInput::sample_touch` →
`IInput::touch_screen()` → `HidState`), in Konsolenpixeln 1280×720 über dem Spielbild. Dafür nimmt `TouchPad` an, dass
der Renderer ein 16:9-Bild ins Fenster einpasst (Balken links/rechts bzw. auf 4:3 oben/unten). Ein Finger, der auf einem
Balken aufsetzt, zählt nicht. Wandert ein Finger aus dem Bild, bleibt er am Rand stehen. Finger-IDs sind 0–15 und bleiben,
solange der Finger liegt. Ein Tippen, das zwischen zwei hid-Takten beginnt und endet, wird einmal gemeldet. Ein freier
Finger drückt keine Taste, auch wenn er über eine rutscht (früher konnte ein Finger von leerer Fläche auf eine Taste
rutschen). hid setzt `Start` beim ersten Eintrag eines Fingers und meldet ihn nach dem Abheben einmal mit `End` an der
letzten Stelle. Der Durchmesser ist fest 15 (geschätzt, nicht gemessen). Ist das Overlay aus (`--touch off`, Einstellungen),
sind alle Finger frei und gehen nur noch an den Touchscreen.

**Gesten.** LGP importiert `nn::hid::InitializeTouchScreen`, `InitializeGesture` und `GetGestureStates`, aber kein
`GetTouchScreenState(s)` (`docs/analysis/01b-imports.md`). Berührungen erreicht das Spiel also, soweit es sie nutzt,
über die Gesten-LIFO. Laut Recherche nutzt LGP Touch im Handheld zum Streicheln des Partner-Pokémon und für Beeren; am
Spiel geprüft ist das hier nicht. Die Konsole leitet die Gesten im hid-Sysmodul ab. Hier tut das `GestureTracker`
(`runtime/hle/src/hid_gesture.*`) mit einem eigenen, ungeprüften Modell:
- Neue Berührung: `Touch` mit `IsNewTouch` und neuer Kontextnummer.
- Ruhig gehalten: nach 0,5 s `Press`.
- Mehr als 16 px bewegt: `Pan` mit Delta und Geschwindigkeit in px/s über die letzten 50 ms.
- Letzter Finger hebt ab: `Tap` (≤ 0,4 s und ruhig; `IsDoubleTap`, wenn ≤ 0,3 s zuvor in 48 px ein Tap endete), `Swipe`
  mit Richtung (am Ende ≥ 1500 px/s) oder sonst `Complete`.
- Mehrere Finger melden ihren Mittelpunkt; `Pinch`/`Rotate` entstehen nicht.

Solange ein Finger liegt, bekommt die LIFO in jedem Takt einen Eintrag, im Leerlauf keinen. Tests: `runtime.hle_hid`
(`test_touch_screen`, `test_gestures`).

**Werfen per Wischen.** Ein freier Finger, der binnen 0,3 s mindestens 0,12 Fensterhöhen nach oben wischt und dabei
höchstens 70° neben senkrecht geht, löst einmal einen Wurf aus (`TouchPad::kSwipeDistance`, `kSwipeTimeNs`). Stellt
Slot 0 einen einzelnen Joy-Con dar (`--controller joycon-right|joycon-left`), setzt das `HostPad::throw_gesture`, also
dieselbe Geste wie die Leertaste. Im Handheld-Modus, wo A wirft, drückt es A für 100 ms (`kSwipePressNs`). Tippen auf
freie Fläche löst am Pad nichts aus. Wischen auf Stick oder Tasten zählt nicht. Test: `platform.touch_pad`
(`test_touches`, `test_swipe`, `test_swipe_direction`). Schwellen ungeprüft am Gerät.
Erlaubt sind bis 70° neben senkrecht (`kSwipeMaxAngle`). Der Winkel wird zu `HostPad::throw_side` (−1 ab 35° links,
1 ab 35° rechts, `kSwipeFullSideAngle`; vorher 45° für beides, Paul am iPad 2026-10-22: zu flach, stark nach rechts
gewischt passierte nichts), das Tempo vom Aufsetzen bis zum Auslösen zu `throw_strength` (bis 1,2 Fensterhöhen/s → 1, ab
3,6 → 2, dazwischen linear; ohne Ereigniszeiten 1) und zu `throw_reach` (über dieselben Tempi −1…1, ohne Ereigniszeiten 0). Gemessen wird also der Anfang des Wischens bis 0,12 Höhen, nicht
das Ende. Nur der einzelne Joy-Con nutzt das, und ab Werk nur die Richtung, weil eine höhere Stärke in LGP steiler statt weiter wirft (Abschnitt Bewegung, „Richtung und Stärke“); im Handheld-Modus wirft
LGP per A und zielt per Gyro, dort gehen Richtung und Tempo verloren. Das Wischen wirkt in allen Stilen, aber nicht,
solange ein Gamepad Spieler 1 stellt (`player_pad(0)`; Pauls Entscheidung 2026-10-21: die Hände sind am Pad, ein
versehentliches Wischen würde werfen). Als spielend gilt ein Pad erst, sobald seit dem Verbinden eine Taste daran
gedrückt wurde (`Pad::used`); ein nur gekoppeltes, liegengelassenes Pad schaltet das Wischen nicht ab (Standrunde
2026-10-22: vorher reichte das Verbinden). Einstellungen: „Pokébälle per Wischen werfen“ (`settings.ini`
`touch_throw = 0|1`, Standard 1) und darunter „Wischen auch mit Controller“ (`touch_throw_pad = 0|1`, Standard 0),
Startprogramm `--touch-throw on|off|always` (`always` = auch mit Gamepad). `SdlInput::update_swipe_action` setzt die
`SwipeAction` bei jedem `sample()` neu, kommt oder geht ein Gamepad, gilt das ab dem nächsten Wischen (Test
`platform.sdl_input`, `test_touch_throw_with_gamepad`).

**Zielen im Handheld mit dem Gerät.** `SdlInputOptions::device_motion` (Standard wie `touch`: iOS, Android außer TV) öffnet
Gyro und Beschleunigungssensor des Telefons bzw. Tablets (SDL-Sensoren, nicht die eines Gamepads). Ihre Werte werden zur
Bewegung von Slot 0, solange kein Gamepad diesen Slot hält und dessen Pad keine eigenen Bewegungsdaten hat. `--pad-motion off`
schaltet auch das ab; mit `--press` oder `--clock virtual` ist es immer aus (ein liegendes Gerät würde sonst Sensorrauschen
in reproduzierbare Läufe tragen). Umrechnung: Gerätachsen (Hochformat: x rechts, y oben, z aus dem Bildschirm) nach der
Bildschirmausrichtung (`SDL_ORIENTATION_LANDSCAPE` = rechte Geräteseite oben, `_FLIPPED` = linke) in den Rahmen eines
vorgehaltenen Controllers, dann wie bei Gamepads (`to_switch_frame`). Achsen und Vorzeichen sind am Spiel nicht geprüft.
Zielen per Finger (halten und ziehen) gibt es nicht.

**Lage zurücksetzen.** hid integriert die Drehrate zu `angle` und `direction` (`SixAxis`), ohne Lagekorrektur. Fehler
und Rauschen summieren sich also. Zwei Wege setzen beides auf die Startlage (`SixAxis::reset_orientation`):
L und R zusammen an einem Controller (einmal je Drücken, Logzeile `hid: L+R: motion sensor orientation reset`) und `StartSixAxisSensor`, wenn der Sensor vorher mindestens 30 hid-Takte gestoppt war. Ein
Stop/Start im selben Bild behält die Lage. Ob LGP `direction` oder nur die Drehrate liest und ob das Pauls Drift nach
mehrfachem Schwenken behebt, ist am Gerät nicht geprüft (Verdacht). Test `runtime.hle_hid` (`test_six_axis_recenter`).
Prüfen lässt sich die erste Frage mit `--motion-pose frozen` (`HidConfig::freeze_orientation`): `angle` bleibt 0,
`direction` die Einheitsmatrix, Drehrate und Beschleunigung gehen unverändert durch. Zielt man damit genauso wie mit
`--motion-pose on`, liest das Spiel nur die Drehrate, und Lage-Reset oder Lagekorrektur bringen nichts. Test
`test_six_axis_frozen_pose`.

Im Stil „einzelner Joy-Con“ macht `PadInput` aus den Schultertasten des Hosts SL/SR. Deshalb zählt SL+SR desselben
Joy-Con ebenfalls als L+R (iPad-Test 22.10.: im Joy-Con-Stil setzte L+R nichts zurück, und der Ball flog). Das Spiel
sieht das Tastenpaar nicht (`HidState::filter_recenter`): Eine einzelne Schultertaste wartet bis zu 80 ms auf die
zweite. Kommt sie nicht, geht die Taste ab da durch, und ein kürzeres Tippen kommt beim Loslassen verspätet an. Kommen
beide, bleiben sie ausgeblendet, bis beide losgelassen sind. Der Preis sind bis zu 80 ms Verzögerung auf L/R bzw. SL/SR.
Test `test_recenter_chord`.

Mit `--input-log` zählt die Zeile `hid: 1s:` auch neue Finger auf dem Touchscreen (`touches N (first at x,y, screen …)`,
in 1280×720) und sagt, ob das Spiel den Touchscreen aktiviert hat. Damit lässt sich prüfen, ob Finger im Pikachu-Spiel
überhaupt ankommen und wo. Ob LGP im Stil „einzelner Joy-Con“ statt des Touchscreens einen Gyro-Zeiger nimmt, wie mit
abgenommenem Joy-Con an der Konsole, ist nicht geprüft (Verdacht).

**Bildschirm-Controller ausblenden.** Unten in der Mitte liegt ein kleiner Knopf (`TouchPad::Kind::Toggle`). Er blendet
Stick und Tasten aus und wieder ein (`TouchPad::set_hidden`). Ausgeblendet gehen alle übrigen Finger frei an den
Touchscreen, auch in der Stick-Zone links. Gedacht für „Mit Pikachu spielen“, wo Beeren gezogen und Pikachu
gestreichelt wird (Paul 2026-10-21: es kam nur Tippen oder kurzes Wischen an). Ausgeblendet wirft kein Wisch und
drückt kein A; jeder Finger geht nur als Berührung an den Touchscreen (Paul 2026-10-22: in diesem Modus wird nie
geworfen, die Berührung muss exakt ankommen; #410 hatte das kurz umgedreht). `--input-log` schreibt jeden Finger
(`touch: down/up/cancel …` mit Fenster- und Spielkoordinaten) und jeden erkannten Wisch (`touch: swipe …`), dazu hid
jeden SixAxis-Wert, den das Spiel bei einer Drehung über 0,5 U/s liest (`hid: motion …` mit accel, gyro, angle und
direction), um echte Würfe gegen den synthetischen Schwung zu vergleichen. `--throw sensors=replace` ersetzt während
des Schwungs die echten Gerätesensoren (iPad-Lage, Schwerkraft) ganz durch den Schwung mit seiner eigenen Ruhe-Schwerkraft,
statt ihn aufzuaddieren (Diagnose: Paul 22.10., im Joy-Con-Stil flog der Wischwurf auf dem iPad immer in die Mitte, am
PC mit fester Schwerkraft lenkt er). Der Knopf
schaltet erst, wenn sein Finger ihn ohne Bewegung verlässt (Tippen); ein Finger, der auf ihm landet und weiterzieht,
ist ab seinem Landepunkt frei. Ein Wurf-Wisch, der unten in der Mitte beginnt, wirft also und blendet nichts um. Test
`platform.touch_pad` (`test_toggle`, `test_toggle_swipe`).

**Von selbst ausblenden.** Das Spiel meldet „Mit Pikachu spielen“ nicht. Erkannt wird der Modus an einem Shader-Programm,
das nur dort gezeichnet wird (PC 22.10.: Hash `24a5b148`, etwa 18 Draws je Bild, sonst nirgends, auch nicht im X-Menü,
Kampf oder Pokédex). Das Startprogramm meldet dem WebGPU-Backend die Hash-Präfixe (`WebGpuBackend::watch_programs`,
CMake `LGP_TOUCH_ONLY_PROGRAMS`, Umgebung `LGP_TOUCH_ONLY_PROGRAMS` gewinnt, leer = aus) und fragt je Present ab, ob eins
gezeichnet wurde (`take_watched_drawn`). Dann blendet `SdlInput::set_touch_only_scene(true)` die Steuerung aus; nach
30 Bildern ohne solchen Draw kommt sie zurück, außer der Spieler hatte sie vorher selbst ausgeblendet. `--input-log`
schreibt `touch: scene with touches only began/ended`. Test `nvn.webgpu_draw` (Block `watch_programs`). Nur mit
`--renderer window` und SDL; ein Spiel-Update kann den Hash ändern.
The program hash is computed from the translated WGSL, so every change to the shader translator moves it: on
2026-10-30 `24a5b148` no longer matched (Android emulator E9, the controls stayed in the scene and the floating stick
took the touches meant for the berries on its half). A prefix therefore also matches the guest code hash
(`nvn::ProgramDesc::guest_hash`, FNV-1a 64 of the shader code the game hands over), which only a game update changes.
`LGP_WEBGPU_DEBUG=firstdraw` logs every program's first draw with both hashes
(`nvn webgpu: program N first drawn after present P: hash … guest …`); the programs that first draw on entering the
scene are the candidates for `LGP_TOUCH_ONLY_PROGRAMS`. Default since 2026-10-30: guest hash `d519effb` (Android
emulator, two runs: the controls hide on entering, no false alarm in the field or the X menu; Pokédex and battle not
yet checked). The game leaves the scene with B only: tap the toggle at the bottom centre to bring the controls back,
as on the iPhone and iPad (Paul 2026-10-30: no button kept on screen during the scene). The CMake cache holds `default` for
`LGP_TOUCH_ONLY_PROGRAMS` (the built-in list, `LGP_TOUCH_ONLY_PROGRAMS_DEFAULT`), so a changed default reaches existing
build trees; an old `24a5b148` in a cache is reset to `default`, empty turns the detection off.

**Offen:** Die Abbildung Fenster → Spielbild nimmt immer 16:9 mit Einpassen an. Kommt ein anderer Bildmodus (z. B.
„fill“ mit Beschnitt), muss `TouchPad::to_game` die Abbildung des Renderers übernehmen
(`WebGpuRenderer::window_to_image`, im Branch `claude/ios-fullscreen`). Ebenfalls offen: ob LGP Gesten oder Touch-Punkte
liest und welche Gestentypen es erwartet, und ob das Wischen im Handheld wie gewünscht wirft.

## Diagnose im Startprogramm

- `--input-log` schreibt (1) jede Änderung eines Vibrationswerts, den das Spiel schickt, mit Befehl (#201 `SendVibrationValue`, #206 `SendVibrationValues`), Handle, beiden Amplituden und Frequenzen und dem Ergebnis: `motor` oder `dropped: style not presented` bzw. `dropped: no npad`; (2) einmal pro Sekunde eine hid-Zeile (`hid: 1s: …`) mit den Vibrationswerten, den Antworten auf `IsSixAxisSensorAtRest`, den „Stick-Einbrüchen“, wie das Spiel sie liest, größter und kleinster gehaltener Stick-Länge je Npad nach der Abbildung (`stick max/held min`) samt Zahl der Wertänderungen des gehaltenen, längeren Sticks (beim einzelnen rechten Joy-Con der rechte) und größter Richtungsänderung zwischen zwei Takten (`N changes, max turn X deg`), und der größten Lücke im 5-ms-Takt von hid; (3) einmal pro Sekunde und Pad eine SDL-Zeile (`lgp: input 1s pad …`) mit den SDL-Ereignissen nach Art, der größten Lücke zwischen zwei Ereignissen des Pads, den Stick-Einbrüchen auf SDLs Rohwerten, der größten Länge des linken Sticks und der kleinsten, solange er über halb ausgelenkt ist (`left stick max … held min …`), den Rumble-Aufrufen samt Maximum und der größten Lücke der Hauptschleife.
- **Stick-Einbruch** (`StickDropCounter`): Ein Stick mit ≥ 50 % Auslenkung fällt unter 20 % und ist binnen 150 ms wieder bei ≥ 50 %. So schnell lässt kein Spieler los und greift wieder zu; ein Gerät oder ein Pfad, der Werte verliert, schon. Einbrüche in der SDL-Zeile liegen am Gerät oder Treiber, Einbrüche nur in der hid-Zeile an der Abbildung dazwischen.
- `--pad-motion off` lässt Gyro und Beschleunigungssensor der Gamepads aus (keine Sensor-Ereignisse, keine Bewegungsdaten). Damit lässt sich prüfen, ob ein Fehler mit den Sensoren zusammenhängt.
- `--motion-frame SPEC` drückt die echten Sensordaten von Spieler 1 in einem anderen Rahmen aus, z. B. `-y,x,z` (out = Vorzeichen · in[Achse], für Beschleunigung und Drehrate gleich; `parse_motion_frame`). Für Versuche, ob ein Spiel einen einzelnen Joy-Con anders gehalten erwartet als das Host-Pad.
- `--motion-gain G` verstärkt die echte Drehrate (Beschleunigung bleibt, damit die Schwerkraft 1 G bleibt; `MapperConfig::gyro_gain`). Die Einheiten stimmen ohne Faktor: SDL liefert rad/s und m/s², hid erwartet Umdrehungen/s und G (`sdl_input.cpp`, geteilt durch 2π bzw. `SDL_STANDARD_GRAVITY`). Gedacht für Geräte, die schwerer zu schwenken sind als ein Joy-Con (Pro Controller als einzelner Joy-Con, Paul 2026-10-13: „viel zu unempfindlich“, mit 2 „noch zu stumpf“, mit 3 „okay“). Standard des Startprogramms: 3 für Slots, die einen einzelnen Joy-Con zeigen, 1 für Handheld (dort zielte der Pro Controller schon mit 1 „genau so, wie es soll“). Ein echter Joy-Con über SDL braucht vermutlich `--motion-gain 1` (nicht geprüft).
- `rumble failed` und das Abstecken (`lgp: gamepad … removed`) stehen immer im Log, auch ohne `--input-log`.

## Threads

- **SDL** liefert Ereignisse an den Thread, der sie pumpt, also an den Haupt-Thread (`SdlWindow::pump_events`). Das Backend hält deshalb einen eigenen Zustand, den es aus den Ereignissen (Tasten, Achsen, Sensoren, An- und Abstecken) fortschreibt, statt SDL aus dem hid-Thread abzufragen. hid liest nur diesen Zustand unter einer Sperre.
- **Browser** (`PROXY_TO_PTHREAD`, ADR-0002): Die Tastatur-Callbacks sind mit `EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD` registriert, laufen also im Haupt-Thread des Browsers und setzen nur atomare Flags. `poll()` läuft im hid-Takt-Thread und liest die Gamepads über `emscripten_sample_gamepad_data`/`emscripten_get_gamepad_status`. Beide sind in Emscripten als `__proxy: 'sync'` markiert und laufen damit synchron im Haupt-Thread. `browser.web_input` belegt diesen Weg: Playwright hält Z, Pfeil hoch und D, die Probe sieht A, Oben und den Stick rechts.

## Testen ohne Geräte

- SDL3 hat **virtuelle Joysticks** (`SDL_AttachVirtualJoystick`, Typ `SDL_JOYSTICK_TYPE_GAMEPAD`) mit Tasten, Achsen, Sensoren (`SDL_SendJoystickVirtualSensorData`) und Rumble-Rückruf. Damit lässt sich das Backend inklusive An- und Abstecken ohne Gerät und ohne Display testen. Dafür reicht `SDL_INIT_GAMEPAD`, Video ist nicht nötig.
- Ältere Build-Bäume behalten `SDL_AUDIO`/`SDL_JOYSTICK`/… = OFF im CMake-Cache, wenn sie zuvor mit `FORCE` abgeschaltet waren. `cmake/LgpSdl.cmake` setzt die P5-Subsysteme deshalb ebenfalls mit `FORCE` wieder an.
