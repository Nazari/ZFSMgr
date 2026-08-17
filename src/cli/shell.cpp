#include "shell.h"

#include "daemonpayload.h"
#include "helpers.h"
#include "json.h"
#include "process.h"
#include "secretinput.h"
#include "strutil.h"
#include "transportcmd.h"
#include "transporttunnel.h"
#include "transportrpc.h"
#include "zfsmurl.h"

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
            error = "no hay ninguna conexión llamada «" + out.connection + "»";
            return false;
        }
        out.connection = p->id.empty() ? p->name : p->id;
        return true;
    }

    if (texto == "-") {
        if (!e.hayAnterior) {
            error = "no hay sitio anterior";
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
            error = "hace falta estar en un dataset";
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
                err = "tramo mal codificado: " + tramoCrudo;
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
                        err = "no hay ninguna conexión llamada «" + nombre + "»";
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
                    err = "una instantánea no tiene hijos: " + nombre;
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

    // Relativa primero. Si el primer tramo no encaja donde estamos pero SÍ es el nombre de
    // una conexión, se toma como absoluta: es lo que uno escribe al saltar de una máquina a
    // otra, y sin esto habría que anteponer una barra que nadie recuerda.
    std::string errRelativa;
    if (aplica(e.actual, tramos, out, errRelativa)) {
        return true;
    }
    ZfsmUrl comoAbsoluta;
    std::string errAbsoluta;
    if (buscarConexion(e.conns, tramos.front()) != nullptr
        && aplica(ZfsmUrl{}, tramos, comoAbsoluta, errAbsoluta)) {
        out = comoAbsoluta;
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
        error = "hace falta estar en una conexión";
        return nullptr;
    }
    const auto* p = buscarConexion(e.conns, u.connection);
    if (!p) {
        error = "no hay ninguna conexión llamada «" + u.connection + "»";
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
        error = id + " está marcada como desconectada; use «connect " + id + "» para usarla";
        return nullptr;
    }
    return p;
}

// Una operación de `zfs` por el verbo genérico, que recibe el argv en JSON y solo admite
// una lista cerrada de operaciones. Declarada aquí porque la usan órdenes de más arriba.
bool zfsGenerico(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv);
bool zpoolGenerico(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv);
struct Opts;
bool cmdCrearPool(Estado& e, const Opts& o, const ZfsmUrl& destino);
bool enviaComoTrabajo(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv);
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
        std::fprintf(stderr, "no se pudo hablar con %s: %s\n",
                     (p->name.empty() ? p->id : p->name).c_str(), motivo.c_str());
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

// --- Órdenes.

struct Opts {
    std::vector<std::string> libres;          // lo que no es opción
    std::map<std::string, std::string> con;   // --clave valor
    std::vector<std::string> banderas;        // -r, -f, -y...
    // Opciones cortas CON valor que pueden repetirse: `-o ashift=12 -o autotrim=on`. Van
    // en una lista y no en un mapa porque el orden importa y la clave se repite.
    std::vector<std::pair<std::string, std::string>> repetidas;
    bool tiene(const std::string& b) const {
        for (const auto& x : banderas) {
            if (x == b) {
                return true;
            }
        }
        return false;
    }
    std::string valor(const std::string& k, const std::string& pordefecto = {}) const {
        const auto it = con.find(k);
        return it == con.end() ? pordefecto : it->second;
    }
};

// Trocea los argumentos. Las opciones con valor se declaran para no tener que adivinar si
// lo que viene detrás es su valor o un argumento suelto.
Opts trocea(const std::vector<std::string>& args, const std::vector<std::string>& conValor,
            const std::vector<std::string>& cortasConValor = {}) {
    Opts o;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        // Cortas con valor, que además pueden repetirse.
        if (a.size() == 2 && a[0] == '-' && i + 1 < args.size()) {
            const std::string letra = a.substr(1);
            bool esperaValor = false;
            for (const auto& c : cortasConValor) {
                if (c == letra) {
                    esperaValor = true;
                }
            }
            if (esperaValor) {
                o.repetidas.emplace_back(letra, args[++i]);
                continue;
            }
        }
        if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
            const std::string clave = a.substr(2);
            bool esperaValor = false;
            for (const auto& c : conValor) {
                if (c == clave) {
                    esperaValor = true;
                }
            }
            if (esperaValor && i + 1 < args.size()) {
                o.con[clave] = args[++i];
            } else {
                o.banderas.push_back(a);
            }
            continue;
        }
        if (a.size() >= 2 && a[0] == '-') {
            o.banderas.push_back(a);
            continue;
        }
        o.libres.push_back(a);
    }
    return o;
}

// El destino de una orden: `--on`/`--from` si se dan, y si no, el sitio actual.
//
// **Las dos son sinónimas a propósito.** «from» es la palabra natural en las órdenes que
// además tienen un destino —`todir`, `assemble`—, y «on» en las que actúan sobre un solo
// sitio. Obligar a recordar cuál lleva cada una sería una regla que no aporta nada.
bool destinoDe(const Estado& e, const Opts& o, ZfsmUrl& out) {
    std::string texto = o.valor("on");
    if (texto.empty()) {
        texto = o.valor("from");
    }
    std::string error;
    if (!resuelve(e, texto, out, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    return true;
}

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

bool confirma(const Estado& e, const std::string& que) {
    if (e.asumirSi) {
        return true;
    }
    if (!hayTerminal()) {
        std::fprintf(stderr,
                     "%s: hace falta confirmación y no hay terminal. Use -y si está seguro.\n",
                     que.c_str());
        return false;
    }
    std::string resp;
    std::string err;
    if (!preguntarPorTerminal(que + " [s/N]: ", resp, err)) {
        return false;
    }
    const std::string r = B::toLowerAscii(B::trim(resp));
    return r == "s" || r == "si" || r == "sí" || r == "y" || r == "yes";
}

// --- ls

// El listado de conexiones es EL MISMO que el de la orden suelta `connections list`. La
// tabla se construye en un solo sitio (session.cpp): tenerla duplicada hacía que la misma
// pregunta se contestara con columnas distintas según por dónde se preguntara.
void listaConexiones(const Estado& e) { tablaDeConexiones(e.conns).imprime(e.formato); }

// Los pools de una conexión, del JSON de `zpool list`.
bool listaPools(Estado& e, const ZfsmUrl& destino) {
    std::string out;
    if (!agente(e, destino, {"--dump-zpool-list"}, out)) {
        return false;
    }
    B::json::Value raiz;
    std::string err;
    if (!B::json::parse(out, raiz, &err)) {
        std::fprintf(stderr, "respuesta ilegible de zpool list: %s\n", err.c_str());
        return false;
    }
    Tabla t;
    t.nombreJson = "pools";
    t.cabecerasTexto = {"NOMBRE", "ESTADO", "TAMAÑO", "LIBRE", "USO", "SALUD"};
    t.campos = {"name", "state", "size", "free", "capacity", "health"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    const auto prop = [](const B::json::Value& p, const char* k) {
        return p["properties"][k]["value"].toString();
    };
    for (const auto& kv : raiz["pools"].toObject()) {
        const auto& p = kv.second;
        t.filas.push_back({p["name"].toString(kv.first), p["state"].toString(), prop(p, "size"),
                           prop(p, "free"), prop(p, "capacity"), prop(p, "health")});
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
    t.cabecerasTexto = {"NOMBRE", "TIPO", "USADO", "COMPR", "MONTADO", "PUNTO DE MONTAJE"};
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
        std::fprintf(stderr, "respuesta ilegible de zfs get: %s\n", err.c_str());
        return false;
    }
    Tabla t;
    t.nombreJson = "properties";
    t.cabecerasTexto = {"PROPIEDAD", "VALOR", "ORIGEN"};
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
        std::fprintf(stderr, "hace falta un dataset\n");
        return false;
    }
    std::string out;
    if (!agente(e, destino, {"--dump-zfs-allow", objetivo}, out)) {
        return false;
    }
    Tabla t;
    t.nombreJson = "permissions";
    t.cabecerasTexto = {"ALCANCE", "CLASE", "QUIÉN", "PERMISOS"};
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
bool cmdPermisos(Estado& e, const std::vector<std::string>& args, bool conceder) {
    const Opts o = trocea(args, {"on", "from", "user", "group", "set"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino)) {
        return false;
    }
    const std::string objetivo = destino.zfsName();
    if (objetivo.empty()) {
        std::fprintf(stderr, "hace falta un dataset (ahora: %s)\n", textoDe(destino).c_str());
        return false;
    }
    // Sin argumentos, `allow` LISTA. Es lo que hace `zfs allow` a secas, y quien viene de
    // ahí lo espera.
    if (conceder && o.libres.empty() && o.valor("user").empty() && o.valor("group").empty()
        && !o.tiene("--everyone")) {
        ZfsmUrl conSeccion = destino;
        conSeccion.section = B::zfsmSection::kPermissions;
        return listaPermisos(e, conSeccion);
    }

    std::vector<std::string> argv{conceder ? "allow" : "unallow"};
    if (o.tiene("-r")) {
        argv.push_back("-r");  // solo unallow: recursivo
    }
    if (o.tiene("--local")) {
        argv.push_back("-l");
    }
    if (o.tiene("--descend")) {
        argv.push_back("-d");
    }
    if (o.tiene("--create")) {
        argv.push_back("-c");
    }
    int aQuien = 0;
    if (!o.valor("user").empty()) {
        argv.push_back("-u");
        argv.push_back(o.valor("user"));
        ++aQuien;
    }
    if (!o.valor("group").empty()) {
        argv.push_back("-g");
        argv.push_back(o.valor("group"));
        ++aQuien;
    }
    if (o.tiene("--everyone")) {
        argv.push_back("-e");
        ++aQuien;
    }
    if (!o.valor("set").empty()) {
        argv.push_back("-s");
        argv.push_back(o.valor("set"));
        ++aQuien;
    }
    if (aQuien == 0) {
        std::fprintf(stderr,
                     "uso: %s --user <u> | --group <g> | --everyone | --set @<nombre>\n"
                     "         <permiso>[,<permiso>...] [--local] [--descend] [--create]%s\n"
                     "  Sin nada, «allow» lista los permisos delegados.\n",
                     conceder ? "allow" : "unallow", conceder ? "" : " [-r]");
        return false;
    }
    if (aQuien > 1) {
        // zfs acepta uno solo, y pasarle dos da un error suyo que no dice cuál sobra.
        std::fprintf(stderr, "elija UNO: --user, --group, --everyone o --set\n");
        return false;
    }
    // Los permisos. En `unallow` pueden omitirse: entonces se quitan TODOS los de ese
    // usuario, que es lo que hace zfs y conviene decir en la confirmación.
    if (!o.libres.empty()) {
        argv.push_back(B::join(o.libres, ","));
    } else if (conceder) {
        std::fprintf(stderr, "hace falta al menos un permiso\n");
        return false;
    }
    argv.push_back(objetivo);

    if (!conceder
        && !confirma(e, o.libres.empty()
                            ? "Se van a quitar TODOS los permisos delegados de ese destinatario en "
                                  + objetivo + ". ¿Continuar?"
                            : "Se van a quitar los permisos " + B::join(o.libres, ",") + " en "
                                  + objetivo + ". ¿Continuar?")) {
        std::fprintf(stderr, "cancelado\n");
        return false;
    }
    if (!zfsGenerico(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, "%s en %s\n", conceder ? "permisos delegados" : "permisos retirados",
                 objetivo.c_str());
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
            std::fprintf(stderr, "el pool %s no tiene letra de unidad asignada\n",
                         destino.pool.c_str());
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
            std::fprintf(stderr, "%s no está montado en ningún sitio\n",
                         destino.zfsName().c_str());
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
            std::fprintf(stderr, "no se pudo listar %s (código %d)\n", base.c_str(), rc);
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

bool cmdLs(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!o.libres.empty()) {
        std::string error;
        if (!resuelve(e, o.libres.front(), destino, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
    } else if (!destinoDe(e, o, destino)) {
        return false;
    }
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
        std::fprintf(stderr, "no sé listar la sección «%s»\n", sec.c_str());
        return false;
    }
    switch (nodoDe(destino)) {
        case Nodo::Raiz:
            listaConexiones(e);
            return true;
        case Nodo::Conexion:
            return listaPools(e, destino);
        case Nodo::Dataset:
            return listaDataset(e, destino);
        case Nodo::Snapshot:
            // Una instantánea no tiene hijos; lo interesante es lo que hay dentro.
            std::fprintf(stderr,
                         "una instantánea no tiene hijos: pruebe «ls #content» o «ls #properties»\n");
            return false;
    }
    return false;
}

bool cmdCd(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    std::string texto = o.libres.empty() ? o.valor("on", o.valor("from")) : o.libres.front();
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
            std::fprintf(stderr, "no se pudo comprobar %s: %s\n", destino.zfsName().c_str(),
                         motivo.c_str());
            e.ultimoRc = 1;
            return false;
        }
        if (rc != 0 || B::contains(out, "EXISTS=no")) {
            std::fprintf(stderr, "no existe: %s\n", destino.zfsName().c_str());
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
        std::fprintf(stderr, "hace falta estar en un dataset (ahora: %s)\n", textoDe(u).c_str());
        return false;
    }
    return true;
}

bool cmdSnapshot(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino)) {
        return false;
    }
    if (!exigeDataset(destino)) {
        return false;
    }
    std::string nombre = o.libres.empty() ? std::string() : o.libres.front();
    if (!nombre.empty() && nombre.front() == '@') {
        nombre = nombre.substr(1);
    }
    if (nombre.empty()) {
        std::fprintf(stderr, "hace falta un nombre: snapshot @<nombre>\n");
        return false;
    }
    const bool recursivo = o.tiene("-r");
    std::string out;
    if (!agente(e, destino,
                {"--mutate-zfs-snapshot", destino.dataset + "@" + nombre, recursivo ? "1" : "0"},
                out)) {
        return false;
    }
    std::fprintf(stderr, "creada %s@%s\n", destino.dataset.c_str(), nombre.c_str());
    return true;
}

bool cmdDestroy(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!o.libres.empty()) {
        std::string error;
        if (!resuelve(e, o.libres.front(), destino, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
    } else if (!destinoDe(e, o, destino)) {
        return false;
    }
    // En una CONEXIÓN, `destroy` la quita de la configuración. Es lo único que se puede
    // «destruir» ahí, y la pregunta lo deja claro: no se toca nada en la máquina.
    if (nodoDe(destino) == Nodo::Conexion) {
        const std::string id = idDe(e, destino);
        if (!confirma(e, "Se va a quitar la conexión " + id
                             + " de la configuración. NO se toca nada en la máquina. ¿Continuar?")) {
            std::fprintf(stderr, "cancelado\n");
            return false;
        }
        std::string error;
        if (!borrarConexion(*e.ses, id, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
        recarga(e);
        std::fprintf(stderr, "quitada la conexión %s\n", id.c_str());
        cmdCd(e, {"/"});
        return true;
    }
    // En un POOL, `destroy` es `zpool destroy`: `zfs destroy` sobre el dataset raíz de un
    // pool no funciona, así que es la única lectura posible.
    if (nodoDe(destino) == Nodo::Dataset && destino.isPoolRoot() && destino.snapshot.empty()) {
        if (!confirma(e, "Se va a DESTRUIR EL POOL " + destino.pool + " en " + destino.connection
                             + ", con todos sus datasets y todos sus datos. ¿Continuar?")) {
            std::fprintf(stderr, "cancelado\n");
            return false;
        }
        std::vector<std::string> argv{"destroy"};
        if (o.tiene("-f")) {
            argv.push_back("-f");
        }
        argv.push_back(destino.pool);
        if (!zpoolGenerico(e, destino, argv)) {
            return false;
        }
        std::fprintf(stderr, "destruido el pool %s\n", destino.pool.c_str());
        cmdCd(e, {".."});
        return true;
    }
    const std::string objetivo = destino.zfsName();
    if (objetivo.empty()) {
        std::fprintf(stderr, "no hay nada que borrar en %s\n", textoDe(destino).c_str());
        return false;
    }
    const bool recursivo = o.tiene("-r") || o.tiene("-R");
    // La pregunta dice QUÉ se va a borrar y con qué alcance: «¿seguro?» a secas es lo que
    // hace que se conteste que sí sin leer.
    if (!confirma(e, std::string("Se va a DESTRUIR ") + objetivo
                         + (recursivo ? " y todos sus descendientes" : "") + " en "
                         + destino.connection + ". ¿Continuar?")) {
        std::fprintf(stderr, "cancelado\n");
        return false;
    }
    std::string out;
    if (!agente(e, destino,
                {"--mutate-zfs-destroy", objetivo, o.tiene("-f") ? "1" : "0",
                 o.tiene("-R") ? "R" : (o.tiene("-r") ? "r" : "")},
                out)) {
        return false;
    }
    std::fprintf(stderr, "destruido %s\n", objetivo.c_str());
    // Si se ha borrado justo donde estábamos, se sube: quedarse apuntando a algo que ya no
    // existe hace que la siguiente orden falle sin que se entienda por qué.
    if (destino.zfsName() == e.actual.zfsName() && destino.connection == e.actual.connection) {
        cmdCd(e, {".."});
    }
    return true;
}

bool cmdRollback(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!o.libres.empty()) {
        std::string error;
        if (!resuelve(e, o.libres.front(), destino, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
    } else if (!destinoDe(e, o, destino)) {
        return false;
    }
    if (destino.snapshot.empty()) {
        std::fprintf(stderr, "hace falta una instantánea: rollback @<nombre>\n");
        return false;
    }
    if (!confirma(e, "Se va a volver " + destino.dataset + " al estado de @" + destino.snapshot
                         + ", DESCARTANDO todo lo posterior. ¿Continuar?")) {
        std::fprintf(stderr, "cancelado\n");
        return false;
    }
    std::string out;
    if (!agente(e, destino,
                {"--mutate-zfs-rollback", destino.zfsName(), o.tiene("-f") ? "1" : "0",
                 o.tiene("-R") ? "R" : (o.tiene("-r") ? "r" : "")},
                out)) {
        return false;
    }
    std::fprintf(stderr, "%s vuelto a @%s\n", destino.dataset.c_str(), destino.snapshot.c_str());
    return true;
}

bool cmdClone(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    if (o.libres.empty()) {
        std::fprintf(stderr, "uso: clone <nuevo-dataset> [--from <@instantánea>]\n");
        return false;
    }
    ZfsmUrl origen;
    if (!destinoDe(e, o, origen)) {
        return false;
    }
    if (origen.snapshot.empty()) {
        std::fprintf(stderr, "el origen tiene que ser una instantánea (ahora: %s)\n",
                     textoDe(origen).c_str());
        return false;
    }
    std::string out;
    if (!agente(e, origen, {"--mutate-zfs-clone", origen.zfsName(), o.libres.front()}, out)) {
        return false;
    }
    std::fprintf(stderr, "clonado %s -> %s\n", origen.zfsName().c_str(), o.libres.front().c_str());
    return true;
}

// Alta de una conexión. Es `create` estando en la RAÍZ, por la misma regla que hace que
// `create` en un dataset cree un hijo: se crea un nodo donde uno está.
//
// Los campos se piden por el terminal si no vienen por opciones, y **la contraseña NUNCA
// por argumento**: iría en `argv` y se vería en `ps` para cualquier usuario de la máquina.
// O se teclea, o entra por un descriptor con --password-fd.
bool cmdCrearConexion(Estado& e, const Opts& o) {
    if (o.libres.empty()) {
        std::fprintf(stderr,
                     "uso: create <identificador> [--name <n>] [--type LOCAL|SSH] [--os <so>]\n"
                     "            [--host <h>] [--port <p>] [--user <u>] [--key <ruta>]\n"
                     "            [--sudo] [--password-fd <n>]\n"
                     "  Da de alta una conexión. Lo que no se dé por opciones se pregunta.\n");
        return false;
    }
    B::ConnectionProfile p;
    p.id = B::trim(o.libres.front());
    if (buscarConexion(e.conns, p.id) != nullptr) {
        std::fprintf(stderr, "ya existe una conexión «%s»\n", p.id.c_str());
        return false;
    }
    const bool interactivo = hayTerminal();

    p.name = o.valor("name");
    p.connType = B::toUpperAscii(o.valor("type"));
    p.osType = o.valor("os");
    p.host = o.valor("host");
    p.username = o.valor("user");
    p.keyPath = o.valor("key");
    p.useSudo = o.tiene("--sudo");
    const std::string puertoTexto = o.valor("port");

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
            std::fprintf(stderr, "una conexión SSH necesita un host\n");
            return false;
        }
        if (p.username.empty() && interactivo && !pide("Usuario", "", p.username)) return false;
        if (p.keyPath.empty() && interactivo && !pide("Ruta de clave SSH (vacío = contraseña)", "",
                                                      p.keyPath)) {
            return false;
        }
    }
    p.port = puertoTexto.empty() ? 0 : std::atoi(puertoTexto.c_str());

    // La contraseña. Por descriptor si se dio, y si no por el terminal con el eco apagado.
    // Solo hace falta si no hay clave SSH, o si la máquina va a necesitar sudo.
    //
    // **Se comprueba ANTES si hay dónde cifrarla.** Guardar una contraseña de acceso en
    // claro no se hace, así que sin contraseña maestra la creación fracasaría — y hacerla
    // teclear para tirarla después es la peor forma de contarlo. Si hay terminal se pide la
    // maestra en ese momento; si no, se avisa y se crea sin contraseña.
    const std::string fdTexto = o.valor("password-fd");
    const bool quiereClave = !fdTexto.empty() || (p.keyPath.empty() || p.useSudo || local);
    std::string err;
    if (quiereClave && e.ses->maestra.empty()) {
        if (interactivo) {
            std::fprintf(stderr,
                         "Para guardar la contraseña de una conexión hace falta una "
                         "contraseña maestra:\n"
                         "es con la que se cifra en config.json, y sin ella no se guarda en "
                         "claro.\n");
            std::string maestra;
            if (!preguntarSecretoPorTerminal("Contraseña maestra: ", maestra, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return false;
            }
            e.ses->maestra = maestra;
        } else {
            std::fprintf(stderr,
                         "aviso: sin contraseña maestra (--password-fd) la conexión se crea "
                         "SIN contraseña\n");
        }
    }
    if (!fdTexto.empty()) {
        if (!leerSecretoDeDescriptor(std::atoi(fdTexto.c_str()), p.password, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
    } else if (interactivo && quiereClave && !e.ses->maestra.empty()) {
        std::string clave;
        if (!preguntarSecretoPorTerminal("Contraseña (vacío = ninguna): ", clave, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
        p.password = clave;
    }
    if (!o.tiene("--sudo") && interactivo) {
        std::string resp;
        if (!pide("¿Usa sudo? (s/N)", "n", resp)) return false;
        const std::string r = B::toLowerAscii(resp);
        p.useSudo = (r == "s" || r == "si" || r == "sí" || r == "y" || r == "yes");
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
    std::fprintf(stderr, "creada la conexión %s (%s)\n", p.id.c_str(),
                 local ? "local" : (p.username + "@" + p.host).c_str());
    return true;
}

bool cmdEditarConexion(Estado& e, const Opts& o, const ZfsmUrl& destino) {
    const std::string id = idDe(e, destino);
    const auto* actual = buscarConexion(e.conns, id);
    if (!actual) {
        std::fprintf(stderr, "hace falta estar en una conexión (ahora: %s)\n",
                     textoDe(destino).c_str());
        return false;
    }
    B::ConnectionProfile p = *actual;
    const bool interactivo = hayTerminal();
    const auto toma = [&](const char* clave, std::string& campo, const char* etiqueta) {
        const std::string v = o.valor(clave);
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
    if (o.tiene("--sudo")) {
        p.useSudo = true;
    } else if (o.tiene("--no-sudo")) {
        p.useSudo = false;
    } else if (interactivo) {
        std::string resp;
        if (!pide("¿Usa sudo? (s/n)", p.useSudo ? "s" : "n", resp)) return false;
        const std::string r = B::toLowerAscii(resp);
        p.useSudo = (r == "s" || r == "si" || r == "sí" || r == "y" || r == "yes");
    }

    // La contraseña solo se toca si se pide expresamente: en una edición, dejarla en blanco
    // tiene que CONSERVARLA, no borrarla. Ya viene descifrada del perfil cargado, y
    // guardarConexion la vuelve a cifrar.
    const std::string fdTexto = o.valor("password-fd");
    if (!fdTexto.empty()) {
        std::string err;
        if (!leerSecretoDeDescriptor(std::atoi(fdTexto.c_str()), p.password, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
    } else if (o.tiene("--password") && interactivo) {
        std::string err;
        std::string clave;
        if (!preguntarSecretoPorTerminal("Contraseña nueva: ", clave, err)) {
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
    std::fprintf(stderr, "actualizada la conexión %s\n", id.c_str());
    return true;
}

bool cmdEdit(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from", "name", "type", "os", "host", "port", "user",
                                 "key", "password-fd"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino)) {
        return false;
    }
    return cmdEditarConexion(e, o, destino);
}

bool cmdCreate(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from", "name", "type", "os", "host", "port", "user",
                                 "key", "password-fd", "mountpoint"},
                          {"o", "O"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino)) {
        return false;
    }
    // El mismo verbo en los tres niveles, porque es la misma idea —crear un nodo donde uno
    // está—: en la RAÍZ una conexión, en una CONEXIÓN un pool, en un DATASET un hijo.
    if (nodoDe(destino) == Nodo::Raiz) {
        return cmdCrearConexion(e, o);
    }
    if (nodoDe(destino) == Nodo::Conexion) {
        return cmdCrearPool(e, o, destino);
    }
    if (o.libres.empty()) {
        std::fprintf(stderr, "uso: create <nombre> [prop=valor...]\n");
        return false;
    }
    if (!exigeDataset(destino)) {
        return false;
    }
    std::vector<std::string> argv{"create"};
    for (std::size_t i = 1; i < o.libres.size(); ++i) {
        argv.push_back("-o");
        argv.push_back(o.libres[i]);
    }
    // El nombre se toma como HIJO del sitio actual salvo que ya venga con ruta: escribir
    // `create datos` estando en `tank/casa` debe crear `tank/casa/datos`.
    const std::string hijo = o.libres.front();
    argv.push_back(hijo.find('/') == std::string::npos ? destino.dataset + "/" + hijo : hijo);
    if (!zfsGenerico(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, "creado %s\n", argv.back().c_str());
    return true;
}

bool cmdRename(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    if (o.libres.empty()) {
        std::fprintf(stderr, "uso: rename <nuevo-nombre>\n");
        return false;
    }
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino)) {
        return false;
    }
    if (!exigeDataset(destino)) {
        return false;
    }
    const std::string nuevo = o.libres.front();
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

bool cmdMontaje(Estado& e, const std::vector<std::string>& args, bool montar) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino)) {
        return false;
    }
    if (!exigeDataset(destino)) {
        return false;
    }
    std::vector<std::string> argv{montar ? "mount" : "unmount"};
    if (o.tiene("-f")) {
        argv.push_back("-f");
    }
    argv.push_back(destino.dataset);
    if (!zfsGenerico(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, "%s %s\n", montar ? "montado" : "desmontado", destino.dataset.c_str());
    return true;
}

bool cmdPromote(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino) || !exigeDataset(destino)) {
        return false;
    }
    if (!zfsGenerico(e, destino, {"promote", destino.dataset})) {
        return false;
    }
    std::fprintf(stderr, "promovido %s\n", destino.dataset.c_str());
    return true;
}

bool cmdSet(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    if (o.libres.empty()) {
        std::fprintf(stderr, "uso: set <propiedad>=<valor> [más...]\n");
        return false;
    }
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino)) {
        return false;
    }
    const std::string objetivo = destino.zfsName();
    if (objetivo.empty()) {
        std::fprintf(stderr, "hace falta un dataset o una instantánea\n");
        return false;
    }
    std::vector<std::string> argv{"set"};
    for (const auto& asig : o.libres) {
        if (asig.find('=') == std::string::npos) {
            std::fprintf(stderr, "«%s» no es propiedad=valor\n", asig.c_str());
            return false;
        }
        argv.push_back(asig);
    }
    argv.push_back(objetivo);
    if (!zfsGenerico(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, "aplicadas %zu propiedades a %s\n", o.libres.size(), objetivo.c_str());
    return true;
}

bool cmdGet(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino)) {
        return false;
    }
    if (!o.libres.empty()) {
        destino.section = B::zfsmSection::kProperties;
        destino.detail = {o.libres.front()};
    } else {
        destino.section = B::zfsmSection::kProperties;
        destino.detail.clear();
    }
    return listaPropiedades(e, destino);
}

bool cmdClaves(Estado& e, const std::vector<std::string>& args, const char* op) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino) || !exigeDataset(destino)) {
        return false;
    }
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
        std::fprintf(stderr, "clave cargada en %s\n", destino.dataset.c_str());
        return true;
    }
    if (!zfsGenerico(e, destino, {op, destino.dataset})) {
        return false;
    }
    std::fprintf(stderr, "%s en %s\n", op, destino.dataset.c_str());
    return true;
}

// --- Las cuatro acciones de Avanzado.

bool cmdBreakdown(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino) || !exigeDataset(destino)) {
        return false;
    }
    if (o.libres.size() < 2 || o.libres.size() % 2 != 0) {
        std::fprintf(stderr,
                     "uso: breakdown <directorio> <nombre-hijo> [<directorio> <nombre-hijo>...]\n"
                     "  Convierte cada directorio del dataset en un dataset hijo.\n");
        return false;
    }
    if (!confirma(e, "Se van a convertir " + std::to_string(o.libres.size() / 2)
                         + " directorios de " + destino.dataset
                         + " en datasets hijos. ¿Continuar?")) {
        std::fprintf(stderr, "cancelado\n");
        return false;
    }
    std::vector<std::string> argv{"--mutate-advanced-breakdown", destino.dataset};
    for (const auto& x : o.libres) {
        argv.push_back(x);
    }
    // Con --job se manda al daemon y se vuelve enseguida; sin él se espera. SIN PLAZO al
    // esperar: mueve datos de verdad, y matarlo a mitad es peor que aguardar.
    if (o.tiene("--job")) {
        return enviaComoTrabajo(e, destino, argv);
    }
    std::string out;
    if (!agente(e, destino, argv, out, 0)) {
        return false;
    }
    std::fprintf(stdout, "%s", out.c_str());
    return true;
}

bool cmdAssemble(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino) || !exigeDataset(destino)) {
        return false;
    }
    if (o.libres.empty()) {
        std::fprintf(stderr,
                     "uso: assemble <hijo> [<hijo>...]\n"
                     "  Deshace lo de breakdown: devuelve cada dataset hijo a directorio.\n");
        return false;
    }
    if (!confirma(e, "Se van a reintegrar " + std::to_string(o.libres.size()) + " datasets hijos en "
                         + destino.dataset + " como directorios. ¿Continuar?")) {
        std::fprintf(stderr, "cancelado\n");
        return false;
    }
    // Los hijos van con NOMBRE COMPLETO. El agente los comprueba con `zfs list <hijo>`, así
    // que un nombre relativo no existe para él y la operación se salda con «ya absorbido»
    // y rc=0: parece que ha funcionado y no ha hecho nada.
    std::vector<std::string> argv{"--mutate-advanced-assemble", destino.dataset};
    for (const auto& x : o.libres) {
        argv.push_back(x.find('/') == std::string::npos ? destino.dataset + "/" + x : x);
    }
    if (o.tiene("--job")) {
        return enviaComoTrabajo(e, destino, argv);
    }
    std::string out;
    if (!agente(e, destino, argv, out, 0)) {
        return false;
    }
    std::fprintf(stdout, "%s", out.c_str());
    return true;
}

bool cmdToDir(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino) || !exigeDataset(destino)) {
        return false;
    }
    if (o.libres.empty()) {
        std::fprintf(stderr,
                     "uso: todir <directorio-destino> [--delete-source]\n"
                     "  Vuelca el contenido del dataset a un directorio.\n");
        return false;
    }
    const bool borraOrigen = o.tiene("--delete-source");
    if (!confirma(e, "Se va a volcar " + destino.dataset + " a " + o.libres.front()
                         + (borraOrigen ? " y DESTRUIR el dataset de origen" : "") + ". ¿Continuar?")) {
        std::fprintf(stderr, "cancelado\n");
        return false;
    }
    const std::vector<std::string> argvTodir{"--mutate-advanced-todir", destino.dataset,
                                             o.libres.front(), borraOrigen ? "1" : "0"};
    if (o.tiene("--job")) {
        return enviaComoTrabajo(e, destino, argvTodir);
    }
    std::string out;
    if (!agente(e, destino, argvTodir, out, 0)) {
        return false;
    }
    std::fprintf(stdout, "%s", out.c_str());
    return true;
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
bool cmdFromDir(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from", "subdir"});
    if (o.libres.empty()) {
        std::fprintf(stderr,
                     "uso: fromdir <directorio-origen> [--from <url-origen>] [--subdir <rel>]\n"
                     "  Vuelca un directorio dentro del dataset actual. El origen puede estar\n"
                     "  en otra máquina: --from acepta cualquier URL zfsm://.\n");
        return false;
    }
    // El DESTINO es donde estamos. `--from` nombra el origen, que aquí es otra máquina y no
    // otro sitio: es la única orden en la que las dos opciones no son sinónimas.
    ZfsmUrl destino = e.actual;
    if (!o.valor("on").empty()) {
        std::string error;
        if (!resuelve(e, o.valor("on"), destino, error)) {
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
    if (!o.valor("from").empty()) {
        std::string error;
        if (!resuelve(e, o.valor("from"), origen, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
        pSrc = perfilDe(e, origen, error);
        if (!pSrc) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
    }

    const std::string dir = o.libres.front();
    const std::string rel = o.valor("subdir");
    if (!confirma(e, "Se va a volcar " + dir + " de " + origen.connection + " dentro de "
                         + destino.dataset + " en " + destino.connection + ". ¿Continuar?")) {
        std::fprintf(stderr, "cancelado\n");
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
        std::fprintf(stderr, "fromdir falló (código %d)\n", rc);
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
bool cmdCopy(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from", "base", "flags", "to"});
    if (o.libres.empty() && o.valor("to").empty()) {
        std::fprintf(stderr,
                     "uso: copy <destino> [--from <@instantánea>] [--base <@instantánea>]\n"
                     "          [--flags <banderas de zfs send>] [--wait]\n"
                     "  El destino es una URL: puede estar en OTRA máquina.\n"
                     "  Sin --from se usa el sitio actual como origen.\n"
                     "  Con --base solo viaja lo que cambió desde ahí (lo que la interfaz\n"
                     "  llama «Nivelar»).\n");
        return false;
    }
    ZfsmUrl origen;
    if (!destinoDe(e, o, origen)) {
        return false;
    }
    if (origen.snapshot.empty()) {
        std::fprintf(stderr, "el origen tiene que ser una INSTANTÁNEA (ahora: %s)\n",
                     textoDe(origen).c_str());
        return false;
    }
    ZfsmUrl destino;
    std::string error;
    if (!resuelve(e, o.libres.empty() ? o.valor("to") : o.libres.front(), destino, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    if (destino.dataset.empty()) {
        std::fprintf(stderr, "el destino tiene que ser un dataset\n");
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
        std::fprintf(stderr,
                     "la transferencia por socket no está disponible en Windows.\n"
                     "Para llevar datos a o desde una máquina Windows, use «todir» y "
                     "«fromdir».\n");
        return false;
    }
    const bool mismaMaquina = B::toLowerAscii(origen.connection) == B::toLowerAscii(destino.connection);

    std::string base = o.valor("base");
    if (!base.empty() && base.front() == '@') {
        base = origen.dataset + base;
    }
    if (!confirma(e, std::string("Se va a ") + (base.empty() ? "copiar" : "nivelar") + " "
                         + origen.zfsName() + " de " + origen.connection + " a "
                         + destino.dataset + " en " + destino.connection
                         + ".\nSi el destino existe y difiere, `zfs recv -F` lo SOBRESCRIBE. "
                           "¿Continuar?")) {
        std::fprintf(stderr, "cancelado\n");
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
        std::fprintf(stderr, "el destino no abrió el puerto de recepción correctamente\n");
        return false;
    }

    // 2) Con qué dirección ve el origen al destino. En la misma máquina, por el bucle
    // local; si no, por el host del perfil — que es como el origen llega a él.
    const std::string peer = mismaMaquina ? "127.0.0.1" : B::trim(pDestino->host);
    if (peer.empty()) {
        std::fprintf(stderr,
                     "no se sabe con qué dirección ve %s a %s: la conexión de destino no "
                     "tiene host\n",
                     origen.connection.c_str(), destino.connection.c_str());
        return false;
    }

    // 3) El origen envía. Como TRABAJO: una transferencia grande no cabe en una espera.
    std::string sendOut;
    if (!agente(e, origen,
                {"--zfs-send-to-peer-async", origen.zfsName(), peer, puerto, testigo, base,
                 o.valor("flags")},
                sendOut, 30000)) {
        return false;
    }
    const auto cs = clavesDe(sendOut);
    const std::string jobId = cs.count("JOB_ID") ? cs.at("JOB_ID") : std::string();
    if (jobId.empty()) {
        std::fprintf(stderr, "el origen no devolvió identificador de trabajo\n");
        return false;
    }
    std::fprintf(stdout, "%s\n", jobId.c_str());
    std::fprintf(stderr, "transferencia en marcha como trabajo %s en %s\n", jobId.c_str(),
                 origen.connection.c_str());

    if (!o.tiene("--wait")) {
        std::fprintf(stderr, "siga con «job %s --on %s»\n", jobId.c_str(),
                     textoDe(origen).c_str());
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
bool cmdInstalarDaemon(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino)) {
        return false;
    }
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

    if (!confirma(e, "Se va a instalar o actualizar el daemon en " + quien
                         + " y arrancarlo con el gestor de servicios del sistema. ¿Continuar?")) {
        std::fprintf(stderr, "cancelado\n");
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
        std::fprintf(stderr,
                     "no se encontró el binario del daemon para %s/%s en este equipo.\n"
                     "No se instala nada: el respaldo por guion no habla TLS, y dejarlo puesto\n"
                     "daría una máquina que parece atendida y no lo está.\n",
                     plataforma.c_str(), arq.empty() ? "?" : arq.c_str());
        return false;
    }
    std::ifstream f(binario, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string contenido = ss.str();
    if (contenido.empty()) {
        std::fprintf(stderr, "el binario del daemon está vacío: %s\n", binario.c_str());
        return false;
    }
    std::fprintf(stderr, "desplegando %s (%zu bytes) en %s...\n", binario.c_str(),
                 contenido.size(), quien.c_str());

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
                std::fprintf(stderr, "no se pudo copiar el daemon a %s\n", subida.c_str());
                return false;
            }
        } else {
            const H::ScpInvocacion inv = H::scpUpload(perfil, binario, subida, false);
            const B::ExecResult r =
                B::runExecStream(inv.program, inv.args, std::string(), 300000, B::StreamCallbacks{});
            if (r.rc != 0) {
                std::fprintf(stderr, "scp falló (código %d): %s\n", r.rc,
                             B::trim(r.err).c_str());
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
            std::fprintf(stderr, "la instalación falló (código %d): %s\n", rc,
                         B::trim(err.empty() ? out : err).c_str());
            return false;
        }
        std::fprintf(stderr, "daemon instalado en %s\n", quien.c_str());
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
        std::fprintf(stderr, "la instalación falló (código %d): %s\n", rc,
                     B::trim(err.empty() ? out : err).c_str());
        e.ultimoRc = rc == 0 ? 1 : rc;
        return false;
    }
    // Lo que había cacheado del daemon anterior ya no vale.
    T::closeTunnelForConnection(e.ses->transporte, *p);
    T::clearRemoteDaemonTlsCacheForConnection(*p);
    T::clearLocalDaemonTlsCache();
    std::fprintf(stderr, "daemon instalado en %s\n", quien.c_str());
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

bool cmdJobs(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino)) {
        return false;
    }
    std::string out;
    if (!agente(e, destino, {"--job-list"}, out, 30000)) {
        return false;
    }
    Tabla t;
    t.nombreJson = "jobs";
    t.cabecerasTexto = {"ID", "ESTADO", "TIPO", "INSTANTÁNEA", "BYTES", "MiB/s", "SEGUNDOS", "ERROR"};
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
        std::fprintf(stderr, "no hay trabajos\n");
    }
    t.imprime(e.formato);
    return true;
}

bool cmdJob(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino)) {
        return false;
    }
    if (o.libres.empty()) {
        std::fprintf(stderr, "uso: job <id> | job cancel <id>\n");
        return false;
    }
    const bool cancelar = B::toLowerAscii(o.libres.front()) == "cancel";
    if (cancelar && o.libres.size() < 2) {
        std::fprintf(stderr, "uso: job cancel <id>\n");
        return false;
    }
    const std::string id = cancelar ? o.libres[1] : o.libres.front();
    if (cancelar) {
        if (!confirma(e, "Se va a cancelar el trabajo " + id
                             + ". Lo que ya haya hecho NO se deshace. ¿Continuar?")) {
            std::fprintf(stderr, "cancelado\n");
            return false;
        }
        std::string out;
        if (!agente(e, destino, {"--job-cancel", id}, out, 30000)) {
            return false;
        }
        std::fprintf(stderr, "cancelado el trabajo %s\n", id.c_str());
        return true;
    }
    std::string out;
    if (!agente(e, destino, {"--job-status", id}, out, 30000)) {
        return false;
    }
    Tabla t;
    t.nombreJson = "job";
    t.cabecerasTexto = {"CAMPO", "VALOR"};
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
        std::fprintf(stderr, "el daemon no devolvió identificador de trabajo\n");
        return false;
    }
    std::fprintf(stdout, "%s\n", it->second.c_str());
    std::fprintf(stderr, "en marcha como trabajo %s; siga con «job %s»\n", it->second.c_str(),
                 it->second.c_str());
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
        std::fprintf(stderr, "hace falta estar en un pool (ahora: %s)\n", textoDe(u).c_str());
        return false;
    }
    return true;
}

// Las operaciones de mantenimiento que se piden igual: un verbo y el nombre del pool, con
// un subcomando opcional para parar o pausar.
bool cmdMantenimientoPool(Estado& e, const std::vector<std::string>& args, const char* op) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino) || !exigePool(destino)) {
        return false;
    }
    std::vector<std::string> argv{op};
    // `zpool scrub -s` para, `-p` pausa; trim e initialize usan -s/-c/-u. Se aceptan por
    // nombre para no obligar a recordar qué letra usa cada uno.
    for (const std::string& x : o.libres) {
        const std::string v = B::toLowerAscii(x);
        if (v == "stop" || v == "cancel") {
            argv.push_back(std::string(op) == "scrub" ? "-s" : "-c");
        } else if (v == "pause" || v == "suspend") {
            argv.push_back(std::string(op) == "scrub" ? "-p" : "-s");
        } else if (v == "start") {
            // el comportamiento por omisión; se acepta por simetría
        } else {
            argv.push_back(x);  // un vdev concreto, para trim o initialize
        }
    }
    argv.push_back(destino.pool);
    if (!zpoolGenerico(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, "%s: %s en marcha\n", destino.pool.c_str(), op);
    return true;
}

bool cmdPoolSimple(Estado& e, const std::vector<std::string>& args, const char* op,
                   const char* aviso) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino) || !exigePool(destino)) {
        return false;
    }
    if (aviso && !confirma(e, std::string(aviso) + " en " + destino.pool + ". ¿Continuar?")) {
        std::fprintf(stderr, "cancelado\n");
        return false;
    }
    std::vector<std::string> argv{op};
    for (const std::string& x : o.libres) {
        argv.push_back(x);
    }
    argv.push_back(destino.pool);
    if (!zpoolGenerico(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, "%s: %s hecho\n", destino.pool.c_str(), op);
    return true;
}

// El estado detallado. Es texto para leer y se saca tal cual: fingir columnas donde
// `zpool status` dibuja un árbol de vdevs sería inventarse una estructura que no tiene.
bool cmdStatus(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino)) {
        return false;
    }
    if (destino.pool.empty()) {
        std::fprintf(stderr, "hace falta un pool (ahora: %s)\n", textoDe(destino).c_str());
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

bool cmdHistory(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino)) {
        return false;
    }
    if (destino.pool.empty()) {
        std::fprintf(stderr, "hace falta un pool (ahora: %s)\n", textoDe(destino).c_str());
        return false;
    }
    std::string out;
    if (!agente(e, destino, {"--dump-zpool-history", destino.pool}, out, 60000)) {
        return false;
    }
    Tabla t;
    t.nombreJson = "history";
    t.cabecerasTexto = {"CUÁNDO", "ORDEN"};
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
bool cmdImport(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from", "as"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino)) {
        return false;
    }
    if (destino.connection.empty()) {
        std::fprintf(stderr, "hace falta una conexión\n");
        return false;
    }
    if (o.libres.empty()) {
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
    if (o.tiene("-f")) {
        argv.push_back("-f");
    }
    argv.push_back(o.libres.front());
    if (!o.valor("as").empty()) {
        argv.push_back(o.valor("as"));  // importar con otro nombre
    }
    if (!zpoolGenerico(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, "importado %s\n", o.libres.front().c_str());
    return true;
}

// Crear un pool. Es la orden MÁS destructiva de todas: escribe en los dispositivos que se
// le den, y si alguno tenía datos, desaparecen. La confirmación los enumera uno a uno.
bool cmdCrearPool(Estado& e, const Opts& o, const ZfsmUrl& destino) {
    if (o.libres.size() < 2) {
        std::fprintf(stderr,
                     "uso: create <pool> <dispositivo> [<dispositivo>...] [-f]\n"
                     "            [-o prop=valor] [-O prop-fs=valor] [--mountpoint <ruta>]\n"
                     "  Los dispositivos se ESCRIBEN: lo que hubiera en ellos se pierde.\n");
        return false;
    }
    const std::string pool = o.libres.front();
    const std::vector<std::string> vdevs(o.libres.begin() + 1, o.libres.end());
    if (!confirma(e, "Se va a crear el pool " + pool + " en " + destino.connection
                         + " ESCRIBIENDO en: " + B::join(vdevs, ", ")
                         + ".\nLo que hubiera en esos dispositivos SE PIERDE. ¿Continuar?")) {
        std::fprintf(stderr, "cancelado\n");
        return false;
    }
    std::vector<std::string> argv{"create"};
    if (o.tiene("-f")) {
        argv.push_back("-f");
    }
    if (!o.valor("mountpoint").empty()) {
        argv.push_back("-m");
        argv.push_back(o.valor("mountpoint"));
    }
    for (const auto& kv : o.repetidas) {
        argv.push_back(kv.first == "o" ? "-o" : "-O");
        argv.push_back(kv.second);
    }
    argv.push_back(pool);
    for (const auto& v : vdevs) {
        argv.push_back(v);
    }
    if (!zpoolGenerico(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, "creado el pool %s\n", pool.c_str());
    return true;
}

// --- Editar una conexión ya dada de alta.
//
// Solo cambia lo que se pasa. Con terminal se ofrece el valor actual entre corchetes, de
// modo que pulsar Intro lo conserva: es lo que uno espera de «editar», frente a tener que
// volver a teclear todo.
bool cmdEditarConexion(Estado& e, const Opts& o, const ZfsmUrl& destino);

// --- Conectar, desconectar y refrescar.

std::string idDe(const Estado& e, const ZfsmUrl& u) {
    const auto* p = buscarConexion(e.conns, u.connection);
    return p ? (p->id.empty() ? p->name : p->id) : std::string();
}

bool cmdConectar(Estado& e, const std::vector<std::string>& args, bool conectar) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!o.libres.empty()) {
        std::string error;
        if (!resuelve(e, o.libres.front(), destino, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
    } else if (!destinoDe(e, o, destino)) {
        return false;
    }
    const std::string id = idDe(e, destino);
    if (id.empty()) {
        std::fprintf(stderr, "hace falta una conexión (ahora: %s)\n", textoDe(destino).c_str());
        return false;
    }
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
bool cmdRefrescar(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!o.libres.empty()) {
        std::string error;
        if (!resuelve(e, o.libres.front(), destino, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
    } else if (!destinoDe(e, o, destino)) {
        return false;
    }
    const std::string id = idDe(e, destino);
    if (id.empty()) {
        std::fprintf(stderr, "hace falta una conexión (ahora: %s)\n", textoDe(destino).c_str());
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
    t.cabecerasTexto = {"CAMPO", "VALOR"};
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
bool cmdHolds(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!o.libres.empty()) {
        std::string error;
        if (!resuelve(e, o.libres.front(), destino, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
    } else if (!destinoDe(e, o, destino)) {
        return false;
    }
    if (destino.snapshot.empty()) {
        std::fprintf(stderr, "hace falta una instantánea (ahora: %s)\n", textoDe(destino).c_str());
        return false;
    }
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
    t.cabecerasTexto = {"INSTANTÁNEA", "ETIQUETA", "CREADA"};
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

bool cmdRetencion(Estado& e, const std::vector<std::string>& args, bool poner) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino)) {
        return false;
    }
    if (o.libres.empty()) {
        std::fprintf(stderr, "uso: %s <etiqueta> [-r] [--on <@instantánea>]\n",
                     poner ? "hold" : "release");
        return false;
    }
    if (destino.snapshot.empty()) {
        std::fprintf(stderr, "hace falta una instantánea (ahora: %s)\n", textoDe(destino).c_str());
        return false;
    }
    std::vector<std::string> argv{poner ? "hold" : "release"};
    if (o.tiene("-r")) {
        argv.push_back("-r");
    }
    argv.push_back(o.libres.front());
    argv.push_back(destino.zfsName());
    if (!zfsGenerico(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, "%s la retención «%s» en %s\n", poner ? "puesta" : "quitada",
                 o.libres.front().c_str(), destino.zfsName().c_str());
    return true;
}

// Qué cambió entre dos instantáneas del mismo dataset.
bool cmdDiff(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    if (o.libres.empty()) {
        std::fprintf(stderr,
                     "uso: diff <@instantánea-o-dataset> [--from <@instantánea>]\n"
                     "  Compara dos puntos del MISMO dataset. Sin --from se usa el sitio\n"
                     "  actual como origen.\n");
        return false;
    }
    ZfsmUrl origen;
    if (!destinoDe(e, o, origen)) {
        return false;
    }
    ZfsmUrl hasta;
    std::string error;
    if (!resuelve(e, o.libres.front(), hasta, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    if (origen.snapshot.empty()) {
        std::fprintf(stderr, "el origen tiene que ser una instantánea (ahora: %s)\n",
                     textoDe(origen).c_str());
        return false;
    }
    std::string out;
    if (!agente(e, origen, {"--dump-zfs-diff", origen.zfsName(), hasta.zfsName()}, out, 120000)) {
        return false;
    }
    Tabla t;
    t.nombreJson = "changes";
    t.cabecerasTexto = {"CAMBIO", "RUTA", "RENOMBRADO A"};
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

bool cmdInfo(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino)) {
        return false;
    }
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

void ayuda() {
    std::fprintf(stderr,
        "Navegación (la posición es una URL, y todas las órdenes actúan sobre ella):\n"
        "  cd [destino]        Cambia de sitio. Sin argumento, a la raíz.\n"
        "                      Acepta ruta relativa, absoluta (/OldLau/winpool),\n"
        "                      URL completa, «..», «.» y «-» (el sitio anterior).\n"
        "  pwd                 La URL actual\n"
        "  ls [destino]        Lista. En la raíz, las conexiones; en una conexión,\n"
        "                      los pools; en un dataset, sus hijos e instantáneas.\n"
        "                      Con #content lista ficheros, con #properties propiedades\n"
        "                      y con #permissions los permisos delegados.\n"
        "  info [--on <url>]   Qué hay aquí y estado del daemon\n"
        "\n"
        "Conexiones (en la raíz, «cd /»):\n"
        "  create <id> [--name …] [--type LOCAL|SSH] [--os …] [--host …] [--port …]\n"
        "         [--user …] [--key …] [--sudo] [--password-fd <n>]\n"
        "                      Da de alta una conexión. Lo que falte se pregunta.\n"
        "                      La contraseña NUNCA por argumento: se teclea o entra\n"
        "                      por descriptor.\n"
        "  edit [--name …] [--host …] …\n"
        "                      Cambia una conexión. Lo que no se dé se pregunta, y\n"
        "                      pulsar Intro conserva el valor actual. --password para\n"
        "                      cambiar la contraseña; si no, se conserva.\n"
        "  destroy             Estando en una conexión, la quita de la configuración.\n"
        "                      No toca nada en la máquina.\n"
        "  connect / disconnect [destino]\n"
        "                      Marca la conexión como usable o no. Al desconectar se\n"
        "                      cierra su túnel. Es la misma marca que usa la interfaz.\n"
        "  refresh [destino]   Suelta túnel, material TLS y castigos, relee la\n"
        "                      configuración y vuelve a sondear la máquina.\n"
        "\n"
        "Dataset:\n"
        "  create <nombre> [prop=valor...]   Crea un dataset hijo\n"
        "  rename <nuevo>                    Renombra\n"
        "  destroy [destino] [-r|-R] [-f]    DESTRUYE. Pide confirmación.\n"
        "  mount / unmount [-f]              Monta o desmonta\n"
        "  promote                           Promueve un clon\n"
        "  snapshot @<nombre> [-r]           Crea una instantánea\n"
        "  rollback [@<nombre>] [-f|-r|-R]   Vuelve a una instantánea. Pide confirmación.\n"
        "  clone <nuevo> [--from @<inst>]    Clona una instantánea\n"
        "  get [propiedad]                   Lee propiedades\n"
        "  set <prop>=<valor> [más...]       Escribe propiedades\n"
        "  load-key / unload-key             Carga o descarga la clave de cifrado\n"
        "\n"
        "Instantáneas:\n"
        "  holds [destino]                   Retenciones de una instantánea\n"
        "  hold <etiqueta> [-r]              Pone una retención\n"
        "  release <etiqueta> [-r]           La quita\n"
        "  diff <@hasta> [--from <@desde>]   Qué cambió entre dos instantáneas\n"
        "\n"
        "Pools (estando en uno; import y create, en la conexión):\n"
        "  status / history                  Estado detallado / historial\n"
        "  scrub [stop|pause]                Verificación\n"
        "  trim / initialize [stop|pause] [<vdev>]\n"
        "  clear [<vdev>]                    Borra los errores contados\n"
        "  sync / upgrade / reguid           Mantenimiento\n"
        "  export [-f]                       Lo desmonta y lo suelta\n"
        "  import [<pool>] [--as <nuevo>] [-f]\n"
        "                                    Sin nombre, enseña los importables\n"
        "  create <pool> <dispositivo>... [-f] [-o p=v] [-O p=v] [--mountpoint <r>]\n"
        "                                    ESCRIBE en los dispositivos\n"
        "  destroy                           Estando en un pool, lo destruye entero\n"
        "\n"
        "Transferencias entre máquinas:\n"
        "  copy <destino> [--from <@inst>] [--base <@inst>] [--flags …] [--wait]\n"
        "                                    Manda una instantánea a otro dataset, aquí\n"
        "                                    o en otra máquina. Con --base solo viaja lo\n"
        "                                    que cambió («Nivelar»). Va como trabajo;\n"
        "                                    con --wait se espera aquí.\n"
        "\n"
        "Daemon:\n"
        "  install-daemon [--on <url>]       Instala o actualiza el daemon y lo arranca\n"
        "                                    con el gestor de servicios del sistema\n"
        "\n"
        "Trabajos en segundo plano:\n"
        "  jobs                              Los que hay en la máquina\n"
        "  job <id>                          Estado de uno\n"
        "  job cancel <id>                   Lo cancela\n"
        "  breakdown/assemble/todir --job    Los manda al daemon en vez de esperarlos\n"
        "\n"
        "Permisos delegados:\n"
        "  allow                             Los lista (igual que «ls #permissions»)\n"
        "  allow --user <u> <permisos...>    Delega. También --group, --everyone, --set\n"
        "        [--local] [--descend] [--create]\n"
        "  unallow --user <u> [permisos...]  Retira. Sin permisos, TODOS los suyos.\n"
        "\n"
        "Acciones:\n"
        "  breakdown <dir> <hijo> [...]      Convierte directorios en datasets hijos\n"
        "  assemble <hijo> [...]             Lo contrario de breakdown\n"
        "  todir <directorio> [--delete-source]  Vuelca el dataset a un directorio\n"
        "  fromdir <dir> [--from <url>] [--subdir <rel>]\n"
        "                                    Vuelca un directorio —de esta máquina o de\n"
        "                                    otra— dentro del dataset actual\n"
        "\n"
        "Del intérprete:\n"
        "  format text|tsv|json  Cambia el formato de los listados\n"
        "  yes on|off            Deja de preguntar antes de lo destructivo (o vuelve a hacerlo)\n"
        "  help                  Esta ayuda\n"
        "  exit, quit, Ctrl-D    Salir\n"
        "\n"
        "Todas las órdenes admiten --on <url> (o --from, que es lo mismo) para actuar\n"
        "sobre otro sitio sin moverse. Sin ella se usa el sitio actual.\n");
}

}  // namespace

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
            std::fprintf(stderr, "no se pudo empezar en %s: %s\n", inicio.c_str(),
                         errInicio.empty() ? "esa conexión no existe" : errInicio.c_str());
        }
        e.actual = ZfsmUrl{};
    }

    const bool interactivo = hayTerminal();
    if (interactivo) {
        std::fprintf(stderr, "zfsmgr-cli — «help» para la lista de órdenes, «exit» para salir.\n");
    }

    using Manejador = std::function<bool(Estado&, const std::vector<std::string>&)>;
    const std::map<std::string, Manejador> ordenes = {
        {"cd", cmdCd},
        {"ls", cmdLs},
        {"info", cmdInfo},
        {"create", cmdCreate},
        {"rename", cmdRename},
        {"destroy", cmdDestroy},
        {"mount", [](Estado& s, const std::vector<std::string>& a) { return cmdMontaje(s, a, true); }},
        {"unmount", [](Estado& s, const std::vector<std::string>& a) { return cmdMontaje(s, a, false); }},
        {"promote", cmdPromote},
        {"connect", [](Estado& s, const std::vector<std::string>& a) { return cmdConectar(s, a, true); }},
        {"disconnect", [](Estado& s, const std::vector<std::string>& a) { return cmdConectar(s, a, false); }},
        {"refresh", cmdRefrescar},
        {"edit", cmdEdit},
        {"holds", cmdHolds},
        {"hold", [](Estado& s, const std::vector<std::string>& a) { return cmdRetencion(s, a, true); }},
        {"release", [](Estado& s, const std::vector<std::string>& a) { return cmdRetencion(s, a, false); }},
        {"diff", cmdDiff},
        {"scrub", [](Estado& s, const std::vector<std::string>& a) { return cmdMantenimientoPool(s, a, "scrub"); }},
        {"trim", [](Estado& s, const std::vector<std::string>& a) { return cmdMantenimientoPool(s, a, "trim"); }},
        {"initialize", [](Estado& s, const std::vector<std::string>& a) { return cmdMantenimientoPool(s, a, "initialize"); }},
        {"sync", [](Estado& s, const std::vector<std::string>& a) { return cmdPoolSimple(s, a, "sync", nullptr); }},
        {"upgrade", [](Estado& s, const std::vector<std::string>& a) { return cmdPoolSimple(s, a, "upgrade", "Se va a subir la versión del pool, y eso NO se puede deshacer"); }},
        {"reguid", [](Estado& s, const std::vector<std::string>& a) { return cmdPoolSimple(s, a, "reguid", "Se va a cambiar el identificador único del pool"); }},
        {"clear", [](Estado& s, const std::vector<std::string>& a) { return cmdPoolSimple(s, a, "clear", nullptr); }},
        {"export", [](Estado& s, const std::vector<std::string>& a) { return cmdPoolSimple(s, a, "export", "Se va a exportar el pool y dejará de estar disponible"); }},
        {"import", cmdImport},
        {"status", cmdStatus},
        {"jobs", cmdJobs},
        {"job", cmdJob},
        {"install-daemon", cmdInstalarDaemon},
        {"copy", cmdCopy},
        {"history", cmdHistory},
        {"allow", [](Estado& s, const std::vector<std::string>& a) { return cmdPermisos(s, a, true); }},
        {"unallow", [](Estado& s, const std::vector<std::string>& a) { return cmdPermisos(s, a, false); }},
        {"snapshot", cmdSnapshot},
        {"rollback", cmdRollback},
        {"clone", cmdClone},
        {"get", cmdGet},
        {"set", cmdSet},
        {"load-key", [](Estado& s, const std::vector<std::string>& a) { return cmdClaves(s, a, "load-key"); }},
        {"unload-key", [](Estado& s, const std::vector<std::string>& a) { return cmdClaves(s, a, "unload-key"); }},
        {"breakdown", cmdBreakdown},
        {"assemble", cmdAssemble},
        {"todir", cmdToDir},
        {"fromdir", cmdFromDir},
    };

    std::string linea;
    while (!e.salir) {
        if (interactivo) {
            std::fprintf(stderr, "%s> ", textoDe(e.actual).c_str());
            std::fflush(stderr);
        }
        if (!std::getline(std::cin, linea)) {
            if (interactivo) {
                std::fprintf(stderr, "\n");
            }
            break;
        }
        const std::string recortada = B::trim(linea);
        if (recortada.empty() || recortada.front() == '#') {
            continue;
        }
        // Se trocea como lo haría un shell POSIX: las comillas protegen los espacios, que
        // es lo que hace falta para un nombre de dataset o un directorio con espacios.
        const std::vector<std::string> partes = H::posixShellSplitArgs(recortada);
        if (partes.empty()) {
            continue;
        }
        const std::string orden = B::toLowerAscii(partes.front());
        const std::vector<std::string> args(partes.begin() + 1, partes.end());

        if (orden == "exit" || orden == "quit") {
            e.salir = true;
            continue;
        }
        if (orden == "help" || orden == "?") {
            ayuda();
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
                std::fprintf(stderr, "formato desconocido: %s (use text, tsv o json)\n", v.c_str());
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
            std::fprintf(stderr, "orden desconocida: %s (pruebe «help»)\n", orden.c_str());
            e.ultimoRc = 127;
            continue;
        }
        if (!it->second(e, args)) {
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
