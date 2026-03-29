# Navegación y estados

- El cursor cambia a ocupado durante acciones y refrescos.
- El árbol unificado es ahora la fuente principal de navegación.
- La selección visual del árbol no sustituye a `Origen` y `Destino`.
- `Origen` y `Destino` se fijan explícitamente desde el menú contextual del dataset.
- La caja `Selected datasets` refleja esa selección lógica.
- Si una conexión está desconectada:
  - la conexión sigue visible
  - sus pools desaparecen
- `Clonar` solo se habilita cuando:
  - origen es snapshot
  - destino es dataset
  - misma conexión
  - mismo pool
- Si origen o destino usan OpenZFS `< 2.3.3`, `Copiar`, `Nivelar` y `Sincronizar` se bloquean.
- `Aplicar cambios` solo se activa si hay cambios reales en `Pending changes`.
- La navegación normal usa caché; el refresco ocurre por acción explícita o tras cambios que lo requieran.
