# Application logs

This panel uses a combined log:

- It includes internal application events.
- It includes SSH/PSRP command output from all connections.
- Remote session lines are prefixed as: `[SSH <connection>]`.

## Initial load on startup

When ZFSMgr starts:

- Persisted logs are read (`application.log` and rotated files `.1` ... `.5`).
- Only the last `N` lines are loaded into the view.
- `N` is the configured maximum lines limit.
- If logs do not exist or are empty, no error is shown.

## Compact on-screen rendering

Each new line is compared against the previous visible line.  
The view shows only changes in:

- Date.
- Time.
- SSH connection.
- Log level.

If none of those fields changes, `...` is shown as compact header.

Visual format:

- `<changes> | <message>`

Examples:

- `2026-03-08 10:12:01 ssh=fc16 lvl=INFO | Refresh started...`
- `... | zpool list -H ...`
- `10:12:02 lvl=NORMAL | Refresh finished: fc16 -> OK`

## Persistence

- Full-format lines are still stored on disk for traceability.
- The compact rendering is only applied in the on-screen view.
