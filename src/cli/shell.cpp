#include "shell.h"

#include "agentversion.h"

#include "daemoninstall.h"
#include "daemonpayload.h"
#include "ayuda.h"
#include "creacion.h"
#include "gramatica_cli.h"
#include "connectionjson.h"
#include "gsa.h"
#include "copyspec.h"
#include "refreshparse.h"
#include "storefiles.h"
#include "syncing.h"
#include "transfer.h"
#include "zfsallow.h"
#include "zfsprops.h"
#include "helpers.h"
#include "linea.h"
#include "json.h"
#include "processes.h"
#include "secretinput.h"
#include "strutil.h"
#include "tr.h"
#include "peers.h"
#include "advanced.h"
#include "listings.h"
#include "requests.h"
#include "pools.h"
#include "snapshots.h"
#include "datasets.h"
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
#include <array>
#include <set>
#include <chrono>
#include <sstream>
#include <thread>
#include <string>
#include <vector>

// Duplicar descriptores, para capturar la salida de una orden. Mismas llamadas con otro
// nombre en Windows.
#ifdef _WIN32
#include <io.h>
#define DUP_ _dup
#define DUP2_ _dup2
#define CLOSE_ _close
#define FILENO_ _fileno
#else
#include <unistd.h>
#define DUP_ dup
#define DUP2_ dup2
#define CLOSE_ close
#define FILENO_ fileno
#endif

namespace zfsmgr::cli {
namespace {

namespace B = zfsmgr::base;
namespace H = zfsmgr::base::helpers;
namespace T = zfsmgr::base::transport;
namespace PR = zfsmgr::base::peers;
namespace AV = zfsmgr::commands::advanced;
namespace PL = zfsmgr::commands::pools;
// `INST` y no `IN`: en Windows `IN` es un MACRO de `windows.h`, y
// `namespace IN = …` no compila allí. Lo cazó el cruce de MinGW.
namespace INST = zfsmgr::commands::snapshots;
namespace DS = zfsmgr::commands::datasets;
namespace TR = zfsmgr::base::transfer;
namespace L = zfsmgr::base::listings;
namespace CS = zfsmgr::base::copyspec;
namespace PET = zfsmgr::commands::requests;
namespace SY = zfsmgr::base::syncing;
using B::ZfsmUrl;
using B::ZfsmKind;

// Qué clase de sitio es el actual. La RAÍZ no es una URL válida —`zfsm://` sin conexión no
// nombra nada—, así que se representa con la conexión vacía y solo existe dentro del
// intérprete, donde sí hace falta: es lo que da un sitio al que subir desde una conexión.
enum class Nodo { Raiz, Conexion, Dataset, Snapshot };

// El filtro de filas de `ls` cuando el ÚLTIMO tramo del destino es un patrón:
// `ls tank/user/M*` lista tank/user y deja solo las filas que casan con «M*». Ver
// ejecutaConPatrones.
struct FiltroFilas {
    // Qué filas mira. Un patrón en un tramo de ruta es de HIJOS: «*» no debe sacar las
    // instantáneas, que no son hijos sino `tank/x@algo`. Uno tras la `@` es de
    // INSTANTÁNEAS. El resto —conexiones, pools, propiedades, ficheros— es de todo.
    enum class Clase { Todo, Hijos, Instantaneas };
    std::string patron;  // el tramo CRUDO, con sus comodines; vacío = sin filtro
    Clase clase{Clase::Todo};
    std::string texto;   // lo que escribió el usuario, para el mensaje de «nada coincide»
    // Ficheros: como la shell, «*» no casa con los que empiezan por «.» salvo que el patrón
    // empiece también por «.».
    bool ocultos{false};
    // Con VARIOS destinos (`ls ds#content/*/x*`), una sección sin coincidencias no es un
    // error: se calla y lo apunta aquí. «nada coincide» se dice una vez, si no casa ninguna.
    bool enVarios{false};
    bool quedoVacio{false};
};

struct Estado {
    FiltroFilas filtro;
    // Las rutas del origen de `rsync` cuando las ha expandido un patrón (`#content/*.log`).
    // Vacío = las que diga la URL. Ver ejecutaConPatrones.
    std::vector<std::string> rsyncRelativas;
    // Y entonces cada una aterriza DENTRO del destino con su nombre, como
    // `rsync -a src/s* dst/` deja dst/s1 y dst/s2. Sin patrón, `#content/{a,b}` vuelca el
    // CONTENIDO de cada una en el destino, que es lo que ha hecho siempre.
    bool rsyncConNombre = false;
    // La versión de agente de cada conexión, recordada durante la sesión. Ver
    // listaConexiones: preguntarla cuesta una ida y vuelta por máquina.
    std::map<std::string, std::string> versionDaemon;
    // Lo que el daemon dice que le impide trabajar bien, por conexión: hoy, en macOS, que no
    // puede leer los discos («sin acceso al disco»). Vacío si nada.
    std::map<std::string, std::string> avisoDaemon;
    // Los pools importables por conexión, para el tabulador de `import`: la sonda tarda un
    // par de segundos y pulsar el tabulador dos veces no tiene por qué esperar dos veces.
    std::map<std::string, std::pair<std::chrono::steady_clock::time_point, std::vector<std::string>>>
        importablesCache;
    // Los pools de cada conexión. Ver destinoDePool: se usan para decidir si el primer
    // argumento suelto nombra un pool o es un argumento de la orden.
    std::map<std::string, std::set<std::string>> poolsPorConexion;
    // Las propiedades de cada sitio, para el completado. Ver propiedadesDe.
    std::map<std::string, std::vector<std::string>> propsPorSitio;
    // Datasets sin montar que esta sesión ha montado EN SOLO LECTURA en un temporal para
    // leer dentro (ver dondeEstaMontado). Se sueltan al salir de ellos y al terminar.
    struct MontajeLectura {
        std::string conexion;
        std::string dataset;
        std::string dir;
    };
    std::vector<MontajeLectura> montajesLectura;
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
        // La raíz se enseña como «zfsm://», con las DOS barras del esquema.
        //
        // Decía «zfsm:/», que no es una URL de este esquema: `kEsquema` es «zfsm://» y el
        // analizador exige las dos. Se veía poco porque la raíz era un sitio de paso; desde
        // que el intérprete arranca aquí, es lo primero que se lee, y una URL que el propio
        // programa no aceptaría de vuelta es mal sitio donde empezar.
        return "zfsm://";
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

// La URL de la CONEXIÓN en la que se está, sin pool ni dataset. Es la base desde la que se
// lee un nombre ZFS completo.
ZfsmUrl base_conexion_de(const Estado& e) {
    ZfsmUrl base;
    base.kind = ZfsmKind::Connection;
    base.connection = e.actual.connection;
    return base;
}

bool resuelveCrudo(const Estado& e, const std::string& textoEntrada, ZfsmUrl& out,
                   std::string& error) {
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

    // La raíz escrita como URL. Es lo que enseña `pwd` en la raíz, y no se podía volver a
    // escribir: el analizador de URL exige una conexión y contestaba «falta la conexión».
    if (B::toLowerAscii(texto) == "zfsm://" || B::toLowerAscii(texto) == "zfsm:///") {
        out = ZfsmUrl{};
        return true;
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
        }
        // Y la sección en la que se esté: `#properties` desde `ds#content/x` es
        // `ds#properties`, no `ds#content/x#properties`, que no es una URL.
        limpia.section.clear();
        limpia.detail.clear();
        return B::parseZfsmUrl(B::formatZfsmUrl(limpia) + texto, out, error);
    }

    // La SECCIÓN se aparta antes de trocear por «/».
    //
    // `ls zfsm://local/fc16/user#content` funcionaba y `ls user#content` no: la ruta
    // relativa se troceaba entera y «user#content» acababa siendo el nombre del dataset,
    // que viajaba tal cual hasta zfs y volvía con «bookmark delimiter '#' is not expected
    // here» — un error de ZFS sobre una sintaxis que es NUESTRA, en una orden idéntica a
    // otra que sí valía. La sección se pega al final, cuando la ruta ya está resuelta.
    std::string colaSeccion;
    const std::size_t almohadilla = texto.find('#');
    if (almohadilla != std::string::npos) {
        colaSeccion = texto.substr(almohadilla);
        texto = texto.substr(0, almohadilla);
        if (texto.empty()) {
            texto = ".";
        }
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
                    err = B::format(T("t_snap_sin_hijos", "un snapshot no tiene hijos: %1"), {nombre});
                    return false;
            }
            if (!snap.empty()) {
                base.snapshot = snap;
                base.kind = ZfsmKind::Snapshot;
            }
        }
        // Y aquí se vuelve a pegar la sección, con la ruta ya resuelta. Se hace DENTRO
        // porque de esta función se sale por cuatro sitios distintos —absoluta, por nombre
        // de pool, por nombre de conexión y relativa— y ponerlo en cada uno era pedir que
        // se olvidara en el quinto.
        if (!colaSeccion.empty()) {
            const std::string sinAlmohadilla = colaSeccion.substr(1);
            const std::vector<std::string> trozos = B::split(sinAlmohadilla, "/", false);
            base.section.clear();
            base.detail.clear();
            for (std::size_t i = 0; i < trozos.size(); ++i) {
                std::string trozo;
                if (!B::percentDecode(trozos[i], trozo)) {
                    err = B::format(T("t_tramo_malo", "tramo mal codificado: %1"), {trozos[i]});
                    return false;
                }
                if (i == 0) {
                    base.section = B::toLowerAscii(trozo);
                } else if (!trozo.empty()) {
                    base.detail.push_back(trozo);
                }
            }
        }
        res = base;
        return true;
    };

    const std::vector<std::string> tramos = B::split(texto, "/", false);
    if (absoluta) {
        return aplica(ZfsmUrl{}, tramos, out, error);
    }

    // DENTRO de una sección de ficheros —`#content/…` o `#path/…`— lo relativo son
    // directorios, como en una shell: `cd sub` baja a `…/sub` y `cd ..` sube uno. Antes
    // `cd ..` desde `ds#content/docs` se saltaba la sección y subía de DATASET, y `cd sub`
    // buscaba un dataset hijo llamado «sub».
    //
    // Al salir por arriba (`..` en la raíz de la sección) se vuelve al dataset o a la
    // conexión, y lo que quede se lee como siempre. Para saltar a otra máquina desde aquí
    // está la URL completa o la barra inicial: un nombre suelto es un fichero.
    {
        const std::string sec = B::toLowerAscii(e.actual.section);
        if (sec == B::zfsmSection::kContent || sec == B::zfsmSection::kPath) {
            ZfsmUrl u = e.actual;
            std::size_t i = 0;
            for (; i < tramos.size(); ++i) {
                std::string tramo;
                if (!B::percentDecode(tramos[i], tramo)) {
                    error = B::format(T("t_tramo_malo", "tramo mal codificado: %1"), {tramos[i]});
                    return false;
                }
                if (tramo.empty() || tramo == ".") {
                    continue;
                }
                if (tramo == "..") {
                    if (!u.detail.empty()) {
                        u.detail.pop_back();
                        continue;
                    }
                    u.section.clear();
                    ++i;
                    break;
                }
                u.detail.push_back(tramo);
            }
            if (!u.section.empty()) {
                out = u;
                return true;
            }
            const std::vector<std::string> resto(tramos.begin() + static_cast<long>(i), tramos.end());
            return aplica(u, resto, out, error);
        }
    }

    // Una ruta que empieza por el nombre del POOL en el que estamos es el nombre ZFS
    // completo, no un hijo. Sin esto, `destroy zfsmgrtest/clonado` desde
    // `zfsm://Local/zfsmgrtest/origen` apuntaba a
    // `zfsmgrtest/origen/zfsmgrtest/clonado`, que es un nombre válido y no existe — o sea
    // un error confuso donde uno había escrito exactamente lo que quería.
    //
    // Con la MISMA salvedad que la regla de abajo: si no mueve, no era eso. Desde
    // `zfsm://local/local` —el pool `local` de la conexión `local`— un `cd local` daba otra
    // vez `zfsm://local/local` y el intérprete se quedaba callado, aunque existiera un hijo
    // `local/local`. Cuando la lectura de «nombre ZFS completo» devuelve el sitio actual,
    // la única que queda en pie es la relativa: un hijo que se llama así.
    bool saltaAtajoDeConexion = false;
    if (!e.actual.pool.empty() && tramosDe(texto).front() == e.actual.pool) {
        ZfsmUrl porPool;
        std::string errPool;
        if (!aplica(base_conexion_de(e), tramosDe(texto), porPool, errPool)) {
            error = errPool;
            return false;
        }
        if (B::formatZfsmUrl(porPool) != B::formatZfsmUrl(e.actual)) {
            out = porPool;
            return true;
        }
        // Y se va DIRECTO a la relativa, sin pasar por el atajo de conexión: si el primer
        // tramo se llama igual que una conexión —pasa cuando la máquina y su pool comparten
        // nombre— ese atajo se llevaría el `cd` a la raíz de la máquina, que es aún menos lo
        // que se quería decir que quedarse quieto.
        saltaAtajoDeConexion = true;
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
    if (!saltaAtajoDeConexion && buscarConexion(e.conns, tramos.front()) != nullptr) {
        ZfsmUrl porConexion;
        std::string errConexion;
        if (!aplica(ZfsmUrl{}, tramos, porConexion, errConexion)) {
            error = errConexion;
            return false;
        }
        // …salvo que el atajo no MUEVA. Estando en `zfsm://fc16`, `cd fc16` resolvía como
        // absoluta desde la raíz y aterrizaba exactamente donde ya estabas: el intérprete no
        // decía nada y no pasaba nada, que es la peor respuesta posible a una orden que uno
        // ha escrito a propósito. Y en esa máquina había un pool llamado `fc16`, o sea que
        // lo escrito sí nombraba algo; solo que el atajo se lo comía antes.
        //
        // La regla es la que ya justifica el atajo: existe para SALTAR de máquina, y saltar
        // a la que ya se está no es un salto. Cuando el resultado coincide con el sitio
        // actual se sigue por la vía relativa, que desde una conexión lee el tramo como el
        // pool — que es lo que quedaba por probar y lo que el usuario quería decir.
        //
        // Se compara el resultado, no «¿es el primer tramo mi conexión?»: así `cd fc16`
        // desde un dataset hondo de fc16 sigue subiendo a la raíz de la máquina, que sí
        // mueve y sí es útil.
        if (B::formatZfsmUrl(porConexion) != B::formatZfsmUrl(e.actual)) {
            out = porConexion;
            return true;
        }
    }

    std::string errRelativa;
    if (aplica(e.actual, tramos, out, errRelativa)) {
        return true;
    }
    error = errRelativa;
    return false;
}

// Resolver, y lo que vale para TODAS las URL sea cual sea la forma en que se escribieron:
// `#path` cuelga de una máquina, nunca de un dataset. Ahí la ruta es `#content`, y
// aceptarlo sería tener dos nombres para lo mismo —o, peor, uno que parece lo que no es—.
bool resuelve(const Estado& e, const std::string& texto, ZfsmUrl& out, std::string& error) {
    if (!resuelveCrudo(e, texto, out, error)) {
        return false;
    }
    if (B::toLowerAscii(out.section) == B::zfsmSection::kPath && !out.dataset.empty()) {
        error = CS::labelOf(CS::Failure::PathOnDataset);
        return false;
    }
    return true;
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
// Relee las conexiones del disco, y deja DESCONECTADA la que tenga una clave SSH que ya no
// existe.
//
// Una ruta de clave que desaparece —un disco que no está, un fichero borrado, una errata
// que se coló antes de comprobarlo al dar de alta— no rompe la conexión a la vista: ssh
// avisa en cada orden «Identity file … not accessible» y entra por otro lado si puede. Se
// aparta, se dice UNA vez con su nombre y su ruta, y `connect` la vuelve a poner cuando la
// clave esté.
bool sshClaveFalta(const B::ConnectionProfile& p, std::string& ruta);
void recarga(Estado& e) {
    e.conns = cargarConexiones(e.ses->dirConfig, e.ses->maestra);
    bool cambio = false;
    for (const auto& p : e.conns.perfiles) {
        const std::string id = p.id.empty() ? p.name : p.id;
        std::string ruta;
        if (!sshClaveFalta(p, ruta) || e.conns.desconectada(id)) {
            continue;
        }
        std::string error;
        if (marcarDesconectada(*e.ses, id, true, error)) {
            cambio = true;
            std::fprintf(stderr,
                         TC("t_clave_falta_desconectada",
                            "aviso: la clave SSH de «%s» no existe (%s); la conexión queda "
                            "desconectada. Cuando la clave esté: connect zfsm://%s\n"),
                         id.c_str(), ruta.c_str(), id.c_str());
        }
    }
    if (cambio) {
        e.conns = cargarConexiones(e.ses->dirConfig, e.ses->maestra);
    }
}
bool sshClaveFalta(const B::ConnectionProfile& p, std::string& ruta) {
    return H::sshKeyMissing(p, ruta);
}

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

bool zfsGeneric(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv);
void avisaSinMetadatos(const std::string& salida);
bool zpoolGeneric(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv);
bool cmdCrearPool(Estado& e, const Peticion& pet, const ZfsmUrl& destino,
                  const std::string& nombre);
bool enviaComoTrabajo(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv);
bool lanzaOEspera(Estado& e, const Peticion& pet, const ZfsmUrl& destino,
                  const std::vector<std::string>& argv);
std::map<std::string, std::string> clavesDe(const std::string& texto);

// --- Ejecutar un verbo del agente sobre el destino, contando lo que salga mal.
// `errorDeOrden`: si se da, el error de la ORDEN (rc ≠ 0) se devuelve ahí en vez de
// imprimirse, para quien tiene un plan B —`status` de un pool no importado busca entre los
// importables, y el «no such pool» de zfs solo vale si ese plan B tampoco encuentra nada—.
bool agente(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& args,
            std::string& out, int timeoutMs = 60000, std::string* errorDeOrden = nullptr) {
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
        if (errorDeOrden) {
            *errorDeOrden = detalle;
        } else {
            std::fprintf(stderr, "%s\n", detalle.empty() ? "la orden falló" : detalle.c_str());
        }
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
bool encaja(const ZfsmUrl& u, Objetivo pedido) {
    switch (pedido) {
        case Objetivo::None_:
        case Objetivo::Cualquiera:
            return true;
        case Objetivo::Conexion:
            // La conexión y NADA MÁS. Antes bastaba con que la URL tuviera conexión, y con
            // eso `install-daemon local/sobra1` se daba por bueno tomando «sobra1» como si
            // fuera una máquina.
            // Una ruta suelta de la máquina no es la máquina: `devices zfsm://m#path/x` es
            // un error, no `devices` de m. Desde la POSICIÓN sí se sube (subeHasta).
            return nodoDe(u) == Nodo::Conexion
                   && B::toLowerAscii(u.section) != B::zfsmSection::kPath;
        case Objetivo::Pool:
            return nodoDe(u) == Nodo::Dataset && u.isPoolRoot();
        case Objetivo::Dataset:
            return nodoDe(u) == Nodo::Dataset;
        case Objetivo::Snapshot:
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
std::string nombreDe(Objetivo pedido) {
    switch (pedido) {
        case Objetivo::None_: return {};
        case Objetivo::Cualquiera: return {};
        case Objetivo::Conexion: return T("t_obj_conexion", "una conexión");
        case Objetivo::Pool: return T("t_obj_pool", "un pool");
        case Objetivo::Dataset: return T("t_obj_dataset", "un dataset");
        case Objetivo::Snapshot: return T("t_obj_snap", "un snapshot");
        case Objetivo::DatasetOInstantanea:
            return T("t_obj_ds_snap", "un dataset o un snapshot");
    }
    return {};
}

// El sitio ACTUAL, subido hasta lo que la orden pide.
//
// Un argumento explícito tiene que ser exactamente lo que se pide —si escribo algo, quiero
// eso—, pero el sitio actual se interpreta con sentido: estando en `local/tank/datos`,
// `install-daemon` habla de la MÁQUINA, no del dataset, y no tiene sentido obligar a subir
// a mano antes de teclearlo.
ZfsmUrl subeHasta(const ZfsmUrl& u, Objetivo pedido) {
    ZfsmUrl r = u;
    switch (pedido) {
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
        case Objetivo::None_:
        case Objetivo::Cualquiera:
        case Objetivo::Snapshot:
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
bool esSeccionDeFicheros(const ZfsmUrl& u);
bool esRutaSuelta(const ZfsmUrl& u);
bool equivaleADataset(Estado& e, ZfsmUrl& u, const std::string& comoSeEscribio);

bool prepara(Estado& e, const LineaAnalizada& linea, Peticion& p) {
    const Orden* orden = ordenPorNombre(linea.verbo);
    if (!orden) {
        return false;
    }
    p.l = &linea;
    p.orden = orden;

    std::string explicito = p.valor("on");
    // En `fromdir`, `--from` es la máquina de ORIGEN y no el destino de la orden: es la única
    // en la que las dos opciones no son sinónimas. Tomarlo por el destino lo comprobaba
    // contra «un dataset» y `fromdir … --from zfsm://maq` no podía funcionar nunca.
    if (explicito.empty() && std::string(orden->nombre) != "fromdir") {
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
        // Una orden de MÁQUINA cuya ruta viene como `zfsm://maq#path/…` —`labelclear
        // zfsm://fc16#path/dev/sda9`—: la máquina es la de la URL, no la posición.
        if (orden->objetivo == Objetivo::Conexion) {
            for (const Ranura& r : orden->ranuras) {
                if (r.tipo != Ranura::Tipo::Ruta || p.lista(r.nombre).empty()) {
                    continue;
                }
                ZfsmUrl u;
                std::string err;
                if (B::startsWith(B::toLowerAscii(p.uno(r.nombre)), "zfsm://")
                    && resuelve(e, p.uno(r.nombre), u, err) && esRutaSuelta(u)) {
                    p.objetivo = subeHasta(u, Objetivo::Conexion);
                }
            }
        }
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

    // Un FICHERO —`#path/…` o `#content/…`— donde la orden quiere algo de ZFS: solo si es
    // exactamente el punto de montaje (ver equivaleADataset). `destroy` y `create` declaran
    // «cualquiera» porque también actúan sobre pools y conexiones, pero tampoco pueden
    // tomar un subdirectorio por su dataset. `rsync` maneja sus extremos él mismo.
    //
    // Desde la POSICIÓN dentro de `#content` se sigue actuando sobre el dataset, como
    // siempre: estando en `ds#content/docs`, `snapshot` es del dataset en el que se está.
    // En `send`, un fichero escrito como origen puede ser un flujo ya volcado: lo decide
    // cmdSend, que es quien sabe mirar si es un fichero o el directorio de una instantánea.
    if (std::string(orden->nombre) == "send" && !explicito.empty()
        && esSeccionDeFicheros(p.objetivo)) {
        return true;
    }
    {
        const bool quiereZfs = orden->objetivo == Objetivo::Pool
                               || orden->objetivo == Objetivo::Dataset
                               || orden->objetivo == Objetivo::Snapshot
                               || orden->objetivo == Objetivo::DatasetOInstantanea
                               || std::string(orden->nombre) == "destroy"
                               || std::string(orden->nombre) == "create";
        if (quiereZfs && std::string(orden->nombre) != "rsync" && esSeccionDeFicheros(p.objetivo)
            && (!explicito.empty() || esRutaSuelta(p.objetivo))) {
            // Y después NO se sube: si el punto de montaje es de `tank/data`, una orden de
            // pool no puede acabar en `tank`. Lo que no encaje lo dice la comprobación de
            // abajo.
            if (!equivaleADataset(e, p.objetivo, explicito)) {
                return false;
            }
        }
    }

    if (orden->objetivo != Objetivo::None_ && !encaja(p.objetivo, orden->objetivo)) {
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

// Deja en la tabla solo las filas cuyo nombre casa con el filtro, mirando las columnas
// dadas (la del nombre; en las conexiones, el identificador y el nombre visible).
//
// Falso —y lo dice— si con filtro no queda ninguna: «nada coincide» es un error y no una
// lista vacía. Una lista vacía se lee como «no hay nada», que es otra afirmación.
bool aplicaFiltro(Estado& e, Tabla& t, std::initializer_list<std::size_t> columnas = {0}) {
    if (e.filtro.patron.empty()) {
        return true;
    }
    std::vector<std::vector<std::string>> quedan;
    for (auto& fila : t.filas) {
        bool casa = false;
        for (const std::size_t c : columnas) {
            if (c >= fila.size()) {
                continue;
            }
            std::string nombre = fila[c];
            const bool esSnap = !nombre.empty() && nombre[0] == '@';
            if (e.filtro.clase == FiltroFilas::Clase::Hijos && esSnap) {
                continue;
            }
            if (e.filtro.clase == FiltroFilas::Clase::Instantaneas) {
                if (!esSnap) {
                    continue;
                }
                nombre = nombre.substr(1);
            }
            if (e.filtro.ocultos && !nombre.empty() && nombre[0] == '.'
                && (e.filtro.patron.empty() || e.filtro.patron[0] != '.')) {
                continue;
            }
            if (B::matchesRawPattern(e.filtro.patron, nombre)) {
                casa = true;
                break;
            }
        }
        if (casa) {
            quedan.push_back(std::move(fila));
        }
    }
    t.filas = std::move(quedan);
    if (t.filas.empty() && e.filtro.enVarios) {
        e.filtro.quedoVacio = true;
        return false;
    }
    if (t.filas.empty()) {
        std::fprintf(stderr, TC("t_nada_coincide", "nada coincide con «%s»\n"), e.filtro.texto.c_str());
        e.ultimoRc = 1;
        return false;
    }
    return true;
}

// --- ls

// El listado de conexiones es EL MISMO que el de la orden suelta `connections list`. La
// tabla se construye en un solo sitio (session.cpp): tenerla duplicada hacía que la misma
// pregunta se contestara con columnas distintas según por dónde se preguntara.
// El listado de conexiones, SIEMPRE con la versión del agente de cada máquina, marcada
// con `*` cuando no es la que espera este cliente.
//
// Estuvo detrás de una opción `--daemon` con el argumento de que `ls` en la raíz era la
// única orden que responde al instante y sin hablar con nadie, y que con una máquina
// apagada cada una cuesta su plazo de espera. Se retiró porque el argumento se cae por su
// propio peso: la versión del agente es la razón por la que uno mira ese listado, y una
// columna que hay que pedir aparte es una columna que no se mira. Quien no quiera esperar
// tiene la conexión desconectada, que sigue sin sondearse.
//
// El plazo por máquina es de 8 s y el resultado se recuerda durante la sesión —el fallo
// también—, así que la espera se paga una vez.
// Olvida la versión del daemon que la sesión recordaba para esta conexión.
//
// `listaConexiones` la recuerda toda la sesión —el fallo también— para no esperar dos veces
// por una máquina que no contesta. Pero hay órdenes que la dejan OBSOLETA en el acto, y sin
// esto el «-» de antes de instalar seguía saliendo después de instalar, hasta salir del
// intérprete: `install-daemon` cambia el daemon, `edit` puede cambiar la máquina a la que
// apunta la conexión, y `refresh` es justo para volver a preguntar.
void olvidaVersionDaemon(Estado& e, const B::ConnectionProfile& p) {
    e.versionDaemon.erase(p.id);
    e.versionDaemon.erase(p.name);
    e.avisoDaemon.erase(p.id);
    e.avisoDaemon.erase(p.name);
}

// TODO lo que la sesión recuerda de una conexión y que una de estas órdenes deja obsoleto: el
// túnel abierto, el material TLS leído, la penalización por un fallo reciente y la versión
// del daemon. Es lo que hacía `refresh`, sacado aquí para que `install-daemon` y `edit` hagan
// lo mismo en vez de dejar a medias lo que había.
//
// Las tres primeras van por usuario, máquina, puerto y clave SSH; tras un `edit` que cambie
// la dirección hay que llamarla con el perfil de ANTES, o el túnel contra la máquina vieja
// seguiría abierto.
void olvidaEstadoDeConexion(Estado& e, const B::ConnectionProfile& p) {
    T::closeTunnelForConnection(e.ses->transporte, p);
    T::clearRemoteDaemonTlsCacheForConnection(p);
    {
        std::lock_guard<std::mutex> lock(e.ses->transporte.mutex);
        const std::string clave = T::remoteDaemonTlsCacheKey(p);
        e.ses->transporte.retryAfterByConnKey.erase(clave);
        e.ses->transporte.retryReasonByConnKey.erase(clave);
    }
    olvidaVersionDaemon(e, p);
}

bool listaConexiones(Estado& e, const Peticion& pet) {
    (void)pet;
    Tabla t = tablaDeConexiones(e.conns);
    t.cabecerasTexto.push_back(T("t_cab_daemon", "DAEMON"));
    t.campos.push_back("daemon");
    t.tipos.push_back(Tipo::Cadena);
    t.cabecerasTexto.push_back(T("t_cab_avisos", "AVISOS"));
    t.campos.push_back("warnings");
    t.tipos.push_back(Tipo::Cadena);
    const std::string esperada = B::agentversion::expected();
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
                    auto claves = clavesDe(salida);
                    version = claves["VERSION"];
                    if (claves["DISK_ACCESS"] == "denied") {
                        e.avisoDaemon[id] = claves["DISK_ACCESS_RESTART"] == "1"
                                                ? T("t_aviso_disco_reiniciar",
                                                    "sin acceso al disco: reiniciar el agente")
                                                : T("t_aviso_disco", "sin acceso al disco");
                    }
                }
                // Se recuerda durante la sesión, incluido el fallo: repetir `ls --daemon`
                // no vuelve a esperar por una máquina que ya no contestó.
                e.versionDaemon[id] = version;
            }
        }
        // El « * » distingue VIEJA de NUEVA, que no es lo mismo aunque las dos difieran.
        //
        // Se comparaba con `!=`, así que un agente más nuevo que este cliente salía
        // marcado igual que uno anticuado, y el usuario no podía saber cuál de las dos
        // mitades hay que actualizar. La comparación ordenada es la que ya usaba la
        // interfaz; ahora es la misma para las dos.
        if (version.empty()) {
            version = "-";
        } else {
            // Por la versión de publicación, no por la cadena entera: el cuarto número es
            // una huella y compararlo como número marcaba « + » a un agente de otra
            // compilación de la misma versión, que es justo uno que hay que actualizar.
            switch (B::agentversion::verdictFor(version, esperada)) {
                case B::agentversion::Verdict::UpToDate: break;
                case B::agentversion::Verdict::Update: version += " *"; break;  // a actualizar
                case B::agentversion::Verdict::Ahead: version += " +"; break;   // el viejo es el cliente
            }
        }
        t.filas[i].push_back(version);
        const auto aviso = e.avisoDaemon.find(id);
        t.filas[i].push_back(aviso == e.avisoDaemon.end() ? std::string("-") : aviso->second);
    }
    if (!aplicaFiltro(e, t, {0, 1})) {
        return false;
    }
    t.imprime(e.formato);
    // Y qué hacer, una vez, debajo de la tabla: la columna dice QUÉ pasa, esto CÓMO se arregla.
    for (const auto& fila : t.filas) {
        if (fila.size() >= 2 && fila.back() != "-") {
            std::fputs(TC("t_aviso_disco_remedio",
                          "aviso: un agente de macOS sin «Acceso total al disco» no ve los pools por "
                          "importar.\n  Configuración del Sistema > Privacidad y Seguridad > Acceso "
                          "total al disco > /usr/local/libexec/zfsmgr-agent,\n  y después reiniciar "
                          "el agente en esa máquina: sudo launchctl kickstart -k "
                          "system/org.zfsmgr.agent\n"),
                       stderr);
            break;
        }
    }
    return true;
}

// Qué se dice cuando la sonda de importables no encuentra nada.
//
// «No hay ninguno» y «no puedo mirar» se leen igual en la salida de `zpool import`, y aquí
// se decidía solo el primero de los dos casos: se buscaba la marca de discos ilegibles y se
// soltaba el aviso de macOS, sin mirar el tercer estado —el permiso concedido DESPUÉS de
// arrancar el agente, que en macOS no surte efecto hasta reiniciarlo—. La base ya sabe
// distinguirlos y la interfaz gráfica ya los distinguía; esto pone al CLI de acuerdo con
// las dos, y en un único sitio para las dos órdenes que preguntan lo mismo: `ls` e `import`.
void avisaSondaImportables(const std::string& sonda, bool huboPools) {
    if (huboPools) {
        return;  // hay importables: no hay nada que explicar
    }
    const auto lectura = H::readImportProbe(sonda, false);
    using D = H::ImportProbeDiagnosis;
    if (lectura.diagnosis != D::DisksUnreadable && lectura.diagnosis != D::ProbablyNeedsRestart) {
        return;  // se puede mirar y no hay ninguno: NO se avisa de nada
    }
    const std::string donde = lectura.device.empty() ? T("t_los_discos", "los discos")
                                                     : lectura.device;
    std::fprintf(stderr, TC("t_sonda_ilegible", "aviso: el agente no puede leer %s, así que un "
                            "pool sin importar NO aparece aquí.\n"), donde.c_str());
    if (lectura.diagnosis == D::ProbablyNeedsRestart) {
        std::fputs(TC("t_sonda_reinicio_cli", "  El permiso del sistema cambió DESPUÉS de que el "
                      "agente arrancara, y macOS lo decide al arrancar el proceso:\n"
                      "  reinícielo en esa máquina: sudo launchctl kickstart -k system/org.zfsmgr.agent\n"), stderr);
    } else {
        std::fputs(TC("t_sonda_permiso_cli", "  En macOS: Configuración del Sistema > Privacidad y "
                      "Seguridad > Acceso total al disco >\n"
                      "  /usr/local/libexec/zfsmgr-agent, y después reiniciar el agente en esa máquina:\n"
                      "  sudo launchctl kickstart -k system/org.zfsmgr.agent\n"),
                   stderr);
    }
}

// Los pools de una conexión, del JSON de `zpool list`.
bool listaPools(Estado& e, const ZfsmUrl& destino) {
    std::string out;
    if (!agente(e, destino, PET::poolList(), out)) {
        return false;
    }
    B::json::Value raiz;
    std::string err;
    // Sin pools, `zpool list -j` NO devuelve lo mismo en todas partes: en Linux imprime un
    // objeto con «pools» vacío, y el OpenZFS de macOS —2.4.1— no imprime nada y sale con 0.
    // Tratar eso como JSON ilegible decía «respuesta ilegible de zpool list: texto JSON
    // vacío» en una máquina donde lo único que pasa es que todavía no hay ningún pool.
    if (B::trim(out).empty()) {
        raiz = B::json::Value(B::json::Object{});
    } else if (!B::json::parse(out, raiz, &err)) {
        std::fprintf(stderr, TC("t_respuesta__74ad0d", "respuesta ilegible de zpool list: %s\n"), err.c_str());
        return false;
    }
    // **Un pool es también un dataset: el raíz.** Por eso esta tabla lleva las dos mitades,
    // las columnas de POOL —estado, tamaño, libre, uso, salud, importado— y las de DATASET
    // —tipo, usado, compresión, montado, punto de montaje—.
    //
    // Antes solo llevaba las primeras, y las segundas se enseñaban añadiendo la fila del
    // propio pool DENTRO de su listado, al hacer `cd`. Eso confundía: al entrar en un pool
    // aparecía él mismo junto a sus hijos, y no era obvio si esa fila era el pool o un
    // dataset con el mismo nombre. Los datos son los mismos; el sitio, no.
    Tabla t;
    t.nombreJson = "pools";
    t.cabecerasTexto = {T("t_cab_nombre", "NOMBRE"),   T("t_cab_estado", "ESTADO"),
                        T("t_cab_tamano", "TAMAÑO"),   T("t_cab_libre", "LIBRE"),
                        T("t_cab_uso", "USO"),         T("t_cab_dedup", "DEDUP"),
                        T("t_cab_ram_ddt", "RAM DDT"),
                        T("t_cab_salud", "SALUD"),
                        T("t_cab_importado", "IMPORTADO"),
                        T("t_cab_tipo", "TIPO"),       T("t_cab_usado", "USADO"),
                        // MONTADO, y no ESTADO como en el listado de datasets: esta tabla
                        // YA tiene una columna ESTADO —la del pool: ONLINE, FAULTED— y dos
                        // con el mismo nombre no se leen. El motivo de ESTADO allí era que
                        // un volumen no se monta sino que se expone; un pool raíz es siempre
                        // un sistema de ficheros, así que aquí la pregunta vieja sigue valiendo.
                        T("t_cab_compr", "COMPR"),     T("t_cab_montado", "MONTADO"),
                        T("t_cab_punto_de_montaje", "PUNTO DE MONTAJE")};
    // `dedupratio` es del POOL, no de un dataset, y por eso va en ESTA tabla y no en la de
    // datasets: la DDT es una sola para el pool entero y ZFS no reparte el ahorro entre sus
    // datasets. Lo único que hay por dataset es `dedup = off|on|sha256…`, que es un AJUSTE
    // —si lo que se escriba de ahora en adelante se deduplica— y no una medida de nada.
    //
    // No cuesta una llamada más: `zpool list -j` ya la devuelve y el agente ya la reenviaba
    // entera; era el cliente el que la tiraba.
    t.campos = {"name", "state", "size", "free", "capacity", "dedupratio", "ddtmemory",
                "health", "imported",
                "type", "used", "compressratio", "mounted", "mountpoint"};
    // USADO va como CADENA, no como `Tipo::Bytes`, y no es un descuido.
    //
    // `Tipo::Bytes` promete que el valor de dentro está en bytes: el texto lo formatea y el
    // tsv y el json emiten el número. La fuente de esta columna es `zfs get`, que da «336K»
    // ya formateado, y el agente no sirve esa propiedad en crudo. Declararla `Bytes` hacía
    // que el json emitiera `336` —`atoll("336K")`— y perdiera la unidad.
    //
    // Reconvertir «336K» a bytes aquí sería inventarse una precisión que la fuente no dio.
    // La consecuencia, que conviene saber: en el listado de DATASETS esta columna sí va en
    // bytes, porque allí la fuente es `zfs list -p`. Igualarlas pide un verbo de propiedades
    // en crudo en el agente.
    // RAM DDT va como `Bytes`: en texto se lee «292M» y en tsv y json salen los bytes
    // exactos, que es lo que un guion puede comparar contra un umbral.
    t.tipos = {Tipo::Cadena, Tipo::Cadena,   Tipo::Cadena, Tipo::Cadena,
               Tipo::Cadena, Tipo::Cadena,   Tipo::Bytes,   Tipo::Cadena, Tipo::Booleano,
               Tipo::Cadena, Tipo::Cadena,   Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};

    // Los campos de dataset del pool raíz, uno por pool.
    //
    // Es una consulta por pool, no recursiva: `--dump-zfs-get-json` con las cinco propiedades
    // que hacen falta. Pedir el árbol entero —`--dump-zfs-list-all`— traería todos los
    // descendientes para quedarse con la primera fila.
    //
    // Si un pool no contesta, sus celdas van vacías y el resto de la tabla se enseña igual:
    // que falte el «usado» no es motivo para no enseñar el estado.
    const auto camposDeDataset = [&](const std::string& pool) {
        std::array<std::string, 5> v{};
        std::string salidaProps;
        if (!agente(e, destino,
                    PET::specificProperties(
                        {"type", "used", "compressratio", "mounted", "mountpoint"}, pool),
                    salidaProps, 15000)) {
            return v;
        }
        std::vector<L::Property> props;
        std::string errProps;
        if (!L::properties(salidaProps, props, errProps)) {
            return v;
        }
        for (const L::Property& pr : props) {
            if (pr.name == "type") {
                // La MISMA palabra que usa el listado de datasets. `zfs` dice «filesystem» y
                // allí se escribe «dataset»: dos tablas con la columna TIPO que no significan
                // lo mismo se leen peor que una columna menos.
                v[0] = pr.value == "snapshot" ? "snapshot" : "dataset";
            } else if (pr.name == "used") {
                v[1] = pr.value;
            } else if (pr.name == "compressratio") {
                // Sin la «x» final: `zfs get` la pone y `zfs list -p` no, y esta columna sale
                // en las dos tablas.
                v[2] = (!pr.value.empty() && pr.value.back() == 'x')
                           ? pr.value.substr(0, pr.value.size() - 1)
                           : pr.value;
            } else if (pr.name == "mounted") {
                v[3] = pr.value;
            } else if (pr.name == "mountpoint") {
                v[4] = pr.value;
            }
        }
        return v;
    };
    const auto prop = [](const B::json::Value& p, const char* k) {
        return p["properties"][k]["value"].toString();
    };
    std::set<std::string> yaEstan;
    for (const auto& kv : raiz["pools"].toObject()) {
        const auto& p = kv.second;
        const std::string nombre = p["name"].toString(kv.first);
        yaEstan.insert(nombre);
        const auto ds = camposDeDataset(nombre);
        // Sin la «x» final, igual que COMPR: las dos salen de sitios que la ponen o no
        // según la orden, y en una misma tabla no pueden escribirse distinto.
        std::string dedup = prop(p, "dedupratio");
        if (!dedup.empty() && dedup.back() == 'x') {
            dedup.pop_back();
        }
        // Lo que la DDT cuesta en MEMORIA. Es una llamada por pool, y se paga porque se
        // midió lo que vale: 0,03 s sobre una tabla de 1,15 M de entradas, lo mismo que un
        // `zpool status` a secas, porque el histograma sale de contadores acumulados.
        //
        // Vacía cuando el pool no tiene tabla, que es el caso normal, y también cuando no
        // se pudo preguntar. Las dos se enseñan como «-»: ninguna de las dos es «0 bytes»,
        // y escribir un cero diría que la deduplicación está puesta y no cuesta nada.
        std::string ramDdt;
        std::string salidaDedup;
        if (agente(e, destino, PET::poolDedupStatus(nombre), salidaDedup, 15000)) {
            const H::DedupDdt ddt = H::parseDedupDdt(salidaDedup);
            if (ddt.hayTabla) {
                ramDdt = std::to_string(ddt.bytesEnMemoria);
            }
        }
        t.filas.push_back({nombre, p["state"].toString(), prop(p, "size"), prop(p, "free"),
                           prop(p, "capacity"), dedup, ramDdt, prop(p, "health"), "true", ds[0],
                           ds[1], ds[2], ds[3], ds[4]});
    }

    // Los que están AHÍ pero sin importar. `zpool list` no los ve —solo enumera los
    // importados— y por eso un pool perfectamente visible en la interfaz no salía en `ls`:
    // no es que estuviera oculto, es que se preguntaba por otra cosa.
    //
    // La sonda es una orden aparte y puede fallar sin que eso invalide lo ya listado (falta
    // de permisos sobre los discos, un agente sin «Acceso total al disco» en macOS): si no
    // responde, se enseña lo importado y se sigue, en vez de no enseñar nada.
    std::string sonda;
    if (agente(e, destino, PET::importableProbe(), sonda, 25000)) {
        const std::vector<H::ImportablePoolInfo> sondados = H::parseZpoolImportOutput(sonda);
        avisaSondaImportables(sonda, !sondados.empty());
        for (const H::ImportablePoolInfo& imp : sondados) {
            // El mismo conjunto sirve para dos cosas: no repetir uno ya importado, y no
            // repetirlo consigo mismo. La sonda ejecuta `zpool import` Y `zpool import -s`
            // y pega las dos salidas, así que un pool que aparece en las dos —lo normal—
            // llegaba aquí dos veces y salía dos veces en el listado.
            if (imp.pool.empty() || !yaEstan.insert(imp.pool).second) {
                continue;
            }
            // Sin tamaño ni uso: eso solo se sabe una vez importado, y rellenarlo con ceros
            // diría que el pool está vacío.
            // Un pool sin importar no tiene campos de dataset que enseñar: sus datasets no
            // están montados ni se pueden consultar hasta importarlo.
            t.filas.push_back({imp.pool, imp.state, "", "", "", "", "", imp.state, "false", "",
                               "", "", "", ""});
        }
    }
    if (!aplicaFiltro(e, t)) {
        return false;
    }
    t.imprime(e.formato);
    return true;
}

// Los hijos y las instantáneas de un dataset. Sale de `--dump-zfs-list-all`, que es TSV con
// las columnas name,guid,used,compressratio,encryption,creation,referenced,montados,
// mountpoint,canmount.
bool listaDataset(Estado& e, const ZfsmUrl& destino) {
    std::string out;
    if (!agente(e, destino, PET::datasetList(destino.dataset), out)) {
        return false;
    }
    Tabla t;
    t.nombreJson = "entries";
    t.cabecerasTexto = {T("t_cab_nombre", "NOMBRE"), T("t_cab_tipo", "TIPO"),
                        T("t_cab_usado", "USADO"), T("t_cab_compr", "COMPR"),
                        T("t_cab_estado_ds", "ESTADO"),
                        T("t_cab_punto_de_montaje", "PUNTO DE MONTAJE")};
    // El campo se llama `status` y no `mounted`, y eso es a propósito: su significado ha
    // cambiado, no solo su presentación. Un guion que leyera `mounted` esperando «yes»/«no»
    // debe encontrarse con que ese campo YA NO ESTÁ, en vez de con uno que se llama igual y
    // contesta otra cosa. Fallar a la cara vale más que mentir con el mismo nombre.
    t.campos = {"name", "type", "used", "compressratio", "status", "mountpoint"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Bytes, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    // Aquí se añadía la fila del PROPIO pool cuando lo listado era un pool, para que sus
    // campos de dataset se vieran en alguna parte. Confundía: al entrar en un pool aparecía
    // él mismo junto a sus hijos, y no había forma de saber si esa fila era el pool o un
    // dataset con su mismo nombre.
    //
    // Esos campos están ahora donde corresponde: en el listado de POOLS, que lleva las dos
    // mitades —las columnas de pool y las de dataset— porque un pool es también el dataset
    // raíz. Aquí solo salen los hijos.
    const std::string prefijo = destino.dataset + "/";
    for (const std::string& linea : B::split(out, "\n", true)) {
        const std::vector<std::string> c = B::split(linea, "\t", false);
        if (c.size() < 10) {
            continue;
        }
        const std::string& nombre = c[0];
        if (nombre == destino.dataset) {
            continue;  // uno no es hijo de sí mismo
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
        // El TIPO lo dice ZFS, no se adivina.
        //
        // Se deducía del nombre —«lleva @, luego es una instantánea; si no, un dataset»— y
        // así un VOLUMEN salía como «dataset». Visto en vivo: `back/tooling` es un zvol de
        // 78 G y el listado lo enseñaba igual que un sistema de ficheros, con el punto de
        // montaje a «-» como única pista. No es un detalle de rótulo: en un volumen no se
        // puede entrar, no tiene hijos que listar y no se monta.
        //
        // La columna puede no venir: un agente anterior a esto manda diez y ahí se acaba la
        // línea. Entonces se vuelve a la deducción de siempre, que acierta con las
        // instantáneas y es lo único que había.
        const std::string tipoCrudo = c.size() > 10 ? B::toLowerAscii(B::trim(c[10]))
                                                     : std::string();
        const std::string tipo = !tipoCrudo.empty()
                                     ? (tipoCrudo == "filesystem" ? "dataset" : tipoCrudo)
                                     : (esSnap ? "snapshot" : "dataset");
        // El ESTADO: una sola columna que contesta «¿puedo llegar a esto ahora mismo?» para
        // las dos clases de cosa.
        //
        // Antes era MONTADO, y para un volumen decía «-»: nada. Lo que un volumen tiene no
        // es montaje sino DISPOSITIVO, y si está expuesto o no es justo lo que uno quiere
        // saber. No se usa `volmode` para decirlo: su valor habitual es `default` —«lo que
        // diga el parámetro del módulo»— y eso no contesta la pregunta. El agente mira si
        // existe el nodo, que es un hecho y no una intención.
        const std::string dispositivo = c.size() > 11 ? B::toLowerAscii(B::trim(c[11]))
                                                       : std::string();
        std::string estado;
        if (esSnap || tipo == "snapshot") {
            estado = "-";  // una instantánea ni se monta ni se expone
        } else if (tipo == "volume") {
            // Sin la columna —agente anterior a esto— no se inventa: se dice que no se sabe.
            estado = dispositivo.empty() || dispositivo == "-" ? "?" : dispositivo;
        } else {
            estado = B::toLowerAscii(B::trim(c[7])) == "yes" ? "mounted" : "unmounted";
        }
        t.filas.push_back({esSnap ? "@" + nombre.substr(arroba + 1) : nombre.substr(prefijo.size()),
                           tipo, c[2], c[3], estado, c[8]});
    }
    if (!aplicaFiltro(e, t)) {
        return false;
    }
    t.imprime(e.formato);
    return true;
}

// Las propiedades: `#properties`, y `#properties/<nombre>` para una sola.
bool listaPropiedades(Estado& e, const ZfsmUrl& destino) {
    const std::string objetivo = destino.zfsName();
    std::string out;
    // `get tank compr*`: la propiedad es un patrón. Se piden TODAS —una sola llamada— y se
    // filtran aquí. (Por la URL, `#properties/compr*`, ya llega como filtro desde
    // ejecutaConPatrones; esto cubre el segundo argumento de `get`, que no pasa por ella.)
    // Con VARIOS destinos (`get tank/{a,b} compression`), el valor pelado de una sola
    // propiedad dejaría secciones sueltas «destino: valor». Se pide la tabla y se filtra por
    // el nombre exacto: así sale UNA tabla con DESTINO, PROPIEDAD, VALOR y ORIGEN.
    const bool variosDestinos = acumuladorActivo() != nullptr;
    if (!destino.detail.empty() && e.filtro.patron.empty()
        && (B::hasWildcard(destino.detail.front()) || variosDestinos)) {
        e.filtro.patron = B::hasWildcard(destino.detail.front())
                              ? destino.detail.front()
                              : B::percentEncodeSegment(destino.detail.front());
        e.filtro.clase = FiltroFilas::Clase::Todo;
        e.filtro.texto = destino.detail.front();
        ZfsmUrl todas = destino;
        todas.detail.clear();
        const bool ok = listaPropiedades(e, todas);
        e.filtro = FiltroFilas{};
        return ok;
    }
    if (!destino.detail.empty()) {
        if (!agente(e, destino, PET::datasetProperty(destino.detail.front(), objetivo), out)) {
            return false;
        }
        std::fprintf(stdout, "%s\n", B::trim(out).c_str());
        return true;
    }
    if (!agente(e, destino, PET::datasetProperties(objetivo), out)) {
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
    if (!aplicaFiltro(e, t)) {
        return false;
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
    // Una INSTANTÁNEA no tiene permisos delegados: son del dataset. Se corta AQUÍ y no se
    // pregunta.
    //
    // Sin esto llegaba al usuario el error crudo de ZFS —«cannot open 'x@s': snapshot
    // delimiter '@' is not expected here» y un «Failed to open dataset» detrás—, que habla
    // de la sintaxis de una orden que él no ha escrito. Y hay una trampa peor: medido en
    // unib, `zfs allow` sobre una instantánea devuelve **rc=0** y escribe el error por la
    // salida, así que mirar el código de retorno no sirve para detectarlo.
    if (!destino.snapshot.empty()) {
        ZfsmUrl deDataset = destino;
        deDataset.snapshot.clear();
        deDataset.section.clear();
        deDataset.detail.clear();
        std::fprintf(stderr,
                     TC("t_permisos_de_instantanea",
                        "un snapshot no tiene permisos delegados: son del dataset. "
                        "Pruebe «ls #permissions --on %s»\n"),
                     B::formatZfsmUrl(deDataset).c_str());
        return false;
    }
    std::string out;
    if (!agente(e, destino, PET::permissionsOf(objetivo), out)) {
        return false;
    }
    Tabla t;
    t.nombreJson = "permissions";
    t.cabecerasTexto = {T("t_cab_alcance", "ALCANCE"), T("t_cab_clase", "CLASE"),
                        T("t_cab_quien", "QUIÉN"), T("t_cab_permisos", "PERMISOS")};
    t.campos = {"scope", "kind", "who", "permissions"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    // El análisis vive en `base/zfsallow`, no aquí.
    //
    // Estaba escrito dos veces —esta y la que hizo falta para el servidor web— sobre un
    // formato que tiene una trampa: el ALCANCE va en el título de la sección y no en la
    // línea, así que una copia que se despiste concede a los descendientes lo que se quería
    // conceder solo aquí. Una sola copia, con pruebas sobre salida real.
    namespace ZA = zfsmgr::base::zfsallow;
    for (const ZA::Entry& en : ZA::parse(out)) {
        const std::string quien = en.name.empty() ? std::string(ZA::zfsToken(en.who))
                                                    : en.name;
        // Con los TEXTOS DE ZFS y no con los legibles: esta salida la leen guiones, y ya
        // decía «Local+Descendent permissions» y «user». Cambiarlos por algo más bonito
        // rompería un guion sin avisar.
        t.filas.push_back({ZA::zfsSectionTitle(en.scope), ZA::zfsToken(en.who), quien,
                           B::join(en.permissions, ",")});
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
    if (!zfsGeneric(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, TC("t_s_en_s_35a806", "%s en %s\n"), conceder ? "permisos delegados" : "permisos retirados", objetivo.c_str());
    return true;
}

// La ruta REAL que hay detrás de `#content`, que no es la del dataset.
//
// Vive aparte de quien la lista porque hay dos que la necesitan: la orden `ls` y el
// TABULADOR, que completa nombres de fichero sin enseñar nada. `callado` es para el
// segundo: una pulsación de tabulador no debe escribir errores por pantalla.
// --- Dónde está montado un dataset, DE VERDAD.
//
// La propiedad `mountpoint` dice dónde se montaría, no dónde está. Fiarse de ella daba dos
// fallos, los dos peores que un error porque contestan algo creíble y ajeno:
//   - Sin montar, la ruta de la propiedad puede existir y ser de OTRO: un entorno de
//     arranque con `mountpoint=/` hacía que `cp …kali#content/etc/hostname` leyera el
//     /etc/hostname de la máquina, y que copiar HACIA él escribiera en el /etc de la máquina.
//   - Montado en otro sitio —a mano, o por nosotros en un temporal—, la propiedad sigue
//     diciendo `/`.
// Así que se pregunta a `zfs mount`. Y un dataset sin montar, para LEER, se monta en solo
// lectura en un temporal sin tocar sus propiedades (el verbo del agente lo explica); para
// ESCRIBIR no se monta nada: eso es `mount`, y lo decide quien lo administra.

// El montaje temporal de esta sesión para ese dataset, o vacío.
std::string montajeLecturaDe(const Estado& e, const std::string& conexion, const std::string& dataset) {
    for (const auto& m : e.montajesLectura) {
        if (B::toLowerAscii(m.conexion) == B::toLowerAscii(conexion) && m.dataset == dataset) {
            return m.dir;
        }
    }
    return {};
}

// Dónde está montado según `zfs mount -j`, o vacío si no aparece.
std::string montajeReal(Estado& e, const ZfsmUrl& u, const std::string& dataset) {
    std::string out;
    if (!agente(e, u, PET::mounts(), out, 20000)) {
        return {};
    }
    B::json::Value v;
    if (!B::json::parse(out, v, nullptr)) {
        return {};
    }
    std::string mp = v["datasets"][dataset]["mountpoint"].toString();
    while (mp.size() > 1 && (mp.back() == '/' || mp.back() == '\\') && mp[mp.size() - 2] != ':') {
        mp.pop_back();
    }
    return mp;
}

// La ruta donde se abre `dataset`. `paraEscribir` no monta nada; `callado` (el tabulador) no
// monta ni dice nada: pulsar TAB no debería dejar cosas montadas.
bool dondeEstaMontado(Estado& e, const ZfsmUrl& u, const std::string& dataset, bool paraEscribir,
                      bool callado, std::string& ruta) {
    ruta = montajeLecturaDe(e, u.connection, dataset);
    if (!ruta.empty()) {
        if (paraEscribir) {
            if (!callado) {
                std::fprintf(stderr, TC("t_montaje_ro_no_escribe",
                                        "%s no está montado: solo se ha montado en solo lectura "
                                        "para mirarlo, y ahí no se escribe. Para escribir, móntelo "
                                        "en su sitio con «mount»\n"),
                             dataset.c_str());
            }
            e.ultimoRc = 1;
            return false;
        }
        return true;
    }
    std::string montado;
    if (!agente(e, u, PET::datasetProperty("mounted", dataset), montado)) {
        return false;
    }
    if (B::trim(montado) != "no") {
        ruta = montajeReal(e, u, dataset);
        if (!ruta.empty()) {
            return true;
        }
        // No aparece en `zfs mount` aunque diga que está montado: se recurre a la
        // propiedad, que es lo que se hacía siempre.
        std::string mp;
        if (!agente(e, u, PET::datasetProperty("mountpoint", dataset), mp)) {
            return false;
        }
        ruta = B::trim(mp);
        if (ruta.empty() || ruta == "none" || ruta == "-" || ruta == "legacy") {
            if (!callado) {
                std::fprintf(stderr, TC("t_s_no_est_m_7fb2be", "%s no está montado en ningún sitio\n"),
                             dataset.c_str());
            }
            e.ultimoRc = 1;
            return false;
        }
        return true;
    }
    // Sin montar.
    std::string error;
    const auto* p = perfilVivoDe(e, u, error);
    const bool esWindows = p && T::isWindowsConnection(*p);
    if (paraEscribir || callado || esWindows) {
        if (callado) {
        } else if (paraEscribir) {
            std::fprintf(stderr, TC("t_no_montado_escribir",
                                    "%s no está montado: no se escribe en él. Para escribir, "
                                    "móntelo con «mount»\n"),
                         dataset.c_str());
        } else {
            std::fprintf(stderr, TC("t_no_montado_content", "%s no está montado: no hay contenido que "
                                    "listar\n"), dataset.c_str());
        }
        e.ultimoRc = 1;
        return false;
    }
    std::string out;
    std::string fallo;
    if (!agente(e, u, PET::mountReadOnlyTemporarily(dataset), out, 60000, &fallo)) {
        std::fprintf(stderr, TC("t_montaje_ro_fallo",
                                "%s no está montado, y no se pudo montar en solo lectura para "
                                "mirarlo: %s\n"),
                     dataset.c_str(), fallo.c_str());
        return false;
    }
    const auto claves = clavesDe(out);
    ruta = claves.count("MOUNTPOINT") ? B::trim(claves.at("MOUNTPOINT")) : std::string();
    if (ruta.empty()) {
        std::fprintf(stderr, "%s\n", B::trim(out).c_str());
        e.ultimoRc = 1;
        return false;
    }
    if (claves.count("TEMPORARY") && B::trim(claves.at("TEMPORARY")) == "1") {
        e.montajesLectura.push_back({u.connection, dataset, ruta});
        std::fprintf(stderr, TC("t_montaje_ro_hecho",
                                "%s no está montado: se monta en solo lectura en %s mientras se "
                                "use, sin cambiar su punto de montaje\n"),
                     dataset.c_str(), ruta.c_str());
    }
    return true;
}

// Suelta los montajes temporales que ya no hacen falta: todos, o los que no contienen la
// posición. Uno ocupado —un trabajo de copia que aún lee de ahí— se queda para la próxima.
void sueltaMontajesLectura(Estado& e, bool todos) {
    std::vector<Estado::MontajeLectura> quedan;
    for (const auto& m : e.montajesLectura) {
        const bool dentro = B::toLowerAscii(e.actual.connection) == B::toLowerAscii(m.conexion)
                            && e.actual.dataset == m.dataset;
        if (!todos && dentro) {
            quedan.push_back(m);
            continue;
        }
        ZfsmUrl u;
        u.kind = ZfsmKind::Connection;
        u.connection = m.conexion;
        std::string out;
        std::string fallo;
        if (!agente(e, u, PET::unmountReadOnlyTemporary(m.dir), out, 30000, &fallo)) {
            if (todos) {
                std::fprintf(stderr, TC("t_montaje_ro_queda",
                                        "%s sigue montado en solo lectura en %s (%s); se suelta "
                                        "con «umount %s» o al reiniciar\n"),
                             m.dataset.c_str(), m.dir.c_str(), fallo.c_str(), m.dir.c_str());
            } else {
                quedan.push_back(m);
            }
        }
    }
    e.montajesLectura = quedan;
}

bool rutaDeContenido(Estado& e, const ZfsmUrl& destino, std::string& base, bool& esWindows,
                     bool callado) {
    std::string error;
    const auto* p = perfilVivoDe(e, destino, error);
    if (!p) {
        if (!callado) {
            std::fprintf(stderr, "%s\n", error.c_str());
        }
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
    // ¿Está MONTADO? Un dataset sin montar conserva su `mountpoint`, y ese directorio
    // puede existir y tener cosas dentro que no son suyas: `fc16/user` dice
    // `/Users/linarese` estando desmontado, así que `ls #content` enseñaba el /Users de la
    // máquina como si fuera el contenido del dataset. Es peor que un error: es una lista
    // creíble y ajena.
    //
    // Se pregunta por el DATASET aunque se venga de una instantánea: el contenido de una
    // instantánea se lee bajo el `.zfs` del dataset, que también hace falta montado.
    base.clear();
    esWindows = T::isWindowsConnection(*p);
    if (esWindows) {
        std::string montado;
        if (!agente(e, destino, PET::datasetProperty("mounted", destino.dataset), montado)) {
            return false;
        }
        if (B::trim(montado) == "no") {
            if (!callado) {
                std::fprintf(stderr, TC("t_no_montado_content", "%s no está montado: no hay contenido que "
                             "listar\n"), destino.dataset.c_str());
            }
            return false;
        }
        std::string letra;
        if (!agente(e, destino, PET::datasetProperty("driveletter", destino.pool), letra)) {
            return false;
        }
        letra = B::trim(letra);
        if (letra.empty() || letra == "-" || letra == "none") {
            if (!callado) {
                std::fprintf(stderr, TC("t_el_pool_s__020fd2", "el pool %s no tiene letra de unidad asignada\n"), destino.pool.c_str());
            }
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
        // A QUIÉN se le pregunta el punto de montaje. Para una INSTANTÁNEA, al DATASET y no
        // a ella: `zfs get mountpoint pool/ds@s` contesta «-» SIEMPRE, y ese «-» caía en la
        // rama de abajo y salía «no está montado en ningún sitio» aunque el contenido fuera
        // perfectamente alcanzable. Medido en unib: con `ppal/user/srvs` montado en
        // /home/linarese/srvs, `/home/linarese/srvs/.zfs/snapshot/--1` existe y se recorre.
        //
        // El apaño de más abajo —añadir «/.zfs/snapshot/<nombre>»— ya estaba escrito, pero
        // era inalcanzable: la consulta fallaba antes de llegar a él.
        // Un VOLUMEN no es que no esté montado: es que no se monta nunca. Es un dispositivo
        // de bloques, y lo que haya dentro —si es que hay un sistema de ficheros— lo sabe
        // quien lo formateó, no ZFS. Decir «no está montado» manda a buscar un `zfs mount`
        // que no existe para esto. Para una instantánea se pregunta por su DATASET.
        std::string tipo;
        if (agente(e, destino, PET::datasetProperty("type", destino.dataset), tipo)
            && B::toLowerAscii(B::trim(tipo)) == "volume") {
            if (!callado) {
                std::fprintf(stderr,
                             TC("t_es_un_volumen",
                                "%s es un VOLUMEN: un dispositivo de bloques, no tiene "
                                "ficheros que listar\n"),
                             destino.zfsName().c_str());
            }
            return false;
        }
        // Dónde está montado DE VERDAD el dataset —también el de una instantánea, cuyo
        // contenido se lee bajo su `.zfs`—. Si no lo está, se monta en solo lectura en un
        // temporal para mirarlo: ver dondeEstaMontado.
        if (!dondeEstaMontado(e, destino, destino.dataset, /*paraEscribir=*/false, callado, base)) {
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
    return true;
}



// =====================================================================================
// --- Ficheros: `#content/…` dentro de un dataset y `#path/…` en la máquina.
//
// Las dos secciones nombran un sitio del sistema de ficheros. `ubicacionDe` es la única
// puerta para saber qué hay ahí y a qué dataset pertenece: la usan `ls`, `cd`, `info` y la
// equivalencia con un dataset de `prepara`.

bool esSeccionDeFicheros(const ZfsmUrl& u) {
    const std::string sec = B::toLowerAscii(u.section);
    return sec == B::zfsmSection::kContent || sec == B::zfsmSection::kPath;
}

bool esRutaSuelta(const ZfsmUrl& u) {
    return B::toLowerAscii(u.section) == B::zfsmSection::kPath;
}

// La ruta en la máquina de una URL de ficheros.
bool rutaDeFicheros(Estado& e, const ZfsmUrl& u, std::string& ruta, bool callado) {
    if (esRutaSuelta(u)) {
        const CS::LoosePath lp = CS::loosePathOf(u);
        if (!lp.ok()) {
            if (!callado) {
                std::fprintf(stderr, "%s\n", CS::labelOf(lp.failure).c_str());
            }
            e.ultimoRc = 1;
            return false;
        }
        ruta = lp.absolute;
        return true;
    }
    bool esWindows = false;
    return rutaDeContenido(e, u, ruta, esWindows, callado);
}

struct Ubicacion {
    std::string ruta;
    bool existe{false};
    bool esDirectorio{false};   // siguiendo enlaces
    std::string tipo;           // d f l b c p s o
    long long tam{0};
    long long mtime{0};
    long long modo{0};
    std::string dueno;
    std::string grupo;
    std::string enlace;
    // El dataset montado que la contiene, si lo hay.
    std::string dataset;
    std::string relativa;
    std::string montaje;
    std::string instantanea;
    bool esMontaje{false};      // la ruta ES el punto de montaje (o la raíz de la instantánea)
};

bool ubicacionDe(Estado& e, const ZfsmUrl& u, Ubicacion& ub) {
    ub = Ubicacion{};
    if (!rutaDeFicheros(e, u, ub.ruta, /*callado=*/false)) {
        return false;
    }
    std::string out;
    if (!agente(e, u, PET::pathInfo(ub.ruta), out, 20000)) {
        return false;
    }
    B::json::Value v;
    std::string err;
    if (!B::json::parse(out, v, &err)) {
        std::fprintf(stderr, TC("t_contenido_ilegible", "respuesta ilegible al listar %s: %s\n"),
                     ub.ruta.c_str(), err.c_str());
        e.ultimoRc = 1;
        return false;
    }
    ub.existe = v["exists"].toBool();
    ub.esDirectorio = v["is_dir"].toBool();
    ub.tipo = v["type"].toString();
    ub.tam = v["size"].toInt();
    ub.mtime = v["mtime"].toInt();
    ub.modo = v["mode"].toInt();
    ub.dueno = v["owner"].toString();
    ub.grupo = v["group"].toString();
    ub.enlace = v["target"].toString();
    ub.dataset = v["dataset"].toString();
    ub.relativa = v["relative"].toString();
    ub.montaje = v["mountpoint"].toString();
    ub.instantanea = v["snapshot"].toString();
    ub.esMontaje = v["exact_mountpoint"].toBool();
    return true;
}

// La URL de un dataset (o de su instantánea) en una conexión.
ZfsmUrl urlDeDataset(const std::string& conexion, const std::string& dataset,
                     const std::string& instantanea) {
    ZfsmUrl d;
    d.connection = conexion;
    d.dataset = dataset;
    d.pool = dataset.substr(0, dataset.find('/'));
    d.snapshot = instantanea;
    d.kind = instantanea.empty() ? ZfsmKind::Dataset : ZfsmKind::Snapshot;
    return d;
}

// Un fichero donde la orden quiere un dataset, un pool o una instantánea.
//
// **Solo vale si ES el punto de montaje** (o la raíz de `.zfs/snapshot/<s>`), y la
// traducción se enseña antes de actuar. Estar DENTRO de un dataset no basta y nunca se sube
// al contenedor: `destroy` de un subdirectorio no puede acabar destruyendo el dataset. El
// error da la alternativa con `#content`, que es como se nombra esa ruta dentro de él.
//
// Lo mismo vale para `#content/sub` escrito a mano: antes `destroy ds#content/fotos` se
// leía como `destroy ds`, porque la sección no se miraba.
bool equivaleADataset(Estado& e, ZfsmUrl& u, const std::string& comoSeEscribio) {
    const std::string escrito = comoSeEscribio.empty() ? textoDe(u) : comoSeEscribio;
    if (!esRutaSuelta(u)) {
        if (u.detail.empty()) {
            u.section.clear();   // `ds#content` es la raíz del dataset: el dataset mismo
            return true;
        }
        ZfsmUrl d = u;
        d.section.clear();
        d.detail.clear();
        std::fprintf(stderr, TC("t_ruta_no_es_ds_dentro",
                                "%s no es un dataset; es una ruta dentro de %s\n"),
                     escrito.c_str(), textoDe(d).c_str());
        e.ultimoRc = 1;
        return false;
    }
    Ubicacion ub;
    if (!ubicacionDe(e, u, ub)) {
        return false;
    }
    if (!ub.dataset.empty() && ub.esMontaje) {
        const ZfsmUrl d = urlDeDataset(u.connection, ub.dataset, ub.instantanea);
        std::fprintf(stderr, "%s → %s\n", escrito.c_str(), textoDe(d).c_str());
        u = d;
        return true;
    }
    if (!ub.dataset.empty()) {
        ZfsmUrl d = urlDeDataset(u.connection, ub.dataset, ub.instantanea);
        ZfsmUrl c = d;
        c.section = B::zfsmSection::kContent;
        c.detail = B::split(ub.relativa, "/", true);
        std::fprintf(stderr, TC("t_ruta_no_es_ds_alternativa",
                                "%s no es un dataset; está dentro de %s: %s\n"),
                     escrito.c_str(), (ub.instantanea.empty() ? ub.dataset
                                                              : ub.dataset + "@" + ub.instantanea).c_str(),
                     textoDe(c).c_str());
    } else {
        std::fprintf(stderr, TC("t_ruta_fuera_de_ds",
                                "%s no es un dataset: no está dentro de ningún dataset montado\n"),
                     escrito.c_str());
    }
    e.ultimoRc = 1;
    return false;
}

// Una ruta de la máquina escrita donde la orden espera una ruta: «/var/tmp» o
// `zfsm://maq#path/var/tmp`. Con la URL, la máquina va dentro, y tiene que ser la misma en
// la que actúa la orden (`maquina`); vacía, se acepta cualquiera y se devuelve en
// `conexion`. El texto plano se devuelve tal cual: es la ruta en la máquina de la orden.
bool rutaDeRanura(Estado& e, const std::string& textoEntrada, const std::string& maquina,
                  std::string& ruta, std::string* conexion = nullptr) {
    const std::string texto = B::trim(textoEntrada);
    if (conexion) {
        *conexion = maquina;
    }
    if (!B::startsWith(B::toLowerAscii(texto), "zfsm://") && (texto.empty() || texto.front() != '#')) {
        ruta = texto;
        return true;
    }
    ZfsmUrl u;
    std::string err;
    if (!resuelve(e, texto, u, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        e.ultimoRc = 2;
        return false;
    }
    if (!esRutaSuelta(u)) {
        std::fprintf(stderr, TC("t_ranura_no_es_ruta",
                                "«%s» no es una ruta de la máquina: se escribe «/ruta» o "
                                "«zfsm://<máquina>#path/ruta»\n"),
                     texto.c_str());
        e.ultimoRc = 2;
        return false;
    }
    const CS::LoosePath lp = CS::loosePathOf(u);
    if (!lp.ok()) {
        std::fprintf(stderr, "%s\n", CS::labelOf(lp.failure).c_str());
        e.ultimoRc = 2;
        return false;
    }
    if (!maquina.empty() && B::toLowerAscii(lp.connection) != B::toLowerAscii(maquina)) {
        std::fprintf(stderr, TC("t_ranura_otra_maquina",
                                "«%s» es de %s, y la orden actúa en %s\n"),
                     texto.c_str(), lp.connection.c_str(), maquina.c_str());
        e.ultimoRc = 2;
        return false;
    }
    ruta = lp.absolute;
    if (conexion) {
        *conexion = lp.connection;
    }
    return true;
}

// Discos o ficheros de un pool: cada uno, como `rutaDeRanura` en la máquina del pool. Las
// palabras de topología —`mirror`, `raidz2`, `log`…— pasan tal cual, igual que «/dev/sdb».
bool discosDe(Estado& e, const std::vector<std::string>& textos, const std::string& maquina,
              std::vector<std::string>& out) {
    out.clear();
    for (const std::string& t : textos) {
        std::string ruta;
        if (!rutaDeRanura(e, t, maquina, ruta)) {
            return false;
        }
        out.push_back(ruta);
    }
    return true;
}

// Los permisos como los enseña `ls -l`: «drwxr-xr-x».
std::string modoComoLs(const std::string& tipo, long long modo) {
    std::string r(1, tipo.empty() || tipo == "f" ? '-' : tipo[0]);
    const char* letras = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i) {
        r += (modo & (1LL << (8 - i))) ? letras[i] : '-';
    }
    return r;
}

std::string fechaCorta(long long segundos) {
    if (segundos <= 0) {
        return {};
    }
    const std::time_t t = static_cast<std::time_t>(segundos);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::string nombreDeTipo(const std::string& letra) {
    switch (letra.empty() ? 'o' : letra[0]) {
        case 'd': return "directory";
        case 'f': return "file";
        case 'l': return "symlink";
        case 'b': return "block";
        case 'c': return "char";
        case 'p': return "fifo";
        case 's': return "socket";
        default: return "other";
    }
}

// La tabla de un directorio cualquiera, o la fila de un fichero.
bool imprimeFicheros(Estado& e, const std::vector<B::json::Value>& entradas) {
    Tabla t;
    t.nombreJson = "entries";
    t.cabecerasTexto = {T("t_cab_nombre", "NOMBRE"),   T("t_cab_tipo", "TIPO"),
                        T("t_cab_tamano", "TAMAÑO"),   T("t_cab_permisos", "PERMISOS"),
                        T("t_cab_dueno", "DUEÑO"),     T("t_cab_grupo", "GRUPO"),
                        T("t_cab_modificado", "MODIFICADO")};
    t.campos = {"name", "type", "size", "mode", "owner", "group", "modified"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Bytes,  Tipo::Cadena,
               Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    std::vector<B::json::Value> orden = entradas;
    std::sort(orden.begin(), orden.end(), [](const B::json::Value& a, const B::json::Value& b) {
        return a["name"].toString() < b["name"].toString();
    });
    for (const B::json::Value& v : orden) {
        const std::string tipo = v["type"].toString();
        std::string nombre = v["name"].toString();
        if (!v["target"].toString().empty()) {
            nombre += " -> " + v["target"].toString();
        }
        t.filas.push_back({nombre, nombreDeTipo(tipo),
                           tipo == "f" ? std::to_string(v["size"].toInt()) : "",
                           // Sin bits de modo —Windows— se deja en blanco: «----------»
                           // diría «sin permisos», que es otra cosa.
                           v["mode"].toInt() != 0 ? modoComoLs(tipo, v["mode"].toInt()) : "",
                           v["owner"].toString(), v["group"].toString(),
                           fechaCorta(v["mtime"].toInt())});
    }
    if (!aplicaFiltro(e, t)) {
        return false;
    }
    t.imprime(e.formato);
    return true;
}

// `ls` de `#path/…` o de `#content/…`: el directorio, o la fila del fichero si es un
// fichero. Las dos secciones se listan igual —nombre, tipo, tamaño, permisos, dueño, grupo y
// fecha—: antes `#content` daba solo las tres primeras y no sabía listar un fichero.
bool listaFicheros(Estado& e, const ZfsmUrl& destino) {
    std::string ruta;
    if (!rutaDeFicheros(e, destino, ruta, /*callado=*/false)) {
        return false;
    }
    std::string out;
    std::string errOrden;
    if (agente(e, destino, PET::pathContents(ruta), out, 60000, &errOrden)) {
        B::json::Value raiz;
        std::string err;
        if (!B::json::parse(out, raiz, &err)) {
            std::fprintf(stderr, TC("t_contenido_ilegible", "respuesta ilegible al listar %s: %s\n"),
                         ruta.c_str(), err.c_str());
            e.ultimoRc = 1;
            return false;
        }
        std::vector<B::json::Value> entradas;
        for (const auto& v : raiz["entries"].toArray()) {
            entradas.push_back(v);
        }
        return imprimeFicheros(e, entradas);
    }
    // No es un directorio: la fila del fichero, como `ls` de un fichero en una shell.
    Ubicacion ub;
    e.ultimoRc = 0;
    if (!ubicacionDe(e, destino, ub)) {
        return false;
    }
    if (!ub.existe) {
        std::fprintf(stderr, TC("t_no_existe__608f9e", "no existe: %s\n"), ruta.c_str());
        e.ultimoRc = 1;
        return false;
    }
    B::json::Value v;
    const std::size_t barra = ruta.find_last_of("/\\");
    v.set("name", B::json::Value(barra == std::string::npos ? ruta : ruta.substr(barra + 1)));
    v.set("type", B::json::Value(ub.tipo));
    v.set("size", B::json::Value(ub.tam));
    v.set("mode", B::json::Value(ub.modo));
    v.set("owner", B::json::Value(ub.dueno));
    v.set("group", B::json::Value(ub.grupo));
    v.set("mtime", B::json::Value(ub.mtime));
    if (!ub.enlace.empty()) {
        v.set("target", B::json::Value(ub.enlace));
    }
    return imprimeFicheros(e, {v});
}

// `info` de un fichero: qué es, de quién, y a qué dataset pertenece.
bool infoDeRuta(Estado& e, const ZfsmUrl& destino) {
    Ubicacion ub;
    if (!ubicacionDe(e, destino, ub)) {
        return false;
    }
    if (!ub.existe) {
        std::fprintf(stderr, TC("t_no_existe__608f9e", "no existe: %s\n"), ub.ruta.c_str());
        e.ultimoRc = 1;
        return false;
    }
    Tabla t;
    t.cabecerasTexto = {T("t_cab_campo", "CAMPO"), T("t_cab_valor", "VALOR")};
    t.campos = {"field", "value"};
    t.filas.push_back({"url", textoDe(destino)});
    t.filas.push_back({"path", ub.ruta});
    t.filas.push_back({"kind", nombreDeTipo(ub.tipo)});
    if (!ub.enlace.empty()) {
        t.filas.push_back({"target", ub.enlace});
    }
    if (ub.tipo == "f") {
        t.filas.push_back({"size", std::to_string(ub.tam)});
    }
    if (ub.modo != 0) {
        t.filas.push_back({"mode", modoComoLs(ub.tipo, ub.modo)});
    }
    if (!ub.dueno.empty()) {
        t.filas.push_back({"owner", ub.dueno});
    }
    if (!ub.grupo.empty()) {
        t.filas.push_back({"group", ub.grupo});
    }
    t.filas.push_back({"modified", fechaCorta(ub.mtime)});
    if (!ub.dataset.empty()) {
        const ZfsmUrl d = urlDeDataset(destino.connection, ub.dataset, ub.instantanea);
        t.filas.push_back({"dataset", textoDe(d)});
        if (ub.esMontaje) {
            t.filas.push_back({"mountpoint_of", textoDe(d)});
        } else {
            ZfsmUrl c = d;
            c.section = B::zfsmSection::kContent;
            c.detail = B::split(ub.relativa, "/", true);
            t.filas.push_back({"content_url", textoDe(c)});
        }
    }
    t.imprime(e.formato);
    return true;
}


bool cmdLs(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    const std::string sec = B::toLowerAscii(destino.section);
    if (sec == B::zfsmSection::kContent || sec == B::zfsmSection::kPath) {
        return listaFicheros(e, destino);
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
    switch (nodoDe(destino)) {
        case Nodo::Raiz:
            return listaConexiones(e, pet);
        case Nodo::Conexion:
            return listaPools(e, destino);
        case Nodo::Dataset:
            return listaDataset(e, destino);
        case Nodo::Snapshot:
            // Una instantánea no tiene hijos; lo interesante es lo que hay dentro.
            std::fputs(TC("t_una_instan_e727ff", "un snapshot no tiene hijos: pruebe «ls #content» o «ls #properties»\n"), stderr);
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
        if (!ejecutarAgente(*e.ses, *p, PET::datasetExists(destino.zfsName()), out, err, rc,
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
    // Dentro de los ficheros, lo mismo que haría cualquier shell: que exista y que sea un
    // directorio. Antes `cd zfsm://m#path/loquesea` se aceptaba sin mirar.
    if (esSeccionDeFicheros(destino)) {
        Ubicacion ub;
        if (!ubicacionDe(e, destino, ub)) {
            return false;
        }
        if (!ub.existe) {
            std::fprintf(stderr, TC("t_no_existe__608f9e", "no existe: %s\n"), ub.ruta.c_str());
            e.ultimoRc = 1;
            return false;
        }
        if (!ub.esDirectorio) {
            std::fprintf(stderr, TC("t_no_es_directorio", "no es un directorio: %s\n"),
                         ub.ruta.c_str());
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
bool zfsGeneric(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv) {
    // El empaquetado lo hace la capa base: es cómo espera el daemon los argumentos.
    std::string out;
    return agente(e, destino, PET::zfsGeneric(B::helpers::agentArgv(argv)), out);
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
        std::fprintf(stderr, TC("t_bandera_no_instantanea", "«%s» no vale al crear un snapshot: "
                     "solo -r\n"), n.c_str());
        return false;
    }
    std::string out;
    if (!agente(e, destino,
                INST::argvCreateSnapshot(destino.dataset, nombre, recursivo),
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
        if (!zpoolGeneric(e, destino, argv)) {
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
                INST::argvDestroy(objetivo, pet.tiene("-f"),
                                   pet.tiene("-R")   ? INST::Scope::Dependents
                                   : pet.tiene("-r") ? INST::Scope::Descendants
                                                     : INST::Scope::Only),
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
                INST::argvRollback(destino.zfsName(), pet.tiene("-f"),
                                   pet.tiene("-R")   ? INST::Scope::Dependents
                                   : pet.tiene("-r") ? INST::Scope::Descendants
                                                     : INST::Scope::Only),
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
        std::fputs(TC("t_uso_clone__ea5ed7", "uso: clone <nuevo-dataset> [--from <@snapshot>]\n"), stderr);
        return false;
    }
    const ZfsmUrl& origen = pet.objetivo;
    if (origen.snapshot.empty()) {
        std::fprintf(stderr, TC("t_el_origen__604ce4", "el origen tiene que ser un snapshot (ahora: %s)\n"), textoDe(origen).c_str());
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
    if (!agente(e, origen, INST::argvClone(origen.zfsName(), nuevo), out)) {
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
        if (p.name.empty() && !pide(TC("t_p_nombre_visible", "Nombre visible"), p.id, p.name)) return false;
        if (p.connType.empty() && !pide(TC("t_p_tipo_conexion", "Tipo (LOCAL/SSH)"), "SSH", p.connType)) return false;
        p.connType = B::toUpperAscii(p.connType);
        if (p.osType.empty() && !pide(TC("t_p_sistema_operativo", "Sistema operativo"), "Linux", p.osType)) return false;
    }
    if (p.name.empty()) p.name = p.id;
    if (p.connType.empty()) p.connType = "SSH";
    if (p.osType.empty()) p.osType = "Linux";

    const bool local = B::toLowerAscii(p.connType) == "local";
    if (!local) {
        if (p.host.empty() && interactivo && !pide(TC("t_p_host", "Host"), "", p.host)) return false;
        if (p.host.empty()) {
            std::fputs(TC("t_una_conexi_86be2f", "una conexión SSH necesita un host\n"), stderr);
            return false;
        }
        if (p.username.empty() && interactivo && !pide(TC("t_p_usuario", "Usuario"), "", p.username)) return false;
        if (p.keyPath.empty() && interactivo && !pide(TC("t_p_ruta_clave", "Ruta de clave SSH (vacío = contraseña)"), "",
                                                      p.keyPath)) {
            return false;
        }
        // Una clave que no está no se da de alta: la conexión «funcionaría» entrando por
        // otro lado y arrastraría la errata hasta el día que deje de hacerlo.
        std::string ruta;
        if (sshClaveFalta(p, ruta)) {
            std::fprintf(stderr,
                         TC("t_clave_no_existe_alta",
                            "la clave SSH «%s» no existe o no se puede leer: no se da de alta la "
                            "conexión\n"),
                         ruta.c_str());
            e.ultimoRc = 1;
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
            // Es una maestra NUEVA —si hubiera otra, ya se habría pedido al entrar—, así que
            // se pide dos veces, como en `master-password`: sin eco, un dedazo dejaría
            // cifrado con algo que nadie conoce.
            std::string maestra;
            std::string otra;
            if (!preguntarSecretoPorTerminal(T("t_p_maestra_nueva", "Contraseña maestra nueva: "),
                                             maestra, err)
                || !preguntarSecretoPorTerminal(T("t_mp_p2", "Repítala: "), otra, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return false;
            }
            const bool iguales = maestra == otra;
            for (char& c : otra) { c = '\0'; }
            if (!iguales || maestra.empty()) {
                for (char& c : maestra) { c = '\0'; }
                std::fputs(iguales ? TC("t_mp_vacia", "la contraseña maestra no puede quedar vacía\n")
                                   : TC("t_maestras_no_coinciden",
                                        "las dos contraseñas no coinciden; no se ha creado nada\n"),
                           stderr);
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
    // El de ANTES, para limpiar lo que la sesión recordaba de él: si cambia la dirección, lo
    // recordado va con la clave vieja y su túnel seguiría abierto contra la máquina vieja.
    const B::ConnectionProfile antes = *actual;
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
        std::string ruta;
        if (sshClaveFalta(p, ruta)) {
            std::fprintf(stderr,
                         TC("t_clave_no_existe_edit",
                            "la clave SSH «%s» no existe o no se puede leer: no se guarda el "
                            "cambio\n"),
                         ruta.c_str());
            e.ultimoRc = 1;
            return false;
        }
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

    // La contraseña, PREGUNTADA como los demás campos.
    //
    // Antes solo se tocaba con `--password-fd` o con `--password`, así que un `edit mbp` a
    // secas recorría nombre, tipo, sistema, host, usuario, clave, puerto y sudo… y se
    // saltaba la contraseña sin decir nada. Quien quería cambiarla —el caso normal: la de
    // sudo de una máquina cambió y esta copia se quedó con la vieja— repasaba el
    // cuestionario entero, leía «actualizada la conexión mbp» y seguía con la de antes.
    //
    // El motivo que había era bueno y se conserva: en una edición, dejarla en blanco tiene
    // que CONSERVARLA, no borrarla. Pero eso no obliga a no preguntar, obliga a que el
    // vacío signifique «la de antes» — que es justo lo que `pide()` hace con todos los
    // demás campos. La diferencia es que aquí no se puede enseñar el valor actual entre
    // corchetes, así que lo que se dice es si HAY una guardada o no.
    const std::string fdTexto = pet.valor("password-fd");
    if (!fdTexto.empty()) {
        std::string err;
        if (!leerSecretoDeDescriptor(std::atoi(fdTexto.c_str()), p.password, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
    } else if (interactivo) {
        std::string err;
        std::string clave;
        const std::string aviso =
            p.password.empty()
                ? T("t_p_pass_nueva_sin", "Contraseña (no hay ninguna guardada; Intro = seguir sin ella): ")
                : T("t_p_pass_nueva_con", "Contraseña (Intro = conservar la guardada): ");
        if (!preguntarSecretoPorTerminal(aviso, clave, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
        if (!clave.empty()) {
            p.password = clave;
        }
    }

    std::string error;
    if (!guardarConexion(*e.ses, p, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        for (char& c : p.password) { c = 0; }
        return false;
    }
    olvidaEstadoDeConexion(e, antes);
    olvidaVersionDaemon(e, p);

    // Si lo editado es la conexión LOCAL, guardar no basta: hay dos copias vivas de esas
    // credenciales y las dos se quedarían con las de antes.
    //
    //  - La sesión las recuerda para no preguntar en cada orden que eleve.
    //  - Y el material TLS del daemon local se leyó ELEVANDO con la contraseña anterior,
    //    así que el RPC seguiría usando lo cacheado.
    //
    // Sin esto, cambiar la contraseña de sudo parecía no surtir efecto hasta salir y
    // volver a entrar — que es justo el fallo que la interfaz comenta en su propio código.
    if (B::connjson::isLocalProfile(p)) {
        e.ses->sudoUsuario = p.username;
        e.ses->sudoClave = p.password;
        e.ses->sudoResuelto = !p.password.empty();
        T::clearLocalDaemonTlsCache();
    }
    const bool eraLocal = B::connjson::isLocalProfile(p);
    for (char& c : p.password) { c = 0; }
    recarga(e);
    std::fprintf(stderr, TC("t_actualizad_41b05c", "actualizada la conexión %s\n"), id.c_str());

    // Y se COMPRUEBAN, que es lo que uno quiere saber al cambiarlas: si la contraseña
    // nueva no eleva, enterarse ahora y no la próxima vez que haga falta de verdad.
    //
    // Esto solo se hacía en la conexión LOCAL, y es en las de SSH donde más falta hace. Una
    // contraseña de sudo equivocada en una máquina remota no da ningún error al guardarla:
    // lo que da es que MÁS TARDE no se pueda leer el material TLS de su daemon, y ese
    // mensaje —«no se pudo leer el material TLS del daemon»— no menciona sudo por ninguna
    // parte. Es exactamente lo que pasó con mbp visto desde mmela: la contraseña de esa
    // máquina había cambiado, la copia de mmela se quedó con la vieja, y el síntoma salía
    // tres pantallas más allá y hablando de otra cosa.
    const bool haySudo = p.useSudo || eraLocal;
    if (haySudo && (eraLocal ? e.ses->sudoResuelto : true)) {
        std::string out;
        std::string err;
        int rc = -1;
        const auto* vivo = buscarConexion(e.conns, id);
        if (vivo) {
            const B::ConnectionProfile perfil = conSudo(e, *vivo);
            if (T::runSsh(e.ses->transporte, perfil, H::withSudoCommand(perfil, "id -u"), 15000, out,
                          err, rc, {}, {}, {}, {}, /*allowAgentRpc=*/false,
                          /*echoOutputToLog=*/false)
                && rc == 0 && B::trim(out) == "0") {
                std::fputs(TC("t_sudo_ok", "las credenciales de sudo elevan correctamente\n"), stderr);
            } else {
                // Con el motivo, que es lo que convierte el aviso en algo accionable.
                //
                // La clave es NUEVA a propósito. La de antes decía «en esta máquina» y no
                // llevaba ningún `%s`; reutilizarla con dos marcas de formato haría que el
                // catálogo —que manda sobre el literal— devolviera el texto viejo y
                // `fprintf` se pusiera a leer argumentos que ese texto no pide. Es el mismo
                // fallo que se quitó de cuatro sitios hace dos días.
                const std::string detalle = B::trim(H::oneLine(err));
                const std::string cola = detalle.empty() ? std::string() : ": " + detalle;
                std::fprintf(stderr, TC("t_sudo_mal_en", "aviso: con esas credenciales NO se "
                             "consigue elevar en %s%s\n"), id.c_str(), cola.c_str());
            }
        }
    }
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
    ZfsmUrl destino = pet.objetivo;
    if (pet.lista("texto").empty() && nodoDe(destino) != Nodo::Raiz) {
        std::fputs(TC("t_uso_create_c8fa17", "uso: create <nombre> [prop=valor...]\n"), stderr);
        return false;
    }
    // El mismo verbo en los tres niveles, porque es la misma idea —crear un nodo donde uno
    // está—: en la RAÍZ una conexión, en una CONEXIÓN un pool, en un DATASET un hijo.
    //
    // Pero **el nivel no lo decide solo dónde uno está: también la forma del nombre**. Un
    // nombre con barra no puede ser el de una conexión, así que desde la raíz
    // `create unibody/sback/tmp` no es «una conexión que se llama así» —lo era, y se
    // intentaba dar de alta— sino «el dataset sback/tmp en unibody». La regla vive en
    // `creacion::queSeCrea`, entera y con sus pruebas.
    namespace CR = creacion;
    const CR::Nivel nivel = nodoDe(destino) == Nodo::Raiz       ? CR::Nivel::Raiz
                            : nodoDe(destino) == Nodo::Conexion ? CR::Nivel::Conexion
                                                                : CR::Nivel::Dataset;
    const CR::Decision d = CR::queSeCrea(nivel, pet.uno("texto"));
    if (!d.ruta.empty()) {
        // Solo hay ruta que resolver cuando el nombre traía delante el tramo de la
        // máquina. Se resuelve con la MISMA función que `cd`, para que «la conexión no
        // existe» se diga igual en las dos.
        ZfsmUrl base;
        std::string error;
        if (!resuelve(e, d.ruta, base, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
        destino = base;
    }
    if (d.que == CR::Objeto::Conexion) {
        return cmdCrearConexion(e, pet);
    }
    if (d.que == CR::Objeto::Pool) {
        return cmdCrearPool(e, pet, destino, d.ruta.empty() ? std::string() : d.nombre);
    }
    if (d.que == CR::Objeto::Dataset && destino.dataset.empty()) {
        // El destino es una CONEXIÓN —se llegó desde la raíz con ruta, o se estaba ya en
        // ella—, así que se le cuelga el nombre ZFS completo y el resto del camino, que ya
        // sabe tratar un nombre con ruta, funciona igual. La condición mira el destino y
        // no si hubo ruta: estando en `zfsm://unibody`, `create sback/tmp` no trae ruta
        // que resolver y necesita esto lo mismo.
        destino.dataset = d.nombre;
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
    const std::string hijo = d.nombre;
    argv.push_back(hijo.find('/') == std::string::npos ? destino.dataset + "/" + hijo : hijo);
    if (!zfsGeneric(e, destino, argv)) {
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
    // La regla —sin barra se conserva el padre— vive en `commands::datasets`, compartida con
    // el servidor web, que no la tenía.
    const std::vector<std::string> argvRen =
        DS::argvRename(destino.dataset, pet.uno("texto"));
    if (argvRen.empty()) {
        std::fputs(TC("t_rename_invalido", "ese nombre no sirve para renombrar\n"), stderr);
        return false;
    }
    const std::string completo = argvRen.back();
    if (!zfsGeneric(e, destino, argvRen)) {
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
    if (!zfsGeneric(e, destino, argv)) {
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
    if (!zfsGeneric(e, destino, {"promote", destino.dataset})) {
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
    if (!zfsGeneric(e, destino, argv)) {
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

// --- Las instantáneas PROGRAMADAS (GSA).
//
// La programación son propiedades del dataset, y las REGLAS de lo que es válido están en
// la capa base —`src/base/gsa.h`—, compartidas con la interfaz gráfica. Aquí solo se
// hablan: leer lo que hay, escribir lo que se pide y enseñarlo.

// La salida cruda del agente: «dataset<TAB>propiedad<TAB>valor[<TAB>origen]», una línea por
// propiedad. Se agrupa por dataset y se descartan las HEREDADAS: una programación heredada
// no es de ese dataset, es del padre, y enseñarla como suya haría creer que hay dos.
std::map<std::string, std::map<std::string, std::string>> agrupaGsa(const std::string& crudo) {
    std::map<std::string, std::map<std::string, std::string>> out;
    for (const std::string& linea : B::split(crudo, "\n", true)) {
        const std::vector<std::string> campos = B::split(linea, "\t", false);
        if (campos.size() < 3) {
            continue;
        }
        const std::string origen = campos.size() > 3 ? B::toLowerAscii(B::trim(campos[3])) : "";
        if (B::startsWith(origen, "inherited")) {
            continue;
        }
        const std::string valor = B::trim(campos[2]);
        if (valor.empty() || valor == "-") {
            continue;
        }
        out[B::trim(campos[0])][B::trim(campos[1])] = valor;
    }
    return out;
}

// Lo programado bajo un dataset o un pool, ya convertido a estructuras.
bool leeProgramaciones(Estado& e, const ZfsmUrl& destino, const std::string& raiz,
                       std::vector<B::gsa::Entry>& out) {
    std::string crudo;
    if (!agente(e, destino, PET::gsaOfDataset(raiz), crudo, 30000)) {
        return false;
    }
    for (const auto& kv : agrupaGsa(crudo)) {
        B::gsa::Schedule p;
        B::gsa::Reason m;
        if (!B::gsa::fromProperties(kv.second, p, m)) {
            // Un valor que no es un entero está PUESTO en la máquina: no se puede corregir
            // desde aquí, pero callarlo dejaría una fila que miente.
            std::fprintf(stderr, TC("t_sch_valor_malo", "aviso: %s tiene %s con un valor que no es un "
                         "número; se ignora esa programación\n"), kv.first.c_str(), m.detail.c_str());
            continue;
        }
        out.push_back({kv.first, p});
    }
    return true;
}

Tabla tablaDeProgramaciones(const std::vector<std::pair<std::string, B::gsa::Entry>>& filas) {
    Tabla t;
    t.nombreJson = "schedules";
    t.cabecerasTexto = {T("t_cab_conexion", "CONEXIÓN"), T("t_cab_dataset", "DATASET"),
                        T("t_cab_activa", "ACTIVA"), T("t_cab_recursiva", "RECURSIVA"),
                        T("t_cab_hora", "HORA"), T("t_cab_dia", "DÍA"), T("t_cab_sem", "SEM"),
                        T("t_cab_mes", "MES"), T("t_cab_ano", "AÑO"),
                        T("t_cab_destino", "DESTINO")};
    t.campos = {"connection", "dataset", "enabled", "recursive",
                "hourly", "daily", "weekly", "monthly", "yearly", "destination"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Booleano, Tipo::Booleano,
               Tipo::Entero, Tipo::Entero, Tipo::Entero, Tipo::Entero, Tipo::Entero,
               Tipo::Cadena};
    for (const auto& f : filas) {
        const B::gsa::Schedule& p = f.second.schedule;
        t.filas.push_back({f.first, f.second.dataset, p.enabled ? "true" : "false",
                           p.recursive ? "true" : "false", std::to_string(p.hourly),
                           std::to_string(p.daily), std::to_string(p.weekly),
                           std::to_string(p.monthly), std::to_string(p.yearly),
                           p.target.empty() ? "-" : p.target});
    }
    return t;
}

// La OTRA máquina que nombra una de estas órdenes de dos extremos.
const B::ConnectionProfile* laOtra(Estado& e, const Peticion& pet, std::string& error) {
    const std::string nombre = pet.uno("texto");
    const auto* p = buscarConexion(e.conns, nombre);
    if (!p) {
        error = B::format(T("t_no_conn_nombre", "no hay ninguna conexión llamada «%1»"), {nombre});
        return nullptr;
    }
    return p;
}

// Autorizar la clave SSH de esta máquina en otra.
//
// NO pasa por el daemon, y no puede: esto es lo que hay que hacer ANTES de que haya
// daemon, porque `install-daemon` entra por SSH. Es de las pocas cosas que siguen
// mandando shell, y por un motivo que no va a desaparecer.
bool cmdAuthorizeKey(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    std::string error;
    const auto* origen = perfilVivoDe(e, pet.objetivo, error);
    if (!origen) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    const auto* destino = laOtra(e, pet, error);
    if (!destino) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    if (B::toLowerAscii(origen->id) == B::toLowerAscii(destino->id)) {
        std::fputs(TC("t_ak_misma", "el origen y el destino son la misma conexión\n"), stderr);
        return false;
    }

    // 1) La clave pública del ORIGEN. Se prueban las de siempre, en el orden en que las
    //    genera la gente.
    std::string clave;
    std::string err;
    int rc = -1;
    const std::string leerClave =
        "cat ~/.ssh/id_ed25519.pub 2>/dev/null "
        "|| cat ~/.ssh/id_rsa.pub 2>/dev/null "
        "|| cat ~/.ssh/id_ecdsa.pub 2>/dev/null "
        "|| cat ~/.ssh/id_*.pub 2>/dev/null | head -1";
    if (!T::runSsh(e.ses->transporte, *origen, H::withUnixSearchPathCommand(leerClave), 15000,
                   clave, err, rc, {}, {}, {}, {}, /*allowAgentRpc=*/false,
                   /*echoOutputToLog=*/e.ses->verboso)
        || rc != 0 || B::trim(clave).empty()) {
        std::fprintf(stderr, TC("t_ak_sin_clave", "%s no tiene clave pública SSH.\n"
                     "  Genérela allí con: ssh-keygen -t ed25519\n"), origen->id.c_str());
        return false;
    }
    clave = B::trim(B::split(B::trim(clave), "\n", true).front());

    if (!confirma(e, B::format(T("t_conf_authorize_key",
                                 "Se va a autorizar la clave SSH de %1 en %2: después, %1 podrá "
                                 "entrar en %2 sin contraseña. ¿Continuar?"),
                               {origen->id, destino->id}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }

    // 2) Y se añade allí. Idempotente: si ya está, no se duplica.
    const std::string instalar =
        "mkdir -p ~/.ssh && chmod 700 ~/.ssh && "
        "(grep -qF " + B::shSingleQuote(clave) + " ~/.ssh/authorized_keys 2>/dev/null "
        " || printf '\\n%s\\n' " + B::shSingleQuote(clave) + " >> ~/.ssh/authorized_keys) && "
        "chmod 600 ~/.ssh/authorized_keys";
    std::string out;
    if (!T::runSsh(e.ses->transporte, *destino, H::withUnixSearchPathCommand(instalar), 15000, out,
                   err, rc, {}, {}, {}, {}, /*allowAgentRpc=*/false,
                   /*echoOutputToLog=*/e.ses->verboso)
        || rc != 0) {
        const std::string detalle = B::trim(err).empty() ? B::trim(out) : B::trim(err);
        std::fprintf(stderr, TC("t_ak_fallo", "no se pudo autorizar la clave en %s: %s\n"),
                     destino->id.c_str(), detalle.c_str());
        return false;
    }
    std::fprintf(stderr, TC("t_ak_hecho", "clave de %s autorizada en %s\n"), origen->id.c_str(),
                 destino->id.c_str());
    return true;
}

// El almacén de confianza, a otra máquina.
//
// Es para el CLIENTE que alguien abra allí, no para su daemon: eso es «peers --push». Van
// a sitios distintos —el perfil del usuario frente a /etc— y sirven para cosas distintas.
bool cmdExportTrust(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    std::string error;
    const auto* destino = laOtra(e, pet, error);
    if (!destino) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    if (T::isLocalConnection(*destino)) {
        std::fputs(TC("t_et_local", "la conexión Local ya usa el almacén de esta máquina\n"), stderr);
        return false;
    }
    B::store::Warning aviso;
    const B::json::Value almacen = B::store::readTrustStore(e.ses->dirConfig, aviso);
    if (!aviso.empty()) {
        std::fputs(TC("t_et_sin_almacen", "no se pudo leer el almacén de confianza\n"), stderr);
        return false;
    }
    const std::string carga = B::json::toIndented(almacen);
    if (B::trim(carga).empty() || carga == "{}") {
        std::fputs(TC("t_et_vacio", "el almacén de confianza está vacío\n"), stderr);
        return false;
    }
    if (!confirma(e, B::format(T("t_conf_export_trust",
                                 "Se va a copiar el almacén de confianza a %1. Lleva CLAVES "
                                 "PRIVADAS: quien las tenga podrá hablar con esos daemons como "
                                 "usted. ¿Continuar?"),
                               {destino->id}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    // Por la ENTRADA ESTÁNDAR, no por argumento: el contenido lleva claves y un argumento
    // se ve en `ps` en la máquina de destino.
    const std::string guion =
        "cfg=\"${XDG_CONFIG_HOME:-$HOME/.config}/ZFSMgr\"; mkdir -p \"$cfg\" && "
        "umask 077 && cat > \"$cfg/trust-store.json\" && chmod 600 \"$cfg/trust-store.json\" && "
        "printf 'TRUST=%s\\n' \"$cfg/trust-store.json\"";
    std::string out;
    std::string err;
    int rc = -1;
    // El sexto parámetro opcional es la ENTRADA ESTÁNDAR; los tres anteriores son
    // devoluciones de llamada que aquí no se usan.
    if (!T::runSsh(e.ses->transporte, *destino, H::withUnixSearchPathCommand(guion), 20000, out,
                   err, rc, {}, {}, {}, carga, /*allowAgentRpc=*/false,
                   /*echoOutputToLog=*/false)
        || rc != 0) {
        const std::string detalle = B::trim(err).empty() ? B::trim(out) : B::trim(err);
        std::fprintf(stderr, TC("t_et_fallo", "no se pudo copiar el almacén a %s: %s\n"),
                     destino->id.c_str(), detalle.c_str());
        return false;
    }
    std::fprintf(stderr, TC("t_et_hecho", "almacén de confianza copiado a %s\n"),
                 destino->id.c_str());
    return true;
}

// Los datasets que se quedaron en un punto de montaje temporal.
//
// Sin `--apply` no toca nada, y eso no es una comodidad: es que la orden útil de verdad es
// la que se ejecuta ANTES de decidir. El verbo del daemon ya está partido así.
bool cmdRepairMounts(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const bool aplicar = pet.tiene("--apply");
    if (aplicar
        && !confirma(e, B::format(T("t_conf_repair_mounts",
                                    "Se van a devolver a su punto de montaje los datasets que se "
                                    "quedaron en uno temporal en %1. ¿Continuar?"),
                                  {pet.objetivo.connection}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    std::string out;
    std::vector<std::string> argv = PET::repairAltMountpoints({});
    if (aplicar) {
        argv.push_back("apply");
    }
    if (!agente(e, pet.objetivo, argv, out, 60000)) {
        return false;
    }
    // «STRANDED=<dataset> saved=<punto> current=<punto>», una línea por dataset.
    Tabla t;
    t.nombreJson = "stranded";
    t.cabecerasTexto = {T("t_cab_dataset", "DATASET"), T("t_cab_deberia", "DEBERÍA ESTAR EN"),
                        T("t_cab_esta_en", "ESTÁ EN")};
    t.campos = {"dataset", "saved", "current"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    for (const std::string& l : B::split(out, "\n", true)) {
        if (!B::startsWith(l, "STRANDED=")) {
            continue;
        }
        const std::string resto = l.substr(9);
        const std::size_t s = resto.find(" saved=");
        const std::size_t c = resto.find(" current=");
        if (s == std::string::npos || c == std::string::npos || c < s) {
            continue;
        }
        t.filas.push_back({B::trim(resto.substr(0, s)),
                           B::trim(resto.substr(s + 7, c - s - 7)),
                           B::trim(resto.substr(c + 9))});
    }
    if (t.filas.empty()) {
        std::fprintf(stderr, TC("t_rm_ninguno", "no hay ningún dataset en un punto de montaje "
                     "temporal en %s\n"), pet.objetivo.connection.c_str());
        return true;
    }
    t.imprime(e.formato);
    if (aplicar) {
        std::fprintf(stderr, TC("t_rm_hecho", "devueltos a su sitio: %zu\n"), t.filas.size());
    } else {
        std::fputs(TC("t_rm_ensayo", "no se ha tocado nada: para devolverlos, «repair-mounts "
                     "--apply»\n"), stderr);
    }
    return true;
}

// Las credenciales de los pares: qué otras máquinas sabe alcanzar este daemon.
//
// Hace falta para NIVELAR una programación contra otra máquina, porque eso lo hace el
// daemon solo, de madrugada, sin nadie delante: las credenciales tienen que estar en la
// máquina. Y son las de la OTRA punta —cada daemon verifica a sus clientes contra su
// propio certificado—, que es justo lo que el cliente ya guarda por conexión.
bool cmdPeers(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (!pet.valor("listen").empty()) {
        const std::string dir = pet.valor("listen");
        if (!confirma(e, B::format(T("t_conf_peers_listen",
                                     "El daemon de %1 pasará a atender en %2 y se reiniciará.\n"
                                     "Con un comodín, su puerto queda alcanzable desde la red "
                                     "—protegido por mTLS con el certificado de cliente fijado—. "
                                     "¿Continuar?"),
                                   {destino.connection, dir}))) {
            std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
            return false;
        }
        std::string out;
        if (!agente(e, destino, PET::setBindAddress(dir), out, 20000)) {
            return false;
        }
        std::fprintf(stderr, TC("t_peers_bind", "%s atiende ahora en %s; su daemon se está "
                     "reiniciando\n"), destino.connection.c_str(), dir.c_str());
        return true;
    }
    if (!pet.tiene("--push")) {
        std::string out;
        if (!agente(e, destino, PET::peerList(), out, 20000)) {
            return false;
        }
        const PR::View vista = PR::parse(out);
        // Quién cree ser esa máquina va PRIMERO, y se dice también cuando falta.
        //
        // Su ausencia no rompe nada visible: rompe la nivelación GSA contra un dataset de la
        // propia máquina, en silencio y con un mensaje que culpa al par. Sin enseñarlo aquí
        // no había forma de dar con ello. Un daemon anterior a este cambio no lo emite, y
        // entonces se dice eso mismo en vez de fingir que está vacío.
        if (vista.self.empty()) {
            // Dos causas, y desde aquí NO se distinguen: que la máquina no tenga puesto el
            // dato, o que su daemon sea anterior a esta comprobación y por eso no lo diga.
            // El mensaje las nombra las dos en vez de afirmar la que suene peor: dar por
            // roto lo que solo está desactualizado manda a buscar donde no hay nada.
            std::fputs(TC("t_peers_sin_self",
                          "AVISO: esta máquina no ha dicho con qué nombre la llama usted.\n"
                          "       O no lo tiene puesto —y entonces NO puede nivelar contra un\n"
                          "       dataset suyo propio, sin avisar de ello—, o su daemon es\n"
                          "       anterior a esta comprobación y simplemente no lo informa.\n"
                          "       «peers --push» arregla lo primero; actualizar el daemon,\n"
                          "       lo segundo.\n"),
                       stderr);
        } else {
            std::fprintf(stderr, TC("t_peers_self", "se identifica como: %s\n"),
                         vista.self.c_str());
        }
        // La IDENTIDAD, que es lo que de verdad decide. El nombre de arriba es una etiqueta
        // de quien entregó las credenciales —por eso una máquina puede llamarse «local» a sí
        // misma y otra también—; esto no lo elige nadie.
        if (!vista.realUid.empty()) {
            if (vista.selfUid.empty()) {
                std::fprintf(stderr, TC("t_peers_uid_sin",
                                        "identidad de la máquina: %s (no se le entregó ninguna; "
                                        "decidirá por el nombre)\n"),
                             vista.realUid.c_str());
            } else if (B::toLowerAscii(vista.selfUid) == B::toLowerAscii(vista.realUid)) {
                std::fprintf(stderr, TC("t_peers_uid_ok", "identidad de la máquina: %s\n"),
                             vista.realUid.c_str());
            } else {
                // Las dos no coinciden. Esa configuración se escribió mirando a OTRA
                // máquina —un perfil copiado de otro equipo es la forma habitual— y hasta
                // ahora no había manera de verlo desde fuera.
                std::fprintf(stderr, TC("t_peers_uid_choca",
                                        "AVISO: esta máquina es %s, pero las credenciales que "
                                        "tiene se entregaron a %s.\n"
                                        "       Son máquinas distintas: vuelva a hacer "
                                        "«peers --push» sobre ella.\n"),
                             vista.realUid.c_str(), vista.selfUid.c_str());
            }
        }
        Tabla t;
        t.nombreJson = "peers";
        t.cabecerasTexto = {T("t_cab_id", "ID"), T("t_cab_host", "HOST"),
                            T("t_cab_puerto", "PUERTO"), T("t_cab_identidad", "IDENTIDAD")};
        t.campos = {"id", "host", "port", "uid"};
        t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Entero, Tipo::Cadena};
        for (const PR::Peer& par : vista.pares) {
            t.filas.push_back({par.id, par.host, std::to_string(par.puerto), par.uid});
        }
        t.imprime(e.formato);
        return true;
    }

    // Componer la carga es cosa de la capa base: la interfaz y el servidor web necesitan
    // exactamente lo mismo, y esto estaba solo aquí.
    // El uid de la máquina a la que se entrega. Sale de su perfil, que lo rellena `refresh`
    // preguntándoselo a ella. Si está vacío se dice: sin él la entrega funciona igual pero
    // esa máquina seguirá decidiendo «¿este destino soy yo?» por el nombre, que es lo que se
    // está intentando dejar atrás.
    std::string uidDeDestino;
    {
        const auto* pd = buscarConexion(e.conns, destino.connection);
        if (pd) {
            uidDeDestino = B::trim(pd->machineUid);
        }
        if (uidDeDestino.empty()) {
            std::fputs(TC("t_push_sin_uid",
                          "aviso: no se conoce el identificador de máquina de este destino.\n"
                          "       Se entrega igual, pero decidirá por el NOMBRE y no por su\n"
                          "       identidad. Un «refresh» sobre esa conexión lo averigua.\n"), stderr);
        }
    }
    const PR::Handover entrega =
        PR::composeHandover(e.conns.perfiles, destino.connection, uidDeDestino);
    if (!entrega.ok()) {
        std::fprintf(stderr, "%s\n", PR::labelOf(entrega.failure).c_str());
        return false;
    }
    const std::vector<std::string>& nombres = entrega.names;
    if (!confirma(e, B::format(T("t_conf_peers", "Se van a entregar a %1 las credenciales de: %2.\n"
                                 "Con ellas, esa máquina puede hablar con las otras como si fuera "
                                 "usted. ¿Continuar?"),
                               {destino.connection, B::join(nombres, ", ")}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    std::string out;
    if (!agente(e, destino, PET::setPeers(entrega.payloadB64), out, 30000)) {
        return false;
    }
    std::fprintf(stderr, TC("t_peers_puestos", "entregadas %zu credenciales a %s\n"),
                 nombres.size(), destino.connection.c_str());
    return true;
}

// El registro del daemon.
//
// Se pide por BYTES desde el final y no por líneas: el verbo del agente lee un rango del
// fichero —para que la interfaz pueda ir siguiéndolo sin releerlo entero— y no sabe contar
// renglones. Se pide de sobra y se recorta aquí, que es donde se sabe cuántas se querían.
bool cmdLog(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    long lineas = 200;
    if (!pet.valor("lines").empty()) {
        const std::string v = pet.valor("lines");
        if (v.find_first_not_of("0123456789") != std::string::npos) {
            std::fprintf(stderr, TC("t_log_no_entero", "«%s» no es un número de líneas\n"), v.c_str());
            return false;
        }
        lineas = std::atol(v.c_str());
    }
    // Un renglón del registro anda por los 120 bytes; se piden con holgura y sobra poco.
    const long bytes = (lineas + 20) * 400;
    std::string out;
    if (!agente(e, pet.objetivo, PET::daemonLog(0, bytes), out, 30000)) {
        return false;
    }
    std::vector<std::string> todas = B::split(out, "\n", false);
    while (!todas.empty() && B::trim(todas.back()).empty()) {
        todas.pop_back();
    }
    if (todas.empty()) {
        std::fprintf(stderr, TC("t_log_vacio", "el registro del daemon de %s está vacío\n"),
                     pet.objetivo.connection.c_str());
        return true;
    }
    const std::size_t desde =
        todas.size() > static_cast<std::size_t>(lineas) ? todas.size() - lineas : 0;
    for (std::size_t i = desde; i < todas.size(); ++i) {
        std::fprintf(stdout, "%s\n", todas[i].c_str());
    }
    return true;
}

bool cmdSchedules(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    std::vector<std::pair<std::string, B::gsa::Entry>> filas;
    std::vector<std::string> maquinas;
    // `-a`, como en `jobs -a` e `install-daemon -a`: todas las conexiones, desde cualquier
    // sitio. Sin él, la conexión en la que se está.
    const std::string actual = subeHasta(pet.objetivo, Objetivo::Conexion).connection;
    if (!pet.tiene("-a") && actual.empty()) {
        std::fputs(TC("t_schs_falta_conexion",
                      "hace falta estar en una conexión, o «schedules -a» para todas\n"),
                   stderr);
        return false;
    }
    if (pet.tiene("-a")) {
        for (const auto& p : e.conns.perfiles) {
            const std::string id = p.id.empty() ? p.name : p.id;
            if (!e.conns.desconectada(id)) {
                maquinas.push_back(id);
            }
        }
    } else {
        maquinas.push_back(actual);
    }
    for (const std::string& id : maquinas) {
        ZfsmUrl u;
        u.kind = ZfsmKind::Connection;
        u.connection = id;
        std::string listaPools;
        if (!agente(e, u, PET::poolList(), listaPools, 20000)) {
            continue;   // con --all, una máquina que no contesta no invalida a las demás
        }
        B::json::Value raiz;
        std::string err;
        if (B::trim(listaPools).empty() || !B::json::parse(listaPools, raiz, &err)) {
            continue;
        }
        for (const auto& kv : raiz["pools"].toObject()) {
            std::vector<B::gsa::Entry> deEstePool;
            if (!leeProgramaciones(e, u, kv.first, deEstePool)) {
                continue;
            }
            for (const B::gsa::Entry& en : deEstePool) {
                filas.push_back({id, en});
            }
        }
    }
    tablaDeProgramaciones(filas).imprime(e.formato);
    return true;
}

bool cmdSchedule(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (!exigeDataset(destino)) {
        return false;
    }
    // Lo que hay puesto AHORA. Se lee siempre, también para escribir: fijar `--daily` no
    // debe borrar el resto de la programación.
    std::vector<B::gsa::Entry> aqui;
    if (!leeProgramaciones(e, destino, destino.dataset, aqui)) {
        return false;
    }
    B::gsa::Schedule actual;
    for (const B::gsa::Entry& en : aqui) {
        if (en.dataset == destino.dataset) {
            actual = en.schedule;
        }
    }

    // Quitarla del todo: se heredan las propiedades, que es lo contrario de escribirlas.
    if (pet.tiene("--clear")) {
        if (!confirma(e, B::format(T("t_conf_sch_clear", "Se va a borrar la programación de %1. "
                                     "¿Continuar?"), {destino.dataset}))) {
            std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
            return false;
        }
        for (const auto& kv : B::gsa::toProperties(B::gsa::Schedule{})) {
            std::string sinUsar;
            if (!zfsGeneric(e, destino, {"inherit", kv.first, destino.dataset})) {
                return false;
            }
        }
        std::fprintf(stderr, TC("t_sch_borrada", "borrada la programación de %s\n"),
                     destino.dataset.c_str());
        return true;
    }

    // ¿Se pide algún cambio? Sin ninguno, esta orden solo ENSEÑA.
    const char* const kOpciones[] = {"hourly", "daily", "weekly", "monthly", "yearly", "to"};
    bool cambia = pet.tiene("--off") || pet.tiene("--recursive") || pet.tiene("--no-recursive")
                  || pet.tiene("--level") || pet.tiene("--no-level");
    for (const char* o : kOpciones) {
        if (!pet.valor(o).empty()) {
            cambia = true;
        }
    }
    if (!cambia) {
        std::vector<std::pair<std::string, B::gsa::Entry>> filas;
        if (!aqui.empty()) {
            for (const B::gsa::Entry& en : aqui) {
                filas.push_back({destino.connection, en});
            }
        }
        if (filas.empty()) {
            std::fprintf(stderr, TC("t_sch_ninguna", "%s no tiene programación\n"),
                         destino.dataset.c_str());
            return true;
        }
        tablaDeProgramaciones(filas).imprime(e.formato);
        return true;
    }

    B::gsa::Schedule nueva = actual;
    const auto retencion = [&](const char* opcion, int& campo) {
        const std::string v = pet.valor(opcion);
        if (v.empty()) {
            return true;
        }
        std::map<std::string, std::string> uno{{std::string(B::gsa::kPropertyPrefix) + "diario", v}};
        B::gsa::Schedule tmp;
        B::gsa::Reason m;
        if (!B::gsa::fromProperties(uno, tmp, m)) {
            std::fprintf(stderr, TC("t_sch_no_entero", "«%s» no es un número de snapshots válido "
                         "para --%s\n"), v.c_str(), opcion);
            return false;
        }
        campo = tmp.daily;
        return true;
    };
    if (!retencion("hourly", nueva.hourly) || !retencion("daily", nueva.daily)
        || !retencion("weekly", nueva.weekly) || !retencion("monthly", nueva.monthly)
        || !retencion("yearly", nueva.yearly)) {
        return false;
    }
    if (pet.tiene("--recursive")) { nueva.recursive = true; }
    if (pet.tiene("--no-recursive")) { nueva.recursive = false; }
    if (pet.tiene("--level")) { nueva.level = true; }
    if (pet.tiene("--no-level")) { nueva.level = false; }
    if (!pet.valor("to").empty()) { nueva.target = pet.valor("to"); }
    // Fijar cualquier valor la ACTIVA: programar algo apagado no significa nada. `--off`
    // es lo que la apaga, y por eso se aplica al final.
    nueva.enabled = !pet.tiene("--off");

    // Y ahora las reglas, ANTES de escribir. Primero la programación en sí.
    const auto conexionExiste = [&](const std::string& nombre) {
        return buscarConexion(e.conns, nombre) != nullptr;
    };
    B::gsa::Reason motivo;
    if (!B::gsa::isValid(destino.dataset, nueva, conexionExiste, motivo)) {
        std::fprintf(stderr, "%s: %s\n", destino.dataset.c_str(),
                     B::gsa::labelOf(motivo.failure).c_str());
        if (!motivo.detail.empty()) {
            std::fprintf(stderr, "  %s\n", motivo.detail.c_str());
        }
        return false;
    }
    // Y después el conjunto: dos activadas del mismo pool no pueden solaparse si una es
    // recursiva. Hace falta lo que ya hay programado en el POOL, no solo aquí debajo.
    std::vector<B::gsa::Entry> delPool;
    ZfsmUrl raizPool = destino;
    raizPool.dataset = destino.pool;
    if (leeProgramaciones(e, destino, destino.pool, delPool)) {
        bool sustituido = false;
        for (B::gsa::Entry& en : delPool) {
            if (en.dataset == destino.dataset) {
                en.schedule = nueva;
                sustituido = true;
            }
        }
        if (!sustituido) {
            delPool.push_back({destino.dataset, nueva});
        }
        if (!B::gsa::isValidSet(delPool, motivo)) {
            std::fprintf(stderr, "%s: %s (%s)\n", motivo.dataset.c_str(),
                         B::gsa::labelOf(motivo.failure).c_str(), motivo.detail.c_str());
            return false;
        }
    }

    std::vector<std::string> argv{"set"};
    for (const auto& kv : B::gsa::toProperties(nueva)) {
        argv.push_back(kv.first + "=" + kv.second);
    }
    argv.push_back(destino.dataset);
    if (!zfsGeneric(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, TC("t_sch_puesta", "programación puesta en %s\n"), destino.dataset.c_str());
    return true;
}

bool cmdClaves(Estado& e, const LineaAnalizada& linea, const char* op) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (std::string(op) == "load-key") {
        // Antes de pedir NADA se mira el dataset. Si no, la frase se tecleaba para nada y
        // después salía el error crudo de `zfs`: en un dataset sin cifrar, con la clave ya
        // cargada, o en uno que hereda la clave de su raíz de cifrado.
        const auto prop = [&](const char* nombre) {
            std::string v;
            (void)agente(e, destino, PET::datasetProperty(nombre, destino.dataset), v);
            return B::trim(v);
        };
        const std::string cifrado = prop("encryption");
        if (cifrado.empty() || cifrado == "off" || cifrado == "-") {
            std::fprintf(stderr, TC("t_lk_sin_cifrar", "%s no está cifrado: no tiene clave\n"),
                         destino.dataset.c_str());
            e.ultimoRc = 1;
            return false;
        }
        // La clave de un dataset que hereda el cifrado es la de su RAÍZ de cifrado, y `zfs`
        // solo acepta cargarla ahí. Cargar esa es lo que se quiere decir y no borra ni cambia
        // nada, así que se hace diciéndolo.
        std::string raiz = prop("encryptionroot");
        if (raiz.empty() || raiz == "-") {
            raiz = destino.dataset;
        }
        ZfsmUrl objetivo = destino;
        if (raiz != destino.dataset) {
            std::fprintf(stderr,
                         TC("t_lk_raiz",
                            "la clave de %s es la de su raíz de cifrado, %s: se carga esa\n"),
                         destino.dataset.c_str(), raiz.c_str());
            objetivo.dataset = raiz;
        }
        const auto propRaiz = [&](const char* nombre) {
            std::string v;
            (void)agente(e, objetivo, PET::datasetProperty(nombre, objetivo.dataset), v);
            return B::trim(v);
        };
        if (propRaiz("keystatus") == "available") {
            std::fprintf(stderr, TC("t_lk_ya_cargada", "la clave de %s ya está cargada\n"),
                         objetivo.dataset.c_str());
            return true;
        }
        // La frase de paso NUNCA por argumento: iría en `argv` y se vería en `ps` en las
        // dos máquinas. El verbo dedicado la lleva en base64 dentro de la petición RPC.
        //
        // Con `keylocation` en un fichero no hay nada que preguntar: `zfs` la lee de ahí y
        // no mira la entrada estándar.
        std::string frase;
        std::string err;
        const std::string dondeEsta = propRaiz("keylocation");
        if (dondeEsta == "prompt" || dondeEsta.empty() || dondeEsta == "-") {
            const std::string fd = pet.valor("password-fd");
            const bool leida = !fd.empty()
                                   ? leerSecretoDeDescriptor(std::atoi(fd.c_str()), frase, err)
                                   : preguntarSecretoPorTerminal(
                                         T("t_p_frase", "Frase de paso: "), frase, err);
            if (!leida) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return false;
            }
        } else {
            std::fprintf(stderr, TC("t_lk_de_fichero", "la clave se lee de %s\n"),
                         dondeEsta.c_str());
        }
        std::string out;
        const bool ok = agente(e, objetivo,
                               PET::loadKey(objetivo.dataset, frase),
                               out);
        for (char& c : frase) {
            c = '\0';
        }
        if (!ok) {
            return false;
        }
        std::fprintf(stderr, TC("t_clave_carg_09d2f4", "clave cargada en %s\n"), objetivo.dataset.c_str());
        return true;
    }
    // Cambiar la frase. Va aparte de `zfsGeneric` por lo mismo que `load-key`: la frase no
    // puede ir en el argv, así que la lleva el verbo dedicado del daemon —cifrada dentro de
    // la petición— y allí se le entrega a `zfs` por la entrada estándar.
    if (std::string(op) == "change-key") {
        std::string frase;
        std::string err;
        const std::string fd = pet.valor("password-fd");
        if (!fd.empty()) {
            if (!leerSecretoDeDescriptor(std::atoi(fd.c_str()), frase, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return false;
            }
        } else {
            // Se pide DOS veces: sin eco, un dedazo dejaría el dataset con una frase que
            // nadie conoce, y eso no tiene vuelta atrás.
            std::string otra;
            if (!preguntarSecretoPorTerminal(T("t_p_frase_nueva", "Frase de paso nueva: "), frase, err)
                || !preguntarSecretoPorTerminal(T("t_p_frase_rep", "Repítala: "), otra, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return false;
            }
            if (frase != otra) {
                std::fputs(TC("t_frases_no_coinciden", "las dos frases no coinciden; no se ha cambiado nada\n"), stderr);
                for (char& c : frase) { c = '\0'; }
                for (char& c : otra) { c = '\0'; }
                return false;
            }
            for (char& c : otra) { c = '\0'; }
        }
        if (frase.empty()) {
            std::fputs(TC("t_frase_vacia", "la frase de paso no puede estar vacía\n"), stderr);
            return false;
        }
        std::string out;
        const bool ok = agente(e, destino,
                               PET::changeKey(destino.dataset, frase, std::string()),
                               out);
        for (char& c : frase) { c = '\0'; }
        if (!ok) {
            return false;
        }
        std::fprintf(stderr, TC("t_clave_cambiada", "cambiada la frase de paso de %s\n"),
                     destino.dataset.c_str());
        return true;
    }
    if (!zfsGeneric(e, destino, {op, destino.dataset})) {
        return false;
    }
    std::fprintf(stderr, TC("t_s_en_s_35a806", "%s en %s\n"), op, destino.dataset.c_str());
    return true;
}

// --- Las cuatro acciones de Avanzado.

// Lo que hay que decir antes de Desglosar o Ensamblar un dataset SIN MONTAR, y prepararlo.
//
// El agente lo monta en un temporal mientras dura y le devuelve después su punto de montaje
// (ver AltMountGuard en el daemon). Aquí se suelta antes el montaje de solo lectura que esta
// sesión pudiera tener sobre él —estando dentro con `cd`—, que estorbaría: el agente lo vería
// «montado» en un sitio donde no se puede escribir.
//
// Y en Desglosar, el aviso acordado con el usuario: si el padre tiene `mountpoint=/` y su
// pool se importa solo al arrancar esta máquina, `zfs mount -a` montaría los hijos nuevos
// —que heredan /usr, /var…— encima de los de este sistema. No se toca el pool: se dice, con
// las dos salidas.
std::string preparaMontajeTemporal(Estado& e, const ZfsmUrl& d, bool creaHijos, bool& rechaza) {
    rechaza = false;
    {
        std::vector<Estado::MontajeLectura> quedan;
        for (const auto& m : e.montajesLectura) {
            if (B::toLowerAscii(m.conexion) == B::toLowerAscii(d.connection) && m.dataset == d.dataset) {
                std::string out;
                std::string fallo;
                ZfsmUrl u;
                u.kind = ZfsmKind::Connection;
                u.connection = m.conexion;
                if (!agente(e, u, PET::unmountReadOnlyTemporary(m.dir), out, 30000, &fallo)) {
                    quedan.push_back(m);
                }
                continue;
            }
            quedan.push_back(m);
        }
        e.montajesLectura = quedan;
    }
    std::string montado;
    if (!agente(e, d, PET::datasetProperty("mounted", d.dataset), montado)
        || B::trim(montado) != "no") {
        return {};
    }
    std::string error;
    const auto* p = perfilVivoDe(e, d, error);
    if (!p || T::isWindowsConnection(*p)) {
        return {};
    }
    // Con canmount=off no se monta de ninguna forma, y suele ser un contenedor sin ficheros
    // propios: lo normal es que se quisiera uno de sus hijos. Se dice ANTES de confirmar,
    // no después de que el agente falle.
    std::string canmount;
    (void)agente(e, d, PET::datasetProperty("canmount", d.dataset), canmount);
    if (B::trim(canmount) == "off") {
        std::fputs(B::format(T("t_bd_canmount_off",
                               "%1 tiene canmount=off: no se puede montar y no guarda ficheros "
                               "propios. Si se refería a uno de sus hijos, indíquelo con «--on» "
                               "o entre en él con «cd».\n"),
                             {d.dataset}).c_str(),
                   stderr);
        rechaza = true;
        return {};
    }
    std::string mp;
    (void)agente(e, d, PET::datasetProperty("mountpoint", d.dataset), mp);
    mp = B::trim(mp);
    std::string texto = B::format(T("t_bd_montaje_temporal",
                                    "%1 no está montado: se montará en un directorio temporal "
                                    "mientras dure la operación, y después se le devolverá su "
                                    "punto de montaje (%2)."),
                                  {d.dataset, mp});
    if (creaHijos) {
        texto += " " + T("t_bd_hijos_heredan",
                         "Los datasets nuevos heredan el suyo (canmount=on) y quedan sin montar.");
    }
    if (creaHijos && mp == "/") {
        std::string props;
        std::string cache;
        B::json::Value v;
        if (agente(e, d, PET::poolProperties(d.pool), props, 20000)
            && B::json::parse(props, v, nullptr)) {
            cache = v["pools"][d.pool]["properties"]["cachefile"]["value"].toString();
        }
        if (cache.empty() || cache == "-") {
            texto += "\n" + B::format(
                T("t_bd_riesgo_arranque",
                  "AVISO: el pool %1 se importa solo al arrancar %2 (cachefile por omisión), y "
                  "entonces «zfs mount -a» montaría los datasets nuevos —que heredan /usr, "
                  "/var…— encima de los de ese sistema. Para evitarlo: impórtelo con «zpool "
                  "import -N %1», o quítelo de la caché con «zpool set cachefile=none %1»."),
                {d.pool, d.connection});
        }
    }
    return texto;
}

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
    bool rechaza = false;
    const std::string sinMontar = preparaMontajeTemporal(e, destino, /*creaHijos=*/true, rechaza);
    if (rechaza) {
        return false;
    }
    if (!confirma(e, (sinMontar.empty() ? std::string() : sinMontar + "\n")
                         + B::format(T("t_conf_breakdown",
                                       "Se van a convertir %1 directorios de %2 en datasets hijos. "
                                       "¿Continuar?"),
                                     {std::to_string(pet.lista("texto").size() / 2),
                                      destino.dataset}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    // La lista llega en pares (subdirectorio, dataset nuevo); `argvBreakdown` los empareja y
    // descarta entero el que venga a medias, que es lo que desplazaría a todos los demás.
    std::vector<AV::Breakdown> pares;
    const auto& xs = pet.lista("texto");
    for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
        pares.push_back(AV::Breakdown{xs[i], xs[i + 1]});
    }
    const std::vector<std::string> argv = AV::argvBreakdown(destino.dataset, pares);
    if (argv.empty()) {
        std::fputs(TC("t_nada_que_desglosar",
                      "los subdirectorios van en pares: <subdir> <dataset-nuevo>\n"), stderr);
        return false;
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
    bool rechaza = false;
    const std::string sinMontar = preparaMontajeTemporal(e, destino, /*creaHijos=*/false, rechaza);
    if (rechaza) {
        return false;
    }
    if (!confirma(e, (sinMontar.empty() ? std::string() : sinMontar + "\n")
                         + B::format(T("t_conf_assemble",
                                       "Se van a reintegrar %1 datasets hijos en %2 como "
                                       "directorios. ¿Continuar?"),
                                     {std::to_string(pet.lista("texto").size()),
                                      destino.dataset}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    // La regla de los nombres completos vive en `commands::advanced`, con su porqué y sus
    // pruebas. Aquí estaba escrita en un comentario, otra vez en el servidor web, y resuelta
    // de una tercera manera en la interfaz.
    const std::vector<std::string> argv =
        AV::argvAssemble(destino.dataset, pet.lista("texto"));
    if (argv.empty()) {
        std::fputs(TC("t_nada_que_ensamblar", "ninguno de los nombres dados sirve\n"), stderr);
        return false;
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
    // El directorio puede venir como `zfsm://<misma máquina>#path/…`: `todir` vuelca dentro
    // de la máquina del dataset, así que otra máquina es un error.
    std::string dirDestino;
    if (!rutaDeRanura(e, pet.uno("ruta"), destino.connection, dirDestino)) {
        return false;
    }
    if (!confirma(e, B::format(T("t_conf_todir", "Se va a volcar %1 a %2%3. ¿Continuar?"),
                               {destino.dataset, dirDestino,
                                borraOrigen ? T("t_conf_y_destruir_origen",
                                                " y DESTRUIR el dataset de origen")
                                            : std::string()}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    const std::vector<std::string> argvTodir =
        AV::argvToDir(destino.dataset, dirDestino, borraOrigen);
    if (argvTodir.empty()) {
        std::fputs(TC("t_ruta_no_absoluta",
                      "el directorio de destino tiene que ser una ruta absoluta\n"), stderr);
        return false;
    }
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
    // El origen como `zfsm://maq#path/dir`: la URL ya dice de qué máquina viene, y con ella
    // `--from` sobra. Si se dan las dos, tienen que nombrar la misma máquina.
    std::string dir;
    std::string maquinaDeLaRuta;
    if (!rutaDeRanura(e, pet.uno("ruta"), "", dir, &maquinaDeLaRuta)) {
        return false;
    }
    if (!maquinaDeLaRuta.empty() && pet.valor("from").empty()) {
        origen = ZfsmUrl{};
        origen.kind = ZfsmKind::Connection;
        origen.connection = maquinaDeLaRuta;
        std::string error;
        pSrc = perfilDe(e, origen, error);
        if (!pSrc) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
    }
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

    if (!maquinaDeLaRuta.empty() && !pet.valor("from").empty()
        && B::toLowerAscii(maquinaDeLaRuta) != B::toLowerAscii(origen.connection)) {
        std::fprintf(stderr, TC("t_fromdir_dos_maquinas",
                                "el origen dice %s y --from dice %s: sobra uno\n"),
                     maquinaDeLaRuta.c_str(), origen.connection.c_str());
        return false;
    }
    const std::string rel = pet.valor("subdir");
    // El subdirectorio se comprueba AQUÍ, antes de preguntar y antes de abrir la tubería.
    // Lo miraba solo el daemon, al otro extremo, y para entonces el tar del origen ya estaba
    // corriendo: la operación moría a mitad con parte del contenido ya fuera de su máquina.
    if (!AV::isValidRelativeSubdir(rel)) {
        std::fprintf(stderr,
                     TC("t_fromdir_subdir_malo",
                        "«%s» no sirve como subdirectorio: tiene que ser relativo al dataset y "
                        "sin «..»\n"),
                     rel.c_str());
        return false;
    }
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
    // ── Primero, sin tubería: el árbol entre daemons ─────────────────────────────
    //
    // El destino prepara el directorio y se pone a escuchar; el origen le manda el árbol
    // directamente. Los datos NO pasan por esta máquina, y la copia es incremental.
    //
    // Se INTENTA en vez de comprobarse antes: aquí no hay estado cacheado de qué máquina
    // tiene daemon —eso lo lleva la ventana—, y una llamada que falla dice exactamente lo
    // mismo que diría la comprobación, sin costar un viaje de más cuando sí se puede.
    {
        const std::vector<std::string> prepArgv = AV::argvFromDirPrepare(destino.dataset, rel);
        std::string pOut;
        std::string pErr;
        int pRc = -1;
        const bool preparado = !prepArgv.empty()
                               && ejecutarAgente(*e.ses, dst, prepArgv, pOut, pErr, pRc, nullptr,
                                                 60000)
                               && pRc == 0;
        const std::string dirDestino = preparado ? AV::preparedPath(pOut) : std::string();
        if (!dirDestino.empty()) {
            const auto llama = [&e](const B::ConnectionProfile& maquina,
                                    const std::vector<std::string>& args, int timeoutMs,
                                    std::string& salida, std::string& err, int& rc) {
                return ejecutarAgente(*e.ses, maquina, args, salida, err, rc, nullptr, timeoutMs);
            };
            std::string salidaEnvio;
            const auto hecho = TR::launchTreeJob(
                e.ses->transporte, llama, src, dst, dir, dirDestino,
                origen.connection == destino.connection, e.ses->verboso,
                /*comoTrabajo=*/false, /*borrarEnDestino=*/false, /*enSeco=*/false,
                &salidaEnvio);
            // `fallo` y no `ok()`: sin encolar no hay identificador, y `ok()` exige uno.
            if (hecho.failure == TR::JobFailure::None_) {
                std::fputs(salidaEnvio.c_str(), stdout);
                avisaSinMetadatos(salidaEnvio);
                return true;
            }
            // Que no se pueda por aquí no es un error de la orden: se sigue por la tubería,
            // que solo pide daemon en el destino. Se dice por qué, para que no parezca que
            // se eligió el camino lento porque sí.
            std::fprintf(stderr,
                         TC("t_fromdir_arbol_no", "el árbol entre daemons no pudo (%s); se sigue "
                                                  "por la tubería\n"),
                         TR::labelOf(hecho.failure).c_str());
        }
    }

    // El argv lo compone el módulo, que es el mismo que usa la interfaz. La cadena se deriva
    // de él —nunca al revés—: aquí hace falta porque lo que se ejecuta es una tubería de
    // shell, y esa es justamente la punta que no puede ser un RPC.
    const std::vector<std::string> fdArgv = AV::argvFromDir(destino.dataset, rel);
    if (fdArgv.empty()) {
        std::fputs(TC("t_fromdir_destino_malo", "destino no válido para fromdir\n"), stderr);
        return false;
    }
    std::string recibe = B::daemonpayload::unixBinPath();
    for (const std::string& a : fdArgv) {
        recibe += " " + B::shSingleQuote(a);
    }

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

// Que el fichero de destino de un `send` está entero en su sitio.
//
// El trabajo es del EMISOR, y que acabe bien dice que mandó todo, no que el otro lado lo
// guardara: el último tramo puede estar aún en el búfer del receptor cuando falla al
// escribirlo, y entonces el receptor borra el parcial sin que el emisor se entere. Así que
// con `--wait` se mira el fichero: que exista con su nombre —el receptor solo se lo da a un
// flujo entero— y que mida lo que se mandó. El nombre se lo pone tras cerrar, así que se le
// dan unos segundos.
bool ficheroCompleto(Estado& e, const ZfsmUrl& donde, const std::string& ruta, long long bytes) {
    long long tam = -1;
    for (int intento = 0; intento < 20; ++intento) {
        std::string out;
        B::json::Value v;
        if (agente(e, donde, PET::pathInfo(ruta), out, 20000) && B::json::parse(out, v, nullptr)
            && v["exists"].toBool()) {
            tam = v["size"].toInt();
            if (bytes < 0 || tam == bytes) {
                std::fprintf(stderr, TC("t_send_fichero_guardado", "guardado en %s (%s)\n"),
                             ruta.c_str(), B::humanSize(std::to_string(tam)).c_str());
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (tam < 0) {
        std::fprintf(stderr, TC("t_send_fichero_no_quedo",
                                "el envío terminó, pero %s no está en %s: el receptor no pudo "
                                "guardarlo (su registro dice por qué)\n"),
                     ruta.c_str(), donde.connection.c_str());
    } else {
        std::fprintf(stderr, TC("t_send_fichero_corto",
                                "%s mide %lld bytes y se mandaron %lld\n"),
                     ruta.c_str(), tam, bytes);
    }
    return false;
}

// Send una instantánea a otro dataset, en esta máquina o en otra.
//
// Con `--base` es INCREMENTAL: solo viaja lo que cambió desde esa instantánea, que es lo
// que la interfaz llama «Nivelar». Sin ella va el flujo completo.
bool cmdSend(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    // Las banderas de `zfs send` se escriben SUELTAS: `send /otra/x -w -L`.
    //
    // Hubo una opción `--flags "-w -L"` que envolvía lo mismo en una cadena, y sobraba: el
    // léxico ya sabe cuáles son banderas de esta orden porque están declaradas, así que
    // meterlas en un texto entrecomillado solo añadía una forma más de escribir lo mismo —y
    // una que había que validar aparte, porque ese texto llegaba en crudo al argv de un
    // `zfs send` con privilegios: `--flags "tank/otro@ayer"` sacaba por el socket un
    // dataset que no era el pedido—.
    //
    // El daemon sigue comprobando la lista, que es donde tiene que comprobarse: el
    // intérprete es un cliente más.
    std::string banderasSend;
    for (const std::string& n : pet.nativas()) {
        banderasSend += (banderasSend.empty() ? "" : " ") + n;
    }
    if (pet.lista("destino").empty() && pet.valor("to").empty()) {
        std::fputs(TC("t_uso_copy_d_f6efbc", "uso: send <destino> [--from <@snapshot>] [--base <@snapshot>]\n"
                     "          [banderas de zfs send] [--wait]\n"
                     "  El destino es una URL: puede estar en OTRA máquina.\n"
                     "  Sin --from se usa el sitio actual como origen.\n"
                     "  Cualquiera de los dos puede ser un fichero con el flujo («#path/…»).\n"
                     "  Con --base solo viaja lo que cambió desde ahí (lo que la interfaz\n"
                     "  llama «Nivelar»).\n"), stderr);
        return false;
    }
    // Cualquiera de las dos puntas puede ser un FICHERO con un flujo de `zfs send` dentro:
    // el destino, para volcarlo; el origen (`--from`), para recibirlo como si viniera de una
    // instantánea. Un directorio no es un flujo: solo vale si equivale a un dataset o a una
    // instantánea —su punto de montaje, la raíz de `.zfs/snapshot/<s>`—, como en el resto de
    // órdenes.
    ZfsmUrl origen = pet.objetivo;
    std::string ficheroOrigen;
    if (esSeccionDeFicheros(origen)) {
        const std::string escrito = textoDe(origen);
        Ubicacion ub;
        if (!ubicacionDe(e, origen, ub)) {
            return false;
        }
        if (!ub.existe) {
            std::fprintf(stderr, TC("t_send_no_existe", "%s no existe\n"), escrito.c_str());
            e.ultimoRc = 1;
            return false;
        }
        if (ub.esDirectorio) {
            if (!ub.esMontaje || ub.dataset.empty()) {
                std::fprintf(stderr, TC("t_send_origen_dir",
                                        "%s es un directorio: el origen es un fichero con el "
                                        "flujo, o la raíz de «.zfs/snapshot/<s>»\n"),
                             escrito.c_str());
                e.ultimoRc = 2;
                return false;
            }
            if (!equivaleADataset(e, origen, escrito)) {
                return false;
            }
        } else if (ub.tipo != "f" && ub.tipo != "l") {
            std::fprintf(stderr, TC("t_send_no_es_fichero",
                                    "%s no es un fichero normal: el flujo tiene que estar en uno\n"),
                         escrito.c_str());
            e.ultimoRc = 1;
            return false;
        } else {
            ficheroOrigen = ub.ruta;
        }
    }
    if (ficheroOrigen.empty() && origen.snapshot.empty()) {
        std::fprintf(stderr, TC("t_el_origen__886ec5", "el origen tiene que ser un SNAPSHOT (ahora: %s)\n"), textoDe(origen).c_str());
        return false;
    }
    ZfsmUrl destino;
    std::string error;
    if (!resuelve(e, pet.lista("destino").empty() ? pet.valor("to") : pet.uno("destino"), destino, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    std::string ficheroDestino;
    bool sobrescribir = false;
    if (esSeccionDeFicheros(destino)) {
        const std::string escrito = textoDe(destino);
        if (!ficheroOrigen.empty()) {
            std::fputs(TC("t_send_fichero_a_fichero",
                          "de un fichero a otro no hay nada que enviar: eso es «cp»\n"),
                       stderr);
            e.ultimoRc = 2;
            return false;
        }
        Ubicacion ub;
        if (!ubicacionDe(e, destino, ub)) {
            return false;
        }
        if (ub.existe && ub.esDirectorio) {
            // El punto de montaje de un dataset es ese dataset. Cualquier otro directorio
            // es un sitio donde guardar el fichero, y su nombre no se inventa.
            if (!(ub.esMontaje && !ub.dataset.empty() && ub.instantanea.empty())) {
                std::fprintf(stderr, TC("t_send_destino_dir",
                                        "%s es un directorio: el flujo se guarda en un fichero; "
                                        "escriba también su nombre\n"),
                             escrito.c_str());
                e.ultimoRc = 2;
                return false;
            }
            if (!equivaleADataset(e, destino, escrito)) {
                return false;
            }
        } else if (ub.existe && ub.tipo != "f") {
            std::fprintf(stderr, TC("t_send_destino_raro",
                                    "%s existe y no es un fichero normal\n"),
                         escrito.c_str());
            e.ultimoRc = 2;
            return false;
        } else {
            ficheroDestino = ub.ruta;
            sobrescribir = ub.existe;
            // El directorio tiene que existir: el daemon no crea directorios como root a
            // petición de quien conecta. Se mira AHORA, antes de pedir confirmación.
            std::string padre = ficheroDestino.substr(0, ficheroDestino.find_last_of('/'));
            if (padre.empty() || padre.back() == ':') {
                padre += "/";
            }
            std::string infoPadre;
            B::json::Value v;
            if (!agente(e, destino, PET::pathInfo(padre), infoPadre, 20000)) {
                return false;
            }
            if (!B::json::parse(infoPadre, v, nullptr) || !v["is_dir"].toBool()) {
                std::fprintf(stderr, TC("t_send_sin_directorio",
                                        "el directorio %s no existe en %s\n"),
                             padre.c_str(), destino.connection.c_str());
                e.ultimoRc = 1;
                return false;
            }
        }
    }
    if (ficheroDestino.empty() && destino.dataset.empty()) {
        std::fputs(TC("t_el_destino_e077ee", "el destino tiene que ser un dataset\n"), stderr);
        return false;
    }
    // Lo que el flujo ya trae no se puede pedir otra vez: un fichero es el flujo que es.
    if (!ficheroOrigen.empty() && (!pet.valor("base").empty() || !banderasSend.empty())) {
        std::fputs(TC("t_send_fichero_sin_banderas",
                      "con un fichero de origen no valen «--base» ni las banderas de zfs send: "
                      "el flujo se hizo al volcarlo\n"),
                   stderr);
        e.ultimoRc = 2;
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
    // El extremo de ZFS no puede ser Windows: el flujo por socket no está portado allí.
    // Decirlo AQUÍ evita un fallo a mitad de transfer que no se entiende. Un FICHERO en
    // Windows sí vale: guardarlo o leerlo no pasa por `zfs`.
    if ((ficheroOrigen.empty() && T::isWindowsConnection(*pOrigen))
        || (ficheroDestino.empty() && T::isWindowsConnection(*pDestino))) {
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

    // ¿Una recepción a medias en el destino?
    //
    // Si la hay, `zfs recv` no acepta otro envío hasta que se decida qué hacer con ella, y lo
    // que llegaba al usuario era su error crudo; la salida era `zfs recv -A` a mano en la
    // otra máquina, teniendo allí un daemon como root que sabe hacerlo. Se mira ANTES de
    // preguntar nada, y sin una decisión explícita no se toca.
    const bool reanudar = pet.tiene("--resume");
    const bool descartar = pet.tiene("--discard-partial");
    if (reanudar && descartar) {
        std::fputs(TC("t_send_resume_y_discard",
                      "«--resume» y «--discard-partial» son lo contrario: una de las dos\n"),
                   stderr);
        e.ultimoRc = 2;
        return false;
    }
    // Un fichero no tiene recepciones a medias, y continuar una necesita el `zfs send -t`
    // del origen: un fichero solo puede dar el flujo desde el principio.
    if ((reanudar || descartar) && !ficheroDestino.empty()) {
        std::fputs(TC("t_send_fichero_sin_parcial",
                      "un fichero de destino no tiene recepciones a medias: «--resume» y "
                      "«--discard-partial» no aplican\n"),
                   stderr);
        e.ultimoRc = 2;
        return false;
    }
    if (reanudar && !ficheroOrigen.empty()) {
        std::fputs(TC("t_send_fichero_no_reanuda",
                      "un fichero no puede continuar una recepción a medias: hace falta la "
                      "instantánea de origen, o «--discard-partial» para empezar de cero\n"),
                   stderr);
        e.ultimoRc = 2;
        return false;
    }
    const TR::Resume aMedias = !ficheroDestino.empty() ? TR::Resume{} : TR::findResumeToken(
        [&](const B::ConnectionProfile& maquina, const std::vector<std::string>& args,
            int timeoutMs, std::string& out, std::string& err, int& rc) {
            return ejecutarAgente(*e.ses, maquina, args, out, err, rc, nullptr, timeoutMs);
        },
        *pDestino, destino.dataset);
    std::string testigo;
    if (aMedias.any()) {
        if (!reanudar && !descartar) {
            std::fprintf(stderr,
                         TC("t_send_hay_parcial",
                            "%s tiene una recepción que se cortó a medias, y no acepta otro envío "
                            "hasta decidir qué hacer con ella:\n"
                            "  --resume            la continúa desde donde quedó\n"
                            "  --discard-partial   la descarta (se pierde lo recibido) y empieza de cero\n"),
                         aMedias.heldBy.c_str());
            e.ultimoRc = 1;
            return false;
        }
        if (reanudar) {
            testigo = aMedias.token;
        } else {
            if (!confirma(e, B::format(T("t_conf_descartar_parcial",
                                         "Se va a DESCARTAR la recepción a medias de %1 en %2: "
                                         "se pierde lo ya recibido. ¿Continuar?"),
                                       {aMedias.heldBy, destino.connection}))) {
                std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
                return false;
            }
            std::string out;
            if (!agente(e, destino, PET::abortResumableReceive(aMedias.heldBy), out, 60000)) {
                return false;
            }
            std::fprintf(stderr, TC("t_parcial_descartada", "descartada la recepción a medias de %s\n"),
                         aMedias.heldBy.c_str());
        }
    } else if (reanudar) {
        std::fprintf(stderr, TC("t_send_nada_que_reanudar",
                                "%s no tiene ninguna recepción a medias que continuar\n"),
                     destino.dataset.c_str());
        e.ultimoRc = 1;
        return false;
    }

    if (!testigo.empty()) {
        if (!confirma(e, B::format(T("t_conf_reanudar",
                                     "Se va a continuar la recepción a medias de %1 en %2. ¿Continuar?"),
                                   {aMedias.heldBy, destino.connection}))) {
            std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
            return false;
        }
    } else if (!ficheroDestino.empty()) {
        const std::string que =
            sobrescribir
                ? B::format(T("t_conf_send_fichero_existe",
                              "Se va a volcar el flujo de %1 (en %2) al fichero %3 de %4, que YA "
                              "EXISTE y se sustituye. ¿Continuar?"),
                            {origen.zfsName(), origen.connection, ficheroDestino,
                             destino.connection})
                : B::format(T("t_conf_send_fichero",
                              "Se va a volcar el flujo de %1 (en %2) al fichero %3 de %4. "
                              "¿Continuar?"),
                            {origen.zfsName(), origen.connection, ficheroDestino,
                             destino.connection});
        if (!confirma(e, que)) {
            std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
            return false;
        }
    } else if (!ficheroOrigen.empty()) {
        if (!confirma(e, B::format(T("t_conf_send_desde_fichero",
                                     "Se va a recibir el flujo del fichero %1 (en %2) en %3 de %4.\n"
                                     "Si el destino existe y difiere, `zfs recv -F` lo SOBRESCRIBE. "
                                     "¿Continuar?"),
                                   {ficheroOrigen, origen.connection, destino.dataset,
                                    destino.connection}))) {
            std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
            return false;
        }
    } else if (!confirma(e, B::format(T("t_conf_copy",
                                 "Se va a %1 %2 de %3 a %4 en %5.\nSi el destino existe y "
                                 "difiere, `zfs recv -F` lo SOBRESCRIBE. ¿Continuar?"),
                               {base.empty() ? T("t_copiar", "copiar")
                                             : T("t_nivelar", "nivelar"),
                                origen.zfsName(), origen.connection, destino.dataset,
                                destino.connection}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }

    // Los tres pasos —escuchar, resolver la dirección, enviar— los da `TR::launchJob`.
    //
    // Aquí había una copia entera de esa secuencia, y YA HABÍA DIVERGIDO: resolvía la
    // dirección del destino como «el host del perfil», que cuando el destino es la conexión
    // **Local** vale «localhost» y desde una máquina remota apunta al propio origen. Send
    // de una máquina remota a Local dejaba al emisor conectándose consigo mismo. La capa
    // base pregunta al origen con qué dirección nos ve, que es la única que le sirve para
    // volver, y ese caso es justo el que se perdía al tener dos copias.
    TR::AgentCall llama = [&](const B::ConnectionProfile& maquina,
                                    const std::vector<std::string>& args, int timeoutMs,
                                    std::string& out, std::string& err, int& rc) {
        std::string motivo;
        return ejecutarAgente(*e.ses, maquina, args, out, err, rc, &motivo, timeoutMs);
    };
    const TR::Job trabajo =
        !ficheroDestino.empty()
            ? TR::launchToFileJob(e.ses->transporte, llama, *pOrigen, *pDestino,
                                  origen.zfsName(), ficheroDestino, sobrescribir, base,
                                  banderasSend, mismaMaquina, e.ses->verboso)
        : !ficheroOrigen.empty()
            ? TR::launchFromFileJob(e.ses->transporte, llama, *pOrigen, *pDestino, ficheroOrigen,
                                    destino.dataset, mismaMaquina, e.ses->verboso)
            : TR::launchJob(e.ses->transporte, llama, *pOrigen, *pDestino, origen.zfsName(),
                            destino.dataset, base, banderasSend, testigo, mismaMaquina,
                            e.ses->verboso);
    if (!trabajo.ok()) {
        const std::string detalle = B::trim(trabajo.detail);
        std::fprintf(stderr, "%s%s%s\n", TR::labelOf(trabajo.failure).c_str(),
                     detalle.empty() ? "" : ": ", detalle.c_str());
        e.ultimoRc = 1;
        return false;
    }
    const std::string jobId = trabajo.id;
    e.ultimoRc = 0;
    std::fprintf(stdout, "%s\n", jobId.c_str());
    std::fprintf(stderr, TC("t_transferen_07aa2e", "transferencia en marcha como trabajo %s en %s\n"), jobId.c_str(), origen.connection.c_str());

    // El trabajo es de la MÁQUINA de origen. Con un fichero, la URL del origen es una ruta, y
    // `job --on <ruta>` contestaría que no es una conexión.
    ZfsmUrl maquinaOrigen;
    maquinaOrigen.kind = ZfsmKind::Connection;
    maquinaOrigen.connection = origen.connection;
    if (!pet.tiene("--wait")) {
        std::fprintf(stderr, TC("t_siga_con_j_2debd4", "siga con «job %s --on %s»\n"), jobId.c_str(),
                     textoDe(ficheroOrigen.empty() ? origen : maquinaOrigen).c_str());
        return true;
    }
    // Con --wait se espera aquí. Se sondea cada dos segundos: más a menudo es carga sin
    // información nueva, porque el daemon actualiza el avance a ese ritmo.
    std::string ultimo;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::string est;
        if (!agente(e, maquinaOrigen, PET::jobStatus(jobId), est, 20000)) {
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
            if (e.ultimoRc == 0 && !ficheroDestino.empty()) {
                const long long bytes = k.count("BYTES") ? std::atoll(k.at("BYTES").c_str()) : -1;
                if (!ficheroCompleto(e, destino, ficheroDestino, bytes)) {
                    e.ultimoRc = 1;
                }
            }
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
// Los trabajos que el daemon de esa máquina tiene corriendo o encolados, por identificador.
// Vacío si no hay ninguno o si no contesta: una máquina sin daemon no tiene trabajos.
std::vector<std::string> trabajosEnCurso(Estado& e, const B::ConnectionProfile& p) {
    std::vector<std::string> ids;
    ZfsmUrl u;
    u.kind = ZfsmKind::Connection;
    u.connection = p.id.empty() ? p.name : p.id;
    std::string out;
    std::string err;
    int rc = -1;
    if (!ejecutarAgente(*e.ses, p, PET::jobList(), out, err, rc, nullptr, 15000) || rc != 0) {
        return ids;
    }
    return H::activeJobIds(out);
}

// --- El CLI de la otra máquina, junto con su daemon.
//
// Un daemon nuevo con un `zfsmgr-cli` viejo al lado es una máquina a medio actualizar, así
// que `install-daemon` pone al día también el CLI. Solo donde ya hay uno: instalarlo donde
// nadie lo pidió no es actualizar.
struct PlanCli {
    enum class Accion { NoHay, Windows, Enlace, EsEste, AlDia, PorDelante, Actualizar, SinBinario };
    Accion accion{Accion::NoHay};
    B::daemoninstall::CliProbe sonda;
    std::string binario;  // el de su plataforma, en este equipo
};

PlanCli planDelCli(Estado& e, const B::ConnectionProfile& perfil) {
    namespace DI = B::daemoninstall;
    PlanCli plan;
    if (T::isWindowsConnection(perfil)) {
        plan.accion = PlanCli::Accion::Windows;
        return plan;
    }
    plan.sonda = DI::probeRemoteCli(e.ses->transporte, perfil, e.ses->verboso);
    if (!plan.sonda.found) {
        return plan;
    }
    if (plan.sonda.isLink) {
        plan.accion = PlanCli::Accion::Enlace;  // el de un .app: lo actualiza su instalador
        return plan;
    }
    // El que se está ejecutando no se sustituye a sí mismo a mitad de una orden.
    if (T::isLocalConnection(perfil)) {
        std::error_code ec;
        const auto a = std::filesystem::canonical(plan.sonda.path, ec);
        const auto b = std::filesystem::canonical(dirDelEjecutable() + "/zfsmgr-cli", ec);
        if (!a.empty() && a == b) {
            plan.accion = PlanCli::Accion::EsEste;
            return plan;
        }
    }
    if (!plan.sonda.fullVersion.empty()) {
        switch (B::agentversion::verdictFor(plan.sonda.fullVersion, B::agentversion::expected())) {
            case B::agentversion::Verdict::UpToDate: plan.accion = PlanCli::Accion::AlDia; return plan;
            case B::agentversion::Verdict::Ahead: plan.accion = PlanCli::Accion::PorDelante; return plan;
            case B::agentversion::Verdict::Update: break;
        }
    }
    plan.binario = rutaDelCli(DI::platformOf(perfil),
                              DI::remoteArchitecture(e.ses->transporte, perfil, e.ses->verboso));
    plan.accion = plan.binario.empty() || dirDeAgentes().empty() || dirDeCatalogos().empty()
                      ? PlanCli::Accion::SinBinario
                      : PlanCli::Accion::Actualizar;
    return plan;
}

std::string textoDePlanCli(const PlanCli& plan) {
    switch (plan.accion) {
        case PlanCli::Accion::NoHay: return T("t_cli_plan_no_hay", "no tiene: no se instala");
        case PlanCli::Accion::Windows: return T("t_cli_plan_windows", "Windows: no se toca");
        case PlanCli::Accion::Enlace: return T("t_cli_plan_enlace", "es el del .app: no se toca");
        case PlanCli::Accion::EsEste: return T("t_cli_plan_este", "es el que está en marcha: no se toca");
        case PlanCli::Accion::AlDia: return T("t_instala_accion_al_dia", "al día: no se toca");
        case PlanCli::Accion::PorDelante:
            return T("t_instala_accion_delante", "va por delante del cliente: no se toca");
        case PlanCli::Accion::Actualizar: return T("t_instala_accion_actualizar", "actualizar");
        case PlanCli::Accion::SinBinario:
            return T("t_cli_plan_sin_binario", "a actualizar, pero este equipo no tiene su CLI");
    }
    return {};
}

// Lo pone al día según el plan. Cuenta lo que hace; `false` solo si se intentó y falló.
bool actualizaCli(Estado& e, const B::ConnectionProfile& perfilConSudo, const PlanCli& plan) {
    namespace DI = B::daemoninstall;
    const std::string quien = perfilConSudo.name.empty() ? perfilConSudo.id : perfilConSudo.name;
    if (plan.accion != PlanCli::Accion::Actualizar) {
        if (plan.accion != PlanCli::Accion::NoHay && plan.accion != PlanCli::Accion::Windows) {
            std::fprintf(stderr, TC("t_cli_de", "zfsmgr-cli de %s: %s\n"), quien.c_str(),
                         textoDePlanCli(plan).c_str());
        }
        return plan.accion != PlanCli::Accion::SinBinario;
    }
    std::fprintf(stderr, TC("t_cli_desplegando", "actualizando zfsmgr-cli en %s (%s)...\n"),
                 quien.c_str(), plan.sonda.path.c_str());
    std::vector<std::pair<std::string, std::string>> todos;
    for (const auto& pa : std::vector<std::pair<std::string, std::string>>{
             {"linux", "x86_64"}, {"freebsd", "x86_64"}, {"macos", "amd64"},
             {"macos", "arm64"}, {"windows", "x86_64"}}) {
        const std::string ruta = rutaDelCli(pa.first, pa.second);
        if (!ruta.empty()) {
            todos.emplace_back(pa.first + "-" + pa.second, ruta);
        }
    }
    const DI::Result r = DI::installCli(
        e.ses->transporte, perfilConSudo, plan.binario, dirDeCatalogos(), dirDeAgentes(), todos,
        plan.sonda.path, [](const std::string& l) { std::fprintf(stderr, "  %s\n", l.c_str()); },
        e.ses->verboso);
    if (!r.ok()) {
        std::fprintf(stderr, TC("t_cli_fallo", "no se pudo actualizar zfsmgr-cli en %s: %s\n"),
                     quien.c_str(), r.detail.c_str());
        return false;
    }
    if (r.versionBehind) {
        std::fprintf(stderr,
                     TC("t_cli_empaquetado_viejo",
                        "aviso: el zfsmgr-cli empaquetado para esa máquina es %s y este es %s\n"),
                     r.version.c_str(), B::agentversion::expected().c_str());
    }
    std::fprintf(stderr, TC("t_cli_actualizado", "zfsmgr-cli actualizado en %s\n"), quien.c_str());
    return true;
}

bool cmdInstalarDaemon(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    std::string error;
    const auto* p = perfilVivoDe(e, pet.objetivo, error);
    if (!p) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    const std::string quien = p->name.empty() ? p->id : p->name;

    // **Con trabajos en curso no se reinstala.** Reinstalar reinicia el servicio, y lo que
    // estuviera copiando muere a medias: dos copias de 16 y 50 GB quedaron por la mitad en
    // fc16 el 2026-10-10 por una reinstalación. Se dice cuáles y se para; `--force` lo hace
    // de todas formas, sabiéndolo.
    if (!pet.tiene("--force")) {
        const std::vector<std::string> enCurso = trabajosEnCurso(e, conSudo(e, *p));
        if (!enCurso.empty()) {
            std::fprintf(stderr,
                         TC("t_instala_hay_trabajos",
                            "%s tiene %zu trabajos en curso, y reinstalar el daemon los "
                            "cortaría a medias:\n"),
                         quien.c_str(), enCurso.size());
            for (const std::string& id : enCurso) {
                std::fprintf(stderr, "  %s\n", id.c_str());
            }
            std::fputs(TC("t_instala_hay_trabajos_2",
                          "Espere a que terminen («jobs»), o repita con --force para cortarlos.\n"),
                       stderr);
            e.ultimoRc = 1;
            return false;
        }
    }

    if (!confirma(e, B::format(T("t_conf_install_daemon",
                                 "Se va a instalar o actualizar el daemon en %1 y arrancarlo "
                                 "con el gestor de servicios del sistema. ¿Continuar?"),
                               {quien}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }

    // El guion de instalación vive ahora en la capa base —`base/daemoninstall`— porque no
    // tenía nada de intérprete: son 200 líneas de decidir qué toca según el sistema de la
    // otra punta, y el servidor web necesita las mismas. Aquí queda lo que sí es del
    // intérprete: preguntar antes y contar después.
    namespace DI = B::daemoninstall;
    const B::ConnectionProfile perfil = conSudo(e, *p);
    const std::string plataforma = DI::platformOf(perfil);
    // La arquitectura del OTRO lado, no la de aquí: se despliega a máquinas distintas.
    const std::string arq = DI::remoteArchitecture(e.ses->transporte, perfil, e.ses->verboso);
    const std::string binario = rutaDelAgente(plataforma, arq);
    if (binario.empty()) {
        std::fprintf(stderr, TC("t_no_se_enco_393030", "no se encontró el binario del daemon para %s/%s en este equipo.\n"
                     "No se instala nada: el respaldo por guion no habla TLS, y dejarlo puesto\n"
                     "daría una máquina que parece atendida y no lo está.\n"), plataforma.c_str(), arq.empty() ? "?" : arq.c_str());
        return false;
    }
    // La versión de OpenZFS se mira ANTES de anunciar el despliegue. `install()` la vuelve a
    // mirar y es quien de verdad manda —así la interfaz gráfica no puede saltársela—, pero
    // si solo estuviera allí, esto diría «desplegando…» y acto seguido «no se instala nada»,
    // que es contarlo al revés.
    {
        const auto zfs = DI::remoteOpenZfs(e.ses->transporte, perfil, e.ses->verboso);
        if (zfs.soporte != B::helpers::OpenZfsSoporte::Ok) {
            std::fprintf(stderr, TC("t_zfs_no_vale", "no se instala nada en %s: %s\n"),
                         quien.c_str(), B::helpers::textoDeSoporteOpenZfs(zfs).c_str());
            return false;
        }
    }
    std::fprintf(stderr, TC("t_desplegand_79081c", "desplegando %s en %s...\n"), binario.c_str(), quien.c_str());

    const DI::Result r = DI::install(
        e.ses->transporte, perfil, binario,
        [](const std::string& l) { std::fprintf(stderr, "  %s\n", l.c_str()); }, e.ses->verboso);

    if (r.versionBehind) {
        std::fprintf(stderr,
                     TC("t_agente_empaquetado_viejo",
                        "aviso: el agente empaquetado para %s es %s y este cliente espera %s; "
                        "se instala igual, pero la conexión seguirá saliendo desactualizada\n"),
                     plataforma.c_str(), r.version.c_str(), B::agentversion::expected().c_str());
    }
    if (!r.ok()) {
        // La versión de OpenZFS se dice ENTERA y sin envolverla en «la instalación falló»:
        // no ha fallado nada, es que no se ha intentado, y lo que hay que hacer está en el
        // propio texto.
        if (r.fallo == DI::Failure::UnsupportedOpenZfs) {
            std::fprintf(stderr, TC("t_zfs_no_vale", "no se instala nada en %s: %s\n"),
                         quien.c_str(), r.detail.c_str());
            return false;
        }
        if (r.fallo == DI::Failure::UnreadableBinary) {
            std::fprintf(stderr, TC("t_el_binario_ce2086", "el binario del daemon está vacío: %s\n"),
                         r.detail.c_str());
            return false;
        }
        std::fprintf(stderr, TC("t_la_instala_2b9e0d", "la instalación falló (código %d): %s\n"),
                     r.rc, r.detail.c_str());
        e.ultimoRc = r.rc == 0 ? 1 : r.rc;
        return false;
    }
    // Lo que la sesión recordaba de esta máquina ya no vale —es otro daemon, quizá con otro
    // material TLS—, y se le PREGUNTA al nuevo antes de dar la instalación por buena.
    //
    // Con reintentos, porque el guion de instalación da el servicio por arrancado en cuanto
    // el PROCESO existe, no cuando ya escucha: la primera pregunta podía llegar antes de
    // tiempo, fallar, y la sesión recordaba ese fallo —el «-» de la columna DAEMON— hasta
    // salir. Medido en FreeBSD 15.1: el `ls` de justo después fallaba con «unexpected eof» en
    // el saludo TLS, y uno lanzado unos segundos más tarde contestaba.
    std::string versionNueva;
    std::string ultimoMotivo;
    for (int intento = 0; intento < 8 && versionNueva.empty(); ++intento) {
        if (intento > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        // Cada intento desde cero: sin esto, el primer fallo dejaba la conexión penalizada
        // 30 s y los siguientes ni siquiera llegaban a preguntar.
        olvidaEstadoDeConexion(e, *p);
        std::string salida;
        std::string err;
        int rcSalud = -1;
        if (ejecutarAgente(*e.ses, *p, {"--health"}, salida, err, rcSalud, &ultimoMotivo, 4000)
            && rcSalud == 0) {
            auto claves = clavesDe(salida);
            versionNueva = claves["VERSION"];
            // Lo que el listado enseñará en AVISOS: sin esto, la versión quedaba recordada
            // y el aviso no, así que `ls` en la misma sesión no volvía a preguntar.
            const std::string id = p->id.empty() ? p->name : p->id;
            e.avisoDaemon.erase(id);
            if (claves["DISK_ACCESS"] == "denied") {
                e.avisoDaemon[id] = T("t_aviso_disco", "sin acceso al disco");
            }
        }
    }
    if (versionNueva.empty()) {
        olvidaEstadoDeConexion(e, *p);  // que el fallo no quede recordado
        std::fprintf(stderr,
                     TC("t_daemon_instalado_mudo",
                        "daemon instalado en %s, pero todavía no contesta (%s); «refresh» "
                        "vuelve a preguntar\n"),
                     quien.c_str(), ultimoMotivo.c_str());
    } else {
        e.versionDaemon[p->id.empty() ? p->name : p->id] = versionNueva;
        std::fprintf(stderr, TC("t_daemon_instalado_contesta", "daemon instalado en %s: contesta, versión %s\n"),
                     quien.c_str(), versionNueva.c_str());
    }
    // En macOS hace falta UN PASO MÁS, y a mano: sin «Acceso total al disco» el agente
    // arranca, contesta y responde STATUS=OK, pero no ve los discos, así que no encuentra
    // ningún pool para importar. Todo parece bien salvo el resultado, que es la peor forma
    // de fallar; por eso se dice aquí, al instalar, y no cuando la lista salga vacía. La
    // interfaz gráfica ya lo avisa —`mainwindow_connections.cpp`—, y el intérprete callaba.
    // Y el CLI de esa máquina, si tiene uno. Un fallo aquí no deshace el daemon, pero se
    // cuenta: la máquina queda a medias y la orden lo dice.
    const bool cliBien = actualizaCli(e, perfil, planDelCli(e, perfil));
    if (r.isMac) {
        std::fputs(TC("t_mac_acceso_disco", "\nEn macOS hace falta concederle «Acceso total al disco» al agente, o no verá\n"
                     "los discos y no encontrará pools que importar:\n"
                     "  Configuración del Sistema → Privacidad y Seguridad → Acceso total al\n"
                     "  disco → añadir /usr/local/libexec/zfsmgr-agent\n"), stderr);
    }
    if (!cliBien) {
        e.ultimoRc = 1;
    }
    return cliBien;
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
    // La conexión del sitio actual (o de --on). No se exige en la declaración porque con
    // `-a` vale desde cualquier sitio, también desde la raíz.
    const ZfsmUrl destino = subeHasta(pet.objetivo, Objetivo::Conexion);
    if (!pet.tiene("-a") && destino.connection.empty()) {
        std::fputs(TC("t_jobs_falta_conexion",
                      "hace falta estar en una conexión, o «jobs -a» para todas\n"),
                   stderr);
        return false;
    }
    // Por omisión, SOLO lo que está corriendo. Un daemon que lleva meses en pie acumula
    // trabajos terminados, y enseñarlos todos convierte «¿qué está pasando ahora?» —que es
    // la pregunta que uno tiene al teclear `jobs`— en buscar entre decenas de líneas.
    //
    // El filtro se pide por el NOMBRE DEL ESTADO, que es el mismo que sale en la columna:
    // así no hay que aprender un vocabulario aparte ni mantener una tabla de traducción
    // entre lo que se escribe y lo que devuelve el daemon.
    std::set<std::string> estados;
    if (!pet.tiene("--history")) {
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
    // `-a`: los de TODAS las conexiones, una detrás de otra en la misma tabla, que por eso
    // lleva la columna CONEXIÓN. Sin `-a`, los de la conexión en la que se está.
    const bool todas = pet.tiene("-a");
    // `--detail`: qué hace cada uno —«cp /origen -> /destino», «send pool/ds@s -> host»—,
    // que el agente compone al recibirlo. El tipo solo («mutation») no lo dice.
    const bool detalle = pet.tiene("--detail");
    std::vector<std::pair<std::string, ZfsmUrl>> maquinas;   // (id, url)
    if (todas) {
        for (const auto& p : e.conns.perfiles) {
            const std::string id = p.id.empty() ? p.name : p.id;
            if (e.conns.desconectada(id)) {
                continue;
            }
            ZfsmUrl u;
            u.kind = ZfsmKind::Connection;
            u.connection = id;
            maquinas.emplace_back(id, u);
        }
    } else {
        maquinas.emplace_back(destino.connection, destino);
    }
    Tabla t;
    t.nombreJson = "jobs";
    t.cabecerasTexto = {T("t_cab_conexion", "CONEXIÓN"), T("t_cab_id", "ID"),
                        T("t_cab_estado", "ESTADO"), T("t_cab_tipo", "TIPO"),
                        T("t_cab_instantanea", "SNAPSHOT"), T("t_cab_bytes", "BYTES"),
                        T("t_cab_mib_s", "MiB/s"), T("t_cab_segundos", "SEGUNDOS"),
                        T("t_cab_error", "ERROR")};
    t.campos = {"connection", "id", "state", "type", "snap", "bytes", "rate", "elapsed", "error"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena,
               Tipo::Bytes,  Tipo::Cadena, Tipo::Entero, Tipo::Cadena};
    if (detalle) {
        t.cabecerasTexto.push_back(T("t_cab_que_hace", "QUÉ HACE"));
        t.campos.push_back("description");
        t.tipos.push_back(Tipo::Cadena);
    }
    bool algunaFallo = false;
    for (const auto& [idMaquina, url] : maquinas) {
        std::string out;
        if (!agente(e, url, PET::jobList(), out, 30000)) {
            // Con -a, una máquina que no contesta no tapa a las demás: se dice y se sigue.
            if (!todas) {
                return false;
            }
            algunaFallo = true;
            continue;
        }
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
            std::vector<std::string> fila{idMaquina, j["id"].toString(), j["state"].toString(),
                                          j["type"].toString(), j["snap"].toString(),
                                          std::to_string(j["bytes"].toInt()), ritmo,
                                          std::to_string(j["elapsed"].toInt()), B::trim(textoErr)};
            if (detalle) {
                const std::string d = j["desc"].toString();
                fila.push_back(d.empty() ? std::string("-") : d);   // agente anterior: no lo dice
            }
            t.filas.push_back(fila);
        }
    }
    if (algunaFallo) {
        e.ultimoRc = 1;
    }
    if (t.filas.empty()) {
        // Con filtro puesto, «no hay trabajos» es engañoso: puede haber diez terminados. Se
        // dice cuál era el filtro para que la respuesta signifique lo que parece.
        std::fputs(estados.empty()
                       ? TC("t_no_hay_tra_f891dc", "no hay trabajos\n")
                       : TC("t_no_hay_tra_filtro2", "no hay trabajos en curso (pruebe «jobs --history»)\n"),
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
        if (!agente(e, destino, PET::cancelJob(id), out, 30000)) {
            return false;
        }
        std::fprintf(stderr, TC("t_cancelado__b0d1d4", "cancelado el trabajo %s\n"), id.c_str());
        return true;
    }
    std::string out;
    if (!agente(e, destino, PET::jobStatus(id), out, 30000)) {
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
// Antes no era así: `send` iba como trabajo y se esperaba con `--wait`, mientras que
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
    // `enqueue` además comprueba que el daemon sepa encolar ESE verbo: mandarle uno que no
    // está en su lista es un viaje para recibir un rc=2.
    const std::vector<std::string> conJob = PET::enqueue(argv);
    if (conJob.empty()) {
        std::fputs(TC("t_no_encolable", "esa orden no se puede encolar como trabajo\n"), stderr);
        return false;
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

bool zpoolGeneric(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv) {
    std::string out;
    if (!agente(e, destino, PET::zpoolGeneric(B::helpers::agentArgv(argv)),
                out, 0)) {
        return false;
    }
    if (!B::trim(out).empty()) {
        std::fprintf(stdout, "%s", out.c_str());
    }
    return true;
}






bool cmdMantenimientoPool(Estado& e, const LineaAnalizada& linea, const char* op) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    // La traducción de la fase a su letra —que NO es la misma para las tres operaciones— y
    // el orden de los argumentos viven en `commands::pools`, compartidos con el servidor web.
    // Aquí solo se decide QUÉ operación y con qué palabras la pidió el usuario.
    const std::string fase = B::toLowerAscii(pet.uno("fase"));
    PL::Phase faseOp = PL::Phase::Start;
    if (fase == "stop" || fase == "cancel") {
        faseOp = PL::Phase::Stop;
    } else if (fase == "pause" || fase == "suspend") {
        faseOp = PL::Phase::Pause;
    }
    const std::string sub(op);
    const PL::Operation opPool = sub == "scrub"      ? PL::Operation::Scrub
                                 : sub == "trim"     ? PL::Operation::Trim
                                                     : PL::Operation::Initialize;
    std::vector<std::string> discos;
    if (!discosDe(e, pet.lista("disco"), destino.connection, discos)) {
        return false;
    }
    std::vector<std::string> argv = PL::argv(opPool, destino.pool, faseOp, pet.nativas(), discos);
    if (argv.empty()) {
        std::fputs(TC("t_pool_op_invalida", "no se puede pedir eso sobre ese pool\n"), stderr);
        return false;
    }
    if (!zpoolGeneric(e, destino, argv)) {
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
    std::vector<std::string> discos;
    if (!discosDe(e, pet.lista("disco"), destino.connection, discos)) {
        return false;
    }
    for (const std::string& d : discos) {
        argv.push_back(d);
    }
    if (!zpoolGeneric(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, "%s: %s hecho\n", destino.pool.c_str(), op);
    return true;
}

// El estado detallado. Es texto para leer y se saca tal cual: fingir columnas donde
// `zpool status` dibuja un árbol de vdevs sería inventarse una estructura que no tiene.
// Un pool que NO está importado pero aparece en la sonda: su estado, su motivo y sus
// dispositivos, cada uno con la ruta real en esa máquina. Es lo que hace falta para saber,
// por ejemplo, de qué disco sale una etiqueta vieja antes de limpiarla. `false` si el pool
// no aparece.
bool poolImportable(Estado& e, const ZfsmUrl& conexion, const std::string& nombre,
                    H::ImportablePoolInfo& info, std::map<std::string, std::string>& rutas) {
    std::string sonda;
    if (!agente(e, conexion, PET::importableProbe(), sonda, 60000)) {
        return false;
    }
    bool hay = false;
    for (const H::ImportablePoolInfo& imp : H::parseZpoolImportOutput(sonda)) {
        if (imp.pool == nombre) {
            // `zpool import` y `zpool import -s` pueden dar el mismo pool dos veces: se
            // queda el que trae más dispositivos.
            if (!hay || imp.devices.size() > info.devices.size()) {
                info = imp;
            }
            hay = true;
        }
    }
    if (!hay) {
        return false;
    }
    std::vector<std::string> nombres;
    for (const H::ImportableDevice& d : info.devices) {
        if (d.leaf) {
            nombres.push_back(d.name);
        }
    }
    std::string out;
    if (!nombres.empty() && agente(e, conexion, PET::resolveDevices(nombres), out, 20000)) {
        for (const auto& kv : clavesDe(out)) {
            rutas[kv.first] = kv.second;
        }
    }
    return true;
}

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
    std::string errZpool;
    if (!agente(e, destino, PET::poolStatus(destino.pool), out, 60000, &errZpool)) {
        // ¿No está importado? Si aparece entre los importables, se cuenta lo que se sabe de
        // él en vez de dejar el «no such pool» de zpool: esa respuesta es cierta y no ayuda.
        H::ImportablePoolInfo imp;
        std::map<std::string, std::string> rutas;
        if (!poolImportable(e, subeHasta(destino, Objetivo::Conexion), destino.pool, imp, rutas)) {
            if (!errZpool.empty()) {
                std::fprintf(stderr, "%s\n", errZpool.c_str());
            }
            e.ultimoRc = 1;
            return false;
        }
        e.ultimoRc = 0;
        std::fprintf(stdout, TC("t_status_importable",
                                "  pool: %s  (NO importado)\n    id: %s\n state: %s\n"),
                     imp.pool.c_str(), imp.guid.c_str(), imp.state.c_str());
        if (!imp.reason.empty()) {
            std::fprintf(stdout, "status: %s\n", imp.reason.c_str());
        }
        if (!imp.action.empty()) {
            std::fprintf(stdout, "action: %s\n", imp.action.c_str());
        }
        std::fputs(TC("t_status_dispositivos", "dispositivos:\n"), stdout);
        for (const H::ImportableDevice& d : imp.devices) {
            const std::string sangria(static_cast<std::size_t>(2 + 2 * d.depth), ' ');
            const auto ruta = rutas.find(d.name);
            const std::string donde = !d.leaf ? std::string()
                                      : (ruta != rutas.end() && !ruta->second.empty())
                                          ? "  -> " + ruta->second
                                          : std::string("  -> ?");
            std::fprintf(stdout, "%s%s%s  %s  %s\n", sangria.c_str(), d.name.c_str(), donde.c_str(),
                         d.state.c_str(), d.detail.c_str());
        }
        return true;
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
    if (!agente(e, destino, PET::poolHistory(destino.pool), out, 60000)) {
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
        if (!agente(e, destino, PET::importableProbe(), out, 60000)) {
            return false;
        }
        // La salida CRUDA de la sonda no vale como respuesta. La sonda ejecuta `zpool
        // import` Y `zpool import -s` y pega las dos salidas, así que un pool que aparece
        // en las dos —lo normal— salía DOS VECES, en dos bloques idénticos de siete líneas
        // cada uno; y con un pool de verdad detrás, quien lea la lista no puede saber si
        // son dos pools distintos con el mismo nombre o el mismo contado dos veces. `ls` ya
        // lo analizaba y lo unificaba por nombre; esto hace lo mismo, y además es lo que
        // permite dar la lista en tsv o en json como el resto de las órdenes.
        Tabla t;
        t.nombreJson = "importable_pools";
        t.cabecerasTexto = {T("t_cab_pool", "POOL"), T("t_cab_id", "ID"),
                            T("t_cab_estado", "ESTADO"), T("t_cab_motivo", "MOTIVO")};
        t.campos = {"pool", "guid", "state", "reason"};
        std::set<std::string> yaEstan;
        for (const H::ImportablePoolInfo& imp : H::parseZpoolImportOutput(out)) {
            if (imp.pool.empty() || !yaEstan.insert(imp.pool).second) {
                continue;
            }
            t.filas.push_back({imp.pool, imp.guid, imp.state, imp.reason});
        }
        avisaSondaImportables(out, !t.filas.empty());
        t.imprime(e.formato);
        return true;
    }
    // Las banderas de `zpool import` que la orden declara, TODAS: antes se construía
    // `import [-f] <pool>` y `-d`, `-R`, `-N`… se aceptaban sin pasarse. `-d` admite además
    // `zfsm://<esta máquina>#path/dir`.
    std::vector<std::string> argv{"import"};
    const std::vector<std::string> nat = pet.nativas();
    for (std::size_t i = 0; i < nat.size(); ++i) {
        if (nat[i] == "-d" && i + 1 < nat.size()) {
            std::string dir;
            if (!rutaDeRanura(e, nat[i + 1], destino.connection, dir)) {
                return false;
            }
            argv.push_back("-d");
            argv.push_back(dir);
            ++i;
            continue;
        }
        argv.push_back(nat[i]);
    }
    argv.push_back(pet.uno("texto"));
    if (!pet.valor("as").empty()) {
        argv.push_back(pet.valor("as"));  // importar con otro nombre
    }
    if (!zpoolGeneric(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, "importado %s\n", pet.uno("texto").c_str());
    return true;
}

// Crear un pool. Es la orden MÁS destructiva de todas: escribe en los dispositivos que se
// le den, y si alguno tenía datos, desaparecen. La confirmación los enumera uno a uno.
bool cmdCrearPool(Estado& e, const Peticion& pet, const ZfsmUrl& destino,
                  const std::string& nombre) {
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
    // El nombre viene DADO y no se toma de textos.front(): desde la raíz, `create
    // unibody/apar sda` lo trae ya separado de la máquina en la que se crea.
    const std::string pool = nombre.empty() ? textos.front() : nombre;
    std::vector<std::string> vdevs;
    if (!discosDe(e, std::vector<std::string>(textos.begin() + 1, textos.end()),
                  destino.connection, vdevs)) {
        return false;
    }
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
    if (!zpoolGeneric(e, destino, argv)) {
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
    // Con la clave sin estar, conectarla solo serviría para que `recarga` la vuelva a apartar.
    if (conectar) {
        const auto* perfil = buscarConexion(e.conns, id);
        std::string ruta;
        if (perfil && sshClaveFalta(*perfil, ruta)) {
            std::fprintf(stderr,
                         TC("t_clave_falta_connect",
                            "la clave SSH de «%s» sigue sin existir (%s): corríjala con «edit» o "
                            "ponga el fichero, y vuelva a conectarla\n"),
                         id.c_str(), ruta.c_str());
            e.ultimoRc = 1;
            return false;
        }
    }
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
    std::fprintf(stderr,
                 conectar ? TC("t_marcada_conectada", "%s marcada como conectada\n")
                          : TC("t_marcada_desconectada", "%s marcada como desconectada\n"),
                 id.c_str());
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
        olvidaEstadoDeConexion(e, *p);
    }
    recarga(e);

    // Y ahora se pregunta a la máquina. Los dos verbos que la interfaz usa para lo mismo.
    std::string basicos;
    if (!agente(e, destino, PET::refreshBasics(), basicos, 30000)) {
        return false;
    }
    std::string salud;
    std::string err;
    int rc = -1;
    std::string motivo;
    const auto* p = buscarConexion(e.conns, id);
    ejecutarAgente(*e.ses, *p, {"--health"}, salud, err, rc, &motivo, 20000);

    // El IDENTIFICADOR DE MÁQUINA se guarda, no solo se enseña.
    //
    // El daemon lo publica en cada refresco y los dos clientes lo tiraban: lo leían, lo
    // pintaban y se olvidaban. Resultado, en esta instalación: `local` y `mmela` tenían uid
    // —puesto a mano hace tiempo— y `unib` y `mbp` lo tenían VACÍO.
    //
    // Hace falta porque es la única identidad de una máquina que no depende de quién la
    // mire. El nombre no lo es: `local` significa una máquina distinta según desde dónde se
    // pregunte, y la misma máquina aparece como `unib` en una lista y `unibody` en otra. Es
    // lo que `peers` necesita para que dos daemons sepan de quién hablan.
    {
        const auto claves = clavesDe(basicos);
        const auto it = claves.find("MACHINE_UUID");
        const std::string uid =
            it == claves.end() ? std::string() : B::refresh::extractMachineUuid(it->second);
        const auto* vivo = buscarConexion(e.conns, id);
        if (!uid.empty() && vivo && B::trim(vivo->machineUid) != uid && !e.ses->sinSecretos) {
            B::ConnectionProfile copia = *vivo;
            copia.machineUid = uid;
            std::string errGuardar;
            if (guardarConexion(*e.ses, copia, errGuardar)) {
                recarga(e);
            } else {
                // Se avisa y se sigue: no poder guardarlo no invalida el refresco, pero
                // callarlo dejaría a `peers` sin identidad sin que nadie supiera por qué.
                std::fprintf(stderr, TC("t_uid_no_guardado",
                                        "aviso: no se pudo guardar el identificador de máquina "
                                        "de %s: %s\n"),
                             id.c_str(), errGuardar.c_str());
            }
        }
    }

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
        std::fprintf(stderr, "%s\n", detalle.empty()
                                       ? TC("t_holds_ilegibles", "no se pudieron leer los holds")
                                       : detalle.c_str());
        e.ultimoRc = rc == 0 ? 1 : rc;
        return false;
    }
    Tabla t;
    t.nombreJson = "holds";
    t.cabecerasTexto = {T("t_cab_instantanea", "SNAPSHOT"), T("t_cab_etiqueta", "ETIQUETA"),
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
    // El orden —etiqueta primero— y la validación viven en `commands::snapshots`,
    // compartidos con el servidor web. Se usa la forma de argv de `zfs` y no el verbo tipado
    // porque el intérprete admite `-r`, y el tipado NO: ese lee exactamente dos parámetros.
    const std::vector<std::string> argv =
        poner ? INST::argvZfsHold(pet.uno("etiqueta"), destino.zfsName(), pet.tiene("-r"))
              : INST::argvZfsRelease(pet.uno("etiqueta"), destino.zfsName(), pet.tiene("-r"));
    if (argv.empty()) {
        std::fputs(TC("t_retencion_invalida",
                      "la etiqueta no puede llevar espacios, arrobas ni barras, y hay que "
                      "nombrar un snapshot\n"),
                   stderr);
        return false;
    }
    if (!zfsGeneric(e, destino, argv)) {
        return false;
    }
    // Una frase entera por caso: «puesta»/«quitada» iban como literales sin traducir, y en
    // inglés salía «puesta the hold».
    std::fprintf(stderr,
                 poner ? TC("t_hold_puesto", "puesto el hold «%s» en %s\n")
                       : TC("t_hold_quitado", "quitado el hold «%s» en %s\n"),
                 pet.uno("etiqueta").c_str(), destino.zfsName().c_str());
    return true;
}

// Qué cambió entre dos instantáneas del mismo dataset.
bool cmdDiff(Estado& e, const LineaAnalizada& lineaOriginal) {
    // Un «@snapshot» suelto en un extremo se completa con el dataset del OTRO, no con el
    // sitio actual: `diff` compara dos puntos del MISMO dataset, así que es el único que
    // puede ser. Antes `diff zfsm://c/tank/ds@b --from @a` desde la raíz contestaba «hace
    // falta estar en un dataset», con el dataset escrito en la propia orden.
    LineaAnalizada linea = lineaOriginal;
    {
        const auto itFrom = linea.opciones.find("from");
        auto itDest = linea.ranuras.find("destino");
        const bool fromSuelto = itFrom != linea.opciones.end() && !itFrom->second.empty()
                                && itFrom->second.front() == '@';
        const bool destSuelto = itDest != linea.ranuras.end() && !itDest->second.empty()
                                && itDest->second.front() == '@';
        const auto completaCon = [&](const std::string& otroTexto, const std::string& suelto,
                                     std::string& completo) {
            ZfsmUrl otro;
            std::string err;
            if (!resuelve(e, otroTexto, otro, err) || otro.zfsName().empty()) {
                return false;
            }
            otro.snapshot.clear();
            otro.section.clear();
            otro.detail.clear();
            completo = B::formatZfsmUrl(otro) + suelto;
            return true;
        };
        std::string completo;
        if (fromSuelto && itDest != linea.ranuras.end() && !destSuelto
            && completaCon(itDest->second, itFrom->second, completo)) {
            linea.opciones["from"] = completo;
        } else if (destSuelto && itFrom != linea.opciones.end() && !fromSuelto
                   && completaCon(itFrom->second, itDest->second, completo)) {
            itDest->second = completo;
        }
    }
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    if (pet.lista("destino").empty()) {
        std::fputs(TC("t_uso_diff_i_10a43a", "uso: diff <@snapshot-o-dataset> [--from <@snapshot>]\n"
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
        std::fprintf(stderr, TC("t_el_origen__604ce4", "el origen tiene que ser un snapshot (ahora: %s)\n"), textoDe(origen).c_str());
        return false;
    }
    std::string out;
    if (!agente(e, origen, PET::diffBetween(origen.zfsName(), hasta.zfsName()), out, 120000)) {
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
    if (!agente(e, destino, PET::blockDevices(), out, 25000)) {
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

    // --- Un dispositivo REAL por fila, y sus alias debajo.
    //
    // El agente manda los `/dev/disk/by-id/...` como entradas aparte, cada una con
    // `resolved` apuntando al dispositivo de verdad. El cliente tiraba ese campo y las
    // pintaba como si fueran discos más: en esta máquina salían 36 filas de las que 27 eran
    // el MISMO disco visto por otro nombre, con tamaño 0B y sin tipo ni sistema de ficheros.
    // Tres cuartas partes del listado eran ruido, y el ruido tapaba justo lo que se viene a
    // ver: qué discos hay y cuáles están libres.
    //
    // Los alias NO se tiran, se subordinan. Son la forma correcta de nombrar un disco al
    // crear un pool —`/dev/sdb` puede ser `sdc` en el siguiente arranque y el pool se rompe;
    // el alias por identificador no cambia—, así que esconderlos sería cambiar un defecto
    // por otro.
    struct Fila {
        std::string path, size, type, fstype, mountpoint;
        bool ocupado = false;
        std::vector<std::string> alias;
    };
    std::vector<Fila> filas;
    std::map<std::string, std::size_t> porRuta;   // ruta real -> posición en `filas`
    std::vector<B::json::Value> sueltos;          // alias cuyo destino no está en la lista

    for (const auto& d : raiz["devices"].toArray()) {
        if (!d["resolved"].toString().empty()) {
            continue;  // los alias en la segunda pasada, cuando ya están todos los reales
        }
        Fila f;
        f.path       = d["path"].toString();
        f.size       = std::to_string(d["size"].toInt());
        f.type       = d["type"].toString();
        f.fstype     = d["fstype"].toString();
        f.mountpoint = d["mountpoint"].toString();
        f.ocupado    = d["inuse"].toBool();
        porRuta[f.path] = filas.size();
        filas.push_back(f);
    }
    for (const auto& d : raiz["devices"].toArray()) {
        const std::string destino = d["resolved"].toString();
        if (destino.empty()) {
            continue;
        }
        const auto it = porRuta.find(destino);
        if (it == porRuta.end()) {
            // Un alias que apunta a algo que no está en la lista NO se pierde: se queda como
            // fila propia. Perder una entrada en silencio al reorganizar un listado es
            // exactamente la clase de arreglo que crea un problema peor que el que resuelve.
            sueltos.push_back(d);
            continue;
        }
        filas[it->second].alias.push_back(d["path"].toString());
    }
    for (const auto& d : sueltos) {
        Fila f;
        f.path       = d["path"].toString();
        f.size       = std::to_string(d["size"].toInt());
        f.type       = d["type"].toString();
        f.fstype     = d["fstype"].toString();
        f.mountpoint = d["mountpoint"].toString();
        f.ocupado    = d["inuse"].toBool();
        filas.push_back(f);
    }
    // Orden estable dentro de cada dispositivo: los alias llegan en el orden del directorio,
    // que es el de creación del sistema de ficheros y no significa nada.
    for (Fila& f : filas) {
        std::sort(f.alias.begin(), f.alias.end());
    }

    Tabla t;
    t.nombreJson = "devices";
    t.cabecerasTexto = {T("t_cab_ruta", "RUTA"),       T("t_cab_tamano", "TAMAÑO"),
                        T("t_cab_tipo", "TIPO"),       T("t_cab_fs", "FS"),
                        T("t_cab_punto_de_montaje", "PUNTO DE MONTAJE"),
                        T("t_cab_ocupado", "OCUPADO")};
    t.campos = {"path", "size", "type", "fstype", "mountpoint", "inuse"};
    t.tipos = {Tipo::Cadena, Tipo::Bytes, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Booleano};

    // Para LEER, los alias van debajo y sangrados. Para un GUION van en una columna: una
    // sangría no se puede cortar por tabuladores ni meter en un array de JSON, y quien
    // llama a esto desde un guion quiere los alias de un disco juntos, no repartidos en
    // líneas que hay que volver a coser.
    const bool paraLeer = (e.formato == Formato::Texto);
    if (!paraLeer) {
        t.cabecerasTexto.push_back(T("t_cab_alias", "ALIAS"));
        t.campos.push_back("aliases");
        t.tipos.push_back(Tipo::Lista);
    }
    for (const Fila& f : filas) {
        if (soloLibres && f.ocupado) {
            continue;
        }
        std::vector<std::string> fila{f.path, f.size, f.type, f.fstype, f.mountpoint,
                                      f.ocupado ? "true" : "false"};
        if (!paraLeer) {
            fila.push_back(B::join(f.alias, ","));
            t.filas.push_back(fila);
            continue;
        }
        t.filas.push_back(fila);
        for (const std::string& a : f.alias) {
            // UNA sola celda, no seis vacías. `Tabla` recorre la fila que le dan, así que
            // una fila corta imprime solo lo que lleva; con seis celdas vacías salía un «-»
            // en cada columna, que es cambiar un ruido por otro.
            t.filas.push_back({"    " + a});
        }
    }
    t.imprime(e.formato);
    return true;
}

// El punto de montaje de un dataset. Solo Unix: en Windows la ruta no es la que dice
// `mountpoint` —el pool se monta en una letra de unidad— y ahí la sincronización va por
// otro camino, que no está portado al intérprete.
// Dónde se abre el dataset de un extremo de `cp` o `rsync`. Se pregunta a `zfs mount`, no a
// la propiedad: sin montar, la ruta de la propiedad puede ser de otro (ver dondeEstaMontado).
// Para leer, un dataset sin montar se monta en solo lectura; para escribir, no.
bool montajeDe(Estado& e, const ZfsmUrl& u, std::string& out, bool paraEscribir) {
    return dondeEstaMontado(e, u, u.zfsName(), paraEscribir, /*callado=*/false, out);
}


// La copia entre máquinas no conserva ACL ni atributos extendidos, y el agente que envía
// cuenta los ficheros que los llevaban (`SIN_ACL_XATTR=n`). Se dice aquí, en una frase y en
// el idioma de quien lo lee: `send` es la vía que lo conserva todo.
void avisaSinMetadatos(const std::string& salida) {
    const std::size_t pos = salida.find("SIN_ACL_XATTR=");
    if (pos == std::string::npos) {
        return;
    }
    const std::size_t ini = pos + std::string("SIN_ACL_XATTR=").size();
    const std::size_t fin = salida.find('\n', ini);
    const std::string n = B::trim(salida.substr(ini, fin == std::string::npos ? std::string::npos : fin - ini));
    if (n.empty() || n == "0") {
        return;
    }
    std::fprintf(stderr,
                 TC("t_aviso_sin_acl_xattr",
                    "aviso: %s ficheros llevaban ACL o atributos extendidos, y la copia entre "
                    "máquinas no los conserva; para llevarlos tal cual, use «send»\n"),
                 n.c_str());
}

// «Sincronizar» de la interfaz: copia el CONTENIDO de un dataset a otro con rsync.
//
// Faltaba entera en el intérprete, y encima el hueco estaba disimulado: existía un `sync`
// que es `zpool sync` —forzar la escritura pendiente, cosa de un instante— con el mismo
// nombre que la acción de la interfaz, que puede tardar horas. Ese se llama ahora `flush`.
// Copiar un árbol de una ruta a otra, eligiendo motor según si cruzan máquina.
//
// Los dos motores ya existían y los usa `rsync`: dentro de una máquina, el `rsync` del
// daemon; entre dos, el árbol entre agentes. Esto no añade ninguno, solo los pone detrás de
// una misma llamada para que `cp` no tenga que saber de los dos.
// El último tramo de una ruta, con «/» o con «\».
std::string hojaDe(const std::string& ruta) {
    const std::size_t barra = ruta.find_last_of("/\\");
    return barra == std::string::npos ? ruta : ruta.substr(barra + 1);
}

// ¿Qué hay en `ruta` de la máquina `conexion`? Lo contesta `--dump-path-info`, que mira
// cualquier ruta y no solo las de dentro de un montaje. Falso si no se pudo preguntar (el
// motivo ya está dicho); `existe` falso si se preguntó y no hay nada.
bool estadoDeRuta(Estado& e, const std::string& conexion, const std::string& ruta, bool& existe,
                  bool& esDirectorio) {
    existe = false;
    esDirectorio = false;
    ZfsmUrl u;
    u.kind = ZfsmKind::Connection;
    u.connection = conexion;
    std::string out;
    if (!agente(e, u, PET::pathInfo(ruta), out, 20000)) {
        return false;
    }
    B::json::Value v;
    std::string err;
    if (!B::json::parse(out, v, &err)) {
        std::fprintf(stderr, TC("t_contenido_ilegible", "respuesta ilegible al listar %s: %s\n"),
                     ruta.c_str(), err.c_str());
        e.ultimoRc = 1;
        return false;
    }
    existe = v["exists"].toBool();
    esDirectorio = v["is_dir"].toBool();
    return true;
}

// ¿El agente de `conexion` anuncia `capacidad` en su `--health`? Para lo que un agente
// anterior no rechazaría sino que IGNORARÍA en silencio, como el `--as` del emisor del árbol.
bool agenteAnuncia(Estado& e, const std::string& conexion, const std::string& capacidad) {
    ZfsmUrl u;
    u.kind = ZfsmKind::Connection;
    u.connection = conexion;
    std::string out;
    if (!agente(e, u, {"--health"}, out)) {
        return false;
    }
    for (const std::string& linea : B::split(out, "\n", true)) {
        if (linea.rfind("CAPS=", 0) == 0) {
            for (const std::string& c : B::split(B::trim(linea.substr(5)), ",", true)) {
                if (B::trim(c) == capacidad) {
                    return true;
                }
            }
        }
    }
    return false;
}

// `ruta` partida en su directorio y su último tramo. En Windows el padre de «C:/x» es «C:/»,
// no «C:», que sería «el directorio actual de la unidad C».
bool padreYHoja(const std::string& ruta, std::string& padre, std::string& hoja) {
    std::string r = ruta;
    while (r.size() > 1 && (r.back() == '/' || r.back() == '\\')) {
        r.pop_back();
    }
    const std::size_t barra = r.find_last_of("/\\");
    if (barra == std::string::npos || barra + 1 >= r.size()) {
        return false;
    }
    hoja = r.substr(barra + 1);
    padre = barra == 0 ? std::string("/") : r.substr(0, barra);
    if (padre.size() == 2 && padre[1] == ':') {
        padre += "/";
    }
    return hoja != "." && hoja != "..";
}

// ¿`ruta` es un fichero —y no un directorio— en la máquina de `u`? Se mira en el listado de
// su directorio padre, que es lo que el agente ya sabe contestar. Si no se puede saber, se
// dice que no: el camino de siempre, el del árbol.
bool esFicheroEn(Estado& e, const ZfsmUrl& u, const std::string& ruta) {
    const std::size_t barra = ruta.find_last_of("/\\");
    if (barra == std::string::npos || barra + 1 >= ruta.size()) {
        return false;
    }
    const std::string padre = barra == 0 ? std::string("/") : ruta.substr(0, barra);
    const std::string hoja = ruta.substr(barra + 1);
    std::string out;
    std::vector<L::DirectoryEntry> entradas;
    std::string errJson;
    // `pathContents` y no `directoryContents`: este último se queda dentro de los montajes de
    // ZFS, y un extremo `#path` puede estar en cualquier sitio. Con él, un fichero suelto
    // fuera de ZFS se tomaba por directorio.
    if (!agente(e, u, PET::pathContents(padre), out)
        || !L::directoryContents(out, entradas, errJson)) {
        return false;
    }
    for (const L::DirectoryEntry& ent : entradas) {
        if (ent.name == hoja) {
            return !ent.directory;
        }
    }
    return false;
}

bool copiaArbol(Estado& e, const std::string& maqOrigen, const std::string& rutaOrigen,
                const std::string& maqDestino, const std::string& baseDestino,
                const std::string& nombre, bool simula, bool esperar) {
    const std::string rutaDestino =
        baseDestino + (baseDestino.empty() || baseDestino.back() == '/' ? "" : "/") + nombre;
    const bool mismaMaquina =
        B::toLowerAscii(maqOrigen) == B::toLowerAscii(maqDestino);
    ZfsmUrl uOrigen;
    uOrigen.kind = ZfsmKind::Connection;
    uOrigen.connection = maqOrigen;

    if (mismaMaquina) {
        // El motor PROPIO del proyecto, no el `rsync` del sistema.
        //
        // `copytree` es el que ya usan `todir`, `breakdown` y `assemble`, y hace todo lo que
        // hace falta: ficheros dispersos —un disco de máquina virtual de 100 GB con 8 GB
        // usados no llega ocupando 100—, fechas con nanosegundos, enlaces duros, dueño y
        // modo. Llamar al `rsync` del sistema era una dependencia externa para hacer lo
        // mismo, y dos motores locales que se comportan parecido pero no igual.
        //
        // La orden `rsync` sigue usando el suyo: ahí lo que se pide es ESPEJAR, con
        // `--delete`, y eso es otra conversación que no toca cambiar de paso.
        const std::string carga =
            SY::copyLocalPayload({{rutaOrigen, rutaDestino}}, /*borra=*/false, simula);
        if (carga.empty()) {
            std::fputs(TC("t_cp_rutas_mal",
                          "las rutas de origen y destino tienen que ser absolutas\n"), stderr);
            return false;
        }
        const std::vector<std::string> argv = PET::copyLocal(carga);
        // Una simulación se LEE ahora: no hay nada que encolar y lo que uno quiere es verlo.
        if (simula) {
            std::string out;
            if (!agente(e, uOrigen, argv, out, 0)) {
                return false;
            }
            std::fprintf(stdout, "%s", out.c_str());
            return true;
        }
        // Lo mismo que hace `lanzaOEspera`, pero sin fabricar una `Peticion` falsa solo
        // para que lea de ella una bandera que ya tenemos aquí como un `bool`.
        if (!esperar) {
            return enviaComoTrabajo(e, uOrigen, argv);
        }
        std::string out;
        if (!agente(e, uOrigen, argv, out, 0)) {   // sin plazo: mueve datos de verdad
            return false;
        }
        std::fprintf(stdout, "%s", out.c_str());
        return true;
    }

    // Entre máquinas: el destino escucha y el origen le manda el árbol directamente, sin
    // pasar por aquí. El directorio de destino tiene que EXISTIR —lo exige el receptor— y
    // eso no se arregla creándolo: crear rutas arbitrarias en una máquina remota es
    // justamente la superficie que este proyecto ha decidido no tener.
    std::string errSrc;
    std::string errDst;
    ZfsmUrl uDestino;
    uDestino.kind = ZfsmKind::Connection;
    uDestino.connection = maqDestino;
    const auto* pSrc = perfilDe(e, uOrigen, errSrc);
    const auto* pDst = perfilDe(e, uDestino, errDst);
    if (!pSrc || !pDst) {
        std::fprintf(stderr, "%s\n", (pSrc ? errDst : errSrc).c_str());
        return false;
    }
    const B::ConnectionProfile src = conSudo(e, *pSrc);
    const B::ConnectionProfile dst = conSudo(e, *pDst);
    const auto llama = [&e](const B::ConnectionProfile& maquina,
                            const std::vector<std::string>& argv, int timeoutMs,
                            std::string& salida, std::string& error, int& rc) {
        return ejecutarAgente(*e.ses, maquina, argv, salida, error, rc, nullptr, timeoutMs);
    };
    // ¿Es un FICHERO? Entonces el receptor se pone en el directorio de destino —que ya
    // existe— y el origen manda un árbol de una sola entrada con el nombre del fichero.
    //
    // Hacía falta saberlo AQUÍ, antes de preparar nada: el paso siguiente creaba en el
    // destino un directorio con el nombre del fichero, y el envío fallaba después con «no es
    // un directorio». Quedaba basura con forma de éxito a medias.
    const bool esFichero = esFicheroEn(e, uOrigen, rutaOrigen);
    {
        const std::string hoja = hojaDe(rutaOrigen);
        {
            // Con otro nombre lo pone el EMISOR (`--as`), que manda el manifiesto con el
            // nombre de llegada. Un emisor anterior lo ignoraría y lo mandaría con el suyo:
            // se mira antes de pedirlo.
            if (esFichero && nombre != hoja && !agenteAnuncia(e, maqOrigen, "--tree-send-as")) {
                std::fprintf(stderr,
                             TC("t_cp_as_fichero_remoto",
                                "el agente de %s no sabe mandar un fichero con otro nombre (llegaría "
                                "como «%s»): actualícelo con «install-daemon»\n"),
                             maqOrigen.c_str(), hoja.c_str());
                e.ultimoRc = 1;
                return false;
            }
        }
    }
    // El directorio donde aterriza tiene que EXISTIR antes: el receptor del árbol no lo
    // crea y se planta con «no es un directorio». Se crea con un verbo que solo puede crear
    // DENTRO de algo que ya está, no en cualquier parte del sistema.
    //
    // En una simulación no se crea nada: `--check` promete no tocar, y crear un directorio
    // es tocar. Un fichero no lo necesita: aterriza en `baseDestino`, que ya existe.
    if (!simula && !esFichero) {
        std::string prepOut;
        std::string prepErr;
        int prepRc = -1;
        const std::vector<std::string> prep = AV::argvPrepareDir(baseDestino, nombre);
        if (prep.empty()
            || !ejecutarAgente(*e.ses, dst, prep, prepOut, prepErr, prepRc, nullptr, 60000)
            || prepRc != 0) {
            std::fprintf(stderr, TC("t_cp_destino_no", "no se pudo preparar el destino: %s\n"),
                         B::trim(B::helpers::oneLine(prepErr)).c_str());
            return false;
        }
    }
    std::string salidaEnvio;
    const auto hecho = TR::launchTreeJob(e.ses->transporte, llama, src, dst, rutaOrigen,
                                         esFichero ? baseDestino : rutaDestino,
                                         /*mismaConexion=*/false, e.ses->verboso,
                                         /*comoTrabajo=*/!esperar && !simula, /*borra=*/false,
                                         simula, &salidaEnvio,
                                         esFichero && nombre != hojaDe(rutaOrigen) ? nombre
                                                                                   : std::string(),
                                         /*metadatosDeRaiz=*/!esFichero);
    if (hecho.failure != TR::JobFailure::None_) {
        std::fprintf(stderr, "%s: %s\n", TR::labelOf(hecho.failure).c_str(),
                     hecho.detail.c_str());
        e.ultimoRc = 1;
        return false;
    }
    std::fputs(salidaEnvio.c_str(), stdout);
    avisaSinMetadatos(salidaEnvio);
    return true;
}

// --- `cp`: copiar ficheros y directorios, de donde sea a donde sea.
//
// Lo que no hacía ninguna de las tres órdenes que ya copian: `rsync` exige datasets en los
// dos extremos, `todir` saca de un dataset a un directorio LOCAL, y `fromdir` mete un
// directorio en el dataset actual por una tubería tar. Entre las tres no se podía copiar de
// una ruta suelta a otra, ni un fichero suelto.
//
// No se inventa motor: dentro de una máquina va por el `rsync` del daemon y entre dos por el
// árbol entre agentes, que es lo que ya usan esas órdenes. Lo que aporta esto es el
// DIRECCIONAMIENTO uniforme —la regla vive en `base::copyspec`, con pruebas— y la semántica
// de copiar en vez de espejar.
bool cmdCp(Estado& e, const LineaAnalizada& linea) {
    namespace CS = B::copyspec;
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    // `cp <origen>... <destino>`: varios orígenes, como el `cp` de toda la vida. Es también
    // lo que recibe de la rama de patrones (`cp *.log destino`), que antes llamaba una vez
    // por origen y dejaba un TRABAJO por fichero.
    const std::vector<std::string> args = pet.lista("texto");
    if (args.size() < 2) {
        std::fputs(TC("t_cp_uso_err2", "uso: cp <origen>... <destino> [--as <nombre>]\n"), stderr);
        return false;
    }
    const std::vector<std::string> textosOrigen(args.begin(), args.end() - 1);
    if (textosOrigen.size() > 1 && !pet.valor("as").empty()) {
        std::fputs(TC("t_cp_as_varios",
                      "--as pone UN nombre, y el origen casa con varios: no tiene sentido\n"),
                   stderr);
        e.ultimoRc = 1;
        return false;
    }

    // Cada extremo: se resuelve como URL y se le pasa a la regla junto con el texto crudo.
    // El texto crudo hace falta porque la barra inicial manda —`/var/tmp/x` es una ruta del
    // sistema de ficheros y no el árbol `zfsm://var/tmp/x`— y eso solo se ve en lo escrito.
    const std::string idLocal = idDe(e, ZfsmUrl{});
    const auto extremo = [&](const std::string& texto, const char* cual, CS::End& out) -> bool {
        ZfsmUrl u;
        std::string err;
        if (texto.empty() || texto.front() != '/') {
            if (!resuelve(e, texto, u, err)) {
                std::fprintf(stderr, "%s: %s\n", cual, err.c_str());
                return false;
            }
        }
        const CS::Reading r = CS::readEnd(texto, u, idLocal.empty() ? std::string("local") : idLocal);
        if (!r.ok()) {
            std::fprintf(stderr, "%s: %s\n", cual, CS::labelOf(r.failure).c_str());
            return false;
        }
        out = r.end;
        return true;
    };
    // La ruta ABSOLUTA de cada extremo. Un dataset no la tiene hasta que se le pregunta a la
    // máquina dónde está montado: el `mountpoint` de la propiedad no siempre es donde se
    // puede abrir —en Windows dice «/pool/ds», que no existe— y por eso se pregunta.
    const auto rutaReal = [&](const CS::End& x, std::string& out, bool paraEscribir) -> bool {
        if (x.kind == CS::EndKind::PlainPath) {
            out = x.absolute;
            return true;
        }
        ZfsmUrl u;
        u.kind = ZfsmKind::Dataset;
        u.connection = x.connection;
        u.dataset = x.zfsName;
        u.pool = B::split(x.zfsName, "/", false).front();
        std::string base;
        if (!montajeDe(e, u, base, paraEscribir)) {
            return false;
        }
        out = base;
        if (!x.relative.empty()) {
            out += (out.empty() || out.back() == '/' ? "" : "/") + x.relative;
        }
        return true;
    };

    CS::End destino;
    std::string rutaDestino;
    if (!extremo(args.back(), TC("t_cp_destino", "destino"), destino)
        || !rutaReal(destino, rutaDestino, /*paraEscribir=*/true)) {
        return false;
    }

    // **Lo que hace `cp` de toda la vida según lo que haya en el destino:**
    //   - un directorio: se copia DENTRO, con su nombre (`cp a dir` deja `dir/a`);
    //   - nada: el destino ES la copia. Un fichero se copia con ese nombre, y un directorio
    //     crea ese directorio con su contenido. Su padre sí tiene que existir;
    //   - un fichero: un fichero lo sobrescribe; un directorio no puede.
    // Con varios orígenes el destino tiene que ser un directorio que exista.
    //
    // Antes el destino era SIEMPRE el contenedor: si no existía, el `cp` de un fichero
    // fallaba con «no es un directorio» y el de un directorio creaba `destino/origen`.
    std::string llegaComo;           // el nombre lo da el destino, no el origen
    bool destinoEsFichero = false;   // y lo que hay ahí es un fichero que se sobrescribe
    {
        bool existe = false;
        bool esDir = false;
        if (!estadoDeRuta(e, destino.connection, rutaDestino, existe, esDir)) {
            return false;
        }
        if (!existe || !esDir) {
            if (textosOrigen.size() > 1) {
                std::fprintf(stderr,
                             TC("t_cp_varios_sin_dir",
                                "%s no es un directorio: con varios orígenes el destino tiene que "
                                "ser un directorio que exista\n"),
                             rutaDestino.c_str());
                e.ultimoRc = 1;
                return false;
            }
            if (!pet.valor("as").empty()) {
                std::fprintf(stderr,
                             TC("t_cp_as_sin_dir",
                                "%s no es un directorio: el nombre ya lo da el destino, «--as» "
                                "sobra\n"),
                             rutaDestino.c_str());
                e.ultimoRc = 1;
                return false;
            }
            std::string padre;
            std::string hoja;
            bool padreExiste = false;
            bool padreEsDir = false;
            if (!padreYHoja(rutaDestino, padre, hoja)) {
                std::fprintf(stderr, TC("t_cp_destino_sin_nombre",
                                        "no se puede copiar sobre %s\n"),
                             rutaDestino.c_str());
                e.ultimoRc = 1;
                return false;
            }
            if (!estadoDeRuta(e, destino.connection, padre, padreExiste, padreEsDir)) {
                return false;
            }
            if (!padreExiste || !padreEsDir) {
                std::fprintf(stderr, TC("t_cp_padre_no_existe",
                                        "no existe el directorio %s\n"),
                             padre.c_str());
                e.ultimoRc = 1;
                return false;
            }
            rutaDestino = padre;
            llegaComo = hoja;
            destinoEsFichero = existe;
        }
    }

    // Todos los orígenes resueltos ANTES de copiar nada: un error en el tercero no debe
    // dejar copiados los dos primeros.
    struct Copia {
        std::string maquina;
        std::string ruta;
        std::string nombre;
        std::string rutaFinal;
    };
    std::vector<Copia> copias;
    std::map<std::string, std::string> llegadas;
    for (const std::string& texto : textosOrigen) {
        CS::End origen;
        Copia c;
        if (!extremo(texto, TC("t_cp_origen", "origen"), origen) || !rutaReal(origen, c.ruta, /*paraEscribir=*/false)) {
            return false;
        }
        c.maquina = origen.connection;
        // Que el origen EXISTA, antes de lanzar nada: si no, el trabajo se encolaba y fallaba
        // después, o la copia local dejaba un destino a medias.
        {
            bool existe = false;
            bool esDir = false;
            if (!estadoDeRuta(e, c.maquina, c.ruta, existe, esDir)) {
                return false;
            }
            if (!existe) {
                std::fprintf(stderr, TC("t_cp_origen_no_existe", "no existe: %s\n"),
                             c.ruta.c_str());
                e.ultimoRc = 1;
                return false;
            }
            if (destinoEsFichero && esDir) {
                std::fprintf(stderr,
                             TC("t_cp_dir_sobre_fichero",
                                "no se puede sobrescribir el fichero %s con un directorio\n"),
                             (rutaDestino + "/" + llegaComo).c_str());
                e.ultimoRc = 1;
                return false;
            }
        }
        // Con qué nombre aterriza: el suyo (o el de `--as`) si el destino es un directorio, y
        // el del destino si no lo es.
        c.nombre = llegaComo.empty() ? CS::landingName(origen, pet.valor("as")) : llegaComo;
        if (c.nombre.empty()) {
            std::fputs(TC("t_cp_sin_nombre", "no se puede averiguar con qué nombre copiarlo; "
                          "dígalo con «--as»\n"), stderr);
            return false;
        }
        c.rutaFinal =
            rutaDestino + (rutaDestino.empty() || rutaDestino.back() == '/' ? "" : "/") + c.nombre;
        if (B::toLowerAscii(c.maquina) == B::toLowerAscii(destino.connection)
            && c.ruta == c.rutaFinal) {
            std::fputs(TC("t_cp_mismo", "el origen y el destino son el mismo sitio\n"), stderr);
            return false;
        }
        // Dos orígenes con el mismo nombre de llegada: el segundo pisaría al primero.
        const auto ya = llegadas.find(c.nombre);
        if (ya != llegadas.end()) {
            std::fprintf(stderr,
                         TC("t_cp_misma_llegada",
                            "«%s» y «%s» llegarían con el mismo nombre, «%s»: el segundo "
                            "pisaría al primero\n"),
                         ya->second.c_str(), texto.c_str(), c.nombre.c_str());
            e.ultimoRc = 1;
            return false;
        }
        llegadas[c.nombre] = texto;
        copias.push_back(c);
    }

    const bool simula = pet.tiene("--check");
    const bool esperar = pet.tiene("--wait");
    for (const Copia& c : copias) {
        std::fprintf(stderr, TC("t_cp_yendo", "%s  ->  %s\n"), c.ruta.c_str(), c.rutaFinal.c_str());
    }

    // **Todos en la misma máquina que el destino: UNA sola copia, UN solo trabajo.** La
    // carga del motor local ya admite varios pares; repartirla en una llamada por origen
    // dejaba tantos trabajos como ficheros, y un `cp` es una cosa.
    bool todosAqui = copias.size() > 1;
    for (const Copia& c : copias) {
        todosAqui = todosAqui && B::toLowerAscii(c.maquina) == B::toLowerAscii(destino.connection);
    }
    if (todosAqui) {
        std::vector<std::pair<std::string, std::string>> pares;
        for (const Copia& c : copias) {
            pares.emplace_back(c.ruta, c.rutaFinal);
        }
        const std::string carga = SY::copyLocalPayload(pares, /*borra=*/false, simula);
        if (carga.empty()) {
            std::fputs(TC("t_cp_rutas_mal",
                          "las rutas de origen y destino tienen que ser absolutas\n"), stderr);
            return false;
        }
        ZfsmUrl maquina;
        maquina.kind = ZfsmKind::Connection;
        maquina.connection = destino.connection;
        const std::vector<std::string> argv = PET::copyLocal(carga);
        if (simula || esperar) {
            std::string out;
            if (!agente(e, maquina, argv, out, 0)) {   // sin plazo: mueve datos de verdad
                return false;
            }
            std::fprintf(stdout, "%s", out.c_str());
            return true;
        }
        return enviaComoTrabajo(e, maquina, argv);
    }

    // Entre máquinas, cada origen es su propio envío de agente a agente.
    std::size_t mal = 0;
    for (const Copia& c : copias) {
        if (!copiaArbol(e, c.maquina, c.ruta, destino.connection, rutaDestino, c.nombre, simula,
                        esperar)) {
            ++mal;
            if (copias.size() > 1) {
                std::fprintf(stderr, TC("t_patron_fallo_uno", "  falló: %s\n"), c.ruta.c_str());
            }
        }
    }
    if (copias.size() > 1) {
        std::fprintf(stderr, TC("t_patron_resumen", "%s: %zu bien, %zu fallaron\n"), "cp",
                     copias.size() - mal, mal);
    }
    if (mal > 0) {
        e.ultimoRc = 1;
    }
    return mal == 0;
}

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
    // **`#content` restringe el subárbol, y `{a,b}` nombra varios.**
    //
    // `rsync` trabaja sobre FICHEROS, no sobre datasets: el dataset solo dice dónde empieza
    // el árbol. Por eso la sección que admite es `content` —la que nombra ese árbol— y lo
    // que va detrás acota la parte que se sincroniza:
    //
    //     zfsm://c/pool/ds#content/sub          solo «sub»
    //     zfsm://c/pool/ds#content/{a,b,dir}    esos tres, al mismo destino
    //
    // Las otras secciones no nombran ficheros y se rechazan en vez de ignorarse, que es lo
    // que se hacía: una URL con `#properties` sincronizaba el dataset entero sin decir nada.
    //
    // Y `#path`: una ruta suelta de la máquina, en cualquiera de los dos extremos.
    const auto seccionValida = [](const ZfsmUrl& u, const char* cual) {
        if (u.section.empty() || esSeccionDeFicheros(u)) {
            return true;
        }
        std::fprintf(stderr,
                     TC("t_rsync_seccion",
                        "rsync trabaja sobre ficheros: «#%s» no nombra ninguno (%s)\n"),
                     u.section.c_str(), cual);
        return false;
    };
    if (!seccionValida(origen, TC("t_rsync_origen", "origen"))) {
        return false;
    }
    // Un dataset o una ruta suelta; no una máquina ni una instantánea a secas.
    const auto extremoValido = [](const ZfsmUrl& u) {
        if (esRutaSuelta(u) || (nodoDe(u) == Nodo::Dataset)) {
            return true;
        }
        std::fprintf(stderr, TC("t_rsync_extremo",
                                "rsync necesita un dataset o una ruta «#path/…» (ahora: %s)\n"),
                     textoDe(u).c_str());
        return false;
    };
    if (!extremoValido(origen)) {
        return false;
    }
    ZfsmUrl destino;
    std::string err;
    if (!resuelve(e, pet.uno("destino"), destino, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return false;
    }
    if (!seccionValida(destino, TC("t_rsync_destino", "destino"))) {
        return false;
    }
    if (!extremoValido(destino)) {
        return false;
    }
    const bool mismaMaquina =
        B::toLowerAscii(origen.connection) == B::toLowerAscii(destino.connection);
    if (mismaMaquina && esRutaSuelta(origen) == esRutaSuelta(destino)
        && origen.zfsName() == destino.zfsName() && origen.detail == destino.detail) {
        std::fputs(TC("t_rsync_mismo", "el origen y el destino son el mismo dataset\n"), stderr);
        return false;
    }
    // La BASE de cada extremo y la ruta relativa a ella. En un dataset, su punto de montaje
    // y lo que va tras `#content`; en una ruta suelta, la raíz de la máquina («/», o la
    // unidad en Windows) y el resto de la ruta. Así las llaves y la comprobación de «..»
    // valen igual para las dos formas.
    const auto baseYRelativa = [&e](const ZfsmUrl& u, std::string& base, std::string& rel,
                                    bool paraEscribir) {
        if (!esRutaSuelta(u)) {
            rel = B::join(u.detail, "/");
            return montajeDe(e, u, base, paraEscribir);
        }
        std::vector<std::string> tramos = u.detail;
        base = "/";
        if (!tramos.empty() && CS::isDriveSegment(tramos.front())) {
            base = tramos.front() + "/";
            tramos.erase(tramos.begin());
        }
        rel = B::join(tramos, "/");
        return true;
    };
    std::string baseOrigen;
    std::string baseDestino;
    std::string relOrigen;
    std::string relDestinoBase;
    if (!baseYRelativa(origen, baseOrigen, relOrigen, /*paraEscribir=*/false)
        || !baseYRelativa(destino, baseDestino, relDestinoBase, /*paraEscribir=*/true)) {
        return false;
    }
    // Una ruta suelta de destino tiene que EXISTIR y ser un directorio, como en `cp`: no se
    // crea un árbol en cualquier sitio de la máquina por una errata.
    if (esRutaSuelta(destino)) {
        Ubicacion ub;
        if (!ubicacionDe(e, destino, ub)) {
            return false;
        }
        if (!ub.existe || !ub.esDirectorio) {
            std::fprintf(stderr, TC("t_no_es_directorio", "no es un directorio: %s\n"),
                         ub.ruta.c_str());
            e.ultimoRc = 1;
            return false;
        }
    }
    const bool borra = pet.tiene("--delete");
    const bool simula = pet.tiene("--check");

    // Los subárboles del origen. Con `#content/{a,b}` son varios; sin sección, uno solo que
    // es el árbol entero.
    // `detail` viene ya troceado por «/»; se rejunta para volver a tener la ruta relativa
    // tal y como se escribió, que es sobre la que actúan las llaves.
    const auto rutaDe = [&](const ZfsmUrl& u) {
        return &u == &origen ? relOrigen : relDestinoBase;
    };
    // Si un patrón ya las ha expandido (`#content/*.log`), esas; todas a UN solo rsync,
    // como hace la shell, para que `--delete` las considere juntas.
    std::vector<std::string> relativas =
        !e.rsyncRelativas.empty() ? e.rsyncRelativas : AV::contentPaths(relOrigen);
    // Las que vienen de un patrón sobre `#path` llevan la ruta entera desde la raíz; en
    // Windows, con la unidad delante, que es la base y no parte de la ruta relativa.
    if (esRutaSuelta(origen) && !e.rsyncRelativas.empty()) {
        for (std::string& rel : relativas) {
            if (rel.size() >= 2 && CS::isDriveSegment(rel.substr(0, 2))) {
                baseOrigen = rel.substr(0, 2) + "/";
                rel = rel.size() > 3 ? rel.substr(3) : std::string();
            }
        }
    }
    if (relativas.empty()) {
        std::fprintf(stderr,
                     TC("t_rsync_llaves_mal",
                        "«%s» no se entiende: las llaves se escriben «{a,b,c}», sin anidar y "
                        "sin piezas vacías\n"),
                     relOrigen.c_str());
        return false;
    }
    // Sincronizar la raíz entera de una máquina no es algo que nadie quiera por una errata.
    if (esRutaSuelta(origen) && relativas.size() == 1 && relativas.front().empty()) {
        std::fputs(TC("t_rsync_raiz", "rsync: el origen no puede ser la raíz de la máquina\n"),
                   stderr);
        return false;
    }
    const auto dentroDelArbol = [](const std::string& rel, const char* cual) {
        if (AV::isValidContentPath(rel)) {
            return true;
        }
        std::fprintf(stderr,
                     TC("t_rsync_ruta_fuera",
                        "«%s» sale del árbol del dataset (%s): las rutas de «#content» son "
                        "relativas y sin «..»\n"),
                     rel.c_str(), cual);
        return false;
    };
    for (const std::string& rel : relativas) {
        if (!dentroDelArbol(rel, TC("t_rsync_origen", "origen"))) {
            return false;
        }
    }
    if (!dentroDelArbol(rutaDe(destino), TC("t_rsync_destino", "destino"))) {
        return false;
    }

    // **`--delete` con varios orígenes está roto de partida**, y por eso se rechaza.
    //
    // La carga son PARES (origen, destino) y el daemon lanza un rsync por par: con `{a,b}`
    // la segunda pasada borraría en el destino lo que acaba de dejar la primera. Aceptarlo
    // sería prometer una sincronización y entregar la última carpeta a secas.
    // Con patrón cada una aterriza en SU subdirectorio, y ahí `--delete` es lo de rsync:
    // no hay dos pasadas sobre el mismo sitio.
    if (borra && relativas.size() > 1 && !e.rsyncConNombre) {
        std::fputs(TC("t_rsync_borrar_varios",
                      "«--delete» no se puede usar con varios orígenes: cada uno borraría lo "
                      "que dejó el anterior\n"),
                   stderr);
        return false;
    }

    const auto unir = [](const std::string& base, const std::string& rel) {
        if (rel.empty()) {
            return base;
        }
        return base + (base.empty() || base.back() == '/' ? "" : "/") + rel;
    };
    const std::string rutaDestino = unir(baseDestino, rutaDe(destino));
    std::vector<std::pair<std::string, std::string>> pares;
    for (const std::string& rel : relativas) {
        const std::size_t barra = rel.rfind('/');
        pares.emplace_back(unir(baseOrigen, rel),
                           e.rsyncConNombre
                               ? unir(rutaDestino, barra == std::string::npos ? rel : rel.substr(barra + 1))
                               : rutaDestino);
    }
    // Confirmación solo si va a BORRAR: sin `--delete` esto añade y actualiza, y no hay
    // nada que perder. Con `--delete` el destino acaba idéntico al origen, borrado incluido.
    if (borra && !simula
        && !confirma(e, B::format(T("t_conf_rsync",
                                    "%1 va a quedar IDÉNTICO a %2, borrando lo que sobre. ¿Continuar?"),
                                  {rutaDestino, pares.front().first}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    // La carga la arma la capa base. Estaba escrita aquí, otra vez en la ventana principal
    // y una tercera en el servidor web: tres copias del mismo orden de campos, que es un
    // contrato del daemon y no de ninguno de los tres.
    // ── Entre máquinas: el árbol entre daemons ───────────────────────────────────
    //
    // `--mutate-rsync-local` es local por definición, así que aquí no sirve. El transporte
    // es el mismo que usa «Desde Dir»: el destino escucha y el origen le manda el árbol
    // directamente, sin pasar por esta máquina.
    //
    // **Entre máquinas no es rsync**, aunque la orden se llame así: es la copia propia del
    // agente. Hace delta igual —compara tamaño y fecha—, y admite borrado y pasada en seco,
    // que es lo que esta orden promete.
    if (!mismaMaquina) {
        std::string errSrc;
        std::string errDst;
        const auto* pSrc = perfilDe(e, origen, errSrc);
        const auto* pDst = perfilDe(e, destino, errDst);
        if (!pSrc || !pDst) {
            std::fprintf(stderr, "%s\n", (pSrc ? errDst : errSrc).c_str());
            return false;
        }
        const B::ConnectionProfile src = conSudo(e, *pSrc);
        const B::ConnectionProfile dst = conSudo(e, *pDst);
        const auto llama = [&e](const B::ConnectionProfile& maquina,
                                const std::vector<std::string>& args, int timeoutMs,
                                std::string& salida, std::string& err, int& rc) {
            return ejecutarAgente(*e.ses, maquina, args, salida, err, rc, nullptr, timeoutMs);
        };
        bool todoBien = true;
        for (const auto& par : pares) {
            // Con patrón, cada uno aterriza con su nombre: un directorio en su
            // subdirectorio, y un fichero dentro del destino —el agente de origen lo manda
            // como un árbol de una entrada—. Sin patrón, el contenido va al destino tal cual.
            const bool fichero = e.rsyncConNombre && esFicheroEn(e, origen, par.first);
            std::string relDestino = rutaDe(destino);
            if (e.rsyncConNombre && !fichero) {
                relDestino = unir(relDestino, hojaDe(par.first));
            }
            // El directorio de destino tiene que EXISTIR: el receptor del árbol lo exige.
            // En un dataset, `--mutate-advanced-fromdir-prepare` lo monta, lo resuelve y lo
            // crea. En una ruta suelta ya se comprobó que existe; solo se crea, con
            // `--mutate-prepare-dir`, el subdirectorio con nombre de un patrón.
            std::string prepOut;
            std::string prepErr;
            int prepRc = -1;
            std::string dirPreparado;
            const std::string dirDestino = unir(baseDestino, rutaDe(destino));
            if (esRutaSuelta(destino) && relDestino == rutaDe(destino)) {
                dirPreparado = dirDestino;
            } else {
                const std::vector<std::string> prep =
                    esRutaSuelta(destino) ? AV::argvPrepareDir(dirDestino, hojaDe(par.first))
                                          : AV::argvFromDirPrepare(destino.zfsName(), relDestino);
                if (prep.empty()
                    || !ejecutarAgente(*e.ses, dst, prep, prepOut, prepErr, prepRc, nullptr, 60000)
                    || prepRc != 0) {
                    std::fputs(TC("t_rsync_destino_no", "no se pudo preparar el destino\n"), stderr);
                    return false;
                }
                dirPreparado = B::trim(AV::preparedPath(prepOut));
            }
            std::string salidaEnvio;
            const auto hecho = TR::launchTreeJob(
                e.ses->transporte, llama, src, dst, par.first, dirPreparado,
                /*mismaConexion=*/false, e.ses->verboso, /*comoTrabajo=*/false,
                borra && !fichero, simula,
                &salidaEnvio);
            if (hecho.failure != TR::JobFailure::None_) {
                std::fprintf(stderr, "%s: %s\n", TR::labelOf(hecho.failure).c_str(),
                             hecho.detail.c_str());
                todoBien = false;
                break;
            }
            std::fputs(salidaEnvio.c_str(), stdout);
            avisaSinMetadatos(salidaEnvio);
        }
        if (!todoBien) {
            e.ultimoRc = 1;
        }
        return todoBien;
    }

    const std::string carga = SY::rsyncPayload(pares, borra, simula, std::string(), std::string());
    if (carga.empty()) {
        std::fputs(TC("t_rsync_rutas_mal",
                      "las rutas de origen y destino tienen que ser absolutas\n"),
                   stderr);
        return false;
    }
    const std::vector<std::string> argv = PET::rsyncCopy(carga);
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

// El detalle de un pool, un dataset, un volumen o un snapshot, en CAMPO/VALOR como el de la
// conexión. Un pool que no está importado pero aparece en la sonda se describe con lo que
// dice `zpool import`, dispositivos incluidos.
bool infoDeObjeto(Estado& e, const ZfsmUrl& destino) {
    const std::string objeto = destino.zfsName();
    Tabla t;
    t.cabecerasTexto = {T("t_cab_campo", "CAMPO"), T("t_cab_valor", "VALOR")};
    t.campos = {"field", "value"};
    t.filas.push_back({"url", textoDe(destino)});
    // Las propiedades que dicen QUÉ es y CÓMO está. Las demás siguen en `ls #properties`.
    const bool esSnapshot = !destino.snapshot.empty();
    const std::vector<std::string> claves =
        esSnapshot ? std::vector<std::string>{"type", "creation", "used", "referenced",
                                              "compressratio", "userrefs", "clones",
                                              "defer_destroy"}
                   : std::vector<std::string>{"type", "creation", "used", "available", "referenced",
                                              "usedbysnapshots", "compressratio", "compression",
                                              "dedup", "encryption", "keystatus", "mountpoint",
                                              "mounted", "canmount", "readonly", "quota",
                                              "reservation", "recordsize", "volsize", "origin"};
    std::string out;
    std::vector<L::Property> props;
    std::string errProps;
    std::string errZfs;
    const bool hayDataset =
        agente(e, destino, PET::specificProperties(claves, objeto), out, 20000, &errZfs)
        && L::properties(out, props, errProps) && !props.empty();
    // Un pool: además, lo que es del POOL (tamaño, salud, fragmentación…), de `zpool list`.
    const bool esPool = destino.dataset.empty() || destino.dataset == destino.pool;
    if (esPool && !esSnapshot) {
        std::string lista;
        B::json::Value raiz;
        if (agente(e, destino, PET::poolList(), lista) && B::json::parse(lista, raiz, nullptr)) {
            for (const auto& kv : raiz["pools"].toObject()) {
                if (kv.second["name"].toString(kv.first) != destino.pool) {
                    continue;
                }
                t.filas.push_back({"kind", "pool"});
                for (const auto& pr : kv.second["properties"].toObject()) {
                    t.filas.push_back({"pool." + pr.first, pr.second["value"].toString()});
                }
            }
        }
        // No importado: lo que dice la sonda.
        if (!hayDataset && t.filas.size() == 1) {
            e.ultimoRc = 0;
            H::ImportablePoolInfo imp;
            std::map<std::string, std::string> rutas;
            if (!poolImportable(e, subeHasta(destino, Objetivo::Conexion), destino.pool, imp, rutas)) {
                std::fprintf(stderr, TC("t_info_no_existe",
                                        "%s no es un pool importado ni importable\n"),
                             destino.pool.c_str());
                e.ultimoRc = 1;
                return false;
            }
            t.filas.push_back({"kind", "pool (importable)"});
            t.filas.push_back({"imported", "no"});
            t.filas.push_back({"guid", imp.guid});
            t.filas.push_back({"state", imp.state});
            if (!imp.reason.empty()) t.filas.push_back({"status", imp.reason});
            if (!imp.action.empty()) t.filas.push_back({"action", imp.action});
            for (const H::ImportableDevice& d : imp.devices) {
                if (!d.leaf) {
                    continue;
                }
                const auto ruta = rutas.find(d.name);
                t.filas.push_back({"device", d.name + "  -> "
                                                 + (ruta != rutas.end() && !ruta->second.empty()
                                                        ? ruta->second : std::string("?"))
                                                 + "  " + d.state
                                                 + (d.detail.empty() ? "" : "  " + d.detail)});
            }
            t.imprime(e.formato);
            return true;
        }
    }
    if (!hayDataset) {
        if (!errZfs.empty()) {
            std::fprintf(stderr, "%s\n", errZfs.c_str());
        }
        e.ultimoRc = 1;
        return false;
    }
    if (!esPool || esSnapshot) {
        t.filas.push_back({"kind", esSnapshot ? "snapshot" : "dataset"});
    }
    for (const L::Property& pr : props) {
        if (pr.value.empty() || pr.value == "-") {
            continue;   // lo que no aplica (volsize en un sistema de ficheros…) no se enseña
        }
        t.filas.push_back({pr.name, pr.value});
    }
    t.imprime(e.formato);
    return true;
}

// --- `labelclear`: la etiqueta de ZFS de un dispositivo que ya no es de ningún pool.
bool cmdLabelclear(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    ZfsmUrl conexion = subeHasta(pet.objetivo, Objetivo::Conexion);
    // `labelclear zfsm://fc16#path/dev/sda9`: la URL da la máquina y el dispositivo. Con
    // `--on` además, tienen que coincidir.
    std::string dispositivo;
    std::string maquina;
    const bool conOn = !pet.valor("on").empty() || !pet.valor("from").empty();
    if (!rutaDeRanura(e, pet.uno("dispositivo"), conOn ? conexion.connection : std::string(),
                      dispositivo, &maquina)) {
        return false;
    }
    if (!maquina.empty() && B::toLowerAscii(maquina) != B::toLowerAscii(conexion.connection)) {
        conexion = ZfsmUrl{};
        conexion.kind = ZfsmKind::Connection;
        conexion.connection = maquina;
    }
    if (dispositivo.empty() || (dispositivo.front() != '/'
                                && !(dispositivo.size() > 1 && dispositivo[1] == ':'))) {
        std::fputs(TC("t_labelclear_ruta", "labelclear necesita la ruta del dispositivo, como "
                      "«/dev/sda9»\n"), stderr);
        e.ultimoRc = 2;
        return false;
    }
    // Lo que se va a perder, con nombre: la confirmación no dice «¿seguro?» a secas.
    if (!confirma(e, B::format(T("t_conf_labelclear",
                                 "Se va a BORRAR la etiqueta de ZFS de %1 en %2. Si era de un "
                                 "pool, ese pool deja de poder importarse desde ese disco. "
                                 "¿Continuar?"),
                               {dispositivo, conexion.connection}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    std::string out;
    if (!agente(e, conexion, PET::labelClear(dispositivo, pet.tiene("-f")), out, 60000)) {
        return false;
    }
    e.importablesCache.clear();
    std::fprintf(stderr, TC("t_labelclear_hecho", "borrada la etiqueta de %s\n"),
                 dispositivo.c_str());
    return true;
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
    // Un fichero o un directorio: qué es y a qué dataset pertenece.
    if (esSeccionDeFicheros(destino) && (esRutaSuelta(destino) || !destino.detail.empty())) {
        return infoDeRuta(e, destino);
    }
    // **Un pool, un dataset o un snapshot: SU detalle, no el de la conexión.** Antes `info`
    // daba siempre la salud del daemon, pidiera uno lo que pidiera.
    if (nodoDe(destino) != Nodo::Conexion) {
        return infoDeObjeto(e, destino);
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
    // Una sola forma de presentarlo, la misma que `refresh`.
    //
    // Antes se escribían tres líneas propias en «clave<TAB>valor» y después se volcaba TAL
    // CUAL la respuesta del agente, que habla en «CLAVE=valor» y en mayúsculas. La misma
    // salida traía dos convenciones y una línea `CAPS=` de cuatrocientos caracteres por el
    // medio. Y con `--format tsv|json` lo volcado ni siquiera pasaba por el formateador,
    // así que no era analizable por un guion, que es justo para lo que están esos formatos.
    Tabla t;
    t.cabecerasTexto = {T("t_cab_campo", "CAMPO"), T("t_cab_valor", "VALOR")};
    t.campos = {"field", "value"};
    t.filas.push_back({"url", textoDe(destino)});
    t.filas.push_back({"connection", p->id});
    t.filas.push_back({"host", T::isLocalConnection(*p) ? "-" : p->host});
    for (const std::string& linea : B::split(out, "\n", true)) {
        const std::size_t igual = linea.find('=');
        if (igual == std::string::npos) {
            continue;
        }
        t.filas.push_back({B::toLowerAscii(B::trim(linea.substr(0, igual))),
                           B::trim(linea.substr(igual + 1))});
    }
    t.imprime(e.formato);
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
                || !ejecutarAgente(*e.ses, *p, PET::poolList(), texto, err, rc, nullptr, 8000)
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
                || !ejecutarAgente(*e.ses, *p, PET::datasetList(u.dataset), texto, err, rc,
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
        if (agente(e, donde, PET::datasetProperties(donde.zfsName()), out, 20000)
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

// Los nombres que hay dentro de `#content[/ruta]`, para el tabulador.
//
// Only NOMBRES: `ls -lA` trae permisos, dueño y fechas que aquí no pintan nada, y habría
// que volver a partirlos. Los directorios salen con la barra puesta, que es lo que uno va
// a escribir a continuación.
std::vector<std::string> nombresDeContenido(Estado& e, const ZfsmUrl& destino) {
    std::vector<std::string> out;
    std::string error;
    const auto* p = perfilVivoDe(e, destino, error);
    if (!p) {
        return out;
    }
    // Vale para `#content` y para `#path`: la ruta sale de rutaDeFicheros y los nombres, del
    // agente. Antes era `ls -A -p` (o `Get-ChildItem`) por SSH: una shell para pulsar el
    // tabulador, y dos sintaxis que mantener.
    std::string base;
    if (!rutaDeFicheros(e, destino, base, /*callado=*/true)) {
        return out;
    }
    std::string texto;
    std::string err;
    int rc = -1;
    if (!ejecutarAgente(*e.ses, *p, PET::pathContents(base), texto, err, rc, nullptr, 12000)
        || rc != 0) {
        return out;
    }
    B::json::Value raiz;
    if (!B::json::parse(texto, raiz, nullptr)) {
        return out;
    }
    for (const auto& v : raiz["entries"].toArray()) {
        const std::string n = v["name"].toString();
        if (!n.empty()) {
            // Los directorios, con la barra puesta: es lo que uno escribe a continuación.
            out.push_back(n + (v["type"].toString() == "d" ? "/" : ""));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> completaEn(Estado& e, const std::string& linea, std::size_t cursor,
                                    std::size_t& desde, bool& puedeSeguir) {
    // Por omisión, lo completado se cierra: una orden, una opción, un valor de propiedad.
    // Solo lo que admite «/» detrás dice lo contrario, y lo dice donde se sabe.
    puedeSeguir = false;
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
            for (const std::string& v : B::zfsprops::valuesOf(prop)) {
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

    // --- `import <pool>`: los pools que se pueden importar en la conexión en la que se está.
    //
    // Caía en el caso general, que ofrece los HIJOS del sitio actual —los pools ya
    // importados—: justo los que no se pueden importar. Visto en mmela con un pool recién
    // exportado que el tabulador no proponía.
    if (orden == "import" && palabras.size() == 1) {
        const ZfsmUrl conexion = subeHasta(e.actual, Objetivo::Conexion);
        if (conexion.connection.empty()) {
            return {};
        }
        const std::string clave = B::toLowerAscii(conexion.connection);
        const auto ahora = std::chrono::steady_clock::now();
        auto it = e.importablesCache.find(clave);
        if (it == e.importablesCache.end() || ahora - it->second.first > std::chrono::seconds(30)) {
            std::vector<std::string> nombres;
            std::string sonda;
            if (agente(e, conexion, PET::importableProbe(), sonda, 60000)) {
                std::set<std::string> vistos;
                for (const H::ImportablePoolInfo& imp : H::parseZpoolImportOutput(sonda)) {
                    if (!imp.pool.empty() && vistos.insert(imp.pool).second) {
                        nombres.push_back(imp.pool);
                    }
                }
            }
            it = e.importablesCache.insert_or_assign(clave, std::make_pair(ahora, nombres)).first;
        }
        std::vector<std::string> out;
        for (const std::string& n : it->second.second) {
            if (B::startsWith(n, parcial)) {
                out.push_back(n);
            }
        }
        return out;
    }

    // --- Las SECCIONES: `#content`, `#properties`, `#permissions`, y lo que cuelga.
    //
    // Van antes que la URL corriente porque el « # » no es un separador de URL: partiendo
    // por «/» y «@», `ls #con<tab>` preguntaba por los HIJOS del dataset —que se llaman
    // «datos», no «#content»— y no ofrecía nada. Lo mismo dentro: `#content/Doc<tab>` se
    // resolvía como si «Doc» fuera un dataset.
    const std::size_t alm = parcial.find('#');
    if (alm != std::string::npos) {
        const std::string prefijo = parcial.substr(0, alm);   // la URL de delante, si la hay
        const std::string resto = parcial.substr(alm);        // desde el « # »
        ZfsmUrl sitio;
        std::string err;
        if (!resuelve(e, prefijo.empty() ? std::string(".") : prefijo, sitio, err)) {
            return {};
        }
        const std::size_t barra = resto.find('/');
        if (barra == std::string::npos) {
            // Todavía se está escribiendo el nombre de la sección. Solo se ofrecen donde
            // significan algo: en una conexión o en la raíz no hay contenido ni propiedades.
            // En una máquina, `#path/`; en un dataset o una instantánea, las suyas. Las que
            // llevan algo detrás se ofrecen con la barra puesta, como un directorio:
            // `#permissions` no lleva nada.
            std::vector<std::string> out;
            std::vector<std::string> secciones;
            const Nodo n = nodoDe(sitio);
            if (n == Nodo::Conexion) {
                secciones = {std::string("#") + B::zfsmSection::kPath + "/"};
            } else if (n == Nodo::Dataset || n == Nodo::Snapshot) {
                secciones = {std::string("#") + B::zfsmSection::kContent + "/",
                             std::string("#") + B::zfsmSection::kProperties + "/",
                             std::string("#") + B::zfsmSection::kPermissions};
            }
            for (const std::string& s : secciones) {
                if (B::startsWith(s, resto)) {
                    out.push_back(prefijo + s);
                }
            }
            puedeSeguir = out.size() == 1 && out.front().back() == '/';
            return out;
        }
        const std::string seccion = B::toLowerAscii(resto.substr(1, barra - 1));
        const std::size_t ultima = resto.find_last_of('/');
        const std::string yaEscrito = resto.substr(0, ultima + 1);  // «#content/» o «#content/a/»
        const std::string cola = resto.substr(ultima + 1);
        std::vector<std::string> candidatos;
        if (seccion == B::zfsmSection::kProperties) {
            // `#properties/<nombre>`: una sola propiedad, y no hay más niveles.
            if (yaEscrito != std::string("#") + B::zfsmSection::kProperties + "/") {
                return {};
            }
            candidatos = propiedadesDe(e, sitio);
        } else if (seccion == B::zfsmSection::kContent || seccion == B::zfsmSection::kPath) {
            // Se resuelve el directorio ya escrito —sin la barra final, que `resuelve` no
            // espera— y se preguntan sus nombres.
            std::string dir = prefijo + yaEscrito;
            if (dir.size() > 1 && dir.back() == '/') {
                dir.erase(dir.size() - 1);
            }
            ZfsmUrl donde;
            if (!resuelve(e, dir, donde, err)) {
                return {};
            }
            candidatos = nombresDeContenido(e, donde);
            // «..» y «.» con su barra, como en la URL corriente (ver más abajo). Se suman a
            // los nombres: aquí uno puede empezar por punto, y `.bashrc` no debe taparse.
            if (cola == "." || cola == "..") {
                ZfsmUrl conPuntos;
                if (resuelve(e, prefijo + yaEscrito + cola, conPuntos, err)) {
                    candidatos.push_back(cola + "/");
                }
            }
        } else {
            return {};   // `#permissions` no tiene hijos, y una sección desconocida tampoco
        }
        std::vector<std::string> out;
        for (const std::string& c : candidatos) {
            if (B::startsWith(c, cola)) {
                out.push_back(prefijo + yaEscrito + c);
            }
        }
        // Dentro de `#content`, un DIRECTORIO admite seguir bajando y un fichero no. La
        // diferencia viene ya marcada con la barra final desde la máquina —`ls -p` en Unix,
        // `PSIsContainer` en Windows—, así que no hay que preguntarlo otra vez. En
        // `#properties` no hay más niveles: una propiedad se cierra.
        puedeSeguir = out.size() == 1 && !out.front().empty() && out.front().back() == '/';
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
    // «..» y «.» se completan con su barra, como en bash: `cd fc16/..<tab>` no ofrecía nada
    // porque ningún hijo se llama así, y con «../» sí, así que había que teclearla a mano.
    // Solo si de verdad resuelven. Tras una «@» la cola lleva la arroba delante y no entra.
    std::string conPuntos;
    if (cola == "." || cola == "..") {
        ZfsmUrl destinoPuntos;
        std::string errPuntos;
        if (resuelve(e, base + cola, destinoPuntos, errPuntos)) {
            conPuntos = base + cola + "/";
        }
    }
    // Dentro de `#path/…` o `#content/…`, un nombre suelto es un fichero de ese directorio,
    // no un dataset hijo: lo mismo que lee `resuelve`.
    if (esSeccionDeFicheros(sitio)) {
        std::vector<std::string> out;
        for (const std::string& n : nombresDeContenido(e, sitio)) {
            if (B::startsWith(n, cola)) {
                out.push_back(base + n);
            }
        }
        // Aquí un nombre SÍ puede empezar por punto (`.bashrc`): «../» se suma a ellos, no
        // los tapa.
        if (!conPuntos.empty()) {
            out.push_back(conPuntos);
        }
        puedeSeguir = out.size() == 1 && out.front().back() == '/';
        return out;
    }
    std::vector<std::string> out;
    for (const std::string& h : hijosDe(e, sitio)) {
        if (B::startsWith(h, cola)) {
            out.push_back(base + h);
        }
    }
    // Un dataset también puede empezar por punto (`tank/.oculto` es un nombre válido; solo
    // «.» y «..» no lo son): «../» se suma a ellos, igual que en los ficheros.
    if (!conPuntos.empty()) {
        out.push_back(conPuntos);
        puedeSeguir = out.size() == 1;
        return out;
    }
    // Lo escrito ya ES el único candidato —`cd local<tab>` en zfsm://—: no hay nada que
    // alargar y el tabulador no hacía nada. Se ofrece lo que puede seguir: «/» si hay hijos
    // —una conexión siempre—, «@» si tiene instantáneas, y «#» donde hay secciones, en un
    // dataset o una instantánea. Con uno solo se escribe; con varios se enseñan.
    if (out.size() == 1 && out.front() == parcial) {
        ZfsmUrl elegido;
        if (resuelve(e, parcial, elegido, err)) {
            std::vector<std::string> sigue;
            const Nodo n = nodoDe(elegido);
            bool hayHijos = n == Nodo::Conexion;
            bool hayInstantaneas = false;
            if (n == Nodo::Dataset) {
                for (const std::string& h : hijosDe(e, elegido)) {
                    if (!h.empty()) {
                        (h.front() == '@' ? hayInstantaneas : hayHijos) = true;
                    }
                }
            }
            if (hayHijos) {
                sigue.push_back(parcial + "/");
            }
            if (hayInstantaneas) {
                sigue.push_back(parcial + "@");
            }
            if (n == Nodo::Dataset || n == Nodo::Snapshot) {
                sigue.push_back(parcial + "#");
            }
            // Una máquina sigue con «/» (sus pools) o con `#path/` (sus ficheros).
            if (n == Nodo::Conexion) {
                sigue.push_back(parcial + "#" + B::zfsmSection::kPath + "/");
            }
            if (!sigue.empty()) {
                puedeSeguir = true;
                return sigue;
            }
        }
    }
    // Un pool, un dataset o una conexión admiten «/» y otro nombre detrás, así que no se
    // cierran: es lo que permite bajar con el tabulador sin borrar el espacio a cada paso.
    puedeSeguir = out.size() == 1;
    return out;
}

// =====================================================================================
// --- Patrones en el destino: `*`, `?`, `[..]` y `{a,b}`.
//
// La regla de cada orden está en `reglasDePatron`. La idea general:
//
//   - En `ls`, un patrón en el ÚLTIMO tramo FILTRA las filas de lo que se lista:
//     `ls tank/user/M*` lista tank/user y deja las filas que casan con «M*», como
//     `ls -d M*` y no como la shell, que listaría el contenido de cada uno. En un tramo
//     intermedio multiplica los destinos: `ls tank/*/backups`.
//   - En las lecturas, cada destino que casa se atiende y sus tablas se juntan en una.
//   - En las que MODIFICAN, el patrón solo puede ir en el último tramo —nunca abarcar
//     varias máquinas o pools de golpe con un `destroy`—, la lista exacta se enseña SIEMPRE
//     y se pide confirmación salvo `-y`.
//   - Si nada coincide es un ERROR. Nunca se pasa el patrón tal cual a la orden: un
//     `destroy` que buscara un dataset llamado «auto-*» es la peor forma de enterarse.
//
// La expansión se hace sobre el texto TODAVÍA CODIFICADO, antes de `resuelve()`: allí cada
// tramo tiene que existir, y una vez descodificado `%2A` y `*` ya no se distinguen.

// Una URL absoluta partida en sus piezas SIN descodificar.
struct UrlCruda {
    std::string conexion;
    std::vector<std::string> ruta;  // el pool y los datasets
    bool hayInstantanea{false};
    std::string instantanea;
    bool haySeccion{false};
    std::string seccion;
    std::vector<std::string> detalle;

    bool esRaiz() const { return conexion.empty(); }

    std::string texto() const {
        if (conexion.empty()) {
            return std::string();  // la raíz no es una URL; quien la use pone «/»
        }
        std::string t = "zfsm://" + conexion;
        if (!ruta.empty()) {
            t += "/" + B::join(ruta, "/");
        }
        if (hayInstantanea) {
            t += "@" + instantanea;
        }
        if (haySeccion) {
            t += "#" + seccion;
            for (const std::string& d : detalle) {
                t += "/" + d;
            }
        }
        return t;
    }
};

UrlCruda parteCruda(const std::string& absoluta) {
    UrlCruda u;
    std::string resto = absoluta.substr(std::string("zfsm://").size());
    const auto sinVaciosFinales = [](std::vector<std::string> v) {
        while (!v.empty() && v.back().empty()) {
            v.pop_back();
        }
        return v;
    };
    const std::size_t almohadilla = resto.find('#');
    if (almohadilla != std::string::npos) {
        u.haySeccion = true;
        const std::vector<std::string> partes =
            sinVaciosFinales(B::split(resto.substr(almohadilla + 1), "/", false));
        resto = resto.substr(0, almohadilla);
        if (!partes.empty()) {
            u.seccion = partes.front();
            u.detalle.assign(partes.begin() + 1, partes.end());
        }
    }
    const std::size_t barra = resto.find('/');
    u.conexion = barra == std::string::npos ? resto : resto.substr(0, barra);
    std::string ruta = barra == std::string::npos ? std::string() : resto.substr(barra + 1);
    const std::size_t arroba = ruta.find('@');
    if (arroba != std::string::npos) {
        u.hayInstantanea = true;
        u.instantanea = ruta.substr(arroba + 1);
        ruta = ruta.substr(0, arroba);
    }
    if (!ruta.empty()) {
        u.ruta = sinVaciosFinales(B::split(ruta, "/", false));
    }
    return u;
}

// El destino que escribió el usuario, convertido en URL ABSOLUTA cruda: el sitio actual
// delante si es relativo, y los «.» y «..» aplicados sobre los tramos, sin descodificar.
bool absolutaCruda(const Estado& e, const std::string& textoEntrada, UrlCruda& out,
                   std::string& err) {
    const std::string t = B::trim(textoEntrada);
    if (B::startsWith(B::toLowerAscii(t), "zfsm://")) {
        out = parteCruda(t);
        return true;
    }
    if (!t.empty() && (t.front() == '@' || t.front() == '#')) {
        if (e.actual.connection.empty()) {
            err = T("t_falta_estar_ds", "hace falta estar en un dataset");
            return false;
        }
        ZfsmUrl limpia = e.actual;
        limpia.section.clear();
        limpia.detail.clear();
        if (t.front() == '@') {
            limpia.snapshot.clear();
        }
        out = parteCruda(B::formatZfsmUrl(limpia) + t);
        return true;
    }
    std::string cuerpo = t;
    std::string cola;
    const std::size_t almohadilla = cuerpo.find('#');
    if (almohadilla != std::string::npos) {
        cola = cuerpo.substr(almohadilla);
        cuerpo = cuerpo.substr(0, almohadilla);
    }
    std::vector<std::string> piezas;
    if (!cuerpo.empty() && cuerpo.front() == '/') {
        cuerpo = cuerpo.substr(1);
    } else if (!e.actual.connection.empty()) {
        ZfsmUrl base = e.actual;
        base.snapshot.clear();
        base.section.clear();
        base.detail.clear();
        const UrlCruda b = parteCruda(B::formatZfsmUrl(base));
        piezas.push_back(b.conexion);
        piezas.insert(piezas.end(), b.ruta.begin(), b.ruta.end());
    }
    for (const std::string& tr : B::split(cuerpo, "/", true)) {
        if (tr == ".") {
            continue;
        }
        if (tr == "..") {
            if (!piezas.empty()) {
                piezas.pop_back();
            }
            continue;
        }
        piezas.push_back(tr);
    }
    if (piezas.empty()) {
        out = UrlCruda{};
        return true;
    }
    out = parteCruda("zfsm://" + B::join(piezas, "/") + cola);
    return true;
}

// Qué es cada pieza de la URL, para saber qué hay que enumerar en ella y qué permite cada
// orden.
enum class Pieza { Conexion, Pool, Hijo, Instantanea, Seccion, Detalle };

// Lo que existe en cada nivel, para casarlo con un patrón. Una llamada al agente por
// nivel y padre, recordada mientras dura la expansión.
class Enumerador {
public:
    explicit Enumerador(Estado& e) : e_(e) {}

    std::vector<std::string> conexiones() {
        std::vector<std::string> v;
        for (const auto& p : e_.conns.perfiles) {
            v.push_back(p.id.empty() ? p.name : p.id);
        }
        return v;
    }

    // El nombre visible de una conexión, para casar también por él.
    std::string nombreDe(const std::string& id) {
        const auto* p = buscarConexion(e_.conns, id);
        return p ? p->name : std::string();
    }

    std::vector<std::string> pools(const UrlCruda& prefijo) {
        return enCache("pools|" + prefijo.texto(), [&](std::vector<std::string>& v) {
            ZfsmUrl u;
            std::string out;
            if (!resolver(prefijo, u) || !agente(e_, u, PET::poolList(), out)) {
                return false;
            }
            B::json::Value raiz;
            if (!B::trim(out).empty() && B::json::parse(out, raiz, nullptr)) {
                for (const auto& kv : raiz["pools"].toObject()) {
                    v.push_back(kv.second["name"].toString(kv.first));
                }
            }
            return true;
        });
    }

    std::vector<std::string> hijos(const UrlCruda& prefijo) { return deListado(prefijo, false); }
    std::vector<std::string> instantaneas(const UrlCruda& prefijo) { return deListado(prefijo, true); }

    std::vector<std::string> propiedades(const UrlCruda& prefijo) {
        return enCache("props|" + prefijo.texto(), [&](std::vector<std::string>& v) {
            ZfsmUrl u;
            std::string out;
            if (!resolver(prefijo, u) || !agente(e_, u, PET::datasetProperties(u.zfsName()), out)) {
                return false;
            }
            B::json::Value raiz;
            if (B::json::parse(out, raiz, nullptr)) {
                for (const auto& ds : raiz["datasets"].toObject()) {
                    for (const auto& pr : ds.second["properties"].toObject()) {
                        v.push_back(pr.first);
                    }
                }
            }
            return true;
        });
    }

    // `soloDirectorios`: cuando detrás quedan más tramos, un fichero no puede ser parte del
    // camino —`alfa/*/x.log`—, y pedir su contenido daba «Not a directory» por cada uno.
    std::vector<std::string> contenido(const UrlCruda& prefijo, bool soloDirectorios = false) {
        const std::vector<std::string> todos = contenidoTipado(prefijo);
        std::vector<std::string> v;
        for (const std::string& t : todos) {
            const bool esDir = !t.empty() && t.back() == '/';
            if (soloDirectorios && !esDir) {
                continue;
            }
            v.push_back(esDir ? t.substr(0, t.size() - 1) : t);
        }
        return v;
    }

    // Los nombres de un directorio, con «/» al final los que son directorios.
    std::vector<std::string> contenidoTipado(const UrlCruda& prefijo) {
        return enCache("cont|" + prefijo.texto(), [&](std::vector<std::string>& v) {
            ZfsmUrl u;
            if (!resolver(prefijo, u)) {
                return false;
            }
            // `#content` o `#path`: la ruta sale de rutaDeFicheros y los nombres, del mismo
            // verbo para las dos.
            std::string base;
            std::string out;
            if (!rutaDeFicheros(e_, u, base, /*callado=*/false)
                || !agente(e_, u, PET::pathContents(base), out)) {
                return false;
            }
            std::vector<L::DirectoryEntry> entradas;
            std::string errJson;
            if (L::directoryContents(out, entradas, errJson)) {
                for (const L::DirectoryEntry& ent : entradas) {
                    v.push_back(ent.directory ? ent.name + "/" : ent.name);
                }
            }
            return true;
        });
    }

    bool fallo() const { return fallo_; }

private:
    bool resolver(const UrlCruda& prefijo, ZfsmUrl& u) {
        std::string err;
        if (!resuelve(e_, prefijo.texto(), u, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
        return true;
    }

    template <typename F>
    std::vector<std::string> enCache(const std::string& clave, F&& llena) {
        const auto ya = cache_.find(clave);
        if (ya != cache_.end()) {
            return ya->second;
        }
        std::vector<std::string> v;
        if (!llena(v)) {
            fallo_ = true;
        }
        cache_[clave] = v;
        return v;
    }

    // Los hijos directos o las instantáneas de un dataset, del mismo listado recursivo que
    // usa `ls`: una sola llamada sirve para los dos.
    std::vector<std::string> deListado(const UrlCruda& prefijo, bool snaps) {
        const std::string clave = std::string(snaps ? "snaps|" : "hijos|") + prefijo.texto();
        return enCache(clave, [&](std::vector<std::string>& v) {
            ZfsmUrl u;
            std::string out;
            if (!resolver(prefijo, u) || !agente(e_, u, PET::datasetList(u.dataset), out)) {
                return false;
            }
            const std::string padre = u.dataset;
            for (const std::string& linea : B::split(out, "\n", true)) {
                const std::string nombre = B::trim(B::split(linea, "\t", false).front());
                if (snaps) {
                    if (B::startsWith(nombre, padre + "@")) {
                        v.push_back(nombre.substr(padre.size() + 1));
                    }
                } else if (B::startsWith(nombre, padre + "/")) {
                    const std::string resto = nombre.substr(padre.size() + 1);
                    if (resto.find('/') == std::string::npos && resto.find('@') == std::string::npos) {
                        v.push_back(resto);
                    }
                }
            }
            return true;
        });
    }

    Estado& e_;
    std::map<std::string, std::vector<std::string>> cache_;
    bool fallo_{false};
};

// Un destino ya concreto, con el filtro de filas que lleve (vacío si no lleva).
struct DestinoConcreto {
    std::string url;  // vacío = la raíz
    FiltroFilas filtro;
    std::string etiqueta() const {
        return url.empty() ? std::string("zfsm://") : url.substr(std::string("zfsm://").size());
    }
};

struct PiezaCruda {
    Pieza clase;
    std::string crudo;
};

std::vector<PiezaCruda> piezasDe(const UrlCruda& u) {
    std::vector<PiezaCruda> v;
    if (u.esRaiz()) {
        return v;
    }
    v.push_back({Pieza::Conexion, u.conexion});
    for (std::size_t i = 0; i < u.ruta.size(); ++i) {
        v.push_back({i == 0 ? Pieza::Pool : Pieza::Hijo, u.ruta[i]});
    }
    if (u.hayInstantanea) {
        v.push_back({Pieza::Instantanea, u.instantanea});
    }
    if (u.haySeccion) {
        v.push_back({Pieza::Seccion, u.seccion});
        for (const std::string& d : u.detalle) {
            v.push_back({Pieza::Detalle, d});
        }
    }
    return v;
}

// Expande el destino contra lo que existe. Con `filtraUltimo`, un patrón en la ÚLTIMA pieza
// no se expande: queda como filtro de filas del listado de su padre.
bool expandeDestinos(Estado& e, const UrlCruda& u, bool filtraUltimo,
                     std::vector<DestinoConcreto>& out, std::string& err) {
    out.clear();
    const std::vector<PiezaCruda> piezas = piezasDe(u);
    for (const PiezaCruda& pz : piezas) {
        if (!B::hasWildcard(pz.crudo)) {
            continue;
        }
        if (pz.clase == Pieza::Seccion) {
            err = T("t_patron_seccion", "el nombre de una sección no admite comodines");
            return false;
        }
        if (!B::checkRawPattern(pz.crudo)) {
            err = B::format(T("t_patron_mal", "patrón mal formado: «%1»"), {pz.crudo});
            return false;
        }
    }
    const std::size_t filtroEn = (filtraUltimo && !piezas.empty()
                                  && B::hasWildcard(piezas.back().crudo))
                                     ? piezas.size() - 1
                                     : std::string::npos;
    Enumerador en(e);
    std::set<std::string> vistos;

    const auto conPieza = [](UrlCruda base, const PiezaCruda& pz, const std::string& valor) {
        switch (pz.clase) {
            case Pieza::Conexion: base.conexion = valor; break;
            case Pieza::Pool:
            case Pieza::Hijo: base.ruta.push_back(valor); break;
            case Pieza::Instantanea: base.hayInstantanea = true; base.instantanea = valor; break;
            case Pieza::Seccion: base.haySeccion = true; base.seccion = valor; break;
            case Pieza::Detalle: base.detalle.push_back(valor); break;
        }
        return base;
    };

    const auto trasComodin = [&](std::size_t i) {
        for (std::size_t k = 0; k < i; ++k) {
            if (B::hasWildcard(piezas[k].crudo)) {
                return true;
            }
        }
        return false;
    };
    std::function<bool(std::size_t, const UrlCruda&)> expande =
        [&](std::size_t i, const UrlCruda& prefijo) -> bool {
        if (i == piezas.size() || i == filtroEn) {
            DestinoConcreto d;
            d.url = prefijo.texto();
            if (i == filtroEn) {
                d.filtro.patron = piezas[i].crudo;
                d.filtro.ocultos = piezas[i].clase == Pieza::Detalle
                                   && (B::toLowerAscii(prefijo.seccion) == "content"
                                       || B::toLowerAscii(prefijo.seccion) == "path");
                d.filtro.clase = piezas[i].clase == Pieza::Hijo ? FiltroFilas::Clase::Hijos
                                 : piezas[i].clase == Pieza::Instantanea
                                     ? FiltroFilas::Clase::Instantaneas
                                     : FiltroFilas::Clase::Todo;
            }
            if (vistos.insert(d.url + "\n" + d.filtro.patron).second) {
                out.push_back(d);
            }
            return true;
        }
        const PiezaCruda& pz = piezas[i];
        // Lo que existe en este nivel, colgando del prefijo.
        const auto candidatosDe = [&](std::vector<std::string>& candidatos) -> bool {
            switch (pz.clase) {
                case Pieza::Conexion: candidatos = en.conexiones(); break;
                case Pieza::Pool: candidatos = en.pools(prefijo); break;
                case Pieza::Hijo: candidatos = en.hijos(prefijo); break;
                case Pieza::Instantanea: candidatos = en.instantaneas(prefijo); break;
                case Pieza::Seccion: break;
                case Pieza::Detalle:
                    if (B::toLowerAscii(prefijo.seccion) == "properties") {
                        candidatos = en.propiedades(prefijo);
                    } else if (B::toLowerAscii(prefijo.seccion) == "content"
                               || B::toLowerAscii(prefijo.seccion) == "path") {
                        candidatos = en.contenido(prefijo, /*soloDirectorios=*/i + 1 < piezas.size());
                    } else {
                        err = B::format(T("t_patron_no_seccion", "la sección «%1» no admite comodines"),
                                        {prefijo.seccion});
                        return false;
                    }
                    break;
            }
            return true;
        };
        if (!B::hasWildcard(pz.crudo)) {
            // Un tramo LITERAL detrás de un comodín tiene que EXISTIR, como en la shell:
            // `alfa/*/x.log` da solo las rutas donde hay un x.log. Sin esto, «*» casaba con
            // todo lo del nivel —ficheros incluidos— y se le pegaba «x.log» a cada cosa:
            // salían «a.log/x.log» y «b.log/x.log», que no existen.
            if (trasComodin(i) && pz.clase != Pieza::Seccion) {
                std::vector<std::string> candidatos;
                if (!candidatosDe(candidatos)) {
                    return false;
                }
                std::string literal;
                if (!B::rawSegmentToPattern(pz.crudo, literal)) {
                    return true;
                }
                std::string sinEscapes;
                for (std::size_t k = 0; k < literal.size(); ++k) {
                    if (literal[k] == '\\' && k + 1 < literal.size()) {
                        ++k;
                    }
                    sinEscapes.push_back(literal[k]);
                }
                if (std::find(candidatos.begin(), candidatos.end(), sinEscapes) == candidatos.end()) {
                    return true;  // no existe aquí: esta rama no da nada
                }
            }
            return expande(i + 1, conPieza(prefijo, pz, pz.crudo));
        }
        std::vector<std::string> candidatos;
        if (!candidatosDe(candidatos)) {
            return false;
        }
        const bool deFicheros = pz.clase == Pieza::Detalle
                                && (B::toLowerAscii(prefijo.seccion) == "content"
                                    || B::toLowerAscii(prefijo.seccion) == "path");
        for (const std::string& c : candidatos) {
            if (deFicheros && !c.empty() && c[0] == '.' && pz.crudo[0] != '.') {
                continue;  // como la shell: «*» no saca los ocultos
            }
            const bool casa = B::matchesRawPattern(pz.crudo, c)
                              || (pz.clase == Pieza::Conexion
                                  && B::matchesRawPattern(pz.crudo, en.nombreDe(c)));
            if (casa && !expande(i + 1, conPieza(prefijo, pz, B::percentEncodeSegment(c)))) {
                return false;
            }
        }
        return true;
    };
    if (!expande(0, UrlCruda{})) {
        return false;
    }
    if (out.empty()) {
        // Si una enumeración falló —máquina que no contesta—, el error ya está dicho y es
        // ESE el motivo; «nada coincide» lo taparía.
        err = en.fallo() ? std::string() : std::string("-");
        return false;
    }
    return true;
}

// Una ruta de ESTA máquina con comodines —un origen o destino de `cp` que empieza por «/»—,
// expandida contra el disco, como la shell. Ordenada: el orden de un directorio es el que le
// apetece al sistema de ficheros, y un resultado que cambia de una vez a otra no se puede
// comprobar.
bool expandeRutaLocal(const std::string& ruta, std::vector<std::string>& out, std::string& err) {
    out.clear();
    std::vector<std::string> actuales = {"/"};
    const auto une = [](const std::string& a, const std::string& b) {
        return a == "/" ? "/" + b : a + "/" + b;
    };
    for (const std::string& tramo : B::split(ruta, "/", true)) {
        std::vector<std::string> siguientes;
        if (!B::hasWildcard(tramo)) {
            for (const std::string& a : actuales) {
                siguientes.push_back(une(a, tramo));
            }
        } else {
            std::vector<std::string> alternativas;
            if (!B::expandBraces(tramo, alternativas)) {
                err = B::format(T("t_patron_mal", "patrón mal formado: «%1»"), {tramo});
                return false;
            }
            for (const std::string& a : actuales) {
                std::vector<std::string> nombres;
                std::error_code ec;
                for (const auto& ent : std::filesystem::directory_iterator(a, ec)) {
                    const std::string nombre = ent.path().filename().string();
                    if (!nombre.empty() && nombre[0] == '.' && tramo[0] != '.') {
                        continue;  // como la shell: «*» no saca los ocultos
                    }
                    for (const std::string& alt : alternativas) {
                        if (B::globMatch(alt, nombre)) {
                            nombres.push_back(nombre);
                            break;
                        }
                    }
                }
                std::sort(nombres.begin(), nombres.end());
                for (const std::string& n : nombres) {
                    siguientes.push_back(une(a, n));
                }
            }
        }
        actuales = std::move(siguientes);
    }
    out = std::move(actuales);
    return true;
}

// Un patrón que tiene que casar con UNO: el destino de `cp`, `rsync`, `send` y `diff`.
// Como la shell, donde `cp a* dir*` solo hace lo que uno cree si `dir*` es un solo sitio.
// `rutaLocal`: el texto es una ruta de esta máquina (en `cp`, la que empieza por «/»).
bool expandeUnico(Estado& e, const std::string& texto, const std::string& orden, bool rutaLocal,
                  std::string& concreto) {
    std::vector<std::string> candidatos;
    std::string err;
    if (rutaLocal) {
        if (!expandeRutaLocal(texto, candidatos, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
    } else {
        UrlCruda u;
        std::vector<DestinoConcreto> ds;
        bool hecho = absolutaCruda(e, texto, u, err) && expandeDestinos(e, u, false, ds, err);
        // El destino de `send` es un dataset que muchas veces AÚN NO EXISTE: en
        // `send tank/cop*/nuevo` el patrón elige dónde, y «nuevo» es lo que se va a crear.
        // Exigir que exista —como se exige a un tramo literal tras un comodín— hacía
        // imposible justo el caso útil. Se expande la madre y se le pega el nombre.
        if (!hecho && err == "-" && orden == "send" && u.ruta.size() >= 2
            && !B::hasWildcard(u.ruta.back()) && !u.hayInstantanea && !u.haySeccion) {
            UrlCruda madre = u;
            const std::string nuevo = madre.ruta.back();
            madre.ruta.pop_back();
            err.clear();
            hecho = expandeDestinos(e, madre, false, ds, err);
            for (DestinoConcreto& d : ds) {
                d.url += "/" + nuevo;
            }
        }
        if (!hecho) {
            if (err == "-") {
                std::fprintf(stderr, TC("t_nada_coincide", "nada coincide con «%s»\n"), texto.c_str());
            } else if (!err.empty()) {
                std::fprintf(stderr, "%s\n", err.c_str());
            }
            return false;
        }
        for (const DestinoConcreto& d : ds) {
            candidatos.push_back(d.url);
        }
    }
    if (candidatos.empty()) {
        std::fprintf(stderr, TC("t_nada_coincide", "nada coincide con «%s»\n"), texto.c_str());
        return false;
    }
    if (candidatos.size() > 1) {
        std::fprintf(stderr,
                     TC("t_patron_varios_uno", "«%s» necesita UN destino y «%s» casa con %zu:\n"),
                     orden.c_str(), texto.c_str(), candidatos.size());
        for (std::size_t i = 0; i < candidatos.size() && i < 20; ++i) {
            std::fprintf(stderr, "  %s\n", candidatos[i].c_str());
        }
        return false;
    }
    concreto = candidatos.front();
    return true;
}

// Qué hace cada orden con un patrón en su destino. Las que no están, lo rechazan.
enum class Politica { Filtra, Uno, Varios, VariosModifica };
struct ReglaPatron {
    Politica politica;
    // En las que modifican: en qué pieza puede ir el patrón. Es la regla que impide un
    // `destroy` sobre varios pools o un `scrub` sobre varias máquinas por un despiste.
    std::set<Pieza> piezas;
};

const std::map<std::string, ReglaPatron>& reglasDePatron() {
    static const std::map<std::string, ReglaPatron> r = {
        {"ls", {Politica::Filtra, {}}},
        {"cd", {Politica::Uno, {}}},
        // Origen y destino emparejados: el origen, si casa uno, como el destino.
        {"send", {Politica::Uno, {}}},
        {"diff", {Politica::Uno, {}}},
        {"get", {Politica::Varios, {}}},
        {"info", {Politica::Varios, {}}},
        {"status", {Politica::Varios, {}}},
        {"history", {Politica::Varios, {}}},
        {"holds", {Politica::Varios, {}}},
        {"schedules", {Politica::Varios, {}}},
        {"log", {Politica::Varios, {}}},
        {"jobs", {Politica::Varios, {}}},
        {"peers", {Politica::Varios, {}}},
        {"devices", {Politica::Varios, {}}},
        {"refresh", {Politica::Varios, {}}},
        {"connect", {Politica::Varios, {}}},
        {"disconnect", {Politica::Varios, {}}},
        {"destroy", {Politica::VariosModifica, {Pieza::Hijo, Pieza::Instantanea}}},
        {"set", {Politica::VariosModifica, {Pieza::Pool, Pieza::Hijo, Pieza::Instantanea}}},
        {"mount", {Politica::VariosModifica, {Pieza::Pool, Pieza::Hijo}}},
        {"unmount", {Politica::VariosModifica, {Pieza::Pool, Pieza::Hijo}}},
        {"hold", {Politica::VariosModifica, {Pieza::Instantanea}}},
        {"release", {Politica::VariosModifica, {Pieza::Instantanea}}},
        {"scrub", {Politica::VariosModifica, {Pieza::Pool}}},
        {"install-daemon", {Politica::VariosModifica, {Pieza::Conexion}}},
    };
    return r;
}

// Lo que una orden escribe en la salida estándar mientras se ejecuta, capturado. Hace falta
// para repetir una orden sobre varios destinos: lo que imprime con `Tabla` se junta en una
// tabla, y lo que imprime a mano —`status`, `history`— se enseña en una sección con su
// destino delante. Sin capturar no se sabría cuál de las dos cosas hizo cada repetición.
class CapturaSalida {
public:
    CapturaSalida() {
        std::fflush(stdout);
        tmp_ = std::tmpfile();
        if (!tmp_) {
            return;
        }
        copia_ = DUP_(FILENO_(stdout));
        DUP2_(FILENO_(tmp_), FILENO_(stdout));
    }
    std::string termina() {
        if (!tmp_) {
            return {};
        }
        std::fflush(stdout);
        DUP2_(copia_, FILENO_(stdout));
        CLOSE_(copia_);
        std::string texto;
        std::rewind(tmp_);
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), tmp_)) > 0) {
            texto.append(buf, n);
        }
        std::fclose(tmp_);
        tmp_ = nullptr;
        return texto;
    }
    ~CapturaSalida() { termina(); }

private:
    std::FILE* tmp_{nullptr};
    int copia_{-1};
};

// El nombre con el que aterriza un origen de `cp`: el último tramo de su ruta. Sirve para
// negarse ANTES de copiar cuando dos orígenes acabarían con el mismo nombre en el destino:
// el segundo pisaría al primero.
std::string nombreDeLlegada(const std::string& origen) {
    if (!origen.empty() && origen.front() == '/') {
        const std::size_t barra = origen.find_last_of('/');
        return origen.substr(barra + 1);
    }
    ZfsmUrl u;
    std::string err;
    if (!B::parseZfsmUrl(origen, u, err)) {
        return origen;
    }
    if (!u.detail.empty()) {
        return u.detail.back();
    }
    const std::size_t barra = u.dataset.find_last_of('/');
    return barra == std::string::npos ? u.dataset : u.dataset.substr(barra + 1);
}

// La envoltura de cada orden: si su destino lleva un patrón, lo resuelve según la regla de
// la orden; si no, la orden se ejecuta como siempre, sin pasar por nada de esto.
// `install-daemon -a`: el despliegue a todas las conexiones conocidas, que hasta ahora se
// hacía a mano máquina a máquina después de cada versión.
//
// No es `install-daemon zfsm://*` —que reinstala en todas—: se le pregunta antes a cada una
// qué versión tiene, y solo se toca la que lo necesita. Una al día no se reinstala (cuesta
// un reinicio del servicio por nada), y una que va POR DELANTE de este cliente tampoco: sería
// rebajarla, y lo que hay que actualizar ahí es el cliente. Una desconectada se salta: está
// apartada a propósito. Las que no contestan se intentan, porque «no contesta» es justo lo
// que tiene una máquina sin daemon todavía.
bool instalaDaemonEnTodas(Estado& e, const LineaAnalizada& an,
                          const std::function<bool(Estado&, const LineaAnalizada&)>& f) {
    if (!an.objetivo.empty() || an.opciones.count("on") || an.opciones.count("from")) {
        std::fputs(TC("t_instala_todas_destino",
                      "-a es «todas las conexiones»: no lleva destino\n"),
                   stderr);
        e.ultimoRc = 2;
        return false;
    }
    const std::string esperada = B::agentversion::expected();
    enum class Accion { Instalar, Actualizar, AlDia, PorDelante, Desconectada };
    struct Fila {
        std::string id;
        std::string nombre;
        std::string version;
        Accion accion;
        PlanCli cli;
        const B::ConnectionProfile* perfil{nullptr};
    };
    std::vector<Fila> filas;
    for (const auto& p : e.conns.perfiles) {
        Fila fila;
        fila.perfil = &p;
        fila.id = p.id.empty() ? p.name : p.id;
        fila.nombre = p.name.empty() ? fila.id : p.name;
        if (e.conns.desconectada(fila.id)) {
            fila.accion = Accion::Desconectada;
            filas.push_back(fila);
            continue;
        }
        // Se pregunta de nuevo: lo recordado de la sesión puede ser de antes de otra
        // instalación, y aquí decide si se toca una máquina.
        olvidaEstadoDeConexion(e, p);
        ZfsmUrl u;
        u.kind = ZfsmKind::Connection;
        u.connection = fila.id;
        std::string salida;
        if (agente(e, u, {"--health"}, salida, 8000)) {
            fila.version = clavesDe(salida)["VERSION"];
        }
        if (fila.version.empty()) {
            fila.accion = Accion::Instalar;
        } else {
            switch (B::agentversion::verdictFor(fila.version, esperada)) {
                case B::agentversion::Verdict::UpToDate: fila.accion = Accion::AlDia; break;
                case B::agentversion::Verdict::Update: fila.accion = Accion::Actualizar; break;
                case B::agentversion::Verdict::Ahead: fila.accion = Accion::PorDelante; break;
            }
        }
        fila.cli = planDelCli(e, conSudo(e, p));
        filas.push_back(fila);
    }

    Tabla t;
    t.cabecerasTexto = {T("t_cab_conexion", "CONEXIÓN"), T("t_cab_daemon", "DAEMON"),
                        T("t_cab_accion", "ACCIÓN"), T("t_cab_cli", "CLI"),
                        T("t_cab_accion_cli", "ACCIÓN CLI")};
    t.campos = {"connection", "daemon", "action", "cli", "cli_action"};
    std::vector<const Fila*> aTocar;
    for (const Fila& fila : filas) {
        std::string accion;
        switch (fila.accion) {
            case Accion::Instalar: accion = T("t_instala_accion_instalar", "instalar"); break;
            case Accion::Actualizar: accion = T("t_instala_accion_actualizar", "actualizar"); break;
            case Accion::AlDia: accion = T("t_instala_accion_al_dia", "al día: no se toca"); break;
            case Accion::PorDelante:
                accion = T("t_instala_accion_delante", "va por delante del cliente: no se toca");
                break;
            case Accion::Desconectada:
                accion = T("t_instala_accion_desconectada", "desconectada: se salta");
                break;
        }
        const bool agenteToca = fila.accion == Accion::Instalar || fila.accion == Accion::Actualizar;
        const bool cliToca = fila.cli.accion == PlanCli::Accion::Actualizar;
        if (agenteToca || cliToca) {
            aTocar.push_back(&fila);
        }
        const bool desconectada = fila.accion == Accion::Desconectada;
        t.filas.push_back({fila.id, fila.version.empty() ? "-" : fila.version, accion,
                           fila.cli.sonda.found ? (fila.cli.sonda.fullVersion.empty()
                                                       ? std::string("?")
                                                       : fila.cli.sonda.fullVersion)
                                                : std::string("-"),
                           desconectada ? std::string("-") : textoDePlanCli(fila.cli)});
    }
    t.imprime(e.formato);
    if (aTocar.empty()) {
        std::fprintf(stderr, TC("t_instala_nada", "nada que hacer: todas al día (%s)\n"),
                     esperada.c_str());
        return true;
    }
    if (an.opciones.count("check")) {
        std::fputs(TC("t_patron_check", "--check: no se ha tocado nada\n"), stderr);
        return true;
    }
    if (!confirma(e, B::format(T("t_instala_todas_conf2",
                                 "Se van a poner al día %2 conexiones con %1: el daemon y, donde "
                                 "lo tengan, zfsmgr-cli. ¿Continuar?"),
                               {esperada, std::to_string(aTocar.size())}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        e.ultimoRc = 1;
        return false;
    }
    const bool asumiaSi = e.asumirSi;
    e.asumirSi = true;
    std::size_t bien = 0;
    std::vector<std::string> fallidas;
    for (const Fila* fila : aTocar) {
        const std::string& id = fila->id;
        std::fprintf(stderr, "\n== %s\n", id.c_str());
        // Cada una con su propio código: el fallo de una no se arrastra a la siguiente.
        e.ultimoRc = 0;
        bool ok = false;
        if (fila->accion == Accion::Instalar || fila->accion == Accion::Actualizar) {
            // El daemon, y con él el CLI: `install-daemon` hace las dos cosas.
            LineaAnalizada una = an;
            una.banderas.erase(std::remove(una.banderas.begin(), una.banderas.end(), "-a"),
                               una.banderas.end());
            una.opciones.erase("check");
            una.objetivo = "zfsm://" + B::percentEncodeSegment(id);
            ok = f(e, una);
        } else {
            // El daemon ya está al día: solo el CLI, sin reinstalar el servicio por nada.
            ok = actualizaCli(e, conSudo(e, *fila->perfil), fila->cli);
        }
        if (ok) {
            ++bien;
        } else {
            fallidas.push_back(id);
        }
    }
    e.asumirSi = asumiaSi;
    std::fprintf(stderr, TC("t_patron_resumen", "%s: %zu bien, %zu fallaron\n"), "install-daemon",
                 bien, fallidas.size());
    for (const std::string& id : fallidas) {
        std::fprintf(stderr, TC("t_patron_fallo_uno", "  falló: %s\n"), id.c_str());
    }
    e.ultimoRc = fallidas.empty() ? 0 : 1;
    return fallidas.empty();
}

bool ejecutaConPatrones(Estado& e, const std::string& orden, const LineaAnalizada& original,
                        const std::function<bool(Estado&, const LineaAnalizada&)>& f) {
    LineaAnalizada an = original;
    if (orden == "install-daemon" && an.tiene("-a")) {
        return instalaDaemonEnTodas(e, an, f);
    }

    // --- Ejecutar sobre varios destinos que MODIFICAN: la lista exacta SIEMPRE —también con
    // -y, para que quede en el registro qué se hizo—, `--check` para verla sin tocar nada,
    // una sola confirmación, y una por una sin volver a preguntar.
    const auto modificaVarios = [&](const std::vector<std::string>& destinos, const std::string& texto,
                                    const std::function<bool(const std::string&)>& unoSolo) {
        std::fprintf(stderr, TC("t_patron_lista", "%s: %zu destinos casan con «%s»:\n"),
                     orden.c_str(), destinos.size(), texto.c_str());
        for (const std::string& d : destinos) {
            std::fprintf(stderr, "  %s\n", d.c_str());
        }
        if (an.opciones.count("check")) {
            std::fputs(TC("t_patron_check", "--check: no se ha tocado nada\n"), stderr);
            return true;
        }
        if (!confirma(e, B::format(T("t_patron_confirma", "¿%1 sobre estos %2?"),
                                   {orden, std::to_string(destinos.size())}))) {
            e.ultimoRc = 1;
            return false;
        }
        const bool asumiaSi = e.asumirSi;
        e.asumirSi = true;
        std::size_t bien = 0;
        std::size_t mal = 0;
        for (const std::string& d : destinos) {
            if (unoSolo(d)) {
                ++bien;
            } else {
                ++mal;
                std::fprintf(stderr, TC("t_patron_fallo_uno", "  falló: %s\n"), d.c_str());
            }
        }
        e.asumirSi = asumiaSi;
        std::fprintf(stderr, TC("t_patron_resumen", "%s: %zu bien, %zu fallaron\n"), orden.c_str(),
                     bien, mal);
        if (mal > 0) {
            e.ultimoRc = 1;
        }
        return mal == 0;
    };
    const auto sinZfsm = [](const std::string& url) {
        return B::startsWith(url, "zfsm://") ? url.substr(7) : url;
    };

    // --- El DESTINO de `rsync`, `send` y `diff`: un patrón vale si casa con UNO, como en la
    // shell. Va antes que el origen porque es igual para todas.
    for (auto it = an.ranuras.begin(); it != an.ranuras.end(); ++it) {
        if (it->first == "destino" && B::hasWildcard(it->second)) {
            std::string concreto;
            if (!expandeUnico(e, it->second, orden, false, concreto)) {
                e.ultimoRc = 1;
                return false;
            }
            it->second = concreto;
        }
    }

    // --- `cp`: los ORÍGENES se expanden a varios; el DESTINO, solo si casa con uno. Igual que
    // `cp *.log /tmp/` en la shell: cada origen aterriza en el destino con su nombre.
    if (orden == "cp") {
        const std::vector<std::string> args = an.lista("texto");
        if (args.size() != 2 || (!B::hasWildcard(args[0]) && !B::hasWildcard(args[1]))) {
            return f(e, an);  // sin patrones, o mal formado: que lo diga `cp`
        }
        std::string destino = args[1];
        if (B::hasWildcard(destino)
            && !expandeUnico(e, destino, orden, !destino.empty() && destino.front() == '/', destino)) {
            e.ultimoRc = 1;
            return false;
        }
        std::vector<std::string> origenes;
        std::string err;
        if (!B::hasWildcard(args[0])) {
            origenes = {args[0]};
        } else if (args[0].front() == '/') {
            if (!expandeRutaLocal(args[0], origenes, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                e.ultimoRc = 1;
                return false;
            }
        } else {
            UrlCruda u;
            std::vector<DestinoConcreto> ds;
            if (!absolutaCruda(e, args[0], u, err) || !expandeDestinos(e, u, false, ds, err)) {
                if (err == "-") {
                    origenes.clear();
                } else {
                    if (!err.empty()) {
                        std::fprintf(stderr, "%s\n", err.c_str());
                    }
                    e.ultimoRc = 1;
                    return false;
                }
            }
            for (const DestinoConcreto& d : ds) {
                origenes.push_back(d.url);
            }
        }
        if (origenes.empty()) {
            std::fprintf(stderr, TC("t_nada_coincide", "nada coincide con «%s»\n"), args[0].c_str());
            e.ultimoRc = 1;
            return false;
        }
        if (origenes.size() > 1 && an.opciones.count("as")) {
            std::fputs(TC("t_cp_as_varios",
                          "--as pone UN nombre, y el origen casa con varios: no tiene sentido\n"),
                       stderr);
            e.ultimoRc = 1;
            return false;
        }
        // Dos orígenes con el mismo nombre de llegada: el segundo pisaría al primero.
        std::map<std::string, std::string> llegadas;
        for (const std::string& o : origenes) {
            const std::string nombre = nombreDeLlegada(o);
            const auto ya = llegadas.find(nombre);
            if (ya != llegadas.end()) {
                std::fprintf(stderr,
                             TC("t_cp_misma_llegada",
                                "«%s» y «%s» llegarían con el mismo nombre, «%s»: el segundo "
                                "pisaría al primero\n"),
                             sinZfsm(ya->second).c_str(), sinZfsm(o).c_str(), nombre.c_str());
                e.ultimoRc = 1;
                return false;
            }
            llegadas[nombre] = o;
        }
        // Como `cp` en la shell, no se pide confirmación: copiar a un sitio no destruye lo
        // que se copia. Pero la lista sí se enseña, y `--check` sigue valiendo.
        if (origenes.size() > 1) {
            std::fprintf(stderr, TC("t_patron_lista", "%s: %zu destinos casan con «%s»:\n"),
                         orden.c_str(), origenes.size(), args[0].c_str());
            for (const std::string& o : origenes) {
                std::fprintf(stderr, "  %s\n", sinZfsm(o).c_str());
            }
        }
        // Todos de una vez a `cp`, que los junta en UNA copia si están en la misma máquina.
        LineaAnalizada an2 = an;
        an2.ranuras.erase("texto");
        for (const std::string& o : origenes) {
            an2.ranuras.emplace("texto", o);
        }
        an2.ranuras.emplace("texto", destino);
        return f(e, an2);
    }

    const auto it = an.opciones.find("on");
    const auto itFrom = an.opciones.find("from");
    const std::string texto = it != an.opciones.end()       ? it->second
                              : itFrom != an.opciones.end() ? itFrom->second
                                                            : an.objetivo;
    if (texto.empty() || !B::hasWildcard(texto)) {
        return f(e, an);
    }

    // --- El ORIGEN de `rsync`.
    if (orden == "rsync") {
        UrlCruda u;
        std::string err;
        if (!absolutaCruda(e, texto, u, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            e.ultimoRc = 1;
            return false;
        }
        bool enRuta = false;
        bool enContenido = false;  // con `*`, `?` o `[` en #content; las llaves solas no
        for (const PiezaCruda& pz : piezasDe(u)) {
            if (pz.clase == Pieza::Detalle) {
                enContenido = enContenido || pz.crudo.find_first_of("*?[") != std::string::npos;
            } else if (B::hasWildcard(pz.crudo)) {
                enRuta = true;
            }
        }
        // `#content/{a,b,dir}` sin comodines es lo que `rsync` ya entendía por sí mismo: tal cual.
        if (!enRuta && !enContenido) {
            return f(e, an);
        }
        if (enRuta && enContenido) {
            std::fprintf(stderr, TC("t_rsync_patron_doble",
                                    "un patrón en el dataset de origen Y en su contenido a la vez no "
                                    "se admite: «%s»\n"),
                         texto.c_str());
            e.ultimoRc = 1;
            return false;
        }
        std::vector<DestinoConcreto> ds;
        if (!expandeDestinos(e, u, false, ds, err)) {
            if (err == "-") {
                std::fprintf(stderr, TC("t_nada_coincide", "nada coincide con «%s»\n"), texto.c_str());
            } else if (!err.empty()) {
                std::fprintf(stderr, "%s\n", err.c_str());
            }
            e.ultimoRc = 1;
            return false;
        }
        if (enContenido) {
            // Como `rsync -a src/*.log src/s* dst/` en la shell: TODAS las rutas en UNA orden,
            // y cada una aterriza en el destino con su nombre —un fichero como fichero, un
            // directorio como subdirectorio—, así que `--delete` solo toca lo de cada una.
            std::vector<std::string> relativas;
            for (const DestinoConcreto& d : ds) {
                ZfsmUrl c;
                if (B::parseZfsmUrl(d.url, c, err)) {
                    relativas.push_back(B::join(c.detail, "/"));
                }
            }
            UrlCruda base = u;
            base.detalle.clear();
            LineaAnalizada an2 = an;
            an2.opciones.erase("on");
            an2.opciones.erase("from");
            an2.objetivo = base.texto();
            e.rsyncRelativas = relativas;
            e.rsyncConNombre = true;
            const bool ok = f(e, an2);
            e.rsyncRelativas.clear();
            e.rsyncConNombre = false;
            return ok;
        }
        // Varios datasets de origen al mismo destino: varios rsync seguidos. Con `--delete`,
        // cada uno borraría lo que dejó el anterior, así que se niega.
        if (ds.size() > 1 && an.opciones.count("delete")) {
            std::fprintf(stderr,
                         TC("t_rsync_delete_varios",
                            "con --delete y %zu orígenes, cada rsync borraría lo que copió el "
                            "anterior en el mismo destino: no se hace\n"),
                         ds.size());
            e.ultimoRc = 1;
            return false;
        }
        if (ds.size() == 1) {
            LineaAnalizada an2 = an;
            an2.opciones.erase("on");
            an2.opciones.erase("from");
            an2.objetivo = ds.front().url;
            return f(e, an2);
        }
        std::vector<std::string> urls;
        for (const DestinoConcreto& d : ds) {
            urls.push_back(sinZfsm(d.url));
        }
        return modificaVarios(urls, texto, [&](const std::string& etiqueta) {
            LineaAnalizada an2 = an;
            an2.opciones.erase("on");
            an2.opciones.erase("from");
            an2.opciones.erase("check");
            an2.objetivo = "zfsm://" + etiqueta;
            return f(e, an2);
        });
    }

    const auto& reglas = reglasDePatron();
    const auto regla = reglas.find(orden);
    if (regla == reglas.end()) {
        std::fprintf(stderr,
                     TC("t_patron_no_admite",
                        "«%s» no admite patrones en el destino: actúa sobre uno solo (%s)\n"),
                     orden.c_str(), texto.c_str());
        e.ultimoRc = 1;
        return false;
    }
    const Politica politica = regla->second.politica;

    // Una lista `{…}` va DENTRO de un tramo: no puede contener `/`. Se dice con todas las
    // letras, porque lo que sale si no —«patrón mal formado: «{unib»»— no explica nada.
    {
        int nivel = 0;
        for (const char c : texto) {
            if (c == '{') {
                ++nivel;
            } else if (c == '}') {
                nivel = nivel > 0 ? nivel - 1 : 0;
            } else if (c == '/' && nivel > 0) {
                std::fprintf(stderr,
                             TC("t_patron_llave_barra",
                                "una lista {…} no puede contener «/»: cada tramo lleva la suya, "
                                "como en zfsm://{unib,freebsd}/* (%s)\n"),
                             texto.c_str());
                e.ultimoRc = 1;
                return false;
            }
        }
    }

    UrlCruda u;
    std::string err;
    if (!absolutaCruda(e, texto, u, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        e.ultimoRc = 1;
        return false;
    }
    const std::vector<PiezaCruda> piezas = piezasDe(u);

    // En las que modifican, el patrón SOLO en el último tramo, y en la clase de pieza que
    // la orden permite. Se mira ANTES de preguntar nada a ninguna máquina.
    if (politica == Politica::VariosModifica) {
        for (std::size_t i = 0; i + 1 < piezas.size(); ++i) {
            if (B::hasWildcard(piezas[i].crudo)) {
                std::fprintf(stderr,
                             TC("t_patron_solo_ultimo",
                                "«%s» solo admite el patrón en el último tramo (%s): no se "
                                "abarcan varias máquinas o pools de una vez\n"),
                             orden.c_str(), texto.c_str());
                e.ultimoRc = 1;
                return false;
            }
        }
        if (piezas.empty() || regla->second.piezas.count(piezas.back().clase) == 0) {
            std::fprintf(stderr,
                         TC("t_patron_pieza_no",
                            "«%s» no admite un patrón en esa parte del destino (%s)\n"),
                         orden.c_str(), texto.c_str());
            e.ultimoRc = 1;
            return false;
        }
    }

    std::vector<DestinoConcreto> destinos;
    if (!expandeDestinos(e, u, politica == Politica::Filtra, destinos, err)) {
        if (err == "-") {
            std::fprintf(stderr, TC("t_nada_coincide", "nada coincide con «%s»\n"), texto.c_str());
        } else if (!err.empty()) {
            std::fprintf(stderr, "%s\n", err.c_str());
        }
        e.ultimoRc = 1;
        return false;
    }

    bool vacio = false;   // la última sección se quedó sin filas tras el filtro
    const auto corre = [&](const DestinoConcreto& d, bool enVarios = false) {
        LineaAnalizada an2 = an;
        an2.opciones.erase("on");
        an2.opciones.erase("from");
        an2.opciones.erase("check");
        an2.objetivo = d.url.empty() ? std::string("/") : d.url;
        e.filtro = d.filtro;
        e.filtro.texto = texto;
        e.filtro.enVarios = enVarios;
        const bool ok = f(e, an2);
        vacio = e.filtro.quedoVacio;
        e.filtro = FiltroFilas{};
        return ok;
    };

    if (politica == Politica::Uno) {
        if (destinos.size() != 1) {
            std::fprintf(stderr, TC("t_patron_varios_uno", "«%s» necesita UN destino y «%s» casa con %zu:\n"),
                         orden.c_str(), texto.c_str(), destinos.size());
            for (std::size_t i = 0; i < destinos.size() && i < 20; ++i) {
                std::fprintf(stderr, "  %s\n", destinos[i].etiqueta().c_str());
            }
            e.ultimoRc = 1;
            return false;
        }
        return corre(destinos.front());
    }

    if (politica == Politica::VariosModifica) {
        std::vector<std::string> etiquetas;
        std::map<std::string, DestinoConcreto> porEtiqueta;
        for (const DestinoConcreto& d : destinos) {
            etiquetas.push_back(d.etiqueta());
            porEtiqueta[d.etiqueta()] = d;
        }
        return modificaVarios(etiquetas, texto,
                              [&](const std::string& etiqueta) { return corre(porEtiqueta[etiqueta]); });
    }

    // Filtra y Varios. Con un solo destino, tal cual.
    if (destinos.size() == 1) {
        return corre(destinos.front());
    }
    Acumulador acu;
    acumuladorActivo() = &acu;
    bool todoBien = true;
    std::size_t vacias = 0;
    for (const DestinoConcreto& d : destinos) {
        acu.destinoActual = d.etiqueta();
        CapturaSalida captura;
        bool ok = corre(d, /*enVarios=*/true);
        if (!ok && vacio) {
            ok = true;
            e.ultimoRc = 0;
            ++vacias;
        }
        const std::string suelto = captura.termina();
        if (!suelto.empty()) {
            // Lo que la orden imprimió a mano: en su sección, con el destino delante.
            acumuladorActivo() = nullptr;
            std::fprintf(stdout, "%s:\n%s%s\n", d.etiqueta().c_str(), suelto.c_str(),
                         suelto.back() == '\n' ? "" : "\n");
            acumuladorActivo() = &acu;
        }
        todoBien = todoBien && ok;
    }
    acumuladorActivo() = nullptr;
    if (vacias == destinos.size()) {
        std::fprintf(stderr, TC("t_nada_coincide", "nada coincide con «%s»\n"), texto.c_str());
        e.ultimoRc = 1;
        return false;
    }
    vuelcaAcumulador(acu, e.formato);
    if (!todoBien) {
        e.ultimoRc = 1;
    }
    return todoBien;
}

int ejecutarShell(Sesion& ses, Formato formato, const std::string& urlInicial, bool asumirSi) {
    Estado e;
    // Al salir —por donde sea: `exit`, fin de la entrada o una orden que falla en un guion—
    // se sueltan los montajes de solo lectura que queden.
    struct AlSalir {
        Estado& e;
        ~AlSalir() {
            if (!e.montajesLectura.empty()) {
                sueltaMontajesLectura(e, /*todos=*/true);
            }
        }
    } alSalir{e};
    e.ses = &ses;
    e.formato = formato;
    e.asumirSi = asumirSi;
    recarga(e);
    if (!e.conns.aviso.empty()) {
        std::fprintf(stderr, "%s\n", e.conns.aviso.c_str());
    }

    // Se empieza en la RAÍZ, no en «Local».
    //
    // Antes se arrancaba en `zfsm://Local` por parecer el sitio útil, pero da una primera
    // impresión falsa: hace creer que esa máquina es el punto de partida obligado, y en un
    // cliente cuyo trabajo es gobernar VARIAS máquinas eso es lo contrario de lo que se
    // quiere enseñar. Desde la raíz, `ls` lista las conexiones y se ve el mapa entero antes
    // de bajar a ninguna; `cd local` sigue estando a una orden de distancia.
    //
    // Una URL dada en la línea de órdenes SÍ se respeta: quien la escribe sabe dónde quiere
    // empezar. Y por eso el aviso de fallo solo tiene sentido en ese caso —arrancar en la
    // raíz porque no se pidió otra cosa no es un error del que avisar—.
    //
    // Se resuelve con `resuelve()` y no con `parseZfsmUrl()` a secas para que el nombre de
    // la conexión quede normalizado a su IDENTIFICADOR desde el primer momento: si no, el
    // indicador diría «zfsm://Local» y «zfsm://local/...» tras el primer `cd`, que parecen
    // dos sitios distintos.
    if (urlInicial.empty()) {
        e.actual = ZfsmUrl{};
    } else {
        std::string errInicio;
        if (!resuelve(e, urlInicial, e.actual, errInicio)
            || buscarConexion(e.conns, e.actual.connection) == nullptr) {
            std::fprintf(stderr, TC("t_no_se_pudo_3b2240", "no se pudo empezar en %s: %s\n"),
                         urlInicial.c_str(), errInicio.c_str());
            e.actual = ZfsmUrl{};
        }
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
        {"labelclear", cmdLabelclear},
        {"cp", cmdCp},
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
        // `clear` PREGUNTA. No borra datos, pero borra la cuenta de errores del pool, y se
        // teclea queriendo limpiar el terminal —dos veces en una misma sesión de pruebas—.
        // Perder eso sin haberlo pedido es perder justo lo que uno estaba mirando.
        {"clear", [](Estado& s, const LineaAnalizada& l) {
             return cmdPoolSimple(s, l, "clear", "clear",
                                  "Se van a poner a cero los errores contados del pool (para limpiar "
                                  "la PANTALLA la orden es «cls»)");
         }},
        // Las dos vacían la lista de importables del tabulador: lo que era importable ya no
        // lo es, y al revés.
        {"export", [](Estado& s, const LineaAnalizada& l) { s.importablesCache.clear(); return cmdPoolSimple(s, l, "export", "export", "Se va a exportar el pool y dejará de estar disponible"); }},
        {"import", [](Estado& s, const LineaAnalizada& l) { s.importablesCache.clear(); return cmdImport(s, l); }},
        {"status", cmdStatus},
        {"jobs", cmdJobs},
        {"job", cmdJob},
        {"install-daemon", cmdInstalarDaemon},
        {"send", cmdSend},
        {"history", cmdHistory},
        {"allow", [](Estado& s, const LineaAnalizada& l) { return cmdPermisos(s, l, true); }},
        {"unallow", [](Estado& s, const LineaAnalizada& l) { return cmdPermisos(s, l, false); }},
        {"rollback", cmdRollback},
        {"clone", cmdClone},
        {"get", cmdGet},
        {"set", cmdSet},
        {"load-key", [](Estado& s, const LineaAnalizada& l) { return cmdClaves(s, l, "load-key"); }},
        {"unload-key", [](Estado& s, const LineaAnalizada& l) { return cmdClaves(s, l, "unload-key"); }},
        {"change-key", [](Estado& s, const LineaAnalizada& l) { return cmdClaves(s, l, "change-key"); }},
        {"schedule", cmdSchedule},
        {"schedules", cmdSchedules},
        {"log", cmdLog},
        {"peers", cmdPeers},
        {"repair-mounts", cmdRepairMounts},
        {"authorize-key", cmdAuthorizeKey},
        {"export-trust", cmdExportTrust},
        {"breakdown", cmdBreakdown},
        {"assemble", cmdAssemble},
        {"todir", cmdToDir},
        {"fromdir", cmdFromDir},
    };

    LectorDeLinea lector;
    lector.setCompletador([&e](const std::string& l, std::size_t c, std::size_t& d, bool& sigue) {
        return completaEn(e, l, c, d, sigue);
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
            // En un guion, un error de sintaxis DETIENE la sesión, como una orden
            // desconocida. Antes seguía con la línea siguiente y, al acabarse la entrada,
            // salía con 0: `set atime=off tank` —con el destino detrás— decía «uso: …» y el
            // guion se daba por bueno sin haber cambiado nada.
            if (!interactivo) {
                return e.ultimoRc;
            }
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
            // `help load-key --on ..` enseña la ayuda y NO ejecuta nada. Se dice, porque lo
            // que se escribió detrás parece una orden que uno esperaba ver hecha.
            // `--on`/`--from` y las opciones no llegan como texto: se reconstruyen de lo
            // analizado para enseñar la orden tal y como se escribió.
            std::string resto;
            for (std::size_t i = 1; i < args.size(); ++i) {
                resto += " " + args[i];
            }
            if (!an.objetivo.empty()) {
                resto += " --on " + an.objetivo;
            }
            for (const auto& kv : an.opciones) {
                resto += " --" + kv.first + (kv.second.empty() ? "" : " " + kv.second);
            }
            for (const std::string& b : an.banderas) {
                resto += " " + b;
            }
            if (!args.empty() && !resto.empty()) {
                std::fprintf(stderr,
                             TC("t_help_no_ejecuta",
                                "help solo enseña la ayuda y no ejecuta nada: ¿quería «%s%s»?\n"),
                             args.front().c_str(), resto.c_str());
            }
            if (!args.empty()) {
                if (!imprimeAyudaDe(args.front(), anchoTerminal())) {
                    std::fprintf(stderr, TC("t_no_hay_nin_fea411", "no hay ninguna orden «%s»\n"), args.front().c_str());
                }
            } else {
                imprimeAyuda(anchoTerminal());
            }
            continue;
        }
        // Limpiar la pantalla. Con la secuencia ANSI y no llamando a `clear(1)`: no hay que
        // lanzar un proceso para esto, y en Windows ese binario no existe.
        //
        // Se borra también el HISTORIAL de desplazamiento —`3J`—, que es lo que uno espera
        // de limpiar: sin eso queda todo ahí arriba y basta con subir para verlo.
        if (orden == "cls") {
            if (hayTerminal()) {
                std::fputs("\033[H\033[2J\033[3J", stdout);
                std::fflush(stdout);
            }
            continue;
        }
        // Cambiar la clave maestra. La rotación la hace la capa base; aquí se piden las dos
        // claves sin que asomen por el argv y se recarga lo que la sesión tenía en memoria.
        if (orden == "master-password") {
            std::string vieja;
            std::string nueva;
            std::string err;
            const auto opcionDe = [&an](const char* clave) {
                const auto it = an.opciones.find(clave);
                return it == an.opciones.end() ? std::string() : it->second;
            };
            const std::string fdVieja = opcionDe("old-fd");
            const std::string fdNueva = opcionDe("new-fd");
            if (!fdVieja.empty()) {
                if (!leerSecretoDeDescriptor(std::atoi(fdVieja.c_str()), vieja, err)) {
                    std::fprintf(stderr, "%s\n", err.c_str());
                    e.ultimoRc = 2;
                    continue;
                }
            } else {
                vieja = e.ses->maestra;   // la de esta sesión: ya se comprobó al entrar
            }
            if (!fdNueva.empty()) {
                if (!leerSecretoDeDescriptor(std::atoi(fdNueva.c_str()), nueva, err)) {
                    std::fprintf(stderr, "%s\n", err.c_str());
                    e.ultimoRc = 2;
                    continue;
                }
            } else {
                if (!hayTerminal()) {
                    std::fputs(TC("t_mp_sin_terminal",
                                  "sin terminal: use --new-fd para dar la contraseña nueva\n"),
                               stderr);
                    e.ultimoRc = 2;
                    continue;
                }
                std::string otra;
                if (!preguntarSecretoPorTerminal(T("t_mp_p1", "Contraseña maestra nueva: "), nueva, err)
                    || !preguntarSecretoPorTerminal(T("t_mp_p2", "Repítala: "), otra, err)) {
                    std::fprintf(stderr, "%s\n", err.c_str());
                    e.ultimoRc = 2;
                    continue;
                }
                if (nueva != otra) {
                    std::fputs(TC("t_mp_no_coincide", "las dos no coinciden: no se ha cambiado nada\n"),
                               stderr);
                    e.ultimoRc = 2;
                    continue;
                }
            }
            if (nueva.empty()) {
                std::fputs(TC("t_mp_vacia", "la contraseña maestra no puede quedar vacía\n"), stderr);
                e.ultimoRc = 2;
                continue;
            }
            std::string copia;
            B::store::Warning aviso;
            if (!B::store::rotateMasterKey(e.ses->dirConfig, vieja, nueva, copia, aviso)) {
                std::fprintf(stderr, "%s\n", B::store::labelOf(aviso).c_str());
                e.ultimoRc = 1;
                continue;
            }
            // La sesión sigue abierta, así que su copia de la maestra tiene que moverse con
            // el fichero: si no, la siguiente escritura cifraría con la vieja.
            e.ses->maestra = nueva;
            recarga(e);
            std::fprintf(stdout,
                         TC("t_mp_ok", "contraseña maestra cambiada; copia de seguridad en «*%s»\n"),
                         copia.c_str());
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
            // Basta con las primeras letras de una orden mientras no haya dos que empiecen
            // igual. Cuando las hay, decir «desconocida» es mentir: la orden existe, lo que
            // pasa es que hay varias. Se enumeran, que es lo que hace falta para elegir.
            // Del CATÁLOGO, no del mapa de este bucle: hay órdenes que se atienden antes de
            // llegar a él —`cls`, `format`, `yes`, `exit`— y no estarían en la lista. Es
            // además la misma función que usa el TABULADOR, así que lo que se ofrece al
            // completar y lo que se enumera al abreviar no pueden discrepar.
            std::vector<std::string> candidatas = nombresQueEmpiezanPor(orden);
            if (candidatas.size() > 1) {
                std::sort(candidatas.begin(), candidatas.end());
                std::string lista;
                for (const std::string& c : candidatas) {
                    if (!lista.empty()) {
                        lista += ", ";
                    }
                    lista += c;
                }
                std::fprintf(stderr,
                             TC("t_orden_ambigua", "«%s» es ambigua: %s\n"),
                             orden.c_str(), lista.c_str());
                e.ultimoRc = 127;
                if (!interactivo) {
                    return e.ultimoRc;
                }
                continue;
            }
            std::fprintf(stderr, TC("t_orden_desc_b05fd4", "orden desconocida: %s (pruebe «help»)\n"), orden.c_str());
            e.ultimoRc = 127;
            // En un guion, una orden que no existe DETIENE la sesión, igual que una que
            // falla. Antes seguía adelante y su 127 lo pisaba la siguiente orden que
            // saliera bien, así que una errata terminaba con código 0: el guion se daba por
            // bueno habiendo hecho otra cosa. Es el peor final posible para una errata.
            if (!interactivo) {
                return e.ultimoRc;
            }
            continue;
        }
        const bool bien = ejecutaConPatrones(e, orden, an, it->second);
        // Los montajes de solo lectura que ya no contienen la posición se sueltan aquí,
        // tras cada orden: un `ls` de un dataset sin montar no deja nada montado detrás.
        if (!e.montajesLectura.empty()) {
            const int rcOrden = e.ultimoRc;
            sueltaMontajesLectura(e, /*todos=*/false);
            e.ultimoRc = rcOrden;
        }
        if (!bien) {
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
