#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Contrasta las órdenes del intérprete con sus FIRMAS declaradas.

Existe porque la familia de fallos «la orden acepta un argumento y no le hace caso» no se
ve mirando el código de una orden: solo se ve comparando las 50 entre sí. Este guion hace
esa comparación y dice, para cada una, si ya pasa por el preámbulo único o sigue
resolviendo su destino por su cuenta.

Ver docs/gramatica_cli.md.

    python3 scripts/revisa_firmas_cli.py          # el resumen
    python3 scripts/revisa_firmas_cli.py --mudas  # solo las que pueden ignorar argumentos
"""
import re
import sys
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
SHELL = RAIZ / "src/cli/shell.cpp"
AYUDA = RAIZ / "src/cli/ayuda.cpp"


def cuerpos_de_funciones(texto):
    """El código de cada `bool cmdX(...)`, equilibrando llaves."""
    out = {}
    for m in re.finditer(r"^bool (cmd[A-Za-z]+)\(Estado&", texto, re.M):
        i = texto.index("{", m.start())
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

    # Sale con error si queda alguna muda: así vale para integración continua.
    return 1 if mudas else 0


if __name__ == "__main__":
    sys.exit(main())
