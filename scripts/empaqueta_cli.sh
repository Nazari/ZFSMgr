#!/usr/bin/env bash
# Se queda por costumbre: genera solo el paquete de línea de órdenes.
#
# El trabajo lo hace scripts/empaqueta.sh, que arma los DOS artefactos de una versión
# —`zfsmgr-<ver>.zip` con interfaz y `zfsmgr-cli-<ver>.zip` sin ella— con la misma
# estructura y un instalador por sistema. Esto es su modo `--solo-cli`, que no exige tener
# compiladas las interfaces.
exec "$(dirname "${BASH_SOURCE[0]}")/empaqueta.sh" --solo-cli "$@"
