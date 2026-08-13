# Windows 连接

在 Windows 上，ZFSMgr **只通过原生代理工作**。它不会在远程主机上执行 shell 命令，
也不需要在那里安装任何 Unix 命令层。

## Windows 主机需要什么

- **运行中的 OpenSSH Server。** 这是唯一受支持的传输方式。Windows 10 和 11 自带，
  但**默认关闭**。

  如果在同一台主机上安装 ZFSMgr，安装程序会主动为您启用它：*启用 OpenSSH 服务器*
  这一项默认勾选，并把执行结果记录到 `%TEMP%\zfsmgr-openssh.log`。若该主机无法访问
  互联网，启用可能无法完成；安装程序**不会一直等待**，仍会正常结束，并在该日志中
  说明情况。

  手动操作：

  ```powershell
  Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
  Start-Service sshd
  Set-Service -Name sshd -StartupType 'Automatic'
  New-NetFirewallRule -Name sshd -DisplayName 'OpenSSH Server (sshd)' `
      -Enabled True -Direction Inbound -Protocol TCP -Action Allow -LocalPort 22
  ```

  防火墙规则很重要：没有它，服务已经启动却仍然从外部连不上，这是三者中最令人费解
  的症状。

- **OpenZFS on Windows**，它提供 `zfs` 和 `zpool`。安装程序会检查它；若未找到，
  会提供打开下载页面的选项。
- **ZFSMgr 代理**，可从应用自身安装：在连接的右键菜单中选择*重新安装／更新守护进程*。

不再需要 MSYS2、MinGW 或其他 Unix 工具链。早期版本确实需要，用于安装它们的菜单项
已经移除。

## 通信方式

与 Linux、macOS 和 FreeBSD 完全相同：命令以**类型化调用**的形式发送给代理，经过双向
认证与加密，通过在 SSH 连接之上建立的隧道传输。代理直接执行这些调用，中间不经过任何
命令解释器。

正因如此，传输方式必须是 SSH：没有它，就没有承载这些调用的隧道。

## 尚不可用的功能

Windows 代理尚未实现部分功能。应用**不会去尝试**它们：这些项会显示为禁用并给出原因，
连接卡片也会在*不可用功能*中列出。

- 后台作业，以及依赖它的守护进程之间的直接传输。
- 任一端为 Windows 时的快照`复制`与`同步快照`。
- 使用 `rsync` 的`同步文件`。
- *到目录*。
- 修复临时挂载点。
- 计划自动快照。

可以正常使用的部分：读取和修改数据集与存储池、快照、克隆、ZFS 权限、*拆分*与*组装*，
以及代理的日志和心跳。

该列表并非写死在应用中：**由代理在被询问状态时自行声明**，因此只要安装了覆盖更多功能的
版本，列表就会自动更新。

## 值得注意的差异

- **挂载点。** 在 Linux 上创建的池会保留 Unix 风格的路径（`/mnt/data`），在 Windows 上
  并不对应任何盘符。这是池中真实的数据，并非读取错误。
- **挂载点是一个盘符，而不是路径。** 它保存在 `driveletter` 属性中；`mountpoint`
  显示为 `-`，这是正常的。ZFSMgr 查询真实的挂载列表，而不是从该属性推断路径。
  大小写没有区别（`z:` 与 `Z:` 表现一致，已验证）。
- **在数据集已挂载时修改 `driveletter` 会先卸载再重新挂载。** 原盘符上打开的内容
  会中断。这不是故障，但在运行中修改之前值得了解。
- **没有 `sudo`。** 命令以当前会话的权限运行，而代理作为系统服务运行。
- **使用 `-N` 导入的池**保持未挂载状态，因此挂载列表为空是正常的。

## 在 Windows 上创建池

对于目前可用的 OpenZFS on Windows 预览版（`zfswin-2.4.1rc…`），**创建池可能会因为
ZFSMgr 之外的原因而失败**。这一点已通过在应用之外手动执行以下命令得到验证：

```powershell
zpool create probepool \\.\PhysicalDrive2
```

在一整块空闲磁盘上它会返回 `invalid argument for this pool operation`。在这个问题
解决之前，可行的做法是**在 Linux 或 FreeBSD 主机上创建池，然后在 Windows 上导入**；
使用虚拟磁盘（VHDX）会很方便。导入、读取、挂载以及对池的日常操作都是正常的。

## 把快照传到 Windows

当任一端为 Windows 时，复制或同步快照会显示为禁用，因为它需要通过管道串联
`zfs send | zfs recv`，而代理尚未实现该功能。有一条**确实可行**的手动路径，
值得了解为什么只能这样做：

```bash
# 在源主机上（Linux、macOS、FreeBSD）
zfs send tank/data@snapshot > stream.zfs
```

把文件传到 Windows 主机（用共享文件夹即可），然后在**`cmd.exe` 中，而不是
PowerShell 中**执行：

```
zfs recv -F winpool/data < C:\路径\stream.zfs
```

两个细节，均已在真实机器上验证：

- **通过文件可行，通过管道不行。** 同一个数据流经 SSH 直接送入 `zfs recv` 会失败并
  报 `cannot receive new filesystem stream: I/O error`，而从文件接收则完整无误、
  校验和一致。问题出在 Windows 上读取标准输入，而不是数据流本身。
- **`<` 重定向属于 `cmd.exe`。** PowerShell 没有这个语法，而且它在传输约 132 KB
  二进制数据之后就会卡住。

## 如果没有响应

连接卡片会显示代理是否已安装、是否正在运行、其 API 版本，以及该二进制是否为原生版本。
**Daemon** 分页可查看其日志，并可请求一次心跳。若 API 版本与应用期望的不一致，请从右键
菜单重新安装代理。
