#include "shell.h"

#include "daemonpayload.h"
#include "ayuda.h"
#include "gramatica_cli.h"
#include "zfsprops.h"
#include "helpers.h"
#include "linea.h"
#include "json.h"
#include "process.h"
#include "secretinput.h"
#include "strutil.h"
#include "tr.h"
#include "transportcmd.h"
#include "transporttunnel.h"
#include "transportrpc.h"
#include "zfsmurl.h"

#ifndef _WIN32
#include <sys/ioctl.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <chrono>
#include <sstream>
#include <thread>
#include <string>
#include <vector>

namespace zfsmgr::cli {
namespace {

namespace B = zfsmgr::base;
namespace H = zfsmgr::base::helpers;
namespace T = zfsmgr::base::transport;
using B::ZfsmUrl;
using B::ZfsmKind;

// Qué clase de sitio es el actual. La RAÍZ no es una URL válida —`zfsm://` sin conexión no
// nombra nada—, así que se representa con la conexión vacía y solo existe dentro del
// intérprete, donde sí hace falta: es lo que da un sitio al que subir desde una conexión.
enum class Nodo { Raiz, Conexion, Dataset, Snapshot };

struct Estado {
    // La versión de agente de cada conexión, recordada durante la sesión. Ver
    // listaConexiones: preguntarla cuesta una ida y vuelta por máquina.
    std::map<std::string, std::string> versionDaemon;
    // Los pools de cada conexión. Ver destinoDePool: se usan para decidir si el primer
    // argumento suelto nombra un pool o es un argumento de la orden.
    std::map<std::string, std::set<std::string>> poolsPorConexion;
    // Las propiedades de cada sitio, para el completado. Ver propiedadesDe.
    std::map<std::string, std::vector<std::string>> propsPorSitio;
    Sesion* ses{nullptr};
    Conexiones conns;
    ZfsmUrl actual;   // conexión vacía = raíz
    ZfsmUrl anterior;
    bool hayAnterior{false};
    Formato formato{Formato::Texto};
    bool asumirSi{false};  // `-y`: no preguntar antes de lo destructivo
    bool salir{false};
    int ultimoRc{0};
};

Nodo nodoDe(const ZfsmUrl& u) {
    if (u.connection.empty()) {
        return Nodo::Raiz;
    }
    if (!u.snapshot.empty()) {
        return Nodo::Snapshot;
    }
    if (!u.dataset.empty()) {
        return Nodo::Dataset;
    }
    return Nodo::Conexion;
}

std::string textoDe(const ZfsmUrl& u) {
    if (u.connection.empty()) {
        return "zfsm:/";
    }
    return B::formatZfsmUrl(u);
}

// --- Resolución de un destino.
//
// El modelo es el de un sistema de ficheros, con la lista de conexiones como raíz:
//
//     /                          las conexiones
//     /OldLau                    una conexión
//     /OldLau/winpool            el pool, que ES un dataset
//     /OldLau/winpool/sa         un dataset
//     /OldLau/winpool/sa@ayer    una instantánea
//
// Se aceptan las cuatro formas que uno escribiría sin pensar: la URL entera, la ruta
// absoluta, la relativa, y `..` / `.` / `-`.
std::vector<std::string> tramosDe(const std::string& texto) {
    std::vector<std::string> t = B::split(texto, "/", false);
    if (t.empty()) {
        t.push_back(std::string());
    }
    return t;
}

bool resuelve(const Estado& e, const std::string& textoEntrada, ZfsmUrl& out, std::string& error) {
    error.clear();
    std::string texto = B::trim(textoEntrada);
    if (texto.empty()) {
        out = e.actual;
        return true;
    }

    // Un esquema que NO es el nuestro se dice claramente. Sin esto, «zfsmgr://fc16» se
    // trataba como una ruta relativa y el error hablaba de un tramo llamado «zfsmgr:»,
    // que no ayuda a ver que lo único que sobra son tres letras.
    {
        const std::size_t dosPuntos = texto.find("://");
        if (dosPuntos != std::string::npos
            && B::toLowerAscii(texto.substr(0, dosPuntos)) != "zfsm") {
            error = B::format(T("t_esquema_malo", "el esquema es «zfsm://», no «%1://»"),
                              {texto.substr(0, dosPuntos)});
            return false;
        }
    }

    // La URL completa: siempre vale y nunca es ambigua.
    if (B::startsWith(B::toLowerAscii(texto), "zfsm://")) {
        if (!B::parseZfsmUrl(texto, out, error)) {
            return false;
        }
        // El nombre de la conexión se normaliza a su IDENTIFICADOR. Se admite escribirla
        // por su nombre visible —«Local»— pero la URL que se enseña y se copia tiene que
        // ser siempre la misma, o dos formas de decir lo mismo parecen dos sitios.
        const auto* p = buscarConexion(e.conns, out.connection);
        if (!p) {
            error = B::format(T("t_no_conn_nombre", "no hay ninguna conexión llamada «%1»"),
                              {out.connection});
            return false;
        }
        out.connection = p->id.empty() ? p->name : p->id;
        return true;
    }

    if (texto == "-") {
        if (!e.hayAnterior) {
            error = T("t_no_hay_anterior", "no hay sitio anterior");
            return false;
        }
        out = e.anterior;
        return true;
    }

    // Un fragmento suelto (`#properties`) o una instantánea suelta (`@ayer`) se aplican al
    // sitio actual: es lo que uno escribe estando ya en el dataset.
    if (texto.front() == '#' || texto.front() == '@') {
        const std::string base = textoDe(e.actual);
        if (e.actual.connection.empty()) {
            error = T("t_falta_estar_ds", "hace falta estar en un dataset");
            return false;
        }
        // Al pegar un `@` hay que quitar antes el que hubiera: `@a` desde `ds@b` es `ds@a`,
        // no `ds@b@a`, que además ni siquiera es una URL válida.
        ZfsmUrl limpia = e.actual;
        if (texto.front() == '@') {
            limpia.snapshot.clear();
            limpia.section.clear();
            limpia.detail.clear();
        }
        return B::parseZfsmUrl(B::formatZfsmUrl(limpia) + texto, out, error);
    }

    const bool absoluta = texto.front() == '/';
    if (absoluta) {
        texto = texto.substr(1);
    }

    // Se trocea y se aplica tramo a tramo, para que `..` funcione en medio.
    const auto aplica = [&](ZfsmUrl base, const std::vector<std::string>& tramos,
                            ZfsmUrl& res, std::string& err) -> bool {
        for (const std::string& tramoCrudo : tramos) {
            std::string tramo;
            if (!B::percentDecode(tramoCrudo, tramo)) {
                err = B::format(T("t_tramo_malo", "tramo mal codificado: %1"), {tramoCrudo});
                return false;
            }
            if (tramo.empty() || tramo == ".") {
                continue;
            }
            if (tramo == "..") {
                switch (nodoDe(base)) {
                    case Nodo::Raiz:
                        break;  // de la raíz no se sube
                    case Nodo::Conexion:
                        base = ZfsmUrl{};
                        break;
                    case Nodo::Snapshot:
                        base.snapshot.clear();
                        base.section.clear();
                        base.detail.clear();
                        break;
                    case Nodo::Dataset: {
                        base.section.clear();
                        base.detail.clear();
                        const std::size_t barra = base.dataset.rfind('/');
                        if (barra == std::string::npos) {
                            // Del pool se sube a la conexión: el pool ES el dataset raíz.
                            base.dataset.clear();
                            base.pool.clear();
                        } else {
                            base.dataset = base.dataset.substr(0, barra);
                        }
                        break;
                    }
                }
                continue;
            }
            // Un tramo con `@` dentro nombra una instantánea del hijo.
            std::string nombre = tramo;
            std::string snap;
            const std::size_t arroba = nombre.find('@');
            if (arroba != std::string::npos) {
                snap = nombre.substr(arroba + 1);
                nombre = nombre.substr(0, arroba);
            }
            switch (nodoDe(base)) {
                case Nodo::Raiz: {
                    const auto* p = buscarConexion(e.conns, nombre);
                    if (!p) {
                        err = B::format(T("t_no_conn_nombre", "no hay ninguna conexión llamada «%1»"), {nombre});
                        return false;
                    }
                    base = ZfsmUrl{};
                    base.kind = ZfsmKind::Connection;
                    base.connection = p->id.empty() ? p->name : p->id;
                    break;
                }
                case Nodo::Conexion:
                    base.pool = nombre;
                    base.dataset = nombre;
                    base.kind = ZfsmKind::Dataset;
                    break;
                case Nodo::Dataset:
                    base.dataset += "/" + nombre;
                    break;
                case Nodo::Snapshot:
                    err = B::format(T("t_snap_sin_hijos", "una instantánea no tiene hijos: %1"), {nombre});
                    return false;
            }
            if (!snap.empty()) {
                base.snapshot = snap;
                base.kind = ZfsmKind::Snapshot;
            }
        }
        res = base;
        return true;
    };

    const std::vector<std::string> tramos = B::split(texto, "/", false);
    if (absoluta) {
        return aplica(ZfsmUrl{}, tramos, out, error);
    }

    // Una ruta que empieza por el nombre del POOL en el que estamos es el nombre ZFS
    // completo, no un hijo. Sin esto, `destroy zfsmgrtest/clonado` desde
    // `zfsm://Local/zfsmgrtest/origen` apuntaba a
    // `zfsmgrtest/origen/zfsmgrtest/clonado`, que es un nombre válido y no existe — o sea
    // un error confuso donde uno había escrito exactamente lo que quería.
    if (!e.actual.pool.empty() && tramosDe(texto).front() == e.actual.pool) {
        ZfsmUrl base;
        base.kind = ZfsmKind::Connection;
        base.connection = e.actual.connection;
        return aplica(base, tramosDe(texto), out, error);
    }

    // Si el primer tramo nombra una CONEXIÓN, la ruta es absoluta. Es lo que uno escribe al
    // saltar de una máquina a otra, sin anteponer una barra que nadie recuerda.
    //
    // Se comprueba ANTES de probar la relativa, y esa es la corrección: antes se intentaba
    // la relativa primero y solo se caía aquí «si el primer tramo no encajaba donde
    // estamos». El problema es que estando DENTRO de una conexión no encajar es imposible:
    // cualquier nombre se acepta como pool sin preguntar si existe. Así que la regla nunca
    // llegaba a aplicarse y `cd unibody` desde `zfsm://local` acababa en
    // `zfsm://local/unibody`, que no existe — mientras la ayuda prometía lo contrario.
    //
    // Va DESPUÉS de la regla del pool: si uno está plantado en el pool `sback` y escribe
    // `sback/x`, quiere el nombre ZFS completo aunque exista una conexión llamada igual.
    // Para llegar a un hijo que se llame como una conexión queda `./nombre`, que empieza
    // por un tramo que no es nombre de nada.
    if (buscarConexion(e.conns, tramos.front()) != nullptr) {
        return aplica(ZfsmUrl{}, tramos, out, error);
    }

    std::string errRelativa;
    if (aplica(e.actual, tramos, out, errRelativa)) {
        return true;
    }
    error = errRelativa;
    return false;
}

// Una copia del perfil con las credenciales de sudo puestas si hacían falta y no estaban.
//
// Hace falta en los caminos que ejecutan una orden de shell —el listado de contenido—,
// porque `withSudoCommand` mete la contraseña en la orden y sin ella `sudo` se queda
// pidiéndola por un terminal que no está escuchando. El RPC del daemon no pasa por aquí:
// allí quien eleva es el propio daemon.
B::ConnectionProfile conSudo(Estado& e, const B::ConnectionProfile& p) {
    B::ConnectionProfile copia = p;
    if (!copia.useSudo || !copia.password.empty() || !T::isLocalConnection(copia)) {
        return copia;
    }
    e.ses->transporte.resolveLocalSudo(copia);
    return copia;
}

// El identificador de la conexión que nombra una URL, o vacío si es la raíz.
std::string idDe(const Estado& e, const ZfsmUrl& u);

// Relee las conexiones y con ellas las marcas de desconexión, que viven en el mismo
// fichero. Van juntas a propósito: leer unas sin las otras deja el intérprete creyendo que
// una conexión está disponible cuando acaban de apartarla.
void recarga(Estado& e) { e.conns = cargarConexiones(e.ses->dirConfig, e.ses->maestra); }

bool estaApartada(const Estado& e, const std::string& id) { return e.conns.desconectada(id); }

// El perfil de la conexión que nombra una URL.
const B::ConnectionProfile* perfilDe(const Estado& e, const ZfsmUrl& u, std::string& error) {
    if (u.connection.empty()) {
        error = T("t_falta_conexion", "hace falta estar en una conexión");
        return nullptr;
    }
    const auto* p = buscarConexion(e.conns, u.connection);
    if (!p) {
        error = B::format(T("t_no_conn_nombre", "no hay ninguna conexión llamada «%1»"),
                          {u.connection});
    }
    return p;
}

// El perfil de una conexión CON LA QUE SE VA A HABLAR. Distinto de `perfilDe`: aquí se
// respeta la marca de desconexión, que es lo que le da sentido.
//
// Navegar a una conexión apartada sí se permite —hay que poder llegar a ella para volver a
// conectarla—; lo que no se hace es tocar la máquina.
const B::ConnectionProfile* perfilVivoDe(const Estado& e, const ZfsmUrl& u, std::string& error) {
    const auto* p = perfilDe(e, u, error);
    if (!p) {
        return nullptr;
    }
    const std::string id = p->id.empty() ? p->name : p->id;
    if (estaApartada(e, id)) {
        error = B::format(T("t_apartada",
                        "%1 está marcada como desconectada; use «connect %1» para usarla"),
                      {id});
        return nullptr;
    }
    return p;
}

// Una operación de `zfs` por el verbo genérico, que recibe el argv en JSON y solo admite
// una lista cerrada de operaciones. Declarada aquí porque la usan órdenes de más arriba.
// Lo que una orden recibe: el destino ya resuelto y las ranuras que reconoció la gramática.
struct Peticion {
    ZfsmUrl objetivo;
    const LineaAnalizada* l{nullptr};
    const Orden* orden{nullptr};

    // Las banderas del mandato original que se han escrito, listas para añadir al argv.
    //
    // Se reconstruyen desde lo DECLARADO y no copiando lo que llegó: así lo que se le pasa
    // a `zfs`/`zpool` no puede ser nada que no esté en la lista, por mucho que cambie el
    // analizador.
    std::vector<std::string> nativas() const {
        std::vector<std::string> out;
        if (!orden) {
            return out;
        }
        for (const Nativa& n : orden->nativas) {
            const std::string corta = n.forma;
            const std::string larga =
                corta.size() > 2 && corta[1] == '-' ? corta.substr(2) : corta.substr(1);
            if (n.valor) {
                const auto it = l->opciones.find(larga);
                if (it != l->opciones.end()) {
                    out.push_back(corta);
                    out.push_back(it->second);
                }
                continue;
            }
            for (const std::string& b : l->banderas) {
                if (b == corta) {
                    out.push_back(corta);
                }
            }
            if (l->opciones.count(larga) > 0 && corta.rfind("--", 0) == 0) {
                out.push_back(corta);
            }
        }
        return out;
    }

    // Las ranuras llegan con NOMBRE desde la gramática; ya no hay que contar posiciones.
    std::vector<std::string> lista(const std::string& n) const { return l->lista(n); }
    std::string uno(const std::string& n) const { return l->uno(n); }
    bool tiene(const std::string& b) const { return l->tiene(b); }
    std::string valor(const std::string& k) const {
        std::string limpia = k;
        while (!limpia.empty() && limpia.front() == '-') {
            limpia.erase(limpia.begin());
        }
        const auto it = l->opciones.find(limpia);
        return it == l->opciones.end() ? std::string() : it->second;
    }
};

bool zfsGenerico(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv);
bool zpoolGenerico(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv);
bool cmdCrearPool(Estado& e, const Peticion& pet, const ZfsmUrl& destino);
bool enviaComoTrabajo(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv);
bool lanzaOEspera(Estado& e, const Peticion& pet, const ZfsmUrl& destino,
                  const std::vector<std::string>& argv);
std::map<std::string, std::string> clavesDe(const std::string& texto);

// --- Ejecutar un verbo del agente sobre el destino, contando lo que salga mal.
bool agente(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& args,
            std::string& out, int timeoutMs = 60000) {
    out.clear();
    std::string error;
    const auto* p = perfilVivoDe(e, destino, error);
    if (!p) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    std::string err;
    int rc = -1;
    std::string motivo;
    if (!ejecutarAgente(*e.ses, *p, args, out, err, rc, &motivo, timeoutMs)) {
        std::fprintf(stderr, TC("t_no_se_pudo_f6e380", "no se pudo hablar con %s: %s\n"), (p->name.empty() ? p->id : p->name).c_str(), motivo.c_str());
        e.ultimoRc = 1;
        return false;
    }
    if (rc != 0) {
        const std::string detalle = B::trim(err).empty() ? B::trim(out) : B::trim(err);
        std::fprintf(stderr, "%s\n", detalle.empty() ? "la orden falló" : detalle.c_str());
        e.ultimoRc = rc;
        return false;
    }
    e.ultimoRc = 0;
    return true;
}

// El destino de una orden: `--on`/`--from` si se dan, y si no, el sitio actual.
//
// **Las dos son sinónimas a propósito.** «from» es la palabra natural en las órdenes que
// además tienen un destino —`todir`, `assemble`—, y «on» en las que actúan sobre un solo
// sitio. Obligar a recordar cuál lleva cada una sería una regla que no aporta nada.
// El destino cuando la orden NO tiene argumentos propios: vale escribirlo suelto.
//
// Existe porque `edit fc16` se limitaba a IGNORAR el «fc16» y editaba la conexión donde
// uno estaba — decía «actualizada la conexión local» y uno se quedaba pensando que había
// editado fc16. Ignorar un argumento en silencio es la peor manera de equivocarse.


// Pide un campo por el terminal, ofreciendo un valor por omisión entre corchetes.
// Devuelve false solo si no hay terminal: en un guion se usan las opciones.
bool pide(const std::string& etiqueta, const std::string& porOmision, std::string& out) {
    std::string err;
    const std::string aviso =
        etiqueta + (porOmision.empty() ? "" : " [" + porOmision + "]") + ": ";
    if (!preguntarPorTerminal(aviso, out, err)) {
        return false;
    }
    out = B::trim(out);
    if (out.empty()) {
        out = porOmision;
    }
    return true;
}


// --- El preámbulo único: resolver el destino y repartir los argumentos contra la FIRMA.
//
// Sustituye a las cuatro maneras que convivían de hacer lo mismo. Ver docs/gramatica_cli.md.
//
// Lo importante no es que centralice: es la última comprobación. **Lo que no consume
// ninguna ranura es un error.** Sin ella, una orden que no mira sus argumentos sueltos los
// ignora en silencio, que es como `install-daemon oldlau` acabó reinstalando el daemon en
// la máquina local sin decir nada.

// ¿Encaja este nodo en lo que la orden pide?
bool encaja(const ZfsmUrl& u, Objetivo qué) {
    switch (qué) {
        case Objetivo::Ninguno:
        case Objetivo::Cualquiera:
            return true;
        case Objetivo::Conexion:
            // La conexión y NADA MÁS. Antes bastaba con que la URL tuviera conexión, y con
            // eso `install-daemon local/sobra1` se daba por bueno tomando «sobra1» como si
            // fuera una máquina.
            return nodoDe(u) == Nodo::Conexion;
        case Objetivo::Pool:
            return nodoDe(u) == Nodo::Dataset && u.isPoolRoot();
        case Objetivo::Dataset:
            return nodoDe(u) == Nodo::Dataset;
        case Objetivo::Instantanea:
            return nodoDe(u) == Nodo::Snapshot;
        case Objetivo::DatasetOInstantanea:
            return nodoDe(u) == Nodo::Dataset || nodoDe(u) == Nodo::Snapshot;
    }
    return false;
}

// Devuelve `std::string` y NO `const char*`: `TC()` da el `c_str()` de un temporal, así
// que un puntero a eso queda colgando en cuanto termina la expresión. Se coló aquí y el
// mensaje salió sin el objeto —«hace falta  (ahora: zfsm://local)»—, que es el mismo fallo
// que ya había mordido en `etiqueta()`. El compilador no lo ve: no es un switch incompleto,
// es una vida útil.
std::string nombreDe(Objetivo qué) {
    switch (qué) {
        case Objetivo::Ninguno: return {};
        case Objetivo::Cualquiera: return {};
        case Objetivo::Conexion: return T("t_obj_conexion", "una conexión");
        case Objetivo::Pool: return T("t_obj_pool", "un pool");
        case Objetivo::Dataset: return T("t_obj_dataset", "un dataset");
        case Objetivo::Instantanea: return T("t_obj_snap", "una instantánea");
        case Objetivo::DatasetOInstantanea:
            return T("t_obj_ds_snap", "un dataset o una instantánea");
    }
    return {};
}

// El sitio ACTUAL, subido hasta lo que la orden pide.
//
// Un argumento explícito tiene que ser exactamente lo que se pide —si escribo algo, quiero
// eso—, pero el sitio actual se interpreta con sentido: estando en `local/tank/datos`,
// `install-daemon` habla de la MÁQUINA, no del dataset, y no tiene sentido obligar a subir
// a mano antes de teclearlo.
ZfsmUrl subeHasta(const ZfsmUrl& u, Objetivo qué) {
    ZfsmUrl r = u;
    switch (qué) {
        case Objetivo::Conexion:
            r.pool.clear();
            r.dataset.clear();
            r.snapshot.clear();
            r.section.clear();
            r.detail.clear();
            if (!r.connection.empty()) {
                r.kind = ZfsmKind::Connection;
            }
            return r;
        case Objetivo::Pool:
            if (!r.pool.empty()) {
                r.dataset = r.pool;
                r.snapshot.clear();
                r.section.clear();
                r.detail.clear();
                r.kind = ZfsmKind::Dataset;
            }
            return r;
        case Objetivo::Dataset:
            // De una instantánea se sube a su dataset; de un dataset no se sube.
            if (!r.snapshot.empty()) {
                r.snapshot.clear();
                r.section.clear();
                r.detail.clear();
                r.kind = ZfsmKind::Dataset;
            }
            return r;
        case Objetivo::Ninguno:
        case Objetivo::Cualquiera:
        case Objetivo::Instantanea:
        case Objetivo::DatasetOInstantanea:
            return r;
    }
    return r;
}





// Resuelve el DESTINO de la orden. Nada más.
//
// Aquí ya no se trocea ni se reparten argumentos: eso lo hace la gramática, y las ranuras
// llegan con nombre. Lo que queda es lo que sí es semántica y ninguna gramática puede
// decidir: convertir el texto en una URL, y comprobar que el nodo es del tipo que la orden
// pide.
//
// Se fue de aquí un bloque de treinta líneas con tres reglas de precedencia —«mira si
// nombra un pool, luego si lo quiere la ranura, luego si vale como destino»— y con ellas la
// consulta al daemon para saber qué pools existían. Con URL y palabra separadas en el
// léxico, esa pregunta ya no hace falta.
//
// El orden de preferencia, único para todas las órdenes:
//   1. `--on <url>` (o `--from`), si se dio.
//   2. El destino posicional que haya reconocido la gramática.
//   3. El sitio actual, subido hasta lo que la orden pide.
bool prepara(Estado& e, const LineaAnalizada& linea, Peticion& p) {
    const Orden* orden = ordenPorNombre(linea.verbo);
    if (!orden) {
        return false;
    }
    p.l = &linea;
    p.orden = orden;

    std::string explicito = p.valor("on");
    if (explicito.empty()) {
        explicito = p.valor("from");
    }
    if (explicito.empty()) {
        explicito = linea.objetivo;
    }
    if (!explicito.empty()) {
        std::string err;
        if (!resuelve(e, explicito, p.objetivo, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
    } else {
        p.objetivo = subeHasta(e.actual, orden->objetivo);
    }

    // Las BANDERAS CORTAS, contra lo declarado. Es el mismo criterio que las largas, y su
    // ausencia era un agujero: `import apar -N` se tragaba la bandera sin protestar Y sin
    // pasarla a zpool, así que ni hacía lo pedido ni lo decía.
    for (const std::string& b : linea.banderas) {
        bool declarada = false;
        for (const Nativa& n : orden->nativas) {
            if (b == n.forma) {
                declarada = true;
            }
        }
        for (const Parametro& par : orden->params) {
            const std::string forma = T(par.forma.clave, par.forma.es);
            for (const std::string& trozo : B::split(forma, " ", true)) {
                if (trozo == b) {
                    declarada = true;
                }
            }
        }
        // `-y` es universal: confirma cualquier acción destructiva.
        if (!declarada && b != "-y") {
            std::fprintf(stderr, TC("t_bandera_no_admitida", "«%s» no es una bandera de %s\n"),
                         b.c_str(), orden->nombre);
            return false;
        }
    }

    // Las OPCIONES que la orden no declara son un error, no ruido que se ignora.
    //
    // Sin esto, `ls --daemon` dentro de una conexión se tragaba la opción y listaba los
    // pools como si nada: `ls` sí declara `--daemon`, pero solo hace algo en la raíz. Y una
    // opción mal escrita —`--daemn`— desaparecía sin más. Es la misma familia que los
    // argumentos ignorados, por la otra puerta.
    //
    // Las declaradas se leen del catálogo, que ya las tiene como datos: la línea de un
    // parámetro es «--delete» o «--name / --type / --os» o «--password-fd <n>», así que se
    // saca de ahí cada palabra que empiece por dos guiones.
    for (const auto& kv : linea.opciones) {
        static const std::set<std::string> universales{"on", "from"};
        if (universales.count(kv.first) > 0) {
            continue;
        }
        bool declarada = false;
        for (const Nativa& n : orden->nativas) {
            // Peladas de guiones, que es como se guardan. Comparando solo las largas, una
            // bandera corta CON valor —`trim -r 100M`— se declaraba y aun así se rechazaba.
            std::string f = n.forma;
            while (!f.empty() && f.front() == '-') {
                f.erase(f.begin());
            }
            if (f == kv.first) {
                declarada = true;
            }
        }
        for (const Parametro& par : orden->params) {
            const std::string forma = T(par.forma.clave, par.forma.es);
            for (const std::string& trozo : B::split(forma, " ", true)) {
                if (B::startsWith(trozo, "--") && trozo.substr(2) == kv.first) {
                    declarada = true;
                }
            }
        }
        if (!declarada) {
            // Con uno o dos guiones según su forma: las claves se guardan peladas, y
            // escribir siempre «--» delante producía cosas como «---r».
            const std::string comoSeEscribe = (kv.first.size() == 1 ? "-" : "--") + kv.first;
            std::fprintf(stderr, TC("t_opcion_no_admitida", "«%s» no es una opción de %s\n"),
                         comoSeEscribe.c_str(), orden->nombre);
            return false;
        }
    }

    if (orden->objetivo != Objetivo::Ninguno && !encaja(p.objetivo, orden->objetivo)) {
        // Si el destino vino ESCRITO, se dice qué es lo que no encaja; si es el sitio
        // actual, se dice qué falta. Son dos errores distintos para quien los lee.
        if (!explicito.empty()) {
            std::fprintf(stderr, TC("t_no_es_lo_pedido", "%s no es %s\n"), explicito.c_str(),
                         nombreDe(orden->objetivo).c_str());
        } else {
            std::fprintf(stderr, TC("t_hace_falta_obj", "hace falta %s (ahora: %s)\n"),
                         nombreDe(orden->objetivo).c_str(), textoDe(p.objetivo).c_str());
        }
        return false;
    }
    return true;
}


bool confirma(const Estado& e, const std::string& que) {
    if (e.asumirSi) {
        return true;
    }
    if (!hayTerminal()) {
        std::fprintf(stderr, TC("t_s_hace_fal_27dc69", "%s: hace falta confirmación y no hay terminal. Use -y si está seguro.\n"), que.c_str());
        return false;
    }
    std::string resp;
    std::string err;
    if (!preguntarPorTerminal(que + T("t_sufijo_sn", " [s/N]: "), resp, err)) {
        return false;
    }
    const std::string r = B::toLowerAscii(B::trim(resp));
    return r == "s" || r == "si" || r == "sí" || r == "y" || r == "yes";
}

// --- ls

// El listado de conexiones es EL MISMO que el de la orden suelta `connections list`. La
// tabla se construye en un solo sitio (session.cpp): tenerla duplicada hacía que la misma
// pregunta se contestara con columnas distintas según por dónde se preguntara.
// El listado de conexiones. Con `--daemon` se pregunta además a cada máquina qué versión
// de agente tiene, y se marca con `*` la que no sea la esperada por este cliente.
//
// **No va por omisión, y es a propósito.** `ls` en la raíz es hoy la única orden que
// responde al instante y SIN hablar con nadie: lee la configuración y ya. Es lo primero
// que uno teclea cuando algo no va, y justo entonces es cuando hay máquinas apagadas —cada
// una costaría su plazo de espera antes de rendirse—. Preguntando solo cuando se pide, la
// orden rápida sigue siendo rápida y la lenta se elige a sabiendas.
void listaConexiones(Estado& e, const Peticion& pet) {
    Tabla t = tablaDeConexiones(e.conns);
    if (!pet.tiene("--daemon")) {
        t.imprime(e.formato);
        return;
    }
    t.cabecerasTexto.push_back(T("t_cab_daemon", "DAEMON"));
    t.campos.push_back("daemon");
    t.tipos.push_back(Tipo::Cadena);
    const std::string esperada = ZFSMGR_AGENT_VERSION_STRING;
    for (std::size_t i = 0; i < t.filas.size() && i < e.conns.perfiles.size(); ++i) {
        const auto& p = e.conns.perfiles[i];
        const std::string id = p.id.empty() ? p.name : p.id;
        std::string version;
        // Una desconectada no se sondea: está apartada a propósito y preguntarle sería
        // saltarse esa marca, además de costar el plazo entero.
        if (!e.conns.desconectada(id)) {
            const auto ya = e.versionDaemon.find(id);
            if (ya != e.versionDaemon.end()) {
                version = ya->second;
            } else {
                ZfsmUrl u;
                u.kind = ZfsmKind::Connection;
                u.connection = id;
                std::string salida;
                if (agente(e, u, {"--health"}, salida, 8000)) {
                    version = clavesDe(salida)["VERSION"];
                }
                // Se recuerda durante la sesión, incluido el fallo: repetir `ls --daemon`
                // no vuelve a esperar por una máquina que ya no contestó.
                e.versionDaemon[id] = version;
            }
        }
        if (version.empty()) {
            version = "-";
        } else if (version != esperada) {
            version += " *";
        }
        t.filas[i].push_back(version);
    }
    t.imprime(e.formato);
}

// Los pools de una conexión, del JSON de `zpool list`.
bool listaPools(Estado& e, const ZfsmUrl& destino) {
    std::string out;
    if (!agente(e, destino, {"--dump-zpool-list"}, out)) {
        return false;
    }
    B::json::Value raiz;
    std::string err;
    if (!B::json::parse(out, raiz, &err)) {
        std::fprintf(stderr, TC("t_respuesta__74ad0d", "respuesta ilegible de zpool list: %s\n"), err.c_str());
        return false;
    }
    Tabla t;
    t.nombreJson = "pools";
    t.cabecerasTexto = {T("t_cab_nombre", "NOMBRE"),   T("t_cab_estado", "ESTADO"),
                        T("t_cab_tamano", "TAMAÑO"),   T("t_cab_libre", "LIBRE"),
                        T("t_cab_uso", "USO"),         T("t_cab_salud", "SALUD"),
                        T("t_cab_importado", "IMPORTADO")};
    t.campos = {"name", "state", "size", "free", "capacity", "health", "imported"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena,   Tipo::Cadena, Tipo::Cadena,
               Tipo::Cadena, Tipo::Cadena,   Tipo::Booleano};
    const auto prop = [](const B::json::Value& p, const char* k) {
        return p["properties"][k]["value"].toString();
    };
    std::set<std::string> yaEstan;
    for (const auto& kv : raiz["pools"].toObject()) {
        const auto& p = kv.second;
        const std::string nombre = p["name"].toString(kv.first);
        yaEstan.insert(nombre);
        t.filas.push_back({nombre, p["state"].toString(), prop(p, "size"), prop(p, "free"),
                           prop(p, "capacity"), prop(p, "health"), "true"});
    }

    // Los que están AHÍ pero sin importar. `zpool list` no los ve —solo enumera los
    // importados— y por eso un pool perfectamente visible en la interfaz no salía en `ls`:
    // no es que estuviera oculto, es que se preguntaba por otra cosa.
    //
    // La sonda es una orden aparte y puede fallar sin que eso invalide lo ya listado (falta
    // de permisos sobre los discos, un agente sin «Acceso total al disco» en macOS): si no
    // responde, se enseña lo importado y se sigue, en vez de no enseñar nada.
    std::string sonda;
    if (agente(e, destino, {"--dump-zpool-import-probe"}, sonda, 25000)) {
        for (const H::ImportablePoolInfo& imp : H::parseZpoolImportOutput(sonda)) {
            // El mismo conjunto sirve para dos cosas: no repetir uno ya importado, y no
            // repetirlo consigo mismo. La sonda ejecuta `zpool import` Y `zpool import -s`
            // y pega las dos salidas, así que un pool que aparece en las dos —lo normal—
            // llegaba aquí dos veces y salía dos veces en el listado.
            if (imp.pool.empty() || !yaEstan.insert(imp.pool).second) {
                continue;
            }
            // Sin tamaño ni uso: eso solo se sabe una vez importado, y rellenarlo con ceros
            // diría que el pool está vacío.
            t.filas.push_back({imp.pool, imp.state, "", "", "", imp.state, "false"});
        }
    }
    t.imprime(e.formato);
    return true;
}

// Los hijos y las instantáneas de un dataset. Sale de `--dump-zfs-list-all`, que es TSV con
// las columnas name,guid,used,compressratio,encryption,creation,referenced,mounted,
// mountpoint,canmount.
bool listaDataset(Estado& e, const ZfsmUrl& destino) {
    std::string out;
    if (!agente(e, destino, {"--dump-zfs-list-all", destino.dataset}, out)) {
        return false;
    }
    Tabla t;
    t.nombreJson = "entries";
    t.cabecerasTexto = {T("t_cab_nombre", "NOMBRE"), T("t_cab_tipo", "TIPO"),
                        T("t_cab_usado", "USADO"), T("t_cab_compr", "COMPR"),
                        T("t_cab_montado", "MONTADO"),
                        T("t_cab_punto_de_montaje", "PUNTO DE MONTAJE")};
    t.campos = {"name", "type", "used", "compressratio", "mounted", "mountpoint"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Bytes, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    const std::string prefijo = destino.dataset + "/";
    for (const std::string& linea : B::split(out, "\n", true)) {
        const std::vector<std::string> c = B::split(linea, "\t", false);
        if (c.size() < 10) {
            continue;
        }
        const std::string& nombre = c[0];
        if (nombre == destino.dataset) {
            continue;  // el propio dataset no es hijo suyo
        }
        const std::size_t arroba = nombre.find('@');
        const bool esSnap = arroba != std::string::npos;
        // Solo los hijos DIRECTOS y las instantáneas propias: `list-all` es recursivo, y
        // volcar el árbol entero convierte un `ls` en un listado de miles de líneas.
        if (esSnap) {
            if (nombre.substr(0, arroba) != destino.dataset) {
                continue;
            }
        } else {
            if (!B::startsWith(nombre, prefijo)
                || nombre.find('/', prefijo.size()) != std::string::npos) {
                continue;
            }
        }
        t.filas.push_back({esSnap ? "@" + nombre.substr(arroba + 1) : nombre.substr(prefijo.size()),
                           esSnap ? "snapshot" : "dataset", c[2], c[3], c[7], c[8]});
    }
    t.imprime(e.formato);
    return true;
}

// Las propiedades: `#properties`, y `#properties/<nombre>` para una sola.
bool listaPropiedades(Estado& e, const ZfsmUrl& destino) {
    const std::string objetivo = destino.zfsName();
    std::string out;
    if (!destino.detail.empty()) {
        if (!agente(e, destino, {"--dump-zfs-get-prop", destino.detail.front(), objetivo}, out)) {
            return false;
        }
        std::fprintf(stdout, "%s\n", B::trim(out).c_str());
        return true;
    }
    if (!agente(e, destino, {"--dump-zfs-get-all", objetivo}, out)) {
        return false;
    }
    B::json::Value raiz;
    std::string err;
    if (!B::json::parse(out, raiz, &err)) {
        std::fprintf(stderr, TC("t_respuesta__6a8a80", "respuesta ilegible de zfs get: %s\n"), err.c_str());
        return false;
    }
    Tabla t;
    t.nombreJson = "properties";
    t.cabecerasTexto = {T("t_cab_propiedad", "PROPIEDAD"), T("t_cab_valor", "VALOR"),
                        T("t_cab_origen", "ORIGEN")};
    t.campos = {"property", "value", "source"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    for (const auto& ds : raiz["datasets"].toObject()) {
        for (const auto& pr : ds.second["properties"].toObject()) {
            t.filas.push_back({pr.first, pr.second["value"].toString(),
                               B::toLowerAscii(pr.second["source"]["type"].toString())});
        }
    }
    t.imprime(e.formato);
    return true;
}

// Los permisos delegados: `#permissions`, o la orden `allow` sin argumentos.
//
// `zfs allow` NO tiene salida tabulada: escribe un bloque para leer, con secciones y
// entradas indentadas. Se analiza aquí para poder darlo en las tres formas, que es lo que
// permite que un guion compruebe quién tiene qué sin leer prosa.
//
//     ---- Permissions on fc16/work ----
//     Local+Descendent permissions:
//     \tuser linarese snapshot
bool listaPermisos(Estado& e, const ZfsmUrl& destino) {
    const std::string objetivo = destino.zfsName();
    if (objetivo.empty()) {
        std::fputs(TC("t_hace_falta_dc4ecf", "hace falta un dataset\n"), stderr);
        return false;
    }
    std::string out;
    if (!agente(e, destino, {"--dump-zfs-allow", objetivo}, out)) {
        return false;
    }
    Tabla t;
    t.nombreJson = "permissions";
    t.cabecerasTexto = {T("t_cab_alcance", "ALCANCE"), T("t_cab_clase", "CLASE"),
                        T("t_cab_quien", "QUIÉN"), T("t_cab_permisos", "PERMISOS")};
    t.campos = {"scope", "kind", "who", "permissions"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    std::string alcance;
    for (const std::string& cruda : B::split(out, "\n", true)) {
        if (B::startsWith(cruda, "----")) {
            continue;  // la cabecera con el nombre del dataset
        }
        // Las secciones no van indentadas y acaban en dos puntos.
        if (!cruda.empty() && cruda.front() != '\t' && cruda.front() != ' '
            && B::endsWith(B::trim(cruda), ":")) {
            alcance = B::trim(cruda);
            alcance.pop_back();
            continue;
        }
        const std::string linea = B::trim(cruda);
        if (linea.empty()) {
            continue;
        }
        // «user <quien> <permisos>», «group <quien> <permisos>», «everyone <permisos>»,
        // «@conjunto <permisos>».
        const std::vector<std::string> c = B::split(linea, " ", true);
        if (c.empty()) {
            continue;
        }
        const std::string clase = c.front();
        std::size_t iPerms = 1;
        std::string quien;
        if ((clase == "user" || clase == "group") && c.size() >= 2) {
            quien = c[1];
            iPerms = 2;
        } else if (clase == "everyone") {
            quien = "everyone";
        } else {
            quien = clase;  // un conjunto @nombre
        }
        std::vector<std::string> permisos(c.begin() + std::min(iPerms, c.size()), c.end());
        t.filas.push_back({alcance, clase, quien, B::join(permisos, ",")});
    }
    t.imprime(e.formato);
    return true;
}

// Delegar o retirar permisos. Un solo cambio por orden, con el verbo genérico: el verbo por
// lotes existe para cuando la interfaz aplica una tabla entera de golpe.
bool cmdPermisos(Estado& e, const LineaAnalizada& linea, bool conceder) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    const std::string objetivo = destino.zfsName();
    if (objetivo.empty()) {
        std::fprintf(stderr, TC("t_hace_falta_6afff6", "hace falta un dataset (ahora: %s)\n"), textoDe(destino).c_str());
        return false;
    }
    // Sin argumentos, `allow` LISTA. Es lo que hace `zfs allow` a secas, y quien viene de
    // ahí lo espera.
    if (conceder && pet.lista("texto").empty() && pet.valor("user").empty() && pet.valor("group").empty()
        && !pet.tiene("--everyone")) {
        ZfsmUrl conSeccion = destino;
        conSeccion.section = B::zfsmSection::kPermissions;
        return listaPermisos(e, conSeccion);
    }

    std::vector<std::string> argv{conceder ? "allow" : "unallow"};
    if (pet.tiene("-r")) {
        argv.push_back("-r");  // solo unallow: recursivo
    }
    if (pet.tiene("--local")) {
        argv.push_back("-l");
    }
    if (pet.tiene("--descend")) {
        argv.push_back("-d");
    }
    if (pet.tiene("--create")) {
        argv.push_back("-c");
    }
    int aQuien = 0;
    if (!pet.valor("user").empty()) {
        argv.push_back("-u");
        argv.push_back(pet.valor("user"));
        ++aQuien;
    }
    if (!pet.valor("group").empty()) {
        argv.push_back("-g");
        argv.push_back(pet.valor("group"));
        ++aQuien;
    }
    if (pet.tiene("--everyone")) {
        argv.push_back("-e");
        ++aQuien;
    }
    if (!pet.valor("set").empty()) {
        argv.push_back("-s");
        argv.push_back(pet.valor("set"));
        ++aQuien;
    }
    if (aQuien == 0) {
        std::fprintf(stderr,
                     TC("t_uso_s_user_a24007", "uso: %s --user <u> | --group <g> | --everyone | --set @<nombre>\n"
                     "         <permiso>[,<permiso>...] [--local] [--descend] [--create]%s\n"
                     "  Sin nada, «allow» lista los permisos delegados.\n"),
                     conceder ? "allow" : "unallow", conceder ? "" : " [-r]");
        return false;
    }
    if (aQuien > 1) {
        // zfs acepta uno solo, y pasarle dos da un error suyo que no dice cuál sobra.
        std::fputs(TC("t_elija_uno__6ccf67", "elija UNO: --user, --group, --everyone o --set\n"), stderr);
        return false;
    }
    // Los permisos. En `unallow` pueden omitirse: entonces se quitan TODOS los de ese
    // usuario, que es lo que hace zfs y conviene decir en la confirmación.
    if (!pet.lista("texto").empty()) {
        argv.push_back(B::join(pet.lista("texto"), ","));
    } else if (conceder) {
        std::fputs(TC("t_hace_falta_7ead59", "hace falta al menos un permiso\n"), stderr);
        return false;
    }
    argv.push_back(objetivo);

    if (!conceder
        && !confirma(e, pet.lista("texto").empty()
                            ? B::format(T("t_conf_unallow_todos",
                                          "Se van a quitar TODOS los permisos delegados de ese "
                                          "destinatario en %1. ¿Continuar?"),
                                        {objetivo})
                            : B::format(T("t_conf_unallow",
                                          "Se van a quitar los permisos %1 en %2. ¿Continuar?"),
                                        {B::join(pet.lista("texto"), ","), objetivo}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    if (!zfsGenerico(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, TC("t_s_en_s_35a806", "%s en %s\n"), conceder ? "permisos delegados" : "permisos retirados", objetivo.c_str());
    return true;
}

// El contenido: `#content[/ruta]`.
//
// Este camino NO es RPC tipado: se lista con `ls -lA` por el transporte de siempre, igual
// que hace el navegador de ficheros de la interfaz. El agente no tiene verbo para esto, y
// añadirlo es trabajo aparte.
bool listaContenido(Estado& e, const ZfsmUrl& destino) {
    std::string error;
    const auto* p = perfilVivoDe(e, destino, error);
    if (!p) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    // Dónde empieza la ruta. Y aquí Windows NO se parece a nada de lo demás.
    //
    // En Unix es el `mountpoint` del dataset y ya está. En Windows ese mismo `mountpoint`
    // dice «/winpool/sa» y ESA RUTA NO EXISTE: el pool se monta en una letra de unidad y
    // los descendientes heredan la del POOL, no la suya. `winpool/sa` vive en `Z:\sa`.
    // Comprobado contra OldLau: `Test-Path /winpool/sa` da False y `Test-Path Z:\sa` da
    // True. Preguntar `driveletter` al dataset tampoco vale — hay que preguntárselo al
    // pool, que es de donde sale la letra.
    std::string base;
    const bool esWindows = T::isWindowsConnection(*p);
    if (esWindows) {
        std::string letra;
        if (!agente(e, destino, {"--dump-zfs-get-prop", "driveletter", destino.pool}, letra)) {
            return false;
        }
        letra = B::trim(letra);
        if (letra.empty() || letra == "-" || letra == "none") {
            std::fprintf(stderr, TC("t_el_pool_s__020fd2", "el pool %s no tiene letra de unidad asignada\n"), destino.pool.c_str());
            return false;
        }
        base = letra;
        // Lo que cuelga del pool, con las barras de Windows. Si el dataset ES el pool, no
        // cuelga nada y la ruta es la raíz de la unidad.
        if (destino.dataset.size() > destino.pool.size()) {
            std::string rel = destino.dataset.substr(destino.pool.size() + 1);
            B::replaceAll(rel, "/", "\\");
            base += "\\" + rel;
        } else {
            base += "\\";
        }
        if (!destino.snapshot.empty()) {
            base += "\\.zfs\\snapshot\\" + destino.snapshot;
        }
        for (const std::string& d : destino.detail) {
            std::string t = d;
            B::replaceAll(t, "/", "\\");
            base += "\\" + t;
        }
    } else {
        std::string mp;
        if (!agente(e, destino, {"--dump-zfs-get-prop", "mountpoint", destino.zfsName()}, mp)) {
            return false;
        }
        base = B::trim(mp);
        if (base.empty() || base == "none" || base == "-") {
            std::fprintf(stderr, TC("t_s_no_est_m_7fb2be", "%s no está montado en ningún sitio\n"), destino.zfsName().c_str());
            return false;
        }
        // En una instantánea el contenido vive bajo el directorio oculto `.zfs`.
        if (!destino.snapshot.empty()) {
            base += "/.zfs/snapshot/" + destino.snapshot;
        }
        for (const std::string& d : destino.detail) {
            base += "/" + d;
        }
    }

    // La orden que lista. **SIN envolver en PowerShell aquí**: `runSsh` ya envuelve lo que
    // va a una conexión Windows, y hacerlo dos veces devolvía el script expandido como
    // texto en vez de ejecutarlo —las variables `$p` las resolvía el envoltorio de fuera y
    // llegaban vacías—. Es el mismo motivo por el que la rama Unix pasa `sh -lc` en crudo.
    std::string orden;
    if (esWindows) {
        // En Windows NO hay `sh`: el intérprete respondía «sh : The term 'sh' is not
        // recognized as the name of a cmdlet». Se usa Get-ChildItem con campos separados
        // por tabuladores, igual que el navegador de ficheros de la interfaz — y por el
        // mismo motivo: imitar `ls -l` obligaría a inventar permisos, propietario y grupo
        // que allí no significan lo mismo.
        std::string entrecomillada = base;
        B::replaceAll(entrecomillada, "'", "''");  // en PowerShell la comilla se dobla
        orden = "$ErrorActionPreference='Stop'; $p='" + entrecomillada + "'; "
                "if(-not (Test-Path -LiteralPath $p)){ Write-Error ('no existe: ' + $p); exit 3 }; "
                "if(-not (Test-Path -LiteralPath $p -PathType Container)){ "
                "  Write-Error ('no es un directorio: ' + $p); exit 2 }; "
                "Get-ChildItem -LiteralPath $p -Force | ForEach-Object { "
                "  $d = if($_.PSIsContainer){'d'}else{'-'}; "
                "  $z = if($_.PSIsContainer){0}else{$_.Length}; "
                "  ($d + \"`t\" + $z + \"`t\" + $_.LastWriteTime.ToString('yyyy-MM-dd HH:mm') "
                "     + \"`t\" + $_.Name) }";
    } else {
        const std::string guion =
            "p=" + B::shSingleQuote(base)
            + "; if [ -d \"$p\" ]; then ls -lA \"$p\" 2>&1; "
              "elif [ -e \"$p\" ]; then echo \"no es un directorio: $p\" >&2; exit 2; "
              "else echo \"no existe: $p\" >&2; exit 3; fi";
        orden = "sh -lc " + B::shSingleQuote(guion);
    }

    const B::ConnectionProfile perfil = conSudo(e, *p);
    std::string out;
    std::string err;
    int rc = -1;
    if (!T::runSsh(e.ses->transporte, perfil, H::withSudoCommand(perfil, orden),
                   20000, out, err, rc, {}, {}, {}, {}, /*allowAgentRpc=*/false,
                   /*echoOutputToLog=*/e.ses->verboso)
        || rc != 0) {
        // Con el CLIXML limpiado, un fallo de PowerShell puede quedarse SIN texto: se dice
        // al menos qué ruta y con qué código, que es lo que hace falta para entenderlo.
        const std::string detalle = B::trim(err).empty() ? B::trim(out) : B::trim(err);
        if (detalle.empty()) {
            std::fprintf(stderr, TC("t_no_se_pudo_cf3a73", "no se pudo listar %s (código %d)\n"), base.c_str(), rc);
        } else {
            std::fprintf(stderr, "%s\n", detalle.c_str());
        }
        e.ultimoRc = rc == 0 ? 1 : rc;
        return false;
    }
    std::fprintf(stdout, "%s", out.c_str());
    if (!out.empty() && out.back() != '\n') {
        std::fprintf(stdout, "\n");
    }
    return true;
}

bool cmdLs(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    const std::string sec = B::toLowerAscii(destino.section);
    if (sec == B::zfsmSection::kContent) {
        return listaContenido(e, destino);
    }
    if (sec == B::zfsmSection::kProperties) {
        return listaPropiedades(e, destino);
    }
    if (sec == B::zfsmSection::kPermissions) {
        return listaPermisos(e, destino);
    }
    if (!sec.empty()) {
        std::fprintf(stderr, TC("t_no_s_lista_57ee7d", "no sé listar la sección «%s»\n"), sec.c_str());
        return false;
    }
    // `--daemon` solo significa algo en la RAÍZ, que es donde se listan las máquinas. En
    // cualquier otro sitio se decía que sí y se listaban los pools como si nada: la opción
    // está declarada, así que la comprobación general la daba por buena. Aceptar algo y no
    // hacerlo es peor que rechazarlo.
    if (pet.tiene("daemon") && nodoDe(destino) != Nodo::Raiz) {
        std::fputs(TC("t_daemon_solo_raiz",
                      "«--daemon» solo vale en la raíz, que es donde se listan las máquinas\n"),
                   stderr);
        return false;
    }
    switch (nodoDe(destino)) {
        case Nodo::Raiz:
            listaConexiones(e, pet);
            return true;
        case Nodo::Conexion:
            return listaPools(e, destino);
        case Nodo::Dataset:
            return listaDataset(e, destino);
        case Nodo::Snapshot:
            // Una instantánea no tiene hijos; lo interesante es lo que hay dentro.
            std::fputs(TC("t_una_instan_e727ff", "una instantánea no tiene hijos: pruebe «ls #content» o «ls #properties»\n"), stderr);
            return false;
    }
    return false;
}

// Mueve el sitio actual, sin pasar por la línea. Lo usan las órdenes que tienen que
// reubicarse tras destruir algo: `destroy` sobre donde uno está deja el sitio apuntando a
// lo que ya no existe.
//
// Existe porque `cmdCd(e, {".."})` dejó de significar lo que parecía en cuanto `cmdCd` pasó
// a recibir la línea analizada: `".."` se convertía en el `bool vacia` de la estructura, así
// que la llamada no movía nada. El compilador solo lo dijo como aviso de estrechamiento, y
// se vio en el registro del cruce a Windows, no aquí.
void vaA(Estado& e, const std::string& destino) {
    ZfsmUrl u;
    std::string err;
    if (resuelve(e, destino, u, err)) {
        e.anterior = e.actual;
        e.hayAnterior = true;
        e.actual = u;
    }
}

bool cmdCd(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    std::string texto = linea.objetivo;
    if (texto.empty()) {
        texto = pet.valor("on").empty() ? pet.valor("from") : pet.valor("on");
    }
    if (texto.empty()) {
        texto = "/";
    }
    ZfsmUrl destino;
    std::string error;
    if (!resuelve(e, texto, destino, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    // Se COMPRUEBA que existe, como haría el `cd` de cualquier intérprete. Sin esto uno se
    // queda apuntando a algo que no está y la siguiente orden falla por un motivo que no
    // parece tener nada que ver con el `cd` que la precedió.
    //
    // Cuesta una ida y vuelta, pero por el túnel ya montado son un par de milisegundos, y
    // en la raíz y en las conexiones no hace falta preguntar a nadie.
    if (nodoDe(destino) == Nodo::Dataset || nodoDe(destino) == Nodo::Snapshot) {
        std::string errPerfil;
        const auto* p = perfilVivoDe(e, destino, errPerfil);
        if (!p) {
            std::fprintf(stderr, "%s\n", errPerfil.c_str());
            return false;
        }
        // Se llama al agente DIRECTAMENTE y no por `agente()`: aquí un rc distinto de cero
        // no es un fallo, es la respuesta —«no existe»— y confundir las dos cosas daría un
        // mensaje de error donde hace falta uno que diga qué pasa.
        std::string out;
        std::string err;
        int rc = -1;
        std::string motivo;
        if (!ejecutarAgente(*e.ses, *p, {"--dump-zfs-exists", destino.zfsName()}, out, err, rc,
                            &motivo, 20000)) {
            std::fprintf(stderr, TC("t_no_se_pudo_1253da", "no se pudo comprobar %s: %s\n"), destino.zfsName().c_str(), motivo.c_str());
            e.ultimoRc = 1;
            return false;
        }
        if (rc != 0 || B::contains(out, "EXISTS=no")) {
            std::fprintf(stderr, TC("t_no_existe__608f9e", "no existe: %s\n"), destino.zfsName().c_str());
            e.ultimoRc = 1;
            return false;
        }
    }
    e.anterior = e.actual;
    e.hayAnterior = true;
    e.actual = destino;
    return true;
}

// --- Acciones sobre datasets, todas por `--mutate-zfs-generic`, que recibe el argv de
// `zfs` en JSON y solo admite una lista cerrada de operaciones.
bool zfsGenerico(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv) {
    B::json::Value arr{B::json::Array{}};
    for (const std::string& a : argv) {
        arr.push(B::json::Value(a));
    }
    std::string out;
    return agente(e, destino, {"--mutate-zfs-generic", B::base64Encode(B::json::toCompact(arr))}, out);
}

bool exigeDataset(const ZfsmUrl& u) {
    if (nodoDe(u) != Nodo::Dataset) {
        std::fprintf(stderr, TC("t_hace_falta_fdcdb6", "hace falta estar en un dataset (ahora: %s)\n"), textoDe(u).c_str());
        return false;
    }
    return true;
}

// Crear una INSTANTÁNEA. No es una orden suelta: es la rama de `create` para cuando el
// nombre empieza por `@`.
//
// Antes tenía verbo propio, `snapshot`, heredado de que en la interfaz hay un botón
// distinto. Pero el modelo del intérprete no es el de la interfaz: aquí `create` significa
// «crea un nodo donde estás» —una conexión en la raíz, un pool en una conexión, un hijo en
// un dataset—, y una instantánea es otro nodo que se crea donde estás. El `@` ya es el
// marcador que la distingue en la propia URL, y `ls` dentro de un dataset ya lista hijos e
// instantáneas como hermanos.
//
// Lo que había antes era el peor de los dos mundos: `create @x` construía el nombre
// `tank/datos/@x` y lo mandaba a ZFS, que respondía «snapshot delimiter '@' is not
// expected here». El programa tenía delante todo lo necesario para saber qué se le pedía y
// devolvía un mensaje sobre delimitadores.
bool creaInstantanea(Estado& e, const Peticion& pet, const ZfsmUrl& destinoEntrada) {
    ZfsmUrl destino = destinoEntrada;
    // Estando en una instantánea, `create @hoy` se entiende sobre SU dataset: de una
    // instantánea no cuelga nada. Lo hacía el preámbulo, que subía hasta el dataset porque
    // la orden pedía uno; ahora `create` no pide nivel —vale en todos—, así que sube aquí.
    if (nodoDe(destino) == Nodo::Snapshot) {
        destino.snapshot.clear();
        destino.kind = ZfsmKind::Dataset;
    }
    if (!exigeDataset(destino)) {
        return false;
    }
    std::string nombre = pet.lista("texto").empty() ? std::string() : pet.uno("texto");
    if (!nombre.empty() && nombre.front() == '@') {
        nombre = nombre.substr(1);
    }
    if (nombre.empty()) {
        std::fputs(TC("t_hace_falta_638a1d", "hace falta un nombre: create @<nombre>\n"), stderr);
        return false;
    }
    const bool recursivo = pet.tiene("-r");
    // Una instantánea no pasa por un argv de `zfs`, sino por un verbo del daemon que solo
    // entiende «recursiva o no». Las demás banderas de `create` —las del pool y las del
    // dataset— no tienen dónde ir, así que se DICE. Callarlas sería justo el fallo que se
    // está persiguiendo: aceptar algo y no hacerle caso.
    for (const std::string& n : pet.nativas()) {
        std::fprintf(stderr, TC("t_bandera_no_instantanea", "«%s» no vale al crear una instantánea: "
                     "solo -r\n"), n.c_str());
        return false;
    }
    std::string out;
    if (!agente(e, destino,
                {"--mutate-zfs-snapshot", destino.dataset + "@" + nombre, recursivo ? "1" : "0"},
                out)) {
        return false;
    }
    std::fprintf(stderr, TC("t_creada_s_s_d15312", "creada %s@%s\n"), destino.dataset.c_str(), nombre.c_str());
    return true;
}

bool cmdDestroy(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    // En una CONEXIÓN, `destroy` la quita de la configuración. Es lo único que se puede
    // «destruir» ahí, y la pregunta lo deja claro: no se toca nada en la máquina.
    if (nodoDe(destino) == Nodo::Conexion) {
        const std::string id = idDe(e, destino);
        if (!confirma(e, B::format(T("t_conf_quitar_conn",
                                     "Se va a quitar la conexión %1 de la configuración. NO "
                                     "se toca nada en la máquina. ¿Continuar?"),
                                   {id}))) {
            std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
            return false;
        }
        std::string error;
        if (!borrarConexion(*e.ses, id, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
        recarga(e);
        std::fprintf(stderr, TC("t_quitada_la_2486c6", "quitada la conexión %s\n"), id.c_str());
        vaA(e, "/");
        return true;
    }
    // En un POOL, `destroy` es `zpool destroy`: `zfs destroy` sobre el dataset raíz de un
    // pool no funciona, así que es la única lectura posible.
    if (nodoDe(destino) == Nodo::Dataset && destino.isPoolRoot() && destino.snapshot.empty()) {
        if (!confirma(e, B::format(T("t_conf_destroy_pool",
                                     "Se va a DESTRUIR EL POOL %1 en %2, con todos sus "
                                     "datasets y todos sus datos. ¿Continuar?"),
                                   {destino.pool, destino.connection}))) {
            std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
            return false;
        }
        std::vector<std::string> argv{"destroy"};
        if (pet.tiene("-f")) {
            argv.push_back("-f");
        }
        argv.push_back(destino.pool);
        if (!zpoolGenerico(e, destino, argv)) {
            return false;
        }
        std::fprintf(stderr, TC("t_destruido__a157ec", "destruido el pool %s\n"), destino.pool.c_str());
        vaA(e, "..");
        return true;
    }
    const std::string objetivo = destino.zfsName();
    if (objetivo.empty()) {
        std::fprintf(stderr, TC("t_no_hay_nad_da1f25", "no hay nada que borrar en %s\n"), textoDe(destino).c_str());
        return false;
    }
    const bool recursivo = pet.tiene("-r") || pet.tiene("-R");
    // La pregunta dice QUÉ se va a borrar y con qué alcance: «¿seguro?» a secas es lo que
    // hace que se conteste que sí sin leer.
    if (!confirma(e, B::format(T("t_conf_destroy_ds",
                                 "Se va a DESTRUIR %1%2 en %3. ¿Continuar?"),
                               {objetivo,
                                recursivo ? T("t_conf_y_desc", " y todos sus descendientes")
                                          : std::string(),
                                destino.connection}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    std::string out;
    if (!agente(e, destino,
                {"--mutate-zfs-destroy", objetivo, pet.tiene("-f") ? "1" : "0",
                 pet.tiene("-R") ? "R" : (pet.tiene("-r") ? "r" : "")},
                out)) {
        return false;
    }
    std::fprintf(stderr, TC("t_destruido__e8a7d6", "destruido %s\n"), objetivo.c_str());
    // Si se ha borrado justo donde estábamos, se sube: quedarse apuntando a algo que ya no
    // existe hace que la siguiente orden falle sin que se entienda por qué.
    if (destino.zfsName() == e.actual.zfsName() && destino.connection == e.actual.connection) {
        vaA(e, "..");
    }
    return true;
}

bool cmdRollback(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;

    if (!confirma(e, B::format(T("t_conf_rollback",
                                 "Se va a volver %1 al estado de @%2, DESCARTANDO todo lo "
                                 "posterior. ¿Continuar?"),
                               {destino.dataset, destino.snapshot}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    std::string out;
    if (!agente(e, destino,
                {"--mutate-zfs-rollback", destino.zfsName(), pet.tiene("-f") ? "1" : "0",
                 pet.tiene("-R") ? "R" : (pet.tiene("-r") ? "r" : "")},
                out)) {
        return false;
    }
    std::fprintf(stderr, "%s vuelto a @%s\n", destino.dataset.c_str(), destino.snapshot.c_str());
    return true;
}

bool cmdClone(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    if (pet.lista("texto").empty()) {
        std::fputs(TC("t_uso_clone__ea5ed7", "uso: clone <nuevo-dataset> [--from <@instantánea>]\n"), stderr);
        return false;
    }
    const ZfsmUrl& origen = pet.objetivo;
    if (origen.snapshot.empty()) {
        std::fprintf(stderr, TC("t_el_origen__604ce4", "el origen tiene que ser una instantánea (ahora: %s)\n"), textoDe(origen).c_str());
        return false;
    }
    // El nombre se toma como hijo del sitio actual salvo que ya venga con ruta, igual que en
    // `create`. Sin esto había que escribir el nombre ZFS COMPLETO —`clone clonado`
    // respondía «missing dataset name», que es del propio zfs y no dice qué falta— y la
    // ayuda anunciaba `clone <nuevo>` como si un nombre suelto valiera. Venía de antes.
    const std::string nombre = pet.uno("texto");
    const std::string nuevo =
        nombre.find('/') == std::string::npos ? origen.dataset + "/" + nombre : nombre;
    std::string out;
    if (!agente(e, origen, {"--mutate-zfs-clone", origen.zfsName(), nuevo}, out)) {
        return false;
    }
    std::fprintf(stderr, "clonado %s -> %s\n", origen.zfsName().c_str(), nuevo.c_str());
    return true;
}

// Alta de una conexión. Es `create` estando en la RAÍZ, por la misma regla que hace que
// `create` en un dataset cree un hijo: se crea un nodo donde uno está.
//
// Los campos se piden por el terminal si no vienen por opciones, y **la contraseña NUNCA
// por argumento**: iría en `argv` y se vería en `ps` para cualquier usuario de la máquina.
// O se teclea, o entra por un descriptor con --password-fd.
bool cmdCrearConexion(Estado& e, const Peticion& pet) {
    if (pet.lista("texto").empty()) {
        std::fputs(TC("t_uso_create_ac2703", "uso: create <identificador> [--name <n>] [--type LOCAL|SSH] [--os <so>]\n"
                     "            [--host <h>] [--port <p>] [--user <u>] [--key <ruta>]\n"
                     "            [--sudo] [--password-fd <n>]\n"
                     "  Da de alta una conexión. Lo que no se dé por opciones se pregunta.\n"), stderr);
        return false;
    }
    B::ConnectionProfile p;
    p.id = B::trim(pet.uno("texto"));
    if (buscarConexion(e.conns, p.id) != nullptr) {
        std::fprintf(stderr, TC("t_ya_existe__484f91", "ya existe una conexión «%s»\n"), p.id.c_str());
        return false;
    }
    const bool interactivo = hayTerminal();

    p.name = pet.valor("name");
    p.connType = B::toUpperAscii(pet.valor("type"));
    p.osType = pet.valor("os");
    p.host = pet.valor("host");
    p.username = pet.valor("user");
    p.keyPath = pet.valor("key");
    p.useSudo = pet.tiene("--sudo");
    const std::string puertoTexto = pet.valor("port");

    if (interactivo) {
        if (p.name.empty() && !pide("Nombre visible", p.id, p.name)) return false;
        if (p.connType.empty() && !pide("Tipo (LOCAL/SSH)", "SSH", p.connType)) return false;
        p.connType = B::toUpperAscii(p.connType);
        if (p.osType.empty() && !pide("Sistema operativo", "Linux", p.osType)) return false;
    }
    if (p.name.empty()) p.name = p.id;
    if (p.connType.empty()) p.connType = "SSH";
    if (p.osType.empty()) p.osType = "Linux";

    const bool local = B::toLowerAscii(p.connType) == "local";
    if (!local) {
        if (p.host.empty() && interactivo && !pide("Host", "", p.host)) return false;
        if (p.host.empty()) {
            std::fputs(TC("t_una_conexi_86be2f", "una conexión SSH necesita un host\n"), stderr);
            return false;
        }
        if (p.username.empty() && interactivo && !pide(TC("t_p_usuario", "Usuario"), "", p.username)) return false;
        if (p.keyPath.empty() && interactivo && !pide(TC("t_p_ruta_clave", "Ruta de clave SSH (vacío = contraseña)"), "",
                                                      p.keyPath)) {
            return false;
        }
    }
    p.port = puertoTexto.empty() ? 0 : std::atoi(puertoTexto.c_str());

    // El sudo se pregunta ANTES que la contraseña, porque es lo que decide si hace falta.
    //
    // Estaba después, y con una clave SSH eso dejaba la conexión sin salida: `quiereClave`
    // miraba `useSudo`, que todavía era falso porque la respuesta no había llegado, así que
    // no se pedía contraseña; y acto seguido se contestaba que sí usa sudo. La conexión
    // quedaba con sudo y sin contraseña que darle, sin que nada lo dijera. Es justo el caso
    // de una máquina a la que se entra por clave y que necesita elevar.
    if (!pet.tiene("--sudo") && interactivo) {
        std::string resp;
        if (!pide(TC("t_p_sudo_n", "¿Usa sudo? (s/N)"), "n", resp)) return false;
        const std::string r = B::toLowerAscii(resp);
        p.useSudo = (r == "s" || r == "si" || r == "sí" || r == "y" || r == "yes");
    }

    // La contraseña. Por descriptor si se dio, y si no por el terminal con el eco apagado.
    // Solo hace falta si no hay clave SSH, o si la máquina va a necesitar sudo.
    //
    // **Se comprueba ANTES si hay dónde cifrarla.** Guardar una contraseña de acceso en
    // claro no se hace, así que sin contraseña maestra la creación fracasaría — y hacerla
    // teclear para tirarla después es la peor forma de contarlo. Si hay terminal se pide la
    // maestra en ese momento; si no, se avisa y se crea sin contraseña.
    const std::string fdTexto = pet.valor("password-fd");
    const bool quiereClave = !fdTexto.empty() || (p.keyPath.empty() || p.useSudo || local);
    std::string err;
    if (quiereClave && e.ses->maestra.empty()) {
        if (interactivo) {
            std::fputs(TC("t_para_guard_a024db", "Para guardar la contraseña de una conexión hace falta una "
                         "contraseña maestra:\n"
                         "es con la que se cifra en config.json, y sin ella no se guarda en "
                         "claro.\n"), stderr);
            std::string maestra;
            if (!preguntarSecretoPorTerminal(T("t_p_maestra", "Contraseña maestra: "), maestra, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return false;
            }
            e.ses->maestra = maestra;
        } else {
            std::fputs(TC("t_aviso_sin__287990", "aviso: sin contraseña maestra (--password-fd) la conexión se crea "
                         "SIN contraseña\n"), stderr);
        }
    }
    if (!fdTexto.empty()) {
        if (!leerSecretoDeDescriptor(std::atoi(fdTexto.c_str()), p.password, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
    } else if (interactivo && quiereClave && !e.ses->maestra.empty()) {
        std::string clave;
        if (!preguntarSecretoPorTerminal(T("t_p_pass_vacio", "Contraseña (vacío = ninguna): "), clave, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
        p.password = clave;
    }
    std::string error;
    if (!guardarConexion(*e.ses, p, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        // La contraseña no se queda en memoria si no se llegó a guardar.
        for (char& c : p.password) { c = 0; }
        return false;
    }
    for (char& c : p.password) { c = 0; }
    // Se recargan: a partir de ahora se puede navegar a ella.
    recarga(e);
    std::fprintf(stderr, TC("t_creada_la__d038ed", "creada la conexión %s (%s)\n"), p.id.c_str(), local ? "local" : (p.username + "@" + p.host).c_str());
    return true;
}

bool cmdEditarConexion(Estado& e, const Peticion& pet, const ZfsmUrl& destino) {
    const std::string id = idDe(e, destino);
    const auto* actual = buscarConexion(e.conns, id);
    if (!actual) {
        std::fprintf(stderr, TC("t_hace_falta_2bed6d", "hace falta estar en una conexión (ahora: %s)\n"), textoDe(destino).c_str());
        return false;
    }
    B::ConnectionProfile p = *actual;
    const bool interactivo = hayTerminal();
    const auto toma = [&](const char* clave, std::string& campo, const char* etiqueta) {
        const std::string v = pet.valor(clave);
        if (!v.empty()) {
            campo = v;
            return true;
        }
        if (!interactivo) {
            return true;
        }
        std::string leido;
        if (!pide(etiqueta, campo, leido)) {
            return false;
        }
        campo = leido;
        return true;
    };
    if (!toma("name", p.name, "Nombre visible")) return false;
    if (!toma("type", p.connType, "Tipo (LOCAL/SSH)")) return false;
    p.connType = B::toUpperAscii(p.connType);
    if (!toma("os", p.osType, "Sistema operativo")) return false;
    if (B::toLowerAscii(p.connType) != "local") {
        if (!toma("host", p.host, "Host")) return false;
        if (!toma("user", p.username, "Usuario")) return false;
        if (!toma("key", p.keyPath, "Ruta de clave SSH")) return false;
        std::string puerto = std::to_string(p.port);
        if (!toma("port", puerto, "Puerto")) return false;
        p.port = std::atoi(puerto.c_str());
    }
    if (pet.tiene("--sudo")) {
        p.useSudo = true;
    } else if (pet.tiene("--no-sudo")) {
        p.useSudo = false;
    } else if (interactivo) {
        std::string resp;
        if (!pide(TC("t_p_sudo", "¿Usa sudo? (s/n)"), p.useSudo ? "s" : "n", resp)) return false;
        const std::string r = B::toLowerAscii(resp);
        p.useSudo = (r == "s" || r == "si" || r == "sí" || r == "y" || r == "yes");
    }

    // La contraseña solo se toca si se pide expresamente: en una edición, dejarla en blanco
    // tiene que CONSERVARLA, no borrarla. Ya viene descifrada del perfil cargado, y
    // guardarConexion la vuelve a cifrar.
    const std::string fdTexto = pet.valor("password-fd");
    if (!fdTexto.empty()) {
        std::string err;
        if (!leerSecretoDeDescriptor(std::atoi(fdTexto.c_str()), p.password, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
    } else if (pet.tiene("--password") && interactivo) {
        std::string err;
        std::string clave;
        if (!preguntarSecretoPorTerminal(T("t_p_pass_nueva", "Contraseña nueva: "), clave, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
        p.password = clave;
    }

    std::string error;
    if (!guardarConexion(*e.ses, p, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        for (char& c : p.password) { c = 0; }
        return false;
    }
    for (char& c : p.password) { c = 0; }
    recarga(e);
    std::fprintf(stderr, TC("t_actualizad_41b05c", "actualizada la conexión %s\n"), id.c_str());
    return true;
}

bool cmdEdit(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    return cmdEditarConexion(e, pet, pet.objetivo);
}

bool cmdCreate(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    // El mismo verbo en los tres niveles, porque es la misma idea —crear un nodo donde uno
    // está—: en la RAÍZ una conexión, en una CONEXIÓN un pool, en un DATASET un hijo.
    if (nodoDe(destino) == Nodo::Raiz) {
        return cmdCrearConexion(e, pet);
    }
    if (nodoDe(destino) == Nodo::Conexion) {
        return cmdCrearPool(e, pet, destino);
    }
    if (pet.lista("texto").empty()) {
        std::fputs(TC("t_uso_create_c8fa17", "uso: create <nombre> [prop=valor...]\n"), stderr);
        return false;
    }
    // Un nombre que empieza por `@` nombra una INSTANTÁNEA, no un hijo. Se decide por el
    // marcador y no por una opción, porque `@` es lo que ya distingue una instantánea en la
    // URL: `zfsm://local/tank/datos@ayer`.
    if (pet.uno("texto").rfind('@', 0) == 0) {
        return creaInstantanea(e, pet, destino);
    }
    if (!exigeDataset(destino)) {
        return false;
    }
    std::vector<std::string> argv{"create"};
    for (const std::string& n : pet.nativas()) {
        argv.push_back(n);
    }
    for (std::size_t i = 1; i < pet.lista("texto").size(); ++i) {
        argv.push_back("-o");
        argv.push_back(pet.lista("texto")[i]);
    }
    // El nombre se toma como HIJO del sitio actual salvo que ya venga con ruta: escribir
    // `create datos` estando en `tank/casa` debe crear `tank/casa/datos`.
    const std::string hijo = pet.uno("texto");
    argv.push_back(hijo.find('/') == std::string::npos ? destino.dataset + "/" + hijo : hijo);
    if (!zfsGenerico(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, TC("t_creado_s_ea96d0", "creado %s\n"), argv.back().c_str());
    return true;
}

bool cmdRename(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    if (pet.lista("texto").empty()) {
        std::fputs(TC("t_uso_rename_c4838f", "uso: rename <nuevo-nombre>\n"), stderr);
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (!exigeDataset(destino)) {
        return false;
    }
    const std::string nuevo = pet.uno("texto");
    const std::size_t barra = destino.dataset.rfind('/');
    const std::string completo =
        nuevo.find('/') == std::string::npos && barra != std::string::npos
            ? destino.dataset.substr(0, barra + 1) + nuevo
            : nuevo;
    if (!zfsGenerico(e, destino, {"rename", destino.dataset, completo})) {
        return false;
    }
    std::fprintf(stderr, "renombrado %s -> %s\n", destino.dataset.c_str(), completo.c_str());
    return true;
}

bool cmdMontaje(Estado& e, const LineaAnalizada& linea, bool montar) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    std::vector<std::string> argv{montar ? "mount" : "unmount"};
    if (pet.tiene("-f")) {
        argv.push_back("-f");
    }
    argv.push_back(destino.dataset);
    if (!zfsGenerico(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, "%s %s\n", montar ? "montado" : "desmontado", destino.dataset.c_str());
    return true;
}

bool cmdPromote(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (!zfsGenerico(e, destino, {"promote", destino.dataset})) {
        return false;
    }
    std::fprintf(stderr, "promovido %s\n", destino.dataset.c_str());
    return true;
}

bool cmdSet(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    const std::string objetivo = destino.zfsName();
    std::vector<std::string> argv{"set"};
    // La firma ya ha exigido que sean `prop=valor` y que haya al menos una: la ranura es
    // de tipo Propiedad y de cardinalidad UnaOMas.
    for (const auto& asig : pet.lista("props")) {
        argv.push_back(asig);
    }
    argv.push_back(objetivo);
    if (!zfsGenerico(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, "aplicadas %zu propiedades a %s\n", pet.lista("props").size(),
                 objetivo.c_str());
    return true;
}

bool cmdGet(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    ZfsmUrl destino = pet.objetivo;
    destino.section = B::zfsmSection::kProperties;
    destino.detail.clear();
    if (!pet.uno("propiedad").empty()) {
        destino.detail = {pet.uno("propiedad")};
    }
    return listaPropiedades(e, destino);
}

bool cmdClaves(Estado& e, const LineaAnalizada& linea, const char* op) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (std::string(op) == "load-key") {
        // La frase de paso NUNCA por argumento: iría en `argv` y se vería en `ps` en las
        // dos máquinas. El verbo dedicado la lleva en base64 dentro de la petición RPC.
        std::string frase;
        std::string err;
        if (!preguntarSecretoPorTerminal("Frase de paso: ", frase, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
        std::string out;
        const bool ok = agente(e, destino,
                               {"--mutate-zfs-load-key", B::base64Encode(destino.dataset),
                                B::base64Encode(frase)},
                               out);
        for (char& c : frase) {
            c = '\0';
        }
        if (!ok) {
            return false;
        }
        std::fprintf(stderr, TC("t_clave_carg_09d2f4", "clave cargada en %s\n"), destino.dataset.c_str());
        return true;
    }
    if (!zfsGenerico(e, destino, {op, destino.dataset})) {
        return false;
    }
    std::fprintf(stderr, TC("t_s_en_s_35a806", "%s en %s\n"), op, destino.dataset.c_str());
    return true;
}

// --- Las cuatro acciones de Avanzado.

bool cmdBreakdown(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (pet.lista("texto").size() < 2 || pet.lista("texto").size() % 2 != 0) {
        std::fputs(TC("t_uso_breakd_14be27", "uso: breakdown <directorio> <nombre-hijo> [<directorio> <nombre-hijo>...]\n"
                     "  Convierte cada directorio del dataset en un dataset hijo.\n"), stderr);
        return false;
    }
    if (!confirma(e, B::format(T("t_conf_breakdown",
                                 "Se van a convertir %1 directorios de %2 en datasets hijos. "
                                 "¿Continuar?"),
                               {std::to_string(pet.lista("texto").size() / 2), destino.dataset}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    std::vector<std::string> argv{"--mutate-advanced-breakdown", destino.dataset};
    for (const auto& x : pet.lista("texto")) {
        argv.push_back(x);
    }
    return lanzaOEspera(e, pet, destino, argv);
}

bool cmdAssemble(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (pet.lista("texto").empty()) {
        std::fputs(TC("t_uso_assemb_8aa75c", "uso: assemble <hijo> [<hijo>...]\n"
                     "  Deshace lo de breakdown: devuelve cada dataset hijo a directorio.\n"), stderr);
        return false;
    }
    if (!confirma(e, B::format(T("t_conf_assemble",
                                 "Se van a reintegrar %1 datasets hijos en %2 como "
                                 "directorios. ¿Continuar?"),
                               {std::to_string(pet.lista("texto").size()), destino.dataset}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    // Los hijos van con NOMBRE COMPLETO. El agente los comprueba con `zfs list <hijo>`, así
    // que un nombre relativo no existe para él y la operación se salda con «ya absorbido»
    // y rc=0: parece que ha funcionado y no ha hecho nada.
    std::vector<std::string> argv{"--mutate-advanced-assemble", destino.dataset};
    for (const auto& x : pet.lista("texto")) {
        argv.push_back(x.find('/') == std::string::npos ? destino.dataset + "/" + x : x);
    }
    return lanzaOEspera(e, pet, destino, argv);
}

bool cmdToDir(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (pet.lista("ruta").empty()) {
        std::fputs(TC("t_uso_todir__d626ba", "uso: todir <directorio-destino> [--delete-source]\n"
                     "  Vuelca el contenido del dataset a un directorio.\n"), stderr);
        return false;
    }
    const bool borraOrigen = pet.tiene("--delete-source");
    if (!confirma(e, B::format(T("t_conf_todir", "Se va a volcar %1 a %2%3. ¿Continuar?"),
                               {destino.dataset, pet.uno("ruta"),
                                borraOrigen ? T("t_conf_y_destruir_origen",
                                                " y DESTRUIR el dataset de origen")
                                            : std::string()}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    const std::vector<std::string> argvTodir{"--mutate-advanced-todir", destino.dataset,
                                             pet.uno("ruta"), borraOrigen ? "1" : "0"};
    return lanzaOEspera(e, pet, destino, argvTodir);
}

// `fromdir` NO es la inversa de `todir`, aunque el nombre lo sugiera.
//
// Es «Desde Dir»: crea el contenido de un dataset A PARTIR de uno o varios directorios,
// que pueden estar en OTRA máquina. Por eso no puede ser un RPC como sus tres hermanas: el
// verbo del agente lee un tar por la entrada estándar y el canal RPC no tiene stdin —lo
// dice el propio daemon en su comentario—. Se arma la misma tubería que la interfaz:
//
//     <ssh origen> tar -c ...  |  <ssh destino> zfsmgr-agent --mutate-advanced-fromdir
//
// Se ejecuta EN ESTA máquina, que es la que tiene las credenciales de las dos puntas.
bool cmdFromDir(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    if (pet.lista("ruta").empty()) {
        std::fputs(TC("t_uso_fromdi_d4b854", "uso: fromdir <directorio-origen> [--from <url-origen>] [--subdir <rel>]\n"
                     "  Vuelca un directorio dentro del dataset actual. El origen puede estar\n"
                     "  en otra máquina: --from acepta cualquier URL zfsm://.\n"), stderr);
        return false;
    }
    // El DESTINO es donde estamos. `--from` nombra el origen, que aquí es otra máquina y no
    // otro sitio: es la única orden en la que las dos opciones no son sinónimas.
    ZfsmUrl destino = e.actual;
    if (!pet.valor("on").empty()) {
        std::string error;
        if (!resuelve(e, pet.valor("on"), destino, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
    }
    if (!exigeDataset(destino)) {
        return false;
    }
    std::string errDst;
    const auto* pDst = perfilDe(e, destino, errDst);
    if (!pDst) {
        std::fprintf(stderr, "%s\n", errDst.c_str());
        return false;
    }

    const B::ConnectionProfile* pSrc = pDst;
    ZfsmUrl origen = destino;
    if (!pet.valor("from").empty()) {
        std::string error;
        if (!resuelve(e, pet.valor("from"), origen, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
        pSrc = perfilDe(e, origen, error);
        if (!pSrc) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
    }

    const std::string dir = pet.uno("ruta");
    const std::string rel = pet.valor("subdir");
    if (!confirma(e, B::format(T("t_conf_fromdir",
                                 "Se va a volcar %1 de %2 dentro de %3 en %4. ¿Continuar?"),
                               {dir, origen.connection, destino.dataset,
                                destino.connection}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }

    const B::ConnectionProfile src = conSudo(e, *pSrc);
    const B::ConnectionProfile dst = conSudo(e, *pDst);

    // Sin compresión: elegirla exige saber qué hay instalado en LAS DOS puntas, que es una
    // sonda por máquina. La interfaz la hace porque ya tiene esos datos cacheados.
    const std::string tarOrigen =
        H::buildTarSourceCommand(T::isWindowsConnection(src), dir, H::StreamCodec::None);
    const std::string recibe = B::daemonpayload::unixBinPath() + " --mutate-advanced-fromdir "
                               + B::shSingleQuote(destino.dataset) + " " + B::shSingleQuote(rel);

    // `sshExecFromLocal`: para una conexión LOCAL la orden se queda tal cual; para una SSH
    // se envuelve en `ssh host '...'`. Así la misma tubería vale para local->local,
    // local->remoto, remoto->local y remoto->remoto.
    const auto porSsh = [](const B::ConnectionProfile& p, const std::string& cmd) {
        if (T::isLocalConnection(p)) {
            return cmd;
        }
        return H::sshBaseCommand(p) + " " + B::shSingleQuote(H::sshUserHost(p)) + " "
               + B::shSingleQuote(T::wrapRemoteCommand(p, cmd));
    };
    const std::string tuberia = porSsh(src, H::withSudoCommand(src, tarOrigen)) + " | "
                                + porSsh(dst, H::withSudoStreamInputCommand(dst, recibe));

    // Se ejecuta EN ESTA máquina: es la que tiene las credenciales de las dos puntas.
    B::ConnectionProfile aqui;
    aqui.id = "local";
    aqui.connType = "LOCAL";
    aqui.osType = "Linux";
    std::string out;
    std::string err;
    int rc = -1;
    if (!T::runSsh(e.ses->transporte, aqui, tuberia, 0, out, err, rc,
                   [](const std::string& l) { std::fprintf(stdout, "%s\n", l.c_str()); },
                   [](const std::string& l) { std::fprintf(stderr, "%s\n", l.c_str()); }, {}, {},
                   /*allowAgentRpc=*/false, /*echoOutputToLog=*/e.ses->verboso)
        || rc != 0) {
        std::fprintf(stderr, TC("t_fromdir_fa_67991a", "fromdir falló (código %d)\n"), rc);
        e.ultimoRc = rc == 0 ? 1 : rc;
        return false;
    }
    return true;
}

// --- Transferencias entre máquinas.
//
// El protocolo tiene DOS EXTREMOS y no es una tubería:
//
//   1. En el DESTINO, `--zfs-recv-listen <dataset> 1` abre un puerto y devuelve
//      `PORT=` y `TOKEN=`.
//   2. En el ORIGEN, `--zfs-send-to-peer-async <snap> <host> <puerto> <testigo> …` conecta
//      con ese puerto y devuelve un `JOB_ID=`.
//   3. El avance se consulta con `job <id>` en la máquina de ORIGEN.
//
// Que sea un trabajo y no una espera es lo que permite mandar terabytes y cerrar la
// sesión. Con `--wait` se espera aquí, sondeando.

// Copiar una instantánea a otro dataset, en esta máquina o en otra.
//
// Con `--base` es INCREMENTAL: solo viaja lo que cambió desde esa instantánea, que es lo
// que la interfaz llama «Nivelar». Sin ella va el flujo completo.
bool cmdCopy(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    // Las banderas de `zfs send` se pueden escribir sueltas —`copy /otra/x -w -L`— o en
    // bloque con `--flags "-w -L"`, que es como se hacía. Las dos acaban en la misma cadena
    // y las dos se comprueban contra la misma lista.
    //
    // `--flags` era texto libre hasta el argv de un `zfs send` con privilegios: lo que se
    // escribiera ahí se troceaba por espacios y se metía tal cual, así que un
    // `--flags "tank/otro@ayer"` sacaba por el socket un dataset que no era el pedido. El
    // daemon lo rechaza igualmente; aquí se comprueba ANTES de preguntar, porque hacer
    // confirmar una copia para luego decir que la bandera no vale es preguntar en balde.
    std::string banderasSend;
    for (const std::string& n : pet.nativas()) {
        banderasSend += (banderasSend.empty() ? "" : " ") + n;
    }
    if (!pet.valor("flags").empty()) {
        std::string mala;
        if (!B::zfsprops::banderasDeSendValidas(pet.valor("flags"), mala)) {
            std::fprintf(stderr, TC("t_bandera_send_no_admitida",
                         "«%s» no es una bandera de zfs send\n"), mala.c_str());
            return false;
        }
        banderasSend += (banderasSend.empty() ? "" : " ") + pet.valor("flags");
    }
    if (pet.lista("destino").empty() && pet.valor("to").empty()) {
        std::fputs(TC("t_uso_copy_d_f6efbc", "uso: copy <destino> [--from <@instantánea>] [--base <@instantánea>]\n"
                     "          [--flags <banderas de zfs send>] [--wait]\n"
                     "  El destino es una URL: puede estar en OTRA máquina.\n"
                     "  Sin --from se usa el sitio actual como origen.\n"
                     "  Con --base solo viaja lo que cambió desde ahí (lo que la interfaz\n"
                     "  llama «Nivelar»).\n"), stderr);
        return false;
    }
    const ZfsmUrl& origen = pet.objetivo;
    if (origen.snapshot.empty()) {
        std::fprintf(stderr, TC("t_el_origen__886ec5", "el origen tiene que ser una INSTANTÁNEA (ahora: %s)\n"), textoDe(origen).c_str());
        return false;
    }
    ZfsmUrl destino;
    std::string error;
    if (!resuelve(e, pet.lista("destino").empty() ? pet.valor("to") : pet.uno("destino"), destino, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    if (destino.dataset.empty()) {
        std::fputs(TC("t_el_destino_e077ee", "el destino tiene que ser un dataset\n"), stderr);
        return false;
    }
    const auto* pOrigen = perfilVivoDe(e, origen, error);
    if (!pOrigen) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    const auto* pDestino = perfilVivoDe(e, destino, error);
    if (!pDestino) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    // Ninguno de los dos extremos puede ser Windows: el flujo por socket no está portado
    // allí. Decirlo AQUÍ evita un fallo a mitad de transferencia que no se entiende.
    if (T::isWindowsConnection(*pOrigen) || T::isWindowsConnection(*pDestino)) {
        std::fputs(TC("t_la_transfe_6f9799", "la transferencia por socket no está disponible en Windows.\n"
                     "Para llevar datos a o desde una máquina Windows, use «todir» y "
                     "«fromdir».\n"), stderr);
        return false;
    }
    const bool mismaMaquina = B::toLowerAscii(origen.connection) == B::toLowerAscii(destino.connection);

    std::string base = pet.valor("base");
    if (!base.empty() && base.front() == '@') {
        base = origen.dataset + base;
    }
    if (!confirma(e, B::format(T("t_conf_copy",
                                 "Se va a %1 %2 de %3 a %4 en %5.\nSi el destino existe y "
                                 "difiere, `zfs recv -F` lo SOBRESCRIBE. ¿Continuar?"),
                               {base.empty() ? T("t_copiar", "copiar")
                                             : T("t_nivelar", "nivelar"),
                                origen.zfsName(), origen.connection, destino.dataset,
                                destino.connection}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }

    // 1) El destino se pone a escuchar.
    std::string recvOut;
    if (!agente(e, destino, {"--zfs-recv-listen", destino.dataset, "1"}, recvOut, 20000)) {
        return false;
    }
    const auto claves = clavesDe(recvOut);
    const std::string puerto = claves.count("PORT") ? claves.at("PORT") : std::string();
    const std::string testigo = claves.count("TOKEN") ? claves.at("TOKEN") : std::string();
    if (puerto.empty() || testigo.size() != 64) {
        std::fputs(TC("t_el_destino_5cc244", "el destino no abrió el puerto de recepción correctamente\n"), stderr);
        return false;
    }

    // 2) Con qué dirección ve el origen al destino. En la misma máquina, por el bucle
    // local; si no, por el host del perfil — que es como el origen llega a él.
    const std::string peer = mismaMaquina ? "127.0.0.1" : B::trim(pDestino->host);
    if (peer.empty()) {
        std::fprintf(stderr, TC("t_no_se_sabe_0d2422", "no se sabe con qué dirección ve %s a %s: la conexión de destino no "
                     "tiene host\n"), origen.connection.c_str(), destino.connection.c_str());
        return false;
    }

    // 3) El origen envía. Como TRABAJO: una transferencia grande no cabe en una espera.
    //
    std::string sendOut;
    if (!agente(e, origen,
                {"--zfs-send-to-peer-async", origen.zfsName(), peer, puerto, testigo, base,
                 banderasSend},
                sendOut, 30000)) {
        return false;
    }
    const auto cs = clavesDe(sendOut);
    const std::string jobId = cs.count("JOB_ID") ? cs.at("JOB_ID") : std::string();
    if (jobId.empty()) {
        std::fputs(TC("t_el_origen__722922", "el origen no devolvió identificador de trabajo\n"), stderr);
        return false;
    }
    std::fprintf(stdout, "%s\n", jobId.c_str());
    std::fprintf(stderr, TC("t_transferen_07aa2e", "transferencia en marcha como trabajo %s en %s\n"), jobId.c_str(), origen.connection.c_str());

    if (!pet.tiene("--wait")) {
        std::fprintf(stderr, TC("t_siga_con_j_2debd4", "siga con «job %s --on %s»\n"), jobId.c_str(), textoDe(origen).c_str());
        return true;
    }
    // Con --wait se espera aquí. Se sondea cada dos segundos: más a menudo es carga sin
    // información nueva, porque el daemon actualiza el avance a ese ritmo.
    std::string ultimo;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::string est;
        if (!agente(e, origen, {"--job-status", jobId}, est, 20000)) {
            return false;
        }
        const auto k = clavesDe(est);
        const std::string estado = k.count("STATE") ? k.at("STATE") : std::string();
        const std::string linea = k.count("PROGRESS_LINE") ? k.at("PROGRESS_LINE") : std::string();
        if (!linea.empty() && linea != ultimo) {
            ultimo = linea;
            std::fprintf(stderr, "  %s\n", linea.c_str());
        }
        if (estado != "running") {
            const std::string err = k.count("ERROR") ? k.at("ERROR") : std::string();
            std::fprintf(stderr, "trabajo %s: %s%s\n", jobId.c_str(), estado.c_str(),
                         err.empty() ? "" : (" — " + err).c_str());
            e.ultimoRc = (estado == "done" || estado == "finished") ? 0 : 1;
            return e.ultimoRc == 0;
        }
    }
}

// --- Instalar o actualizar el daemon.
//
// Se sube el binario NATIVO y se instala con el arranque propio de cada sistema: systemd
// en Linux, launchd en macOS, rc.d en FreeBSD, tarea programada en Windows. El binario
// viaja por la ENTRADA ESTÁNDAR en Unix; en Windows va por scp, porque PowerShell no
// devuelve de ReadToEnd() con megabytes y la instalación se quedaba colgada.
//
// **No hay respaldo por guion.** Si no está el binario de esa plataforma, no se instala
// nada: un agente de guion no habla TLS, y dejarlo puesto da una máquina que PARECE
// atendida y no lo está.
bool cmdInstalarDaemon(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    std::string error;
    const auto* p = perfilVivoDe(e, destino, error);
    if (!p) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    const std::string quien = p->name.empty() ? p->id : p->name;
    const bool esWindows = T::isWindowsConnection(*p);
    const std::string soBajo = B::toLowerAscii(p->osType);
    const bool esMac = B::contains(soBajo, "mac") || B::contains(soBajo, "darwin")
                       || B::contains(soBajo, "os x");
    const bool esFreeBsd = B::contains(soBajo, "freebsd");
    const std::string plataforma = esWindows ? "windows" : (esMac ? "macos" : (esFreeBsd ? "freebsd" : "linux"));

    if (!confirma(e, B::format(T("t_conf_install_daemon",
                                 "Se va a instalar o actualizar el daemon en %1 y arrancarlo "
                                 "con el gestor de servicios del sistema. ¿Continuar?"),
                               {quien}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }

    const B::ConnectionProfile perfil = conSudo(e, *p);

    // La arquitectura del OTRO lado, no la de aquí: se despliega a máquinas distintas.
    std::string arq = esWindows ? "x86_64" : "";
    if (!esWindows) {
        std::string out;
        std::string err;
        int rc = -1;
        if (T::runSsh(e.ses->transporte, perfil, "uname -m", 15000, out, err, rc, {}, {}, {}, {},
                      false, e.ses->verboso)
            && rc == 0) {
            arq = B::trim(out);
        }
    }
    const std::string binario = rutaDelAgente(plataforma, arq);
    if (binario.empty()) {
        std::fprintf(stderr, TC("t_no_se_enco_393030", "no se encontró el binario del daemon para %s/%s en este equipo.\n"
                     "No se instala nada: el respaldo por guion no habla TLS, y dejarlo puesto\n"
                     "daría una máquina que parece atendida y no lo está.\n"), plataforma.c_str(), arq.empty() ? "?" : arq.c_str());
        return false;
    }
    std::ifstream f(binario, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string contenido = ss.str();
    if (contenido.empty()) {
        std::fprintf(stderr, TC("t_el_binario_ce2086", "el binario del daemon está vacío: %s\n"), binario.c_str());
        return false;
    }
    std::fprintf(stderr, TC("t_desplegand_79081c", "desplegando %s (%zu bytes) en %s...\n"), binario.c_str(), contenido.size(), quien.c_str());

    namespace DP = B::daemonpayload;
    // La versión del agente y la del protocolo, que van en agent.conf. Vienen del
    // compilador, igual que en la interfaz: la del agente NO es la de la aplicación, lleva
    // el sufijo del marcador de esquema.
    const std::string version = ZFSMGR_AGENT_VERSION_STRING;
    const std::string api = "3";

    if (esWindows) {
        // Por scp y no por la entrada estándar: PowerShell no vuelve de ReadToEnd() con
        // megabytes, y la instalación se colgaba hasta agotar el plazo.
        const std::string subida = DP::windowsUploadPath();
        if (T::isLocalConnection(*p)) {
            std::error_code ec;
            std::filesystem::remove(subida, ec);
            std::filesystem::copy_file(binario, subida, ec);
            if (ec) {
                std::fprintf(stderr, TC("t_no_se_pudo_56946b", "no se pudo copiar el daemon a %s\n"), subida.c_str());
                return false;
            }
        } else {
            const H::ScpInvocacion inv = H::scpUpload(perfil, binario, subida, false);
            const B::ExecResult r =
                B::runExecStream(inv.program, inv.args, std::string(), 300000, B::StreamCallbacks{});
            if (r.rc != 0) {
                std::fprintf(stderr, TC("t_scp_fall_c_1d482f", "scp falló (código %d): %s\n"), r.rc, B::trim(r.err).c_str());
                return false;
            }
        }
        std::string out;
        std::string err;
        int rc = -1;
        if (!T::runSsh(e.ses->transporte, perfil,
                       H::withSudoCommand(perfil, DP::windowsNativeInstallCommand()), 300000, out,
                       err, rc, {}, {}, {}, {}, false, e.ses->verboso)
            || rc != 0) {
            std::fprintf(stderr, TC("t_la_instala_2b9e0d", "la instalación falló (código %d): %s\n"), rc, B::trim(err.empty() ? out : err).c_str());
            return false;
        }
        std::fprintf(stderr, TC("t_daemon_ins_3282b0", "daemon instalado en %s\n"), quien.c_str());
        return true;
    }

    // Unix. El binario entra por la entrada estándar y el guion lo coloca con `install`.
    const std::string despliegue =
        "tmp_bin='/tmp/zfsmgr-agent.bin.$$'; cat > \"$tmp_bin\"; install -m 700 \"$tmp_bin\" "
        + B::shSingleQuote(DP::unixBinPath()) + "; rm -f \"$tmp_bin\"; ";
    const std::string conf = DP::simpleConfigPayload(version, api);
    const std::string tls = DP::tlsBootstrapShellCommand();
    const std::string bin = DP::unixBinPath();
    const std::string confPath = DP::unixConfigPath();
    const std::string tlsFiles = DP::tlsDirPath() + " " + DP::tlsServerCertPath() + " "
                                 + DP::tlsServerKeyPath() + " " + DP::tlsClientCertPath() + " "
                                 + DP::tlsClientKeyPath();
    std::string guion;
    if (esMac) {
        guion = "mkdir -p /usr/local/libexec /etc/zfsmgr; " + despliegue
                + "cat > " + confPath + " <<'EOF_AGENT_CONF'\n" + conf + "\nEOF_AGENT_CONF\n"
                + "cat > " + DP::macPlistPath() + " <<'EOF_AGENT_PLIST'\n" + DP::macLaunchdPlist()
                + "\nEOF_AGENT_PLIST\n" + tls + "; "
                + "chmod 600 " + confPath + "; chmod 644 " + DP::macPlistPath() + "; "
                + "chown root:wheel " + bin + " " + confPath + " " + DP::macPlistPath() + "; "
                + "chown root:wheel " + tlsFiles + "; "
                  "launchctl bootout system/org.zfsmgr.agent >/dev/null 2>&1 || true; "
                  "launchctl bootstrap system "
                + DP::macPlistPath() + " >/dev/null 2>&1 || true; "
                  "launchctl enable system/org.zfsmgr.agent >/dev/null 2>&1 || true; "
                  "ok=0; i=0; "
                  "while [ \"$i\" -lt 30 ]; do "
                  "  if launchctl print system/org.zfsmgr.agent >/dev/null 2>&1; then ok=1; break; fi; "
                  "  launchctl kickstart system/org.zfsmgr.agent >/dev/null 2>&1 || true; "
                  "  i=$((i+1)); sleep 1; "
                  "done; "
                  "if [ \"$ok\" -ne 1 ]; then echo 'launchd agent not active after install' >&2; exit 1; fi";
    } else if (esFreeBsd) {
        guion = "mkdir -p /usr/local/libexec /etc/zfsmgr /usr/local/etc/rc.d; " + despliegue
                // Sin OpenSSL el daemon se instala y no arranca, y el motivo real queda en
                // un error del cargador que no dice qué falta.
                + "ldd_missing=$(ldd " + bin + " 2>&1 | grep 'not found' || true); "
                  "if [ -n \"$ldd_missing\" ]; then "
                  "  printf 'ERROR: el daemon tiene dependencias sin resolver:\\n%s\\n' \"$ldd_missing\" >&2; "
                  "  printf 'Instala OpenSSL con: pkg install openssl\\n' >&2; exit 1; fi; "
                + "cat > " + confPath + " <<'EOF_AGENT_CONF'\n" + conf + "\nEOF_AGENT_CONF\n"
                + "cat > " + DP::freeBsdRcPath() + " <<'EOF_AGENT_RC'\n" + DP::freeBsdRcScript()
                + "\nEOF_AGENT_RC\n" + tls + "; "
                + "chmod 700 " + DP::freeBsdRcPath() + "; chmod 600 " + confPath + "; "
                + "chown root:wheel " + bin + " " + confPath + " " + DP::freeBsdRcPath() + "; "
                + "chown root:wheel " + tlsFiles + "; "
                  "service zfsmgr_agent stop >/dev/null 2>&1 || true; "
                  "service zfsmgr_agent start; sleep 2; "
                  "if ! service zfsmgr_agent onestatus >/dev/null 2>&1; then "
                  "  printf 'ERROR: el daemon no permanece activo tras el arranque\\n' >&2; exit 1; fi";
    } else {
        guion = "if ! command -v systemctl >/dev/null 2>&1; then echo 'systemd not available' >&2; "
                "exit 1; fi; mkdir -p /usr/local/libexec /etc/zfsmgr; "
                + despliegue
                + "cat > " + confPath + " <<'EOF_AGENT_CONF'\n" + conf + "\nEOF_AGENT_CONF\n"
                + "cat > " + DP::linuxServicePath() + " <<'EOF_AGENT_SERVICE'\n"
                + DP::linuxSystemdService() + "\nEOF_AGENT_SERVICE\n" + tls + "; "
                + "chmod 600 " + confPath + "; chmod 644 " + DP::linuxServicePath() + "; "
                + "chown root:root " + bin + " " + confPath + " " + DP::linuxServicePath() + "; "
                + "chown root:root " + tlsFiles + "; "
                  "systemctl daemon-reload; systemctl enable zfsmgr-agent.service; "
                  "systemctl restart zfsmgr-agent.service";
    }

    std::string out;
    std::string err;
    int rc = -1;
    // allowAgentRpc=false: se está INSTALANDO el agente; desviar esto al RPC del agente que
    // se quiere sustituir no tendría ningún sentido.
    if (!T::runSsh(e.ses->transporte, perfil, H::withSudoStreamInputCommand(perfil, guion), 300000,
                   out, err, rc,
                   [](const std::string& l) { std::fprintf(stderr, "  %s\n", l.c_str()); },
                   [](const std::string& l) { std::fprintf(stderr, "  %s\n", l.c_str()); }, {},
                   contenido, false, e.ses->verboso)
        || rc != 0) {
        std::fprintf(stderr, TC("t_la_instala_2b9e0d", "la instalación falló (código %d): %s\n"), rc, B::trim(err.empty() ? out : err).c_str());
        e.ultimoRc = rc == 0 ? 1 : rc;
        return false;
    }
    // Lo que había cacheado del daemon anterior ya no vale.
    T::closeTunnelForConnection(e.ses->transporte, *p);
    T::clearRemoteDaemonTlsCacheForConnection(*p);
    T::clearLocalDaemonTlsCache();
    std::fprintf(stderr, TC("t_daemon_ins_3282b0", "daemon instalado en %s\n"), quien.c_str());
    return true;
}

// --- Trabajos en segundo plano.
//
// El daemon puede ejecutar las operaciones LARGAS sin que nadie espere al otro lado:
// breakdown, assemble, todir y rsync. Es lo que permite lanzar una copia de horas y cerrar
// la sesión sin matarla.
//
// Se pide con `--job` en esas órdenes; aquí están las de consultar y cancelar.

// Una línea «CLAVE=valor» en un mapa, que es el formato con el que contesta el agente.
std::map<std::string, std::string> clavesDe(const std::string& texto) {
    std::map<std::string, std::string> m;
    for (const std::string& linea : B::split(texto, "\n", true)) {
        const std::size_t igual = linea.find('=');
        if (igual != std::string::npos) {
            m[B::trim(linea.substr(0, igual))] = B::trim(linea.substr(igual + 1));
        }
    }
    return m;
}

bool cmdJobs(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    // Por omisión, SOLO lo que está corriendo. Un daemon que lleva meses en pie acumula
    // trabajos terminados, y enseñarlos todos convierte «¿qué está pasando ahora?» —que es
    // la pregunta que uno tiene al teclear `jobs`— en buscar entre decenas de líneas.
    //
    // El filtro se pide por el NOMBRE DEL ESTADO, que es el mismo que sale en la columna:
    // así no hay que aprender un vocabulario aparte ni mantener una tabla de traducción
    // entre lo que se escribe y lo que devuelve el daemon.
    std::set<std::string> estados;
    if (!pet.tiene("--all")) {
        for (const char* est : {"running", "queued", "done", "failed", "cancelled"}) {
            if (pet.tiene(std::string("--") + est)) {
                estados.insert(est);
            }
        }
        if (estados.empty()) {
            estados.insert("running");
            estados.insert("queued");  // encolado es «todavía va a pasar», no historia
        }
    }
    std::string out;
    if (!agente(e, destino, {"--job-list"}, out, 30000)) {
        return false;
    }
    Tabla t;
    t.nombreJson = "jobs";
    t.cabecerasTexto = {T("t_cab_id", "ID"), T("t_cab_estado", "ESTADO"), T("t_cab_tipo", "TIPO"),
                        T("t_cab_instantanea", "INSTANTÁNEA"), T("t_cab_bytes", "BYTES"),
                        T("t_cab_mib_s", "MiB/s"), T("t_cab_segundos", "SEGUNDOS"),
                        T("t_cab_error", "ERROR")};
    t.campos = {"id", "state", "type", "snap", "bytes", "rate", "elapsed", "error"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena,
               Tipo::Bytes,  Tipo::Cadena, Tipo::Entero, Tipo::Cadena};
    // Cada línea es «JOB={…json…}», que se analiza con el JSON de la capa base.
    for (const std::string& linea : B::split(out, "\n", true)) {
        if (!B::startsWith(linea, "JOB=")) {
            continue;
        }
        B::json::Value j;
        std::string err;
        if (!B::json::parse(linea.substr(4), j, &err)) {
            continue;
        }
        if (!estados.empty() && estados.count(j["state"].toString()) == 0) {
            continue;
        }
        // El ritmo con DOS decimales: `std::to_string` de un double da seis, y «0.000000»
        // ocupa una columna entera para no decir nada.
        char ritmo[32];
        std::snprintf(ritmo, sizeof(ritmo), "%.2f", j["rate"].toDouble());
        // El texto de error puede traer saltos de línea; en una tabla los rompe.
        std::string textoErr = j["error"].toString();
        B::replaceAll(textoErr, "\n", " ");
        B::replaceAll(textoErr, "\r", " ");
        t.filas.push_back({j["id"].toString(), j["state"].toString(), j["type"].toString(),
                           j["snap"].toString(), std::to_string(j["bytes"].toInt()), ritmo,
                           std::to_string(j["elapsed"].toInt()), B::trim(textoErr)});
    }
    if (t.filas.empty()) {
        // Con filtro puesto, «no hay trabajos» es engañoso: puede haber diez terminados. Se
        // dice cuál era el filtro para que la respuesta signifique lo que parece.
        std::fputs(estados.empty()
                       ? TC("t_no_hay_tra_f891dc", "no hay trabajos\n")
                       : TC("t_no_hay_tra_filtro", "no hay trabajos en curso (pruebe «jobs --all»)\n"),
                   stderr);
    }
    t.imprime(e.formato);
    return true;
}

bool cmdJob(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (pet.lista("texto").empty()) {
        std::fputs(TC("t_uso_job_id_7597d2", "uso: job <id> | job cancel <id>\n"), stderr);
        return false;
    }
    const bool cancelar = B::toLowerAscii(pet.uno("texto")) == "cancel";
    if (cancelar && pet.lista("texto").size() < 2) {
        std::fputs(TC("t_uso_job_ca_1e9ae2", "uso: job cancel <id>\n"), stderr);
        return false;
    }
    const std::string id = cancelar ? pet.lista("texto")[1] : pet.uno("texto");
    if (cancelar) {
        if (!confirma(e, B::format(T("t_conf_job_cancel",
                                     "Se va a cancelar el trabajo %1. Lo que ya haya hecho NO "
                                     "se deshace. ¿Continuar?"),
                                   {id}))) {
            std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
            return false;
        }
        std::string out;
        if (!agente(e, destino, {"--job-cancel", id}, out, 30000)) {
            return false;
        }
        std::fprintf(stderr, TC("t_cancelado__b0d1d4", "cancelado el trabajo %s\n"), id.c_str());
        return true;
    }
    std::string out;
    if (!agente(e, destino, {"--job-status", id}, out, 30000)) {
        return false;
    }
    Tabla t;
    t.nombreJson = "job";
    t.cabecerasTexto = {T("t_cab_campo", "CAMPO"), T("t_cab_valor", "VALOR")};
    t.campos = {"field", "value"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena};
    for (const auto& kv : clavesDe(out)) {
        t.filas.push_back({B::toLowerAscii(kv.first), kv.second});
    }
    t.imprime(e.formato);
    return true;
}

// Manda una operación larga al daemon en vez de esperarla. Devuelve el identificador, que
// es con lo que después se consulta o se cancela.
// Las cuatro órdenes que mueven datos de verdad se comportan IGUAL: van al daemon como
// trabajo, y `--wait` es lo que pide esperar aquí a que terminen.
//
// Antes no era así: `copy` iba como trabajo y se esperaba con `--wait`, mientras que
// `breakdown`, `assemble` y `todir` eran síncronas y `--job` las mandaba al daemon. Dos
// banderas para la misma idea, con el valor por omisión INVERTIDO entre ellas: había que
// recordar cuál era cuál para cada orden.
//
// El que manda es el trabajo: son operaciones de horas, y ese es además el camino que usa
// la interfaz. Esperar aquí es lo excepcional —un guion que quiere el resultado antes de
// seguir—, y por eso es lo que lleva bandera.
bool lanzaOEspera(Estado& e, const Peticion& pet, const ZfsmUrl& destino,
                  const std::vector<std::string>& argv) {
    if (!pet.tiene("--wait")) {
        return enviaComoTrabajo(e, destino, argv);
    }
    // SIN PLAZO: mueve datos de verdad, y matarlo a mitad es peor que aguardar.
    std::string out;
    if (!agente(e, destino, argv, out, 0)) {
        return false;
    }
    std::fprintf(stdout, "%s", out.c_str());
    return true;
}

bool enviaComoTrabajo(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv) {
    std::vector<std::string> conJob{"--job-submit"};
    for (const auto& a : argv) {
        conJob.push_back(a);
    }
    std::string out;
    if (!agente(e, destino, conJob, out, 60000)) {
        return false;
    }
    const auto claves = clavesDe(out);
    const auto it = claves.find("JOB_ID");
    if (it == claves.end()) {
        std::fputs(TC("t_el_daemon__64a9ed", "el daemon no devolvió identificador de trabajo\n"), stderr);
        return false;
    }
    std::fprintf(stdout, "%s\n", it->second.c_str());
    std::fprintf(stderr, TC("t_en_marcha__e494cb", "en marcha como trabajo %s; siga con «job %s»\n"), it->second.c_str(), it->second.c_str());
    return true;
}

// --- Pools.
//
// Todo por `--mutate-zpool-generic`, que recibe el argv de `zpool` en JSON y solo admite
// una lista cerrada de operaciones: nunca hay un intérprete de por medio.

bool zpoolGenerico(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv) {
    B::json::Value arr{B::json::Array{}};
    for (const std::string& a : argv) {
        arr.push(B::json::Value(a));
    }
    std::string out;
    if (!agente(e, destino, {"--mutate-zpool-generic", B::base64Encode(B::json::toCompact(arr))},
                out, 0)) {
        return false;
    }
    if (!B::trim(out).empty()) {
        std::fprintf(stdout, "%s", out.c_str());
    }
    return true;
}

// ¿Estamos en un pool? Un pool ES un dataset —el de la raíz—, así que se distingue por
// `isPoolRoot()` y no por un tipo de nodo aparte.
bool exigePool(const ZfsmUrl& u) {
    if (nodoDe(u) != Nodo::Dataset || !u.isPoolRoot()) {
        std::fprintf(stderr, TC("t_hace_falta_c7ec3b", "hace falta estar en un pool (ahora: %s)\n"), textoDe(u).c_str());
        return false;
    }
    return true;
}

// Las operaciones de mantenimiento que se piden igual: un verbo y el nombre del pool, con
// un subcomando opcional para parar o pausar.
// Los nombres de pool de una conexión, recordados durante la sesión.
//
// Hace falta para decidir si el primer argumento suelto de una orden de pool es el POOL
// sobre el que actuar o un argumento de la propia orden. Se pregunta una vez por conexión:
// sin la caché, cada `scrub`, `trim` o `clear` costaría una ida y vuelta de más.
const std::set<std::string>& poolsDe(Estado& e, const ZfsmUrl& donde) {
    const std::string clave = B::toLowerAscii(donde.connection);
    const auto ya = e.poolsPorConexion.find(clave);
    if (ya != e.poolsPorConexion.end()) {
        return ya->second;
    }
    std::set<std::string> nombres;
    std::string out;
    if (agente(e, donde, {"--dump-zpool-list"}, out, 20000)) {
        B::json::Value raiz;
        std::string err;
        if (B::json::parse(out, raiz, &err)) {
            for (const auto& kv : raiz["pools"].toObject()) {
                nombres.insert(kv.second["name"].toString(kv.first));
            }
        }
    }
    return e.poolsPorConexion.emplace(clave, std::move(nombres)).first->second;
}




bool cmdMantenimientoPool(Estado& e, const LineaAnalizada& linea, const char* op) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    std::vector<std::string> argv{op};
    // `zpool scrub -s` para, `-p` pausa; trim e initialize usan -s/-c/-u. Se aceptan por
    // nombre para no obligar a recordar qué letra usa cada uno. Cuál de las palabras vino
    // ya lo ha separado la firma: aquí solo se traduce a la letra.
    const std::string fase = B::toLowerAscii(pet.uno("fase"));
    if (fase == "stop" || fase == "cancel") {
        argv.push_back(std::string(op) == "scrub" ? "-s" : "-c");
    } else if (fase == "pause" || fase == "suspend") {
        argv.push_back(std::string(op) == "scrub" ? "-p" : "-s");
    }
    // El orden que pide zpool: BANDERAS, luego el pool, luego los discos
    // —`zpool trim [-r <rate>] <pool> [device]`—.
    //
    // Las dos mitades vienen de verlo fallar. Los discos iban ANTES del pool, así que
    // `trim <pool> <disco>` respondía «invalid character '/' in pool name». Y las banderas
    // nativas iban DESPUÉS del pool, donde zpool las ignora en silencio: `trim -r
    // noesunritmo` decía «trim en marcha» y el historial del pool registraba
    // `zpool trim pruebacli` a secas, sin el `-r`. Aceptada y no aplicada es la peor de las
    // dos formas de fallar.
    for (const std::string& b : pet.nativas()) {
        argv.push_back(b);
    }
    argv.push_back(destino.pool);
    for (const std::string& d : pet.lista("disco")) {
        argv.push_back(d);
    }
    if (!zpoolGenerico(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, TC("t_s_s_en_mar_afe49e", "%s: %s en marcha\n"), destino.pool.c_str(), op);
    return true;
}

bool cmdPoolSimple(Estado& e, const LineaAnalizada& linea, const char* verbo,
                   const char* op, const char* aviso) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (aviso && !confirma(e, B::format(T("t_conf_en_pool", "%1 en %2. ¿Continuar?"),
                                {aviso, destino.pool}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    std::vector<std::string> argv{op};
    // Las banderas del mandato original, tal cual. La lista está declarada en el catálogo y
    // es la MISMA con la que se validan, así que aceptar y pasar no se pueden separar.
    for (const std::string& b : pet.nativas()) {
        argv.push_back(b);
    }
    argv.push_back(destino.pool);
    // `clear` admite un dispositivo detrás del pool. Al declararlo como ranura dejó de
    // llegar aquí como suelto, así que hay que pasarlo a mano: consumido y no usado es
    // exactamente el fallo que este preámbulo viene a quitar.
    for (const std::string& d : pet.lista("disco")) {
        argv.push_back(d);
    }
    if (!zpoolGenerico(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, "%s: %s hecho\n", destino.pool.c_str(), op);
    return true;
}

// El estado detallado. Es texto para leer y se saca tal cual: fingir columnas donde
// `zpool status` dibuja un árbol de vdevs sería inventarse una estructura que no tiene.
bool cmdStatus(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (destino.pool.empty()) {
        std::fprintf(stderr, TC("t_hace_falta_8acbda", "hace falta un pool (ahora: %s)\n"), textoDe(destino).c_str());
        return false;
    }
    std::string out;
    if (!agente(e, destino, {"--dump-zpool-status", destino.pool}, out, 60000)) {
        return false;
    }
    std::fprintf(stdout, "%s", out.c_str());
    if (!out.empty() && out.back() != '\n') {
        std::fprintf(stdout, "\n");
    }
    return true;
}

bool cmdHistory(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (destino.pool.empty()) {
        std::fprintf(stderr, TC("t_hace_falta_8acbda", "hace falta un pool (ahora: %s)\n"), textoDe(destino).c_str());
        return false;
    }
    std::string out;
    if (!agente(e, destino, {"--dump-zpool-history", destino.pool}, out, 60000)) {
        return false;
    }
    Tabla t;
    t.nombreJson = "history";
    t.cabecerasTexto = {T("t_cab_cuando", "CUÁNDO"), T("t_cab_orden", "ORDEN")};
    t.campos = {"when", "command"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena};
    for (const std::string& linea : B::split(out, "\n", true)) {
        if (B::startsWith(linea, "History for")) {
            continue;
        }
        // «2026-02-04.11:23:49 zpool create …»: la marca de tiempo es el primer campo.
        const std::size_t esp = linea.find(' ');
        if (esp == std::string::npos) {
            t.filas.push_back({std::string(), B::trim(linea)});
        } else {
            t.filas.push_back({linea.substr(0, esp), B::trim(linea.substr(esp + 1))});
        }
    }
    t.imprime(e.formato);
    return true;
}

// Importar. Sin nombre, ENSEÑA lo que hay para importar en vez de fallar: es la pregunta
// que uno tiene antes de poder contestar la otra.
bool cmdImport(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (destino.connection.empty()) {
        std::fputs(TC("t_hace_falta_6a3acd", "hace falta una conexión\n"), stderr);
        return false;
    }
    if (pet.lista("texto").empty()) {
        std::string out;
        if (!agente(e, destino, {"--dump-zpool-import-probe"}, out, 60000)) {
            return false;
        }
        std::fprintf(stdout, "%s", out.c_str());
        if (!out.empty() && out.back() != '\n') {
            std::fprintf(stdout, "\n");
        }
        return true;
    }
    std::vector<std::string> argv{"import"};
    if (pet.tiene("-f")) {
        argv.push_back("-f");
    }
    argv.push_back(pet.uno("texto"));
    if (!pet.valor("as").empty()) {
        argv.push_back(pet.valor("as"));  // importar con otro nombre
    }
    if (!zpoolGenerico(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, "importado %s\n", pet.uno("texto").c_str());
    return true;
}

// Crear un pool. Es la orden MÁS destructiva de todas: escribe en los dispositivos que se
// le den, y si alguno tenía datos, desaparecen. La confirmación los enumera uno a uno.
bool cmdCrearPool(Estado& e, const Peticion& pet, const ZfsmUrl& destino) {
    if (pet.lista("texto").size() < 2) {
        std::fputs(TC("t_uso_create_63b88e", "uso: create <pool> <dispositivo> [<dispositivo>...] [-f]\n"
                     "            [-o prop=valor] [-O prop-fs=valor] [--mountpoint <ruta>]\n"
                     "  Los dispositivos se ESCRIBEN: lo que hubiera en ellos se pierde.\n"), stderr);
        return false;
    }
    // Una sola copia de la lista, y los iteradores DE ESA. `lista()` devuelve por valor, así
    // que `pet.lista(...).begin()` y `pet.lista(...).end()` eran extremos de dos vectores
    // distintos: el rango no significaba nada y el intérprete se caía al construir los
    // nombres. No se veía porque hasta ahora no se llegaba aquí: el preámbulo exigía estar
    // en un dataset y crear un pool era imposible, así que el fallo estaba tapado.
    const std::vector<std::string> textos = pet.lista("texto");
    const std::string pool = textos.front();
    const std::vector<std::string> vdevs(textos.begin() + 1, textos.end());
    if (!confirma(e, B::format(T("t_conf_create_pool",
                                 "Se va a crear el pool %1 en %2 ESCRIBIENDO en: %3.\nLo que "
                                 "hubiera en esos dispositivos SE PIERDE. ¿Continuar?"),
                               {pool, destino.connection, B::join(vdevs, ", ")}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    std::vector<std::string> argv{"create"};
    // Las banderas nativas van DELANTE del nombre del pool. Detrás, zpool las toma por
    // dispositivos o las ignora sin decir nada: `zpool trim pruebacli -r 100M` se tragó el
    // ritmo en silencio, y ese fue el fallo que enseñó dónde tienen que ir.
    for (const std::string& n : pet.nativas()) {
        argv.push_back(n);
    }
    if (pet.tiene("-f")) {
        argv.push_back("-f");
    }
    if (!pet.valor("mountpoint").empty()) {
        argv.push_back("-m");
        argv.push_back(pet.valor("mountpoint"));
    }
    for (const auto& kv : pet.l->repetidas) {
        argv.push_back(kv.first == "-o" ? "-o" : "-O");
        argv.push_back(kv.second);
    }
    argv.push_back(pool);
    for (const auto& v : vdevs) {
        argv.push_back(v);
    }
    if (!zpoolGenerico(e, destino, argv)) {
        return false;
    }
    // Con `-n` no se ha creado nada: zpool solo enseña la disposición que saldría. Decir
    // «creado el pool» ahí sería mentir sobre lo único que importa de esta orden.
    if (!pet.tiene("-n")) {
        std::fprintf(stderr, TC("t_creado_el__2de5da", "creado el pool %s\n"), pool.c_str());
    }
    return true;
}

// --- Editar una conexión ya dada de alta.
//
// Solo cambia lo que se pasa. Con terminal se ofrece el valor actual entre corchetes, de
// modo que pulsar Intro lo conserva: es lo que uno espera de «editar», frente a tener que
// volver a teclear todo.
bool cmdEditarConexion(Estado& e, const Peticion& pet, const ZfsmUrl& destino);

// --- Conectar, desconectar y refrescar.

std::string idDe(const Estado& e, const ZfsmUrl& u) {
    const auto* p = buscarConexion(e.conns, u.connection);
    return p ? (p->id.empty() ? p->name : p->id) : std::string();
}

bool cmdConectar(Estado& e, const LineaAnalizada& linea, bool conectar) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    const std::string id = idDe(e, destino);
    std::string error;
    if (!marcarDesconectada(*e.ses, id, !conectar, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    if (!conectar) {
        // Se cierran el túnel y la caché TLS: dejar una conexión abierta contra una máquina
        // que se acaba de marcar como desconectada es exactamente lo contrario de lo que se
        // ha pedido.
        T::closeTunnelForConnection(e.ses->transporte, *buscarConexion(e.conns, id));
        T::clearRemoteDaemonTlsCacheForConnection(*buscarConexion(e.conns, id));
    }
    recarga(e);
    std::fprintf(stderr, "%s marcada como %s\n", id.c_str(),
                 conectar ? "conectada" : "desconectada");
    return true;
}

// Refrescar: soltar lo que se tenía guardado de esa máquina y volver a preguntárselo.
//
// No es solo un listado: se cierra el túnel, se vacía la caché del material TLS, se quita
// el castigo por fallos recientes y se releen las conexiones del disco —por si la interfaz
// las ha cambiado mientras tanto—. Después se sondea. Es lo que uno espera de «refrescar»
// cuando algo se ha quedado colgado.
bool cmdRefrescar(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    const std::string id = idDe(e, destino);
    if (id.empty()) {
        std::fprintf(stderr, TC("t_hace_falta_901c78", "hace falta una conexión (ahora: %s)\n"), textoDe(destino).c_str());
        return false;
    }
    {
        const auto* p = buscarConexion(e.conns, id);
        T::closeTunnelForConnection(e.ses->transporte, *p);
        T::clearRemoteDaemonTlsCacheForConnection(*p);
        std::lock_guard<std::mutex> lock(e.ses->transporte.mutex);
        const std::string clave = T::remoteDaemonTlsCacheKey(*p);
        e.ses->transporte.retryAfterByConnKey.erase(clave);
        e.ses->transporte.retryReasonByConnKey.erase(clave);
    }
    recarga(e);

    // Y ahora se pregunta a la máquina. Los dos verbos que la interfaz usa para lo mismo.
    std::string basicos;
    if (!agente(e, destino, {"--dump-refresh-basics"}, basicos, 30000)) {
        return false;
    }
    std::string salud;
    std::string err;
    int rc = -1;
    std::string motivo;
    const auto* p = buscarConexion(e.conns, id);
    ejecutarAgente(*e.ses, *p, {"--health"}, salud, err, rc, &motivo, 20000);

    Tabla t;
    t.nombreJson = "refresh";
    t.cabecerasTexto = {T("t_cab_campo", "CAMPO"), T("t_cab_valor", "VALOR")};
    t.campos = {"field", "value"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena};
    t.filas.push_back({"connection", id});
    t.filas.push_back({"url", textoDe(destino)});
    // Las dos salidas son «CLAVE=valor» por línea, que es el formato del agente.
    for (const std::string& bloque : {basicos, salud}) {
        for (const std::string& linea : B::split(bloque, "\n", true)) {
            const std::size_t igual = linea.find('=');
            if (igual == std::string::npos) {
                continue;
            }
            t.filas.push_back({B::toLowerAscii(B::trim(linea.substr(0, igual))),
                               B::trim(linea.substr(igual + 1))});
        }
    }
    t.imprime(e.formato);
    return true;
}

// --- Retenciones (holds) y comparación de instantáneas.

// `zfs holds` NO tiene verbo tipado en el agente: se lee con una orden corriente, igual
// que hace la interfaz. Es una lectura, no una mutación.
bool cmdHolds(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;

    std::string error;
    const auto* p = perfilVivoDe(e, destino, error);
    if (!p) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    const B::ConnectionProfile perfil = conSudo(e, *p);
    std::string out;
    std::string err;
    int rc = -1;
    if (!T::runSsh(e.ses->transporte, perfil,
                   H::withSudoCommand(perfil, "zfs holds -H " + B::shSingleQuote(destino.zfsName())),
                   20000, out, err, rc, {}, {}, {}, {}, false, e.ses->verboso)
        || rc != 0) {
        const std::string detalle = B::trim(err).empty() ? B::trim(out) : B::trim(err);
        std::fprintf(stderr, "%s\n", detalle.empty() ? "no se pudieron leer las retenciones"
                                                      : detalle.c_str());
        e.ultimoRc = rc == 0 ? 1 : rc;
        return false;
    }
    Tabla t;
    t.nombreJson = "holds";
    t.cabecerasTexto = {T("t_cab_instantanea", "INSTANTÁNEA"), T("t_cab_etiqueta", "ETIQUETA"),
                        T("t_cab_creada", "CREADA")};
    t.campos = {"snapshot", "tag", "created"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    for (const std::string& linea : B::split(out, "\n", true)) {
        const std::vector<std::string> c = B::split(linea, "\t", false);
        if (c.size() >= 3) {
            t.filas.push_back({c[0], c[1], c[2]});
        }
    }
    t.imprime(e.formato);
    return true;
}

bool cmdRetencion(Estado& e, const LineaAnalizada& linea, bool poner) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    std::vector<std::string> argv{poner ? "hold" : "release"};
    if (pet.tiene("-r")) {
        argv.push_back("-r");
    }
    argv.push_back(pet.uno("etiqueta"));
    argv.push_back(destino.zfsName());
    if (!zfsGenerico(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, TC("t_s_la_reten_cb09d7", "%s la retención «%s» en %s\n"), poner ? "puesta" : "quitada", pet.uno("etiqueta").c_str(), destino.zfsName().c_str());
    return true;
}

// Qué cambió entre dos instantáneas del mismo dataset.
bool cmdDiff(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    if (pet.lista("destino").empty()) {
        std::fputs(TC("t_uso_diff_i_10a43a", "uso: diff <@instantánea-o-dataset> [--from <@instantánea>]\n"
                     "  Compara dos puntos del MISMO dataset. Sin --from se usa el sitio\n"
                     "  actual como origen.\n"), stderr);
        return false;
    }
    const ZfsmUrl& origen = pet.objetivo;
    ZfsmUrl hasta;
    std::string error;
    if (!resuelve(e, pet.uno("destino"), hasta, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    if (origen.snapshot.empty()) {
        std::fprintf(stderr, TC("t_el_origen__604ce4", "el origen tiene que ser una instantánea (ahora: %s)\n"), textoDe(origen).c_str());
        return false;
    }
    std::string out;
    if (!agente(e, origen, {"--dump-zfs-diff", origen.zfsName(), hasta.zfsName()}, out, 120000)) {
        return false;
    }
    Tabla t;
    t.nombreJson = "changes";
    t.cabecerasTexto = {T("t_cab_cambio", "CAMBIO"), T("t_cab_ruta", "RUTA"),
                        T("t_cab_renombrado_a", "RENOMBRADO A")};
    t.campos = {"change", "path", "renamed_to"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    // `zfs diff -H` da: <marca>\t<ruta>[\t<ruta nueva>]. La marca es +, -, M o R.
    for (const std::string& linea : B::split(out, "\n", true)) {
        const std::vector<std::string> c = B::split(linea, "\t", false);
        if (c.size() >= 2) {
            t.filas.push_back({c[0], c[1], c.size() >= 3 ? c[2] : std::string()});
        }
    }
    t.imprime(e.formato);
    return true;
}

// --- Información suelta.

bool cmdDevices(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (destino.connection.empty()) {
        std::fputs(TC("t_falta_conexion", "hace falta estar en una conexión"), stderr);
        std::fputc('\n', stderr);
        return false;
    }
    std::string out;
    if (!agente(e, destino, {"--dump-block-devices"}, out, 25000)) {
        return false;
    }
    B::json::Value raiz;
    std::string err;
    if (!B::json::parse(out, raiz, &err)) {
        std::fprintf(stderr, TC("t_resp_devices", "respuesta ilegible al listar dispositivos: %s\n"),
                     err.c_str());
        return false;
    }
    // Salen TODOS, con la columna que dice si están ocupados, y no solo los libres.
    // Esconder los ocupados escondería justo el disco que uno va a reutilizar a propósito
    // —un `zfs_member` de un pool viejo, por ejemplo—, y son pocos: aquí caben en pantalla.
    const bool soloLibres = pet.tiene("--free");
    Tabla t;
    t.nombreJson = "devices";
    t.cabecerasTexto = {T("t_cab_ruta", "RUTA"),       T("t_cab_tamano", "TAMAÑO"),
                        T("t_cab_tipo", "TIPO"),       T("t_cab_fs", "FS"),
                        T("t_cab_punto_de_montaje", "PUNTO DE MONTAJE"),
                        T("t_cab_ocupado", "OCUPADO")};
    t.campos = {"path", "size", "type", "fstype", "mountpoint", "inuse"};
    t.tipos = {Tipo::Cadena, Tipo::Bytes, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Booleano};
    for (const auto& d : raiz["devices"].toArray()) {
        const bool ocupado = d["inuse"].toBool();
        if (soloLibres && ocupado) {
            continue;
        }
        t.filas.push_back({d["path"].toString(), std::to_string(d["size"].toInt()),
                           d["type"].toString(), d["fstype"].toString(),
                           d["mountpoint"].toString(), ocupado ? "true" : "false"});
    }
    t.imprime(e.formato);
    return true;
}

// El punto de montaje de un dataset. Solo Unix: en Windows la ruta no es la que dice
// `mountpoint` —el pool se monta en una letra de unidad— y ahí la sincronización va por
// otro camino, que no está portado al intérprete.
bool montajeDe(Estado& e, const ZfsmUrl& u, std::string& out) {
    std::string mp;
    if (!agente(e, u, {"--dump-zfs-get-prop", "mountpoint", u.zfsName()}, mp)) {
        return false;
    }
    out = B::trim(mp);
    if (out.empty() || out == "none" || out == "-" || out.front() != '/') {
        std::fprintf(stderr, TC("t_s_no_est_m_7fb2be", "%s no está montado en ningún sitio\n"),
                     u.zfsName().c_str());
        return false;
    }
    return true;
}

// «Sincronizar» de la interfaz: copia el CONTENIDO de un dataset a otro con rsync.
//
// Faltaba entera en el intérprete, y encima el hueco estaba disimulado: existía un `sync`
// que es `zpool sync` —forzar la escritura pendiente, cosa de un instante— con el mismo
// nombre que la acción de la interfaz, que puede tardar horas. Ese se llama ahora `flush`.
bool cmdRsync(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& origen = pet.objetivo;
    if (pet.lista("destino").empty()) {
        std::fputs(TC("t_uso_rsync", "uso: rsync <destino> [--delete] [--check]\n"), stderr);
        return false;
    }
    ZfsmUrl destino;
    std::string err;
    if (!resuelve(e, pet.uno("destino"), destino, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return false;
    }
    if (!exigeDataset(destino)) {
        return false;
    }
    // Los dos extremos en la MISMA máquina: entre máquinas distintas la interfaz usa `tar`
    // sobre SSH, que no está portado aquí. Se dice, en vez de sincronizar contra un
    // directorio local que casualmente exista con el mismo nombre.
    if (B::toLowerAscii(origen.connection) != B::toLowerAscii(destino.connection)) {
        std::fputs(TC("t_rsync_dos_maquinas",
                      "rsync solo funciona dentro de una misma máquina; entre máquinas use copy\n"),
                   stderr);
        return false;
    }
    if (origen.zfsName() == destino.zfsName()) {
        std::fputs(TC("t_rsync_mismo", "el origen y el destino son el mismo dataset\n"), stderr);
        return false;
    }
    std::string rutaOrigen;
    std::string rutaDestino;
    if (!montajeDe(e, origen, rutaOrigen) || !montajeDe(e, destino, rutaDestino)) {
        return false;
    }
    const bool borra = pet.tiene("--delete");
    const bool simula = pet.tiene("--check");
    // Confirmación solo si va a BORRAR: sin `--delete` esto añade y actualiza, y no hay
    // nada que perder. Con `--delete` el destino acaba idéntico al origen, borrado incluido.
    if (borra && !simula
        && !confirma(e, B::format(T("t_conf_rsync",
                                    "%1 va a quedar IDÉNTICO a %2, borrando lo que sobre. ¿Continuar?"),
                                  {destino.zfsName(), origen.zfsName()}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    // El orden lo fija el daemon: [delete, dryRun, rsh, dstHost, origen, destino].
    B::json::Array carga;
    carga.push_back(B::json::Value(std::string(borra ? "1" : "0")));
    carga.push_back(B::json::Value(std::string(simula ? "1" : "0")));
    carga.push_back(B::json::Value(std::string()));  // rsh: mismo host
    carga.push_back(B::json::Value(std::string()));  // dstHost: mismo host
    carga.push_back(B::json::Value(rutaOrigen));
    carga.push_back(B::json::Value(rutaDestino));
    const std::vector<std::string> argv{
        "--mutate-rsync-local",
        B::base64Encode(B::json::toCompact(B::json::Value(std::move(carga))))};
    // Un `--check` es una simulación: no hay nada que mandar al daemon como trabajo, y lo
    // que uno quiere es LEER la salida ahora mismo.
    if (simula) {
        std::string out;
        if (!agente(e, origen, argv, out, 0)) {
            return false;
        }
        std::fprintf(stdout, "%s", out.c_str());
        return true;
    }
    return lanzaOEspera(e, pet, origen, argv);
}

bool cmdInfo(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (nodoDe(destino) == Nodo::Raiz) {
        std::fprintf(stdout, "kind\troot\nconnections\t%zu\n", e.conns.perfiles.size());
        return true;
    }
    std::string error;
    const auto* p = perfilDe(e, destino, error);
    if (!p) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    std::string out;
    if (!agente(e, destino, {"--health"}, out)) {
        return false;
    }
    std::fprintf(stdout, "url\t%s\nconnection\t%s\nhost\t%s\n", textoDe(destino).c_str(),
                 p->id.c_str(), T::isLocalConnection(*p) ? "-" : p->host.c_str());
    std::fprintf(stdout, "%s", out.c_str());
    return true;
}

// El ancho del terminal, para partir las descripciones de la ayuda. Sin poder
// averiguarlo se toman 80, que es lo que siempre se ha supuesto.
int anchoTerminal() {
#ifndef _WIN32
    winsize w{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 40) {
        return w.ws_col;
    }
#else
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        const int ancho = info.srWindow.Right - info.srWindow.Left + 1;
        if (ancho > 40) {
            return ancho;
        }
    }
#endif
    return 80;
}

}  // namespace

// Los hijos de un sitio, para completar una URL. Se pregunta a la máquina, que por un
// túnel ya montado son un par de milisegundos.
//
// Los fallos se TRAGAN a propósito: pulsar el tabulador no es pedir una operación, y llenar
// la pantalla de errores porque una máquina está apagada convierte una comodidad en un
// estorbo. Sin respuesta, simplemente no completa.
std::vector<std::string> hijosDe(Estado& e, const ZfsmUrl& u) {
    std::vector<std::string> out;
    switch (nodoDe(u)) {
        case Nodo::Raiz:
            for (const auto& p : e.conns.perfiles) {
                out.push_back(p.id.empty() ? p.name : p.id);
            }
            return out;
        case Nodo::Snapshot:
            return out;  // una instantánea no tiene hijos
        case Nodo::Conexion: {
            std::string texto;
            std::string err;
            int rc = -1;
            const auto* p = buscarConexion(e.conns, u.connection);
            if (!p || e.conns.desconectada(u.connection)
                || !ejecutarAgente(*e.ses, *p, {"--dump-zpool-list"}, texto, err, rc, nullptr, 8000)
                || rc != 0) {
                return out;
            }
            B::json::Value raiz;
            std::string errJson;
            if (!B::json::parse(texto, raiz, &errJson)) {
                return out;
            }
            for (const auto& kv : raiz["pools"].toObject()) {
                out.push_back(kv.first);
            }
            return out;
        }
        case Nodo::Dataset: {
            std::string texto;
            std::string err;
            int rc = -1;
            const auto* p = buscarConexion(e.conns, u.connection);
            if (!p || e.conns.desconectada(u.connection)
                || !ejecutarAgente(*e.ses, *p, {"--dump-zfs-list-all", u.dataset}, texto, err, rc,
                                   nullptr, 12000)
                || rc != 0) {
                return out;
            }
            const std::string prefijo = u.dataset + "/";
            for (const std::string& linea : B::split(texto, "\n", true)) {
                const std::size_t tab = linea.find('\t');
                const std::string nombre = tab == std::string::npos ? linea : linea.substr(0, tab);
                const std::size_t arroba = nombre.find('@');
                if (arroba != std::string::npos) {
                    if (nombre.substr(0, arroba) == u.dataset) {
                        out.push_back("@" + nombre.substr(arroba + 1));
                    }
                    continue;
                }
                if (B::startsWith(nombre, prefijo)
                    && nombre.find('/', prefijo.size()) == std::string::npos) {
                    out.push_back(nombre.substr(prefijo.size()));
                }
            }
            return out;
        }
    }
    return out;
}

// Qué se puede escribir donde está el cursor.
//
// Tres casos, y el orden importa: la PRIMERA palabra es una orden; una palabra que empieza
// por guion es una opción DE ESA orden; y cualquier otra cosa se trata como una URL.
// Las propiedades que ese dataset tiene de verdad, recordadas durante la sesión.
//
// Se preguntan a la máquina —`zfs get all`— en vez de llevar una lista escrita: así no
// envejece con OpenZFS y no se ofrece lo que ese pool no soporta. Se cachea porque el
// tabulador se pulsa muchas veces y cada pulsación costaría una ida y vuelta.
const std::vector<std::string>& propiedadesDe(Estado& e, const ZfsmUrl& donde) {
    const std::string clave = textoDe(donde);
    const auto ya = e.propsPorSitio.find(clave);
    if (ya != e.propsPorSitio.end()) {
        return ya->second;
    }
    std::vector<std::string> nombres;
    if (nodoDe(donde) == Nodo::Dataset || nodoDe(donde) == Nodo::Snapshot) {
        std::string out;
        B::json::Value raiz;
        std::string err;
        // El verbo devuelve el JSON de `zfs get -j`, no columnas: los nombres son las claves
        // de `properties`.
        if (agente(e, donde, {"--dump-zfs-get-all", donde.zfsName()}, out, 20000)
            && B::json::parse(out, raiz, &err)) {
            for (const auto& ds : raiz["datasets"].toObject()) {
                for (const auto& prop : ds.second["properties"].toObject()) {
                    nombres.push_back(prop.first);
                }
            }
        }
    }
    std::sort(nombres.begin(), nombres.end());
    nombres.erase(std::unique(nombres.begin(), nombres.end()), nombres.end());
    return e.propsPorSitio.emplace(clave, std::move(nombres)).first->second;
}

std::vector<std::string> completaEn(Estado& e, const std::string& linea, std::size_t cursor,
                                    std::size_t& desde) {
    // El trozo que se está escribiendo: desde el último espacio antes del cursor.
    desde = linea.rfind(' ', cursor == 0 ? 0 : cursor - 1);
    desde = (desde == std::string::npos || cursor == 0) ? 0 : desde + 1;
    if (desde > cursor) {
        desde = cursor;
    }
    const std::string parcial = linea.substr(desde, cursor - desde);
    const std::string antes = B::trim(linea.substr(0, desde));

    if (antes.empty()) {
        return nombresQueEmpiezanPor(parcial);
    }
    const std::vector<std::string> palabras = B::split(antes, " ", true);
    const std::string orden = B::toLowerAscii(palabras.front());
    if (!parcial.empty() && parcial.front() == '-') {
        return opcionesQueEmpiezanPor(orden, parcial);
    }
    // `help` completa nombres de orden, que es lo que lleva detrás.
    if (orden == "help" || orden == "?") {
        return nombresQueEmpiezanPor(parcial);
    }

    // --- Propiedades, en `get` y `set`.
    //
    // Los NOMBRES salen de la máquina, no de una lista escrita aquí: se pregunta al dataset
    // en el que uno está. Una lista propia envejecería con cada versión de OpenZFS y, peor,
    // ofrecería propiedades que ese pool no tiene.
    //
    // Los VALORES salen del catálogo de la capa base, que es el mismo que usa el
    // desplegable de la interfaz. Solo las de lista cerrada: para `quota` o `mountpoint` no
    // se ofrece nada, que es mejor que inventar.
    if (orden == "get" || orden == "set") {
        const std::size_t igual = parcial.find('=');
        if (igual != std::string::npos) {
            const std::string prop = parcial.substr(0, igual);
            const std::string escrito = parcial.substr(igual + 1);
            std::vector<std::string> out;
            for (const std::string& v : B::zfsprops::valoresDe(prop)) {
                if (B::startsWith(v, escrito)) {
                    out.push_back(prop + "=" + v);
                }
            }
            return out;
        }
        std::vector<std::string> out;
        for (const std::string& p : propiedadesDe(e, e.actual)) {
            if (B::startsWith(p, parcial)) {
                out.push_back(orden == "set" ? p + "=" : p);
            }
        }
        return out;
    }

    // Una URL. Se separa lo ya escrito en «lo que ya está resuelto» y «lo que se teclea»,
    // para preguntar por los hijos del sitio correcto.
    std::string base = parcial;
    std::string cola;
    const std::size_t corte = base.find_last_of("/@");
    if (corte == std::string::npos) {
        cola = base;
        base.clear();
    } else {
        cola = base.substr(base[corte] == '@' ? corte : corte + 1);
        base = base.substr(0, base[corte] == '@' ? corte : corte + 1);
    }
    ZfsmUrl sitio;
    std::string err;
    if (!resuelve(e, base.empty() ? std::string(".") : base, sitio, err)) {
        return {};
    }
    std::vector<std::string> out;
    for (const std::string& h : hijosDe(e, sitio)) {
        if (B::startsWith(h, cola)) {
            out.push_back(base + h);
        }
    }
    return out;
}

int ejecutarShell(Sesion& ses, Formato formato, const std::string& urlInicial, bool asumirSi) {
    Estado e;
    e.ses = &ses;
    e.formato = formato;
    e.asumirSi = asumirSi;
    recarga(e);
    if (!e.conns.aviso.empty()) {
        std::fprintf(stderr, "%s\n", e.conns.aviso.c_str());
    }

    // Se empieza en la máquina donde uno ya está. Si no hay conexión «Local» configurada,
    // se empieza en la raíz en vez de en una URL que no nombra nada.
    // Se resuelve con `resuelve()` y no con `parseZfsmUrl()` a secas para que el nombre de
    // la conexión quede normalizado a su IDENTIFICADOR desde el primer momento: si no, el
    // indicador decía «zfsm://Local» al arrancar y «zfsm://local/...» tras el primer `cd`,
    // que parecen dos sitios distintos.
    const std::string inicio = urlInicial.empty() ? std::string("zfsm://Local") : urlInicial;
    std::string errInicio;
    if (!resuelve(e, inicio, e.actual, errInicio)
        || buscarConexion(e.conns, e.actual.connection) == nullptr) {
        if (!urlInicial.empty()) {
            std::fprintf(stderr, TC("t_no_se_pudo_3b2240", "no se pudo empezar en %s: %s\n"), inicio.c_str(), errInicio.empty() ? "esa conexión no existe" : errInicio.c_str());
        }
        e.actual = ZfsmUrl{};
    }

    const bool interactivo = hayTerminal();
    if (interactivo) {
        std::fputs(TC("t_zfsmgr_cli_44809f", "zfsmgr-cli — «help» lista las órdenes y «help <orden>» explica una.\n"
                     "El tabulador completa órdenes y URL; las flechas recorren el historial.\n"), stderr);
    }

    using Manejador = std::function<bool(Estado&, const LineaAnalizada&)>;
    const std::map<std::string, Manejador> ordenes = {
        {"cd", cmdCd},
        {"ls", cmdLs},
        {"info", cmdInfo},
        {"create", cmdCreate},
        {"devices", cmdDevices},
        {"rsync", cmdRsync},
        {"rename", cmdRename},
        {"destroy", cmdDestroy},
        {"mount", [](Estado& s, const LineaAnalizada& l) { return cmdMontaje(s, l, true); }},
        {"unmount", [](Estado& s, const LineaAnalizada& l) { return cmdMontaje(s, l, false); }},
        {"promote", cmdPromote},
        {"connect", [](Estado& s, const LineaAnalizada& l) { return cmdConectar(s, l, true); }},
        {"disconnect", [](Estado& s, const LineaAnalizada& l) { return cmdConectar(s, l, false); }},
        {"refresh", cmdRefrescar},
        {"edit", cmdEdit},
        {"holds", cmdHolds},
        {"hold", [](Estado& s, const LineaAnalizada& l) { return cmdRetencion(s, l, true); }},
        {"release", [](Estado& s, const LineaAnalizada& l) { return cmdRetencion(s, l, false); }},
        {"diff", cmdDiff},
        {"scrub", [](Estado& s, const LineaAnalizada& l) { return cmdMantenimientoPool(s, l, "scrub"); }},
        {"trim", [](Estado& s, const LineaAnalizada& l) { return cmdMantenimientoPool(s, l, "trim"); }},
        {"initialize", [](Estado& s, const LineaAnalizada& l) { return cmdMantenimientoPool(s, l, "initialize"); }},
        {"flush", [](Estado& s, const LineaAnalizada& l) { return cmdPoolSimple(s, l, "flush", "sync", nullptr); }},
        {"upgrade", [](Estado& s, const LineaAnalizada& l) { return cmdPoolSimple(s, l, "upgrade", "upgrade", "Se va a subir la versión del pool, y eso NO se puede deshacer"); }},
        {"reguid", [](Estado& s, const LineaAnalizada& l) { return cmdPoolSimple(s, l, "reguid", "reguid", "Se va a cambiar el identificador único del pool"); }},
        {"clear", [](Estado& s, const LineaAnalizada& l) { return cmdPoolSimple(s, l, "clear", "clear", nullptr); }},
        {"export", [](Estado& s, const LineaAnalizada& l) { return cmdPoolSimple(s, l, "export", "export", "Se va a exportar el pool y dejará de estar disponible"); }},
        {"import", cmdImport},
        {"status", cmdStatus},
        {"jobs", cmdJobs},
        {"job", cmdJob},
        {"install-daemon", cmdInstalarDaemon},
        {"copy", cmdCopy},
        {"history", cmdHistory},
        {"allow", [](Estado& s, const LineaAnalizada& l) { return cmdPermisos(s, l, true); }},
        {"unallow", [](Estado& s, const LineaAnalizada& l) { return cmdPermisos(s, l, false); }},
        {"rollback", cmdRollback},
        {"clone", cmdClone},
        {"get", cmdGet},
        {"set", cmdSet},
        {"load-key", [](Estado& s, const LineaAnalizada& l) { return cmdClaves(s, l, "load-key"); }},
        {"unload-key", [](Estado& s, const LineaAnalizada& l) { return cmdClaves(s, l, "unload-key"); }},
        {"breakdown", cmdBreakdown},
        {"assemble", cmdAssemble},
        {"todir", cmdToDir},
        {"fromdir", cmdFromDir},
    };

    LectorDeLinea lector;
    lector.setCompletador([&e](const std::string& l, std::size_t c, std::size_t& d) {
        return completaEn(e, l, c, d);
    });

    std::string linea;
    while (!e.salir) {
        if (!lector.lee(textoDe(e.actual) + "> ", linea)) {
            break;
        }
        const std::string recortada = B::trim(linea);
        if (recortada.empty() || recortada.front() == '#') {
            continue;
        }
        // Se ANALIZA con la gramática, una sola vez. Antes se troceaba como un shell POSIX
        // y cada orden repartía los trozos por su cuenta, con cuatro criterios distintos
        // conviviendo; de ahí salían los argumentos ignorados en silencio.
        lector.recuerda(recortada);
        const LineaAnalizada an = analizaLinea(recortada);
        if (!an.error.empty()) {
            std::fprintf(stderr, "%s\n", an.error.c_str());
            e.ultimoRc = 2;
            continue;
        }
        if (an.vacia) {
            continue;
        }
        const std::string orden = B::toLowerAscii(an.verbo);
        const std::vector<std::string> args = an.lista("texto");

        if (orden == "exit" || orden == "quit") {
            e.salir = true;
            continue;
        }
        if (orden == "help" || orden == "?") {
            if (!args.empty()) {
                if (!imprimeAyudaDe(args.front(), anchoTerminal())) {
                    std::fprintf(stderr, TC("t_no_hay_nin_fea411", "no hay ninguna orden «%s»\n"), args.front().c_str());
                }
            } else {
                imprimeAyuda(anchoTerminal());
            }
            continue;
        }
        if (orden == "pwd") {
            std::fprintf(stdout, "%s\n", textoDe(e.actual).c_str());
            continue;
        }
        if (orden == "format") {
            if (args.empty()) {
                std::fprintf(stdout, "%s\n", e.formato == Formato::Tsv    ? "tsv"
                                             : e.formato == Formato::Json ? "json"
                                                                          : "text");
                continue;
            }
            const std::string v = B::toLowerAscii(args.front());
            if (v == "tsv") {
                e.formato = Formato::Tsv;
            } else if (v == "json") {
                e.formato = Formato::Json;
            } else if (v == "text") {
                e.formato = Formato::Texto;
            } else {
                std::fprintf(stderr, TC("t_formato_de_9610b7", "formato desconocido: %s (use text, tsv o json)\n"), v.c_str());
            }
            continue;
        }
        if (orden == "yes") {
            e.asumirSi = args.empty() || B::toLowerAscii(args.front()) != "off";
            std::fprintf(stderr, "confirmaciones %s\n", e.asumirSi ? "desactivadas" : "activadas");
            continue;
        }

        const auto it = ordenes.find(orden);
        if (it == ordenes.end()) {
            std::fprintf(stderr, TC("t_orden_desc_b05fd4", "orden desconocida: %s (pruebe «help»)\n"), orden.c_str());
            e.ultimoRc = 127;
            continue;
        }
        if (!it->second(e, an)) {
            if (e.ultimoRc == 0) {
                e.ultimoRc = 1;
            }
            // En un guion —sin terminal— un fallo detiene la sesión: seguir ejecutando
            // órdenes sobre un estado que no es el previsto es cómo se destruye lo que no
            // se quería tocar.
            if (!interactivo) {
                return e.ultimoRc;
            }
        }
    }
    return 0;
}

}  // namespace zfsmgr::cli
