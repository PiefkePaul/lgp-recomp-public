# Desktop app shell for a recompiled game (Windows, macOS)

Cross-game knowledge (status 2026-10-13, P7.1). LGP decision: [ADR-0008](../decisions/0008-native-apps-auf-allen-geraeten.md),
overview of all platforms: [`native-apps-fuer-switch-recomp.md`](native-apps-fuer-switch-recomp.md).

## From command-line program to app

A boot program for developers (`lgp_game --exefs … --game … --save …`) becomes an app when it starts
sensibly without arguments. In the project this is called app mode (`lgp_add_boot_executable(… APP)`, `lgp/boot/app.hpp`):

- **Without arguments** (double-click) the program reads its options from `settings.ini` in the settings folder and
  writes its output to `logs/lgp.log` (the previous run stays as `lgp.prev.log`). If the run ends with an
  error, a message box shows the path of the log, because a program started by clicking has no console.
- **With `--app`** the same applies, but output stays in the console, and further options may follow; they
  come at the end of the command line and win (tests use this, e.g. `--app --renderer soft --frames 2`).
- **First start:** no valid game folder saved → notice dialog, then the system's folder dialog
  (SDL3 `SDL_ShowFileDialogWithProperties` with `SDL_FILEDIALOG_OPENFOLDER`). The folder must contain `exefs/<every module
  of the lift output>` and `romfs/` or `romfs.bin`. If one accidentally selects `exefs/` or `romfs/`
  itself, the parent folder is used. The checked folder is saved.
- **App defaults:** window with SDL input and SDL sound, `--graphics hle`, paced, pipeline cache always on
  (in the cache folder). Fullscreen via `fullscreen = 1` or F11 in game (borderless fullscreen on the desktop; Enter and
  the other keys belong to the controller mapping).

## Where the files belong

Never next to the program: under `Program Files` and in a signed `.app` bundle the app must not write,
and an update would delete saves.

| | Settings, saves, log | Pipeline cache (can be regenerated) |
|---|---|---|
| Windows | `%APPDATA%\lgp-recomp` (Roaming) | `%LOCALAPPDATA%\lgp-recomp` |
| macOS | `~/Library/Application Support/lgp-recomp` | `~/Library/Caches/lgp-recomp` |
| Linux | `$XDG_CONFIG_HOME`, `$XDG_DATA_HOME` (default `~/.config`, `~/.local/share`) | `$XDG_CACHE_HOME` (`~/.cache`) |

`LGP_APP_DIR=<folder>` puts everything into one folder (tests, portable copy on an external drive).
Windows: read environment variables as UTF-16 (`_wdupenv_s`), otherwise user names outside the ANSI code page
get lost. The boot program itself still takes paths as narrow strings (ANSI code page on Windows); folders
with characters outside it do not open there (suspicion, not checked).

`settings.ini` (UTF-8, `#` at the start of a line = comment, `#` after a value belongs to the value, unknown keys are ignored, writing goes via a temporary file):

```ini
# folder with exefs/ and romfs/ or romfs.bin
game = D:\Spiele\Dump
# empty: <data folder>/saves; relative: below the data folder
saves =
# handheld | joycon-right | joycon-left (player 1)
controller = handheld
fullscreen = 0
# further options of the boot program, separated by spaces
args = --stick 0.15,0.9
```

Relative paths in `saves =`, in `args =` and in the options after `--app` (`--save`, `--game`, `--exefs`,
`--pipeline-cache`, `--pipeline-cache-seed`, `--frame-dump`) apply below the data folder, not the
working directory: an app does not get its own working directory, and on iOS the container path changes with every
reinstallation. `--pipeline-cache none` stays `none`; absolute paths stay unchanged.

## App menus (P7)

With a window, app mode can show a start screen before the game (continue, choose save,
settings, quit; below it in gray, where the game comes from: "in der App enthalten" (included in the app), "Ordner "game" der App" (the app's "game" folder) or the path, `boot::game_source_text`, also as the log line `app: Spiel: …`) and in game a pause button at the top center (also Escape, on the controller Home, on Android also Start held for 0.7 s, because Android presumably keeps Home of the Shield controller to itself). Built with
Dear ImGui 1.92.9 on SDL3 events (`platform/ui`, `lgp/platform/ui/app_menu.hpp`); drawn by our own
WebGPU renderer via a neutral triangle list (`lgp/platform/ui_draw.hpp`, `WebGpuRenderer::set_ui`), because
ImGui's WebGPU binding does not fit wgpu-native 29. The pause stops the thread that presents
(`WebGpuRenderer::set_paused`: `end_frame()` waits); meanwhile the main thread shows the last frame with the menu
(`redraw()`). The game thus stands at its next present; guest threads without a present keep running until they wait for
it.

The start screen comes only with `settings.ini` `start_screen = 1` (settings row "Main menu at start" on the page
"Game", from the next start; `--start-screen on|off` for one run, a `--menu-script` turns it on so tests can drive
it). By default the game starts at once with the preselected save (the one settings.ini names, else slot 1), as
"Continue" would, log line `menu: start screen skipped (start_screen = 0)`; the menu opens with the pause button.
The start screen still comes when it has to ask: no save folder could be made, or iCloud holds another state
(test `boot.sdk_app_no_start_screen`; Paul 2026-10-28).

The settings row "Controller" (player 1: handheld, right or left Joy-Con) takes effect at once in the game:
`SdlInput::set_player1_controller` maps slot 0 with the new style, and hid presents it at its next update as a
controller swap on the console would (handheld npad gone, npad No1 with the Joy-Con, or back). How the game reacts
to such a swap mid-game is not checked on the game yet (test `boot.sdk_app_menu_controller`, menu script
`controller:handheld|joycon-right|joycon-left`).

The game's own controller screen at every start is skipped by the hidden mod `skip-controller-screen`
(`runtime/mods/src/skip_controller_screen.cpp`, Paul 2026-10-28): at boot the game creates its controller check
with a mode that always shows the screen; the mod's `patch boot_mode VA` gives it mode 0, the quiet check the game
never uses itself (screen only with two or more controllers or an unsupported style). The game's options still
reach the screen. The app names the mod when `<game>/mods/skip-controller-screen.cfg` exists, unless
`settings.ini` `controller_screen = 1` (settings row "Controller screen at start", page "Controls"). Found
statically in Ghidra; whether the game runs on cleanly without the screen is not checked on the game yet.

Saves are folders below the data folder: `saves` (save 1), `saves-2` … (`saves =` in `settings.ini`).
A different folder (`saves = /woanders` or `--save`) appears on the start screen as "Eigener Ordner" (own folder) and stays
in use until a save is chosen. "Spielstand sichern" (back up save) copies the folder to
`backups/<foldername>-<date>`; if the copy fails (disk full), the half-finished backup is deleted again.
Copying only happens when no guest thread is running any more (`ProcessControl::pause` reports quiet, otherwise it re-checks briefly):
a thread in the middle of an `fsp-srv` save commit would otherwise capture a half-switched state. If the game stays
busy, the menu reports "bitte gleich noch einmal" (please try again shortly); without `ProcessControl` the button is grayed out.
`settings.ini` only remembers a save if it was tapped in the menu or newly created (including the already
preselected one), not a one-off `--save` that happens to name a save folder. Only the game itself can save; a save cannot be forced from outside.

With the menu open, the game still receives all release events (keys, buttons, fingers) and the stick movement,
so that nothing stays pressed after resuming. If the game is currently asking for text (software keyboard),
Escape goes to the keyboard dialog, not to the menu. On iOS and Android the menu draws the keyboard itself
(`--keyboard menu`, see `horizon-basisdienste-am-set-time-acc.md`). The touch controls can be switched on and
off in game (`SdlInput::set_touch_enabled`), without a restart.

Further keys in `settings.ini`: `touch` (empty, 0, 1), `touch_size` (50–140 %), `touch_opacity` (25–100 %),
`touch_mirrored` (0, 1), `volume` (0–100, start option `--volume`), `aspect` (image format, see below), `ldn` (`lan`
default, `off`: trading and battling on Wi-Fi, `--ldn`, [`ldn-ueber-lan.md`](ldn-ueber-lan.md)). The settings show the values of the current
start (i.e. with start options such as `--volume 0`); only what was changed in the menu ends up in `settings.ini`. `--menu off` switches the menus off; with `--press`
(script runs) the game starts without the start screen. Tests drive the menus with `--menu-script`
(`play,pause@0,backup,resume@20`, `settings` opens the settings, `slot<N>` taps the N-th entry of the save list (from 0); test `boot.sdk_app_menu` under xvfb; `@N` counts the game's shown
frames; only while the menu pauses the game do the menu frames count too, otherwise a step after `pause` would never get its turn.
After `resume`, `@N` thus means "game frame N" again, also for the stop at the N-th present). Steps at frame 0
are taken by the app before the game starts. For a later step (`@N`, N > 0) the game stops at its N-th present
until the host loop has taken the step: the guest thread pauses the process itself and waits in the scheduler; after the
step (for `pause` after its own pause) the loop releases it. Without this lock it depended on the timing of the
host loop: the test game ends after 3 frames, and just 40 ms delay in the window thread after frame 1 made
`pause@1` only take effect when the process was finished (`game paused in 0 ms`, after that no `backup`). With
asynchronous execution (`LGP_WEBGPU_ASYNC_EXECUTE`) the pause waits for the queue, at most 2 s (like the pause in the background;
if the worker hangs in the driver, the window thread thus does not freeze as well: line `the GPU queue did not empty`, the menu
then shows an older frame), and then reports
`menu: game stands at frame G, F shown` (the game's presents, frames shown). Pausing mid-game is checked by
`boot.sdk_app_menu_midgame` and `boot.sdk_app_menu_midgame_async` (`pause@2`, the third frame only after `resume`). The menus themselves
are checked by `platform.app_menu` with SDL events (dummy video driver).

## Pitfalls

- **Dialogs need the event loop:** on Windows and macOS SDL calls the folder dialog's callback from
  the main thread's event processing, on Linux (desktop portal via D-Bus) from another thread. So
  wait with `SDL_WaitEventTimeout` in a loop and report the result atomically.
- **Linux:** folder dialog via the XDG desktop portal or `zenity`; if both are missing, SDL reports an error. Without
  a display even the message box fails; the app then exits silently (for tests without a screen).
- **stdout and stderr into one file:** first truncate the file, then open both streams in append mode, otherwise
  they overwrite each other at different file positions.

### Image format and safe area

The game picture is 16:9. On other screens (iPhone from X about 19.5:9, iPad 4:3) `aspect` in
`settings.ini` or the start option `--aspect` determines how it fills the area (`lgp/platform/screen_fit.hpp`,
`place_image`): `fit` (default, as before: whole picture, bars left/right or top/bottom), `fill` (fills the
area, the protruding edge is cut off) or `stretch` (distorted to the area). In the settings
this is called "Bildformat" (image format) with "Einpassen" (fit), "Füllen" (fill), "Strecken" (stretch) and takes effect immediately, also in the pause picture. The WebGPU renderer places
the blit accordingly (`WebGpuRenderer::set_aspect_mode`); touch overlay and menu always cover the whole area.
Whoever converts finger positions to the game picture (the game's touchscreen) uses
`WebGpuRenderer::window_to_image(wx, wy, &ix, &iy)`: window fractions (SDL finger coordinates) → fractions of the last
shown frame, `false` on bars and the cut-off edge (pure calculation `platform::window_to_image`, test
`platform.screen_fit`).

Menu panels, pause button and touch controls respect the window's safe area
(`SDL_GetWindowSafeArea`: notch or Dynamic Island, rounded corners, home indicator; `SdlWindow::safe_insets`,
`TouchPad::set_insets`, changes arrive as `SDL_EVENT_WINDOW_SAFE_AREA_CHANGED`). The game picture and the darkened
menu background extend beneath it. On the desktop the safe area is the whole window; nothing changes there.
