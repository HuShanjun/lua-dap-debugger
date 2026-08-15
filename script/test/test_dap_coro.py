"""DAP stopped.threadId is the coroutine that hit the breakpoint."""
import subprocess
import time
from pathlib import Path

from dap_client import DapClient

ROOT = Path(__file__).resolve().parents[2]
PORT = 18201
DEBUGEE = ROOT / "script" / "test" / "run_debugee_coro.lua"


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
        if "local sum = a + b" in line:
            return i
    raise SystemExit("breakpoint line not found")


def main():
    src_text = DEBUGEE.read_text(encoding="utf-8")
    assert 'package.path = ""' in src_text or "package.path = ''" in src_text
    assert "lua-runtime.debugger" not in src_text
    assert "bin/?.dll" in src_text
    assert "require(\"luadap\")" in src_text or "require('luadap')" in src_text
    assert "coroutine.create" in src_text

    lua = find_lua()
    line = breakpoint_line()
    src = str(DEBUGEE).replace("\\", "/")
    proc = subprocess.Popen(
        [lua, str(DEBUGEE), str(ROOT), str(PORT)],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    time.sleep(0.6)
    c = None
    try:
        c = DapClient("127.0.0.1", PORT, timeout=5.0)
        c.send_request(
            "initialize",
            {
                "adapterID": "lua-dap",
                "pathFormat": "path",
                "linesStartAt1": True,
                "columnsStartAt1": True,
            },
        )
        c.wait_for(
            lambda m: m.get("type") == "response" and m.get("command") == "initialize"
        )
        c.wait_for(lambda m: m.get("type") == "event" and m.get("event") == "initialized")
        c.send_request("attach", {})
        c.wait_for(lambda m: m.get("type") == "response" and m.get("command") == "attach")
        c.send_request(
            "setBreakpoints",
            {
                "source": {"path": src},
                "breakpoints": [{"line": line}],
            },
        )
        c.wait_for(
            lambda m: m.get("type") == "response" and m.get("command") == "setBreakpoints"
        )
        c.send_request("configurationDone", {})
        c.wait_for(
            lambda m: m.get("type") == "response" and m.get("command") == "configurationDone"
        )

        stopped = c.wait_for(lambda m: m.get("event") == "stopped")
        body = stopped.get("body") or {}
        tid = body.get("threadId")
        assert tid is not None and tid != 1, stopped
        assert tid >= 2, stopped
        assert body.get("allThreadsStopped") is False, stopped
        assert body.get("reason") == "breakpoint", stopped

        c.send_request("threads", {})
        resp = c.wait_for(
            lambda m: m.get("type") == "response" and m.get("command") == "threads"
        )
        assert resp.get("success") is True, resp
        threads = (resp.get("body") or {}).get("threads") or []
        ids = [t.get("id") for t in threads]
        names = [t.get("name") for t in threads]
        assert 1 in ids, threads
        assert "main" in names, threads
        assert tid in ids, threads

        c.send_request("continue", {"threadId": tid})
        c.wait_for(
            lambda m: m.get("type") == "response" and m.get("command") == "continue"
        )
        out, _ = proc.communicate(timeout=5)
        assert "DEBUGEE_DONE" in out, out
        print("coro stopped ok")
    finally:
        try:
            proc.kill()
        except Exception:
            pass
        if c is not None:
            try:
                c.close()
            except Exception:
                pass


if __name__ == "__main__":
    main()
