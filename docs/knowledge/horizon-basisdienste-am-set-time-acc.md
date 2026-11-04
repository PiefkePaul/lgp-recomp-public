# Horizon base services as HLE: `appletOE`, `set`, `time:u`, `csrng`, `pctl`, `acc:u0` and quiet services

Cross-game knowledge from P3.4 (`runtime/hle/src/{am,set,time,csrng,pctl,acc}.cpp`). Command numbers and layouts come from switchbrew ("Applet Manager services", "Settings services", "Glue services", "Account services", "Error codes") and the libnx headers. No code was taken over. Which commands exist is based on the Ryujinx reference run of LGP. All other commands halt the guest, like every unknown command (see [`horizon-ipc-hipc-cmif-sm.md`](horizon-ipc-hipc-cmif-sm.md)).

## Framework in the runtime

- **Everything the guest sees about the "console" is in `lgp::hle::SystemConfig`**: language, region, handheld or docked, user ID and nickname, start time and time zone. The defaults are fixed values so that runs stay reproducible. The embedding program decides the real values.
- `register_base_services(sm, config, process)` registers all six services. It returns `BaseServices` with the `AppletState`, through which the host feeds in applet messages.
- **Events for HLE services** (`lgp::hle::Event`): the service holds a `KReadableEvent`, the guest gets copy handles to it (`make_handle`, a new handle to the same object each time). `signal`/`clear` go through `Scheduler::signal_readable`/`clear_readable`. That wakes waiters, **but never preempts the caller**, because an HLE handler can hold service locks that the preempting thread would need (in baton mode that would be a deadlock). The guest may clear the event itself via its handle (`svcClearEvent`, `svcResetSignal`).
- **Buffers:** `Request::in_buffer(i)` is A or X, `out_buffer(i)` is B or C. With AutoSelect (type 0x21/0x22) the SDK sends both descriptors, one of them with size 0. The index counts per descriptor list. That is correct as long as a command does not mix map-alias and pointer buffers of the same direction, which is not the case for any of the commands implemented here.
- **Transfer memory** is often locked on the guest side with permissions `None`. Services therefore access it kernel-side (`Request::read_mapped`/`write_mapped` via `GuestMemory::host_ptr`), not with user-pointer semantics. A copy handle stays with the client. The service only holds a reference to the object so that the lock remains until the end of the storage's lifetime.

## `appletOE` (am for applications)

| Object | Commands here | Behavior |
|---|---|---|
| `appletOE` | 0 `OpenApplicationProxy` (PID, u64, copy handle process) | returns `IApplicationProxy` |
| `IApplicationProxy` | 0 CommonStateGetter, 1 SelfController, 2 WindowController, 3 AudioController, 4 DisplayController, 10 ProcessWindingController, 11 LibraryAppletCreator, 20 ApplicationFunctions, 1000 DebugFunctions | 4, 10 and 1000 are empty objects (opened in the reference run, never used) |
| `ICommonStateGetter` | 0 `GetEventHandle`, 1 `ReceiveMessage`, 5 `GetOperationMode` (u8: 0 handheld, 1 docked), 6 `GetPerformanceMode` (u32: 0 normal, 1 boost), 9 `GetCurrentFocusState` (u8: 1 InFocus) | see messages |
| `ISelfController` | 9 `GetLibraryAppletLaunchableEvent`, 11/12/13/14/16 settings, 40 `CreateManagedDisplayLayer` (u64), 62/63 `Set`/`GetIdleTimeDetectionExtension` (u32 0–2) | event stays signaled; layer ID fixed at 1 (vi needs it in P4); idle extension only stored (no dimming), values > 2 halt (error code undocumented) |
| `IWindowController` | 1 `GetAppletResourceUserId` (u64), 10 `AcquireForegroundRights` | ARUID = PID of the application |
| `IAudioController` | 0 `SetExpectedMasterVolume` (f32, f32), 1/2 Get (f32) | default 1.0 |
| `ILibraryAppletCreator` | 0 `CreateLibraryApplet` (u32 AppletId, u32 mode), 10 `CreateStorage` (s64), 11 `CreateTransferMemoryStorage` (u8, s64, copy handle) | AppletId 0x11 (swkbd), 0x0E (error), 0x0F (netConnect) and 0x10 (playerSelect, see [`horizon-offline-konsole-nifm-ldn-friend.md`](horizon-offline-konsole-nifm-ldn-friend.md)); others halt the guest |
| `ILibraryAppletAccessor` | 0 `GetAppletStateChangedEvent`, 1 `IsCompleted`, 10 `Start`, 30 `GetResult`, 100 `PushInData`, 101 `PopOutData`, 103 `PushInteractiveInData`, 104 `PopInteractiveOutData`, 105/106 events for out and interactive-out data | see software keyboard |
| `IStorage` / `IStorageAccessor` | 0 `Open`; 0 `GetSize`, 10 `Write` (s64, buffer 0x21), 11 `Read` (s64, buffer 0x22) | out of range: 0x3EE80 |
| `IApplicationFunctions` | 1 `PopLaunchParameter` (u32 kind), 21 `GetDesiredLanguage` (u64), 40 `NotifyRunning` (u8) | see below |

- **Messages:** a starting application has `FocusStateChanged` (15) in its queue at startup; the event is signaled. Empty queue: `ReceiveMessage` returns 0x680 (module 128, 3) and the event is cleared. Further numbers: 30 `OperationModeChanged`, 31 `PerformanceModeChanged`. The reference run matches this: two `ReceiveMessage`, i.e. one message and one "none".
- **Handheld ↔ docked at runtime:** `AppletState::set_docked(kernel, docked)` acts like docking or undocking. From then on `GetOperationMode`, `GetPerformanceMode` and `apm` report the new value, and only on a real change do `OperationModeChanged` (30) and `PerformanceModeChanged` (31) go into the queue, both together, as yuzu and Ryujinx do when switching. The game then rereads the mode itself and picks its target sizes. Whether the game expects the messages only after `SetOperationModeChangedNotification(1)` has not been checked: the runtime always sends them, the setting commands 11/12 are only acknowledged. The host reaches this via `boot::ProcessControl::set_docked` (before the start, the request replaces `SystemConfig::docked`).
- **`PopLaunchParameter`:** kind 1 (UserChannel) is empty and returns 0x480 (module 128, 2). Kind 2 (PreselectedUser) returns a storage of 0x88 bytes once. Layout: u32 magic `0xC79497CA`, u8 1, 3 bytes of padding, user ID (16 bytes), 0x70 bytes unused (libnx `acc.c` checks the magic and the 1). A second pop returns 0x480.
- **`GetDesiredLanguage`** on real hardware picks the system language if the application supports it according to its NACP, otherwise a substitute. The runtime does not read the NACP yet and returns the system language.

## `set`

0 `GetLanguageCode` (u64), 1 `GetAvailableLanguageCodes` (buffer 0xA, at most 15), 3 count (15), 4 `GetRegionCode` (s32), 5 `GetAvailableLanguageCodes2` (buffer 0x6, 18), 6 count (18). A `LanguageCode` is the string in a little-endian u64 ("en-US", "de", …). The list follows the `Language` enumeration: ja, en-US, fr, de, it, es, zh-CN, ko, nl, pt, ru, zh-TW, en-GB, fr-CA, es-419, zh-Hans, zh-Hant, pt-BR. The SDK queries command 5 with room for 0x40 entries.

## `time:u`

| Object | Commands here |
|---|---|
| `IStaticService` (or "ForGlue", same numbers) | 0/1/4 user/network/local SystemClock, 2 SteadyClock, 3 TimeZoneService, 100 auto correction (u8 0), 300 `CalculateMonotonicSystemClockBaseTimePoint`, 400 `GetClockSnapshot`, 500 `CalculateStandardUserSystemClockDifferenceByUser` |
| `ISystemClock` | 0 `GetCurrentTime` (s64), 2 `GetSystemClockContext` (0x20) |
| `ISteadyClock` | 0 `GetCurrentTimePoint` (0x18) |
| `ITimeZoneService` | 0 `GetDeviceLocationName` (0x24), 2 count, 3 `LoadLocationNameList`, 4 `LoadTimeZoneRule` (name, buffer 0x16 with 0x4000 bytes), 100 `ToCalendarTime` (s64, rule 0x15), 101 `…WithMyRule`, 201 `ToPosixTime` (CalendarTime, rule 0x15, output 0xA) → s32, 202 `…WithMyRule` |

Layouts:

| Structure | Size | Content |
|---|---|---|
| `SteadyClockTimePoint` | 0x18 | s64 seconds, 16-byte source ID |
| `SystemClockContext` | 0x20 | s64 offset, `SteadyClockTimePoint`; POSIX time = offset + current steady value |
| `CalendarTime` | 8 | u16 year, u8 month (1–12), day, hour, minute, second, padding |
| `CalendarAdditionalInfo` | 0x18 | u32 weekday (0 = Sunday), u32 day of year (from 0), char[8] zone abbreviation, u32 DST, s32 offset in seconds |
| `ClockSnapshot` | 0xD0 | 0x00 user context, 0x20 network context, 0x40 s64 user time, 0x48 s64 network time, 0x50/0x58 CalendarTime, 0x60/0x78 AdditionalInfo, 0x90 SteadyClockTimePoint, 0xA8 LocationName, 0xCC u8 auto correction, 0xCD u8 type |

- **One clock for everything:** steady = seconds since runtime start (`system_tick`). The system clocks are steady plus a fixed offset that comes once at startup from `SystemConfig::start_posix_time` or the host clock (`std::chrono::system_clock`). That way time never runs backwards, and two snapshots of one run have the same context. The network clock is the same clock (no network).
- **300** returns the POSIX time at which the system tick was 0: `context.offset + steady.now − tick seconds` if the source ID matches, otherwise 0xCC74 (module 116, 102, "OffsetInvalid" on switchbrew).
- **500** returns the user's adjustment of the user clock between two snapshots as a TimeSpan in nanoseconds: offset difference of the user contexts, 0 for a different source ID or if auto correction was on in both. The snapshots come as X buffers (type 0x19). The semantics are inferred from the command name and the behavior of other HLE implementations, not measured on real hardware.
- **Time zones without tzdata:** there is exactly one fixed zone (offset and abbreviation from `SystemConfig`) without daylight saving time. `LoadTimeZoneRule` writes its own marker into the opaque 0x4000-byte buffer (`LGPTZRL1`, offset, abbreviation). The conversion commands read it back. A foreign buffer falls back to the configured zone with a warning. Names that denote UTC without daylight saving (`GMT`, `Etc/UTC`, `Zulu` …) get a rule with offset 0 and the requested abbreviation; Pokémon Let's Go loads "GMT" this way about once per second, regardless of the device name. Any other unknown location name returns 0x7BA74 (116, 989 NotFound). Both warnings appear only once per name. Without daylight saving, `ToPosixTime` always returns exactly one value. Out-of-range fields (month 13) are normalized.
- Calendar: proleptic Gregorian, days since 1970-01-01 with floor division. Tested with leap day and times before 1970.

## `csrng`, `pctl`, `acc:u0`

- **`csrng`** 0 `GetRandomBytes` (buffer 0x6). Deterministic: SplitMix64 from `ProcessConfig::random_entropy`, one stream for all sessions. Not cryptographic. Offline the game does not need that, and equal inputs give equal runs (important for differential comparisons). The reference run calls it 18,163 times.
- **`pctl`**: factory 0 `CreateService` / 1 `CreateServiceWithoutInitialize` (u64, PID) → `IParentalControlService` with 1 `Initialize`. There are no restrictions.
- **`acc:u0`**: exactly one user. 0 `GetUserCount`, 1 `GetUserExistence`, 2/3 `ListAllUsers`/`ListOpenUsers` (buffer 0xA, 8 IDs, rest zero), 4 `GetLastOpenedUser`, 5 `GetProfile` → `IProfile` (0 `Get` with UserData 0x80 in buffer 0x1A, 1 `GetBase`; `ProfileBase` 0x38 = ID, u64 timestamp, char[0x20] nickname), 100/140 `InitializeApplicationInfo(V0)` (PID), 101 `GetBaasAccountManagerForApplication` → empty `IManagerForApplication`. A foreign ID returns 0xC87C (124, 100 UserNotExist).
- **The user ID must stay stable:** `fsp-srv` stores SaveData under this ID. Whoever changes it "loses" the save.

## Quiet services: `prepo:u`, `arp:r`, `btm:u`, `bt`, `lm`

These services have no effect without network and without Bluetooth, but must answer (`runtime/hle/src/quiet.cpp`, `register_quiet_services`).

- **`prepo:u`** (play reports): 10100–10107, all revisions of `SaveReport(WithUser)` (event ID in the X buffer, msgpack report in the A buffer). The reports are discarded, size only in the debug log. The numbers have shifted per firmware (switchbrew "BCAT services"). The reference run uses Ryujinx's name `SaveReportWithUserOld`; that is 10101.
- **`arp:r`**: 0 `GetApplicationLaunchProperty` → 0x10 bytes (u64 program ID, u32 version, u8 storage ID, u8 patch storage, u8 kind, padding) and 3 `GetApplicationInstanceId` (u64 PID → u64). **The meaning of 3 changed with 10.0.0:** before that it was `GetApplicationControlPropertyWithApplicationId` with an NACP output buffer, and 0 took the PID instead of the instance ID. The runtime uses the PID as the instance ID so that 0 understands both variants. If 3 comes with an output buffer (old variant), the buffer stays untouched and there is a warning in the log, because the NACP is not read yet. Version and storage ID are in `ApplicationInfo` (default 0 and 5 = SdCard, neither measured).
- **`btm:u`**: 0 `GetCore` → `IBtmUserCore` with 0/17/26/33 `AcquireBle{Scan,Connection,ServiceDiscovery,MtuConfig}Event` → bool (1) and a copy handle to an event that never fires. **`bt`**: 9 `RegisterBleEvent` (PID, ARUID) → event. So the game never finds a BLE device (Poké Ball Plus, Pokémon GO Plus).
- **`lm`**: 0 `OpenLogger` (u64, PID) → `ILogger` with 0 `Log` (A buffer, packets are only counted) and 1 `SetDestination`.

## `apm`

LGP does not open `apm` (reference run), but libnx's application start (`appletInitialize`) does (`apmInitialize`; without
the service the whole applet start fails). The runtime answers minimally: 0 `OpenSession` → `ISession` with
0 `SetPerformanceConfiguration(u32 mode, u32 config)` (only stored) and 1 `GetPerformanceConfiguration(u32
mode)` (the stored value, otherwise 0x00010000, Ryujinx's default, not checked against hardware); 1 `GetPerformanceMode` →
1 in docked mode of the `AppletState` (start: `SystemConfig::docked`, then `set_docked`), otherwise 0.

With `apm` and the domain clone (see [`horizon-ipc-hipc-cmif-sm.md`](horizon-ipc-hipc-cmif-sm.md)) libnx's
own clients for `applet` (application, focus), `set`, `time`, `csrng`, `acc` (preselected user, profile),
`fsp-srv` (RomFS via `romfsMountSelf`, save) and `swkbd` run without errors against the runtime
(`boot.fixture_services_nso`, [`sdk-prozessstart-ohne-rtld.md`](sdk-prozessstart-ohne-rtld.md)).

## Software keyboard (library applet swkbd, AppletId 0x11)

The application starts the applet via `ILibraryAppletCreator::CreateLibraryApplet` and passes data in as `IStorage` (`PushInData`, in-interface: domain object ID or copy handle to the storage session). Layouts from libnx `swkbd.h`/`swkbd.c`/`libapplet.c`:

| Data | Content |
|---|---|
| In 0 | `CommonArguments` (0x20): u32 version 1, u32 size 0x20, u32 applet version, s32 color scheme, u8 play startup sound, u64 tick |
| In 1 | keyboard configuration, size depending on SDK version (0x3E0, 0x400 or 0x4C8), always starting with `SwkbdArgCommon`: header/sub/guide text (UTF-16 at 0x24/0xA6/0x1A8), `stringLenMax`/`Min` at 0x3AC/0x3B0, password at 0x3B4, initial text offset/length (UTF-16 units) in the work buffer at 0x3C0/0x3C4, text check u8 at 0x3D0 |
| In 2 | work buffer as transfer memory (initial text in libnx from 0x14, dictionary from 0x7E8) |
| Interactive out (only with text check) | u64 byte size + UTF-16 text, together 8 + 0x7D4 |
| Interactive in | u32 check result (0 OK, 1 error with message, 2 confirmation prompt, from 3 silently rejected) + UTF-16 message (0x7D4) |
| Out | u32 CloseResult (0 OK, 1 cancel) + UTF-16 text, together 0x7D8 |

- **Flow in the runtime:** `Start` asks the host synchronously on the guest thread (`KeyboardHandler`, may block). Without text check, the output is ready immediately afterwards and the state event is set. With text check, interactive output comes first. If the game answers with 1 or 3 and above, the runtime asks the host again (with message and rejection counter); with 2 the confirmation prompt counts as confirmed.
- **Without a host handler** (tests, runs without input): the initial text if it fits within the length limits, otherwise the account nickname. If the game rejects it, the keyboard cancels (CloseResult 1) instead of sending the same thing endlessly.
- **Host input** (`platform::ITextInput`, `lgp/platform/text_input.hpp`; the bridge `lgp/boot/keyboard.hpp` sets the handler and converts UTF-16 ↔ UTF-8; `BootOptions::text_input`). Boot program `--keyboard`:
  - `window` (default with `--renderer window`, `SdlTextInput`): a separate small **dialog window** above the game (`SDL_WINDOW_ALWAYS_ON_TOP`) shows header text, what has been typed so far, message and keys; in addition, the request appears in the title of the game window. Paul overlooked the input that was only in the window title (first version) on the PC. SDL text input (including IME) delivers the characters, Enter confirms, Esc or closing the dialog cancels, Backspace deletes a character. While the keyboard is open, key presses do not go to the controllers (releases do). Drawing uses `SDL_Renderer` and the built-in 8×8 debug font (`SDL_RenderDebugText`). For that `cmake/LgpSdl.cmake` builds SDL with `SDL_RENDER`. The font only knows ASCII: umlauts appear as ae/oe/ue/ss, other characters as `?`; the entered text itself stays unchanged. Window events of the dialog (close, resize) do not reach the game window, because `SdlWindow` checks the window ID.
  - `menu` (default in app programs on iOS and Android, `SdlTextInput::set_host_drawn`): a screen of the app menu (`ui::Screen::Text`) with header text, text field (Dear ImGui `InputTextWithHint`, counter at max length in UTF-16 units), message and OK/Cancel; the field gets focus, so the ImGui SDL3 backend opens the on-screen keyboard via `SDL_StartTextInput`. The field sits at the top so that the keyboard does not cover it. Reason: iOS and Android have a single fullscreen window; a second SDL window (the `window` dialog) never appears there. On the iPad the game therefore got stuck at name entry. Answers that are too short stay open with a hint, in the same field with the typed text (a new field would restart the system keyboard). On iOS and Android the menu reports the top edge of the screen to SDL as the input area: from that area SDL computes how far it pushes the whole view above the keyboard, and back on every show and hide of the keyboard (with a hardware keyboard also of the shortcut bar). On the iPad with a keyboard case the dialog jumped up and down because of this (cause not yet confirmed on the device; log lines `menu: system keyboard shown/hidden`, `menu: text input event`, `menu: key …`, `menu: text refused`). Error messages (`show_message`) appear on the same screen with OK. The menu starts the text input itself with `SDL_StartTextInputWithProperties` (without autocorrect and suggestions, without capitalization, single-line, hidden for passwords), because SDL otherwise turns on autocorrect and the system may rewrite typed names, even with a hardware keyboard (suspicion after the iPad test, not yet confirmed on the device); the backend afterwards only sets the input position. On the desktop, `--keyboard menu` for trying it out. Quirk of Dear ImGui: a freshly focused field only gets Enter assigned one frame later (key routing), so tests must wait two frames.
  - `host` (default in the browser, `WebTextInput`): modal `<dialog>` on the page with header text, message, text field (`maxLength`/`minLength` count UTF-16 units like the game, password field) and OK/Cancel. The guest thread waits; the answer comes via the shared Wasm memory (status word with `Atomics.notify`). `WebInput` leaves keys alone while a text field of the page has focus (otherwise it would swallow letters via `preventDefault`). Traps in the `EM_ASM` code: no comma at the top level (splits the macro argument), `growMemViews` does not exist in every build.
  - `console`: a line from stdin (empty line = initial text, end of file = cancel). `text:NAME`: always NAME (scripted runs). `none` (default without a window): as without a host handler.
  - Unattended browser runs: `tests/web/browser_test.mjs` and `tools/web/profile_chromium.mjs` append `--keyboard none` to every page they start themselves (or `--keyboard <spec>` from the option `--keyboard` or `LGP_KEYBOARD`, e.g. `LGP_KEYBOARD=text:Red`), unless the arguments name one. Otherwise the page dialog waits forever; the last line in the log is then "swkbd: asking the host …". Pages opened by hand keep the dialog.
  - The runtime logs every request (`swkbd: asking the host …`, info) and whether it was answered or cancelled.
  - The same host input shows the message of the error applet (`ITextInput::show_message`, see [`horizon-offline-konsole-nifm-ldn-friend.md`](horizon-offline-konsole-nifm-ldn-friend.md)). Unattended runs therefore also need `--keyboard none` for that.
- **Not checked:** whether a byte at 0x3D1 (padding in libnx) turns on UTF-8 output in newer SDKs. The runtime always answers in UTF-16 and warns if the byte is set. `GetResult` always returns success; only CloseResult shows the cancel.

## Result codes (switchbrew "Error codes")

| Value | Module/description | Meaning |
|---|---|---|
| 0x480 | 128 / 2 | am: no data in the channel (IStorage not available) |
| 0x680 | 128 / 3 | am: no message |
| 0x3EE80 | 128 / 503 | am: IStorage access out of range |
| 0xCC74 | 116 / 102 | time: OffsetInvalid (clock source does not match) |
| 0x7BA74 | 116 / 989 | time: NotFound |
| 0xC87C | 124 / 100 | acc: user does not exist |

## Open

- The inline keyboard (`swkbdInline`) is missing; LGP does not use it (imports: only `nn::swkbd::ShowKeyboard`).
- Software keyboard in the real game (name entry in LGP) with `--keyboard window` or in the browser unverified.
- Read the NACP (`GetDesiredLanguage`, later `EnsureSaveData` sizes).
- `apm` is declared in the NPDM but not used in the reference run. It is deliberately missing until a run requests it (sm then reports `NotRegistered` with a log line).
