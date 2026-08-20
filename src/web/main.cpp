// zfsmgr-web — fase 0.
//
// Ver docs/diseno_tecnico_servidor_web.md. Aquí solo hay: TLS, sesión, testigo anti-CSRF y
// UNA página con la lista de conexiones. Nada de mutaciones todavía, a propósito: la fase 0
// existe para dejar bien la superficie, que es donde está el riesgo.
//
// Corre como el USUARIO, no como root, y escucha en 127.0.0.1. El daemon no se toca.
#include "dav.h"
#include "http.h"
#include "sesion.h"

#include "agentversion.h"
#include "connectionjson.h"
#include "gsa.h"
#include "listados.h"
#include "session.h"
#include "secretinput.h"
#include "storefiles.h"
#include "storewarnings.h"
#include "strutil.h"
#include "tlsserver.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace B = zfsmgr::base;
namespace ST = zfsmgr::base::store;
namespace CJ = zfsmgr::base::connjson;
namespace H = zfsmgr::web::http;
namespace L = zfsmgr::base::listados;
namespace D = zfsmgr::web::dav;

namespace {

std::atomic<bool> g_vivo{true};

void alSenal(int) { g_vivo.store(false); }

// El MISMO directorio que usan la interfaz y el intérprete. Copiado de `cli/main.cpp` a
// propósito de momento: si las tres empiezan a discrepar, esto baja a la capa base.
std::string dirConfigPorOmision() {
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::string(home) + "/.config/ZFSMgr";
    }
#ifdef _WIN32
    const char* perfil = std::getenv("USERPROFILE");
    if (perfil && *perfil) {
        return std::string(perfil) + "/.config/ZFSMgr";
    }
#endif
    return ".config/ZFSMgr";
}

struct Opciones {
    std::string dirConfig;
    std::string bind{"127.0.0.1"};
    int puerto{47654};
    int passwordFd{-1};
};

void uso() {
    std::fprintf(stderr,
                 "Uso: zfsmgr-web [opciones]\n"
                 "\n"
                 "Sirve la interfaz por HTTPS en 127.0.0.1. El daemon no interviene: esto es\n"
                 "un CLIENTE, como el intérprete, y corre como tu usuario.\n"
                 "\n"
                 "Opciones:\n"
                 "  --config-dir <ruta>   Dónde está config.json.\n"
                 "  --bind <dir>          Dirección de escucha. Por omisión 127.0.0.1, y\n"
                 "                        cambiarlo expone tus máquinas: hazlo a sabiendas.\n"
                 "  --port <n>            Puerto. Por omisión 47654.\n"
                 "  --password-fd <n>     Lee la contraseña maestra de ese descriptor.\n"
                 "\n"
                 "La contraseña maestra NO se pasa por argumento ni por variable de entorno:\n"
                 "las dos salen en «ps» para cualquier usuario de la máquina.\n");
}

// La hoja de estilo, servida por el propio servidor en `/estilo.css`.
//
// No es un fichero en disco: va compilada dentro. Un binario que no depende de que nadie
// haya copiado sus recursos al sitio correcto es un binario que arranca en cualquier parte,
// y ese es medio motivo de que el agente sea un solo fichero.
//
// La CSP dice `style-src 'self'`, así que esto se puede enlazar pero un `style=` suelto
// dentro del HTML no: si algún día se cuela texto sin escapar, el navegador tampoco lo
// pintaría.
const char* const kEstiloCss = R"CSS(
:root { color-scheme: light dark; --borde:#c9ced6; --suave:#f4f6f8; --tinta:#1c2530; --tenue:#5b6673; --acento:#2f5f8c; }
@media (prefers-color-scheme: dark) {
  :root { --borde:#39424e; --suave:#232a33; --tinta:#e7ecf2; --tenue:#9aa5b3; --acento:#7fb0e0; }
}
body { font: 15px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; color: var(--tinta);
       margin: 0; padding: 0 1.5rem 3rem; max-width: 1100px; }
body.ancho { max-width: none; }
a { color: var(--acento); text-decoration: none; }
a:hover { text-decoration: underline; }
h1 { font-size: 1.5rem; margin: .4rem 0 1rem; }
h2 { font-size: 1.05rem; margin: 1.6rem 0 .5rem; color: var(--tenue);
     text-transform: uppercase; letter-spacing: .04em; }
nav.migas { padding: .8rem 0; color: var(--tenue); font-size: .9rem; }
nav.migas a { color: var(--tenue); }
table { border-collapse: collapse; width: 100%; margin: .3rem 0 1rem; font-size: .93rem; }
th { text-align: left; font-weight: 600; color: var(--tenue); font-size: .8rem;
     text-transform: uppercase; letter-spacing: .03em; padding: .4rem .6rem;
     border-bottom: 1px solid var(--borde); }
td { padding: .38rem .6rem; border-bottom: 1px solid var(--suave); }
tr:hover td { background: var(--suave); }
form.enlinea { display: inline-block; margin: 0 .4rem .4rem 0; }
button { font: inherit; padding: .3rem .8rem; border: 1px solid var(--borde);
         background: var(--suave); color: var(--tinta); border-radius: 4px; cursor: pointer; }
button:hover { border-color: var(--acento); }
button.peligro { border-color: #a33; color: #a33; }
input { font: inherit; padding: .28rem .45rem; border: 1px solid var(--borde);
        border-radius: 4px; background: transparent; color: var(--tinta); }
pre { background: var(--suave); padding: .8rem; overflow-x: auto; font-size: .85rem;
      border-radius: 4px; }
p.vacio { color: var(--tenue); font-style: italic; }
span.tenue { color: var(--tenue); font-size: .85em; }
details > summary { cursor: pointer; padding: .16rem 0; list-style: none; }
details > summary::-webkit-details-marker { display: none; }
details > summary::before { content: "\25B8"; color: var(--tenue); display: inline-block;
                            width: 1em; transition: transform .12s; }
details[open] > summary::before { transform: rotate(90deg); }
div.rama { margin-left: 1.1rem; padding-left: .6rem; border-left: 1px solid var(--borde); }
ul.instantaneas { list-style: none; margin: .1rem 0 .3rem; padding: 0; font-size: .88rem; }
ul.instantaneas li { padding: .1rem 0; color: var(--tenue); }
details.menu { display: inline-block; margin: .1rem 0 .35rem; }
details.menu > summary { font-size: .82rem; color: var(--tenue); text-transform: uppercase;
                         letter-spacing: .03em; }
div.dos { display: grid; grid-template-columns: minmax(230px, 26%) 1fr; gap: 0 1.5rem;
           align-items: start; }
div.izq { border-right: 1px solid var(--borde); padding: 0 1rem .5rem 0;
          position: sticky; top: 0; max-height: 100vh; overflow: auto; }
div.der { min-width: 0; }
@media (max-width: 820px) {
  div.dos { grid-template-columns: 1fr; }
  div.izq { border-right: none; border-bottom: 1px solid var(--borde); position: static;
            max-height: 22rem; margin-bottom: 1rem; }
}
div.arbol { font-size: .9rem; white-space: nowrap; }
div.arbol a.nodo { color: var(--tinta); }
div.arbol a.sel { background: var(--acento); color: #fff; border-radius: 3px;
                  padding: .05rem .35rem; text-decoration: none; }
div.hoja { padding: .1rem 0 .1rem 1.15em; }
div.hoja a.nodo { color: var(--tenue); }
span.seccion { color: var(--tenue); }
div.seccion.tit { color: var(--tenue); font-size: .78rem; text-transform: uppercase;
                  letter-spacing: .04em; margin: .7rem 0 .2rem; }
table.ficha { width: auto; }
table.ficha th { width: 12rem; border-bottom: 1px solid var(--suave); vertical-align: top; }
table.ficha td { font-variant-numeric: tabular-nums; }
div.grupo { border: 1px solid var(--borde); border-radius: 6px; padding: .6rem .8rem;
            margin: 0 0 .7rem; }
div.grupotit { font-size: .78rem; text-transform: uppercase; letter-spacing: .04em;
               color: var(--tenue); margin-bottom: .45rem; }
div.pendiente { color: var(--tenue); font-size: .86rem; border-left: 3px solid var(--borde);
                padding: .3rem 0 .3rem .7rem; margin: .8rem 0; }
footer { margin-top: 2.5rem; padding-top: .8rem; border-top: 1px solid var(--borde);
         color: var(--tenue); font-size: .85rem; }
)CSS";

// La cabecera y el pie, iguales en todas las páginas.
std::string envuelve(const std::string& titulo, const std::string& migas,
                     const std::string& cuerpo, const std::string& testigo,
                     bool ancho = false) {
    std::string h = "<!doctype html><html lang=\"es\"><head><meta charset=\"utf-8\">";
    h += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
    h += "<link rel=\"stylesheet\" href=\"/estilo.css\">";

    h += "<title>" + H::escapaHtml(titulo) + " — ZFSMgr</title></head>";
    // El ancho máximo de 1100px es bueno para leer un texto y malo para dos paneles: la
    // columna del árbol se queda sin sitio en cuanto los nombres se anidan. Se suelta justo
    // en las páginas que lo necesitan, en vez de castigar a todas.
    h += ancho ? "<body class=\"ancho\">" : "<body>";
    h += "<nav class=\"migas\">" + migas + "</nav>";
    h += "<h1>" + H::escapaHtml(titulo) + "</h1>";
    h += cuerpo;
    h += "<footer><form class=\"enlinea\" method=\"post\" action=\"/salir\">";
    h += "<input type=\"hidden\" name=\"testigo\" value=\"" + H::escapaHtml(testigo) + "\">";
    h += "<button type=\"submit\">Cerrar el servidor</button></form>";
    h += " zfsmgr-web " + H::escapaHtml(B::agentversion::laEsperada()) + "</footer>";
    h += "</body></html>";
    return h;
}

// Bytes legibles. `--dump-zfs-list-all` los da en crudo —«1647018242320»— porque el TSV
// sale de `zfs list -p`, y esa cifra no la lee nadie.
//
// Vive aquí y no en la capa base a propósito: las unidades son PRESENTACIÓN, y el
// intérprete tiene la suya en su tabla. Si aparece un tercer consumidor, baja.
std::string bytesLegibles(const std::string& crudo) {
    const std::string t = B::trim(crudo);
    if (t.empty() || t == "-") {
        return t;
    }
    for (const char c : t) {
        if (c < '0' || c > '9') {
            return t;   // ya viene con unidad, o no es un número: se deja como está
        }
    }
    double v = std::strtod(t.c_str(), nullptr);
    static const char* const unidades[] = {"B", "K", "M", "G", "T", "P"};
    std::size_t i = 0;
    while (v >= 1024.0 && i + 1 < sizeof(unidades) / sizeof(unidades[0])) {
        v /= 1024.0;
        ++i;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), i == 0 ? "%.0f%s" : "%.1f%s", v, unidades[i]);
    return buf;
}

// La fecha de creación. `zfs list -p` la da en segundos desde 1970 —«1787205288»— porque
// se pidió el listado en crudo para poder ordenar y restar; enseñar eso es no enseñar nada.
//
// Se pinta en la hora LOCAL de quien mira, no en la de la máquina remota: quien lee la
// página está aquí, y una instantánea «de anoche» tiene que salir con la hora de anoche
// suya. La diferencia importa con una máquina en otro huso.
std::string fechaLegible(const std::string& crudo) {
    const std::string t = B::trim(crudo);
    if (t.empty() || t == "-") {
        return t;
    }
    for (const char c : t) {
        if (c < '0' || c > '9') {
            return t;   // ya viene con formato: se deja como está
        }
    }
    const std::time_t cuando = static_cast<std::time_t>(std::strtoll(t.c_str(), nullptr, 10));
    std::tm partes{};
    if (localtime_r(&cuando, &partes) == nullptr) {
        return t;
    }
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &partes) == 0) {
        return t;
    }
    return buf;
}

// Un enlace. El destino se escapa igual que el texto: un identificador de conexión con
// comillas dentro rompería el atributo si no.
std::string enlace(const std::string& destino, const std::string& texto) {
    return "<a href=\"" + H::escapaHtml(destino) + "\">" + H::escapaHtml(texto) + "</a>";
}

std::string tabla(const std::vector<std::string>& cabeceras,
                  const std::vector<std::vector<std::string>>& filas) {
    if (filas.empty()) {
        return "<p class=\"vacio\">(no hay nada aquí)</p>";
    }
    std::string h = "<table><thead><tr>";
    for (const std::string& c : cabeceras) {
        h += "<th>" + H::escapaHtml(c) + "</th>";
    }
    h += "</tr></thead><tbody>";
    for (const auto& f : filas) {
        h += "<tr>";
        for (const std::string& c : f) {
            // OJO: las celdas llegan YA compuestas —algunas traen un enlace—, así que aquí
            // no se escapa. Quien las compone es responsable de escapar lo que venga de
            // fuera, y por eso `enlace()` escapa sus dos partes.
            h += "<td>" + c + "</td>";
        }
        h += "</tr>";
    }
    h += "</tbody></table>";
    return h;
}

// El argv de `zfs`, en JSON y base64, que es lo que espera `--mutate-zfs-generic`. El
// daemon solo admite una lista cerrada de operaciones; esto no envía shell.
std::string argvEnBase64(const std::vector<std::string>& argv) {
    B::json::Value arr{B::json::Array{}};
    for (const std::string& a : argv) {
        arr.push(B::json::Value(a));
    }
    return B::base64Encode(B::json::toCompact(arr));
}

// Un campo oculto con el testigo. Va en TODOS los formularios: sin él, el servidor
// rechaza la petición con 403 aunque la cookie sea la buena.
std::string campoTestigo(const std::string& testigo) {
    return "<input type=\"hidden\" name=\"testigo\" value=\"" + H::escapaHtml(testigo) + "\">";
}

std::string marcaDeVersion(const std::string& version) {
    if (version.empty()) {
        return "-";
    }
    const int cmp = B::agentversion::compara(version, B::agentversion::laEsperada());
    if (cmp < 0) {
        return version + " *";
    }
    if (cmp > 0) {
        return version + " +";
    }
    return version;
}

std::string paginaConexiones(const std::vector<B::ConnectionProfile>& perfiles,
                             const std::vector<std::string>& versiones,
                             const std::string& testigo) {
    std::vector<std::vector<std::string>> filas;
    for (std::size_t i = 0; i < perfiles.size(); ++i) {
        const B::ConnectionProfile& p = perfiles[i];
        const std::string id = p.id.empty() ? p.name : p.id;
        filas.push_back({enlace("/c/" + id, id),
                         H::escapaHtml(p.name),
                         H::escapaHtml(p.connType),
                         H::escapaHtml(p.osType),
                         H::escapaHtml(p.host),
                         H::escapaHtml(p.username),
                         H::escapaHtml(i < versiones.size() ? versiones[i] : std::string("-"))});
    }
    return envuelve("Conexiones", "ZFSMgr",
                    tabla({"ID", "Nombre", "Tipo", "Sistema", "Host", "Usuario", "Daemon"}, filas),
                    testigo);
}

std::string paginaPools(const std::string& conn, const std::vector<L::Pool>& pools,
                        const std::string& testigo) {
    std::vector<std::vector<std::string>> filas;
    for (const L::Pool& p : pools) {
        filas.push_back({enlace("/c/" + conn + "/" + p.nombre, p.nombre),
                         H::escapaHtml(p.salud.empty() ? p.estado : p.salud),
                         H::escapaHtml(p.tamano),
                         H::escapaHtml(p.libre),
                         H::escapaHtml(p.uso)});
    }
    std::string cuerpo = tabla({"Pool", "Salud", "Tamaño", "Libre", "Uso"}, filas);
    cuerpo += "<h2>De esta máquina</h2><p>";
    cuerpo += enlace("/c/" + conn + "?registro=1", "Registro del daemon") + " · ";
    cuerpo += enlace("/c/" + conn + "?trabajos=1", "Trabajos en curso") + " · ";
    cuerpo += enlace("/c/" + conn + "?programacion=1", "Instantáneas programadas");
    cuerpo += "</p>";
    return envuelve(conn, enlace("/", "ZFSMgr"), cuerpo, testigo);
}

// ── El árbol de la izquierda ─────────────────────────────────────────────────
//
// Dos paneles, como en la interfaz de Qt: a la izquierda la ESTRUCTURA y a la derecha lo
// que se sabe del nodo elegido más lo que se le puede hacer.
//
// La razón de partirlo en dos no es estética. Con un solo panel, cada nodo tenía que
// llevar encima su propio menú —un «acciones» colgando de cada dataset— y con treinta
// datasets eso son treinta menús pidiendo sitio en la misma columna. Separando, el árbol
// vuelve a ser una lista de nombres y las acciones tienen una superficie entera para
// ellas: es exactamente el reparto de la ventana de Qt.
//
// **Todo el árbol sale de UNA sola llamada.** `--dump-zfs-list-all` ya es recursivo, así
// que los datasets, sus instantáneas y los tamaños vienen juntos. Los nodos que necesitan
// otra pregunta —propiedades, permisos, contenido, estado del pool— NO la hacen al pintar
// el árbol: son enlaces, y solo preguntan cuando uno los pulsa. Un árbol que sondeara cada
// rama al dibujarse tardaría lo que tarde la máquina más lenta multiplicado por el número
// de datasets.

// Qué se está mirando en el panel derecho. Un TIPO y no una cadena: así el compilador
// obliga a contestar en los dos sitios que reparten por vista —el título y el contenido—
// cuando se añada una, que es como se evita la vista que existe en el menú y no se pinta.
enum class Vista {
    Resumen,
    Props,
    Permisos,
    Contenido,
    Estado,
    PropsPool,
    Capacidades,
    Historial,
    Programacion,
};

const char* claveDeVista(Vista v) {
    switch (v) {
        case Vista::Resumen:      return "resumen";
        case Vista::Props:        return "props";
        case Vista::Permisos:     return "permisos";
        case Vista::Contenido:    return "contenido";
        case Vista::Estado:       return "estado";
        case Vista::PropsPool:    return "poolprops";
        case Vista::Capacidades:  return "caps";
        case Vista::Historial:    return "historial";
        case Vista::Programacion: return "gsa";
    }
    return "resumen";
}

std::string tituloDeVista(Vista v, const std::string& objeto) {
    switch (v) {
        case Vista::Resumen:      return objeto;
        case Vista::Props:        return "Propiedades de " + objeto;
        case Vista::Permisos:     return "Permisos de " + objeto;
        case Vista::Contenido:    return "Contenido de " + objeto;
        case Vista::Estado:       return "Estado de " + objeto;
        case Vista::PropsPool:    return "Propiedades del pool " + objeto;
        case Vista::Capacidades:  return "Capacidades de " + objeto;
        case Vista::Historial:    return "Historial de " + objeto;
        case Vista::Programacion: return "Instantáneas programadas de " + objeto;
    }
    return objeto;
}

Vista vistaDesde(const std::string& s) {
    static const Vista todas[] = {
        Vista::Resumen,  Vista::Props,       Vista::Permisos,   Vista::Contenido, Vista::Estado,
        Vista::PropsPool, Vista::Capacidades, Vista::Historial, Vista::Programacion,
    };
    for (const Vista v : todas) {
        if (s == claveDeVista(v)) {
            return v;
        }
    }
    return Vista::Resumen;
}

// El listado, ordenado por parentesco. Se construye una vez por petición y lo consultan
// tanto el árbol como el panel.
struct Arbol {
    std::map<std::string, std::vector<L::Entrada>> hijos;
    std::map<std::string, std::vector<L::Entrada>> instantaneas;
    std::map<std::string, L::Entrada> porNombre;
};

Arbol construyeArbol(const std::vector<L::Entrada>& entradas, const std::string& raiz) {
    Arbol a;
    for (const L::Entrada& e : entradas) {
        if (e.esInstantanea()) {
            a.instantaneas[e.nombre.substr(0, e.nombre.find('@'))].push_back(e);
            continue;
        }
        a.porNombre[e.nombre] = e;
        if (e.nombre == raiz) {
            continue;
        }
        a.hijos[e.nombre.substr(0, e.nombre.find_last_of('/'))].push_back(e);
    }
    return a;
}

// La URL de un nodo. La selección viaja en la CONSULTA y no en la ruta porque la ruta ya
// dice de qué pool es el árbol: así una recarga reconstruye el mismo árbol con el mismo
// nodo elegido, y el enlace se puede guardar en marcadores.
std::string urlDe(const std::string& conn, const std::string& raiz, const std::string& sel,
                  Vista v) {
    return "/c/" + H::haciaUrl(conn) + "/" + H::haciaUrl(raiz) + "?sel=" + H::haciaUrl(sel)
           + "&v=" + claveDeVista(v);
}

std::string enlaceDeNodo(const std::string& destino, const std::string& texto, bool elegido) {
    return "<a class=\"" + std::string(elegido ? "sel" : "nodo") + "\" href=\""
           + H::escapaHtml(destino) + "\">" + H::escapaHtml(texto) + "</a>";
}

// Un nodo sin hijos: los «Propiedades», «Permisos», «Contenido» que en Qt cuelgan de cada
// dataset. Aquí son enlaces y no ramas que se abren, porque su contenido no está en el
// listado y pedirlo por adelantado costaría una llamada por dataset.
std::string hojaDelArbol(const std::string& texto, const std::string& url, bool elegida) {
    return "<div class=\"hoja\">" + enlaceDeNodo(url, texto, elegida) + "</div>";
}

// ¿Es `posible` el nodo elegido o un antepasado suyo? Sirve para decidir qué ramas salen
// abiertas: las que llevan hasta la selección, y ninguna más.
bool llevaHasta(const std::string& posible, const std::string& sel) {
    if (sel == posible) {
        return true;
    }
    return sel.size() > posible.size() && sel.compare(0, posible.size(), posible) == 0
           && (sel[posible.size()] == '/' || sel[posible.size()] == '@');
}

// Los grupos de instantáneas, como en Qt: por clase del planificador —horarias, diarias,
// semanales…— y las de mano aparte. El reparto no se hace aquí: lo hace la capa base, que
// es la misma que usa el planificador para decidir qué poda.
std::string etiquetaDeClase(const std::string& clase) {
    if (clase.empty())          { return "Manuales"; }
    if (clase == "hourly")      { return "Horarias"; }
    if (clase == "daily")       { return "Diarias"; }
    if (clase == "weekly")      { return "Semanales"; }
    if (clase == "monthly")     { return "Mensuales"; }
    if (clase == "yearly")      { return "Anuales"; }
    return clase;
}

std::string ramaDeInstantaneas(const std::string& conn, const std::string& raiz,
                               const std::string& ds, const std::vector<L::Entrada>& snaps,
                               const std::string& sel, Vista vista) {
    if (snaps.empty()) {
        return {};
    }
    std::vector<std::string> cortos;
    std::map<std::string, const L::Entrada*> porCorto;
    for (const L::Entrada& s : snaps) {
        const std::string corto = s.nombre.substr(s.nombre.find('@') + 1);
        cortos.push_back(corto);
        porCorto[corto] = &s;
    }
    const bool dentro = llevaHasta(ds, sel) && sel.find('@') != std::string::npos;
    std::string h = std::string("<details") + (dentro ? " open" : "") + "><summary>";
    h += "<span class=\"seccion\">@ instantáneas</span> <span class=\"tenue\">("
         + std::to_string(snaps.size()) + ")</span></summary><div class=\"rama\">";
    for (const auto& grupo : B::gsa::agrupaInstantaneas(cortos)) {
        const bool grupoDentro = std::any_of(
            grupo.second.begin(), grupo.second.end(),
            [&](const std::string& c) { return sel == ds + "@" + c; });
        h += std::string("<details") + (grupoDentro ? " open" : "") + "><summary>";
        h += "<span class=\"seccion\">" + H::escapaHtml(etiquetaDeClase(grupo.first))
             + "</span> <span class=\"tenue\">(" + std::to_string(grupo.second.size())
             + ")</span></summary><div class=\"rama\">";
        for (const std::string& corto : grupo.second) {
            const std::string entera = ds + "@" + corto;
            const auto it = porCorto.find(corto);
            h += "<div class=\"hoja\">"
                 + enlaceDeNodo(urlDe(conn, raiz, entera, Vista::Resumen), corto,
                                sel == entera && vista == Vista::Resumen);
            if (it != porCorto.end()) {
                h += " <span class=\"tenue\">" + H::escapaHtml(bytesLegibles(it->second->usado))
                     + "</span>";
            }
            h += "</div>";
        }
        h += "</div></details>";
    }
    h += "</div></details>";
    return h;
}

std::string ramaDelArbol(const std::string& conn, const std::string& raiz, const std::string& nodo,
                         const Arbol& arbol, const std::string& sel, Vista vista, int profundidad) {
    const auto itE = arbol.porNombre.find(nodo);
    const std::string corto =
        nodo.find('/') == std::string::npos ? nodo : nodo.substr(nodo.find_last_of('/') + 1);
    const bool montado = itE != arbol.porNombre.end() && itE->second.montado == "yes";
    const std::string punto = itE != arbol.porNombre.end() ? itE->second.puntoMontaje : std::string();

    // Abierto si es de los dos primeros niveles o si por ahí se llega al nodo elegido. Lo
    // segundo es lo que hace que una recarga deje el árbol como estaba: sin estado que
    // guardar, se DEDUCE de la selección.
    const bool abierto = profundidad < 2 || llevaHasta(nodo, sel);
    std::string h = std::string("<details") + (abierto ? " open" : "") + "><summary>";
    h += enlaceDeNodo(urlDe(conn, raiz, nodo, Vista::Resumen), corto,
                      sel == nodo && vista == Vista::Resumen);
    if (itE != arbol.porNombre.end()) {
        h += " <span class=\"tenue\">" + H::escapaHtml(bytesLegibles(itE->second.usado));
        if (!montado) {
            h += " · sin montar";
        }
        h += "</span>";
    }
    h += "</summary><div class=\"rama\">";

    // Los nodos fijos de cada dataset, en el mismo orden que en la interfaz de Qt.
    // Se resalta el que se está mirando —dataset Y vista—, no solo el dataset: pulsar
    // «Propiedades» en Qt marca ese nodo hijo, no su padre.
    const bool esteNodo = (sel == nodo);
    h += hojaDelArbol("Propiedades", urlDe(conn, raiz, nodo, Vista::Props),
                      esteNodo && vista == Vista::Props);
    h += hojaDelArbol("Permisos", urlDe(conn, raiz, nodo, Vista::Permisos),
                      esteNodo && vista == Vista::Permisos);
    // «Contenido» solo si está montado y con punto de montaje de verdad: sin eso no hay
    // nada que listar, y un nodo que siempre falla al pulsarlo es peor que no tenerlo.
    if (montado && !punto.empty() && punto != "none" && punto != "-") {
        h += hojaDelArbol("Contenido", urlDe(conn, raiz, nodo, Vista::Contenido),
                          esteNodo && vista == Vista::Contenido);
    }
    h += hojaDelArbol("Programación", urlDe(conn, raiz, nodo, Vista::Programacion),
                      esteNodo && vista == Vista::Programacion);

    const auto itS = arbol.instantaneas.find(nodo);
    if (itS != arbol.instantaneas.end()) {
        h += ramaDeInstantaneas(conn, raiz, nodo, itS->second, sel, vista);
    }
    const auto itH = arbol.hijos.find(nodo);
    if (itH != arbol.hijos.end()) {
        for (const L::Entrada& hijo : itH->second) {
            h += ramaDelArbol(conn, raiz, hijo.nombre, arbol, sel, vista, profundidad + 1);
        }
    }
    h += "</div></details>";
    return h;
}

// El panel izquierdo entero: el pool con sus nodos propios y debajo el árbol de datasets.
std::string panelArbol(const std::string& conn, const std::string& raiz, const Arbol& arbol,
                       const std::string& sel, Vista vista) {
    std::string h = "<div class=\"arbol\">";
    // Los nodos del POOL solo tienen sentido si la raíz del árbol es el pool. Entrando por
    // `/c/maquina/pool/hijo` la raíz es un dataset, y un «Historial del pool» ahí colgaría
    // de algo que no es un pool.
    if (raiz.find('/') == std::string::npos) {
        h += "<div class=\"seccion tit\">Pool</div>";
        h += hojaDelArbol("Estado y dispositivos", urlDe(conn, raiz, raiz, Vista::Estado),
                          vista == Vista::Estado);
        h += hojaDelArbol("Propiedades del pool", urlDe(conn, raiz, raiz, Vista::PropsPool),
                          vista == Vista::PropsPool);
        h += hojaDelArbol("Capacidades", urlDe(conn, raiz, raiz, Vista::Capacidades),
                          vista == Vista::Capacidades);
        h += hojaDelArbol("Historial", urlDe(conn, raiz, raiz, Vista::Historial),
                          vista == Vista::Historial);
        h += "<div class=\"seccion tit\">Datasets</div>";
    }
    h += ramaDelArbol(conn, raiz, raiz, arbol, sel, vista, 0);
    h += "</div>";
    return h;
}

// ── El panel de la derecha ───────────────────────────────────────────────────
//
// Lo que en Qt son las columnas C1…Cx y el menú del botón derecho, juntos: arriba la ficha
// del nodo y debajo lo que se le puede hacer.
//
// Que las acciones vivan aquí y no colgando de cada nodo del árbol no es solo cuestión de
// sitio. Un menú contextual en Qt se abre sobre el nodo que se ha pulsado y por eso puede
// no decir sobre QUÉ actúa; una página web no tiene ese contexto, así que cada formulario
// lleva su objeto dentro y el panel lo nombra en la cabecera. El que mira sabe siempre
// sobre qué va a actuar el botón que tiene delante, que es más de lo que da un menú
// contextual.

// La ficha del nodo: una fila por dato. Es la lectura vertical de las columnas de Qt —el
// árbol de un navegador no puede tener veinte columnas y seguir siendo legible en la mitad
// izquierda de la pantalla, así que las columnas bajan aquí y se leen enteras.
std::string fichaDeDatos(const std::vector<std::pair<std::string, std::string>>& datos) {
    std::string h = "<table class=\"ficha\"><tbody>";
    for (const auto& d : datos) {
        h += "<tr><th>" + H::escapaHtml(d.first) + "</th><td>" + H::escapaHtml(d.second)
             + "</td></tr>";
    }
    h += "</tbody></table>";
    return h;
}

// Un botón que hace algo, con su objeto dentro. `campos` son los `<input>` extra —el
// nombre de una instantánea, el del dataset nuevo— que la acción necesite.
std::string boton(const std::string& conn, const std::string& objeto, const std::string& raiz,
                  const std::string& que, const std::string& etiqueta, const std::string& testigo,
                  const std::string& campos = std::string(), bool peligro = false) {
    std::string h = "<form class=\"enlinea\" method=\"post\" action=\"/accion\">";
    h += campoTestigo(testigo);
    h += "<input type=\"hidden\" name=\"c\" value=\"" + H::escapaHtml(conn) + "\">";
    h += "<input type=\"hidden\" name=\"o\" value=\"" + H::escapaHtml(objeto) + "\">";
    // A dónde volver después. Sin esto, cada acción devolvía a la raíz del pool y había
    // que rehacer el camino hasta el dataset sobre el que se acababa de actuar.
    h += "<input type=\"hidden\" name=\"raiz\" value=\"" + H::escapaHtml(raiz) + "\">";
    h += "<input type=\"hidden\" name=\"que\" value=\"" + H::escapaHtml(que) + "\">";
    h += campos;
    h += "<button type=\"submit\"" + std::string(peligro ? " class=\"peligro\"" : "") + ">"
         + H::escapaHtml(etiqueta) + "</button></form>";
    return h;
}

std::string campo(const std::string& nombre, const std::string& hueco, bool obligatorio = true) {
    return "<input name=\"" + H::escapaHtml(nombre) + "\" placeholder=\"" + H::escapaHtml(hueco)
           + "\"" + (obligatorio ? " required" : "") + ">";
}

std::string grupoDeAcciones(const std::string& titulo, const std::string& cuerpo) {
    return "<div class=\"grupo\"><div class=\"grupotit\">" + H::escapaHtml(titulo) + "</div>"
           + cuerpo + "</div>";
}

// El menú contextual de un DATASET, con los mismos submenús que en Qt: «Dataset» para el
// estado del propio dataset y «Acciones» para lo que toca sus DATOS.
std::string accionesDeDataset(const std::string& conn, const std::string& raiz,
                              const std::string& ds, const L::Entrada* e,
                              const std::string& testigo) {
    const bool montado = e != nullptr && e->montado == "yes";
    const bool cifrado = e != nullptr && !e->cifrado.empty() && e->cifrado != "off"
                         && e->cifrado != "-";
    std::string h = "<h2>Acciones</h2>";

    std::string ds1;
    ds1 += boton(conn, ds, raiz, "crear-dataset", "Crear", testigo,
                 "<label>hijo <input name=\"nombre\" placeholder=\"nombre\" required></label> ");
    ds1 += boton(conn, ds, raiz, "renombrar", "Renombrar", testigo,
                 "<label>a <input name=\"nombre\" placeholder=\"pool/otro\" required></label> ");
    // Montar solo si NO está montado y desmontar solo si lo está. Lo que no aplica no sale:
    // un botón que siempre falla enseña menos que su ausencia.
    if (!montado) {
        ds1 += boton(conn, ds, raiz, "montar", "Montar", testigo);
    } else {
        ds1 += boton(conn, ds, raiz, "desmontar", "Desmontar", testigo);
    }
    ds1 += enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(ds)
                      + "&que=borrar-dataset&raiz=" + H::haciaUrl(raiz),
                  "Borrar…");
    h += grupoDeAcciones("Dataset", ds1);

    std::string snap;
    snap += boton(conn, ds, raiz, "crear-instantanea", "Crear instantánea", testigo,
                  campo("nombre", "nombre") + " <label><input type=\"checkbox\" name=\"rec\" "
                  "value=\"1\"> recursiva</label> ");
    h += grupoDeAcciones("Instantáneas", snap);

    if (cifrado) {
        // La frase de cifrado va por un campo de CONTRASEÑA y por POST, nunca en la URL:
        // una URL se queda en el historial del navegador y en el registro de cualquier
        // intermediario. Es la misma regla que impide pasarla por argumento al agente.
        std::string cif;
        cif += boton(conn, ds, raiz, "cargar-clave", "Cargar clave", testigo,
                     "<input type=\"password\" name=\"frase\" placeholder=\"frase\" required> ");
        cif += boton(conn, ds, raiz, "descargar-clave", "Descargar clave", testigo);
        h += grupoDeAcciones("Clave de cifrado", cif);
    }

    // Lo que TODAVÍA no está, dicho en su sitio. Un panel que enseña ocho acciones y calla
    // las otras seis deja creyendo que la web puede hacer lo mismo que la interfaz; y estas
    // seis no son un olvido, es que necesitan dos extremos —un origen marcado y un
    // destino—, que es un estado entre páginas que aquí no existe todavía.
    h += "<div class=\"pendiente\">Copiar, Mover, Clonar, Sincronizar, Nivelar y Diff piden "
         "dos extremos y aún no están en la web; Desglosar, Ensamblar, Desde Dir y Hacia Dir "
         "tampoco. Se hacen desde la interfaz o desde el intérprete.</div>";
    return h;
}

// El menú de una INSTANTÁNEA. En Qt son cinco entradas; aquí están las tres que no piden
// un segundo extremo.
std::string accionesDeInstantanea(const std::string& conn, const std::string& raiz,
                                  const std::string& snap, const std::string& testigo) {
    std::string h = "<h2>Acciones</h2>";
    std::string g;
    g += boton(conn, snap, raiz, "clonar", "Clonar", testigo,
               "<label>en <input name=\"nombre\" placeholder=\"pool/clon\" required></label> ");
    g += enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(snap)
                    + "&que=rollback&raiz=" + H::haciaUrl(raiz),
                "Rollback…");
    g += " · ";
    g += enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(snap)
                    + "&que=borrar-instantanea&raiz=" + H::haciaUrl(raiz),
                "Borrar…");
    h += grupoDeAcciones("Instantánea", g);
    h += "<div class=\"pendiente\">«Nuevo Hold» y «Release» no están: el daemon no tiene "
         "todavía un verbo para leer los holds, y la interfaz los lee por shell — que es "
         "justo lo que este servidor no hace.</div>";
    return h;
}

// El menú de un POOL: las acciones de mantenimiento que en Qt cuelgan de «Gestión».
std::string accionesDePool(const std::string& conn, const std::string& pool,
                           const std::string& testigo) {
    std::string h = "<h2>Acciones</h2>";
    std::string g;
    g += boton(conn, pool, pool, "pool-scrub", "Scrub", testigo);
    g += boton(conn, pool, pool, "pool-scrub-parar", "Parar scrub", testigo);
    g += boton(conn, pool, pool, "pool-trim", "Trim", testigo);
    g += boton(conn, pool, pool, "pool-sync", "Sync", testigo);
    g += boton(conn, pool, pool, "pool-clear", "Clear", testigo);
    h += grupoDeAcciones("Gestión", g);
    h += "<div class=\"pendiente\">Importar, Exportar, Upgrade, Reguid, Initialize y Destroy "
         "no están todavía en la web.</div>";
    return h;
}

// La ficha de un dataset o de una instantánea: lo que ya viene en el listado, sin pedir
// nada más. Las propiedades enteras son otro nodo del árbol precisamente por eso — esto
// sale gratis y aquello cuesta una llamada.
std::string resumenDelNodo(const std::string& objeto, const Arbol& arbol) {
    const auto it = arbol.porNombre.find(objeto);
    const L::Entrada* e = it != arbol.porNombre.end() ? &it->second : nullptr;
    if (e == nullptr && objeto.find('@') != std::string::npos) {
        const auto its = arbol.instantaneas.find(objeto.substr(0, objeto.find('@')));
        if (its != arbol.instantaneas.end()) {
            for (const L::Entrada& s : its->second) {
                if (s.nombre == objeto) {
                    e = &s;
                    break;
                }
            }
        }
    }
    if (e == nullptr) {
        return "<p class=\"vacio\">(ese nodo no está en el listado)</p>";
    }
    std::vector<std::pair<std::string, std::string>> datos = {
        {"Nombre", e->nombre},
        {"GUID", e->guid},
        {"Usado", bytesLegibles(e->usado)},
        {"Referenciado", bytesLegibles(e->referenciado)},
        {"Compresión", e->compresion},
        {"Cifrado", e->cifrado},
        {"Creación", fechaLegible(e->creacion)},
    };
    if (!e->esInstantanea()) {
        datos.push_back({"Montado", e->montado});
        datos.push_back({"Punto de montaje", e->puntoMontaje});
        datos.push_back({"canmount", e->canmount});
        const auto ith = arbol.hijos.find(e->nombre);
        datos.push_back({"Datasets hijos",
                         std::to_string(ith == arbol.hijos.end() ? 0 : ith->second.size())});
        const auto its = arbol.instantaneas.find(e->nombre);
        datos.push_back({"Instantáneas",
                         std::to_string(its == arbol.instantaneas.end() ? 0 : its->second.size())});
    }
    return fichaDeDatos(datos);
}

// Las dos columnas. El árbol va en un panel que se queda quieto al desplazar la derecha:
// perder de vista dónde se está es lo que hace inservible un árbol grande.
std::string envuelveDosPaneles(const std::string& titulo, const std::string& migas,
                               const std::string& izq, const std::string& der,
                               const std::string& testigo) {
    std::string cuerpo = "<div class=\"dos\"><div class=\"izq\">" + izq + "</div>";
    cuerpo += "<div class=\"der\">" + der + "</div></div>";
    return envuelve(titulo, migas, cuerpo, testigo, true);
}

// ── Las vistas del panel derecho ─────────────────────────────────────────────
//
// Cada una devuelve un TROZO, no una página: quien las llama ya tiene el árbol de la
// izquierda montado y solo necesita el contenido del hueco de la derecha.

std::string panelPropiedades(const std::string& conn, const std::string& raiz,
                             const std::string& objeto, const std::vector<L::Propiedad>& props,
                             bool soloCapacidades, const std::string& testigo) {
    std::vector<std::vector<std::string>> filas;
    for (const L::Propiedad& pr : props) {
        // Las «capacidades» de un pool son sus propiedades `feature@…`, no otra consulta:
        // `zpool get all` ya las trae mezcladas con las demás y separarlas es un filtro.
        const bool esCapacidad = B::startsWith(pr.nombre, "feature@");
        if (esCapacidad != soloCapacidades) {
            continue;
        }
        filas.push_back({H::escapaHtml(soloCapacidades ? pr.nombre.substr(8) : pr.nombre),
                         H::escapaHtml(pr.valor), H::escapaHtml(pr.origen)});
    }
    std::string h = tabla({"Propiedad", "Valor", "Origen"}, filas);
    if (!soloCapacidades && objeto.find('@') == std::string::npos) {
        // Cambiar una propiedad: el nombre se teclea a mano de momento. La edición en línea
        // sobre la tabla es de la fase 4; esto ya ejercita el camino entero.
        h += grupoDeAcciones(
            "Cambiar una propiedad",
            boton(conn, objeto, raiz, "set", "Aplicar", testigo,
                  "<label>Propiedad " + campo("prop", "compression") + "</label> <label>Valor "
                      + campo("valor", "lz4") + "</label> "));
    }
    return h;
}

// Texto tal cual —el estado de un pool, sus permisos delegados, su historial—. Se escapa y
// se mete en un <pre>, que es lo que respeta los espacios de una salida alineada a mano.
std::string panelTexto(const std::string& texto) {
    const std::string t = B::trim(texto);
    return t.empty() ? std::string("<p class=\"vacio\">(no hay nada)</p>")
                     : "<pre>" + H::escapaHtml(t) + "</pre>";
}

// El contenido de un dataset montado, de `--dump-dir-list`. No es shell: el daemon recorre
// el directorio él mismo y contesta JSON, y solo si la ruta cae dentro de un punto de
// montaje de ZFS.
std::string panelContenido(const std::string& conn, const std::string& objeto,
                           const std::string& salidaJson) {
    B::json::Value raiz;
    std::string err;
    if (!B::json::parse(salidaJson, raiz, &err)) {
        return "<p class=\"vacio\">respuesta ilegible: " + H::escapaHtml(err) + "</p>";
    }
    std::vector<std::vector<std::string>> filas;
    for (const B::json::Value& e : raiz["entries"].toArray()) {
        const bool dir = e["type"].toString() == "d";
        filas.push_back({H::escapaHtml(e["name"].toString()),
                         dir ? std::string("directorio") : std::string("fichero"),
                         dir ? std::string() : bytesLegibles(std::to_string(e["size"].toInt()))});
    }
    std::sort(filas.begin(), filas.end());
    std::string h = tabla({"Nombre", "Tipo", "Tamaño"}, filas);
    h += "<p>" + enlace("/dav/" + H::haciaUrl(conn) + "/" + H::haciaUrl(objeto) + "/",
                        "Abrir por WebDAV")
         + " <span class=\"tenue\">— para montarlo en el explorador de archivos</span></p>";
    return h;
}

// La programación de instantáneas, tal como la guarda ZFS: una propiedad `org.fc16.gsa:*`
// por línea. Se enseña agrupada por dataset porque la consulta es recursiva.
std::string panelProgramacion(const std::string& salidaTsv) {
    std::vector<std::vector<std::string>> filas;
    for (const std::string& linea : B::split(salidaTsv, "\n", true)) {
        const std::vector<std::string> col = B::split(linea, "\t", false);
        if (col.size() < 4) {
            continue;
        }
        if (B::trim(col[2]) == "-") {
            continue;   // sin poner: no dice nada y llenaría la tabla de guiones
        }
        std::string prop = col[1];
        if (B::startsWith(prop, B::gsa::kPrefijo)) {
            prop = prop.substr(std::string(B::gsa::kPrefijo).size());
        }
        filas.push_back({H::escapaHtml(col[0]), H::escapaHtml(prop), H::escapaHtml(col[2]),
                         H::escapaHtml(col[3])});
    }
    if (filas.empty()) {
        return "<p class=\"vacio\">(no hay ninguna programación puesta aquí)</p>";
    }
    return tabla({"Dataset", "Ajuste", "Valor", "Origen"}, filas);
}

// La página de confirmación de algo que destruye.
//
// Existe porque un botón que borra sin preguntar, en una página que se puede recargar, es
// una forma de perder datos por un clic de más. Dice EXACTAMENTE qué se va a hacer y sobre
// qué, igual que el intérprete antes de una orden destructiva.
std::string paginaConfirmar(const std::string& conn, const std::string& objeto,
                            const std::string& que, const std::string& raiz,
                            const std::string& testigo) {
    std::string texto;
    if (que == "borrar-instantanea") {
        texto = "Se va a DESTRUIR la instantánea «" + objeto + "» en «" + conn
                + "». Lo que hubiera en ella se pierde y no hay vuelta atrás.";
    } else if (que == "borrar-dataset") {
        texto = "Se va a DESTRUIR el dataset «" + objeto + "» en «" + conn
                + "» con todo lo que contenga. No hay vuelta atrás.";
    } else if (que == "rollback") {
        texto = "Se va a volver el dataset al estado de «" + objeto + "» en «" + conn
                + "». Todo lo escrito DESPUÉS de esa instantánea se pierde.";
    } else {
        texto = "Acción desconocida.";
    }
    std::string cuerpo = "<p>" + H::escapaHtml(texto) + "</p>";
    if (que == "borrar-instantanea" || que == "borrar-dataset" || que == "rollback") {
        // El alcance se elige AQUÍ y no en el botón del panel, porque es parte de lo que
        // hay que entender antes de decir que sí: un dataset con hijos o instantáneas no se
        // destruye sin más, y ZFS se niega —con razón— hasta que alguien dice que también
        // se lleva lo de dentro. Sin esta casilla, el botón daba un error de ZFS sin salida.
        std::string extra;
        if (que == "borrar-dataset") {
            extra = "<label><input type=\"checkbox\" name=\"alcance\" value=\"r\"> "
                    "y todo lo que contenga (hijos e instantáneas)</label> ";
        } else if (que == "rollback") {
            extra = "<label><input type=\"checkbox\" name=\"alcance\" value=\"r\"> "
                    "destruyendo las instantáneas posteriores</label> ";
        }
        cuerpo += boton(conn, objeto, raiz, que,
                        que == "rollback" ? "Sí, volver atrás" : "Sí, destruirlo", testigo,
                        extra, true);
    }
    const std::string padre = objeto.find('@') != std::string::npos
                                  ? objeto.substr(0, objeto.find('@'))
                                  : objeto;
    cuerpo += "<p>" + enlace(urlDe(conn, raiz.empty() ? padre : raiz, padre, Vista::Resumen),
                             "No, volver")
              + "</p>";
    return envuelve("Confirmar", enlace("/", "ZFSMgr"), cuerpo, testigo);
}

// Una página que solo enseña texto tal cual: el registro del daemon, los permisos
// delegados. Se escapa y se mete en un <pre>, que es lo que respeta los espacios.
std::string paginaTexto(const std::string& titulo, const std::string& migas,
                        const std::string& texto, const std::string& testigo) {
    const std::string t = B::trim(texto);
    const std::string cuerpo = t.empty() ? std::string("<p class=\"vacio\">(no hay nada)</p>")
                                         : "<pre>" + H::escapaHtml(t) + "</pre>";
    return envuelve(titulo, migas, cuerpo, testigo);
}

std::string paginaError(const std::string& que, const std::string& testigo) {
    return envuelve("No se pudo", enlace("/", "ZFSMgr"),
                    "<p>" + H::escapaHtml(que) + "</p>", testigo);
}

}  // namespace

int main(int argc, char** argv) {
    Opciones op;
    op.dirConfig = dirConfigPorOmision();
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            uso();
            return 0;
        }
        if (a == "--config-dir" && i + 1 < argc) { op.dirConfig = argv[++i]; continue; }
        if (a == "--bind" && i + 1 < argc) { op.bind = argv[++i]; continue; }
        if (a == "--port" && i + 1 < argc) { op.puerto = std::atoi(argv[++i]); continue; }
        if (a == "--password-fd" && i + 1 < argc) { op.passwordFd = std::atoi(argv[++i]); continue; }
        std::fprintf(stderr, "zfsmgr-web: opción desconocida: %s\n", a.c_str());
        uso();
        return 2;
    }

    // La contraseña maestra: al arrancar, como hace la interfaz, y viva en memoria mientras
    // el proceso lo esté. Ver la decisión 2 del diseño.
    std::string maestra;
    if (ST::hayAlgoCifrado(op.dirConfig)) {
        std::string err;
        if (op.passwordFd >= 0) {
            if (!zfsmgr::cli::leerSecretoDeDescriptor(op.passwordFd, maestra, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return 2;
            }
        } else if (!zfsmgr::cli::preguntarSecretoPorTerminal("Contraseña maestra: ", maestra, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 2;
        }
        ST::Aviso aviso;
        if (!ST::maestraAbreTodo(op.dirConfig, maestra, aviso)) {
            std::fprintf(stderr, "la contraseña maestra no abre la configuración: %s\n",
                         ST::etiquetaDe(aviso).c_str());
            return 2;
        }
    }

    // El material TLS del servidor, en el directorio del usuario y emitido por nosotros: no
    // hace falta `openssl` en el PATH, que en Windows no está.
    const std::string dirWeb = op.dirConfig + "/web";
    std::error_code ec;
    std::filesystem::create_directories(dirWeb, ec);
    const std::string rutaCert = dirWeb + "/server.crt";
    const std::string rutaClave = dirWeb + "/server.key";
    if (!std::filesystem::exists(rutaCert, ec) || !std::filesystem::exists(rutaClave, ec)) {
        std::string err;
        if (!B::tlsserver::escribeParAutofirmado(rutaCert, rutaClave, "zfsmgr-web", true,
                                                 "DNS:localhost,IP:127.0.0.1", err)) {
            std::fprintf(stderr, "no se pudo emitir el certificado: %s\n", err.c_str());
            return 1;
        }
        std::fprintf(stderr, "certificado emitido en %s\n", rutaCert.c_str());
    }

    // Lo que ya se preguntó a cada máquina, para no volver a esperar por ella en cada
    // recarga. Vive lo que vive el proceso.
    std::map<std::string, std::string> versionPorConexion;

    zfsmgr::web::Sesion sesion;
    sesion.abre();
    if (!sesion.abierta()) {
        std::fprintf(stderr, "no hay fuente de azar: no se puede abrir una sesión segura\n");
        return 1;
    }

    // La sesión de transporte: la misma que monta el intérprete, con su proveedor de
    // credenciales y su persistencia de TLS. No se duplica el cableado.
    auto sesionZfs = zfsmgr::cli::crearSesion(op.dirConfig, maestra, /*verboso=*/false);
    if (!sesionZfs) {
        std::fprintf(stderr, "no se pudo montar la sesión de transporte\n");
        return 1;
    }

    std::signal(SIGINT, alSenal);
    std::signal(SIGTERM, alSenal);

    if (op.bind != "127.0.0.1") {
        std::fprintf(stderr,
                     "AVISO: escuchando en %s, no en 127.0.0.1. Esto expone tus máquinas a la\n"
                     "red; lo normal es dejarlo local y llegar por un túnel SSH.\n",
                     op.bind.c_str());
    }
    std::fprintf(stderr, "zfsmgr-web escuchando en https://%s:%d/\n", op.bind.c_str(), op.puerto);
    std::fprintf(stderr, "abra esta URL, que lleva la sesión:\n  https://%s:%d/?s=%s\n",
                 op.bind.c_str(), op.puerto, sesion.id().c_str());

    const auto atiende = [&](const std::string& crudo, std::string& respuesta) {
        const H::Peticion p = H::analiza(crudo);
        H::Respuesta r;
        if (!p.valida) {
            r.codigo = 400;
            r.tipo = "text/plain; charset=utf-8";
            r.cuerpo = "peticion no valida: " + p.porQueNoVale + "\n";
            respuesta = H::componer(r);
            return true;
        }

        // La hoja de estilo. Va antes de exigir sesión: es lo único público, y sin ella la
        // página de «sin sesión» se vería igual de cruda que lo demás.
        if (p.ruta == "/estilo.css") {
            r.tipo = "text/css; charset=utf-8";
            r.cuerpo = kEstiloCss;
            respuesta = H::componer(r);
            return true;
        }

        // La sesión llega por cookie. La primera vez se admite por la URL que se imprimió en
        // el terminal —es la forma de que el navegador la reciba sin pedir una contraseña
        // más— y a partir de ahí ya va en la cookie.
        const bool porCookie = sesion.cookieVale(p.cookie("zfsmgr_sesion"));
        const bool porUrl = p.consulta == ("s=" + sesion.id());
        if (!porCookie && !porUrl) {
            r.codigo = 403;
            r.tipo = "text/plain; charset=utf-8";
            r.cuerpo = "sin sesion\n";
            respuesta = H::componer(r);
            return true;
        }
        if (porUrl && !porCookie) {
            r.cabecerasExtra.push_back(sesion.cabeceraCookie());
        }

        if (p.metodo == "POST" && p.ruta == "/salir") {
            // Una mutación, aunque sea la de apagar: exige el testigo. Es el mecanismo que
            // usará la fase 2 para todo lo demás.
            if (!sesion.testigoVale(p.campo("testigo"))) {
                r.codigo = 403;
                r.tipo = "text/plain; charset=utf-8";
                r.cuerpo = "testigo no valido\n";
                respuesta = H::componer(r);
                return true;
            }
            g_vivo.store(false);
            r.cuerpo = "<!doctype html><html><body><p>Servidor detenido.</p></body></html>";
            respuesta = H::componer(r);
            return true;
        }
        // --- WebDAV. La misma escucha y la misma sesión: lo que monta el explorador de
        // archivos es este mismo servidor, no otro.
        //
        // Solo LECTURA: OPTIONS, PROPFIND, GET y HEAD. Sin LOCK ni PUT — montar esto en
        // escritura es otra conversación.
        if (p.ruta == "/dav" || p.ruta.rfind("/dav/", 0) == 0) {
            if (p.metodo == "OPTIONS") {
                r.codigo = 200;
                r.tipo = "text/plain; charset=utf-8";
                r.cuerpo.clear();
                r.cabecerasExtra.push_back("DAV: 1");
                r.cabecerasExtra.push_back("Allow: OPTIONS, GET, HEAD, PROPFIND");
                // Explorer no monta sin esto: se lo toma como «aquí no hay WebDAV».
                r.cabecerasExtra.push_back("MS-Author-Via: DAV");
                respuesta = H::componer(r);
                return true;
            }
            if (p.metodo != "PROPFIND" && p.metodo != "GET" && p.metodo != "HEAD") {
                r.codigo = 405;
                r.tipo = "text/plain; charset=utf-8";
                r.cuerpo = "solo lectura\n";
                respuesta = H::componer(r);
                return true;
            }

            std::string ruta = p.ruta.substr(4);   // lo que va tras «/dav»
            while (!ruta.empty() && ruta.front() == '/') {
                ruta.erase(ruta.begin());
            }
            while (!ruta.empty() && ruta.back() == '/') {
                ruta.pop_back();
            }
            const auto conns = zfsmgr::cli::cargarConexiones(op.dirConfig, maestra);
            std::vector<D::Recurso> recursos;
            // El directorio del sistema de ficheros que corresponde a esta URL, si lo hay.
            // Se guarda para poder servir un fichero de dentro sin volver a resolverlo.
            std::string rutaBaseFs;
            const B::ConnectionProfile* perfilPedido = nullptr;
            // ¿Se pudo LISTAR la ruta como directorio? Distingue «un directorio vacío» de
            // «esto no existe», que sin esto se contestaban igual — con un 200 y la palabra
            // «coleccion», también para un fichero que ya no está.
            bool esDirectorioDeVerdad = false;

            if (ruta.empty()) {
                recursos.push_back({"/dav/", "ZFSMgr", true, 0});
                for (const B::ConnectionProfile& perfilC : conns.perfiles) {
                    const std::string id = perfilC.id.empty() ? perfilC.name : perfilC.id;
                    recursos.push_back({"/dav/" + id + "/", id, true, 0});
                }
            } else {
                const std::size_t barra = ruta.find('/');
                const std::string conn = barra == std::string::npos ? ruta : ruta.substr(0, barra);
                const std::string objeto = barra == std::string::npos ? std::string()
                                                                     : ruta.substr(barra + 1);
                const B::ConnectionProfile* perfilD = zfsmgr::cli::buscarConexion(conns, conn);
                perfilPedido = perfilD;
                if (!perfilD) {
                    r.codigo = 404;
                    r.tipo = "text/plain; charset=utf-8";
                    r.cuerpo = "no existe\n";
                    respuesta = H::componer(r);
                    return true;
                }
                std::string salidaD;
                std::string errD;
                int rcD = -1;
                std::string motivoD;
                // ¿Hasta dónde es DATASET y desde dónde es ruta dentro de él?
                //
                // No se puede saber mirando la URL: «sback/dockvols/axigen» puede ser un
                // dataset o un directorio dentro de «sback/dockvols». Se prueba el camino
                // entero y se va recortando por la derecha hasta que uno responde. Es una
                // llamada por nivel, y los niveles de un dataset son pocos.
                std::string dataset = objeto;
                std::string dentro;
                bool encontrado = objeto.empty();
                while (!encontrado) {
                    if (zfsmgr::cli::ejecutarAgente(*sesionZfs, *perfilD,
                                                    {"--dump-zfs-list-all", dataset}, salidaD,
                                                    errD, rcD, &motivoD, 30000)
                        && rcD == 0) {
                        encontrado = true;
                        break;
                    }
                    const std::size_t ultima = dataset.find_last_of('/');
                    if (ultima == std::string::npos) {
                        break;
                    }
                    dentro = dataset.substr(ultima) + dentro;
                    dataset = dataset.substr(0, ultima);
                }
                const std::vector<std::string> verboD =
                    objeto.empty() ? std::vector<std::string>{"--dump-zpool-list"}
                                   : std::vector<std::string>{"--dump-zfs-list-all", dataset};
                if (!encontrado
                    || (objeto.empty()
                        && (!zfsmgr::cli::ejecutarAgente(*sesionZfs, *perfilD, verboD, salidaD,
                                                         errD, rcD, &motivoD, 30000)
                            || rcD != 0))) {
                    r.codigo = 502;
                    r.tipo = "text/plain; charset=utf-8";
                    r.cuerpo = "no se pudo hablar con la maquina\n";
                    respuesta = H::componer(r);
                    return true;
                }
                recursos.push_back({"/dav/" + ruta + "/", objeto.empty() ? conn : objeto, true, 0});
                if (objeto.empty()) {
                    std::vector<L::Pool> pools;
                    std::string errA;
                    L::pools(salidaD, pools, errA);
                    for (const L::Pool& po : pools) {
                        recursos.push_back({"/dav/" + conn + "/" + po.nombre + "/", po.nombre, true, 0});
                    }
                } else {
                    // Los hijos DIRECTOS: `--dump-zfs-list-all` es recursivo, y meter los
                    // nietos aquí haría que el explorador enseñara el árbol entero aplanado.
                    std::string puntoMontaje;
                    for (const L::Entrada& e : L::entradas(salidaD)) {
                        if (e.nombre == dataset) {
                            if (e.montado == "yes") {
                                puntoMontaje = e.puntoMontaje;
                            }
                            continue;
                        }
                        // Los sub-datasets solo cuelgan del propio dataset, no de un
                        // directorio de dentro.
                        if (e.esInstantanea() || !dentro.empty()) {
                            continue;
                        }
                        const std::string resto = e.nombre.substr(dataset.size() + 1);
                        if (resto.find('/') != std::string::npos) {
                            continue;
                        }
                        recursos.push_back({"/dav/" + conn + "/" + e.nombre + "/", resto, true, 0});
                    }
                    // Y los FICHEROS, si el dataset está montado. Por verbo tipado: la ruta
                    // viene de un navegador y no puede acabar dentro de una cadena de shell.
                    if (!puntoMontaje.empty() && puntoMontaje != "-") {
                        const std::string rutaFs = puntoMontaje + dentro;
                        rutaBaseFs = rutaFs;
                        std::string salidaF;
                        std::string errF;
                        int rcF = -1;
                        std::string motivoF;
                        if (zfsmgr::cli::ejecutarAgente(*sesionZfs, *perfilD,
                                                        {"--dump-dir-list", rutaFs}, salidaF, errF,
                                                        rcF, &motivoF, 30000)
                            && rcF == 0) {
                            B::json::Value raizF;
                            std::string errJ;
                            if (B::json::parse(salidaF, raizF, &errJ)) {
                                esDirectorioDeVerdad = true;
                                for (const B::json::Value& en : raizF["entries"].toArray()) {
                                    const std::string nombre = en["name"].toString();
                                    const bool esDir = en["type"].toString() == "d";
                                    recursos.push_back({"/dav/" + ruta + "/" + nombre
                                                            + (esDir ? "/" : ""),
                                                        nombre, esDir,
                                                        en["size"].toInt()});
                                }
                            }
                        }
                    }
                }
            }

            if (p.metodo == "PROPFIND") {
                r.codigo = 207;
                r.tipo = "application/xml; charset=utf-8";
                r.cuerpo = D::multiestado(recursos, p.cabecera("depth").empty()
                                                        ? std::string("1")
                                                        : p.cabecera("depth"));
                respuesta = H::componer(r);
                return true;
            }
            // GET o HEAD. Si lo pedido es un FICHERO —está entre los recursos y no es
            // colección—, se sirve su contenido; si es una colección, algo legible, que es
            // lo que los exploradores esperan cuando tantean.
            // GET o HEAD sobre lo pedido.
            //
            // Se intenta LEERLO como fichero antes que darlo por colección, y no al revés:
            // cuando la URL nombra un fichero, `--dump-dir-list` sobre él ya ha fallado
            // —listar un fichero no tiene sentido—, así que la lista de recursos solo trae
            // el propio recurso y buscar ahí dentro nunca encontraría nada.
            if (!rutaBaseFs.empty() && perfilPedido) {
                std::string b64;
                std::string errG;
                int rcG = -1;
                std::string motivoG;
                std::string contenido;
                if (zfsmgr::cli::ejecutarAgente(*sesionZfs, *perfilPedido,
                                                {"--dump-file", rutaBaseFs}, b64, errG, rcG,
                                                &motivoG, 60000)
                    && rcG == 0 && B::base64Decode(B::trim(b64), contenido)) {
                    r.tipo = "application/octet-stream";
                    r.cuerpo = p.metodo == "HEAD" ? std::string() : contenido;
                    respuesta = H::componer(r);
                    return true;
                }
            }
            // Ni fichero ni directorio: no existe. Contestar 200 aquí hacía que un fichero
            // borrado siguiera pareciendo que estaba.
            if (!rutaBaseFs.empty() && !esDirectorioDeVerdad) {
                r.codigo = 404;
                r.tipo = "text/plain; charset=utf-8";
                r.cuerpo = "no existe\n";
                respuesta = H::componer(r);
                return true;
            }
            r.tipo = "text/plain; charset=utf-8";
            r.cuerpo = p.metodo == "HEAD" ? std::string() : "coleccion\n";
            respuesta = H::componer(r);
            return true;
        }

        // --- Mutaciones. Todas por POST y todas con testigo: ese es el contrato.
        if (p.metodo == "POST" && p.ruta == "/accion") {
            if (!sesion.testigoVale(p.campo("testigo"))) {
                r.codigo = 403;
                r.tipo = "text/plain; charset=utf-8";
                r.cuerpo = "testigo no valido\n";
                respuesta = H::componer(r);
                return true;
            }
            const std::string conn = p.campo("c");
            const std::string objeto = p.campo("o");
            const std::string que = p.campo("que");
            const auto conns = zfsmgr::cli::cargarConexiones(op.dirConfig, maestra);
            const B::ConnectionProfile* perfil = zfsmgr::cli::buscarConexion(conns, conn);
            if (!perfil || objeto.empty()) {
                r.codigo = 400;
                r.cuerpo = paginaError("falta la conexión o el objeto", sesion.testigo());
                respuesta = H::componer(r);
                return true;
            }

            std::vector<std::string> verbo;
            if (que == "crear-instantanea") {
                const std::string nombre = B::trim(p.campo("nombre"));
                // El nombre lo escribe una persona y viaja hasta un argv de `zfs`. Se
                // valida AQUÍ además de en el daemon: una barra o una arroba dentro
                // convertirían «instantánea de este dataset» en otra cosa.
                if (nombre.empty() || nombre.find('@') != std::string::npos
                    || nombre.find('/') != std::string::npos
                    || nombre.find(' ') != std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError("nombre de instantánea no válido: «" + nombre + "»",
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = {"--mutate-zfs-snapshot", objeto + "@" + nombre,
                         p.campo("rec") == "1" ? "1" : "0"};
            } else if (que == "borrar-instantanea") {
                if (objeto.find('@') == std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError("eso no es una instantánea: «" + objeto + "»",
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = {"--mutate-zfs-destroy", objeto, "0", ""};
            } else if (que == "montar" || que == "desmontar") {
                verbo = {"--mutate-zfs-generic",
                         argvEnBase64({que == "montar" ? "mount" : "unmount", objeto})};
            } else if (que == "crear-dataset") {
                const std::string nombre = B::trim(p.campo("nombre"));
                if (nombre.empty() || nombre.find('@') != std::string::npos
                    || nombre.find('/') != std::string::npos
                    || nombre.find(' ') != std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError("nombre de dataset no válido: «" + nombre + "»",
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = {"--mutate-zfs-generic", argvEnBase64({"create", objeto + "/" + nombre})};
            } else if (que == "renombrar") {
                const std::string nombre = B::trim(p.campo("nombre"));
                if (nombre.empty() || nombre.find('@') != std::string::npos
                    || nombre.find(' ') != std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError("nombre de destino no válido: «" + nombre + "»",
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = {"--mutate-zfs-generic", argvEnBase64({"rename", objeto, nombre})};
            } else if (que == "clonar") {
                const std::string nombre = B::trim(p.campo("nombre"));
                if (objeto.find('@') == std::string::npos || nombre.empty()
                    || nombre.find(' ') != std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError("un clon se hace de una instantánea a un dataset nuevo",
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = {"--mutate-zfs-clone", objeto, nombre};
            } else if (que == "borrar-dataset") {
                if (objeto.find('@') != std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError("eso es una instantánea, no un dataset",
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = {"--mutate-zfs-destroy", objeto, "0",
                         p.campo("alcance") == "r" ? "r" : ""};
            } else if (que == "rollback") {
                if (objeto.find('@') == std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError("un rollback va a una instantánea", sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = {"--mutate-zfs-rollback", objeto, "0",
                         p.campo("alcance") == "r" ? "r" : ""};
            } else if (que == "cargar-clave" || que == "descargar-clave") {
                if (que == "descargar-clave") {
                    verbo = {"--mutate-zfs-generic", argvEnBase64({"unload-key", objeto})};
                } else {
                    // La frase va en base64 DENTRO de la carga, cifrada por mTLS, y el
                    // daemon se la pasa a `zfs` por una tubería. Nunca por argumento: eso
                    // sale en el «ps» de las dos máquinas.
                    verbo = {"--mutate-zfs-load-key", B::base64Encode(objeto),
                             B::base64Encode(p.campo("frase"))};
                }
            } else if (B::startsWith(que, "pool-")) {
                const std::string op = que.substr(5);
                std::vector<std::string> argv;
                if (op == "scrub")             { argv = {"scrub", objeto}; }
                else if (op == "scrub-parar")  { argv = {"scrub", "-s", objeto}; }
                else if (op == "trim")         { argv = {"trim", objeto}; }
                else if (op == "sync")         { argv = {"sync", objeto}; }
                else if (op == "clear")        { argv = {"clear", objeto}; }
                if (argv.empty()) {
                    r.codigo = 400;
                    r.cuerpo = paginaError("acción de pool desconocida: «" + op + "»",
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = {"--mutate-zpool-generic", argvEnBase64(argv)};
            } else if (que == "set") {
                const std::string prop = B::trim(p.campo("prop"));
                const std::string valor = p.campo("valor");
                if (prop.empty() || prop.find('=') != std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError("propiedad no válida: «" + prop + "»", sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = {"--mutate-zfs-generic", argvEnBase64({"set", prop + "=" + valor, objeto})};
            } else {
                r.codigo = 400;
                r.cuerpo = paginaError("acción desconocida: «" + que + "»", sesion.testigo());
                respuesta = H::componer(r);
                return true;
            }

            std::string salidaM;
            std::string errM;
            int rcM = -1;
            std::string motivoM;
            const bool hablo = zfsmgr::cli::ejecutarAgente(*sesionZfs, *perfil, verbo, salidaM,
                                                           errM, rcM, &motivoM, 120000);
            if (!hablo || rcM != 0) {
                r.codigo = 502;
                const std::string detalle = !errM.empty() ? errM
                                            : (!salidaM.empty() ? salidaM : motivoM);
                r.cuerpo = paginaError("no se pudo: " + B::trim(detalle), sesion.testigo());
                respuesta = H::componer(r);
                return true;
            }

            // Redirección después de un POST: si no, recargar la página REPITE la acción, y
            // «crear instantánea» dos veces es ruido pero «destruir» dos veces es otra cosa.
            // Se vuelve al MISMO nodo sobre el que se actuó, con su árbol montado. Antes
            // se volvía a la raíz del pool y había que rehacer el camino a mano.
            const std::string volverA = objeto.find('@') != std::string::npos
                                            ? objeto.substr(0, objeto.find('@'))
                                            : objeto;
            const std::string raizV = B::trim(p.campo("raiz"));
            r.codigo = 302;
            r.cabecerasExtra.push_back(
                "Location: " + urlDe(conn, raizV.empty() ? volverA : raizV, volverA,
                                     Vista::Resumen));
            r.cuerpo = "";
            respuesta = H::componer(r);
            return true;
        }

        if (p.metodo != "GET") {
            r.codigo = 405;
            r.tipo = "text/plain; charset=utf-8";
            r.cuerpo = "metodo no admitido\n";
            respuesta = H::componer(r);
            return true;
        }
        // La configuración se relee en cada petición: es barato y evita servir una lista
        // rancia si se edita por el intérprete mientras esto corre.
        const auto conns = zfsmgr::cli::cargarConexiones(op.dirConfig, maestra);

        if (p.ruta == "/") {
            // La versión del agente de cada máquina. Se pregunta y se RECUERDA durante la
            // vida del proceso, incluido el fallo: sin eso, cada recarga de la portada
            // volvería a esperar el plazo entero por una máquina apagada.
            //
            // Una conexión apartada no se sondea: está apartada a propósito.
            std::vector<std::string> versiones;
            for (const B::ConnectionProfile& perfilC : conns.perfiles) {
                const std::string id = perfilC.id.empty() ? perfilC.name : perfilC.id;
                if (conns.desconectada(id)) {
                    versiones.push_back("-");
                    continue;
                }
                const auto ya = versionPorConexion.find(id);
                if (ya != versionPorConexion.end()) {
                    versiones.push_back(ya->second);
                    continue;
                }
                std::string salidaH;
                std::string errH;
                int rcH = -1;
                std::string motivoH;
                std::string version;
                if (zfsmgr::cli::ejecutarAgente(*sesionZfs, perfilC, {"--health"}, salidaH, errH,
                                                rcH, &motivoH, 8000)
                    && rcH == 0) {
                    for (const std::string& linea : B::split(salidaH, "\n", true)) {
                        if (B::startsWith(B::trim(linea), "VERSION=")) {
                            version = B::trim(B::trim(linea).substr(8));
                            break;
                        }
                    }
                }
                const std::string marcada = marcaDeVersion(version);
                versionPorConexion[id] = marcada;
                versiones.push_back(marcada);
            }
            r.cuerpo = paginaConexiones(conns.perfiles, versiones, sesion.testigo());
            respuesta = H::componer(r);
            return true;
        }

        // `/c/<conexión>[/<pool>[/<dataset>]]`. Se trocea a mano y no con una tabla de
        // rutas porque son tres formas y una tabla aquí sería más código que el reparto.
        // La confirmación de algo destructivo: es un GET porque NO hace nada todavía;
        // solo cuenta lo que pasaría. Quien ejecuta es el POST de después.
        if (p.ruta == "/confirmar") {
            const auto campoConsulta = [&p](const std::string& nombre) {
                for (const std::string& par : B::split(p.consulta, "&", true)) {
                    const std::size_t i = par.find('=');
                    if (i == std::string::npos) {
                        continue;
                    }
                    if (H::desdeUrl(par.substr(0, i)) == nombre) {
                        return H::desdeUrl(par.substr(i + 1));
                    }
                }
                return std::string();
            };
            r.cuerpo = paginaConfirmar(campoConsulta("c"), campoConsulta("o"),
                                       campoConsulta("que"), campoConsulta("raiz"),
                                       sesion.testigo());
            respuesta = H::componer(r);
            return true;
        }

        if (p.ruta.rfind("/c/", 0) != 0) {
            r.codigo = 404;
            r.tipo = "text/plain; charset=utf-8";
            r.cuerpo = "no hay nada aqui\n";
            respuesta = H::componer(r);
            return true;
        }
        const std::string resto = p.ruta.substr(3);
        const std::size_t barra = resto.find('/');
        const std::string conn = barra == std::string::npos ? resto : resto.substr(0, barra);
        const std::string objeto = barra == std::string::npos ? std::string() : resto.substr(barra + 1);

        const B::ConnectionProfile* perfil = zfsmgr::cli::buscarConexion(conns, conn);
        if (!perfil) {
            r.codigo = 404;
            r.cuerpo = paginaError("no hay ninguna conexión «" + conn + "»", sesion.testigo());
            respuesta = H::componer(r);
            return true;
        }

        // A partir de aquí SE HABLA CON LA MÁQUINA. El servidor atiende de una en una, así
        // que el túnel se monta en este mismo hilo y no hace falta desviarlo — es la misma
        // situación que el intérprete. Cuando esto pase a atender en paralelo habrá que
        // resolverlo; está anotado en el diseño.
        std::string salida;
        std::string err;
        int rc = -1;
        const auto pide = [&](const std::vector<std::string>& args, int timeoutMs) {
            salida.clear();
            err.clear();
            rc = -1;
            std::string motivo;
            return zfsmgr::cli::ejecutarAgente(*sesionZfs, *perfil, args, salida, err, rc,
                                               &motivo, timeoutMs)
                   && rc == 0;
        };

        // Las páginas de MÁQUINA: registro, trabajos y programación. Van antes que el
        // listado de pools porque comparten la misma URL con otra consulta.
        if (objeto.empty() && !p.consulta.empty()) {
            const std::string migas = enlace("/", "ZFSMgr") + " / " + enlace("/c/" + conn, conn);
            if (p.consulta == "registro=1") {
                if (!pide({"--dump-daemon-log", "0"}, 30000)) {
                    r.codigo = 502;
                    r.cuerpo = paginaError("no se pudo leer el registro", sesion.testigo());
                } else {
                    r.cuerpo = paginaTexto("Registro del daemon", migas, salida, sesion.testigo());
                }
                respuesta = H::componer(r);
                return true;
            }
            if (p.consulta == "trabajos=1") {
                if (!pide({"--job-list"}, 20000)) {
                    r.codigo = 502;
                    r.cuerpo = paginaError("no se pudieron leer los trabajos", sesion.testigo());
                } else {
                    r.cuerpo = paginaTexto("Trabajos", migas, salida, sesion.testigo());
                }
                respuesta = H::componer(r);
                return true;
            }
            if (p.consulta == "programacion=1") {
                if (!pide({"--dump-zfs-get-gsa-raw-all-pools"}, 30000)) {
                    r.codigo = 502;
                    r.cuerpo = paginaError("no se pudo leer la programación", sesion.testigo());
                } else {
                    r.cuerpo = paginaTexto("Instantáneas programadas", migas, salida,
                                           sesion.testigo());
                }
                respuesta = H::componer(r);
                return true;
            }
        }

        if (objeto.empty()) {
            if (!pide({"--dump-zpool-list"}, 20000)) {
                r.codigo = 502;
                r.cuerpo = paginaError("no se pudo hablar con «" + conn + "»: "
                                           + (err.empty() ? std::string("sin respuesta") : err),
                                       sesion.testigo());
                respuesta = H::componer(r);
                return true;
            }
            std::vector<L::Pool> pools;
            std::string errAnalisis;
            if (!L::pools(salida, pools, errAnalisis)) {
                r.codigo = 502;
                r.cuerpo = paginaError("respuesta ilegible de zpool list: " + errAnalisis,
                                       sesion.testigo());
                respuesta = H::componer(r);
                return true;
            }
            r.cuerpo = paginaPools(conn, pools, sesion.testigo());
            respuesta = H::componer(r);
            return true;
        }

        // ── El árbol, con su panel ──────────────────────────────────────────
        //
        // UNA petición, DOS llamadas como mucho: el listado recursivo —que trae el árbol
        // entero— y, si la vista elegida lo necesita, la consulta de ese nodo. El resumen
        // de un dataset no hace la segunda: sus datos ya venían en el listado.
        const auto campoConsulta = [&p](const std::string& nombre) {
            for (const std::string& par : B::split(p.consulta, "&", true)) {
                const std::size_t i = par.find('=');
                if (i == std::string::npos) {
                    continue;
                }
                if (H::desdeUrl(par.substr(0, i)) == nombre) {
                    return H::desdeUrl(par.substr(i + 1));
                }
            }
            return std::string();
        };
        const Vista vista = vistaDesde(campoConsulta("v"));
        std::string sel = B::trim(campoConsulta("sel"));
        if (sel.empty()) {
            sel = objeto;
        }
        // La selección tiene que caer DENTRO del árbol que se está sirviendo. Sin esta
        // comprobación, un `?sel=` a mano haría que el panel preguntara por un dataset de
        // otro pool mientras el árbol de la izquierda enseña este: dos cosas distintas en
        // la misma página, y la de la derecha sin forma de saber de dónde salió.
        if (!llevaHasta(objeto, sel)) {
            sel = objeto;
        }

        if (!pide({"--dump-zfs-list-all", objeto}, 30000)) {
            r.codigo = 502;
            r.cuerpo = paginaError("no se pudo listar «" + objeto + "»", sesion.testigo());
            respuesta = H::componer(r);
            return true;
        }
        const Arbol arbol = construyeArbol(L::entradas(salida), objeto);
        const std::string izq = panelArbol(conn, objeto, arbol, sel, vista);
        const auto itSel = arbol.porNombre.find(sel);
        const L::Entrada* entradaSel =
            itSel != arbol.porNombre.end() ? &itSel->second : nullptr;

        // El panel derecho. Cada vista dice qué le hace falta preguntar; la que no
        // pregunta nada es la que más se usa, y por eso moverse por el árbol es barato.
        std::string der;
        // Un fallo al preguntar NO deja la página en blanco: el árbol de la izquierda ya
        // está montado y sigue sirviendo para moverse. Lo que no se pudo leer se dice en el
        // hueco de la derecha, que es donde se estaba mirando.
        const auto pideOFalla = [&](const std::vector<std::string>& args, const char* queCosa) {
            if (pide(args, 30000)) {
                return true;
            }
            der = "<p class=\"vacio\">no se pudo leer " + H::escapaHtml(queCosa) + ": "
                  + H::escapaHtml(B::trim(err.empty() ? salida : err)) + "</p>";
            return false;
        };
        switch (vista) {
            case Vista::Resumen:
                der = resumenDelNodo(sel, arbol);
                break;
            case Vista::Props:
                if (pideOFalla({"--dump-zfs-get-all", sel}, "las propiedades")) {
                    std::vector<L::Propiedad> props;
                    std::string errAnalisis;
                    if (!L::propiedades(salida, props, errAnalisis)) {
                        der = "<p class=\"vacio\">respuesta ilegible de zfs get: "
                              + H::escapaHtml(errAnalisis) + "</p>";
                    } else {
                        der = panelPropiedades(conn, objeto, sel, props, false, sesion.testigo());
                    }
                }
                break;
            case Vista::Permisos:
                if (pideOFalla({"--dump-zfs-allow", sel}, "los permisos")) {
                    der = panelTexto(salida);
                }
                break;
            case Vista::Contenido: {
                const std::string punto =
                    entradaSel != nullptr ? B::trim(entradaSel->puntoMontaje) : std::string();
                if (punto.empty() || punto == "none" || punto == "-") {
                    der = "<p class=\"vacio\">«" + H::escapaHtml(sel)
                          + "» no tiene punto de montaje.</p>";
                } else if (pideOFalla({"--dump-dir-list", punto}, "el contenido")) {
                    der = panelContenido(conn, sel, salida);
                }
                break;
            }
            case Vista::Estado:
                if (pideOFalla({"--dump-zpool-status", objeto}, "el estado del pool")) {
                    der = panelTexto(salida);
                }
                break;
            case Vista::PropsPool:
            case Vista::Capacidades:
                if (pideOFalla({"--dump-zpool-get-all", objeto}, "las propiedades del pool")) {
                    std::vector<L::Propiedad> props;
                    std::string errAnalisis;
                    if (!L::propiedadesDePool(salida, props, errAnalisis)) {
                        der = "<p class=\"vacio\">respuesta ilegible de zpool get: "
                              + H::escapaHtml(errAnalisis) + "</p>";
                    } else {
                        der = panelPropiedades(conn, objeto, objeto, props,
                                               vista == Vista::Capacidades, sesion.testigo());
                    }
                }
                break;
            case Vista::Historial:
                if (pideOFalla({"--dump-zpool-history", objeto}, "el historial")) {
                    der = panelTexto(salida);
                }
                break;
            case Vista::Programacion:
                if (pideOFalla({"--dump-zfs-get-gsa-raw-recursive", sel}, "la programación")) {
                    der = panelProgramacion(salida);
                }
                break;
        }

        // Las acciones, SIEMPRE: son del nodo elegido, no de la vista. Cambiar de
        // «Propiedades» a «Permisos» no debería quitar de delante el botón de montar.
        const bool esInstantanea = sel.find('@') != std::string::npos;
        if (vista == Vista::Estado || vista == Vista::PropsPool || vista == Vista::Capacidades
            || vista == Vista::Historial) {
            der += accionesDePool(conn, objeto, sesion.testigo());
        } else if (esInstantanea) {
            der += accionesDeInstantanea(conn, objeto, sel, sesion.testigo());
        } else {
            der += accionesDeDataset(conn, objeto, sel, entradaSel, sesion.testigo());
        }

        const std::string migas = enlace("/", "ZFSMgr") + " / " + enlace("/c/" + H::haciaUrl(conn), conn)
                                  + " / " + enlace(urlDe(conn, objeto, objeto, Vista::Resumen), objeto);
        r.cuerpo = envuelveDosPaneles(tituloDeVista(vista, sel), migas, izq, der, sesion.testigo());
        respuesta = H::componer(r);
        return true;
    };

    std::string err;
    if (!B::tlsserver::sirve(op.bind, op.puerto, rutaCert, rutaClave, atiende,
                             [] { return g_vivo.load(); }, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    return 0;
}
