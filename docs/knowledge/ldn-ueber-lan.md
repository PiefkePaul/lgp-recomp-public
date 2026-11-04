# ldn over LAN: local wireless play between two instances of the runtime

Game-independent knowledge on `--ldn lan` (`runtime/hle/src/ldn_lan.{hpp,cpp}`, attached in `ldn.cpp` and
`sockets.cpp`; platform `platform/include/lgp/platform/lan.hpp`, `platform/os/src/lan.cpp`). The LGP status and the
design are in `../analysis/44-multiplayer-stand.md`. Offline ldn without
LAN is described in [`horizon-offline-konsole-nifm-ldn-friend.md`](horizon-offline-konsole-nifm-ldn-friend.md).

## Principle

On the console, `ldn` forms its own Wi-Fi network, and the games speak IPv4 over it with `bsd` sockets. The runtime
replaces the radio network with a **virtual network, tunneled over a UDP socket of the host**. This is the same idea
as in RyuLDN and ldn_mitm (concept only, no code taken):

- The host assigns the addresses 10.A.B.(node+1)/24 (A 96–127, B 1–254, random per host instance, not
  from the session ID, not link-local) and MACs
  `02:00:<IPv4-Bytes>` (`bssid` = MAC of node 0). `CreateNetwork` over LAN randomizes the session ID so that two
  instances never have the same one; the SSID is its hex text. The host
  manages the `NetworkInfo` and distributes it unchanged. Network libraries such as Pia derive keys from `NetworkId` and
  MAC and silently discard if the bytes differ between nodes.
- A guest datagram to an address of the virtual subnet or a broadcast goes as a `Data` packet
  (virtual source and virtual destination, ports, payload) to the host. The host delivers it or forwards it to the
  destination station (star). A broadcast goes to everyone, including the sender (like RyuLDN).
- Packets: `ScanRequest`/`ScanResponse` (`NetworkInfo`), `ConnectRequest`/`Accept`/`Reject`, `NetworkUpdate`,
  `Leave`, `Destroyed`, `Data`, `Ping`. Each starts with magic `LGPL` and version 3 (different versions do not talk
  to each other).
- An instance recognizes its own networks by its random instance ID in the scan request and response, **not** by the
  session ID: with `CreateNetworkPrivate` the game specifies the session ID, and two searching instances then host
  with the same one (LGP, PC run 2026-10-21). A local communication ID −1 in the scan filter becomes the
  program ID, as with `CreateNetwork`.
- Keepalive: in the network every node pings its counterpart every `keepalive_ms` (1 s); every packet counts as a
  sign of life. After `peer_timeout_ms` (5 s) of silence the station loses the network with `DisconnectReason` 6
  (SignalLost); the host removes the station and distributes the new `NetworkInfo`.
- Without a network the receive thread waits indefinitely (no periodic wake-up) and is woken on close by a
  datagram to its own port; in a network it wakes up for the keepalive. The bodies are the packed
  ldn structures (little-endian).

## Mapping of the commands

| ldn command | over LAN |
|---|---|
| 400/402 `Initialize` | opens the host socket (`--ldn-port`, taken → any port) and pump thread |
| 202/203 `CreateNetwork(Private)` | network is offered on the LAN, node 0 gets .1 |
| 206/207 advertise data, accept policy | go to the stations immediately (`NetworkUpdate`) |
| 102/103 `Scan` | also while hosting (own network excluded); channel ≠ 0 filters to that channel; probes to broadcast, the own /24 via unicast and `--ldn-peer`; collect for 150 ms; filter flags LocalCommunicationId, SessionId, SceneId, IntentId, NetworkId |
| 302 `Connect` | looks up the host via the session ID from an earlier scan (remembered also across `Finalize`); repeats every 250 ms, up to 2 s; error → 203-0065 |
| 303 `ConnectPrivate` | likewise, session ID from the `SecurityParameter` |
| 204 `DestroyNetwork`, 201 `CloseAccessPoint` | stations get `Destroyed` (3×), fall to state 4, `DisconnectReason` 3 |
| 304 `Disconnect`, 301 `CloseStation` | `Leave` to the host, whose `NetworkInfo` loses the node |
| 2 `GetIpv4Address` | virtual address of the node, mask /24 |

The state event (100) is also signaled on join and leave of other nodes, from the pump thread.

`bsd:u`: UDP sockets with a destination in the ldn network send through the tunnel (`SendTo`, or `Send`/`Write` to the destination from
`Connect`). Incoming data sits in a queue per bound port (256 datagrams). `Recv`/`RecvFrom`/`Read`
read from it, `RecvFrom` returns the virtual sender address. `Poll`/`Select` report readable, `FIONREAD` the size
of the next datagram. Empty still means `EAGAIN` without waiting.

## Host side

- POSIX (`getifaddrs`, non-blocking sockets, `SO_BROADCAST`, 1 MiB `SO_RCVBUF`). Interfaces only with
  `IFF_BROADCAST` and without `IFF_POINTOPOINT`, i.e. Wi-Fi and Ethernet, not cellular and VPN. The /24 sweep only runs
  on networks of /24 or larger.
- iOS: unicast needs the "Lokales Netzwerk" (local network) permission (`NSLocalNetworkUsageDescription`). Broadcast needs
  `com.apple.developer.networking.multicast`. Bonjour needs `NSBonjourServices`, but no entitlement (Apple
  TN3179). Both are in `platform/ios/Info.plist.in`.
- Apple (macOS, iOS, tvOS): Bonjour discovery with DNS-SD (`platform/apple/src/lan_bonjour.cpp`, `ILan::advertise`/
  `discovered`). The host registers `_lgp-ldn._udp` with its port, without binding it a second time.
  Stations search from `Initialize` and resolve finds via `Resolve` and `GetAddrInfo` into IPv4; the scan then probes them
  via unicast. `kDNSServiceFlagsIncludeP2P` also allows AWDL, but only for discovery: the BSD data socket
  does not send over AWDL. Errors are only logged; the other probes remain.
- Windows: Winsock (`WSAStartup` once, `ioctlsocket(FIONBIO)`, `SO_BROADCAST`, 1 MiB `SO_RCVBUF`,
  `SO_EXCLUSIVEADDRUSE` so that a second instance does not share the host's port, `SIO_UDP_CONNRESET` off,
  otherwise an ICMP "Port unreachable" for a probe reports `WSAECONNRESET` on the next receive). Interfaces via
  `GetAdaptersAddresses`, only Ethernet and Wi-Fi with status "up", without 169.254/16. Linked with `ws2_32` and `iphlpapi`.
- Browser: `make_host_lan()` returns `nullptr` (no UDP), `--ldn lan` stays offline.

## Diagnostics: host without a game

`build/<preset>/tests/runtime/lgp_ldn_fakehost [--port P] [--peer IP:PORT] [--scene N] [--channel C] [--version V]
[--seconds S]` hosts a network via the real `ldn:u` service that never scans: `CreateNetworkPrivate` with the
LGP identifier, session `01 00…`, two slots and advertise data from the public 24-byte header (random network ID
and session param). Channel by default `{1, 6, 11}[Szene % 3]`. Against a game instance it shows whether the game joins a
pure host (`fakehost: … the game joined`). This separates "our scan response is discarded" from
"two searching instances do not assign roles". After that, Pia fails, because the fake does not speak Pia.

## Limits

No check of passphrase and security mode. Rejections always report
203-0065 (real codes not established). Host and station on the same machine do not share a port: the
second instance needs `--ldn-peer 127.0.0.1:<port>`.

Test: `runtime.hle_ldn_lan` (two runtimes in one process over 127.0.0.1).
