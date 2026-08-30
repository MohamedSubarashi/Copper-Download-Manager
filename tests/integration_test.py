#!/usr/bin/env python3
"""Copper Download Manager - integration regression tests.

Launches the real application binary and exercises the local HTTP API and the
single-instance / argument-forwarding behavior. This is the most important
regression surface: startup, IPC, and URL forwarding.

Usage:
    python tests/integration_test.py [path/to/CopperDownloadManager.exe]

Environment:
    COPPER_EXE : path to the app executable (overrides the positional arg)
"""

import json
import os
import socket
import subprocess
import sys
import time

DEFAULT_PORT = 24680


def http_request(port, method, path, body=None, timeout=3.0):
    """Minimal raw-HTTP client (no external deps). Returns (status, parsed_json_or_text)."""
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    payload = body if body is not None else b""
    if isinstance(body, dict):
        payload = json.dumps(body).encode("utf-8")
    req_lines = [
        f"{method} {path} HTTP/1.1".encode(),
        f"Host: 127.0.0.1:{port}".encode(),
        b"Connection: close",
    ]
    if payload:
        req_lines.append(f"Content-Type: application/json".encode())
        req_lines.append(f"Content-Length: {len(payload)}".encode())
    request = b"\r\n".join(req_lines) + b"\r\n\r\n" + payload
    s.sendall(request)
    chunks = []
    while True:
        try:
            data = s.recv(65536)
            if not data:
                break
            chunks.append(data)
        except socket.timeout:
            break
    s.close()
    raw = b"".join(chunks)
    head, _, rest = raw.partition(b"\r\n\r\n")
    status_line = head.split(b"\r\n", 1)[0].decode("latin1", "replace")
    try:
        status = int(status_line.split(" ")[1])
    except (IndexError, ValueError):
        status = 0
    try:
        parsed = json.loads(rest.decode("utf-8"))
    except ValueError:
        parsed = rest.decode("utf-8", "replace")
    return status, parsed


def wait_for_ready(port, timeout=30.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            status, _ = http_request(port, "GET", "/api/version", timeout=1.0)
            if status == 200:
                return True
        except (OSError, socket.timeout):
            pass
        time.sleep(0.5)
    return False


def main():
    exe = os.environ.get("COPPER_EXE") or (sys.argv[1] if len(sys.argv) > 1 else None)
    if not exe or not os.path.isfile(exe):
        print("FATAL: pass the app executable path or set COPPER_EXE", file=sys.stderr)
        return 2

    # Launch from the exe's directory so the bundled Qt/runtime DLLs resolve.
    exe_dir = os.path.dirname(os.path.abspath(exe))

    def launch():
        return subprocess.Popen(
            [exe],
            cwd=exe_dir,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    port = DEFAULT_PORT
    passed = 0
    failed = 0

    def check(name, cond, detail=""):
        nonlocal passed, failed
        if cond:
            passed += 1
            print(f"  PASS  {name}")
        else:
            failed += 1
            print(f"  FAIL  {name}  {detail}")

    # --- Launch the app ---
    print("Launching:", exe)
    proc = launch()
    try:
        if not wait_for_ready(port, timeout=40):
            print("FATAL: app API did not become ready", file=sys.stderr)
            return 1
        print("  app API is ready")

        # --- /api/version ---
        st, j = http_request(port, "GET", "/api/version")
        check("GET /api/version -> 200", st == 200, f"status={st}")
        check("version reported", isinstance(j, dict) and j.get("version") not in (None, ""), str(j))

        # --- /api/ping ---
        st, j = http_request(port, "GET", "/api/ping")
        check("GET /api/ping -> 200", st == 200, f"status={st}")

        # --- /api/downloads ---
        st, j = http_request(port, "GET", "/api/downloads")
        check("GET /api/downloads -> 200", st == 200, f"status={st}")
        check("downloads is a list", isinstance(j.get("downloads"), list), str(j))

        # --- /api/forward (single-instance forwarding path) ---
        st, j = http_request(port, "POST", "/api/forward", {"argument": "show"})
        check("POST /api/forward show -> 200", st == 200, f"status={st}")
        check("forward success flag", isinstance(j, dict) and j.get("success") is True, str(j))

        # --- Single-instance: a second launch should forward and exit ---
        second = launch()
        try:
            rc = second.wait(timeout=20)
        except subprocess.TimeoutExpired:
            rc = "TIMEOUT"
            second.kill()
        check("second instance exits (single-instance)", rc in (0, 1), f"rc={rc}")

        # --- /api/download error handling (no URL) ---
        st, j = http_request(port, "POST", "/api/download", {"url": ""})
        check("POST /api/download empty url -> 400", st == 400, f"status={st}")

    finally:
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            proc.kill()

    print(f"\n{passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
