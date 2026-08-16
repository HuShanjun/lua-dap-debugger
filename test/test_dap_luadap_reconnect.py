"""After DAP client disconnect, the same listen port must accept a second attach."""
import subprocess
import time
from pathlib import Path

from dap_client import DapClient

ROOT = Path(__file__).resolve().parents[2]
PORT = 18210
DEBUGEE = ROOT / "script" / "test" / "run_debugee_luadap_reconnect.lua"


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
        "did not see %r; got: %r" % (token, "".join(buf))
    )


def handshake(c):
    c.send_request("initialize", {"adapterID": "lua-dap"})
    c.wait_for(lambda m: m.get("type") == "response" and m.get("command") == "initialize")
    c.wait_for(lambda m: m.get("event") == "initialized")
    c.send_request("attach", {})
    c.wait_for(lambda m: m.get("command") == "attach" and m.get("type") == "response")
    c.send_request("configurationDone", {})
    c.wait_for(
        lambda m: m.get("command") == "configurationDone" and m.get("type") == "response"
    )


def main():
    lua = find_lua()
    proc = subprocess.Popen(
        [lua, str(DEBUGEE), str(ROOT), str(PORT)],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        read_until(proc, "START_RETURNED", 5.0)
        read_until(proc, "UPDATE_LOOP", 5.0)
        time.sleep(0.2)

        c1 = DapClient("127.0.0.1", PORT, timeout=3.0)
        handshake(c1)
        c1.send_request("disconnect", {})
        c1.wait_for(
            lambda m: m.get("type") == "response" and m.get("command") == "disconnect"
        )
        c1.wait_for(lambda m: m.get("event") == "terminated")
        c1.close()

        # Host must still be listening — second F5 / attach.
        time.sleep(0.3)
        c2 = DapClient("127.0.0.1", PORT, timeout=3.0)
        handshake(c2)
        c2.send_request("disconnect", {})
        c2.wait_for(
            lambda m: m.get("type") == "response" and m.get("command") == "disconnect"
        )
        c2.close()
        print("luadap reconnect ok")
    finally:
        try:
            proc.kill()
        except Exception:
            pass
        try:
            proc.wait(timeout=2)
        except Exception:
            pass


if __name__ == "__main__":
    main()
