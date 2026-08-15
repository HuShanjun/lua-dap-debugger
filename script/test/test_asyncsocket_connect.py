"""asyncsocket connect: Lua listen + Lua connect, ping/pong via pump."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PORT = 18223
SCRIPT = Path(__file__).resolve().parent / "run_asyncsocket_connect.lua"


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


def main():
    lua = find_lua()
    proc = subprocess.run(
        [lua, str(SCRIPT), str(ROOT), "127.0.0.1", str(PORT)],
        cwd=str(ROOT),
        capture_output=True,
        text=True,
        timeout=20,
    )
    out = (proc.stdout or "") + (proc.stderr or "")
    assert proc.returncode == 0, out
    assert "LISTENING" in out, out
    assert "ACCEPT" in out, out
    assert "OPEN" in out, out
    assert "PING" in out, out
    assert "PONG" in out, out
    assert "DONE" in out, out
    print("asyncsocket connect ok")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(exc, file=sys.stderr)
        sys.exit(1)
