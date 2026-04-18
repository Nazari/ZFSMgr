# Configuración, columnas y archivos INI

ZFSMgr usa un directorio de configuración por usuario y sistema operativo:

- Linux: `$HOME/.config/ZFSMgr`
- macOS: `$HOME/.config/ZFSMgr`
- Windows: `%USERPROFILE%/.config/ZFSMgr`

## Estructura de archivos

- `config.ini`: configuración global de la aplicación.

Ejemplo real:

```text
~/.config/ZFSMgr/
  config.ini
```

## Qué se guarda en `config.ini`

- idioma de la UI
- opciones globales de logs
- número de columnas de propiedades (`conn_prop_columns`)
- conexión/dataset marcado como `Origen` y `Destino`
- estado de splitters y geometría de ventana
- anchos de columna del árbol unificado
- orden y grupos de propiedades inline
- valores por defecto (por ejemplo `[ZPoolCreationDefaults]`)
- definición completa de conexiones en grupos `connection:<id>`

## Carga al iniciar

Al arrancar, ZFSMgr:

1. Lee `config.ini`.
2. Carga las conexiones desde grupos `connection:<id>`.

Si existen ficheros antiguos `conn*.ini`, ZFSMgr los migra automáticamente a `config.ini` y después los elimina.
