const statusEl = document.getElementById("status");
const toggleBtn = document.getElementById("toggleBtn");
const openBtn = document.getElementById("openBtn");
const connDot = document.getElementById("connDot");
const urlInput = document.getElementById("urlInput");
const sendBtn = document.getElementById("sendBtn");
const detectBtn = document.getElementById("detectBtn");
const downloadAllBtn = document.getElementById("downloadAllBtn");
const mediaFilter = document.getElementById("mediaFilter");
const mediaList = document.getElementById("mediaList");
const historyList = document.getElementById("historyList");
const historyCount = document.getElementById("historyCount");
const clearHistoryBtn = document.getElementById("clearHistoryBtn");
const portInput = document.getElementById("portInput");
const filterMode = document.getElementById("filterMode");
const fileFilterInput = document.getElementById("fileFilterInput");
const domainFilterToggle = document.getElementById("domainFilterToggle");
const domainWhitelist = document.getElementById("domainWhitelist");
const domainBlacklist = document.getElementById("domainBlacklist");
const saveSettingsBtn = document.getElementById("saveSettingsBtn");
const settingsSaved = document.getElementById("settingsSaved");

let currentMedia = [];
let currentTab = "media";

// --- Tabs ---
document.querySelectorAll(".tab").forEach(tab => {
  tab.addEventListener("click", () => {
    document.querySelectorAll(".tab").forEach(t => t.classList.remove("active"));
    document.querySelectorAll(".tab-content").forEach(c => c.classList.remove("active"));
    tab.classList.add("active");
    currentTab = tab.dataset.tab;
    document.getElementById(`tab-${currentTab}`).classList.add("active");
    if (currentTab === "history") loadHistory();
    if (currentTab === "settings") loadSettings();
  });
});

// --- Status ---
function updateUI(enabled) {
  statusEl.textContent = enabled ? "Interception: ON" : "Interception: OFF";
  statusEl.className = enabled ? "status enabled" : "status disabled";
  toggleBtn.textContent = enabled ? "Disable" : "Enable";
  toggleBtn.className = enabled ? "toggle-btn disable" : "toggle-btn enable";
}

function updateConn(connected) {
  connDot.className = connected ? "conn-dot connected" : "conn-dot disconnected";
  connDot.title = connected ? "Copper is running" : "Copper not found";
}

browser.runtime.sendMessage({ action: "getStatus" }).then(r => { if (r) updateUI(r.enabled); });
browser.runtime.sendMessage({ action: "ping" }).then(r => { if (r) updateConn(r.connected); });

// --- Toggle ---
toggleBtn.addEventListener("click", () => {
  browser.runtime.sendMessage({ action: "getStatus" }).then(r => {
    const next = !r.enabled;
    browser.runtime.sendMessage({ action: "toggle", enabled: next }).then(() => updateUI(next));
  });
});

// --- Open Copper ---
openBtn.addEventListener("click", () => browser.runtime.sendMessage({ action: "openCopper" }));

// --- Quick URL send ---
sendBtn.addEventListener("click", () => {
  const url = urlInput.value.trim();
  if (!url) return;
  browser.runtime.sendMessage({ action: "sendUrl", url }).then(r => {
    if (r && r.success) {
      urlInput.value = "";
      urlInput.placeholder = "Sent!";
      setTimeout(() => { urlInput.placeholder = "Paste URL here..."; }, 1500);
    } else {
      urlInput.style.borderColor = "#dc3545";
      setTimeout(() => { urlInput.style.borderColor = "#444"; }, 1500);
    }
  });
});
urlInput.addEventListener("keydown", (e) => { if (e.key === "Enter") sendBtn.click(); });

// --- Media detection ---
const MEDIA_ICONS = { image: "\u{1F5BC}", video: "\u{1F3AC}", audio: "\u{1F3B5}", document: "\u{1F4C4}", archive: "\u{1F4E6}", executable: "\u{2699}", other: "\u{1F4CE}" };

function renderMedia(media) {
  const filtered = currentTab === "media" && mediaFilter.value !== "all"
    ? media.filter(m => m.type === mediaFilter.value) : media;

  if (filtered.length === 0) {
    mediaList.innerHTML = '<div class="empty-state">No media found</div>';
    return;
  }
  mediaList.innerHTML = filtered.map((m, i) => `
    <div class="media-item" data-idx="${i}">
      <span class="media-icon">${MEDIA_ICONS[m.type] || MEDIA_ICONS.other}</span>
      <div class="media-info">
        <div class="media-name" title="${m.filename}">${m.filename}</div>
        <div class="media-url" title="${m.url}">${m.url.substring(0, 60)}...</div>
      </div>
      <button class="media-dl-btn" data-url="${m.url}" data-filename="${m.filename}">Get</button>
    </div>
  `).join("");

  mediaList.querySelectorAll(".media-dl-btn").forEach(btn => {
    btn.addEventListener("click", (e) => {
      e.stopPropagation();
      browser.runtime.sendMessage({ action: "sendUrl", url: btn.dataset.url, filename: btn.dataset.filename });
      btn.textContent = "Sent!";
      btn.style.borderColor = "#4cff4c";
      btn.style.color = "#4cff4c";
    });
  });
}

detectBtn.addEventListener("click", () => {
  detectBtn.textContent = "Scanning...";
  browser.runtime.sendMessage({ action: "detectMedia" }).then(r => {
    detectBtn.textContent = "Scan Page";
    if (r && r.media) {
      currentMedia = r.media;
      renderMedia(currentMedia);
    }
  });
});

mediaFilter.addEventListener("change", () => renderMedia(currentMedia));

downloadAllBtn.addEventListener("click", () => {
  const filtered = mediaFilter.value !== "all"
    ? currentMedia.filter(m => m.type === mediaFilter.value) : currentMedia;
  filtered.forEach(m => {
    browser.runtime.sendMessage({ action: "sendUrl", url: m.url, filename: m.filename });
  });
  downloadAllBtn.textContent = `Sent ${filtered.length}!`;
  setTimeout(() => { downloadAllBtn.textContent = "Download All"; }, 1500);
});

// --- History ---
function renderHistory(items) {
  if (items.length === 0) {
    historyList.innerHTML = '<div class="empty-state">No downloads intercepted yet</div>';
    historyCount.textContent = "0 intercepted";
    return;
  }
  historyCount.textContent = `${items.length} intercepted`;
  historyList.innerHTML = items.slice(0, 30).map(h => {
    const d = new Date(h.time);
    const timeStr = d.toLocaleTimeString();
    return `
      <div class="media-item">
        <span class="media-icon">\u{2B07}</span>
        <div class="media-info">
          <div class="media-name" title="${h.filename}">${h.filename}</div>
          <div class="media-url">${timeStr} &mdash; ${h.url.substring(0, 50)}...</div>
        </div>
      </div>
    `;
  }).join("");
}

function loadHistory() {
  browser.runtime.sendMessage({ action: "getHistory" }).then(r => {
    if (r) renderHistory(r.history || []);
  });
}

clearHistoryBtn.addEventListener("click", () => {
  browser.runtime.sendMessage({ action: "clearHistory" }).then(() => {
    renderHistory([]);
  });
});

// --- Settings ---
function loadSettings() {
  browser.runtime.sendMessage({ action: "getSettings" }).then(s => {
    if (!s) return;
    portInput.value = s.port || 24680;
    filterMode.value = s.fileFilterMode || "none";
    fileFilterInput.value = (s.fileFilter || []).join(",");
    domainFilterToggle.checked = !!s.domainFilterEnabled;
    domainWhitelist.value = (s.domainWhitelist || []).join("\n");
    domainBlacklist.value = (s.domainBlacklist || []).join("\n");
  });
}

saveSettingsBtn.addEventListener("click", () => {
  const fileFilter = fileFilterInput.value.split(",").map(f => f.trim()).filter(f => f);
  const domainWhitelistArr = domainWhitelist.value.split("\n").map(d => d.trim()).filter(d => d);
  const domainBlacklistArr = domainBlacklist.value.split("\n").map(d => d.trim()).filter(d => d);
  const settings = {
    port: parseInt(portInput.value) || 24680,
    fileFilterMode: filterMode.value,
    fileFilter,
    domainFilterEnabled: domainFilterToggle.checked,
    domainWhitelist: domainWhitelistArr,
    domainBlacklist: domainBlacklistArr
  };
  browser.runtime.sendMessage({ action: "updateSettings", settings }).then(() => {
    settingsSaved.style.display = "block";
    setTimeout(() => { settingsSaved.style.display = "none"; }, 1500);
  });
});

loadHistory();
