# 上下文菜单

ZFSMgr 在统一树上使用上下文菜单。

## 在连接节点上

![连接菜单](qrc:/help/img/auto/connection-context-menu.png)

- 原来属于连接表的菜单现在挂在连接根节点上。
- 当前顺序：
  - `Connect`
  - `Disconnect`
  - `Refresh`
  - 分隔线
  - `New connection`
  - `Edit`
  - `Delete`
  - 分隔线
  - `GSA`
  - 分隔线
  - `New pool`
  - 分隔线
  - `Install MSYS2`
  - `Install helper commands`

## 在合并后的池根节点上

![已导入池菜单](qrc:/help/img/auto/pool-context-menu-imported.png)

- 第一个子菜单是 `Pool`。
- `Pool` 中包含池操作：
  - `Refresh status`
  - `Import`
  - `Import with rename`
  - `Export`
  - `History`
  - `Management`
- `Management` 中的动作（`sync`、`scrub`、`upgrade`、`reguid`、`trim`、`initialize`、`clear`、`destroy`）为即时执行；有参数时会先弹出参数窗口。
- 在 `Pool` 子菜单之后，是该合并节点的普通数据集菜单项。

## 在数据集和快照上

- 在 filesystem 数据集（以及合并池节点）上：
  - `Manage properties`
  - `Dataset`
  - `Actions`
  - `Select as source`
  - `Select as destination`
- `Dataset` 子菜单：
  - `Create`
  - `Rename`
  - `Delete`
  - `Encryption key`（`Load key`、`Unload key`、`Change key`）
  - `Schedule snapshots`
  - `Permissions`（`New set`、`New delegation`）
- `Actions` 子菜单：
  - `Break down`
  - `Assemble`
  - `From Dir`
  - `To Dir`
- 在 snapshot 上：
  - `Manage properties`
  - `Delete snapshot`
  - `Rollback`
  - `New Hold`
  - `Select as source`
- 在 hold 节点上：
  - `Release`

## 规则

- 破坏性操作会要求确认。
- 多个操作使用延迟模式并累积到 `Pending changes`。
- `Select as source` / `Select as destination` 会更新 `Actions` 里的 `Source/Target` 行。
- `Dataset properties`、`Snapshot properties` 和 `@` 节点没有上下文菜单。
