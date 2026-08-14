# Windows connections

On Windows, ZFSMgr works **only through the native agent**. It does not run shell
commands on the remote machine and does not need any Unix command layer installed
there.

## What the Windows machine needs

- **OpenSSH Server running.** It is the only supported transport. Windows 10 and 11
  ship it, but **disabled**.

  If you install ZFSMgr on that same machine, the installer offers to enable it for
  you: the *Enable the OpenSSH server* task is ticked by default, and it records what
  it did in `%TEMP%\zfsmgr-openssh.log`. On a machine with no internet access the
  enabling may not complete; the installer **does not wait forever** and finishes
  anyway, saying so in that log.

  To do it by hand:

  ```powershell
  Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
  Start-Service sshd
  Set-Service -Name sshd -StartupType 'Automatic'
  New-NetFirewallRule -Name sshd -DisplayName 'OpenSSH Server (sshd)' `
      -Enabled True -Direction Inbound -Protocol TCP -Action Allow -LocalPort 22
  ```

  The firewall rule matters: without it the service starts and the connection is still
  refused from outside, which is the most baffling of the three symptoms.

- **OpenZFS on Windows**, which provides `zfs` and `zpool`. The installer checks for it
  and, if it is missing, offers to open the download page.
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

- Sync with `rsync`, which does not exist on Windows.
- *To Dir*, which mounts the dataset at a temporary point to pour it out, and that means
  nothing against drive letters.
- Repairing temporary mountpoints, for the same reason.
- Scheduled automatic snapshots: they rely on ZED, and OpenZFS on Windows does not ship it.

What does work: reading and modifying datasets and pools, snapshots, cloning, ZFS
permissions, *Breakdown* and *Assemble*, **Copy and Level snapshots between machines**,
**background jobs**, and the agent log and heartbeat.

That list is not written into the application: **the agent declares it** when asked for
its status, so it updates itself once a version covering more is installed.

## Breakdown and Assemble: where the content ends up

They work on Windows, but the result **does not look the same as on Unix**, and it is
worth knowing before using them.

There, datasets **do not nest under their parent**: each one mounts as a link at the
root of the drive, named after its last component. You can see it in `Z:\`:

```
Directory of Z:\
  <JUNCTION>  parent  [\??\Volume{...}\]
  <JUNCTION>  child   [\??\Volume{...}\]
```

`winpool/parent/child` lives in `Z:\child`, not inside `Z:\parent`, which looks empty.

In practice: breaking down `Z:\data\photos` leaves the content in `Z:\photos`. It
still belongs to the dataset `winpool/data/photos` —the ZFS hierarchy is correct— but
the path in the explorer changes. Assembling puts it back inside the parent.

This is OpenZFS on Windows' model, not a ZFSMgr decision.

## Differences worth keeping in mind

- **Mountpoints.** A pool created on Linux keeps Unix-style paths (`/mnt/data`), which
  correspond to no drive on Windows. That is the pool's real data, not a misreading.
- **The mountpoint is a LETTER, not a path.** It lives in the `driveletter` property;
  `mountpoint` reads `-`, and that is normal. ZFSMgr consults the real mount list rather
  than deducing the path from that property. Case does not matter (`z:` and `Z:` behave
  identically, verified).
- **Changing `driveletter` on a mounted dataset unmounts and remounts it.** Anything
  open on the previous letter breaks. Not a fault, but worth knowing before changing it
  on a live dataset.
- **No `sudo`.** Commands run with the session's privileges, and the agent runs as a
  system service.
- **A pool imported with `-N`** stays unmounted, so an empty mount list is correct.

## Creating pools on Windows

With the preview builds of OpenZFS on Windows available today (`zfswin-2.4.1rc…`),
**creating a pool may fail for reasons outside ZFSMgr**. This was verified by running,
by hand and outside the application:

```powershell
zpool create probepool \\.\PhysicalDrive2
```

which returns `invalid argument for this pool operation` on a whole, free disk. While
that remains the case, the route that works is to **create the pool on a Linux or
FreeBSD machine and import it on Windows**; a virtual disk (VHDX) makes that easy.
Importing, reading, mounting and working with the pool do work.

## Bringing a snapshot to Windows

Since version 0.90.18 the application does this itself: *Copy here from...* and *Level
with...* work with a Windows end, in both directions, and if the transfer is cut short
you are offered to continue from where it stopped.

What follows is no longer needed, but is kept because it explains **how** it works
underneath, and because it helps when no agent is installed:

```bash
# on the source machine (Linux, macOS, FreeBSD)
zfs send tank/data@snapshot > stream.zfs
```

Move the file to the Windows machine (a shared folder will do) and there, **in
`cmd.exe`, not PowerShell**:

```
zfs recv -F winpool/data < C:\path\stream.zfs
```

Two details, both verified against a real machine:

- **What fails is SSH's pipe, not pipes.** The stream sent over SSH straight into
  `zfs recv` fails with `cannot receive new filesystem stream: I/O error`. But a local
  pipe (`type file | zfs recv`) works fine, and so does a file. In other words,
  `zfs.exe` reads pipes correctly; what it cannot digest is the handle `sshd` hands to
  the remote command. All verified.
- **The `<` redirection belongs to `cmd.exe`.** PowerShell does not have it, and it is
  also what stalls past roughly 132 KB of binary data.

### Without doubling the space: in chunks

The above requires the whole stream on disk, which for a large dataset is unacceptable.
**It is not necessary**: ZFS can resume an interrupted receive, and that allows going
piece by piece with only one chunk on disk at a time.

The key is that **the file is not split into pieces**. Each chunk is generated by the
sender from the point where the receiver stopped, so it is a valid stream in its own
right. That is why it works where blindly splitting bytes would be fragile.

1. First chunk, on the source:
   ```bash
   zfs send tank/data@snapshot | head -c 268435456 > chunk.zfs
   ```
2. Move it to Windows and receive it **with `-s`**, which saves state on failure:
   ```
   zfs recv -s -F winpool/data < C:\path\chunk.zfs
   ```
   It will say `cannot receive ... I/O error`. **That is expected**: the stream is cut.
3. Delete the chunk and ask the receiver for the token:
   ```
   zfs get -H -o value receive_resume_token winpool/data
   ```
4. With that token, the source generates the next chunk:
   ```bash
   zfs send -t <token> | head -c 268435456 > chunk.zfs
   ```
5. Repeat steps 2 to 4 until the token reads `-`, which means it is finished.

Verified end to end: 20 MB in four chunks, with the MD5 of the received file identical
to the original, and never more than one chunk on disk. You choose the size; 256 MB is
a comfortable pick.

## If something does not respond

The connection card shows whether the agent is installed, whether it is running, its
API version, and whether the binary is native. The **Daemon** tab shows its log and
lets you request a heartbeat. If the API version does not match the one the application
expects, reinstall the agent from the context menu.
