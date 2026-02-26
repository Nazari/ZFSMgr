const API_KEY = "zfsmgr.api.base";

const state = {
  connections: [],
  selectedConnectionId: null
};

const ui = {
  apiUrl: document.getElementById("apiUrl"),
  saveApiBtn: document.getElementById("saveApiBtn"),
  reloadBtn: document.getElementById("reloadBtn"),
  connectionsMeta: document.getElementById("connectionsMeta"),
  connectionsBody: document.getElementById("connectionsBody"),
  selectedConn: document.getElementById("selectedConn"),

  poolName: document.getElementById("poolName"),
  importPoolBtn: document.getElementById("importPoolBtn"),
  exportPoolBtn: document.getElementById("exportPoolBtn"),

  createDataset: document.getElementById("createDataset"),
  createKind: document.getElementById("createKind"),
  createVolsize: document.getElementById("createVolsize"),
  createRecursive: document.getElementById("createRecursive"),
  createDatasetBtn: document.getElementById("createDatasetBtn"),

  deleteDataset: document.getElementById("deleteDataset"),
  deleteRecursive: document.getElementById("deleteRecursive"),
  deleteDatasetBtn: document.getElementById("deleteDatasetBtn"),

  renameSource: document.getElementById("renameSource"),
  renameTarget: document.getElementById("renameTarget"),
  renameDatasetBtn: document.getElementById("renameDatasetBtn"),

  propDataset: document.getElementById("propDataset"),
  propName: document.getElementById("propName"),
  propValue: document.getElementById("propValue"),
  setPropBtn: document.getElementById("setPropBtn"),
  inheritPropBtn: document.getElementById("inheritPropBtn"),

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

function selectedConnection() {
  return state.connections.find((c) => Number(c.id) === Number(state.selectedConnectionId)) || null;
}

function requireSelectedConnection() {
  const conn = selectedConnection();
  if (!conn) {
    throw new Error("Select a connection first");
  }
  return conn;
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
  const selected = Number(c.id) === Number(state.selectedConnectionId) ? "selected-row" : "";

  return `
    <tr class="${selected}">
      <td><strong>${c.name}</strong></td>
      <td>${c.conn_type}/${c.transport}</td>
      <td>${host}</td>
      <td>${c.port}</td>
      <td>
        <div class="row-btns">
          <button data-select="${c.id}">Use</button>
          <button data-refresh="${c.id}">Refresh</button>
        </div>
        <span class="${cls}">${c.is_active ? "active" : "inactive"}</span>
      </td>
    </tr>
  `;
}

function updateSelectedConnLabel() {
  const conn = selectedConnection();
  ui.selectedConn.textContent = conn
    ? `${conn.name} (${conn.transport}) ${conn.username || ""}@${conn.host || ""}:${conn.port || ""}`
    : "(none)";
}

async function loadConnections() {
  ui.connectionsMeta.textContent = "Loading connections...";
  try {
    const payload = await fetchJson("/connections");
    const rows = payload.connections || [];
    state.connections = rows;

    if (!selectedConnection() && rows.length > 0) {
      state.selectedConnectionId = rows[0].id;
    }

    ui.connectionsBody.innerHTML = rows.map(rowHtml).join("");
    ui.connectionsMeta.textContent = `${rows.length} connection(s)`;
    updateSelectedConnLabel();
  } catch (err) {
    ui.connectionsBody.innerHTML = "";
    ui.connectionsMeta.textContent = `Error: ${err.message}`;
    updateSelectedConnLabel();
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
  await fetchJson(`/connections/${id}/refresh`, { method: "POST" });
}

async function runAction(path, body) {
  const conn = requireSelectedConnection();

  await fetchJson(`/connections/${conn.id}/actions/${path}`, {
    method: "POST",
    body: JSON.stringify(body || {})
  });

  await Promise.all([loadConnections(), loadLogs()]);
}

async function doImportPool() {
  await runAction("import_pool", { pool: ui.poolName.value.trim() });
}

async function doExportPool() {
  await runAction("export_pool", { pool: ui.poolName.value.trim() });
}

async function doCreateDataset() {
  const kind = ui.createKind.value;
  const body = {
    dataset: ui.createDataset.value.trim(),
    kind,
    recursive: ui.createRecursive.value === "true"
  };

  if (kind === "volume") {
    body.volsize = ui.createVolsize.value.trim();
  }

  await runAction("create_dataset", body);
}

async function doDeleteDataset() {
  await runAction("delete_dataset", {
    dataset: ui.deleteDataset.value.trim(),
    recursive: ui.deleteRecursive.value === "true"
  });
}

async function doRenameDataset() {
  await runAction("rename_dataset", {
    source: ui.renameSource.value.trim(),
    target: ui.renameTarget.value.trim()
  });
}

async function doSetProperty() {
  await runAction("set_property", {
    dataset: ui.propDataset.value.trim(),
    property: ui.propName.value.trim(),
    value: ui.propValue.value.trim()
  });
}

async function doInheritProperty() {
  await runAction("inherit_property", {
    dataset: ui.propDataset.value.trim(),
    property: ui.propName.value.trim()
  });
}

async function guardAction(fn) {
  try {
    await fn();
  } catch (err) {
    ui.logsBox.textContent += `\nAction failed: ${err.message}`;
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
    const selectBtn = ev.target.closest("button[data-select]");
    if (selectBtn) {
      state.selectedConnectionId = Number(selectBtn.dataset.select);
      ui.connectionsBody.innerHTML = state.connections.map(rowHtml).join("");
      updateSelectedConnLabel();
      return;
    }

    const refreshBtn = ev.target.closest("button[data-refresh]");
    if (refreshBtn) {
      guardAction(async () => {
        await refreshConnection(refreshBtn.dataset.refresh);
        await Promise.all([loadConnections(), loadLogs()]);
      });
    }
  });

  ui.importPoolBtn.addEventListener("click", () => guardAction(doImportPool));
  ui.exportPoolBtn.addEventListener("click", () => guardAction(doExportPool));
  ui.createDatasetBtn.addEventListener("click", () => guardAction(doCreateDataset));
  ui.deleteDatasetBtn.addEventListener("click", () => guardAction(doDeleteDataset));
  ui.renameDatasetBtn.addEventListener("click", () => guardAction(doRenameDataset));
  ui.setPropBtn.addEventListener("click", () => guardAction(doSetProperty));
  ui.inheritPropBtn.addEventListener("click", () => guardAction(doInheritProperty));
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
