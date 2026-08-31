"""Cross-thread dual lua_State DAP: A paused while B heartbeats, then dual stopped."""
import subprocess
import threading
import time
from pathlib import Path

from dap_client import DapClient
from test_dap_multi_state import (
    SCRIPT_A,
    SCRIPT_B,
    bp_line,
    find_host,
    handshake,
    read_until,
)

ROOT = Path(__file__).resolve().parents[1]
PORT = 18230
HEART_PATH = ROOT / "ms_heart_b.txt"


def drain_stdout(proc):
    try:
        for _ in proc.stdout:
            pass
    except Exception:
        pass


def read_heart():
    for _ in range(8):
        try:
            text = HEART_PATH.read_text(encoding="utf-8").strip()
            if text:
                return int(text.split()[0])
        except (OSError, ValueError):
            pass
        time.sleep(0.05)
    return 0


def wait_cmd(c, command, limit=40):
    return c.wait_for(
        lambda m: m.get("type") == "response" and m.get("command") == command,
        limit=limit,
    )


def inspect_paused(c, thread_id, expect_name, expect_value):
    """stackTrace + scopes + variables (no threadId on variables, VS Code style)."""
    c.send_request("stackTrace", {"threadId": thread_id})
    st = wait_cmd(c, "stackTrace")
    assert st.get("success") is True, st
    frames = (st.get("body") or {}).get("stackFrames") or []
    assert frames, st
    frame_id = frames[0]["id"]
    c.send_request("scopes", {"frameId": frame_id})
    sc = wait_cmd(c, "scopes")
    assert sc.get("success") is True, sc
    scopes = (sc.get("body") or {}).get("scopes") or []
    assert scopes, sc
    locals_ref = scopes[0].get("variablesReference")
    c.send_request("variables", {"variablesReference": locals_ref})
    vr = wait_cmd(c, "variables")
    assert vr.get("success") is True, vr
    variables = (vr.get("body") or {}).get("variables") or []
    names = {v.get("name"): v for v in variables}
    assert expect_name in names, variables
    got = names[expect_name].get("value")
    assert str(got) == str(expect_value), (expect_name, got, expect_value)
    if "t" in names:
        tref = names["t"].get("variablesReference") or 0
        if tref:
            c.send_request("variables", {"variablesReference": tref})
            nested = wait_cmd(c, "variables")
            assert nested.get("success") is True, nested
            nvars = (nested.get("body") or {}).get("variables") or []
            nnames = {v.get("name"): v for v in nvars}
            assert "v" in nnames, nvars
            assert str(nnames["v"].get("value")) == str(expect_value), nnames["v"]
    return names


def main():
    host = find_host()
    line_a = bp_line(SCRIPT_A, "return x")
    line_b = bp_line(SCRIPT_B, "return y")
    src_a = str(SCRIPT_A).replace("\\", "/")
    src_b = str(SCRIPT_B).replace("\\", "/")
    try:
        HEART_PATH.unlink()
    except OSError:
        pass

    proc = subprocess.Popen(
        [host, "--port", str(PORT), "--mt"],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    c = None
    try:
        read_until(proc, "listening on", timeout=8.0)
        threading.Thread(target=drain_stdout, args=(proc,), daemon=True).start()

        c = DapClient("127.0.0.1", PORT, timeout=8.0)
        handshake(c)

        c.send_request("threads", {})
        resp = wait_cmd(c, "threads")
        assert resp.get("success") is True, resp
        threads = (resp.get("body") or {}).get("threads") or []
        logic = next(t for t in threads if t.get("name") == "logic/main")
        ui = next(t for t in threads if t.get("name") == "ui/main")
        assert logic.get("id") != ui.get("id"), threads

        c.send_request(
            "setBreakpoints",
            {"source": {"path": src_a}, "breakpoints": [{"line": line_a}]},
        )
        wait_cmd(c, "setBreakpoints")
        # B armed but inert until isolation is proven (condition false).
        c.send_request(
            "setBreakpoints",
            {
                "source": {"path": src_b},
                "breakpoints": [{"line": line_b, "condition": "false"}],
            },
        )
        wait_cmd(c, "setBreakpoints")
        c.send_request("configurationDone", {})
        wait_cmd(c, "configurationDone")

        stopped_a = c.wait_for(
            lambda m: m.get("type") == "event" and m.get("event") == "stopped",
            limit=80,
        )
        assert stopped_a["body"]["reason"] == "breakpoint", stopped_a
        assert stopped_a["body"].get("threadId") == logic["id"], stopped_a
        assert not stopped_a["body"].get("allThreadsStopped"), stopped_a

        h1 = read_heart()
        time.sleep(0.4)
        h2 = read_heart()
        assert h2 > h1, "B heartbeat must increase while A is paused (%s -> %s)" % (
            h1,
            h2,
        )

        inspect_paused(c, logic["id"], "x", "1")

        c.send_request(
            "setBreakpoints",
            {"source": {"path": src_b}, "breakpoints": [{"line": line_b}]},
        )
        wait_cmd(c, "setBreakpoints")

        stopped_b = c.wait_for(
            lambda m: m.get("type") == "event"
            and m.get("event") == "stopped"
            and (m.get("body") or {}).get("threadId") == ui["id"],
            limit=80,
        )
        assert stopped_b["body"]["reason"] == "breakpoint", stopped_b
        assert not stopped_b["body"].get("allThreadsStopped"), stopped_b

        c.send_request("threads", {})
        resp2 = wait_cmd(c, "threads")
        names = [t.get("name") for t in ((resp2.get("body") or {}).get("threads") or [])]
        assert "logic/main" in names and "ui/main" in names, resp2

        inspect_paused(c, logic["id"], "x", "1")
        inspect_paused(c, ui["id"], "y", "2")

        c.send_request("continue", {"threadId": logic["id"]})
        cont_a = wait_cmd(c, "continue")
        assert cont_a.get("success") is True, cont_a
        assert not (cont_a.get("body") or {}).get("allThreadsContinued"), cont_a

        c.send_request("continue", {"threadId": ui["id"]})
        cont_b = wait_cmd(c, "continue")
        assert cont_b.get("success") is True, cont_b
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
        try:
            HEART_PATH.unlink()
        except OSError:
            pass
    print("multi-state cross-thread ok")


if __name__ == "__main__":
    main()
