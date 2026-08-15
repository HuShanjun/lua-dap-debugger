"""DAP handshake through lua-runner (statically linked luadap, no luadap.dll)."""
import subprocess
import time
from pathlib import Path

from dap_client import DapClient

ROOT = Path(__file__).resolve().parents[2]
PORT = 18290
RUNNER = ROOT / "bin" / "lua-runner.exe"  # also try bin/Debug/
DEBUGEE = ROOT / "script" / "test" / "run_debugee_runner.lua"


def find_runner():
    for p in [ROOT / "bin" / "lua-runner.exe", ROOT / "bin" / "Debug" / "lua-runner.exe"]:
        if p.exists():
            return str(p)
    raise SystemExit("lua-runner not found; build target lua-runner first")


def main():
    runner = find_runner()
    proc = subprocess.Popen(
        [runner, "--host", "127.0.0.1", "--port", str(PORT), "--", str(DEBUGEE)],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    time.sleep(0.5)
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
        try:
            out, _ = proc.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
        assert "DEBUGEE_DONE" in out, out
        print("runner handshake ok")
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
