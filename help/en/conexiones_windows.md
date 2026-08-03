# Windows connections

ZFSMgr distinguishes between several scenarios when working with Windows connections.

## ZFS detection

- Having a file named `zfs` or `zpool` is not enough.
- For a Unix command to count as detected, ZFSMgr requires that the binary can actually be executed.
- If there is no working Unix layer, the commands are marked as not detected.

## PowerShell and the Unix layer

- The PowerShell commands used for compatibility are listed separately.
- `zfs` and `zpool` are not presented as PowerShell cmdlets.
- This avoids false positives and ambiguous messages.

## Version information

- The OpenZFS version on Windows is resolved by running the real binary when it exists.
- If a Windows connection has no `zfs`/`zpool` installed or reachable, the UI must show them as not detected.

## Functional impact

- Actions that require a real Unix shell may be unavailable on Windows connections without a valid Unix layer.
- **Windows has no native daemon**: only a PowerShell stub without an mTLS RPC server, so none of the daemon-based paths apply and the `Daemon` tab stays inert.
- The connection status indicator should be read together with the list of detected and missing commands.
