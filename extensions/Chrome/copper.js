// Copper Download Manager - command center page (copper.html).
//
// Live download manager + settings panel + install status. Reads the app's
// /api/downloads over the localhost HTTP API (allowed for the extension page),
// talks to the background service worker for enable state and settings.

const $ = (id) => document.getElementById(id);

// Localisation: strings live in _locales/<lang>/messages.json and are read
// through the extension i18n API.
function t(key, subs) {
  try {
    return chrome.i18n.getMessage(key, subs) || "";
  } catch (e) {
    return "";
  }
}

// Like t(), but falls back to the key so a value is always returned.
function msg(key, subs) {
  return t(key, subs) || key;
}

// Replace every [data-i18n] / [data-i18n-title] element with its localized
// text (leaving the built-in English when a message is missing) and switch the
// document to RTL for right-to-left languages.
function localize() {
  let uiLang = "en";
  try {
    uiLang = chrome.i18n.getUILanguage() || "en";
  } catch (e) { /* keep en */ }
  const base = uiLang.split("-")[0].toLowerCase();
  document.documentElement.lang = base;
  if (base === "ar") document.documentElement.dir = "rtl";
  for (const el of document.querySelectorAll("[data-i18n]")) {
    const text = t(el.getAttribute("data-i18n"));
    if (text) el.textContent = text;
  }
  for (const el of document.querySelectorAll("[data-i18n-title]")) {
    const text = t(el.getAttribute("data-i18n-title"));
    if (text) el.title = text;
  }
}

const HTTP_API = "http://127.0.0.1:24680";
const POLL_MS = 2000;
let pollTimer = null;

function sendMessage(message) {
  return new Promise((resolve) => {
    try {
      chrome.runtime.sendMessage(message, (response) => {
        const err = chrome.runtime.lastError;
        if (err || response === undefined) {
          resolve(null);
          return;
        }
        resolve(response);
      });
    } catch (e) {
      resolve(null);
    }
  });
}

// The app's local API requires an install-scoped token (0.4.0). This page cannot
// read the shared config itself, so it asks the background service worker, which
// obtained the token from the native-messaging host.
let apiToken = "";
let apiTokenLoaded = false;

async function loadApiToken(force) {
  if (apiTokenLoaded && !force) return apiToken;
  const reply = await sendMessage({ action: "getToken" });
  apiToken = reply && reply.token ? String(reply.token) : "";
  apiTokenLoaded = apiToken !== "";
  return apiToken;
}

async function httpApiFetch(path, token) {
  const opts = { cache: "no-store" };
  if (token) opts.headers = { "X-Copper-Token": token };
  return fetch(`${HTTP_API}${path}`, opts);
}

async function httpApi(path) {
  try {
    let res = await httpApiFetch(path, await loadApiToken(false));
    if (res.status === 401) {
      res = await httpApiFetch(path, await loadApiToken(true));
    }
    return await res.json();
  } catch (e) {
    return null;
  }
}

function fmtSize(bytes) {
  if (bytes == null || bytes < 0) return "";
  const units = ["B", "KB", "MB", "GB", "TB"];
  let n = bytes;
  let i = 0;
  while (n >= 1024 && i < units.length - 1) {
    n /= 1024;
    i += 1;
  }
  return (i === 0 ? n : n.toFixed(1)) + " " + units[i];
}

function fmtSpeed(bytesPerSec) {
  return bytesPerSec > 0 ? fmtSize(bytesPerSec) + "/s" : "";
}

function statusBadgeClass(status) {
  const s = String(status || "").toLowerCase();
  if (s.includes("download") || s.includes("queued")) return "dl-active";
  if (s.includes("complete") || s.includes("done")) return "dl-done";
  if (s.includes("fail") || s.includes("error") || s.includes("cancel")) return "dl-fail";
  if (s.includes("pause")) return "dl-pause";
  return "dl-active";
}

function show(id) {
  for (const name of ["status-unknown", "status-ready", "status-not-installed", "settings-panel"]) {
    $(name).hidden = name !== id;
  }
}

// ---------------------------------------------------------------------------
// Download list
// ---------------------------------------------------------------------------

async function refreshDownloads() {
  const data = await httpApi("/api/downloads");
  const list = $("dl-list");
  if (!data || !Array.isArray(data.downloads)) {
    $("dl-count").textContent = msg("appNotReachable");
    list.textContent = "";
    const li = document.createElement("li");
    li.className = "dl-empty";
    li.textContent = msg("notResponding");
    list.appendChild(li);
    return;
  }
  const items = data.downloads || [];
  $("dl-count").textContent = items.length
    ? msg("downloadsCount", [String(items.length)])
    : msg("noDownloadsCount");
  list.textContent = "";

  if (items.length === 0) {
    const li = document.createElement("li");
    li.className = "dl-empty";
    li.textContent = msg("noDownloadsYet");
    list.appendChild(li);
    return;
  }

  for (const d of items) {
    const li = document.createElement("li");
    li.className = "dl-row";

    const head = document.createElement("div");
    head.className = "dl-head";
    const name = document.createElement("span");
    name.className = "dl-name";
    name.textContent = d.fileName || d.url || msg("downloadFallback");
    name.title = d.url || "";
    const badge = document.createElement("span");
    badge.className = "dl-badge " + statusBadgeClass(d.status);
    badge.textContent = d.status || "?";
    head.appendChild(name);
    head.appendChild(badge);

    const barOuter = document.createElement("div");
    barOuter.className = "dl-bar";
    const barInner = document.createElement("div");
    barInner.className = "dl-bar-inner";
    const pct = Math.max(0, Math.min(100, Number(d.progress) || 0));
    barInner.style.width = pct + "%";
    barOuter.appendChild(barInner);

    const meta = document.createElement("div");
    meta.className = "dl-meta";
    meta.textContent = [
      pct.toFixed(1) + "%",
      fmtSpeed(d.speed),
      fmtSize(d.downloadedSize) + " / " + fmtSize(d.totalSize),
      d.type ? msg("viaFormat", [d.type]) : "",
    ].filter(Boolean).join(" · ");

    li.appendChild(head);
    li.appendChild(barOuter);
    li.appendChild(meta);
    list.appendChild(li);
  }
}

function startPolling() {
  stopPolling();
  refreshDownloads();
  pollTimer = setInterval(refreshDownloads, POLL_MS);
}

function stopPolling() {
  if (pollTimer) {
    clearInterval(pollTimer);
    pollTimer = null;
  }
}

document.addEventListener("visibilitychange", () => {
  if (!pollTimer) return;
  if (document.hidden) stopPolling();
  else startPolling();
});

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

function renderSettings(s) {
  if (!s) return;
  $("set-autoCapture").checked = s.autoCapture !== false;
  $("set-showBar").checked = s.showBar !== false;
  $("set-contextMenus").checked = s.contextMenus !== false;
  $("set-notifyOnFail").checked = s.notifyOnFail !== false;
  $("set-defaultFormat").value = s.defaultFormat || "mp4";
}

$("btn-save").addEventListener("click", async () => {
  const next = {
    autoCapture: $("set-autoCapture").checked,
    showBar: $("set-showBar").checked,
    contextMenus: $("set-contextMenus").checked,
    notifyOnFail: $("set-notifyOnFail").checked,
    defaultFormat: $("set-defaultFormat").value,
  };
  await sendMessage({ action: "setSettings", settings: next });
  const saved = $("save-state");
  saved.textContent = msg("saved");
  setTimeout(() => { saved.textContent = ""; }, 1500);
});

// ---------------------------------------------------------------------------
// Enable toggle + install flow
// ---------------------------------------------------------------------------

function renderToggle(enabled) {
  $("btn-toggle").textContent = enabled ? msg("turnOffCapture") : msg("turnOnCapture");
  $("toggle-label").textContent = enabled ? msg("captureOn") : msg("captureOff");
}

$("btn-toggle").addEventListener("click", async () => {
  const status = await sendMessage({ action: "getStatus" });
  const enabled = !(status && status.enabled !== false);
  await sendMessage({ action: "toggle", enabled });
  renderToggle(enabled);
});

$("btn-open").addEventListener("click", () => {
  sendMessage({ action: "openCopper" });
});

for (const id of ["btn-recheck", "btn-recheck2"]) {
  $(id).addEventListener("click", () => {
    history.replaceState(null, "", chrome.runtime.getURL("copper.html"));
    refresh();
  });
}

async function refresh() {
  show("status-unknown");
  stopPolling();

  const params = new URLSearchParams(location.search);
  const forcedNotInstalled = params.get("status") === "not-installed";

  const status = await sendMessage({ action: "getStatus" });
  const settingsResp = await sendMessage({ action: "getSettings" });

  const enabled = !status || status.enabled !== false;
  renderToggle(enabled);
  renderSettings(settingsResp ? settingsResp.settings : null);

  const reachable = forcedNotInstalled ? false : !!(status && status.reachable);

  if (reachable) {
    show("status-ready");
    startPolling();
  } else {
    show("status-not-installed");
  }
}

localize();
refresh();