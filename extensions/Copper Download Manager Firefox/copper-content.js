// copper-content.js
// Reliably triggers the copper:// custom protocol from the active tab.
// Injecting a hidden anchor with the copper:// URL and clicking it asks the OS
// / browser to open the registered handler (Copper Download Manager) while the
// user stays on the current page. This is the same injection pattern used by
// IDM / FDM / DownThemAll.
chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (msg && msg.type === "copper" && msg.url) {
    triggerCopper(msg.url);
    sendResponse({ ok: true });
  }
  return true;
});

function triggerCopper(url) {
  try {
    const a = document.createElement("a");
    a.href = url;
    a.setAttribute("style", "display:none;position:absolute;left:-9999px;top:-9999px;");
    a.setAttribute("id", "__copper_protocol_trigger__");
    document.body.appendChild(a);
    a.click();
    setTimeout(() => {
      const el = document.getElementById("__copper_protocol_trigger__");
      if (el) el.remove();
    }, 500);
  } catch (e) {
    // Some pages restrict synthetic clicks; fall back is handled by background.
  }
}
