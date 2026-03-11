# Quick manual

ZFSMgr manages connections and ZFS actions.

- Left panel:
- `Connections`: simple table (one row per connection) with `Source` and `Target` checks.
- `Actions`: transfer and advanced operations.
- Right panel:
- Top area: content tree for the connection marked as `Source`.
- Bottom area: content tree for the connection marked as `Target`.
- Each tree can show multiple pools at once (one root node per pool), with dataset/snapshot nodes below.
- The `Pool` root shows pool info; dataset/snapshot nodes show editable properties in `Prop.` columns.
- Non-importable pools are also shown as root nodes so `Import` can be executed.
- Logs: single `Combined log` panel (includes SSH/PSRP output with connection prefix).

Navigation behavior:

- Switching connection/pool reuses cached data.
- No automatic refresh happens just for navigation.
- Refresh runs after modifying actions or explicit refresh.
- Before each action, both trees preserve/restore visual state (selection and node expansion, when applicable).

Check "Shortcuts and states" for action enabling rules.
