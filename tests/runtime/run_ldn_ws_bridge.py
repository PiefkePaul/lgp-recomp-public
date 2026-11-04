#!/usr/bin/env python3
"""ctest runtime.hle_ldn_ryu_ws(s): ldn through the WebSocket bridge.

Starts tools/ldn/ws_bridge.py in front of a free port, then runs
lgp_test_hle_ldn_ryu with LGP_RYULDN_FAKE_PORT (its small server binds that
port) and LGP_RYULDN_WS (the clients connect over ws:// to the bridge).
With --tls OPENSSL the bridge serves wss:// with a self-signed certificate
for localhost made by OPENSSL, and LGP_RYULDN_CA names it.

    run_ldn_ws_bridge.py <lgp_test_hle_ldn_ryu> <ws_bridge.py> [--tls OPENSSL]
"""

import os
import socket
import subprocess
import sys
import tempfile


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def make_cert(openssl: str, folder: str) -> tuple:
    cert, key = os.path.join(folder, "cert.pem"), os.path.join(folder, "key.pem")
    subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", cert,
                    "-days", "2", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return cert, key


def main() -> int:
    with tempfile.TemporaryDirectory() as folder:
        return run(folder)


def run(folder: str) -> int:
    test, bridge_script = sys.argv[1], sys.argv[2]
    tls = len(sys.argv) > 4 and sys.argv[3] == "--tls"
    extra, env_extra = [], {}
    if tls:
        cert, key = make_cert(sys.argv[4], folder)
        extra = ["--tls-cert", cert, "--tls-key", key]
        env_extra["LGP_RYULDN_CA"] = cert
    upstream = free_port()
    bridge = subprocess.Popen(
        [sys.executable, "-I", bridge_script, "--listen", "127.0.0.1:0", "--upstream", f"127.0.0.1:{upstream}",
         "--path", "/ldn", "--quiet"] + extra,
        stdout=subprocess.PIPE, text=True)
    try:
        line = bridge.stdout.readline()
        if not line.startswith("listening on "):
            print(f"bridge did not start: {line!r}")
            return 1
        port = int(line.split()[2].split(",")[0].rsplit(":", 1)[1])
        print(line.strip(), flush=True)
        url = f"wss://localhost:{port}/ldn" if tls else f"ws://127.0.0.1:{port}/ldn"
        env = dict(os.environ, LGP_RYULDN_FAKE_PORT=str(upstream), LGP_RYULDN_WS=url, **env_extra)
        return subprocess.run([test], env=env, timeout=300).returncode
    finally:
        bridge.terminate()
        try:
            bridge.wait(5)
        except subprocess.TimeoutExpired:
            bridge.kill()


if __name__ == "__main__":
    sys.exit(main())
