# Action: Copy

Goal: send a snapshot from source and receive it on target.

Conditions:

- Source: snapshot selected.
- Target: dataset selected.
- Source and target must run OpenZFS `2.3.3` or newer.

Behavior:

- Uses `zfs send` and `zfs recv`.
- If source and target are two different remote SSH connections, ZFSMgr tries to run the transfer directly from source to target.
  In that case, the data stream does not pass through the machine running ZFSMgr; only control and progress reporting do.
- Shows progress in combined log.
- The action is queued first in `Pending changes`; it runs only when pending changes are applied.
- Refreshes affected connections/trees when done.
- If any side is below `2.3.3`, the action is blocked.
