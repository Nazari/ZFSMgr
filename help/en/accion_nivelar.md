# Action: Level

Goal: align source and target state using dataset/snapshot.

Conditions:

- Source: dataset or snapshot.
- Target: dataset.
- Source and target must run OpenZFS `2.3.3` or newer.

Behavior:

- Computes command based on selection and remote state.
- Executes transfer with pre-checks.
- Logs subcommands at INFO level.
- If any side is below `2.3.3`, the action is blocked.
