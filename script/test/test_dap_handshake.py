import subprocess
import sys
import time
from pathlib import Path

from dap_client import DapClient

ROOT = Path(__file__).resolve().parents[2]
PORT = 18172


def find_lua():
    for p in [
        ROOT / "bin" / "lua.exe",
        ROOT / "bin" / "Debug" / "lua.exe",
    ]:
        if p.exists():
            return str(p)
    # fallback: search build
    hits = list(ROOT.glob("**/lua.exe"))
    if hits:
        return str(hits[0])
    raise SystemExit("lua.exe not found; build the project first")


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
    try:
        c = DapClient("127.0.0.1", PORT, timeout=3.0)
        c.send_request("initialize", {"adapterID": "lua-dap", "pathFormat": "path", "linesStartAt1": True, "columnsStartAt1": True})
        init_resp = c.wait_for(lambda m: m.get("type") == "response" and m.get("command") == "initialize")
        assert init_resp.get("success") is True, init_resp
        ev = c.wait_for(lambda m: m.get("type") == "event" and m.get("event") == "initialized")
        assert ev["event"] == "initialized"
        c.send_request("attach", {})
        c.wait_for(lambda m: m.get("type") == "response" and m.get("command") == "attach")
        c.send_request("setExceptionBreakpoints", {"filters": []})
        c.wait_for(lambda m: m.get("type") == "response" and m.get("command") == "setExceptionBreakpoints")
        c.send_request("configurationDone", {})
        c.wait_for(lambda m: m.get("type") == "response" and m.get("command") == "configurationDone")
        # listen should return and print
        out, _ = proc.communicate(timeout=3)
        assert "LISTEN_DONE" in out, out
        print("handshake ok")
    finally:
        try:
            proc.kill()
        except Exception:
            pass
        try:
            c.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
