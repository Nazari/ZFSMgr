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
        orden = f"ssh -o BatchMode=yes -o ConnectTimeout=10 {ctx.remoto} {orden!r}"
    entrada = (ctx.sudo + "\n") if root else None
    p = subprocess.run(["bash", "-c", orden], input=entrada, capture_output=True,
                       text=True, timeout=300)
    return p.returncode, (p.stdout + p.stderr)


def cli(ctx, ordenes, timeout=180):
    """Manda órdenes al intérprete y devuelve (rc, salida).

    La contraseña maestra va por el DESCRIPTOR 9, abierto con la redirección del shell.
    Montarlo con `preexec_fn` no vale: `subprocess` cierra los descriptores que no están
    en `pass_fds` y el 9 llegaba cerrado — el CLI se quedaba sin contraseña y TODAS las
    mutaciones fallaban mientras las lecturas parecían ir bien, que es el peor síntoma
    posible porque no se lee como un fallo de autenticación.
    """
    guion = "\n".join(ordenes) + "\nexit\n"
    fichero = os.path.join(ctx.dir_tmp, ".maestra")
    orden = f"exec 9<{fichero}; exec {CLI!r} --password-fd 9 --format text -y"
    p = subprocess.run(["bash", "-c", orden], input=guion, capture_output=True,
                       text=True, timeout=timeout)
    return p.returncode, (p.stdout + p.stderr)


def caso(ctx, nombre, ordenes, espera=(), no_espera=(), estado=(), rc=None):
    """Un caso: órdenes al CLI, y después comprobaciones contra la máquina."""
    rc_real, salida = cli(ctx, list(ordenes))
    problemas = []
    if rc is not None and rc_real != rc:
        problemas.append(f"rc={rc_real}, esperaba {rc}")
    for patron in espera:
        if not re.search(patron, salida, re.M):
            problemas.append(f"falta en la salida: /{patron}/")
    for patron in no_espera:
        if re.search(patron, salida, re.M):
            problemas.append(f"NO debería salir: /{patron}/")
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
    sh(ctx, f"bash -c 'echo hola > /mnt/{pool}/datos/fichero.txt'", remoto, root=True)
    sh(ctx, f"bash -c 'mkdir -p /mnt/{pool}/datos/sub && echo anidado > /mnt/{pool}/datos/sub/otro.txt'",
       remoto, root=True)
    sh(ctx, f"zfs snapshot {pool}/datos@s1", remoto, root=True)
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
         ["cd Local", "info"], espera=[r"STATUS=OK", r"ZFS_VERSION_RAW="])


def casos_crear_destruir(ctx):
    P = POOL_LOCAL
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
    caso(ctx, "set: escribe una propiedad y ZFS la tiene",
         [f"cd Local/{P}/datos", "set compression=zstd"],
         estado=[("compression=zstd", f"zfs get -H -o value compression {P}/datos", r"^zstd$")])
    caso(ctx, "set: varias propiedades de una vez",
         [f"cd Local/{P}/datos", "set atime=off readonly=off"],
         estado=[("atime=off", f"zfs get -H -o value atime {P}/datos", r"^off$"),
                 ("readonly=off", f"zfs get -H -o value readonly {P}/datos", r"^off$")])


def casos_montaje(ctx):
    P = POOL_LOCAL
    caso(ctx, "unmount: el dataset queda desmontado",
         [f"cd Local/{P}/datos", "unmount"],
         estado=[("mounted=no", f"zfs get -H -o value mounted {P}/datos", r"^no$")])
    caso(ctx, "mount: y vuelve a montarse",
         [f"cd Local/{P}/datos", "mount"],
         estado=[("mounted=yes", f"zfs get -H -o value mounted {P}/datos", r"^yes$")])


def casos_instantaneas(ctx):
    P = POOL_LOCAL
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


LOCALES = [casos_navegacion, casos_lectura, casos_crear_destruir,
           casos_propiedades, casos_montaje, casos_instantaneas, casos_pool]


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
    try:
        for grupo in LOCALES:
            print(f"\n{grupo.__name__}:")
            grupo(ctx)
    finally:
        if not args.sin_limpieza:
            limpia(ctx)

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
