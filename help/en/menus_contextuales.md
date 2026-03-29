# Context menus

ZFSMgr uses context menus on the unified tree.

## On a connection

![Connection context menu](qrc:/help/img/auto/connection-context-menu.png)

- The menu that used to belong to the connections table now belongs to the connection root node.
- It includes actions such as:
  - `New connection`
  - `Refresh`
  - `Edit`
  - `Delete`
  - `New pool`
  - GSA and refresh actions

## On the merged pool root

![Imported pool context menu](qrc:/help/img/auto/pool-context-menu-imported.png)

- The first submenu is `Pool`.
- Inside `Pool` you get the pool actions:
  - `Refresh`
  - `Import`
  - `Import with rename`
  - `Export`
  - `History`
  - `Management`
  - `Show Pool Information`
  - `Show Scheduled Datasets`
- After the `Pool` submenu, the normal dataset actions continue for that same merged node.

## On datasets and snapshots

- Typical actions:
  - `Manage visible properties`
  - `Show inline properties`
  - `Show inline permissions`
  - `Create dataset/snapshot/vol`
  - `Rename`
  - `Delete`
  - `Encryption`
  - `Select snapshot`
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
- `Select as source` and `Select as destination` fill the `Selected datasets` box.
- `Manage visible properties` applies to both `Dataset properties` and `Pool Information`.
