# 快速手册

ZFSMgr 通过统一树管理连接和 ZFS 操作。

## 总览

![主窗口](qrc:/help/img/auto/main-window.png)

- 上半区：一个占满宽度的统一树。
- 中间区域：
  - `Status` 与 `Progress` 一行
  - `Actions` 框（同一行显示 `Source` 与 `Target`）
  - `Pending changes` 在 `Actions` 右侧
- 底部区域：日志分页（`Settings`、`Combined log`、`Terminal`、`Daemon`、`Transferencias`）。

## 统一树

- 树参考图：

![统一树](qrc:/help/img/auto/top-tree.png)

- 连接始终显示为根节点，即使已断开。
- 如果连接断开：
  - 连接节点仍然可见
  - 不显示任何子节点（包括辅助节点）
- 连接名会显示当前模式：
  - 远端 daemon 可用时显示 `(libzfs_core)`
  - 回退时显示 `(ssh)`
- 如果连接需要 daemon 注意，名称会显示 `(*)`。
- `Connection` 与 `Pool` 节点使用粗体并带类型前缀。
- 池根节点与池根数据集合并：
  - 保留池图标
  - 也作为根数据集使用
  - 避免出现重复的 `pool/pool`
- 已导入的池可以显示：
  - `Pool Information`
    - 其中包含 `Devices`（按 `zpool status -P` 生成的 vdev/磁盘层级）
  - `Scheduled datasets`
- 处于挂起（suspended）状态的池，名称旁会显示 `(Suspended)`，大部分操作被禁用。

## 内联节点

- 数据集显示 `Dataset properties`。
- 快照显示 `Snapshot properties`。
- 非快照数据集还可以显示 `Permissions`。
- 有快照的数据集会显示 `@` 节点，用于分组手动/GSA 快照。
- 连接节点下可显示辅助节点：
  - `Connection properties`（内联显示，按连接类型控制可编辑字段）
  - `Info`
    - `General`（状态与连接元数据）
    - `Daemon`
    - `Commands`

- 内联属性可直接在树中编辑。
- 若属性支持继承，会显示 `Inh.`，并在应用前保持草稿状态。
- `Permissions` 也使用草稿模式。
- `Scheduled datasets` 使用 `org.fc16.gsa:*` 属性。

## Source / Target 选择

- 不再有连接表中的 `Source/Target` 复选框。
- 现在通过数据集右键菜单选择：
  - `Select as source`
  - `Select as destination`
- `Actions` 中的 `Source/Target` 行反映该逻辑选择。
- 树中的当前视觉选中与逻辑 `Source/Target` 选择彼此独立。

## 上下文菜单

- 在连接根节点上：
  - 可打开原连接上下文菜单
- 在合并后的池根节点上：
  - 首先出现 `Pool` 子菜单
  - 然后是数据集操作菜单项
- `Pool` 子菜单包含：
  - `Refresh status`
  - `Import`
  - `Import with rename`
  - `Export`
  - `History`
  - `Management`：
    - `Sync`
    - `Scrub`
    - `Upgrade`
    - `Reguid`
    - `Trim`
    - `Initialize`
    - `Clear`
    - `Destroy`
- 数据集/快照常见菜单项：
  - `Create dataset/snapshot/vol`
  - `Rename`
  - `Delete`
  - `Encryption`
  - `Schedule automatic snapshots`
  - `Rollback`
  - `New Hold`
  - `Release`
  - `Break down`
  - `Assemble`
  - `From Dir`
  - `To Dir`

## 待执行变更

- `Pending changes` 显示可读描述，而不是原始命令。
- 变更按加入顺序累积。
- 点击某一行时，ZFSMgr 会尝试定位到相关对象和相关区段。
- 常见延迟执行操作：
  - 属性修改
  - 权限修改
  - `Rename`、`Move`、`Rollback`、`Hold`、`Release`
  - `Copy`、`Level`、`Sync`
  - 数据集/快照删除

## 连通性和日志

- `Check connectivity` 位于主菜单（不在 `Logs` 下）。
- 顶部 `Logs` 菜单已移除。
- `Settings` 标签页集中以下选项：
  - 日志级别
  - 行数
  - 最大轮转日志大小
  - 执行动作前确认
  - 清空/复制日志

## 创建池

![创建池](qrc:/help/img/crearpool.png)

## 创建数据集

![创建数据集](qrc:/help/img/creardataset.png)

## 分屏面板（Split and root）

- 连接、池或数据集节点的上下文菜单中包含 `Split and root`。
- 选择方向（`向右`、`向左`、`向下`、`向上`）后，会通过分隔器在当前面板旁边打开一个新树面板。
- 面板根节点显示完整路径（例如 `mbp::tank1/ds1/sub`）。
- 分屏面板功能完整：支持相同的上下文菜单、内联属性和可配置列。
- 面板可以嵌套；每个面板有独立的列标题上下文菜单。
- 关闭分屏面板：右键点击其根节点 → `Close`。
- 面板布局在会话之间保留。

## 导航

- 树会保留展开状态、当前选择和所选快照。
- 修改属性列数时，已打开节点会保持展开。
- 点击空属性节点时，会即时生成其子节点并保持展开。
