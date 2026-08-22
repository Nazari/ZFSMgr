#pragma once

#include <string>
#include <vector>

// Las cuatro acciones que mueven CONTENIDO entre datasets y directorios.
//
//   Desglosar   un subdirectorio se convierte en un dataset hijo que ocupa su lugar
//   Ensamblar   lo contrario: un dataset hijo vuelve a ser un directorio
//   Hacia Dir   el contenido del dataset se vuelca en un directorio llano
//   Desde Dir   un directorio —quizá de otra máquina— se vuelca DENTRO del dataset
//
// **Qué hace este módulo y por qué existe.** Aquí se compone el argv de cada una y viven
// sus reglas. Antes cada cliente lo armaba por su cuenta: el intérprete en `cli/shell.cpp`,
// el servidor en `web/main.cpp` y la interfaz en `native/mainwindow_advanced*.cpp`. Tres
// veces la misma orden, y con ella tres veces —o ninguna— sus reglas.
//
// No es una preocupación teórica. La regla de `assemble` de más abajo se descubrió
// EJECUTANDO, después de que la operación dijera que había funcionado sin hacer nada, y
// acabó escrita en un comentario del intérprete, otra vez en el del servidor, y resuelta de
// una tercera manera en la interfaz. Un sitio donde ponerla es lo que faltaba.
namespace zfsmgr::commands::avanzadas {

// --- Desglosar ---------------------------------------------------------------
//
// Cada par dice: QUÉ subdirectorio y QUÉ dataset hijo pasa a ocupar su sitio.
struct Desglose {
    std::string subdirectorio;  // relativo al punto de montaje del dataset
    std::string datasetNuevo;   // relativo al dataset padre
};

// `--mutate-advanced-breakdown <dataset> <subdir> <nuevo> [<subdir> <nuevo>...]`
//
// Devuelve vacío si no hay ningún par utilizable: el verbo con solo el dataset detrás no
// hace nada, y mandarlo sería pedirle al daemon que decida algo que aquí ya se sabe.
std::vector<std::string> argvDesglosar(const std::string& dataset,
                                       const std::vector<Desglose>& pares);

// --- Ensamblar ---------------------------------------------------------------

// El nombre COMPLETO de un hijo, a partir de lo que haya escrito quien llama.
//
// **Esta es la regla que cuesta descubrir.** El agente comprueba cada hijo con
// `zfs list <hijo>`, así que un nombre relativo —«fotos» en vez de «tank/datos/fotos»— no
// existe para él. Y no fallaba: la operación se saldaba con «ya absorbido» y **rc=0**, o
// sea que decía que sí y no había hecho nada. Se vio en vivo, no leyendo.
//
// Un nombre que ya lleve barra se respeta tal cual: puede ser un nieto
// («tank/datos/fotos/2024») y completarlo otra vez lo rompería.
std::string hijoConNombreCompleto(const std::string& dataset, const std::string& hijo);

// `--mutate-advanced-assemble <dataset> <hijo-completo> [<hijo-completo>...]`
//
// Los hijos pasan por `hijoConNombreCompleto`. Vacío si no queda ninguno.
std::vector<std::string> argvEnsamblar(const std::string& dataset,
                                       const std::vector<std::string>& hijos);

// --- Hacia Dir ---------------------------------------------------------------
//
// `--mutate-advanced-todir <dataset> <directorio> <0|1>`
//
// El último argumento es si se DESTRUYE el dataset de origen al terminar. Va como «0» o «1»
// y no como bandera con nombre porque así lo lee el verbo; que sea un booleano en esta
// interfaz y no una cadena es justo lo que evita que alguien mande «true» y destruya, o «no»
// y también destruya.
std::vector<std::string> argvHaciaDir(const std::string& dataset, const std::string& directorio,
                                      bool destruyeOrigen);

// ¿Sirve esta ruta como destino de «Hacia Dir»?
//
// Tiene que ser absoluta. Una relativa la interpretaría el daemon desde SU directorio de
// trabajo, que no es el de quien la escribió: el volcado acabaría en un sitio que nadie
// eligió.
bool rutaDeDestinoValida(const std::string& directorio);

// --- Desde Dir ---------------------------------------------------------------
//
// **Es la única de las cuatro que no puede ser un RPC**, y no por descuido: el verbo del
// agente lee un tar por la entrada estándar, y el canal RPC no tiene entrada estándar. Así
// que se arma una tubería con las dos puntas por SSH y la máquina de quien manda en medio,
// que es la que tiene las credenciales de las dos. Lo dice el propio daemon en el comentario
// de `runMutateAdvancedFromDir`.
//
// Lo que sí es de aquí son sus REGLAS, que hasta ahora vivían dentro de una función de la
// interfaz y no las tenía nadie más.

// ¿Vale este subdirectorio relativo como destino dentro del dataset?
//
// Lo comprobaba solo el daemon, y **después de que el tar ya estuviera corriendo**: para
// entonces la mitad del contenido puede haber salido de la máquina de origen. Aquí se
// comprueba antes de abrir la tubería.
//
// Vacío SÍ vale: significa la raíz del dataset.
bool subdirectorioRelativoValido(const std::string& rel);

// `--mutate-advanced-fromdir <dataset> [<rel>]`
//
// El `rel` solo se pone si no está vacío: el verbo lo trata como opcional y mandarle una
// cadena vacía detrás es pedirle que decida qué significa.
std::vector<std::string> argvDesdeDir(const std::string& dataset, const std::string& rel);

// De dónde sale un contenido: el directorio y la máquina en la que está.
struct OrigenDesdeDir {
    std::string ruta;      // tal como lo dio quien llama
    std::string maquina;   // el nombre de la conexión de la que sale
    bool windows{false};   // si sus separadores son «\\»
};

// Dónde cae cada origen DENTRO del dataset: un subdirectorio relativo por origen, en el
// mismo orden. Vacío significa la raíz.
//
// La regla:
//   - un solo origen   -> su CONTENIDO va a la raíz del dataset;
//   - varios           -> cada uno a un subdirectorio con el nombre de su directorio;
//   - si dos coinciden -> se antepone el nombre de su máquina.
//
// **Y el resultado se garantiza ÚNICO**, que es lo que no se cumplía. Anteponer la máquina
// solo desempata cuando las máquinas son distintas: dos directorios llamados «docs» de la
// MISMA conexión daban los dos «fc16-docs», y el segundo tar se extraía encima del primero.
// En una operación cuyo trabajo es copiar, eso es perder datos sin decir nada.
//
// El nombre resultante se limpia además de lo que no puede ser un nombre de directorio: un
// nombre de conexión con una barra dentro habría creado un nivel de más, y un «..» habría
// sacado el volcado fuera del dataset —lo habría parado el daemon, pero con el tar ya en
// marcha—.
std::vector<std::string> subdirectoriosDeDestino(const std::vector<OrigenDesdeDir>& origenes);

// `--mutate-advanced-fromdir-prepare <dataset> [<rel>]`
//
// La PRIMERA MITAD de Desde Dir: montar, resolver el punto de montaje, crear el
// subdirectorio y decir en qué ruta absoluta quedó. Sin el tar.
//
// **Es lo que permite hacer Desde Dir sin tubería de shell.** `--tree-recv-listen`, el
// receptor del árbol entre daemons, exige que el directorio ya exista; por eso el servidor
// web solo sabe volcar a la raíz del dataset. Con esto delante, el árbol también sirve para
// un subdirectorio, y entonces los datos van de máquina a máquina en vez de pasar por el
// equipo de quien manda.
std::vector<std::string> argvDesdeDirPreparar(const std::string& dataset, const std::string& rel);

// La ruta que contesta ese verbo: una línea «DST=<ruta absoluta>». Vacío si no la trae.
std::string rutaPreparada(const std::string& salida);

// ¿Puede esta pareja hacer Desde Dir por el árbol entre daemons, sin tubería?
//
// Hacen falta las DOS puntas con daemon: el destino para preparar y escuchar, y el origen
// para enviar. El camino del tar solo pide daemon en el destino —al origen le basta SSH—,
// así que esto NO lo sustituye: lo adelanta cuando se puede y deja el otro de respaldo.
//
// La otra razón para conservar el respaldo no se ve desde aquí: el árbol abre un puerto
// efímero en el destino y el origen conecta a él. Donde haya un cortafuegos entre las dos
// máquinas, SSH pasa y esto no.
bool puedeIrPorElArbol(bool origenTieneDaemon, bool destinoTieneDaemon);

}  // namespace zfsmgr::commands::avanzadas
