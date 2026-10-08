#!/usr/bin/env python3
"""Verify the release version is consistent across every file that mirrors it.

Authoritative source: `project(<name> VERSION x.y.z)` in CMakeLists.txt.
CMake regenerates cmake/copper_version.h.in from it, and main.cpp sets
QCoreApplication::applicationVersion() from COPPER_VERSION_STRING, so the
runtime version needs no manual syncing. This script verifies the mirrors
CMake cannot reach:

  * app.rc                      FILEVERSION / PRODUCTVERSION / string values
  * installer/CopperDownloadManager.iss  MyAppVersion + release\\<ver>\\ source path
  * THIRD-PARTY-NOTICES.txt     "COPPER DOWNLOAD MANAGER vX.Y.Z" header
  * main.cpp                    must call setApplicationVersion(COPPER_VERSION_STRING)
  * everywhere                  no forbidden stale literals:
                                - "CopperDownloadManager/<number>..." (hardcoded UA)
                                - previous release versions (see PREVIOUS_RELEASES)

Exit codes: 0 = consistent, 1 = mismatch/forbidden literal found, 2 = usage.

Run from anywhere:  python tools/check_version_consistency.py
Used by CI and by the release checklist (see RELEASE.md).
"""
import io
import os
import re
import subprocess
import sys

# Previous released versions that must no longer appear anywhere in tracked
# source/docs. Append to this list when cutting a release (0.4.0's list still
# contains 0.3.1 until the next release supersedes it).
PREVIOUS_RELEASES = ("0.3.1",)

# Historical documents that legitimately name old versions and old UA strings
# when describing the release history (source and machine files must not).
HISTORY_DOCS = ("OPENCODE_TODO.md",
                "IMPLEMENTATION_REPORT_0.4.0.md",
                "releases/0.4.0/RELEASE-NOTES.md")

# Files scanned for stale/forbidden version literals (text files only).
SCAN_DIRS = ("src", "include", "nativehost", "tests", "tools", "installer",
             "extensions", "cmake", ".github")
SCAN_FILES = ("main.cpp", "app.rc", "CMakeLists.txt", "README.md",
              "RELEASE.md", "THIRD-PARTY-NOTICES.txt", "resources.qrc")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(path):
    with io.open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def authoritative_version():
    text = read(os.path.join(ROOT, "CMakeLists.txt"))
    m = re.search(r"project\(\s*\w+\s+VERSION\s+(\d+\.\d+\.\d+)", text)
    if not m:
        print("FAIL: could not parse `project(... VERSION x.y.z)` from CMakeLists.txt")
        return None
    return m.group(1)


def tracked_files():
    """All tracked text files, via git when available (skips build outputs)."""
    try:
        out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT,
                             capture_output=True, check=True).stdout
        files = [f.decode("utf-8", "replace") for f in out.split(b"\0") if f]
    except Exception:
        files = []
        for d in SCAN_DIRS:
            for base, _dirs, names in os.walk(os.path.join(ROOT, d)):
                for n in names:
                    files.append(os.path.relpath(os.path.join(base, n), ROOT))
        for f in SCAN_FILES:
            if os.path.exists(os.path.join(ROOT, f)):
                files.append(f)
    return files


def check(cond, message, failures):
    if cond:
        print("  ok  : %s" % message)
    else:
        print("  FAIL: %s" % message)
        failures.append(message)


def main():
    ver = authoritative_version()
    if ver is None:
        return 1
    print("authoritative version (CMakeLists.txt): %s" % ver)
    major, minor, patch = ver.split(".")
    rc_dotted = "%s,%s,%s,0" % (major, minor, patch)
    failures = []

    # --- app.rc -------------------------------------------------------------
    rc = read(os.path.join(ROOT, "app.rc"))
    check(("FILEVERSION " + rc_dotted) in rc,
          "app.rc FILEVERSION %s" % rc_dotted, failures)
    check(("PRODUCTVERSION " + rc_dotted) in rc,
          "app.rc PRODUCTVERSION %s" % rc_dotted, failures)
    check('VALUE "FileVersion", "%s"' % ver in rc,
          'app.rc FileVersion string "%s"' % ver, failures)
    check('VALUE "ProductVersion", "%s"' % ver in rc,
          'app.rc ProductVersion string "%s"' % ver, failures)

    # --- Inno Setup ---------------------------------------------------------
    iss_path = os.path.join(ROOT, "installer", "CopperDownloadManager.iss")
    iss = read(iss_path)
    check('#define MyAppVersion "%s"' % ver in iss,
          "iss MyAppVersion %s" % ver, failures)
    check(re.search(r'release\\%s\\' % re.escape(ver), iss) is not None,
          "iss source path release\\%s\\" % ver, failures)

    # --- THIRD-PARTY-NOTICES ------------------------------------------------
    notices = read(os.path.join(ROOT, "THIRD-PARTY-NOTICES.txt"))
    check(notices.startswith("COPPER DOWNLOAD MANAGER v%s" % ver),
          "THIRD-PARTY-NOTICES header v%s" % ver, failures)

    # --- main.cpp single source --------------------------------------------
    main_cpp = read(os.path.join(ROOT, "main.cpp"))
    check("setApplicationVersion(QStringLiteral(COPPER_VERSION_STRING))" in main_cpp,
          "main.cpp uses COPPER_VERSION_STRING (single source)", failures)
    check(re.search(r"setApplicationVersion\(\s*\"", main_cpp) is None,
          "main.cpp has no literal setApplicationVersion(\"...\")", failures)

    # --- forbidden literals in tracked source -------------------------------
    forbidden = [("CopperDownloadManager/", r"CopperDownloadManager/\d")]
    forbidden += [(v, re.escape(v)) for v in PREVIOUS_RELEASES]
    scanned = 0
    for rel in tracked_files():
        # Extensionless files can mirror the version too (e.g. PKGBUILD's
        # pkgver) - match them by name, since they have no suffix to key on.
        if os.path.basename(rel) not in ("PKGBUILD",) and not re.search(
                r"\.(cpp|h|hpp|c|cc|rc|txt|md|iss|in|py|yml|yaml|json|"
                r"cmake|qrc|sh|ps1|desktop|plist|in)$", rel):
            continue
        if rel == "tools/check_version_consistency.py":
            continue  # this script legitimately names the forbidden literals
        try:
            text = read(os.path.join(ROOT, rel))
        except OSError:
            continue
        scanned += 1
        if rel in HISTORY_DOCS:
            continue  # history docs legitimately name old versions/UA strings
        for label, pattern in forbidden:
            if re.search(pattern, text):
                print("  FAIL: %s contains forbidden literal %r" % (rel, label))
                failures.append("%s: stale %s" % (rel, label))

    print("scanned %d tracked text file(s)" % scanned)
    if failures:
        print("\nVERSION CHECK FAILED (%d problem(s))" % len(failures))
        return 1
    print("\nVERSION CHECK PASSED (version %s consistent everywhere)" % ver)
    return 0


if __name__ == "__main__":
    sys.exit(main())
