"""DAP handshake through lua-runner (statically linked luadap, no luadap.dll)."""
import subprocess
import time
from pathlib import Path

from dap_client import DapClient

ROOT = Path(__file__).resolve().parents[2]
PORT = 18290
DEBUGEE = ROOT / "script" / "test" / "run_debugee_runner.lua"


def find_runner():
    for p in [ROOT / "bin" / "lua-runner.exe", ROOT / "bin" / "Debug" / "lua-runner.exe"]:
        if p.exists():
            return str(p)
    raise SystemExit("lua-runner not found; build target lua-runner first")


def wait_for_dap_client(host, port, timeout=15.0):
    """Retry TCP connect until the runner listen socket accepts (CI cold start)."""
    deadline = time.monotonic() + timeout
    last_err = None
    while time.monotonic() < deadline:
        try:
            client = DapClient(host, port, timeout=0.2)
            client.sock.settimeout(3.0)
            return client
        except OSError as e:
            last_err = e
            time.sleep(0.05)
    raise TimeoutError(f"DAP port {port} never accepted a connection: {last_err}")


def main():
    runner = find_runner()
    proc = subprocess.Popen(
        [runner, "--host", "127.0.0.1", "--port", str(PORT), "--", str(DEBUGEE)],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    c = None
    try:
        c = wait_for_dap_client("127.0.0.1", PORT)
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
        c.close()
        c = None
        try:
            out, _ = proc.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
            raise AssertionError(
                "runner did not exit after DAP client disconnect:\n" + (out or "")
            )
        assert proc.returncode == 0, (proc.returncode, out)
        assert "DEBUGEE_DONE" in out, out
        print("runner handshake ok")
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
