# Conexiones Windows

En Windows, ZFSMgr trabaja **solo a través del agente nativo**. No ejecuta órdenes de
shell en el equipo remoto y no necesita que haya instalada ninguna capa de comandos
Unix.

## Qué hace falta en el equipo Windows

- **OpenSSH Server activo.** Es el único transporte admitido. Windows 10 y 11 lo traen
  de serie, pero **desactivado**.

  Si instala ZFSMgr en ese mismo equipo, el instalador se ofrece a activarlo por usted:
  la casilla *Activar el servidor OpenSSH* viene marcada, y deja constancia de lo que
  hizo en `%TEMP%\zfsmgr-openssh.log`. Si el equipo no tiene salida a internet la
  activación puede no completarse; el instalador **no se queda esperando** y termina
  igualmente, avisando en ese registro.

  Para hacerlo a mano:

  ```powershell
  Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
  Start-Service sshd
  Set-Service -Name sshd -StartupType 'Automatic'
  New-NetFirewallRule -Name sshd -DisplayName 'OpenSSH Server (sshd)' `
      -Enabled True -Direction Inbound -Protocol TCP -Action Allow -LocalPort 22
  ```

  La regla de cortafuegos importa: sin ella el servicio arranca y aun así la conexión
  se rechaza desde fuera, que es el síntoma más desconcertante de los tres.

- **OpenZFS para Windows**, que es quien aporta `zfs` y `zpool`. El instalador lo
  comprueba y, si no lo encuentra, ofrece abrir la página de descarga.
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
- **La letra de unidad, en MAYÚSCULA.** En Windows el punto de montaje no es una ruta
  sino la propiedad `driveletter`. Escríbala como `Z`, no como `z`: OpenZFS acepta la
  minúscula sin protestar, pero después el contenido no se puede listar y el dataset
  parece vacío o inaccesible. Es la causa más probable si el árbol dice que un dataset
  montado no tiene contenido.
- **`mountpoint` sale `-`.** Es lo normal: en Windows manda `driveletter`. ZFSMgr
  consulta la lista real de montajes, no deduce la ruta de esa propiedad.
- **Sin `sudo`.** Las órdenes se ejecutan con los privilegios de la sesión, y el agente
  corre como servicio del sistema.
- **Un pool importado con `-N`** queda sin montar, así que la lista de montajes sale
  vacía con razón.

## Crear pools en Windows

Con las versiones preliminares de OpenZFS para Windows disponibles hoy
(`zfswin-2.4.1rc…`), **crear un pool puede fallar por causas ajenas a ZFSMgr**. Se
comprobó ejecutando a mano, fuera de la aplicación:

```powershell
zpool create probepool \\.\PhysicalDrive2
```

que devuelve `invalid argument for this pool operation` sobre un disco entero libre.
Mientras eso siga así, la vía que funciona es **crear el pool en una máquina Linux o
FreeBSD e importarlo en Windows**; con un disco virtual (VHDX) es cómodo de hacer.
Importar, leer, montar y trabajar con el pool sí funcionan.

## Si algo no responde

La ficha de la conexión indica si el agente está instalado, si está activo, qué versión
de API tiene y si el binario es nativo. La pestaña **Daemon** muestra su registro y
permite pedirle un latido. Si la versión de API no coincide con la que espera la
aplicación, reinstale el agente desde el menú contextual.
