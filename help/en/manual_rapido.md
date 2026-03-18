# Quick manual

ZFSMgr manages connections and ZFS actions.

- Left panel:
- `Connections`: simple table (one row per connection) with `Source` and `Target` checks.
- `Actions`: transfer and advanced operations.
  Includes `Copy`, `Clone`, `Move`, `Level`, `Sync`, `Break down`, `Assemble`, `From Dir`, and `To Dir`.
- Right panel:
- Top area: content tree for the connection marked as `Source`.
- Bottom area: content tree for the connection marked as `Target`.
- Each tree can show multiple pools at once (one root node per pool), with dataset/snapshot nodes below.
- A pool can show `Pool information` as a dedicated node.
- Datasets hang directly from the pool root.
- Child datasets hang directly from their parent dataset.
- Dataset/snapshot nodes can show inline `Properties`, and non-snapshot datasets can also show `Permissions`.
- The tree context menu can show or hide `Pool information`, inline `Properties`, and inline `Permissions`.
- Permission sections are shown as `Deleg.`, `New child DS`, and `Sets`.
- Non-importable pools are also shown as root nodes so `Import` can be executed.
- Logs: single `Combined log` panel (includes SSH/PSRP output with connection prefix).
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

Navigation behavior:

- Switching connection/pool reuses cached data.
- No automatic refresh happens just for navigation.
- Refresh runs after modifying actions or explicit refresh.
- Before each action, both trees preserve/restore visual state (selection and node expansion, when applicable).
- If a modification affects a pool shown in both trees, both trees are rebuilt and their state is restored.
- Clicking an empty `Properties` node loads its children and keeps it expanded.
- Changing `Property columns` preserves expansion of an already open `Properties` node.

Check "Shortcuts and states" for action enabling rules.
