# Action: Clone

> **How to invoke it.** Right-click the source (`Mark as source`), then open the context menu **on the target node**: the `With source …` submenu offers this action. There is no button any more. If it is greyed out, the reason is in its tooltip. See `Context menus`.

Goal: clone a snapshot into a target dataset using `zfs clone`.

Button enable conditions:

- Source must be a `snapshot`.
- Target must be a `dataset` (no snapshot selected).
- Source and target must be in the same connection.
- Source and target must belong to the same pool.

The target dataset is proposed as `<target>/<leaf name of the source>` (unless the target already ends with that name) and can be edited before accepting.

Options in the Clone dialog:

- `-p` create parent datasets if missing.
- `-u` do not auto-mount the clone.
- `-o property=value` (one per line) to set clone properties.

Base command:

`zfs clone [-p] [-u] [-o property=value]... <source@snapshot> <target_dataset>`

Notes:

- If conditions are not met, the button is disabled.
- **It runs when you press it.** It used to be added to a pending-changes list and wait for
  you to apply it; that list is gone.
- If either connection runs OpenZFS below `2.3.3`, the action is blocked at run time, even if the button looked enabled.
