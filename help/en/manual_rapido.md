# Quick manual

ZFSMgr manages connections and ZFS actions from a unified tree.

## Overview

![Main window](qrc:/help/img/auto/main-window.png)

- Top area: a single unified tree spanning the full width.
- Middle area:
  - `Status` and `Progress` row
  - `Actions` box (includes `Source` and `Target` in one line)
  - `Pending changes` box to the right of `Actions`
- Bottom area: log tabs (`Settings`, `Combined log`, `Terminal`, `Daemon`, `Transferencias`).

## Unified tree

- Tree reference:

![Unified tree](qrc:/help/img/auto/top-tree.png)

- Connections are always visible as root nodes, even when disconnected.
- If a connection is disconnected:
  - the connection root stays visible
  - it shows no children (including auxiliary nodes)
- Connection name shows active mode:
  - `(libzfs_core)` when remote daemon is active
  - `(ssh)` in fallback mode
- If a connection needs daemon attention, its name shows `(*)`.
- `Connection` and `Pool` nodes are shown in bold with a type prefix.
- The pool root is merged with the pool root dataset:
  - it keeps the pool icon
  - it also acts as the root dataset
  - avoids duplicated `pool/pool`
- Imported pools may show:
  - `Pool Information`
    - includes `Devices` (vdev/disk hierarchy from `zpool status -P`)
  - `Scheduled datasets`
- A pool in suspended state shows `(Suspended)` next to its name and blocks most of its operations.

## Inline nodes

- Datasets show `Dataset properties`.
- Snapshots show `Snapshot properties`.
- Non-snapshot datasets may also show `Permissions`.
- Datasets with snapshots show an `@` node grouping manual and GSA snapshots.
- Connections show auxiliary nodes:
  - `Connection properties` (inline, with edit permissions by connection type)
  - `Info`
    - `General` (status and connection metadata)
    - `Daemon`
    - `Commands`

- Inline properties can be edited directly in the tree.
- If a property supports inheritance, it shows `Inh.` and stays in draft mode until changes are applied.
- `Permissions` also works in draft mode.
- `Scheduled datasets` uses `org.fc16.gsa:*` properties.

## Source and target selection

- There are no longer `Source/Target` checks in a connections table.
- To choose them:
  - right click a dataset
  - `Select as source`
  - `Select as destination`
- The `Source/Target` line in the `Actions` box reflects that logical selection.
- The current visual selection in the tree and the logical `Source/Target` selections are independent.

## Context menus

- On a connection root:
  - the old connection context menu is available
- On the merged pool root:
  - a `Pool` submenu appears first
  - then the dataset actions follow
- The `Pool` submenu contains:
  - `Refresh status`
  - `Import`
  - `Import with rename`
  - `Export`
  - `History`
  - `Management`:
    - `Sync`
    - `Scrub`
    - `Upgrade`
    - `Reguid`
    - `Trim`
    - `Initialize`
    - `Clear`
    - `Destroy`
- Dataset/snapshot actions still include:
  - `Create dataset/snapshot/vol`
  - `Rename`
  - `Delete`
  - `Encryption`
  - `Schedule automatic snapshots`
  - `Rollback`
  - `New Hold`
  - `Release`
  - `Break down`
  - `Assemble`
  - `From Dir`
  - `To Dir`

## Pending changes

- `Pending changes` shows readable descriptions, not raw commands.
- Changes accumulate in insertion order.
- Clicking one line makes ZFSMgr try to focus the affected object and section.
- Typical deferred actions:
  - property changes
  - permissions
  - `Rename`, `Move`, `Rollback`, `Hold`, `Release`
  - `Copy`, `Level`, `Sync`
  - deferred dataset/snapshot deletion

## Connectivity and logs

- `Check connectivity` is in the main app menu (not under `Logs`).
- The `Logs` top menu was removed.
- The `Settings` tab now contains:
  - log level
  - number of lines
  - max rotating log size
  - confirmation before actions
  - clear/copy logs

## Pool creation

![Create pool](qrc:/help/img/crearpool.png)

- `Create pool` opens the VDEV builder and pool parameters dialog.
- The pool tree validates OpenZFS-compatible layouts.
- If creation fails, the dialog stays open so you can correct and retry.

## Dataset creation

![Create dataset](qrc:/help/img/creardataset.png)

- `Create dataset` is launched from the tree context menu.
- If the dataset is encrypted with `keylocation=prompt`, ZFSMgr asks for the passphrase.
- If creation fails, the dialog stays open with the entered values.

## Split panels (Split and root)

- The context menu on any connection, pool, or dataset node includes `Split and root`.
- Choosing a direction (`Right`, `Left`, `Below`, `Above`) opens a new tree panel alongside the existing one using a splitter.
- The root node of the panel shows the full path (e.g. `mbp::tank1/ds1/sub`).
- Split panels have full functionality: same context menus, inline properties, and configurable columns.
- Panels can be nested; each has its own column header context menu.
- To close a split panel: right-click its root node → `Close`.
- The panel layout is preserved between sessions.

## Navigation

- The tree keeps expansion, selection and selected snapshots.
- Changing property columns preserves open nodes.
- Clicking an empty properties node materializes its children and keeps it open.
