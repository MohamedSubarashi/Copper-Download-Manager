// Copper Download Manager - extension background (Chrome & Firefox MV3).
//
// Command center for the browser side:
//   * HTTP-first dispatch: downloads are posted straight to the app's local API
//     (http://127.0.0.1:24680/api/download), the normal IDM-style path. When the
//     app is not running it is launched through the registered copper:// protocol
//     and the request is retried;
//   * settings engine (storage.local) driving capture/bar/menus/notifications;
//   * dedupe + serialized batch queue so "download all page media" never floods
//     the desktop app;
//   * expanded context menus (link/image/video/audio/magnet/selection/page);
//   * auto-capture of normal browser downloads (chrome.downloads.onCreated),
//     honouring the app's include/exclude file-format filter.

"use strict";

const STORAGE_KEY = "copperExtensionEnabled";
const SETTINGS_KEY = "copperSettings";
const LAST_RESULT_KEY = "copperLastResult";
const STATUS_PAGE = "copper.html";
const HTTP_API = "http://127.0.0.1:24680";
const HTTP_TIMEOUT = 3000;
// The self-registration call may open a one-time confirmation prompt in the app,
// so it gets a much longer budget than a normal API call (the user has to click).
const REGISTER_TIMEOUT = 60000;
const LAUNCH_RETRY_MS = 1500;

const DEFAULT_SETTINGS = {
  autoCapture: true,    // intercept the browser's own downloads and hand to Copper
  showBar: true,        // float the media detection bar on http(s) pages
  contextMenus: true,   // enable the right-click menu entries
  notifyOnFail: true,   // system notification when a dispatch fails
  dedupeWindowMs: 10000,
  maxBatch: 8,
  defaultFormat: "mp4",
};

let settings = { ...DEFAULT_SETTINGS };

// ---------------------------------------------------------------------------
// Storage helpers
// ---------------------------------------------------------------------------

function loadSettings(resolve) {
  chrome.storage.local.get([SETTINGS_KEY], (items) => {
    const saved = items[SETTINGS_KEY] || {};
    settings = { ...DEFAULT_SETTINGS, ...saved };
    if (resolve) resolve(settings);
  });
}

function saveSettings(next) {
  settings = { ...settings, ...next };
  chrome.storage.local.set({ [SETTINGS_KEY]: settings });
}

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
// Transports
// ---------------------------------------------------------------------------

// Local API auth (0.4.0): the app's localhost API requires an install-scoped
// token on every non-OPTIONS request. The token is handed over by the native
// messaging host (action "getToken"), which reads it from the app's shared
// config - it never crosses the network, so no web page can ever learn it.
const NATIVE_HOST = "com.copper.dm";
let apiToken = "";
let apiTokenLoaded = false;
let tokenFetch = null;
// After the user declines self-registration, don't nag on every call. The
// window is short and in-memory (a service-worker restart clears it); a fresh
// download attempt may ask again, which is the desired behaviour.
let registrationDeniedUntil = 0;

function nativeGetToken() {
  return new Promise((resolve) => {
    try {
      chrome.runtime.sendNativeMessage(NATIVE_HOST, { action: "getToken" }, (reply) => {
        void chrome.runtime.lastError; // host not installed yet: treat as no token
        resolve(reply && reply.ok && reply.token ? String(reply.token) : "");
      });
    } catch (e) {
      resolve("");
    }
  });
}

// Fallback when the native host cannot hand over a token: ask the app itself.
// The app checks whether this extension's id is already authorized and, if not,
// prompts the user once before adding it to the host manifest. On success it
// returns the token, so the very next API call authenticates - no browser
// restart needed for the freshly authorized id.
function registerWithApp() {
  if (Date.now() < registrationDeniedUntil) return Promise.resolve("");
  return httpApiOnce("/api/register", "POST", { extensionId: chrome.runtime.id }, "",
                     REGISTER_TIMEOUT).then((res) => {
    if (res.ok && res.json && res.json.token) return String(res.json.token);
    if (res.status === 403) registrationDeniedUntil = Date.now() + 60000;
    return "";
  });
}

function fetchApiToken() {
  // Prefer the native host: the token never crosses the network there. Only when
  // that fails (typically an unpacked/dev build whose path-derived id is not in
  // the host manifest yet) fall back to app-side self-registration.
  return nativeGetToken().then((t) => (t ? t : registerWithApp()));
}

function ensureApiToken() {
  if (apiTokenLoaded) return Promise.resolve(apiToken);
  if (!tokenFetch) {
    tokenFetch = fetchApiToken().then((t) => {
      tokenFetch = null;
      if (t) {
        // Only cache a real token. Caching "" would keep every later call
        // tokenless (and 401) for the life of the service worker, even after
        // the app has started and written its config.
        apiToken = t;
        apiTokenLoaded = true;
      }
      return t;
    });
  }
  return tokenFetch;
}

// Drop any cached token and read it again from the host. Used when the app
// rewrote copper_host_config.json (stale token) or when it was not running the
// first time and has only now written it.
function refreshApiToken() {
  apiToken = "";
  apiTokenLoaded = false;
  return ensureApiToken();
}

function httpApiOnce(path, method, body, token, timeoutMs) {
  return new Promise((resolve) => {
    let timer;
    const controller = new AbortController();
    const opts = {
      method: method || "GET",
      cache: "no-store",
      signal: controller.signal,
      headers: {},
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
        } catch (e) { /* non-JSON body */ }
        resolve({ ok: res.ok, status: res.status, json: j });
      })
      .catch((e) => {
        resolve({ ok: false, status: 0, json: null, error: e.message });
      })
      .finally(() => clearTimeout(timer));
    timer = setTimeout(() => controller.abort(), timeoutMs || HTTP_TIMEOUT);
  });
}

function httpApi(path, method, body) {
  return ensureApiToken().then((token) =>
    httpApiOnce(path, method, body, token).then((res) => {
      if (res.status !== 401) return res;
      // The token went stale (config rewritten by a newer app run) or was never
      // readable because the app had not started yet: refresh it once and retry
      // a single time, then surface whatever we get.
      return refreshApiToken().then((t) => httpApiOnce(path, method, body, t));
    })
  );
}

async function httpPing() {
  const res = await httpApi("/api/ping", "GET");
  return res.status >= 200 && res.status < 500;
}

// Launch (or raise) the desktop app through its registered copper:// protocol.
// The app enforces autostart, protocol and pipe launch as part of startup.
function launchCopper() {
  try {
    chrome.tabs.create({ url: "copper://open" });
  } catch (e) { /* ignore */ }
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

// ---------------------------------------------------------------------------
// Dedupe / batch queue
// ---------------------------------------------------------------------------

const recent = new Map(); // url -> last dispatch timestamp
let queue = [];           // pending {url, filename, path, format}
let queueBusy = false;

function isDup(url) {
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
    const result = await sendToCopper(task.url, task.filename, task.path, task.format);
    if (task.resolve) task.resolve(result.accepted);
    await sleep(250);
  }
  queueBusy = false;
}

// ---------------------------------------------------------------------------
// Injection core
// ---------------------------------------------------------------------------

// Send one download request to the app over the local HTTP API. When the app is
// not reachable it is launched via copper:// and the request is retried once.
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

// Dispatch a single URL (deduped) -> the send queue -> the desktop app.
function dispatch(url, filename = "", path = "", format = settings.defaultFormat) {
  if (!url) return Promise.resolve(false);
  if (isDup(url)) return Promise.resolve(true);
  return new Promise((resolve) => {
    enqueue({ url, filename, path, format, resolve });
  });
}

// Dispatch many URLs (page-wide capture). Dedupe, cap at settings.maxBatch.
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
  const title = "Copper download could not be added";
  let message =
    "Copper Download Manager could not be reached. Please open Copper and try again.";
  if (result && result.detail) {
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

// ---------------------------------------------------------------------------
// Auto-capture of normal browser downloads
// ---------------------------------------------------------------------------

chrome.downloads.onCreated.addListener((item) => {
  getCurrentEnabledState().then((enabled) => {
    loadSettings(() => {
      if (!enabled || !settings.autoCapture) return;
      if (!item || !item.url || item.url.startsWith("blob:") || item.url.startsWith("data:")) return;
      const ext = extOfUrlOrName(item.url, item.filename || "");
      fetchFilterConfig().then((cfg) => {
        if (!shouldCaptureExtension(ext, cfg)) return;
        const downloadName = (item.filename || "").split(/[\\/]/).pop() || "";
        sendToCopper(item.url, downloadName, "", "").then((result) => {
          if (result.accepted) {
            try { chrome.downloads.cancel(item.id); } catch (e) { /* already finished */ }
          } else if (settings.notifyOnFail) {
            notifyDispatchFailed(result);
          }
        });
      });
    });
  });
});

// ---------------------------------------------------------------------------
// Context menus
// ---------------------------------------------------------------------------
//
// On MV3 the service worker is torn down and restarted constantly, and menus are
// rebuilt from more than one place (the boot path below and onInstalled). Each
// build is removeAll-then-create; if two builds overlap, the later create() runs
// while the earlier menu items still exist and Chrome reports "Cannot create
// item with duplicate id copper-link". Serialize every build through one promise
// chain so removeAll always completes before the next create, and read lastError
// in each create callback so a failure is never an unchecked runtime.lastError.
const MENU_DEFS = [
  { id: "copper-link", title: "Download link with Copper", contexts: ["link"] },
  { id: "copper-image", title: "Download image with Copper", contexts: ["image"] },
  { id: "copper-video", title: "Download video with Copper", contexts: ["video"] },
  { id: "copper-audio", title: "Download audio with Copper", contexts: ["audio"] },
  { id: "copper-selection-video", title: "Download selection as MP4 with Copper", contexts: ["selection"] },
  { id: "copper-selection-mp3", title: "Download selection as MP3 with Copper", contexts: ["selection"] },
  { id: "copper-page-media", title: "Download all media on this page with Copper", contexts: ["page"] },
  { id: "copper-magnet", title: "Send magnet link to Copper", contexts: ["link"] },
  { id: "copper-playlist-mp4", title: "Download full playlist as MP4", contexts: ["page", "link"] },
  { id: "copper-playlist-mp3", title: "Download full playlist as MP3", contexts: ["page", "link"] },
];

let menusBuildChain = Promise.resolve();

function removeAllMenus() {
  return new Promise((resolve) => {
    chrome.contextMenus.removeAll(() => {
      void chrome.runtime.lastError;
      resolve();
    });
  });
}

function buildMenus() {
  const run = () => removeAllMenus().then(() => {
    for (const def of MENU_DEFS) {
      chrome.contextMenus.create(def, () => void chrome.runtime.lastError);
    }
  });
  // .then(run, run): a failed previous link must not stall the chain.
  menusBuildChain = menusBuildChain.then(run, run);
  return menusBuildChain;
}

chrome.runtime.onInstalled.addListener(() => {
  chrome.storage.local.get([STORAGE_KEY], (items) => {
    if (items[STORAGE_KEY] === undefined) {
      chrome.storage.local.set({ [STORAGE_KEY]: true });
    }
  });
  loadSettings(() => {
    if (settings.contextMenus) buildMenus();
  });
});

chrome.contextMenus.onClicked.addListener((info) => {
  loadSettings(() => {
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
          notifyDispatchFailed({ detail: "no media found on this page" });
        }
      });
      return;
    }
    if (info.menuItemId === "copper-playlist-mp4" || info.menuItemId === "copper-playlist-mp3") {
      const url = info.linkUrl || info.pageUrl;
      if (url) {
        dispatch(url, "Full Playlist", "", info.menuItemId === "copper-playlist-mp3" ? "playlist-mp3" : "playlist-mp4");
      }
      return;
    }
  });
});

// Ask the content script on a tab for its collected media, then convert to URLs.
function collectPageMedia(opts) {
  return new Promise((resolve) => {
    const sendTo = (tabId) => {
      try {
        chrome.tabs.sendMessage(tabId, { action: "collectMedia" }, (resp) => {
          void chrome.runtime.lastError;
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
      chrome.tabs.query({ active: true, currentWindow: true }, (tabs) => {
        if (tabs && tabs[0] && tabs[0].id != null) sendTo(tabs[0].id);
        else resolve([]);
      });
    }
  });
}

// ---------------------------------------------------------------------------
// Message API (status page, content script, devtools/testing)
// ---------------------------------------------------------------------------

function rebuildMenus(enabled) {
  if (enabled) return buildMenus();
  // Route the disable path through the same chain: a bare removeAll() here could
  // interleave with a build's create() and either resurrect menus or fail.
  const run = () => removeAllMenus();
  menusBuildChain = menusBuildChain.then(run, run);
  return menusBuildChain;
}

chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  if (request && request.action === "getStatus") {
    Promise.all([getCurrentEnabledState(), httpPing()]).then(([enabled, reachable]) => {
      try {
        sendResponse({ enabled, reachable, settings, extensionId: chrome.runtime.id });
      } catch (e) { /* noop */ }
    });
    return true;
  }

  if (request && request.action === "getToken") {
    // The command-center page (copper.js) runs its own fetches; hand it the
    // same install-scoped token the background uses so those calls authenticate.
    ensureApiToken().then((token) => {
      try {
        sendResponse({ token: token || "" });
      } catch (e) { /* noop */ }
    });
    return true;
  }

  if (request && request.action === "getSettings") {
    loadSettings(() => {
      try {
        sendResponse({ settings });
      } catch (e) { /* noop */ }
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

  return false;
});

// ---------------------------------------------------------------------------
// Toolbar action
// ---------------------------------------------------------------------------

chrome.action.onClicked.addListener(() => {
  openStatusTab(false);
});

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

loadSettings(() => {
  const enabledPromise = getCurrentEnabledState();
  enabledPromise.then((enabled) => {
    if (settings.contextMenus && enabled) buildMenus();
  });
});