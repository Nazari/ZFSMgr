# Navegación y estados

- El cursor cambia a ocupado durante acciones y refrescos.
- **El origen es el panel izquierdo y el destino el derecho.** No hay que marcar nada como
  origen: lo es por estar donde está.
- Marcar un snapshot en la pestaña `Snapshots` cuenta igual que marcarlo en el árbol.
- Cambiar la conexión de un panel cambia su árbol, su detalle y su log a la vez.
- Si una conexión está desconectada, su árbol queda vacío y lo dice.
- `Clonar` solo se habilita cuando:
  - el origen es un snapshot
  - el destino es un dataset
  - misma conexión
  - mismo pool
- Si origen o destino usan OpenZFS `< 2.3.3`, `Enviar`, `Nivelar` y `Sincronizar` se
  bloquean.
- `Aplicar cambios` solo se activa si hay borradores reales de propiedades o de permisos, y
  la caja de al lado enumera cuáles. Esos dos SÍ se editan en lote; las acciones no: se
  ejecutan al pulsarlas.
- La navegación normal usa caché; el refresco ocurre por acción explícita o tras cambios que
  lo requieran.
- Cada panel recuerda por separado qué tenía desplegado: los dos pueden estar sobre la misma
  conexión sin pisarse.
- La elección de conexión y de pool sobrevive a los refrescos: se busca por identificador, no
  por posición en la lista.
