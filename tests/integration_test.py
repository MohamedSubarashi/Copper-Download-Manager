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
import sqlite3
import subprocess
import sys
import tempfile
import shutil
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import quote

DEFAULT_PORT = 24680


# Install-scoped local API token (0.4.0). Read from the app's host config
# once the API is up and attached to every request by default; tests that
# probe the auth boundary pass with_token=False or a custom header instead.
API_TOKEN = ""


def http_request(port, method, path, body=None, headers=None, timeout=3.0, with_token=True):
    """Minimal raw-HTTP client (no external deps). Returns (status, parsed_json_or_text)."""
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
    if with_token and API_TOKEN and not any(
            k.lower() == "x-copper-token" for k in (headers or {})):
        req_lines.append(f"X-Copper-Token: {API_TOKEN}".encode())
    if payload:
        req_lines.append(f"Content-Type: application/json".encode())
        req_lines.append(f"Content-Length: {len(payload)}".encode())
    request = b"\r\n".join(req_lines) + b"\r\n\r\n" + payload
    chunks = []
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        try:
            s.sendall(request)
            while True:
                try:
                    data = s.recv(65536)
                    if not data:
                        break
                    chunks.append(data)
                except socket.timeout:
                    break
        finally:
            s.close()
    except OSError:
        # Connection refused/reset: the app died mid-run (crash). Degrade to
        # status 0 so assertions fail cleanly instead of raising a traceback;
        # the suite's process exit-code check then reports the crash itself.
        return 0, ""
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
            # 401 counts as ready: before the token is loaded our probe is
            # unauthenticated and the 0.4.0 API answers 401 - which still
            # proves this suite's instance owns the port. The token itself is
            # read from the host config right after readiness.
            if status in (200, 401):
                return True
        except (OSError, socket.timeout):
            pass
        time.sleep(0.5)
    return False


def load_api_token(profile_root, timeout=10.0):
    """Read the app's install-scoped API token from its host config.

    The app mints the token (and atomically writes copper_host_config.json)
    while bringing the API up, so poll briefly for it instead of failing on a
    write that races our first request.
    """
    path = os.path.join(profile_root, "local", "Copper", "copper_host_config.json")
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "r", encoding="utf-8") as f:
                cfg = json.load(f)
            token = cfg.get("apiToken") or ""
            if token:
                return token
        except (OSError, ValueError):
            pass
        time.sleep(0.3)
    return ""


def _pick_free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


# ---------------------------------------------------------------------------
# Range-aware local file servers to exercise the chunked download engine.
# ---------------------------------------------------------------------------

def _make_file_server(payload, mode="normal", chunk_bits="", record=None):
    """mode: 'normal' (full ranges + Content-Length), _
           'truncate' (server closes range connections early to simulate a dropped link),
           'nolength' (no Content-Length, forces the unknown-size path),
           'slow' (full ranges but trickle data so a download can be interrupted mid-way),
           'notfound' (HEAD and GET answer 404, to exercise failure classification).
       record: optional list; the value of the X-Copper-Test request header is
           appended for every HEAD/GET served (custom-header end-to-end check)."""
    payload = payload.encode("utf-8") if isinstance(payload, str) else payload

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def _record(self):
            if record is not None:
                record.append(self.headers.get("X-Copper-Test"))

        def _do_get(self):
            self._record()
            if mode == "notfound":
                self.send_response(404)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
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
                if mode == "slow":
                    # Trickle so a test can observe partial progress and interrupt.
                    # Slice small + sleep long enough that even 16 parallel range
                    # connections finish over several seconds on fast machines.
                    for i in range(0, len(data), 4096):
                        self.wfile.write(data[i:i + 4096])
                        self.wfile.flush()
                        time.sleep(0.25)
                else:
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
            self._record()
            if mode == "notfound":
                self.send_response(404)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
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


def _make_file_server_from_path(path, content_type="application/octet-stream"):
    """Serve the raw bytes of an existing file (used to serve a .torrent file)."""
    with open(path, "rb") as f:
        payload = f.read()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, port


def dl_rows(js):
    """The downloads array of an /api/downloads body. Tolerates a non-JSON
    body (what the raw client returns when the connection is cut because the
    app died mid-run) so one bad poll degrades to a timeout, not a traceback."""
    return js.get("downloads", []) if isinstance(js, dict) else []


def find_download(js, mid):
    for d in dl_rows(js):
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


def _nm_frame(obj):
    body = json.dumps(obj, separators=(",", ":")).encode("utf-8")
    return len(body).to_bytes(4, "little") + body


def _nm_read(stream):
    hdr = stream.read(4)
    if not hdr or len(hdr) < 4:
        return None
    length = int.from_bytes(hdr, "little")
    body = stream.read(length)
    try:
        return json.loads(body.decode("utf-8"))
    except Exception:
        return None


def run_native_host(host_path, message, timeout=20.0, env=None):
    """Run copper_native_host.exe with a native-messaging framed message on
    stdin and return its framed response (dict) or None on failure/timeout."""
    p = subprocess.Popen(
        [host_path],
        cwd=os.path.dirname(os.path.abspath(host_path)),
        env=env,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        bufsize=0,
    )
    try:
        p.stdin.write(_nm_frame(message))
        p.stdin.flush()
        p.stdin.close()
        reply = _nm_read(p.stdout)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill()
        p.stdout.close()
        return reply
    except Exception:
        try:
            p.kill()
        except Exception:
            pass
        return None


def main():
    exe = os.environ.get("COPPER_EXE") or (sys.argv[1] if len(sys.argv) > 1 else None)
    if not exe or not os.path.isfile(exe):
        print("FATAL: pass the app executable path or set COPPER_EXE", file=sys.stderr)
        return 2

    # Launch from the exe's directory so the bundled Qt/runtime DLLs resolve.
    exe_dir = os.path.dirname(os.path.abspath(exe))

    # Isolated profile: every run gets its own APPDATA/LOCALAPPDATA (inherited
    # by every spawned process, including the native host) plus --test-profile,
    # which redirects Qt's QStandardPaths to test-mode dirs before Logger/DB
    # first touch them. Without this the suite runs against the developer's
    # real profile: persisted settings from older releases (e.g. a baked-in
    # User-Agent), a cached yt-dlp binary (turning "fails fast" assertions into
    # real YouTube requests), and leftover rows from previous runs.
    profile_root = tempfile.mkdtemp(prefix="copper_profile_")

    def child_env():
        env = os.environ.copy()
        roaming = os.path.join(profile_root, "roaming")
        local = os.path.join(profile_root, "local")
        os.makedirs(roaming, exist_ok=True)
        os.makedirs(local, exist_ok=True)
        env["APPDATA"] = roaming
        env["LOCALAPPDATA"] = local
        # Instances spawned at runtime (e.g. by the native-messaging host,
        # which starts the app with no args) pick this up and add
        # --test-profile themselves, so they cannot fall back to the
        # developer's real profile and contaminate the run.
        env["COPPER_TEST_PROFILE"] = "1"
        return env

    def kill_stale_instances():
        """Kill leftovers of the exe under test from a previous run.

        A stale instance squatting on the API port makes every launch() exit
        as a "second instance" while wait_for_ready() still succeeds against
        the stale one - the suite would then silently test the wrong process
        (and the wrong profile). Only processes running THIS exe are killed;
        a separately installed Copper is left alone.
        """
        exe_abs = os.path.abspath(exe)
        ps = (
            "Get-CimInstance Win32_Process -Filter \"Name='"
            + os.path.basename(exe_abs).replace("'", "''")
            + "'\" | Where-Object { $_.ExecutablePath -eq '"
            + exe_abs.replace("'", "''")
            + "' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"
        )
        try:
            subprocess.run(
                ["powershell", "-NoProfile", "-NonInteractive", "-Command", ps],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=30,
            )
        except Exception:
            pass  # best effort; the liveness check below catches survivors

    def reset_shared_test_profile():
        """Wipe Qt's SHARED test-mode data dir (except the log).

        QStandardPaths ignores the APPDATA/LOCALAPPDATA overrides above
        (Windows Known Folders API), so the test-mode DB, tools and temp all
        live in one shared %APPDATA%\\qttest location for every run - and for
        every manual --test-profile run of the developer. Leftovers leak
        between runs: a cached yt-dlp.exe turns "fails fast" assertions into
        real YouTube requests, a custom default save path redirects job
        output, and playlist jobs left mid-retry auto-resume at startup and
        starve the yt-dlp job slots, making timing assertions flaky. The log
        is kept: the crash-hunt harness measures [CRASH] deltas against a
        baseline captured in it.
        """
        roaming = os.path.join(
            os.environ.get("APPDATA", ""), "qttest", "Copper",
            "Copper Download Manager")
        if os.path.isdir(roaming):
            for entry in os.listdir(roaming):
                if entry == "copper.log" or entry.startswith("copper.log."):
                    continue
                path = os.path.join(roaming, entry)
                try:
                    if os.path.isdir(path):
                        shutil.rmtree(path, ignore_errors=True)
                    else:
                        os.remove(path)
                except OSError:
                    pass
        local = os.path.join(
            os.environ.get("LOCALAPPDATA", ""), "qttest", "Copper")
        if os.path.isdir(local):
            shutil.rmtree(local, ignore_errors=True)

    def launch():
        p = subprocess.Popen(
            [exe, "--test-profile"],
            cwd=exe_dir,
            env=child_env(),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        app_procs.append(p)
        return p

    app_procs = []      # every app instance started by this run
    killed_procs = set()  # instances this run killed on purpose (not a crash)

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
    kill_stale_instances()
    reset_shared_test_profile()
    proc = launch()
    try:
        if not wait_for_ready(port, timeout=40):
            print("FATAL: app API did not become ready", file=sys.stderr)
            return 1
        print("  app API is ready")
        # If our own instance already exited, the API we just probed belongs
        # to a foreign/stale instance - fail fast instead of testing the
        # wrong process for the next 90 seconds.
        rc = proc.poll()
        if rc is not None:
            print(f"FATAL: launched instance exited early (rc={rc}); "
                  f"another instance owns port {port}", file=sys.stderr)
            return 1

        # --- Local API token (0.4.0): read what the app persisted so every
        # --- later request authenticates the way real clients must.
        global API_TOKEN
        API_TOKEN = load_api_token(profile_root)
        check("local API token persisted in host config", bool(API_TOKEN),
              "copper_host_config.json missing/has no apiToken")

        # --- /api/version ---
        st, j = http_request(port, "GET", "/api/version")
        check("GET /api/version -> 200", st == 200, f"status={st}")
        check("version reported", isinstance(j, dict) and j.get("version") not in (None, ""), str(j))

        # --- /api/ping ---
        st, j = http_request(port, "GET", "/api/ping")
        check("GET /api/ping -> 200", st == 200, f"status={st}")

        # --- /api/diagnostics (0.4.0 support endpoint) ---
        st, j = http_request(port, "GET", "/api/diagnostics")
        diag = j if isinstance(j, dict) else {}
        check("GET /api/diagnostics -> 200", st == 200, f"status={st}")
        check("diagnostics reports app name",
              isinstance(j, dict) and j.get("app") == "Copper Download Manager", str(j))
        check("diagnostics reports version + schema + tool state",
              isinstance(j, dict) and j.get("version") not in (None, "")
              and isinstance(j.get("schemaVersion"), int)
              and "aria2Installed" in j and "ytDlpInstalled" in j, str(j))

        # --- /api/downloads ---
        st, j = http_request(port, "GET", "/api/downloads")
        check("GET /api/downloads -> 200", st == 200, f"status={st}")
        check("downloads is a list", isinstance(j, dict) and isinstance(j.get("downloads"), list), str(j))

        # --- /api/download-filters (include/exclude file-format filter) ---
        st, j = http_request(port, "GET", "/api/download-filters")
        check("GET /api/download-filters -> 200", st == 200, f"status={st}")
        check("download-filters reports enabled flag",
              isinstance(j, dict) and j.get("enabled") is True, str(j))
        check("download-filters exposes exclude list (images blocked by default)",
              isinstance(j, dict) and isinstance(j.get("exclude"), list)
              and "jpg" in j.get("exclude", []), str(j))
        check("download-filters exposes include list (empty default = all formats allowed)",
              isinstance(j, dict) and isinstance(j.get("include"), list), str(j))

        # --- /api/download rejects an excluded image URL (file-format filter) ---
        st, j = http_request(port, "POST", "/api/download",
                             {"url": "https://example.com/photo.jpg", "filename": "photo.jpg", "path": ""})
        check("POST /api/download excluded image -> 400", st == 400, f"status={st} resp={j}")
        check("blocked image reports filter error",
              isinstance(j, dict) and "filter" in str(j.get("error", "")).lower(), str(j))

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

        # --- Local API token enforcement (0.4.0) ---
        st, _ = http_request(port, "GET", "/api/version", with_token=False)
        check("request without token -> 401", st == 401, f"status={st}")

        st, _ = http_request(port, "GET", "/api/version",
                             headers={"X-Copper-Token": "0" * 64})
        check("request with wrong token -> 401", st == 401, f"status={st}")

        st, _ = http_request(port, "GET", "/api/version")
        check("request with valid token -> 200", st == 200, f"status={st}")

        # --- Origin/cors hardening: exact allowlist, not a prefix match ---
        well_chrome = "chrome-extension://" + ("a" * 32)  # 32 chars, all a-p
        well_moz = "moz-extension://12345678-1234-1234-1234-123456789abc"

        st, _ = http_request(port, "GET", "/api/version",
                             headers={"Origin": "https://evil.example.com"})
        check("disallowed Origin (website) -> 403", st == 403, f"status={st}")

        st, _ = http_request(port, "GET", "/api/version", headers={"Origin": well_chrome})
        check("well-formed chrome-extension Origin -> 200", st == 200, f"status={st}")

        st, _ = http_request(port, "GET", "/api/version", headers={"Origin": well_moz})
        check("well-formed moz-extension Origin -> 200", st == 200, f"status={st}")

        for bad_origin in (
                "chrome-extension://abcdefghijklmnop",           # 16 chars, not 32
                "chrome-extension://QRSTUVWXYZ23456789abcdef",   # chars outside a-p
                "chrome-extension://evil.example.com",           # not an extension id
                "moz-extension://not-a-uuid",
                "moz-extension://12345678-1234-1234-1234-123456789abg"):  # non-hex
            st, _ = http_request(port, "GET", "/api/version",
                                 headers={"Origin": bad_origin})
            check(f"malformed Origin {bad_origin} -> 403", st == 403, f"status={st}")

        # --- Unsupported URL scheme rejection ---
        st, j = http_request(port, "POST", "/api/download", {"url": "file:///C:/Windows/notepad.exe"})
        check("POST /api/download file:// scheme -> 400", st == 400, f"status={st}")

        # --- OPTIONS preflight: exempt from the token, advertises the header ---
        st, _ = http_request(port, "OPTIONS", "/api/download", with_token=False,
                             headers={"Origin": well_chrome})
        check("OPTIONS without token from allowed Origin -> 204", st == 204, f"status={st}")

        # --- Request caps: an oversized body is refused before it is read ---
        st, _ = http_request(port, "POST", "/api/download",
                             {"url": "http://127.0.0.1:1/x",
                              "pad": "x" * (1024 * 1024)},
                             timeout=15.0)
        check("POST body over 1 MB -> 413", st == 413, f"status={st}")

        # --- Download-engine regression tests (chunked HTTP via a local server) ---
        chk = "DLCHUNK-REG" + ("x" * 262144)  # ~256KB, forces multi-chunk
        workdir = tempfile.mkdtemp(prefix="copper_it_")
        try:
            def add_download(url, name, path):
                st, j = http_request(port, "POST", "/api/download",
                                     {"url": url, "filename": name, "path": path})
                return st, j.get("id") if isinstance(j, dict) else None

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

            # 3b) File-conflict policy (0.4.0): a fresh add whose target file
            #     already exists must NOT clobber it - the new download lands at
            #     "name (n).ext" and the user's original bytes are untouched.
            conflict_name = "conflict.bin"
            conflict_original = b"ORIGINAL-DO-NOT-TOUCH"
            with open(os.path.join(workdir, conflict_name), "wb") as f:
                f.write(conflict_original)
            conflict_payload = "CONFLICT-" + ("c" * 8192)
            srv, sp = _make_file_server(conflict_payload, mode="normal")
            try:
                st, mid = add_download(f"http://127.0.0.1:{sp}/conflict.bin",
                                       conflict_name, workdir)
                check("add conflicting download -> 200", st == 200, f"status={st}")
                d = wait_download_status(port, mid, {"Completed", "Failed"})
                check("conflicting download completes",
                      d is not None and d.get("status") == "Completed", str(d))
                saved_path = (d or {}).get("filePath") or ""
                check("conflict is renamed to 'name (1).ext'",
                      os.path.basename(saved_path) == "conflict (1).bin", saved_path)
                with open(os.path.join(workdir, conflict_name), "rb") as f:
                    check("pre-existing file is left untouched",
                          f.read() == conflict_original,
                          "the existing file was overwritten")
                renamed = os.path.join(workdir, "conflict (1).bin")
                ok = os.path.isfile(renamed)
                if ok:
                    with open(renamed, "rb") as f:
                        ok = f.read() == conflict_payload.encode("utf-8")
                check("renamed download holds the new file byte-exact", ok, renamed)
            finally:
                srv.shutdown()

            # 3c) HTTP failure classification (0.4.0): a dead link must fail with
            #     an actionable message, not a raw Qt error string.
            srv, sp = _make_file_server("gone", mode="notfound")
            try:
                st, mid = add_download(f"http://127.0.0.1:{sp}/missing.bin",
                                       "missing.bin", workdir)
                check("add missing-file download -> 200", st == 200, f"status={st}")
                d = wait_download_status(port, mid, {"Failed"}, timeout=30.0)
                err = (d or {}).get("error") or ""
                check("404 download is Failed",
                      d is not None and d.get("status") == "Failed", str(d))
                check("404 is classified with an actionable message",
                      "not found" in err.lower() and "404" in err, repr(err))
            finally:
                srv.shutdown()

            # 3d) Custom request headers (Settings) are applied to every request
            #     the engine makes. The setting is written straight into the app's
            #     running database (WAL) and must take effect without a restart.
            def set_custom_headers(value):
                db_path = os.path.join(diag.get("profile") or "", "copper.db")
                con = sqlite3.connect(db_path, timeout=10)
                try:
                    con.execute(
                        "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)",
                        ("customHeaders", value))
                    con.commit()
                finally:
                    con.close()

            seen_headers = []
            hdr_payload = "HEADERS-" + ("h" * 8192)
            srv, sp = _make_file_server(hdr_payload, mode="normal", record=seen_headers)
            try:
                set_custom_headers("X-Copper-Test: header-ok")
                st, mid = add_download(f"http://127.0.0.1:{sp}/hdr.bin", "hdr.bin", workdir)
                check("add custom-header download -> 200", st == 200, f"status={st}")
                d = wait_download_status(port, mid, {"Completed", "Failed"})
                check("custom-header download completes",
                      d is not None and d.get("status") == "Completed", str(d))
                check("custom header reaches the server on every request",
                      bool(seen_headers) and all(v == "header-ok" for v in seen_headers),
                      str(seen_headers[:4]))
            finally:
                srv.shutdown()
                set_custom_headers("")

            # 4) copper:// protocol injection: the desktop app must parse a
            #    copper://download?url=...&filename=...&path=... link (as built
            #    by the extension with encodeURIComponent) and start the download.
            srv, sp = _make_file_server(chk, mode="normal")
            try:
                enc = lambda v: quote(str(v), safe="")
                cu_url = (f"copper://download?url={enc(f'http://127.0.0.1:{sp}/file.bin')}"
                          f"&filename={enc('copper.bin')}"
                          f"&path={enc(workdir)}")
                st, _ = http_request(port, "POST", "/api/forward", {"argument": cu_url})
                check("forward copper:// download -> 200", st == 200, f"status={st}")

                inner_url = f"http://127.0.0.1:{sp}/file.bin"

                def wait_copper_download(timeout=15.0):
                    deadline = time.time() + timeout
                    while time.time() < deadline:
                        _, jl = http_request(port, "GET", "/api/downloads")
                        matched = [x for x in dl_rows(jl)
                                   if x.get("url") == inner_url]
                        if matched:
                            return matched[0]
                        time.sleep(0.5)
                    return None

                target = wait_copper_download()
                ok = target is not None
                if ok:
                    d = wait_download_status(port, target["id"], {"Completed", "Failed"})
                    ok = d is not None and d.get("status") == "Completed"
                if ok and os.path.isfile(os.path.join(workdir, "copper.bin")):
                    with open(os.path.join(workdir, "copper.bin"), "rb") as f:
                        ok = f.read() == chk.encode("utf-8")
                check("copper:// link creates byte-exact download", ok,
                      f"url-matched={'yes' if target else 'no'}")
            finally:
                srv.shutdown()

            # 5) A .torrent HTTP URL injected via copper:// (as the extension
            #    does for a .torrent link) must NOT be downloaded as a plain
            #    HTTP file. It should route to the torrent handling path instead.
            with tempfile.NamedTemporaryFile(suffix=".torrent", delete=False) as tf:
                tf.write(b"d4:infod4:lengthi1ee4:name4:a.t8:piece lengthi1eee")
                tor_path = tf.name
            try:
                srv, sp = _make_file_server_from_path(tor_path, "application/x-bittorrent")
                enc = lambda v: quote(str(v), safe="")
                tor_url = f"http://127.0.0.1:{sp}/a.torrent"
                cu_url = (f"copper://download?url={enc(tor_url)}"
                          f"&filename={enc('a.torrent')}"
                          f"&path={enc(workdir)}")

                # The torrent dialog is modal, so the forward may block; send it
                # in a background thread and just verify no HTTP download is
                # registered for the .torrent URL afterward.
                def do_forward():
                    try:
                        http_request(port, "POST", "/api/forward", {"argument": cu_url}, timeout=2.0)
                    except Exception:
                        pass

                th = threading.Thread(target=do_forward, daemon=True)
                th.start()
                time.sleep(4)

                _, jl = http_request(port, "GET", "/api/downloads")
                bug_http = [x for x in dl_rows(jl)
                            if x.get("type") == "HTTP" and "a.torrent" in x.get("url", "")]
                check("copper// script .torrent URL is not downloaded as HTTP",
                      not bug_http, str(bug_http))

                raw_file = os.path.join(workdir, "a.torrent")
                check(".torrent URL not saved as a raw HTTP file",
                      not os.path.isfile(raw_file))
            finally:
                srv.shutdown()
                try:
                    os.remove(tor_path)
                except OSError:
                    pass

            # 5b) Playlist/video URL routing: a YouTube playlist injected with
            #     format=playlist-mp3 must route to the yt-dlp engine (type
            #     "YtDlp"), NOT the plain HTTP engine. yt-dlp is not installed in
            #     the test environment, so the download fails fast with a clear
            #     "yt-dlp not installed" state instead of downloading the HTML.
            playlist_url = "https://www.youtube.com/playlist?list=PLitPtest123"
            st, j = http_request(port, "POST", "/api/download", {
                "url": playlist_url,
                "filename": "MyPlaylist",
                "path": workdir,
                "format": "playlist-mp3",
            })
            check("add playlist download -> 200", st == 200, f"status={st}")
            pid = j.get("id") if isinstance(j, dict) else None
            # A permanently unreachable playlist is retried automatically (3 attempts
            # with 5/10/20s backoff, and the row reads "Queued" while it waits for
            # the next attempt), so this needs a generous window to see the final
            # Failed state.
            d = wait_download_status(port, pid, {"Failed", "Completed"}, timeout=75.0)
            check("playlist URL routed to yt-dlp engine",
                  d is not None and d.get("type") == "YtDlp",
                  f"type={d.get('type') if d else None}")
            check("playlist without yt-dlp fails clearly",
                  d is not None and d.get("status") == "Failed",
                  f"status={d.get('status') if d else None}")

            # 5c) Playlist job shape: a playlist URL must become ONE folder job with
            #     one child row per item, not N independent yt-dlp processes. The
            #     old code spawned one process per video, which is what got the
            #     extractor throttled and made playlists fail after a few items.
            #     Driven through the --playlist-dialog: hook so the modal item
            #     picker is skipped while the real job-creation path still runs.
            #     The playlist id is unique per run: the job is looked up by URL, and
            #     a fixed id would collide with rows left by an earlier run. It does
            #     not exist, so yt-dlp fails on it - exactly like the old 5c case.
            fake_playlist = ("https://www.youtube.com/playlist?list=PLcoppertest"
                             + uuid.uuid4().hex[:8])
            st, _ = http_request(port, "POST", "/api/forward",
                                 {"argument": f"--playlist-dialog:{fake_playlist}"})
            check("playlist dialog bypass accepted", st == 200, f"status={st}")

            job = None
            kids = []
            deadline = time.time() + 15
            while time.time() < deadline:
                _, jl = http_request(port, "GET", "/api/downloads")
                all_dl = dl_rows(jl)
                job = next((x for x in all_dl
                            if x.get("url") == fake_playlist), None)
                if job:
                    kids = [x for x in all_dl if x.get("parentId") == job.get("id")]
                    if len(kids) == 3:
                        break
                time.sleep(0.4)

            check("playlist becomes a single folder job",
                  job is not None and job.get("isFolder") is True,
                  f"job={job}")
            check("playlist job has one row per item",
                  len(kids) == 3, f"children={len(kids)}")
            check("playlist items are not folder rows",
                  all(k.get("isFolder") is False for k in kids),
                  str([k.get("isFolder") for k in kids]))
            # The rows must carry their playlist position and site video id: that is
            # what lets one yt-dlp process drive the whole playlist and still report
            # each item's progress on its own row (and it becomes the
            # --playlist-items selection, so a missing position would silently
            # download videos the user never picked).
            check("playlist items carry their playlist position",
                  sorted(k.get("trackIndex") for k in kids) == [1, 2, 3],
                  str(sorted(k.get("trackIndex") for k in kids)))
            check("playlist items carry distinct video ids",
                  len({k.get("videoId") for k in kids}) == 3 and
                  all(k.get("videoId") for k in kids),
                  str([k.get("videoId") for k in kids]))
            check("playlist job starts as Queued or Downloading",
                  job is not None and job.get("status") in ("Queued", "Downloading", "Failed"),
                  f"status={job.get('status') if job else None}")

            # 5d) Pause/resume state machine on a playlist job. Pausing must keep
            #     the job (and its items) rather than cancelling them, and resuming
            #     must put it back into a running state.
            if job:
                mid = job.get("id")
                http_request(port, "POST", "/api/forward", {"argument": f"pause-all:{mid}"})
                d = wait_download_status(port, mid, {"Paused"}, timeout=10.0)
                check("playlist job can be paused",
                      d is not None and d.get("status") == "Paused",
                      f"status={d.get('status') if d else None}")

                _, jl = http_request(port, "GET", "/api/downloads")
                paused_kids = [x for x in dl_rows(jl)
                               if x.get("parentId") == mid]
                check("pausing a job pauses its items too",
                      all(k.get("status") == "Paused" for k in paused_kids) and paused_kids,
                      str([k.get("status") for k in paused_kids]))

                http_request(port, "POST", "/api/forward", {"argument": f"resume-all:{mid}"})
                d = wait_download_status(port, mid,
                                         {"Downloading", "Failed", "Completed"}, timeout=15.0)
                check("paused playlist job can be resumed",
                      d is not None and d.get("status") != "Paused",
                      f"status={d.get('status') if d else None}")

                # 5e) Retry must clear the error/attempt state of a failed job.
                http_request(port, "POST", "/api/forward", {"argument": f"retry:{mid}"})
                d = wait_download_status(port, mid,
                                         {"Queued", "Downloading", "Failed", "Completed"},
                                         timeout=15.0)
                check("retry does not leave the job stuck",
                      d is not None and d.get("status") != "Paused",
                      f"status={d.get('status') if d else None}")

            # 5f) Pausing a row INSIDE a playlist job must act on the job, not just
            #     relabel the row. yt-dlp runs one process for the whole playlist, so
            #     a single file cannot be suspended on its own. Before the fix,
            #     pausing a child flipped only that row to "Paused" while the job
            #     kept transferring, and resuming it did nothing at all.
            if job and kids:
                child = kids[0]
                job_id = job.get("id")

                def rows():
                    _, body = http_request(port, "GET", "/api/downloads")
                    return {x.get("id"): x for x in dl_rows(body)}

                before = rows()
                # pauseDownload/cancelDownload only act on a Queued or Downloading
                # row, and the retry above may have left the job Failed. Record
                # what was pausable so the checks below assert the right thing
                # instead of failing for a reason that has nothing to do with
                # routing.
                was_pausable = before.get(job_id, {}).get("status") in ("Queued", "Downloading")

                http_request(port, "POST", "/api/forward",
                             {"argument": f"pause-all:{child['id']}"})
                time.sleep(1.0)
                after = rows()
                j, c = after.get(job_id, {}), after.get(child["id"], {})

                # The defect itself: the item claimed to be Paused while the job it
                # does not own carried on transferring. This holds no matter what
                # state the job was in - if the job was not pausable, the pause was
                # legitimately refused for the item too, and both agree.
                check("a playlist item is never Paused while its job is still running",
                      not (c.get("status") == "Paused" and
                           j.get("status") in ("Queued", "Downloading")),
                      f"item={c.get('status')} job={j.get('status')}")

                if was_pausable:
                    check("pausing a playlist item pauses the job that owns it",
                          j.get("status") == "Paused",
                          f"job status={j.get('status')} while item status="
                          f"{c.get('status')}")

                # Resuming the item must move the job, not just the row. It used to
                # be a no-op that only logged a line.
                if j.get("status") == "Paused":
                    http_request(port, "POST", "/api/forward",
                                 {"argument": f"resume-all:{child['id']}"})
                    time.sleep(1.0)
                    r = rows()
                    check("resuming a playlist item resumes the job that owns it",
                          r.get(job_id, {}).get("status") != "Paused",
                          f"job status={r.get(job_id, {}).get('status')}")
                    check("a resumed item is not left Paused behind a running job",
                          not (r.get(child["id"], {}).get("status") == "Paused" and
                               r.get(job_id, {}).get("status") in ("Queued", "Downloading")),
                          f"item={r.get(child['id'], {}).get('status')} "
                          f"job={r.get(job_id, {}).get('status')}")

                # The API has to report the controlling job so the UI can label the
                # controls; without it the window acts on the whole playlist
                # without saying so.
                now = rows()
                now_kids = [v for k, v in now.items() if v.get("parentId") == job_id]
                check("playlist items report the job that controls them",
                      bool(now_kids) and
                      all(k.get("controlledByJob") == job_id for k in now_kids),
                      str([k.get("controlledByJob") for k in now_kids]))
                check("a job row is its own transfer",
                      now.get(job_id, {}).get("controlledByJob", -1) == -1,
                      "the folder row must not point at a parent job")

            # 5g) "Download only the files I ticked". The picker's ticks used to
            #     reach a code path that silently widened the request to the WHOLE
            #     playlist the moment any row had no known position - that is how
            #     gigabytes nobody asked for landed on disk - and the "add track
            #     numbers" box changed nothing about the names. Both are asserted
            #     on the contract the job carries, which needs no network.
            def jobs_for(url):
                _, body = http_request(port, "GET", "/api/downloads")
                return sorted([x for x in dl_rows(body)
                               if x.get("url") == url and (x.get("parentId") or -1) < 0],
                              key=lambda x: x.get("id") or 0)

            subset_url = "https://www.youtube.com/playlist?list=PLcoppertest" + uuid.uuid4().hex[:8]
            st, _ = http_request(port, "POST", "/api/forward",
                                 {"argument": f"--playlist-dialog:{subset_url}:1,3"})
            check("subset playlist hook accepted", st == 200, f"status={st}")

            subset_job = None
            deadline = time.time() + 15
            while time.time() < deadline:
                found = jobs_for(subset_url)
                if found:
                    subset_job = found[-1]
                    break
                time.sleep(0.3)
            check("ticked subset creates one job", subset_job is not None, f"url={subset_url}")

            if subset_job:
                # The selection is stored ON the job: it is what yt-dlp is told to
                # fetch, so it has to outlive the rows and a restart.
                check("job remembers exactly the ticked positions",
                      subset_job.get("selectedPositions") == [1, 3],
                      str(subset_job.get("selectedPositions")))
                check("job keeps the track-number choice",
                      subset_job.get("trackNumbers") is True,
                      str(subset_job.get("trackNumbers")))

                _, body = http_request(port, "GET", "/api/downloads")
                sub_kids = sorted([x for x in dl_rows(body)
                                   if x.get("parentId") == subset_job.get("id")],
                                  key=lambda x: x.get("trackIndex") or 0)
                check("only the ticked rows are created",
                      [k.get("trackIndex") for k in sub_kids] == [1, 3],
                      str([(k.get("trackIndex"), k.get("fileName")) for k in sub_kids]))
                # Numbering runs over the SELECTION (positions 1 and 3 become 001
                # and 002), so a partial pick reads as a complete set on disk
                # instead of showing up as gaps at 002, 004, ...
                check("rows are numbered over the selection, not the playlist",
                      [k.get("fileName") for k in sub_kids] ==
                      ["001.Copper Test Item 1.mp4", "002.Copper Test Item 3.mp4"],
                      str([k.get("fileName") for k in sub_kids]))
                check("every row repeats its job's track-number choice",
                      all(k.get("trackNumbers") is True for k in sub_kids),
                      str([k.get("trackNumbers") for k in sub_kids]))

            # 5h) Nothing ticked must be REFUSED, never answered with "the whole
            #     playlist". ":0" is the picker with every box cleared.
            empty_url = "https://www.youtube.com/playlist?list=PLcoppertest" + uuid.uuid4().hex[:8]
            st, _ = http_request(port, "POST", "/api/forward",
                                 {"argument": f"--playlist-dialog:{empty_url}:0"})
            check("empty-selection hook accepted", st == 200, f"status={st}")

            empty_job = None
            deadline = time.time() + 15
            while time.time() < deadline:
                found = jobs_for(empty_url)
                if found:
                    empty_job = found[-1]
                    break
                time.sleep(0.3)
            check("a job with nothing ticked is still recorded",
                  empty_job is not None, f"url={empty_url}")

            if empty_job:
                check("empty selection is recorded as empty, not as everything",
                      empty_job.get("selectedPositions") == [],
                      str(empty_job.get("selectedPositions")))
                settled = wait_download_status(port, empty_job.get("id"),
                                                {"Failed"}, timeout=20.0)
                check("empty selection refuses to start instead of downloading all",
                      settled is not None and settled.get("status") == "Failed" and
                      "selected" in (settled.get("error") or "").lower(),
                      f"status={settled.get('status') if settled else None} "
                      f"error={settled.get('error') if settled else None}")

            # 5i) End to end against a real playlist: two NON-CONTIGUOUS positions
            #     must yield exactly two files, numbered 001/002 over the pick.
            #     This is the user-visible defect: a pick of items 9 and 11 either
            #     dragged in the whole playlist or produced 009./011. files.
            #     Opt-in because it needs BOTH the network and yt-dlp installed in
            #     the app's tools folder, and the rest of the suite is offline:
            #         set COPPER_IT_REAL_PLAYLIST=1
            if os.environ.get("COPPER_IT_REAL_PLAYLIST") != "1":
                print("  SKIP real playlist end-to-end (set COPPER_IT_REAL_PLAYLIST=1)")
            else:
                real_url = "https://www.youtube.com/playlist?list=PLbpi6ZahtOH6Blw3RGYpWkSByi_T7Rygb"

                # An earlier run leaves its own job for this playlist behind (the
                # app resumes interrupted jobs at startup) and two jobs writing
                # one folder makes the result unreadable, so park them first.
                leftovers = jobs_for(real_url)
                for old in leftovers:
                    http_request(port, "POST", "/api/forward",
                                 {"argument": f"pause-all:{old.get('id')}"})
                if leftovers:
                    print(f"  parked {len(leftovers)} earlier job(s) for this playlist")
                    time.sleep(2.0)

                # Rows left over by earlier runs all match this URL, so remember
                # the newest id BEFORE asking for the probe: a probe loop that
                # accepts any row would hand back an old job, and every later
                # "newer than the probe" filter would then be wrong.
                before_probe = max([x.get("id") or 0 for x in jobs_for(real_url)],
                                   default=-1)

                # A ":0" job reports the playlist's folder without downloading
                # anything, which is what lets this run start from a clean
                # directory: leftovers (and the download archive) from an earlier
                # run would make yt-dlp skip the very files this test looks for.
                st, _ = http_request(port, "POST", "/api/forward",
                                     {"argument": f"--playlist-real:{real_url}:0"})
                check("real playlist probe accepted", st == 200, f"status={st}")

                probe = None
                deadline = time.time() + 45
                while time.time() < deadline:
                    found = [x for x in jobs_for(real_url)
                             if (x.get("id") or 0) > before_probe]
                    if found:
                        probe = found[-1]
                        break
                    time.sleep(0.5)
                check("real playlist probe created (needs network + yt-dlp)",
                      probe is not None and bool(probe.get("filePath")),
                      f"job={probe}")

                scratch = probe.get("filePath") if probe else None
                if scratch and os.path.isdir(scratch):
                    for name in os.listdir(scratch):
                        try:
                            if os.path.isfile(os.path.join(scratch, name)):
                                os.remove(os.path.join(scratch, name))
                        except OSError:
                            pass

                if scratch:
                    st, _ = http_request(port, "POST", "/api/forward",
                                         {"argument": f"--playlist-real:{real_url}:11,18"})
                    check("real playlist subset accepted", st == 200, f"status={st}")

                    real_job = None
                    # Identified by what it is asked to fetch, not just by being
                    # newer than the probe: the probe itself is newer than
                    # nothing and carries an empty selection.
                    deadline = time.time() + 45
                    while time.time() < deadline:
                        found = [x for x in jobs_for(real_url)
                                 if (x.get("id") or 0) > before_probe and
                                 x.get("selectedPositions") == [11, 18]]
                        if found:
                            real_job = found[-1]
                            break
                        time.sleep(0.5)
                    check("subset job created for the real playlist",
                          real_job is not None and
                          real_job.get("selectedPositions") == [11, 18],
                          f"job={real_job}")

                    if real_job:
                        done = wait_download_status(port, real_job.get("id"),
                                                    {"Completed", "Failed"}, timeout=420)
                        check("two-item playlist job finishes",
                              done is not None and done.get("status") == "Completed",
                              f"status={done.get('status') if done else None} "
                              f"error={done.get('error') if done else None}")

                        on_disk = sorted(n for n in os.listdir(scratch)
                                         if os.path.isfile(os.path.join(scratch, n)))
                        numbered = [n for n in on_disk
                                    if len(n) > 4 and n[:3].isdigit() and n[3] == '.']
                        check("exactly the two picked files are on disk",
                              len(numbered) == 2 and
                              all(n.startswith(("001.", "002.")) for n in numbered),
                              str(on_disk))

                        _, body = http_request(port, "GET", "/api/downloads")
                        rows = [x for x in dl_rows(body)
                                if x.get("parentId") == real_job.get("id")]
                        check("rows follow the renamed files",
                              sorted((x.get("fileName") or "") for x in rows) == numbered,
                              str(sorted((x.get("trackIndex"), x.get("fileName"))
                                         for x in rows)))

            # 6) Native-messaging host -> named pipe injection (the IDM model).
            #    The host exe sits next to the app exe and forwards a browser
            #    native-messaging message to the app over the QLocalServer pipe.
            host_path = os.path.join(exe_dir, "copper_native_host.exe")
            if not os.path.isfile(host_path):
                check("native host executable deployed", False, f"{host_path} missing")
            else:
                check("native host executable deployed", True, "")
                st, _ = http_request(port, "GET", "/api/ping")
                check("app reachable for host ping", st == 200, f"status={st}")

                # a) token: the host must hand out the same install-scoped
                #    token the API enforces, straight from the shared config.
                rep = run_native_host(host_path, {"action": "getToken"}, env=child_env())
                check("native host getToken -> ok",
                      bool(rep and rep.get("ok") is True), str(rep))
                check("native host token matches API token",
                      bool(rep and rep.get("token") and rep.get("token") == API_TOKEN),
                      f"host={str((rep or {}).get('token'))[:12]} api={API_TOKEN[:12]}")

                # b) ping: host must reach the app over the pipe and report running.
                rep = run_native_host(host_path, {"action": "ping"}, env=child_env())
                check("native host ping -> ok", bool(rep and rep.get("ok") is True), str(rep))
                check("native host reports app running",
                      bool(rep and rep.get("running") is True), str(rep))

                # c) download: a small file injected through the host + pipe must
                #    be registered and complete byte-exact.
                nm_payload = "NMPIPE-" + ("y" * 65536)
                nm_srv, nm_sp = _make_file_server(nm_payload, mode="normal")
                try:
                    nm_url = f"http://127.0.0.1:{nm_sp}/nm.bin"
                    rep = run_native_host(host_path, {
                        "action": "download",
                        "url": nm_url,
                        "filename": "nm.bin",
                        "path": workdir,
                    }, env=child_env())
                    check("native host download -> ok", bool(rep and rep.get("ok") is True), str(rep))

                    nm_mid = None
                    deadline = time.time() + 15
                    while time.time() < deadline:
                        _, jl = http_request(port, "GET", "/api/downloads")
                        matched = [x for x in dl_rows(jl)
                                   if x.get("url") == nm_url]
                        if matched:
                            nm_mid = matched[0]["id"]
                            break
                        time.sleep(0.5)
                    check("pipe-injected download registered", nm_mid is not None,
                          f"url={nm_url}")

                    if nm_mid is not None:
                        d = wait_download_status(port, nm_mid, {"Completed", "Failed"})
                        ok = d is not None and d.get("status") == "Completed"
                        if ok and os.path.isfile(os.path.join(workdir, "nm.bin")):
                            with open(os.path.join(workdir, "nm.bin"), "rb") as f:
                                ok = f.read() == nm_payload.encode("utf-8")
                        check("pipe-injected download completes byte-exact", ok, str(d))
                finally:
                    nm_srv.shutdown()

            # 7) Auto-resume of an interrupted HTTP download after app restart.
            #    Start a slow download, interrupt the process mid-transfer, then
            #    relaunch and verify the partial .chunk data is used to continue
            #    from the saved offset rather than restarting from scratch.
            res_payload = "DLRESUME-" + ("z" * 2_000_000)  # ~2MB, trickled slowly
            res_bin = os.path.join(workdir, "resume.bin")
            res_mid = None
            interrupted = False
            srv, sp = _make_file_server(res_payload, mode="slow")
            try:
                res_url = f"http://127.0.0.1:{sp}/resume.bin"
                st, res_mid = add_download(res_url, "resume.bin", workdir)
                check("add resume download -> 200", st == 200, f"status={st}")

                # Wait until the engine has committed a non-trivial amount of
                # partial data (progress moving but far from complete).
                deadline = time.time() + 60
                partial_seen = False
                while time.time() < deadline:
                    _, jl = http_request(port, "GET", "/api/downloads")
                    d = find_download(jl, res_mid)
                    if d is not None:
                        prog = d.get("progress") or 0
                        if 0 < prog < 80 and d.get("status") == "Downloading":
                            partial_seen = True
                            break
                    time.sleep(0.5)
                check("resume download shows partial progress", partial_seen,
                      f"mid={res_mid}")

                # Persist the DB/progress sidecar one more time so the resume
                # state is fresh, then hard-kill the app (not a graceful shutdown).
                time.sleep(1.5)
                killed_procs.add(proc)
                proc.kill()
                try:
                    proc.wait(timeout=10)
                except Exception:
                    pass
                interrupted = True
            finally:
                # Only keep the server for the relaunch if we actually interrupted.
                if not interrupted:
                    srv.shutdown()

            if interrupted:
                try:
                    # Relaunch. The app binds a fixed LocalServer port
                    # (DEFAULT_PORT); the killed instance has released it, so the
                    # fresh instance rebinds and restore resumes the download.
                    port = DEFAULT_PORT
                    proc = launch()
                    ok = wait_for_ready(port, timeout=40)
                    # A relaunched instance that already exited means a
                    # foreign instance owns the port - don't pretend the
                    # relaunch succeeded.
                    if ok and proc.poll() is not None:
                        ok = False
                    check("app relaunches after interrupt", ok, f"port={port}")
                    if ok:
                        # The token must be reused across restarts (loaded from
                        # disk, never regenerated) or every stored client -
                        # second instance, native host, extension - would break
                        # on every app start. The API calls that then use
                        # API_TOKEN against the new instance prove it accepted
                        # the persisted token.
                        new_token = load_api_token(profile_root, timeout=10)
                        check("API token stable across restart",
                              bool(new_token) and new_token == API_TOKEN,
                              f"before={API_TOKEN[:12]} after={new_token[:12]}")
                        d = wait_download_status(port, res_mid, {"Completed", "Failed"}, timeout=120)
                        final_ok = d is not None and d.get("status") == "Completed"
                        if final_ok and os.path.isfile(res_bin):
                            with open(res_bin, "rb") as f:
                                final_ok = f.read() == res_payload.encode("utf-8")
                        check("interrupted download resumes and completes byte-exact",
                              final_ok, f"status={d.get('status') if d else None}")
                finally:
                    srv.shutdown()

        finally:
            shutil.rmtree(workdir, ignore_errors=True)

        # An instance that died on its own with an NTSTATUS code means the
        # crash handler ran mid-suite (e.g. 0xC0000005 access violation).
        # Surface it as an explicit failure so an intermittent crash cannot
        # silently masquerade as unrelated downstream timeouts.
        for p in app_procs:
            if p in killed_procs:
                continue
            rc = p.poll()
            if rc is not None and rc not in (0, 1):
                check(f"app instance exited cleanly (rc={rc})", False,
                      "unexpected process death - search copper.log for [CRASH]")

    finally:
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        # Detached instances (e.g. launched by the native host) are not in
        # app_procs - sweep them so they cannot squat on the port for the
        # next run or outlive the suite.
        kill_stale_instances()
        if failed == 0:
            shutil.rmtree(profile_root, ignore_errors=True)
        else:
            print(f"  profile kept for triage: {profile_root}")

    print(f"\n{passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
