#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_DIR="${1:-${PROJECT_ROOT}/help/img/auto}"

echo "[1/3] Compilando binario de capturas UI"
"${PROJECT_ROOT}/scripts/build-linux.sh"

mkdir -p "${OUTPUT_DIR}"

echo "[2/3] Generando capturas en ${OUTPUT_DIR}"
QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}" \
ZFSMGR_TEST_MODE=1 \
"${PROJECT_ROOT}/build-linux/zfsmgr_ui_doc_capture" "${OUTPUT_DIR}"

echo "[3/3] Capturas generadas:"
find "${OUTPUT_DIR}" -maxdepth 1 -type f -name '*.png' -printf '%f\n' | sort
