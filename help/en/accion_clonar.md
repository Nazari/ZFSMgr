# Action: Clone

Goal: clone a snapshot into a target dataset using `zfs clone`.

Button enable conditions:

- Source must be a `snapshot`.
- Target must be a `dataset` (no snapshot selected).
- Source and target must be in the same connection.
- Source and target must belong to the same pool.

Options in the Clone dialog:

- `-p` create parent datasets if missing.
- `-u` do not auto-mount the clone.
- `-o property=value` (one per line) to set clone properties.

Base command:

`zfs clone [-p] [-u] [-o property=value]... <source@snapshot> <target_dataset>`

Notes:

- If conditions are not met, the button is disabled.
- After completion, target connection and dataset view are refreshed.
