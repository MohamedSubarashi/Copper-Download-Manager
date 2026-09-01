const STORAGE_KEY = "copperExtensionEnabled";
const HOST = "com.copper.dm";
const STATUS_PAGE = "copper.html";
const HTTP_API = "http://127.0.0.1:24680";
const HTTP_PING_TIMEOUT = 1500;

// Sends a JSON message to the native host over native messaging. The host
// forwards it to the desktop app over the named pipe and, if the app isn't
// running, launches it first (IDM-style on-demand launch).
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

// Whether the desktop program is genuinely absent: either the native host
// could not be reached at all (host not installed/registered), or the host is
// present but reports the app executable was not found.
function isNotInstalledReply(rep) {
  if (!rep || rep.ok === undefined) return true;
  if (rep.ok) return false;
  const msg = (rep.error || "").toLowerCase();
  return msg.includes("not found") || msg.includes("not installed");
}

// True when the program is installed and the app is reachable (the host also
// launches it if it isn't running). Falls back to the desktop app's HTTP API
// when the native host isn't registered yet (e.g. dev or portablable setups):
// "installed" means EITHER the native host responds OR the HTTP API answers.
async function pingCopper() {
  const rep = await sendNativeMessage({ action: "ping" });
  if (rep && rep.ok) return true;

  // Native host missing/not responding: try the app's own HTTP ping endpoint.
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

// Open the bundled status page. When the program is not installed, pass a
// query flag so the page shows the 'not installed' instructions immediately.
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

// Route a URL to Copper. On success, the download is handed to the app. If the
// program is not installed, open the 'not installed' page instead of silently
// failing. "format" selects the output container (mp4 | mp3).
async function dispatch(url, filename = "", path = "", format = "mp4") {
  const result = await sendToCopper(url, filename, path, format);
  if (result.accepted) {
    return true;
  }
  if (result.notInstalled) {
    openStatusTab(true);
  } else {
    // Installed but transiently unreachable: alert the user.
    notifyCopperUnreachable();
  }
  return false;
}

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
});

// No popup (IDM-style): the toolbar button opens the status page.
chrome.action.onClicked.addListener(() => {
  openStatusTab(false);
});

// First run against a freshly installed host: report our runtime.id so the app
// can add it to the Chrome host manifest allowed_origins (load-dependent for
// unpacked/dev builds, unlike Firefox's stable gecko.id). Best-effort.
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
    // The host's "ping" ensures the desktop app is running (launching it on
    // demand if needed), so it doubles as "open Copper".
    pingCopper().then((running) => sendResponse({ success: running }));
    return true;
  }

  if (request && request.action === "register") {
    registerWithCopper().then(() => sendResponse({ success: true }));
    return true;
  }

  return false;
});
