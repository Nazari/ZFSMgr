#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Copia de árboles de ficheros sin rsync.
//
// Existe porque rsync no está en Windows, y con él se caen Desglosar, Ensamblar,
// Hacia Dir y Sincronizar. La alternativa —instalar un rsync de Cygwin— reintroduciría
// la capa Unix que se quitó con MSYS2, y sus ACL sobre NTFS son una emulación
// aproximada justo donde más importa. Ver docs/diseno_tecnico_copia_nativa_sin_rsync.md.
//
// Reproduce lo que se le pedía a `rsync -aHWS [-x] --exclude=/x/`: recursivo,
// preservando permisos, marcas de tiempo, enlaces simbólicos, ENLACES DUROS y ficheros
// dispersos, sin cruzar a otro sistema de ficheros si se pide, y con exclusiones
// ancladas en la raíz de la copia.
namespace zfsmgr::copytree {

struct Options {
    // No bajar a otro sistema de ficheros. En Unix se compara el dispositivo; en Windows
    // no se desciende en puntos de reparseo, que es como OpenZFS monta cada dataset.
    bool oneFileSystem = false;
    // Nombres de PRIMER NIVEL que no se copian. Anclados a propósito: sin eso, excluir
    // "Tools" se comería cualquier directorio con ese nombre en todo el subárbol.
    std::vector<std::string> excludes;

    // Saltar lo que ya está igual en el destino, comparando tamaño y fecha.
    //
    // Por defecto SÍ, que es lo que hacía rsync: `rsync -a` no reescribe lo que no ha
    // cambiado. Sin esto, la copia rehacía el árbol entero en cada pasada —da igual en
    // un destino vacío, pero convierte en absurdo cualquier sincronización, y encarece
    // los reintentos de Ensamblar, que repiten la copia hasta cinco veces.
    bool skipUnchanged = true;

    // Borrar del destino lo que no está en el origen (el `--delete` de rsync).
    //
    // Solo tiene sentido al sincronizar. Lo excluido NO se borra, igual que en rsync:
    // dejarlo fuera de la copia a propósito y que el borrado se lo lleve sería lo peor
    // de los dos mundos.
    bool deleteExtraneous = false;

    // No tocar nada: recorrer y contar lo que se haría. Es lo que alimenta la vista
    // previa de la interfaz antes de aplicar.
    bool dryRun = false;
};

struct Result {
    bool ok = false;
    std::string error;
    std::uint64_t filesCopied = 0;
    std::uint64_t dirsCreated = 0;
    std::uint64_t symlinksCopied = 0;
    // Enlaces duros recreados como tales en vez de copiados otra vez. Es la diferencia
    // principal con robocopy, que los duplicaría en silencio.
    std::uint64_t hardLinksRecreated = 0;
    std::uint64_t bytesWritten = 0;
    // Ya estaban iguales en el destino.
    std::uint64_t filesSkipped = 0;
    // Sobraban en el destino y se borraron (o se borrarían, en simulacro).
    std::uint64_t entriesDeleted = 0;
};

// Copia el CONTENIDO de srcDir dentro de dstDir, como `rsync src/ dst/`.
Result copyTree(const std::string& srcDir, const std::string& dstDir, const Options& opt);

// Cuenta lo que todavía FALTA en el destino, con las mismas exclusiones que la copia.
//
// Equivale a `rsync -rni --ignore-existing src/ dst/` contando las líneas «>f»: es la
// verificación que se hace ANTES de borrar el origen en Desglosar, Ensamblar y Hacia
// Dir, y la única red que impide perder datos si la copia quedó a medias.
//
// Devuelve -1 si no se pudo comprobar, que el llamante debe tratar como «no verificado»
// y NO como «cero pendientes».
long long countPending(const std::string& srcDir, const std::string& dstDir,
                       const Options& opt);

}  // namespace zfsmgr::copytree
