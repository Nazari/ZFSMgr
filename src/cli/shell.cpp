#include "shell.h"

#include "daemonpayload.h"
#include "helpers.h"
#include "json.h"
#include "secretinput.h"
#include "strutil.h"
#include "transportcmd.h"
#include "transporttunnel.h"
#include "transportrpc.h"
#include "zfsmurl.h"

#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <set>
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
Opts trocea(const std::vector<std::string>& args, const std::vector<std::string>& conValor) {
    Opts o;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
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

bool cmdCreate(Estado& e, const std::vector<std::string>& args) {
    const Opts o = trocea(args, {"on", "from", "name", "type", "os", "host", "port", "user",
                                 "key", "password-fd"});
    ZfsmUrl destino;
    if (!destinoDe(e, o, destino)) {
        return false;
    }
    // En la RAÍZ se crea una conexión; en un dataset, un hijo. Es el mismo verbo porque es
    // la misma idea: crear un nodo donde uno está.
    if (nodoDe(destino) == Nodo::Raiz) {
        return cmdCrearConexion(e, o);
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
    std::string out;
    // Sin plazo: mueve datos de verdad, y matarlo a mitad es peor que esperar.
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
    std::string out;
    if (!agente(e, destino,
                {"--mutate-advanced-todir", destino.dataset, o.libres.front(),
                 borraOrigen ? "1" : "0"},
                out, 0)) {
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
        "                      Con #content lista ficheros; con #properties, propiedades.\n"
        "  info [--on <url>]   Qué hay aquí y estado del daemon\n"
        "\n"
        "Conexiones (en la raíz, «cd /»):\n"
        "  create <id> [--name …] [--type LOCAL|SSH] [--os …] [--host …] [--port …]\n"
        "         [--user …] [--key …] [--sudo] [--password-fd <n>]\n"
        "                      Da de alta una conexión. Lo que falte se pregunta.\n"
        "                      La contraseña NUNCA por argumento: se teclea o entra\n"
        "                      por descriptor.\n"
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
