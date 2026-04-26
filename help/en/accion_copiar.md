# Action: Copy

Goal: send a snapshot from source and receive it on target.

Conditions:

- Source: snapshot selected.
- Target: dataset selected.
- Source and target must run OpenZFS `2.3.3` or newer.

Behavior:

- Uses `zfs send` and `zfs recv`.
- When both connections have an active daemon with job support (`JOBS_SUPPORT=1`), the transfer runs as a **background job**:
  - Data flows directly between daemons; it does not pass through the machine running ZFSMgr.
  - The GUI does not block; the job starts immediately and the application stays fully usable.
  - Progress is shown in the **Transferencias** tab (bytes, speed, elapsed time).
  - The GUI can be closed while the transfer continues running on the remote daemon.
  - When the GUI is reopened or the connection is re-established, running jobs are recovered automatically.
  - Any job can be cancelled from the Transferencias tab (`SIGTERM` is sent to the `zfs send` process).
- If any daemon does not support jobs, the action falls back to synchronous mode: it is queued in `Pending changes` and runs when changes are applied.
- Refreshes affected connections/trees when done.
- If any side is below `2.3.3`, the action is blocked.
