const statusEl = document.getElementById("status");
const toggleBtn = document.getElementById("toggleBtn");
const openBtn = document.getElementById("openBtn");

function updateUI(enabled) {
  if (enabled) {
    statusEl.textContent = "Interception: Enabled";
    statusEl.className = "status enabled";
    toggleBtn.textContent = "Disable Interception";
    toggleBtn.className = "toggle-btn disable";
  } else {
    statusEl.textContent = "Interception: Disabled";
    statusEl.className = "status disabled";
    toggleBtn.textContent = "Enable Interception";
    toggleBtn.className = "toggle-btn enable";
  }
}

chrome.runtime.sendMessage({ action: "getStatus" }, (response) => {
  if (response) updateUI(response.enabled);
});

toggleBtn.addEventListener("click", () => {
  chrome.runtime.sendMessage({ action: "getStatus" }, (response) => {
    const newState = !response.enabled;
    chrome.runtime.sendMessage({ action: "toggle", enabled: newState }, () => {
      updateUI(newState);
    });
  });
});

openBtn.addEventListener("click", () => {
  chrome.runtime.sendMessage({ action: "openCopper" });
});
