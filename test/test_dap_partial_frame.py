import json
import socket
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PORT = 18178


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


def read_message(sock, buf):
    while True:
        idx = buf.find(b"\r\n\r\n")
        if idx >= 0:
            header = buf[:idx].decode("ascii", errors="replace")
            length = None
            for line in header.split("\r\n"):
                if line.lower().startswith("content-length:"):
                    length = int(line.split(":", 1)[1].strip())
                    break
            if length is None:
                raise RuntimeError(f"missing Content-Length: {header!r}")
            start = idx + 4
            if len(buf) >= start + length:
                body = buf[start : start + length]
                buf = buf[start + length :]
                return json.loads(body.decode("utf-8")), buf
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("socket closed")
        buf += chunk


def wait_for(sock, buf, pred, limit=20):
    for _ in range(limit):
        msg, buf = read_message(sock, buf)
        if pred(msg):
            return msg, buf
    raise TimeoutError("wait_for exceeded limit")


def send_initialize_in_two_parts(sock):
    body = {
        "seq": 1,
        "type": "request",
        "command": "initialize",
        "arguments": {
            "adapterID": "lua-dap",
            "pathFormat": "path",
            "linesStartAt1": True,
            "columnsStartAt1": True,
        },
    }
    data = json.dumps(body).encode("utf-8")
    header = f"Content-Length: {len(data)}\r\n\r\n".encode("ascii")
    msg = header + data
    split_at = max(1, len(msg) // 2)
    sock.sendall(msg[:split_at])
    time.sleep(0.05)
    sock.sendall(msg[split_at:])


def main():
    lua = find_lua()
    script = f"""
package.path = ""
package.cpath = [[{ROOT.as_posix()}/bin/?.dll;{ROOT.as_posix()}/bin/Debug/?.dll;]]
local dap = require("luadap")
dap.start("127.0.0.1", {PORT}, true)
print("LISTEN_DONE")
"""
    proc = subprocess.Popen(
        [lua, "-e", script],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    time.sleep(0.5)
    sock = None
    try:
        sock = socket.create_connection(("127.0.0.1", PORT), timeout=3.0)
        sock.settimeout(3.0)
        send_initialize_in_two_parts(sock)
        buf = b""
        init_resp, buf = wait_for(
            sock, buf, lambda m: m.get("type") == "response" and m.get("command") == "initialize"
        )
        assert init_resp.get("success") is True, init_resp
        ev, buf = wait_for(sock, buf, lambda m: m.get("type") == "event" and m.get("event") == "initialized")
        assert ev["event"] == "initialized"
        print("partial frame ok")
    finally:
        try:
            proc.kill()
        except Exception:
            pass
        try:
            proc.wait(timeout=2)
        except Exception:
            pass
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass


if __name__ == "__main__":
    main()
