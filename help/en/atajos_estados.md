# Shortcuts and states

- Action buttons are disabled while an action is running.
- Cursor changes to busy during actions and refresh.
- Some actions require valid selection (dataset or snapshot).
- If source and target are equal, some transfers are blocked.
- `Clone` is enabled only when source is a snapshot, target is a dataset, and both are in the same connection and pool.
- If source or target uses OpenZFS `< 2.3.3`, `Copy`, `Level`, and `Sync` are blocked and `Source/Target` labels are shown in red.
- In `Dataset properties`, if you edit a value and change tab/focus, the pending edit is preserved.
- Returning to the same dataset restores edited values and keeps `Apply changes` enabled until apply/revert.
- Navigation between connections/pools does not force refresh: cache is reused and refresh runs only on actions or explicit refresh.

Common states:

- `OK` (green): connection works.
- `KO/Error` (red): connection or command failed.
- `OK` with OpenZFS `< 2.3.3` (red): connection works, but transfer actions based on `send/recv` are blocked.
- `OK` with missing commands (orange): connection is up but auxiliary commands are missing.
- `Mounted/Unmounted`: current dataset state.
- `Non-importable pools`: shown in `Connection status`, one block per pool with reason.
