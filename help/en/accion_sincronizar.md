# Action: Sync

Goal: synchronize source dataset content into target dataset.

Conditions:

- Source: dataset selected.
- Target: dataset selected.
- Source and target must be different.
- Source and target must run OpenZFS `2.3.3` or newer.

Behavior:

- Uses `rsync` or `tar` depending on platform/transport.
- The action is queued first in `Pending changes` and runs only when changes are applied.
- Shows progress (MB/GB transferred) in logs.
- Supports cancellation.
- If any side is below `2.3.3`, the action is blocked.
