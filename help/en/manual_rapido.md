# Quick manual

ZFSMgr manages connections and ZFS actions.

- Left panel:
- `Connections`: simple table (one row per connection) with `Source` and `Target` checks.
- `Actions`: transfer and advanced operations.
  Includes `Copy`, `Clone`, `Level`, `Sync`, `Break down`, `Assemble`, `From Dir`, and `To Dir`.
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

Navigation behavior:

- Switching connection/pool reuses cached data.
- No automatic refresh happens just for navigation.
- Refresh runs after modifying actions or explicit refresh.
- Before each action, both trees preserve/restore visual state (selection and node expansion, when applicable).

Check "Shortcuts and states" for action enabling rules.
