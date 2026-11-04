# Offline console as HLE: `nifm:u`, `ldn:u`, `friend:u`, account without a Nintendo Account, library applets error, netConnect and playerSelect

Cross-game knowledge from P3.5 (`runtime/hle/src/{nifm,ldn,friends,applets,acc}.cpp`). Command numbers, structures
and error codes come from switchbrew ("Network Interface services", "LDN services", "Friend services", "Account
services", "Error Applet", "Profile Selector", "Applet Manager services", "Error codes") and the libnx headers
`nifm.h`/`ldn.h`/`ldn.c`, `applets/nifm_la.c`, `applets/psel.h`. No code was taken over. Like all HLE services, unknown commands halt the guest
(see [`horizon-ipc-hipc-cmif-sm.md`](horizon-ipc-hipc-cmif-sm.md)).

## Why these services must exist although nothing gets connected

A Switch game declares in its NPDM the services it may open. If a service is missing in `sm`,
`GetServiceHandle` answers with `NotRegistered` (0xE15). The SDK's `nn::*::Initialize` functions usually check this result
with an abort; the game then stops at the first spot where it goes, e.g., into the communication menu.
A console without network behaves differently: the services are there, only their requests fail with documented
results. That is exactly what the runtime reproduces, so that the game shows its own error messages instead of aborting.

Principle: **no invented data.** Where switchbrew does not document a value, the command stays unimplemented
(the guest halts with a log line) or the value is marked in the code as an assumption.

## `nifm:u`: no network

| Object | Commands | Response |
|---|---|---|
| `nifm:u` (`IStaticService`) | 4 `CreateGeneralServiceOld`, 5 `CreateGeneralService` (PID, u64) | `IGeneralService` |
| `IGeneralService` | 1 `GetClientId` (buffer 0x1A, u32) | consecutive IDs from 1 |
| | 2 `CreateScanRequest` | `IScanRequest`: Submit finds nothing, event is signaled |
| | 4 `CreateRequest` (s32 preset) | `IRequest` |
| | 5 `GetCurrentNetworkProfile`, 12 `GetCurrentIpAddress`, 15 `GetCurrentIpConfigInfo`, 18 `GetInternetConnectionStatus` | 2110-1100 |
| | 17 `IsWirelessCommunicationEnabled` | 1 (radio on, just no access point) |
| | 20 `IsEthernetCommunicationEnabled`, 21 `IsAnyInternetRequestAccepted`, 22 `IsAnyForegroundRequestAccepted` | 0 |
| `IRequest` | 0 `GetRequestState` | always 1 (`Free`: not submitted or failed) |
| | 1 `GetResult` | success before `Submit`, afterwards **2110-1100** (0x8986E, "Request failed, network not available, no connection reset") |
| | 2 `GetSystemEventReadableHandles` | two events; the first reports state changes and is signaled on `Submit` |
| | 3 `Cancel`, 4 `Submit`, 5/6/8–18/23 setters | success; setters change nothing about the response |
| | 21 `GetAppletInfo` (u32 color scheme, buffer 0x6) | AppletId 0x0F (netConnect), mode 0, buffer only with the end entry `{-1}` (no input data) |

Flow in the SDK (switchbrew and libnx `nifm_la.c`, which recreates `nn::nifm::HandleNetworkRequestResult`; not checked
against the game): `SubmitNetworkRequestAndWait` waits until the state is no longer `OnHold`; `IsNetworkAvailable` asks for
`Accepted` (3) and gets `Free`; `HandleNetworkRequestResult` fetches via `GetAppletInfo` which library applet it should
start, pushes the buffer entries in as in-storages (without `CommonArguments`), starts it and reads **one**
out-storage:

| Size | Meaning for the caller |
|---|---|
| 4 bytes (`s32 ret`) | `ret == 0` → 110-0190, otherwise 110-2900 |
| ≥ 8 bytes (`s32 ret`, `Result`) | the `Result` is returned (with `ret == 0` the size must be exactly 8) |
| < 4 bytes, abort | `LibAppletBadExit` |

That is why `GetAppletInfo` names the netConnect applet (0x0F) in our runtime, which immediately ends with `{s32 1, Result 2110-1100}`:
the caller gets 2110-1100 back, as with a console without an access point. The error applet (0x0E) does **not**
fit here: its return value `ErrorViewerReturnValue` is 2 bytes and would be a broken applet exit for this caller
(that was the case in the first version of P3.5, a Codex finding). Which applet the real firmware names and what exactly `ret != 0`
means is not documented (assumption).

`GetRequirement` (19) and `GetAppletInfo` before `Submit` are not implemented (content unknown or meaningless).

## `ldn:u`: alone in the room

The state machine (`nn::ldn::State`: 0 None, 1 Initialized, 2 AccessPoint, 3 AccessPointCreated, 4 Station,
5 StationConnected) runs completely; there is just never another console in range:

| Command | Effect |
|---|---|
| 400 `Initialize`, 402 `InitializeWithVersion` (7.0+) | 0 → 1 |
| 401 `Finalize` | → 0, closes whatever is open |
| 200/201 `Open`/`CloseAccessPoint` | 1 → 2, 2/3 → 1 |
| 202 `CreateNetwork`, 203 `CreateNetworkPrivate` | 2 → 3; network with us as the only node |
| 204 `DestroyNetwork` | 3 → 2, `DisconnectReason` 3 (DestroyedByUser) |
| 300/301 `Open`/`CloseStation` | 1 → 4, 4/5 → 1 |
| 302/303 `Connect(Private)` | in 4: **203-0065** (`ResultNetworkNotFound`) |
| 102/103 `Scan(Private)` | 0 networks |
| 1 `GetNetworkInfo`, 2 `GetIpv4Address`, 4 `GetSecurityParameter`, 5 `GetNetworkConfig`, 101 `GetNetworkInfoAndHistory` | only in 3/5, otherwise 203-0039 (invalid state) |
| 0 `GetState`, 3 `GetDisconnectReason`, 100 `GetStateChangeEvent` | event is signaled on every state change |
| 104, 205–209 | success (policies, advertise data, accept list) |

Wrong state for a transition: 203-0039. Invalid `NetworkConfig` (`node_count_max` outside 1–8, negative
version): 203-0096.

The generated `NetworkInfo` (0x480 bytes, layout from libnx `ldn.h`, checked with `static_assert`): `NetworkId` from the
`NetworkConfig`; `local_communication_id == -1` is replaced as on the console, but by the program ID, because
the NACP is not read (assumption: its first ID is usually the program ID). Session ID and server random are
deterministic per run. Node 0 is us: 169.254.1.1, locally administered MAC `02:4C:47:50:00:01`, user name from
the `UserConfig`, `node_count` 1. SSID: session ID as 32 hex characters (the real derivation is not documented),
channel as passed, `network_type` 0 (meaning undocumented). Advertise data (`SetAdvertiseData`, up to 0x180 bytes)
ends up in the `NetworkInfo`.

With `--ldn lan`, the offline service is replaced by ldn and UDP sockets that reach other instances of the runtime in the
LAN: [`ldn-ueber-lan.md`](ldn-ueber-lan.md).

## `friend:u`

`IServiceCreator` 0 `CreateFriendService` → `IFriendService` (0 `GetCompletionEvent`, never signaled; 1 `Cancel`),
1 `CreateNotificationService` → `INotificationService` (0 `GetEvent`, 1 `Clear`). **Not implemented:**
10700 `GetPlayHistoryRegistrationKey`, 10702 `AddPlayHistory`. Their signature and the layout of the key are
not on switchbrew. Games only need them after they have met another console, which the offline
`ldn:u` never allows.

## `acc:u0`: no Network Service Account

`IManagerForApplication` (from 101 `GetBaasAccountManagerForApplication`): 0 `CheckAvailability`, 1 `GetAccountId`,
3 `LoadIdTokenCache` return **124-0200** (`ResultNetworkServiceAccountRegistrationRequired`), 2
`EnsureIdTokenCacheAsync` returns an `IAsyncContext` that is done immediately (event signaled, `HasDone` 1,
`GetResult` 124-0200). That the console answers exactly like this for a user who is not linked is an assumption:
switchbrew documents the code, not which command returns it.

## Library applets without a UI

All of them run, like the software keyboard, in the calling guest thread (`am_detail::LibraryApplet`); `Start` ends the
applet immediately. For error and playerSelect, in-storage 0 is `CommonArguments`, in-storage 1 the start parameter.

- **error (AppletId 0x0E):** the applet logs the message as a warning, e.g. `error applet: error 2110-1100`.
  Evaluated are `ErrorType` 0 SystemData (Result or `ErrorCode`), 1 SystemError (`ErrorCode` + text) and
  2 ApplicationError (number + text); other types only with their type number. Return value: `ErrorViewerReturnValue` {0, 0}
  (target "Nowhere"), so that a game that fetches the return value does not fail on `NoDataInChannel`. This corresponds to
  a user closing the message with OK.
  **Display:** code and text go via `AppletState::show_message` to the same host input as the
  software keyboard (`platform::ITextInput::show_message`, bridge `boot::message_handler`, set with
  `BootOptions::text_input`). The guest thread waits until the player presses OK; only then does the applet close.
  Boot program `--keyboard`: `window` shows a dialog window over the game (code as the header line, text wrapped at
  48 characters, Enter or Esc closes, typing is ignored), `host` in the browser a modal
  `<dialog>` with an OK button, `console` prints the message and waits for Enter. `none` and `text:NAME` show nothing;
  there the message is only in the log (`error applet: application error 15: …`). Previously, on the PC only a brief
  black screen was visible (presumably the game fades out for the applet, and the HLE applet was done immediately).
  Limit: the window's 8×8 font only knows ASCII (umlauts are transliterated, Japanese text appears as
  `?`). The message texts come from the game in its language; English and German are readable, Japanese is not.
- **netConnect (AppletId 0x0F):** only for the `nifm` path above. Reads no input, logs
  `net connect applet: no network, error 2110-1100` and returns `{s32 1, Result 2110-1100}`.
- **playerSelect (AppletId 0x10):** `UiSettings.mode` 0 (`UserSelector`) returns the one user
  (`UiReturnArg`: u64 result 0, user ID). Exceptions (layout libnx `PselUiSettingsV1`): if
  `is_network_service_account_required` (+0x90) is set or our user is in `invalid_uid_list` (+0x8, 8 IDs),
  there is nobody to choose, result **124-0001**. Mode 2 (`EnsureNetworkServiceAccountAvailable`) and all other
  modes end with **124-0001** (`ResultCancelledByUser`), as if the user had cancelled the linking.

Other applets (e.g. 0x0D dataErase, 0x12 web) still halt the guest.

## `bsd:u` and `ssl`: sockets without a peer

Games often start `nn::socket` and `nn::ssl` already when opening a communication menu, even for local play
(observed in LGP before the local trade). If one of the services is missing in `sm`, the SDK aborts with `svcBreak`
(result 0xE15 `NotRegistered`). The runtime registers both (`sockets.cpp`, switchbrew "Sockets services"/"SSL
services", libnx `bsd.c`/`socket.c`/`ssl.c`).

Local wireless play runs as IP over the `ldn` network that the game builds itself. As host, LGP opens a UDP socket directly
after `ldn` `CreateNetwork` and `GetIpv4Address`. If `Socket` fails, the game tears the network
down (`Finalize`) and starts over (PC run 2026-10-13, endless loop with `ENETDOWN`). That is why there are
sockets that work as in a network without other players: whatever is sent disappears, nothing ever arrives.

| Service | Commands | Response |
|---|---|---|
| `bsd:u` | 0 `RegisterClient` (configuration, PID, transfer memory) | success, output **0**. switchbrew: 4 bytes, meaning undocumented; with the process ID there, LGP's SDK aborted right afterwards (result 0x2A2, observed on the PC), hence 0 (inference, not proven) |
| | 1 `StartMonitoring` | success |
| | 2/3 `Socket`/`SocketExempt` (only `AF_INET`, stream or datagram) | descriptor from 3, shared by all `bsd:u` sessions of the process; other domains errno 97 |
| | 13 `Bind`, 16 `GetSockName` | address is stored, port 0 gets one from 49152 |
| | 14 `Connect` | UDP: success (default destination only). TCP: errno 101 `ENETUNREACH` |
| | 10/11/24 `Send`/`SendTo`/`Write` | UDP: full length, data discarded. TCP unconnected: 107 `ENOTCONN` |
| | 8/9/25 `Recv`/`RecvFrom`/`Read`, 12 `Accept` | errno 11 `EAGAIN`, **also for blocking sockets** (deviation, see below) |
| | 6 `Poll`, 5 `Select` | immediately: UDP writable, never readable; unknown descriptor `POLLNVAL` |
| | 20 `Fcntl` (`F_GETFL` 3/`F_SETFL` 4, `O_NONBLOCK` = 0x800), 19 `Ioctl` (`FIONBIO`, `FIONREAD` 0) | as described, other ioctls ignored |
| | 17/21 `Get`/`SetSockOpt`, 18 `Listen` (TCP), 22/23 `Shutdown` | success, options read as 0 |
| | 26 `Close` | success; afterwards `EBADF` |
| | 4 `Open`, 7 `Sysctl`, 27, 29, 30 | errno 95 `EOPNOTSUPP` |
| `ssl` | 5 `SetInterfaceVersion`, 0 `CreateContext` → `ISslContext` | success |
| `ISslContext` | 0 `SetOption`, 1 `GetOption`, 3 `GetConnectionCount` (0) | option 1 is stored |

Every response begins with `{s32 ret, s32 errno}`, followed by additional outputs (e.g. u32 address length). The
errno values are Linux numbers, not FreeBSD (switchbrew). On an error the caller only reads `ret` and `errno`.

**Assumptions and deviations:** that a real console without network creates sockets at all is not proven (under
FreeBSD `socket()` needs no interface). The choice of errno is an assumption. A blocking `Recv` would wait forever on
the console. Here it returns with `EAGAIN`; likewise `Poll`/`Select` return immediately despite a time limit.
Waiting in the handler would hold the session lock and, under the baton scheduler, the whole guest. A game
that polls in a tight loop thus burns CPU time instead of sleeping. Observed: LGP reads blocking, accepts `EAGAIN`
and waits itself between attempts (about 1 % CPU).

`ISslContext::CreateConnection` and the PKI commands are missing: a TLS connection needs a peer, which never
exists. Anyone continuing after an accepted `nifm` request would also need `sfdnsres`; there `nifm` already
stops the game today.

## `bcat:u`: never been online

`nn::bcat` reads content that the console downloaded in the background (delivery cache). A game opens the
service to look for earlier deliveries; if it is missing, the SDK aborts with `svcBreak` (0xE15). The runtime
(`bcat.cpp`, switchbrew "BCAT services", results of module 122 from "Error codes"):

| Object | Commands | Response |
|---|---|---|
| `bcat:u` | 0 `CreateBcatService`, 1/2 `CreateDeliveryCacheStorageService(WithApplicationId)` | objects |
| `IBcatService` | 10100/10101/20100/20101 `RequestSyncDeliveryCache…` | **2122-0008** `InternetRequestNotAccepted` (immediately, without a progress object; **assumption**) |
| | 10200 `CancelSyncDeliveryCacheRequest` | success |
| `IDeliveryCacheStorageService` | 10 `EnumerateDeliveryCacheDirectory` | 0 directories |
| | 0/1 `CreateFile`/`CreateDirectoryService` | objects whose `Open` fails with 2122-0002 `NotFound`; `Read`/`GetSize`/`GetCount`/`GetDigest` → 2122-0007 `NotOpened` |

The layout behind `IDeliveryCacheProgressService::GetImpl` (status, progress, result) is not on switchbrew.
Therefore the runtime hands out no progress object at all, but lets the request itself fail. Whether the console without
Internet answers the same way or first lets the progress end with an error is not proven.

## `btm:u`/`bt`: no BLE device in range

Games that talk to a phone (e.g. Pokémon GO) or a BLE accessory use `nn::bluetooth` (BLE client).
According to switchbrew "BTM services" and libnx `btmu.h`/`bt.h` it works like this: scanning and connection setup go through
`btm:u` → `IBtmUserCore`, the GATT read and write commands through `bt` (`IBluetoothUser`). Which SDK function calls which
command is not documented; the mapping below follows the names and is an assumption.

| `IBtmUserCore` | Response without a device |
|---|---|
| 0, 17, 21, 26, 33 `AcquireBle{Scan,Connection,Pairing,ServiceDiscovery,MtuConfig}Event` | `true` + event, never fires |
| 1 `GetBleScanFilterParameter(u16)` → `BleAdvertisePacketParameter` (8 bytes) | zeros (**assumption**; the console reads system data) |
| 2 `GetBleScanFilterParameter2(u16)` → `GattAttributeUuid` (0x14 bytes) | size 0x10, UUID zero (**assumption**) |
| 3/6/8 `StartBleScanFor{General,Paired,SmartDevice}`, 4/7/9 `Stop…` | success |
| 5/10 `GetBleScanResultsFor{General,SmartDevice}` | 0 results |
| 20 `BleGetConnectionState`, 25 `BleGetPairedDevices` | 0 entries |
| 36/37 `Register`/`UnregisterBleGattDataPath` (`BleDataPath`: u8, padding, `GattAttributeUuid`) | success without effect (no data ever arrives); LGP calls 36 first when connecting to Pokémon GO |
| 18 `BleConnect`, 19, 22–24, 27–35 (connection, pairing, GATT, MTU) | **not implemented** |

`bt`: only 9 `RegisterBleEvent`. 0–8 (`LeClientRead/Write…`, `GetLeEventInfo`) need a connection and are missing.

Why the connection commands are missing: without a scan result the SDK has no address with which it could call `BleConnect`.
switchbrew lists only internal errors for btm (module 143), no "connection unknown". A call should therefore halt
and become visible, instead of continuing with an invented code. Whether a game aborts by itself when the scan yields
no result or waits for the user (button B) is up to the game.

## What is deliberately missing

- `sfdnsres`/`nsd:u` (DNS): a game only reaches them after an accepted
  `nifm` request. That never happens offline, as long as the game respects `IsNetworkAvailable`.
- `aoc:u`, `caps:*`, `nfp`/`nfc`, `mii:u`, `irs`, `audin:u`: only add them if a game actually calls them
  (log `sm: GetServiceHandle("…"): not registered`).

## Verification

`runtime.hle_services(_checked)`: `test_nifm` (states, result before/after `Submit`, `GetAppletInfo` → netConnect),
`test_ldn` (all transitions, invalid states and inputs, `NetworkInfo` fields at the libnx offsets, ID −1),
`test_bcat` (empty cache, sync → 2122-0008), `test_sockets` (start, `Socket` → ENETDOWN, `Close` → EBADF, SSL context), `test_quiet_services` (BLE scan without a result, `BleConnect` halts), `test_friends`, `test_acc` (NSA errors, async context) and `test_applets` (error, netConnect with an 8-byte return value, playerSelect incl. NSA requirement and excluded user).
None of this has been checked against a real game.
