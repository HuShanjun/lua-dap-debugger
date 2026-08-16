import re
import subprocess
import time
from pathlib import Path

from dap_client import DapClient

ROOT = Path(__file__).resolve().parents[2]
PORT = 18173
DEBUGEE = ROOT / "script" / "test" / "run_debugee.lua"


def find_lua():
    hits = list(ROOT.glob("**/lua.exe"))
    if not hits:
        raise SystemExit("lua.exe not found")
    return str(hits[0])


def breakpoint_line():
    text = DEBUGEE.read_text(encoding="utf-8")
    for i, line in enumerate(text.splitlines(), 1):
        if "local sum = x + y" in line:
            return i
    raise SystemExit("breakpoint line not found")


def main():
    lua = find_lua()
    line = breakpoint_line()
    src = str(DEBUGEE).replace("\\", "/")
    proc = subprocess.Popen(
        [lua, str(DEBUGEE), str(ROOT), str(PORT)],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    time.sleep(0.6)
    c = DapClient("127.0.0.1", PORT, timeout=5.0)
    try:
        c.send_request("initialize", {"adapterID": "lua-dap"})
        c.wait_for(lambda m: m.get("type") == "response" and m.get("command") == "initialize")
        c.wait_for(lambda m: m.get("event") == "initialized")
        c.send_request("attach", {})
        c.wait_for(lambda m: m.get("command") == "attach" and m.get("type") == "response")
        c.send_request("setBreakpoints", {
            "source": {"path": src},
            "breakpoints": [{"line": line}],
        })
        c.wait_for(lambda m: m.get("command") == "setBreakpoints" and m.get("type") == "response")
        c.send_request("configurationDone", {})
        c.wait_for(lambda m: m.get("command") == "configurationDone" and m.get("type") == "response")

        stopped = c.wait_for(lambda m: m.get("event") == "stopped")
        assert stopped["body"]["reason"] == "breakpoint"

        c.send_request("stackTrace", {"threadId": 1})
        st = c.wait_for(lambda m: m.get("command") == "stackTrace" and m.get("type") == "response")
        frames = st["body"]["stackFrames"]
        assert frames, st
        frame_id = frames[0]["id"]

        c.send_request("scopes", {"frameId": frame_id})
        sc = c.wait_for(lambda m: m.get("command") == "scopes" and m.get("type") == "response")
        locals_ref = sc["body"]["scopes"][0]["variablesReference"]

        c.send_request("variables", {"variablesReference": locals_ref})
        vr = c.wait_for(lambda m: m.get("command") == "variables" and m.get("type") == "response")
        names = {v["name"]: v for v in vr["body"]["variables"]}
        assert "x" in names and "y" in names and "player" in names, names
        assert names["player"]["variablesReference"] > 0
        c.send_request("variables", {"variablesReference": names["player"]["variablesReference"]})
        pr = c.wait_for(lambda m: m.get("command") == "variables" and m.get("type") == "response")
        pnames = {v["name"]: v for v in pr["body"]["variables"]}
        assert "name" in pnames and "stats" in pnames, pnames

        c.send_request("continue", {"threadId": 1})
        c.wait_for(lambda m: m.get("command") == "continue" and m.get("type") == "response")
        out, _ = proc.communicate(timeout=5)
        assert "DEBUGEE_DONE" in out, out
        print("breakpoint ok")
    finally:
        try:
            proc.kill()
        except Exception:
            pass
        c.close()


if __name__ == "__main__":
    main()
