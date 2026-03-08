# Shortcuts and states

- Buttons are disabled while an action is running.
- Cursor changes to busy during actions and refresh.
- Some actions require valid selection (dataset or snapshot).
- If source and target are equal, some transfers are blocked.
- In `Dataset properties`, if you edit a value and change tab/focus, the pending edit is preserved.
- When you return to the same dataset, the edited value is restored and `Apply changes` stays enabled until applied or reverted.

Common states:

- OK: connection works.
- KO/Error: connection or command failed.
- Mounted/Unmounted: current dataset state.
- Non-importable pools: shown in `Connection status` with reason.
