# Propuesta: gestion de permisos ZFS por dataset

## Objetivo

Incorporar una gestion de delegacion de permisos ZFS por dataset basada en:

- `zfs allow`
- `zfs unallow`

La gestion debe vivir en el arbol de datasets, no en dialogs sueltos, para mantener el flujo actual de trabajo dentro de los treeviews.

## Base funcional de OpenZFS

Segun la documentacion oficial de `zfs allow` / `zfs unallow`:

- `zfs allow <dataset>` muestra las delegaciones definidas en ese dataset.
- Se puede delegar a:
  - usuario
  - grupo
  - `everyone`
- Se puede delegar con alcance:
  - local (`-l`)
  - descendiente (`-d`)
  - local+descendiente (caso por defecto)
- Existen permisos de creacion (`-c`), que se aplican al creador de nuevos descendientes.
- Existen sets de permisos (`-s @setname ...`) que luego pueden asignarse a usuarios o grupos.
- `zfs unallow` permite retirar:
  - permisos asignados
  - permisos de creacion
  - permisos de sets
  - sets completos

Nota importante de OpenZFS:

- En Linux las delegaciones existen, pero ciertos permisos no se pueden delegar: `mount`, `unmount`, `mountpoint`, `canmount`, `rename` y `share`.

Fuente oficial:

- https://openzfs.github.io/openzfs-docs/man/master/8/zfs-unallow.8.html

## Propuesta de UI

### Estructura del arbol

Bajo cada dataset aparecerian tres nodos hermanos:

- `Propiedades`
- `Permisos`
- `Subdatasets (N)` si existen hijos

Para snapshots:

- no mostrar `Permisos`
- mantener solo lo ya existente (`Propiedades`, `Holds`, etc.)

Para zvol:

- si OpenZFS permite delegacion sobre `volume`, mantener `Permisos`
- si queremos reducir alcance en primera iteracion, dejarlo solo en `filesystem`

Mi recomendacion:

- primera iteracion: `filesystem` y `volume`
- excluir snapshots

### Estructura interna del nodo `Permisos`

`Permisos` seria un nodo colapsado inicialmente con estos hijos:

- `Delegaciones`
- `Permisos para nuevos subdatasets`
- `Sets de permisos`

Cada uno de esos nodos tambien arranca colapsado.

Dentro de cada nodo:

- en `Delegaciones`, cada hijo representa una delegacion concreta:
  - `Usuario <nombre> Ámbito Local`
  - `Grupo <nombre> Ámbito Desc.`
  - `Everyone Ámbito Local y Desc.`
- al desplegar una delegacion, los permisos se muestran inline como en un set:
  - columnas `C1..Cn`
  - fila superior con nombres
  - fila inferior con checks
  - marcar o desmarcar checks modifica un borrador local
- en `Permisos para nuevos subdatasets`, todos los permisos existentes se muestran igual que en un set:
  - columnas `C1..Cn`
  - fila superior con nombres
  - fila inferior con checks
  - el cambio de un check modifica un borrador local
- en `Sets de permisos`, cada set muestra todos los permisos existentes con la misma disposicion visual que las propiedades inline del dataset:
  - columnas `C1..Cn`
  - una fila superior con nombres de permisos
  - una fila inferior con checks `on/off`
  - cada bloque ocupa dos filas, igual que nombre/valor en las propiedades del dataset
- todos los cambios de `Permisos` se aplican mas tarde con `Aplicar cambios`

Ejemplo:

- `Permisos`
- `Delegaciones`
- `Usuario linarese Ámbito Local`
- `create | snapshot | quota`
- `[x]    | [x]      | [x]`
- `Grupo staff Ámbito Local y Desc.`
- `@backupops | mount | snapshot`
- `[x]        | [ ]   | [x]`
- `Permisos para nuevos subdatasets`
- `create | destroy | snapshot`
- `[x]    | [x]     | [ ]`
- `Sets de permisos`
- `@backupops`
- `snapshot | send | hold`
- `[x]      | [ ]  | [x]`

## Propuesta de interaccion

### Menus contextuales

Sobre el nodo `Permisos` del dataset:

- `Refrescar permisos`
- `Nueva delegacion`
- `Nuevo set de permisos`

Sobre un nodo de delegacion:

- `Editar delegacion`
- `Eliminar delegacion`
- los permisos se activan o desactivan inline mediante checks, en modo borrador

Sobre un nodo de set (`@setname`):

- `Renombrar conjunto de permisos`
- `Eliminar set`
- los permisos del set se activan o desactivan inline mediante checks en la rejilla de dos filas, en modo borrador

## Dialogos propuestos

### 1. Dialogo `Nueva delegacion`

Campos:

- `Tipo de destino`
  - `Usuario`
  - `Grupo`
  - `Everyone`
- `Destino`
  - combo editable con autocompletado
  - se alimenta con usuarios o grupos del sistema remoto
  - deshabilitado si `Everyone`
- `Alcance`
  - `Local`
  - `Descendiente`
  - `Local + descendiente`
- sin selector de permisos en este dialogo
- al aceptar se crea un nodo provisional bajo `Delegaciones`
- los permisos se asignan despues marcando los checks inline del nodo creado
- la delegacion no se aplica a ZFS hasta pulsar `Aplicar cambios`

### 2. Dialogo `Nuevo set de permisos`

Campos:

- `Nombre del set`
- lista checkable de permisos
- lista checkable de otros sets reutilizables

Comando:

- `zfs allow -s @<setname> <perm|@set,...> <dataset>`

### 3. Dialogo `Editar delegacion`

Campos:

- `Tipo de destino`
  - `Usuario`
  - `Grupo`
  - `Everyone`
- `Destino`
  - combo editable con autocompletado
  - se alimenta con usuarios o grupos del sistema remoto
  - deshabilitado si `Everyone`
- `Alcance`
  - `Local`
  - `Descendiente`
  - `Local + descendiente`

Los permisos no se editan en este dialogo.

- los permisos de la delegacion se mantienen tal como estan
- su activacion o retirada se hace inline con los checks del nodo desplegado
- el cambio de sujeto o ambito queda en borrador hasta pulsar `Aplicar cambios`

## Obtencion de datos del sistema remoto

Para poblar usuarios y grupos, propongo resolverlos por SO de la conexion.

### Linux / FreeBSD

Usuarios:

- `getent passwd`
- fallback: `cut -d: -f1 /etc/passwd`

Grupos:

- `getent group`
- fallback: `cut -d: -f1 /etc/group`

### macOS

Usuarios:

- `dscl . -list /Users`
- fallback: `cut -d: -f1 /etc/passwd`

Grupos:

- `dscl . -list /Groups`
- fallback: `cut -d: -f1 /etc/group`

### Windows

No lo propondria en primera iteracion.

Motivo:

- la delegacion `zfs allow` ya tiene limitaciones y semantica Unix-like
- la resolucion de usuarios/grupos en Windows requiere otra capa y complica mucho la UX inicial

Mi recomendacion:

- ocultar `Permisos` en conexiones Windows en v1
- documentarlo explicitamente

## Modelo interno propuesto

### Cache nueva por dataset

Añadir una cache especifica, por ejemplo:

- clave: `connIdx::pool::dataset`
- valor:
  - `loaded`
  - `rawText`
  - `localDelegations`
  - `descendentDelegations`
  - `localDescendentDelegations`
  - `createDelegations`
  - `permissionSets`
  - `users`
  - `groups`

### Estructuras recomendadas

```cpp
struct ZfsPermissionGrant {
    enum class SubjectType { User, Group, Everyone };
    enum class Scope { Local, Descendent, LocalAndDescendent };
    SubjectType subjectType;
    Scope scope;
    QString subjectName;
    QStringList perms;
    QStringList sets;
};

struct ZfsPermissionSet {
    QString name;      // sin o con @ segun prefieras internamente
    QStringList perms;
    QStringList sets;
};

struct DatasetPermissionsCacheEntry {
    bool loaded{false};
    QVector<ZfsPermissionGrant> grants;
    QStringList createPerms;
    QStringList createSets;
    QVector<ZfsPermissionSet> sets;
    QStringList systemUsers;
    QStringList systemGroups;
    QString rawAllowOutput;
};
```

## Parsing propuesto

Usar `zfs allow <dataset>` como fuente de verdad.

La salida oficial tiene bloques tipo:

- `Permission sets:`
- `Create time permissions:`
- `Local+Descendent permissions:`
- `Local permissions:`
- `Descendent permissions:`

El parser debe:

- detectar bloque actual
- reconocer sujeto:
  - `user <name> ...`
  - `group <name> ...`
  - `everyone ...`
- separar permisos simples de referencias a `@set`
- para `Permission sets`, guardar nombre del set y permisos asociados

No propondria inferir nada desde ACLs POSIX o permisos Unix del mountpoint.

## Reglas de validacion

### Permisos no delegables en Linux

En conexiones Linux, deshabilitar o marcar como no seleccionables:

- `mount`
- `unmount`
- `mountpoint`
- `canmount`
- `rename`
- `share`

Esto viene explicitamente indicado en la documentacion oficial.

### Dataset types

- `snapshot`: sin nodo `Permisos`
- `filesystem`: soportado
- `volume`: soportado por sintaxis oficial de `zfs allow`

### Seguridad UX

Antes de aplicar:

- mostrar preview de comandos exactos
- seguir usando el flujo actual de confirmacion de acciones
- refrescar el nodo `Permisos` al terminar

## Integracion con la UI actual

### Construccion del arbol

En `populateDatasetTree()`:

- crear `Permisos` como tercer nodo hermano
- rol nuevo, por ejemplo:
  - `kConnPermissionsNodeRole`
  - `kConnPermissionGrantRole`
  - `kConnPermissionSetRole`
  - `kConnPermissionCreateRole`

### Carga lazy

Igual que `Propiedades`, recomiendo carga lazy:

- crear siempre el nodo `Permisos`
- poblarlo cuando:
  - se expanda
  - o se haga click si esta vacio

Motivo:

- `zfs allow <dataset>` y la enumeracion de usuarios/grupos pueden ser costosos en remoto
- no merece hacerlo de golpe para todos los datasets del arbol

### Sincronizacion con borradores

Integrar `Permisos` en el mismo flujo de `Aplicar cambios` de la vista `conncontent`.

Propuesta adoptada:

- las modificaciones de permisos quedan como borrador local
- el tooltip de `Aplicar cambios` muestra tambien los `zfs allow` / `zfs unallow` pendientes
- al aplicar se ejecutan por dataset y se refresca el nodo `Permisos`

## Fases recomendadas

### Fase 1

- nodo `Permisos` en datasets/zvols
- carga y visualizacion de `zfs allow <dataset>`
- parsing de:
  - sets
  - create-time
  - local
  - descendent
  - local+descendent
- menus contextuales basicos
- dialogo `Nueva delegacion`
- dialogo `Nuevo set`
- `Eliminar delegacion`
- `Eliminar set`

### Fase 2

- edicion avanzada de delegaciones existentes
- asignacion/retiro de sets desde UI mas comoda
- filtros por usuario/grupo
- busqueda en usuarios/grupos remotos

### Fase 3

- soporte Windows si se demuestra viable
- ayuda/documentacion especifica

## Recomendacion practica

Mi recomendacion es implementar primero la Fase 1 con este alcance exacto:

- `Permisos` como nodo hermano de `Propiedades` y `Subdatasets`
- solo para datasets y zvols
- carga lazy
- aplicacion diferida con `Aplicar cambios`
- usuarios/grupos remotos reales
- soporte de sets `@setname`
- bloqueo de permisos no delegables en Linux

Eso da una base util y coherente con OpenZFS sin meter un editor demasiado ambicioso en la primera iteracion.
