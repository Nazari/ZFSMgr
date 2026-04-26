# 应用日志

窗口底部使用多个标签页：

- `Settings`：日志参数与执行确认选项。
- `Combined log`：主应用日志。
- `Terminal`：本地/远端命令技术输出。
- `Daemon`：远端 daemon 日志（`/var/lib/zfsmgr/daemon.log`）及 `Heartbeat` 按钮。
- `Transferencias`：后台传输 job 列表（Copy/Level daemon 间传输）。

## Daemon 标签页

- 以增量方式读取并显示远端 daemon 的日志 `/var/lib/zfsmgr/daemon.log`。
- `Heartbeat` 按钮向 daemon 发送心跳，确认其正常响应。
- 检测到 ZED 事件或点击 `Heartbeat` 时日志会刷新。
- 刷新连接时日志不会被清空；仅在重新安装 daemon 后重置。

## Transferencias 标签页

- 每个后台传输 job 显示为一行（daemon 间 Copy 或 Level）。
- 每行包含：状态、源/目标数据集、已传输字节数、速率、耗时。
- 可能的状态：`running`、`done`、`failed`、`cancelled`。
- `Refrescar` 强制立即向 daemon 查询状态。
- `Cancelar seleccionado` 向所选 job 的 `zfs send` 进程发送 `SIGTERM`。
- 重新连接时正在运行的 job 会自动恢复。

`Combined log`：

- 包含应用内部事件。
- 以紧凑格式显示关键执行输出。

## 启动时初始加载

ZFSMgr 启动时：

- 读取持久化日志（`application.log` 及轮转文件 `.1` ... `.5`）。
- 仅加载最后 `N` 行到界面。
- `N` 取自 `Settings` 中配置的最大显示行数。
- 日志不存在或为空时不会报错。

## 屏幕紧凑显示

每条新日志会与前一条可见日志比较。  
界面仅显示以下字段的变化：

- 日期
- 时间
- 连接
- 日志级别

若这些字段均未变化，头部显示 `...`。

显示格式：

- `<变化字段> | <消息>`

## 持久化

- 磁盘中仍保存完整格式日志，便于追踪。
- 紧凑显示仅用于界面展示。
