# Context menus

The GUI uses right-click context menus in three areas.

## Context menu in `Connections`

On a connection row:

- `Refresh`
- `Edit`
- `Delete`
- `Refresh all connections`
- `New connection`
- `New pool`

Notes:

- `Edit` and `Delete` are disabled for `Local` and connections redirected to `Local`.
- While actions are running, `Refresh` is blocked.
- If you right-click on empty table space, only global options are shown (`Refresh all`, `New connection`, `New pool`).

## Context menu in `Content <pool>`

On the `Pool` root node:

- `Refresh`
- `Import`
- `Export`
- `Scrub`
- `Destroy`

On selected dataset/snapshot:

- `Show inline properties` (check)
- `Edit`
- `Rollback`
- `Create`
- `Delete`
- `Break down`
- `Assemble`
- `From Dir`
- `To Dir`

## Rules

- Destructive actions always require confirmation.
- Enabled/disabled state follows the same validation logic used by other actions.
- Unsafe options are blocked while an action is running.
