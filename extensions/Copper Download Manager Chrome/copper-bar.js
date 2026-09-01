// IDM-style media detection bar for Copper Download Manager (content script).
// Runs on http(s) pages in the top frame only. Handy.scans for <video>/<audio>
// elements and direct media links, then shows a small floating bar with
// Download buttons that hand the media URL to the extension background, which
// routes it to the desktop app (native host first, HTTP API as fallback).

(() => {
  "use strict";

  if (window.top !== window.self) return; // avoid duplicated bars from iframes

  const JSON_OK = (v) => !v || v === undefined || v === null || v === "";

  const VIDEO_EXTS = ["mp4", "webm", "mkv", "mov", "m4v", "m3u8"];
  const AUDIO_EXTS = ["mp3", "m4a", "wav", "flac", "aac", "ogg", "opus"];
  const YTDLP_HOSTS = [
    "youtube.com", "youtu.be", "vimeo.com", "dailymotion.com", "tiktok.com",
    "twitter.com", "x.com", "instagram.com", "facebook.com", "reddit.com",
    "rumble.com", "soundcloud.com", "twitch.tv", "odysee.com", "bitchute.com",
    "dlive.tv",
  ];

  const media = new Map(); // url -> { url, kind, ext, title }

  function normalizeUrl(raw) {
    try {
      return new URL(raw, location.href).href.replace(/\s+/g, "%20");
    } catch (e) {
      return null;
    }
  }

  // Returns { kind, ext } when the URL path ends in a media extension.
  function mediaShape(url) {
    if (!url) return null;
    const noQuery = url.split("#")[0].split("?")[0].toLowerCase();
    const m = noQuery.match(/\.([a-z0-9]+)$/);
    if (!m) return null;
    const ext = m[1];
    if (VIDEO_EXTS.includes(ext)) return { kind: "video", ext };
    if (AUDIO_EXTS.includes(ext)) return { kind: "audio", ext };
    return null;
  }

  function filenameOf(u) {
    try {
      const path = new URL(u).pathname.split("/").pop().split("#")[0].split("?")[0];
      if (path) return decodeURIComponent(path);
    } catch (e) { /* fall through */ }
    return "media";
  }

  function addMedia(url) {
    if (!url) return;
    const norm = normalizeUrl(url);
    if (!norm || media.has(norm)) return;
    const shape = mediaShape(norm);
    if (!shape) return;
    media.set(norm, { url: norm, kind: shape.kind, ext: shape.ext, title: filenameOf(norm) });
  }

  // Whether this page is a known yt-dlp site (e.g. YouTube). For those, offer
  // the page URL itself so the desktop app can resolve the best stream.
  function isYtDlpPage() {
    const host = location.hostname.toLowerCase();
    return YTDLP_HOSTS.some((h) => host === h || host.endsWith("." + h));
  }

  function collect() {
    media.clear();

    for (const el of document.querySelectorAll("video, audio")) {
      if (el.currentSrc) addMedia(el.currentSrc);
      else if (el.src) addMedia(el.src);
    }
    for (const src of document.querySelectorAll("video source, audio source")) {
      if (src.src) addMedia(src.src);
    }
    for (const a of document.querySelectorAll("a[href]")) {
      addMedia(a.href || a.href);
    }

    if (media.size === 0 && isYtDlpPage()) {
      media.set(location.href, {
        url: location.href,
        kind: "video",
        ext: "ytdlp",
        title: (document.title || "page video").slice(0, 60),
      });
    }
  }

  // ------------------------------------------------------------------ UI ---

  let barRoot = null;
  let closed = false;
  let lastKey = "";

  function mediaKey() {
    return Array.from(media.values()).map((m) => m.url).join("|");
  }

  function injectStyles() {
    if (document.getElementById("copperDmStyle")) return;
    const style = document.createElement("style");
    style.id = "copperDmStyle";
    style.textContent = `
      #copperDmBar, #copperDmBar * { box-sizing: border-box; }
      #copperDmBar {
        all: initial;
        position: fixed; top: 12px; right: 12px; z-index: 2147483647;
        width: 300px; max-width: calc(100vw - 24px);
        background: #202124; color: #e8eaed;
        font: 13px/1.4 system-ui, -apple-system, Segoe UI, Roboto, sans-serif;
        border: 1px solid #3c4043; border-radius: 8px;
        box-shadow: 0 8px 24px rgba(0,0,0,.45);
        overflow: hidden;
      }
      #copperDmBar .cdm-head {
        display: flex; align-items: center; justify-content: space-between;
        padding: 8px 10px; background: #292b2e;
        font-weight: 600; font-size: 12px; color: #9aa0a6;
      }
      #copperDmBar .cdm-close {
        all: initial; cursor: pointer; color: #9aa0a6; font: 16px/1 sans-serif;
        padding: 0 4px; border-radius: 4px;
      }
      #copperDmBar .cdm-close:hover { color: #fff; background: rgba(255,255,255,.08); }
      #copperDmBar .cdm-items { max-height: 260px; overflow-y: auto; }
      #copperDmBar .cdm-row {
        display: flex; align-items: center; gap: 8px;
        padding: 7px 10px; border-top: 1px solid #3c4043;
      }
      #copperDmBar .cdm-name {
        flex: 1; min-width: 0; color: #e8eaed;
        overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
      }
      #copperDmBar .cdm-actions { display: flex; gap: 6px; }
      #copperDmBar .cdm-btn {
        all: initial; cursor: pointer; white-space: nowrap;
        background: #d2691e; color: #fff;
        font: 600 11px/1 sans-serif; padding: 5px 9px; border-radius: 4px;
      }
      #copperDmBar .cdm-btn:hover { background: #b5571a; }
      #copperDmBar .cdm-btn.cdm-audio { background: #1a73e8; }
      #copperDmBar .cdm-btn.cdm-audio:hover { background: #1765cc; }
      #copperDmBar .cdm-btn.cdm-muted { background: #5f6368; }
      #copperDmBar .cdm-btn.cdm-muted:hover { background: #3c4043; }
      #copperDmBar .cdm-empty {
        padding: 12px; color: #9aa0a6; text-align: center;
      }
    `;
    document.documentElement.appendChild(style);
  }

  function buildBar() {
    injectStyles();
    barRoot = document.createElement("div");
    barRoot.id = "copperDmBar";

    const head = document.createElement("div");
    head.className = "cdm-head";
    const title = document.createElement("span");
    title.textContent = "Download with Copper";
    const close = document.createElement("button");
    close.className = "cdm-close";
    close.textContent = "\u00d7";
    close.title = "Hide";
    close.addEventListener("click", () => { closed = true; hideBar(); });
    head.appendChild(title);
    head.appendChild(close);

    const items = document.createElement("div");
    items.className = "cdm-items";
    barRoot.appendChild(head);
    barRoot.appendChild(items);
    document.documentElement.appendChild(barRoot);

    const io = new IntersectionObserver((entries) => {
      io.disconnect();
      for (const e of entries) {
        if (e.intersectionRatio <= 0) {
          barRoot.style.right = "28px";
        }
      }
    });
    io.observe(barRoot);
  }

  function render() {
    if (!barRoot || closed || media.size === 0) {
      hideBar();
      return;
    }
    const key = mediaKey();
    if (key === lastKey) return; // unchanged; keep existing rows
    lastKey = key;

    const itemsEl = barRoot.querySelector(".cdm-items");
    itemsEl.textContent = "";

    const shown = Array.from(media.values()).slice(0, 4);
    const extra = media.size - shown.length;

    for (const item of shown) {
      const row = document.createElement("div");
      row.className = "cdm-row";

      const name = document.createElement("span");
      name.className = "cdm-name";
      name.textContent = item.title;
      name.title = item.url;
      row.appendChild(name);

      const actions = document.createElement("span");
      actions.className = "cdm-actions";

      const send = (format) => {
        try {
          chrome.runtime.sendMessage(
            { action: "sendUrl", url: item.url, filename: item.title, format: format },
            () => { void chrome.runtime.lastError; }
          );
        } catch (e) { /* noop */ }
      };

      // Video: offer both the raw video (MP4) and an extracted audio (MP3),
      // IDM-style. Audio sources only get the MP3 option.
      if (item.kind === "video") {
        const mp4 = document.createElement("button");
        mp4.className = "cdm-btn";
        mp4.textContent = "Download MP4";
        mp4.addEventListener("click", () => send("mp4"));
        actions.appendChild(mp4);
      }

      const mp3 = document.createElement("button");
      mp3.className = item.kind === "audio" ? "cdm-btn cdm-audio" : "cdm-btn cdm-muted";
      mp3.textContent = "Download MP3";
      mp3.title = item.kind === "audio"
        ? "Download the audio file"
        : "Extract MP3 audio (requires FFmpeg)";
      mp3.addEventListener("click", () => send("mp3"));
      actions.appendChild(mp3);

      row.appendChild(actions);
      itemsEl.appendChild(row);
    }

    if (extra > 0) {
      const more = document.createElement("div");
      more.className = "cdm-row";
      more.textContent = "+" + extra + " more media file" + (extra === 1 ? "" : "s");
      more.style.color = "#9aa0a6";
      itemsEl.appendChild(more);
    }

    barRoot.style.display = "block";
  }

  function hideBar() {
    if (barRoot) barRoot.style.display = "none";
  }

  function init() {
    if (closed || barRoot) return;
    collect();
    if (media.size === 0) return;
    buildBar();
    render();
  }

  // Re-scan periodically for lazy-loaded / dynamically added media.
  setInterval(() => { if (closed) return; init(); }, 2500);

  chrome.runtime.sendMessage({ action: "getStatus" }, (resp) => {
    if (chrome.runtime.lastError || !resp) return;
    if (!resp.enabled || !resp.installed) return;
    init();
  });
})();