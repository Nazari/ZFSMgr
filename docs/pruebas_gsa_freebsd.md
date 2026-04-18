# Pruebas manuales de GSA en FreeBSD

## Objetivo

Verificar que una conexión FreeBSD soporta instalación, activación, ejecución, observabilidad y desinstalación del GSA.

## 1. Refresh de conexión

En ZFSMgr:

- conectar la conexión FreeBSD
- abrir tooltip de la fila
- comprobar que no cae en rama Linux/macOS por error

Puntos a validar:

- la conexión refresca sin error
- la fila sigue operativa
- el menú `GSA` aparece habilitado según el estado real

## 2. Instalación GSA

En ZFSMgr:

- pulsar `Instalar gestor de snapshots`

En FreeBSD:

```bash
sudo ls -ld /etc/zfsmgr /var/db/zfsmgr /usr/local/libexec/zfsmgr-gsa.sh
sudo ls -l /etc/zfsmgr
sudo crontab -l
```

Resultado esperado:

- existe `/usr/local/libexec/zfsmgr-gsa.sh`
- existe `/etc/zfsmgr/gsa.conf`
- existe `/etc/zfsmgr/gsa-connections.conf`
- existe `/etc/zfsmgr/gsa_known_hosts`
- existe `/var/db/zfsmgr`
- ownership `root:wheel`
- permisos restrictivos
- existe un bloque `# BEGIN ZFSMgr GSA` / `# END ZFSMgr GSA` en `crontab`

## 3. Detección de estado en la GUI

En ZFSMgr:

- refrescar otra vez la conexión
- revisar el tooltip y el menú `GSA`

Resultado esperado:

- `gsaInstalled = true`
- scheduler detectado como `cron`
- `gsaActive = true`
- versión del GSA leída desde el script instalado

## 4. Lectura del log

En FreeBSD:

```bash
sudo /usr/local/libexec/zfsmgr-gsa.sh
sudo tail -n 100 /var/db/zfsmgr/GSA.log
```

En ZFSMgr:

- abrir la subpestaña `GSA`

Resultado esperado:

- la GUI muestra el mismo contenido del `GSA.log`
- no aparece vacía
- no intenta leer rutas de Linux o macOS

## 5. Programación simple de snapshots

En ZFSMgr:

- programar un dataset FreeBSD con:
  - `Activado=on`
  - `Horario=2`
- aplicar cambios

En FreeBSD:

```bash
sudo /usr/local/libexec/zfsmgr-gsa.sh
zfs list -t snapshot | grep GSA-
sudo tail -n 100 /var/db/zfsmgr/GSA.log
```

Resultado esperado:

- se crea al menos un snapshot `GSA-hourly-...`
- el log muestra evaluación y creación del snapshot

## 6. Nivelación

En ZFSMgr:

- configurar:
  - `Nivelar=on`
  - `Destino=Con::pool/dataset`
- reinstalar o actualizar GSA en la conexión FreeBSD

En FreeBSD:

```bash
sudo /usr/local/libexec/zfsmgr-gsa.sh
sudo tail -n 150 /var/db/zfsmgr/GSA.log
```

Resultado esperado:

- el log resuelve la conexión destino
- crea snapshot local
- intenta o ejecuta la ruta `send/recv`
- si no puede, deja un motivo explícito y no un fallo silencioso

## 7. Detección de obsolescencia

En ZFSMgr:

- cambiar una conexión usada como destino por un dataset GSA activo del host FreeBSD
- refrescar la conexión FreeBSD

Resultado esperado:

- la fila se marca con `(*)`
- el menú `GSA` pasa a `Actualizar versión del Gestor de snapshots`
- el tooltip refleja el motivo

## 8. Desinstalación

En ZFSMgr:

- pulsar `Desinstalar GSA`

En FreeBSD:

```bash
sudo crontab -l
sudo ls -ld /etc/zfsmgr /var/db/zfsmgr /usr/local/libexec/zfsmgr-gsa.sh
```

Resultado esperado:

- desaparece el bloque `ZFSMgr GSA` del `crontab`
- se eliminan script y configs
- se elimina el runtime/log si no queda contenido

## Riesgos a observar

- `crontab` no disponible o con política distinta en ese host
- ausencia de `logger`
- ausencia de `sshpass` en rutas con password
- `sudo` con política interactiva no compatible
- diferencias locales de ownership o shell por defecto
