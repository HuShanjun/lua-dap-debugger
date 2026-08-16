import subprocess
import time
from pathlib import Path

from dap_client import DapClient

ROOT = Path(__file__).resolve().parents[1]
DEBUGEE = Path(__file__).resolve().parent / "run_debugee.lua"


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


def breakpoint_line():
    text = DEBUGEE.read_text(encoding="utf-8")
    for i, line in enumerate(text.splitlines(), 1):
        if "local sum = x + y" in line:
            return i
    raise SystemExit("breakpoint line not found")


def start_debugee(lua, port):
    return subprocess.Popen(
        [lua, str(DEBUGEE), str(ROOT), str(port)],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def handshake_to_config(c, src=None, line=None):
    c.send_request("initialize", {"adapterID": "lua-dap"})
    c.wait_for(lambda m: m.get("type") == "response" and m.get("command") == "initialize")
    c.wait_for(lambda m: m.get("event") == "initialized")
    c.send_request("attach", {})
    c.wait_for(lambda m: m.get("command") == "attach" and m.get("type") == "response")
    if src is not None and line is not None:
        c.send_request("setBreakpoints", {
            "source": {"path": src},
            "breakpoints": [{"line": line}],
        })
        c.wait_for(lambda m: m.get("command") == "setBreakpoints" and m.get("type") == "response")
    c.send_request("configurationDone", {})
    c.wait_for(lambda m: m.get("command") == "configurationDone" and m.get("type") == "response")


def test_pause_disconnect(lua):
    port = 18175
    src = str(DEBUGEE).replace("\\", "/")
    line = breakpoint_line()
    proc = start_debugee(lua, port)
    time.sleep(0.6)
    c = DapClient("127.0.0.1", port, timeout=5.0)
    try:
        handshake_to_config(c, src, line)
        stopped = c.wait_for(lambda m: m.get("event") == "stopped")
        assert stopped["body"]["reason"] == "breakpoint"
        c.send_request("disconnect", {})
        c.wait_for(lambda m: m.get("type") == "response" and m.get("command") == "disconnect")
        term = c.wait_for(lambda m: m.get("event") == "terminated")
        assert term["event"] == "terminated"
        out, _ = proc.communicate(timeout=5)
        assert "DEBUGEE_DONE" in out, out
        assert "stack traceback" not in out, out
        print("pause disconnect ok")
    finally:
        try:
            proc.kill()
        except Exception:
            pass
        c.close()


def test_pause_client_drop(lua):
    port = 18176
    src = str(DEBUGEE).replace("\\", "/")
    line = breakpoint_line()
    proc = start_debugee(lua, port)
    time.sleep(0.6)
    c = DapClient("127.0.0.1", port, timeout=5.0)
    try:
        handshake_to_config(c, src, line)
        stopped = c.wait_for(lambda m: m.get("event") == "stopped")
        assert stopped["body"]["reason"] == "breakpoint"
        c.close()
        out, _ = proc.communicate(timeout=5)
        assert "DEBUGEE_DONE" in out, out
        assert "stack traceback" not in out, out
        print("pause client drop ok")
    finally:
        try:
            proc.kill()
        except Exception:
            pass
        try:
            c.close()
        except Exception:
            pass


def test_handshake_disconnect(lua):
    port = 18177
    script = f"""
package.path = ""
package.cpath = [[{ROOT.as_posix()}/bin/?.dll;{ROOT.as_posix()}/bin/Debug/?.dll;]]
local dap = require("luadap")
dap.start("127.0.0.1", {port}, true)
print("LISTEN_DONE")
print("DEBUGEE_DONE")
"""
    proc = subprocess.Popen(
        [lua, "-e", script],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    time.sleep(0.5)
    c = DapClient("127.0.0.1", port, timeout=3.0)
    try:
        c.send_request("initialize", {"adapterID": "lua-dap"})
        c.wait_for(lambda m: m.get("type") == "response" and m.get("command") == "initialize")
        c.wait_for(lambda m: m.get("event") == "initialized")
        c.send_request("disconnect", {})
        c.wait_for(lambda m: m.get("type") == "response" and m.get("command") == "disconnect")
        c.wait_for(lambda m: m.get("event") == "terminated")
        out, _ = proc.communicate(timeout=3)
        assert "LISTEN_DONE" in out, out
        assert "DEBUGEE_DONE" in out, out
        assert "stack traceback" not in out, out
        print("handshake disconnect ok")
    finally:
        try:
            proc.kill()
        except Exception:
            pass
        c.close()


def main():
    lua = find_lua()
    test_pause_disconnect(lua)
    test_pause_client_drop(lua)
    test_handshake_disconnect(lua)


if __name__ == "__main__":
    main()
