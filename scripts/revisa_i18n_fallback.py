#!/usr/bin/env python3
"""Busca los textos que saldrían EN CASTELLANO con la interfaz en inglés.

`trk(clave, es, en)` admite dejarse el inglés fuera, y entonces lo busca en
`i18n/en.json`. Si la clave tampoco está allí, `translateKey()` devuelve el
castellano: el rótulo sale en español sin que nada falle ni avise.

Así apareció «Borrar Config» en la pantalla del password maestro, en inglés. Y era
peor de lo que parecía: esa pantalla sale ANTES de que la aplicación sepa si los
catálogos JSON están, y cuando no están se fuerza el castellano — o sea que
justo ahí es donde menos se puede depender del catálogo.

Este guion no mira el catálogo como fuente de verdad; mira las LLAMADAS:

    trk("t_x", "Aceptar")                       -> depende del catálogo
    trk("t_x", "Aceptar", "Accept")             -> vale siempre

y solo se queja de las primeras cuando además la clave no está en `en.json`.

Sale 1 si encuentra alguna, con fichero, línea y el texto castellano.
"""

import json
import pathlib
import re
import sys

RAIZ = pathlib.Path(__file__).resolve().parent.parent


def catalogo_plano(ruta):
    datos = json.loads(ruta.read_text(encoding="utf-8"))
    plano = {}
    for seccion in datos.values():
        if isinstance(seccion, dict):
            plano.update(seccion)
    return plano


def argumentos(texto, pos_parentesis):
    """Los argumentos de la llamada que abre en `pos_parentesis`, sin partir cadenas."""
    profundidad = 0
    inicio = pos_parentesis + 1
    args = []
    i = pos_parentesis
    while i < len(texto):
        c = texto[i]
        if c == '"':
            i += 1
            while i < len(texto) and not (texto[i] == '"' and texto[i - 1] != "\\"):
                i += 1
        elif c in "([":
            profundidad += 1
        elif c in ")]":
            profundidad -= 1
            if profundidad == 0:
                args.append(texto[inicio:i])
                return args
        elif c == "," and profundidad == 1:
            args.append(texto[inicio:i])
            inicio = i + 1
        i += 1
    return None


def main():
    ingles = catalogo_plano(RAIZ / "i18n" / "en.json")
    rx_llamada = re.compile(r"\btrk\s*\(")
    rx_clave = re.compile(r'QStringLiteral\("(t_[a-z0-9_]+)"\)')

    fallos = []
    for ruta in sorted((RAIZ / "src").rglob("*.cpp")) + sorted((RAIZ / "src").rglob("*.h")):
        texto = ruta.read_text(encoding="utf-8", errors="ignore")
        for m in rx_llamada.finditer(texto):
            args = argumentos(texto, m.end() - 1)
            if not args:
                continue
            args = [a.strip() for a in args]
            # Los diálogos de arranque llevan el idioma como primer argumento.
            if args and not args[0].startswith("QStringLiteral"):
                args = args[1:]
            if not args:
                continue
            mk = rx_clave.match(args[0])
            if not mk:
                continue
            clave = mk.group(1)
            trae_ingles = len(args) >= 3 and args[2].startswith("QStringLiteral")
            if trae_ingles or clave in ingles:
                continue
            linea = texto[: m.start()].count("\n") + 1
            castellano = args[1][:70] if len(args) > 1 else ""
            fallos.append((ruta.relative_to(RAIZ), linea, clave, castellano))

    for ruta, linea, clave, castellano in fallos:
        print(f"{ruta}:{linea}: {clave} saldría en castellano: {castellano}")
    if fallos:
        print(f"\n{len(fallos)} texto(s) sin inglés ni en la llamada ni en i18n/en.json")
        return 1
    print("ningún texto se queda en castellano con la interfaz en inglés")
    return 0


if __name__ == "__main__":
    sys.exit(main())
