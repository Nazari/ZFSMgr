# Context menus

ZFSMgr uses context menus on the unified tree.

## On a connection

![Connection context menu](qrc:/help/img/auto/connection-context-menu.png)

- The menu that used to hang off the connections table now hangs off the connection root node.
- Current order on connections:
  - `Connect`
  - `Disconnect`
  - `Refresh`
  - separator
  - `New Connection`
  - `Edit`
  - `Delete`
  - separator
  - `New Pool`
  - separator
  - `Install helper commands`
  - `Reinstall/Update daemon`
  - `Repair temporary mountpoints`
  - `Export trust-store to this connection`
  - `Authorize SSH key on...` (submenu with the other connected SSH connections)
  - separator
  - `Split and root` (submenu: `Right`, `Left`, `Down`, `Up`), or `Close` when the menu is opened on the root of a split panel

Enablement conditions:

- `Connect`: connection marked as disconnected and with no action in progress.
- `Disconnect` and `Refresh`: connection connected and with no action in progress.
- `Edit` and `Delete`: not available on the Local connection nor on connections redirected to Local.
- `New Pool`: connection connected.
- `Install helper commands`: only when the refresh detected a package manager and a supported installation plan for the missing commands.
- `Reinstall/Update daemon`: any remote connection, Windows included.
- `Repair temporary mountpoints`: any non-Windows connection, including Local (a local dataset can also be left on a temporary mountpoint).
- `Export trust-store to this connection`: any remote connection; it does not apply to Local, which already uses the local trust-store.
- `Authorize SSH key on...`: only on non-Windows SSH connections, and it is only populated with other connected SSH connections.

`Repair temporary mountpoints` first makes a read-only pass, shows the datasets left with a relocated mountpoint by an interrupted sync, and asks for confirmation before restoring them (unmounting them first). Those that fail keep their marker and can be retried.

## Automatic daemon update

When a refresh finishes, ZFSMgr reinstalls the daemon automatically and without dialogs **only** when the reason for attention is a version or API mismatch.

A daemon-rpc TLS backoff marks the connection as needing attention but does **not** trigger the automatic reinstall: reinstalling would regenerate the TLS material and perpetuate the failure → reinstall → failure loop. In that case use `Reinstall/Update daemon` or `Export trust-store to this connection` manually.

## On the merged pool root node

![Imported pool context menu](qrc:/help/img/auto/pool-context-menu-imported.png)

- The first submenu is `Pool`.
- Inside `Pool` are the pool actions:
  - `Update status`
  - `Import`
  - `Import renaming`
  - `Export`
  - `History`
  - `Management`
- `Management` runs immediate actions (`Sync`, `Scrub`, `Upgrade`, `Reguid`, `Trim`, `Initialize`, `Clear`, `Destroy`) with a parameter dialog where applicable.
- Right after the `Pool` submenu comes `Split and root`, and then the normal dataset actions on that same dual node.

## On datasets and snapshots

- On a filesystem dataset (and on the merged pool node):
  - `Manage property display`
  - `Dataset`
  - `Actions`
  - `Mark as source`
  - `With source …` (the six two-endpoint actions; see below)
  - `Split and root` (submenu: `Right`, `Left`, `Down`, `Up`)
- `Dataset` submenu:
  - `Create`
  - `Rename`
  - `Delete`
  - `Mount`: only when the dataset has `canmount` other than `off`, a valid
    `mountpoint`, and is **not** already mounted.
  - `Unmount`: only when it **is** mounted. `canmount` is not required: a dataset can be
    mounted and later have `canmount=off`, and that is exactly when you need to unmount it.
  - `Encryption Key` (`Load Key`, `Unload Key`, `Change Key`)
  - `Schedule snapshots`
  - `Permissions` (`New Set`, `New Delegation`)
- `Actions` submenu (operations on the DATA, not on dataset state):
  - `Breakdown`
  - `Assemble`
  - `From Dir`
  - `To Dir`
- On snapshots:
  - `Manage property display`
  - `Delete snapshot`
  - `Rollback`
  - `New Hold`
  - `Mark as source`
  - `With source …`

## The six source-and-target actions

`Copy`, `Move`, `Clone`, `Sync`, `Level` and `Diff` need **two** endpoints. They no longer
have buttons: you request them from the context menu, following the copy-and-paste model.

1. Right-click the starting dataset or snapshot → `Mark as source`.
2. Right-click **any other node**: that node is the target, and the `With source <name>`
   submenu offers the six, naming the source in each one:

```
With source datos@lunes ▸
   Copy here from datos@lunes
   Move here from datos@lunes
   Clone here from datos@lunes
   Sync here from datos@lunes
   Level with datos@lunes
   Compare with datos@lunes
```

**`Move` copies nothing.** It is a `zfs rename`: the dataset changes place in the tree
and the data stays where it is, so it is instantaneous and there is no original left to
delete. That is why it only works **within the same pool and the same machine**, with
datasets on both ends — never snapshots. To take something to another pool or another
machine, use `Copy`. What does change is the mountpoint of that dataset and of everything
beneath it.

There is no target to mark: it is the node you click on. The `Source:` line at the top
remembers what is marked, and its tooltip shows it in full when the name does not fit.

Whatever does not apply appears **greyed out, with the reason in its tooltip**: the source
is not a snapshot, the pools do not match, `Diff` compares two points of the same dataset,
or the OpenZFS versions are not compatible for transfers.
- On holds:
  - `Release`

On the merged pool node, `Split and root` appears right after the `Pool` submenu, not at the end.

## On the root node of a split panel

- If the node is the root of a split panel, this option appears:
  - `Close`: closes that panel and frees its space in the splitter.

## Rules

- Destructive actions ask for confirmation.
- **Properties**, **permissions** and **renames** are edited as drafts and applied with
  `Apply changes`. Actions are not: they run when you press them.
- `Mark as source` updates the `Source:` line in the top band. The target is not marked: it is the node whose menu you opened.
- The `@` node (snapshot grouper) has no context menu.
- On `Dataset properties` and `Snapshot properties` the context menu contains only `Manage property display`.
- Children of the connection root that are not pool roots (`Properties`, `Info`) have no context menu.
- On suspended pools, most context menu actions appear disabled.
