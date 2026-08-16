"""DAP threads lists main (id=1) plus a wrapped coroutine.create thread."""
import subprocess
import time
from pathlib import Path

from dap_client import DapClient

ROOT = Path(__file__).resolve().parents[1]
PORT = 18200
DEBUGEE = Path(__file__).resolve().parent / "run_debugee_coro_threads.lua"


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
    assert "coroutine.create" in src
    assert "dap.track" in src

    lua = find_lua()
    proc = subprocess.Popen(
        [lua, str(DEBUGEE), str(ROOT), str(PORT)],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    time.sleep(0.5)
    c = None
    try:
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

        read_until(proc, "CORO_READY", timeout=5.0)

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
        assert len(threads) >= 2, threads
        print("coro threads ok")
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
