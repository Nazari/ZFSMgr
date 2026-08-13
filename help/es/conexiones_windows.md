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
- **El punto de montaje es una LETRA, no una ruta.** Lo lleva la propiedad
  `driveletter`; `mountpoint` sale `-`, y es lo normal. ZFSMgr consulta la lista real de
  montajes en vez de deducir la ruta de esa propiedad. Mayúscula y minúscula dan lo
  mismo (`z:` y `Z:` se comportan igual, comprobado).
- **Cambiar `driveletter` con el dataset montado lo desmonta y lo vuelve a montar.** Si
  algo estaba abierto en la letra anterior, se cae. No es un fallo, pero conviene
  saberlo antes de tocarlo en caliente.
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

## Traer un snapshot a Windows

Copiar o nivelar un snapshot con un extremo en Windows aparece deshabilitado, porque
encadena `zfs send | zfs recv` por una tubería y el agente todavía no lo implementa.
Hay una vía manual que **sí funciona**, y merece la pena saber por qué es esa:

```bash
# en la máquina de origen (Linux, macOS, FreeBSD)
zfs send deposito/datos@instantanea > flujo.zfs
```

Lleve el fichero al equipo Windows (una carpeta compartida vale) y allí, **en
`cmd.exe`, no en PowerShell**:

```
zfs recv -F winpool/datos < C:\ruta\flujo.zfs
```

Dos detalles, los dos comprobados contra una máquina real:

- **Por fichero funciona; por tubería no.** El mismo flujo enviado por SSH directamente
  a `zfs recv` falla con `cannot receive new filesystem stream: I/O error`, mientras que
  desde un fichero se recibe entero y con las sumas de verificación intactas. El
  problema es leer de la entrada estándar en Windows, no el flujo.
- **La redirección `<` es de `cmd.exe`.** PowerShell no la tiene, y es además quien se
  atasca al pasar unos 132 KB de datos binarios.

### Sin duplicar el espacio: por fragmentos

Lo anterior obliga a tener el flujo entero en disco, que en un dataset grande es
inaceptable. **No hace falta**: ZFS sabe reanudar una recepción interrumpida, y eso
permite ir por trozos con un solo fragmento en disco cada vez.

La clave es que **no se parte el fichero en trozos**. Cada fragmento lo genera el
emisor a partir del punto donde se quedó el receptor, así que es un flujo válido por sí
mismo. Por eso funciona donde trocear bytes a ciegas sería frágil.

1. Primer fragmento, en el origen:
   ```bash
   zfs send deposito/datos@instantanea | head -c 268435456 > frag.zfs
   ```
2. Se lleva a Windows y se recibe **con `-s`**, que guarda el estado al fallar:
   ```
   zfs recv -s -F winpool/datos < C:\ruta\frag.zfs
   ```
   Dirá `cannot receive ... I/O error`. **Es lo esperado**: el flujo está cortado.
3. Se borra el fragmento y se pide el testigo al receptor:
   ```
   zfs get -H -o value receive_resume_token winpool/datos
   ```
4. Con ese testigo, el origen genera el siguiente fragmento:
   ```bash
   zfs send -t <testigo> | head -c 268435456 > frag.zfs
   ```
5. Se repiten los pasos 2 a 4 hasta que el testigo salga `-`, que significa terminado.

Comprobado de punta a punta: 20 MB en cuatro fragmentos, con la suma MD5 del fichero
recibido idéntica a la del original, y sin tener nunca en disco más de un fragmento.
El tamaño lo decide usted; 256 MB es una elección cómoda.

## Si algo no responde

La ficha de la conexión indica si el agente está instalado, si está activo, qué versión
de API tiene y si el binario es nativo. La pestaña **Daemon** muestra su registro y
permite pedirle un latido. Si la versión de API no coincide con la que espera la
aplicación, reinstale el agente desde el menú contextual.
