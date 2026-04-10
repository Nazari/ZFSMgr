# Context menus

ZFSMgr uses context menus on the unified tree.

## On a connection

![Connection context menu](qrc:/help/img/auto/connection-context-menu.png)

- The menu that used to belong to the connections table now belongs to the connection root node.
- Current order on connection nodes:
  - `Connect`
  - `Disconnect`
  - `Refresh`
  - separator
  - `New connection`
  - `Edit`
  - `Delete`
  - separator
  - `GSA`
  - separator
  - `New pool`
  - separator
  - `Split and root` (submenu: `Right`, `Left`, `Below`, `Above`)
  - separator
  - `Install MSYS2`
  - `Install helper commands`

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

- On filesystem datasets (and on the merged pool node):
  - `Manage properties`
  - `Dataset`
  - `Actions`
  - `Split and root` (submenu: `Right`, `Left`, `Below`, `Above`)
  - `Select as source`
  - `Select as destination`
- `Dataset` submenu:
  - `Create`
  - `Rename`
  - `Delete`
  - `Encryption key` (`Load key`, `Unload key`, `Change key`)
  - `Schedule snapshots`
  - `Permissions` (`New set`, `New delegation`)
- `Actions` submenu:
  - `Break down`
  - `Assemble`
  - `From Dir`
  - `To Dir`
- On snapshots:
  - `Manage properties`
  - `Delete snapshot`
  - `Rollback`
  - `New Hold`
  - `Select as source`
- On hold nodes:
  - `Release`

## On the root node of a split panel

- If the node is the root of a split panel, an additional option appears:
  - `Close`: closes that panel and releases its space in the splitter.

## Rules

- Destructive actions ask for confirmation.
- Several actions are deferred and accumulate in `Pending changes`.
- `Select as source` and `Select as destination` update the `Source/Target` line in `Actions`.
- There is no context menu on `Dataset properties`, `Snapshot properties`, or the `@` node.
- On suspended pools, most context menu actions are disabled.
