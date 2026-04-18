# Application logs

The bottom area uses tabs:

- `Settings`: log options and action confirmation.
- `Combined log`: main application log.
- `Terminal`: technical output of local/remote commands.
- `GSA`: events specific to automatic snapshot scheduling.

`Combined log`:

- Includes internal application events.
- Includes relevant execution output in compact format.

## Initial load on startup

When ZFSMgr starts:

- Persisted logs are read (`application.log` and rotated files `.1` ... `.5`).
- Only the last `N` lines are loaded into the view.
- `N` is the configured maximum lines limit from `Settings`.
- If logs do not exist or are empty, no error is shown.

## Compact on-screen rendering

Each new line is compared against the previous visible line.  
The view shows only changes in:

- Date.
- Time.
- Connection.
- Log level.

If none of those fields changes, `...` is shown as compact header.

Visual format:

- `<changes> | <message>`

## Persistence

- Full-format lines are still stored on disk for traceability.
- Compact rendering is only applied in the on-screen view.
