# Ejecuciones acumulables

## Criterio general

En ZFSMgr hay dos tipos de acciones desde la UI:

- acciones acumulables, que se añaden a `Aplicar cambios`
- acciones inmediatas, que ejecutan ZFS/SSH directamente al confirmar

Regla práctica:

- si una acción genera drafts (`queuePending...`, `updateConnContentDraft...`, borradores de permisos o de propiedades), queda acumulada
- si una acción llama a `executeDatasetAction(...)`, se ejecuta al momento

## Acciones que sí se acumulan en `Aplicar cambios`

### Propiedades y programación

- cambios de propiedades inline de dataset/pool
- cambios de `Programar snapshots` (`org.fc16.gsa:*`)
- `Borrar programación`

### Permisos

- cambios inline de `Delegaciones`
- cambios inline de `Permisos para nuevos subdatasets`
- cambios inline de `Sets de permisos`
- crear, editar, renombrar o eliminar delegaciones en el modelo actual de borrador
- crear, renombrar o eliminar sets de permisos en el modelo actual de borrador

### Renombres y shell drafts

- renombres de dataset
- renombres de snapshot
- acciones shell encoladas en borrador

## Acciones que no se acumulan y se ejecutan inmediatamente

### Dataset y snapshots

- `Borrar dataset/snapshot`
- `Rollback`
- `Crear hijo dataset`
- `Montar`
- `Desmontar`

### Cifrado

- `Load key`
- `Unload key`
- `Change key`

### Holds

- `Nuevo hold`
- `Release hold`

### Pool

Bajo `Gestión`:

- `Sync`
- `Scrub`
- `Reguid`
- `Trim`
- `Initialize`
- `Destroy`

### Transferencias y acciones avanzadas

- `Desglosar`
- `Ensamblar`
- `Desde Dir`
- `Hacia Dir`
- acciones directas de `send/recv`, `sync`, `diff`, `clone` o equivalentes cuando se ejecutan desde botones de acción

## Mountpoint efectivo en acciones avanzadas

Para `Desglosar` y `Ensamblar` no debe usarse la propiedad ZFS `mountpoint` como si fuese el punto de montaje real del dataset.

Regla aplicada en el código actual:

- si el dataset ya está montado, se usa solo el mountpoint efectivo devuelto por `zfs mount`
- si el dataset no está montado y el sistema soporta montaje alternativo temporal, la acción debe montar realmente el dataset en un directorio temporal y trabajar desde ahí
- si el dataset está cifrado y `keylocation=prompt`, primero debe pedir la clave y hacer `zfs load-key`

Esto evita dos errores de diseño:

- tratar una propiedad configurada como si el dataset ya estuviera accesible en ese path
- listar o copiar accidentalmente directorios del sistema remoto, por ejemplo `/`, en vez del contenido real del dataset

### Sistemas contemplados

- Linux: soportado
- macOS: soportado
- FreeBSD: soportado
- Windows: no soportado para montaje alternativo temporal en esta ruta

### Consecuencia práctica

En `Desglosar`, el diálogo de selección de directorios debe mostrar:

- directorios leídos del mountpoint efectivo real del dataset
- y un texto informativo con el mountpoint realmente usado:
  - el mountpoint real si el dataset ya estaba montado
  - o el mountpoint temporal en `/tmp/...` si se montó de forma auxiliar

### Conexiones y GSA

- instalar/actualizar/desinstalar GSA
- operaciones de conexión
- refrescos de conexión

## Motivo de la distinción

Las acciones acumulables son adecuadas para:

- cambios de configuración
- cambios combinables entre sí
- operaciones que el usuario puede querer revisar antes de aplicar

Las acciones inmediatas se reservan para:

- operaciones destructivas o ejecutivas de ZFS
- acciones operativas de montaje, rollback o cifrado
- acciones de pool
- operaciones que ya requieren confirmación explícita propia

## Resumen operativo

Si el usuario modifica:

- propiedades
- permisos
- programación GSA
- renombres

entonces debe esperar usar `Aplicar cambios`.

Si el usuario ejecuta:

- borrados
- rollback
- montaje/desmontaje
- holds
- cifrado
- gestión de pool

la acción ocurre inmediatamente tras su confirmación.
