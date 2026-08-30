// Minimal trigger for the Copper custom protocol.
chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  if (message && message.type === "copper" && message.url) {
    try {
      const link = document.createElement("a");
      link.href = message.url;
      link.style.display = "none";
      document.body.appendChild(link);
      link.click();
      link.remove();
    } catch (e) {
      // The background script already creates a fallback tab if needed.
    }
    sendResponse({ ok: true });
  }
  return true;
});
