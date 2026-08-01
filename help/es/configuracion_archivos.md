# Configuración y archivos

ZFSMgr usa un directorio de configuración por usuario y sistema operativo:

- Linux: `$HOME/.config/ZFSMgr`
- macOS: `$HOME/.config/ZFSMgr`
- Windows: `%USERPROFILE%/.config/ZFSMgr`

## Estructura de archivos

- `config.json`: configuración global de la aplicación y definición de conexiones. Los campos sensibles (usuario y contraseña) se guardan cifrados con el password maestro.
- `trust-store.json`: material TLS del daemon (certificado de servidor, y certificado y clave de cliente), también cifrado con el password maestro. Es el fichero que se copia con `Exportar trust-store a esta conexión`.
- `application.log`: log persistente de la aplicación.

Ejemplo real:

```text
~/.config/ZFSMgr/
  config.json
  trust-store.json
  application.log
```

## Qué se guarda en `config.json`

El objeto raíz tiene tres bloques:

- `connections`: array con la definición completa de cada conexión (id, nombre, machine_uid, tipo, sistema operativo, host, puerto, familia de direcciones SSH, usuario, contraseña, ruta de clave y uso de sudo). Usuario y contraseña van cifrados.
- `app`: opciones de la aplicación y estado de la interfaz.
- `ZPoolCreationDefaults`: valores por defecto para la creación de pools.

Dentro de `app` se guardan, entre otras cosas:

- idioma de la interfaz
- opciones de logs (tamaño máximo, nivel, número de líneas)
- confirmación de acciones
- número de columnas de propiedades (`conn_prop_columns`)
- conexión mostrada en el panel superior
- conexiones marcadas como desconectadas
- geometría de ventana y estado de los splitters
- visibilidad de las secciones inline
- orden y grupos de propiedades inline

## Qué NO se guarda

- Las selecciones de `Origen` y `Destino` son de sesión: se pierden al cerrar la aplicación.
- Los anchos de columna del árbol se conservan al cambiar de conexión o de panel dentro de la misma sesión, pero no entre arranques.

## Carga al iniciar

Al arrancar, ZFSMgr:

1. Migra la configuración heredada si existe (ver abajo).
2. Lee `config.json`.
3. Carga las conexiones desde el array `connections`.
4. Mueve a `trust-store.json` el material TLS que siguiera guardado en `config.json`.

## Migración desde el formato antiguo

Si existen ficheros antiguos `config.ini`, `connections.ini` o `conn*.ini`, ZFSMgr los fusiona automáticamente en `config.json` —los grupos `connection:<id>` pasan a ser entradas del array `connections`— y después los elimina. El formato INI ya no se usa.
