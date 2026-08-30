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
import tempfile
import shutil
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_PORT = 24680


def http_request(port, method, path, body=None, headers=None, timeout=3.0):
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
    for k, v in (headers or {}).items():
        req_lines.append(f"{k}: {v}".encode())
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


# ---------------------------------------------------------------------------
# Range-aware local file servers to exercise the chunked download engine.
# ---------------------------------------------------------------------------

def _make_file_server(payload, mode="normal", chunk_bits=""):
    """mode: 'normal' (full ranges + Content-Length), _
           'truncate' (server closes range connections early to simulate a dropped link),
           'nolength' (no Content-Length, forces the unknown-size path)."""
    payload = payload.encode("utf-8") if isinstance(payload, str) else payload

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def _do_get(self):
            if mode == "truncate":
                # Serve at most one range connection at a time, closing the others early.
                rng = self.headers.get("Range")
                if rng:
                    start = int(rng.split("bytes=")[1].split("-")[0])
                    data = payload[start:start + 5000]
                else:
                    data = payload[:5000]
                self.send_response(206 if rng else 200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Accept-Ranges", "bytes")
                if rng:
                    self.send_header("Content-Range", f"bytes {start}-{start + len(data) - 1}/{len(payload)}")
                    self.send_header("Content-Length", str(len(data)))
                else:
                    self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                try:
                    self.wfile.write(data)
                    self.wfile.flush()
                except BrokenPipeError:
                    pass
                return

            rng = self.headers.get("Range")
            if rng:
                start, end = rng.split("bytes=")[1].split("-")
                start = int(start)
                end = int(end) if end else len(payload) - 1
                end = min(end, len(payload) - 1)
                data = payload[start:end + 1]
                self.send_response(206)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Content-Range", f"bytes {start}-{end}/{len(payload)}")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return

            data = payload
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Accept-Ranges", "bytes")
            if mode != "nolength":
                self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_HEAD(self):
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Accept-Ranges", "bytes")
            if mode != "nolength":
                self.send_header("Content-Length", str(len(payload)))
            self.end_headers()

        def do_GET(self):
            self._do_get()

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, port


def find_download(js, mid):
    for d in js.get("downloads", []):
        if d.get("id") == mid:
            return d
    return None


def wait_download_status(port, mid, statuses, timeout=30.0):
    """Poll /api/downloads until the download reaches one of `statuses`."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        _, j = http_request(port, "GET", "/api/downloads")
        d = find_download(j, mid)
        last = d
        if d is not None and d.get("status") in statuses:
            return d
        time.sleep(0.5)
    return last


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

        # --- Origin/cors hardening ---
        st, _ = http_request(port, "GET", "/api/version", headers={"Origin": "https://evil.example.com"})
        check("disallowed Origin (website) -> 403", st == 403, f"status={st}")

        st, _ = http_request(port, "GET", "/api/version", headers={"Origin": "chrome-extension://abcdefghijklmnop"})
        check("chrome-extension Origin -> 200", st == 200, f"status={st}")

        st, _ = http_request(port, "GET", "/api/version", headers={"Origin": "moz-extension://abcdefghijklmnop"})
        check("moz-extension Origin -> 200", st == 200, f"status={st}")

        # --- Unsupported URL scheme rejection ---
        st, j = http_request(port, "POST", "/api/download", {"url": "file:///C:/Windows/notepad.exe"})
        check("POST /api/download file:// scheme -> 400", st == 400, f"status={st}")

        # --- OPTIONS preflight from allowed origin is answered with echoed origin ---
        st, _ = http_request(port, "OPTIONS", "/api/download", headers={"Origin": "chrome-extension://abcdefghijklmnop"})
        check("OPTIONS from allowed Origin -> 204", st == 204, f"status={st}")

        # --- Download-engine regression tests (chunked HTTP via a local server) ---
        chk = "DLCHUNK-REG" + ("x" * 262144)  # ~256KB, forces multi-chunk
        workdir = tempfile.mkdtemp(prefix="copper_it_")
        try:
            def add_download(url, name, path):
                st, j = http_request(port, "POST", "/api/download",
                                     {"url": url, "filename": name, "path": path})
                return st, j.get("id")

            # 1) Complete chunked download reaches Completed and is byte-exact.
            srv, sp = _make_file_server(chk, mode="normal")
            try:
                st, mid = add_download(f"http://127.0.0.1:{sp}/file.bin", "normal.bin", workdir)
                check("add chunked download -> 200", st == 200, f"status={st} mid={mid}")
                d = wait_download_status(port, mid, {"Completed", "Failed"})
                final_path = os.path.join(workdir, "normal.bin")
                ok = d is not None and d.get("status") == "Completed"
                if ok and os.path.isfile(final_path):
                    with open(final_path, "rb") as f:
                        ok = f.read() == chk.encode("utf-8")
                else:
                    ok = ok and os.path.isfile(final_path)
                check("chunked download completes byte-exact", ok,
                      f"status={d.get('status') if d else None} file={os.path.isfile(final_path)}")
            finally:
                srv.shutdown()

            # 2) Truncated range connections must NOT produce a corrupt "Completed" file.
            srv, sp = _make_file_server(chk, mode="truncate")
            try:
                st, mid = add_download(f"http://127.0.0.1:{sp}/file.bin", "trunc.bin", workdir)
                check("add truncated download -> 200", st == 200, f"status={st}")
                d = wait_download_status(port, mid, {"Completed", "Failed"})
                check("truncated download is not marked Completed",
                      d is not None and d.get("status") != "Completed", str(d))
            finally:
                srv.shutdown()

            # 3) Unknown-length (no Content-Length) single-GET completes at 100%.
            srv, sp = _make_file_server(chk, mode="nolength")
            try:
                st, mid = add_download(f"http://127.0.0.1:{sp}/file.bin", "nolength.bin", workdir)
                check("add nolength download -> 200", st == 200, f"status={st}")
                d = wait_download_status(port, mid, {"Completed", "Failed"})
                ok = d is not None and d.get("status") == "Completed"
                if ok and os.path.isfile(os.path.join(workdir, "nolength.bin")):
                    with open(os.path.join(workdir, "nolength.bin"), "rb") as f:
                        ok = f.read() == chk.encode("utf-8")
                check("unknown-length download completes byte-exact", ok, str(d))
            finally:
                srv.shutdown()
        finally:
            shutil.rmtree(workdir, ignore_errors=True)

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
