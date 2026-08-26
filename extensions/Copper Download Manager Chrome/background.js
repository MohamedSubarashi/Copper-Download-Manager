const DEFAULT_PORT = 24680;
let interceptionEnabled = true;
let copperConnected = false;
let interceptedCount = 0;
let history = [];

function getApiUrl(port) {
  return `http://localhost:${port || DEFAULT_PORT}`;
}

async function getPort() {
  const s = await chrome.storage.local.get(["port"]);
  return s.port || DEFAULT_PORT;
}

async function sendToCopper(url, filename, filePath) {
  const port = await getPort();
  const apiUrl = getApiUrl(port);
  try {
    const resp = await fetch(`${apiUrl}/api/download`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ url, filename: filename || "", path: filePath || "" })
    });
    if (resp.ok) {
      interceptedCount++;
      chrome.storage.local.set({ interceptedCount });
      chrome.action.setBadgeText({ text: String(interceptedCount) });
      chrome.action.setBadgeBackgroundColor({ color: "#e8a838" });
      addToHistory(url, filename || url.split("/").pop() || "unknown");
      return true;
    }
  } catch (e) {
    console.log("Copper API not reachable:", e.message);
  }
  return false;
}

function addToHistory(url, filename) {
  const entry = { url, filename, time: Date.now() };
  history.unshift(entry);
  if (history.length > 50) history = history.slice(0, 50);
  chrome.storage.local.set({ downloadHistory: history });
}

async function pingCopper() {
  const port = await getPort();
  try {
    const resp = await fetch(`${getApiUrl(port)}/api/ping`, { method: "GET" });
    copperConnected = resp.ok;
  } catch {
    copperConnected = false;
  }
  chrome.storage.local.set({ copperConnected });
  return copperConnected;
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

  chrome.downloads.pause(downloadItem.id);
  chrome.downloads.cancel(downloadItem.id);
  await sendToCopper(downloadItem.url, filename, downloadItem.savePath || "");
});

// --- Context menus ---
chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.create({
    id: "copper-download-link",
    title: "Download link with Copper",
    contexts: ["link"]
  });
  chrome.contextMenus.create({
    id: "copper-download-image",
    title: "Download image with Copper",
    contexts: ["image"]
  });
  chrome.contextMenus.create({
    id: "copper-download-video",
    title: "Download video with Copper",
    contexts: ["video"]
  });
  chrome.contextMenus.create({
    id: "copper-download-selection",
    title: "Download URL with Copper",
    contexts: ["selection"]
  });
  chrome.contextMenus.create({
    id: "copper-download-all-images",
    title: "Download all images on page with Copper",
    contexts: ["page"]
  });
  chrome.contextMenus.create({
    id: "copper-download-all-links",
    title: "Download all links on page with Copper",
    contexts: ["page"]
  });

  chrome.storage.local.get(["downloadHistory", "interceptedCount"], (s) => {
    history = s.downloadHistory || [];
    interceptedCount = s.interceptedCount || 0;
    if (interceptedCount > 0) {
      chrome.action.setBadgeText({ text: String(interceptedCount) });
      chrome.action.setBadgeBackgroundColor({ color: "#e8a838" });
    }
  });
});

chrome.contextMenus.onClicked.addListener(async (info, tab) => {
  if (info.menuItemId === "copper-download-link" && info.linkUrl) {
    const filename = info.linkUrl.split("/").pop().split("?")[0];
    await sendToCopper(info.linkUrl, filename);
  }
  if (info.menuItemId === "copper-download-image" && info.srcUrl) {
    const filename = info.srcUrl.split("/").pop().split("?")[0];
    await sendToCopper(info.srcUrl, filename);
  }
  if (info.menuItemId === "copper-download-video" && info.srcUrl) {
    const filename = info.srcUrl.split("/").pop().split("?")[0];
    await sendToCopper(info.srcUrl, filename);
  }
  if (info.menuItemId === "copper-download-selection" && info.selectionText) {
    const text = info.selectionText.trim();
    if (text.startsWith("http://") || text.startsWith("https://")) {
      const filename = text.split("/").pop().split("?")[0] || "download";
      await sendToCopper(text, filename);
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
        for (const u of urls) {
          await sendToCopper(u, u.split("/").pop().split("?")[0] || "download");
        }
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

// --- Page media detection (injected from popup) ---
chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  if (request.action === "toggle") {
    interceptionEnabled = request.enabled;
    chrome.storage.local.set({ enabled: request.enabled });
    sendResponse({ success: true });
  }

  if (request.action === "getStatus") {
    sendResponse({ enabled: interceptionEnabled });
  }

  if (request.action === "ping") {
    pingCopper().then(ok => sendResponse({ connected: ok }));
    return true;
  }

  if (request.action === "openCopper") {
    chrome.tabs.create({ url: "copper://open" });
    sendResponse({ success: true });
  }

  if (request.action === "sendUrl") {
    sendToCopper(request.url, request.filename || "").then(ok => {
      sendResponse({ success: ok });
    });
    return true;
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
    const keys = ["fileFilter", "fileFilterMode", "domainBlacklist", "domainWhitelist", "domainFilterEnabled", "port"];
    chrome.storage.local.get(keys, (s) => sendResponse(s));
    return true;
  }

  return true;
});

// --- Periodic ping ---
setInterval(pingCopper, 15000);
pingCopper();

chrome.storage.local.get(["enabled"], (result) => {
  interceptionEnabled = result.enabled !== false;
});
