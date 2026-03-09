# Atajos y estados

- Los botones de acción se desactivan cuando hay una acción en curso.
- El cursor cambia a ocupado durante acciones y refrescos.
- Algunas acciones requieren selección válida (dataset o snapshot según el caso).
- Si origen y destino son iguales, ciertas transferencias se bloquean.
- En `Propiedades del dataset`, si editas un valor y cambias de tab/foco, el cambio pendiente se conserva.
- Al volver al mismo dataset, el valor editado se restaura y `Aplicar cambios` sigue activo hasta aplicar o deshacer.
- Navegar entre conexiones/pools no fuerza refresco: se usa caché y solo se refresca en acciones o refresco explícito.

Estados comunes:

- `OK` (verde): conexión operativa.
- `KO/Error` (rojo): fallo de conexión o comando.
- `OK` con comandos faltantes (naranja): conexión operativa, pero faltan comandos auxiliares.
- `Montado/Desmontado`: estado actual del dataset.
- `Pools no importables`: se informan en `Estado de la conexión`, con un bloque por pool y su motivo.
