const API_KEY = "zfsmgr.api.base";

const state = {
  connections: [],
  selectedConnectionId: null,
  selectedLeftTab: "connections",
  selectedPoolTab: "imported",
  selectedPoolDetailTab: "props",
  pools: [],
  datasetsByPool: {},
  importedPoolSelected: "",
  importablePoolSelected: "",
  originPool: "",
  destPool: "",
  originDataset: "",
  destDataset: "",
  originSnapshot: "",
  destSnapshot: "",
  logs: [],
  logLevel: "normal",
  busy: false,
  refreshing: false
};

const ui = {
  apiUrl: document.getElementById("apiUrl"),
  saveApiBtn: document.getElementById("saveApiBtn"),
  reloadBtn: document.getElementById("reloadBtn"),

  leftTabConnections: document.getElementById("leftTabConnections"),
  leftTabDatasets: document.getElementById("leftTabDatasets"),
  leftConnectionsTab: document.getElementById("leftConnectionsTab"),
  leftDatasetsTab: document.getElementById("leftDatasetsTab"),

  connectionsMeta: document.getElementById("connectionsMeta"),
  connectionsBody: document.getElementById("connectionsBody"),
  newConnBtn: document.getElementById("newConnBtn"),
  refreshAllBtn: document.getElementById("refreshAllBtn"),

  copyBtn: document.getElementById("copyBtn"),
  levelBtn: document.getElementById("levelBtn"),
  syncBtn: document.getElementById("syncBtn"),
  breakdownBtn: document.getElementById("breakdownBtn"),
  assembleBtn: document.getElementById("assembleBtn"),
  originSelectedLabel: document.getElementById("originSelectedLabel"),
  destSelectedLabel: document.getElementById("destSelectedLabel"),
  actionSelectedLabel: document.getElementById("actionSelectedLabel"),

  rightConnectionsDetail: document.getElementById("rightConnectionsDetail"),
  rightDatasetsDetail: document.getElementById("rightDatasetsDetail"),

  poolTabImported: document.getElementById("poolTabImported"),
  poolTabImportable: document.getElementById("poolTabImportable"),
  importedPoolsPane: document.getElementById("importedPoolsPane"),
  importablePoolsPane: document.getElementById("importablePoolsPane"),
  importedPoolsBody: document.getElementById("importedPoolsBody"),
  importablePoolsBody: document.getElementById("importablePoolsBody"),

  poolDetailPropsTab: document.getElementById("poolDetailPropsTab"),
  poolDetailStatusTab: document.getElementById("poolDetailStatusTab"),
  poolPropsPane: document.getElementById("poolPropsPane"),
  poolStatusPane: document.getElementById("poolStatusPane"),
  poolPropsBody: document.getElementById("poolPropsBody"),
  poolStatusBox: document.getElementById("poolStatusBox"),

  originPoolSelect: document.getElementById("originPoolSelect"),
  destPoolSelect: document.getElementById("destPoolSelect"),
  originTargetText: document.getElementById("originTargetText"),
  destTargetText: document.getElementById("destTargetText"),
  originDatasetsBody: document.getElementById("originDatasetsBody"),
  destDatasetsBody: document.getElementById("destDatasetsBody"),

  datasetPropsSelected: document.getElementById("datasetPropsSelected"),
  datasetPropsBody: document.getElementById("datasetPropsBody"),
  datasetPropsApplyBtn: document.getElementById("datasetPropsApplyBtn"),

  statusText: document.getElementById("statusText"),
  lastSshLine: document.getElementById("lastSshLine"),
  logLevelSelect: document.getElementById("logLevelSelect"),
  logLimit: document.getElementById("logLimit"),
  reloadLogsBtn: document.getElementById("reloadLogsBtn"),
  clearLogsBtn: document.getElementById("clearLogsBtn"),
  copyLogsBtn: document.getElementById("copyLogsBtn"),
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

async function fetchJson(path, init = {}) {
  const res = await fetch(`${getApiBase()}${path}`, {
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

function escapeHtml(v) {
  return String(v ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

function setStatus(msg) {
  ui.statusText.textContent = msg;
}

function setLastDetail(msg) {
  ui.lastSshLine.textContent = msg || "";
}

function leftTab(name) {
  state.selectedLeftTab = name;
  const isConn = name === "connections";
  ui.leftTabConnections.classList.toggle("active", isConn);
  ui.leftTabDatasets.classList.toggle("active", !isConn);
  ui.leftConnectionsTab.classList.toggle("active", isConn);
  ui.leftDatasetsTab.classList.toggle("active", !isConn);
  ui.rightConnectionsDetail.classList.toggle("active", isConn);
  ui.rightDatasetsDetail.classList.toggle("active", !isConn);
}

function setBusy(isBusy, message = "") {
  state.busy = isBusy;
  const disable = isBusy || state.refreshing;
  const ids = [
    "reloadBtn",
    "refreshAllBtn",
    "copyBtn",
    "levelBtn",
    "syncBtn",
    "breakdownBtn",
    "assembleBtn",
    "reloadLogsBtn",
    "clearLogsBtn",
    "copyLogsBtn"
  ];
  for (const id of ids) {
    if (ui[id]) ui[id].disabled = disable;
  }

  if (disable) {
    ui.connectionsBody.querySelectorAll("button").forEach((b) => (b.disabled = true));
    ui.importedPoolsBody.querySelectorAll("button").forEach((b) => (b.disabled = true));
    ui.importablePoolsBody.querySelectorAll("button").forEach((b) => (b.disabled = true));
  } else {
    renderConnections();
    renderPoolsTables();
    updateActionButtons();
  }

  if (message) setStatus(message);
}

function setRefreshing(isRefreshing, message = "") {
  state.refreshing = isRefreshing;
  setBusy(state.busy, message);
}

function poolTab(name) {
  state.selectedPoolTab = name;
  const isImported = name === "imported";
  ui.poolTabImported.classList.toggle("active", isImported);
  ui.poolTabImportable.classList.toggle("active", !isImported);
  ui.importedPoolsPane.classList.toggle("active", isImported);
  ui.importablePoolsPane.classList.toggle("active", !isImported);
}

function poolDetailTab(name) {
  state.selectedPoolDetailTab = name;
  const isProps = name === "props";
  ui.poolDetailPropsTab.classList.toggle("active", isProps);
  ui.poolDetailStatusTab.classList.toggle("active", !isProps);
  ui.poolPropsPane.classList.toggle("active", isProps);
  ui.poolStatusPane.classList.toggle("active", !isProps);
}

function logVisibleByLevel(level) {
  const order = { normal: 0, info: 1, debug: 2 };
  const current = order[state.logLevel] ?? 0;
  return (order[(level || "normal").toLowerCase()] ?? 1) <= current;
}

function renderLogs() {
  const lines = state.logs
    .filter((l) => logVisibleByLevel(l.level || "info"))
    .map((l) => `[${l.inserted_at || "-"}] [${(l.source || "application")}] [${(l.level || "info").toUpperCase()}] ${l.message || ""}`);
  ui.logsBox.textContent = lines.join("\n") || "(sin logs)";
  if (lines.length) setLastDetail(lines[lines.length - 1]);
}

function appendLocalLog(level, source, message) {
  const row = {
    inserted_at: new Date().toISOString().replace("T", " ").slice(0, 19),
    level,
    source,
    message
  };
  state.logs.push(row);
  const maxLines = Number(ui.logLimit.value || 500);
  if (state.logs.length > maxLines * 2) {
    state.logs = state.logs.slice(-maxLines * 2);
  }
  renderLogs();
}

function renderConnections() {
  const conn = selectedConnection();
  ui.connectionsMeta.textContent = `${state.connections.length} conexiones`;
  ui.connectionsBody.innerHTML = state.connections
    .map((c) => {
      const selected = Number(c.id) === Number(state.selectedConnectionId) ? "selected" : "";
      return `<tr class="${selected}">
        <td><strong>${escapeHtml(c.name)}</strong></td>
        <td>${escapeHtml(c.conn_type)}/${escapeHtml(c.transport)}</td>
        <td>${escapeHtml(c.host || "-")}</td>
        <td>${escapeHtml(c.port || "")}</td>
        <td>
          <button data-use="${c.id}" ${state.busy || state.refreshing ? "disabled" : ""}>Usar</button>
          <button data-refresh="${c.id}" ${state.busy || state.refreshing ? "disabled" : ""}>Refrescar</button>
        </td>
      </tr>`;
    })
    .join("");
  setStatus(conn ? `Estado: ${conn.name} seleccionado` : "Estado: sin conexion seleccionada");
}

function poolRowsHtml(action) {
  const conn = selectedConnection();
  if (!conn) return `<tr><td colspan="3">Seleccione una conexion</td></tr>`;
  if (state.pools.length === 0) return `<tr><td colspan="3">(Sin pools)</td></tr>`;
  return state.pools
    .map((pool) => `<tr class="${state.importedPoolSelected === pool ? "selected" : ""}">
      <td>${escapeHtml(conn.name)}</td>
      <td>${escapeHtml(pool)}</td>
      <td><button data-${action}="${escapeHtml(pool)}" ${state.busy || state.refreshing ? "disabled" : ""}>${action === "export" ? "Exportar" : "Importar"}</button></td>
    </tr>`)
    .join("");
}

function renderPoolsTables() {
  ui.importedPoolsBody.innerHTML = poolRowsHtml("export");
  ui.importablePoolsBody.innerHTML = poolRowsHtml("import");
}

function fillSelect(selectEl, values, selected) {
  const opts = values.map((v) => `<option value="${escapeHtml(v)}">${escapeHtml(v)}</option>`).join("");
  selectEl.innerHTML = opts || `<option value="">(Sin pools)</option>`;
  if (selected && values.includes(selected)) {
    selectEl.value = selected;
  } else if (values.length > 0) {
    selectEl.value = values[0];
  } else {
    selectEl.value = "";
  }
}

function datasetRowsHtml(datasets, side) {
  if (!datasets || datasets.length === 0) return `<tr><td colspan="2">(Sin datasets)</td></tr>`;
  const selectedName = side === "origin" ? state.originDataset : state.destDataset;
  return datasets
    .map((d) => {
      const selected = d === selectedName ? "selected" : "";
      return `<tr class="${selected}"><td data-${side}-dataset="${escapeHtml(d)}">${escapeHtml(d)}</td><td>(seleccione)</td></tr>`;
    })
    .join("");
}

function renderDatasets() {
  const originPool = state.originPool;
  const destPool = state.destPool;
  const originDatasets = state.datasetsByPool[originPool] || [];
  const destDatasets = state.datasetsByPool[destPool] || [];

  ui.originDatasetsBody.innerHTML = datasetRowsHtml(originDatasets, "origin");
  ui.destDatasetsBody.innerHTML = datasetRowsHtml(destDatasets, "dest");

  ui.originTargetText.textContent = `Origen: ${state.originDataset || "(ninguno)"}`;
  ui.destTargetText.textContent = `Destino: ${state.destDataset || "(ninguno)"}`;
  ui.originSelectedLabel.textContent = `Origen: ${state.originDataset || "(ninguno)"}`;
  ui.destSelectedLabel.textContent = `Destino: ${state.destDataset || "(ninguno)"}`;
  ui.actionSelectedLabel.textContent = `Seleccionado: ${state.originDataset || state.destDataset || "(ninguno)"}`;
  updateActionButtons();
}

function renderDatasetProps() {
  const selected = state.originDataset || state.destDataset;
  ui.datasetPropsSelected.textContent = `Seleccionado: ${selected || "(ninguno)"}`;
  if (!selected) {
    ui.datasetPropsBody.innerHTML = `<tr><td colspan="2">(Sin dataset seleccionado)</td></tr>`;
    return;
  }
  const conn = selectedConnection();
  const rows = [
    ["dataset", selected],
    ["conexion", conn?.name || "-"],
    ["pool", getPoolFromDataset(selected) || "-"],
    ["mounted", "(n/a)"],
    ["mountpoint", "(n/a)"]
  ];
  ui.datasetPropsBody.innerHTML = rows.map((r) => `<tr><td>${escapeHtml(r[0])}</td><td>${escapeHtml(r[1])}</td></tr>`).join("");
}

function isSnapshotName(name) {
  return String(name || "").includes("@");
}

function updateActionButtons() {
  const originOk = !!state.originDataset;
  const destOk = !!state.destDataset;
  const originIsSnapshot = isSnapshotName(state.originDataset);
  const originIsDataset = originOk && !originIsSnapshot;
  const destIsDataset = destOk && !isSnapshotName(state.destDataset);

  ui.copyBtn.disabled = state.busy || state.refreshing || !(originIsSnapshot && destIsDataset);
  ui.levelBtn.disabled = state.busy || state.refreshing || !(originOk && destOk);
  ui.syncBtn.disabled = state.busy || state.refreshing || !(originIsDataset && destIsDataset);
  ui.breakdownBtn.disabled = state.busy || state.refreshing || !(originIsDataset || destIsDataset);
  ui.assembleBtn.disabled = state.busy || state.refreshing || !(originIsDataset || destIsDataset);
}

async function withAction(label, fn) {
  if (state.busy || state.refreshing) return;
  setBusy(true, `${label}: en ejecucion...`);
  appendLocalLog("normal", "application", `${label} iniciado`);
  try {
    await fn();
    appendLocalLog("normal", "application", `${label} finalizado`);
    setStatus(`${label}: OK`);
  } catch (err) {
    appendLocalLog("normal", "application", `${label} error: ${err.message}`);
    setStatus(`${label}: ERROR`);
    throw err;
  } finally {
    setBusy(false);
  }
}

function renderPoolDetail() {
  const pool = state.importedPoolSelected;
  if (!pool) {
    ui.poolPropsBody.innerHTML = `<tr><td colspan="3">Seleccione un pool</td></tr>`;
    ui.poolStatusBox.textContent = "";
    return;
  }
  const conn = selectedConnection();
  ui.poolPropsBody.innerHTML = [
    ["name", pool, "local"],
    ["connection", conn?.name || "-", "local"]
  ]
    .map((r) => `<tr><td>${escapeHtml(r[0])}</td><td>${escapeHtml(r[1])}</td><td>${escapeHtml(r[2])}</td></tr>`)
    .join("");
  ui.poolStatusBox.textContent = `zpool status -v ${pool}\n(no implementado en detalle en esta fase)`;
}

function getPoolFromDataset(dataset) {
  const raw = String(dataset || "");
  const idx = raw.indexOf("/");
  return idx > 0 ? raw.slice(0, idx) : raw;
}

async function loadConnections() {
  const payload = await fetchJson("/connections");
  state.connections = payload.connections || [];
  if (!selectedConnection() && state.connections.length > 0) {
    state.selectedConnectionId = state.connections[0].id;
  }
  renderConnections();
}

async function loadPoolsAndDatasets() {
  const conn = selectedConnection();
  state.pools = [];
  state.datasetsByPool = {};
  state.importedPoolSelected = "";

  if (!conn) {
    renderPoolsTables();
    fillSelect(ui.originPoolSelect, [], "");
    fillSelect(ui.destPoolSelect, [], "");
    renderDatasets();
    renderDatasetProps();
    renderPoolDetail();
    return;
  }

  try {
    const poolsPayload = await fetchJson(`/connections/${conn.id}/pools`);
    state.pools = poolsPayload.pools || [];
    state.importedPoolSelected = state.pools[0] || "";
    state.importablePoolSelected = state.pools[0] || "";

    for (const p of state.pools) {
      try {
        const dsPayload = await fetchJson(`/connections/${conn.id}/datasets?pool=${encodeURIComponent(p)}`);
        state.datasetsByPool[p] = dsPayload.datasets || [];
      } catch {
        state.datasetsByPool[p] = [];
      }
    }

    fillSelect(ui.originPoolSelect, state.pools, state.originPool);
    fillSelect(ui.destPoolSelect, state.pools, state.destPool);
    state.originPool = ui.originPoolSelect.value;
    state.destPool = ui.destPoolSelect.value;

    if (state.originDataset && getPoolFromDataset(state.originDataset) !== state.originPool) {
      state.originDataset = "";
      state.originSnapshot = "";
    }
    if (state.destDataset && getPoolFromDataset(state.destDataset) !== state.destPool) {
      state.destDataset = "";
      state.destSnapshot = "";
    }
  } finally {
    renderPoolsTables();
    renderPoolDetail();
    renderDatasets();
    renderDatasetProps();
  }
}

async function loadLogs() {
  const limit = Number(ui.logLimit.value || 500);
  const payload = await fetchJson(`/logs?limit=${limit}`);
  state.logs = payload.logs || [];
  renderLogs();
}

async function refreshAll() {
  if (state.refreshing || state.busy) return;
  setRefreshing(true, "Refresco en curso...");
  appendLocalLog("normal", "application", "Refresco completo iniciado");
  try {
    await loadConnections();
    await loadPoolsAndDatasets();
    await loadLogs();
    appendLocalLog("normal", "application", "Refresco completo finalizado");
    setStatus("Refresco completo finalizado");
  } finally {
    setRefreshing(false);
  }
}

async function refreshConnection(id) {
  if (state.refreshing || state.busy) return;
  const conn = state.connections.find((c) => Number(c.id) === Number(id));
  const connName = conn?.name || `id=${id}`;
  await withAction(`Refrescar ${connName}`, async () => {
    await fetchJson(`/connections/${id}/refresh`, { method: "POST" });
    await loadConnections();
    await loadPoolsAndDatasets();
    await loadLogs();
  });
}

async function importPool(pool) {
  const conn = selectedConnection();
  if (!conn) return;
  await withAction(`Importar pool ${pool}`, async () => {
    await fetchJson(`/connections/${conn.id}/actions/import_pool`, {
      method: "POST",
      body: JSON.stringify({ pool })
    });
    await loadConnections();
    await loadPoolsAndDatasets();
    await loadLogs();
  });
}

async function exportPool(pool) {
  const conn = selectedConnection();
  if (!conn) return;
  await withAction(`Exportar pool ${pool}`, async () => {
    await fetchJson(`/connections/${conn.id}/actions/export_pool`, {
      method: "POST",
      body: JSON.stringify({ pool })
    });
    await loadConnections();
    await loadPoolsAndDatasets();
    await loadLogs();
  });
}

function copyLogs() {
  navigator.clipboard?.writeText(ui.logsBox.textContent || "").catch(() => {});
}

function clearLogsView() {
  ui.logsBox.textContent = "";
}

function bindEvents() {
  ui.leftTabConnections.addEventListener("click", () => leftTab("connections"));
  ui.leftTabDatasets.addEventListener("click", () => leftTab("datasets"));

  ui.poolTabImported.addEventListener("click", () => poolTab("imported"));
  ui.poolTabImportable.addEventListener("click", () => poolTab("importable"));
  ui.poolDetailPropsTab.addEventListener("click", () => poolDetailTab("props"));
  ui.poolDetailStatusTab.addEventListener("click", () => poolDetailTab("status"));

  ui.saveApiBtn.addEventListener("click", async () => {
    const next = ui.apiUrl.value.trim();
    if (!next) return;
    setApiBase(next);
    await refreshAll();
  });

  ui.reloadBtn.addEventListener("click", async () => {
    await refreshAll();
  });

  ui.refreshAllBtn.addEventListener("click", async () => {
    await refreshAll();
  });

  ui.connectionsBody.addEventListener("click", async (ev) => {
    if (state.busy || state.refreshing) return;
    const useBtn = ev.target.closest("button[data-use]");
    if (useBtn) {
      state.selectedConnectionId = Number(useBtn.dataset.use);
      renderConnections();
      await loadPoolsAndDatasets();
      setStatus("Conexion seleccionada");
      return;
    }

    const rbtn = ev.target.closest("button[data-refresh]");
    if (rbtn) {
      await refreshConnection(rbtn.dataset.refresh);
    }
  });

  ui.importedPoolsBody.addEventListener("click", async (ev) => {
    if (state.busy || state.refreshing) return;
    const exp = ev.target.closest("button[data-export]");
    if (exp) {
      await exportPool(exp.dataset.export);
      return;
    }
    const row = ev.target.closest("tr");
    if (row) {
      const poolCell = row.children?.[1];
      if (poolCell) {
        state.importedPoolSelected = poolCell.textContent?.trim() || "";
        renderPoolsTables();
        renderPoolDetail();
        setStatus(`Pool seleccionado: ${state.importedPoolSelected}`);
      }
    }
  });

  ui.importablePoolsBody.addEventListener("click", async (ev) => {
    if (state.busy || state.refreshing) return;
    const imp = ev.target.closest("button[data-import]");
    if (imp) {
      await importPool(imp.dataset.import);
    }
  });

  ui.originPoolSelect.addEventListener("change", () => {
    if (state.busy || state.refreshing) return;
    state.originPool = ui.originPoolSelect.value;
    state.originDataset = "";
    state.originSnapshot = "";
    renderDatasets();
    renderDatasetProps();
  });

  ui.destPoolSelect.addEventListener("change", () => {
    if (state.busy || state.refreshing) return;
    state.destPool = ui.destPoolSelect.value;
    state.destDataset = "";
    state.destSnapshot = "";
    renderDatasets();
    renderDatasetProps();
  });

  ui.originDatasetsBody.addEventListener("click", (ev) => {
    if (state.busy || state.refreshing) return;
    const cell = ev.target.closest("td[data-origin-dataset]");
    if (!cell) return;
    state.originDataset = cell.dataset.originDataset || "";
    state.originSnapshot = isSnapshotName(state.originDataset) ? state.originDataset.split("@", 2)[1] : "";
    renderDatasets();
    renderDatasetProps();
  });

  ui.destDatasetsBody.addEventListener("click", (ev) => {
    if (state.busy || state.refreshing) return;
    const cell = ev.target.closest("td[data-dest-dataset]");
    if (!cell) return;
    state.destDataset = cell.dataset.destDataset || "";
    state.destSnapshot = isSnapshotName(state.destDataset) ? state.destDataset.split("@", 2)[1] : "";
    renderDatasets();
    renderDatasetProps();
  });

  ui.copyBtn.addEventListener("click", async () => {
    if (ui.copyBtn.disabled) return;
    appendLocalLog("info", "application", `Copiar solicitado: ${state.originDataset} -> ${state.destDataset}`);
    setStatus("Copiar: funcionalidad pendiente (backend)");
  });
  ui.levelBtn.addEventListener("click", async () => {
    if (ui.levelBtn.disabled) return;
    appendLocalLog("info", "application", `Nivelar solicitado: ${state.originDataset} -> ${state.destDataset}`);
    setStatus("Nivelar: funcionalidad pendiente (backend)");
  });
  ui.syncBtn.addEventListener("click", async () => {
    if (ui.syncBtn.disabled) return;
    appendLocalLog("info", "application", `Sincronizar solicitado: ${state.originDataset} -> ${state.destDataset}`);
    setStatus("Sincronizar: funcionalidad pendiente (backend)");
  });
  ui.breakdownBtn.addEventListener("click", async () => {
    if (ui.breakdownBtn.disabled) return;
    const ds = state.originDataset || state.destDataset;
    appendLocalLog("info", "application", `Desglosar solicitado: ${ds}`);
    setStatus("Desglosar: funcionalidad pendiente (backend)");
  });
  ui.assembleBtn.addEventListener("click", async () => {
    if (ui.assembleBtn.disabled) return;
    const ds = state.originDataset || state.destDataset;
    appendLocalLog("info", "application", `Ensamblar solicitado: ${ds}`);
    setStatus("Ensamblar: funcionalidad pendiente (backend)");
  });

  ui.logLevelSelect.addEventListener("change", () => {
    state.logLevel = ui.logLevelSelect.value;
    renderLogs();
  });

  ui.reloadLogsBtn.addEventListener("click", async () => {
    if (state.busy || state.refreshing) return;
    await loadLogs();
  });

  ui.clearLogsBtn.addEventListener("click", clearLogsView);
  ui.copyLogsBtn.addEventListener("click", copyLogs);
}

async function boot() {
  const saved = localStorage.getItem(API_KEY);
  if (saved) ui.apiUrl.value = saved;
  leftTab("connections");
  poolTab("imported");
  poolDetailTab("props");
  bindEvents();

  try {
    await fetchJson("/health");
    appendLocalLog("normal", "application", "Aplicacion iniciada");
    await refreshAll();
  } catch (err) {
    setStatus(`Error inicial: ${err.message}`);
  }
}

boot();
