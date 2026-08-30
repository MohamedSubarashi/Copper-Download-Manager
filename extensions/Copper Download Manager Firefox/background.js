const STORAGE_KEY = "copperExtensionEnabled";
const HOST = "com.copper.dm";

// Returns true when the native host responded (and thus the desktop app's pipe
// intake is reachable). The host also launches the app if it isn't running.
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

async function pingCopper() {
  const rep = await sendNativeMessage({ action: "ping" });
  return !!(rep && rep.ok);
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

async function sendToCopper(url, filename = "", path = "") {
  const enabled = await getCurrentEnabledState();
  if (!enabled) {
    return { accepted: false, reason: "disabled" };
  }
  const rep = await sendNativeMessage({ action: "download", url, filename, path });
  if (rep && rep.ok) {
    return { accepted: true };
  }
  return { accepted: false, reason: "unreachable" };
}

async function dispatch(url, filename = "", path = "") {
  const result = await sendToCopper(url, filename, path);
  if (result.accepted) {
    return true;
  }
  if (result.reason === "unreachable") {
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
      sendResponse({ enabled, reachable, extensionId: chrome.runtime.id });
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
    dispatch(request.url, request.filename || "", request.path || "").then((done) => {
      sendResponse({ success: done });
    });
    return true;
  }

  if (request && request.action === "openCopper") {
    sendNativeMessage({ action: "open" }).then(() => sendResponse({ success: true }));
    return true;
  }

  if (request && request.action === "register") {
    registerWithCopper().then(() => sendResponse({ success: true }));
    return true;
  }

  return false;
});
