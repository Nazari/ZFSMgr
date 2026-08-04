# Conexiones Windows

En Windows, ZFSMgr trabaja **solo a través del agente nativo**. No ejecuta órdenes de
shell en el equipo remoto y no necesita que haya instalada ninguna capa de comandos
Unix.

## Qué hace falta en el equipo Windows

- **OpenSSH Server activo.** Es el único transporte admitido. Windows 10 y 11 lo traen
  de serie; si no está activado:

  ```powershell
  Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
  Start-Service sshd
  Set-Service -Name sshd -StartupType 'Automatic'
  ```

- **OpenZFS para Windows**, que es quien aporta `zfs` y `zpool`.
- **El agente de ZFSMgr**, que se instala desde la propia aplicación con *Reinstalar/
  Actualizar daemon* en el menú contextual de la conexión.

No hace falta MSYS2, MinGW ni ninguna otra herramienta Unix. Las versiones anteriores
sí las pedían, y la opción de instalarlas ha desaparecido del menú.

## Cómo se comunica

Igual que en Linux, macOS y FreeBSD: las órdenes viajan como llamadas tipadas al
agente, cifradas con autenticación por ambos lados, por un túnel que se abre sobre la
propia conexión SSH. El agente las ejecuta directamente, sin intérprete de comandos de
por medio.

De ahí que el transporte tenga que ser SSH: sin él no hay túnel por el que llevar esas
llamadas.

## Qué no está disponible todavía

El agente de Windows no implementa aún algunas funciones. La aplicación **no las
intenta**: aparecen deshabilitadas indicando el motivo, y la ficha de la conexión las
lista bajo *Funciones no disponibles*.

- Trabajos en segundo plano, y con ellos las transferencias entre daemons.
- Copiar y Nivelar instantáneas cuando alguno de los dos extremos es Windows.
- Sincronizar con `rsync`.
- *Hacia Dir*.
- Reparar puntos de montaje temporales.
- Instantáneas automáticas programadas.

Lo que sí funciona: leer y modificar datasets y pools, instantáneas, clonar, permisos
ZFS, *Desglosar* y *Ensamblar*, y el registro y el latido del agente.

Esa lista no está escrita en la aplicación: **la declara el propio agente** al
consultarle su estado, así que se pone al día sola cuando se instala una versión que
cubra más.

## Diferencias que conviene tener presentes

- **Puntos de montaje.** Un pool creado en Linux conserva rutas de estilo Unix
  (`/mnt/datos`), que en Windows no corresponden a ninguna unidad. Es el dato real del
  pool, no un error de lectura.
- **Sin `sudo`.** Las órdenes se ejecutan con los privilegios de la sesión, y el agente
  corre como servicio del sistema.
- **Un pool importado con `-N`** queda sin montar, así que la lista de montajes sale
  vacía con razón.

## Si algo no responde

La ficha de la conexión indica si el agente está instalado, si está activo, qué versión
de API tiene y si el binario es nativo. La pestaña **Daemon** muestra su registro y
permite pedirle un latido. Si la versión de API no coincide con la que espera la
aplicación, reinstale el agente desde el menú contextual.
