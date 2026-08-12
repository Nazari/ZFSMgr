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
  - `New pool`
  - 分隔线
  - `Split and root`（子菜单：`向右`、`向左`、`向下`、`向上`）
  - 分隔线
  - `Install helper commands`

`Daemon` 子菜单包括：
- `Install/update daemon`：daemon 缺失、过旧或需要重新缓存 TLS 时可用。
- `Daemon updated and running`：没有待处理动作时禁用。
- `Uninstall daemon`。

如果检测到 daemon-rpc TLS backoff，ZFSMgr 会将连接标记为需要注意，并在刷新结束后自动尝试更新/重新缓存 TLS。

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
  - `管理属性显示`
  - `Dataset`
  - `Actions`
  - `Split and root`（子菜单：`向右`、`向左`、`向下`、`向上`）
  - `标记为源`
  - `以 … 为源`（六个双端操作，见下文）
- `Dataset` 子菜单：
  - `创建`
  - `重命名`
  - `删除`
  - `挂载`：仅当数据集的 `canmount` 不是 `off`、`mountpoint` 有效，且**尚未**挂载时可用。
  - `卸载`：仅当**已**挂载时可用。不要求 `canmount`：数据集可能先挂载、之后才被设为
    `canmount=off`，而那正是需要卸载它的时候。
  - `加密密钥`（`加载密钥`、`卸载密钥`、`更改密钥`）
  - `计划快照`
  - `权限`（`新建权限集`、`新建委派`）
- `Actions` 子菜单（针对**数据**的操作，而非数据集状态）：
  - `拆分`
  - `组装`
  - `从目录`
  - `到目录`
- 在 snapshot 上：
  - `管理属性显示`
  - `删除快照`
  - `Rollback`
  - `New Hold`
  - `标记为源`
  - `以 … 为源`

## 六个「源与目标」操作

`复制`、`移动`、`克隆`、`同步文件`、`同步快照` 和 `Diff` 需要**两个**端点。它们不再有按钮：
改为通过右键菜单发起，采用复制／粘贴的模式。

1. 在起始的数据集或快照上点右键 →`标记为源`。
2. 在**任意其他节点**上点右键：该节点即为目标，子菜单 `以 <名称> 为源` 会列出这六项，
   并在每一项中写明源：

```
以 datos@lunes 为源 ▸
   从 datos@lunes 复制到此
   从 datos@lunes 移动到此
   从 datos@lunes 克隆到此
   从 datos@lunes 同步到此
   与 datos@lunes 同步快照
   与 datos@lunes 比较
```

无需标记目标：目标就是你点击的节点。顶部的 `源：` 一行会记住当前标记的内容，名称放不下时
可在其提示中看到完整值。

不适用的项会**变灰，并在提示中给出原因**：源不是快照、存储池不一致、`Diff` 比较的是同一个
数据集的两个时间点，或两端的 OpenZFS 版本不兼容传输。
- 在 hold 节点上：
  - `Release`

## 在分屏面板的根节点上

- 如果该节点是分屏面板的根节点，会额外显示：
  - `Close`：关闭该面板并释放分隔器中的空间。

## 规则

- 破坏性操作会要求确认。
- 多个操作使用延迟模式并累积到 `Pending changes`。
- `标记为源` 会更新顶部区域的 `源：` 一行。目标无需标记：就是你打开菜单所在的节点。
- `Dataset properties`、`Snapshot properties` 和 `@` 节点没有上下文菜单。
- 已挂起（suspended）的池，上下文菜单中大多数操作处于禁用状态。
