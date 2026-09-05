// Copper Download Manager - extension background service worker.
//
// Reliable injection pipeline: native messaging host (preferred) -> HTTP API
// (/api/download) fallback -> launch the desktop app and retry. The download
// bar (copper-bar.js) for playlists and single videos sends {action:"sendUrl"};
// context menus and auto-capture (chrome.downloads.onCreated) use the same
// dispatch path so everything reaches Copper the same way.

const STORAGE_KEY = "copperExtensionEnabled";
const HOST = "com.copper.dm";
const STATUS_PAGE = "copper.html";
const HTTP_API = "http://127.0.0.1:24680";
const HTTP_TIMEOUT = 3000;
const LAST_RESULT_KEY = "copperLastResult";

// ---------------------------------------------------------------------------
// Low-level transports
// ---------------------------------------------------------------------------

function sendNativeMessage(message) {
  return new Promise((resolve) => {
    try {
      chrome.runtime.sendNativeMessage(HOST, message, (response) => {
        const err = chrome.runtime.lastError;
        if (err || !response) {
          resolve({ ok: false, error: err ? err.message : "no response" });
          return;
        }
        resolve(response);
      });
    } catch (e) {
      resolve({ ok: false, error: e.message });
    }
  });
}

function httpApi(path, method, body) {
  return new Promise((resolve) => {
    let timer;
    const controller = new AbortController();
    const opts = {
      method,
      cache: "no-store",
      signal: controller.signal,
    };
    if (body) {
      opts.headers = { "Content-Type": "application/json" };
      opts.body = JSON.stringify(body);
    }
    const t0 = Date.now();
    fetch(`${HTTP_API}${path}`, opts)
      .then(async (res) => {
        let j = null;
        try {
          j = await res.json();
        } catch (e) { /* non-JSON body */ }
        resolve({
          ok: res.ok,
          status: res.status,
          json: j,
          ms: Date.now() - t0,
        });
      })
      .catch((e) => {
        resolve({ ok: false, status: 0, json: null, ms: Date.now() - t0, error: e.message });
      })
      .finally(() => clearTimeout(timer));
    timer = setTimeout(() => controller.abort(), HTTP_TIMEOUT);
  });
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

function getCurrentEnabledState() {
  return new Promise((resolve) => {
    chrome.storage.local.get([STORAGE_KEY], (items) => {
      resolve(items[STORAGE_KEY] !== false);
    });
  });
}

function setCurrentEnabledState(enabled) {
  chrome.storage.local.set({ [STORAGE_KEY]: enabled });
}

function rememberResult(ok, detail) {
  chrome.storage.local.set({ [LAST_RESULT_KEY]: { ok, detail, at: Date.now() } });
}

// ---------------------------------------------------------------------------
// App reachability + launch
// ---------------------------------------------------------------------------

async function httpPing() {
  const res = await httpApi("/api/ping", "GET");
  return res.status >= 200 && res.status < 500;
}

// Try to make the desktop app start (the native host launches it on first use).
async function launchCopper() {
  try {
    await sendNativeMessage({ action: "open" });
  } catch (e) { /* ignore */ }
}

// ---------------------------------------------------------------------------
// Injection core
// ---------------------------------------------------------------------------

function isNotInstalledReply(rep) {
  if (!rep || rep.ok === undefined) return true;
  if (rep.ok) return false;
  const msg = (rep.error || "").toLowerCase();
  return msg.includes("not found") || msg.includes("not installed");
}

// Send one download request through every transport until one succeeds.
// Returns {accepted, notInstalled, reason}.
async function sendToCopper(url, filename = "", path = "", format = "mp4") {
  const enabled = await getCurrentEnabledState();
  if (!enabled) {
    return { accepted: false, notInstalled: false, reason: "disabled" };
  }

  // 1) Native messaging host.
  const rep = await sendNativeMessage({ action: "download", url, filename, path, format });
  if (rep && rep.ok) {
    rememberResult(true, "native");
    return { accepted: true, notInstalled: false, reason: "native" };
  }
  const notInstalled = isNotInstalledReply(rep);

  // 2) HTTP API fallback.
  const h = await httpApi("/api/download", "POST", {
    url,
    filename,
    path,
    format,
  });
  if (h.ok) {
    rememberResult(true, "http");
    return { accepted: true, notInstalled: false, reason: "http" };
  }
  // The app may be listening but rejected the download (e.g. filter). A 4xx is
  // a definitive answer, not "unreachable" - don't launch for that.
  const definitiveReject = h.status >= 400 && h.status <= 499;

  // 3) Launch the app and retry the native host once.
  if (!definitiveReject) {
    await launchCopper();
    await new Promise((r) => setTimeout(r, 900));
    const rep2 = await sendNativeMessage({ action: "download", url, filename, path, format });
    if (rep2 && rep2.ok) {
      rememberResult(true, "native-after-launch");
      return { accepted: true, notInstalled: false, reason: "native-after-launch" };
    }
    const h2 = await httpApi("/api/download", "POST", { url, filename, path, format });
    if (h2.ok) {
      rememberResult(true, "http-after-launch");
      return { accepted: true, notInstalled: false, reason: "http-after-launch" };
    }
  }

  rememberResult(false, definitiveReject ? "rejected" : "unreachable");
  return { accepted: false, notInstalled: notInstalled || !definitiveReject, reason: "failed" };
}

async function dispatch(url, filename = "", path = "", format = "mp4") {
  const result = await sendToCopper(url, filename, path, format);
  if (result.accepted) return true;
  notifyDispatchFailed(result);
  return false;
}

function notifyDispatchFailed(result) {
  const title = "Copper download could not be added";
  let message =
    "Copper Download Manager could not be reached. Please open Copper and try again.";
  if (result && result.reason === "rejected" && result.detail) {
    message = "Copper rejected the download: " + result.detail;
  }
  try {
    chrome.notifications.create({
      type: "basic",
      iconUrl: chrome.runtime.getURL("icons/icon128.png"),
      title,
      message,
    });
  } catch (e) { /* noop */ }
}

function openStatusTab(notInstalled) {
  let url = chrome.runtime.getURL(STATUS_PAGE);
  if (notInstalled) url += "?status=not-installed";
  chrome.tabs.create({ url });
}

// ---------------------------------------------------------------------------
// File-format filter (kept in sync with the app's /api/download-filters)
// ---------------------------------------------------------------------------

function normalizeExt(ext) {
  return (ext || "").toLowerCase().replace(/^\.+/, "");
}

function extOfUrlOrName(url, name) {
  const src = (name || "").split(/[\\/]/).pop() || url || "";
  const m = src.match(/\.([a-z0-9]+)(?:$|[?#])/i);
  return m ? normalizeExt(m[1]) : "";
}

const FILTER_CACHE_TTL = 30000;
let filterCache = { ts: 0, data: null };

const DEFAULT_EXCLUDE = "png jpg jpeg gif webp bmp svg ico avif jfif heic heif tif tiff raw psd eps ai dng cr2 nef arw exr".split(/\s+/);
const DEFAULT_INCLUDE = "mp4 mkv webm avi mov wmv flv m4v mpg mpeg ts m2ts 3gp ogm mp3 wav flac aac ogg m4a opus wma mid midi aiff zip rar 7z tar gz bz2 xz tgz iso cab pdf doc docx xls xlsx ppt pptx txt rtf csv odt ods odp epub mobi md exe msi apk deb rpm appimage dmg bat cmd com torrent ttf otf woff woff2 js jsx ts tsx json html css scss py java c cpp h cs go rs php rb sh bin dat db sqlite".split(/\s+/);

async function fetchFilterConfig() {
  const now = Date.now();
  if (filterCache.data && now - filterCache.ts < FILTER_CACHE_TTL) return filterCache.data;
  const res = await httpApi("/api/download-filters", "GET");
  if (res.ok && res.json) {
    const j = res.json;
    const cfg = {
      enabled: j.enabled !== false,
      include: (j.include || []).map(normalizeExt).filter(Boolean),
      exclude: (j.exclude || []).map(normalizeExt).filter(Boolean),
    };
    filterCache = { ts: now, data: cfg };
    return cfg;
  }
  return filterCache.data || { enabled: true, include: DEFAULT_INCLUDE.slice(), exclude: DEFAULT_EXCLUDE.slice() };
}

function shouldCaptureExtension(ext, cfg) {
  if (!cfg.enabled) return true;
  const e = normalizeExt(ext);
  if (!e) return true;
  if (cfg.exclude.includes(e)) return false;
  if (cfg.include.length > 0 && !cfg.include.includes(e)) return false;
  return true;
}

// ---------------------------------------------------------------------------
// Auto-capture of normal browser downloads
// ---------------------------------------------------------------------------

chrome.downloads.onCreated.addListener((item) => {
  getCurrentEnabledState().then((enabled) => {
    if (!enabled) return;
    if (!item || !item.url || item.url.startsWith("blob:") || item.url.startsWith("data:")) return;
    const ext = extOfUrlOrName(item.url, item.filename || "");
    fetchFilterConfig().then((cfg) => {
      if (!shouldCaptureExtension(ext, cfg)) return;
      const downloadName = (item.filename || "").split(/[\\/]/).pop() || "";
      sendToCopper(item.url, downloadName, "", "").then((result) => {
        if (result.accepted) {
          try { chrome.downloads.cancel(item.id, () => {}); } catch (e) {}
        }
      });
    });
  });
});

// ---------------------------------------------------------------------------
// Context menus
// ---------------------------------------------------------------------------

chrome.runtime.onInstalled.addListener(() => {
  chrome.storage.local.get([STORAGE_KEY], (items) => {
    if (items[STORAGE_KEY] === undefined) {
      chrome.storage.local.set({ [STORAGE_KEY]: true });
    }
  });

  chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({ id: "copper-link", title: "Download link with Copper", contexts: ["link"] });
    chrome.contextMenus.create({ id: "copper-image", title: "Download image with Copper", contexts: ["image"] });
    chrome.contextMenus.create({ id: "copper-video", title: "Download video with Copper", contexts: ["video"] });
    chrome.contextMenus.create({ id: "copper-selection", title: "Download selection with Copper", contexts: ["selection"] });
    chrome.contextMenus.create({ id: "copper-playlist-mp4", title: "Download full playlist as MP4", contexts: ["page", "link"] });
    chrome.contextMenus.create({ id: "copper-playlist-mp3", title: "Download full playlist as MP3", contexts: ["page", "link"] });
  });
});

chrome.contextMenus.onClicked.addListener((info) => {
  const filenameOf = (src) => (src || "").split("/").pop().split("?")[0] || "download";

  if (info.menuItemId === "copper-link" && info.linkUrl) {
    dispatch(info.linkUrl, filenameOf(info.linkUrl));
    return;
  }
  if ((info.menuItemId === "copper-image" || info.menuItemId === "copper-video") && info.srcUrl) {
    dispatch(info.srcUrl, filenameOf(info.srcUrl));
    return;
  }
  if (info.menuItemId === "copper-selection" && info.selectionText) {
    const text = info.selectionText.trim();
    if (/^(https?|ftp|magnet):/i.test(text)) {
      dispatch(text, filenameOf(text));
    }
  }
  if (info.menuItemId === "copper-playlist-mp4") {
    const url = info.linkUrl || info.pageUrl;
    if (url) { dispatch(url, "Full Playlist", "", "playlist-mp4"); }
    return;
  }
  if (info.menuItemId === "copper-playlist-mp3") {
    const url = info.linkUrl || info.pageUrl;
    if (url) { dispatch(url, "Full Playlist", "", "playlist-mp3"); }
    return;
  }
});

// ---------------------------------------------------------------------------
// Status page / action + message API
// ---------------------------------------------------------------------------

chrome.action.onClicked.addListener(() => {
  openStatusTab(false);
});

async function registerWithCopper() {
  try {
    const id = chrome.runtime.id;
    if (!id) return;
    const browser = typeof browser !== "undefined" ? "firefox" : "chrome";
    await sendNativeMessage({ action: "register", browser, extensionId: id });
  } catch (e) {
    // Not fatal; the app already registers Firefox's stable id and Chrome store id.
  }
}

chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  if (request && request.action === "getStatus") {
    Promise.all([getCurrentEnabledState(), httpPing()]).then(([enabled, reachable]) => {
      try {
        sendResponse({ enabled, reachable, installed: reachable, extensionId: chrome.runtime.id });
      } catch (e) { /* noop */ }
    });
    return true;
  }

  if (request && request.action === "getAppInstalled") {
    httpPing().then((installed) => {
      try {
        sendResponse({ installed });
      } catch (e) { /* noop */ }
    });
    return true;
  }

  if (request && request.action === "toggle") {
    const enabled = Boolean(request.enabled);
    setCurrentEnabledState(enabled);
    sendResponse({ enabled });
    return true;
  }

  if (request && request.action === "sendUrl") {
    dispatch(request.url, request.filename || "", request.path || "", request.format || "mp4").then((done) => {
      sendResponse({ success: done });
    });
    return true;
  }

  if (request && request.action === "openCopper") {
    launchCopper().then(() => sendResponse({ success: true }));
    return true;
  }

  if (request && request.action === "register") {
    registerWithCopper().then(() => sendResponse({ success: true }));
    return true;
  }

  return false;
});
