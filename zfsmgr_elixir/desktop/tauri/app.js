const API_KEY = "zfsmgr.api.base";

const ui = {
  apiUrl: document.getElementById("apiUrl"),
  saveApiBtn: document.getElementById("saveApiBtn"),
  reloadBtn: document.getElementById("reloadBtn"),
  connectionsMeta: document.getElementById("connectionsMeta"),
  connectionsBody: document.getElementById("connectionsBody"),
  logLimit: document.getElementById("logLimit"),
  reloadLogsBtn: document.getElementById("reloadLogsBtn"),
  logsBox: document.getElementById("logsBox")
};

function getApiBase() {
  const persisted = localStorage.getItem(API_KEY);
  if (persisted && persisted.trim()) return persisted.trim();
  return ui.apiUrl.value.trim();
}

function setApiBase(url) {
  localStorage.setItem(API_KEY, url);
  ui.apiUrl.value = url;
}

async function fetchJson(path, init = {}) {
  const base = getApiBase();
  const url = `${base}${path}`;
  const res = await fetch(url, {
    headers: { "content-type": "application/json" },
    ...init
  });
  if (!res.ok) {
    const txt = await res.text();
    throw new Error(`${res.status} ${res.statusText}: ${txt}`);
  }
  if (res.status === 204) return {};
  return res.json();
}

function rowHtml(c) {
  const host = c.host || "-";
  const cls = c.is_active ? "ok" : "err";
  return `
    <tr>
      <td><strong>${c.name}</strong></td>
      <td>${c.conn_type}/${c.transport}</td>
      <td>${host}</td>
      <td>${c.port}</td>
      <td>
        <button data-refresh="${c.id}">Refresh</button>
        <span class="${cls}">${c.is_active ? "active" : "inactive"}</span>
      </td>
    </tr>
  `;
}

async function loadConnections() {
  ui.connectionsMeta.textContent = "Loading connections...";
  try {
    const payload = await fetchJson("/connections");
    const rows = payload.connections || [];
    ui.connectionsBody.innerHTML = rows.map(rowHtml).join("");
    ui.connectionsMeta.textContent = `${rows.length} connection(s)`;
  } catch (err) {
    ui.connectionsBody.innerHTML = "";
    ui.connectionsMeta.textContent = `Error: ${err.message}`;
  }
}

async function loadLogs() {
  ui.logsBox.textContent = "Loading logs...";
  try {
    const limit = Number(ui.logLimit.value || 500);
    const payload = await fetchJson(`/logs?limit=${limit}`);
    const lines = (payload.logs || []).map((l) => {
      const ts = l.inserted_at || "-";
      const lvl = (l.level || "info").toUpperCase();
      const src = l.source || "application";
      const msg = l.message || "";
      return `[${ts}] [${src}] [${lvl}] ${msg}`;
    });
    ui.logsBox.textContent = lines.join("\n") || "(no logs)";
    ui.logsBox.scrollTop = ui.logsBox.scrollHeight;
  } catch (err) {
    ui.logsBox.textContent = `Error loading logs: ${err.message}`;
  }
}

async function refreshConnection(id) {
  try {
    await fetchJson(`/connections/${id}/refresh`, { method: "POST" });
    await Promise.all([loadConnections(), loadLogs()]);
  } catch (err) {
    ui.logsBox.textContent += `\nRefresh failed (${id}): ${err.message}`;
  }
}

function bindEvents() {
  ui.saveApiBtn.addEventListener("click", () => {
    const next = ui.apiUrl.value.trim();
    if (!next) return;
    setApiBase(next);
    loadConnections();
    loadLogs();
  });

  ui.reloadBtn.addEventListener("click", () => {
    loadConnections();
    loadLogs();
  });

  ui.reloadLogsBtn.addEventListener("click", loadLogs);

  ui.connectionsBody.addEventListener("click", (ev) => {
    const btn = ev.target.closest("button[data-refresh]");
    if (!btn) return;
    refreshConnection(btn.dataset.refresh);
  });
}

async function boot() {
  const saved = localStorage.getItem(API_KEY);
  if (saved) ui.apiUrl.value = saved;

  bindEvents();

  try {
    await fetchJson("/health");
  } catch (err) {
    ui.connectionsMeta.textContent = `Health check failed: ${err.message}`;
  }

  await Promise.all([loadConnections(), loadLogs()]);
}

boot();
