# Action: Level

Goal: align source and target state using a snapshot/dataset.

Conditions:

- Source: dataset or snapshot.
- Target: dataset.
- Source and target must run OpenZFS `2.3.3` or newer.

Levelling requirements (if they are not met the action is cancelled with a warning):

- Levelling is **always incremental** (`zfs send -I`), from the newest snapshot on the target up to the target snapshot on the source.
- The target must have **at least one snapshot**.
- The newest snapshot on the target must also exist on the source. The match is made **by GUID, not by name**: two snapshots with the same name but different GUIDs do not qualify.
- That target snapshot cannot be newer than the source's target snapshot.
- If source and target already match, you are told it is already level and nothing is done.

Behavior:

- A dialog with the `zfs send` options opens before queuing.
- If the target has an active daemon with job support (`JOBS_SUPPORT=1`) — and so does the source, when they are different connections — and neither is Windows, the transfer runs as a **background job**:
  - Data flows directly between daemons, without passing through the machine running ZFSMgr.
  - The GUI does not block; progress is shown in the **Transfers** tab.
  - The GUI can be closed while the transfer continues on the daemon.
  - Jobs can be cancelled from the Transfers tab.
- **If either end is Windows the action is unavailable**: the Windows agent does not implement streaming between machines yet.
- If there is no job support, the action falls back to synchronous mode: it is queued in `Pending changes` and runs when changes are applied.
- With source and target on the **same connection**, the `zfs send | zfs recv` pipe is built by the daemon itself (`--zfs-pipe-local`), with no remote shell. On that path **no progress lines are shown**; progress is only visible on the jobs path.
- Logs the chosen transfer mode at INFO level and the pending change at NORMAL level.
- If either side is below `2.3.3`, the action is blocked.
