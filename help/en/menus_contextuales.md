# Context menus

The GUI uses right-click context menus in two areas.

## Context menu in `Connections`

On the selected connection:

- `Refresh`
- `Edit`
- `Delete`

Notes:

- `Edit` and `Delete` are disabled for `Local` and connections redirected to `Local`.
- While actions are running, `Refresh` is blocked.

## Context menu in `Content <pool>`

On the selected dataset/snapshot:

- `Rollback`
- `Create`
- `Delete`
- `Source`
- `Target`
- `Break down`
- `Assemble`
- `From Dir`
- `To Dir`

## Rules

- Destructive actions always require confirmation.
- Enabled/disabled state follows the same validation logic used by other actions.
- Unsafe options are blocked while an action is running.
