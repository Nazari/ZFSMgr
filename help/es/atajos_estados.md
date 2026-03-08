# Atajos y estados

- Los botones se desactivan cuando hay una accion en curso.
- El cursor cambia a ocupado durante acciones y refrescos.
- Algunas acciones requieren seleccion valida (dataset o snapshot segun el caso).
- Si origen y destino son iguales, ciertas transferencias se bloquean.
- En `Propiedades del dataset`, si editas un valor y cambias de tab/foco, el cambio pendiente se conserva.
- Al volver al mismo dataset, se restaura el valor editado y `Aplicar cambios` sigue activo hasta aplicar o deshacer.

Estados comunes:

- OK: conexion operativa.
- KO/Error: fallo de conexion o comando.
- Montado/Desmontado: estado actual del dataset.
- Pools no importables: se informan en `Estado de la conexion` con su motivo.
