# Quick manual

ZFSMgr manages connections and ZFS actions.

- Left panel:
- `Connections`: simple list (one row per connection).
- `New`: create connection or create pool.
- `Actions`: transfer and advanced operations.
- Right panel:
- When a connection is selected: `Connection <name>` tab.
- For selected connections with visible pools: one tab per imported/importable pool.
- `Properties <pool>`: pool properties (top) and pool status (bottom).
- `Content <pool>`: dataset content (top) and dataset properties (bottom).
- Non-importable pools do not get tabs; they are listed in connection status with reason.
- Logs: single `Combined log` panel (includes SSH/PSRP output with connection prefix).

Check "Shortcuts and states" for action enabling rules.
