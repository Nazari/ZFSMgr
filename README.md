# ZFSMgr

Aplicacion grafica en Python para gestionar pools ZFS con:

- Conexion `LOCAL` usando `py-libzfs` si esta disponible, o `zpool` CLI como fallback.
- Conexion `SSH` (Linux/macOS y Windows con OpenSSH).
- Conexion `PSRP` (PowerShell Remoting via WSMan para Windows).

## Funciones

- Alta, modificacion y borrado de conexiones remotas.
- Comprobacion de privilegios:
  - `LOCAL`/`SSH`: verifica `sudo -n true`.
  - `PSRP`: verifica si el usuario remoto es administrador.
- Al arrancar, refresca todas las conexiones:
  - Lista pools importados.
  - Lista pools disponibles para importar.
- Importacion de pools con dialogo avanzado de parametros (`zpool import`):
  - Flags: `-f`, `-m`, `-N`, `-F`, `-n`, `-D`, `-X`, `-l`.
  - Parametros con valor: `-c`, `-R`, `-d`, `-o`, `-T`, nuevo nombre y argumentos extra.

## Requisitos

- Python 3.10+
- Herramientas del sistema segun tipo de conexion:
  - Local/SSH: `zpool` en el host de destino.
  - PSRP: PowerShell Remoting habilitado en Windows remoto.

Instalar dependencias:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Ejecucion

```bash
python app.py
```

## Empaquetado multiplataforma (Windows, Linux, macOS)

Se usa `PyInstaller` para generar binarios standalone.

### Dependencias de build

```bash
python3 -m pip install -r requirements.txt
python3 -m pip install -r requirements-build.txt
```

### Generar paquete (en el sistema actual)

```bash
python3 packaging/build.py
```

Salida:
- Linux: `dist/ZFSMgr`
- Windows: `dist/ZFSMgr.exe`
- macOS: `dist/ZFSMgr.app` (bundle nativo para Finder)
- Artefacto empaquetado: `dist/packages/`

### Instalacion local del binario generado

- Linux:
```bash
bash packaging/install-linux.sh
```
Esto instala:
- Binario en `~/.local/bin/ZFSMgr`
- Lanzador nativo en `~/.local/share/applications/zfsmgr.desktop`
- Icono en `~/.local/share/icons/hicolor/scalable/apps/zfsmgr.svg`

- macOS:
```bash
bash packaging/install-macos.sh
```
Instala `~/Applications/ZFSMgr.app`, listo para abrir con doble click.
Si no arranca al doble click, revisa: `~/Library/Application Support/ZFSMgr/startup_error.log`.

- Windows (PowerShell):
```powershell
.\packaging\install-windows.ps1
```

### Build para las 3 plataformas con GitHub Actions

Incluido workflow: `.github/workflows/build-packages.yml`

- Lanza el workflow manualmente (`workflow_dispatch`) o por push.
- Genera artefactos para `ubuntu`, `windows` y `macos` en `dist/packages/`.

## Camino concreto para tener `py-libzfs` operativo

`py-libzfs` no suele estar disponible como `pip install py-libzfs` en PyPI.
La forma estable es instalar el binding `libzfs` desde paquetes del sistema.

### Debian/Ubuntu (recomendado)

```bash
sudo apt update
sudo apt install -y zfsutils-linux python3-libzfs python3-venv
cd /Users/linarese/Work/ZFSMgr
python3 -m venv --system-site-packages .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
python /Users/linarese/Work/ZFSMgr/scripts/test_py_libzfs.py
```

### OpenZFS compilado/instalado manualmente

Si no existe paquete `python3-libzfs` para tu distro, instala OpenZFS + bindings
de Python del sistema y despues:

```bash
cd /Users/linarese/Work/ZFSMgr
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
python /Users/linarese/Work/ZFSMgr/scripts/test_py_libzfs.py
```

### macOS

En macOS, lo normal es que no exista `libzfs` para Python instalable por `pip`.
En esta app, la conexion `LOCAL` funciona sin `py-libzfs` si el comando `zpool`
esta disponible en el host.

Validacion local en macOS:

```bash
cd /Users/linarese/Work/ZFSMgr
source .venv/bin/activate
zpool list -H -o name
python /Users/linarese/Work/ZFSMgr/scripts/test_py_libzfs.py --json
```

Interpretacion:
- Si `zpool list` funciona, la app puede operar en local por CLI aunque `libzfs` no este.
- El script `test_py_libzfs.py` puede marcar `ok=false` en macOS y eso no bloquea el modo CLI.

Validacion esperada:
- Salida con `RESULT: OK`.
- Lista de pools (si existen) o `Pools: 0` si no hay pools importados.

Validacion en formato JSON (para logs/CI):

```bash
python /Users/linarese/Work/ZFSMgr/scripts/test_py_libzfs.py --json
```

Si el test falla al crear `libzfs.ZFS()`, revisa que OpenZFS y sus bindings Python
esten instalados en el sistema y que tu `venv` pueda verlos (`--system-site-packages`).

## Notas

- Las conexiones se guardan en `connections.ini` (archivo de texto) en este directorio.
- Si existe un `connections.json` antiguo, la app lo migra automaticamente a `connections.ini`.
- Las contrasenas se almacenan en texto plano en `connections.ini`.
  Para entorno productivo, conviene integrar un almacen seguro (keyring/secret manager).
- En hosts SSH Linux/macOS, la app puede ejecutar `zpool` con `sudo` usando el password de la conexion.
