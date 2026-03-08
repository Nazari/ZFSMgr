# Action: Breakdown

Goal: create child datasets from directories in the parent dataset.

Conditions:

- Dataset selected in `Content <pool>`.
- Dataset and descendants mounted (safety rules).

Behavior:

- Shows a selection dialog for directories.
- Creates child dataset, copies data, then removes source directory.
- Logs progress per directory at NORMAL level.
