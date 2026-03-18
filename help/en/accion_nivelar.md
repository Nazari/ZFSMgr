# Action: Level

Goal: align source and target state using dataset/snapshot.

Conditions:

- Source: dataset or snapshot.
- Target: dataset.
- Source and target must run OpenZFS `2.3.3` or newer.

Behavior:

- Computes command based on selection and remote state.
- Executes transfer with pre-checks.
- If source and target are two different remote SSH connections, it tries to transfer directly from source to target without routing data through the machine running ZFSMgr.
- Logs subcommands at INFO level.
- The action is queued first in `Pending changes` and runs only when changes are applied.
- If any side is below `2.3.3`, the action is blocked.
