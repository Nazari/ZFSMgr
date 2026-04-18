# 内联属性与列

ZFSMgr 会在统一树中直接显示数据集和池属性。

## 出现位置

- 数据集下显示 `Dataset properties`。
- 快照下显示 `Snapshot properties`。
- 非快照数据集还可以显示 `Permissions`。
- 合并后的池根节点可以显示 `Pool Information`。
  - 其中可包含 `Devices`（池的 vdev/磁盘层级）。
- filesystem 数据集的快照位于 `@` 节点下。
- 含有活动 GSA 数据集的池可显示 `Scheduled datasets`。
- 连接节点下可显示 `Connection properties`。
- 连接节点下的 `Info` 分组为：
  - `General`
  - `GSA`
  - `Commands`

## 显示

- 可见属性列数从树表头的上下文菜单调整。
- 当前可选列数为：
  - `4, 6, 8, 10, 12, 14, 16`
- 列宽会保存并恢复。

## 可见属性管理

右键：

- `Dataset properties`
- `Snapshot properties`
- `Pool Information`
- `Connection properties`

可打开可见属性管理窗口。

该窗口允许：

- 选择可见属性
- 拖放重排
- 创建分组
- 重命名分组
- 删除分组

分组按以下对象独立保存：

- pool
- dataset
- snapshot
- connection

## 内联编辑

- 可编辑属性可直接在树中修改。
- 可继承属性在适用时显示 `Inh.`。
- ZFS 权限也可在树中编辑，但使用草稿模式。
- 点击 `Pending changes` 的某一行时，ZFSMgr 会尝试聚焦对应对象与区段。
