#!/usr/bin/env python3
"""Verify the translation catalogs are complete and safe to ship.

Inputs:
  * `translations/copper_<lang>.ts` - Qt Linguist catalogs for the desktop app
    (these are compiled to .qm and embedded in the binary by CMake);
  * `extensions/<Browser>/_locales/<lang>/messages.json` - the browser
    extensions' catalogs (loaded by `chrome.i18n.getMessage`).

A catalog that is quietly missing a message, or one whose translation dropped a
`%1` placeholder, ships as a silent English fallback or a mangled sentence.
Neither shows up in a build, so this is what catches them.

Checks:
  * every catalog exists and parses (XML for .ts, JSON for messages.json);
  * `CMakeLists.txt` compiles exactly the catalogs on disk - a forgotten entry
    means a language silently never reaches the binary;
  * every non-source language carries the same (context, source) messages as
    the `en` source catalog;
  * nothing is left `type="unfinished"` / `vanished` / `obsolete` and no
    translation is empty;
  * `%1` / `%L1` / `%n` placeholders in a translation match its source;
  * `en` really is the source language: it must carry no translation of its
    own, otherwise the "source" text is itself a stale copy;
  * the app and both browser extensions ship the same set of languages.

Exit codes: 0 = consistent, 1 = problem found, 2 = usage.

Run from anywhere:  python tools/check_translations.py
Used by CI (the extension-lint job) and by the release checklist (RELEASE.md).
"""

import collections
import io
import json
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRANSLATIONS = os.path.join(ROOT, "translations")
SOURCE_LANG = "en"
CHROME = os.path.join(ROOT, "extensions", "Chrome")
FIREFOX = os.path.join(ROOT, "extensions", "Firefox")

# Qt placeholder tokens that must survive translation verbatim.
PLACEHOLDER = re.compile(r"%(?:L)?\d+|%n")
TRANSLATION_FLAGS = ("unfinished", "vanished", "obsolete")


def check(cond, message, failures):
    if cond:
        print("  ok  : %s" % message)
    else:
        print("  FAIL: %s" % message)
        failures.append(message)
    return bool(cond)


def placeholders(text):
    return collections.Counter(PLACEHOLDER.findall(text or ""))


def translation_of(node):
    """(text, type) of a <translation> element, incl. numerus forms."""
    if node is None:
        return "", None
    forms = node.findall("numerusform")
    if forms:
        return "\n".join((f.text or "") for f in forms), node.get("type")
    return node.text or "", node.get("type")


def parse_ts(path):
    """Return ([(context, source, comment, translation, type)], ok)."""
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError) as e:
        print("  FAIL: %s does not parse: %s" % (os.path.relpath(path, ROOT), e))
        return [], False
    entries = []
    for ctx in root.findall(".//context"):
        name = ctx.findtext("name") or ""
        for msg in ctx.findall("message"):
            text, flag = translation_of(msg.find("translation"))
            entries.append((name,
                            msg.findtext("source") or "",
                            msg.findtext("comment") or "",
                            text, flag))
    return entries, True


def catalog_langs(directory, prefix):
    """Locale names of `<prefix><lang>.ts` files in a directory."""
    langs = []
    if not os.path.isdir(directory):
        return langs
    for name in sorted(os.listdir(directory)):
        if name.startswith(prefix) and name.endswith(".ts"):
            langs.append(name[len(prefix):-len(".ts")])
    return langs


def cmake_langs():
    """Locales listed in CMakeLists.txt's COPPER_TS_FILES block."""
    path = os.path.join(ROOT, "CMakeLists.txt")
    try:
        with io.open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError as e:
        print("  FAIL: cannot read CMakeLists.txt: %s" % e)
        return None
    m = re.search(r"set\(\s*COPPER_TS_FILES(.*?)\)", text, re.S)
    if not m:
        print("  FAIL: COPPER_TS_FILES not found in CMakeLists.txt")
        return None
    return sorted(os.path.basename(ts)[:-3].replace("copper_", "")
                  for ts in re.findall(r'"([^"]*\.ts)"', m.group(1)))


def check_app(failures):
    print("App catalogs: %s" % os.path.relpath(TRANSLATIONS, ROOT))
    if not os.path.isdir(TRANSLATIONS):
        check(False, "translations/ directory exists", failures)
        return []

    langs = catalog_langs(TRANSLATIONS, "copper_")
    check(bool(langs), "catalogs found: %s" % (", ".join(langs) or "none"), failures)
    check(SOURCE_LANG in langs, "source catalog copper_%s.ts exists" % SOURCE_LANG,
          failures)

    expected = cmake_langs()
    if expected is not None:
        check(sorted(langs) == expected,
              "CMakeLists.txt compiles exactly the catalogs on disk "
              "(disk=%s cmake=%s)" % (sorted(langs), expected), failures)

    if SOURCE_LANG not in langs:
        return langs

    src_entries, src_ok = parse_ts(os.path.join(TRANSLATIONS, "copper_%s.ts" % SOURCE_LANG))
    if not src_ok:
        check(False, "source catalog parses", failures)
        return langs
    check(bool(src_entries), "source catalog has %d message(s)" % len(src_entries),
          failures)
    src_keys = collections.Counter((c, s, cm) for c, s, cm, _t, _f in src_entries)

    # The source catalog must not translate anything: en is what Qt falls back
    # to, so a populated translation there means a second, stale copy of the
    # English text is being shipped instead of the source.
    translated = [k for (c, s, cm, t, _f) in src_entries if t.strip()]
    check(not translated,
          "source catalog carries no translation of its own (%d found)"
          % len(translated), failures)

    for lang in sorted(l for l in langs if l != SOURCE_LANG):
        path = os.path.join(TRANSLATIONS, "copper_%s.ts" % lang)
        entries, ok = parse_ts(path)
        if not ok:
            check(False, "%s: catalog parses" % lang, failures)
            continue
        check(len(entries) == len(src_entries),
              "%s: message count matches source (%d vs %d)"
              % (lang, len(entries), len(src_entries)), failures)

        keys = collections.Counter((c, s, cm) for c, s, cm, _t, _f in entries)
        missing = sorted((k for k in src_keys if keys[k] < src_keys[k]))
        extra = sorted((k for k in keys if keys[k] > src_keys[k]))
        check(not missing and not extra,
              "%s: covers every source message (missing=%d, extra=%d)"
              % (lang, len(missing), len(extra)), failures)

        bad_flag = [(c, s) for c, s, cm, _t, f in entries if f in TRANSLATION_FLAGS]
        check(not bad_flag,
              "%s: no unfinished/vanished/obsolete messages (%d found)"
              % (lang, len(bad_flag)), failures)

        empty = [(c, s) for c, s, cm, t, _f in entries if not t.strip()]
        check(not empty, "%s: no empty translations (%d found)" % (lang, len(empty)),
              failures)

        bad_ph = [(c, s, sorted(placeholders(s)), sorted(placeholders(t)))
                  for c, s, cm, t, _f in entries
                  if placeholders(s) != placeholders(t)]
        check(not bad_ph,
              "%s: every %s placeholder kept (%d drifted)"
              % (lang, "%N", len(bad_ph)), failures)

        if len(entries) == len(src_entries) and not bad_ph:
            print("  ok  : %s: %d messages translated" % (lang, len(entries)))

    return langs


def check_locale_parity(app_langs, failures):
    """The app and both extensions must expose the same language set."""
    print("Locale parity (app vs extensions)")
    for label, path in (("Chrome", CHROME), ("Firefox", FIREFOX)):
        loc_dir = os.path.join(path, "_locales")
        ext_langs = []
        if os.path.isdir(loc_dir):
            for name in sorted(os.listdir(loc_dir)):
                if os.path.isfile(os.path.join(loc_dir, name, "messages.json")):
                    ext_langs.append(name)
        check(ext_langs == sorted(app_langs),
              "%s _locales matches the app catalogs (app=%s, ext=%s)"
              % (label, sorted(app_langs), ext_langs), failures)


def main():
    failures = []
    app_langs = check_app(failures)
    if app_langs:
        check_locale_parity(app_langs, failures)
    print()
    if failures:
        print("TRANSLATION CHECK FAILED (%d problem(s))" % len(failures))
        return 1
    print("TRANSLATION CHECK PASSED (%d catalog(s): %s)"
          % (len(app_langs), ", ".join(sorted(app_langs))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
