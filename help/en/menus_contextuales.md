# Context menus

ZFSMgr uses context menus on the unified tree.

## On a connection

![Connection context menu](qrc:/help/img/auto/connection-context-menu.png)

- The menu that used to belong to the connections table now belongs to the connection root node.
- Current order on connection nodes:
  - `New connection`
  - separator
  - remaining actions in existing order (`Refresh`, `Edit`, `Delete`, `New pool`, `GSA`, etc.)

## On the merged pool root

![Imported pool context menu](qrc:/help/img/auto/pool-context-menu-imported.png)

- The first submenu is `Pool`.
- Inside `Pool` you get the pool actions:
  - `Refresh status`
  - `Import`
  - `Import with rename`
  - `Export`
  - `History`
  - `Management`
- `Management` runs immediate actions (`sync`, `scrub`, `upgrade`, `reguid`, `trim`, `initialize`, `clear`, `destroy`) with a parameter dialog when applicable.
- After the `Pool` submenu, the normal dataset actions continue for that same merged node.

## On datasets and snapshots

- Typical actions:
  - `Manage visible properties`
  - `Create dataset/snapshot/vol`
  - `Rename`
  - `Delete`
  - `Encryption`
  - `Schedule automatic snapshots` (only on filesystem datasets with no active GSA in ancestors)
  - `Rollback`
  - `New Hold`
  - `Release`
  - `Break down`
  - `Assemble`
  - `From Dir`
  - `To Dir`
  - `Select as source`
  - `Select as destination`

## Rules

- Destructive actions ask for confirmation.
- Several actions are deferred and accumulate in `Pending changes`.
- `Select as source` and `Select as destination` update the `Source/Target` line in `Actions`.
- `Manage visible properties` applies to properties nodes (`Dataset properties`, `Snapshot properties`, `Pool Information`).
