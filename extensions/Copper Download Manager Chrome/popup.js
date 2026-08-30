const statusEl = document.getElementById("status");
const connDot = document.getElementById("connDot");
const openBtn = document.getElementById("openBtn");
const pageBtn = document.getElementById("pageBtn");
const urlInput = document.getElementById("urlInput");
const sendBtn = document.getElementById("sendBtn");

function setStatus(isEnabled) {
  statusEl.textContent = isEnabled ? "Copper enabled" : "Copper disabled";
  statusEl.className = isEnabled ? "status enabled" : "status disabled";
  connDot.className = isEnabled ? "conn-dot connected" : "conn-dot disconnected";
  connDot.title = isEnabled ? "Copper is enabled" : "Copper is disabled";
}

function sendUrl(url) {
  if (!url || !url.trim()) {
    return;
  }

  chrome.runtime.sendMessage({ action: "sendUrl", url: url.trim() }, (response) => {
    if (response && response.success) {
      urlInput.value = "";
      urlInput.placeholder = "Sent";
      setTimeout(() => {
        urlInput.placeholder = "Paste URL or magnet link";
      }, 1200);
      return;
    }

    statusEl.textContent = "Failed to send";
    statusEl.className = "status disabled";
    setTimeout(() => {
      chrome.runtime.sendMessage({ action: "getStatus" }, (r) => setStatus(Boolean(r && r.enabled)));
    }, 1200);
  });
}

chrome.runtime.sendMessage({ action: "getStatus" }, (response) => {
  setStatus(Boolean(response && response.enabled));
});

openBtn.addEventListener("click", () => {
  chrome.runtime.sendMessage({ action: "openCopper" }, () => {
    window.close();
  });
});

pageBtn.addEventListener("click", () => {
  chrome.tabs.query({ active: true, currentWindow: true }, (tabs) => {
    const tab = tabs && tabs[0];
    const pageUrl = tab && tab.url ? tab.url : "";
    if (!pageUrl || pageUrl.startsWith("chrome://") || pageUrl.startsWith("about:")) {
      statusEl.textContent = "Open a web page first";
      statusEl.className = "status disabled";
      return;
    }
    sendUrl(pageUrl);
  });
});

sendBtn.addEventListener("click", () => {
  sendUrl(urlInput.value);
});

urlInput.addEventListener("keydown", (event) => {
  if (event.key === "Enter") {
    sendUrl(urlInput.value);
  }
});
