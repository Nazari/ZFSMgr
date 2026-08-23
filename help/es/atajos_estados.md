# Navegación y estados

- El cursor cambia a ocupado durante acciones y refrescos.
- El árbol unificado es ahora la fuente principal de navegación.
- La selección visual del árbol no sustituye al `Origen` marcado.
- El `Origen` se fija desde el menú contextual (`Marcar como origen`). El destino no se
  marca: es el nodo sobre el que se abre el menú para pedir la acción.
- La línea `Origen:` de la banda superior refleja lo marcado.
- Si una conexión está desconectada:
  - la conexión sigue visible
  - no muestra hijos
- `Clonar` solo se habilita cuando:
  - origen es snapshot
  - destino es dataset
  - misma conexión
  - mismo pool
- Los snapshots se seleccionan desde el nodo `@` (ya no hay menú `Seleccionar snapshot`).
- Si origen o destino usan OpenZFS `< 2.3.3`, `Enviar`, `Nivelar` y `Sincronizar` se bloquean.
- `Aplicar cambios` solo se activa si hay borradores reales de propiedades o de permisos.
  Esos dos SÍ se editan en lote y se aplican con un botón; las acciones no: se ejecutan al
  pulsarlas.
- La navegación normal usa caché; el refresco ocurre por acción explícita o tras cambios que lo requieran.
