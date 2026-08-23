# Menús contextuales

ZFSMgr usa menús contextuales sobre el árbol unificado.

## Sobre una conexión

![Menú contextual de conexiones](qrc:/help/img/auto/connection-context-menu.png)

- El menú que antes colgaba de la tabla de conexiones ahora cuelga del nodo raíz de conexión.
- Orden actual en conexiones:
  - `Conectar`
  - `Desconectar`
  - `Refrescar`
  - separador
  - `Nueva Conexión`
  - `Editar`
  - `Borrar`
  - separador
  - `Nuevo Pool`
  - separador
  - `Instalar comandos auxiliares`
  - `Reinstalar/Actualizar daemon`
  - `Reparar mountpoints temporales`
  - `Exportar trust-store a esta conexión`
  - `Autorizar clave SSH en...` (submenú con las demás conexiones SSH conectadas)
  - separador
  - `Split and root` (submenú: `Derecha`, `Izquierda`, `Abajo`, `Arriba`), o `Close` si el menú se abre sobre la raíz de un panel dividido

Condiciones de habilitación:

- `Conectar`: conexión marcada como desconectada y sin acción en curso.
- `Desconectar` y `Refrescar`: conexión conectada y sin acción en curso.
- `Editar` y `Borrar`: no disponibles en la conexión Local ni en conexiones redirigidas a Local.
- `Nuevo Pool`: conexión conectada.
- `Instalar comandos auxiliares`: solo si el refresco detectó un gestor de paquetes y un plan de instalación soportado para los comandos que faltan. No aplica a conexiones Windows, que trabajan solo con el agente nativo.
- `Reinstalar/Actualizar daemon`: cualquier conexión remota, Windows incluido.
- `Reparar mountpoints temporales`: cualquier conexión no Windows, incluida Local (un dataset local también puede quedarse en un mountpoint temporal).
- `Exportar trust-store a esta conexión`: cualquier conexión remota; no aplica a Local, que ya usa el trust-store local.
- `Autorizar clave SSH en...`: solo en conexiones SSH no Windows, y solo se puebla con otras conexiones SSH conectadas.

`Reparar mountpoints temporales` hace primero una pasada de solo lectura, muestra los datasets que quedaron con el mountpoint relocalizado por una sincronización interrumpida y pide confirmación antes de restaurarlos (los desmonta antes de hacerlo). Los que fallen conservan su marca y se pueden reintentar.

## Actualización automática del daemon

Al terminar un refresco, ZFSMgr reinstala el daemon automáticamente y sin diálogos **solo** cuando el motivo de atención es una desalineación de versión o de API.

Un backoff TLS de daemon-rpc marca la conexión para atención pero **no** dispara la reinstalación automática: reinstalar regeneraría el material TLS y perpetuaría el bucle fallo → reinstalación → fallo. En ese caso use `Reinstalar/Actualizar daemon` o `Exportar trust-store a esta conexión` manualmente.

## Sobre el nodo raíz del pool fusionado

![Menú contextual de pool importado](qrc:/help/img/auto/pool-context-menu-imported.png)

- El primer submenú es `Pool`.
- Dentro de `Pool` aparecen las acciones de pool:
  - `Actualizar estado`
  - `Importar`
  - `Importar renombrando`
  - `Exportar`
  - `Historial`
  - `Gestión`
- `Gestión` ejecuta acciones inmediatas (`Sync`, `Scrub`, `Upgrade`, `Reguid`, `Trim`, `Initialize`, `Clear`, `Destroy`) con diálogo de parámetros cuando aplica.
- Justo después del submenú `Pool` aparece `Split and root`, y a continuación las acciones normales de dataset sobre ese mismo nodo dual.

## Sobre datasets y snapshots

- En dataset filesystem (y en nodo pool fusionado):
  - `Gestionar visualización de propiedades`
  - `Dataset`
  - `Acciones`
  - `Marcar como origen`
  - `Con el origen …` (las seis acciones de dos extremos; ver abajo)
  - `Split and root` (submenú: `Derecha`, `Izquierda`, `Abajo`, `Arriba`)
- Submenú `Dataset`:
  - `Crear`
  - `Renombrar`
  - `Borrar`
  - `Montar`: solo si el dataset tiene `canmount` distinto de `off`, un `mountpoint`
    válido y **no** está ya montado.
  - `Desmontar`: solo si **está** montado. No se le exige `canmount`: un dataset puede
    estar montado y tener después `canmount=off`, y es justo entonces cuando hace falta.
  - `Clave de Encriptación` (`Cargar Clave`, `Descargar Clave`, `Cambiar Clave`)
  - `Programar snapshots`
  - `Permisos` (`Nuevo Set`, `Nueva Delegación`)
- Submenú `Acciones` (operaciones sobre los DATOS, no sobre el estado del dataset):
  - `Desglosar`
  - `Ensamblar`
  - `Desde Dir`
  - `Hacia Dir`
- En snapshots:
  - `Gestionar visualización de propiedades`
  - `Borrar snapshot`
  - `Rollback`
  - `Nuevo Hold`
  - `Marcar como origen`
  - `Con el origen …`

## Las seis acciones de origen y destino

`Enviar`, `Mover`, `Clonar`, `Sincronizar`, `Nivelar` y `Diff` necesitan **dos** extremos.
Ya no tienen botones: se piden desde el menú contextual, siguiendo el modelo de
copiar y pegar.

1. Clic derecho sobre el dataset o snapshot de partida → `Marcar como origen`.
2. Clic derecho sobre **cualquier otro nodo**: ese nodo es el destino, y el submenú
   `Con el origen <nombre>` ofrece las seis, nombrando el origen en cada una:

```
Con el origen datos@lunes ▸
   Enviar aquí desde datos@lunes
   Mover aquí desde datos@lunes
   Clonar aquí desde datos@lunes
   Sincronizar aquí desde datos@lunes
   Nivelar con datos@lunes
   Comparar con datos@lunes
```

No hay que marcar el destino: es el nodo sobre el que se pulsa. La línea `Origen:` de
arriba recuerda qué hay marcado, y su tooltip lo muestra completo cuando el nombre no cabe.

Lo que no aplica sale **en gris, con el motivo en el tooltip**: que el origen no es un
snapshot, que los pools no coinciden, que `Diff` compara dos puntos del mismo dataset, o
que las versiones de OpenZFS no son compatibles para transferir.

**`Mover` no copia nada.** Es un `zfs rename`: el dataset cambia de sitio en el árbol y
los datos se quedan donde están, así que es instantáneo y no queda un original que borrar.
Por eso solo funciona **dentro del mismo pool y la misma máquina**, y con datasets a los
dos lados —nunca snapshots—. Para llevar algo a otro pool o a otra máquina es `Enviar`.
Lo que sí cambia es la ruta de montaje de ese dataset y la de todo lo que cuelgue de él.
- En holds:
  - `Release`

En el nodo pool fusionado, `Split and root` aparece justo después del submenú `Pool`, no al final.

## Sobre el nodo raíz de un panel dividido

- Si el nodo es la raíz de un panel dividido (split), aparece la opción:
  - `Close`: cierra ese panel y libera el espacio en el divisor.

## Reglas

- Las acciones destructivas piden confirmación.
- Las **propiedades**, los **permisos** y los **renombrados** se editan como borradores y se
aplican con `Aplicar cambios`. Las acciones no: se ejecutan al pulsarlas.
- `Marcar como origen` actualiza la línea `Origen:` de la banda superior. El destino no se marca: es el nodo sobre el que se abre el menú.
- El nodo `@` (agrupador de snapshots) no tiene menú contextual.
- En `Dataset properties` y `Snapshot properties` el menú contextual contiene únicamente `Gestionar visualización de propiedades`.
- Los hijos de la raíz de conexión que no son raíz de pool (`Properties`, `Info`) no tienen menú contextual.
- En pools suspendidos, la mayoría de las acciones del menú contextual aparecen deshabilitadas.
