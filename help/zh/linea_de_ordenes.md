# 命令行（`zfsmgr-cli`）

ZFSMgr 附带一个终端工具，它所做的事与图形窗口相同，并且面向**同一批连接**：从同一位置
读取 `config.json` 与 `trust-store.json`，通过同一条加密隧道通信，执行相同的代理动词。
它不是一个拥有自己配置的独立程序。

在 Windows 上，安装程序会把它加入 `PATH`（只要保留相应的勾选项），因此可以在 `cmd` 或
PowerShell 中直接按名称调用。在 Linux 与 macOS 上，它**会随应用一起安装**到 `bin`，
因此同样可以按名称调用。以前不是这样，它没有出现在任何 Unix 软件包中。

此外还有一个 **Web 服务器** `zfsmgr-web`：在浏览器中展示同样的内容，不使用 JavaScript，
通过 SSH 隧道访问。三者——窗口、命令行和服务器——都与同一个代理通信，共享同样的规则。

## 两种用法

**带命令**，用于脚本：

```sh
zfsmgr-cli connections list
zfsmgr-cli --format json connections list | jq '.connections[] | select(.tls == false)'
```

**不带任何命令**，此时它表现为一个交互式解释器：存在一个“位置”——一个 `zfsm://` URL——
所输入的一切都作用于该位置。

```text
zfsm://local> cd oldlau/winpool
zfsm://oldlau/winpool> ls
zfsm://oldlau/winpool> cd sa@yesterday
zfsm://oldlau/winpool/sa@yesterday> ls #content
```

`help` 列出所有命令，`help <命令>` 解释其中一个。Tab 键补全命令与 URL，方向键浏览历史。

## 位置就是一个 URL

与应用程序使用的完全相同：`zfsm://连接/池/数据集@快照#区段`。区段为
`#content[/路径]`（其中的文件）、`#properties[/属性]` 与 `#permissions`，与 URL 的其余
部分一样**使用英文**。

由此带来一个实际好处：**任何命令都可以从历史中复制出来单独执行**，只需加上
`--on <url>`（或 `--from`，二者等价）。需要源与目标的命令，若未另行指定，则以当前位置
作为源。

两条规则消除相对路径的歧义：

- 若第一段是某个**连接**的名称，则该路径为绝对路径。在机器之间跳转时就是这样写的：
  在 `zfsm://local` 下输入 `cd unibody`。
- 若第一段是**当前所在的池**，则按 ZFS 全名理解，而不是子节点。位于 `tank/source` 时，
  `destroy tank/clone` 指向 `tank/clone`，而不是 `tank/source/tank/clone`。

若要进入一个与连接同名的子节点，请使用 `./名称`。

## 三种输出格式

- `text`（默认）用于阅读：对齐的列、翻译过的表头、易读的容量，布尔值显示为 `是`/`否`。
- `tsv` 用于脚本：无表头、制表符分隔、列固定且**为英文**，无值处为 `-`，与
  `zfs list -H` 一致。
- `json` 用于程序：数字即数字，布尔即布尔，不适用者为 `null`。

**在 `tsv` 与 `json` 中，字段名始终为英文，不随语言变化。** 这是刻意的：不应因为有人
切换了界面语言，脚本就不能用了。

## 语言

`--lang es|en|zh`。不指定时，采用图形界面所用的语言（`config.json` 中的
`app.language`），以便两个工具口径一致。

会被翻译的是：消息、帮助文本，以及 `text` 格式下的表头。**不会**被翻译的（这并非疏漏）：
所输入的动词、`tsv` 与 `json` 的字段名，以及 URL 中的字面量。

## 密码

绝不通过参数或环境变量传递：这两者在 `ps` 中对本机任何用户都可见。只能通过终端或文件
描述符传入，因而可以配合任意密码管理器使用：

```sh
zfsmgr-cli --password-fd 3 connections list  3< <(pass show zfsmgr)
```

使用 `--no-secrets` 时不解密任何内容，也不会索取主密码：加密字段显示为 `<cifrado>`。
适合在手边没有密钥时清点资产。该选项下**不会写入配置**：保存一个加密字段为空的配置，
会把这些字段在文件中置空，从而抹掉已保存的密码。

## 破坏性操作

每一项都会先请求确认，并列出将被清除的内容。`-y` 表示全部确认；若没有终端又未指定
`-y`，命令会拒绝继续，而不是擅自当作同意。

## 出问题时

`-v` 会在标准错误上说明传输层对每台机器做了什么：发送了哪条命令、走的是守护进程隧道
（`[daemon-rpc]`）还是 SSH，以及失败时的原因。该日志中的密码与 TLS 材料均已遮蔽。

请求的结果输出到**标准输出**，日志输出到**标准错误**，因此
`zfsmgr-cli ... > data.tsv` 不会把两者混在一起。
