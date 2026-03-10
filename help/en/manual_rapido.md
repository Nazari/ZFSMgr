# Quick manual

ZFSMgr manages connections and ZFS actions.

- Left panel:
- `Connections`: simple table (one row per connection) with `Source` and `Target` checks.
- `Actions`: transfer and advanced operations.
- Right panel:
- Top area: tabs per pool for the connection marked as `Source`.
- Bottom area: tabs per pool for the connection marked as `Target`.
- Each pool tab shows a single `Content` tree, with `Pool` root plus dataset/snapshot nodes.
- The `Pool` root shows pool info; dataset/snapshot nodes show editable properties in `Prop.` columns.
- Non-importable pools do not get tabs; they are shown in connection tooltip/status with reason.
- Logs: single `Combined log` panel (includes SSH/PSRP output with connection prefix).

Navigation behavior:

- Switching connection/tab reuses cached data.
- No automatic refresh happens just for navigation.
- Refresh runs after modifying actions or explicit refresh.
- Before each action, active pool tabs (top and bottom) are saved and restored after completion.

Check "Shortcuts and states" for action enabling rules.
