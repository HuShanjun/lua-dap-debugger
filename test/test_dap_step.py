import subprocess
import time
from pathlib import Path

from dap_client import DapClient

ROOT = Path(__file__).resolve().parents[1]
PORT = 18174
DEBUGEE = Path(__file__).resolve().parent / "run_debugee_step.lua"


def find_lua():
    hits = list(ROOT.glob("**/lua.exe"))
    if not hits:
        raise SystemExit("lua.exe not found")
    return str(hits[0])


def find_line(needle):
    text = DEBUGEE.read_text(encoding="utf-8")
    for i, line in enumerate(text.splitlines(), 1):
        if needle in line:
            return i
    raise SystemExit(f"line not found: {needle}")


def stack_top(c):
    c.send_request("stackTrace", {"threadId": 1})
    st = c.wait_for(lambda m: m.get("command") == "stackTrace" and m.get("type") == "response")
    frames = st["body"]["stackFrames"]
    assert frames, st
    return frames[0]


def main():
    lua = find_lua()
    a_line = find_line("local a = 1")
    b_line = find_line("local b = inner()")
    z_line = find_line("local z = 42")
    c_line = find_line("local c = b + 1")
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
            "breakpoints": [{"line": a_line}],
        })
        c.wait_for(lambda m: m.get("command") == "setBreakpoints" and m.get("type") == "response")
        c.send_request("configurationDone", {})
        c.wait_for(lambda m: m.get("command") == "configurationDone" and m.get("type") == "response")

        stopped = c.wait_for(lambda m: m.get("event") == "stopped")
        assert stopped["body"]["reason"] == "breakpoint"
        top = stack_top(c)
        assert top["line"] == a_line, top

        c.send_request("next", {"threadId": 1})
        c.wait_for(lambda m: m.get("command") == "next" and m.get("type") == "response")
        stopped = c.wait_for(lambda m: m.get("event") == "stopped")
        assert stopped["body"]["reason"] == "step", stopped
        top = stack_top(c)
        assert top["line"] == b_line, top
        assert top["name"] == "work", top

        c.send_request("stepIn", {"threadId": 1})
        c.wait_for(lambda m: m.get("command") == "stepIn" and m.get("type") == "response")
        stopped = c.wait_for(lambda m: m.get("event") == "stopped")
        assert stopped["body"]["reason"] == "step", stopped
        top = stack_top(c)
        assert top["line"] == z_line, top
        assert top["name"] == "inner", top

        c.send_request("stepOut", {"threadId": 1})
        c.wait_for(lambda m: m.get("command") == "stepOut" and m.get("type") == "response")
        stopped = c.wait_for(lambda m: m.get("event") == "stopped")
        assert stopped["body"]["reason"] == "step", stopped
        top = stack_top(c)
        assert top["line"] == c_line, top
        assert top["name"] == "work", top

        c.send_request("continue", {"threadId": 1})
        c.wait_for(lambda m: m.get("command") == "continue" and m.get("type") == "response")
        out, _ = proc.communicate(timeout=5)
        assert "DEBUGEE_DONE" in out, out
        print("step ok")
    finally:
        try:
            proc.kill()
        except Exception:
            pass
        c.close()


if __name__ == "__main__":
    main()
