const API_KEY = "zfsmgr.api.base";

const state = {
  connections: [],
  selectedConnectionId: null,
  pools: [],
  datasets: [],
  properties: []
};

const ACTION_BUTTON_IDS = [
  "importPoolBtn",
  "exportPoolBtn",
  "createDatasetBtn",
  "deleteDatasetBtn",
  "renameDatasetBtn",
  "setPropBtn",
  "inheritPropBtn"
];

const ui = {
  apiUrl: document.getElementById("apiUrl"),
  saveApiBtn: document.getElementById("saveApiBtn"),
  reloadBtn: document.getElementById("reloadBtn"),
  connectionsMeta: document.getElementById("connectionsMeta"),
  actionStatus: document.getElementById("actionStatus"),
  connectionsBody: document.getElementById("connectionsBody"),
  leftTabConnections: document.getElementById("leftTabConnections"),
  leftTabDatasets: document.getElementById("leftTabDatasets"),
  leftPanelConnections: document.getElementById("leftPanelConnections"),
  leftPanelDatasets: document.getElementById("leftPanelDatasets"),
  selectedConn: document.getElementById("selectedConn"),
  detailPools: document.getElementById("detailPools"),
  detailDatasets: document.getElementById("detailDatasets"),
  poolList: document.getElementById("poolList"),
  datasetList: document.getElementById("datasetList"),
  propertyList: document.getElementById("propertyList"),

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

const datasetInputs = [
  () => ui.createDataset,
  () => ui.deleteDataset,
  () => ui.renameSource,
  () => ui.propDataset
];

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

function setDatalistOptions(el, values) {
  el.innerHTML = (values || [])
    .map((v) => `<option value="${String(v).replace(/"/g, "&quot;")}"></option>`)
    .join("");
}

function appendLogLine(msg) {
  ui.logsBox.textContent = `${ui.logsBox.textContent}\n${msg}`.trim();
  ui.logsBox.scrollTop = ui.logsBox.scrollHeight;
}

function setActionStatus(text, type = "neutral") {
  ui.actionStatus.textContent = text;
  ui.actionStatus.classList.remove("ok", "err");
  if (type === "ok") ui.actionStatus.classList.add("ok");
  if (type === "err") ui.actionStatus.classList.add("err");
}

function setButtonsDisabled(disabled) {
  for (const id of ACTION_BUTTON_IDS) {
    const btn = ui[id];
    if (btn) btn.disabled = disabled;
  }
}

function clearInvalidMarks() {
  const inputs = [
    ui.poolName,
    ui.createDataset,
    ui.createVolsize,
    ui.deleteDataset,
    ui.renameSource,
    ui.renameTarget,
    ui.propDataset,
    ui.propName,
    ui.propValue
  ];
  inputs.forEach((el) => {
    el.classList.remove("input-error");
    el.title = "";
  });
}

function markInvalid(el, reason) {
  if (!el) return;
  el.classList.add("input-error");
  if (reason) el.title = reason;
}

function validateNonEmpty(el, label) {
  if (!el.value.trim()) {
    markInvalid(el, `${label} is required`);
    throw new Error(`${label} is required`);
  }
}

function assertKnownPool(poolName) {
  if (!poolName) return;
  if (state.pools.length > 0 && !state.pools.includes(poolName)) {
    markInvalid(ui.poolName, "Pool not found in selected connection");
    throw new Error(`Pool not found: ${poolName}`);
  }
}

function assertKnownDataset(datasetName, inputEl, label = "Dataset") {
  if (!datasetName) return;
  if (state.datasets.length > 0 && !state.datasets.includes(datasetName)) {
    markInvalid(inputEl, `${label} not found in selected pool`);
    throw new Error(`${label} not found: ${datasetName}`);
  }
}

function getPoolFromDataset(dataset) {
  const raw = (dataset || "").trim();
  if (!raw) return "";
  const idx = raw.indexOf("/");
  return idx > 0 ? raw.slice(0, idx) : raw;
}

function propagateDatasetSelection(sourceInput) {
  const value = sourceInput.value.trim();
  if (!value) return;
  for (const getInput of datasetInputs) {
    const input = getInput();
    if (input !== sourceInput && !input.value.trim()) {
      input.value = value;
    }
  }
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

function activateLeftTab(name) {
  const isConnections = name === "connections";
  ui.leftTabConnections.classList.toggle("active", isConnections);
  ui.leftTabDatasets.classList.toggle("active", !isConnections);
  ui.leftPanelConnections.classList.toggle("active", isConnections);
  ui.leftPanelDatasets.classList.toggle("active", !isConnections);
}

function renderDetailLists() {
  ui.detailPools.innerHTML = "";
  ui.detailDatasets.innerHTML = "";

  const pools = state.pools || [];
  const datasets = state.datasets || [];

  for (const p of pools) {
    const li = document.createElement("li");
    li.textContent = p;
    ui.detailPools.appendChild(li);
  }

  for (const ds of datasets.slice(0, 400)) {
    const li = document.createElement("li");
    li.textContent = ds;
    ui.detailDatasets.appendChild(li);
  }
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
    setActionStatus("Ready");
    updateSelectedConnLabel();
    await loadAutocompleteData();
  } catch (err) {
    ui.connectionsBody.innerHTML = "";
    ui.connectionsMeta.textContent = `Error: ${err.message}`;
    setActionStatus(`Connections error: ${err.message}`, "err");
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

async function loadAutocompleteData() {
  const conn = selectedConnection();
  if (!conn) {
    state.pools = [];
    state.datasets = [];
    setDatalistOptions(ui.poolList, []);
    setDatalistOptions(ui.datasetList, []);
    renderDetailLists();
    return;
  }

  try {
    const poolsPayload = await fetchJson(`/connections/${conn.id}/pools`);
    state.pools = poolsPayload.pools || [];
    setDatalistOptions(ui.poolList, state.pools);

    const fromDataset = getPoolFromDataset(ui.createDataset.value || ui.deleteDataset.value || ui.renameSource.value || ui.propDataset.value);
    const selectedPool = fromDataset || ui.poolName.value.trim() || state.pools[0] || "";
    if (selectedPool && !ui.poolName.value.trim()) {
      ui.poolName.value = selectedPool;
    }

    if (!selectedPool) {
      state.datasets = [];
      setDatalistOptions(ui.datasetList, []);
      renderDetailLists();
      return;
    }

    const datasetsPayload = await fetchJson(
      `/connections/${conn.id}/datasets?pool=${encodeURIComponent(selectedPool)}`
    );
    state.datasets = datasetsPayload.datasets || [];
    setDatalistOptions(ui.datasetList, state.datasets);
    renderDetailLists();
  } catch (_err) {
    state.pools = [];
    state.datasets = [];
    setDatalistOptions(ui.poolList, []);
    setDatalistOptions(ui.datasetList, []);
    setActionStatus("Autocomplete data unavailable for selected connection", "err");
    renderDetailLists();
  }
}

async function loadEditableProperties() {
  try {
    const payload = await fetchJson("/dataset_properties/editable");
    state.properties = payload.properties || [];
    setDatalistOptions(ui.propertyList, state.properties);
  } catch (_err) {
    state.properties = [];
    setDatalistOptions(ui.propertyList, []);
  }
}

async function refreshConnection(id) {
  await fetchJson(`/connections/${id}/refresh`, { method: "POST" });
}

async function runAction(path, body) {
  const conn = requireSelectedConnection();
  setActionStatus(`Running ${path} on ${conn.name}...`);

  await fetchJson(`/connections/${conn.id}/actions/${path}`, {
    method: "POST",
    body: JSON.stringify(body || {})
  });

  await Promise.all([loadConnections(), loadLogs()]);
  setActionStatus(`${path} completed on ${conn.name}`, "ok");
}

async function doImportPool() {
  validateNonEmpty(ui.poolName, "Pool");
  const pool = ui.poolName.value.trim();
  assertKnownPool(pool);
  await runAction("import_pool", { pool });
}

async function doExportPool() {
  validateNonEmpty(ui.poolName, "Pool");
  const pool = ui.poolName.value.trim();
  assertKnownPool(pool);
  await runAction("export_pool", { pool });
}

async function doCreateDataset() {
  const kind = ui.createKind.value;
  validateNonEmpty(ui.createDataset, "Dataset");
  const dataset = ui.createDataset.value.trim();
  if (state.datasets.length > 0 && state.datasets.includes(dataset)) {
    markInvalid(ui.createDataset, "Dataset already exists");
    throw new Error(`Dataset already exists: ${dataset}`);
  }
  const body = {
    dataset,
    kind,
    recursive: ui.createRecursive.value === "true"
  };

  if (kind === "volume") {
    validateNonEmpty(ui.createVolsize, "Volsize");
    body.volsize = ui.createVolsize.value.trim();
  }

  await runAction("create_dataset", body);
}

async function doDeleteDataset() {
  const dataset = ui.deleteDataset.value.trim();
  validateNonEmpty(ui.deleteDataset, "Dataset");
  assertKnownDataset(dataset, ui.deleteDataset);
  await runAction("delete_dataset", {
    dataset,
    recursive: ui.deleteRecursive.value === "true"
  });
}

async function doRenameDataset() {
  const source = ui.renameSource.value.trim();
  const target = ui.renameTarget.value.trim();
  validateNonEmpty(ui.renameSource, "Rename source");
  validateNonEmpty(ui.renameTarget, "Rename target");
  assertKnownDataset(source, ui.renameSource, "Source dataset");
  if (state.datasets.length > 0 && state.datasets.includes(target)) {
    markInvalid(ui.renameTarget, "Target already exists");
    throw new Error(`Target dataset already exists: ${target}`);
  }
  await runAction("rename_dataset", {
    source,
    target
  });
}

async function doSetProperty() {
  const dataset = ui.propDataset.value.trim();
  const prop = ui.propName.value.trim();
  validateNonEmpty(ui.propDataset, "Dataset");
  validateNonEmpty(ui.propName, "Property");
  validateNonEmpty(ui.propValue, "Value");
  assertKnownDataset(dataset, ui.propDataset);
  if (state.properties.length > 0 && !state.properties.includes(prop)) {
    markInvalid(ui.propName, "Property not in editable list");
    throw new Error(`Property not editable: ${prop}`);
  }
  await runAction("set_property", {
    dataset,
    property: prop,
    value: ui.propValue.value.trim()
  });
}

async function doInheritProperty() {
  const dataset = ui.propDataset.value.trim();
  const prop = ui.propName.value.trim();
  validateNonEmpty(ui.propDataset, "Dataset");
  validateNonEmpty(ui.propName, "Property");
  assertKnownDataset(dataset, ui.propDataset);
  if (state.properties.length > 0 && !state.properties.includes(prop)) {
    markInvalid(ui.propName, "Property not in editable list");
    throw new Error(`Property not editable: ${prop}`);
  }
  await runAction("inherit_property", {
    dataset,
    property: prop
  });
}

async function guardAction(fn) {
  clearInvalidMarks();
  setButtonsDisabled(true);
  try {
    await fn();
    await loadAutocompleteData();
  } catch (err) {
    setActionStatus(err.message || "Action failed", "err");
    appendLogLine(`Action failed: ${err.message}`);
  } finally {
    setButtonsDisabled(false);
  }
}

function bindEvents() {
  activateLeftTab("connections");

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
      loadAutocompleteData();
      activateLeftTab("datasets");
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

  ui.poolName.addEventListener("change", () => {
    loadAutocompleteData();
  });

  ui.leftTabConnections.addEventListener("click", () => activateLeftTab("connections"));
  ui.leftTabDatasets.addEventListener("click", () => activateLeftTab("datasets"));

  for (const getInput of datasetInputs) {
    const input = getInput();
    input.addEventListener("change", async () => {
      propagateDatasetSelection(input);
      const inferredPool = getPoolFromDataset(input.value);
      if (inferredPool && inferredPool !== ui.poolName.value.trim()) {
        ui.poolName.value = inferredPool;
        await loadAutocompleteData();
      }
    });
  }

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

  await Promise.all([loadConnections(), loadLogs(), loadEditableProperties()]);
}

boot();
