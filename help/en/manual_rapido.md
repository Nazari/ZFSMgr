# Quick manual

ZFSMgr manages connections and ZFS actions.

- Left panel:
- `Connections`: simple list (one row per connection).
- `New`: `Connection`, `Pool`, and `Refresh All` buttons.
- `Actions`: transfer and advanced operations using source/target.
- Right panel:
- When a connection is selected: `Connection <name>` tab.
- In that tab: connection properties (top) and connection status (bottom).
- For selected connections with imported pools: one tab per imported pool.
- `Properties <pool>` subtab: pool properties (top) and pool status (bottom).
- `Content <pool>` subtab: dataset/snapshot tree (top) and dataset/snapshot properties (bottom).
- Non-importable pools do not get tabs; they are listed in connection status with reason.
- Logs: single `Combined log` panel (includes SSH/PSRP output with connection prefix).

Navigation behavior:

- Switching connection/tab reuses cached data.
- No automatic refresh happens just for navigation.
- Refresh runs after modifying actions or explicit refresh.

Check "Shortcuts and states" for action enabling rules.
