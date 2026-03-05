# Action: Copy

Goal: send a snapshot from source and receive it on target.

Conditions:

- Source: snapshot selected.
- Target: dataset selected.

Behavior:

- Uses `zfs send` and `zfs recv`.
- Shows progress in combined log.
- Refreshes target connection when done.

