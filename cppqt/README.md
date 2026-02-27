# ZFSMgr C++/Qt (cppqt)

Port inicial de la app Python a **C++ + Qt6** con objetivo **multiplataforma** (Windows, Linux, macOS).

## Estado

Esta versión es una base funcional de migración:

- Estructura Qt6/CMake lista para Windows/Linux/macOS.
- UI principal equivalente en layout general:
  - Panel izquierdo con tabs (`Conexiones`, `Datasets`, `Avanzado`).
  - Panel derecho de detalle con tabs.
  - Sección inferior de `Log combinado`.
- Carga de conexiones desde `connections.ini`.

Pendiente: migrar toda la lógica de acciones ZFS/SSH/PSRP, i18n completa y comportamiento fino.

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

## Notas de compatibilidad

- Usa `QStandardPaths::AppConfigLocation` para ubicación de configuración en cada OS.
- Si no existe `connections.ini` en la ruta de config de usuario, intenta fallback a `../vpython/connections.ini`.
