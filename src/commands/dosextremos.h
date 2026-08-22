#pragma once

#include <string>

// Las acciones que necesitan DOS extremos: un origen y un destino.
//
// En la interfaz de Qt se piden marcando un origen y pulsando después sobre otro nodo, como
// copiar y pegar; el submenú «Con el origen …» ofrece las seis y **deja en gris las que no
// aplican, con el motivo**. Ese «qué aplica y por qué no» es una REGLA —qué deja hacer ZFS
// entre dos objetos— y no una decisión de interfaz, así que vive aquí: la interfaz la tenía
// dentro del menú contextual y el servidor web habría acabado con una segunda copia que se
// desincroniza.
//
// Ver help/es/menus_contextuales.md, «Las seis acciones de origen y destino».
namespace zfsmgr::base::dosextremos {

enum class Accion {
    Diff,
    Clonar,
    Copiar,
    Mover,
    Sincronizar,
    Nivelar,
};

// Por qué NO se puede, tipificado. Un booleano obligaba a que quien pinta el menú
// adivinara el motivo, y el motivo es justo lo que hay que enseñar: «no aplica» sin decir
// por qué deja al usuario probando combinaciones.
enum class NoAplica {
    Ninguna,                 // sí aplica
    SinOrigen,
    ElMismoObjeto,
    OrigenNoEsInstantanea,
    DestinoNoEsDataset,
    DistintoDataset,         // `zfs diff` compara dos puntos del MISMO dataset
    DistintaMaquina,
    DistintoPool,            // `zfs rename` no cruza pools
    OrigenNoEsDataset,
    DestinoDentroDelOrigen,  // meter un dataset dentro de sí mismo
    TodaviaNoEstaEnLaWeb,
};

const char* claveDe(Accion a);
std::string etiquetaDe(Accion a);
std::string etiquetaDe(NoAplica n);

// Un extremo: en qué máquina y qué objeto.
struct Extremo {
    std::string conexion;
    std::string objeto;

    bool vacio() const { return conexion.empty() || objeto.empty(); }
    bool esInstantanea() const { return objeto.find('@') != std::string::npos; }
    // El dataset al que pertenece: lo mismo si no es instantánea, y lo de delante de la
    // «@» si lo es.
    std::string dataset() const {
        const std::size_t i = objeto.find('@');
        return i == std::string::npos ? objeto : objeto.substr(0, i);
    }
    // El pool: lo de delante de la primera barra. Hace falta porque `zfs rename` NO cruza
    // pools —para eso está copiar— y el motivo hay que poder decirlo.
    std::string pool() const {
        const std::string d = dataset();
        const std::size_t i = d.find('/');
        return i == std::string::npos ? d : d.substr(0, i);
    }
    // El último componente del nombre, que es el que conserva al moverlo.
    std::string hoja() const {
        const std::string d = dataset();
        const std::size_t i = d.rfind('/');
        return i == std::string::npos ? d : d.substr(i + 1);
    }
};

// A dónde queda un dataset movido bajo otro: «destino/hoja del origen».
//
// Vive aquí y no en quien pinta el menú porque es la MISMA cuenta que hace la interfaz de
// Qt al encolar el renombrado, y tenerla dos veces es tenerla mal una de las dos.
std::string destinoDeMover(const Extremo& origen, const Extremo& destino);

// ¿Se puede hacer `a` desde `origen` hasta `destino`? `NoAplica::Ninguna` es que sí.
NoAplica compruebo(Accion a, const Extremo& origen, const Extremo& destino);

}  // namespace zfsmgr::base::dosextremos
