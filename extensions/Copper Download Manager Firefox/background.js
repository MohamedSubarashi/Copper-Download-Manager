const STORAGE_KEY = "copperExtensionEnabled";

const copperUrlFor = (url, filename = "", path = "") => {
  const enc = (value) => (value == null ? "" : encodeURIComponent(String(value)));
  return `copper://download?url=${enc(url)}&filename=${enc(filename)}&path=${enc(path)}`;
};

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

async function openCopper(url, filename = "", path = "") {
  const copperUrl = copperUrlFor(url, filename, path);
  const enabled = await getCurrentEnabledState();
  if (!enabled) {
    return;
  }

  try {
    const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
    if (tab && tab.id != null) {
      await chrome.tabs.sendMessage(tab.id, { type: "copper", url: copperUrl });
      return;
    }
  } catch (e) {
    // Some pages do not allow an injected content script.
  }

  chrome.tabs.create({ url: copperUrl });
}

function dispatch(url, filename = "", path = "") {
  openCopper(url, filename, path);
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
  if (info.menuItemId === "copper-link" && info.linkUrl) {
    dispatch(info.linkUrl, info.linkUrl.split("/").pop().split("?")[0] || "download");
    return;
  }

  if ((info.menuItemId === "copper-image" || info.menuItemId === "copper-video") && info.srcUrl) {
    dispatch(info.srcUrl, info.srcUrl.split("/").pop().split("?")[0] || "download");
    return;
  }

  if (info.menuItemId === "copper-selection" && info.selectionText) {
    const text = info.selectionText.trim();
    if (text.startsWith("http://") || text.startsWith("https://") || text.startsWith("ftp://") || text.startsWith("magnet:")) {
      dispatch(text, text.split("/").pop().split("?")[0] || "download");
    }
  }
});

chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  if (request && request.action === "getStatus") {
    getCurrentEnabledState().then((enabled) => {
      sendResponse({ enabled });
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
    dispatch(request.url, request.filename || "", request.path || "");
    sendResponse({ success: true });
    return true;
  }

  if (request && request.action === "openCopper") {
    chrome.tabs.create({ url: "copper://open" });
    sendResponse({ success: true });
    return true;
  }

  return false;
});
