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
import json
import os
import re
import time
import shlex
import shutil
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


# Órdenes que actúan sobre TODOS los pools de la máquina, no solo sobre los de prueba. La
# batería corre en máquinas con pools de verdad —en fc16, entornos de arranque con /usr y
# /var heredados— y una de estas los toca. Se rechazan antes de llegar al shell.
ORDENES_GLOBALES = re.compile(
    r"\b(zfs\s+(mount|unmount|umount|load-key|unload-key)|zpool\s+(import|export))\b"
    r"[^;|&]*\s-[a-zA-Z]*a\b"
    r"|\bzfs\s+(mount|unmount|umount)\s*$")


def sh(ctx, orden, en_remoto=False, root=False):
    """Una orden de shell en fc16 o en unib. Devuelve (rc, salida)."""
    if ORDENES_GLOBALES.search(orden):
        raise SystemExit(f"orden global rechazada en la batería: {orden}")
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


def cli(ctx, ordenes, timeout=180, idioma=None, frase=None):
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
    # `frase`: un secreto para la orden (`load-key --password-fd 8`). Por fichero 0600 y
    # descriptor, igual que la maestra: nunca por argumento ni por entorno.
    extra = ""
    if frase is not None:
        ruta = os.path.join(ctx.dir_tmp, ".frase")
        with open(os.open(ruta, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
            f.write(frase + "\n")
        extra = f"exec 8<{ruta}; "
    orden = f"{extra}exec 9<{fichero}; exec {CLI!r} --password-fd 9 --format text{lang} -y"
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
         idioma=None, frase=None):
    """Un caso: órdenes al CLI, y después comprobaciones contra la máquina.

    `veces` son pares (patrón, n): el patrón tiene que aparecer EXACTAMENTE n veces. Existe
    porque `espera` da por bueno «una o más», y eso se tragó un listado que salía DOBLE: la
    sonda de importables ejecuta `zpool import` y `zpool import -s` y pega las dos salidas,
    así que cada pool se enumeraba dos veces. Con un solo pool de prueba se lee como un
    listado raro; con varios, no hay forma de saber si son dos pools homónimos o el mismo
    contado dos veces.
    """
    rc_real, salida = cli(ctx, list(ordenes), idioma=idioma, frase=frase)
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


def a_bytes(texto):
    """«292M» → 306184192. Los mismos 1024 y las mismas unidades que usa `zfs`.

    Se escribe aquí a mano A PROPÓSITO, sin llamar a nada del proyecto: si la batería
    usara el mismo conversor que el programa, un error en él se cancelaría solo y las
    dos partes coincidirían en estar mal.
    """
    t = (texto or "").strip()
    if not t:
        return None
    unidades = "BKMGTPE"
    sufijo = t[-1].upper()
    if sufijo in unidades:
        cuerpo, exp = t[:-1], unidades.index(sufijo)
    else:
        cuerpo, exp = t, 0
    try:
        return int(float(cuerpo) * (1024 ** exp))
    except ValueError:
        return None


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
         espera=[r"^status\s+OK", r"^version\s+\d+\.\d+\.\d+\.\d+", r"^api\s+\d"],
         no_espera=[r"STATUS=OK", r"CAPS="])


def casos_crear_destruir(ctx):
    P = POOL_LOCAL
    prepara(ctx)   # terreno limpio: ningún grupo hereda lo que dejó otro
    caso(ctx, "create: un dataset hijo",
         [f"cd Local/{P}", "create hijo"],
         estado=[("el dataset existe", f"zfs list -H -o name {P}/hijo", rf"^{P}/hijo$")])
    caso(ctx, "create: una bandera sin valor no se come la propiedad que la sigue",
         [f"cd Local/{P}", "create hijou -u compression=lz4"],
         estado=[("con su propiedad", f"zfs get -H -o value compression {P}/hijou", r"^lz4$")])
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


def casos_volumenes(ctx):
    """Un VOLUMEN no es un sistema de ficheros, y el listado tiene que decirlo.

    El tipo se deducía del NOMBRE —«lleva @, luego instantánea; si no, dataset»— así que lo
    único que se distinguía era la instantánea. Un zvol salía como «dataset», con el punto
    de montaje a «-» como única pista. Visto en fc16: `back/tooling`, 78 G.
    """
    P = POOL_LOCAL
    sh(ctx, f"zfs create -V 64M {P}/vol", root=True)
    sh(ctx, f"zfs create -V 64M -o volmode=none {P}/voloculto", root=True)
    try:
        # `idioma` FIJADO en los tres: afirman un texto, y sin fijarlo la batería usa el de
        # la máquina —inglés aquí—. Ya pasó con el rótulo «Snapshots» y vuelve a pasar: el
        # caso del volumen llegó a dar verde solo porque su traducción inglesa aún no
        # existía y el texto caía al castellano de reserva. Un caso que pasa por una
        # traducción que falta no está midiendo nada.
        caso(ctx, "ls: un volumen sale como «volume» y no como «dataset»",
             [f"ls zfsm://local/{P}"], idioma="es",
             espera=[r"^vol\s+volume\s"],
             no_espera=[r"^vol\s+dataset\s"])
        # Y lo que de verdad importa: que no mande a buscar un montaje que no existe.
        caso(ctx, "ls #content de un volumen lo dice, en vez de «no está montado»",
             [f"ls zfsm://local/{P}/vol#content"], idioma="es",
             espera=[r"VOLUMEN"], no_espera=[r"no está montado"])
        caso(ctx, "ls: y en inglés también dice que es un volumen",
             [f"ls zfsm://local/{P}/vol#content"], idioma="en",
             espera=[r"VOLUME"], no_espera=[r"not mounted"])
        # --- La columna ESTADO: una sola pregunta para las dos clases de cosa.
        #
        # Antes era MONTADO y para un volumen decía «-»: nada. Lo que un volumen tiene no es
        # montaje sino DISPOSITIVO. NO se usa `volmode` para decirlo —su valor habitual es
        # `default`, «lo que diga el parámetro del módulo», que no contesta la pregunta—: el
        # agente mira si existe el nodo en /dev/zvol, que es un hecho comprobable.
        caso(ctx, "ls: un volumen expuesto sale como «device»",
             [f"ls zfsm://local/{P}"], idioma="es", espera=[r"^vol\s+volume\s+\S+\s+\S+\s+device\b"])
        caso(ctx, "ls: uno con volmode=none sale como «no-device»",
             [f"ls zfsm://local/{P}"], idioma="es",
             espera=[r"^voloculto\s+volume\s+\S+\s+\S+\s+no-device\b"])
        caso(ctx, "ls: un dataset montado sale como «mounted» y no como «sí»",
             [f"ls zfsm://local/{P}"], idioma="es",
             espera=[r"^datos\s+dataset\s+\S+\s+\S+\s+mounted\b"],
             no_espera=[r"^datos\s+dataset\s+\S+\s+\S+\s+(sí|yes)\b"])
        # El campo de json se llama ahora `status`, NO `mounted`: su significado cambió, y un
        # guion que leyera el viejo tiene que encontrárselo ausente en vez de con otro valor.
        rc, js = cli(ctx, ["format json", f"ls zfsm://local/{P}"])
        nombre = "ls: en json el campo es «status» y «mounted» ya no está"
        problemas = []
        try:
            filas = json.loads(js)["entries"]
            if not any("status" in f for f in filas):
                problemas.append("no hay campo «status»")
            if any("mounted" in f for f in filas):
                problemas.append("sigue habiendo «mounted», que ya no significa lo mismo")
            vol = next((f for f in filas if f.get("type") == "volume"), None)
            if vol is None or vol.get("status") not in ("device", "no-device"):
                problemas.append(f"el volumen no trae un estado útil: {vol}")
        except Exception as exc:          # noqa: BLE001
            problemas.append(f"json ilegible: {exc}")
        if problemas:
            ctx.fallos.append((nombre, problemas, js[:300]))
            print(f"  FALLO  {nombre}")
            for x in problemas:
                print(f"         {x}")
        else:
            ctx.pasados.append(nombre)
            print(f"  ok     {nombre}")

        # Un volumen trae consigo un dispositivo SIN ALIAS, y ese es el único momento en que
        # la batería tiene uno a mano. El caso de `devices` que exige una lista en json pasaba
        # sin morder porque todos los discos de esta máquina tienen alias por `by-id`: aquí sí
        # hay uno con cero, que es donde se veía el `null`.
        rc, js = cli(ctx, ["format json", "devices --on zfsm://local"])
        nombre = "devices: un dispositivo sin alias trae [] y no null"
        problemas = []
        try:
            disp = json.loads(js)["devices"]
            zvols = [d for d in disp if d.get("path", "").startswith("/dev/zd")]
            if not zvols:
                problemas.append("no hay ningún /dev/zd*: el caso no está probando nada")
            for d in zvols:
                if not isinstance(d.get("aliases"), list):
                    problemas.append(f"{d['path']}: aliases es {d.get('aliases')!r}, no una lista")
        except Exception as exc:          # noqa: BLE001
            problemas.append(f"json ilegible: {exc}")
        if problemas:
            ctx.fallos.append((nombre, problemas, js[:300]))
            print(f"  FALLO  {nombre}")
            for x in problemas:
                print(f"         {x}")
        else:
            ctx.pasados.append(nombre)
            print(f"  ok     {nombre}")

        # Un dataset sin montar SIGUE diciendo lo suyo: el arreglo no puede tragarse ese caso.
        sh(ctx, f"zfs create -o canmount=off {P}/apagado", root=True)
        caso(ctx, "ls #content de un dataset sin montar sigue diciendo lo de siempre",
             [f"ls zfsm://local/{P}/apagado#content"], idioma="es",
             espera=[r"no está montado"], no_espera=[r"VOLUMEN"])
    finally:
        sh(ctx, f"zfs destroy {P}/vol", root=True)
        sh(ctx, f"zfs destroy {P}/voloculto", root=True)
        sh(ctx, f"zfs destroy {P}/apagado", root=True)


def casos_dedup(ctx):
    """La columna DEDUP del listado de POOLS, sobre un pool que deduplica de verdad.

    Pool APARTE y no el de siempre: `dedup=on` cambia cómo se contabiliza el espacio, y
    dejarlo puesto en el pool común haría que otros casos midieran otra cosa.

    Y con DATOS REPETIDOS de verdad. Un caso que solo comprobara que la columna existe
    pasaría igual con la columna clavada a «1.00», que es lo que sale en cualquier pool
    normal: no distinguiría una lectura correcta de ninguna lectura.
    """
    P = f"{POOL_LOCAL}dd"
    img = f"{IMG_DIR}/{P}.img"
    sh(ctx, f"zpool destroy -f {P}", root=True)
    sh(ctx, f"rm -rf /mnt/{P}", root=True)
    sh(ctx, f"rm -f {img} && truncate -s 1G {img}", root=False)
    rc, salida = sh(ctx, f"zpool create -f -m /mnt/{P} {P} {img}", root=True)
    if rc != 0:
        ctx.fallos.append(("dedup: preparar el pool", [salida.strip()], ""))
        print("  FALLO  dedup: no se pudo crear el pool de pruebas")
        return
    try:
        # Sin compresión: con ella encima, el ahorro que se vea podría ser de la otra.
        sh(ctx, f"zfs set dedup=on {P}", root=True)
        sh(ctx, f"zfs set compression=off {P}", root=True)
        sh(ctx, f"dd if=/dev/urandom of=/mnt/{P}/base.bin bs=1M count=40 status=none", root=True)
        for i in range(1, 4):
            sh(ctx, f"cp /mnt/{P}/base.bin /mnt/{P}/copia{i}.bin", root=True)
        sh(ctx, f"zpool sync {P}", root=True)

        # Lo que diga la MÁQUINA, no lo que diga la herramienta: se le pregunta a `zpool`
        # y el listado tiene que contestar lo mismo.
        rc, crudo = sh(ctx, f"zpool list -H -o dedup {P}", root=True)
        esperado = crudo.strip().rstrip("x")
        nombre = "dedup: la columna DEDUP dice lo mismo que zpool"
        problemas = []
        if not esperado or esperado == "1.00":
            problemas.append(f"el pool no llegó a deduplicar nada (zpool dice {crudo.strip()!r}): "
                             "el caso no estaría probando nada")
        rc2, js = cli(ctx, ["format json", "ls zfsm://local"])
        try:
            pools = {x["name"]: x for x in json.loads(js)["pools"]}
            fila = pools.get(P)
            if fila is None:
                problemas.append(f"{P} no sale en el listado")
            elif fila.get("dedupratio") != esperado:
                problemas.append(f"dedupratio={fila.get('dedupratio')!r}, y zpool dice {esperado!r}")
        except Exception as exc:          # noqa: BLE001
            problemas.append(f"json ilegible: {exc}")
        if problemas:
            ctx.fallos.append((nombre, problemas, js[:300]))
            print(f"  FALLO  {nombre}")
            for x in problemas:
                print(f"         {x}")
        else:
            ctx.pasados.append(nombre)
            print(f"  ok     {nombre}  (zpool: {esperado}x)")

        # La RAM que cuesta la DDT. Es el dato por el que se añadió todo esto: un pool
        # puede tener DEDUP 1.00 —sin ahorrar nada— y estar pagando cientos de MB.
        rc, crudoD = sh(ctx, f"zpool status -D {P}", root=True)
        linea = next((l for l in crudoD.splitlines() if "dedup:" in l), "")
        nombre = "dedup: la RAM de la DDT coincide con zpool status -D"
        problemas = []
        if "DDT entries" not in linea:
            problemas.append(f"zpool no da la linea de la DDT: {linea!r}")
        else:
            # «dedup: DDT entries N, size X on disk, Y in core»
            enMemoria = linea.split(",")[-1].replace("in core", "").strip()
            ramEsperada = a_bytes(enMemoria)
            if ramEsperada is None:
                problemas.append(f"no se entiende el tamanyo {enMemoria!r}")
            else:
                try:
                    fila = {x["name"]: x for x in json.loads(js)["pools"]}[P]
                except Exception as exc:          # noqa: BLE001
                    fila, _ = {}, problemas.append(f"json ilegible: {exc}")
                visto = fila.get("ddtmemory")
                if visto is None:
                    problemas.append("no hay campo «ddtmemory» para un pool CON deduplicacion")
                # En bytes exactos en json, y redondeo de la unidad: se admite un 2%.
                elif abs(visto - ramEsperada) > max(4096, ramEsperada * 0.02):
                    problemas.append(f"ddtmemory={visto}, y zpool dice {enMemoria} ({ramEsperada})")
        if problemas:
            ctx.fallos.append((nombre, problemas, linea))
            print(f"  FALLO  {nombre}")
            for x in problemas:
                print(f"         {x}")
        else:
            ctx.pasados.append(nombre)
            print(f"  ok     {nombre}  ({linea.split(',')[-1].strip()})")

        # Y en texto, con su encabezado: que el campo exista en json no quiere decir que
        # haya columna que mirar.
        caso(ctx, "dedup: en texto sale la columna y su valor",
             ["ls zfsm://local"], idioma="es",
             espera=[r"DEDUP", rf"^{P}\s+\S+\s+\S+\s+\S+\s+\S+\s+" + esperado.replace(".", r"\.")])
        # Un pool SIN deduplicacion deja la celda VACIA, no a cero. Cero diria que la
        # tabla existe y no cuesta nada, que es una afirmacion distinta de «no hay tabla».
        caso(ctx, "dedup: un pool sin DDT deja la RAM en blanco, no en cero",
             ["ls zfsm://local"], idioma="es",
             no_espera=[rf"^{POOL_LOCAL}\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+0B\b"])
        rc, js2 = cli(ctx, ["format json", "ls zfsm://local"])
        nombre = "dedup: sin DDT el campo de RAM es null y no 0"
        try:
            fila = {x["name"]: x for x in json.loads(js2)["pools"]}[POOL_LOCAL]
            if fila.get("ddtmemory") is None:
                ctx.pasados.append(nombre)
                print(f"  ok     {nombre}")
            else:
                ctx.fallos.append((nombre, [f"ddtmemory={fila.get('ddtmemory')!r}"], js2[:200]))
                print(f"  FALLO  {nombre}")
                print(f"         ddtmemory={fila.get('ddtmemory')!r}, y ese pool no tiene DDT")
        except Exception as exc:          # noqa: BLE001
            ctx.fallos.append((nombre, [f"json ilegible: {exc}"], js2[:200]))
            print(f"  FALLO  {nombre}")
    finally:
        sh(ctx, f"zpool destroy -f {P}", root=True)
        sh(ctx, f"rm -rf /mnt/{P}", root=True)
        sh(ctx, f"rm -f {img}", root=False)


def casos_patrones(ctx):
    """Comodines en las URL: `*`, `?`, `[..]` y `{a,b}`. Ver «help patrones».

    Lo que de verdad importa aquí son las órdenes que MODIFICAN: que borren exactamente lo
    que casa y nada más, que se nieguen donde deben, y que sin coincidencias no hagan nada.
    Por eso cada una se comprueba contra la máquina —presencia Y ausencia—, no contra lo que
    dice la herramienta.
    """
    P = POOL_LOCAL
    prepara(ctx)
    for h in ("hijoA", "hijoB", "otro"):
        sh(ctx, f"zfs create {P}/{h}", root=True)
    for sn in ("auto-1", "auto-2", "manual"):
        sh(ctx, f"zfs snapshot {P}/datos@{sn}", root=True)
    sh(ctx, f"bash -c 'touch \"/mnt/{P}/datos/a*b.txt\" /mnt/{P}/datos/axb.txt'", root=True)
    U = f"zfsm://local/{P}"
    snaps = f"zfs list -H -t snapshot -o name -r {P}/datos"
    hijos = f"zfs list -H -o name -r {P}"

    # --- ls: el último tramo FILTRA las filas.
    caso(ctx, "patrones: #properties/u* deja solo las que empiezan por u",
         [f"ls {U}#properties/u*"], rc=0, espera=[r"^used\s"],
         no_espera=[r"^compression\s", r"^atime\s"])
    caso(ctx, "patrones: #properties/{a*, b*} con espacio, como se escribe a mano",
         [f"ls {U}#properties/{{a*, b*}}"], rc=0, espera=[r"^atime\s", r"^available\s"],
         no_espera=[r"^used\s"])
    caso(ctx, "patrones: ls de hijos filtra las FILAS, no lista el contenido de cada uno",
         [f"ls {U}/hijo*"], rc=0, espera=[r"^hijoA\s", r"^hijoB\s"],
         no_espera=[r"^otro\s", r"^datos\s"])
    caso(ctx, "patrones: «*» en un tramo de ruta no saca instantáneas",
         [f"ls {U}/*"], rc=0, espera=[r"^hijoA\s", r"^datos\s"], no_espera=[r"^@"])
    caso(ctx, "patrones: ? es exactamente un carácter",
         [f"ls {U}/hijo?"], rc=0, espera=[r"^hijoA\s", r"^hijoB\s"], no_espera=[r"^otro\s"])
    caso(ctx, "patrones: una clase [A] deja solo esa",
         [f"ls {U}/hijo[A]"], rc=0, espera=[r"^hijoA\s"], no_espera=[r"^hijoB\s"])
    caso(ctx, "patrones: @auto-* deja solo esas instantáneas",
         [f"ls {U}/datos@auto-*"], rc=0, espera=[r"^@auto-1\s", r"^@auto-2\s"],
         no_espera=[r"^@manual\s", r"^@s1\s"])
    caso(ctx, "patrones: %2A es un asterisco LITERAL en #content",
         [f"ls {U}/datos#content/a%2A*"], rc=0, espera=[r"^a\*b\.txt\s"],
         no_espera=[r"^axb\.txt\s"])
    caso(ctx, "patrones: sin coincidencias es un ERROR, no una lista vacía",
         [f"ls {U}/zz*"], rc=1, idioma="es", espera=[r"nada coincide con"])

    # --- cd: solo si casa uno.
    caso(ctx, "patrones: cd con un único resultado entra en él",
         [f"cd {U}/ot*", "pwd"], rc=0, espera=[rf"zfsm://local/{P}/otro"])
    caso(ctx, "patrones: cd con varios resultados se niega y los enumera",
         [f"cd {U}/hijo*"], rc=1, idioma="es",
         espera=[r"necesita UN destino", r"hijoA", r"hijoB"])

    # --- Lecturas con varios destinos: UNA tabla con la columna DESTINO.
    caso(ctx, "patrones: get sobre varios destinos junta una sola tabla",
         [f"get {U}/{{hijoA,hijoB}} compression"], rc=0, idioma="es",
         espera=[rf"^local/{P}/hijoA\s+compression", rf"^local/{P}/hijoB\s+compression"],
         veces=[(r"^DESTINO\s", 1)])
    rc, js = cli(ctx, ["format json", f"get {U}/{{hijoA,hijoB}} compression"])
    nombre = "patrones: en json, varios destinos son UN documento con el campo target"
    problemas = []
    try:
        doc = json.loads(js)
        filas = next(iter(doc.values()))
        destinos = sorted({f.get("target") for f in filas})
        if destinos != [f"local/{P}/hijoA", f"local/{P}/hijoB"]:
            problemas.append(f"los target son {destinos}")
    except Exception as exc:          # noqa: BLE001
        problemas.append(f"no es UN documento json: {exc}")
    if problemas:
        ctx.fallos.append((nombre, problemas, js[:300]))
        print(f"  FALLO  {nombre}")
        for x in problemas:
            print(f"         {x}")
    else:
        ctx.pasados.append(nombre)
        print(f"  ok     {nombre}")

    # --- Rechazos: ANTES de tocar nada.
    caso(ctx, "patrones: rename con patrón se rechaza",
         [f"rename {U}/hijo* nuevo"], rc=1, idioma="es", espera=[r"no admite patrones"],
         estado=[("siguen los dos hijos", hijos, rf"{P}/hijoA\n{P}/hijoB")])
    caso(ctx, "patrones: destroy con el patrón en el POOL se rechaza",
         [f"destroy zfsm://local/{P[:3]}*"], rc=1, idioma="es",
         espera=[r"no admite un patrón en esa parte"],
         estado=[("el pool sigue", f"zpool list -H -o name {P}", rf"^{P}$")])
    caso(ctx, "patrones: destroy con el patrón en un tramo intermedio se rechaza",
         [f"destroy {U}/*/x"], rc=1, idioma="es", espera=[r"solo admite el patrón en el último tramo"])
    caso(ctx, "patrones: una lista {…} con «/» dentro se explica",
         [f"ls zfsm://{{local/{P},x}}"], rc=1, idioma="es", espera=[r"no puede contener"])
    caso(ctx, "patrones: destroy sin coincidencias no toca nada",
         [f"destroy {U}/datos@nada-*"], rc=1, idioma="es", espera=[r"nada coincide"],
         estado=[("siguen las cinco", snaps, rf"@auto-1\n{P}/datos@auto-2\n{P}/datos@manual")])

    # --- destroy: --check no toca; sin él, EXACTAMENTE las que casan.
    caso(ctx, "patrones: destroy --check enseña la lista y no toca nada",
         [f"destroy {U}/datos@auto-* --check"], rc=0, idioma="es",
         espera=[r"2 destinos casan", r"no se ha tocado nada"],
         estado=[("siguen las dos", snaps, rf"{P}/datos@auto-1\n{P}/datos@auto-2")])
    caso(ctx, "patrones: destroy borra exactamente las que casan y nada más",
         [f"destroy {U}/datos@auto-*"], rc=0, idioma="es", espera=[r"2 bien, 0 fallaron"],
         estado=[("ya no hay @auto-", snaps, r"\A(?![\s\S]*@auto-)"),
                 ("@manual sigue", snaps, rf"{P}/datos@manual"),
                 ("@s1 sigue", snaps, rf"{P}/datos@s1")])

    # --- set sobre varios: a los que casan, y a nadie más.
    caso(ctx, "patrones: set sobre varios hijos",
         [f"set {U}/{{hijoA,hijoB}} atime=off"], rc=0,
         estado=[("hijoA con atime=off", f"zfs get -H -o value atime {P}/hijoA", r"^off"),
                 ("hijoB con atime=off", f"zfs get -H -o value atime {P}/hijoB", r"^off"),
                 ("otro sin tocar", f"zfs get -H -o value atime {P}/otro", r"^on")])

    # --- Y un error de sintaxis en un guion termina con código distinto de 0. Con el
    # destino DETRÁS de la asignación, `set` decía «uso: …» y la sesión acababa con 0.
    caso(ctx, "un error de sintaxis en un guion termina con código ≠ 0",
         [f"set atime=on {U}/otro"], rc=2,
         estado=[("otro sigue igual", f"zfs get -H -o value atime {P}/otro", r"^on")])


def casos_patrones_copia(ctx):
    """`cp` y `rsync` con patrones, con las reglas del sistema operativo.

    El origen puede casar con varios; el destino solo vale si casa con UNO. Cada caso se
    comprueba en la máquina: que esté lo que se copió Y que no esté lo que no casaba.
    """
    P = POOL_LOCAL
    prepara(ctx)
    for h in ("hijoA", "hijoB", "otro"):
        sh(ctx, f"zfs create {P}/{h}", root=True)
    D = f"/mnt/{P}/datos"
    sh(ctx, f"bash -c 'cd {D} && echo 1 > x1.log && echo 2 > x2.log && echo o > .oculto.log "
            f"&& echo c > c.txt && mkdir -p d1 d2 && echo y1 > d1/y.log && echo y2 > d2/y.log'",
       root=True)
    U = f"zfsm://local/{P}"
    hay = lambda ruta: f"bash -c 'ls -A {ruta} 2>&1'"

    # --- cp: varios orígenes a UN destino.
    caso(ctx, "patrones cp: *.log copia los que casan, sin los ocultos",
         [f"cp {U}/datos#content/*.log {U}/hijoA --wait"], rc=0, idioma="es",
         espera=[r"2 destinos casan"],
         estado=[("hijoA tiene x1.log y x2.log", hay(f"/mnt/{P}/hijoA"), r"\Ax1\.log\nx2\.log\n?\Z")])
    # `jobs -a`: todas las conexiones, desde la raíz, con la columna CONEXIÓN; y --detail
    # dice qué hace cada trabajo (lo compone el agente al recibirlo).
    caso(ctx, "jobs -a: desde la raíz, con la columna CONEXIÓN",
         ["jobs -a --history"], idioma="es", espera=[r"^CONEXIÓN\s+ID\s+ESTADO"])
    caso(ctx, "jobs --all ya no existe: se llama --history",
         ["jobs -a --all"], rc=1, idioma="es", espera=[r"«--all» no es una opción de jobs"])
    # schedules: lo mismo que jobs. `-a` son todas las conexiones, y la columna se llama
    # CONEXIÓN en las dos tablas.
    caso(ctx, "schedules -a: desde la raíz, con la columna CONEXIÓN",
         ["schedules -a"], rc=0, idioma="es", espera=[r"^CONEXIÓN\s+DATASET"])
    caso(ctx, "schedules --all ya no existe: es -a",
         ["schedules --all"], rc=1, idioma="es", espera=[r"«--all» no es una opción de schedules"])
    caso(ctx, "jobs sin -a en la raíz pide una conexión",
         ["jobs"], rc=1, idioma="es", espera=[r"jobs -a"])
    # Varios orígenes en la misma máquina: UN trabajo, no uno por fichero.
    caso(ctx, "patrones cp: varios orígenes sin --wait son UN solo trabajo",
         [f"cp {U}/datos#content/*.log {U}/otro"], rc=0, idioma="es",
         veces=[(r"en marcha como trabajo", 1)])
    caso(ctx, "jobs --detail: el trabajo de cp dice qué copia y adónde",
         [f"jobs --on {U} --history --detail"], idioma="es",
         espera=[r"QUÉ HACE", r"cp /mnt/\S+/datos/x1\.log -> /mnt/\S+/otro/x1\.log \(\+1\)"])
    caso(ctx, "patrones cp: dos orígenes que llegarían con el mismo nombre se niegan",
         [f"cp {U}/datos#content/d*/y.log {U}/hijoB --wait"], rc=1, idioma="es",
         espera=[r"mismo nombre"],
         estado=[("hijoB sigue vacío", hay(f"/mnt/{P}/hijoB"), r"\A\Z")])
    caso(ctx, "patrones cp: un destino que casa con varios se niega",
         [f"cp {U}/datos#content/c.txt {U}/hijo* --wait"], rc=1, idioma="es",
         espera=[r"necesita UN destino"],
         estado=[("hijoB sigue vacío", hay(f"/mnt/{P}/hijoB"), r"\A\Z")])
    caso(ctx, "patrones cp: --as con varios orígenes se niega",
         [f"cp {U}/datos#content/*.log {U}/hijoB --as z --wait"], rc=1, idioma="es",
         espera=[r"--as"],
         estado=[("hijoB sigue vacío", hay(f"/mnt/{P}/hijoB"), r"\A\Z")])
    caso(ctx, "patrones cp: un origen sin coincidencias es un error",
         [f"cp {U}/datos#content/*.zz {U}/hijoB --wait"], rc=1, idioma="es",
         espera=[r"nada coincide"])
    caso(ctx, "patrones cp: un destino con patrón que casa con UNO vale",
         [f"cp {U}/datos#content/c.txt {U}/ot* --wait"], rc=0,
         estado=[("otro/c.txt", f"cat /mnt/{P}/otro/c.txt", r"^c$")])

    # --- rsync: con comodines en #content, cada uno aterriza CON SU NOMBRE.
    caso(ctx, "patrones rsync: d* deja otro/d1 y otro/d2, como rsync src/d* dst/",
         [f"rsync {U}/otro --from {U}/datos#content/d* --wait"], rc=0,
         estado=[("otro/d1/y.log", f"cat /mnt/{P}/otro/d1/y.log", r"^y1$"),
                 ("otro/d2/y.log", f"cat /mnt/{P}/otro/d2/y.log", r"^y2$")])
    sh(ctx, f"bash -c 'echo s > /mnt/{P}/otro/d1/sobra'", root=True)
    caso(ctx, "patrones rsync: --delete solo borra DENTRO de cada uno",
         [f"rsync {U}/otro --from {U}/datos#content/d* --delete --wait"], rc=0,
         estado=[("otro/d1/sobra ya no está", hay(f"/mnt/{P}/otro/d1"), r"\Ay\.log\n?\Z"),
                 ("otro/c.txt sigue", f"cat /mnt/{P}/otro/c.txt", r"^c$")])
    caso(ctx, "patrones rsync: un fichero con patrón aterriza dentro del destino",
         [f"rsync {U}/hijoB --from {U}/datos#content/c.tx? --wait"], rc=0,
         estado=[("hijoB/c.txt", f"cat /mnt/{P}/hijoB/c.txt", r"^c$")])
    # Sin comodines, las llaves son lo de siempre: el CONTENIDO de cada uno al destino.
    caso(ctx, "patrones rsync: {d1} sin comodines vuelca el contenido, como siempre",
         [f"rsync {U}/hijoB --from {U}/datos#content/{{d1}} --wait"], rc=0,
         estado=[("hijoB/y.log", f"cat /mnt/{P}/hijoB/y.log", r"^y1$"),
                 ("no hay hijoB/d1", hay(f"/mnt/{P}/hijoB"), r"\A(?![\s\S]*^d1$)")])
    caso(ctx, "patrones rsync: varios datasets de origen con --delete se niega",
         [f"rsync {U}/otro --from {U}/{{hijoA,hijoB}} --delete --wait"], rc=1, idioma="es",
         espera=[r"--delete"],
         estado=[("otro/c.txt sigue", f"cat /mnt/{P}/otro/c.txt", r"^c$")])
    caso(ctx, "patrones rsync: un destino que casa con varios se niega",
         [f"rsync {U}/hijo* --from {U}/datos --wait"], rc=1, idioma="es",
         espera=[r"necesita UN destino"])
    caso(ctx, "patrones rsync: patrón en el dataset Y en el contenido a la vez se niega",
         [f"rsync {U}/otro --from {U}/{{hijoA,hijoB}}#content/*.log --wait"], rc=1,
         idioma="es", espera=[r"a la vez"])

    # --- send: origen y destino UNO cada uno; el último tramo del destino puede ser nuevo.
    sh(ctx, f"zfs snapshot {P}/datos@s2", root=True)
    caso(ctx, "patrones send: destino NUEVO bajo una madre con patrón",
         [f"send {U}/ot*/copia --on {U}/datos@s[1] --wait"], rc=0,
         estado=[("existe otro/copia@s1", f"zfs list -H -t all -o name -r {P}/otro",
                  rf"^{P}/otro/copia@s1$")])
    caso(ctx, "patrones send: un origen que casa con varias se niega",
         [f"send {U}/otro/copia2 --on {U}/datos@s* --wait"], rc=1, idioma="es",
         espera=[r"necesita UN destino"],
         estado=[("no hay otro/copia2", f"zfs list -H -o name -r {P}/otro",
                  r"\A(?![\s\S]*copia2)")])

    # --- La raíz escrita como URL. `pwd` la enseña así y no se podía volver a escribir.
    caso(ctx, "ls zfsm:// lista las conexiones",
         ["ls zfsm://"], rc=0, espera=[r"^local\s"], no_espera=[r"falta la conexión"])

    # --- install-daemon -a: el plan, sin tocar nada. Cada conexión sale con su acción; la
    # instalación de verdad no se prueba aquí (reinicia servicios en otras máquinas).
    caso(ctx, "install-daemon -a --check enseña qué haría en cada conexión",
         ["install-daemon -a --check"], rc=0, idioma="es",
         espera=[r"^CONEXIÓN\s+DAEMON\s+ACCIÓN", r"^local\s",
                 r"no se ha tocado nada|nada que hacer"],
         no_espera=[r"desplegando"])
    caso(ctx, "install-daemon -a con destino se niega",
         ["install-daemon -a zfsm://local"], rc=2, idioma="es", espera=[r"no lleva destino"],
         no_espera=[r"desplegando"])


def casos_terminos_openzfs(ctx):
    """Los nombres de OpenZFS se dejan en inglés en el texto del CLI (backlog, punto 7).

    «snapshot», «hold» y «bookmark» son términos del producto: se teclean en `zfs`, salen en
    sus mensajes y se buscan en su documentación. Traducirlos obligaba a traducir de vuelta.
    Se barre la ayuda de las órdenes que tratan con ellos, que es donde más aparecían.
    """
    ordenes = ["ls", "create", "destroy", "hold", "holds", "release", "clone", "rollback",
               "send", "diff", "schedule", "rsync", "rename", "breakdown", "patrones"]
    for o in ordenes:
        caso(ctx, f"términos OpenZFS: «help {o}» no traduce snapshot ni hold",
             [f"help {o}"], rc=0, idioma="es",
             no_espera=[r"(?i)instant[áa]ne", r"(?i)\bretenciones\b", r"(?i)una retención\b",
                        r"(?i)la retención «"])


def casos_mensajes_capa_base(ctx):
    """Los mensajes que se generan en src/commands también se traducen.

    Antes eran literales en castellano y salían así con --lang en: «dos '@' en la misma URL».
    """
    caso(ctx, "capa base: un error de URL sale en inglés con --lang en",
         ["ls zfsm://local/x@a@b"], rc=1, idioma="en",
         espera=[r"two '@' in the same URL"], no_espera=[r"misma URL"])
    caso(ctx, "capa base: un error de cp sale en inglés con --lang en",
         ["cp zfsm://local/x#properties/y /tmp"], rc=1, idioma="en",
         espera=[r"that section names no files"], no_espera=[r"no nombra ficheros"])
    # PowerShell antepone un BOM a lo que pasa por una tubería si su codificación de salida
    # es UTF-8 con BOM: la primera orden llegaba con un carácter invisible delante.
    caso(ctx, "entrada: un BOM de UTF-8 delante de la primera orden se ignora",
         ["\ufeffls zfsm://local*"], rc=0, espera=[r"^local\s"], no_espera=[r"desconocida|unknown"])
    # La columna AVISOS del listado de conexiones: en macOS dice si el agente no puede leer
    # los discos; en una máquina sin problemas va vacía, no ausente.
    caso(ctx, "ls de la raíz: columna AVISOS, vacía si no pasa nada",
         ["ls zfsm://local*"], rc=0, idioma="es",
         espera=[r"\sAVISOS\s*$", r"^local\s.*\s-\s*$"])
    caso(ctx, "capa base: y en castellano sigue en castellano",
         ["ls zfsm://local/x@a@b"], rc=1, idioma="es", espera=[r"dos '@' en la misma URL"])


def casos_clave_ssh(ctx):
    """Una clave SSH que no existe no se da de alta; si desaparece, la conexión se aparta.

    Sobre una COPIA de la configuración: la del usuario no se toca.
    """
    cfg = os.path.join(ctx.dir_tmp, "cfg-clave")
    if os.path.isdir(cfg):
        shutil.rmtree(cfg)
    shutil.copytree(os.path.expanduser("~/.config/ZFSMgr"), cfg)
    clave = os.path.join(ctx.dir_tmp, "clave-de-prueba")
    fichero = os.path.join(ctx.dir_tmp, ".maestra")

    def corre(nombre, guion, rc, espera=(), no_espera=()):
        orden = (f"exec 9<{fichero}; exec {CLI!r} --password-fd 9 --config-dir {shlex.quote(cfg)} "
                 f"--lang es --format text -y")
        p = subprocess.run(["bash", "-c", orden], input=guion + "\nexit\n", capture_output=True,
                           text=True, timeout=120)
        salida = p.stdout + p.stderr
        problemas = []
        if p.returncode != rc:
            problemas.append(f"rc={p.returncode}, esperaba {rc}")
        problemas += [f"falta: /{x}/" for x in espera if not re.search(x, salida, re.M)]
        problemas += [f"NO debería salir: /{x}/" for x in no_espera if re.search(x, salida, re.M)]
        if problemas:
            ctx.fallos.append((nombre, problemas, salida))
            print(f"  FALLO  {nombre}")
            for x in problemas:
                print(f"         {x}")
        else:
            ctx.pasados.append(nombre)
            print(f"  ok     {nombre}")

    if os.path.exists(clave):
        os.remove(clave)
    corre("clave SSH: una ruta que no existe no se da de alta",
          f"create pclave --type SSH --host 192.0.2.1 --user x --os Linux --key {clave}", 1,
          espera=[r"no se da de alta"])
    corre("clave SSH: y la conexión no queda creada", "ls zfsm://pclave*", 1,
          espera=[r"nada coincide"])
    with open(clave, "w") as f:
        f.write("x")
    corre("clave SSH: con el fichero, sí",
          f"create pclave --type SSH --host 192.0.2.1 --user x --os Linux --key {clave}", 0,
          espera=[r"creada la conexión pclave"])
    os.remove(clave)
    corre("clave SSH: si desaparece, se avisa y queda desconectada", "ls zfsm://pclave*", 0,
          espera=[r"la clave SSH de «pclave» no existe", r"^pclave\s.*\sno\s+-\s+-\s*$"])
    corre("clave SSH: el aviso no se repite", "ls zfsm://pclave*", 0,
          no_espera=[r"aviso: la clave"])
    corre("clave SSH: connect se niega mientras falte", "connect zfsm://pclave", 1,
          espera=[r"sigue sin existir"])
    corre("clave SSH: edit no guarda una ruta que no existe", "edit zfsm://pclave --key /no/existe", 1,
          espera=[r"no se guarda el cambio"])
    with open(clave, "w") as f:
        f.write("x")
    corre("clave SSH: con la clave de vuelta, connect la recupera", "connect zfsm://pclave", 0,
          espera=[r"marcada como conectada"])
    os.remove(clave)
    shutil.rmtree(cfg)


def casos_diff_relativo(ctx):
    """En `diff`, un «@snapshot» suelto se completa con el dataset del OTRO extremo.

    Desde la raíz, `diff zfsm://…/datos@s2 --from @s1` contestaba «hace falta estar en un
    dataset» con el dataset escrito en la orden.
    """
    P = POOL_LOCAL
    prepara(ctx)
    sh(ctx, f"bash -c 'echo nuevo > /mnt/{P}/datos/nuevo.txt'", root=True)
    sh(ctx, f"zfs snapshot {P}/datos@s2", root=True)
    U = f"zfsm://local/{P}/datos"
    caso(ctx, "diff: --from @snap se completa con el dataset del destino",
         [f"diff {U}@s2 --from @s1"], rc=0, espera=[r"^\+\s+\S*/nuevo\.txt"])
    caso(ctx, "diff: y un destino @snap con el dataset de --from",
         [f"diff @s2 --from {U}@s1"], rc=0, espera=[r"^\+\s+\S*/nuevo\.txt"])


def casos_send_level(ctx):
    """`send --level`: the base is the newest snapshot both sides share, found by guid.

    What the graphical interface's «Level» did on its own, now the shell's.
    """
    P = POOL_LOCAL
    prepara(ctx)
    U = f"zfsm://local/{P}"
    snaps = f"zfs list -H -o name -t snapshot -s createtxg -d 1 {P}/nivel"
    sh(ctx, f"zfs snapshot {P}/datos@n1", root=True)
    sh(ctx, f"bash -c 'zfs create {P}/ajeno && zfs snapshot {P}/ajeno@suyo'", root=True)
    caso(ctx, "send --level: sin nada en común lo dice y no manda",
         [f"send {U}/ajeno --on {U}/datos@n1 --level --wait"], rc=1, idioma="es",
         espera=[r"no comparte"])
    caso(ctx, "send --level: la copia completa de partida, sin --level",
         [f"send {U}/nivel --on {U}/datos@n1 --wait"], rc=0,
         estado=[("el destino tiene n1", snaps, rf"{P}/nivel@n1$")])
    sh(ctx, f"bash -c 'echo uno > /mnt/{P}/datos/n2.txt'", root=True)
    sh(ctx, f"zfs snapshot {P}/datos@n2", root=True)
    sh(ctx, f"zfs snapshot {P}/datos@n3", root=True)
    # Renamed on the target: the guid still finds it as the base.
    sh(ctx, f"zfs rename {P}/nivel@n1 {P}/nivel@otro-nombre", root=True)
    caso(ctx, "send --level: hasta el snapshot pedido, con la base por guid",
         [f"send {U}/nivel --on {U}/datos@n2 --level --wait"], rc=0, idioma="es",
         espera=[rf"nivelar desde {P}/datos@n1"],
         estado=[("llega n2 y no n3", snaps, rf"\A(?![\s\S]*@n3)[\s\S]*{P}/nivel@n2$")])
    caso(ctx, "send --level: ya nivelado lo dice y no lanza nada",
         [f"send {U}/nivel --on {U}/datos@n2 --level --wait"], rc=0, idioma="es",
         espera=[r"ya está nivelado"], no_espera=[r"trabajo"])
    sh(ctx, f"zfs snapshot {P}/nivel@propio", root=True)
    caso(ctx, "send --level: un snapshot propio del destino lo para y lo nombra",
         [f"send {U}/nivel --on {U}/datos@n3 --level --wait"], rc=1, idioma="es",
         espera=[r"más nuevos que la base.*\(propio\)"])
    caso(ctx, "send --level no va con --base",
         [f"send {U}/nivel --on {U}/datos@n3 --level --base @n1"], rc=2, idioma="es",
         espera=[r"busca la base por su cuenta"])
    sh(ctx, f"bash -c 'zfs destroy -r {P}/nivel; zfs destroy -r {P}/ajeno'", root=True)


def casos_recepcion_a_medias(ctx):
    """Una recepción cortada deja un testigo en el destino, y `send` tiene que decidir.

    Antes el CLI no lo miraba: llegaba el error crudo de zfs recv y la salida era
    `zfs recv -A` a mano. El corte se provoca de verdad —el flujo truncado con `head`—, no
    se simula.
    """
    P = POOL_LOCAL
    prepara(ctx)
    sh(ctx, f"bash -c 'head -c 8000000 /dev/urandom > /mnt/{P}/datos/grande.bin'", root=True)
    sh(ctx, f"zfs snapshot {P}/datos@corte", root=True)
    corta = (f"bash -c 'zfs destroy -r {P}/medias 2>/dev/null; "
             f"zfs send {P}/datos@corte | head -c 2000000 | zfs recv -s {P}/medias 2>/dev/null; true'")
    testigo = f"zfs get -H -o value receive_resume_token {P}/medias"
    md5 = (f"bash -c 'zfs mount {P}/medias 2>/dev/null; "
           f"md5sum < /mnt/{P}/datos/grande.bin; md5sum < /mnt/{P}/medias/grande.bin'")
    U = f"zfsm://local/{P}"
    sh(ctx, corta, root=True)
    caso(ctx, "send: con una recepción a medias se niega y explica las dos salidas",
         [f"send {U}/medias --on {U}/datos@corte --wait"], rc=1, idioma="es",
         espera=[r"se cortó a medias", r"--resume", r"--discard-partial"],
         estado=[("el testigo sigue", testigo, r"^1-")])
    caso(ctx, "send --resume: la continúa y el fichero llega entero",
         [f"send {U}/medias --on {U}/datos@corte --resume --wait"], rc=0,
         estado=[("ya no hay testigo", testigo, r"^-$"),
                 ("mismo contenido", md5, r"\A(\w+)\s+-\n\1\s+-\s*\Z")])
    sh(ctx, corta, root=True)
    caso(ctx, "send --discard-partial: la descarta y manda de cero",
         [f"send {U}/medias --on {U}/datos@corte --discard-partial --wait"], rc=0, idioma="es",
         espera=[r"descartada la recepción a medias"],
         estado=[("ya no hay testigo", testigo, r"^-$"),
                 ("mismo contenido", md5, r"\A(\w+)\s+-\n\1\s+-\s*\Z")])
    caso(ctx, "send --resume sin nada a medias se niega",
         [f"send {U}/otra --on {U}/datos@corte --resume --wait"], rc=1, idioma="es",
         espera=[r"no tiene ninguna recepción a medias"])
    # Una instantánea que no existe se dice en la misma orden: antes se aceptaba, el trabajo
    # se quedaba en 0 B «running» y nadie decía por qué.
    caso(ctx, "send de una instantánea que no existe: falla al momento, sin trabajo",
         [f"send {U}/noesta --on {U}/datos@noexiste --wait"], rc=1, idioma="es",
         espera=[r"no existe la instantánea .*datos@noexiste"], no_espera=[r"trabajo"],
         estado=[("no se crea el destino",
                  f"bash -c 'zfs list -H -o name {P}/noesta 2>/dev/null || echo NO'", r"^NO$")])


def casos_send_fichero(ctx):
    """`send` con un FICHERO en una punta: volcar el flujo en él, o recibirlo de él.

    Lo que de verdad importa aquí no es que el fichero aparezca, sino que un flujo roto NO se
    dé por bueno: antes el trabajo era del emisor y acababa «done» en cuanto mandaba el último
    byte, aunque el `zfs recv` del otro lado lo rechazara. Ahora el receptor devuelve su
    veredicto. El corte se provoca de verdad, truncando el fichero con `head`.
    """
    P = POOL_LOCAL
    prepara(ctx)
    caso = lambda *a, **k: globals()["caso"](*a, idioma="es", **k)
    dst = "/var/tmp/zfsmgr-prueba-flujo"
    F = f"zfsm://local#path{dst}"
    U = f"zfsm://local/{P}"
    sh(ctx, f"rm -rf {dst} && mkdir -p {dst}", root=True)
    sh(ctx, f"bash -c 'head -c 3000000 /dev/urandom > /mnt/{P}/datos/flujo.bin'", root=True)
    sh(ctx, f"zfs snapshot {P}/datos@sf1", root=True)
    md5 = (f"bash -c 'zfs mount {P}/defichero 2>/dev/null; "
           f"md5sum < /mnt/{P}/datos/flujo.bin; md5sum < /mnt/{P}/defichero/flujo.bin'")
    caso(ctx, "send: vuelca el flujo en un fichero, solo para root",
         [f"send {F}/s.zfs --on {U}/datos@sf1 --wait"], rc=0, espera=[r"guardado en "],
         estado=[("el fichero está y es 600", f"stat -c %a {dst}/s.zfs", r"^600$"),
                 ("no queda el parcial", f"ls {dst}", r"(?s)\A(?!.*zfsmgr-part)")])
    caso(ctx, "send --from un fichero: lo recibe como si fuera el snapshot",
         [f"send {U}/defichero --from {F}/s.zfs --wait"], rc=0,
         estado=[("llegó el snapshot", f"zfs list -H -o name {P}/defichero@sf1", rf"^{P}/defichero@sf1$"),
                 ("mismo contenido", md5, r"\A(\w+)\s+-\n\1\s+-\s*\Z")])
    sh(ctx, f"bash -c 'head -c 1000000 {dst}/s.zfs > {dst}/corto.zfs'", root=True)
    caso(ctx, "send --from un fichero cortado FALLA, y dice por qué",
         [f"send {U}/cortado --from {F}/corto.zfs --wait"], rc=1,
         espera=[r"failed", r"incomplete stream"])
    caso(ctx, "send a un directorio pide el nombre del fichero",
         [f"send {F} --on {U}/datos@sf1"], rc=2, espera=[r"es un directorio"])
    caso(ctx, "send a un directorio que no existe se niega antes de nada",
         [f"send {F}/no-existe/s.zfs --on {U}/datos@sf1"], rc=1, espera=[r"no existe en local"])
    caso(ctx, "send de un fichero a otro es cp",
         [f"send {F}/otro.zfs --from {F}/s.zfs"], rc=2, espera=[r"eso es «cp»"])
    caso(ctx, "send --from un fichero no admite --base ni banderas",
         [f"send {U}/x --from {F}/s.zfs -R"], rc=2, espera=[r"no valen «--base» ni las banderas"])
    caso(ctx, "send --from la raíz de .zfs/snapshot vale como el snapshot",
         [f"send {F}/s2.zfs --from zfsm://local#path/mnt/{P}/datos/.zfs/snapshot/sf1 --wait"],
         rc=0, espera=[rf"→ {re.escape(U)}/datos@sf1"],
         estado=[("el fichero está", f"stat -c %s {dst}/s2.zfs", r"^\d+$")])
    sh(ctx, f"bash -c 'zfs recv -A {P}/cortado; zfs destroy -r {P}/cortado; "
            f"zfs destroy -r {P}/defichero; rm -rf {dst}'", root=True)


def casos_send_comprimido(ctx):
    """`send … --comp`: el fichero de flujo, comprimido; y al leerlo, reconocido por sus bytes.

    Lo que importa: que se comprima DE VERDAD (los primeros bytes y el tamaño), que vuelva
    idéntico, que el formato se reconozca aunque la extensión mienta, y que un fichero
    comprimido cortado o dañado falle con su motivo en vez de pasar por bueno.
    """
    P = POOL_LOCAL
    prepara(ctx)
    caso = lambda *a, **k: globals()["caso"](*a, idioma="es", **k)
    dst = "/var/tmp/zfsmgr-prueba-comp"
    F = f"zfsm://local#path{dst}"
    U = f"zfsm://local/{P}"
    sh(ctx, f"rm -rf {dst} && mkdir -p {dst}", root=True)
    # Comprimible a propósito: con datos aleatorios no se vería si comprime o no.
    sh(ctx, f"bash -c 'yes zfsmgr-prueba-de-compresion | head -c 6000000 > /mnt/{P}/datos/texto.txt'",
       root=True)
    sh(ctx, f"zfs snapshot {P}/datos@sc1", root=True)
    caso(ctx, "send --comp: el flujo crudo, para comparar",
         [f"send {F}/crudo.zfs --on {U}/datos@sc1 --wait"], rc=0, espera=[r"guardado en "])
    magia = {"gzip": "1f 8b", "lz4": "04 22 4d 18", "zstd": "28 b5 2f fd"}
    for fmt, ext in (("gzip", "gz"), ("lz4", "lz4"), ("zstd", "zst")):
        f = f"{dst}/c.zfs.{ext}"
        caso(ctx, f"send --comp {fmt}: el fichero sale comprimido",
             [f"send {F}/c.zfs.{ext} --on {U}/datos@sc1 --comp {fmt} --wait"], rc=0,
             espera=[r"guardado en "],
             estado=[(f"empieza por la firma de {fmt}", f"od -An -tx1 -N4 {f}",
                      rf"^\s*{magia[fmt]}"),
                     ("y ocupa menos que el crudo",
                      f"bash -c 'test $(stat -c %s {f}) -lt $(stat -c %s {dst}/crudo.zfs) && echo MENOR'",
                      r"^MENOR$"),
                     ("solo para root", f"stat -c %a {f}", r"^600$")])
        md5 = (f"bash -c 'zfs mount {P}/de_{fmt} 2>/dev/null; md5sum < /mnt/{P}/datos/texto.txt; "
               f"md5sum < /mnt/{P}/de_{fmt}/texto.txt'")
        caso(ctx, f"send --from un fichero {fmt}: se reconoce solo y vuelve idéntico",
             [f"send {U}/de_{fmt} --from {F}/c.zfs.{ext} --wait"], rc=0,
             estado=[("llegó el snapshot", f"zfs list -H -o name {P}/de_{fmt}@sc1",
                      rf"^{P}/de_{fmt}@sc1$"),
                     ("mismo contenido", md5, r"\A(\w+)\s+-\n\1\s+-\s*\Z")])
    # La extensión miente: es zstd con nombre de gzip, y se lee por sus bytes.
    sh(ctx, f"cp {dst}/c.zfs.zst {dst}/miente.gz", root=True)
    caso(ctx, "send --from: el formato sale de los bytes, no de la extensión",
         [f"send {U}/miente --from {F}/miente.gz --wait"], rc=0,
         estado=[("llegó", f"zfs list -H -o name {P}/miente@sc1", rf"^{P}/miente@sc1$")])
    sh(ctx, f"bash -c 'head -c $(( $(stat -c %s {dst}/c.zfs.zst) / 2 )) {dst}/c.zfs.zst > {dst}/corto.zst'",
       root=True)
    caso(ctx, "send --from un fichero comprimido CORTADO falla y dice por qué",
         [f"send {U}/cortado_c --from {F}/corto.zst --wait"], rc=1,
         espera=[r"cut short|damaged|incomplete"],
         estado=[("no llegó ningún snapshot",
                  f"bash -c 'zfs list -H -t snapshot -o name {P}/cortado_c 2>&1 | grep -c @ || true'",
                  r"^0$")])
    # Dañado: un byte cambiado en medio. Con XOR, para que cambie SEGURO: escribir un valor
    # fijo no cambiaba nada si el byte ya lo tenía, y la prueba pasaba sin dañar el fichero.
    sh(ctx, f"cp {dst}/c.zfs.lz4 {dst}/danado.lz4", root=True)
    sh(ctx, f"python3 -I -c \"import sys; p = sys.argv[1]; b = bytearray(open(p, 'rb').read()); "
            f"b[len(b) // 2] ^= 0x5a; open(p, 'wb').write(b)\" {dst}/danado.lz4", root=True)
    caso(ctx, "send --from un fichero comprimido DAÑADO falla",
         [f"send {U}/danado_c --from {F}/danado.lz4 --wait"], rc=1,
         espera=[r"damaged|cut short|invalid|incomplete|checksum"],
         estado=[("el fichero estaba de verdad dañado",
                  f"bash -c 'cmp -s {dst}/c.zfs.lz4 {dst}/danado.lz4 || echo DISTINTO'",
                  r"^DISTINTO$")])
    caso(ctx, "send --comp a un dataset se niega: es para un fichero",
         [f"send {U}/x --on {U}/datos@sc1 --comp zstd"], rc=2,
         espera=[r"es para un FICHERO de destino"])
    caso(ctx, "send --comp con un nivel fuera de rango se niega antes de nada",
         [f"send {F}/n.zfs --on {U}/datos@sc1 --comp zstd:25"], rc=2,
         espera=[r"el nivel de zstd va de 1 a 19"],
         estado=[("no se creó nada", f"bash -c 'ls {dst} | grep -c \"^n\\.zfs\" || true'",
                  r"^0$")])
    caso(ctx, "send --comp con un formato desconocido se niega",
         [f"send {F}/n.zfs --on {U}/datos@sc1 --comp bzip2"], rc=2,
         espera=[r"compresión desconocida «bzip2»"])
    sh(ctx, f"bash -c 'for d in de_gzip de_lz4 de_zstd miente cortado_c danado_c; do "
            f"zfs recv -A {P}/$d 2>/dev/null; zfs destroy -r {P}/$d 2>/dev/null; done; "
            f"rm -rf {dst}'", root=True)


def casos_sin_montar(ctx):
    """Un dataset SIN MONTAR: leerlo, y Desglosar/Ensamblar un entorno de arranque.

    El caso real es un sistema arrancable con `mountpoint=/`: montarlo en su sitio taparía la
    raíz de la máquina. Para leer se monta en solo lectura en un temporal sin tocar
    propiedades; para Desglosar y Ensamblar, en un punto de montaje temporal que después se
    devuelve —con su origen: aquí `received`— SIN MONTAR NADA. Lo que de verdad se comprueba
    es eso último: devolverle el punto de montaje al padre montaba sus hijos nuevos (en la VM
    de FreeBSD, encima de su /usr). Aquí las rutas caen bajo /mnt/<pool>, así que un fallo no
    pisa nada de la máquina; la prueba con `/` se hizo a mano en la VM.
    """
    P = POOL_LOCAL
    prepara(ctx)
    caso = lambda *a, **k: globals()["caso"](*a, idioma="es", **k)
    U = f"zfsm://local/{P}"
    be = f"{P}/arranque"
    w = "/tmp/zfsmgr-prueba-sinmontar"
    sh(ctx, f"bash -c 'zfs create -o mountpoint=/mnt/{P}/sistema -o canmount=noauto {P}/origen && "
            f"mkdir -p {w} && mount -t zfs -o zfsutil {P}/origen {w} && "
            f"mkdir -p {w}/usr/bin {w}/etc && echo kali > {w}/etc/hostname && echo b > {w}/usr/bin/ls && "
            f"umount {w} && rmdir {w} && zfs snapshot {P}/origen@a && "
            f"zfs send -p {P}/origen@a | zfs recv -u {be}'", root=True)
    montado = f"zfs get -H -o value mounted {be}"
    punto = f"zfs get -H -o value,source mountpoint {be}"
    temporales = "bash -c 'ls -d /tmp/zfsmgr-ro-* /tmp/zfsmgr-sync-* 2>/dev/null | wc -l'"
    caso(ctx, "sin montar: ls de #content lo monta en solo lectura, y lo suelta",
         [f"ls {U}/arranque#content/etc"], rc=0,
         espera=[r"se monta en solo lectura", r"^hostname\s+file\s"],
         estado=[("sigue sin montar", montado, r"^no$"),
                 ("su punto de montaje no cambia", punto, r"^/mnt/\S+/sistema\s+received$"),
                 ("no queda ningún temporal", temporales, r"^0$")])
    caso(ctx, "sin montar: cp HACIA él se niega, no escribe en la ruta de la propiedad",
         [f"cp zfsm://local#path/etc/hostname {U}/arranque#content/etc/"], rc=1,
         espera=[r"no se escribe en él"])
    caso(ctx, "sin montar: breakdown en un temporal, y le devuelve su punto RECIBIDO sin montar nada",
         [f"breakdown usr usr --on {U}/arranque --wait"], rc=0,
         espera=[r"\[BREAKDOWN\] ok usr"],
         estado=[("el padre, sin montar", montado, r"^no$"),
                 ("el padre, con su punto recibido", punto, r"^/mnt/\S+/sistema\s+received$"),
                 ("el hijo, heredado y SIN MONTAR",
                  f"zfs get -H -o value mounted,canmount {be}/usr", r"\Ano\non\s*\Z"),
                 ("el hijo hereda", f"zfs get -H -o source mountpoint {be}/usr", r"^inherited"),
                 ("sin migas", f"zfs get -H -s local -o value org.zfsmgr:savedmountpoint {be}",
                  r"\A\s*\Z"),
                 ("no queda ningún temporal", temporales, r"^0$")])
    caso(ctx, "sin montar: assemble lo devuelve a directorio, también sin montar nada",
         [f"assemble usr --on {U}/arranque --wait"], rc=0,
         estado=[("el hijo ya no está", f"bash -c 'zfs list -H {be}/usr 2>&1; true'", r"does not exist"),
                 ("el padre, sin montar y con su punto", punto, r"^/mnt/\S+/sistema\s+received$"),
                 ("no queda ningún temporal", temporales, r"^0$")])
    caso(ctx, "sin montar: el contenido vuelve a estar en usr/bin",
         [f"ls {U}/arranque#content/usr/bin"], rc=0, espera=[r"^ls\s+file\s"])
    sh(ctx, f"bash -c 'zfs destroy -r {be}; zfs destroy -r {P}/origen'", root=True)


def casos_cp_destino(ctx):
    """`cp` como el de toda la vida según lo que haya en el destino.

    Directorio: dentro y con su nombre. Nada: el destino ES la copia (fichero con ese
    nombre; directorio creado con su contenido). Fichero: otro fichero lo sobrescribe, un
    directorio no. Y el directorio de llegada con el dueño y el modo del original, no
    root:root 0755, que es como lo dejaba el agente al crearlo.
    """
    w = "/var/tmp/zfsmgr-prueba-cp"
    sh(ctx, f"rm -rf {w}", root=True)
    os.makedirs(f"{w}/src/dir/sub", exist_ok=True)
    os.makedirs(f"{w}/dst", exist_ok=True)
    for ruta, texto in (("src/f.txt", "hola"), ("src/dir/a", "x"), ("src/dir/sub/b", "y"),
                        ("dst/ex.txt", "viejo")):
        with open(f"{w}/{ruta}", "w") as f:
            f.write(texto + "\n")
    os.chmod(f"{w}/src/dir", 0o750)
    caso(ctx, "cp: fichero a un destino que no existe, con ese nombre",
         [f"cp {w}/src/f.txt {w}/dst/nuevo.txt --wait"], rc=0,
         estado=[("nuevo.txt con el contenido", f"cat {w}/dst/nuevo.txt", r"^hola$")])
    caso(ctx, "cp: directorio a un destino que no existe, crea ese directorio",
         [f"cp {w}/src/dir {w}/dst/copia --wait"], rc=0,
         estado=[("con su contenido", f"cat {w}/dst/copia/sub/b", r"^y$"),
                 ("sin anidar el origen dentro", f"bash -c 'ls {w}/dst/copia'", r"^a\nsub$"),
                 ("con el dueño y el modo del original",
                  f"stat -c '%U %a' {w}/dst/copia", rf"^{os.environ.get('USER', 'linarese')} 750$")])
    caso(ctx, "cp: un fichero sobrescribe a otro",
         [f"cp {w}/src/f.txt {w}/dst/ex.txt --wait"], rc=0,
         estado=[("ex.txt sobrescrito", f"cat {w}/dst/ex.txt", r"^hola$")])
    caso(ctx, "cp: un directorio NO sobrescribe un fichero",
         [f"cp {w}/src/dir {w}/dst/ex.txt --wait"], rc=1,
         espera=[r"overwrite the file|sobrescribir el fichero"])
    caso(ctx, "cp: con varios orígenes el destino tiene que ser un directorio",
         [f"cp {w}/src/f.txt {w}/src/dir {w}/dst/noexiste --wait"], rc=1,
         espera=[r"several sources|varios orígenes"],
         estado=[("no crea nada", f"bash -c 'ls {w}/dst/noexiste 2>&1'", r"No such|No existe")])
    caso(ctx, "cp: el padre del destino tiene que existir",
         [f"cp {w}/src/f.txt {w}/nohay/x.txt --wait"], rc=1,
         espera=[r"does not exist|no existe el directorio"])
    caso(ctx, "cp: un origen que no existe se dice ANTES de lanzar nada",
         [f"cp {w}/src/nada.txt {w}/dst/"], rc=1,
         espera=[r"does not exist: .*nada\.txt|no existe: .*nada\.txt"],
         no_espera=[r"job|trabajo"])
    caso(ctx, "cp: a un directorio que existe, dentro y con su nombre",
         [f"cp {w}/src/dir {w}/dst --wait"], rc=0,
         estado=[("dst/dir/a", f"cat {w}/dst/dir/a", r"^x$")])
    sh(ctx, f"rm -rf {w}", root=True)


def casos_load_key(ctx):
    """`load-key` mira el dataset ANTES de pedir la frase.

    Sin cifrar o con la clave ya cargada no pide nada; en un hijo que hereda el cifrado
    carga la de su raíz de cifrado y lo dice; con `keylocation` en un fichero la lee de ahí.
    """
    P = POOL_LOCAL
    prepara(ctx)
    frase = "frase-de-la-bateria"
    ruta_frase = os.path.join(ctx.dir_tmp, "frase-cif")
    ruta_clave = os.path.join(ctx.dir_tmp, "clave-fich")
    with open(os.open(ruta_frase, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
        f.write(frase + "\n")
    with open(os.open(ruta_clave, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "wb") as f:
        f.write(os.urandom(32))
    # Se crea leyendo la frase de un fichero y después se pasa a `prompt`: por la entrada
    # estándar no se puede, porque `sudo -S` se come la primera línea.
    sh(ctx, f"zfs create -o encryption=on -o keyformat=passphrase "
            f"-o keylocation=file://{ruta_frase} {P}/cif", root=True)
    sh(ctx, f"zfs set keylocation=prompt {P}/cif", root=True)
    sh(ctx, f"zfs create {P}/cif/hijo", root=True)
    sh(ctx, f"zfs create -o encryption=on -o keyformat=raw "
            f"-o keylocation=file://{ruta_clave} {P}/fich", root=True)
    descarga = (f"bash -c 'zfs unmount {P}/cif/hijo; zfs unmount {P}/cif; "
                f"zfs unload-key {P}/cif; zfs unmount {P}/fich; zfs unload-key {P}/fich'")
    sh(ctx, descarga, root=True)

    caso(ctx, "help con opciones avisa de que no ejecuta nada",
         ["cd Local", "help load-key --on .."],
         espera=[r"did you mean «load-key --on \.\.»|¿quería «load-key --on \.\.»"])
    caso(ctx, "load-key: sin cifrar, lo dice sin pedir frase",
         [f"load-key --on Local/{P}/datos"], rc=1,
         espera=[r"not encrypted|no está cifrado"], no_espera=[r"assphrase|rase de paso"])
    caso(ctx, "load-key: en un hijo carga la de su raíz de cifrado",
         [f"load-key --on Local/{P}/cif/hijo --password-fd 8"], rc=0, frase=frase,
         espera=[rf"encryption root, {P}/cif|raíz de cifrado, {P}/cif",
                 rf"key loaded on {P}/cif$|clave cargada en {P}/cif$"],
         estado=[("la clave está cargada", f"zfs get -H -o value keystatus {P}/cif",
                  r"^available$")])
    caso(ctx, "load-key: con la clave ya cargada no pide nada",
         [f"load-key --on Local/{P}/cif"], rc=0,
         espera=[r"already loaded|ya está cargada"])
    sh(ctx, descarga, root=True)
    caso(ctx, "load-key: con la frase equivocada no carga",
         [f"load-key --on Local/{P}/cif --password-fd 8"], frase="no-es-esta-frase",
         espera=[r"Incorrect key provided"], no_espera=[r"key loaded|clave cargada"],
         estado=[("la clave sigue sin cargar", f"zfs get -H -o value keystatus {P}/cif",
                  r"^unavailable$")])
    caso(ctx, "load-key: con keylocation en un fichero no pide frase",
         [f"load-key --on Local/{P}/fich"], rc=0,
         espera=[r"read from file://|se lee de file://"], no_espera=[r"assphrase|rase de paso"],
         estado=[("la clave está cargada", f"zfs get -H -o value keystatus {P}/fich",
                  r"^available$")])
    caso(ctx, "el uso de una orden mal escrita sale en el idioma de la sesión",
         ["cd Local", "help send @a --wait -n"], rc=2, idioma="en",
         espera=[r"^usage: help"], no_espera=[r"^uso:"])
    caso(ctx, "create: cifrado con frase la lee del descriptor, nunca de argv",
         [f"create cnueva encryption=on keyformat=passphrase --password-fd 8 --on Local/{P}"],
         rc=0, frase=frase, espera=[rf"{P}/cnueva created|creado {P}/cnueva"],
         estado=[("cifrado y con su clave", f"zfs get -H -o value encryptionroot,keystatus {P}/cnueva",
                  rf"^{P}/cnueva\navailable$")])
    sh(ctx, f"bash -c 'zfs unmount {P}/cnueva; zfs unload-key {P}/cnueva'", root=True)
    caso(ctx, "create: y esa frase es la que abre la clave",
         [f"load-key --on Local/{P}/cnueva --password-fd 8"], rc=0, frase=frase,
         estado=[("la clave está cargada", f"zfs get -H -o value keystatus {P}/cnueva",
                  r"^available$")])
    sh(ctx, f"bash -c 'zfs destroy -r {P}/cif; zfs destroy -r {P}/fich; zfs destroy -r {P}/cnueva'", root=True)


def casos_editor_ancho(ctx):
    """El editor de línea no puede pintar más ancho que el terminal (camino sin pantalla).

    Con `--no-tui`: es el pintado de una sola fila que se desplaza de lado. El de la pantalla
    —la línea partida en filas— lo prueba `casos_pantalla`.

    Con una línea más larga que el terminal, el repintado volvía con «\\r» al principio de
    la fila FÍSICA y no al de la línea: al pegar una orden larga se imprimía una copia
    entera por cada carácter. Se prueba con un pseudoterminal de 60 columnas de verdad: cada
    repintado —los tramos que empiezan por el indicador— tiene que caber en 59.
    """
    import pty, select, fcntl, termios, struct
    nombre = "editor: una orden más larga que el terminal no se repite al pegarla"
    fichero = os.path.join(ctx.dir_tmp, ".maestra")
    r, w = os.pipe()
    with open(fichero) as f:
        os.write(w, f.read().encode())
    os.close(w)
    pid, fd = pty.fork()
    if pid == 0:
        os.dup2(r, 9)
        os.execv(CLI, [CLI, "--lang", "es", "--password-fd", "9", "--no-tui"])
    os.close(r)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 60, 0, 0))
    salida = b""

    def lee(t):
        nonlocal salida
        fin = time.time() + t
        while time.time() < fin:
            listo, _, _ = select.select([fd], [], [], 0.1)
            if listo:
                try:
                    salida += os.read(fd, 65536)
                except OSError:
                    return

    lee(3)
    os.write(fd, ("ls zfsm://local/" + "x" * 180).encode())  # pegada de golpe
    lee(2)
    os.write(fd, b"\x01")   # Ctrl-A: el cursor al principio, con la línea desplazada
    lee(0.5)
    os.write(fd, b"\r")
    lee(3)
    os.write(fd, b"exit\r")
    lee(1)
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass
    texto = salida.decode("utf-8", "replace")
    repintados = [t for t in re.split(r"[\r\n]", texto) if t.startswith("zfsm://")]
    problemas = []
    if len(repintados) < 100:
        problemas.append(f"solo {len(repintados)} repintados: no se tecleó la orden")
    elif max(len(t) for t in repintados) > 59:
        problemas.append(f"un repintado ocupa {max(len(t) for t in repintados)} columnas en un "
                         "terminal de 60")
    # Y que la orden llegó ENTERA, aunque no se viera entera.
    if "x" * 180 not in texto.replace("\r", "").split("cannot open '", 1)[-1][:200]:
        problemas.append("la orden no llegó entera")
    if problemas:
        ctx.fallos.append((nombre, problemas, texto[-400:]))
        print(f"  FALLO  {nombre}")
        for x in problemas:
            print(f"         {x}")
    else:
        ctx.pasados.append(nombre)
        print(f"  ok     {nombre}")


class Tmux:
    """Un terminal de verdad para probar la PANTALLA: tmux interpreta las secuencias de escape,
    se le puede cambiar el tamaño y devuelve lo que se VE. En un servidor propio
    (`-L zfsmgr-bateria`), sin tocar ninguna sesión del usuario."""

    def __init__(self, ctx, ancho, alto, extra=""):
        self.t = ["tmux", "-L", "zfsmgr-bateria"]
        fichero = os.path.join(ctx.dir_tmp, ".maestra")
        subprocess.run(self.t + ["kill-server"], capture_output=True)
        orden = (f"bash -c 'exec 9<{fichero}; exec {CLI} --password-fd 9 --lang es "
                 f"--no-schedule-check {extra}'")
        subprocess.run(self.t + ["new-session", "-d", "-s", "p", "-x", str(ancho), "-y",
                                 str(alto), orden], check=True)
        subprocess.run(self.t + ["set", "-g", "window-size", "manual"], capture_output=True)
        self.espera("zfsm://>", 10)

    def teclas(self, *k):
        subprocess.run(self.t + ["send-keys", "-t", "p"] + list(k), check=True)

    def pantalla(self):
        return subprocess.run(self.t + ["capture-pane", "-p", "-t", "p"], capture_output=True,
                              text=True).stdout

    def espera(self, patron, segs=8):
        fin = time.time() + segs
        while time.time() < fin:
            if re.search(patron, self.pantalla(), re.M):
                return True
            time.sleep(0.2)
        return False

    def tamano(self, ancho, alto):
        subprocess.run(self.t + ["resize-window", "-t", "p", "-x", str(ancho), "-y", str(alto)],
                       check=True)
        time.sleep(0.8)   # la pantalla espera a que deje de cambiar antes de repintar

    def cierra(self):
        subprocess.run(self.t + ["kill-server"], capture_output=True)


def casos_servicio(ctx):
    """`zfsmgr-cli --serve`: el intérprete como servicio de otro programa (la interfaz gráfica).

    Una orden JSON por línea y eventos JSON de vuelta. Una orden que falla no termina el
    servicio; una confirmación sale como evento y la respuesta vuelve por la entrada.
    """
    P = POOL_LOCAL
    prepara(ctx)
    U = f"zfsm://local/{P}/datos"

    def comprueba(nombre, problemas, vista=""):
        if problemas:
            ctx.fallos.append((nombre, problemas, vista[-600:]))
            print(f"  FALLO  {nombre}")
            for x in problemas:
                print(f"         {x}")
        else:
            ctx.pasados.append(nombre)
            print(f"  ok     {nombre}")

    p = subprocess.Popen([CLI, "--serve", "--lang", "es"], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, text=True, bufsize=1)
    p.stdin.write(ctx.maestra + "\n")
    p.stdin.flush()

    def evento():
        linea = p.stdout.readline()
        return json.loads(linea) if linea else None

    def pide(i, linea, respuesta=None):
        """Manda una orden; devuelve (resultado, preguntas recibidas)."""
        p.stdin.write(json.dumps({"id": i, "line": linea}) + "\n")
        p.stdin.flush()
        preguntas = []
        while True:
            ev = evento()
            if ev is None:
                return None, preguntas
            if ev.get("type") == "confirm":
                preguntas.append(ev.get("text", ""))
                p.stdin.write(json.dumps({"id": i, "answer": bool(respuesta)}) + "\n")
                p.stdin.flush()
                continue
            return ev, preguntas

    try:
        listo = evento()
        comprueba("servicio: anuncia que está listo", [] if listo and listo.get("type") == "ready"
                  else [f"primer evento: {listo}"])
        r, _ = pide(1, "noexiste")
        comprueba("servicio: una orden desconocida da 127 y lo dice",
                  [] if r and r["rc"] == 127 and "noexiste" in r["stderr"] else [f"{r}"])
        r, _ = pide(2, "pwd")
        comprueba("servicio: y sigue atendiendo después", [] if r and r["rc"] == 0
                  and r["stdout"].strip() == "zfsm://" else [f"{r}"])
        r, _ = pide(3, f"set atime=off --on {U}")
        _, vista = sh(ctx, f"zfs get -H -o value atime {P}/datos", root=True)
        comprueba("servicio: set cambia la propiedad", [] if r and r["rc"] == 0
                  and vista.strip() == "off" else [f"{r} / atime={vista.strip()}"])
        r, _ = pide(4, f"inherit atime --on {U}")
        _, vista = sh(ctx, f"zfs get -H -o source atime {P}/datos", root=True)
        comprueba("servicio: inherit la devuelve a heredada", [] if r and r["rc"] == 0
                  and vista.strip() != "local" else [f"{r} / source={vista.strip()}"])
        r, preguntas = pide(5, f"destroy {U}", respuesta=False)
        _, vista = sh(ctx, f"zfs list -H -o name {P}/datos", root=True)
        comprueba("servicio: la confirmación llega como evento y un «no» no destruye",
                  [] if preguntas and "DESTRUIR" in preguntas[0] and r and r["rc"] != 0
                  and vista.strip() == f"{P}/datos" else [f"{preguntas} {r} {vista}"])
        r, _ = pide(6, f"ls zfsm://local/{P}")
        try:
            filas = json.loads(r["stdout"]).get("entries", [])
        except (ValueError, TypeError, KeyError):
            filas = None
        comprueba("servicio: ls responde en JSON", [] if filas else [f"{r}"])
    finally:
        p.stdin.close()
        p.wait(timeout=60)


def casos_servicio_conexiones(ctx):
    """`create`/`edit` of a connection through `--serve`, as the graphical interface runs them.

    Without a terminal the password used to be asked only by the terminal, so the connection
    was created WITHOUT one. Now it comes as a `secret` event. On a COPY of the
    configuration: the user's is not touched.
    """
    cfg = os.path.join(ctx.dir_tmp, "cfg-servicio")
    if os.path.isdir(cfg):
        shutil.rmtree(cfg)
    shutil.copytree(os.path.expanduser("~/.config/ZFSMgr"), cfg)
    clave = os.path.join(ctx.dir_tmp, "clave-servicio")
    with open(os.open(clave, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
        f.write("not a real key\n")

    def comprueba(nombre, problemas):
        if problemas:
            ctx.fallos.append((nombre, problemas, ""))
            print(f"  FALLO  {nombre}")
            for x in problemas:
                print(f"         {x}")
        else:
            ctx.pasados.append(nombre)
            print(f"  ok     {nombre}")

    def perfil():
        with open(os.path.join(cfg, "config.json")) as f:
            datos = json.load(f)
        for c in datos.get("connections", []):
            if c.get("id") == "prueba-srv":
                return c
        return None

    p = subprocess.Popen([CLI, "--serve", "--lang", "es", "--config-dir", cfg],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    p.stdin.write(ctx.maestra + "\n")
    p.stdin.flush()

    def pide(i, linea, secreto="", si=True):
        p.stdin.write(json.dumps({"id": i, "line": linea}) + "\n")
        p.stdin.flush()
        secretos = 0
        while True:
            linea_ev = p.stdout.readline()
            if not linea_ev:
                return None, secretos
            ev = json.loads(linea_ev)
            if ev.get("type") == "secret":
                secretos += 1
                p.stdin.write(json.dumps({"id": i, "secret": secreto}) + "\n")
            elif ev.get("type") == "confirm":
                p.stdin.write(json.dumps({"id": i, "answer": si}) + "\n")
            else:
                return ev, secretos
            p.stdin.flush()

    try:
        p.stdout.readline()   # ready
        r, secretos = pide(1, "create prueba-srv --type SSH --host 127.0.0.1 --user nadie "
                              "--sudo --family ipv6", secreto="clave-srv")
        c = perfil()
        comprueba("servicio: create de conexión pide la contraseña como evento y la guarda",
                  [] if r and r["rc"] == 0 and secretos == 1 and c and c.get("password")
                  and c.get("ssh_address_family") == "ipv6" else [f"{r} {secretos} {c}"])
        r, secretos = pide(2, f"edit prueba-srv --key {clave} --family ipv4")
        c = perfil()
        comprueba("servicio: edit con Intro conserva la contraseña y cambia el resto",
                  [] if r and r["rc"] == 0 and secretos == 1 and c and c.get("password")
                  and c.get("key_path") == clave and c.get("ssh_address_family") == "ipv4"
                  else [f"{r} {secretos} {c}"])
        r, _ = pide(3, "edit prueba-srv --no-key")
        c = perfil()
        comprueba("servicio: edit --no-key quita la clave",
                  [] if r and r["rc"] == 0 and c and c.get("key_path") == "" else [f"{r} {c}"])
        r, _ = pide(4, "edit prueba-srv --family ipv5")
        comprueba("servicio: --family con otro valor se rechaza",
                  [] if r and r["rc"] != 0 and "--family" in r["stderr"] else [f"{r}"])
        r, _ = pide(5, "destroy zfsm://prueba-srv")
        comprueba("servicio: y se borra", [] if r and r["rc"] == 0 and perfil() is None
                  else [f"{r}"])
    finally:
        p.stdin.close()
        p.wait(timeout=60)
        shutil.rmtree(cfg, ignore_errors=True)


def casos_pantalla(ctx):
    """La pantalla del intérprete con terminal: la línea de órdenes partida en filas como en
    bash, lo escrito repintado al cambiar el tamaño, las preguntas y Ctrl-C."""
    if not shutil.which("tmux"):
        print("  (sin tmux: no se prueba la pantalla)")
        return
    P = POOL_LOCAL

    def comprueba(nombre, problemas, vista):
        if problemas:
            ctx.fallos.append((nombre, problemas, vista[-600:]))
            print(f"  FALLO  {nombre}")
            for x in problemas:
                print(f"         {x}")
        else:
            ctx.pasados.append(nombre)
            print(f"  ok     {nombre}")

    t = Tmux(ctx, 60, 15)
    try:
        larga = "ls zfsm://local/" + "x" * 150
        t.teclas(larga)
        t.espera("x{20}$")
        vista = t.pantalla()
        filas = [l for l in vista.splitlines() if "x" in l]
        problemas = []
        if len(filas) < 3:
            problemas.append(f"la orden ocupa {len(filas)} filas: no se parte como en bash")
        if "".join(l.strip() for l in filas).count("x" * 150) != 1:
            problemas.append("la orden no se ve entera y una sola vez")
        if any(len(l) > 60 for l in vista.splitlines()):
            problemas.append("una fila más ancha que el terminal")
        comprueba("pantalla: una orden larga se parte en filas y se ve entera", problemas, vista)

        # Ctrl-A al principio y una letra: entra en su sitio, en la primera fila.
        t.teclas("C-a", "Z")
        t.espera(r"zfsm://> Zls")
        vista = t.pantalla()
        comprueba("pantalla: el cursor cruza las filas (Ctrl-A y escribir al principio)",
                  [] if re.search(r"^zfsm://> Zls zfsm://local/x", vista, re.M)
                  else ["la «Z» no está al principio de la orden"], vista)

        t.teclas("C-u", "C-k")
        t.teclas("C-e", "C-u")
        t.teclas("pwd", "Enter")
        t.espera(r"^zfsm://$")
        t.teclas("help cd", "Enter")
        t.espera("sube un nivel")
        t.tamano(40, 15)
        vista = t.pantalla()
        problemas = []
        if any(len(l) > 40 for l in vista.splitlines()):
            problemas.append("tras estrechar, una fila más ancha que el terminal")
        # El terminal parte las líneas largas de antes: se busca una palabra corta.
        if not re.search(r"vuelve", vista):
            problemas.append("tras estrechar, no se ve lo escrito antes")
        # Abajo del todo va la zona de estado; la línea de órdenes, justo encima.
        if not any(re.search(r"^zfsm://> *$", l) for l in vista.rstrip().splitlines()[-2:]):
            problemas.append("tras estrechar, la línea de órdenes no queda abajo")
        comprueba("pantalla: al estrechar la ventana se repinta lo escrito y la línea",
                  problemas, vista)

        t.tamano(90, 20)
        t.teclas(f"cd Local/{P}/datos", "Enter")
        t.espera(rf"zfsm://local/{P}/datos>")
        t.teclas("schedule --clear", "Enter")
        hay = t.espera(r"\[s/N\]: *$")
        t.teclas("n", "Enter")
        t.espera("cancelado")
        t.tamano(70, 20)
        vista = t.pantalla()
        comprueba("pantalla: una pregunta s/N y su respuesta quedan, también tras repintar",
                  [] if hay and re.search(r"\[s/N\]: n$", vista, re.M) and "cancelado" in vista
                  else ["no se ve la pregunta con su «n» y el «cancelado»"], vista)

        t.teclas("abc", "C-c")
        time.sleep(0.5)
        t.teclas("pwd", "Enter")
        vivo = t.espera(rf"^zfsm://local/{P}/datos$")
        vista = t.pantalla()
        comprueba("pantalla: Ctrl-C abandona la línea y el intérprete sigue",
                  [] if vivo and "abc^C" in vista else ["Ctrl-C mató el intérprete o no marcó la línea"],
                  vista)

        # Las tablas: con bordes, en fichas cuando no caben, y de vuelta a tabla al ensanchar.
        t.teclas("cls", "Enter")
        t.tamano(170, 40)
        t.teclas("ls zfsm://", "Enter")
        t.espera(r"^└", 20)
        vista = t.pantalla()
        comprueba("pantalla: una tabla sale con bordes y su cabecera",
                  [] if re.search(r"^┌─", vista, re.M) and re.search(r"^│ ID\s+│", vista, re.M)
                  and re.search(r"^├─", vista, re.M) else ["no hay bordes ni cabecera"], vista)
        t.tamano(60, 40)
        vista = t.pantalla()
        comprueba("pantalla: si no cabe, una ficha por fila en vez de tiras",
                  [] if re.search(r"^── \w+ ─", vista, re.M) and re.search(r"^  TIPO\s+SSH", vista, re.M)
                  and not any(len(l) > 60 for l in vista.splitlines())
                  else ["no está en fichas, o alguna fila se sale"], vista)
        t.tamano(170, 40)
        vista = t.pantalla()
        comprueba("pantalla: al ensanchar vuelve a ser tabla",
                  [] if re.search(r"^├─", vista, re.M) and not re.search(r"^── local", vista, re.M)
                  else ["no ha vuelto a la tabla"], vista)

        # La zona de estado: el último error, que se quita tras una que va bien. La posición NO:
        # ya la da la línea de órdenes justo encima, y repetirla abajo era ruido.
        t.teclas("cd zfsm://local/noexiste", "Enter")
        t.espera("✗")
        vista = t.pantalla().rstrip()
        abajo = vista.splitlines()[-1]
        comprueba("pantalla: la zona de estado da el último error, y no repite la posición",
                  [] if "✗ no existe" in abajo and "zfsm://" not in abajo.split("✗")[0]
                  else [f"la última fila es «{abajo}»"], vista)
        t.teclas("pwd", "Enter")
        time.sleep(1.5)
        abajo = t.pantalla().rstrip().splitlines()[-1]
        comprueba("pantalla: tras una orden que va bien, el error se quita",
                  [] if "✗" not in abajo else [f"sigue: «{abajo}»"], abajo)
    finally:
        t.cierra()


def casos_tab_continua(ctx):
    """El tabulador sobre un nombre ya escrito entero ofrece lo que puede seguir.

    `cd local<tab>` en zfsm:// no hacía nada: el único candidato era lo ya escrito y no había
    nada que alargar. Ahora una conexión sigue con «/», y un dataset ofrece «/» (si tiene
    hijos), «@» (si tiene instantáneas) y «#».
    """
    import pty, select
    P = POOL_LOCAL
    prepara(ctx)
    U = f"zfsm://local/{P}"
    nombre = "tabulador: tras un nombre completo propone «/», «@», «#» o «#path/», sin repetir lo común"
    fichero = os.path.join(ctx.dir_tmp, ".maestra")
    r, w = os.pipe()
    with open(fichero) as f:
        os.write(w, f.read().encode())
    os.close(w)
    pid, fd = pty.fork()
    if pid == 0:
        os.dup2(r, 9)
        os.execv(CLI, [CLI, "--lang", "es", "--password-fd", "9"])
    os.close(r)

    def lee(t):
        salida = b""
        fin = time.time() + t
        while time.time() < fin:
            listo, _, _ = select.select([fd], [], [], 0.1)
            if listo:
                try:
                    salida += os.read(fd, 65536)
                except OSError:
                    break
        # Sin las secuencias de escape de la pantalla: lo que se comprueba es QUÉ se completa,
        # no cómo se pinta.
        # El borrado de la zona («\r», subir y limpiar) es un salto a efectos de lo que se lee.
        texto = re.sub(r"\r(\x1b\[\d+A)?\x1b\[J", "\n", salida.decode("utf-8", "replace"))
        return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", texto)

    lee(3)
    os.write(fd, b"cd local\t")
    conexion = lee(3)
    os.write(fd, f"\x15ls {U}\t".encode())
    pool = lee(4)
    os.write(fd, b"\x15cd local#\t")
    seccion = lee(3)
    os.write(fd, b"\x15cd local#path/et\t")
    ruta = lee(3)
    # «..» se completa con su barra, como en bash: no ofrecía nada porque ningún hijo se
    # llama así. En una URL de datasets y dentro de `#path`.
    os.write(fd, b"\x15cd local/..\t")
    subir = lee(3)
    os.write(fd, b"\x15ls zfsm://local#path/etc/..\t")
    subir_path = lee(3)
    os.write(fd, b"\x15exit\r")
    lee(1)
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass
    problemas = []

    # Lo que quedó en la línea de órdenes: la ÚLTIMA fila del indicador. Debajo puede ir la
    # zona de estado de la pantalla, así que «lo último de la salida» ya no es la línea.
    def enLinea(texto):
        filas = [l.rstrip() for l in texto.replace("\r", "\n").split("\n") if l.startswith("zfsm://> ")]
        return filas[-1] if filas else ""
    # Una máquina sigue con «/» (sus pools) o con `#path/` (sus ficheros): se enseñan las dos.
    if not re.search(r"^\s+local/\s+local#path/\s*$", conexion.replace("\r", ""), re.M):
        problemas.append("`cd local<tab>` no enseña «local/  local#path/»")
    if not enLinea(seccion).endswith("cd local#path/"):
        problemas.append("`cd local#<tab>` no escribe «local#path/»")
    if not enLinea(ruta).endswith("cd local#path/etc/"):
        problemas.append("`cd local#path/et<tab>` no escribe «etc/»")
    if not enLinea(subir).endswith("cd local/../"):
        problemas.append("`cd local/..<tab>` no escribe «../»")
    if not enLinea(subir_path).endswith("ls zfsm://local#path/etc/../"):
        problemas.append("`ls …#path/etc/..<tab>` no escribe «../»")
    # Y las opciones se enseñan SIN lo que comparten: «ztfc16/  ztfc16#», no la URL entera
    # repetida delante de cada una.
    if not re.search(rf"^\s+{P}/\s+{P}#\s*$", pool.replace("\r", ""), re.M):
        problemas.append("`ls <pool><tab>` no enseña «<pool>/  <pool>#» sin la URL delante")
    if problemas:
        ctx.fallos.append((nombre, problemas,
                           (conexion + pool + seccion + ruta + subir + subir_path)[-600:]))
        print(f"  FALLO  {nombre}")
        for x in problemas:
            print(f"         {x}")
    else:
        ctx.pasados.append(nombre)
        print(f"  ok     {nombre}")


def casos_path(ctx):
    """`#path`: una ruta cualquiera de la máquina, entendida en TODAS las órdenes.

    Las que manejan ficheros la usan; las de datasets solo si es EXACTAMENTE el punto de
    montaje, y nunca suben al dataset que la contiene: `destroy` de un subdirectorio no puede
    acabar destruyendo el dataset. Lo mismo para `#content/sub` escrito a mano.
    """
    P = POOL_LOCAL
    prepara(ctx)
    # Los mensajes se comprueban en castellano: sin `--lang` saldrían en inglés.
    caso = lambda *a, **k: globals()["caso"](*a, idioma="es", **k)
    R = f"zfsm://local#path/mnt/{P}/datos"
    U = f"zfsm://local/{P}/datos"
    caso(ctx, "path: ls de un directorio cualquiera de la máquina",
         ["ls zfsm://local#path/etc"], rc=0, espera=[r"^hostname\s+file\s"])
    caso(ctx, "path: ls de un fichero da su fila",
         ["ls zfsm://local#path/etc/hostname"], rc=0, veces=[(r"^hostname\s+file\s", 1)])
    caso(ctx, "path: ls de #content tiene las mismas columnas",
         [f"ls {U}#content"], rc=0, espera=[r"^NOMBRE\s+TIPO\s+TAMAÑO\s+PERMISOS\s+DUEÑO",
                                            r"^fichero\.txt\s+file\s"])
    caso(ctx, "path: info dice a qué dataset pertenece y su URL #content",
         [f"info {R}/fichero.txt"], rc=0,
         espera=[rf"^dataset\s+{re.escape(U)}$",
                 rf"^content_url\s+{re.escape(U)}#content/fichero\.txt$"])
    caso(ctx, "path: info del punto de montaje lo dice",
         [f"info {R}"], rc=0, espera=[rf"^mountpoint_of\s+{re.escape(U)}$"])
    caso(ctx, "path: cd a un fichero se niega",
         [f"cd {R}/fichero.txt"], rc=1, espera=[r"no es un directorio"])
    caso(ctx, "path: cd a lo que no existe se niega",
         [f"cd {R}/no-existe"], rc=1, espera=[r"no existe"])
    caso(ctx, "path: cd y cd .. se mueven por directorios",
         [f"cd {R}/sub", "cd ..", "pwd", "cd ../../../..", "pwd"], rc=0,
         espera=[rf"^{re.escape(R)}$", r"^zfsm://local$"])
    caso(ctx, "content: cd .. sube un directorio, no un dataset",
         [f"cd {U}#content/sub", "cd ..", "pwd"], rc=0, espera=[rf"^{re.escape(U)}#content$"])
    caso(ctx, "path: el punto de montaje vale como el dataset, y lo dice",
         [f"get compression --on {R}"], rc=0, espera=[rf"→ {re.escape(U)}"])
    caso(ctx, "path: destroy de un subdirectorio se niega y da la URL #content",
         [f"destroy {R}/sub -y"], rc=1,
         espera=[rf"no es un dataset; está dentro de {P}/datos", r"#content/sub"],
         estado=[("el dataset sigue", f"zfs list -H -o name {P}/datos", rf"^{P}/datos$")])
    # `destroy ds#content/sub` borra ESE directorio (con -r), nunca el dataset que lo tiene.
    caso(ctx, "content: destroy de #content/sub sin -r se niega, y no toca el dataset",
         [f"destroy {U}#content/sub -y"], rc=2, espera=[r"es un directorio"],
         estado=[("el dataset sigue", f"zfs list -H -o name {P}/datos", rf"^{P}/datos$"),
                 ("y el directorio", f"bash -c 'test -d /mnt/{P}/datos/sub && echo SI'", r"^SI$")])
    # Una sección mal escrita se rechaza en el `cd`, no después en lo que se haga dentro.
    caso(ctx, "sección desconocida: cd la rechaza",
         [f"cd {U}#xonten"], rc=1, espera=[r"«#xonten» no es una sección"])
    caso(ctx, "sección desconocida: y cualquier otra orden también",
         [f"ls {U}#prop"], rc=1, espera=[r"«#prop» no es una sección"])
    caso(ctx, "path: una ruta suelta no es una máquina",
         ["devices zfsm://local#path/dev"], rc=1, espera=[r"no es una conexión"])
    caso(ctx, "path: #path en un dataset se rechaza",
         [f"ls {U}#path/x"], espera=[r"cuelga de una máquina"])
    caso(ctx, "path: comodines en el último tramo",
         [f"ls {R}/*.txt"], rc=0, espera=[r"^fichero\.txt\s"], no_espera=[r"otro\.txt"])
    dst = "/var/tmp/zfsmgr-prueba-path"
    sh(ctx, f"rm -rf {dst} && mkdir -p {dst}", root=True)
    caso(ctx, "path: rsync de un dataset a una ruta suelta",
         [f"rsync zfsm://local#path{dst} --on {U}#content/sub --wait"], rc=0,
         estado=[("el fichero llegó", f"cat {dst}/otro.txt", r"^anidado$")])
    caso(ctx, "path: rsync a una ruta que no existe se niega",
         [f"rsync zfsm://local#path{dst}/no-existe --on {U}"], rc=1,
         espera=[r"no es un directorio"])
    sh(ctx, f"rm -rf {dst}", root=True)
    # labelclear por #path, sobre un pool de FICHERO propio: nunca sobre un disco de verdad.
    img = f"{IMG_DIR}/ztpath.img"
    sh(ctx, f"zpool destroy ztpath", root=True)
    sh(ctx, f"rm -f {img} && truncate -s 128M {img}")
    sh(ctx, f"zpool create -f ztpath {img}", root=True)
    caso(ctx, "path: labelclear por #path se niega con el pool importado",
         [f"labelclear zfsm://local#path{img} -f -y"], rc=1)
    sh(ctx, "zpool destroy ztpath", root=True)
    caso(ctx, "path: labelclear por #path, con la máquina de la URL",
         [f"labelclear zfsm://local#path{img} -f -y"], rc=0, espera=[r"borrada la etiqueta"])
    sh(ctx, f"rm -f {img}")


def casos_info_labelclear(ctx):
    """`info` describe el objeto pedido, y `labelclear` borra la etiqueta de lo que no está en uso.

    `info` daba siempre la salud de la conexión, pidiera uno lo que pidiera. `labelclear` se
    prueba sobre un pool de fichero propio: NUNCA sobre un disco de verdad.
    """
    P = POOL_LOCAL
    prepara(ctx)
    U = f"zfsm://local/{P}"
    caso(ctx, "info de un pool: sus datos de pool y los de su dataset raíz",
         [f"info {U}"], rc=0, espera=[r"^kind\s+pool$", r"^pool\.health\s+ONLINE", r"^compression\s"])
    caso(ctx, "info de un dataset: sus propiedades, no la conexión",
         [f"info {U}/datos"], rc=0, espera=[r"^kind\s+dataset$", r"^used\s"],
         no_espera=[r"^connection\s"])
    caso(ctx, "info de un snapshot",
         [f"info {U}/datos@s1"], rc=0, espera=[r"^kind\s+snapshot$", r"^referenced\s"])
    caso(ctx, "info de un pool que no existe lo dice claro",
         ["info zfsm://local/noexiste-zfsmgr"], rc=1, idioma="es",
         espera=[r"no es un pool importado ni importable"])

    img = f"{IMG_DIR}/ztlabel.img"
    sh(ctx, f"bash -c 'zpool destroy ztlabel 2>/dev/null; rm -f {img}; truncate -s 256M {img}'", root=True)
    sh(ctx, f"zpool create -f -m none ztlabel {img}", root=True)
    importables = f"bash -c 'zpool import -d {IMG_DIR} 2>&1'"
    caso(ctx, "labelclear se niega sobre un pool importado, aunque se pida -f",
         [f"labelclear {img} -f --on zfsm://local"], rc=1,
         espera=[r"part of an imported pool"],
         estado=[("el pool sigue importado", "zpool list -H -o name ztlabel", r"^ztlabel$")])
    sh(ctx, "zpool export ztlabel", root=True)
    caso(ctx, "labelclear -f sobre un pool exportado borra su etiqueta",
         [f"labelclear {img} -f --on zfsm://local"], rc=0, idioma="es",
         espera=[r"borrada la etiqueta"],
         estado=[("ya no es importable", importables, r"\A(?![\s\S]*pool: ztlabel)")])
    caso(ctx, "labelclear exige una ruta absoluta",
         ["labelclear sda9 --on zfsm://local"], rc=2, idioma="es", espera=[r"necesita la ruta"])
    sh(ctx, f"rm -f {img}", root=True)


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
    # --- Una instantánea NO se monta, pero su contenido SÍ se ve.
    #
    # Vive bajo `<punto del dataset>/.zfs/snapshot/<nombre>`. El cliente preguntaba el
    # `mountpoint` DE LA INSTANTÁNEA —que es «-» siempre— y contestaba «no está montado en
    # ningún sitio» aunque el contenido estuviera ahí mismo. El caso mira que salga el
    # fichero, no que no salga el error: un mensaje que cambie de texto seguiría pasando.
    caso(ctx, "ls #content de una instantánea enseña lo que tenía dentro",
         [f"cd Local/{P}/datos@s1", "ls #content"], idioma="es",
         espera=[r"^fichero\.txt\b", r"^sub\b"],
         no_espera=[r"no está montado", r"not mounted"])
    caso(ctx, "ls #content de una instantánea baja a sus subdirectorios",
         [f"cd Local/{P}/datos@s1", "ls #content/sub"], idioma="es",
         espera=[r"^otro\.txt\b"])
    # Y que lo que enseña es la FOTO, no el dataset de ahora: si leyera el directorio vivo,
    # un fichero creado después de la instantánea saldría también.
    sh(ctx, f"bash -c 'echo posterior > /mnt/{P}/datos/despues.txt'", root=True)
    caso(ctx, "ls #content de una instantánea no enseña lo escrito DESPUÉS",
         [f"cd Local/{P}/datos@s1", "ls #content"], idioma="es",
         espera=[r"^fichero\.txt\b"], no_espera=[r"^despues\.txt\b"])
    caso(ctx, "ls #content del dataset sí enseña lo escrito después",
         [f"cd Local/{P}/datos", "ls #content"], idioma="es",
         espera=[r"^despues\.txt\b"])

    # --- `destroy ds#content/<ruta>` borra ESE fichero o directorio, nada más.
    existe = lambda ruta: f"bash -c 'test -e {ruta} && echo SI || echo NO'"
    caso(ctx, "destroy #content: borra un fichero del dataset",
         [f"cd Local/{P}/datos", "destroy #content/despues.txt"], idioma="es",
         espera=[r"borrado .*despues\.txt"],
         estado=[("el fichero ya no está", existe(f"/mnt/{P}/datos/despues.txt"), r"^NO$"),
                 ("el dataset sigue", f"zfs list -H -o name {P}/datos", rf"^{P}/datos$")])
    sh(ctx, f"bash -c 'echo otra > /mnt/{P}/datos/relativo.txt'", root=True)
    caso(ctx, "destroy: con el nombre suelto desde dentro de #content",
         [f"cd Local/{P}/datos#content", "destroy relativo.txt"], idioma="es",
         estado=[("borrado", existe(f"/mnt/{P}/datos/relativo.txt"), r"^NO$")])
    caso(ctx, "destroy #content: un directorio sin -r se niega",
         [f"cd Local/{P}/datos", "destroy #content/sub"], idioma="es",
         espera=[r"es un directorio"],
         estado=[("sigue ahí", existe(f"/mnt/{P}/datos/sub/otro.txt"), r"^SI$")])
    caso(ctx, "destroy -r #content: el directorio con lo de dentro",
         [f"cd Local/{P}/datos", "destroy -r #content/sub"], idioma="en",
         espera=[r"deleted"],
         estado=[("ya no está", existe(f"/mnt/{P}/datos/sub"), r"^NO$")])
    caso(ctx, "destroy #content/.zfs: las instantáneas son de solo lectura",
         [f"cd Local/{P}/datos", "destroy #content/.zfs/snapshot/s1/fichero.txt"], idioma="es",
         espera=[r"solo lectura"],
         estado=[("la foto intacta", existe(f"/mnt/{P}/datos/.zfs/snapshot/s1/fichero.txt"),
                  r"^SI$")])
    caso(ctx, "destroy #content de una instantánea: se niega",
         [f"cd Local/{P}/datos@s1", "destroy #content/fichero.txt"], idioma="es",
         espera=[r"solo lectura"], no_espera=[r"DESTRUIR"])
    # Un dataset hijo montado DENTRO no es un directorio del padre: no se entra en él.
    sh(ctx, f"zfs create {P}/datos/hijo", root=True)
    sh(ctx, f"bash -c 'echo h > /mnt/{P}/datos/hijo/h.txt'", root=True)
    caso(ctx, "destroy -r #content: no entra en un dataset hijo montado dentro",
         [f"cd Local/{P}/datos", "destroy -r #content/hijo"], idioma="es",
         espera=[r"otro sistema de ficheros"],
         estado=[("el hijo intacto", existe(f"/mnt/{P}/datos/hijo/h.txt"), r"^SI$")])
    sh(ctx, f"zfs destroy {P}/datos/hijo", root=True)

    # `zfs allow` sobre una instantánea escribe su error por la salida y devuelve rc=0, así
    # que el código de retorno no sirve para detectarlo: llegaba al usuario el mensaje crudo
    # de ZFS, que habla de la sintaxis de una orden que él no ha escrito.
    caso(ctx, "ls #permissions de una instantánea explica, no suelta el error de ZFS",
         [f"cd Local/{P}/datos@s1", "ls #permissions"], idioma="es",
         espera=[r"no tiene permisos delegados", rf"zfsm://local/{P}/datos"],
         no_espera=[r"snapshot delimiter", r"Failed to open dataset"])
    caso(ctx, "ls #permissions de una instantánea, también en inglés",
         [f"cd Local/{P}/datos@s1", "ls #permissions"], idioma="en",
         espera=[r"no delegated permissions"], no_espera=[r"snapshot delimiter"])

    # El mensaje en una frase por idioma: «puesta»/«quitada» iban sin traducir.
    caso(ctx, "hold: el hold queda puesto",
         [f"cd Local/{P}/datos@s1", "hold prueba"], idioma="es",
         espera=[r"puesto el hold «prueba»"],
         estado=[("el hold existe", f"zfs holds -H {P}/datos@s1", r"prueba")])
    caso(ctx, "holds: y se ve desde el CLI",
         [f"cd Local/{P}/datos@s1", "holds"], espera=[r"prueba"])
    caso(ctx, "release: el hold se quita, y en inglés lo dice en inglés",
         [f"cd Local/{P}/datos@s1", "release prueba"], idioma="en",
         espera=[r"hold «prueba» released"], no_espera=[r"quitad"],
         estado=[("ya no hay hold", f"zfs holds -H {P}/datos@s1", r"^$")])
    caso(ctx, "clone: con propiedades, -p y -u",
         [f"cd Local/{P}/datos", f"clone {P}/nuevo/padre/clon2 compression=zstd -p -u --from @s1"],
         estado=[("con su propiedad y sin montar",
                  f"zfs get -H -o value compression,mounted {P}/nuevo/padre/clon2",
                  r"^zstd\nno$")])
    sh(ctx, f"zfs destroy -r {P}/nuevo", root=True)
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
    # By scope, as the GUI's permissions tab removes what it changed.
    caso(ctx, "allow: solo en este dataset con --local",
         [f"cd Local/{P}/datos", "allow --user linarese mount --local"],
         estado=[("en la sección local", f"zfs allow {P}/datos",
                  r"Local permissions:\n\s+user linarese mount")])
    caso(ctx, "unallow: --local quita lo de ese alcance",
         [f"cd Local/{P}/datos", "unallow --user linarese mount --local"], no_espera=[r"error"],
         estado=[("ya no aparece", f"zfs allow {P}/datos", r"\A(?![\s\S]*linarese mount)")])
    caso(ctx, "allow: --create sin destinatario",
         [f"cd Local/{P}/datos", "allow --create destroy"],
         estado=[("en la sección al crear", f"zfs allow {P}/datos",
                  r"Create time permissions:\n\s+destroy")])
    caso(ctx, "unallow: --create lo quita",
         [f"cd Local/{P}/datos", "unallow --create destroy"], no_espera=[r"error"],
         estado=[("ya no aparece", f"zfs allow {P}/datos", r"\A(?![\s\S]*Create time)")])


def casos_programacion(ctx):
    P = POOL_LOCAL
    prepara(ctx)   # terreno limpio: ningún grupo hereda lo que dejó otro
    caso(ctx, "schedule: programa instantáneas diarias",
         [f"cd Local/{P}/datos", "schedule --daily 7"],
         estado=[("la propiedad GSA queda puesta",
                  f"zfs get -H -o value org.zfsmgr:schedule-daily {P}/datos", r"^7$")])
    caso(ctx, "schedule: sin argumentos la enseña",
         [f"cd Local/{P}/datos", "schedule"], espera=[r"7"])
    caso(ctx, "schedule --clear: la borra",
         [f"cd Local/{P}/datos", "schedule --clear"],
         estado=[("ya no hay programación",
                  f"zfs get -H -o value org.zfsmgr:schedule-daily {P}/datos", r"^-$")])


def casos_gsa(ctx):
    """El planificador de snapshots: nivelación por guid con -I, poda del destino a la vez
    que el origen, snapshots sin cambios que se renombran y el estado que lee el CLI.

    Origen y destino en el MISMO pool de pruebas de fc16 (fc16 se llama a sí misma «local»),
    así la nivelación va por la tubería local y no depende de la red. Las pasadas se lanzan
    con `schedule run`, que hace la de UN dataset: nunca una pasada global.

    Para que una clase vuelva a «tocar» sin esperar un día se renombra el último snapshot
    diario del origen a una fecha vieja: es justo el estado de una máquina cuyo último
    snapshot es de ayer. El hilo del daemon también pasa cada cinco minutos; por eso se
    comprueba el ESTADO del pool y no las líneas del registro.
    """
    P = POOL_LOCAL
    O = f"{P}/datos"
    C = f"{P}/copia"
    snaps = lambda ds: f"zfs list -H -o name -t snapshot -s createtxg -d 1 {ds}"
    # El último diario del origen, a una fecha vieja: «ayer» para el planificador.
    envejece = (f"bash -c 'u=$({snaps(O)} | grep @GSA-daily- | tail -1); "
                f"[ -n \"$u\" ] && zfs rename \"$u\" {O}@GSA-daily-$1-000000' _ ")
    sh(ctx, f"zfs set atime=off {O}", root=True)

    caso(ctx, "gsa: --to sin URL no vale",
         [f"cd Local/{O}", f"schedule --daily 3 --to local::{C}"],
         rc=1, idioma="es", espera=[r"URL de un dataset"])
    caso(ctx, "gsa: --to con una conexión sola no vale",
         [f"cd Local/{O}", "schedule --daily 3 --to zfsm://local"],
         rc=1, idioma="es", espera=[r"URL de un dataset"])
    caso(ctx, "gsa: --to con la URL, y se guarda como URL",
         [f"cd Local/{O}", f"schedule --daily 3 --to zfsm://local/{C} --level"], rc=0,
         estado=[("destino guardado",
                  f"zfs get -H -o value org.zfsmgr:schedule-target {O}", rf"^zfsm://local/{C}$")])
    caso(ctx, "gsa: schedules enseña el destino como URL",
         [f"cd Local/{O}", "schedule"], espera=[rf"zfsm://local/{C}"])

    caso(ctx, "gsa: schedule run hace el snapshot y la primera nivelación",
         [f"cd Local/{O}", "schedule run --wait"], rc=0,
         estado=[("el diario en el origen", snaps(O), r"@GSA-daily-\d{8}-\d{6}$"),
                 ("y en el destino", snaps(C), r"@GSA-daily-\d{8}-\d{6}$")])
    caso(ctx, "gsa: schedule status en verde",
         [f"cd Local/{O}", "schedule status"], rc=0, idioma="es",
         espera=[rf"local\s+{O}\s+GSA-daily-\S+\s+ok .*verde"])

    # Sin cambios: el diario de «ayer» se RENOMBRA al de hoy y no se crea otro. En el destino
    # se repite el renombrado (mismo guid) y no viaja nada.
    sh(ctx, envejece + "20200101", root=True)
    caso(ctx, "gsa: sin cambios se renombra el anterior en vez de crear otro",
         [f"cd Local/{O}", "schedule run --wait"], rc=0,
         estado=[("un solo diario en el origen, de hoy",
                  f"bash -c '{snaps(O)} | grep -c @GSA-daily-'", r"^1$"),
                 ("el de 2020 ya no está", snaps(O), r"\A(?![\s\S]*20200101)"),
                 ("el destino tiene el mismo nombre",
                  f"bash -c 'diff <({snaps(O)} | grep -o @GSA.*) <({snaps(C)} | grep -o @GSA.*) && echo IGUALES'",
                  r"IGUALES")])

    # Con cambios y un snapshot manual por medio: -I lo lleva también al destino.
    sh(ctx, f"bash -c 'echo cambio > /mnt/{O}/nuevo.txt'", root=True)
    sh(ctx, f"zfs snapshot {O}@manual1", root=True)
    sh(ctx, f"bash -c 'echo otro >> /mnt/{O}/nuevo.txt'", root=True)
    sh(ctx, envejece + "20200102", root=True)
    caso(ctx, "gsa: con cambios, un diario nuevo; -I trae también el manual intermedio",
         [f"cd Local/{O}", "schedule run --wait"], rc=0,
         estado=[("dos diarios en el origen", f"bash -c '{snaps(O)} | grep -c @GSA-daily-'", r"^2$"),
                 ("el manual en el destino", snaps(C), rf"^{C}@manual1$"),
                 ("el renombrado también en el destino", snaps(C), r"@GSA-daily-20200102-000000$")])

    # Bajar la retención poda el origen Y el destino; el manual del destino se queda.
    caso(ctx, "gsa: bajar la retención poda el origen y el destino a la vez",
         [f"cd Local/{O}", "schedule --daily 1", "schedule run --wait"], rc=0,
         estado=[("un diario en el origen", f"bash -c '{snaps(O)} | grep -c @GSA-daily-'", r"^1$"),
                 ("un diario en el destino", f"bash -c '{snaps(C)} | grep -c @GSA-daily-'", r"^1$"),
                 ("el de 2020 fuera del destino", snaps(C), r"\A(?![\s\S]*20200102)"),
                 ("el manual del destino sigue", snaps(C), rf"^{C}@manual1$")])

    # Un snapshot propio del destino por detrás del último común: no se manda nada.
    sh(ctx, f"zfs snapshot {C}@ajeno", root=True)
    sh(ctx, f"bash -c 'echo mas >> /mnt/{O}/nuevo.txt'", root=True)
    sh(ctx, envejece + "20200103", root=True)
    caso(ctx, "gsa: un snapshot propio del destino para la nivelación y dice cuál",
         [f"cd Local/{O}", "schedule run --wait"], rc=1, espera=[r"ajeno", r"recv -F"],
         estado=[("el ajeno sigue en el destino", snaps(C), rf"^{C}@ajeno$")])
    caso(ctx, "gsa: schedule status en rojo con el motivo",
         [f"cd Local/{O}", "schedule status"], rc=1, idioma="es",
         espera=[r"\brojo\b", rf"zfsm://local/{O} — la nivelación a zfsm://local/{C} falló .*ajeno"])
    caso(ctx, "gsa: schedule status -a también lo ve",
         ["schedule status -a"], rc=1, idioma="es", espera=[r"ajeno"])
    sh(ctx, f"zfs destroy {C}@ajeno", root=True)
    caso(ctx, "gsa: quitado el ajeno, la siguiente pasada reintenta sola y vuelve a verde",
         [f"cd Local/{O}", "schedule run --wait", "schedule status"], rc=0, idioma="es",
         espera=[r"\bverde\b"], no_espera=[r"\brojo\b"])

    # La nivelación es un TRABAJO: sin --wait se dice cuál y se vuelve.
    sh(ctx, f"bash -c 'echo trabajo >> /mnt/{O}/nuevo.txt'", root=True)
    sh(ctx, envejece + "20200104", root=True)
    caso(ctx, "gsa: schedule run sin --wait lanza la nivelación como trabajo y lo dice",
         [f"cd Local/{O}", "schedule run"], rc=0, idioma="es",
         espera=[r"nivelación en marcha como trabajo [0-9a-f]{16} en local", r"siga con «job"])
    caso(ctx, "gsa: el trabajo sale en jobs como gsa-level",
         ["cd local", "jobs --history"], espera=[rf"\sgsa-level\s+{O}\s"])
    caso(ctx, "gsa: schedule run --wait con nada nuevo no lanza nada y sigue en verde",
         [f"cd Local/{O}", "schedule run --wait", "schedule status"], rc=0, idioma="es",
         espera=[r"\bverde\b"])
    caso(ctx, "gsa: --wait solo vale con run",
         [f"cd Local/{O}", "schedule --wait"], rc=1, idioma="es", espera=[r"sobra"])

    caso(ctx, "gsa: schedule status sin conexión desde la raíz lo pide",
         ["schedule status"], rc=1, idioma="es", espera=[r"hace falta estar en una conexión"])
    caso(ctx, "gsa: schedule status de una conexión por su nombre",
         ["schedule status local"], rc=0, espera=[rf"{O}"])
    caso(ctx, "gsa: una palabra que no es status ni run es un error",
         [f"cd Local/{O}", "schedule otra"], rc=1, idioma="es", espera=[r"sobra"])
    caso(ctx, "gsa: schedule run sin programación lo dice",
         [f"cd Local/{P}", "schedule run --wait"], rc=1, espera=[r"no schedule enabled"])
    caso(ctx, "gsa: --no-to quita el destino y deja el resto",
         [f"cd Local/{O}", "schedule --no-to --no-level"], rc=0,
         estado=[("sin destino", f"zfs get -H -o value org.zfsmgr:schedule-target {O}", r"^-?$"),
                 ("la diaria sigue", f"zfs get -H -o value org.zfsmgr:schedule-daily {O}", r"^[1-9]")])
    caso(ctx, "gsa: --clear la quita",
         [f"cd Local/{O}", "schedule --clear"], rc=0,
         estado=[("sin destino", f"zfs get -H -o value org.zfsmgr:schedule-target {O}", r"^-$")])


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
    # Los CONTADORES del trabajo, que es otra cosa que la línea de progreso.
    #
    # Un trabajo de mutación dejaba `bytes` y `MiB/s` en CERO de principio a fin: el
    # muestreador calculaba las dos cifras, las pegaba dentro de una frase —«5,7 GiB
    # copiados a 17,6 MiB/s»— y tiraba los números. `jobs` enseñaba «0B» y «0.00» mientras
    # la máquina movía gigabytes, y un cero no se lee como «no lo sé», se lee como «no está
    # pasando nada». Es el mismo síntoma que ya nos costó una tarde con `send-to-peer`.
    #
    # SIN `--wait`: con él la orden va por el camino síncrono y no crea trabajo ninguno, así
    # que no habría nada que contar.
    sh(ctx, f"bash -c 'mkdir -p /mnt/{P}/datos/medido && dd if=/dev/urandom "
            f"of=/mnt/{P}/datos/medido/f.bin bs=1M count=40 status=none && sync'", root=True)
    rc, salida = cli(ctx, [f"cd Local/{P}/datos", "breakdown medido hijomedido"])
    ident = ""
    m = re.search(r"\b([0-9a-f]{16})\b", salida)
    if m:
        ident = m.group(1)
    # Se espera a que termine preguntando, no durmiendo a ojo.
    fila = ""
    for _ in range(30):
        # `format` es un VERBO del intérprete; `--format` solo vale al arrancar el binario.
        _, vista = cli(ctx, ["format tsv", "jobs --history --on zfsm://local"])
        # Por el identificador, no por la posición: la tabla lleva delante la columna de la
        # conexión desde que `jobs -a` junta varias máquinas.
        for l in vista.splitlines():
            if ident and ident in l.split("\t"):
                c = l.split("\t")
                fila = "\t".join(c[c.index(ident):])
        if fila and fila.split("\t")[1] in ("done", "failed", "cancelled"):
            break
        sh(ctx, "sleep 2")
    nombre = "jobs: un trabajo de mutación cuenta los bytes y el ritmo"
    problemas = []
    if not ident:
        problemas.append(f"no se obtuvo identificador de trabajo: «{salida.strip()[:120]}»")
    elif not fila:
        problemas.append(f"el trabajo {ident} no aparece en «jobs --history»")
    else:
        campos = fila.split("\t")
        estado, bytes_, ritmo = campos[1], campos[4], campos[5]
        if estado != "done":
            problemas.append(f"el trabajo terminó en «{estado}»: {campos[-1][:100]}")
        # 40 MiB copiados: los bytes tienen que estar en ese orden de magnitud, no en cero.
        if not bytes_.isdigit() or int(bytes_) < 1_000_000:
            problemas.append(f"bytes={bytes_}: el contador se quedó en cero o casi")
        try:
            if float(ritmo) <= 0.0:
                problemas.append(f"MiB/s={ritmo}: el ritmo se quedó en cero")
        except ValueError:
            problemas.append(f"MiB/s={ritmo} no es un número")
    if problemas:
        ctx.fallos.append((nombre, problemas, salida))
        print(f"  FALLO  {nombre}")
        for x in problemas: print(f"         {x}")
    else:
        ctx.pasados.append(nombre); print(f"  ok     {nombre}")
    sh(ctx, f"zfs destroy -r {P}/datos/hijomedido", root=True)

    caso(ctx, "assemble: y vuelve a ser un directorio (nombre COMPLETO)",
         [f"cd Local/{P}/datos", f"assemble {P}/datos/hijosub --wait"],
         estado=[("el dataset ya no existe", f"zfs list -H -o name {P}/datos/hijosub 2>&1",
                  r"does not exist|no existe")])
    # El destino se BORRA antes: sobrevivía entre ejecuciones, y la prueba leía el fichero de
    # una pasada anterior dando por bueno un `todir` que había fallado (pasó el 2026-10-11).
    sh(ctx, "rm -rf /var/tmp/zfsmgr-pruebas/volcado /var/tmp/zfsmgr-pruebas/volcado.zfsmgr-new",
       root=True)
    caso(ctx, "todir: vuelca el dataset en un directorio llano",
         [f"cd Local/{P}/datos", "todir /var/tmp/zfsmgr-pruebas/volcado --wait"], rc=0,
         estado=[("el fichero está en el directorio",
                  "bash -c 'cat /var/tmp/zfsmgr-pruebas/volcado/fichero.txt 2>&1'",
                  r"hola")])
    sh(ctx, f"zfs mount {P}/datos", root=True)
    caso(ctx, "todir: deja el origen con canmount=off (efecto lateral, sin avisar)",
         [], estado=[("canmount quedó en off",
                      f"zfs get -H -o value canmount {P}/datos", r"^off$")])
    sh(ctx, f"zfs set canmount=on {P}/datos", root=True)
    sh(ctx, f"zfs mount {P}/datos", root=True)
    # --- `cp`: copiar de donde sea a donde sea.
    #
    # Lo que no hacía ninguna de las tres órdenes que ya copian: `rsync` exige datasets en
    # los dos extremos, `todir` saca de un dataset a un directorio LOCAL y `fromdir` mete un
    # directorio en el dataset actual. Entre las tres no se podía copiar de una ruta suelta a
    # otra, ni un fichero suelto.
    base = f"{IMG_DIR}/cp"
    sh(ctx, f"rm -rf {base}", root=True)
    sh(ctx, f"bash -c 'mkdir -p {base}/origen/sub {base}/destino && "
            f"echo hola > {base}/origen/a.txt && echo mundo > {base}/origen/sub/b.txt && "
            f"echo suelto > {base}/fichero.txt'")
    caso(ctx, "cp: un árbol de una ruta suelta a otra",
         [f"cp {base}/origen {base}/destino --wait"],
         estado=[("el árbol aterriza con su nombre", f"find {base}/destino -type f | sort",
                  rf"{base}/destino/origen/a\.txt"),
                 ("y el fichero anidado también",
                  f"cat {base}/destino/origen/sub/b.txt", r"^mundo$")])
    # Un FICHERO suelto, que es lo que ninguna de las otras tres sabe hacer. Y el defecto que
    # destapó: el daemon pegaba una «/» a los dos lados, y `rsync f.txt/ dst/` falla con
    # «change_dir … Not a directory». Se arregló donde se puede SABER si una ruta es un
    # fichero: en el daemon, que es quien tiene el sistema de ficheros delante.
    caso(ctx, "cp: un fichero suelto, y llega como fichero",
         [f"cp {base}/fichero.txt {base}/destino --wait"],
         estado=[("es un fichero y no un directorio",
                  f"bash -c 'test -f {base}/destino/fichero.txt && echo FICHERO || echo NO'",
                  r"^FICHERO$"),
                 ("con su contenido", f"cat {base}/destino/fichero.txt", r"^suelto$")])
    caso(ctx, "cp: «--as» cambia el nombre con el que aterriza",
         [f"cp {base}/origen {base}/destino --as otro --wait"],
         estado=[("aterriza con el nombre pedido", f"ls {base}/destino", r"^otro$"),
                 ("y con su contenido dentro", f"cat {base}/destino/otro/a.txt", r"^hola$")])
    # El destino es el CONTENEDOR, así que un fichero NO se mete dentro de un directorio que
    # se llame igual: eso daba «destino/f.txt/f.txt» en silencio. `cp` de toda la vida se
    # niega, y esto también.
    sh(ctx, f"mkdir -p {base}/destino/choque.txt && echo x > {base}/choque.txt", root=True)
    caso(ctx, "cp: se niega a meter un fichero dentro de un directorio homónimo",
         [f"cp {base}/choque.txt {base}/destino --wait"],
         espera=[r"already has a DIRECTORY|ya hay un DIRECTORIO"],
         estado=[("y no ha anidado nada",
                  f"bash -c 'test -e {base}/destino/choque.txt/choque.txt && echo ANIDÓ || echo LIMPIO'",
                  r"^LIMPIO$")])
    caso(ctx, "cp: de una ruta suelta a un dataset",
         [f"cp {base}/origen zfsm://local/{P}/datos --wait"],
         estado=[("el árbol está dentro del dataset",
                  f"cat /mnt/{P}/datos/origen/sub/b.txt", r"^mundo$")])
    caso(ctx, "cp: de un subárbol de dataset a una ruta suelta",
         [f"cp zfsm://local/{P}/datos#content/origen/sub {base}/destino --wait"],
         estado=[("llega el subárbol", f"cat {base}/destino/sub/b.txt", r"^mundo$")])
    # La fecha con NANOSEGUNDOS.
    #
    # NO distingue un motor del otro: se comprobó devolviendo `cp` al `rsync` del sistema y
    # el caso pasaba igual, porque el rsync de este Linux también los conserva. Se queda
    # como guardián de una PROPIEDAD del motor que usamos ahora —`copytree` pone la fecha
    # con `utimensat`—, no como prueba de que uno sea mejor que el otro. La diferencia entre
    # los dos es la dependencia externa, no esto.
    sh(ctx, f"bash -c 'echo n > {base}/nano.txt && "
            f"touch -d \"2019-05-06 07:08:09.123456789\" {base}/nano.txt'")
    caso(ctx, "cp: la fecha llega con los nanosegundos exactos",
         [f"cp {base}/nano.txt {base}/destino --wait"],
         estado=[("la fecha es la misma hasta el nanosegundo",
                  f"stat -c %y {base}/destino/nano.txt", r"2019-05-06.*09:08:09\.123456789")])
    caso(ctx, "cp: «--check» no toca nada",
         [f"cp {base}/origen {base}/destino --as simulacro --check"],
         no_espera=[r"error"],
         estado=[("no se ha creado nada",
                  f"bash -c 'test -e {base}/destino/simulacro && echo CREÓ || echo NADA'",
                  r"^NADA$")])
    # Lo que se RECHAZA, en vez de interpretarse.
    caso(ctx, "cp: una sección que no nombra ficheros se rechaza",
         [f"cp zfsm://local/{P}/datos#properties {base}/destino"],
         idioma="es", espera=[r"no nombra ficheros"])
    caso(ctx, "cp: un «..» en la ruta se rechaza",
         [f"cp /var/../etc/shadow {base}/destino"], idioma="es",
         espera=[r"no puede llevar «\.\.»"])
    caso(ctx, "cp: «#path» colgando de un dataset se rechaza",
         [f"cp zfsm://local/{P}/datos#path/x {base}/destino"],
         idioma="es", espera=[r"cuelga de una máquina"])
    sh(ctx, f"rm -rf {base}", root=True)

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
         ["cd Local", "import"], no_espera=[r"falta <|missing <"])
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
    # `zpool import` ya monta los datasets de ESE pool, y solo los de ese. Aquí había un
    # `zfs mount -a`, que monta los de TODOS los pools importados: el 2026-10-11 montó los
    # hijos de un entorno de arranque de otro pool (/usr, /var, /home, /etc heredados) encima
    # de los de fc16 y tumbó la máquina. Ver `sh`, que ya no deja pasar órdenes globales.
    sh(ctx, f"zpool import -d {IMG_DIR} {P}", root=True)


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
         espera=[r"done"], no_espera=[r"failed|no movió|did not move"],
         estado=[("el dataset está en unib", f"zfs list -H -o name {R}/de_fc16",
                  rf"^{R}/de_fc16$", True),
                 ("con el fichero dentro",
                  f"bash -c 'zfs mount {R}/de_fc16 2>/dev/null; cat /mnt/{R}/de_fc16/fichero.txt'",
                  r"hola desde fc16", True)])
    caso(ctx, "send: unib -> fc16, completo",
         [f"cd unib/{R}/datos@env", f"send zfsm://local/{L}/de_unib --wait"],
         espera=[r"done"], no_espera=[r"failed|no movió|did not move"],
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
    # `cp` entre máquinas con el destino que no existe: el fichero llega con OTRO nombre
    # (lo pone el emisor con `--as`) y el directorio de llegada con el dueño del original.
    w = "/var/tmp/zfsmgr-prueba-cpx"
    sh(ctx, f"rm -rf {w}", root=True)
    sh(ctx, f"rm -rf {w}", True, root=True)
    os.makedirs(f"{w}/dir", exist_ok=True)
    with open(f"{w}/f.txt", "w") as f:
        f.write("entre maquinas\n")
    with open(f"{w}/dir/a", "w") as f:
        f.write("x\n")
    os.chmod(f"{w}/dir", 0o750)
    sh(ctx, f"mkdir -p {w}", True)
    caso(ctx, "cp entre máquinas: fichero con otro nombre",
         [f"cp {w}/f.txt zfsm://unib#path{w}/renombrado.txt --wait"], rc=0,
         estado=[("llega como renombrado.txt", f"cat {w}/renombrado.txt",
                  r"^entre maquinas$", True)])
    caso(ctx, "cp entre máquinas: directorio a un destino que no existe",
         [f"cp {w}/dir zfsm://unib#path{w}/copia --wait"], rc=0,
         estado=[("con su contenido", f"cat {w}/copia/a", r"^x$", True),
                 ("con el modo del original, no 755", f"stat -c '%a' {w}/copia", r"^750$",
                  True)])
    sh(ctx, f"rm -rf {w}", root=True)
    sh(ctx, f"rm -rf {w}", True, root=True)
    caso(ctx, "jobs: la máquina enseña sus trabajos",
         ["cd Local", "jobs --history"], espera=[r"done|failed|ID|JOB"])


# `casos_pool_export` va el ÚLTIMO de los locales A PROPÓSITO: exporta el pool, y si algo
# sale mal ahí, todo lo que viniera detrás fallaría por no tener pool y no por su culpa.
# En la primera pasada pasó justo eso y escondió el resultado de las transferencias.
def casos_version_openzfs(ctx):
    """No se instala el agente donde no hay OpenZFS 2.3 o posterior.

    Un agente instalado donde no hay ZFS arranca, contesta STATUS=OK y no ve un solo pool:
    desde el cliente eso se lee como «la máquina está bien y está vacía», que es lo que NO
    es. Mismo razonamiento que el del respaldo por guion: una máquina que parece atendida y
    no lo está es peor que una que falla a la cara.

    Se prueba con una conexión que apunta a una máquina sin OpenZFS. No se inventa: se mira
    si hay alguna a mano, y si no la hay se dice y se salta, porque un caso que se salta en
    silencio es un caso que no existe.
    """
    sin_zfs = os.environ.get("ZFSMGR_PRUEBA_HOST_SIN_ZFS", "")
    if not sin_zfs:
        print("  (saltado: defina ZFSMGR_PRUEBA_HOST_SIN_ZFS=usuario@maquina "
              "con una máquina SIN OpenZFS para probar el rechazo)")
        return
    usuario, _, maquina = sin_zfs.partition("@")
    if not maquina:
        maquina, usuario = usuario, "linarese"
    # Una configuración de usar y tirar: la del usuario no se toca.
    cfg = os.path.join(ctx.dir_tmp, "cfg-sin-zfs")
    if os.path.isdir(cfg):
        shutil.rmtree(cfg)
    shutil.copytree(os.path.expanduser("~/.config/ZFSMgr"), cfg)
    ruta = os.path.join(cfg, "config.json")
    with open(ruta) as f:
        c = json.load(f)
    # La clave es `conn_type`, no `type`: se miró en el fichero en vez de suponerlo, después
    # de que la primera versión no encontrara ninguna conexión y se saltara el caso entero
    # —un caso que se salta solo es un caso que no prueba nada—.
    plantilla = next((x for x in c["connections"]
                      if str(x.get("conn_type", "")).upper() == "SSH"), None)
    if plantilla is None:
        print("  (saltado: no hay ninguna conexión SSH de la que copiar las credenciales)")
        return
    nueva = dict(plantilla)
    nueva.update({"id": "sinzfs", "name": "sinzfs", "host": maquina, "username": usuario})
    # El sistema se le PREGUNTA a la máquina. Copiar el de la plantilla hacía que una prueba
    # contra una máquina Linux buscara el agente de macOS: el rechazo pasa igual —ocurre
    # antes de mirar la plataforma— pero el registro del fallo sale hablando de otra cosa,
    # y un mensaje que despista vale menos que ninguno.
    _, uname = sh(ctx, f"ssh -o BatchMode=yes -o ConnectTimeout=10 {shlex.quote(sin_zfs)} 'uname -s'")
    uname = uname.strip().lower()
    nueva["os_type"] = {"linux": "Linux", "darwin": "macos", "freebsd": "FreeBSD"}.get(uname, "Linux")
    c["connections"].append(nueva)
    with open(ruta, "w") as f:
        json.dump(c, f, ensure_ascii=False, indent=2)

    guion = "install-daemon --on zfsm://sinzfs\nexit\n"
    fichero = os.path.join(ctx.dir_tmp, ".maestra")
    orden = (f"exec 9<{fichero}; exec {CLI!r} --password-fd 9 --config-dir {shlex.quote(cfg)} "
             f"--lang es --format text -y")
    # El plazo es CORTO y agotarlo es un FALLO del caso, no una excepción que se lleve por
    # delante la batería entera. Negarse es instantáneo —se mira la versión y se vuelve—;
    # tardar dos minutos significa que no se negó y que está instalando de verdad, que es
    # justo lo que no debe pasar. La primera versión de esto usaba 300 s y, al desarmar la
    # puerta para comprobar que el caso mordía, reventó con un TimeoutExpired sin llegar a
    # decir qué había pasado.
    nombre = "install-daemon: se niega donde no hay OpenZFS 2.3+"
    problemas = []
    try:
        p = subprocess.run(["bash", "-c", orden], input=guion, capture_output=True,
                           text=True, timeout=120)
        salida = p.stdout + p.stderr
    except subprocess.TimeoutExpired as e:
        # Lo capturado hasta el plazo llega en BYTES aunque se pidiera `text=True`: al
        # expirar, `communicate` devuelve los trozos crudos. Se decode cada uno por separado
        # y ANTES de juntarlos; hacerlo después revienta con «can only concatenate str».
        def _texto(x):
            if x is None:
                return ""
            return x.decode(errors="replace") if isinstance(x, bytes) else x
        salida = _texto(e.stdout) + _texto(e.stderr)
        problemas.append("no se negó: tardó más de 120 s, o sea que se puso a instalar")
    if "no se instala nada" not in salida:
        problemas.append(f"no se negó: «{salida.strip()[:200]}»")
    if "2.3" not in salida:
        problemas.append("no dice cuál es la versión mínima")
    # Y lo que NO puede pasar: que haya llegado a tocar la máquina.
    if re.search(r"daemon instalado", salida):
        problemas.append("DIJO QUE LO INSTALÓ pese a no haber OpenZFS")
    if problemas:
        ctx.fallos.append((nombre, problemas, salida))
        print(f"  FALLO  {nombre}")
        for x in problemas:
            print(f"         {x}")
    else:
        ctx.pasados.append(nombre)
        print(f"  ok     {nombre}")
    # Y que de verdad no dejó nada puesto en la otra máquina.
    rc, vista = sh(ctx, f"ssh -o BatchMode=yes {shlex.quote(sin_zfs)} "
                        f"'ls /usr/local/libexec/zfsmgr-agent 2>/dev/null || echo LIMPIA'")
    nombre2 = "install-daemon: y no deja nada puesto en la máquina rechazada"
    if "LIMPIA" in vista:
        ctx.pasados.append(nombre2); print(f"  ok     {nombre2}")
    else:
        ctx.fallos.append((nombre2, [f"quedó algo: {vista.strip()[:120]}"], vista))
        print(f"  FALLO  {nombre2}")
    shutil.rmtree(cfg, ignore_errors=True)

    # --- Y la otra mitad de la regla: el CLIENTE sí se instala sin OpenZFS.
    #
    # El agente necesita ZFS —sin él no ve un pool— pero el intérprete no: su trabajo es
    # hablar con los daemons de OTRAS máquinas. Un portátil de administración sin un solo
    # pool propio es un caso normal, y durante unas horas el instalador se lo negaba por
    # confundir el cliente con el agente.
    paquete = os.path.join(RAIZ, "dist")
    # El de SOLO CLI: el completo trae además las interfaces, que aquí no se prueban.
    zips = sorted(f for f in os.listdir(paquete)
                  if f.startswith("zfsmgr-cli-") and f.endswith(".zip")) if os.path.isdir(paquete) else []
    nombre3 = "instalar-linux.sh: el cliente SÍ se instala en una máquina sin OpenZFS"
    if not zips:
        print(f"  (saltado: {nombre3} — no hay zfsmgr-cli-*.zip en dist/; "
              f"ejecute scripts/empaqueta.sh --solo-cli)")
    else:
        z = os.path.join(paquete, zips[-1])
        base = os.path.basename(z)[:-4]
        remoto_q = shlex.quote(sin_zfs)
        sh(ctx, f"scp -q -o BatchMode=yes {shlex.quote(z)} {remoto_q}:/tmp/")
        rc, vista = sh(ctx,
            f"ssh -o BatchMode=yes {remoto_q} 'cd /tmp && rm -rf zpprueba && mkdir zpprueba "
            f"&& cd zpprueba && (unzip -q ../{base}.zip 2>/dev/null || 7z x -y ../{base}.zip "
            f">/dev/null) && cd {base} && sh instalar-linux.sh --prefijo \"$HOME/zpprueba\" "
            f"--sin-daemon 2>&1; "
            f"echo RC=$?; $HOME/zpprueba/bin/zfsmgr-cli version 2>&1'")
        problemas = []
        if "RC=0" not in vista:
            problemas.append(f"no se instaló: «{vista.strip()[-200:]}»")
        if "como CLIENTE" not in vista:
            problemas.append("no explica que se instala como cliente")
        if "zfsmgr-cli 0." not in vista:
            problemas.append("el binario instalado no arranca ahí")
        if problemas:
            ctx.fallos.append((nombre3, problemas, vista))
            print(f"  FALLO  {nombre3}")
            for x in problemas:
                print(f"         {x}")
        else:
            ctx.pasados.append(nombre3)
            print(f"  ok     {nombre3}")
        sh(ctx, f"ssh -o BatchMode=yes {remoto_q} 'cd /tmp/zpprueba/{base} 2>/dev/null && "
                f"sh instalar-linux.sh --prefijo \"$HOME/zpprueba\" --desinstalar --sin-daemon "
                f">/dev/null 2>&1; "
                f"rm -rf /tmp/zpprueba /tmp/{base}.zip \"$HOME/zpprueba\"'")


def casos_identidad(ctx):
    """La identidad de una máquina no es su nombre.

    `local` significa una máquina distinta según quién pregunte —fc16 se llama «local» a sí
    misma y mmela también—, y la misma máquina aparecía como «unib» en una lista y
    «unibody» en otra. El daemon decidía «¿este destino soy yo?» comparando dos etiquetas de
    cliente, y de eso depende que un envío salga por la red o se quede en casa.
    """
    # 1. `refresh` tiene que GUARDAR el identificador, no solo enseñarlo.
    rc, _ = cli(ctx, ["refresh zfsm://local"])
    nombre = "refresh: guarda el identificador de máquina en la conexión"
    try:
        with open(os.path.expanduser("~/.config/ZFSMgr/config.json")) as f:
            conns = json.load(f)["connections"]
        uid = next((c.get("machine_uid") or "" for c in conns if c.get("id") == "local"), "")
    except Exception as exc:          # noqa: BLE001
        uid = ""
        print(f"         (no se pudo leer la configuración: {exc})")
    # Y el que de verdad tiene la máquina, preguntado aparte.
    _, crudo = sh(ctx, "/usr/local/libexec/zfsmgr-agent --dump-refresh-basics", root=True)
    real = ""
    for l in crudo.splitlines():
        if l.startswith("MACHINE_UUID="):
            real = l.split("=", 1)[1].strip()
    problemas = []
    if not uid:
        problemas.append("la conexión Local sigue sin identificador guardado")
    if real and uid and uid.lower() not in real.lower() and real.lower() not in uid.lower():
        problemas.append(f"el guardado ({uid}) no es el de la máquina ({real})")
    if problemas:
        ctx.fallos.append((nombre, problemas, crudo[:200]))
        print(f"  FALLO  {nombre}")
        for x in problemas:
            print(f"         {x}")
    else:
        ctx.pasados.append(nombre)
        print(f"  ok     {nombre}")

    # 2. `peers` tiene que ENSEÑARLO, el propio y el de cada par.
    rc2, vista = cli(ctx, ["peers --on zfsm://local"], idioma="es")
    nombre2 = "peers: enseña la identidad de la máquina y la de cada par"
    problemas2 = []
    if "identidad de la máquina" not in vista:
        problemas2.append("no dice cuál es la identidad de la máquina")
    if uid and uid not in vista:
        problemas2.append(f"no sale el identificador real ({uid})")
    if "IDENTIDAD" not in vista:
        problemas2.append("la tabla de pares no trae la columna de identidad")
    if problemas2:
        ctx.fallos.append((nombre2, problemas2, vista[:400]))
        print(f"  FALLO  {nombre2}")
        for x in problemas2:
            print(f"         {x}")
    else:
        ctx.pasados.append(nombre2)
        print(f"  ok     {nombre2}")


def casos_devices(ctx):
    """`devices`: un dispositivo real por línea y sus alias colgando.

    El agente manda los `/dev/disk/by-id/...` como entradas aparte con `resolved` apuntando
    al dispositivo de verdad, y el cliente tiraba ese campo: en fc16 salían 36 filas de las
    que 27 eran el MISMO disco con otro nombre, a 0B y sin tipo ni sistema de ficheros.
    """
    # Lo que de verdad hay, preguntado a la máquina y no al CLI.
    _, lsb = sh(ctx, "lsblk -nro PATH,TYPE")
    reales = sorted({l.split()[0] for l in lsb.splitlines()
                     if len(l.split()) == 2 and l.split()[1] != "loop"})
    if not reales:
        print("  (saltado: lsblk no dice nada)")
        return

    rc, salida = cli(ctx, ["devices --on zfsm://local"], idioma="es")
    lineas = [l for l in salida.splitlines() if l.strip()]
    # Las filas de dispositivo son las que NO van sangradas; la cabecera, la primera.
    filas = [l for l in lineas[1:] if not l.startswith(" ")]
    alias = [l.strip() for l in lineas if l.startswith("    /dev/")]

    nombre = "devices: una línea por dispositivo real, sin repetir por los alias"
    problemas = []
    rutas = sorted({l.split()[0] for l in filas})
    faltan = [d for d in reales if d not in rutas]
    if faltan:
        problemas.append(f"faltan dispositivos que lsblk sí ve: {faltan[:4]}")
    # **Lo que no puede pasar al agrupar: perder información.** Un `/dev/disk/by-id/...` no
    # puede estar como fila de primer nivel, pero tampoco puede haber desaparecido.
    porId = [r for r in rutas if r.startswith("/dev/disk/by-")]
    if porId:
        problemas.append(f"un alias sale como si fuera un dispositivo: {porId[:3]}")
    if not alias:
        problemas.append("no sale ni un alias: se han perdido por el camino")
    if problemas:
        ctx.fallos.append((nombre, problemas, salida[:500]))
        print(f"  FALLO  {nombre}")
        for x in problemas:
            print(f"         {x}")
    else:
        ctx.pasados.append(nombre)
        print(f"  ok     {nombre}")

    # Y el recuento, que es la prueba de que NADA se perdió: dispositivos + alias enseñados
    # tiene que ser lo mismo que entradas mandaba el agente.
    # `format` es el verbo del intérprete: `--format` solo vale al arrancar el binario, y la
    # batería arranca siempre en texto.
    rc2, js = cli(ctx, ["format json", "devices --on zfsm://local"])
    nombre2 = "devices: dispositivos + alias suman lo que mandó el agente"
    try:
        datos = json.loads(js)["devices"]
        suma = len(datos) + sum(len(d.get("aliases") or []) for d in datos)
    except Exception as exc:          # noqa: BLE001 - se informa, no se traga
        datos, suma = [], -1
        print(f"         (json ilegible: {exc})")
    _, crudo = sh(ctx, "/usr/local/libexec/zfsmgr-agent --dump-block-devices", root=True)
    try:
        esperado = len(json.loads(crudo)["devices"])
    except Exception:                 # noqa: BLE001
        esperado = -1
    if esperado < 0:
        print(f"  (saltado: {nombre2} — el agente no contestó en crudo)")
    elif suma != esperado:
        ctx.fallos.append((nombre2, [f"el CLI enseña {suma} y el agente mandó {esperado}"], ""))
        print(f"  FALLO  {nombre2}")
        print(f"         el CLI enseña {suma} y el agente mandó {esperado}")
    else:
        ctx.pasados.append(nombre2)
        print(f"  ok     {nombre2}  ({len(datos)} dispositivos + {suma - len(datos)} alias)")

    # En JSON los alias son un ARRAY, que es lo que un guion puede recorrer.
    nombre3 = "devices: en json los alias son un array, no una cadena con comas"
    if datos and all(isinstance(d.get("aliases"), list) for d in datos):
        ctx.pasados.append(nombre3); print(f"  ok     {nombre3}")
    else:
        ctx.fallos.append((nombre3, ["«aliases» no es una lista"], js[:300]))
        print(f"  FALLO  {nombre3}")


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
    # ANTES que nada: que el binario que se va a medir lea los catálogos de AHORA.
    #
    # El CLI busca `i18n/` al lado del ejecutable, y ahí llega por un objetivo de CMake
    # aparte —`zfsmgr_cli_i18n`—. Compilar solo `--target zfsmgr-cli` no lo dispara, así que
    # el programa sigue leyendo el catálogo anterior: una traducción recién escrita no
    # aparece, y todo caso que mire texto estaría midiendo otra cosa. Esto no lo puede ver
    # `revisa_i18n_fallback.py`, que lee el árbol de fuentes y no el de compilación.
    nombre = "catálogos: los que hay junto al binario son los del árbol de fuentes"
    problemas = []
    for idi in ("es", "en"):
        fuente = os.path.join(RAIZ, "i18n", f"{idi}.json")
        copia = os.path.join(os.path.dirname(CLI), "i18n", f"{idi}.json")
        if not os.path.exists(copia):
            problemas.append(f"no hay catálogo {idi} junto al binario: {copia}")
        elif os.path.getmtime(copia) < os.path.getmtime(fuente):
            problemas.append(f"{idi}.json junto al binario es MÁS VIEJO que el de i18n/: "
                             "falta «cmake --build <dir> --target zfsmgr_cli_i18n»")
    if problemas:
        ctx.fallos.append((nombre, problemas, ""))
        print(f"  FALLO  {nombre}")
        for x in problemas:
            print(f"         {x}")
    else:
        ctx.pasados.append(nombre)
        print(f"  ok     {nombre}")
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

    # --- `install-daemon-local`: lo que usan los instaladores del paquete.
    #
    # Tiene que ejecutar EL MISMO guion que `install-daemon` manda por SSH, no una copia. En
    # simulacro lo imprime, así que se mira que sea el de systemd con la huella del agente
    # que va al lado: si alguien escribiera aquí una segunda versión de la lógica de
    # servicios, divergiría de la remota sin que nada lo dijera.
    caso_argv(ctx, "install-daemon-local --simular: es el guion de install-daemon",
              ["--lang", "es", "install-daemon-local", "--simular"], rc=0,
              espera=[r"systemctl (daemon-reload|enable)", r"VERSION='\d+\.\d+\.\d+\.\d+'",
                      r"/usr/local/libexec/zfsmgr-agent"])
    caso_argv(ctx, "uninstall-daemon-local --simular: quita lo que el otro pone",
              ["--lang", "es", "uninstall-daemon-local", "--simular"], rc=0,
              espera=[r"systemctl disable --now zfsmgr-agent\.service",
                      r"/usr/local/libexec/zfsmgr-agent", r"/etc/zfsmgr/tls"])
    # Sin `--simular` y sin ser root, se niega ANTES de tocar nada.
    if os.geteuid() != 0:
        caso_argv(ctx, "install-daemon-local sin root se niega",
                  ["--lang", "es", "install-daemon-local"], rc=1, espera=[r"como root"])
    # `--comprobar` es suya: el instalador la usa ANTES de tocar nada para no reinstalar un
    # daemon con trabajos en curso. Sin root se niega como las demás, sin dar el «uso».
    if os.geteuid() != 0:
        caso_argv(ctx, "install-daemon-local --comprobar se reconoce (y pide root)",
                  ["--lang", "es", "install-daemon-local", "--comprobar"], rc=1,
                  espera=[r"como root"], no_espera=[r"uso:"])
    # Sus opciones son SUYAS: antes, cualquier «--algo» detrás de la orden se tomaba por
    # opción global y se rechazaba sin llegar a preguntarle.
    caso_argv(ctx, "install-daemon-local: una opción que no es suya da el uso",
              ["--lang", "es", "install-daemon-local", "--inventada"], rc=2,
              espera=[r"uso: zfsmgr-cli install-daemon-local"])

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
LOCALES = [casos_arranque, casos_identidad, casos_devices, casos_ayuda, casos_version_openzfs, casos_nombres_que_chocan, casos_navegacion, casos_lectura, casos_crear_destruir,
           casos_volumenes, casos_dedup, casos_patrones, casos_patrones_copia, casos_terminos_openzfs, casos_mensajes_capa_base, casos_clave_ssh, casos_diff_relativo, casos_send_level, casos_recepcion_a_medias, casos_send_fichero, casos_send_comprimido, casos_gsa, casos_sin_montar, casos_cp_destino, casos_load_key, casos_editor_ancho, casos_tab_continua, casos_pantalla, casos_path, casos_info_labelclear, casos_servicio, casos_servicio_conexiones, casos_propiedades, casos_montaje, casos_instantaneas, casos_pool,
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
        # Va siempre, también con --sin-limpieza: dentro está la maestra en claro, y
        # quedaba un directorio por ejecución en /tmp.
        shutil.rmtree(ctx.dir_tmp, ignore_errors=True)

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
