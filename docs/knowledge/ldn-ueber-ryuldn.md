# ldn via a RyuLDN server: online play instead of Wi-Fi

Game-independent knowledge on `--ldn ryuldn` (`runtime/hle/src/ldn_ryu.{hpp,cpp}`, attached like `--ldn lan` via
the `ldn::Link` interface in `ldn_lan.hpp`; TCP in `platform/include/lgp/platform/lan.hpp` (`ILanStream`,
`ILan::connect_tcp`), `platform/os/src/lan.cpp`). The LAN approach is described in
[`ldn-ueber-lan.md`](ldn-ueber-lan.md), the LGP status in `../analysis/44-multiplayer-stand.md`.

## Principle

Instead of letting the instances search each other on Wi-Fi, each instance connects via TCP to a server that speaks the
**RyuLDN protocol** (online play of the Ryujinx emulator). The server manages the rooms (`NetworkInfo`), assigns
virtual addresses from 10.114.0.0/16 and forwards the game's datagrams (`ProxyData`). This works across
NAT and the internet, without port forwarding on the players' side.

- Server: [`github.com/Ryubing/LdnServer`](https://github.com/Ryubing/LdnServer) (MIT, C#/.NET 8, Docker + Redis,
  TCP port 30456). Maintained successor of `tsrberry/Ryujinx-LdnServer`, protocol version 1 (as of May 2026).
- Client: own implementation based on the packet layouts of the MIT server (`Network/Types`, `LdnServer/Types`). No code taken from
  Ryujinx, ldn_mitm or ryu_ldn_nx.
- The official server (`ldn.ryujinx.app`) is, according to its README, only for Ryujinx-to-Ryujinx; other clients may end up on
  its IP block list. **Only run against your own server.**

## Protocol (version 1)

Header 12 bytes: `u32 magic` = `RLDN`, `u8 type`, `u8 version` = 1, 2 bytes padding, `s32 size`, then the body
(little-endian, addresses as u32 in host order as in ldn). The padding is not in the C# type (`Size = 0xA`),
but arises from the alignment of `size`; the server reads 12 bytes.

| Packet (Id) | Direction | Body |
|---|---|---|
| Initialize (0) | both | `u8 id[16]`, `u8 mac[6]`; client sends zeros (or the old Id), server replies with Id and MAC |
| Passphrase (1) | → | `char[128]`; only `""` or `Ryujinx-` + 8 hex digits (lowercase) are valid, otherwise `""` |
| CreateAccessPointPrivate (3) | → | SecurityConfig, SecurityParameter, UserConfig, NetworkConfig, AddressEntry[8], RyuNetworkConfig (0x28) = 0x13C, then advertise data |
| SyncNetwork (7) | ← | `NetworkInfo` (room changed: join, leave, advertise data, acceptance) |
| Scan (10) | → | `ScanFilter` (0x60); flags as one byte, IntentId = 0x21, NetworkId = 0x23 |
| ScanReply (11) / ScanReplyEnd (12) | ← | one `NetworkInfo` per room, then the end (the server waits 200 ms beforehand) |
| ConnectPrivate (14) | → | SecurityConfig, SecurityParameter, UserConfig, `u32` version, `u32` option, 4 bytes padding, NetworkConfig = **0xC0** |
| Connected (15) | ← | `NetworkInfo` after opening or joining a room |
| Disconnect (16) | both | `u32 ip` (0); client: leave or close the room; server: room closed |
| ProxyConfig (17) | ← | `u32 ip`, `u32 mask` (before Connected) |
| ProxyData (20) | both | ProxyInfo (`u32` source, `u16` port, `u32` destination, `u16` port, `s32` protocol 17 = UDP), `u32` length, data |
| SetAcceptPolicy (22) / SetAdvertiseData (23) | → | `u8` / raw advertise data |
| Ping (254) | both | `u8 requester`, `u8 id`; if the server asks (requester 0), the client sends the same two bytes back |
| NetworkError (255) | ← | `s32`: 2 full, 3/4 version too old/new, 6 room not found, −1 unknown (e.g. room Id taken), 127 banned |

Pitfalls, found against the real server:

- **ConnectPrivate is 0xC0 bytes, not 0xBC.** The C# type declares `Size = 0xBC`, but `NetworkConfig` contains an
  `s64` and therefore sits at 0xA0. With 0xBC bytes the server throws (`ArgumentOutOfRangeException`) and never replies.
- Every body must be complete: a `Disconnect` without the 4 bytes likewise ends the session on the server.
- The server does not remember `SecurityParameter.server_random` (zero in the `NetworkInfo`). The client puts the game's
  value back in, the same on both sides (Pia derives keys from the `NetworkInfo`).
- A broadcast to the subnet broadcast address (or 192.168.0.255) goes to all members, **including the sender**.
  The client sends broadcasts as 10.114.255.255 and does not deliver an additional copy to itself.
- The room Id is the session ID. If a second instance opens a room with the same session ID (two searching
  LGP instances do that), the server refuses with −1. The game still sees its network (as offline), finds the
  other instance's room in the scan and joins.

## Security

The server has **no authentication**. The passphrase only hides rooms with a different passphrase in the search;
`ConnectPrivate` does not check it, the session ID is enough. The client turns the freely chosen room text
(`--ldn-room`, `settings.ini ldn_room`) into a passphrase via FNV-1a, so that foreign clients do not see your own rooms.
That is a privacy screen, not a lock. If you want more: make the server reachable only via VPN, or extend the server with a
shared secret (MIT allows that). The server also sees each client's real IP and the
game traffic in plain text.

## Options

- Boot program: `--ldn ryuldn --ldn-server HOST[:PORT] [--ldn-room TEXT]` (port 30456 if missing) or
  `--ldn-server ws://HOST[:PORT][/PATH]` via a WebSocket bridge (port 80, path `/`, see below), with TLS
  `wss://HOST[:PORT][/PATH]` (port 443). `--ldn-ca FILE` (PEM) replaces the system's root certificates for `wss://`
  (own certificate, tests). Without `--ldn-server` startup aborts with a hint.
- App: online through the server is always on, there is no switch for it (Paul, 2026-10-28); it needs the mod file
  `<game>/mods/online-button.cfg` (next section) and a server. `ldn_server = …` (also `ws://…`/`wss://…`) only names
  another server: empty means the build's server (CMake `LGP_LDN_SERVER_DEFAULT`, default `wss://lgp.server-work.de`,
  in builds without TLS `ws://…`, empty: none). `ldn_room = …` as `--ldn-room`. With `ldn = lan` (Wi-Fi on) the app
  starts with `--ldn both`, with `ldn = off` with `--ldn ryuldn` (then "nearby" goes over the server too). Without the
  mod file or without a server only the Wi-Fi (`--ldn lan`) or nothing. `ldn = ryuldn` of older builds is read as
  `ldn = lan`; `ldn_online` of older builds is read and dropped (also its 0). Menu, page "Spiel" (game): "Tauschen und
  Kämpfen im WLAN" (trade and battle over Wi-Fi), below it "Server" and "Raum" (room) as text rows (the server is
  checked with `parse_ryuldn_server`, like `--ldn-server`). iOS settings app: the same rows.
- Unreachable server: the instance stays alone (as offline), a new attempt at the earliest after 15 s or at the
  next ldn session. The connection persists across `Finalize` (games end ldn between scan and
  join).
- Server gone (connection closed or 30 s silent, even outside a room; the server pings every client that has been silent for 10 s):
  a station falls back to station with `SignalLost`. An access point keeps its network as the game
  knows it and stays alone (ldn cannot report a network loss to an access point); the next
  `CreateNetworkPrivate` opens a room on the server again.
- Log: lines `ldn ryu: …` (connected, room opened/joined, server errors, datagram counters every 5 s).

## LAN and server in the same run (`--ldn both`)

Goal: "Spieler in der Nähe" (players nearby) stays on Wi-Fi, "Spieler in der Ferne" (players far away) goes via the server, without a restart in between.
`--ldn both --ldn-server …` creates a `DualLink` (`runtime/hle/src/ldn_dual.{hpp,cpp}`), which holds a `LanLink` and
a `RyuLink` and uses one of them per ldn session:

- Which one is determined by `hle::set_ldn_remote(bool)` (default `false` = LAN; `LGP_LDN_REMOTE=1` starts with the server).
  Intended for the mod on the game's online button; without it, `both` stays on LAN.
- The choice takes effect at `Initialize` and, as long as no network exists, at every `Scan`, `CreateNetwork*` and `Connect*`
  (games may initialize ldn before the menu choice). An existing network stays on its route until it ends;
  the datagrams of `bsd:u` always go over the currently active route.
- Log: `ldn: this session goes over the LAN|through the server`, when switching `ldn: switched to the server|to the LAN`.
- Test: `test_dual` in `runtime.hle_ldn_ryu` (four rounds: LAN, server, and both directions switched after
  `Initialize`; the route shows in the netmask: server /16, LAN /24, the LAN networks 10.96–127.x also lie within 10.114).
- In the app: if "WLAN" (`ldn = lan`) is on and the mod file `<game>/mods/online-button.cfg` is present, it starts
  with `--ldn both` and names the mod `online-button` (next section); with the Wi-Fi off `--ldn ryuldn` plus the mod.
  Without the file, "Weit entfernt" (far away) would stay on the Nintendo route and the server would be unreachable;
  then only the Wi-Fi.

## Waiting for the server without a core slot (2026-10-24)

The ldn requests run synchronously in the guest thread (like every HLE IPC). Scan, Connect and `CreateNetwork` wait there
for the server's reply (the server replies to a scan only after 200 ms). In `core-baton` mode the
thread held its core's slot during this, and other guest threads of the same core stood still. On the PC (PC2, 2026-10-24,
`--time-stats`) the frame rate dropped only during search and connection setup, more strongly with latency; in battle
all blocks stayed at ≥ 29.3 fps. On the Switch the client blocks in the IPC, and its core keeps computing.

That is why `RyuLink` (waiting for a reply, TCP/TLS/WebSocket setup) and `LanLink` (scan, connect) give up the slot for the
wait (`rt::HostWorkScope(true)`, independent of `LGP_RENDER_RELEASE_SLOT`). Other modes are not affected
(`baton` stays reproducible, `free` has no slots). So that this does not deadlock, nobody holds a host lock meanwhile
that a slot holder wants to take. `CreateNetwork` calls `Link::host` outside the service mutex, because `GetState` takes it
every frame. Whoever finds the mutex of a session (`hle::Session`, every request) or the `DualLink` mutex taken waits without a slot (same pattern as the NVN lock,
`host_work.hpp`). Measurement on the PC (PC2, 2026-10-24, `--ldn ryuldn` via the NAS bridge without latency, ABAB, two
runs each): in the search and connection phase the frame rate previously dropped to 28.2 fps; afterwards it stayed at ≥ 29.9 after the
loading block. The `ready` time of the other guest threads dropped from 6–18 to 2–3.5 ms per frame; the ldn thread
waits just as long. There was no hang. Short probes with `--ldn both` ("Weit entfernt" via the server,
"In der Nähe" (nearby) via the LAN) and with `--ldn lan`: the connection was established each time, without a hang. With 200 ms
latency (PC2, 2026-10-25, link battle via "Weit entfernt", ABAB): before, the worst blocks during search
and connect fell to 25.1–27.0 fps, the other threads stood still 25–28 ms per frame. Afterwards they stayed at 29.9–30.0 fps,
the other threads stood still 1–2 ms. The individual requests still take 55–272 ms, but no longer hold a core.
In battle before up to 27.4 fps, afterwards 29.2–29.3. The LAN shows no regression (29.9–30.0).

## What is missing

- P2P proxy (ExternalProxy\*): the game traffic always goes via the server.
- TCP within the room (ProxyConnect\*) and `Reject`: according to current knowledge LGP needs neither (UDP only).
- Not yet checked with a real game (as of 2026-10-22); checked are opening a room, finding, joining,
  datagrams in both directions, broadcast and closing against the real Ryubing server (test
  `runtime.hle_ldn_ryu` with `LGP_RYULDN_SERVER=HOST:PORT`) and against the small server in the test (CI).

## WebSocket instead of TCP (Cloudflare tunnel)

A Cloudflare tunnel (and any HTTP reverse proxy) forwards only HTTP and WebSocket without software on the devices,
not plain TCP. With `ws://…` the client therefore sends the RyuLDN byte stream as binary WebSocket messages
(RFC 6455, `runtime/hle/src/ldn_ws.{hpp,cpp}`: upgrade with verification of `Sec-WebSocket-Accept`, masked frames,
ping/pong, close). In front of the server sits the bridge `tools/ldn/ws_bridge.py` (standard library only): per
WebSocket connection one TCP connection to the server, `GET` without upgrade answers 200 (health check),
`--path` accepts only one path, `--delay-ms N` holds back each message per direction by N ms (latency test: reproduce the internet route
between two instances on the same LAN, without delays adding up).

- `ws://` works via Cloudflare on port 80 as long as the zone does not redirect HTTP to HTTPS ("Always Use HTTPS" off
  or an exception for the host). `wss://` (port 443) encrypts up to Cloudflare; TLS ends there, the tunnel brings
  the traffic to the bridge. The bridge itself stays at `ws://` (`--tls-cert`/`--tls-key` only for tests without a tunnel).
  The server sees the game traffic in plain text either way.
- The server sees all clients with the bridge's address. Its IP block (`bannedips.txt`, automatic ban for
  profanity in the trainer name) would then hit everyone; the real address is only in the bridge's log
  (`CF-Connecting-IP`).
- The server pings idle clients every 10 s; that also keeps the WebSocket connection open past Cloudflare's
  idle limit (100 s).
- Tests: `runtime.hle_ldn_ryu_ws` (bridge in front of the small server in the test), `runtime.hle_ldn_ryu_wss` (the same with
  TLS: self-signed certificate for `localhost` via `openssl`, plus rejected without CA and with a wrong name;
  not created without `openssl` or without TLS in the build); against the real server with bridge
  `LGP_RYULDN_SERVER=ws://HOST:PORT/ build/default/tests/runtime/lgp_test_hle_ldn_ryu` (checked locally 2026-10-22),
  with `wss://lgp.server-work.de` (system roots) not yet checked from outside the cloud sandbox.

## TLS (`wss://`)

Mbed TLS 3.6.7 LTS (Apache-2.0, `cmake/LgpMbedTls.cmake`, static, `LGP_ENABLE_MBEDTLS`, on by default except for
Emscripten, offline `LGP_MBEDTLS_DIR`) in `platform/os/src/lan_tls.cpp`, access via `ILan::connect_tls`
(`lgp/platform/lan.hpp`, without TLS in the build an error "no TLS in this build"). One code path on all platforms instead of
a separate TLS layer per OS (SChannel, Network.framework, Java SSL): this way CI under Linux and Windows checks the same
connection that the devices use. Only certificate verification differs:

- Windows: roots from the system store `ROOT` (crypt32).
- Linux/Android: the system's CA bundle or folder (`/etc/ssl/certs/…`, `/apex/com.android.conscrypt/cacerts`,
  `/system/etc/security/cacerts`).
- macOS/iOS/tvOS: Mbed TLS does not verify itself; after the handshake the server's chain goes to `SecTrust`
  (`SecPolicyCreateSSL` with hostname, `platform/apple/src/tls_trust_apple.cpp`), i.e. to the system's roots and
  rules.

The name (SNI and verification) is the host from the address. Reading waits for data without a lock (`ILanStream::wait_readable`),
so that sending from other threads does not hang. In the cloud sandbox the egress gateway intercepts TLS (own CA in the
system bundle); a run against the real server there says nothing about the route to Cloudflare.
- There is no ldn in the browser yet (no host sockets). The WebSocket route is the prerequisite for it: there it would go
  via the browser's WebSocket API, with `wss://` from an HTTPS page.

## Running the server yourself

The repo's Dockerfile only builds for `linux-musl-arm64`. For x86_64, use your own Dockerfile that builds with
`dotnet publish -p:PublishAot=false` and runs on `mcr.microsoft.com/dotnet/runtime:8.0`. The server needs
Redis **with RedisJSON** (`redis/redis-stack-server`), otherwise it aborts at startup (`unknown command 'JSON.SET'`).
The port is plain TCP: behind a reverse proxy only as stream forwarding, no HTTP, no TLS; via a
Cloudflare tunnel with the WebSocket bridge (above) and `ws://`/`wss://` in the client.

## Online button ("Weit entfernt"), status 2026-10-23

Research on the PC (addresses private only): the online route first asks about internet and saves, then `ssl`
SetInterfaceVersion and a nifm request (preset 2). Without a network it ends there (2110-1100, netConnect, back to the
selection). Only after that do the routes split, in a shared PIA initialization with mode 0 = local (LDN),
1 = NEX (online), local from an object field, online from a helper function with a fixed 1. Only online is there
random matchmaking without a code and NAT/relay; link trade and link battle with a code exist on both routes.

Experiment on the PC (2026-10-23): `LGP_NIFM_ONLINE=1` gets the route past nifm, after which it fails at
`acc` (CheckAvailability 0x1907c, player selection aborted without a Nintendo Account, error 15). Mode 0 via
patch site changes nothing about that. Rebuilding a Nintendo Account and NEX is not worth it, hence plan B:

The selection menu is a separate frame of the quick-play app. After confirming, a method of this frame writes
a follow-up state and an online flag depending on the choice (nearby 2/0, far 3/1, both MOV constants) and then changes
the state, still before the internet question and before saving. With the nearby values in the far branch (PC,
`LGP_PATCH_IMM`), "Weit entfernt" leads image-identically into "Was möchtest du tun? Link-Tausch / Einzelkampf / Doppelkampf" (what do you want to do? link trade / single battle / double battle),
without the internet question, without saving, without nifm or ssl calls. Only random matchmaking without a code is lost.

Mod `online-button` (`runtime/mods/src/online_button.cpp`, mod file `<game>/mods/online-button.cfg`, private):
`patch far_state VA` and `patch far_flag VA` (the two MOVs in the far branch) get the nearby values
(`value far_state`/`far_flag`, default 2/0), plus `hle::set_ldn_remote(true)` via
`mods::set_ldn_route_handler`; optionally `patch near_state VA` (MOV in the nearby branch, only observed) sets the route
back to the LAN. The mod is not in the menu (`ModInfo::hidden`); the app names it as long as "Online" is on and
the mod file is in the game folder. Lift with `--mods <game>/mods` (patch sites). Log
`online-button: far away chosen, …`, test `runtime.mods` (part `online_button`, decoding of the MOV forms
`rt::decode_move_immediate`).

Checked on the PC (2026-10-23, `wss://lgp.server-work.de`): "Weit entfernt" leads without the internet question into the kind selection,
back and "In der Nähe" likewise, without nifm and ssl calls; the log line `ldn: this session goes …` only comes after
the kind selection. Two instances with "Weit entfernt", link trade and the same code find each other via the server and
both stand in the trade box. The second one first reports `server error: unknown (room id taken?)` and then
joins: the game itself first tries to host and searches afterwards, as on the LAN. Without the mod the
internet question comes back.
