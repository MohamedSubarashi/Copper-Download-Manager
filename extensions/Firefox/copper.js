// Copper Download Manager - command center page (copper.html).
//
// Live download manager + settings panel + install status. Reads the app's
// /api/downloads over the localhost HTTP API (allowed for the extension page),
// talks to the background service worker for enable state and settings.

const $ = (id) => document.getElementById(id);

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

async function httpApi(path) {
  try {
    const res = await fetch(`${HTTP_API}${path}`, { cache: "no-store" });
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
    $("dl-count").textContent = "app is not reachable";
    list.textContent = "";
    const li = document.createElement("li");
    li.className = "dl-empty";
    li.textContent = "Copper is running but not responding.";
    list.appendChild(li);
    return;
  }
  const items = data.downloads || [];
  $("dl-count").textContent = items.length ? `${items.length} download(s)` : "no downloads yet";
  list.textContent = "";

  if (items.length === 0) {
    const li = document.createElement("li");
    li.className = "dl-empty";
    li.textContent = "No downloads yet. Use the right-click menu or the media bar to send files to Copper.";
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
    name.textContent = d.fileName || d.url || "download";
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
      d.type ? "via " + d.type : "",
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
  saved.textContent = "Saved";
  setTimeout(() => { saved.textContent = ""; }, 1500);
});

// ---------------------------------------------------------------------------
// Enable toggle + install flow
// ---------------------------------------------------------------------------

function renderToggle(enabled) {
  $("btn-toggle").textContent = enabled ? "Turn Off Capture" : "Turn On Capture";
  $("toggle-label").textContent = enabled ? "Browser capture: on" : "Browser capture: off";
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

refresh();