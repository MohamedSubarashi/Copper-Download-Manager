// background.js — Copper Download Manager (copper:// scheme injection)
// IDM/FDM pattern: instead of polling a localhost port, the extension navigates
// to a copper://download URL which the OS routes to the registered Copper
// Download Manager application. Works whether or not the app is running at the
// moment of the click (the OS launches it).
let interceptionEnabled = true;
let interceptedCount = 0;
let history = [];

function buildCopperUrl(url, filename, filePath) {
  const enc = (v) => (v == null ? "" : encodeURIComponent(String(v)));
  return `copper://download?url=${enc(url)}&filename=${enc(filename)}&path=${enc(filePath)}`;
}

// Primary trigger: ask the content script on the focused tab to click a hidden
// copper:// anchor. This keeps the user on the page and asks the OS/browser to
// open the registered handler.
async function focusAndTrigger(copperUrl) {
  try {
    const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
    if (tab && tab.id != null) {
      await chrome.tabs.sendMessage(tab.id, { type: "copper", url: copperUrl });
      return true;
    }
  } catch (e) {
    // no content script / restricted page — fall through to tab creation
  }
  await chrome.tabs.create({ url: copperUrl });
  return true;
}

function addToHistory(url, filename) {
  const entry = { url, filename, time: Date.now() };
  history.unshift(entry);
  if (history.length > 50) history = history.slice(0, 50);
  chrome.storage.local.set({ downloadHistory: history });
}

function recordIntercept(url, filename) {
  interceptedCount++;
  chrome.storage.local.set({ interceptedCount });
  chrome.action.setBadgeText({ text: String(interceptedCount) });
  chrome.action.setBadgeBackgroundColor({ color: "#e8a838" });
  addToHistory(url, filename || url.split("/").pop().split("?")[0] || "unknown");
}

function dispatch(url, filename, filePath) {
  focusAndTrigger(buildCopperUrl(url, filename, filePath)).then(() => {
    recordIntercept(url, filename);
  });
}

async function shouldIntercept(filename, url) {
  const s = await chrome.storage.local.get(["fileFilter", "fileFilterMode", "domainBlacklist", "domainWhitelist", "domainFilterEnabled"]);
  if (!interceptionEnabled || s.enabled === false) return false;

  if (s.domainFilterEnabled) {
    try {
      const domain = new URL(url).hostname;
      if (s.domainBlacklist && s.domainBlacklist.includes(domain)) return false;
      if (s.domainWhitelist && s.domainWhitelist.length > 0 && !s.domainWhitelist.includes(domain)) return false;
    } catch {}
  }

  if (s.fileFilter && s.fileFilter.length > 0) {
    const ext = filename ? filename.split(".").pop().toLowerCase() : "";
    const matches = s.fileFilter.some(f => ext === f.toLowerCase() || filename.toLowerCase().includes(f.toLowerCase()));
    if (s.fileFilterMode === "exclude" && matches) return false;
    if (s.fileFilterMode === "include" && !matches) return false;
  }

  return true;
}

// --- Download interception ---
chrome.downloads.onCreated.addListener(async (downloadItem) => {
  const settings = await chrome.storage.local.get(["enabled"]);
  if (settings.enabled === false) return;

  const filename = downloadItem.filename ? downloadItem.filename.split(/[/\\]/).pop() : "";
  if (!(await shouldIntercept(filename, downloadItem.url))) return;

  try {
    chrome.downloads.pause(downloadItem.id);
    chrome.downloads.cancel(downloadItem.id);
  } catch (e) {}
  dispatch(downloadItem.url, filename, downloadItem.savePath || "");
});

// --- Context menus ---
function createMenus() {
  chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({ id: "copper-download-link", title: "Download link with Copper", contexts: ["link"] });
    chrome.contextMenus.create({ id: "copper-download-image", title: "Download image with Copper", contexts: ["image"] });
    chrome.contextMenus.create({ id: "copper-download-video", title: "Download video with Copper", contexts: ["video"] });
    chrome.contextMenus.create({ id: "copper-download-selection", title: "Download URL with Copper", contexts: ["selection"] });
    chrome.contextMenus.create({ id: "copper-download-all-images", title: "Download all images on page with Copper", contexts: ["page"] });
    chrome.contextMenus.create({ id: "copper-download-all-links", title: "Download all links on page with Copper", contexts: ["page"] });
  });
}

chrome.runtime.onInstalled.addListener(() => {
  createMenus();
  chrome.storage.local.get(["downloadHistory", "interceptedCount"], (s) => {
    history = s.downloadHistory || [];
    interceptedCount = s.interceptedCount || 0;
    if (interceptedCount > 0) {
      chrome.action.setBadgeText({ text: String(interceptedCount) });
      chrome.action.setBadgeBackgroundColor({ color: "#e8a838" });
    }
  });
});
chrome.runtime.onStartup.addListener(() => createMenus());

chrome.contextMenus.onClicked.addListener(async (info, tab) => {
  if (info.menuItemId === "copper-download-link" && info.linkUrl) {
    dispatch(info.linkUrl, info.linkUrl.split("/").pop().split("?")[0]);
  }
  if (info.menuItemId === "copper-download-image" && info.srcUrl) {
    dispatch(info.srcUrl, info.srcUrl.split("/").pop().split("?")[0]);
  }
  if (info.menuItemId === "copper-download-video" && info.srcUrl) {
    dispatch(info.srcUrl, info.srcUrl.split("/").pop().split("?")[0]);
  }
  if (info.menuItemId === "copper-download-selection" && info.selectionText) {
    const text = info.selectionText.trim();
    if (text.startsWith("http://") || text.startsWith("https://")) {
      dispatch(text, text.split("/").pop().split("?")[0] || "download");
    }
  }
  if (info.menuItemId === "copper-download-all-images" || info.menuItemId === "copper-download-all-links") {
    try {
      const results = await chrome.scripting.executeScript({
        target: { tabId: tab.id },
        func: (type) => {
          const urls = [];
          if (type === "images") {
            document.querySelectorAll("img").forEach(img => {
              if (img.src && img.src.startsWith("http")) urls.push(img.src);
            });
          } else {
            document.querySelectorAll("a[href]").forEach(a => {
              const href = a.href;
              if (href && href.startsWith("http")) urls.push(href);
            });
          }
          return urls;
        },
        args: [info.menuItemId === "copper-download-all-images" ? "images" : "links"]
      });
      if (results && results[0] && results[0].result) {
        const urls = results[0].result;
        for (const u of urls) dispatch(u, u.split("/").pop().split("?")[0] || "download");
        chrome.notifications.create({
          type: "basic",
          iconUrl: "icons/icon128.png",
          title: "Copper Download Manager",
          message: `Sent ${urls.length} URLs to Copper`
        });
      }
    } catch (e) {
      console.log("Script injection failed:", e.message);
    }
  }
});

// --- Message handling ---
chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  if (request.action === "toggle") {
    interceptionEnabled = request.enabled;
    chrome.storage.local.set({ enabled: request.enabled });
    sendResponse({ success: true });
  }

  if (request.action === "getStatus") {
    sendResponse({ enabled: interceptionEnabled });
  }

  if (request.action === "openCopper") {
    chrome.tabs.create({ url: "copper://open" });
    sendResponse({ success: true });
  }

  if (request.action === "sendUrl") {
    dispatch(request.url, request.filename || "", request.path || "");
    const filename = request.filename || request.url.split("/").pop().split("?")[0] || "unknown";
    recordIntercept(request.url, filename);
    sendResponse({ success: true });
  }

  if (request.action === "detectMedia") {
    chrome.tabs.query({ active: true, currentWindow: true }, async (tabs) => {
      if (!tabs[0]) return sendResponse({ media: [] });
      try {
        const results = await chrome.scripting.executeScript({
          target: { tabId: tabs[0].id },
          func: () => {
            const media = [];
            const seen = new Set();
            document.querySelectorAll("img, video, audio, source, a[href]").forEach(el => {
              let url = el.src || el.href || el.currentSrc || "";
              if (!url || url.startsWith("data:") || url.startsWith("blob:")) return;
              try { url = new URL(url, document.baseURI).href; } catch { return; }
              if (seen.has(url)) return;
              seen.add(url);
              let type = "other";
              const tag = el.tagName.toLowerCase();
              if (tag === "img") type = "image";
              else if (tag === "video" || tag === "source") type = "video";
              else if (tag === "audio") type = "audio";
              else if (tag === "a") {
                const ext = url.split(".").pop().toLowerCase().split("?")[0];
                if (["jpg","jpeg","png","gif","webp","svg","bmp","ico"].includes(ext)) type = "image";
                else if (["mp4","webm","mkv","avi","mov","flv"].includes(ext)) type = "video";
                else if (["mp3","wav","ogg","flac","aac"].includes(ext)) type = "audio";
                else if (["pdf"].includes(ext)) type = "document";
                else if (["zip","rar","7z","tar","gz"].includes(ext)) type = "archive";
                else if (["exe","msi","dmg","app","deb","rpm"].includes(ext)) type = "executable";
              }
              const filename = url.split("/").pop().split("?")[0] || "unknown";
              media.push({ url, filename, type });
            });
            return media;
          }
        });
        sendResponse({ media: results && results[0] ? results[0].result : [] });
      } catch (e) {
        sendResponse({ media: [], error: e.message });
      }
    });
    return true;
  }

  if (request.action === "getHistory") {
    chrome.storage.local.get(["downloadHistory"], (s) => {
      sendResponse({ history: s.downloadHistory || [] });
    });
    return true;
  }

  if (request.action === "clearHistory") {
    history = [];
    interceptedCount = 0;
    chrome.storage.local.set({ downloadHistory: [], interceptedCount: 0 });
    chrome.action.setBadgeText({ text: "" });
    sendResponse({ success: true });
  }

  if (request.action === "updateSettings") {
    chrome.storage.local.set(request.settings);
    sendResponse({ success: true });
  }

  if (request.action === "getSettings") {
    const keys = ["fileFilter", "fileFilterMode", "domainBlacklist", "domainWhitelist", "domainFilterEnabled"];
    chrome.storage.local.get(keys, (s) => sendResponse(s));
    return true;
  }

  return true;
});

chrome.storage.local.get(["enabled"], (result) => {
  interceptionEnabled = result.enabled !== false;
});
