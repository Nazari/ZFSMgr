# Action: Sync

Goal: synchronize source dataset content into target dataset.

Conditions:

- Source: dataset selected.
- Target: dataset selected.
- Source and target must be different.

Behavior:

- Uses `rsync` or `tar` depending on platform/transport.
- Shows progress (MB/GB transferred) in logs.
- Supports cancellation.

