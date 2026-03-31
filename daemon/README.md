# ZFSMgr daemon

Este directorio contiene la base del daemon remoto que expondrá `libzfs_core` a través de `libssh` y servirá operaciones de control a ZFSMgr.

### Próximos pasos

1. Implementar el servidor `libssh` que acepte únicamente el fingerprint gestionado por el cliente principal.
2. Añadir handlers JSON/RPC para las rutas descritas en `docs/remote-daemon-design.md`.
3. Integrar un backend específico por plataforma que invoque `libzfs_core` (Linux/FreeBSD/macOS) o `zfs.exe` (Windows) según corresponda.
4. Registrar el daemon con `CMake` y empaquetarlo para las plataformas soportadas.
