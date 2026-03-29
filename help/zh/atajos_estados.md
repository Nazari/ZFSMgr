# 导航与状态

- 在操作和刷新期间，光标会变为忙碌状态。
- 统一树现在是主要导航界面。
- 当前树选择不等同于逻辑上的 `Source` 和 `Target`。
- `Source` 和 `Target` 通过数据集上下文菜单显式设置。
- `Selected datasets` 区域反映该逻辑选择。
- 如果连接断开：
  - 连接根节点仍然可见
  - 其池会消失
- 如果源或目标使用 OpenZFS `< 2.3.3`，`Copy`、`Level` 和 `Sync` 会被阻止。
- 只有 `Pending changes` 中存在真实待执行变更时，`Apply changes` 才会启用。
