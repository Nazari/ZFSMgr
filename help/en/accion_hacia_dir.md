# Action: To Dir

Goal: copy dataset content into a directory on the selected connection host.

Conditions:

- Dataset selected in the pool tree.
- Selection must be a dataset (not a snapshot).

Behavior:

- Opens a dialog to choose destination directory.
- Copies dataset content preserving metadata/permissions.
- If copy/verification succeeds:
  - optionally deletes source dataset (checkbox).
- If dataset is not deleted, it remains unmounted to avoid overlap with destination directory.
- Commands and progress are logged in the combined log.
