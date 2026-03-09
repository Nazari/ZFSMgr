# Shortcuts and states

- Action buttons are disabled while an action is running.
- Cursor changes to busy during actions and refresh.
- Some actions require valid selection (dataset or snapshot).
- If source and target are equal, some transfers are blocked.
- In `Dataset properties`, if you edit a value and change tab/focus, the pending edit is preserved.
- Returning to the same dataset restores edited values and keeps `Apply changes` enabled until apply/revert.
- Navigation between connections/pools does not force refresh: cache is reused and refresh runs only on actions or explicit refresh.

Common states:

- `OK` (green): connection works.
- `KO/Error` (red): connection or command failed.
- `OK` with missing commands (orange): connection is up but auxiliary commands are missing.
- `Mounted/Unmounted`: current dataset state.
- `Non-importable pools`: shown in `Connection status`, one block per pool with reason.
