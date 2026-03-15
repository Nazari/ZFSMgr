# Configuración, columnas y archivos INI

ZFSMgr usa un directorio de configuración por usuario y sistema operativo:

- Linux: `$HOME/.config/ZFSMgr`
- macOS: `$HOME/.config/ZFSMgr`
- Windows: `%USERPROFILE%/.config/ZFSMgr`

## Estructura de archivos

- `config.ini`: configuración global de la aplicación.
- `conn*.ini`: un archivo por conexión (por ejemplo `conn_fc16.ini`, `conn_surface_psrp.ini`).

Ejemplo real:

```text
~/.config/ZFSMgr/
  config.ini
  conn_fc16.ini
  conn_surface_psrp.ini
  conn_mbp_local.ini
```

## Qué se guarda en cada archivo

- `config.ini`:
  - idioma de la UI
  - opciones globales de logs
  - número de columnas de propiedades (`conn_prop_columns`)
  - conexión marcada como `Origen` y como `Destino`
  - anchos de columna del treeview superior e inferior
  - orden persistido de propiedades inline de pool, dataset y snapshot
  - grupos de visualización de pool, dataset y snapshot
  - valores por defecto (por ejemplo `[ZPoolCreationDefaults]`)
- `conn*.ini`:
  - definición completa de una conexión concreta (host, puerto, usuario, clave, etc.)

## Carga al iniciar

Al arrancar, ZFSMgr:

1. Lee `config.ini`.
2. Busca todos los `conn*.ini` en el directorio de configuración.
3. Carga cada conexión encontrada.

Si existía formato antiguo (conexiones dentro de `config.ini`), ZFSMgr migra automáticamente a `conn*.ini`.
