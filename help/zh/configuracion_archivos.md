# 配置与文件

ZFSMgr 为每个用户、每个操作系统使用一个配置目录：

- Linux：`$HOME/.config/ZFSMgr`
- macOS：`$HOME/.config/ZFSMgr`
- Windows：`%USERPROFILE%/.config/ZFSMgr`

## 文件结构

- `config.json`：应用程序的全局配置与连接定义。敏感字段（用户名与密码）以主密码加密保存。
- `trust-store.json`：守护进程的 TLS 材料（服务器证书，以及客户端证书与密钥），同样以主密码
  加密。`导出 trust-store 到该连接` 复制的就是这个文件。
- `application.log`：应用程序的持久日志。

实际示例：

```text
~/.config/ZFSMgr/
  config.json
  trust-store.json
  application.log
```

## `config.json` 中保存了什么

根对象包含三个区块：

- `connections`：数组，包含每个连接的完整定义（id、名称、machine_uid、类型、操作系统、
  主机、端口、SSH 地址族、用户名、密码、密钥路径以及是否使用 sudo）。用户名与密码为加密存储。
- `app`：应用程序选项与界面状态。
- `ZPoolCreationDefaults`：创建池时的默认值。

`app` 中保存的内容包括：

- 界面语言
- 日志选项（最大大小、级别、行数）
- 是否确认操作
- 属性列数（`conn_prop_columns`）
- 上部面板中显示的连接
- 被标记为已断开的连接
- 窗口几何信息与分隔条状态
- 内联区段的可见性
- 内联属性的顺序与分组

## 哪些内容不会保存

- `源` 标记属于本次会话：关闭应用后即丢失。没有需要保存的目标：目标就是发起操作时所在的节点。
- **待应用更改列表**会保存（键 `pending_actions`），其中含每个操作已构造好的命令，但**不含
  密码**：保存时以占位符替换，加载时从连接中重新取回。
- 树的列宽在同一会话内切换连接或面板时会保留，但不会跨越程序的两次启动。

## 启动时的加载流程

启动时，ZFSMgr 会：

1. 如存在旧版配置，先进行迁移（见下）。
2. 读取 `config.json`。
3. 从 `connections` 数组加载连接。
4. 把仍保存在 `config.json` 中的 TLS 材料移入 `trust-store.json`。

## 从旧格式迁移

若存在旧的 `config.ini`、`connections.ini` 或 `conn*.ini` 文件，ZFSMgr 会自动把它们合并进
`config.json`——`connection:<id>` 分组变成 `connections` 数组中的条目——然后删除它们。
INI 格式已不再使用。
