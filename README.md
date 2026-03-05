# ZFSMgr (C++/Qt)

Cross-platform OpenZFS GUI manager built with **C++17 + Qt6** for **Linux, macOS, and Windows**.

## Screenshot

![ZFSMgr UI](docs/images/zfsmgr-ui.png)

## Main capabilities

- Remote connection management (SSH and Windows through SSH/PowerShell).
- Full/partial refresh and remote OpenZFS version detection.
- Pool management:
  - unified imported/importable pool list,
  - import/export,
  - pool creation with device selection and options,
  - pool destroy with strong confirmation.
- Dataset and snapshot management:
  - create, modify, rename (`zfs rename`), delete,
  - mount/unmount (including recursive flows),
  - snapshot rollback,
  - bulk snapshot deletion.
- Source/destination transfers:
  - snapshot copy (`zfs send`/`zfs recv`),
  - level and sync operations,
  - breakdown/assemble operations.
- Advanced operations:
  - `From Dir` and `To Dir` with optional source deletion.
- Logging:
  - combined UI log plus persistent rotating logs,
  - selectable log level and visible line limits,
  - command and execution detail views.
- Multi-language UI (Spanish, English, Chinese) with runtime switching.
- Secret masking in logs (`[secret]`).

## Remote source/destination support

ZFSMgr can operate with **remote source and/or destination** on:

- Linux
- macOS/Unix
- Windows

The app adapts command strategies by remote OS and available tooling.

## Windows compatibility checks

For Windows targets, ZFSMgr validates runtime prerequisites so operations can run safely:

- OpenZFS tools availability (`zfs`, `zpool`), including common install paths.
- Shell/runtime availability and compatibility (PowerShell and optional MSYS64/MINGW tooling when needed).
- Command path resolution and fallback behavior for mixed Unix/Windows command flows.
- Mount semantics handling (including `driveletter`-based effective mount resolution).

If required components are missing, connection status and command availability are reported in the UI/logs.

## UI layout

- Left panel tabs: `Connections`, `Datasets`, `Advanced`.
- Right panel: context detail (pools, pool state/properties, dataset trees/properties).
- Bottom panel: combined log.

## Configuration and data

- User config location: `~/.config/ZFSMgr/` on Linux (Qt-equivalent path on macOS/Windows).
- Connections file: `connections.ini`.
- Master password used to protect credentials in configuration.

## Build requirements

- CMake >= 3.21
- C++17-capable compiler
- Qt6 (`Core`, `Gui`, `Widgets`)
- OpenSSL (especially relevant on Windows/Qt environments)

## Quick build

### Linux

```bash
./build-linux.sh
```

Expected binary: `build-linux/zfsmgr_qt`

### macOS

```bash
./build-macos.sh
```

The script builds the binary and can also generate an unsigned `.app` bundle.

### Windows (PowerShell)

```powershell
.\build-windows.ps1
```

The script auto-detects toolchain/Qt and builds under `build-windows`.

## Run

After building, run the generated binary for your platform and unlock with the master password.
