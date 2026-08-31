"""Same-thread dual lua_State DAP: both mains, BP on A, empty stack on B."""
import socket
import subprocess
import time
from pathlib import Path

from dap_client import DapClient

ROOT = Path(__file__).resolve().parents[1]
PORT = 18210
PORT_MISMATCH = 18220
SCRIPT_A = Path(__file__).resolve().parent / "run_ms_a.lua"
SCRIPT_B = Path(__file__).resolve().parent / "run_ms_b.lua"


def find_host():
    for p in [
        ROOT / "bin" / "multi_state_dap_host.exe",
        ROOT / "bin" / "Debug" / "multi_state_dap_host.exe",
        ROOT / "bin" / "multi_state_dap_host",
    ]:
        if p.exists():
            return str(p)
    hits = list(ROOT.glob("**/multi_state_dap_host.exe"))
    if hits:
        return str(hits[0])
    raise SystemExit("multi_state_dap_host not found; build it first")


def bp_line(path, needle):
    text = path.read_text(encoding="utf-8")
    for i, line in enumerate(text.splitlines(), 1):
        if needle in line:
            return i
    raise SystemExit("breakpoint line not found in %s: %r" % (path, needle))


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
        "did not see %r in host output; got: %r" % (token, "".join(buf))
    )


def handshake(c):
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
        lambda m: m.get("type") == "response" and m.get("command") == "initialize",
        limit=40,
    )
    assert init_resp.get("success") is True, init_resp
    ev = c.wait_for(
        lambda m: m.get("type") == "event" and m.get("event") == "initialized",
        limit=40,
    )
    assert ev["event"] == "initialized"
    c.send_request("attach", {})
    c.wait_for(
        lambda m: m.get("type") == "response" and m.get("command") == "attach",
        limit=40,
    )


def test_same_thread_multi_state():
    host = find_host()
    line = bp_line(SCRIPT_A, "local x = 1")
    src = str(SCRIPT_A).replace("\\", "/")
    proc = subprocess.Popen(
        [host, "--port", str(PORT)],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    c = None
    try:
        read_until(proc, "listening on", timeout=8.0)
        c = DapClient("127.0.0.1", PORT, timeout=8.0)
        handshake(c)
        c.send_request("threads", {})
        resp = c.wait_for(
            lambda m: m.get("type") == "response" and m.get("command") == "threads",
            limit=40,
        )
        assert resp.get("success") is True, resp
        threads = (resp.get("body") or {}).get("threads") or []
        names = [t.get("name") for t in threads]
        assert any(n == "logic/main" for n in names), threads
        assert any(n == "ui/main" for n in names), threads
        logic = next(t for t in threads if t.get("name") == "logic/main")
        ui = next(t for t in threads if t.get("name") == "ui/main")
        assert logic.get("id") != ui.get("id"), threads

        c.send_request(
            "setBreakpoints",
            {"source": {"path": src}, "breakpoints": [{"line": line}]},
        )
        c.wait_for(
            lambda m: m.get("type") == "response"
            and m.get("command") == "setBreakpoints",
            limit=40,
        )
        c.send_request("configurationDone", {})
        c.wait_for(
            lambda m: m.get("type") == "response"
            and m.get("command") == "configurationDone",
            limit=40,
        )

        stopped = c.wait_for(
            lambda m: m.get("type") == "event" and m.get("event") == "stopped",
            limit=80,
        )
        assert stopped["body"]["reason"] == "breakpoint", stopped
        assert stopped["body"].get("threadId") == logic["id"], stopped

        c.send_request("stackTrace", {"threadId": ui["id"]})
        st_b = c.wait_for(
            lambda m: m.get("type") == "response" and m.get("command") == "stackTrace",
            limit=40,
        )
        assert st_b.get("success") is True, st_b
        frames_b = (st_b.get("body") or {}).get("stackFrames") or []
        assert frames_b == [], st_b

        c.send_request("stackTrace", {"threadId": logic["id"]})
        st_a = c.wait_for(
            lambda m: m.get("type") == "response" and m.get("command") == "stackTrace",
            limit=40,
        )
        assert st_a.get("success") is True, st_a
        frames_a = (st_a.get("body") or {}).get("stackFrames") or []
        assert frames_a, st_a

        # Clear while paused using a differently-cased path (VS Code / Win32
        # often disagree with Lua @source). Must not leave a stale BP entry.
        clear_src = src
        if clear_src[0].isalpha() and len(clear_src) > 1 and clear_src[1] == ":":
            clear_src = clear_src[0].swapcase() + clear_src[1:]
        clear_src = clear_src.replace("test/", "Test/", 1).replace("test\\", "Test\\", 1)
        c.send_request(
            "setBreakpoints",
            {"source": {"path": clear_src}, "breakpoints": []},
        )
        c.wait_for(
            lambda m: m.get("type") == "response"
            and m.get("command") == "setBreakpoints",
            limit=40,
        )

        c.send_request("continue", {"threadId": logic["id"]})
        cont = c.wait_for(
            lambda m: m.get("type") == "response" and m.get("command") == "continue",
            limit=40,
        )
        assert cont.get("success") is True, cont

        old_timeout = c.sock.gettimeout()
        c.sock.settimeout(0.15)
        try:
            deadline = time.time() + 0.8
            while time.time() < deadline:
                try:
                    msg = c.read_message()
                except (TimeoutError, socket.timeout, OSError):
                    continue
                if msg.get("type") == "event" and msg.get("event") == "stopped":
                    if (msg.get("body") or {}).get("reason") == "breakpoint":
                        raise AssertionError(
                            "hit breakpoint after clear (path casing?): %r" % (msg,)
                        )
        finally:
            c.sock.settimeout(old_timeout)
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


def test_port_mismatch():
    host = find_host()
    proc = subprocess.Popen(
        [host, "--port", str(PORT_MISMATCH), "--mismatch"],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        out, _ = proc.communicate(timeout=8)
        assert proc.returncode == 2, (proc.returncode, out)
        assert "FAIL_JOIN" in (out or ""), out
    finally:
        try:
            proc.kill()
        except Exception:
            pass


def main():
    src_a = SCRIPT_A.read_text(encoding="utf-8")
    src_b = SCRIPT_B.read_text(encoding="utf-8")
    assert 'package.path = ""' in src_a and 'package.path = ""' in src_b
    assert "bin/?.dll" in src_a and "bin/?.dll" in src_b
    assert 'require("luadap")' in src_a and 'require("luadap")' in src_b
    assert '"logic"' in src_a and '"ui"' in src_b

    test_port_mismatch()
    test_same_thread_multi_state()
    print("multi-state same-thread ok")


if __name__ == "__main__":
    main()
