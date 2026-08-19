#pragma once

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Las instantáneas PROGRAMADAS (GSA): qué son y qué es válido.
//
// La programación de un dataset no vive en un fichero del programa: son PROPIEDADES DE
// USUARIO del propio dataset, con prefijo `org.fc16.gsa:`. Quien las ejecuta es otro
// agente —`/usr/local/libexec/zfsmgr-gsa.sh`, con su temporizador— y ni la interfaz ni el
// intérprete intervienen en eso: los dos se limitan a leer y escribir propiedades.
//
// **Por qué esto está en la capa base y no en la interfaz.** Las reglas vivían dentro de
// `MainWindow::validatePendingGsaDrafts`, en Qt, y en ningún sitio más. En cuanto el
// intérprete quisiera programar —y ya puede: `set org.fc16.gsa:diario=7` funciona hoy—
// habría que reescribirlas, y serían dos copias que se separan. Este repositorio ya ha
// pagado esa factura tres veces: la tabla de propiedades de ZFS, los motivos del
// transporte y las banderas de `zfs send`.
//
// Aquí no hay mensajes: hay MOTIVOS TIPADOS. Cada interfaz los redacta como le toca —la
// gráfica con sus tres idiomas, el intérprete con los suyos— y ninguna puede inventarse
// una regla que la otra no tenga.
//
// Ver docs/propuesta_gsa_cli.md.
namespace zfsmgr::base::gsa {

// El prefijo de las propiedades. Los nombres van en castellano porque así están en las
// máquinas ya instaladas y renombrarlos rompería sus programaciones.
extern const char* const kPrefijo;

struct Programacion {
    bool activado{false};
    bool recursivo{false};
    bool nivelar{false};
    int horario{0};
    int diario{0};
    int semanal{0};
    int mensual{0};
    int anual{0};
    std::string destino;   // «Conexión::Pool/Dataset»

    bool sinRetenciones() const {
        return horario <= 0 && diario <= 0 && semanal <= 0 && mensual <= 0 && anual <= 0;
    }
};

enum class Fallo {
    Ninguno,
    RetencionNoEntera,      // detalle: la propiedad culpable
    ActivadaSinRetencion,
    NivelarSinDestino,
    DestinoMalFormado,      // sin «::»
    DestinoSinConexion,     // detalle: el nombre de la conexión que falta
    ChocaConRecursiva,      // detalle: el dataset que ya la tiene
};

struct Motivo {
    Fallo fallo{Fallo::Ninguno};
    std::string dataset;   // a quién le pasa
    std::string detalle;   // la propiedad, la conexión ausente o el otro dataset
};

// Propiedades tal y como las devuelve `zfs get` → estructura. Insensible a mayúsculas en
// el nombre de la propiedad, como lo era la interfaz.
//
// Devuelve false solo si una retención no es un entero >= 0; el resto de valores no puede
// fallar aquí (un booleano que no se reconoce es «off», que es lo conservador).
bool desdePropiedades(const std::map<std::string, std::string>& props, Programacion& out,
                      Motivo& porQue);

// Estructura → las propiedades que hay que escribir, con su prefijo.
std::map<std::string, std::string> aPropiedades(const Programacion& p);

// Una programación, por sí sola. `conexionExiste` la resuelve quien llama: la lista de
// conexiones es del cliente, no de esta capa.
bool valida(const std::string& dataset, const Programacion& p,
            const std::function<bool(const std::string&)>& conexionExiste, Motivo& porQue);

// El conjunto: dos programaciones ACTIVADAS del mismo pool no pueden solaparse si una es
// recursiva. Se comprueba aparte porque no es una propiedad de ninguna de las dos.
struct Entrada {
    std::string dataset;
    Programacion prog;
};
bool validaConjunto(const std::vector<Entrada>& delMismoPool, Motivo& porQue);

// ¿`dataset` es `ancestro` o cuelga de él?
bool esMismoODescendiente(const std::string& dataset, const std::string& ancestro);

// El castellano de reserva del motivo, para quien no tenga catálogo propio.
std::string etiquetaDe(Fallo f);

// A qué CLASE pertenece una instantánea por su nombre: «hourly», «daily», «weekly»,
// «monthly», «yearly» —o lo que ponga, que las clases no son un conjunto cerrado—. Vacío
// si no la hizo el planificador.
//
// El nombre lo escribe `gsaCreateSnapshot` como «GSA-<clase>-<fecha>-<hora>», así que la
// clase es lo que va entre el primer y el segundo guion.
std::string claseDeInstantanea(const std::string& nombre);

// Las instantáneas de un dataset, ORDENADAS para enseñarlas: primero las manuales, en el
// orden en que llegaron, y después las programadas agrupadas por clase, con las clases
// conocidas en orden de periodo —de la hora al año— y las desconocidas al final.
//
// Devuelve pares (clase, instantáneas); la clase vacía es el grupo de las manuales, que
// va siempre primero y solo si hay alguna. Vive aquí y no dentro del árbol porque es una
// REGLA, no dibujo: el intérprete querrá la misma cuando liste instantáneas.
std::vector<std::pair<std::string, std::vector<std::string>>> agrupaInstantaneas(
    const std::vector<std::string>& nombres);

}  // namespace zfsmgr::base::gsa
