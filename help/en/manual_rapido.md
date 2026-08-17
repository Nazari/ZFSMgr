# Quick manual

ZFSMgr manages connections and ZFS actions from a unified tree.

## Overview

![Main window](qrc:/help/img/auto/main-window.png)

- Top area: a single unified tree spanning the full width and nearly the full height.
- Middle band, a **single line**: `Source`, `Status` and `Progress`.
- Bottom area: tabs (`Pending changes`, `Settings`, `Combined log`, `Transfers`).
  `Terminal` and `Daemon` are not here: they are sub-tabs **of each connection**, inside
  the `Combined log`.


The `Actions` box with its six buttons is gone: `Copy`, `Move`, `Clone`, `Sync`, `Level`
and `Diff` are requested from the target node's context menu (see `Context menus`).
`Pending changes` became the first tab at the bottom.

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

## Pending changes

- `Pending changes` shows readable descriptions, not raw commands.
- **The tab title carries the count in parentheses, in bold**, whenever something is
  pending, so it does not go unnoticed while the tab is out of sight.
- Changes accumulate in insertion order.
- Clicking one line makes ZFSMgr try to focus the affected object and section.
- Typical deferred actions:
  - property changes
  - permissions
  - `Rename`, `Move`, `Rollback`, `Hold`, `Release`
  - `Copy`, `Level`, `Sync`
  - deferred dataset/snapshot deletion

### The list is a work plan, not a queue that drains

**Actions** (`Breakdown`, `Assemble`, `From Dir`, `To Dir`, mount, unmount, create,
destroy…) behave like this:

- **They are not removed when they run.** They keep their result and **untick
  themselves**. Unticking rather than deleting is what stops a second `Apply changes`
  from accidentally repeating a `Breakdown` or a `To Dir` with deletion.
- **`Active` checkbox**: decides whether the entry takes part in the next
  `Apply changes`. Ticking it again is all it takes to run the action once more.
- **The list survives closing the app.** Leave without applying and it is still there
  next time.
- **`Set name...`** (context menu) to tell similar entries apart.
- **`Edit...`** (context menu) reopens the dialog with what you asked for, for all four
  advanced actions. Cancelling the edit does **not** delete the action.
- Removing an entry is **manual**: `Delete`, or `Empty list` to discard everything, which
  asks for confirmation and lists what it takes with it.
- A queued action stores the command ALREADY BUILT, so it does not pick up later fixes to
  the program. If another version queued it, the row shows **⚠** and you are asked before
  it runs: the safe move is to remove it and request the action again.

**Properties, permissions and renames** do not work this way: they still disappear once
applied. They are edits to a state, with a natural end, not jobs worth repeating.

### What is NOT written to disk

- **Passwords.** Each action's command carries the `sudo` password inside it; on save it
  is replaced by a marker and on load restored from the connection, where it lives
  encrypted. Change the password between sessions and the restored action uses the new
  one.
- **Encryption passphrases.** An action that creates an encrypted dataset **is not
  saved**: saving it without the secret would be worse, since applying it would create
  the dataset unencrypted or fail midway. Re-editing it asks for the passphrase again.
- An action whose **connection no longer exists** is dropped at startup, rather than
  staying as a line that fails when you click it.

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
