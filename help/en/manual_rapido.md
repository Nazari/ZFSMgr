# Quick manual

ZFSMgr manages connections and ZFS actions from a unified tree.

## Overview

![Main window](qrc:/help/img/auto/main-window.png)

- Top area: a single unified tree spanning the full width and nearly the full height.
- Middle band, a **single line**: `Source`, `Status` and `Progress`.
- Bottom area: tabs (`Transfers`, `Settings`, `Combined log`).
  `Terminal` and `Daemon` are not here: they are sub-tabs **of each connection**, inside
  the `Combined log`.


The `Actions` box with its six buttons is gone: `Send`, `Move`, `Clone`, `Sync`, `Level`
and `Diff` are requested from the target node's context menu (see `Context menus`).
`Transfers` is the first tab at the bottom.

## Unified tree

- Tree reference:

![Unified tree](qrc:/help/img/auto/top-tree.png)

- Connections are always visible as root nodes, even when disconnected.
- If a connection is disconnected:
  - the connection root stays visible
  - it shows no children (including auxiliary nodes)
- If a connection needs daemon attention, its name shows `(*)`, and the reason is spelled
  out in the `Info` → `Daemon` node.
- If daemon-rpc is waiting after a TLS problem, the reason appears in brackets next to the
  connection name. ZFSMgr does **not** reinstall the daemon or rebuild the TLS material on
  its own: it flags it and waits for you to ask from the context menu. That is deliberate —
  re-provisioning just because a TLS handshake failed can lock you out of a connection that
  was fine.
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

- Only the **source** is marked: right-click a dataset or snapshot → `Mark as source`.
- The **target is not marked**. It is the node whose context menu you open to request the
  action, just like pasting.
- The `Source:` line in the top band remembers what is marked; its tooltip shows it in
  full when the name does not fit.
- The tree's visual selection and the marked source are independent.

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

## Transfers

This tab used to be `Pending changes` and held commands waiting for you to press
`Apply changes`. **Not any more.** Actions run when you press them, and the tab now shows the
**jobs in flight**: what is running, its progress, and a button to cancel it.

What is still edited in batches are **properties** and **permissions**: they pile up as
drafts and are applied with `Apply changes`. Those are edits to a state, with a natural end;
an action like `Break down` or `To dir` is not.

What follows from the list being gone:

- Nothing survives closing the application: if you did not run it, it did not happen. The
  list used to be stored on disk, with each action's command inside it.
- There is nothing to remember to apply. An action requested and not applied used to look
  done.
- Property and permission drafts **are** lost if you close without applying them.


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
