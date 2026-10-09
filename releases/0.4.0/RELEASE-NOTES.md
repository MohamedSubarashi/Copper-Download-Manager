# Copper Download Manager 0.4.0

Reliability, security and integrity release. This release hardens what
0.3.1 already does — no features were removed and the interface is
unchanged, with one deliberate exception noted under **Breaking change**.

## Highlights

### Security
- **The local API now requires a per-install token.** Every request to
  `http://127.0.0.1:24680` must carry `X-Copper-Token` (except the
  preflight), so other programs running as the same user can no longer
  queue, list or cancel downloads without consent. The browser
  extensions obtain the token through the native messaging host, and a
  `copper://` fallback still works.
- **Exact browser-origin allowlist.** Cross-site pages are rejected by
  origin, not by substring match.
- **Third-party binaries are verified before they run.** aria2c and
  ffmpeg downloads are checked against SHA-256 pins tied to their exact
  download URL; yt-dlp is checked against the checksum file published
  with its own release. A URL change without a manifest update fails
  closed. Update downloads are verified with the release asset digest
  and staged atomically.
- **No more killing by port number.** The aria2 daemon is tracked with
  a PID file and its process identity is verified before it is stopped,
  on a dynamically negotiated RPC port. Previously any unrelated
  process that happened to own the RPC port could be terminated.

### Reliability & integrity
- **Range validation end to end**: `206`/`Content-Range`/`Content-Length`
  are validated per chunk, remote-size changes are detected, and an
  exact final size check guards completion.
- **Resume is integrity-safe**: resume state is written atomically
  (no torn writes), stale state is rejected, and merged output is
  verified byte-for-byte.
- **Never overwrite an existing file**: a new download whose target
  already exists is saved as `name (n).ext` instead of clobbering it.
- **Disk-space guard**: a download that cannot fit on the target drive
  is refused up front with a clear message instead of failing mid-transfer.
- **Actionable HTTP errors**: 401/403/404/410/416/429/5xx, timeouts,
  DNS and TLS failures are translated into plain-language messages.
- **System proxy support**: WinINET/PAC proxy settings now apply to
  all downloads (previously every proxied environment failed outright).
- **Custom headers**: Settings → Downloads accepts extra request headers
  (e.g. `Referer:`, `Authorization:`) applied to every download request.
- **Database safety**: schema migrations run in a single transaction
  with a pre-migration snapshot, so an interrupted upgrade can no longer
  leave a half-migrated database.

### Diagnostics & quality
- **`GET /api/diagnostics`** reports app/schema versions, profile and
  tools paths, tool install state, daemon state and update state in one
  call — enough to triage a bug report from a log excerpt.
- **Version single-sourced**: `0.4.0` comes from CMake alone; the
  User-Agent, resources, installer and notices are verified against it
  by `tools/check_version_consistency.py`.
- **New test layers**: a 17-case QtTest binary (`copper_tests`) for the
  verification/sanitization/token utilities, and the integration suite
  grew to 99 checks (token auth, origin allowlist, diagnostics,
  byte-exact resume, crash-free relaunch, file-conflict policy, HTTP
  failure classification, custom request headers reaching the server).

## Breaking change
Local-API consumers outside this repository must send
`X-Copper-Token` (available to the extensions via the native-messaging
`getToken` action). The bundled extensions are updated in this release.

## Install
- **Installer**: `CopperDownloadManager-0.4.0-setup.exe`
- **Portable**: extract `CopperDownloadManager-portable-0.4.0.zip`
  anywhere and run `CopperDownloadManager.exe`
- Verify downloads with `SHA256SUMS` (`sha256sum -c SHA256SUMS`).

## Known limitations
- Binaries are not code-signed (no signing certificate available in
  this cycle); Windows SmartScreen may warn on first run.
