# Quick manual

ZFSMgr manages connections and ZFS actions from a unified tree.

## Overview

![Main window](qrc:/help/img/auto/main-window.png)

- Left column:
- `Selected datasets`: shows the dataset marked as `Source` and the one marked as `Target`.
- `Status and progress`: current status, loading and progress.
- Right column:
- one unified tree with:
  - connections as root nodes
  - pools under each connection
  - datasets and snapshots under each pool
- Below the tree:
  - `Pending changes`
- Bottom area:
  - logs

## Unified tree

- Connections are always visible as root nodes, even when disconnected.
- If a connection is disconnected:
  - the connection root stays visible
  - its pools disappear from the tree
- Connection row colors and tooltips keep the same meaning the old table had.
- If a connection needs GSA attention, its name shows `(*)`.
- Pools are no longer shown as `Connection::Pool`; the visible pool text is just the pool name.
- The pool root is merged with the pool root dataset:
  - it keeps the pool icon and pool tooltip
  - it also acts as the root dataset
  - its real children hang directly from it
- Imported pools may show:
  - `Pool Information`
  - `Datasets programados`

## Inline nodes

- Datasets and snapshots may show `Dataset properties`.
- Non-snapshot datasets may also show `Permissions`.
- Filesystem datasets may show `Schedule snapshots`.

- `Schedule snapshots` view:

![Schedule snapshots node](qrc:/help/img/auto/schedule-snapshots-node.png)

- Inline properties can be edited directly in the tree.
- If a property supports inheritance, it shows `Inh.` and stays in draft mode until changes are applied.
- `Permissions` also works in draft mode.
- `Schedule snapshots` uses `org.fc16.gsa:*` properties.

## Source and target selection

- There are no longer `Source/Target` checks in a connections table.
- To choose them:
  - right click a dataset
  - `Select as source`
  - `Select as destination`
- The `Selected datasets` box reflects that logical selection.
- The current visual selection in the tree and the logical `Source/Target` selections are independent.

## Context menus

- On a connection root:
  - the old connection context menu is available
- On the merged pool root:
  - a `Pool` submenu appears first
  - then the dataset actions follow
- The `Pool` submenu contains:
  - `Refresh`
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
    - `Destroy`
  - `Show Pool Information`
  - `Show Scheduled Datasets`
- Dataset/snapshot actions still include:
  - `Create dataset/snapshot/vol`
  - `Rename`
  - `Delete`
  - `Encryption`
  - `Select snapshot`
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
  - `Rename`
  - `Move`
  - deferred dataset/snapshot deletion

## Connectivity and logs

- `Check connectivity` is no longer a floating button.
- It now lives in the main application menu.
- `Combined log` still shows application and connection output.

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

## Navigation

- The tree keeps expansion, selection and selected snapshots.
- Changing property columns preserves open nodes.
- Clicking an empty `Dataset properties` node materializes its children and keeps it open.
