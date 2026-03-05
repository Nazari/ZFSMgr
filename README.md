# ZFSMgr (C++/Qt)

Gestor gráfico de OpenZFS multiplataforma en **C++17 + Qt6** para **Linux, macOS y Windows**.

## Funcionalidades principales

- Gestión de conexiones remotas (SSH y Windows por SSH/PowerShell).
- Refresco completo/parcial de conexiones y detección de versión de OpenZFS.
- Gestión de pools:
  - listado unificado de pools (importados/importables),
  - importar/exportar,
  - crear pool con selección de dispositivos y opciones,
  - destruir pool con confirmación reforzada.
- Gestión de datasets y snapshots:
  - crear, modificar, renombrar (`zfs rename`), borrar,
  - montar/desmontar (incluyendo opciones recursivas),
  - rollback de snapshot,
  - borrado masivo de snapshots.
- Transferencias entre origen/destino:
  - copiar snapshot (`zfs send`/`zfs recv`),
  - nivelar y sincronizar,
  - desglosar/ensamblar.
- Operaciones avanzadas:
  - `Desde Dir` y `Hacia Dir` con opciones de conservación/borrado de origen.
- Logs:
  - log combinado en UI y logs persistentes con rotación,
  - control de nivel y número de líneas visibles,
  - vista de comandos y detalle de ejecución.
- UI multiidioma (español, inglés y chino) con cambio dinámico desde menú.
- Enmascarado de secretos en logs (`[secret]`).

## Estructura de interfaz

- Panel izquierdo: `Conexiones`, `Datasets`, `Avanzado`.
- Panel derecho: detalle contextual (pools, propiedades/estado, árbol y propiedades de dataset).
- Panel inferior: log combinado.

## Configuración y datos

- Configuración de usuario: `~/.config/ZFSMgr/` (Linux; equivalente en macOS/Windows según Qt).
- Fichero de conexiones: `connections.ini`.
- Password maestro para proteger credenciales en configuración.

## Requisitos de compilación

- CMake >= 3.21
- Compilador con soporte C++17
- Qt6 (`Core`, `Gui`, `Widgets`)
- OpenSSL (especialmente en Windows para algunos entornos Qt)

## Compilación rápida

### Linux

```bash
./build-linux.sh
```

Binario esperado: `build-linux/zfsmgr_qt`

### macOS

```bash
./build-macos.sh
```

El script genera binario y, si se configura así, bundle `.app` sin firmar.

### Windows (PowerShell)

```powershell
.\build-windows.ps1
```

El script detecta toolchain/Qt y compila en `build-windows`.

## Ejecución

Tras compilar, ejecuta el binario generado para tu plataforma y abre sesión con el password maestro.
