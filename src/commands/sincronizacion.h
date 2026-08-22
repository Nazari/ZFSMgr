#pragma once

#include <string>
#include <utility>
#include <vector>

// Sincronizar dos datasets.
//
// **No tiene nada que ver con `zfs send`.** Sincronizar trabaja a nivel de FICHEROS, sobre
// los puntos de montaje de los dos extremos: compara, copia lo que cambió y —si se le
// pide— borra en el destino lo que ya no está en el origen. Por eso necesita que los dos
// estén montados, y por eso puede destruir trabajo en el destino, cosa que copiar o
// nivelar no hacen nunca.
//
// El documento de diseño la agrupaba con las demás como si compartiera camino. No lo
// comparte: `zfs send` manda bloques de una historia común, y esto compara árboles de
// directorios. Es la razón de que sea «la que menos comparte».
//
// Lo que hay aquí es la REGLA —cuándo se puede y por qué no— y la carga tipada del verbo
// `--mutate-rsync-local`, que estaba dentro de la ventana principal
// (`mainwindow.cpp:1963`) y que el servidor web necesita igual.
namespace zfsmgr::base::sincronizacion {

enum class Fallo {
    Ninguno,
    ElMismoObjeto,
    OrigenNoEsDataset,
    DestinoNoEsDataset,
    ExtremoWindows,
    DistintaMaquina,
    OrigenNoMontado,
    DestinoNoMontado,
    RutaNoUsable,
    SinDaemon,
};

struct Extremo {
    std::string conexion;
    std::string objeto;         // el dataset; con «@» dentro no vale
    bool montado{false};
    std::string puntoMontaje;
    bool esWindows{false};
    bool tieneDaemon{false};
};

struct Plan {
    std::string rutaOrigen;
    std::string rutaDestino;
    Fallo fallo{Fallo::Ninguno};
    bool sePuede() const { return fallo == Fallo::Ninguno; }
};

// Lo que se puede decidir SIN preguntar a nadie: misma máquina, los dos datasets, ningún
// extremo Windows, daemon en pie.
//
// Está separado de `planea` porque el punto de montaje del origen cuesta una consulta al
// agente, y quien pinta el menú de acciones lo pinta para cada dataset que se mire. Con una
// sola función, ofrecer la acción costaba una consulta por dibujo; así el dibujo es gratis
// y la consulta se hace una vez, al pulsar.
Fallo compruebo(const Extremo& origen, const Extremo& destino);

// La comprobación entera, ya con los montajes. Devuelve las dos rutas.
//
// Los montajes son EL dato: sin ellos no hay nada que comparar. Un dataset con
// `canmount=off`, o montado donde no hay ruta absoluta, no se sincroniza por aquí aunque
// exista.
Plan planea(const Extremo& origen, const Extremo& destino);

std::string etiquetaDe(Fallo f);

// ¿Sirve esta ruta para sincronizar?
//
// En Unix, una ruta absoluta. **En Windows, una con letra de unidad** —«Z:/sa/»—, que es lo
// que de verdad se puede abrir allí: la propiedad `mountpoint` de un dataset en Windows dice
// «/winpool/sa», y esa ruta NO EXISTE para el sistema. Comprobado en vivo: `Test-Path` la da
// por falsa, y la buena sale de `zfs mount`.
//
// Está aparte porque es la misma comprobación que hace la interfaz de Qt
// (`isUsableMountPath`) y tenerla dos veces es tenerla mal en una de las dos.
bool rutaUsable(const std::string& ruta, bool esWindows = false);

// La carga de `--mutate-rsync-local`: base64 de un JSON
// `[borrar, enSeco, rsh, hostDestino, origen1, destino1, ...]`.
//
// Devuelve vacío si algún par no sirve. Las rutas tienen que ser absolutas: el daemon
// rechaza las que no empiezan por barra, así que dejarlas pasar aquí solo cambia dónde
// falla.
std::string cargaRsync(const std::vector<std::pair<std::string, std::string>>& pares,
                       bool borrar, bool enSeco,
                       const std::string& rsh, const std::string& hostDestino);

}  // namespace zfsmgr::base::sincronizacion
