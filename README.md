# ZFSMgr: OpenZFS manager for the command line, local and remote

**ZFSMgr** manages **OpenZFS** pools, datasets, snapshots and transfers on **Linux, macOS,
FreeBSD and Windows**, on the machine you are sitting at and on any other you can reach over
SSH, from a single command line tool: **`zfsmgr-cli`**.

## Version 1.x: the command line

**ZFSMgr 1.x is the command line client (`zfsmgr-cli`) and its daemon (`zfsmgr-agent`).**
That is what is published, tested and supported in this release.

The graphical interface that ZFSMgr started with is **not part of 1.x**. It has not kept up
with the work of the last months and has not been tested against this version, so it is not
published. It will return in **version 2.0.0**, built on the same base the CLI uses today.

## Beta notice and legal disclaimer

This software is provided **"AS IS"**. ZFS administration can destroy data: commands that
modify ask for confirmation, but they do what you tell them.

- It may contain defects, regressions, data-loss scenarios, or incomplete behaviours.
- Use it at your own risk, especially on production systems or critical data.
- The author (Eladio Linares) provides **no warranty** and assumes **no liability** for direct
  or indirect damage, data loss, service interruption, or any other consequence of its use.

Legal references: **GNU GPL v3**, Section 15 (*Disclaimer of Warranty*) and Section 16
(*Limitation of Liability*).

## Installing

Download `zfsmgr-cli-1.2.1.zip` from the
[Releases page](https://github.com/Nazari/ZFSMgr/releases). From a terminal, `curl` avoids
the quarantine that macOS browsers attach to downloads:

```sh
curl -LO https://github.com/Nazari/ZFSMgr/releases/download/v1.2.1/zfsmgr-cli-1.2.1.zip
curl -LO https://github.com/Nazari/ZFSMgr/releases/download/v1.2.1/zfsmgr-cli-1.2.1.zip.sha256
sha256sum -c zfsmgr-cli-1.2.1.zip.sha256      # macOS: shasum -a 256 -c …
unzip zfsmgr-cli-1.2.1.zip && cd zfsmgr-cli-1.2.1
```

The package carries the CLI and the daemon for every platform, and one installer per system:

| System | Installer | Notes |
|---|---|---|
| Linux | `sudo sh instalar-linux.sh [--con-daemon]` | x86_64 |
| macOS | `sudo sh instalar-macos.sh [--con-daemon]` | Apple Silicon and Intel |
| FreeBSD | `sudo sh instalar-freebsd.sh [--con-daemon]` | experimental |
| Windows | `instalar-windows.cmd` | CLI only; the daemon is installed from another machine |

Each installer checks the package before touching anything, needs **OpenZFS 2.3 or later**
when the machine has OpenZFS, and with `--con-daemon` also installs the daemon of that machine.
Every installer has `--help`.

You install the package on **one** machine, the one you will work from. The other machines
get their daemon from there, but only once you have told the CLI about them: see below.

## Getting started

**1. Start the shell.** Run `zfsmgr-cli` with no arguments. The first time there is a single
connection, `Local`, which is the machine you are on. When it needs to elevate on this machine
it asks for your sudo password.

**2. Add every other machine as a connection.** `install-daemon -a` only reaches the
connections you have created; it does not discover machines on its own. At the root,
`zfsm://`, `create` makes a connection:

```text
zfsm://> create nas --type SSH --host nas.local --user me --sudo
zfsm://> create laptop --type SSH --host laptop.local --user me --key ~/.ssh/id_ed25519
```

- `--sudo` when that user has to elevate there, which it does for ZFS. The password is typed
  in, or comes through `--password-fd`; it is never an argument.
- Passwords are stored encrypted with a **master password**, which the shell asks you to
  choose the first time it has something to encrypt.
- Each machine has to be reachable by SSH with that user and have **OpenZFS 2.3 or later**.
- `authorize-key nas` copies this machine's SSH key to `nas`, so it stops asking for the SSH
  password.

`ls` at `zfsm://` lists the connections, whether they answer and which daemon version each one
runs. `edit` changes a connection and `destroy` removes it.

**3. Install the daemons.** With the connections in place:

```text
zfsm://> install-daemon -a --check      # what it would do on each one, touching nothing
zfsm://> install-daemon -a              # installs or updates the daemon everywhere
```

It installs the daemon where it is missing, updates it where it is older, and updates
`zfsmgr-cli` on the machines that already have one. It is the same command for later updates:
install the new package here, then run `install-daemon -a` again.

On macOS the daemon needs **Full Disk Access** to read and write datasets: grant it in System
Settings → Privacy & Security to `/usr/local/libexec/zfsmgr-agent`. The connection list warns
when it is missing.

**4. Work.** `cd nas/tank` and every command acts there. `help` lists the commands and
`help <command>` explains one.

## A quick tour

`zfsmgr-cli` with no arguments is an **interactive shell** where the current location is a
**`zfsm://` URL**, and every command acts on it:

```text
zfsm://> cd local/tank/data
zfsm://local/tank/data> ls
zfsm://local/tank/data> create @before-upgrade
zfsm://local/tank/data> send zfsm://backup/pool/data --from @before-upgrade --wait
zfsm://local/tank/data> cp #content/report.pdf zfsm://laptop#path/home/me/
```

A URL names any object on any machine:

| URL | What it names |
|---|---|
| `zfsm://backup` | a machine (a connection) |
| `zfsm://backup/pool/data` | a pool or a dataset |
| `zfsm://backup/pool/data@daily` | a snapshot |
| `zfsm://backup/pool/data#content/dir/file` | a file inside a dataset |
| `zfsm://backup#path/etc/hosts` | any path on that machine |
| `zfsm://backup/pool/data#properties` | its properties |

Relative names work as in a file system, and **TAB** completes commands, machines, datasets
and files. URLs accept wildcards and lists (`tank/*`, `tank@auto-*`, `{a,b}`): commands that
read gather one table, and commands that modify show the exact list and ask before acting.
`help patterns` and `help urls` explain both.

The commands, as `help` lists them:

```text
Navigation: cd, pwd, ls, info
Creating and destroying, at any level: create, destroy
Connections: devices, edit, connect, disconnect, refresh, repair-mounts,
             authorize-key, export-trust
Pools: labelclear, status, history, scrub, trim, initialize, clear, flush,
       upgrade, reguid, export, import
Datasets: rename, mount, unmount, promote, get, set, load-key, unload-key,
          change-key
Snapshots: schedule, schedules, rollback, clone, holds, hold, release, diff
Delegated permissions: allow, unallow
Actions: cp, rsync, breakdown, assemble, todir, fromdir
Transfers between machines: send
Background jobs: jobs, job
Daemon: peers, log, install-daemon
```

`help <command>` gives what it does, its options and examples.

### Some things it does

- **`send`** a snapshot to another dataset on the same or another machine, in full or
  incrementally (`--base`), with the native `zfs send` flags. Either end can also be a
  **file** holding the stream: write a snapshot to `zfsm://nas#path/backups/x.zfs`, or receive
  one with `--from` that file. With `--comp gzip|lz4|zstd[:level]` the file is stored
  compressed; reading one needs nothing, the format is recognised by its first bytes. An interrupted receive is detected before sending, and you
  choose to continue it (`--resume`) or start over (`--discard-partial`).
- **Transfers run as daemon jobs**, directly between the two machines: the data never passes
  through yours, they survive closing the shell, and `jobs`/`job` follow them. A job only
  reports `done` once the receiving end confirms it.
- **`cp`** and **`rsync`** copy files and trees between datasets and paths, across machines,
  keeping owner, group, mode and times, and warning about ACLs or extended attributes that
  cannot travel.
- **`schedule`** automatic snapshots with retention, and **level** a dataset against another
  machine, run by the daemon itself.
- **`breakdown`** a directory into child datasets and **`assemble`** them back; **`todir`** and
  **`fromdir`** move content between a dataset and a directory.
- **Pools**: create them (including from devices given as `#path/dev/...`), scrub, trim,
  import with the native flags, look at an exported pool's status, clear old labels.
- **Delegated permissions** (`allow`/`unallow`), **encryption keys**, **holds**, **diff**,
  **rollback**, **clone**, **promote**.

## Scripting

The shell reads its commands from standard input, so a script is the same commands you type,
one per line:

```sh
zfsmgr-cli --password-fd 3 --format json -y 3< <(pass show zfsmgr) <<'END'
send zfsm://nas/pool/home --on zfsm://local/tank/home@daily --wait
ls zfsm://nas/pool
END
```

- `--on <url>` (or `--from`) acts on a location without moving to it.
- `--format tsv` is for shell scripts —no header, tabs, `-` where there is no value, like
  `zfs list -H`—; `--format json` is for programs.
- `-y` skips the confirmations. Without a terminal, a command that needs confirmation refuses
  instead of guessing.
- In a script, a syntax error or an unknown command stops the session instead of carrying on
  with the next line.
- **Secrets are never arguments or environment variables**, which any user can see in `ps`.
  The master password comes from a descriptor (`--password-fd`) or from the terminal, and so
  do connection passwords (`create … --password-fd 4`) and passphrases (`load-key
  --password-fd 4`).

On the command line itself, outside the shell, there are only `version`, `connections list`,
`url parse <url>` and `--help`.

## How it works

Each managed machine runs **`zfsmgr-agent`**, a native daemon: a single binary with no runtime
dependencies, started by systemd, launchd, rc.d or a scheduled task. The CLI talks to it with
**typed requests** over TLS, tunnelled through SSH for remote machines. **No shell commands
are sent to the remote machine**: SSH is the transport, not an interpreter.

The daemons also talk to each other: transfers, copies and levelling go from one daemon to
the other, with a per-transfer token.

On **Windows** (OpenZFS for Windows) everything goes through the agent as well, with no Unix
tools needed. Some operations are not available there —the ZFS end of `send`, for example—
and the CLI says so instead of attempting them.

## Configuration

Connections live in `~/.config/ZFSMgr/config.json`, and the daemons' TLS material in
`trust-store.json` next to it. Passwords and TLS keys are encrypted with a **master password**
(`master-password` changes it). Several `zfsmgr-cli` can run at the same time: writes are
atomic and locked between processes.

## Building from source

Requirements: CMake 3.21 or later, a C++17 compiler, OpenSSL, and Qt 6 for the configure step
(the CLI and the daemon link no Qt; the build files still find it for the future GUI).

```sh
./scripts/build-linux.sh              # builds/linux/zfsmgr-cli and zfsmgr_agent
```

All platforms are cross-compiled from a Linux host —Windows with MinGW, FreeBSD with a
sysroot, macOS with osxcross—, and `scripts/buildall-cross.sh` builds every platform and the
release zip into `builds/artifacts/`:

```sh
./scripts/provision-cross-targets.sh --windows --freebsd
./scripts/buildall-cross.sh
```

See `docs/cross-compiling-linux.md` for the toolchains.

## The graphical interface (2.0.0)

The repository still contains the Qt 6 desktop application (`src/native`). It is **not
released with 1.x** and should be considered unmaintained until 2.0.0, which will bring it
back on top of the same base layer, daemon and transfers that the CLI uses.

## Keywords

OpenZFS CLI, ZFS command line manager, remote ZFS manager, ZFS replication, zfs send to file,
ZFS snapshot scheduling, zpool management, OpenZFS on Windows, OpenZFS on macOS.
