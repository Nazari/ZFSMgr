# 配置与 INI 文件

ZFSMgr 在每个系统中使用按用户区分的配置目录：

- Linux：`$HOME/.config/ZFSMgr`
- macOS：`$HOME/.config/ZFSMgr`
- Windows：`%USERPROFILE%/.config/ZFSMgr`

## 文件结构

- `config.ini`：应用全局配置。

实际示例：

```text
~/.config/ZFSMgr/
  config.ini
```

## `config.ini` 中保存的内容

- 界面语言
- 全局日志选项
- 属性列数量（`conn_prop_columns`）
- 当前 source/target 连接与数据集
- splitter 状态与窗口几何信息
- 统一树列宽
- 内联属性顺序与分组
- 默认参数（例如 `[ZPoolCreationDefaults]`）
- 各连接完整定义（`connection:<id>` 分组）

## 启动加载流程

ZFSMgr 启动时会：

1. 读取 `config.ini`。
2. 从 `connection:<id>` 分组加载连接。

若发现旧的 `conn*.ini` 文件，ZFSMgr 会自动迁移到 `config.ini`，然后删除旧文件。
