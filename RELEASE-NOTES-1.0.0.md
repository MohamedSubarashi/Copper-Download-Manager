# Copper Download Manager 1.0.0

The first stable release. This version focuses on the browser-integration
experience: Copper now wins the race against the browser, stays out of the way
while it captures downloads, and supports every major Chromium browser.

## Highlights

### Faster than the browser
- The extension cancels `chrome.downloads.onCreated` **synchronously**, before
  it does any storage, filter or token work. The browser no longer gets a head
  start on the file.
- Settings, the API token and the include/exclude filter are cached in the
  service worker instead of being re-read for every download.
- A new `copper-capture.js` content script grabs clicks on download links in the
  capture phase at `document_start`, so archive/installer/document links are
  handed to Copper before the browser navigates or downloads.
- Single dispatches no longer wait for a serialized batch queue; batch imports
  are faster as well.
- If Copper is unreachable and the fallback setting is on, the extension
  re-issues the download in the browser so nothing is lost.

### No more popups or repeated prompts
- Browser-injected downloads no longer raise or focus the main window. The
  window only appears for an explicit open, a second launch, or when the new
  **Show the main window when a download starts** setting is enabled.
- Allow/deny decisions for browser extensions are persisted in the app
  database, so the authorization prompt appears once per extension. A reset
  button lives in **Settings → System → Browser Extensions**.

### Chromium-family support
- Native-messaging host manifests and Windows registry keys are now registered
  for Chrome, Edge, Brave, Vivaldi, Opera, Opera GX and Chromium (plus Firefox).
  One registration covers the whole Chromium family, which removes the repeated
  authorization prompt when switching browsers.

### Power features
- **Clipboard monitor** — optionally add a copied `http(s)`, `ftp` or `magnet`
  link automatically.
- **Batch URL import** — paste many links at once from the Add URL dialog.
- **Search & filter** — narrow the download table by name, URL, type or status.
- **Completion notifications** — a tray balloon when a top-level download
  finishes (playlist children stay quiet).

## Packaging

1.0.0 ships one asset: `CopperDownloadManager-1.0.0-setup.exe`. No portable
zip, no cross-platform archives. The extension sources are in the repository
and can be loaded unpacked for development.

## Notes

- Windows SmartScreen may warn because the installer is unsigned.
- Existing settings and downloads are preserved when upgrading.
