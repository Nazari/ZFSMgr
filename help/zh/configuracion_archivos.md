# 配置与 INI 文件

ZFSMgr 在每个系统中使用按用户区分的配置目录：

- Linux：`$HOME/.config/ZFSMgr`
- macOS：`$HOME/.config/ZFSMgr`
- Windows：`%USERPROFILE%/.config/ZFSMgr`

## 文件结构

- `config.ini`：应用全局配置。
- `conn*.ini`：每个连接一个文件（例如 `conn_fc16.ini`、`conn_surface_psrp.ini`）。

实际示例：

```text
~/.config/ZFSMgr/
  config.ini
  conn_fc16.ini
  conn_surface_psrp.ini
  conn_mbp_local.ini
```

## 各文件内容

- `config.ini`：
  - 界面语言
  - 全局日志选项
  - 默认参数（例如 `[ZPoolCreationDefaults]`）
- `conn*.ini`：
  - 单个连接的完整定义（主机、端口、用户、密钥等）

## 启动加载流程

ZFSMgr 启动时会：

1. 读取 `config.ini`。
2. 在配置目录中查找所有 `conn*.ini`。
3. 加载每个连接文件。

若检测到旧格式（连接写在 `config.ini` 内），ZFSMgr 会自动迁移到 `conn*.ini`。
