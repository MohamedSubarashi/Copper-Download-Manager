const COPPER_API = "http://localhost:24680";
let interceptionEnabled = true;

browser.downloads.onCreated.addListener(async (downloadItem) => {
  if (!interceptionEnabled) return;

  const settings = await browser.storage.local.get(["enabled", "port"]);
  if (settings.enabled === false) return;

  const port = settings.port || 24680;
  const apiUrl = `http://localhost:${port}`;

  browser.downloads.pause(downloadItem.id);
  browser.downloads.cancel(downloadItem.id);

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
      browser.notifications.create({
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

browser.runtime.onMessage.addListener((request, sender, sendResponse) => {
  if (request.action === "toggle") {
    interceptionEnabled = request.enabled;
    browser.storage.local.set({ enabled: request.enabled });
    sendResponse({ success: true });
  }

  if (request.action === "getStatus") {
    sendResponse({ enabled: interceptionEnabled });
  }

  if (request.action === "openCopper") {
    browser.tabs.create({ url: "copper://open" });
    sendResponse({ success: true });
  }

  return true;
});

browser.storage.local.get(["enabled"], (result) => {
  interceptionEnabled = result.enabled !== false;
});
