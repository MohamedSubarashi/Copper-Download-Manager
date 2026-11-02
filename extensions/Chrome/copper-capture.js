// Copper Download Manager - instant click interception (content script).
//
// Runs at document_start in the top frame and grabs clicks on obvious download
// links in the capture phase, before the browser starts its own download. The
// URL is handed to the background service worker, which dedupes it against
// chrome.downloads.onCreated and dispatches it to the desktop app. If Copper is
// unreachable, the background re-issues the download in the browser.

(() => {
  "use strict";

  const api =
    typeof browser !== "undefined" && browser.runtime ? browser : chrome;

  const ARCHIVE_EXT = /\.(zip|rar|7z|tar|gz|tgz|bz2|xz|zst|lz|lzma|iso|img|dmg|cab|arj)$/i;
  const INSTALLER_EXT = /\.(exe|msi|msix|msu|apk|appx|deb|rpm|pkg|snap|flatpak|appimage)$/i;
  const DOC_EXT = /\.(pdf|epub|mobi|azw3|doc|docx|xls|xlsx|ppt|pptx|odt|ods|odp|csv)$/i;
  const TORRENT_EXT = /\.(torrent)$/i;

  function fileNameOf(url) {
    try {
      const u = new URL(url, location.href);
      const raw = u.pathname.split("/").pop() || "";
      return decodeURIComponent(raw);
    } catch (e) {
      return "";
    }
  }

  function looksDownloadable(url, anchor) {
    if (!/^https?:/i.test(url)) return false;
    if (anchor && anchor.hasAttribute("download")) return true;
    let path = url;
    try {
      const u = new URL(url, location.href);
      path = u.pathname;
      const q = u.search.toLowerCase();
      if (q.includes("download=") || q.includes("attachment") || q.includes("filename=")) {
        return true;
      }
    } catch (e) {
      // keep the raw URL
    }
    return (
      ARCHIVE_EXT.test(path) ||
      INSTALLER_EXT.test(path) ||
      DOC_EXT.test(path) ||
      TORRENT_EXT.test(path)
    );
  }

  function onClick(event) {
    if (event.defaultPrevented) return;
    if (event.button !== 0) return;
    if (event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;

    let anchor = event.target;
    while (anchor && anchor.tagName !== "A") anchor = anchor.parentElement;
    if (!anchor || !anchor.href) return;

    const url = anchor.href;
    if (!looksDownloadable(url, anchor)) return;

    // Take the click away from the browser immediately.
    event.preventDefault();
    event.stopPropagation();
    event.stopImmediatePropagation();

    const payload = {
      action: "interceptClick",
      url,
      filename: fileNameOf(url),
      format: "",
    };

    try {
      api.runtime.sendMessage(payload, (reply) => {
        void api.runtime.lastError;
        if (!reply || !reply.ok) {
          // Background could not take it; fall back to normal navigation so the
          // link still works.
          window.location.href = url;
        }
      });
    } catch (e) {
      window.location.href = url;
    }
  }

  window.addEventListener("click", onClick, true);
  window.addEventListener("auxclick", (event) => {
    // Middle-click opens a new tab; only intercept left clicks (handled above).
    void event;
  }, true);
})();
