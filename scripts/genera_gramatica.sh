#!/usr/bin/env bash
# Regenera el analizador del intérprete a partir de la gramática.
#
# Lo generado va EN EL REPOSITORIO a propósito: el agente y la interfaz se cruzan dentro de
# un contenedor y en integración continua, y exigir bison/flex allí sería añadir una
# dependencia de construcción a cambio de nada. Quien toca la gramática ejecuta esto y
# comprueba los ficheros resultantes.
#
# `--expect 0` está en la propia gramática: si alguien introduce una ambigüedad, bison falla
# aquí y no se llega a generar nada.
set -euo pipefail
cd "$(dirname "$0")/../src/cli"
for t in bison flex; do
  command -v "$t" >/dev/null || { echo "hace falta $t" >&2; exit 1; }
done
bison -Wcounterexamples -d -o generado/gramatica.tab.c gramatica.y
flex -o generado/gramatica.lex.c gramatica.l
echo "regenerado: src/cli/generado/"
