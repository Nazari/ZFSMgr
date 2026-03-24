# Navegación y estados

- Los botones de acción se desactivan cuando hay una acción en curso.
- El cursor cambia a ocupado durante acciones y refrescos.
- Algunas acciones requieren selección válida (dataset o snapshot según el caso).
- Si origen y destino son iguales, ciertas transferencias se bloquean.
- Los checks `Origen` y `Destino` determinan qué conexión alimenta el árbol superior e inferior.
- Los checks `Origen` y `Destino` se persisten entre ejecuciones.
- Cada árbol recuerda su navegación por conexión/pool de forma independiente.
- Cada árbol recuerda también sus anchos de columna de forma independiente.
- Al borrar una conexión durante un refresco, los resultados pendientes se descartan de forma segura.
- `Clonar` solo se habilita cuando origen es snapshot, destino es dataset, y ambos estan en la misma conexion y el mismo pool.
- Si origen o destino usan OpenZFS `< 2.3.3`, `Copiar`, `Nivelar` y `Sincronizar` se bloquean y los labels `Origen/Destino` se muestran en rojo.
- En propiedades inline, los cambios pendientes se conservan al navegar y volver al mismo dataset/pool.
- `Aplicar cambios` se desactiva cuando no hay cambios reales pendientes y los comandos acumulados se muestran en `Cambios pendientes`.
- Navegar entre conexiones/pools no fuerza refresco: se usa caché y solo se refresca en acciones o refresco explícito.

Estados comunes:

- `OK` (verde): conexión operativa.
- `KO/Error` (rojo): fallo de conexión o comando.
- `OK` con OpenZFS `< 2.3.3` (rojo): conexión operativa, pero no apta para transferencias `send/recv` y asociadas.
- `OK` con comandos faltantes (naranja): conexión operativa, pero faltan comandos auxiliares.
- En Windows, la detección distingue entre comandos Unix realmente ejecutables y comandos PowerShell usados por compatibilidad.
- `Montado/Desmontado`: estado actual del dataset.
- `Pools no importables`: se informan en `Estado de la conexión`, con un bloque por pool y su motivo.
