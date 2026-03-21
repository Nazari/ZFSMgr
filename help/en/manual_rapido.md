# Quick manual

ZFSMgr manages connections and ZFS actions.

## Overview

<img src="help-img/ventanaprincipal.png" alt="Main window" width="50%">

- Left panel:
- `Connections`: simple table (one row per connection) with a `Connection` column and `O` / `D` checks.
- `Selected datasets`: transfer and advanced operations.
  Includes `Copy`, `Clone`, `Move`, `Level`, `Sync`, `Break down`, `Assemble`, `From Dir`, and `To Dir`.
- Right panel:
- Top area: content tree for the connection marked as `Source`.
- Bottom area: content tree for the connection marked as `Target`.
- Effective detail selection is driven by the checks, not by simply clicking a row:
  - `O` controls the top tree (`Source`)
  - `D` controls the bottom tree (`Target`)
- Tree state is kept independently per connection/pool:
  - expanded/collapsed nodes
  - selected dataset
  - selected snapshot
  - column widths
- The first column header is always shown as `Source:...` in the top tree and `Target:...` in the bottom tree.
- Each tree can show multiple pools at once (one root node per pool), with dataset/snapshot nodes below.
- A pool can show `Pool information` as a dedicated node.
- Datasets hang directly from the pool root.
- Child datasets hang directly from their parent dataset.
- Dataset/snapshot nodes can show inline `Properties`, and non-snapshot datasets can also show `Permissions`.
- When a snapshot is selected, the tree shows that snapshot's properties/groups and also a `Holds (N)` node.
- Inline properties may include direct editing and an `Inh.` inheritance control.
- If `Inh.=on`, the value editor is disabled and greyed out.
  If `Inh.=off`, the value becomes editable again.
- The tree context menu can show or hide `Pool information`, inline `Properties`, and inline `Permissions`.
- Permission sections are shown as `Deleg.`, `New child DS`, and `Sets`.
- Non-importable pools are also shown as root nodes so `Import` can be executed.
- Logs: single `Combined log` panel (includes SSH/PSRP output with connection prefix).
- The connections table includes a floating `Connectivity` button.
  It opens a matrix where each row is the source connection and each column is the target connection.
- A `✓` means the machine in the row can connect directly to the machine in the column using the credentials defined in the target connection.
- If that `✓` is missing, ZFSMgr cannot perform a direct remote-to-remote transfer between that source and target.
  In that case, the transfer has to pass through the local machine where ZFSMgr is running.
  That means a double hop, more local traffic, and higher time/resource cost.
- The lower log area uses tabs:
  - `Pending changes` as the first visible tab by default
  - `Combined log`, which includes the `Application` box for textual logs
- `Pending changes` shows one readable description per line with a `connection::pool` prefix, not the raw command.
- Pending changes keep execution order.
- `Move` does not execute immediately: it adds a pending `zfs rename` that moves the `Source` dataset under the `Target` dataset.
  It is only enabled when both selections are datasets in the same pool and connection.
- `Rename` from the tree context menu for dataset/snapshot/zvol is also deferred and adds a `zfs rename` entry to `Pending changes`.
- Clicking a `Pending changes` line tries to focus the affected dataset and section.
  If the pool is visible in both trees, `Source` is preferred.
- `Copy` and `Level`, when they use two different remote SSH connections, try to transfer directly from `Source` to `Target`.
  The data stream does not go through the machine running ZFSMgr; that host only keeps the control session and receives progress output.
- The tree header has a context menu to resize one column, resize all visible columns, and change `Property columns`.
- If no `O` or `D` check is active for one side, that tree stays empty but keeps consistent headers.
- `O` and `D` are persisted across runs.
- The `Select snapshot` menu is only enabled when the dataset actually has snapshots.

Pool creation:

<img src="help-img/crearpool.png" alt="Create pool" width="50%">

- `Create pool` opens a dialog with a horizontal splitter:
  - left side: `Pool parameters` and `VDEV builder`
  - right side: `Available block devices`
- `altroot` starts empty by default.
  If it stays empty, `-R` is omitted from the final `zpool create` command.
- `Available block devices` shows a device/partition tree with size, partition type, mounted state, and whether the device already belongs to a pool.
- On macOS, physical disks without partitions are also shown.
- On macOS, internal/system APFS disks and synthesized APFS disks are not selectable.
- The `Mounted` column lets you unmount directly from the dialog (`diskutil unmount` / `umount`).
- Once a device is used in the pool layout, it becomes unavailable and cannot be reused elsewhere in the tree.
- `VDEV builder` no longer uses free-form text:
  - the root node is `Pool`
  - valid nodes are created through the context menu
  - block devices are dragged into the tree
  - pool-tree nodes can also be reordered by drag and drop
- The pool tree follows a restricted OpenZFS-compatible grammar:
  - the root may contain direct devices (implicit stripe), `mirror`, `raidz*`, and top-level classes (`log`, `cache`, `special`, `dedup`, `spare`)
  - normal vdevs may only contain devices
  - `log` may only contain a `mirror` subgroup
  - `special` and `dedup` may contain direct devices or `mirror` / `raidz*` subgroups
  - `cache` and `spare` may only contain direct devices
- At least one root data group must exist, either as direct devices or as `mirror` / `raidz*`.
- A full-width `zpool create` command preview is shown below the splitter.
- That preview updates when:
  - the pool tree changes
  - `Pool parameters` change
  - extra arguments change
- If the structure is not valid, the preview is shown in red.
- If `Create pool` fails, the dialog stays open so the input can be corrected and retried.

Dataset creation and encrypted mounts:

<img src="help-img/creardataset.png" alt="Create dataset" width="50%">

- `Create dataset` is launched from the content tree.
- If the dataset uses:
  - `encryption=on` or an `aes-*` mode
  - `keyformat=passphrase`
  - `keylocation=prompt`
  the dialog shows `Encryption passphrase` and `Repeat passphrase`.
- That passphrase is sent through standard input when the dataset is created; it is not appended to the previewed command line or logs.
- If `Create dataset` fails, the dialog remains open with the entered values so they can be fixed and retried.
- When mounting an encrypted dataset with `keylocation=prompt`, ZFSMgr asks for the passphrase first, runs `zfs load-key`, and then runs `zfs mount`.

Navigation behavior:

- Switching connection/pool reuses cached data.
- No automatic refresh happens just for navigation.
- Refresh runs after modifying actions or explicit refresh.
- Before each action, both trees preserve/restore visual state (selection and node expansion, when applicable).
- If a modification affects a pool shown in both trees, both trees are rebuilt and their state is restored.
- Clicking an empty `Properties` node loads its children and keeps it expanded.
- Changing `Property columns` preserves expansion of an already open `Properties` node.

Check "Shortcuts and states" for action enabling rules.
