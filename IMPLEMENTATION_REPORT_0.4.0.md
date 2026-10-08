# Implementation Report — Copper Download Manager 0.4.0

Upgrade from 0.3.1 to 0.4.0 as a reliability / security / integrity
release. This was **not** a rewrite: the architecture, feature set and
interfaces of 0.3.1 are preserved (HTTP/HTTPS downloads, multi-connection
chunks, pause/resume/cancel/retry/queue, speed limits, SQLite history,
yt-dlp + playlists, FFmpeg, aria2, torrents, browser extensions, native
messaging, local API, themes, settings, updater). The one intentional
behavior change is the token-gated local API (see *Breaking change*
below).

---

## 1. Requirements coverage

Statuses use the required vocabulary: **PASS / FAIL / NOT RUN / BLOCKED**.
"PASS" means the behavior was executed and asserted by an automated
check or tool listed under *Evidence*; anything not executed is reported
as NOT RUN, never inferred.

### P0 — correctness, security, integrity

| Requirement | Status | Evidence |
|---|---|---|
| Version 0.4.0 single-sourced from one place; no stale `0.3.1` / `CopperDownloadManager/1.0`; centralized `copperUserAgent()` | **PASS** | `tools/check_version_consistency.py` (81 tracked files) + `copper_tests::version_isThreePartSemver`, `userAgent_followsApplicationVersion` |
| Range validation: `206`, `Content-Range`, `Content-Length`, exact-size checks | **PASS** | Integration suite: chunked/truncated/unknown-length + exact-size cases (88/0, run 9) |
| `bool mergeChunks()` merging with per-piece validation | **PASS** | Integration suite: byte-exact completion cases |
| Resume integrity: atomic resume writes (QSaveFile), stale-state rejection | **PASS** | Suite: interrupt → relaunch → resume byte-exact; API token stability across restart |
| Local API token auth (`X-Copper-Token`), extensions updated in lockstep | **PASS** | Suite: 401 without token, 200 with token, token stable across restart; extension lint |
| Exact browser-origin allowlist | **PASS** | Suite: origin allow/reject matrix |
| Dependency SHA-256 verification (pinned, URL-tied for aria2/ffmpeg; published sums for yt-dlp) | **PASS** (logic + aria2 live) / **NOT RUN** (live ffmpeg/yt-dlp download) | `copper_tests` fail-closed cases (8); suite run 7 fresh-profile aria2 auto-install verified the live pin |
| Updater verification (GitHub asset digest + atomic staging) | **NOT RUN** | Requires a published GitHub release to exercise; code paths compile-verified only |
| Remove unsafe kill-by-port; PID-file + identity-checked shutdown; dynamic RPC port | **PASS** | Suite: aria2 daemon lifecycle (start, dynamic port, PID cleanup, relaunch) |
| P0 crash/bug fixes (§71) — see §3 below | **PASS** | 6-run crash hunt 6/6 × 85/0, no `[CRASH]` reports; 30-min native-host watch |

### P1 — reliability, operability

| Requirement | Status | Evidence |
|---|---|---|
| DB transactional migration + pre-migration backup | **PASS** (migration) / **NOT RUN** (backup, rollback) | Every fresh-profile suite run executes the 0→3 migration in one transaction; backup only triggers for `0 < schema < 3` and failure-rollback needs fault injection — neither scenario was run |
| Disk-space check before transfer | **NOT RUN** | Requires a full target volume; logic reviewed only |
| File-conflict resolution on add (never overwrite) | **NOT RUN** | No harness case was added; reported honestly |
| HTTP error classification (actionable messages) | **NOT RUN** | Suite does not assert negative-status message text |
| System proxy support | **NOT RUN** | Development machine has `ProxyEnable=0`, so the setting is a no-op here |
| Custom request headers (Settings UI + application) | **NOT RUN** | UI path not exercised |
| Browser/environment diagnostics | **PASS** | `GET /api/diagnostics`: 3 harness checks (app name, version, schema + tool state) |
| Streaming / unknown-length (chunked) responses | **PASS** | Suite unknown-length case reaches 100% and merges byte-exact |

### P2 — packaging, metadata, docs, CI

| Requirement | Status | Evidence |
|---|---|---|
| Installer (Inno Setup) for 0.4.0 | **PASS** (compiled) / **NOT RUN** (runtime install) | ISCC 6.7.3 compiled `CopperDownloadManager-0.4.0-setup.exe` from the verified deploy dir; executing it requires UAC elevation, which this environment lacks |
| Windows metadata (version resource) | **PASS** | `check_version_consistency.py` verifies FILEVERSION/PRODUCTVERSION/FileVersion/ProductVersion strings = 0.4.0 |
| Signing preparation | **BLOCKED** | No code-signing certificate available; prep delivered as documented procedure (RELEASE.md §7, commented `SignTool` scaffolding in the .iss) |
| Release validation & docs | **PASS** | This report; RELEASE.md updated; `releases/0.4.0/` populated (portable zip, installer, SHA256SUMS, release notes) |
| CI jobs | **PASS** | Runs 37835072437 and 37835776956: all 6 executed jobs green each (Windows Qt 6.6.3 Debug/Release + unit tests + CRT/startup smoke + zip + installer; Linux build + unit tests + .deb; macOS .dmg; Arch .pkg; extension lint; Publish skipped = tag-only). The first run (37833762124) failed 2 of 7 jobs — both root-caused and fixed in `747286f` (stale `PKGBUILD` pkgver; Windows-only pin assertion on Linux) rather than suppressed. As of `caec584` the pipeline also enforces `check_version_consistency.py` |
| Qt matrix documentation | **PASS** | README "Qt version matrix" (local 6.11.2 validated; CI 6.6.3) |

---

## 2. Test status (§47–53, §72 format)

The spec's testing requirements are served by three test layers plus two
validators. Overall: **no FAIL anywhere.**

| Test layer | Result | When |
|---|---|---|
| Unit tests — `copper_tests` (QtTest, 17 cases: dependency verification fail-closed contract, file-name sanitization, URL intake classification, version/User-Agent single-source, API token round trip) | **PASS — 17 passed, 0 failed** | Final build |
| Integration suite — `tests/integration_test.py` (88 checks: intake, single-instance, protocol forwarding, chunked/truncated/unknown-length, range validation, byte-exact resume, torrent injection, native-messaging pipe, playlist job model + selection contract, token auth, origin allowlist, diagnostics, crash-free relaunch) | **PASS — 88 passed, 0 failed** | Run 9 (runs 7 and 8 also 85/0 and 88/0) |
| Crash hunt — repeated full-suite runs looking for `[CRASH]`/WER evidence | **PASS — 6/6 runs clean** | Before Phase 5; runs 7–9 add three more clean full runs |
| Extension lint — `tests/validate_extensions.py` (MV3 shape, permissions, popup wiring, gecko.id, native-token handshake, HTTP dispatch, cancel path) | **PASS — Chrome OK, Firefox OK** | Final |
| Version consistency — `tools/check_version_consistency.py` | **PASS — 81 files, version 0.4.0 everywhere** | Final |
| Opt-in real-playlist cases (network, yt-dlp + ffmpeg required; expected 95 = 88 + 7) | **NOT RUN** | Requires network + installed tools; unchanged from 0.3.1 |
| CI matrix execution (Qt 6.6.3, Linux, macOS, Arch) | **PASS** | Run 37835776956 (latest), 6/6 executed jobs green incl. the pipeline-enforced version-consistency check; previous run 37835072437 equally green (first run's 2 failures root-caused and fixed in `747286f`, not suppressed) |
| Updater digest verification against a live GitHub release | **NOT RUN** | See §1 |
| Code-signing of artifacts | **BLOCKED** | No certificate |

Local manual-QA items in RELEASE.md §4 were not re-executed for this
release; every automated check above supersedes only the sections it
explicitly names.

---

## 3. Bugs found and fixed during the cycle (§71)

All were root-caused and fixed in code, not worked around:

1. **Fresh-profile aria2 auto-install wiped the launch directory**
   (`Aria2cManager::extractZipToTools`): a default-constructed `QDir`
   pointed at the process cwd and `removeRecursively()` deleted the
   app's own directory — observed as `copper_native_host.exe`
   "disappearing". Fixed with an explicit `QDir(extractDir)` +
   `mkpath`.
2. **Use-after-free in LocalServer** and **in DownloadManagerDialog**:
   signal handlers dereferenced torn-down objects; fixed with
   `QPointer` guards.
3. **Double-resume**: a second resume request re-armed an active
   download; fixed with a contains-guard + restore-loop skip.
4. **Kill-by-port**: unrelated processes owning the RPC port could be
   terminated; replaced with PID-file + image-identity-checked targeted
   shutdown (P0 requirement above).
5. **`tests/validate_extensions.py` drifted** from the shipped design
   (still asserted the pre-token "no native messaging" world) and failed
   for both browsers; updated to require the `nativeMessaging`
   permission and the `getToken` handshake the API-token feature needs.
6. **`check_version_consistency.py` false positives**: comments that
   *discussed* stale UA literals verbatim tripped the forbidden-literal
   scan; comments rephrased (behavior of the checker unchanged).
7. Build/test-hygiene fixes en route: most-vexing-parse in the RPC
   worker, AUTOMOC header list for the new test target, two incorrect
   unit-test expectations (a `\x01c` hex-escape swallow; the sanitizer's
   fallback semantics).
8. **Concurrent-writer incident**: a second automated session edited
   then rolled back parts of this working tree (MEGAsync + OneDrive both
   sync it). Mitigation used: every phase verified by fresh re-reads and
   committed immediately as a checkpoint; `.git` marker files untouched.

---

## 4. Artifacts

`releases/0.4.0/` contains:

| File | Notes |
|---|---|
| `CopperDownloadManager-portable-0.4.0.zip` | 25 entries, contents at zip root (CI-identical layout), integrity-verified |
| `CopperDownloadManager-0.4.0-setup.exe` | Inno Setup 6.7.3, lzma2, payload = `installer/release/0.4.0/` (the directory the 88-check suite validated) |
| `SHA256SUMS` | `sha256sum -c`-compatible |
| `RELEASE-NOTES.md` | User-facing notes |

Deploy directory contents verified against RELEASE.md §5: app exe,
`copper_native_host.exe`, 9 Qt/runtime DLLs + 12 plugin DLLs across
`platforms/`, `imageformats/`, `iconengines/`, `generic/`, `styles/`,
`sqldrivers/`, `tls/`, `networkinformation/`, MinGW runtimes
(`libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`) and
`THIRD-PARTY-NOTICES.txt`.

Build configuration (the validated one): Qt 6.11.2 `mingw_64`,
MinGW 13.1.0, CMake + Ninja, `Release` with `-O2 -g`, windres +
`fix_version_resource.py` preserved.

## 5. Commit series (14 commits, `64c2468` → `caec584`, pushed to `origin/main`)

| Commit | Content |
|---|---|
| `5e19f54` | feat(version): 0.4.0 single source of truth, centralized UA |
| `01fb22f` | fix(core): range/chunk validation, integrity-safe resume |
| `cfb0732` | fix(tools): stop aria2 auto-install from wiping the launch dir |
| `50664ba` | feat(api): per-install token on the local API |
| `a37a8a5` | feat(extensions): fetch and attach the local API token |
| `00ecb56` | test(integration): token-aware harness with full isolation |
| `197bfe1` | feat(security): verify dependency downloads, stop killing by port |
| `1b183df` | feat(reliability): disk guard, conflict policy, HTTP classification, proxy, custom headers, DB safety, diagnostics |
| `e357f3d` | test(unit): QtTest target + validator drift fixes |
| `0c854c1` | docs(release): implementation report, release-guide refresh, signing prep |
| `d061111` | release: stage the 0.4.0 artifacts in releases/0.4.0/ |
| `747286f` | fix(ci): PKGBUILD version, platform-aware pin test, failure-visible test steps |
| `2311a34` | docs(report): record the executed CI matrix result |
| `caec584` | ci: enforce the single-source version check in the pipeline |

## 6. Breaking change

The local API now requires `X-Copper-Token`. In-repo consumers
(extensions, integration harness) were updated in lockstep; the
`copper://` flow is unchanged. No other externally visible behavior was
removed or renamed.

## 7. Open items / follow-ups

- ~~Push~~ — done: all 14 commits are on `origin/main` and the CI
  matrix has executed and passed (latest run 37835776956, 6/6 jobs).
- ~~`releases/0.4.0/` placement~~ — decided: the artifacts are tracked
  in the repository; `.gitignore` documents the `*.zip`/`*.exe`
  force-add exception.
- **Registry contamination under test profile**: `MainWindow` still
  touches `QSettings("Copper","DownloadManager")`; harmless in practice
  but not redirected under `--test-profile`.
- **NOT RUN items in §1/§2** are the honest gap list for this cycle;
  the cheapest next gains are harness cases for the file-conflict
  policy and a fault-injected migration rollback.
