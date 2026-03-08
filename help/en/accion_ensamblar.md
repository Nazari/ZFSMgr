# Action: Assemble

Goal: convert child datasets into directories under the parent dataset.

Conditions:

- Dataset selected in `Content <pool>`.
- Dataset and descendants mounted (safety rules).

Behavior:

- Shows a selection dialog for child datasets.
- Copies each child dataset content into parent directory.
- Destroys child dataset only if copy succeeds.
- Logs progress per child dataset at NORMAL level.
