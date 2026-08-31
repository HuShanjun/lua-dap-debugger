#!/usr/bin/env python3
"""Run all test/test_*.py (and optional C smoke binaries) from repo root."""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEST_DIR = Path(__file__).resolve().parent
BIN = ROOT / "bin"

# Stable order (asyncsocket → core DAP → multi-state last).
ORDER = [
    "test_asyncsocket_smoke.py",
    "test_asyncsocket_multi.py",
    "test_asyncsocket_connect.py",
    "test_dap_luadap_handshake.py",
    "test_dap_luadap_nowait.py",
    "test_dap_luadap_reconnect.py",
    "test_dap_handshake.py",
    "test_dap_breakpoint.py",
    "test_dap_step.py",
    "test_dap_disconnect.py",
    "test_dap_partial_frame.py",
    "test_dap_condition.py",
    "test_dap_evaluate.py",
    "test_dap_table_cycle.py",
    "test_dap_coro_threads.py",
    "test_dap_coro.py",
    "test_dap_runner_handshake.py",
    "test_dap_multi_state.py",
    "test_dap_multi_state_mt.py",
]

C_STEMS = [
    "coro_registry_test",
    "circle_buffer_test",
]


def list_c_tests() -> list[Path]:
    out: list[Path] = []
    for stem in C_STEMS:
        candidates = [
            BIN / f"{stem}.exe",
            BIN / stem,
        ]
        for p in candidates:
            if p.exists():
                out.append(p)
                break
    return out


def list_python_tests(only: str | None) -> list[Path]:
    by_name = {p.name: p for p in TEST_DIR.glob("test_*.py")}
    ordered: list[Path] = []
    for name in ORDER:
        if name in by_name:
            ordered.append(by_name.pop(name))
    # Any new test_*.py not in ORDER — append alphabetically.
    ordered.extend(sorted(by_name.values(), key=lambda p: p.name))
    if only:
        key = only.lower()
        ordered = [p for p in ordered if key in p.name.lower()]
    return ordered


def run_one(cmd: list[str], label: str, env: dict) -> None:
    print(f"\n=== {label} ===", flush=True)
    r = subprocess.run(cmd, cwd=str(ROOT), env=env)
    if r.returncode != 0:
        raise SystemExit(f"FAILED: {label} (exit {r.returncode})")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--skip-c", action="store_true", help="skip C executables in bin/")
    ap.add_argument(
        "--only",
        metavar="SUBSTR",
        help="only Python tests whose filename contains SUBSTR (e.g. dap, asyncsocket)",
    )
    ap.add_argument("--list", action="store_true", help="print planned tests and exit")
    args = ap.parse_args()

    py_tests = list_python_tests(args.only)
    c_bins: list[Path] = []
    if not args.skip_c and not args.only:
        c_bins = list_c_tests()

    if args.list:
        for p in py_tests:
            print(p.relative_to(ROOT).as_posix())
        for p in c_bins:
            print(p.relative_to(ROOT).as_posix())
        return

    if not py_tests and not c_bins:
        raise SystemExit("no tests selected")

    env = os.environ.copy()
    env["PATH"] = str(BIN) + os.pathsep + env.get("PATH", "")

    for p in py_tests:
        run_one([sys.executable, str(p)], p.name, env)
    for p in c_bins:
        run_one([str(p)], p.name, env)

    print(
        f"\nALL OK: {len(py_tests)} Python + {len(c_bins)} C",
        flush=True,
    )


if __name__ == "__main__":
    main()
