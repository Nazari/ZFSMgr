#!/usr/bin/env python3
# Texts in Spanish that do not go through the translation catalogs.
#
#   scripts/revisa_castellano.py            all of src/ (except the GUI, frozen until 2.0)
#
# Code is written in English (CONTRIBUTING.md) and what a user reads goes through
# T(…)/TC(…)/i18n::tr(…) with a key, so it comes out in the user's language. A Spanish
# literal outside that is either a text that will always come out in Spanish —the reported
# case: `clear` asked for confirmation half in Spanish, half in English— or one of the few
# that must stay as they are, listed in PERMITIDOS with the reason.
# Exit code 1 if anything is found.
import re, sys, glob

# Literals that are Spanish ON PURPOSE.
PERMITIDOS = {
    # What sudo prints in Spanish when the password is wrong or missing: recognised, not shown.
    'no está en el fichero sudoers', 'intento de contraseña incorrecto',
    'intentos de contraseña incorrectos', 'se requiere una contraseña',
    'se necesita una contraseña', 'fallo de autenticación',
    # OpenZFS's «modules are not loaded» in a Spanish locale: recognised, not shown.
    'módulos',
    # What the user may type as «yes».
    'sí',
    # The name of a slot of the command grammar (gramatica.y), not a text.
    'destino',
}

ES = re.compile(r'[áéíóúñ¿¡ÁÉÍÓÚÑ]|\b(se|la|el|los|las|una?|que|para|con|sin|del|hecho|cancelado|'
                r'ninguna?|está|esta|puede|hay|ya|pero|como|todo|nada|origen|destino|conexión|'
                r'máquina|fichero|directorio|orden|uso|aviso|error al|falta|sobra)\b', re.I)

def sin_comentarios(t):
    """Blanks out comments, keeping offsets and line breaks."""
    out = list(t)
    i, n = 0, len(t)
    while i < n:
        if t.startswith('//', i):
            j = t.find('\n', i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = ' '
            i = j
        elif t.startswith('/*', i):
            j = t.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != '\n':
                    out[k] = ' '
            i = j
        elif t[i] == '"':
            j = i + 1
            while j < n and t[j] != '"':
                j += 2 if t[j] == '\\' else 1
            i = j + 1
        elif t[i] == "'":
            j = i + 1
            while j < n and t[j] != "'":
                j += 2 if t[j] == '\\' else 1
            i = j + 1
        else:
            i += 1
    return ''.join(out)

LIT = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
total = 0
archivos = sys.argv[1:] or sorted(
    f for d in ('src/base', 'src/commands', 'src/cli', 'src/daemon')
    for f in glob.glob(d + '/*.cpp') + glob.glob(d + '/*.h')
    # the agent's catalog: each code with its Spanish AND its English, side by side
    if not f.endswith('agentmsg.cpp'))
for f in archivos:
    t = sin_comentarios(open(f).read())
    # adjacent literals («"a" "b"») count as one: what matters is where the run starts
    for m in LIT.finditer(t):
        s = m.group(1)
        if not ES.search(s) or s.startswith('t_'):
            continue
        # Is it the text of a catalog entry? Look backwards past spaces and the comma.
        antes = t[:m.start()].rstrip()
        # a literal split over several lines: step back over the earlier pieces
        while antes.endswith('"'):
            k = antes.rfind('"', 0, len(antes) - 1)
            antes = t[:k].rstrip() if k >= 0 else ''
        # T("t_…", "…"), i18n::tr(…), {"t_…", "…"}: the text comes right after its key
        if re.search(r'"t_[A-Za-z0-9_]+"\s*,\s*$', antes):
            continue
        if s in PERMITIDOS:
            continue
        linea = t.count('\n', 0, m.start()) + 1
        print(f'{f}:{linea}: "{s[:90]}"')
        total += 1
print(f'revisa_castellano: {total} Spanish literal(s) outside the catalogs', file=sys.stderr)
sys.exit(1 if total else 0)
