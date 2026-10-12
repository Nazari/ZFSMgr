#!/usr/bin/env bash
# Formato de C++ SOLO en las líneas que ha tocado un cambio.
#
#   scripts/formato.sh [--check] [<desde>]
#
# Sin --check, reformatea en el árbol de trabajo las líneas cambiadas desde <desde> (por
# omisión, HEAD: lo que aún no tiene commit). Con --check no toca nada y falla si algo no
# cumple, que es lo que hace el CI con cada push.
#
# Necesita clang-format 19 (con su git-clang-format). Si el sistema no lo trae:
#   python3 -m venv ~/.local/clang-format && ~/.local/clang-format/bin/pip install clang-format==19.1.7
# y CLANG_FORMAT=~/.local/clang-format/bin/clang-format.
set -euo pipefail
cd "$(dirname "$0")/.."

check=0
if [[ "${1:-}" == "--check" ]]; then
  check=1
  shift
fi
desde="${1:-HEAD}"

cf="${CLANG_FORMAT:-$(command -v clang-format || true)}"
[[ -n "$cf" && -x "$cf" ]] || { echo "formato: no encuentro clang-format (CLANG_FORMAT=…)" >&2; exit 2; }
gcf="$(dirname "$cf")/git-clang-format"
[[ -x "$gcf" ]] || gcf="$(command -v git-clang-format || true)"
[[ -n "$gcf" ]] || { echo "formato: no encuentro git-clang-format" >&2; exit 2; }

exts=cpp,h,c
if [[ $check -eq 1 ]]; then
  # `--diff` enseña lo que cambiaría; sale vacío o con «no modified files» si todo cumple.
  salida="$("$gcf" --binary "$cf" --extensions "$exts" --diff "$desde" 2>&1 || true)"
  if [[ -z "$salida" ]] || grep -qiE "no modified files|did not modify" <<<"$salida"; then
    echo "formato: todo cumple"
    exit 0
  fi
  echo "$salida"
  echo "formato: hay líneas cambiadas sin formatear; arréglalo con scripts/formato.sh $desde" >&2
  exit 1
fi
# --force: también sobre ficheros con cambios sin añadir al índice, que es lo normal cuando se
# acaba de editar. Puede hacer falta repetirlo: formatear mueve los bordes de lo cambiado.
"$gcf" --binary "$cf" --extensions "$exts" --force "$desde"
