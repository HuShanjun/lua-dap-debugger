"""DAP handshake through luadap.start(host, port, false) + update() loop."""
import subprocess
import time
from pathlib import Path

from dap_client import DapClient

ROOT = Path(__file__).resolve().parents[2]
PORT = 18181
DEBUGEE = ROOT / "script" / "test" / "run_debugee_luadap_nowait.lua"


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
    src = DEBUGEE.read_text(encoding="utf-8")
    assert 'package.path = ""' in src or "package.path = ''" in src, (
        "debugee must clear package.path (no disk lua-runtime)"
    )
    assert "lua-runtime.debugger" not in src, "debugee must use require('luadap') only"
    assert "bin/?.dll" in src
    assert "require(\"luadap\")" in src or "require('luadap')" in src
    assert "start(" in src and "false" in src
    assert "update()" in src

    lua = find_lua()
    proc = subprocess.Popen(
        [lua, str(DEBUGEE), str(ROOT), str(PORT)],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    try:
        # start(false) must return before any DAP client exists.
        prefix = read_until(proc, "START_RETURNED", timeout=5.0)
        assert "LISTEN_DONE" not in prefix, prefix

        c = DapClient("127.0.0.1", PORT, timeout=3.0)
        c.send_request(
            "initialize",
            {
                "adapterID": "lua-dap",
                "pathFormat": "path",
                "linesStartAt1": True,
                "columnsStartAt1": True,
            },
        )
        init_resp = c.wait_for(
            lambda m: m.get("type") == "response" and m.get("command") == "initialize"
        )
        assert init_resp.get("success") is True, init_resp
        ev = c.wait_for(lambda m: m.get("type") == "event" and m.get("event") == "initialized")
        assert ev["event"] == "initialized"
        c.send_request("attach", {})
        c.wait_for(lambda m: m.get("type") == "response" and m.get("command") == "attach")
        c.send_request("setExceptionBreakpoints", {"filters": []})
        c.wait_for(
            lambda m: m.get("type") == "response" and m.get("command") == "setExceptionBreakpoints"
        )
        c.send_request("configurationDone", {})
        c.wait_for(
            lambda m: m.get("type") == "response" and m.get("command") == "configurationDone"
        )
        out, _ = proc.communicate(timeout=8)
        combined = prefix + (out or "")
        assert "LISTEN_DONE" in combined, combined
        assert "DEBUGEE_DONE" in combined, combined
        print("luadap nowait ok")
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
