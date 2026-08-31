// Status/install page controller for the Copper extension.
//
// Runs inside a chrome-extension://.../copper.html tab. It asks the background
// service worker for the current install/capture state and shows the matching
// screen (ready / not-installed), plus an enable toggle and an "Open Copper"
// button.

const $ = (id) => document.getElementById(id);

function show(id) {
  for (const name of ["status-unknown", "status-ready", "status-not-installed"]) {
    $(name).hidden = name !== id;
  }
}

function sendMessage(message) {
  return new Promise((resolve) => {
    try {
      chrome.runtime.sendMessage(message, (response) => {
        const err = chrome.runtime.lastError;
        if (err || response === undefined) {
          resolve(null);
          return;
        }
        resolve(response);
      });
    } catch (e) {
      resolve(null);
    }
  });
}

async function refresh() {
  show("status-unknown");
  const params = new URLSearchParams(location.search);
  const forcedNotInstalled = params.get("status") === "not-installed";

  const status = await sendMessage({ action: "getStatus" });

  let enabled = true;
  if (status) {
    enabled = status.enabled !== false;
  }

  const reachable = forcedNotInstalled ? false : !!(status && status.reachable);
  renderToggle(enabled);

  if (reachable) {
    show("status-ready");
  } else {
    show("status-not-installed");
  }
}

function renderToggle(enabled) {
  $("btn-toggle").textContent = enabled ? "Turn Off Capture" : "Turn On Capture";
  $("toggle-label").textContent = enabled ? "Browser capture: on" : "Browser capture: off";
}

$("btn-toggle").addEventListener("click", async () => {
  const status = await sendMessage({ action: "getStatus" });
  const enabled = !(status && status.enabled !== false);
  await sendMessage({ action: "toggle", enabled });
  renderToggle(enabled);
});

$("btn-open").addEventListener("click", () => {
  sendMessage({ action: "openCopper" });
});

$("btn-recheck").addEventListener("click", () => {
  history.replaceState(null, "", chrome.runtime.getURL("copper.html"));
  refresh();
});

refresh();
