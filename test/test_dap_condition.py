"""Conditional breakpoint: stop only when expression is true."""
import subprocess
import time
from pathlib import Path

from dap_client import DapClient

ROOT = Path(__file__).resolve().parents[1]
PORT = 18175
DEBUGEE = Path(__file__).resolve().parent / "run_debugee_cond.lua"


def find_lua():
    hits = list(ROOT.glob("**/lua.exe"))
    if not hits:
        raise SystemExit("lua.exe not found")
    return str(hits[0])


def breakpoint_line():
    text = DEBUGEE.read_text(encoding="utf-8")
    for i, line in enumerate(text.splitlines(), 1):
        if "hits = hits + 1" in line:
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
        init = c.wait_for(lambda m: m.get("type") == "response" and m.get("command") == "initialize")
        assert init["body"].get("supportsConditionalBreakpoints") is True, init
        c.wait_for(lambda m: m.get("event") == "initialized")
        c.send_request("attach", {})
        c.wait_for(lambda m: m.get("command") == "attach" and m.get("type") == "response")
        c.send_request("setBreakpoints", {
            "source": {"path": src},
            "breakpoints": [{"line": line, "condition": "i == 3"}],
        })
        bp = c.wait_for(lambda m: m.get("command") == "setBreakpoints" and m.get("type") == "response")
        assert bp["body"]["breakpoints"][0].get("verified") is True

        c.send_request("configurationDone", {})
        c.wait_for(lambda m: m.get("command") == "configurationDone" and m.get("type") == "response")

        stopped = c.wait_for(lambda m: m.get("event") == "stopped")
        assert stopped["body"]["reason"] == "breakpoint"

        c.send_request("stackTrace", {"threadId": 1})
        st = c.wait_for(lambda m: m.get("command") == "stackTrace" and m.get("type") == "response")
        frame_id = st["body"]["stackFrames"][0]["id"]
        c.send_request("scopes", {"frameId": frame_id})
        sc = c.wait_for(lambda m: m.get("command") == "scopes" and m.get("type") == "response")
        locals_ref = sc["body"]["scopes"][0]["variablesReference"]
        c.send_request("variables", {"variablesReference": locals_ref})
        vr = c.wait_for(lambda m: m.get("command") == "variables" and m.get("type") == "response")
        names = {v["name"]: v for v in vr["body"]["variables"]}
        assert names["i"]["value"] == "3", names
        assert names["hits"]["value"] == "2", names  # line hook is pre-exec of hits=hits+1

        c.send_request("continue", {"threadId": 1})
        c.wait_for(lambda m: m.get("command") == "continue" and m.get("type") == "response")
        # Should not stop again (i==4,5 never satisfy i==3)
        out, _ = proc.communicate(timeout=5)
        assert "DEBUGEE_DONE" in out, out
        print("condition ok")
    finally:
        try:
            proc.kill()
        except Exception:
            pass
        c.close()


if __name__ == "__main__":
    main()
