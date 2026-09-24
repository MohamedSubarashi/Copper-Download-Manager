// Copper Download Manager - media detection bar (content script).
//
// "Media radar" for http(s) pages (top frame only). Finds <video>/<audio>
// elements, <source> entries, HLS/DASH streams, direct media links, og:/meta
// video tags, JSON-LD Video/Audio objects and playlists on known sites, then
// floats a small bar with per-item Download MP4 / Download MP3 / Copy actions
// plus a "Download all page media" batch button. It talks to the background
// service worker (sendUrl/sendBatch), which routes everything to the desktop
// app via the native host with an HTTP fallback.

(() => {
  "use strict";

  if (window.top !== window.self) return;

  const api =
    typeof browser !== "undefined" && browser.runtime ? browser : chrome;

  const VIDEO_EXTS = ["mp4", "webm", "mkv", "mov", "m4v", "m3u8", "mpd"];
  const AUDIO_EXTS = ["mp3", "m4a", "wav", "flac", "aac", "ogg", "opus"];
  const YTDLP_HOSTS = [
    "youtube.com", "youtu.be", "vimeo.com", "dailymotion.com", "tiktok.com",
    "twitter.com", "x.com", "instagram.com", "facebook.com", "reddit.com",
    "rumble.com", "soundcloud.com", "twitch.tv", "odysee.com", "bitchute.com",
    "dlive.tv", "streamable.com", "vk.com", "bilibili.com", "dailymail.co.uk",
  ];

  const media = new Map();
  let playlistInfo = null;

  function normalizeUrl(raw) {
    try {
      return new URL(raw, location.href).href.replace(/\s+/g, "%20");
    } catch (e) {
      return null;
    }
  }

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

  function titleOf(el) {
    const t = (el && (el.getAttribute("title") || el.getAttribute("aria-label")) || "").trim();
    return t || (document.title || "").slice(0, 60);
  }

  function isYtDlpPage() {
    const host = location.hostname.toLowerCase();
    return YTDLP_HOSTS.some((h) => host === h || host.endsWith("." + h));
  }

  function detectPlaylist() {
    const host = location.hostname.toLowerCase();
    const href = location.href;
    const params = new URLSearchParams(location.search);

    if (host.includes("youtube.com") || host.includes("youtu.be")) {
      if (params.has("list")) {
        const title = (document.title || "").replace(/ - YouTube$/, "").trim() || "YouTube Playlist";
        return { url: href, title };
      }
    }
    if (host.includes("soundcloud.com") && href.includes("/sets/")) {
      const title = (document.title || "").replace(/ \| SoundCloud$/, "").trim() || "SoundCloud Set";
      return { url: href, title };
    }
    if (host.includes("spotify.com") && href.includes("/playlist")) {
      const title = (document.title || "").replace(/ - Spotify$/, "").trim() || "Spotify Playlist";
      return { url: href, title };
    }
    if (host.includes("music.apple.com") && href.includes("/playlist")) {
      const title = (document.title || "").replace(/ - Apple Music$/, "").trim() || "Apple Music Playlist";
      return { url: href, title };
    }
    return null;
  }

  function collect() {
    media.clear();
    playlistInfo = null;

    for (const el of document.querySelectorAll("video, audio")) {
      if (el.currentSrc) addMedia(el.currentSrc);
      else if (el.src) addMedia(el.src);
    }
    for (const src of document.querySelectorAll("video source, audio source")) {
      if (src.src) addMedia(src.src);
    }
    for (const a of document.querySelectorAll("a[href]")) {
      const href = (a.getAttribute("href") || "").trim();
      if (href.startsWith("#") || href.startsWith("javascript:")) continue;
      addMedia(href ? new URL(href, location.href).href : href);
    }

    // og:/twitter: meta video/audio tags.
    for (const meta of document.querySelectorAll("meta[property], meta[name]")) {
      const prop = (meta.getAttribute("property") || meta.getAttribute("name") || "").toLowerCase();
      if (/^(og:video|og:audio|twitter:player:stream|twitter:player|video:tag|music:song|music:album)/.test(prop)) {
        addMedia(meta.content);
      }
    }

    // JSON-LD Video/Audio objects.
    for (const el of document.querySelectorAll('script[type="application/ld+json"]')) {
      try {
        const data = JSON.parse(el.textContent);
        const nodes = Array.isArray(data) ? data : [data];
        const scan = (node, depth) => {
          if (!node || depth > 6) return;
          if (Array.isArray(node)) { node.forEach((n) => scan(n, depth + 1)); return; }
          if (typeof node === "object") {
            if (node["@type"]) {
              const type = String(node["@type"]).toLowerCase();
              if (type === "videoobject" && node.contentUrl) addMedia(node.contentUrl);
              if (type === "videoobject" && node.embedUrl) addMedia(node.embedUrl);
              if (type === "audioobject" && node.contentUrl) addMedia(node.contentUrl);
            }
            for (const k of Object.keys(node)) scan(node[k], depth + 1);
          }
        };
        scan(nodes, 0);
      } catch (e) { /* malformed JSON-LD */ }
    }

    playlistInfo = detectPlaylist();

    if (media.size === 0 && isYtDlpPage() && !playlistInfo) {
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
    const parts = Array.from(media.values()).map((m) => m.url);
    if (playlistInfo) parts.push("playlist:" + playlistInfo.url);
    return parts.join("|");
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
        width: 320px; max-width: calc(100vw - 24px);
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
      #copperDmBar .cdm-head-actions { display: flex; align-items: center; gap: 6px; }
      #copperDmBar .cdm-iconbtn {
        all: initial; cursor: pointer; color: #9aa0a6; font: 15px/1 sans-serif;
        padding: 2px 6px; border-radius: 4px;
      }
      #copperDmBar .cdm-iconbtn:hover { color: #fff; background: rgba(255,255,255,.08); }
      #copperDmBar .cdm-items { max-height: 340px; overflow-y: auto; }
      #copperDmBar .cdm-row {
        display: flex; align-items: center; gap: 8px;
        padding: 7px 10px; border-top: 1px solid #3c4043;
      }
      #copperDmBar .cdm-name {
        flex: 1; min-width: 0; color: #e8eaed;
        overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
      }
      #copperDmBar .cdm-actions { display: flex; gap: 6px; flex-wrap: wrap; justify-content: flex-end; }
      #copperDmBar .cdm-btn {
        all: initial; cursor: pointer; white-space: nowrap;
        background: #d2691e; color: #fff;
        font: 600 11px/1 sans-serif; padding: 5px 9px; border-radius: 4px;
      }
      #copperDmBar .cdm-btn:hover { background: #b5571a; }
      #copperDmBar .cdm-btn.cdm-audio { background: #1a73e8; }
      #copperDmBar .cdm-btn.cdm-audio:hover { background: #1765cc; }
      #copperDmBar .cdm-btn.cdm-copy { background: #5f6368; }
      #copperDmBar .cdm-btn.cdm-copy:hover { background: #3c4043; }
      #copperDmBar .cdm-btn.cdm-playlist { background: #7b1fa2; }
      #copperDmBar .cdm-btn.cdm-playlist:hover { background: #6a1b9a; }
      #copperDmBar .cdm-btn.cdm-playlist-audio { background: #0277bd; }
      #copperDmBar .cdm-btn.cdm-playlist-audio:hover { background: #01579b; }
      #copperDmBar .cdm-btn.cdm-all { background: #188038; width: 100%; }
      #copperDmBar .cdm-btn.cdm-all:hover { background: #146c2e; }
      #copperDmBar .cdm-empty { padding: 12px; color: #9aa0a6; text-align: center; }
      #copperDmBar .cdm-separator { border: none; border-top: 1px solid #5f6368; margin: 0 10px; }
      #copperDmBar .cdm-playlist-label {
        padding: 7px 10px 2px; color: #b39ddb; font-size: 11px;
        font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px;
      }
      #copperDmBar .cdm-more {
        padding: 7px 10px; color: #9aa0a6; font-size: 11px;
        border-top: 1px solid #3c4043;
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

    const headActions = document.createElement("span");
    headActions.className = "cdm-head-actions";

    const refresh = document.createElement("button");
    refresh.className = "cdm-iconbtn";
    refresh.textContent = "\u21bb";
    refresh.title = "Re-scan this page";
    refresh.addEventListener("click", () => { lastKey = ""; init(); });

    const close = document.createElement("button");
    close.className = "cdm-iconbtn";
    close.textContent = "\u00d7";
    close.title = "Hide";
    close.addEventListener("click", () => { closed = true; hideBar(); });

    headActions.appendChild(refresh);
    headActions.appendChild(close);
    head.appendChild(title);
    head.appendChild(headActions);

    const items = document.createElement("div");
    items.className = "cdm-items";
    barRoot.appendChild(head);
    barRoot.appendChild(items);
    document.documentElement.appendChild(barRoot);

    const io = new IntersectionObserver((entries) => {
      io.disconnect();
      for (const e of entries) {
        if (e.intersectionRatio <= 0) barRoot.style.right = "28px";
      }
    });
    io.observe(barRoot);
  }

  function setBtnState(btn, text, busy) {
    btn.disabled = !!busy;
    btn.textContent = text;
  }

  function send(btn, url, filename, format, doneText) {
    setBtnState(btn, "Sending\u2026", true);
    try {
      api.runtime.sendMessage(
        { action: "sendUrl", url, filename, format },
        (resp) => {
          void api.runtime.lastError;
          const ok = resp && resp.success;
          setBtnState(btn, ok ? (doneText || "Sent \u2713") : "Copper off / failed", false);
          if (ok) setTimeout(() => setBtnState(btn, doneText || "Sent \u2713", false), 1500);
        }
      );
    } catch (e) {
      setBtnState(btn, "Copper off / failed", false);
    }
  }

  function rowFor(item) {
    const row = document.createElement("div");
    row.className = "cdm-row";

    const name = document.createElement("span");
    name.className = "cdm-name";
    name.textContent = item.title;
    name.title = item.url;
    row.appendChild(name);

    const actions = document.createElement("span");
    actions.className = "cdm-actions";

    const copy = document.createElement("button");
    copy.className = "cdm-btn cdm-copy";
    copy.textContent = "Copy";
    copy.title = "Copy media link";
    copy.addEventListener("click", () => {
      try { navigator.clipboard.writeText(item.url); } catch (e) { /* noop */ }
      setBtnState(copy, "Copied \u2713", false);
    });
    actions.appendChild(copy);

    if (item.kind === "video") {
      const mp4 = document.createElement("button");
      mp4.className = "cdm-btn";
      mp4.textContent = "MP4";
      mp4.title = "Download as MP4";
      mp4.addEventListener("click", () => send(mp4, item.url, item.title, "mp4", "Sent \u2713"));
      actions.appendChild(mp4);
    }

    const mp3 = document.createElement("button");
    mp3.className = "cdm-btn cdm-audio";
    mp3.textContent = "MP3";
    mp3.title = item.kind === "audio"
      ? "Download the audio file"
      : "Extract MP3 audio (requires FFmpeg)";
    mp3.addEventListener("click", () => send(mp3, item.url, item.title, "mp3", "Sent \u2713"));
    actions.appendChild(mp3);

    row.appendChild(actions);
    return row;
  }

  function render() {
    if (!barRoot || closed || (media.size === 0 && !playlistInfo)) {
      hideBar();
      return;
    }
    const key = mediaKey();
    if (key === lastKey) return;
    lastKey = key;

    const itemsEl = barRoot.querySelector(".cdm-items");
    itemsEl.textContent = "";

    if (playlistInfo) {
      const label = document.createElement("div");
      label.className = "cdm-playlist-label";
      label.textContent = "Playlist Detected";
      itemsEl.appendChild(label);

      const row = document.createElement("div");
      row.className = "cdm-row";
      const name = document.createElement("span");
      name.className = "cdm-name";
      name.textContent = playlistInfo.title;
      name.title = playlistInfo.url;
      row.appendChild(name);

      const actions = document.createElement("span");
      actions.className = "cdm-actions";

      const mp4 = document.createElement("button");
      mp4.className = "cdm-btn cdm-playlist";
      mp4.textContent = "MP4";
      mp4.title = "Download entire playlist as video (MP4)";
      mp4.addEventListener("click", () => send(mp4, playlistInfo.url, playlistInfo.title, "playlist-mp4", "Sent \u2713"));
      actions.appendChild(mp4);

      const mp3 = document.createElement("button");
      mp3.className = "cdm-btn cdm-playlist-audio";
      mp3.textContent = "MP3";
      mp3.title = "Download entire playlist as audio (MP3)";
      mp3.addEventListener("click", () => send(mp3, playlistInfo.url, playlistInfo.title, "playlist-mp3", "Sent \u2713"));
      actions.appendChild(mp3);

      row.appendChild(actions);
      itemsEl.appendChild(row);

      if (media.size > 0) {
        const sep = document.createElement("hr");
        sep.className = "cdm-separator";
        itemsEl.appendChild(sep);
      }
    }

    const shown = Array.from(media.values()).slice(0, 4);
    const extra = media.size - shown.length;
    for (const item of shown) itemsEl.appendChild(rowFor(item));

    if (extra > 0) {
      const more = document.createElement("div");
      more.className = "cdm-more";
      more.textContent = "+" + extra + " more media file" + (extra === 1 ? "" : "s");
      itemsEl.appendChild(more);
    }

    if (media.size > 1) {
      const all = document.createElement("button");
      all.className = "cdm-btn cdm-all";
      all.textContent = "Download all page media (" + media.size + ")";
      all.title = "Send every detected media file to Copper Download Manager";
      all.addEventListener("click", () => {
        setBtnState(all, "Sending\u2026", true);
        const urls = Array.from(media.values()).map((m) => m.url);
        try {
          api.runtime.sendMessage(
            { action: "sendBatch", urls, format: "mp4" },
            (resp) => {
              void api.runtime.lastError;
              const ok = resp && resp.success;
              setBtnState(all, ok ? "Sent to Copper \u2713" : "Copper off / failed", false);
            }
          );
        } catch (e) {
          setBtnState(all, "Copper off / failed", false);
        }
      });
      const wrap = document.createElement("div");
      wrap.className = "cdm-row";
      wrap.style.borderTop = "none";
      wrap.appendChild(all);
      itemsEl.appendChild(wrap);
    }

    barRoot.style.display = "block";
  }

  function hideBar() {
    if (barRoot) barRoot.style.display = "none";
  }

  function init() {
    if (closed || barRoot) return;
    collect();
    if (media.size === 0 && !playlistInfo) return;
    buildBar();
    render();
  }

  // Re-scan when the DOM changes (debounced) plus a periodic safety net.
  let timer = null;
  const observer = new MutationObserver(() => {
    if (timer) return;
    timer = setTimeout(() => {
      timer = null;
      if (closed) return;
      const prev = mediaKey();
      collect();
      const next = mediaKey();
      if (prev !== next) {
        if (!barRoot) {
          if (media.size === 0 && !playlistInfo) return;
          buildBar();
        }
        lastKey = "";
        render();
      }
    }, 800);
  });
  observer.observe(document.documentElement, { childList: true, subtree: true });

  setInterval(() => { if (closed) return; init(); }, 2500);

  api.runtime.onMessage.addListener((request, sender, sendResponse) => {
    if (request && request.action === "collectMedia") {
      collect();
      sendResponse({ media: Array.from(media.values()), playlist: playlistInfo });
      return true;
    }
    if (request && request.action === "toggleBar") {
      closed = request.visible === false || request.visible === "hide";
      if (!closed) {
        if (!barRoot) init();
        else { lastKey = ""; render(); }
      } else {
        hideBar();
      }
      sendResponse({ visible: !closed, count: media.size });
      return true;
    }
    if (request && request.action === "getState") {
      sendResponse({ visible: barRoot ? barRoot.style.display !== "none" : false, count: media.size });
      return true;
    }
    return false;
  });

  api.runtime.sendMessage({ action: "getStatus" }, (resp) => {
    if (api.runtime.lastError || !resp) return;
    if (resp.enabled === false) return;
    if (resp.settings && resp.settings.showBar === false) return;
    init();
  });
})();