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
#include "listados.h"
#include "session.h"
#include "secretinput.h"
#include "storefiles.h"
#include "storewarnings.h"
#include "strutil.h"
#include "tlsserver.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <csignal>
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
div.opciones { border: 1px solid var(--borde); border-radius: 5px; padding: .5rem .7rem;
               background: var(--suave); margin: .2rem 0 .5rem; }
footer { margin-top: 2.5rem; padding-top: .8rem; border-top: 1px solid var(--borde);
         color: var(--tenue); font-size: .85rem; }
)CSS";

// La cabecera y el pie, iguales en todas las páginas.
std::string envuelve(const std::string& titulo, const std::string& migas,
                     const std::string& cuerpo, const std::string& testigo) {
    std::string h = "<!doctype html><html lang=\"es\"><head><meta charset=\"utf-8\">";
    h += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
    h += "<link rel=\"stylesheet\" href=\"/estilo.css\">";
    h += "<title>" + H::escapaHtml(titulo) + " — ZFSMgr</title></head><body>";
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

// Las acciones que caben en un dataset. Todas por POST: un GET que cambia algo lo repite
// el navegador al recargar, y lo precarga solo si le apetece.
std::string formularioAcciones(const std::string& conn, const std::string& ds,
                               const std::string& testigo) {
    const std::string comunes = campoTestigo(testigo)
                                + "<input type=\"hidden\" name=\"c\" value=\"" + H::escapaHtml(conn) + "\">"
                                + "<input type=\"hidden\" name=\"o\" value=\"" + H::escapaHtml(ds) + "\">";
    std::string h = "<h2>Acciones</h2>";
    h += "<form class=\"enlinea\" method=\"post\" action=\"/accion\">" + comunes
         + "<input type=\"hidden\" name=\"que\" value=\"crear-instantanea\">"
         + "<label>Instantánea nueva: <input name=\"nombre\" required></label>"
         + "<button type=\"submit\">Crear</button></form>";
    h += "<form class=\"enlinea\" method=\"post\" action=\"/accion\">" + comunes
         + "<input type=\"hidden\" name=\"que\" value=\"montar\">"
         + "<button type=\"submit\">Montar</button></form>";
    h += "<form class=\"enlinea\" method=\"post\" action=\"/accion\">" + comunes
         + "<input type=\"hidden\" name=\"que\" value=\"desmontar\">"
         + "<button type=\"submit\">Desmontar</button></form>";
    return h;
}

// La versión del agente de cada máquina, marcada como en el intérprete: « * » si se ha
// quedado atrás y « + » si va por delante de este cliente. Distinguirlas importa: si no,
// uno no sabe cuál de las dos mitades hay que actualizar.
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

// El ÁRBOL, como en la interfaz de Qt: cada dataset se despliega en su sitio y sus
// instantáneas cuelgan de él.
//
// Sale de UNA sola llamada por pool. `--dump-zfs-list-all` ya es recursivo —era lo que
// mezclaba las instantáneas de todo el árbol en una tabla— y ahí está lo bueno: con esa
// misma respuesta se construye el árbol entero sin una llamada por nivel.
//
// Se pliega con `<details>`, que es HTML y no JavaScript. La política de contenido de este
// servidor no permite guiones, y tampoco hacen falta: el navegador sabe plegar solo.
// El menú de un nodo del árbol.
//
// **No es un menú del botón derecho, y no puede serlo sin JavaScript**: el «contextmenu» es
// un evento del navegador y hay que atenderlo con un guion. Este servidor no sirve guiones
// —su política de contenido dice `default-src 'none'`— así que se pliega con `<details>`,
// que es HTML puro: un clic en «acciones» y se abre donde está el nodo.
std::string menuDelNodo(const std::string& conn, const std::string& ds, const std::string& testigo) {
    const std::string comunes = campoTestigo(testigo)
                                + "<input type=\"hidden\" name=\"c\" value=\"" + H::escapaHtml(conn) + "\">"
                                + "<input type=\"hidden\" name=\"o\" value=\"" + H::escapaHtml(ds) + "\">";
    std::string h = "<details class=\"menu\"><summary>acciones</summary><div class=\"opciones\">";
    h += enlace("/c/" + conn + "/" + ds, "Abrir") + " · ";
    h += enlace("/c/" + conn + "/" + ds + "?props=1", "Propiedades") + " · ";
    h += enlace("/c/" + conn + "/" + ds + "?permisos=1", "Permisos") + " · ";
    h += enlace("/dav/" + conn + "/" + ds + "/", "Contenido");
    h += "<form class=\"enlinea\" method=\"post\" action=\"/accion\">" + comunes
         + "<input type=\"hidden\" name=\"que\" value=\"crear-instantanea\">"
         + "<input name=\"nombre\" placeholder=\"instantánea\" required>"
         + "<button type=\"submit\">Crear</button></form>";
    h += "<form class=\"enlinea\" method=\"post\" action=\"/accion\">" + comunes
         + "<input type=\"hidden\" name=\"que\" value=\"montar\">"
         + "<button type=\"submit\">Montar</button></form>";
    h += "<form class=\"enlinea\" method=\"post\" action=\"/accion\">" + comunes
         + "<input type=\"hidden\" name=\"que\" value=\"desmontar\">"
         + "<button type=\"submit\">Desmontar</button></form>";
    h += "</div></details>";
    return h;
}

std::string ramaDelArbol(const std::string& conn, const std::string& nodo,
                         const std::map<std::string, std::vector<L::Entrada>>& hijos,
                         const std::map<std::string, std::vector<L::Entrada>>& instantaneas,
                         const std::map<std::string, L::Entrada>& porNombre, int profundidad,
                         const std::string& testigoDelArbol) {
    std::string h;
    const auto itE = porNombre.find(nodo);
    const std::string corto = nodo.find('/') == std::string::npos
                                  ? nodo
                                  : nodo.substr(nodo.find_last_of('/') + 1);
    // Abierto en los dos primeros niveles: el pool y sus hijos. Más adentro, plegado — un
    // árbol que se abre entero de golpe no es un árbol, es la lista de antes.
    h += std::string("<details") + (profundidad < 2 ? " open" : "") + "><summary>";
    h += "<strong>" + H::escapaHtml(corto) + "</strong>";
    if (itE != porNombre.end()) {
        h += " <span class=\"tenue\">" + H::escapaHtml(bytesLegibles(itE->second.usado));
        if (itE->second.montado == "no") {
            h += " · sin montar";
        }
        h += "</span>";
    }
    h += "</summary><div class=\"rama\">";
    h += menuDelNodo(conn, nodo, testigoDelArbol);

    const auto itS = instantaneas.find(nodo);
    if (itS != instantaneas.end() && !itS->second.empty()) {
        h += "<ul class=\"instantaneas\">";
        for (const L::Entrada& s : itS->second) {
            const std::string sufijo = s.nombre.substr(s.nombre.find('@') + 1);
            h += "<li>@" + H::escapaHtml(sufijo) + " <span class=\"tenue\">"
                 + H::escapaHtml(bytesLegibles(s.usado)) + "</span> "
                 + enlace("/confirmar?c=" + conn + "&o=" + s.nombre + "&que=borrar-instantanea",
                          "borrar")
                 + "</li>";
        }
        h += "</ul>";
    }
    const auto itH = hijos.find(nodo);
    if (itH != hijos.end()) {
        for (const L::Entrada& hijo : itH->second) {
            h += ramaDelArbol(conn, hijo.nombre, hijos, instantaneas, porNombre, profundidad + 1,
                              testigoDelArbol);
        }
    }
    h += "</div></details>";
    return h;
}

std::string paginaArbol(const std::string& conn, const std::string& raiz,
                        const std::vector<L::Entrada>& entradas, const std::string& testigo) {
    std::map<std::string, std::vector<L::Entrada>> hijos;
    std::map<std::string, std::vector<L::Entrada>> instantaneas;
    std::map<std::string, L::Entrada> porNombre;
    for (const L::Entrada& e : entradas) {
        if (e.esInstantanea()) {
            instantaneas[e.nombre.substr(0, e.nombre.find('@'))].push_back(e);
            continue;
        }
        porNombre[e.nombre] = e;
        if (e.nombre == raiz) {
            continue;
        }
        hijos[e.nombre.substr(0, e.nombre.find_last_of('/'))].push_back(e);
    }
    std::string cuerpo = ramaDelArbol(conn, raiz, hijos, instantaneas, porNombre, 0, testigo);
    cuerpo += "<h2>De este dataset</h2><p>";
    cuerpo += enlace("/c/" + conn + "/" + raiz + "?props=1", "Propiedades") + " · ";
    cuerpo += enlace("/c/" + conn + "/" + raiz + "?permisos=1", "Permisos delegados") + " · ";
    cuerpo += enlace("/dav/" + conn + "/" + raiz + "/", "Contenido (WebDAV)");
    cuerpo += "</p>";
    cuerpo += formularioAcciones(conn, raiz, testigo);
    const std::string migas = enlace("/", "ZFSMgr") + " / " + enlace("/c/" + conn, conn);
    return envuelve(raiz, migas, cuerpo, testigo);
}

std::string paginaDatasets(const std::string& conn, const std::string& raiz,
                           const std::vector<L::Entrada>& entradas, const std::string& testigo) {
    std::vector<std::vector<std::string>> datasets;
    std::vector<std::vector<std::string>> instantaneas;
    for (const L::Entrada& e : entradas) {
        if (e.esInstantanea()) {
            // Solo las de ESTE dataset, no las de sus descendientes.
            //
            // `--dump-zfs-list-all` es recursivo, y los datasets ya se filtran a los hijos
            // directos; dejar pasar las instantáneas de todo el árbol mezclaba en una sola
            // tabla las de una docena de datasets, y enseñando solo lo que va tras la «@»
            // no había forma de saber de cuál era cada una. Las de un hijo se ven entrando
            // en el hijo, que es donde uno las busca.
            const std::string duenno = e.nombre.substr(0, e.nombre.find('@'));
            if (duenno != raiz) {
                continue;
            }
            const std::string corto = e.nombre.substr(e.nombre.find('@') + 1);
            instantaneas.push_back({H::escapaHtml(corto),
                                    H::escapaHtml(bytesLegibles(e.usado)),
                                    H::escapaHtml(e.creacion),
                                    enlace("/confirmar?c=" + conn + "&o=" + e.nombre
                                               + "&que=borrar-instantanea",
                                           "Borrar…")});
            continue;
        }
        datasets.push_back({enlace("/c/" + conn + "/" + e.nombre, e.nombre),
                            H::escapaHtml(bytesLegibles(e.usado)),
                            H::escapaHtml(e.compresion),
                            H::escapaHtml(e.montado),
                            H::escapaHtml(e.puntoMontaje)});
    }
    std::string cuerpo = "<h2>Datasets</h2>";
    cuerpo += tabla({"Nombre", "Usado", "Compr.", "Montado", "Punto de montaje"}, datasets);
    cuerpo += "<h2>Instantáneas</h2>";
    cuerpo += tabla({"Nombre", "Usado", "Creación", ""}, instantaneas);
    cuerpo += "<h2>De este dataset</h2><p>";
    cuerpo += enlace("/c/" + conn + "/" + raiz + "?props=1", "Propiedades") + " · ";
    cuerpo += enlace("/c/" + conn + "/" + raiz + "?permisos=1", "Permisos delegados") + " · ";
    cuerpo += enlace("/dav/" + conn + "/" + raiz + "/", "Contenido (WebDAV)");
    cuerpo += "</p>";
    cuerpo += formularioAcciones(conn, raiz, testigo);
    const std::string migas = enlace("/", "ZFSMgr") + " / " + enlace("/c/" + conn, conn);
    return envuelve(raiz, migas, cuerpo, testigo);
}

std::string paginaPropiedades(const std::string& conn, const std::string& objeto,
                              const std::vector<L::Propiedad>& props, const std::string& testigo) {
    std::vector<std::vector<std::string>> filas;
    for (const L::Propiedad& p : props) {
        filas.push_back({H::escapaHtml(p.nombre), H::escapaHtml(p.valor), H::escapaHtml(p.origen)});
    }
    // Cambiar una propiedad: el nombre se teclea a mano de momento. La edición en línea
    // sobre la tabla es de la fase 4; esto ya ejercita el camino entero.
    std::string form = "<h2>Cambiar una propiedad</h2>";
    form += "<form class=\"enlinea\" method=\"post\" action=\"/accion\">" + campoTestigo(testigo)
            + "<input type=\"hidden\" name=\"c\" value=\"" + H::escapaHtml(conn) + "\">"
            + "<input type=\"hidden\" name=\"o\" value=\"" + H::escapaHtml(objeto) + "\">"
            + "<input type=\"hidden\" name=\"que\" value=\"set\">"
            + "<label>Propiedad <input name=\"prop\" required></label> "
            + "<label>Valor <input name=\"valor\" required></label> "
            + "<button type=\"submit\">Aplicar</button></form>";
    const std::string migas = enlace("/", "ZFSMgr") + " / " + enlace("/c/" + conn, conn)
                              + " / " + enlace("/c/" + conn + "/" + objeto, objeto);
    return envuelve("Propiedades de " + objeto, migas,
                    tabla({"Propiedad", "Valor", "Origen"}, filas) + form, testigo);
}

// La página de confirmación de algo que destruye.
//
// Existe porque un botón que borra sin preguntar, en una página que se puede recargar, es
// una forma de perder datos por un clic de más. Dice EXACTAMENTE qué se va a hacer y sobre
// qué, igual que el intérprete antes de una orden destructiva.
std::string paginaConfirmar(const std::string& conn, const std::string& objeto,
                            const std::string& que, const std::string& testigo) {
    std::string texto;
    if (que == "borrar-instantanea") {
        texto = "Se va a DESTRUIR la instantánea «" + objeto + "» en «" + conn
                + "». Lo que hubiera en ella se pierde y no hay vuelta atrás.";
    } else {
        texto = "Acción desconocida.";
    }
    std::string cuerpo = "<p>" + H::escapaHtml(texto) + "</p>";
    if (que == "borrar-instantanea") {
        cuerpo += "<form class=\"enlinea\" method=\"post\" action=\"/accion\">" + campoTestigo(testigo)
                  + "<input type=\"hidden\" name=\"c\" value=\"" + H::escapaHtml(conn) + "\">"
                  + "<input type=\"hidden\" name=\"o\" value=\"" + H::escapaHtml(objeto) + "\">"
                  + "<input type=\"hidden\" name=\"que\" value=\"" + H::escapaHtml(que) + "\">"
                  + "<button type=\"submit\">Sí, destruirla</button></form>";
    }
    const std::string padre = objeto.substr(0, objeto.find('@'));
    cuerpo += "<p>" + enlace("/c/" + conn + "/" + padre, "No, volver") + "</p>";
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
                verbo = {"--mutate-zfs-snapshot", objeto + "@" + nombre, "0"};
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
            const std::string volverA = objeto.find('@') != std::string::npos
                                            ? objeto.substr(0, objeto.find('@'))
                                            : objeto;
            r.codigo = 302;
            r.cabecerasExtra.push_back("Location: /c/" + conn + "/" + volverA);
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
                                       campoConsulta("que"), sesion.testigo());
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

        if (p.consulta == "permisos=1") {
            const std::string migas = enlace("/", "ZFSMgr") + " / " + enlace("/c/" + conn, conn)
                                      + " / " + enlace("/c/" + conn + "/" + objeto, objeto);
            if (!pide({"--dump-zfs-allow", objeto}, 30000)) {
                r.codigo = 502;
                r.cuerpo = paginaError("no se pudieron leer los permisos de «" + objeto + "»",
                                       sesion.testigo());
            } else {
                r.cuerpo = paginaTexto("Permisos de " + objeto, migas, salida, sesion.testigo());
            }
            respuesta = H::componer(r);
            return true;
        }

        if (p.consulta == "props=1") {
            if (!pide({"--dump-zfs-get-all", objeto}, 30000)) {
                r.codigo = 502;
                r.cuerpo = paginaError("no se pudieron leer las propiedades de «" + objeto + "»",
                                       sesion.testigo());
                respuesta = H::componer(r);
                return true;
            }
            std::vector<L::Propiedad> props;
            std::string errAnalisis;
            if (!L::propiedades(salida, props, errAnalisis)) {
                r.codigo = 502;
                r.cuerpo = paginaError("respuesta ilegible de zfs get: " + errAnalisis,
                                       sesion.testigo());
                respuesta = H::componer(r);
                return true;
            }
            r.cuerpo = paginaPropiedades(conn, objeto, props, sesion.testigo());
            respuesta = H::componer(r);
            return true;
        }

        if (!pide({"--dump-zfs-list-all", objeto}, 30000)) {
            r.codigo = 502;
            r.cuerpo = paginaError("no se pudo listar «" + objeto + "»", sesion.testigo());
            respuesta = H::componer(r);
            return true;
        }
        r.cuerpo = paginaArbol(conn, objeto, L::entradas(salida), sesion.testigo());
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
