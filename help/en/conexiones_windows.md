# Windows connections

On Windows, ZFSMgr works **only through the native agent**. It does not run shell
commands on the remote machine and does not need any Unix command layer installed
there.

## What the Windows machine needs

- **OpenSSH Server running.** It is the only supported transport. Windows 10 and 11
  ship it; if it is not enabled:

  ```powershell
  Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
  Start-Service sshd
  Set-Service -Name sshd -StartupType 'Automatic'
  ```

- **OpenZFS on Windows**, which provides `zfs` and `zpool`.
- **The ZFSMgr agent**, installed from the application itself with *Reinstall/Update
  daemon* in the connection context menu.

MSYS2, MinGW and other Unix tooling are no longer needed. Earlier versions did require
them, and the menu entry that installed them is gone.

## How it communicates

Exactly as on Linux, macOS and FreeBSD: commands travel as typed calls to the agent,
encrypted with authentication on both ends, through a tunnel opened over the SSH
connection itself. The agent runs them directly, with no command interpreter in
between.

That is why the transport must be SSH: without it there is no tunnel to carry those
calls.

## What is not available yet

The Windows agent does not implement some features yet. The application **does not
attempt them**: they appear disabled with the reason, and the connection card lists
them under *Unavailable features*.

- Background jobs, and with them daemon-to-daemon transfers.
- Copy and Level snapshots when either end is Windows.
- Sync with `rsync`.
- *To Dir*.
- Repairing temporary mountpoints.
- Scheduled automatic snapshots.

What does work: reading and modifying datasets and pools, snapshots, cloning, ZFS
permissions, *Breakdown* and *Assemble*, and the agent log and heartbeat.

That list is not written into the application: **the agent declares it** when asked for
its status, so it updates itself once a version covering more is installed.

## Differences worth keeping in mind

- **Mountpoints.** A pool created on Linux keeps Unix-style paths (`/mnt/data`), which
  correspond to no drive on Windows. That is the pool's real data, not a misreading.
- **No `sudo`.** Commands run with the session's privileges, and the agent runs as a
  system service.
- **A pool imported with `-N`** stays unmounted, so an empty mount list is correct.

## If something does not respond

The connection card shows whether the agent is installed, whether it is running, its
API version, and whether the binary is native. The **Daemon** tab shows its log and
lets you request a heartbeat. If the API version does not match the one the application
expects, reinstall the agent from the context menu.
