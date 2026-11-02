// Copper Download Manager - extension background (Chrome & Firefox MV3).
//
// Goal: beat the browser. Cancel downloads synchronously on creation, cache
// everything aggressively, intercept link clicks in content scripts before the
// browser handles them, and fail gracefully back to the browser if Copper is
// unreachable (only when explicitly allowed).
"use strict";

const STORAGE_KEY = "copperExtensionEnabled";
const SETTINGS_KEY = "copperSettings";
const LAST_RESULT_KEY = "copperLastResult";
const STATUS_PAGE = "copper.html";
const HTTP_API = "http://127.0.0.1:24680";
const HTTP_TIMEOUT = 2000;
const REGISTER_TIMEOUT = 60000;
const LAUNCH_RETRY_MS = 1000;

const api = typeof browser !== "undefined" && browser.runtime ? browser : chrome;

function msg(key, subs) {
  try {
    return api.i18n.getMessage(key, subs) || key;
  } catch (e) {
    return key;
  }
}

const DEFAULT_SETTINGS = {
  autoCapture: true,
  showBar: true,
  contextMenus: true,
  notifyOnFail: true,
  dedupeWindowMs: 8000,
  maxBatch: 12,
  defaultFormat: "mp4",
  fallbackToBrowserOnFailure: true
};

let settings = { ...DEFAULT_SETTINGS };
let settingsLoaded = false;
let settingsLoadPromise = null;

function loadSettings() {
  if (settingsLoaded) return Promise.resolve(settings);
  if (settingsLoadPromise) return settingsLoadPromise;
  settingsLoadPromise = new Promise((resolve) => {
    api.storage.local.get([SETTINGS_KEY], (items) => {
      const saved = items[SETTINGS_KEY] || {};
      settings = { ...DEFAULT_SETTINGS, ...saved };
      settingsLoaded = true;
      settingsLoadPromise = null;
      resolve(settings);
    });
  });
  return settingsLoadPromise;
}

function saveSettings(next) {
  settings = { ...settings, ...next };
  api.storage.local.set({ [SETTINGS_KEY]: settings });
}

function getCurrentEnabledState() {
  return new Promise((resolve) => {
    api.storage.local.get([STORAGE_KEY], (items) => {
      resolve(items[STORAGE_KEY] !== false);
    });
  });
}

function setCurrentEnabledState(enabled) {
  api.storage.local.set({ [STORAGE_KEY]: enabled });
}

function rememberResult(ok, detail) {
  api.storage.local.set({ [LAST_RESULT_KEY]: { ok, detail, at: Date.now() } });
}

// Auth
const NATIVE_HOST = "com.copper.dm";
let apiToken = "";
let apiTokenLoaded = false;
let tokenFetch = null;
let registrationDeniedUntil = 0;

function nativeGetToken() {
  return new Promise((resolve) => {
    try {
      api.runtime.sendNativeMessage(NATIVE_HOST, { action: "getToken" }, (reply) => {
        void api.runtime.lastError;
        resolve(reply && reply.ok && reply.token ? String(reply.token) : "");
      });
    } catch (e) {
      resolve("");
    }
  });
}

function httpApiOnce(path, method, body, token, timeoutMs) {
  return new Promise((resolve) => {
    let timer;
    const controller = new AbortController();
    const opts = {
      method: method || "GET",
      cache: "no-store",
      signal: controller.signal,
      headers: {}
    };
    if (body) {
      opts.headers["Content-Type"] = "application/json";
      opts.body = JSON.stringify(body);
    }
    if (token) opts.headers["X-Copper-Token"] = token;
    fetch(`${HTTP_API}${path}`, opts)
      .then(async (res) => {
        let j = null;
        try {
          j = await res.json();
        } catch (e) {}
        resolve({ ok: res.ok, status: res.status, json: j });
      })
      .catch((e) => {
        resolve({ ok: false, status: 0, json: null, error: e.message });
      })
      .finally(() => clearTimeout(timer));
    timer = setTimeout(() => controller.abort(), timeoutMs || HTTP_TIMEOUT);
  });
}

function registerWithApp() {
  if (Date.now() < registrationDeniedUntil) return Promise.resolve("");
  return httpApiOnce("/api/register", "POST", { extensionId: api.runtime.id }, "", REGISTER_TIMEOUT).then((res) => {
    if (res.ok && res.json && res.json.token) return String(res.json.token);
    if (res.status === 403) registrationDeniedUntil = Date.now() + 60000;
    return "";
  });
}

function fetchApiToken() {
  return nativeGetToken().then((t) => (t ? t : registerWithApp()));
}

function ensureApiToken() {
  if (apiTokenLoaded) return Promise.resolve(apiToken);
  if (!tokenFetch) {
    tokenFetch = fetchApiToken().then((t) => {
      tokenFetch = null;
      if (t) {
        apiToken = t;
        apiTokenLoaded = true;
      }
      return t;
    });
  }
  return tokenFetch;
}

function refreshApiToken() {
  apiToken = "";
  apiTokenLoaded = false;
  return ensureApiToken();
}

function httpApi(path, method, body) {
  return ensureApiToken().then((token) =>
    httpApiOnce(path, method, body, token).then((res) => {
      if (res.status !== 401) return res;
      return refreshApiToken().then((t) => httpApiOnce(path, method, body, t));
    })
  );
}

async function httpPing() {
  const res = await httpApi("/api/ping", "GET");
  return res.status >= 200 && res.status < 500;
}

function launchCopper() {
  try {
    api.tabs.create({ url: "copper://open" });
  } catch (e) {}
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

// Dedupe
const recent = new Map();
const inFlight = new Set();
let queue = [];
let queueBusy = false;

function isDup(url) {
  if (inFlight.has(url)) return true;
  const t = recent.get(url);
  if (t && Date.now() - t < settings.dedupeWindowMs) return true;
  recent.set(url, Date.now());
  return false;
}

function enqueue(task) {
  queue.push(task);
  drainQueue();
}

async function drainQueue() {
  if (queueBusy) return;
  queueBusy = true;
  while (queue.length > 0) {
    const task = queue.shift();
    inFlight.add(task.url);
    try {
      const result = await sendToCopper(task.url, task.filename, task.path, task.format);
      if (task.resolve) task.resolve(result.accepted);
      if (task.onResult) task.onResult(result);
    } finally {
      inFlight.delete(task.url);
    }
    await sleep(100);
  }
  queueBusy = false;
}

// Send
async function sendToCopper(url, filename = "", path = "", format = settings.defaultFormat) {
  const enabled = await getCurrentEnabledState();
  if (!enabled) {
    return { accepted: false, notInstalled: false, reason: "disabled" };
  }

  let rejectDetail = "";
  const h = await httpApi("/api/download", "POST", { url, filename, path, format });
  if (h.ok) {
    rememberResult(true, "http");
    return { accepted: true, notInstalled: false, reason: "http" };
  }
  const definitiveReject = h.status >= 400 && h.status <= 499;
  if (definitiveReject && h.json && h.json.error) rejectDetail = h.json.error;

  if (!definitiveReject) {
    launchCopper();
    await sleep(LAUNCH_RETRY_MS);
    const h2 = await httpApi("/api/download", "POST", { url, filename, path, format });
    if (h2.ok) {
      rememberResult(true, "http-after-launch");
      return { accepted: true, notInstalled: false, reason: "http-after-launch" };
    }
    if (h2.json && h2.json.error) rejectDetail = h2.json.error;
  }

  rememberResult(false, definitiveReject ? "rejected" : "unreachable");
  return {
    accepted: false,
    notInstalled: !definitiveReject,
    reason: "failed",
    detail: rejectDetail,
  };
}

function dispatch(url, filename = "", path = "", format = settings.defaultFormat) {
  if (!url) return Promise.resolve(false);
  if (isDup(url)) return Promise.resolve(true);
  return new Promise((resolve) => {
    enqueue({ url, filename, path, format, resolve });
  });
}

function dispatchBatch(urls, filenamePrefix = "", format = settings.defaultFormat) {
  return new Promise((resolve) => {
    const unique = [];
    for (const u of urls) {
      if (!u || unique.includes(u)) continue;
      if (isDup(u)) continue;
      unique.push(u);
    }
    const take = unique.slice(0, settings.maxBatch);
    if (take.length === 0) return resolve({ accepted: true, count: 0 });
    for (let i = 0; i < take.length; i++) {
      enqueue({
        url: take[i],
        filename: filenamePrefix ? `${filenamePrefix}-${i + 1}` : "",
        path: "",
        format,
      });
    }
    rememberResult(true, "batch");
    resolve({ accepted: true, count: take.length });
  });
}

function notifyDispatchFailed(result) {
  const title = msg("notifyTitle");
  let message = msg("notifyUnreachable");
  if (result && result.detail) {
    message = msg("notifyRejected", [result.detail]);
  }
  try {
    api.notifications.create({
      type: "basic",
      iconUrl: api.runtime.getURL("icons/icon128.png"),
      title,
      message,
    });
  } catch (e) {}
}

function openStatusTab(notInstalled) {
  let url = api.runtime.getURL(STATUS_PAGE);
  if (notInstalled) url += "?status=not-installed";
  api.tabs.create({ url });
}

// Filters
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
const DEFAULT_INCLUDE = "".split(/\s+/).filter(Boolean);

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

// Auto-capture - cancel immediately, synchronously if possible
api.downloads.onCreated.addListener((item) => {
  if (!item || !item.url) return;
  if (item.url.startsWith("blob:") || item.url.startsWith("data:")) return;
  
  // Cancel first, then decide - this is the key to beating the browser
  try {
    api.downloads.cancel(item.id);
  } catch (e) {}
  
  (async () => {
    const enabled = await getCurrentEnabledState();
    if (!enabled) {
      // Re-download in browser if Copper disabled and fallback enabled
      try {
        if (settings.fallbackToBrowserOnFailure) {
          api.downloads.download({ url: item.url, filename: item.filename, saveAs: false });
        }
      } catch (e) {}
      return;
    }
    await loadSettings();
    if (!settings.autoCapture) {
      try {
        if (settings.fallbackToBrowserOnFailure) {
          api.downloads.download({ url: item.url, filename: item.filename, saveAs: false });
        }
      } catch (e) {}
      return;
    }
    
    const ext = extOfUrlOrName(item.url, item.filename || "");
    const cfg = await fetchFilterConfig();
    if (!shouldCaptureExtension(ext, cfg)) {
      // Not captured by Copper filter - let browser handle it
      try {
        if (settings.fallbackToBrowserOnFailure) {
          api.downloads.download({ url: item.url, filename: item.filename, saveAs: false });
        }
      } catch (e) {}
      return;
    }
    
    const downloadName = (item.filename || "").split(/[\\/]/).pop() || "";
    const result = await sendToCopper(item.url, downloadName, "", "");
    if (result.accepted) {
      // Already cancelled above
      rememberResult(true, "intercepted");
    } else {
      // Copper unreachable/rejected - optionally let browser download
      if (settings.fallbackToBrowserOnFailure && !result.notInstalled) {
        try {
          api.downloads.download({ url: item.url, filename: item.filename, saveAs: false });
        } catch (e) {}
      }
      if (settings.notifyOnFail) {
        notifyDispatchFailed(result);
      }
    }
  })().catch(() => {});
});

// Context menus
const MENU_DEFS = [
  { id: "copper-link", title: msg("menuLink"), contexts: ["link"] },
  { id: "copper-image", title: msg("menuImage"), contexts: ["image"] },
  { id: "copper-video", title: msg("menuVideo"), contexts: ["video"] },
  { id: "copper-audio", title: msg("menuAudio"), contexts: ["audio"] },
  { id: "copper-selection-video", title: msg("menuSelectionMp4"), contexts: ["selection"] },
  { id: "copper-selection-mp3", title: msg("menuSelectionMp3"), contexts: ["selection"] },
  { id: "copper-page-media", title: msg("menuPageMedia"), contexts: ["page"] },
  { id: "copper-magnet", title: msg("menuMagnet"), contexts: ["link"] },
  { id: "copper-playlist-mp4", title: msg("menuPlaylistMp4"), contexts: ["page", "link"] },
  { id: "copper-playlist-mp3", title: msg("menuPlaylistMp3"), contexts: ["page", "link"] },
];

let menusBuildChain = Promise.resolve();

function removeAllMenus() {
  return new Promise((resolve) => {
    try {
      api.contextMenus.removeAll(() => {
        void api.runtime.lastError;
        resolve();
      });
    } catch (e) {
      resolve();
    }
  });
}

function buildMenus() {
  const run = () => removeAllMenus().then(() => {
    for (const def of MENU_DEFS) {
      try {
        api.contextMenus.create(def, () => void api.runtime.lastError);
      } catch (e) {}
    }
  });
  menusBuildChain = menusBuildChain.then(run, run);
  return menusBuildChain;
}

api.runtime.onInstalled.addListener(() => {
  api.storage.local.get([STORAGE_KEY], (items) => {
    if (items[STORAGE_KEY] === undefined) {
      api.storage.local.set({ [STORAGE_KEY]: true });
    }
  });
  loadSettings().then(() => {
    if (settings.contextMenus) buildMenus();
  });
});

api.contextMenus.onClicked.addListener((info) => {
  loadSettings().then(() => {
    const filenameOf = (src) => (src || "").split("/").pop().split("?")[0].split("#")[0] || "download";

    if (info.menuItemId === "copper-link" && info.linkUrl) {
      dispatch(info.linkUrl, filenameOf(info.linkUrl));
      return;
    }
    if ((info.menuItemId === "copper-image" || info.menuItemId === "copper-video" ||
         info.menuItemId === "copper-audio") && info.srcUrl) {
      dispatch(info.srcUrl, filenameOf(info.srcUrl));
      return;
    }
    if (info.menuItemId === "copper-selection-video" && info.selectionText) {
      const text = info.selectionText.trim();
      if (/^(https?|ftp):/i.test(text)) {
        dispatch(text, filenameOf(text), "", "mp4");
      }
      return;
    }
    if (info.menuItemId === "copper-selection-mp3" && info.selectionText) {
      const text = info.selectionText.trim();
      if (/^(https?|ftp):/i.test(text)) {
        dispatch(text, filenameOf(text), "", "mp3");
      }
      return;
    }
    if (info.menuItemId === "copper-magnet" && info.linkUrl && info.linkUrl.startsWith("magnet:?")) {
      dispatch(info.linkUrl, "");
      return;
    }
    if (info.menuItemId === "copper-page-media") {
      collectPageMedia(info.tabId ? { tabId: info.tabId } : {}).then((list) => {
        if (list && list.length > 0) {
          dispatchBatch(list, "page-media", settings.defaultFormat);
        } else {
          notifyDispatchFailed({ detail: msg("detailNoMedia") });
        }
      });
      return;
    }
    if (info.menuItemId === "copper-playlist-mp4" || info.menuItemId === "copper-playlist-mp3") {
      const url = info.linkUrl || info.pageUrl;
      if (url) {
        dispatch(url, msg("fallbackFullPlaylist"), "", info.menuItemId === "copper-playlist-mp3" ? "playlist-mp3" : "playlist-mp4");
      }
      return;
    }
  });
});

function collectPageMedia(opts) {
  return new Promise((resolve) => {
    const sendTo = (tabId) => {
      try {
        api.tabs.sendMessage(tabId, { action: "collectMedia" }, (resp) => {
          void api.runtime.lastError;
          if (resp && Array.isArray(resp.media)) {
            resolve(resp.media.map((m) => m.url).filter(Boolean));
            return;
          }
          resolve([]);
        });
      } catch (e) {
        resolve([]);
      }
    };
    if (opts && opts.tabId != null) {
      sendTo(opts.tabId);
    } else {
      api.tabs.query({ active: true, currentWindow: true }, (tabs) => {
        if (tabs && tabs[0] && tabs[0].id != null) sendTo(tabs[0].id);
        else resolve([]);
      });
    }
  });
}

// Messages
function rebuildMenus(enabled) {
  if (enabled) {
    loadSettings().then(() => {
      if (settings.contextMenus) buildMenus();
    });
    return;
  }
  const run = () => removeAllMenus();
  menusBuildChain = menusBuildChain.then(run, run);
}

api.runtime.onMessage.addListener((request, sender, sendResponse) => {
  if (request && request.action === "getStatus") {
    Promise.all([getCurrentEnabledState(), httpPing()]).then(([enabled, reachable]) => {
      try {
        sendResponse({ enabled, reachable, settings, extensionId: api.runtime.id });
      } catch (e) {}
    });
    return true;
  }

  if (request && request.action === "getToken") {
    ensureApiToken().then((token) => {
      try {
        sendResponse({ token: token || "" });
      } catch (e) {}
    });
    return true;
  }

  if (request && request.action === "getSettings") {
    loadSettings().then(() => {
      try {
        sendResponse({ settings });
      } catch (e) {}
    });
    return true;
  }

  if (request && request.action === "setSettings") {
    saveSettings(request.settings || {});
    if (request.settings && request.settings.contextMenus !== undefined) {
      rebuildMenus(request.settings.contextMenus);
    }
    sendResponse({ settings });
    return true;
  }

  if (request && request.action === "toggle") {
    const enabled = Boolean(request.enabled);
    setCurrentEnabledState(enabled);
    sendResponse({ enabled });
    return true;
  }

  if (request && request.action === "sendUrl") {
    dispatch(
      request.url,
      request.filename || "",
      request.path || "",
      request.format || settings.defaultFormat
    ).then((done) => {
      sendResponse({ success: done });
    });
    return true;
  }

  if (request && request.action === "sendBatch") {
    dispatchBatch(request.urls || [], request.filenamePrefix || "", request.format || settings.defaultFormat).then((r) => {
      sendResponse({ success: !!r.accepted, count: r.count });
    });
    return true;
  }

  if (request && request.action === "collectPageMedia") {
    collectPageMedia(sender && sender.tab ? { tabId: sender.tab.id } : {}).then((list) => {
      sendResponse({ media: list });
    });
    return true;
  }

  if (request && request.action === "openCopper") {
    launchCopper();
    sendResponse({ success: true });
    return true;
  }

  if (request && request.action === "openManager") {
    openStatusTab(false);
    sendResponse({ success: true });
    return true;
  }

  if (request && request.action === "interceptClick") {
    // Content script intercepted a click on a download link - dispatch immediately
    const url = request.url || "";
    const filename = request.filename || "";
    if (url) {
      dispatch(url, filename, "", request.format || settings.defaultFormat);
      sendResponse({ ok: true });
      return true;
    }
    sendResponse({ ok: false });
    return true;
  }

  return false;
});

api.action.onClicked.addListener(() => {
  openStatusTab(false);
});

loadSettings().then(() => {
  const enabledPromise = getCurrentEnabledState();
  enabledPromise.then((enabled) => {
    if (settings.contextMenus && enabled) buildMenus();
  });
  ensureApiToken();
});
