# Context menus

The GUI now combines visible buttons and right-click context menus.

## Current workflow

- Use buttons in `Connections` and `New`.
- Use buttons in the left `Actions` area for transfer operations.
- In `Content <pool>`, run dataset/snapshot actions using right-click on the tree.
- Apply dataset property changes with `Apply changes`.

## Context menu in `Content <pool>`

For the selected dataset/snapshot, the context menu provides:
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

- Unsafe options are blocked while an action is running.
- Destructive actions always require confirmation.
- Enabled/disabled state follows the same logic used by action buttons.

## Recommendations

- Always review the confirmation window before execution.
- Use `Reset` in the Actions area to clear source/target selection when needed.
