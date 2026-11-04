#include "lgp/platform/ui_text.hpp"

#include <atomic>
#include <cctype>
#include <string_view>
#include <unordered_map>

namespace lgp::platform {

namespace {

struct Row {
    const char* english;
    const char* german;
};

// English text -> German. tests/platform/check_ui_text.py (platform.ui_text_table)
// finds tr()/trf() texts without a line here and lines nobody uses any more.
const Row kGerman[] = {
    // Settings pages (platform/ui/src/app_menu.cpp)
    {"Settings", "Einstellungen"},
    {"Controls", "Steuerung"},
    {"Touch controls", "Touch-Steuerung"},
    {"Size",
     "Gr\xC3\xB6\xC3\x9F" "e"},
    {"Opacity", "Deckkraft"},
    {"Stick", "Stick"},
    {"Left", "Links"},
    {"Right", "Rechts"},
    {"Throw Pok\xC3\xA9 Balls by swiping",
     "Pok\xC3\xA9" "b\xC3\xA4lle per Wischen werfen"},
    {"Swipe with a controller too", "Wischen auch mit Controller"},
    {"Controller", "Controller"},
    {"Handheld", "Handheld"},
    {"Joy-Con right", "Joy-Con rechts"},
    {"Joy-Con left", "Joy-Con links"},
    {"Size, opacity and side come with the next update.",
     "Gr\xC3\xB6\xC3\x9F" "e, Deckkraft und Seite kommen mit dem n\xC3\xA4" "chsten Update."},
    {"The on-screen controls are off. Size, opacity and side apply once they are on.",
     "Die Bildschirmsteuerung ist aus. Gr\xC3\xB6\xC3\x9F" "e, Deckkraft und Seite gelten, sobald sie an ist."},
    {"Stick and buttons on the screen, for playing without a controller.",
     "Stick und Kn\xC3\xB6pfe auf dem Bildschirm, zum Spielen ohne Controller."},
    {"How large the stick and buttons on the screen are.",
     "Wie gro\xC3\x9F Stick und Kn\xC3\xB6pfe auf dem Bildschirm sind."},
    {"How clearly the stick and buttons show.", "Wie deutlich Stick und Kn\xC3\xB6pfe zu sehen sind."},
    {"Which side the stick is on; right for left-handers.",
     "Auf welcher Seite der Stick liegt; rechts f\xC3\xBCr Linksh\xC3\xA4nder."},
    {"A swipe upwards throws a Pok\xC3\xA9 Ball while catching.",
     "Ein Wisch nach oben wirft beim Fangen einen Pok\xC3\xA9" "ball."},
    {"Swiping throws even while player 1 plays with a controller.",
     "Wischen wirft auch, solange Spieler 1 mit einem Controller spielt."},
    {"Applies from the next start.",
     "Gilt ab dem n\xC3\xA4" "chsten Start."},
    {"Takes effect at once.", "Wirkt sofort."},
    {"Which controller the game is told about.", "Welcher Controller dem Spiel gemeldet wird."},
    {"Player 1", "Spieler 1"},
    {"Connection", "Verbindung"},
    {"Button press", "Tastendruck"},
    {"Player 1 is the controller on which a button is pressed first.",
     "Spieler 1 ist der Controller, an dem zuerst eine Taste gedr\xC3\xBC" "ckt wird."},
    {"Player 1 is the controller connected first.", "Spieler 1 ist der zuerst verbundene Controller."},
    {"Now: {}.", "Jetzt: {}."},
    {"Choose player 1 again", "Spieler 1 neu w\xC3\xA4hlen"},
    {"Open iOS Settings",
     "iOS-Einstellungen \xC3\xB6" "ffnen"},
    {"Controller, graphics and connection are in Settings > Apps > LGP and apply from the next start.",
     "Controller, Grafik und Verbindung stehen in Einstellungen > Apps > LGP und gelten ab dem n\xC3\xA4" "chsten "
     "Start."},
    {"Sound", "Ton"},
    {"Volume", "Lautst\xC3\xA4rke"},
    {"Mute", "Stumm"},
    {"Volume of the game and its videos.", "Lautst\xC3\xA4rke des Spiels und seiner Videos."},
    {"Picture", "Bild"},
    {"Picture format", "Bildformat"},
    {"Fit", "Einpassen"},
    {"Fill", "F\xC3\xBCllen"},
    {"Stretch", "Strecken"},
    {"Fit: 16:9 with borders. Fill: no borders, cropped. Stretch: no borders, distorted.",
     "Einpassen: 16:9 mit R\xC3\xA4ndern. F\xC3\xBCllen: ohne R\xC3\xA4nder, beschnitten. Strecken: ohne "
     "R\xC3\xA4nder, verzerrt."},
    {"16:9 with borders", "16:9 mit Rand"},
    {"16:9 fill", "16:9 f\xC3\xBCllen"},
    {"16:9 stretched", "16:9 gestreckt"},
    {"Widescreen: more of the game world to fit the screen, nothing stretched; applies from the next start. 16:9 "
     "with borders: as on the console. 16:9 fill: no borders, cropped. 16:9 stretched: no borders, distorted.",
     "Breitbild: mehr Spielwelt passend zum Bildschirm, nichts gestreckt; gilt ab dem n\xC3\xA4" "chsten Start. "
     "16:9 mit Rand: wie auf der Konsole. 16:9 f\xC3\xBCllen: ohne Rand, beschnitten. 16:9 gestreckt: ohne Rand, "
     "verzerrt."},
    {"With widescreen the TV mode applies from the next start.",
     "Mit Breitbild gilt der TV-Modus ab dem n\xC3\xA4" "chsten Start."},
    {"TV mode (1080p)", "TV-Modus (1080p)"},
    {"TV mode: the game renders at 1920\xC3\x97" "1080 as in the dock, sharper, more load on the GPU.",
     "TV-Modus: Das Spiel rechnet in 1920\xC3\x97" "1080 wie im Dock, sch\xC3\xA4rfer, mehr Last f\xC3\xBCr die GPU."},
    {"Handheld mode: the game renders at 1280\xC3\x97" "720, easier on GPU and battery.",
     "Handheld-Modus: Das Spiel rechnet in 1280\xC3\x97" "720, schont GPU und Akku."},
    {"Apple TV: untested, the larger pictures can exceed the memory (the app is ended).",
     "Apple TV: ungetestet, die gr\xC3\xB6\xC3\x9F" "eren Bilder k\xC3\xB6nnen den Speicher sprengen (App wird "
     "beendet)."},
    {"Smoothing", "Bildgl\xC3\xA4ttung"},
    {"On", "An"},
    {"Off", "Aus"},
    {"Soft", "Weich"},
    {"How the picture is scaled to the screen. Off: sharp, pixels of unequal size. Soft: even, a little softer. "
     "FXAA: smoothed edges as well, text gets a little softer too.",
     "Wie das Bild auf den Bildschirm skaliert wird. Aus: scharf, Pixel ungleich gro\xC3\x9F. Weich: "
     "gleichm\xC3\xA4\xC3\x9Fig, etwas weicher. FXAA: zus\xC3\xA4tzlich gegl\xC3\xA4ttete Kanten, auch Schrift wird "
     "etwas weicher."},
    {"Light effects", "Lichteffekte"},
    {"Full", "Voll"},
    {"Half", "Halb"},
    {"Half: expensive light and glow effects at half resolution, a little softer, less load on the GPU.",
     "Halb: teure Licht- und Leuchteffekte in halber Aufl\xC3\xB6sung, etwas weicher, weniger Last f\xC3\xBCr die "
     "GPU."},
    {"Resolution", "Aufl\xC3\xB6sung"},
    {"How many pixels the game renders. Fewer is blurrier and eases the GPU; after a change it may stutter more "
     "often at first, until the shaders are translated again.",
     "Wie viele Bildpunkte das Spiel rechnet. Weniger ist unsch\xC3\xA4rfer und entlastet die GPU; nach dem Wechsel "
     "kann es anfangs \xC3\xB6" "fter ruckeln, bis die Shader neu \xC3\xBC" "bersetzt sind."},
    {"Battle HUD bounds", "HUD-Grenzen im Kampf"},
    {"With the widescreen mod: where the displays on the battle screen sit on a taller or wider screen, between the "
     "16:9 frame and the edge. Everything else always goes to the edge. Nothing is stretched.",
     "Mit der Breitbild-Mod: wo die Anzeigen auf dem Kampfbild auf einem h\xC3\xB6heren oder breiteren Bildschirm "
     "sitzen, zwischen dem 16:9-Rahmen und dem Rand. Alles andere geht immer bis zum Rand. Nichts wird gestreckt."},
    {"Height: {}", "H\xC3\xB6he: {}"},
    {"Width: {}", "Breite: {}"},
    {"The picture is 16:9: there is no room beyond the 16:9 frame.",
     "Das Bild ist 16:9: au\xC3\x9F" "erhalb des 16:9-Rahmens ist kein Platz."},
    {"The battle's displays stay inside the yellow lines. Arrows or a drag move them, opposite lines together.",
     "Die Anzeigen im Kampf bleiben innerhalb der gelben Linien. Pfeile oder Ziehen verschieben sie, "
     "gegen\xC3\xBC" "berliegende Linien gemeinsam."},
    {"Open this screen during a battle to see its displays behind the lines.",
     "\xC3\x96" "ffne diesen Bildschirm w\xC3\xA4hrend eines Kampfes, dann siehst du seine Anzeigen hinter den "
     "Linien."},
    {"Applies from the next battle.", "Gilt ab dem n\xC3\xA4" "chsten Kampf."},
    {"Apply", "\xC3\x9C" "bernehmen"},
    {"Discard", "Verwerfen"},
    {"Graphics thread", "Grafik-Thread"},
    {"On: graphics on a thread of their own, one frame ahead. Whether that brings more frames per second depends on "
     "the device.",
     "An: Grafik auf eigenem Thread, ein Bild voraus. Ob das mehr Bilder pro Sekunde bringt, h\xC3\xA4ngt vom "
     "Ger\xC3\xA4t ab."},
    {"Game", "Spiel"},
    {"Trade and battle over Wi-Fi", "Tauschen und K\xC3\xA4mpfen im WLAN"},
    {"Link trades and link battles with other devices on the same Wi-Fi (in the game: players nearby).",
     "Link-Tausch und Link-Kampf mit anderen Ger\xC3\xA4ten im selben WLAN (im Spiel: Spieler in der N\xC3\xA4he)."},
    {"Server", "Server"},
    {"Online (in the game: players far away) goes over this server. Another server's address, HOST[:PORT] or "
     "ws[s]://HOST[:PORT][/PATH]. Empty: the server of this build.",
     "Online (im Spiel: Spieler in der Ferne) l\xC3\xA4uft \xC3\xBC" "ber diesen Server. Adresse eines anderen "
     "Servers, HOST[:PORT] oder ws[s]://HOST[:PORT][/PFAD]. Leer: der Server dieses Baus."},
    {"Default", "Standard"},
    {"Room", "Raum"},
    {"Only devices with the same room find each other. Empty: everyone on the server.",
     "Nur Ger\xC3\xA4te mit demselben Raum finden sich. Leer: alle auf dem Server."},
    {"Public",
     "\xC3\x96" "ffentlich"},
    {"Language of the game", "Sprache des Spiels"},
    {"Device: this device's language if the game has it, else English. Applies from the next start.",
     "Ger\xC3\xA4t: die Sprache dieses Ger\xC3\xA4ts, wenn das Spiel sie hat, sonst Englisch. Gilt ab dem "
     "n\xC3\xA4" "chsten Start."},
    {"Takes effect at once, as if the controller were swapped.",
     "Wirkt sofort, als w\xC3\xBCrde der Controller gewechselt."},
    {"Main menu at start", "Hauptmen\xC3\xBC beim Start"},
    {"On: this menu comes before the game, to choose a save. Off: the game starts at once with the last save; the "
     "pause button opens the menu. Applies from the next start.",
     "An: Dieses Men\xC3\xBC kommt vor dem Spiel, zur Wahl des Spielstands. Aus: Das Spiel startet sofort mit dem "
     "letzten Spielstand, der Pause-Knopf \xC3\xB6" "ffnet das Men\xC3\xBC. Gilt ab dem n\xC3\xA4" "chsten Start."},
    // The game's languages (kMenuLanguages, tr() of a variable)
    {"Device", "Ger\xC3\xA4t"},
    {"German", "Deutsch"},
    {"English (US)", "Englisch (USA)"},
    {"English (UK)", "Englisch (GB)"},
    {"French", "Franz\xC3\xB6sisch"},
    {"French (Canada)", "Franz\xC3\xB6sisch (Kanada)"},
    {"Italian", "Italienisch"},
    {"Spanish", "Spanisch"},
    {"Spanish (Latin America)", "Spanisch (Lateinamerika)"},
    {"Japanese", "Japanisch"},
    {"Korean", "Koreanisch"},
    {"Chinese (simplified)", "Chinesisch (vereinfacht)"},
    {"Chinese (traditional)", "Chinesisch (traditionell)"},
    // Screens
    {"Main menu", "Hauptmen\xC3\xBC"},
    {"New game", "Neues Spiel"},
    {"Continue", "Weiterspielen"},
    {"Choose save", "Spielstand w\xC3\xA4hlen"},
    {"Load iCloud state", "iCloud-Stand laden"},
    {"Quit", "Beenden"},
    {"Select", "Ausw\xC3\xA4hlen"},
    {"New save", "Neuer Spielstand"},
    {"Preparing graphics", "Grafik wird vorbereitet"},
    {"The first start builds the game's shaders once, so the game does not stutter later. Later "
     "starts are quicker.",
     "Der erste Start baut die Shader des Spiels einmal, damit das Spiel sp\xC3\xA4ter nicht ruckelt. "
     "Sp\xC3\xA4tere Starts gehen schneller."},
    {"Start now", "Jetzt starten"},
    {"The rest is built while playing; the game may stutter.",
     "Der Rest wird w\xC3\xA4hrend des Spielens gebaut; das Spiel kann ruckeln."},
    {"Back",
     "Zur\xC3\xBC" "ck"},
    {"Cancel", "Abbrechen"},
    {"Change", "\xC3\x84ndern"},
    {"Page", "Seite"},
    {"Pause", "Pause"},
    {"Resume", "Fortsetzen"},
    {"Back up save", "Spielstand sichern"},
    {"The game saves in the game (menu \xE2\x86\x92 Save). \"Back up\" also makes a copy of the save.",
     "Gespeichert wird im Spiel (Men\xC3\xBC \xE2\x86\x92 Speichern). \"Sichern\" legt zus\xC3\xA4tzlich eine Kopie "
     "des Spielstands an."},
    {"Quit game", "Spiel beenden"},
    {"Note", "Hinweis"},
    {"Enter text", "Text eingeben"},
    // Software keyboard window (platform/desktop/src/sdl_text_input.cpp)
    {"Text", "Text"},
    {"Enter OK", "Enter OK"},
    {"Enter OK, Esc cancel", "Enter OK, Esc Abbruch"},
    {"Enter: OK", "Enter: OK"},
    {"Enter: OK   Esc: cancel", "Enter: OK   Esc: Abbruch"},
    {"max {}", "max. {}"},
    {"length {} not allowed", "L\xC3\xA4nge {} passt nicht"},
    {"(at least {})", "(mindestens {})"},
    // Save slots and notes on the start screen (runtime/boot/src/main.cpp)
    {"Save {}", "Spielstand {}"},
    {"empty", "leer"},
    {"last played on %b %d at %H:%M", "zuletzt gespielt am %d.%m. um %H:%M"},
    {"Own folder", "Eigener Ordner"},
    {"The game is still busy, please back up again in a moment.",
     "Das Spiel ist gerade noch besch\xC3\xA4" "ftigt, bitte gleich noch einmal sichern."},
    {"Backup failed: {}", "Sichern ging nicht: {}"},
    {"Backed up to {}", "Gesichert nach {}"},
    {"After closing the menu: press a button on the controller that is to be player 1.",
     "Nach dem Schlie\xC3\x9F" "en des Men\xC3\xBCs: eine Taste am Controller dr\xC3\xBC" "cken, der Spieler 1 sein "
     "soll."},
    {"Not like this: HOST[:PORT] or ws[s]://HOST[:PORT][/PATH].",
     "So nicht: HOST[:PORT] oder ws[s]://HOST[:PORT][/PFAD]."},
    {"No save folder under {} (write permission, disk space?)",
     "Kein Spielstandordner unter {} (Schreibrechte, Speicherplatz?)"},
    {"No new save: {}", "Kein neuer Spielstand: {}"},
    {"{}: not loaded in this run (mod file missing or the game was translated without --mods).",
     "{}: in diesem Lauf nicht geladen (Mod-Datei fehlt oder das Spiel wurde ohne --mods \xC3\xBC" "bersetzt)."},
    // iCloud (runtime/boot/src/main.cpp, save_backup.cpp)
    {"Warning: saves are not backed up in iCloud, tvOS may delete them.",
     "Achtung: Spielst\xC3\xA4nde werden nicht in iCloud gesichert, tvOS kann sie l\xC3\xB6schen."},
    {"iCloud has another state (another device). It replaces the saves here, which are kept as a backup.",
     "iCloud hat einen anderen Stand (anderes Ger\xC3\xA4t). Er ersetzt die Spielst\xC3\xA4nde hier, diese bleiben "
     "als Sicherung."},
    {"iCloud state not loaded: {}", "iCloud-Stand nicht geladen: {}"},
    {"tvOS may delete them.", "tvOS kann sie l\xC3\xB6schen."},
    {"Warning: no iCloud account. Saves are not backed up.",
     "Achtung: Kein iCloud-Konto. Spielst\xC3\xA4nde werden nicht gesichert."},
    {"Warning: the iCloud backup does not work right now ({}). Saves are not backed up.",
     "Achtung: iCloud-Sicherung geht gerade nicht ({}). Spielst\xC3\xA4nde werden nicht gesichert."},
    {"Warning: saves in iCloud cannot be fetched ({}).",
     "Achtung: Spielst\xC3\xA4nde in iCloud nicht abrufbar ({})."},
    {"Warning: restoring from iCloud failed ({}).", "Achtung: Wiederherstellen aus iCloud ging nicht ({})."},
    {"Saves restored from iCloud.", "Spielst\xC3\xA4nde aus iCloud wiederhergestellt."},
    {"Warning: iCloud has another save; not backed up.",
     "Achtung: iCloud hat einen anderen Spielstand; nicht gesichert."},
    {"Saves are backed up in iCloud.", "Spielst\xC3\xA4nde werden in iCloud gesichert."},
    {"Warning: save not backed up ({}).", "Achtung: Spielstand nicht gesichert ({})."},
    {"Saves backed up in iCloud ({}).", "Spielst\xC3\xA4nde in iCloud gesichert ({})."},
    {"Warning: last save not in iCloud ({}).", "Achtung: Letzter Spielstand nicht in iCloud ({})."},
    {"iCloud state loaded. This device's previous state is in {}.",
     "iCloud-Stand geladen. Der bisherige Stand dieses Ger\xC3\xA4ts liegt in {}."},
    // The game folder (runtime/boot/src/app.cpp, main.cpp)
    {"Game: included in the app", "Spiel: in der App enthalten"},
    {"Game: the app's folder \"game\"", "Spiel: Ordner \"game\" der App"},
    {"Game: {}", "Spiel: {}"},
    {"The folder does not exist.", "Der Ordner existiert nicht."},
    {"The folder lacks the subfolder exefs (the game's program files).",
     "Im Ordner fehlt der Unterordner exefs (die Programmdateien des Spiels)."},
    {"exefs lacks the file {}.", "In exefs fehlt die Datei {}."},
    {"The folder lacks romfs (subfolder) or romfs.bin (the game data).",
     "Im Ordner fehlt romfs (Unterordner) bzw. romfs.bin (die Spieldaten)."},
    {"Please choose the folder with your game dump. It holds the subfolders exefs and romfs (or the file romfs.bin).",
     "Bitte den Ordner mit deinem Spiel-Dump w\xC3\xA4hlen. Er enth\xC3\xA4lt die Unterordner exefs und romfs (oder "
     "die Datei romfs.bin)."},
    {"The saved game folder no longer fits:\n{}\n{}\n\nPlease choose the folder again.",
     "Der gespeicherte Spielordner passt nicht mehr:\n{}\n{}\n\nBitte den Ordner neu w\xC3\xA4hlen."},
    {"Choose folder", "Ordner w\xC3\xA4hlen"},
    {"Choose the game folder (with exefs and romfs)", "Spielordner w\xC3\xA4hlen (mit exefs und romfs)"},
    {"The folder picker failed: {}\nYou can also enter the folder in settings.ini (game = ...):\n{}",
     "Die Ordnerauswahl ging nicht: {}\nDen Ordner kannst du auch in settings.ini eintragen (game = ...):\n{}"},
    {"This folder does not fit:\n{}\n{}\n\nPlease choose another folder.",
     "Dieser Ordner passt nicht:\n{}\n{}\n\nBitte einen anderen Ordner w\xC3\xA4hlen."},
    {"The saved game folder no longer fits:\n{}\n{}\n\nPlease choose the folder with your game dump, it is copied "
     "into the app.",
     "Der gespeicherte Spielordner passt nicht mehr:\n{}\n{}\n\nBitte den Ordner mit deinem Spiel-Dump "
     "w\xC3\xA4hlen, er wird in die App kopiert."},
    {"The game data in the app does not fit:\n{}\n\nPlease choose the folder with your game dump, it is copied into "
     "the app again.",
     "Die Spieldaten in der App passen nicht:\n{}\n\nBitte den Ordner mit deinem Spiel-Dump w\xC3\xA4hlen, er wird "
     "neu in die App kopiert."},
    {"Welcome! To start, the app needs your own game dump: a folder with the subfolder exefs and the folder romfs or "
     "the file romfs.bin.\n\nPlease choose this folder, it is copied into the app once.",
     "Willkommen! F\xC3\xBCr den Start braucht die App deinen eigenen Spiel-Dump: einen Ordner mit dem Unterordner "
     "exefs und dem Ordner romfs oder der Datei romfs.bin.\n\nBitte diesen Ordner w\xC3\xA4hlen, er wird einmalig in "
     "die App kopiert."},
    {"That did not work:\n{}\n\nPlease choose another folder.",
     "Das hat nicht geklappt:\n{}\n\nBitte einen anderen Ordner w\xC3\xA4hlen."},
    {"The game folder from settings.ini does not fit:\n{}\n{}",
     "Der Spielordner aus settings.ini passt nicht:\n{}\n{}"},
    {"The game folder does not fit:\n{}\n{}", "Der Spielordner passt nicht:\n{}\n{}"},
    {"No game found. This app does not contain the game: copy the folder \"game\" (exefs, romfs.bin and the rest "
     "the build tool made) into the app once, on a Mac in the Finder (select the device, then Files, then LGP), on "
     "Windows in the Apple Devices app (Files), on iPad and iPhone also in the Files app (On My iPad > LGP; on the "
     "Apple TV from the Mac with xcrun devicectl device copy to). The app looks in\n{}\nThen start it again.",
     "Kein Spiel gefunden. Diese App enth\xC3\xA4lt das Spiel nicht: Kopiere den Ordner \"game\" (exefs, romfs.bin "
     "und den Rest, den das Bau-Werkzeug angelegt hat) einmal in die App, am Mac im Finder (Ger\xC3\xA4t "
     "w\xC3\xA4hlen, dann Dateien, dann LGP), unter Windows in der App Apple-Ger\xC3\xA4te (Dateien), auf iPad und "
     "iPhone auch in der Dateien-App (Auf meinem iPad > LGP; auf dem Apple TV vom Mac aus mit xcrun devicectl device "
     "copy to). Die App sucht in\n{}\nDanach neu starten."},
    {"exefs/{} is from another version of the game (build ID {}, this app needs {}).",
     "exefs/{} stammt aus einer anderen Version des Spiels (Build-ID {}, diese App braucht {})."},
    {"The game folder belongs to another build of this app. Copy the game folder of this build into the app again.",
     "Der Spielordner geh\xC3\xB6rt zu einem anderen Bau dieser App. Kopiere den Spielordner dieses Baus erneut in "
     "die App."},
    {"No folder for settings and saves: {}", "Kein Ordner f\xC3\xBCr Einstellungen und Spielst\xC3\xA4nde: {}"},
    {"The settings cannot be read:\n{}", "Die Einstellungen lassen sich nicht lesen:\n{}"},
    {"The game ended with an error (code {}).\nLog: {}",
     "Das Spiel wurde mit einem Fehler beendet (Code {}).\nProtokoll: {}"},
    // Game mods (lgp::mods::ModInfo title and description, tr() of a variable)
    {"Wild battles", "Wilde K\xC3\xA4mpfe"},
    {"Wild Pok\xC3\xA9mon you touch battle first, then the catching scene follows (needs the mod file).",
     "Ber\xC3\xBChrte wilde Pok\xC3\xA9mon k\xC3\xA4mpfen erst, danach folgt die Fangszene (braucht die Mod-Datei)."},
    {"Pok\xC3\xA9mon Yellow", "Pok\xC3\xA9mon Gelb"},
    {"On: random battles while walking where wild Pok\xC3\xA9mon live, with the chance per step, species and "
     "levels of Pok\xC3\xA9mon Yellow; no wild Pok\xC3\xA9mon stand in the field. Off: the game as it is "
     "(needs the mod file).",
     "An: Zufallsk\xC3\xA4mpfe beim Laufen, wo wilde Pok\xC3\xA9mon leben, mit Chance je Schritt, Arten und "
     "Stufen aus Pok\xC3\xA9mon Gelb; im Feld stehen keine wilden Pok\xC3\xA9mon. Aus: das Spiel, wie es ist "
     "(braucht die Mod-Datei)."},
    {"Encounters as in Yellow", "Begegnungen wie in Gelb"},
    {"Species, frequency and levels of the wild Pok\xC3\xA9mon as in Pok\xC3\xA9mon Yellow (needs the data in the "
     "folder mods, from the next start).",
     "Arten, H\xC3\xA4ufigkeit und Level der wilden Pok\xC3\xA9mon wie in Pok\xC3\xA9mon Gelb (braucht die Daten im "
     "Ordner mods, ab dem n\xC3\xA4" "chsten Start)."},
    {"EXP as in Yellow", "EP wie in Gelb"},
    {"Experience by the formula of Pok\xC3\xA9mon Yellow: only fighters, divided by their number, no bonus, catching "
     "gives no EXP (needs the mod file).",
     "Erfahrung nach der Formel von Pok\xC3\xA9mon Gelb: nur K\xC3\xA4mpfer, geteilt durch ihre Zahl, kein Bonus, "
     "Fangen gibt keine EP (braucht die Mod-Datei)."},
    {"Online button", "Online-Knopf"},
    {"\"Far away\" plays over our own server instead of Nintendo's (on with \"Online\", needs the mod file).",
     "\xE2\x80\x9EWeit entfernt\xE2\x80\x9C spielt \xC3\xBC" "ber den eigenen Server statt \xC3\xBC" "ber Nintendo "
     "(an mit \xE2\x80\x9EOnline\xE2\x80\x9C, braucht die Mod-Datei)."},
    {"Audio hang guard", "Schutz gegen Ton-Hänger"},
    {"Keeps the game's sound from hanging after the audio output was reopened (needs the mod file).",
     "Verhindert, dass der Ton des Spiels nach dem Neu-Öffnen der Tonausgabe hängen bleibt (braucht die "
     "Mod-Datei)."},
    {"Start without controller screen", "Start ohne Controller-Auswahl"},
    {"The game starts with the controller set here instead of asking at every start (needs the mod file).",
     "Das Spiel startet mit dem hier eingestellten Controller, statt bei jedem Start zu fragen (braucht die "
     "Mod-Datei)."},
    {"Controller screen at start", "Controller-Auswahl beim Start"},
    {"Hide field Pokémon", "Feld-Pokémon ausblenden"},
    {"Let's Go without wild Pokémon in sight; walking into a hidden one still starts its battle. Takes effect at "
     "once.",
     "Let's Go ohne sichtbare wilde Pokémon; läuft man in ein verstecktes hinein, beginnt trotzdem sein Kampf. "
     "Wirkt sofort."},
    {"On: the game asks for the controller at every start, as on the console. Off: it starts with the controller "
     "from these settings (needs the mod file skip-controller-screen.cfg); the game's options still show the "
     "screen. Applies from the next start.",
     "An: Das Spiel fragt bei jedem Start nach dem Controller, wie auf der Konsole. Aus: Es startet mit dem "
     "Controller aus diesen Einstellungen (braucht die Mod-Datei skip-controller-screen.cfg), die Optionen des "
     "Spiels zeigen die Auswahl weiterhin. Gilt ab dem n\xC3\xA4" "chsten Start."},
    {"Catch in battle", "Fangen im Kampf"},
    {"Pok\xC3\xA9 Balls in the bag of wild battles: catching starts from the battle menu (how: Catch style in "
     "battle), and one defeated is gone. Off: the Let's Go catch scene after a won battle. Needs the file in "
     "the folder mods and Wild battles, from the next start.",
     "Pok\xC3\xA9" "b\xC3\xA4lle im Beutel wilder K\xC3\xA4mpfe: Gefangen wird aus dem Kampfmen\xC3\xBC heraus "
     "(wie: Fangart im Kampf), ein besiegtes Pok\xC3\xA9mon ist weg. Aus: die Fangszene von Let's Go nach einem "
     "gewonnenen Kampf. Braucht die Datei im Ordner mods und Wilde K\xC3\xA4mpfe, ab dem n\xC3\xA4" "chsten Start."},
    {"Catch style in battle", "Fangart im Kampf"},
    {"Classic: the ball flies in the battle as in the main games, draws the Pok\xC3\xA9mon in and wobbles on the "
     "ground; if it breaks out, the Pok\xC3\xA9mon appears again. Let's Go: a ball chosen in the battle menu ends the "
     "battle, and the Let's Go catch scene follows. Only with Catch in battle.",
     "Klassisch: Der Ball fliegt im Kampf wie in den Hauptspielen, zieht das Pok\xC3\xA9mon hinein und wackelt am "
     "Boden; bricht es aus, erscheint es wieder. Let's Go: Ein im Kampfmen\xC3\xBC gew\xC3\xA4hlter Ball beendet den "
     "Kampf, und die Fangszene von Let's Go folgt. Nur mit Fangen im Kampf."},
    {"Classic", "Klassisch"},
    {"Let's Go", "Let's Go"},
    {"Widescreen", "Breitbild"},
    {"More of the game world to fit the screen instead of borders. Applies from the next start.",
     "Mehr Spielwelt passend zum Bildschirm statt Rand. Gilt ab dem n\xC3\xA4" "chsten Start."},
};

std::atomic<UiLanguage> g_language{UiLanguage::English};

const std::unordered_map<std::string_view, const char*>& german() {
    static const auto* table = [] {
        auto* t = new std::unordered_map<std::string_view, const char*>();
        for (const Row& r : kGerman)
            t->emplace(r.english, r.german);
        return t;
    }();
    return *table;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

UiLanguage ui_language_for_locale(std::string_view locale) {
    const std::string l = lower(locale);
    // "de", "de-DE", "de_AT.UTF-8": the language is the part before - or _.
    const bool german = l.rfind("de", 0) == 0 && (l.size() == 2 || l[2] == '-' || l[2] == '_' || l[2] == '.');
    return german ? UiLanguage::German : UiLanguage::English;
}

UiLanguage ui_language_from_name(std::string_view name) {
    const std::string n = lower(name);
    if (n == "en" || n == "english")
        return UiLanguage::English;
    if (n == "de" || n == "german" || n == "deutsch")
        return UiLanguage::German;
    return ui_language_for_locale(name);
}

const char* ui_language_name(UiLanguage language) {
    return language == UiLanguage::German ? "de" : "en";
}

void set_ui_language(UiLanguage language) {
    g_language.store(language, std::memory_order_relaxed);
}

UiLanguage ui_language() {
    return g_language.load(std::memory_order_relaxed);
}

const char* tr(const char* english) {
    if (ui_language() != UiLanguage::German || english == nullptr)
        return english;
    const auto& t = german();
    const auto it = t.find(english);
    return it != t.end() ? it->second : english;
}

std::string tr(const std::string& english) {
    if (ui_language() != UiLanguage::German)
        return english;
    const auto& t = german();
    const auto it = t.find(english);
    return it != t.end() ? std::string(it->second) : english;
}

std::string trf(const char* english, std::initializer_list<std::string_view> args) {
    const std::string_view text = tr(english);
    std::string out;
    out.reserve(text.size() + 32);
    auto arg = args.begin();
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{' && i + 1 < text.size() && text[i + 1] == '}' && arg != args.end()) {
            out += *arg++;
            ++i;
        } else {
            out += text[i];
        }
    }
    return out;
}

std::vector<std::pair<const char*, const char*>> ui_text_table() {
    std::vector<std::pair<const char*, const char*>> out;
    for (const Row& r : kGerman)
        out.emplace_back(r.english, r.german);
    return out;
}

}  // namespace lgp::platform
