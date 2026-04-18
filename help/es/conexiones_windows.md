# Conexiones Windows

ZFSMgr distingue entre varios escenarios al trabajar con conexiones Windows.

## Detección de ZFS

- No basta con que exista un archivo llamado `zfs` o `zpool`.
- Para considerar detectado un comando Unix, ZFSMgr exige que el binario pueda ejecutarse realmente.
- Si no existe una capa Unix funcional, los comandos quedan marcados como no detectados.

## PowerShell y capa Unix

- Los comandos PowerShell usados por compatibilidad se muestran separados.
- `zfs` y `zpool` no se presentan como cmdlets PowerShell.
- Esto evita falsos positivos y mensajes ambiguos.

## Información de versión

- La versión de OpenZFS en Windows se intenta resolver ejecutando el binario real cuando existe.
- Si una conexión Windows no tiene `zfs/zpool` instalados o accesibles, la UI debe reflejarlo como no detectado.

## Impacto funcional

- Acciones que requieren shell Unix real pueden quedar bloqueadas en conexiones Windows sin capa Unix válida.
- La ayuda visual de estado de conexión debe interpretarse junto con la lista de comandos detectados y faltantes.
