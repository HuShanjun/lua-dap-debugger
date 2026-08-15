"""asyncsocket listen/pump smoke: OPEN + MSG hello + CLOSE."""
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PORT = 18221
SCRIPT = Path(__file__).resolve().parent / "run_asyncsocket_smoke.lua"


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
    try:
        prefix = read_until(proc, "LISTENING", timeout=5.0)
        time.sleep(0.05)
        c = socket.create_connection(("127.0.0.1", PORT), timeout=3.0)
        c.sendall(b"hello")
        c.shutdown(socket.SHUT_WR)
        c.close()
        rest, _ = proc.communicate(timeout=8)
        out = prefix + (rest or "")
        assert "OPEN" in out, out
        assert "MSG" in out and "hello" in out, out
        assert "CLOSE" in out, out
        print("asyncsocket smoke ok")
    finally:
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
