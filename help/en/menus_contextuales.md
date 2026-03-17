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

## Context menu in the pool tree

On the `Pool` root node:

- `Refresh`
- `Import`
- `Export`
- `History`
- `Sync`
- `Trim`
- `Initialize`
- `Scrub`
- `Destroy`
- `Show pool information`

On selected dataset/snapshot:

- `Manage property visibility`
- `Show inline properties` (check)
- `Show inline permissions` (check)
- `Rollback`
- `Create`
- `Rename`
- `Delete`
- `Encryption`
- `Select snapshot`
- `New Hold`
- `Release <hold>`
- `Break down`
- `Assemble`
- `From Dir`
- `To Dir`

On the `Permissions` node of a dataset:

- `Refresh permissions`
- `New delegation`
- `New permission set`

On a delegation:

- `Edit delegation`
- `Delete delegation`

On a permission set:

- `Rename permission set`
- `Delete set`

## Rules

- Destructive actions always require confirmation.
- Enabled/disabled state follows the same validation logic used by other actions.
- Unsafe options are blocked while an action is running.
- `Show inline properties`, `Show inline permissions`, and `Show pool information` are persisted in configuration.
- The tree no longer uses intermediate `Content` or `Subdatasets` nodes.
- The `Pending changes` tab in the lower log area lists deferred commands in execution order.
- Clicking a `Pending changes` line makes ZFSMgr try to focus the affected dataset and section (`Properties` or `Permissions`).
- `Rename` on a dataset, snapshot, or zvol is deferred.
  It opens a dialog for the new name and adds a pending `zfs rename`.
- The `Actions` box also includes `Move`, which queues a pending `zfs rename` to move the `Source` dataset under the `Target` dataset in the same pool.
