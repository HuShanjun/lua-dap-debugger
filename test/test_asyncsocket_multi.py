"""asyncsocket multi-client: two TCP clients, per-conn echo."""
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PORT = 18222
SCRIPT = Path(__file__).resolve().parent / "run_asyncsocket_multi.lua"
PAYLOADS = (b"alpha", b"bravo")


def find_lua():
    for p in [
        ROOT / "bin" / "lua.exe",
        ROOT / "bin" / "Debug" / "lua.exe",
    ]:
        if p.exists():
            return str(p)
    hits = list(ROOT.glob("**/lua.exe"))
    if hits:
        return str(hits[0])
    raise SystemExit("lua.exe not found; build the project first")


def read_until(proc, token, timeout):
    buf = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            break
        buf.append(line)
        if token in line:
            return "".join(buf)
    raise TimeoutError(
        "did not see %r in lua stdout before timeout; got: %r" % (token, "".join(buf))
    )


def recv_exact(sock, n, timeout):
    sock.settimeout(timeout)
    data = b""
    deadline = time.time() + timeout
    while len(data) < n and time.time() < deadline:
        chunk = sock.recv(n - len(data))
        if not chunk:
            break
        data += chunk
    return data


def main():
    lua = find_lua()
    proc = subprocess.Popen(
        [lua, str(SCRIPT), str(ROOT), "127.0.0.1", str(PORT)],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    clients = []
    try:
        prefix = read_until(proc, "LISTENING", timeout=5.0)
        time.sleep(0.05)
        for payload in PAYLOADS:
            c = socket.create_connection(("127.0.0.1", PORT), timeout=3.0)
            c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            clients.append(c)
        for c, payload in zip(clients, PAYLOADS):
            c.sendall(payload)
        for c, payload in zip(clients, PAYLOADS):
            echo = recv_exact(c, len(payload), timeout=3.0)
            assert echo == payload, "echo %r expected %r" % (echo, payload)
        for c in clients:
            c.shutdown(socket.SHUT_WR)
            c.close()
        clients = []

        rest, _ = proc.communicate(timeout=8)
        out = prefix + (rest or "")
        assert out.count("ACCEPT") >= 2, out
        assert "ECHO alpha" in out, out
        assert "ECHO bravo" in out, out
        assert out.count("CLOSE") >= 2, out
        assert "DONE" in out, out
        print("asyncsocket multi ok")
    finally:
        for c in clients:
            try:
                c.close()
            except Exception:
                pass
        try:
            proc.kill()
        except Exception:
            pass


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(exc, file=sys.stderr)
        sys.exit(1)
