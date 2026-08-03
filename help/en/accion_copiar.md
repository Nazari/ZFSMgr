# Action: Copy

Goal: send a snapshot from source and receive it on target.

Conditions:

- Source: snapshot selected.
- Target: dataset selected.
- Source and target must run OpenZFS `2.3.3` or newer.

Behavior:

- Uses `zfs send` and `zfs recv`. A dialog with the `zfs send` options opens before queuing.
- The actual `zfs recv` target is `<target dataset>/<leaf name of the source>`, unless the target already ends with that name.
- If the target has an active daemon with job support (`JOBS_SUPPORT=1`) — and so does the source, when they are different connections — and neither is Windows, the transfer runs as a **background job**:
  - Data flows directly between daemons; it does not pass through the machine running ZFSMgr.
  - The GUI does not block; the job starts immediately and the application stays fully usable.
  - Progress is shown in the **Transfers** tab (bytes transferred, speed and elapsed time).
  - The GUI can be closed while the transfer keeps running on the remote daemon.
  - When a connection with a daemon becomes active again, its running jobs are recovered automatically and reappear in the Transfers tab.
  - Any job can be cancelled from the Transfers tab (`SIGTERM` is sent to the `zfs send` process). Only a running job can be cancelled.
- If there is no job support, the action falls back to synchronous mode: it is queued in `Pending changes` and runs when changes are applied.
- With source and target on the **same connection**, the `zfs send | zfs recv` pipe is built by the daemon itself (`--zfs-pipe-local`), with no remote shell. On that path **no progress lines are shown**; progress is only visible on the jobs path.
- Windows connections have neither jobs nor a daemon: the copy is done through a `tar` fallback over `.zfs/snapshot`, without `zfs send`/`recv`.
- When finished, the **target** connection and its contents are refreshed.
- If either side is below `2.3.3`, the action is blocked.
