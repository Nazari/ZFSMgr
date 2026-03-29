# 快速手册

ZFSMgr 现在通过一个统一树管理连接和 ZFS 操作。

## 总览

![主窗口](qrc:/help/img/auto/main-window.png)

- 左侧：
- `Selected datasets`：显示当前标记为 `Source` 和 `Target` 的数据集。
- `Status and progress`：显示当前状态、加载和进度。
- 右侧：
- 一个统一树，结构为：
  - 连接
  - 连接下的池
  - 池下的数据集和快照
- 树下方：
  - `Pending changes`
- 底部区域：
  - 日志

## 统一树

- 连接始终显示为根节点，即使已断开。
- 如果连接断开：
  - 连接节点仍然可见
  - 该连接下的池会从树中消失
- 连接节点保留原来连接表中的颜色和 tooltip 语义。
- 如果连接需要 GSA 注意，名称会显示 `(*)`。
- 池不再显示为 `Connection::Pool`，可见文本只显示池名。
- 池根节点与池根数据集合并：
  - 保留池图标和池 tooltip
  - 也作为根数据集使用
  - 其真实子节点直接挂在其下
- 已导入的池可以显示：
  - `Pool Information`
  - `Datasets programados`

## 内联节点

- 数据集和快照可以显示 `Dataset properties`。
- 非快照数据集还可以显示 `Permissions`。
- filesystem 数据集可以显示 `Schedule snapshots`。

![计划快照节点](qrc:/help/img/auto/schedule-snapshots-node.png)

## Source / Target 选择

- 不再有连接表中的 `Source/Target` 复选框。
- 现在通过数据集右键菜单选择：
  - `Select as source`
  - `Select as destination`
- `Selected datasets` 区域反映这个逻辑选择。

## 上下文菜单

- 在连接根节点上：
  - 可打开原来的连接菜单
- 在合并后的池根节点上：
  - 首先出现 `Pool` 子菜单
  - 之后是数据集菜单项
- `Pool` 子菜单包含：
  - `Refresh`
  - `Import`
  - `Import with rename`
  - `Export`
  - `History`
  - `Management`
  - `Show Pool Information`
  - `Show Scheduled Datasets`

## 待执行变更

- `Pending changes` 显示可读描述，而不是原始命令。
- 变更按加入顺序累积。
- 点击某一行时，ZFSMgr 会尝试定位到相关对象和相关区段。

## 连通性和日志

- `Check connectivity` 不再是浮动按钮。
- 它现在位于主应用菜单中。
- `Combined log` 仍然显示应用和连接输出。

## 创建池

![创建池](qrc:/help/img/crearpool.png)

## 创建数据集

![创建数据集](qrc:/help/img/creardataset.png)

## 导航

- 树会保留展开状态、当前选择和所选快照。
- 修改属性列数时，已打开节点会保持展开。
- 点击空的 `Dataset properties` 节点时，会即时生成其子节点并保持展开。
