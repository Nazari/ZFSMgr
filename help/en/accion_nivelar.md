# Action: Level

Goal: align source and target state using dataset/snapshot.

Conditions:

- Source: dataset or snapshot.
- Target: dataset.
- Source and target must run OpenZFS `2.3.3` or newer.

Behavior:

- Computes command based on selection and remote state.
- Executes transfer with pre-checks.
- When both connections have an active daemon with job support (`JOBS_SUPPORT=1`), the transfer runs as a **background job**:
  - Data flows directly between daemons without passing through the machine running ZFSMgr.
  - The GUI does not block; progress is shown in the **Transferencias** tab.
  - The GUI can be closed while the transfer continues on the daemon.
  - Jobs can be cancelled from the Transferencias tab.
- If any daemon does not support jobs, the action falls back to synchronous mode: queued in `Pending changes` and run when changes are applied.
- Logs subcommands at INFO level.
- If any side is below `2.3.3`, the action is blocked.
