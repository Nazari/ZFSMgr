# ZFSMgr C++/Qt (cppqt)

Port de la app Python a **C++ + Qt6** con objetivo **multiplataforma** (Windows, Linux, macOS).

## Estado actual

Fase 1 + Fase 2 base implementadas:

- Estructura Qt6/CMake lista para Windows/Linux/macOS.
- UI principal con layout equivalente de alto nivel:
  - Panel izquierdo con tabs (`Conexiones`, `Datasets`, `Avanzado`).
  - Panel derecho con tabs de detalle (`Pools importados`, `Pools importables`).
  - Sección inferior de `Log combinado`.
- Carga de perfiles desde `connections.ini`.
- Refresco de conexiones SSH (comando de prueba `uname -a`) con timeout y log.
- Lista de conexiones en dos líneas con estado por conexión.

## Limitaciones actuales (WIP)

- No implementa todavía desencriptado de campos `encv1$` (usuario/password) de la versión Python.
- No implementa aún acciones ZFS completas (import/export/copy/level/sync, etc.).
- Tab `Datasets` y `Avanzado` todavía en migración.

## Build

Requisitos:

- CMake >= 3.21
- Compilador C++17
- Qt6 (Widgets/Core/Gui)

### Linux / macOS

```bash
cd cppqt
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/zfsmgr_qt
```

### Windows (Developer PowerShell + Qt6)

```powershell
cd cppqt
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
.\build\zfsmgr_qt.exe
```

## Compatibilidad

- Usa `QStandardPaths::AppConfigLocation` para ubicación de configuración por sistema.
- Si no existe `connections.ini` en la ruta de config del usuario, intenta fallback a `../vpython/connections.ini`.
