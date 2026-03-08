# Action: From Dir

Goal: create a child dataset from an existing directory on the selected connection host.

Conditions:

- Dataset selected in `Content <pool>`.
- Selection must be a dataset (not a snapshot).

Behavior:

- Opens a dialog to define the new dataset and choose the source directory.
- Creates the dataset using a safe temporary mount workflow.
- Copies content preserving metadata/permissions.
- If copy/verification succeeds:
  - optionally removes source directory (checkbox),
  - leaves dataset unmounted to avoid overlaying the local directory when source is kept.
- Commands and progress are logged in the combined log.
