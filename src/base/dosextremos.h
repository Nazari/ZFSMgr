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
};

// ¿Se puede hacer `a` desde `origen` hasta `destino`? `NoAplica::Ninguna` es que sí.
NoAplica compruebo(Accion a, const Extremo& origen, const Extremo& destino);

}  // namespace zfsmgr::base::dosextremos
