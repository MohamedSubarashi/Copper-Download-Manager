const COPPER_API = "http://localhost:24680";
let interceptionEnabled = true;

chrome.downloads.onCreated.addListener(async (downloadItem) => {
  if (!interceptionEnabled) return;

  const settings = await chrome.storage.local.get(["enabled", "port"]);
  if (settings.enabled === false) return;

  const port = settings.port || 24680;
  const apiUrl = `http://localhost:${port}`;

  chrome.downloads.pause(downloadItem.id);
  chrome.downloads.cancel(downloadItem.id);

  fetch(`${apiUrl}/api/download`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      url: downloadItem.url,
      filename: downloadItem.filename || "",
      path: downloadItem.savePath || ""
    })
  }).then(response => {
    if (response.ok) {
      chrome.notifications.create({
        type: "basic",
        iconUrl: "icons/icon128.png",
        title: "Copper Download Manager",
        message: "Download intercepted and sent to Copper"
      });
    }
  }).catch(err => {
    console.log("Copper API not reachable:", err.message);
  });
});

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

  return true;
});

chrome.storage.local.get(["enabled"], (result) => {
  interceptionEnabled = result.enabled !== false;
});
