#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Contrasta las órdenes del intérprete con sus FIRMAS declaradas.

Existe porque la familia de fallos «la orden acepta un argumento y no le hace caso» no se
ve mirando el código de una orden: solo se ve comparando las 50 entre sí. Este guion hace
esa comparación y dice, para cada una, si ya pasa por el preámbulo único o sigue
resolviendo su destino por su cuenta.

Ver docs/gramatica_cli.md.

    python3 scripts/revisa_firmas_cli.py          # el resumen
    python3 scripts/revisa_firmas_cli.py --mudas   # solo las que pueden ignorar argumentos
    python3 scripts/revisa_firmas_cli.py --sordas  # solo las que piden un valor sin declararlo
    python3 scripts/revisa_firmas_cli.py --opacas  # las que este guion no puede comprobar
"""
import re
import sys
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
SHELL = RAIZ / "src/cli/shell.cpp"
AYUDA = RAIZ / "src/cli/ayuda.cpp"


def cuerpos_de_funciones(texto, patron=r"^bool (cmd[A-Za-z]+)\(Estado&"):
    """El código de cada función que case, equilibrando llaves."""
    out = {}
    for m in re.finditer(patron, texto, re.M):
        i = texto.index("{", m.start())
        # Una DECLARACIÓN no es una definición: `bool cmdX(...);` no tiene cuerpo, y
        # buscarle la siguiente llave da el cuerpo de OTRA función. Con el prototipo detrás
        # de la definición —que es lo que pasa aquí— ese cuerpo falso ganaba, y el guion
        # analizaba código que no era el de la orden. Así se le escapó `edit`.
        punto = texto.find(";", m.start())
        if punto != -1 and punto < i:
            continue
        prof, j = 0, i
        while j < len(texto):
            if texto[j] == "{":
                prof += 1
            elif texto[j] == "}":
                prof -= 1
                if prof == 0:
                    break
            j += 1
        out[m.group(1)] = texto[m.start():j]
    return out


def main():
    shell = SHELL.read_text(encoding="utf-8")
    ayuda = AYUDA.read_text(encoding="utf-8")

    mapa = dict(re.findall(r'\{"([a-z-]+)",\s*(cmd[A-Za-z]+)\}', shell))
    for verbo, fn in re.findall(r'\{"([a-z-]+)",\s*\[\]\(Estado& s[^}]*?return (cmd[A-Za-z]+)\(',
                                shell):
        mapa.setdefault(verbo, fn)

    cuerpos = cuerpos_de_funciones(shell)

    # Qué verbos declaran ya su objetivo en el catálogo.
    con_firma = set()
    for m in re.finditer(r'\{"([a-z-]+)",', ayuda):
        verbo = m.group(1)
        prof, j = 0, m.start()
        while j < len(ayuda):
            if ayuda[j] == "{":
                prof += 1
            elif ayuda[j] == "}":
                prof -= 1
                if prof == 0:
                    break
            j += 1
        if "Objetivo::" in ayuda[m.start():j]:
            con_firma.add(verbo)

    migradas, mudas, propias, sin_destino = [], [], [], []
    for verbo, fn in sorted(mapa.items()):
        c = cuerpos.get(fn, "")
        if not c:
            continue
        if "prepara(e," in c:
            migradas.append(verbo)
        elif "destinoDe" in c and "libres" not in c and "destinoSuelto" not in c \
                and "adoptaPoolSuelto" not in c and "destinoDePool" not in c:
            mudas.append(verbo)          # resuelve destino y NUNCA mira los sueltos
        elif any(x in c for x in ("destinoDe", "libres", "destinoSuelto",
                                  "adoptaPoolSuelto", "destinoDePool")):
            propias.append(verbo)
        else:
            sin_destino.append(verbo)

    # --- Las opciones que se LEEN con valor y no lo declaran.
    #
    # El léxico decide si una opción se lleva por delante el componente siguiente mirando su
    # forma en el catálogo: si ahí no hay un «<...>», la opción es una bandera suelta. Una
    # orden que luego pida `pet.valor("x")` no recibe nada, y el valor que el usuario
    # escribió se cuela como un argumento más.
    #
    # No es teórico: `--mountpoint` estaba declarado sin valor, así que
    # `create pool disco --mountpoint /mnt/x` mandaba «/mnt/x» a la lista de DISPOSITIVOS
    # del pool. Se descubrió de casualidad; esto lo convierte en algo que se comprueba.
    formas = {}
    for m in re.finditer(r'\{"([a-z-]+)",', ayuda):
        verbo = m.group(1)
        prof, j = 0, m.start()
        while j < len(ayuda):
            if ayuda[j] == "{":
                prof += 1
            elif ayuda[j] == "}":
                prof -= 1
                if prof == 0:
                    break
            j += 1
        # Cualquier cadena de la entrada que MENCIONE una opción larga, no solo las que
        # empiezan por ella: la forma «-o p=v / -O p=v / --mountpoint <ruta>» empieza por
        # una corta, y pidiendo que empezara por «--» se quedaba fuera justo esa.
        formas[verbo] = [s for s in re.findall(r'"([^"]*)"', ayuda[m.start():j]) if "--" in s]

    # El cuerpo de la orden NO basta: `create` lee `--mountpoint` dentro de `cmdCrearPool`,
    # a la que llama. Sin seguir las llamadas, el comprobador daba por bueno justo el caso
    # que lo motivó —se verificó reintroduciendo el fallo y viendo que no lo cantaba—.
    todas = cuerpos_de_funciones(shell, r"^(?:bool|std::string|void) ([a-zA-Z_][A-Za-z0-9_]*)\(")

    def alcanzable(fn, vistas=None):
        """El código de `fn` y el de todo lo que llama, hasta donde llegue."""
        vistas = vistas if vistas is not None else set()
        if fn in vistas or fn not in todas:
            return ""
        vistas.add(fn)
        c = todas[fn]
        for llamada in set(re.findall(r"\b([a-zA-Z_][A-Za-z0-9_]*)\s*\(", c)):
            if llamada in todas:
                c += alcanzable(llamada, vistas)
        return c

    # Las opciones que se piden con una CLAVE que no es literal —`pet.valor(clave)` dentro
    # de un ayudante—, que este guion no puede seguir. Se listan en vez de callarlas: es el
    # punto ciego por el que `edit --name X` pasó desapercibido, y era una orden entera
    # inservible fuera del modo interactivo.
    opacas = []
    for verbo, fn in sorted(mapa.items()):
        if re.search(r'\bvalor\((?!")', alcanzable(fn)):
            opacas.append(verbo)

    sordas = []
    for verbo, fn in sorted(mapa.items()):
        c = alcanzable(fn)
        for opcion in sorted(set(re.findall(r'\bvalor\("([a-z][a-z0-9-]*)"\)', c))):
            if opcion in ("on", "from"):      # universales: el léxico ya sabe que llevan valor
                continue
            declarada = [f for f in formas.get(verbo, []) if ("--" + opcion) in f]
            if not declarada:
                continue                       # no declarada: eso lo canta el propio intérprete
            # El tramo que sigue al nombre, hasta la siguiente opción, igual que el léxico.
            for f in declarada:
                i = f.find("--" + opcion) + len(opcion) + 2
                tramo = f[i:].split(" / ")[0]
                if "<" not in tramo:
                    sordas.append(f"{verbo} --{opcion}   (declarada como «{f}»)")

    if "--opacas" in sys.argv:
        for o in opacas:
            print(o)
        return 0

    if "--sordas" in sys.argv:
        for s in sordas:
            print(s)
        return 1 if sordas else 0

    if "--mudas" in sys.argv:
        for v in mudas:
            print(v)
        return 1 if mudas else 0

    total = len(migradas) + len(mudas) + len(propias) + len(sin_destino)
    print(f"órdenes del intérprete: {total}")
    print(f"  con firma declarada en el catálogo: {len(con_firma & set(mapa))}")
    print()
    print(f"  YA por el preámbulo único ({len(migradas)}):")
    print("     ", ", ".join(migradas) or "-")
    print()
    print(f"  PUEDEN IGNORAR ARGUMENTOS EN SILENCIO ({len(mudas)}):")
    print("     ", ", ".join(mudas) or "ninguna")
    print()
    print(f"  resuelven su destino a mano, pendientes de migrar ({len(propias)}):")
    print("     ", ", ".join(propias) or "-")
    if sin_destino:
        print()
        print(f"  sin destino ({len(sin_destino)}): {', '.join(sin_destino)}")

    print()
    print(f"  PIDEN EL VALOR CON UNA CLAVE NO LITERAL, no comprobables aquí ({len(opacas)}):")
    print("     ", ", ".join(opacas) or "ninguna")
    print()
    print(f"  PIDEN UN VALOR QUE NO DECLARAN ({len(sordas)}):")
    for s in sordas:
        print("     ", s)
    if not sordas:
        print("      ninguna")

    # Sale con error si queda alguna: así vale para integración continua.
    return 1 if (mudas or sordas) else 0


if __name__ == "__main__":
    sys.exit(main())
