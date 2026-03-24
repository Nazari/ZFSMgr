# Diseño técnico: capturas automáticas de UI para la ayuda

## Objetivo

Automatizar la generación de imágenes de ventanas, paneles, diálogos y menús contextuales de ZFSMgr para integrarlas en la ayuda y mantenerlas actualizadas sin capturas manuales una a una.

## Problema a resolver

La documentación visual manual tiene varios problemas:

- consume mucho tiempo
- es difícil de mantener
- produce imágenes inconsistentes
- se queda obsoleta cuando cambia la UI

Además, en ZFSMgr hay muchas superficies diferentes:

- ventana principal
- tabs y paneles
- diálogos modales
- menús contextuales
- árboles superior e inferior
- estados con datos cargados y estados vacíos

## Enfoque recomendado

La solución recomendada para este proyecto es híbrida:

1. motor de capturas dentro del propio binario de pruebas GUI
2. script Linux/X11 que lance ese motor y recoja los PNG generados

Esto evita depender del ratón o de coordenadas frágiles del escritorio y permite capturas reproducibles.

## Principios de diseño

### 1. Datos demo estables

Todas las capturas deben generarse sobre un estado demo fijo.

Ese estado debe incluir al menos:

- una conexión `Local`
- una conexión remota tipo `MBP`
- pools importados e importables
- datasets con snapshots manuales y `GSA-*`
- datasets con programación GSA
- datasets con permisos delegados

La fuente ideal para este estado son los helpers ya existentes de test GUI.

### 2. Captura por widget, no por escritorio

Siempre que sea posible, las imágenes deben salir de:

- `QWidget::grab()`
- `QMenu::grab()`
- `QDialog::grab()`

No del escritorio completo.

Ventajas:

- imágenes limpias
- sin dependencia del tema del window manager
- sin ruido de otras ventanas
- resolución y márgenes consistentes

### 3. Nombres de salida deterministas

Las capturas deben guardarse con nombres estables, por ejemplo:

- `main-window.png`
- `connection-context-menu.png`
- `pool-context-menu-imported.png`
- `pool-context-menu-importable.png`
- `top-tree-dataset.png`
- `bottom-tree-dataset.png`
- `schedule-snapshots-node.png`
- `permissions-node.png`

Esto permite enlazarlas desde la ayuda sin depender de fechas u orden de ejecución.

### 4. Ejecución repetible

La generación debe poder ejecutarse con un comando único, por ejemplo:

```bash
./scripts/capture-ui-docs.sh
```

## Arquitectura propuesta

### Pieza 1: capturador Qt

Nuevo ejecutable de apoyo, por ejemplo:

- `zfsmgr_ui_doc_capture`

Responsabilidades:

- crear `MainWindow` en `ZFSMGR_TEST_MODE=1`
- poblar el estado demo con los helpers de test
- mostrar la ventana principal
- abrir estados concretos de la UI
- capturar widgets y guardarlos en disco
- abrir menús contextuales reales y capturarlos

### Pieza 2: script Linux/X11

Script, por ejemplo:

- `scripts/capture-ui-docs.sh`

Responsabilidades:

- crear directorio de salida
- opcionalmente limpiar capturas antiguas
- lanzar el capturador Qt
- validar que se hayan generado las imágenes mínimas esperadas

## Menús contextuales

## Dificultad específica

Los menús contextuales usan `QMenu::exec()` y son modales/bloqueantes.

### Solución propuesta

Usar una secuencia así:

1. programar un `QTimer::singleShot(...)`
2. abrir el menú real
3. en el timer:
   - localizar `QApplication::activePopupWidget()`
   - verificar que es un `QMenu`
   - capturarlo con `grab()`
   - cerrar el menú

Esto permite capturar el menú real sin rehacer su renderizado.

## Diálogos modales

Para diálogos:

- abrir el diálogo con un `QTimer::singleShot(...)` desde la ventana principal o desde la acción correspondiente
- capturar el propio diálogo con `grab()`
- cerrar el diálogo tras la captura

## Salida recomendada

Directorio:

- `help/img/auto/`

Razón:

- mantiene juntas las imágenes reales consumidas por la ayuda
- evita dispersar assets temporales en `docs/`

## Primera versión recomendada

### Alcance inicial

La primera versión no debe intentar cubrir toda la aplicación.

Debe cubrir solo:

- ventana principal completa
- menú contextual de conexiones
- menú contextual de pool importado
- menú contextual de pool importable
- árbol superior con dataset expandido
- árbol inferior con dataset expandido
- nodo `Programar snapshots`
- nodo `Permisos`

Con eso ya se desbloquea gran parte de la documentación de ayuda.

### Requisitos técnicos

- Linux/X11
- Qt Widgets
- helpers GUI de test ya existentes

No debe depender de:

- `xdotool`
- coordenadas absolutas del ratón
- capturas del escritorio completo

## Extensiones posteriores

### Fase 2

Añadir capturas de:

- diálogos de permisos
- diálogo de visibilidad de propiedades
- diálogos de conexión
- diálogos GSA

### Fase 3

Añadir:

- variantes por idioma
- modo claro/oscuro si se llegase a soportar
- pipeline de validación en CI para detectar cambios visuales bruscos

## Riesgos

### 1. Menús contextuales frágiles

Si el menú depende de selección previa incorrecta, la captura puede no corresponder al caso esperado.

Mitigación:

- preparar selección explícita antes de abrir cada menú

### 2. Datos demo insuficientes

Si el estado demo no contiene pools/datasets/permisos suficientes, faltarán superficies relevantes.

Mitigación:

- ampliar gradualmente los seeds de UI test

### 3. Cambios visuales legítimos

Las capturas cambiarán con la UI.

Mitigación:

- asumir regeneración normal como parte del mantenimiento de ayuda

## Criterios de éxito

La primera versión se considera útil si:

- genera capturas sin intervención manual
- produce nombres deterministas
- cubre al menos la ventana principal y varios menús contextuales reales
- las imágenes son reutilizables directamente en la ayuda

## Recomendación final

Para este proyecto, la opción más sólida no es automatizar con ratón global, sino reutilizar el modo de test GUI y capturar widgets reales desde Qt.

Linux/X11 es una buena primera plataforma para arrancar porque:

- el entorno actual ya lo soporta bien
- hay ejecutables de test GUI disponibles
- no hace falta introducir dependencias externas frágiles
