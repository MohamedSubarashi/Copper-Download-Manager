const STORAGE_KEY = "copperExtensionEnabled";
const HOST = "com.copper.dm";
const STATUS_PAGE = "copper.html";
const HTTP_API = "http://127.0.0.1:24680";
const HTTP_PING_TIMEOUT = 1500;

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

function isNotInstalledReply(rep) {
  if (!rep || rep.ok === undefined) return true;
  if (rep.ok) return false;
  const msg = (rep.error || "").toLowerCase();
  return msg.includes("not found") || msg.includes("not installed");
}

async function pingCopper() {
  const rep = await sendNativeMessage({ action: "ping" });
  if (rep && rep.ok) return true;
  return httpPing();
}

function httpPing() {
  return new Promise((resolve) => {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), HTTP_PING_TIMEOUT);
    fetch(`${HTTP_API}/api/ping`, { signal: controller.signal, cache: "no-store" })
      .then((res) => resolve(res.ok))
      .catch(() => resolve(false))
      .finally(() => clearTimeout(timer));
  });
}

function notifyCopperUnreachable() {
  chrome.notifications.create({
    type: "basic",
    iconUrl: chrome.runtime.getURL("icons/icon128.png"),
    title: "Copper isn't reachable",
    message:
      "Copper Download Manager could not be reached. Make sure the native host is installed (open Copper, then try again).",
  });
}

function openStatusTab(notInstalled) {
  let url = chrome.runtime.getURL(STATUS_PAGE);
  if (notInstalled) {
    url += "?status=not-installed";
  }
  chrome.tabs.create({ url });
}

function getCurrentEnabledState() {
  return new Promise((resolve) => {
    chrome.storage.local.get([STORAGE_KEY], (items) => {
      const value = items[STORAGE_KEY];
      resolve(value !== false);
    });
  });
}

function setCurrentEnabledState(enabled) {
  chrome.storage.local.set({ [STORAGE_KEY]: enabled });
}

async function sendToCopper(url, filename = "", path = "", format = "mp4") {
  const enabled = await getCurrentEnabledState();
  if (!enabled) {
    return { accepted: false, notInstalled: false };
  }
  const rep = await sendNativeMessage({ action: "download", url, filename, path, format });
  if (rep && rep.ok) {
    return { accepted: true, notInstalled: false };
  }
  return { accepted: false, notInstalled: isNotInstalledReply(rep) };
}

async function dispatch(url, filename = "", path = "", format = "mp4") {
  const result = await sendToCopper(url, filename, path, format);
  if (result.accepted) {
    return true;
  }
  if (result.notInstalled) {
    openStatusTab(true);
  } else {
    notifyCopperUnreachable();
  }
  return false;
}

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
  try {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 1200);
    const res = await fetch(`${HTTP_API}/api/download-filters`, { signal: controller.signal, cache: "no-store" });
    clearTimeout(timer);
    if (res.ok) {
      const j = await res.json();
      const cfg = {
        enabled: j.enabled !== false,
        include: (j.include || []).map(normalizeExt).filter(Boolean),
        exclude: (j.exclude || []).map(normalizeExt).filter(Boolean),
      };
      filterCache = { ts: now, data: cfg };
      return cfg;
    }
  } catch (e) {}
  const fallback = filterCache.data || { enabled: true, include: DEFAULT_INCLUDE.slice(), exclude: DEFAULT_EXCLUDE.slice() };
  return fallback;
}

function shouldCaptureExtension(ext, cfg) {
  if (!cfg.enabled) return true;
  const e = normalizeExt(ext);
  if (!e) return true;
  if (cfg.exclude.includes(e)) return false;
  if (cfg.include.length > 0 && !cfg.include.includes(e)) return false;
  return true;
}

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
    if (url) dispatch(url, "Full Playlist", "", "playlist-mp4");
    return;
  }
  if (info.menuItemId === "copper-playlist-mp3") {
    const url = info.linkUrl || info.pageUrl;
    if (url) dispatch(url, "Full Playlist", "", "playlist-mp3");
    return;
  }
});

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
    Promise.all([getCurrentEnabledState(), pingCopper()]).then(([enabled, reachable]) => {
      try {
        sendResponse({ enabled, reachable, installed: reachable, extensionId: chrome.runtime.id });
      } catch (e) { /* noop */ }
    });
    return true;
  }

  if (request && request.action === "getAppInstalled") {
    pingCopper().then((installed) => {
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
    pingCopper().then((running) => sendResponse({ success: running }));
    return true;
  }

  if (request && request.action === "register") {
    registerWithCopper().then(() => sendResponse({ success: true }));
    return true;
  }

  return false;
});
