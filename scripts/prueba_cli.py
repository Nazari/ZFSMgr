#!/usr/bin/env python3
"""Batería funcional del CLI: se comprueba la MÁQUINA, no lo que dice el CLI.

Cada caso manda órdenes al intérprete y después mira el estado real con `zfs`/`zpool`.
Esa es la diferencia que importa: un «hecho» en la salida no prueba nada —esta tarde
hemos visto un envío que decía `done` con cero bytes movidos— y lo único que zanja la
duda es preguntarle a ZFS.

Los pools son SOBRE FICHERO y se llaman `ztfc16` y `ztunib`: no tocan nada existente y
se destruyen al terminar. Las máquinas de verdad (`fc16`, `sback`) no se rozan.

Secretos por DESCRIPTOR, nunca por argumento ni entorno: los dos quedan visibles en `ps`.

    ./scripts/prueba_cli.py --maestra-fd 9 --sudo-fd 8 9<clave.txt 8<sudo.txt
    ./scripts/prueba_cli.py --solo snapshot     # solo los casos que casen
    ./scripts/prueba_cli.py --sin-limpieza      # deja los pools para mirarlos
"""

import argparse
import os
import re
import shlex
import subprocess
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(RAIZ, "builds", "linux", "zfsmgr-cli")

POOL_LOCAL = "ztfc16"
POOL_REMOTO = "ztunib"
IMG_DIR = "/var/tmp/zfsmgr-pruebas"


class Contexto:
    def __init__(self, maestra, sudo, remoto=None, dir_tmp=None):
        self.maestra = maestra
        self.sudo = sudo
        # La maestra se deja en un fichero 0600 para pasarla por descriptor al CLI. Por
        # argumento o por entorno quedaría visible en `ps`.
        self.dir_tmp = dir_tmp or os.path.join(
            os.environ.get("TMPDIR", "/tmp"), f"zfsmgr-prueba-{os.getpid()}")
        os.makedirs(self.dir_tmp, mode=0o700, exist_ok=True)
        ruta = os.path.join(self.dir_tmp, ".maestra")
        with open(os.open(ruta, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
            f.write(maestra + "\n")
        self.remoto = remoto          # «linarese@unib.local» o None
        self.pasados = []
        self.fallos = []


def sh(ctx, orden, en_remoto=False, root=False):
    """Una orden de shell en fc16 o en unib. Devuelve (rc, salida)."""
    if root:
        orden = "sudo -S -p '' " + orden
    if en_remoto:
        # `shlex.quote`, NO `repr()`. El repr de Python entrecomilla a la manera de Python:
        # si la orden ya lleva comillas simples —y las lleva, por el `sudo -p ''`— cambia a
        # dobles y escapa las de dentro, que en bash significa otra cosa. La orden llegaba
        # deformada y el fichero de prueba no se creaba en la máquina remota, así que las
        # comprobaciones de contenido fallaban por algo que no era el CLI.
        orden = f"ssh -o BatchMode=yes -o ConnectTimeout=10 {ctx.remoto} {shlex.quote(orden)}"
    entrada = (ctx.sudo + "\n") if root else None
    p = subprocess.run(["bash", "-c", orden], input=entrada, capture_output=True,
                       text=True, timeout=300)
    return p.returncode, (p.stdout + p.stderr)


def cli(ctx, ordenes, timeout=180, idioma=None):
    """Manda órdenes al intérprete y devuelve (rc, salida).

    La contraseña maestra va por el DESCRIPTOR 9, abierto con la redirección del shell.
    Montarlo con `preexec_fn` no vale: `subprocess` cierra los descriptores que no están
    en `pass_fds` y el 9 llegaba cerrado — el CLI se quedaba sin contraseña y TODAS las
    mutaciones fallaban mientras las lecturas parecían ir bien, que es el peor síntoma
    posible porque no se lee como un fallo de autenticación.
    """
    guion = "\n".join(ordenes) + "\nexit\n"
    fichero = os.path.join(ctx.dir_tmp, ".maestra")
    # El IDIOMA, cuando el caso depende de él.
    #
    # Sin `--lang` se usa el de la configuración de la máquina, que aquí es INGLÉS. Un caso
    # que buscaba «Snapshots» en la salida castellana pasaba tan contento sin haber mirado
    # el castellano: el catálogo inglés ya decía «Snapshots» desde siempre. Cuando lo que se
    # afirma es un texto, hay que decir en qué lengua se afirma.
    lang = f" --lang {shlex.quote(idioma)}" if idioma else ""
    orden = f"exec 9<{fichero}; exec {CLI!r} --password-fd 9 --format text{lang} -y"
    p = subprocess.run(["bash", "-c", orden], input=guion, capture_output=True,
                       text=True, timeout=timeout)
    return p.returncode, (p.stdout + p.stderr)


def caso_argv(ctx, nombre, args, espera=(), no_espera=(), rc=None):
    """Un caso que invoca el BINARIO directamente, sin entrar al intérprete.

    Hace falta para lo que ocurre ANTES de que haya sesión: la ayuda, el idioma, las
    opciones mal escritas. `--help` se atendía dentro del bucle de argumentos retornando en
    el acto, o sea antes de resolver el idioma, y salía en castellano con `--lang en`
    puesto; como el intérprete nunca llega a arrancar en ese camino, ningún caso de los de
    arriba podía verlo.
    """
    p = subprocess.run([CLI] + list(args), capture_output=True, text=True, timeout=60)
    salida = p.stdout + p.stderr
    problemas = []
    if rc is not None and p.returncode != rc:
        problemas.append(f"rc={p.returncode}, esperaba {rc}")
    for patron in espera:
        if not re.search(patron, salida, re.M):
            problemas.append(f"falta en la salida: /{patron}/")
    for patron in no_espera:
        if re.search(patron, salida, re.M):
            problemas.append(f"NO debería salir: /{patron}/")
    if problemas:
        ctx.fallos.append((nombre, problemas, salida))
        print(f"  FALLO  {nombre}")
        for x in problemas:
            print(f"         {x}")
    else:
        ctx.pasados.append(nombre)
        print(f"  ok     {nombre}")
    return not problemas


def caso(ctx, nombre, ordenes, espera=(), no_espera=(), estado=(), rc=None, veces=(),
         idioma=None):
    """Un caso: órdenes al CLI, y después comprobaciones contra la máquina.

    `veces` son pares (patrón, n): el patrón tiene que aparecer EXACTAMENTE n veces. Existe
    porque `espera` da por bueno «una o más», y eso se tragó un listado que salía DOBLE: la
    sonda de importables ejecuta `zpool import` y `zpool import -s` y pega las dos salidas,
    así que cada pool se enumeraba dos veces. Con un solo pool de prueba se lee como un
    listado raro; con varios, no hay forma de saber si son dos pools homónimos o el mismo
    contado dos veces.
    """
    rc_real, salida = cli(ctx, list(ordenes), idioma=idioma)
    problemas = []
    if rc is not None and rc_real != rc:
        problemas.append(f"rc={rc_real}, esperaba {rc}")
    for patron in espera:
        if not re.search(patron, salida, re.M):
            problemas.append(f"falta en la salida: /{patron}/")
    for patron in no_espera:
        if re.search(patron, salida, re.M):
            problemas.append(f"NO debería salir: /{patron}/")
    for patron, n in veces:
        hay = len(re.findall(patron, salida, re.M))
        if hay != n:
            problemas.append(f"/{patron}/ sale {hay} veces, esperaba {n}")
    # Lo que de verdad decide: el estado de la máquina.
    for descripcion, orden, patron, *resto in estado:
        remoto = bool(resto and resto[0])
        _, vista = sh(ctx, orden, en_remoto=remoto, root=True)
        if not re.search(patron, vista, re.M):
            problemas.append(f"la máquina dice que NO: {descripcion} "
                             f"(/{patron}/ no está en «{vista.strip()[:120]}»)")
    if problemas:
        ctx.fallos.append((nombre, problemas, salida))
        print(f"  FALLO  {nombre}")
        for p in problemas:
            print(f"         {p}")
    else:
        ctx.pasados.append(nombre)
        print(f"  ok     {nombre}")
    return not problemas


# ── Preparación y limpieza ───────────────────────────────────────────────────
#
# Pools SOBRE FICHERO. No tocan nada existente, y se destruyen al terminar aunque la
# batería se caiga a la mitad.

def prepara(ctx, remoto=False):
    pool = POOL_REMOTO if remoto else POOL_LOCAL
    img = f"{IMG_DIR}/{pool}.img"
    sh(ctx, f"mkdir -p {IMG_DIR}", remoto)
    sh(ctx, f"zpool destroy {pool}", remoto, root=True)
    # El punto de montaje sobrevive al pool con los directorios de sus datasets dentro, y
    # entonces `zpool create` se niega: «mountpoint exists and is not empty». Sin esto, la
    # preparación falla de una pasada a la siguiente sin que se vea por qué.
    sh(ctx, f"rm -rf /mnt/{pool}", remoto, root=True)
    sh(ctx, f"rm -f {img} && truncate -s 1G {img}", remoto)
    rc, salida = sh(ctx, f"zpool create -f -m /mnt/{pool} {pool} {img}", remoto, root=True)
    if rc != 0:
        raise SystemExit(f"no se pudo crear {pool}: {salida}")
    sh(ctx, f"zfs create {pool}/datos", remoto, root=True)
    # Texto DISTINTO en cada máquina. Con el mismo en las dos, una copia que no se hizo se
    # lee igual que una que sí: el fichero está, pero es el de siempre.
    # Comillas DOBLES por dentro: la orden entera se vuelve a entrecomillar para el ssh,
    # y unas simples anidadas cierran la de fuera. Con eso el fichero no se creaba y
    # `todir`, `rsync` y los dos `send` fallaban comprobando algo que nunca existió.
    marca = "hola desde unib" if remoto else "hola desde fc16"
    sh(ctx, f'bash -c "echo {marca} > /mnt/{pool}/datos/fichero.txt"', remoto, root=True)
    sh(ctx, f"bash -c 'mkdir -p /mnt/{pool}/datos/sub && echo anidado > /mnt/{pool}/datos/sub/otro.txt'",
       remoto, root=True)
    sh(ctx, f"zfs snapshot {pool}/datos@s1", remoto, root=True)
    if os.environ.get("ZFSMGR_PRUEBA_VERBOSA"):
        print(f"  preparado {pool} en {'unib' if remoto else 'fc16'}")


def limpia(ctx, remoto=False):
    pool = POOL_REMOTO if remoto else POOL_LOCAL
    sh(ctx, f"zpool destroy -f {pool}", remoto, root=True)
    sh(ctx, f"rm -f {IMG_DIR}/{pool}.img", remoto)


# ── Casos ────────────────────────────────────────────────────────────────────

def casos_navegacion(ctx):
    P = POOL_LOCAL
    caso(ctx, "nav: ls en la raíz lista las conexiones",
         ["ls"], espera=[r"^local\s", r"LOCAL"])
    caso(ctx, "nav: pwd da la URL completa",
         [f"cd Local/{P}/datos", "pwd"], espera=[rf"zfsm://local/{P}/datos"])
    caso(ctx, "nav: ruta relativa desde el pool",
         [f"cd Local/{P}", "cd datos", "pwd"], espera=[rf"zfsm://local/{P}/datos"])
    caso(ctx, "nav: «..» sube un nivel",
         [f"cd Local/{P}/datos", "cd ..", "pwd"], espera=[rf"zfsm://local/{P}$"])
    caso(ctx, "nav: «-» vuelve a la anterior",
         [f"cd Local/{P}", "cd datos", "cd -", "pwd"], espera=[rf"zfsm://local/{P}$"])
    caso(ctx, "nav: un destino que no existe falla con rc=1",
         ["cd Local/noexiste"], rc=1)
    # Para un guion esto es lo que más importa: una errata NO puede terminar con éxito.
    # Antes salía con 0 —el 127 lo pisaba la siguiente orden que fuera bien— y el guion se
    # daba por bueno habiendo hecho otra cosa.
    caso(ctx, "un verbo que no existe detiene el guion con rc=127",
         ["ordeninventada", "ls"], rc=127, espera=[r"orden desconocida|unknown command"])


def casos_lectura(ctx):
    P = POOL_LOCAL
    caso(ctx, "ls: en la conexión salen los pools",
         ["cd Local", "ls"], espera=[rf"^{P}\s"])
    caso(ctx, "ls: en el pool salen los datasets",
         [f"cd Local/{P}", "ls"], espera=[r"^datos\s"])
    caso(ctx, "ls: en el dataset salen sus instantáneas",
         [f"cd Local/{P}/datos", "ls"], espera=[r"^@s1\s"])
    caso(ctx, "ls: #properties enseña las propiedades",
         [f"cd Local/{P}/datos", "ls #properties"], espera=[r"mountpoint", r"compression"])
    caso(ctx, "ls: #content enseña los ficheros",
         [f"cd Local/{P}/datos", "ls #content"], espera=[r"fichero\.txt", r"sub"])
    caso(ctx, "get: devuelve el valor que ZFS tiene puesto",
         [f"cd Local/{P}/datos", "set compression=lz4", "get compression"],
         espera=[r"^lz4$"],
         estado=[("y ZFS lo confirma", f"zfs get -H -o value compression {P}/datos", r"^lz4$")])
    caso(ctx, "info: cuenta el estado de la conexión",
         ["cd Local", "info"],
         # En minúsculas y en tabla, la MISMA forma que `refresh`. Antes eran tres líneas
         # propias más el volcado crudo del agente en MAYÚSCULAS=valor, dos convenciones en
         # la misma salida y sin pasar por el formateador.
         espera=[r"^status\s+OK", r"^version\s+0\.", r"^api\s+\d"],
         no_espera=[r"STATUS=OK", r"CAPS="])


def casos_crear_destruir(ctx):
    P = POOL_LOCAL
    prepara(ctx)   # terreno limpio: ningún grupo hereda lo que dejó otro
    caso(ctx, "create: un dataset hijo",
         [f"cd Local/{P}", "create hijo"],
         estado=[("el dataset existe", f"zfs list -H -o name {P}/hijo", rf"^{P}/hijo$")])
    caso(ctx, "create: una instantánea con @",
         [f"cd Local/{P}/datos", "create @s2"],
         estado=[("la instantánea existe", f"zfs list -H -t snapshot -o name {P}/datos@s2",
                  rf"^{P}/datos@s2$")])
    caso(ctx, "destroy: la instantánea se va",
         [f"cd Local/{P}/datos", "destroy @s2"],
         estado=[("ya no existe", f"zfs list -H -t snapshot -o name {P}/datos@s2 2>&1",
                  r"does not exist|no existe")])
    caso(ctx, "rename: el dataset cambia de nombre",
         [f"cd Local/{P}/hijo", "rename hijo2"],
         estado=[("el nuevo existe", f"zfs list -H -o name {P}/hijo2", rf"^{P}/hijo2$"),
                 ("el viejo ya no", f"zfs list -H -o name {P}/hijo 2>&1",
                  r"does not exist|no existe")])
    caso(ctx, "destroy: el dataset se va",
         [f"cd Local/{P}/hijo2", "destroy"],
         estado=[("ya no existe", f"zfs list -H -o name {P}/hijo2 2>&1",
                  r"does not exist|no existe")])


def casos_propiedades(ctx):
    P = POOL_LOCAL
    prepara(ctx)   # terreno limpio: ningún grupo hereda lo que dejó otro
    caso(ctx, "set: escribe una propiedad y ZFS la tiene",
         [f"cd Local/{P}/datos", "set compression=zstd"],
         estado=[("compression=zstd", f"zfs get -H -o value compression {P}/datos", r"^zstd$")])
    caso(ctx, "set: varias propiedades de una vez",
         [f"cd Local/{P}/datos", "set atime=off readonly=off"],
         estado=[("atime=off", f"zfs get -H -o value atime {P}/datos", r"^off$"),
                 ("readonly=off", f"zfs get -H -o value readonly {P}/datos", r"^off$")])


def casos_montaje(ctx):
    P = POOL_LOCAL
    prepara(ctx)   # terreno limpio: ningún grupo hereda lo que dejó otro
    caso(ctx, "unmount: el dataset queda desmontado",
         [f"cd Local/{P}/datos", "unmount"],
         estado=[("mounted=no", f"zfs get -H -o value mounted {P}/datos", r"^no$")])
    caso(ctx, "mount: y vuelve a montarse",
         [f"cd Local/{P}/datos", "mount"],
         estado=[("mounted=yes", f"zfs get -H -o value mounted {P}/datos", r"^yes$")])


def casos_instantaneas(ctx):
    P = POOL_LOCAL
    prepara(ctx)   # terreno limpio: ningún grupo hereda lo que dejó otro
    caso(ctx, "hold: la retención queda puesta",
         [f"cd Local/{P}/datos@s1", "hold prueba"],
         estado=[("el hold existe", f"zfs holds -H {P}/datos@s1", r"prueba")])
    caso(ctx, "holds: y se ve desde el CLI",
         [f"cd Local/{P}/datos@s1", "holds"], espera=[r"prueba"])
    caso(ctx, "release: la retención se suelta",
         [f"cd Local/{P}/datos@s1", "release prueba"],
         estado=[("ya no hay hold", f"zfs holds -H {P}/datos@s1", r"^$")])
    caso(ctx, "clone: se crea un dataset desde la instantánea",
         [f"cd Local/{P}/datos", "clone clonado --from @s1"],
         estado=[("el clon existe", f"zfs list -H -o name {P}/datos/clonado",
                  rf"^{P}/datos/clonado$"),
                 ("y viene de la instantánea",
                  f"zfs get -H -o value origin {P}/datos/clonado", rf"{P}/datos@s1")])
    caso(ctx, "promote: el clon deja de depender del original",
         [f"cd Local/{P}/datos/clonado", "promote"],
         estado=[("sin origen", f"zfs get -H -o value origin {P}/datos/clonado", r"^-$")])


def casos_pool(ctx):
    P = POOL_LOCAL
    caso(ctx, "status: el pool sale sano",
         [f"cd Local/{P}", "status"], espera=[r"ONLINE"])
    caso(ctx, "history: el pool cuenta lo que se le ha hecho",
         [f"cd Local/{P}", "history"], espera=[r"zpool create|zfs create"])
    caso(ctx, "flush: fuerza las escrituras pendientes",
         [f"cd Local/{P}", "flush"], rc=0)
    caso(ctx, "devices: enseña los discos de la máquina",
         ["cd Local", "devices"], espera=[r"NAME|PATH|SIZE"])


def casos_diff_rollback(ctx):
    P = POOL_LOCAL
    prepara(ctx)   # terreno limpio: ningún grupo hereda lo que dejó otro
    caso(ctx, "diff: cuenta lo que cambió entre dos instantáneas",
         [f"cd Local/{P}/datos", "create @s2"],
         estado=[("@s2 existe", f"zfs list -H -t snapshot -o name {P}/datos@s2",
                  rf"{P}/datos@s2")])
    # Un cambio real entre las dos, para que el diff tenga algo que contar.
    sh(ctx, f"bash -c 'echo segundo > /mnt/{P}/datos/nuevo.txt'", root=True)
    sh(ctx, f"zfs snapshot {P}/datos@s3", root=True)
    caso(ctx, "diff: el fichero nuevo aparece",
         [f"cd Local/{P}/datos", "diff @s3 --from @s2"], espera=[r"nuevo\.txt"])
    caso(ctx, "rollback: sin -r se niega si hay instantáneas posteriores",
         [f"cd Local/{P}/datos", "rollback @s2"],
         espera=[r"more recent snapshots|-r"])
    caso(ctx, "rollback: con -r vuelve y se pierde lo de después",
         [f"cd Local/{P}/datos", "rollback @s2 -r"],
         estado=[("el fichero posterior ya no está",
                  f"bash -c 'ls /mnt/{P}/datos/nuevo.txt 2>&1'", r"No such file|No existe")])


def casos_permisos(ctx):
    P = POOL_LOCAL
    prepara(ctx)   # terreno limpio: ningún grupo hereda lo que dejó otro
    caso(ctx, "allow: delega un permiso y ZFS lo tiene",
         [f"cd Local/{P}/datos", "allow --user linarese snapshot"],
         estado=[("zfs allow lo enseña", f"zfs allow {P}/datos", r"linarese.*snapshot")])
    caso(ctx, "allow: sin argumentos LISTA lo delegado",
         [f"cd Local/{P}/datos", "allow"], espera=[r"linarese"])
    caso(ctx, "unallow: lo revoca",
         [f"cd Local/{P}/datos", "unallow --user linarese snapshot"],
         no_espera=[r"error"],
         estado=[("ya no aparece", f"zfs allow {P}/datos", r"^(?!.*linarese snapshot).*$")])


def casos_programacion(ctx):
    P = POOL_LOCAL
    prepara(ctx)   # terreno limpio: ningún grupo hereda lo que dejó otro
    caso(ctx, "schedule: programa instantáneas diarias",
         [f"cd Local/{P}/datos", "schedule --daily 7"],
         estado=[("la propiedad GSA queda puesta",
                  f"zfs get -H -o value org.fc16.gsa:diario {P}/datos", r"^7$")])
    caso(ctx, "schedule: sin argumentos la enseña",
         [f"cd Local/{P}/datos", "schedule"], espera=[r"7"])
    caso(ctx, "schedule --clear: la borra",
         [f"cd Local/{P}/datos", "schedule --clear"],
         estado=[("ya no hay programación",
                  f"zfs get -H -o value org.fc16.gsa:diario {P}/datos", r"^-$")])


def casos_acciones(ctx):
    P = POOL_LOCAL
    prepara(ctx)   # terreno limpio: ningún grupo hereda lo que dejó otro
    sh(ctx, f"bash -c 'mkdir -p /mnt/{P}/datos/subdir && echo x > /mnt/{P}/datos/subdir/f.txt'",
       root=True)
    caso(ctx, "breakdown: un subdirectorio se convierte en dataset hijo",
         [f"cd Local/{P}/datos", "breakdown subdir hijosub --wait"],
         estado=[("el dataset hijo existe", f"zfs list -H -o name {P}/datos/hijosub",
                  rf"^{P}/datos/hijosub$"),
                 ("y conserva el fichero",
                  f"bash -c 'cat /mnt/{P}/datos/subdir/f.txt 2>&1'", r"^x$")])
    caso(ctx, "assemble: y vuelve a ser un directorio (nombre COMPLETO)",
         [f"cd Local/{P}/datos", f"assemble {P}/datos/hijosub --wait"],
         estado=[("el dataset ya no existe", f"zfs list -H -o name {P}/datos/hijosub 2>&1",
                  r"does not exist|no existe")])
    caso(ctx, "todir: vuelca el dataset en un directorio llano",
         [f"cd Local/{P}/datos", "todir /var/tmp/zfsmgr-pruebas/volcado --wait"],
         estado=[("el fichero está en el directorio",
                  "bash -c 'cat /var/tmp/zfsmgr-pruebas/volcado/fichero.txt 2>&1'",
                  r"hola")])
    sh(ctx, f"zfs mount {P}/datos", root=True)
    caso(ctx, "todir: deja el origen con canmount=off (efecto lateral, sin avisar)",
         [], estado=[("canmount quedó en off",
                      f"zfs get -H -o value canmount {P}/datos", r"^off$")])
    sh(ctx, f"zfs set canmount=on {P}/datos", root=True)
    sh(ctx, f"zfs mount {P}/datos", root=True)
    caso(ctx, "rsync: sincroniza ficheros entre dos datasets montados",
         [f"cd Local/{P}", "create destinorsync",
          f"cd Local/{P}/datos", "rsync zfsm://local/{}/destinorsync --wait".format(P)],
         estado=[("el fichero llegó",
                  f"bash -c 'cat /mnt/{P}/destinorsync/fichero.txt 2>&1'", r"hola")])


def casos_pool_export(ctx):
    P = POOL_LOCAL
    prepara(ctx)   # terreno limpio: ningún grupo hereda lo que dejó otro
    caso(ctx, "export: el pool se suelta",
         [f"cd Local/{P}", "export"],
         estado=[("ya no está importado", "zpool list -H -o name", rf"^(?!.*{P}).*$")])
    # Dos comprobaciones, no una. Que LISTE, y que liste el pool UNA sola vez: la sonda pega
    # las salidas de `zpool import` y `zpool import -s`, y antes se volcaban en crudo, con lo
    # que cada pool salía en dos bloques idénticos de siete líneas. `no_espera` sola no lo
    # veía —el listado sí estaba— y por eso el caso pasaba con el defecto dentro.
    caso(ctx, "import: sin nombre lista los importables (la ayuda lo promete)",
         ["cd Local", "import"], no_espera=[r"falta <"])
    # La cuenta va sobre la CABECERA, no sobre un pool concreto: `ztfc16` está sobre fichero
    # en /var/tmp y `zpool import` sin `-d` no mira ahí, así que el único pool que puede
    # salir aquí es el que tenga la máquina de verdad —y eso cambia de una máquina a otra—.
    # La cabecera sale siempre y sale UNA vez; si el listado se volviera a imprimir doble,
    # saldría dos. Y el volcado crudo de la sonda no tiene cabecera ninguna.
    caso(ctx, "import: el listado es una tabla, y una sola",
         ["cd Local", "import"],
         veces=[(r"^POOL\s+ID\s+(ESTADO|STATE)\s+(MOTIVO|REASON)", 1)],
         no_espera=[r"^config:", r"^\s+state: "])
    # Se devuelve por shell: `zpool import` sin `-d` no mira en /var/tmp, y el CLI no
    # ofrece esa opción. Si no se recupera, todo lo que venga después falla sin motivo.
    sh(ctx, f"zpool import -d {IMG_DIR} {P}", root=True)
    sh(ctx, f"zfs mount -a", root=True)


def casos_entre_maquinas(ctx):
    """Lo que de verdad importa: fc16 <-> unib, las dos direcciones."""
    L, R = POOL_LOCAL, POOL_REMOTO
    prepara(ctx)
    prepara(ctx, remoto=True)
    # Instantánea propia. Depender de la que dejó otro grupo es frágil: `promote` TRASLADA
    # las instantáneas al clon —comportamiento correcto de ZFS— y dejaba estas pruebas sin
    # origen por algo que no tenía nada que ver con ellas.
    sh(ctx, f"zfs snapshot -r {L}/datos@env", root=True)
    sh(ctx, f"zfs snapshot -r {R}/datos@env", True, root=True)
    caso(ctx, "send: fc16 -> unib, completo",
         [f"cd Local/{L}/datos@env", f"send zfsm://unib/{R}/de_fc16 --wait"],
         espera=[r"done"], no_espera=[r"failed|no movió"],
         estado=[("el dataset está en unib", f"zfs list -H -o name {R}/de_fc16",
                  rf"^{R}/de_fc16$", True),
                 ("con el fichero dentro",
                  f"bash -c 'zfs mount {R}/de_fc16 2>/dev/null; cat /mnt/{R}/de_fc16/fichero.txt'",
                  r"hola desde fc16", True)])
    caso(ctx, "send: unib -> fc16, completo",
         [f"cd unib/{R}/datos@env", f"send zfsm://local/{L}/de_unib --wait"],
         espera=[r"done"], no_espera=[r"failed|no movió"],
         estado=[("el dataset está en fc16", f"zfs list -H -o name {L}/de_unib",
                  rf"^{L}/de_unib$"),
                 ("con el fichero dentro",
                  f"bash -c 'zfs mount {L}/de_unib 2>/dev/null; cat /mnt/{L}/de_unib/fichero.txt'",
                  r"hola desde unib")])
    # Incremental: solo lo que cambió desde la instantánea base. Es «Nivelar».
    sh(ctx, f"bash -c 'echo incremental > /mnt/{L}/datos/inc.txt'", root=True)
    sh(ctx, f"zfs snapshot {L}/datos@env2", root=True)
    caso(ctx, "send --base: incremental entre máquinas",
         [f"cd Local/{L}/datos@env2", f"send zfsm://unib/{R}/de_fc16 --base @env --wait"],
         espera=[r"done"], no_espera=[r"failed"],
         estado=[("el fichero nuevo llegó a unib",
                  f"bash -c 'zfs mount {R}/de_fc16 2>/dev/null; cat /mnt/{R}/de_fc16/inc.txt'",
                  r"incremental", True)])
    caso(ctx, "jobs: la máquina enseña sus trabajos",
         ["cd Local", "jobs --all"], espera=[r"done|failed|ID|JOB"])


# `casos_pool_export` va el ÚLTIMO de los locales A PROPÓSITO: exporta el pool, y si algo
# sale mal ahí, todo lo que viniera detrás fallaría por no tener pool y no por su culpa.
# En la primera pasada pasó justo eso y escondió el resultado de las transferencias.
def casos_ayuda(ctx):
    """`help` a secas es un ÍNDICE: categorías y nombres, una línea por categoría."""
    rc, salida = cli(ctx, ["help"])
    lineas = salida.splitlines()
    nombre = "help: es un índice corto, no el manual entero"
    problemas = []
    # Sacaba la ficha completa de las 50 y pico órdenes: 239 líneas, diez pantallazos en un
    # terminal normal. El número exacto da igual; lo que no puede es volver a ser eso.
    if len(lineas) > 40:
        problemas.append(f"{len(lineas)} líneas; `help` volvió a soltar el manual entero")
    if len(lineas) < 10:
        problemas.append(f"solo {len(lineas)} líneas: no se ha leído nada")
    if problemas:
        ctx.fallos.append((nombre, problemas, salida[:400]))
        print(f"  FALLO  {nombre}")
        for x in problemas: print(f"         {x}")
    else:
        ctx.pasados.append(nombre); print(f"  ok     {nombre}")

    # Ninguna categoría DOS veces. El defecto era del renderizado, no de los datos: la
    # cabecera se sacaba cuando el grupo cambiaba respecto de la orden anterior, así que un
    # grupo partido en dos sitios de la tabla salía dos veces —«Conexiones» lo hacía— y
    # desde fuera eso son dos categorías distintas que se llaman igual.
    cats = [l.split(":")[0] for l in lineas if re.match(r"^[^ ].*?: \S", l)]
    repes = sorted({c for c in cats if cats.count(c) > 1})
    nombre2 = "help: ninguna categoría aparece dos veces"
    if repes or len(cats) < 5:
        motivo = (f"repetidas: {repes}" if repes
                  else f"solo se han reconocido {len(cats)} categorías: no se ha medido nada")
        ctx.fallos.append((nombre2, [motivo], salida[:400]))
        print(f"  FALLO  {nombre2}"); print(f"         {motivo}")
    else:
        ctx.pasados.append(nombre2); print(f"  ok     {nombre2}")

    # Y dónde cae cada orden. `schedule`/`schedules` programan INSTANTÁNEAS, así que su
    # categoría es la de las instantáneas y no la de los datasets.
    caso(ctx, "help: schedule y schedules van con los snapshots",
         ["help"], idioma="en",
         espera=[r"^Snapshots:.*\bschedule\b", r"^Snapshots:.*\bschedules\b"],
         no_espera=[r"^Datasets:.*\bschedules?\b"])
    # «snapshot» es un término de OpenZFS: no se traduce, y en CASTELLANO es donde eso se
    # comprueba —en inglés el rótulo ya decía «Snapshots» y el caso pasaba sin mirar nada—.
    caso(ctx, "help: en castellano la categoría es Snapshots, no Instantáneas",
         ["help"], idioma="es", espera=[r"^Snapshots:"], no_espera=[r"^Instant"])
    caso(ctx, "help: y en inglés también, que es de donde viene el nombre",
         ["help"], idioma="en", espera=[r"^Snapshots:"])


def casos_nombres_que_chocan(ctx):
    """Cuando el pool se llama IGUAL que la conexión o que el dataset donde estás.

    No es un caso rebuscado: la conexión a una máquina suele llamarse como la máquina, y el
    pool principal de esa máquina también. Con `fc16` visto desde mmela pasaba lo peor que
    puede pasar —`cd fc16` estando ya en `zfsm://fc16` no hacía NADA y no decía nada—,
    porque el atajo de «el primer tramo nombra una conexión» resolvía como absoluta desde la
    raíz y aterrizaba donde ya se estaba.

    El pool se crea con el nombre EXACTO de la conexión local para reproducirlo; es sobre
    fichero y se destruye al terminar.
    """
    choque = "local"          # el identificador de la conexión Local
    img = f"{IMG_DIR}/{choque}.img"
    sh(ctx, f"mkdir -p {IMG_DIR}")
    sh(ctx, f"zpool destroy {choque}", root=True)
    sh(ctx, f"rm -rf /mnt/{choque}", root=True)
    sh(ctx, f"rm -f {img} && truncate -s 200M {img}")
    rc, salida = sh(ctx, f"zpool create -f -m /mnt/{choque} {choque} {img}", root=True)
    if rc != 0:
        print(f"  (saltado: no se pudo crear el pool «{choque}»: {salida.strip()[:80]})")
        return
    sh(ctx, f"zfs create {choque}/{choque}", root=True)
    try:
        # Tres escalones, tres sitios DISTINTOS. Que el tercero sea distinto del segundo es
        # justo lo que se rompía; `pwd` después de cada uno es lo que lo hace visible.
        caso(ctx, "cd: conexión, luego pool del mismo nombre, luego hijo del mismo nombre",
             [f"cd {choque}", "pwd", f"cd {choque}", "pwd", f"cd {choque}", "pwd"],
             espera=[rf"^zfsm://{choque}$",
                     rf"^zfsm://{choque}/{choque}$",
                     rf"^zfsm://{choque}/{choque}/{choque}$"])
        # La forma explícita tiene que llevar al MISMO sitio que la corta: si no, una de las
        # dos miente.
        caso(ctx, "cd: «./nombre» y la ruta absoluta llevan donde la forma corta",
             [f"cd {choque}", f"cd ./{choque}", "pwd", f"cd /{choque}/{choque}", "pwd"],
             veces=[(rf"^zfsm://{choque}/{choque}$", 2)])
        # Y lo que NO se puede romper al arreglar lo anterior: desde un dataset hondo, el
        # nombre del pool sigue siendo el nombre ZFS completo, que sí mueve.
        caso(ctx, "cd: desde un hijo, el nombre del pool sigue siendo el nombre completo",
             [f"cd /{choque}/{choque}/{choque}", f"cd {choque}", "pwd"],
             espera=[rf"^zfsm://{choque}/{choque}$"])
    finally:
        sh(ctx, f"zpool destroy {choque}", root=True)
        sh(ctx, f"rm -f {img}", root=True)
        sh(ctx, f"rm -rf /mnt/{choque}", root=True)


def casos_arranque(ctx):
    """Lo que pasa antes de que haya sesión: ayuda, idioma y opciones mal escritas."""
    caso_argv(ctx, "ayuda: --lang en la da EN INGLÉS",
              ["--lang", "en", "--help"], rc=0,
              espera=[r"^Usage: zfsmgr-cli"], no_espera=[r"^Uso: zfsmgr-cli"])
    caso_argv(ctx, "ayuda: --lang es la da en castellano",
              ["--lang", "es", "--help"], rc=0,
              espera=[r"^Uso: zfsmgr-cli"], no_espera=[r"^Usage: zfsmgr-cli"])
    # Lo mismo por el otro camino: el idioma también decide el mensaje de error, y ahí sí
    # se resolvía antes. Se comprueba para que la corrección no rompa lo que ya iba bien.
    caso_argv(ctx, "una opción que no existe: error en inglés y rc=2",
              ["--lang", "en", "--inventada"], rc=2, espera=[r"unknown option"])
    caso_argv(ctx, "version: sale y termina con 0",
              ["version"], rc=0, espera=[r"zfsmgr-cli \d+\.\d+"])

    # Las dos ayudas tienen que describir LAS MISMAS opciones.
    #
    # La ayuda entera es UNA cadena traducible, así que el catálogo inglés puede quedarse
    # corto sin que falte ninguna clave y sin que nada avise: a la versión inglesa le
    # faltaba la línea de `--lang`, precisamente la opción con la que se pide el inglés.
    # Contar claves no lo veía; hay que mirar DENTRO del texto.
    salidas = {}
    for idioma in ("es", "en"):
        r = subprocess.run([CLI, "--lang", idioma, "--help"], capture_output=True,
                           text=True, timeout=60)
        salidas[idioma] = set(re.findall(r"^  (-{1,2}[a-z-]+)", r.stdout + r.stderr, re.M))
    nombre = "ayuda: las dos lenguas documentan las mismas opciones"
    problemas = []
    # **La guarda del conjunto vacío, y no es de adorno.** La primera versión de esto miraba
    # solo `stdout`, y la ayuda salía por `stderr`: los dos conjuntos venían vacíos, «es» y
    # «en» coincidían perfectamente y el caso daba OK con la línea de `--lang` borrada a
    # mano de la traducción. Dos conjuntos vacíos son iguales; eso no es que la prueba pase,
    # es que no ha medido nada.
    if len(salidas["es"]) < 5 or len(salidas["en"]) < 5:
        problemas.append(f"no se ha leído la ayuda: es={len(salidas['es'])} "
                         f"opciones, en={len(salidas['en'])}")
    faltan = salidas["es"] ^ salidas["en"]
    if faltan:
        problemas.append(f"solo en una de las dos lenguas: {sorted(faltan)}")
    if problemas:
        ctx.fallos.append((nombre, problemas, ""))
        print(f"  FALLO  {nombre}")
        for x in problemas:
            print(f"         {x}")
    else:
        ctx.pasados.append(nombre)
        print(f"  ok     {nombre}")

    # Y a dónde va cada una: la pedida a stdout con 0, la del error a stderr con 2.
    pedida = subprocess.run([CLI, "--help"], capture_output=True, text=True, timeout=60)
    mala = subprocess.run([CLI, "--inventada"], capture_output=True, text=True, timeout=60)
    nombre2 = "ayuda: la pedida va a stdout; la del error, a stderr"
    problemas2 = []
    if "Uso:" not in pedida.stdout and "Usage:" not in pedida.stdout:
        problemas2.append("`--help` no escribe la ayuda en stdout: «zfsmgr-cli --help | less» "
                          "no enseñaría nada")
    if pedida.returncode != 0:
        problemas2.append(f"`--help` sale con {pedida.returncode}, esperaba 0")
    if "Uso:" not in mala.stderr and "Usage:" not in mala.stderr:
        problemas2.append("una opción mala no escribe la ayuda en stderr")
    if mala.returncode != 2:
        problemas2.append(f"una opción mala sale con {mala.returncode}, esperaba 2")
    if problemas2:
        ctx.fallos.append((nombre2, problemas2, ""))
        print(f"  FALLO  {nombre2}")
        for x in problemas2:
            print(f"         {x}")
    else:
        ctx.pasados.append(nombre2)
        print(f"  ok     {nombre2}")


# `casos_pool_export` exporta e importa el pool LOCAL: no necesita segunda máquina, y
# estaba en la lista que sí. Sin `--remoto` no había manera de ejecutarlo, ni siquiera con
# `--solo`, y eso es justo lo que uno quiere cuando está mirando un defecto de `import`.
LOCALES = [casos_arranque, casos_ayuda, casos_nombres_que_chocan, casos_navegacion, casos_lectura, casos_crear_destruir,
           casos_propiedades, casos_montaje, casos_instantaneas, casos_pool,
           casos_diff_rollback, casos_permisos, casos_programacion,
           casos_acciones, casos_pool_export]
ENTRE_MAQUINAS = [casos_entre_maquinas]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--maestra-fd", type=int, required=True)
    ap.add_argument("--sudo-fd", type=int, required=True)
    ap.add_argument("--remoto", default=None, help="usuario@host de la segunda máquina")
    ap.add_argument("--solo", default=None, help="solo los casos cuyo nombre case")
    ap.add_argument("--sin-limpieza", action="store_true")
    args = ap.parse_args()

    with os.fdopen(args.maestra_fd) as f:
        maestra = f.readline().strip()
    with os.fdopen(args.sudo_fd) as f:
        sudo = f.readline().strip()
    ctx = Contexto(maestra, sudo, args.remoto)

    if not os.path.exists(CLI):
        raise SystemExit(f"no está {CLI}")

    global caso
    if args.solo:
        original = caso
        def caso(ctx, nombre, *a, **k):
            if re.search(args.solo, nombre):
                return original(ctx, nombre, *a, **k)
            return True

    print("Preparando…")
    prepara(ctx)
    if ctx.remoto:
        prepara(ctx, remoto=True)
    try:
        grupos = list(LOCALES) + (ENTRE_MAQUINAS if ctx.remoto else [])
        for grupo in grupos:
            print(f"\n{grupo.__name__}:")
            grupo(ctx)
    finally:
        if not args.sin_limpieza:
            limpia(ctx)
            if ctx.remoto:
                limpia(ctx, remoto=True)

    print(f"\n{'='*60}")
    print(f"{len(ctx.pasados)} pasados, {len(ctx.fallos)} fallos")
    for nombre, problemas, salida in ctx.fallos:
        print(f"\n--- {nombre}")
        for p in problemas:
            print(f"    {p}")
        # La salida del CLI, que es lo que dice el fallo de verdad. Sin esto solo se sabe
        # que la comprobación no casó, no POR QUÉ.
        recorte = salida.strip().splitlines()[:6]
        for linea in recorte:
            print(f"    | {linea[:150]}")
    return 1 if ctx.fallos else 0


if __name__ == "__main__":
    sys.exit(main())
