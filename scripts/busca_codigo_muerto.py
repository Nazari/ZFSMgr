#!/usr/bin/env python3
"""Busca lo que ya no llama nadie: miembros, campos y declaraciones sin definición.

Por qué hace falta un guion y no basta con el compilador: el árbol se compila SIN
`-Wall`, y aunque se compilara, `-Wunused-function` solo ve lo de espacio anónimo
DENTRO de una unidad de traducción. Un método de `MainWindow` declarado en la
cabecera y definido en uno de sus veinte ficheros no se lo salta ningún aviso: para
el compilador puede llamarlo cualquiera.

Lo que sí ve el compilador —funciones estáticas, constantes y variables locales— se
saca aparte, con clang y en un directorio de construcción de usar y tirar:

    cmake -S resources -B /tmp/aviso -DCMAKE_CXX_COMPILER=clang++ \
          -DCMAKE_CXX_FLAGS="-Wunused-function -Wunused-variable -Wunused-private-field"
    cmake --build /tmp/aviso -j"$(nproc)" 2>&1 | grep -E "warning:.*unused"

Y hay que REPETIR las dos cosas hasta que no salga nada: borrar una función deja
huérfana a la que solo ella llamaba. En la pasada de agosto de 2026 hicieron falta
cuatro vueltas.

Criterio: un identificador cuyo nombre aparece dos veces en todo el árbol está
declarado y definido, y no lo usa nadie. Uno que aparece una vez está declarado y ni
siquiera existe —eso no falla al compilar, falla al enlazar, y solo si alguien lo usa—.

Falsos positivos que hay que mirar a mano antes de borrar:
  - Nombres de parámetro en una declaración con valor por defecto.
  - Ranuras invocadas por nombre (`SLOT(...)`, `invokeMethod("nombre")`). Hoy no hay.
  - Sobrecargas: el recuento va por NOMBRE, así que si hay dos y una se usa, no sale
    ninguna; y al revés, al borrar hay que asegurarse de cuál sobra.
  - Los ficheros `.c` generados por bison/flex, que llaman a lo suyo desde código
    que este guion sí lee, pero conviene no tocar.

Sale 1 si encuentra algo.
"""

import collections
import pathlib
import re
import sys

RAIZ = pathlib.Path(__file__).resolve().parent.parent
EXTENSIONES = ('*.cpp', '*.h', '*.c')


def fuentes():
    for base in ('src', 'tests'):
        for ext in EXTENSIONES:
            yield from (RAIZ / base).rglob(ext)


def main():
    textos = {f: f.read_text(encoding='utf-8', errors='ignore') for f in sorted(fuentes())}
    conteo = collections.Counter()
    for t in textos.values():
        for tok in re.findall(r'\b[A-Za-z_]\w*\b', t):
            conteo[tok] += 1

    hallazgos = []

    # 1. Definiciones fuera de línea: `Tipo Clase::nombre(` o `Tipo espacio::nombre(`
    rx_def = re.compile(r'^[A-Za-z_][\w:<>,\s&*\[\]]*?\b([A-Za-z_]\w*)::([A-Za-z_]\w*)\s*\(', re.M)
    for f, t in textos.items():
        if f.suffix != '.cpp':
            continue
        for m in rx_def.finditer(t):
            cls, nombre = m.group(1), m.group(2)
            if cls == nombre or nombre == 'operator':
                continue
            if conteo[nombre] <= 2:
                hallazgos.append((f, t[:m.start()].count('\n') + 1,
                                  f"{cls}::{nombre}() no lo llama nadie"))

    # 2. Variables miembro y campos declarados en cabeceras
    rx_miembro = re.compile(r'^\s{4,}[\w:<>,\s*&\[\]]*?\b(m_[A-Za-z_]\w*)\s*(?:\{|=|;|\[)', re.M)
    rx_campo = re.compile(
        r'^\s{4,}(?!return\b)[A-Za-z_][\w:<>,\s*&]*?\b([a-z][A-Za-z0-9_]*)\s*(?:\{[^}]*\}|=\s*[^;]+)?;\s*(?://.*)?$',
        re.M)
    vistos = set()
    for f, t in textos.items():
        if f.suffix != '.h':
            continue
        for rx, que, avisa_de_uno in ((rx_miembro, 'variable miembro', True),
                                      (rx_campo, 'campo', False)):
            for m in rx.finditer(t):
                n = m.group(1)
                # Un nombre de PARÁMETRO dentro de una declaración partida en varias
                # líneas se lee igual que un campo. Se descarta por ahí: un campo cabe en
                # su línea y no lleva paréntesis.
                if '(' in m.group(0) or '\n' in m.group(0):
                    continue
                if n in vistos:
                    continue
                vistos.add(n)
                if conteo[n] <= 1:
                    hallazgos.append((f, t[:m.start()].count('\n') + 1,
                                      f"{que} `{n}` declarada y jamás nombrada"))
                elif conteo[n] == 2 and avisa_de_uno:
                    # Un solo uso en una variable miembro casi siempre es solo escritura
                    # (nadie la lee) o solo lectura (una condición constante). En un campo
                    # de struct es normal, así que ahí no se avisa.
                    hallazgos.append((f, t[:m.start()].count('\n') + 1,
                                      f"{que} `{n}` con un solo uso: mírala, suele ser "
                                      f"solo escritura o solo lectura"))

    # 3. Declaraciones sin definición en ninguna parte.
    #
    # El TIPO de retorno es obligatorio en el patrón: sin él, una LLAMADA suelta
    # —`imprimeJson();` dentro de un cuerpo— se lee como una declaración y sale como
    # muerta lo que está vivo justo al lado.
    todo = "".join(textos.values())
    rx_decl = re.compile(
        r'^[ \t]{4}(?:static\s+|virtual\s+|explicit\s+|inline\s+)*'
        r'[A-Za-z_][\w:<>,*&]*[\w>*&][\w:<>,\s*&]*?\s+'
        r'([A-Za-z_]\w*)\s*\([^;{]*\)\s*(?:const\s*)?(?:override\s*)?;[ \t]*$', re.M)
    rx_seccion = re.compile(r'^\s*(Q_SIGNALS|signals|public|protected|private)\s*(?:slots)?\s*:', re.M)
    for f, t in textos.items():
        if f.suffix != '.h':
            continue
        # Las señales las define moc, no el código: declararlas y no definirlas es lo normal.
        secciones = [(m.start(), m.group(1)) for m in rx_seccion.finditer(t)]
        for m in rx_decl.finditer(t):
            n = m.group(1)
            if n[0].isupper() or n in ('if', 'for', 'while', 'return', 'switch'):
                continue
            previa = [s for s in secciones if s[0] < m.start()]
            if previa and previa[-1][1] in ('Q_SIGNALS', 'signals'):
                continue
            if re.search(r'::' + n + r'\s*\(', todo):
                continue
            # Definida en línea en alguna cabecera: `Tipo nombre(...) const { ... }`
            if re.search(r'\b' + n + r'\s*\([^;()]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{', todo):
                continue
            hallazgos.append((f, t[:m.start()].count('\n') + 1,
                              f"`{n}()` declarada y sin definición: falla al ENLAZAR, "
                              f"no al compilar"))

    for f, ln, que in sorted(hallazgos, key=lambda x: (str(x[0]), x[1])):
        print(f"{f.relative_to(RAIZ)}:{ln}: {que}")
    if hallazgos:
        print(f"\n{len(hallazgos)} hallazgo(s). Repite tras borrar: unos tapan a otros.")
        return 1
    print("nada muerto por el recuento de identificadores")
    return 0


if __name__ == '__main__':
    sys.exit(main())
