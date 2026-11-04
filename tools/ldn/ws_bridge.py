#!/usr/bin/env python3
"""WebSocket bridge in front of a RyuLDN server (docs/knowledge/ldn-ueber-ryuldn.md).

`--ldn-server ws://HOST[:PORT][/PATH]` sends the RyuLDN byte stream as binary
WebSocket messages (RFC 6455). This bridge accepts such connections and opens
one TCP connection to the server for each, so the server can sit behind a
Cloudflare tunnel or another HTTP proxy that only forwards HTTP and WebSocket.
Plain HTTP GET (no upgrade) answers 200 "lgp ldn bridge" for health checks.

Only the standard library; Python >= 3.9.

    python3 ws_bridge.py --listen 0.0.0.0:8080 --upstream ldn-server:30456

With --listen HOST:0 the bridge picks a free port and prints
"listening on HOST:PORT" (the tests read that line). --delay-ms N holds
every message N ms in each direction (latency test, an internet path
between two instances on one LAN). --tls-cert/--tls-key serve wss:// itself
(tests, a server without a TLS-terminating proxy in front).
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import os
import ssl
import struct
import sys

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_FRAME = 1 << 20
MAX_HEADERS = 16384


def accept_key(key: str) -> str:
    return base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()


def frame(opcode: int, payload: bytes) -> bytes:
    """A server frame: final, unmasked."""
    n = len(payload)
    if n < 126:
        head = struct.pack("!BB", 0x80 | opcode, n)
    elif n <= 0xFFFF:
        head = struct.pack("!BBH", 0x80 | opcode, 126, n)
    else:
        head = struct.pack("!BBQ", 0x80 | opcode, 127, n)
    return head + payload


async def read_frame(reader: asyncio.StreamReader) -> tuple[int, bytes]:
    b0, b1 = await reader.readexactly(2)
    opcode = b0 & 0x0F
    n = b1 & 0x7F
    if n == 126:
        (n,) = struct.unpack("!H", await reader.readexactly(2))
    elif n == 127:
        (n,) = struct.unpack("!Q", await reader.readexactly(8))
    if n > MAX_FRAME:
        raise ValueError("frame too large")
    mask = await reader.readexactly(4) if b1 & 0x80 else b"\0\0\0\0"
    data = bytearray(await reader.readexactly(n))
    if b1 & 0x80:
        for i in range(n):
            data[i] ^= mask[i & 3]
    return opcode, bytes(data)


async def read_request(reader: asyncio.StreamReader) -> tuple[str, dict[str, str]]:
    raw = await reader.readuntil(b"\r\n\r\n")
    if len(raw) > MAX_HEADERS:
        raise ValueError("headers too long")
    lines = raw.decode("latin-1").split("\r\n")
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    return lines[0], headers


class Bridge:
    def __init__(self, upstream_host: str, upstream_port: int, path: str | None, quiet: bool, delay_ms: int = 0):
        self.upstream = (upstream_host, upstream_port)
        self.path = path
        self.quiet = quiet
        self.delay = delay_ms / 1000.0
        self.count = 0

    def log(self, text: str) -> None:
        if not self.quiet:
            print(text, flush=True)

    async def handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self.count += 1
        tag = f"#{self.count}"
        # Behind Cloudflare the client address is in CF-Connecting-IP.
        peer = writer.get_extra_info("peername")
        try:
            try:
                request, headers = await asyncio.wait_for(read_request(reader), 10)
            except (asyncio.TimeoutError, asyncio.IncompleteReadError, asyncio.LimitOverrunError, ValueError):
                return
            client = headers.get("cf-connecting-ip") or headers.get("x-forwarded-for") or (peer[0] if peer else "?")
            parts = request.split(" ")
            if len(parts) < 2 or parts[0] != "GET":
                writer.write(b"HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
                return
            if headers.get("upgrade", "").lower() != "websocket" or "sec-websocket-key" not in headers:
                body = b"lgp ldn bridge\n"
                writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n"
                             b"Connection: close\r\n\r\n" % len(body) + body)
                return
            if self.path is not None and parts[1] != self.path:
                writer.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
                return
            try:
                up_reader, up_writer = await asyncio.wait_for(asyncio.open_connection(*self.upstream), 5)
            except (OSError, asyncio.TimeoutError) as e:
                self.log(f"{tag} {client}: upstream {self.upstream[0]}:{self.upstream[1]} unreachable: {e}")
                writer.write(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
                return
            writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          f"Sec-WebSocket-Accept: {accept_key(headers['sec-websocket-key'])}\r\n\r\n").encode())
            await writer.drain()
            self.log(f"{tag} {client}: connected")
            await self.pump(reader, writer, up_reader, up_writer)
            self.log(f"{tag} {client}: closed")
        except (ConnectionError, OSError):
            pass
        finally:
            writer.close()

    async def pump(self, reader, writer, up_reader, up_writer) -> None:
        loop = asyncio.get_running_loop()
        # With a delay, each direction queues (due time, bytes) and a sender
        # writes them when due, so the delay does not add up between messages.
        up_queue: asyncio.Queue = asyncio.Queue()
        down_queue: asyncio.Queue = asyncio.Queue()

        async def send_due(queue: asyncio.Queue, out: asyncio.StreamWriter, wrap) -> None:
            while True:
                due, data = await queue.get()
                if data is None:
                    return
                wait = due - loop.time()
                if wait > 0:
                    await asyncio.sleep(wait)
                out.write(wrap(data))
                await out.drain()

        async def to_upstream(data: bytes) -> None:
            if self.delay > 0:
                up_queue.put_nowait((loop.time() + self.delay, data))
            else:
                up_writer.write(data)
                await up_writer.drain()

        async def to_downstream(data: bytes) -> None:
            if self.delay > 0:
                down_queue.put_nowait((loop.time() + self.delay, data))
            else:
                writer.write(frame(0x2, data))
                await writer.drain()

        async def to_server() -> None:
            while True:
                opcode, data = await read_frame(reader)
                if opcode in (0x0, 0x1, 0x2):
                    await to_upstream(data)
                elif opcode == 0x9:  # ping
                    writer.write(frame(0xA, data))
                    await writer.drain()
                elif opcode == 0x8:  # close
                    writer.write(frame(0x8, data[:2]))
                    await writer.drain()
                    return
                elif opcode != 0xA:
                    return

        async def to_client() -> None:
            while True:
                data = await up_reader.read(65536)
                if not data:
                    writer.write(frame(0x8, struct.pack("!H", 1000)))
                    await writer.drain()
                    return
                await to_downstream(data)

        tasks = [asyncio.ensure_future(to_server()), asyncio.ensure_future(to_client())]
        if self.delay > 0:
            tasks += [asyncio.ensure_future(send_due(up_queue, up_writer, lambda d: d)),
                      asyncio.ensure_future(send_due(down_queue, writer, lambda d: frame(0x2, d)))]
        try:
            await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        finally:
            for t in tasks:
                t.cancel()
            for t in tasks:
                try:
                    await t
                except (asyncio.CancelledError, asyncio.IncompleteReadError, ConnectionError, OSError, ValueError):
                    pass
            up_writer.close()


def split_host_port(text: str, what: str) -> tuple[str, int]:
    host, sep, port = text.rpartition(":")
    if not sep or not port.isdigit():
        raise SystemExit(f"{what}: HOST:PORT expected, got {text!r}")
    return host, int(port)


async def main_async(args: argparse.Namespace) -> None:
    host, port = split_host_port(args.listen, "--listen")
    up_host, up_port = split_host_port(args.upstream, "--upstream")
    bridge = Bridge(up_host, up_port, args.path, args.quiet, args.delay_ms)
    tls = None
    if args.tls_cert:
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(args.tls_cert, args.tls_key)
    server = await asyncio.start_server(bridge.handle, host, port, ssl=tls)
    bound = server.sockets[0].getsockname()
    print(f"listening on {bound[0]}:{bound[1]}, upstream {up_host}:{up_port}", flush=True)
    async with server:
        await server.serve_forever()


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--listen", default=os.environ.get("BRIDGE_LISTEN", "0.0.0.0:8080"))
    p.add_argument("--upstream", default=os.environ.get("BRIDGE_UPSTREAM", "ldn-server:30456"))
    p.add_argument("--path", default=os.environ.get("BRIDGE_PATH") or None,
                   help="only upgrade requests to this path (default: any)")
    p.add_argument("--quiet", action="store_true", help="no line per connection")
    p.add_argument("--delay-ms", type=int, default=0, help="hold every message this long in each direction")
    p.add_argument("--tls-cert", help="PEM certificate (chain): serve wss:// instead of ws://")
    p.add_argument("--tls-key", help="PEM private key of --tls-cert")
    args = p.parse_args()
    try:
        asyncio.run(main_async(args))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
