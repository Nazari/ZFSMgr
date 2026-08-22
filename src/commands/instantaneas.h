#pragma once

#include <string>
#include <vector>

// Instantáneas y lo que se hace con ellas: crear, destruir, volver atrás, clonar y retener.
//
// Como en el resto de `commands/`, aquí está el argv y las reglas; quién habla con el agente
// es cosa del cliente.
namespace zfsmgr::commands::instantaneas {

// El alcance de un destroy o un rollback. Es un enumerado y no una cadena a propósito: el
// verbo del daemon lo recibe como «R», «r» o vacío, y esas tres letras no se distinguen de
// un vistazo en una llamada. Con nombres, quien lee la línea sabe qué se lleva por delante.
enum class Alcance {
    Solo,          // solo el objeto nombrado
    Descendientes, // -r: sus hijos
    Dependientes,  // -R: sus hijos Y lo que dependa de ellos (clones)
};

// La letra que espera el verbo del daemon.
std::string letraDeAlcance(Alcance a);

// ¿Se lleva por delante algo más que el objeto nombrado? Sirve para decidir cuánto avisar.
bool arrastraOtros(Alcance a);

// `--mutate-zfs-snapshot <dataset@nombre> <0|1>`
//
// El último argumento es si arrastra a los descendientes. Va como «0» o «1» y el nombre
// completo se compone aquí: pasarlo ya montado era invitar a que un cliente mandara el
// dataset y otro el nombre suelto.
//
// Vacío si el nombre no vale —ver `nombreValido`—, porque ZFS acepta muy poco ahí y el error
// que devuelve no dice cuál de los dos trozos está mal.
std::vector<std::string> argvCrearInstantanea(const std::string& dataset,
                                              const std::string& nombre, bool recursiva);

// `--mutate-zfs-destroy <objeto> <0|1> <alcance>`
//
// Vale para datasets Y para instantáneas: el verbo mira si el nombre lleva «@».
std::vector<std::string> argvDestruir(const std::string& objeto, bool forzar,
                                      Alcance alcance = Alcance::Solo);

// `--mutate-zfs-rollback <instantánea> <0|1> <alcance>`
//
// **Rollback DESCARTA todo lo escrito después de esa instantánea.** No hay alcance «Solo»
// útil aquí cuando existen instantáneas posteriores: ZFS se niega hasta que se le dice que
// también se las lleva, así que quien no pase `Descendientes` verá un error de ZFS en vez
// de una pregunta. Se devuelve el argv igual: la decisión de preguntar es del cliente.
std::vector<std::string> argvRollback(const std::string& instantanea, bool forzar,
                                      Alcance alcance = Alcance::Solo);

// `--mutate-zfs-clone <instantánea-origen> <dataset-nuevo>`
//
// Vacío si el origen no es una instantánea: clonar un dataset no existe en ZFS, y mandarlo
// devuelve un mensaje que habla de otra cosa.
std::vector<std::string> argvClonar(const std::string& instantaneaOrigen,
                                    const std::string& datasetNuevo);

// La misma, en forma de argv de `zfs` para el camino genérico: `zfs clone [banderas] <origen>
// <nuevo>`.
//
// Existe por lo mismo que su gemela de las retenciones: el verbo tipado del daemon toma
// exactamente dos argumentos y no admite banderas, y hay pantallas que ofrecen `-p`, `-u` y
// propiedades. Las comprobaciones son las mismas en las dos formas.
std::vector<std::string> argvZfsClonar(const std::string& instantaneaOrigen,
                                       const std::string& datasetNuevo,
                                       const std::vector<std::string>& banderas = {});

// `--mutate-zfs-hold <etiqueta> <instantánea>` y su contrario.
//
// **La etiqueta va PRIMERO**, que es al revés de lo que uno escribe al hablar («retén esta
// instantánea con esta etiqueta»). Invertirlos no da error: `zfs hold` acepta cualquier par
// de cadenas y falla luego diciendo que no encuentra la instantánea «micopia».
//
// **Estos verbos tipados NO admiten `-r`**: el daemon lee exactamente dos parámetros y se
// los pasa a `zfs`. Para retener un árbol entero está el par de abajo, que va por el verbo
// genérico. No es un descuido de este módulo: el verbo tipado es más estrecho a propósito,
// y aquí se refleja en vez de disimularse con un parámetro que se ignoraría.
std::vector<std::string> argvRetener(const std::string& etiqueta, const std::string& instantanea);
std::vector<std::string> argvSoltar(const std::string& etiqueta, const std::string& instantanea);

// Las mismas, en forma de argv de `zfs` para `--mutate-zfs-generic`, que es el único camino
// que admite el recursivo: `zfs hold [-r] <etiqueta> <instantánea>`.
std::vector<std::string> argvZfsRetener(const std::string& etiqueta,
                                        const std::string& instantanea, bool recursivo);
std::vector<std::string> argvZfsSoltar(const std::string& etiqueta,
                                       const std::string& instantanea, bool recursivo);

// ¿Sirve esta etiqueta de retención?
//
// Viaja hasta un argv de `zfs`, y un espacio o una arroba dentro convertirían la orden en
// otra. La barra tampoco: haría que pareciera un nombre de dataset.
bool etiquetaValida(const std::string& etiqueta);

// ¿Es esto una instantánea? Lo que lo decide es la arroba, y esa comprobación estaba escrita
// a mano en los tres clientes.
bool esInstantanea(const std::string& objeto);

// El nombre completo de una instantánea nueva sobre un dataset: `<dataset>@<nombre>`.
//
// Si quien llama ya trae la arroba, se respeta lo que haya puesto.
std::string nombreDeInstantanea(const std::string& dataset, const std::string& nombre);

}  // namespace zfsmgr::commands::instantaneas
