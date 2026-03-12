# Action: Copy

Goal: send a snapshot from source and receive it on target.

Conditions:

- Source: snapshot selected.
- Target: dataset selected.
- Source and target must run OpenZFS `2.3.3` or newer.

Behavior:

- Uses `zfs send` and `zfs recv`.
- Shows progress in combined log.
- Refreshes target connection when done.
- If any side is below `2.3.3`, the action is blocked.
