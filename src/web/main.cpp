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
#include "daemoninstall.h"
#include "dosextremos.h"
#include "helpers.h"
#include "sincronizacion.h"
#include "peers.h"
#include "avanzadas.h"
#include "pools.h"
#include "instantaneas.h"
#include "datasets.h"
#include "gsa.h"
#include "i18n.h"
#include "listados.h"
#include "peticiones.h"
#include "session.h"
#include "secretinput.h"
#include "storefiles.h"
#include "transferencia.h"
#include "transportcmd.h"
// El `T(clave, castellano)` del intérprete: mismos catálogos, mismas claves, mismo
// ayudante. Un tercer sistema de traducción en el mismo programa acabaría discrepando.
#include "tr.h"
#include "zfsallow.h"
#include "zfsprops.h"
#include "storewarnings.h"
#include "strutil.h"
#include "tlsserver.h"
#include "transporttunnel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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
namespace PET = zfsmgr::commands::peticiones;
namespace ZP = zfsmgr::base::zfsprops;
namespace DX = zfsmgr::base::dosextremos;
namespace ZA = zfsmgr::base::zfsallow;
namespace TR = zfsmgr::base::transferencia;
namespace SY = zfsmgr::base::sincronizacion;
namespace PR = zfsmgr::base::peers;
namespace AV = zfsmgr::commands::avanzadas;
namespace PL = zfsmgr::commands::pools;
// `INST` y no `IN`: en Windows `IN` es un MACRO de `windows.h`, y
// `namespace IN = …` no compila allí. Lo cazó el cruce de MinGW.
namespace INST = zfsmgr::commands::instantaneas;
namespace DS = zfsmgr::commands::datasets;

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
    bool verboso{false};
    std::string idioma;   // vacío = el de la configuración, y si no, castellano
};

void uso() {
    // Una sola cadena y no una por línea: así el traductor ve la ayuda entera y puede
    // recolocar lo que haga falta, en vez de tener que casar veinte trozos sueltos.
    std::fprintf(stderr, "%s",
                 TC("t_web_uso",
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
                 "  --lang es|en|zh       Idioma. Sin él, el de la interfaz gráfica\n"
                 "                        (app.language de config.json). Cada navegador\n"
                 "                        puede elegir el suyo desde el pie de la página.\n"
                 "  -v, --verbose         Cuenta por la salida de error lo que hace el\n"
                 "                        transporte con cada máquina. Sin esto, un fallo\n"
                 "                        de transporte solo se ve como «no se pudo».\n"
                 "\n"
                 "La contraseña maestra NO se pasa por argumento ni por variable de entorno:\n"
                 "las dos salen en «ps» para cualquier usuario de la máquina.\n"));
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
:root { color-scheme: light dark; --borde:#c9ced6; --suave:#f4f6f8; --tinta:#1c2530; --tenue:#5b6673; --acento:#2f5f8c; --fondo:#ffffff; }
@media (prefers-color-scheme: dark) {
  :root { --borde:#39424e; --suave:#232a33; --tinta:#e7ecf2; --tenue:#9aa5b3; --acento:#7fb0e0; --fondo:#161b21; }
}
body { font: 15px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; color: var(--tinta);
       margin: 0; padding: 0 1.5rem 3rem; max-width: 1100px; }
body.ancho { max-width: none; }
a { color: var(--acento); text-decoration: none; }
a:hover { text-decoration: underline; }
h1 { font-size: 1.5rem; margin: .4rem 0 1rem; }
h2 { font-size: 1.05rem; margin: 1.6rem 0 .5rem; color: var(--tenue); }
nav.migas { padding: .8rem 0; color: var(--tenue); font-size: .9rem; }
nav.migas a { color: var(--tenue); }
table { border-collapse: collapse; width: 100%; margin: .3rem 0 1rem; font-size: .93rem; }
th { text-align: left; font-weight: 600; color: var(--tenue); font-size: .86rem;
     padding: .4rem .6rem; border-bottom: 1px solid var(--borde); }
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
details.menu > summary { font-size: .82rem; color: var(--tenue); }
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
div.seccion.tit { color: var(--tenue); font-size: .82rem; margin: .7rem 0 .2rem; }
table.ficha { width: auto; }
table.ficha th { width: 12rem; border-bottom: 1px solid var(--suave); vertical-align: top; }
table.ficha td { font-variant-numeric: tabular-nums; }
div.grupo { border: 1px solid var(--borde); border-radius: 6px; padding: .6rem .8rem;
            margin: 0 0 .7rem; }
div.grupotit { font-size: .88rem; color: var(--tenue); font-weight: 600;
               margin-bottom: .45rem; }
div.fila { display: flex; flex-wrap: wrap; gap: .3rem 1.1rem; align-items: center;
           margin-bottom: .5rem; }
label.campo { font-size: .92rem; }
label.campo input[type="number"] { width: 4.5rem; }
div.pendiente { color: var(--tenue); font-size: .86rem; border-left: 3px solid var(--borde);
                padding: .3rem 0 .3rem .7rem; margin: .8rem 0; }
details.marco { border: 1px solid var(--borde); border-radius: 6px; margin: 0 0 .9rem;
                background: var(--fondo); }
details.marco > summary { padding: .55rem .8rem; background: var(--suave);
                          border-radius: 5px 5px 5px 5px; }
details.marco[open] > summary { border-bottom: 1px solid var(--borde);
                                border-radius: 5px 5px 0 0; }
span.marcotit { font-size: .95rem; color: var(--tinta); font-weight: 600; }
div.marco.cerrado { padding: .55rem .8rem; background: var(--suave); }
a.marcoenlace { display: block; color: inherit; }
a.marcoenlace:hover span.marcotit { color: var(--acento); }
details.marco > summary::before { margin-right: .1rem; }
div.marco.cerrado span.marcotit { padding-left: 1em; }
div.hoja.hojads { padding-left: 1.15em; }
div.abajo { margin-top: 1.2rem; }
div.pestanas { display: flex; flex-wrap: wrap; gap: .25rem; align-items: center;
               margin-bottom: .35rem; }
div.pestanas.dentro { margin: .1rem 0 .35rem 1.2rem; }
div.detalle { border: 1px solid var(--borde); border-radius: 6px; padding: .8rem 1rem .3rem;
              margin-top: .5rem; }
h2.detalletit { margin: 0 0 .6rem; font-size: 1.05rem; color: var(--tinta); }
div.detalle > h2:first-child { margin-top: 0; }
body.ancho > div.dos { margin-top: .8rem; }
span.pestgrupo { font-size: .86rem; color: var(--tenue); margin-right: .4rem;
                 min-width: 4.5rem; }
div.pestanas.dentro a.pest { font-size: .82rem; padding: .15rem .6rem; }
a.pest { font-size: .86rem; padding: .2rem .7rem; border: 1px solid var(--borde);
         border-radius: 4px; color: var(--tenue); background: var(--fondo); }
a.pest:hover { border-color: var(--acento); text-decoration: none; }
a.pest.activa { background: var(--acento); border-color: var(--acento); color: #fff;
                font-weight: 600; }
a.idioma { color: var(--tenue); margin-right: .3rem; }
a.idioma.activo { color: var(--tinta); font-weight: 600; text-decoration: underline; }
div.origen { border: 1px solid var(--acento); border-radius: 5px; background: var(--suave);
             padding: .35rem .7rem; margin: .3rem 0 .6rem; font-size: .9rem; }
div.engris { color: var(--tenue); padding: .12rem 0; }
div.logcuerpo { margin-top: .5rem; max-height: 26rem; overflow: auto;
                border: 1px solid var(--borde); border-radius: 5px; padding: .4rem .6rem; }
div.logcuerpo pre { margin: 0; background: transparent; padding: 0; }
span.malo { color: #a33; font-weight: 600; }
div.hoja.hojads a.nodo { color: var(--tinta); }
div.marcocuerpo { padding: .7rem .8rem .2rem; }
div.marcocuerpo > table { margin-top: 0; }
div.marcocuerpo > div.grupo:last-child { margin-bottom: .5rem; }
th.prop { font-size: .93rem; text-transform: none; letter-spacing: 0; font-weight: 500;
          color: var(--tinta); white-space: nowrap; width: 1%; }
td.tenue { color: var(--tenue); font-size: .85em; }
select { font: inherit; padding: .26rem .3rem; border: 1px solid var(--borde);
         border-radius: 4px; background: var(--suave); color: var(--tinta); }
form.enlinea input[name="valor"] { width: 12rem; }
form.enlinea button { margin-left: .3rem; }
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
    // Ni migas ni <h1> en las páginas de dos paneles.
    //
    // Había hasta TRES líneas antes de llegar al árbol: la ruta de migas, el título de la
    // vista y el aviso del origen marcado. Las dos primeras decían lo mismo que ya dice el
    // árbol —el nodo elegido va resaltado— y una la repetía la pestaña activa, así que
    // ocupaban un tercio de la pantalla para no añadir nada. El título de la vista baja
    // DENTRO de la pestaña, que es donde tiene contexto.
    //
    // Las páginas sueltas —confirmar, error, instalar el daemon— sí los conservan: ahí no
    // hay árbol ni pestañas que digan dónde se está.
    if (!ancho) {
        h += "<nav class=\"migas\">" + migas + "</nav>";
        h += "<h1>" + H::escapaHtml(titulo) + "</h1>";
    }
    h += cuerpo;
    h += "<footer><form class=\"enlinea\" method=\"post\" action=\"/salir\">";
    h += "<input type=\"hidden\" name=\"testigo\" value=\"" + H::escapaHtml(testigo) + "\">";
    h += "<button type=\"submit\">" + H::escapaHtml(T("t_web_cerrar", "Cerrar el servidor"))
         + "</button></form>";
    // El idioma se elige por NAVEGADOR y no por proceso: dos personas mirando el mismo
    // servidor pueden querer idiomas distintos, y un `--lang` que mandara sobre todos
    // obligaría a reiniciarlo para cambiarlo. Va en su propia cookie, no en la de sesión:
    // cambiar de idioma no debe tocar la autenticación.
    h += " · ";
    for (const auto& idi : {std::pair<const char*, const char*>{"es", "Español"},
                            {"en", "English"},
                            {"zh", "中文"}}) {
        const bool esta = (zfsmgr::base::i18n::language() == idi.first);
        h += "<a class=\"idioma" + std::string(esta ? " activo" : "") + "\" href=\"/idioma?a="
             + idi.first + "\">" + idi.second + "</a> ";
    }
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
// La fecha que da el daemon en sus trabajos: ISO-8601 en UTC. Se deja en UTC —es lo que
// dice la cadena y cambiarlo a la hora local exigiría convertirla, no reescribirla— pero se
// quita la «T» y la «Z», que son ruido para quien la lee.
std::string fechaIso(const std::string& crudo) {
    std::string t = B::trim(crudo);
    const std::size_t i = t.find('T');
    if (i != std::string::npos) {
        t[i] = ' ';
    }
    if (!t.empty() && t.back() == 'Z') {
        t.pop_back();
        t += " UTC";
    }
    return t;
}

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
    // `localtime_r` es POSIX y en MinGW no existe; Windows trae `localtime_s`, con los
    // argumentos al revés. Mismo reparto que ya hace el daemon — este binario se compila
    // para Windows igual que el intérprete.
    std::tm partes{};
#ifdef _WIN32
    if (localtime_s(&partes, &cuando) != 0) {
        return t;
    }
#else
    if (localtime_r(&cuando, &partes) == nullptr) {
        return t;
    }
#endif
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
// El empaquetado vive en la capa base: es cómo espera el DAEMON los argumentos, no una
// decisión de este cliente. Aquí queda solo el nombre corto que usa el resto del fichero.
std::string argvEnBase64(const std::vector<std::string>& argv) {
    return zfsmgr::base::helpers::argvParaAgente(argv);
}

// Un campo oculto con el testigo. Va en TODOS los formularios: sin él, el servidor
// rechaza la petición con 403 aunque la cookie sea la buena.
std::string campoTestigo(const std::string& testigo) {
    return "<input type=\"hidden\" name=\"testigo\" value=\"" + H::escapaHtml(testigo) + "\">";
}

// ── El registro de lo que ESTE servidor ha hecho ─────────────────────────────
//
// La interfaz de Qt tiene un «Log combinado» que sale de un fichero que ella misma escribe
// (`~/.config/ZFSMgr/application.log`). Este servidor no escribía ninguno, así que no había
// forma de saber qué le había pedido a qué máquina, ni con qué resultado, ni por qué había
// tardado lo que tardó.
//
// Vive en MEMORIA y no en un fichero, a propósito: un servidor que uno arranca cuando le
// hace falta y para al terminar no debería dejar en el disco la lista de qué datasets miró.
// Se pierde al parar, y aquí eso es lo correcto. El registro que sí persiste es el del
// daemon, que es de la máquina y no de quien la mira.
struct Apunte {
    std::string cuando;
    std::string conexion;
    std::string verbo;
    int rc{0};
    long ms{0};
    bool hablo{true};
    std::string detalle;
};

// Un anillo, no una lista que crece: este proceso puede estar días levantado, y un registro
// sin tope es una fuga de memoria con otro nombre.
constexpr std::size_t kApuntesMax = 500;
std::vector<Apunte> g_apuntes;

void anota(Apunte a) {
    if (g_apuntes.size() >= kApuntesMax) {
        g_apuntes.erase(g_apuntes.begin());
    }
    g_apuntes.push_back(std::move(a));
}

std::string ahoraLegible() {
    const std::time_t t = std::time(nullptr);
    std::tm partes{};
#ifdef _WIN32
    if (localtime_s(&partes, &t) != 0) {
        return {};
    }
#else
    if (localtime_r(&t, &partes) == nullptr) {
        return {};
    }
#endif
    char buf[16];
    if (std::strftime(buf, sizeof(buf), "%H:%M:%S", &partes) == 0) {
        return {};
    }
    return buf;
}

// La ÚNICA puerta por la que este servidor habla con una máquina.
//
// Existe para que anotar no dependa de acordarse: había siete sitios llamando al agente y
// cada uno tendría que haber escrito su apunte. Uno que se olvidara dejaría un hueco en el
// registro justo donde importa —el sitio raro, el que falla— y nadie lo notaría.
bool llamaAgente(zfsmgr::cli::Sesion& ses, const B::ConnectionProfile& perfil,
                 const std::vector<std::string>& args, std::string& salida, std::string& err,
                 int& rc, std::string* motivo, int timeoutMs) {
    const auto t0 = std::chrono::steady_clock::now();
    std::string porQue;
    const bool hablo =
        zfsmgr::cli::ejecutarAgente(ses, perfil, args, salida, err, rc, &porQue, timeoutMs);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    Apunte a;
    a.cuando = ahoraLegible();
    a.conexion = perfil.id.empty() ? perfil.name : perfil.id;
    // El VERBO y sus argumentos, menos las cargas en base64: ahí dentro viajan frases de
    // cifrado y contenidos de fichero, y un registro no es sitio para ninguna de las dos.
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!a.verbo.empty()) {
            a.verbo += " ";
        }
        a.verbo += (i == 0 || args[i].size() < 40) ? args[i] : "<carga>";
    }
    a.rc = rc;
    a.ms = static_cast<long>(ms);
    a.hablo = hablo;
    if (!hablo) {
        a.detalle = porQue;
    } else if (rc != 0) {
        a.detalle = B::trim(err.empty() ? salida : err).substr(0, 200);
    }
    anota(std::move(a));
    if (motivo != nullptr) {
        *motivo = porQue;
    }
    return hablo;
}

// El identificador que devuelve `--job-submit`, o vacío.
//
// Estaba escrito dos veces en este mismo fichero. Es una forma de respuesta del daemon, no
// una decisión de quien la lee.
std::string idDeTrabajoEn(const std::string& salida) {
    for (const std::string& linea : B::split(salida, "\n", true)) {
        const std::string l = B::trim(linea);
        if (l.rfind("JOB_ID=", 0) == 0) {
            return l.substr(7);
        }
    }
    return {};
}

// Si un dataset está montado y dónde. Cuesta UNA consulta.
//
// Sincronizar lo necesita de los dos extremos, y no se puede sacar del árbol que ya está
// cargado: el origen marcado puede estar en otra máquina, o en un pool que no se ha
// desplegado en esta pantalla. Por eso se pregunta al pulsar y no al pintar.
bool montajeDeDataset(zfsmgr::cli::Sesion& ses, const B::ConnectionProfile& perfil,
                      const std::string& ds, bool& montado, std::string& punto) {
    montado = false;
    punto.clear();
    std::string salida;
    std::string err;
    int rc = 0;
    if (!llamaAgente(ses, perfil, PET::listaDeDatasets(ds), salida, err, rc, nullptr, 60000)
        || rc != 0) {
        return false;
    }
    for (const L::Entrada& e : L::entradas(salida)) {
        if (e.nombre == ds) {
            montado = (e.montado == "yes");
            punto = e.puntoMontaje;
            break;
        }
    }
    if (punto.empty()) {
        return false;
    }
    if (!B::transport::isWindowsConnection(perfil)) {
        return true;
    }

    // **En Windows la propiedad `mountpoint` NO es una ruta.**
    //
    // Un dataset que en `zfs list` sale como «/winpool/sa» está de verdad en «Z:/sa/», y
    // esa «/winpool/sa» no existe para el sistema: comprobado con `Test-Path`, que la da
    // por falsa. La ruta buena solo la sabe `zfs mount`. Sin esto, sincronizar contra
    // Windows escribiría en un sitio inventado o no escribiría en ninguno.
    std::string salidaM;
    std::string errM;
    int rcM = 0;
    if (!llamaAgente(ses, perfil, PET::montajes(), salidaM, errM, rcM, nullptr, 60000)
        || rcM != 0) {
        return false;
    }
    std::string errJ;
    B::json::Value raiz;
    if (!B::json::parse(salidaM, raiz, &errJ)) {
        return false;
    }
    std::string real = raiz["datasets"][ds]["mountpoint"].toString();
    // `zfs mount` devuelve la ruta con barra final —«Z:/sa/»—; quitarla evita que cada
    // concatenación posterior meta una barra doble.
    while (real.size() > 3 && (real.back() == '/' || real.back() == '\\')) {
        real.pop_back();
    }
    if (real.empty()) {
        return false;
    }
    punto = real;
    return true;
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

std::string grupoDeAcciones(const std::string& titulo, const std::string& cuerpo);
std::string marco(const std::string& titulo, const std::string& cuerpo, bool abierto);
std::string boton(const std::string& conn, const std::string& objeto, const std::string& raiz,
                  const std::string& que, const std::string& etiqueta, const std::string& testigo,
                  const std::string& campos = std::string(), bool peligro = false);

std::string panelConexiones(const std::vector<B::ConnectionProfile>& perfiles,
                            const std::vector<std::string>& versiones) {
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
                         // La versión, enlazada a la instalación cuando se ha quedado
                         // atrás. Es el sitio donde uno se entera de que lo está, y
                         // obligar a entrar en la máquina para encontrar el enlace era
                         // esconderlo justo del que acaba de verlo.
                         (i < versiones.size() && B::endsWith(versiones[i], " *")
                              ? enlace("/confirmar?c=" + H::haciaUrl(id) + "&que=instalar-daemon",
                                       versiones[i])
                              : H::escapaHtml(i < versiones.size() ? versiones[i]
                                                                  : std::string("-")))});
    }
    return tabla({"ID", T("t_poolcrt_auto004", "Nombre"), T("t_tipo_6cc619", "Tipo"), T("t_web_sistema_c00416", "Sistema"), T("t_host_3960ec", "Host"), T("t_usuario_3f2ecd", "Usuario"), T("t_conn_agent_001", "Daemon")}, filas);
}

std::string panelPools(const std::string& conn, const std::vector<L::Pool>& pools) {
    std::vector<std::vector<std::string>> filas;
    for (const L::Pool& p : pools) {
        filas.push_back({enlace("/c/" + conn + "/" + p.nombre, p.nombre),
                         H::escapaHtml(p.salud.empty() ? p.estado : p.salud),
                         H::escapaHtml(p.tamano),
                         H::escapaHtml(p.libre),
                         H::escapaHtml(p.uso)});
    }
    return tabla({T("t_tree_pool_prefix_001", "Pool"), T("t_web_salud_d302f9", "Salud"), T("t_poolcrt_auto018", "Tamaño"), T("t_web_libre_a68851", "Libre"), T("t_web_uso_2483c7", "Uso")}, filas);
}

// El formulario de una conexión: el mismo para crear y para editar.
//
// **La contraseña va siempre por POST y en un campo de tipo `password`.** Nunca en la URL:
// una URL se queda en el historial del navegador y en el registro de cualquier
// intermediario. Es la misma regla que impide pasarla por argumento a un proceso.
//
// Al editar, el campo de contraseña sale VACÍO y vacío significa «déjala como estaba». Si
// se rellenara con la guardada, bastaría con abrir la página para que la contraseña
// apareciera en el HTML; y si vacío significara «bórrala», editar el puerto se llevaría la
// contraseña por delante.
std::string formularioConexion(const B::ConnectionProfile* p, const std::string& testigo) {
    const bool esNueva = (p == nullptr);
    const auto v = [&](const std::string& x) { return H::escapaHtml(x); };
    std::string h = "<form method=\"post\" action=\"/accion\">";
    h += campoTestigo(testigo);
    h += "<input type=\"hidden\" name=\"que\" value=\"guardar-conexion\">";
    if (esNueva) {
        h += "<label class=\"campo\">" + H::escapaHtml(T("t_web_cn_id", "Identificador"))
             + " <input name=\"id\" required autocomplete=\"off\"></label>";
    } else {
        h += "<input type=\"hidden\" name=\"id\" value=\"" + v(p->id) + "\">";
        h += "<p class=\"tenue\">" + H::escapaHtml(T("t_web_cn_id", "Identificador")) + ": <code>"
             + v(p->id) + "</code></p>";
    }
    h += "<label class=\"campo\">" + H::escapaHtml(T("t_web_cn_nombre", "Nombre visible"))
         + " <input name=\"nombre\" value=\"" + (esNueva ? "" : v(p->name)) + "\"></label>";
    const std::string tipo = esNueva ? "SSH" : p->connType;
    h += "<label class=\"campo\">" + H::escapaHtml(T("t_web_cn_tipo", "Tipo"))
         + " <select name=\"tipo\">";
    for (const char* t : {"SSH", "LOCAL"}) {
        h += std::string("<option") + (tipo == t ? " selected" : "") + ">" + t + "</option>";
    }
    h += "</select></label>";
    const std::string so = esNueva ? "Linux" : p->osType;
    h += "<label class=\"campo\">" + H::escapaHtml(T("t_web_cn_so", "Sistema"))
         + " <select name=\"so\">";
    for (const char* t : {"Linux", "macos", "FreeBSD", "Windows"}) {
        h += std::string("<option") + (so == t ? " selected" : "") + ">" + t + "</option>";
    }
    h += "</select></label>";
    h += "<label class=\"campo\">" + H::escapaHtml(T("t_web_cn_host", "Host"))
         + " <input name=\"host\" value=\"" + (esNueva ? "" : v(p->host)) + "\"></label>";
    h += "<label class=\"campo\">" + H::escapaHtml(T("t_web_cn_user", "Usuario"))
         + " <input name=\"usuario\" value=\"" + (esNueva ? "" : v(p->username)) + "\"></label>";
    h += "<label class=\"campo\">" + H::escapaHtml(T("t_web_cn_port", "Puerto SSH"))
         + " <input name=\"puerto\" inputmode=\"numeric\" value=\""
         + (esNueva || p->port <= 0 ? "" : std::to_string(p->port)) + "\"></label>";
    h += "<label class=\"campo\">" + H::escapaHtml(T("t_web_cn_key", "Clave privada (ruta)"))
         + " <input name=\"clave\" value=\"" + (esNueva ? "" : v(p->keyPath)) + "\"></label>";
    h += "<label class=\"campo\"><input type=\"checkbox\" name=\"sudo\" value=\"1\""
         + std::string(!esNueva && p->useSudo ? " checked" : "") + "> "
         + H::escapaHtml(T("t_web_cn_sudo", "usa sudo")) + "</label>";
    h += "<label class=\"campo\">" + H::escapaHtml(T("t_web_cn_pass", "Contraseña"))
         + " <input type=\"password\" name=\"clavesecreta\" autocomplete=\"off\"></label>";
    h += "<p class=\"tenue\">"
         + H::escapaHtml(esNueva
                             ? T("t_web_cn_pass_n", "Se guarda cifrada con la clave maestra.")
                             : T("t_web_cn_pass_e",
                                 "Vacío deja la que ya estaba. Se guarda cifrada con la clave "
                                 "maestra."))
         + "</p>";
    h += "<button type=\"submit\">"
         + H::escapaHtml(esNueva ? T("t_web_cn_crear", "Crear la conexión")
                                 : T("t_web_cn_guardar", "Guardar los cambios"))
         + "</button></form>";
    return h;
}

// Lo que se le puede hacer a una MÁQUINA, que no es lo mismo que a un dataset.
//
// El registro, los trabajos y la programación ya NO están aquí: son marcos propios, porque
// no son acciones sino cosas que se miran. Un enlace que solo enseña algo no pinta en el
// mismo sitio que un botón que cambia la máquina.
// El formulario para crear un pool: nombre, redundancia y qué discos.
//
// Los dispositivos EN USO salen marcados y no se pueden elegir. `zpool create` los
// rechazaría igual, pero el error de ZFS llega después de haber elegido y no dice cuál de
// los que marcaste era el problema; verlo antes es la diferencia entre elegir y adivinar.
//
// La lista viene de `--dump-block-devices`, que dice de cada uno si está ocupado él o
// cualquiera de sus hijos: un disco con una partición montada no es candidato aunque el
// disco en sí no tenga sistema de ficheros.
std::string formularioPoolNuevo(const std::string& conn, const std::string& salidaJson,
                                const std::string& testigo) {
    std::string errJ;
    B::json::Value raiz;
    if (!B::json::parse(salidaJson, raiz, &errJ)) {
        return "<p class=\"vacio\">"
               + H::escapaHtml(T("t_web_pool_sin_disc",
                                 "(no se pudo leer la lista de dispositivos)"))
               + "</p>";
    }
    std::string libres;
    std::string ocupados;
    std::function<void(const B::json::Value&)> recorre = [&](const B::json::Value& d) {
        const std::string ruta = d["path"].toString();
        const std::string tipo = d["type"].toString();
        const bool enUso = d["inuse"].toBool();
        if (!ruta.empty() && (tipo == "disk" || tipo == "part")) {
            const std::string etiqueta = ruta + "  " + bytesLegibles(d["size"].toString())
                                         + (d["fstype"].toString().empty()
                                                ? std::string()
                                                : "  " + d["fstype"].toString());
            if (enUso) {
                ocupados += "<div class=\"engris\">" + H::escapaHtml(etiqueta) + "</div>";
            } else {
                libres += "<label class=\"campo\"><input type=\"checkbox\" name=\"disco\" "
                          "value=\"" + H::escapaHtml(ruta) + "\"> " + H::escapaHtml(etiqueta)
                          + "</label>";
            }
        }
        for (const auto& h : d["children"].toArray()) {
            recorre(h);
        }
    };
    for (const auto& d : raiz["devices"].toArray()) {
        recorre(d);
    }
    if (libres.empty()) {
        return "<p class=\"vacio\">"
               + H::escapaHtml(T("t_web_pool_nada_libre",
                                 "(no hay ningún dispositivo libre en esta máquina)"))
               + "</p>" + ocupados;
    }
    std::string h = "<form method=\"post\" action=\"/accion\">";
    h += campoTestigo(testigo);
    h += "<input type=\"hidden\" name=\"que\" value=\"crear-pool\">";
    h += "<input type=\"hidden\" name=\"c\" value=\"" + H::escapaHtml(conn) + "\">";
    h += "<input type=\"hidden\" name=\"o\" value=\"" + H::escapaHtml(conn) + "\">";
    h += "<label class=\"campo\">" + H::escapaHtml(T("t_web_pool_nombre", "Nombre del pool"))
         + " <input name=\"nombre\" required autocomplete=\"off\"></label>";
    h += "<label class=\"campo\">" + H::escapaHtml(T("t_web_pool_redun", "Redundancia"))
         + " <select name=\"redundancia\">";
    for (const char* t : {"sin redundancia", "mirror", "raidz", "raidz2", "raidz3"}) {
        h += std::string("<option>") + t + "</option>";
    }
    h += "</select></label>";
    // **«Libre» no quiere decir «vacío».**
    //
    // Lo que dice la sonda es que ese dispositivo no tiene sistema de ficheros reconocible
    // ni está montado, no que no haya nada dentro. Una partición reservada, una de otro
    // sistema operativo o un disco que se usó y se olvidó salen aquí como candidatos, y
    // `zpool create -f` los sobrescribe sin preguntar. Visto en la máquina de pruebas: una
    // partición real del disco del sistema figuraba como elegible.
    h += "<p class=\"peligro\">"
         + H::escapaHtml(T("t_web_pool_aviso",
                           "«Libre» significa que no se le ha reconocido sistema de ficheros y "
                           "que no está montado. NO significa que esté vacío: crear el pool "
                           "sobrescribe lo que hubiera dentro."))
         + "</p>";
    h += libres;
    h += "<button type=\"submit\">"
         + H::escapaHtml(T("t_web_pool_crear", "Crear el pool")) + "</button></form>";
    if (!ocupados.empty()) {
        h += "<p class=\"tenue\">"
             + H::escapaHtml(T("t_web_pool_ocupados", "En uso, no se pueden elegir:")) + "</p>"
             + ocupados;
    }
    return h;
}

// Los pools que esa máquina ve pero NO tiene importados.
//
// La sonda del daemon ejecuta `zpool import` y `zpool import -s`, cuya salida es un texto
// pensado para leer, no para analizar. Lo único que se saca de ahí es el nombre —la línea
// «  pool: <nombre>»—, y el estado se deja como lo escribe ZFS: reinterpretarlo aquí sería
// inventarse un vocabulario propio para algo que el usuario ya sabe leer.
std::vector<std::pair<std::string, std::string>> poolsImportables(const std::string& salida) {
    std::vector<std::pair<std::string, std::string>> out;
    // La sonda ejecuta `zpool import` Y `zpool import -s`, así que el mismo pool sale DOS
    // veces —la segunda pasada repite lo que ya vio la primera—. Sin deduplicar, la lista
    // ofrecía importar dos veces cada uno.
    std::set<std::string> vistos;
    for (const std::string& linea : B::split(salida, "\n", false)) {
        const std::string l = B::trim(linea);
        if (l.rfind("pool:", 0) == 0) {
            const std::string nombre = B::trim(l.substr(5));
            if (nombre.empty() || vistos.count(nombre) > 0) {
                continue;
            }
            vistos.insert(nombre);
            out.push_back({nombre, std::string()});
        } else if (l.rfind("state:", 0) == 0 && !out.empty() && out.back().second.empty()) {
            out.back().second = B::trim(l.substr(6));
        }
    }
    return out;
}

std::string accionesDeMaquina(const std::string& conn, const B::ConnectionProfile* perfil,
                              const std::string& testigo,
                              const std::vector<std::pair<std::string, std::string>>& importables,
                              bool sondaHecha, const std::string& discos,
                              const std::vector<std::string>& extraviados, bool apartada) {
    std::string h = grupoDeAcciones(
        T("t_conn_agent_001", "Daemon"),
        enlace("/confirmar?c=" + H::haciaUrl(conn) + "&que=instalar-daemon",
               T("t_web_instalar_o_a_81953d", "Instalar o actualizar el daemon…")));
    // La conexión en sí: editarla y borrarla. Hasta ahora esto solo se podía desde la
    // interfaz o el intérprete, así que una instalación nueva no se podía montar desde el
    // navegador — se podía gobernar lo que ya existía, pero no darlo de alta.
    // Importar un pool que la máquina ve pero no tiene montado. La sonda cuesta una consulta
    // y puede tardar —recorre discos—, así que va en su propio marco plegado y solo se
    // pregunta cuando alguien lo despliega… salvo que ya venga la respuesta.
    if (!importables.empty()) {
        std::string imp;
        for (const auto& par : importables) {
            imp += "<div>"
                   + boton(conn, par.first, par.first, "pool-import",
                           B::format(T("t_web_pool_importar", "Importar «%1»"), {par.first}),
                           testigo)
                   + " <span class=\"tenue\">" + H::escapaHtml(par.second) + "</span></div>";
            // Importar con otro nombre va en su PROPIO control, no como campo opcional del
            // botón de arriba.
            //
            // Con un campo opcional, un nombre tecleado por descuido renombra el pool sin que
            // nadie lo haya pedido, y el botón diría «Importar «tank»» mientras importa otra
            // cosa. Aquí lo que pone el botón es lo que hace, que es la misma regla que sigue
            // el resto de esta página al no tener JavaScript con que avisar.
            imp += "<div>"
                   + boton(conn, par.first, par.first, "pool-import-como",
                           B::format(T("t_web_pool_importar_como", "Importar «%1» como…"),
                                     {par.first}),
                           testigo,
                           "<label class=\"campo\">"
                               + H::escapaHtml(T("t_web_pool_nuevo_nombre", "Nombre nuevo"))
                               + " <input name=\"nuevo\" required autocomplete=\"off\"></label> ")
                   + "</div>";
        }
        h += grupoDeAcciones(T("t_web_pool_importables", "Pools sin importar"), imp);
    } else if (!sondaHecha) {
        h += grupoDeAcciones(
            T("t_web_pool_importables", "Pools sin importar"),
            "<div>"
                + enlace("/c/" + H::haciaUrl(conn) + "?v=acciones&sonda=1",
                         T("t_web_pool_sondear", "Buscar pools que se puedan importar…"))
                + "</div>");
    } else {
        h += grupoDeAcciones(T("t_web_pool_importables", "Pools sin importar"),
                             "<p class=\"vacio\">"
                                 + H::escapaHtml(T("t_web_pool_sin_import",
                                                   "(no hay ninguno esperando)"))
                                 + "</p>");
    }

    // Datasets que se quedaron en un punto de montaje temporal.
    //
    // Los dejan ahí «Hacia Dir» y «Desglosar» si algo se tuerce a mitad. El verbo del daemon
    // SIN «apply» solo informa —no toca nada—, así que la web enseña primero lo que hay y
    // solo después ofrece devolverlos a su sitio. Ese reparto lo trae el propio verbo; aquí
    // no se inventa nada.
    if (!extraviados.empty()) {
        std::string ex;
        for (const std::string& linea : extraviados) {
            ex += "<div class=\"tenue\">" + H::escapaHtml(linea) + "</div>";
        }
        ex += boton(conn, conn, conn, "reparar-montajes",
                    T("t_web_rm_hacer", "Devolverlos a su sitio"), testigo, std::string(), true);
        h += grupoDeAcciones(T("t_web_rm_grupo", "Montajes extraviados"), ex);
    } else if (sondaHecha) {
        h += grupoDeAcciones(T("t_web_rm_grupo", "Montajes extraviados"),
                             "<p class=\"vacio\">"
                                 + H::escapaHtml(T("t_web_rm_nada", "(ninguno)")) + "</p>");
    }

    // Crear un pool: como la sonda, cuesta una consulta y solo se hace si la piden.
    if (!discos.empty()) {
        h += grupoDeAcciones(T("t_web_pool_nuevo", "Crear un pool"),
                             formularioPoolNuevo(conn, discos, testigo));
    } else if (!sondaHecha) {
        h += grupoDeAcciones(
            T("t_web_pool_nuevo", "Crear un pool"),
            "<div>"
                + enlace("/c/" + H::haciaUrl(conn) + "?v=acciones&sonda=1",
                         T("t_web_pool_verdiscos", "Ver los dispositivos de esta máquina…"))
                + "</div>");
    }

    // Un solo control, y dice lo que va a hacer.
    //
    // No son dos botones «Conectar» y «Desconectar» con uno de los dos siempre inútil: se
    // pinta el que corresponde al estado actual. Apartar no toca nada en la máquina —solo
    // deja de hablarle— y por eso no pasa por la página de confirmación.
    const std::string apartar =
        "<p>"
        + boton(conn, conn, conn, apartada ? "conectar" : "desconectar",
                apartada ? T("t_web_cn_conectar", "Volver a usar esta conexión")
                         : T("t_web_cn_desconectar", "Apartar esta conexión"),
                testigo)
        + " <span class=\"tenue\">"
        + H::escapaHtml(apartada
                            ? T("t_web_cn_apartada_si",
                                "ahora mismo está apartada: no se le habla ni se le sondea")
                            : T("t_web_cn_apartada_no",
                                "apartarla no cambia nada en esa máquina; solo deja de "
                                "consultarse desde aquí"))
        + "</span></p>";
    h += grupoDeAcciones(T("t_web_cn_grupo", "Esta conexión"),
                         formularioConexion(perfil, testigo)
                             + apartar
                             + "<p>"
                             + enlace("/confirmar?c=" + H::haciaUrl(conn)
                                          + "&que=borrar-conexion",
                                      T("t_web_cn_borrar", "Borrar esta conexión…"))
                             + "</p>");
    return h;
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
    // Las tres siguientes no consultan nada —salen del listado del árbol o son
    // formularios— pero necesitan una vista propia para poder ser pestañas: una pestaña es
    // un enlace, y un enlace necesita una URL.
    Instantaneas,
    Acciones,
    AccionesPool,
    Diff,
    Holds,
    // Con qué otras máquinas puede hablar el daemon de ESTA. Es de la conexión, no de un
    // pool: lo que se mira es a quién puede llamar el daemon por su cuenta.
    Pares,
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
        case Vista::Instantaneas: return "instantaneas";
        case Vista::Acciones:     return "acciones";
        case Vista::AccionesPool: return "acciones-pool";
        case Vista::Diff:         return "diff";
        case Vista::Holds:        return "holds";
        case Vista::Pares:        return "pares";
    }
    return "";
}

std::string tituloDeVista(Vista v, const std::string& objeto) {
    switch (v) {
        case Vista::Resumen:      return objeto;
        case Vista::Props:        return B::format(T("t_web_t_props", "Propiedades de %1"), {objeto});
        case Vista::Permisos:     return B::format(T("t_web_t_perms", "Permisos de %1"), {objeto});
        case Vista::Contenido:    return B::format(T("t_web_t_cont", "Contenido de %1"), {objeto});
        case Vista::Estado:       return B::format(T("t_web_t_estado", "Estado de %1"), {objeto});
        case Vista::PropsPool:    return B::format(T("t_web_t_ppool", "Propiedades del pool %1"), {objeto});
        case Vista::Capacidades:  return B::format(T("t_web_t_caps", "Capacidades de %1"), {objeto});
        case Vista::Pares:        return B::format(T("t_web_t_pares", "Pares de %1"), {objeto});
        case Vista::Historial:    return B::format(T("t_web_t_hist", "Historial de %1"), {objeto});
        case Vista::Programacion: return B::format(T("t_web_t_gsa", "Instantáneas programadas de %1"), {objeto});
        case Vista::Instantaneas: return B::format(T("t_web_t_snaps", "Instantáneas de %1"), {objeto});
        case Vista::Acciones:     return B::format(T("t_web_t_acc", "Acciones sobre %1"), {objeto});
        case Vista::AccionesPool: return B::format(T("t_web_t_accpool", "Acciones sobre el pool %1"), {objeto});
        case Vista::Diff:         return B::format(T("t_web_t_diff", "Comparar con %1"), {objeto});
        case Vista::Holds:        return B::format(T("t_web_t_holds", "Retenciones de %1"), {objeto});
    }
    return objeto;
}

Vista vistaDesde(const std::string& s) {
    static const Vista todas[] = {
        Vista::Resumen,   Vista::Props,      Vista::Permisos,  Vista::Contenido,
        Vista::Estado,    Vista::PropsPool,  Vista::Capacidades, Vista::Historial,
        Vista::Pares,
        Vista::Programacion, Vista::Instantaneas, Vista::Acciones, Vista::AccionesPool,
        Vista::Diff, Vista::Holds,
    };
    for (const Vista v : todas) {
        if (s == claveDeVista(v)) {
            return v;
        }
    }
    // Lo que no se reconoce —y la ausencia— es la FICHA. Es lo que se ve al llegar a un
    // nodo, y no cuesta ninguna consulta: sus datos ya vinieron con el listado del árbol.
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
    if (conn.empty()) {
        return "/";
    }
    if (raiz.empty()) {
        return "/c/" + H::haciaUrl(conn);
    }
    const std::string base = "/c/" + H::haciaUrl(conn) + "/" + H::haciaUrl(raiz)
                             + "?sel=" + H::haciaUrl(sel);
    // La vista de fábrica no se escribe: un enlace del árbol no tiene por qué llevar
    // «&v=resumen» colgando, y sin él la URL dice lo mismo.
    return v == Vista::Resumen ? base : base + "&v=" + claveDeVista(v);
}

std::string enlaceDeNodo(const std::string& destino, const std::string& texto, bool elegido) {
    return "<a class=\"" + std::string(elegido ? "sel" : "nodo") + "\" href=\""
           + H::escapaHtml(destino) + "\">" + H::escapaHtml(texto) + "</a>";
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
    if (clase.empty())          { return T("t_web_manuales_f41c0e", "Manuales"); }
    if (clase == "hourly")      { return T("t_web_horarias_5399f3", "Horarias"); }
    if (clase == "daily")       { return T("t_web_diarias_31be0d", "Diarias"); }
    if (clase == "weekly")      { return T("t_ctx_snap_group_weekly", "Semanales"); }
    if (clase == "monthly")     { return T("t_ctx_snap_group_monthly", "Mensuales"); }
    if (clase == "yearly")      { return T("t_ctx_snap_group_yearly", "Anuales"); }
    return clase;
}

// Una rama del árbol: UN DATASET y sus datasets hijos. Nada más.
//
// Antes de cada dataset colgaban además cuatro nodos fijos —Propiedades, Permisos,
// Contenido, Programación— y un subárbol de instantáneas con sus grupos. Con treinta
// datasets eso son ciento veinte nodos de adorno entre los que buscar el que uno quiere:
// el árbol dejaba de servir para lo único que tiene que hacer, que es enseñar dónde está
// cada cosa. Todo aquello vive ahora en los marcos de la derecha.
//
// El árbol es, por tanto, SOLO la jerarquía de almacenamiento: máquinas, pools y datasets.
std::string ramaDelArbol(const std::string& conn, const std::string& raiz, const std::string& nodo,
                         const Arbol& arbol, const std::string& sel, int profundidad) {
    const auto itE = arbol.porNombre.find(nodo);
    const std::string corto =
        nodo.find('/') == std::string::npos ? nodo : nodo.substr(nodo.find_last_of('/') + 1);
    const auto itH = arbol.hijos.find(nodo);
    const bool tieneHijos = itH != arbol.hijos.end() && !itH->second.empty();

    // Abierto si es de los dos primeros niveles o si por ahí se llega al nodo elegido. Lo
    // segundo es lo que hace que una recarga deje el árbol como estaba: sin estado que
    // guardar, se DEDUCE de la selección.
    const bool abierto = profundidad < 2 || llevaHasta(nodo, sel);
    std::string h;
    if (tieneHijos) {
        h += std::string("<details") + (abierto ? " open" : "") + "><summary>";
    } else {
        // Sin hijos no hay nada que plegar, y un triángulo que no abre nada engaña. Se
        // pinta como hoja, con la misma sangría que si lo tuviera.
        h += "<div class=\"hoja hojads\">";
    }
    h += enlaceDeNodo(urlDe(conn, raiz, nodo, Vista::Resumen), corto, sel == nodo);
    if (itE != arbol.porNombre.end()) {
        h += " <span class=\"tenue\">" + H::escapaHtml(bytesLegibles(itE->second.usado));
        if (itE->second.montado != "yes") {
            h += " · sin montar";
        }
        const auto itS = arbol.instantaneas.find(nodo);
        if (itS != arbol.instantaneas.end() && !itS->second.empty()) {
            h += " · " + std::to_string(itS->second.size()) + "@";
        }
        h += "</span>";
    }
    if (!tieneHijos) {
        return h + "</div>";
    }
    h += "</summary><div class=\"rama\">";
    for (const L::Entrada& hijo : itH->second) {
        h += ramaDelArbol(conn, raiz, hijo.nombre, arbol, sel, profundidad + 1);
    }
    h += "</div></details>";
    return h;
}

// **El árbol entero, desde `zfsm://`.** Antes había tres pantallas encadenadas —lista de
// conexiones, lista de pools y por fin el árbol—, y las dos primeras eran tablas que
// obligaban a perder de vista dónde se estaba. Ahora es UN árbol, como en la interfaz de
// Qt: la raíz, las máquinas, sus pools y sus datasets.
//
// **Se despliega a lo largo del camino elegido, no entero.** Las conexiones salen siempre
// porque están en `config.json` y no cuestan nada; los pools de una máquina cuestan una
// llamada, y el árbol de un pool otra. Preguntárselo a las cuatro máquinas en cada página
// —para pintar ramas que casi siempre están plegadas— sería pagar el arranque más lento en
// cada clic, y con una máquina apagada, esperar su plazo entero. Las otras son ENLACES: se
// abren cuando se pulsan.
std::string panelArbol(const std::vector<B::ConnectionProfile>& perfiles,
                       const std::string& conn, const std::string& raiz,
                       const std::vector<L::Pool>& pools, const Arbol& arbol,
                       const std::string& sel, bool hayArbol) {
    std::string h = "<div class=\"arbol\"><details open><summary>";
    h += enlaceDeNodo("/", "zfsm://", conn.empty());
    h += "</summary><div class=\"rama\">";
    for (const B::ConnectionProfile& p : perfiles) {
        const std::string id = p.id.empty() ? p.name : p.id;
        const bool esta = (id == conn);
        h += std::string("<details") + (esta ? " open" : "") + "><summary>";
        h += enlaceDeNodo("/c/" + H::haciaUrl(id), id, esta && raiz.empty());
        if (!p.osType.empty()) {
            h += " <span class=\"tenue\">" + H::escapaHtml(p.osType) + "</span>";
        }
        h += "</summary><div class=\"rama\">";
        if (!esta) {
            // Una máquina que no es la elegida no se sondea. Sale con un aviso en vez de
            // vacía: un nodo que se abre y no enseña nada parece una máquina sin pools.
            h += "<div class=\"hoja\"><span class=\"tenue\">(pulse la máquina para ver sus "
                 "pools)</span></div>";
        } else {
            for (const L::Pool& pool : pools) {
                const bool esteP = (pool.nombre == raiz);
                h += std::string("<details") + (esteP ? " open" : "") + "><summary>";
                h += enlaceDeNodo("/c/" + H::haciaUrl(conn) + "/" + H::haciaUrl(pool.nombre),
                                  pool.nombre, esteP && sel == raiz);
                const std::string salud = pool.salud.empty() ? pool.estado : pool.salud;
                if (!salud.empty()) {
                    h += " <span class=\"tenue\">" + H::escapaHtml(salud) + "</span>";
                }
                h += "</summary><div class=\"rama\">";
                if (esteP && hayArbol) {
                    // FUNDIDO: los hijos del dataset raíz van aquí directamente, sin
                    // repetir un nodo «fc16» dentro de otro nodo «fc16».
                    const auto itR = arbol.hijos.find(raiz);
                    if (itR != arbol.hijos.end()) {
                        for (const L::Entrada& hijo : itR->second) {
                            h += ramaDelArbol(conn, raiz, hijo.nombre, arbol, sel, 1);
                        }
                    }
                } else if (!esteP) {
                    h += "<div class=\"hoja\"><span class=\"tenue\">(pulse el pool para ver sus "
                         "datasets)</span></div>";
                }
                h += "</div></details>";
            }
            if (pools.empty()) {
                h += "<div class=\"hoja\"><span class=\"tenue\">(sin pools)</span></div>";
            }
        }
        h += "</div></details>";
    }
    h += "</div></details></div>";
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
                  const std::string& campos, bool peligro) {
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

// Un MARCO plegable. Del panel derecho se fue —allí son pestañas— y le queda un solo
// usuario: la ventana del registro, que se pliega entera porque es la tercera y no
// siempre se mira.
//
// `<details>`, que es HTML y no JavaScript: el navegador sabe plegar solo.
std::string marco(const std::string& titulo, const std::string& cuerpo, bool abierto) {
    return std::string("<details class=\"marco\"") + (abierto ? " open" : "") + ">"
           + "<summary><span class=\"marcotit\">" + H::escapaHtml(titulo) + "</span></summary>"
           + "<div class=\"marcocuerpo\">" + cuerpo + "</div></details>";
}

// ── Las pestañas del panel derecho ───────────────────────────────────────────
//
// El detalle era una pila de marcos plegables, todos plegados de fábrica. Funcionaba, pero
// con doce sobre un pool la pila era una lista de títulos entre los que buscar, y abrir dos
// dejaba el segundo fuera de la pantalla. Con pestañas hay una barra que se lee de un
// vistazo y un solo contenido debajo, que es la forma que ya tiene la ventana de abajo.
//
// **Cambia el valor de fábrica, y a mejor.** Antes, al llegar a un nodo no se veía nada:
// todo plegado. Ahora se ve la FICHA, que sigue sin costar ninguna consulta —sus datos
// vinieron con el listado del árbol—. Se pasa de «nada, y gratis» a «algo útil, y gratis».
//
// El coste no cambia: se consulta la pestaña activa y ninguna más.
struct Pestana {
    Vista vista;
    std::string texto;
};

// Una barra con sus grupos. Los grupos existen por el nodo del POOL, que va fundido con su
// dataset raíz: son dos objetos en un nodo, y sus pestañas no se mezclan bien sin decir
// cuál es de cuál.
std::string barraDePestanas(const std::vector<std::pair<std::string, std::vector<Pestana>>>& grupos,
                            const std::string& conn, const std::string& raiz,
                            const std::string& sel, Vista activa) {
    std::string h;
    for (const auto& grupo : grupos) {
        if (grupo.second.empty()) {
            continue;
        }
        h += "<div class=\"pestanas\">";
        if (!grupo.first.empty()) {
            h += "<span class=\"pestgrupo\">" + H::escapaHtml(grupo.first) + "</span>";
        }
        for (const Pestana& t : grupo.second) {
            h += "<a class=\"pest" + std::string(t.vista == activa ? " activa" : "")
                 + "\" href=\"" + H::escapaHtml(urlDe(conn, raiz, sel, t.vista)) + "\">"
                 + H::escapaHtml(t.texto) + "</a>";
        }
        h += "</div>";
    }
    return h;
}

std::string grupoDeAcciones(const std::string& titulo, const std::string& cuerpo) {
    return "<div class=\"grupo\"><div class=\"grupotit\">" + H::escapaHtml(titulo) + "</div>"
           + cuerpo + "</div>";
}

// ── El origen marcado, y las acciones de dos extremos ────────────────────────
//
// El mecanismo de la interfaz de Qt: se marca un origen y luego se pulsa sobre otro nodo,
// como copiar y pegar. Aquí el origen vive en una COOKIE, no en la URL: es una marca del
// navegador que tiene que sobrevivir a moverse por el árbol, y meterla en cada enlace
// obligaría a arrastrarla por todas las páginas y se perdería al escribir una a mano.
//
// En su propia cookie y no en la de sesión, por lo mismo que el idioma: marcar un origen no
// tiene nada que ver con estar autenticado.
DX::Extremo origenDe(const H::Peticion& p) {
    // El valor va codificado ENTERO —«local%7Cwdx%2Fdatos%40lunes»— porque un nombre de
    // dataset puede llevar dentro casi cualquier cosa y una cookie no admite comas ni
    // puntos y coma. Así que primero se descodifica y luego se parte por la barra; al
    // revés, la barra viene escrita «%7C» y no se encuentra nunca.
    const std::string crudo = H::desdeUrl(p.cookie("zfsmgr_origen"));
    const std::size_t i = crudo.find('|');
    if (i == std::string::npos) {
        return {};
    }
    return {crudo.substr(0, i), crudo.substr(i + 1)};
}

// El aviso de qué hay marcado. Sale en TODAS las páginas mientras haya origen: una marca
// invisible es una marca que se olvida, y la siguiente acción de dos extremos sorprende.
std::string avisoDeOrigen(const DX::Extremo& origen) {
    if (origen.vacio()) {
        return {};
    }
    // Como URL y no como «conexión::objeto»: es la misma cosa que nombra el árbol y la
    // barra de direcciones, y tener dos nomenclaturas para lo mismo obliga a traducir de
    // cabeza cada vez.
    return "<div class=\"origen\">" + H::escapaHtml(T("t_web_origen_marcado", "Origen marcado"))
           + ": <strong>zfsm://" + H::escapaHtml(origen.conexion + "/" + origen.objeto)
           + "</strong> "
           + enlace("/origen?quitar=1", T("t_web_quitar_origen", "quitar")) + "</div>";
}

// Las seis, con las que no aplican EN GRIS y con el motivo. Esconderlas haría creer que no
// existen; enseñarlas sin decir por qué no se pueden deja al usuario probando.
std::string accionesDeDosExtremos(const std::string& conn, const std::string& raiz,
                                  const std::string& sel, const DX::Extremo& origen,
                                  const TR::Plan& plan, SY::Fallo falloSync,
                                  const std::string& testigo) {
    const DX::Extremo destino{conn, sel};
    std::string h;
    for (const DX::Accion a : {DX::Accion::Diff, DX::Accion::Clonar, DX::Accion::Copiar,
                               DX::Accion::Mover, DX::Accion::Sincronizar, DX::Accion::Nivelar}) {
        const DX::NoAplica porQue = DX::compruebo(a, origen, destino);
        const std::string etiqueta = DX::etiquetaDe(a);
        // Copiar y Nivelar SÍ se pueden, si el plan de transferencia lo dice. El motivo de
        // que no —un extremo Windows, un ZFS viejo, un daemon sin trabajos— sale del plan,
        // que es quien lo sabe, y no de una lista escrita aquí.
        // Sincronizar ya está, pero NO es una transferencia: compara ficheros sobre los
        // puntos de montaje. Va por la página de confirmación porque esa página hace antes
        // una pasada EN SECO, y con `--delete` puede borrar en el destino.
        //
        // Lo que se mira aquí es solo lo barato —misma máquina, datasets, nada de Windows,
        // daemon en pie—; los montajes cuestan una consulta y se comprueban al pulsar.
        if (a == DX::Accion::Sincronizar) {
            if (falloSync == SY::Fallo::Ninguno) {
                h += "<div>"
                     + enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(sel)
                                  + "&raiz=" + H::haciaUrl(raiz) + "&que=sincronizar-desde-origen",
                              etiqueta)
                     + " <span class=\"tenue\">" + H::escapaHtml(origen.objeto) + " → "
                     + H::escapaHtml(sel) + "</span></div>";
            } else {
                h += "<div class=\"engris\">" + H::escapaHtml(etiqueta)
                     + " <span class=\"tenue\">— " + H::escapaHtml(SY::etiquetaDe(falloSync))
                     + "</span></div>";
            }
            continue;
        }
        // Mover ya está: es un `zfs rename` dentro del pool, no una transferencia. Va por
        // la página de confirmación porque cambia de sitio un dataset entero y con él la
        // ruta de montaje de todo lo que cuelgue.
        if (a == DX::Accion::Mover && porQue == DX::NoAplica::Ninguna) {
            h += "<div>"
                 + enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(sel)
                              + "&raiz=" + H::haciaUrl(raiz) + "&que=mover-desde-origen",
                          etiqueta)
                 + " <span class=\"tenue\">" + H::escapaHtml(origen.objeto) + " → "
                 + H::escapaHtml(DX::destinoDeMover(origen, destino)) + "</span></div>";
            continue;
        }
        const bool esDeTransferencia = (a == DX::Accion::Copiar || a == DX::Accion::Nivelar);
        if (esDeTransferencia && porQue == DX::NoAplica::TodaviaNoEstaEnLaWeb) {
            if (plan.sePuede()) {
                h += "<div>"
                     + boton(conn, sel, raiz,
                             a == DX::Accion::Copiar ? "copiar-desde-origen"
                                                     : "nivelar-desde-origen",
                             etiqueta, testigo,
                             "<label class=\"campo\"><input type=\"checkbox\" name=\"rec\" "
                             "value=\"1\" checked> "
                                 + H::escapaHtml(T("t_web_con_hijos",
                                                   "con sus descendientes"))
                                 + "</label> ")
                     + " <span class=\"tenue\">" + H::escapaHtml(origen.objeto) + " → "
                     + H::escapaHtml(sel) + "</span></div>";
                continue;
            }
            h += "<div class=\"engris\">" + H::escapaHtml(etiqueta) + " <span class=\"tenue\">— "
                 + H::escapaHtml(TR::etiquetaDe(plan.fallo)) + "</span></div>";
            continue;
        }
        if (porQue != DX::NoAplica::Ninguna) {
            h += "<div class=\"engris\">" + H::escapaHtml(etiqueta) + " <span class=\"tenue\">— "
                 + H::escapaHtml(DX::etiquetaDe(porQue)) + "</span></div>";
            continue;
        }
        if (a == DX::Accion::Diff) {
            h += "<div>" + enlace(urlDe(conn, raiz, sel, Vista::Diff), etiqueta)
                 + " <span class=\"tenue\">" + H::escapaHtml(origen.objeto) + " → "
                 + H::escapaHtml(sel) + "</span></div>";
            continue;
        }
        if (a == DX::Accion::Clonar) {
            h += "<div>"
                 + boton(conn, sel, raiz, "clonar-desde-origen", etiqueta, testigo,
                         "<label class=\"campo\">"
                             + H::escapaHtml(T("t_web_con_el_nombre", "con el nombre"))
                             + " <input name=\"nombre\" placeholder=\"clon\" required></label> ")
                 + "</div>";
        }
    }
    return h;
}

// El menú contextual de un DATASET, con los mismos submenús que en Qt: «Dataset» para el
// estado del propio dataset y «Acciones» para lo que toca sus DATOS.
// Los hijos DIRECTOS de un dataset, con su nombre relativo.
//
// Relativo y no completo porque es lo que se le enseña a quien elige, y porque el verbo del
// daemon los quiere así de todas formas. El árbol los tiene ya: esto no cuesta ninguna
// consulta.
// Los identificadores de todas las conexiones, para poder elegir una en un formulario.
std::vector<std::string> nombresDe(const zfsmgr::cli::Conexiones& conns) {
    std::vector<std::string> out;
    for (const B::ConnectionProfile& p : conns.perfiles) {
        out.push_back(p.id.empty() ? p.name : p.id);
    }
    return out;
}

std::vector<std::string> hijosDirectosDe(const Arbol& arbol, const std::string& ds) {
    std::vector<std::string> out;
    const auto it = arbol.hijos.find(ds);
    if (it == arbol.hijos.end()) {
        return out;
    }
    for (const L::Entrada& hijo : it->second) {
        const std::size_t barra = hijo.nombre.rfind('/');
        out.push_back(barra == std::string::npos ? hijo.nombre : hijo.nombre.substr(barra + 1));
    }
    return out;
}

std::string accionesDeDataset(const std::string& conn, const std::string& raiz,
                              const std::string& ds, const L::Entrada* e,
                              const DX::Extremo& origen, const TR::Plan& plan,
                              SY::Fallo falloSync, const std::string& testigo,
                              const std::vector<std::string>& hijos, bool esWindows,
                              const std::vector<std::string>& maquinas) {
    const bool montado = e != nullptr && e->montado == "yes";
    const bool cifrado = e != nullptr && !e->cifrado.empty() && e->cifrado != "off"
                         && e->cifrado != "-";
    std::string h;

    std::string ds1;
    ds1 += boton(conn, ds, raiz, "crear-dataset", T("t_ctx_ds_create001", "Crear"), testigo,
                 "<label>hijo <input name=\"nombre\" placeholder=\"nombre\" required></label> ");
    ds1 += boton(conn, ds, raiz, "renombrar", T("t_rename_group_001", "Renombrar"), testigo,
                 "<label>a <input name=\"nombre\" placeholder=\"pool/otro\" required></label> ");
    // Montar solo si NO está montado y desmontar solo si lo está. Lo que no aplica no sale:
    // un botón que siempre falla enseña menos que su ausencia.
    if (!montado) {
        ds1 += boton(conn, ds, raiz, "montar", T("t_ctx_ds_mount001", "Montar"), testigo);
    } else {
        ds1 += boton(conn, ds, raiz, "desmontar", T("t_ctx_ds_unmount001", "Desmontar"), testigo);
    }
    ds1 += enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(ds)
                      + "&que=borrar-dataset&raiz=" + H::haciaUrl(raiz),
                  T("t_web_borrar_6b5f63", "Borrar…"));
    h += grupoDeAcciones(T("t_web_dataset_105268", "Dataset"), ds1);

    std::string snap;
    snap += boton(conn, ds, raiz, "crear-instantanea", T("t_web_crear_instan_300c04", "Crear instantánea"), testigo,
                  campo("nombre", "nombre") + " <label><input type=\"checkbox\" name=\"rec\" "
                  "value=\"1\"> recursiva</label> ");
    h += grupoDeAcciones("Instantáneas", snap);

    // Promover un clon: pasa a ser el dataset «de verdad» y su origen queda colgando de él.
    //
    // Se ofrece siempre y no solo en clones porque el listado del árbol no trae la propiedad
    // `origin`, y averiguarla costaría una consulta por dataset dibujado. Sobre uno que no
    // es clon, `zfs promote` no toca nada y responde «not a cloned filesystem», que es un
    // error entendible y sin consecuencias.
    h += grupoDeAcciones(T("t_web_clon_grupo", "Clon"),
                         boton(conn, ds, raiz, "promover",
                               T("t_web_promover", "Promover este clon"), testigo)
                             + " <span class=\"tenue\">"
                             + H::escapaHtml(T("t_web_promover_nota",
                                               "solo hace algo si es un clon"))
                             + "</span>");

    if (cifrado) {
        // La frase de cifrado va por un campo de CONTRASEÑA y por POST, nunca en la URL:
        // una URL se queda en el historial del navegador y en el registro de cualquier
        // intermediario. Es la misma regla que impide pasarla por argumento al agente.
        std::string cif;
        cif += boton(conn, ds, raiz, "cargar-clave", T("t_ctx_load_key001", "Cargar clave"), testigo,
                     "<input type=\"password\" name=\"frase\" placeholder=\"frase\" required> ");
        cif += boton(conn, ds, raiz, "descargar-clave", T("t_ctx_unload_key001", "Descargar clave"), testigo);
        // Cambiar la frase pide escribirla DOS veces.
        //
        // Sin JavaScript no hay forma de comparar los dos campos antes de enviar, así que la
        // comparación se hace en el servidor. Y hay que hacerla en alguna parte: una errata
        // aquí no da un error, da un dataset que a partir de ahora solo se abre con una frase
        // que nadie conoce. Es el mismo motivo por el que el intérprete la pregunta dos veces.
        cif += boton(conn, ds, raiz, "cambiar-clave",
                     T("t_ctx_change_key001", "Cambiar la frase"), testigo,
                     "<input type=\"password\" name=\"frase\" placeholder=\"frase nueva\" required> "
                     "<input type=\"password\" name=\"frase2\" placeholder=\"repítala\" required> ",
                     true);
        h += grupoDeAcciones(T("t_web_clave_de_cif_e5875e", "Clave de cifrado"), cif);
    }

    // Las de DOS extremos. El origen se marca aquí y se usa desde otro nodo.
    h += grupoDeAcciones(T("t_web_origen_destino", "Con dos extremos"),
                         "<div>" + enlace("/origen?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(ds),
                                          T("t_web_marcar_origen", "Marcar como origen"))
                             + "</div>"
                             + accionesDeDosExtremos(conn, raiz, ds, origen, plan, falloSync,
                                                     testigo));
    // Las avanzadas. Las tres primeras van por RPC y como TRABAJO del daemon: mueven datos
    // y pueden tardar, y una petición HTTP colgada durante horas no es forma de esperar.
    std::string av;

    // Desglosar: un subdirectorio corriente pasa a ser un dataset hijo en su sitio. Los
    // argumentos van en PARES —qué directorio y con qué nombre— y se admiten varios de una
    // vez, que es como lo hace el intérprete.
    av += "<form method=\"post\" action=\"/accion\">" + campoTestigo(testigo)
          + "<input type=\"hidden\" name=\"que\" value=\"desglosar\">"
            "<input type=\"hidden\" name=\"c\" value=\"" + H::escapaHtml(conn) + "\">"
            "<input type=\"hidden\" name=\"o\" value=\"" + H::escapaHtml(ds) + "\">"
            "<input type=\"hidden\" name=\"raiz\" value=\"" + H::escapaHtml(raiz) + "\">";
    av += "<p class=\"tenue\">"
          + H::escapaHtml(T("t_web_bd_nota",
                            "Cada subdirectorio con el nombre del dataset que lo sustituye. El "
                            "contenido no se mueve de sitio para quien mire desde fuera."))
          + "</p>";
    for (int i = 0; i < 3; ++i) {
        av += "<label class=\"campo\">"
              + H::escapaHtml(T("t_web_bd_subdir", "Subdirectorio"))
              + " <input name=\"subdir\" autocomplete=\"off\"> → "
              + H::escapaHtml(T("t_web_bd_nuevo", "dataset"))
              + " <input name=\"nombre\" autocomplete=\"off\"></label>";
    }
    av += "<button type=\"submit\">" + H::escapaHtml(T("t_web_bd_hacer", "Desglosar"))
          + "</button></form>";
    h += grupoDeAcciones(T("t_web_bd_grupo", "Desglosar"), av);

    // Ensamblar: lo contrario. Los hijos se ELIGEN de los que hay, no se escriben: el árbol
    // ya los trae, y escribir un nombre a mano es la forma de equivocarse en una operación
    // que mueve datos.
    if (!hijos.empty()) {
        std::string en = "<form method=\"post\" action=\"/accion\">" + campoTestigo(testigo)
                         + "<input type=\"hidden\" name=\"que\" value=\"ensamblar\">"
                           "<input type=\"hidden\" name=\"c\" value=\"" + H::escapaHtml(conn)
                         + "\"><input type=\"hidden\" name=\"o\" value=\"" + H::escapaHtml(ds)
                         + "\"><input type=\"hidden\" name=\"raiz\" value=\""
                         + H::escapaHtml(raiz) + "\">";
        for (const std::string& hijo : hijos) {
            en += "<label class=\"campo\"><input type=\"checkbox\" name=\"hijo\" value=\""
                  + H::escapaHtml(hijo) + "\"> " + H::escapaHtml(hijo) + "</label>";
        }
        en += "<button type=\"submit\">"
              + H::escapaHtml(T("t_web_as_hacer", "Devolverlos a directorios")) + "</button></form>";
        h += grupoDeAcciones(T("t_web_as_grupo", "Ensamblar"), en);
    }

    // Hacia Dir: vuelca el dataset a un directorio corriente. Solo en Unix — el verbo del
    // daemon está entre `#ifndef _WIN32` porque usa el montaje alternativo, que allí no
    // existe. Decirlo vale más que ofrecerlo y que falle.
    if (esWindows) {
        h += grupoDeAcciones(T("t_web_td_grupo", "Hacia un directorio"),
                             "<div class=\"engris\">"
                                 + H::escapaHtml(T("t_web_td_nowin",
                                                   "en Windows esto no pasa por el agente; "
                                                   "hágalo desde la interfaz"))
                                 + "</div>");
    } else {
        h += grupoDeAcciones(
            T("t_web_td_grupo", "Hacia un directorio"),
            boton(conn, ds, raiz, "hacia-dir", T("t_web_td_hacer", "Volcar a un directorio"),
                  testigo,
                  "<label class=\"campo\">" + H::escapaHtml(T("t_web_td_dest", "Directorio"))
                      + " <input name=\"destino\" required autocomplete=\"off\"></label>"
                        "<label class=\"campo\"><input type=\"checkbox\" name=\"borra\" "
                        "value=\"1\"> "
                      + H::escapaHtml(T("t_web_td_del", "y destruir el dataset al terminar"))
                      + "</label> "));
    }

    // Desde Dir: traer el contenido de un directorio corriente a este dataset.
    //
    // **Por otro camino que el intérprete.** El verbo `--mutate-advanced-fromdir` del agente
    // lee un tar por la entrada estándar, y el canal RPC no tiene entrada estándar: por eso
    // está marcado como «solo terminal». Aquí se usa el árbol por el socket entre daemons,
    // que hace lo mismo sin tar y funciona igual con un extremo Windows.
    //
    // El contenido entra en la RAÍZ del dataset: aquí no hay `--subdir` como en el
    // intérprete. El receptor sigue exigiendo que el directorio exista, pero el verbo que
    // faltaba para crearlo ya está —`--mutate-advanced-fromdir-prepare`, el que usan la
    // ventana y el intérprete para hacer esto mismo sin tubería—, así que ofrecerlo aquí es
    // añadir el campo al formulario y llamarlo antes de ponerse a escuchar.
    std::string fd = "<form method=\"post\" action=\"/accion\">" + campoTestigo(testigo)
                     + "<input type=\"hidden\" name=\"que\" value=\"desde-dir\">"
                       "<input type=\"hidden\" name=\"c\" value=\"" + H::escapaHtml(conn)
                     + "\"><input type=\"hidden\" name=\"o\" value=\"" + H::escapaHtml(ds)
                     + "\"><input type=\"hidden\" name=\"raiz\" value=\""
                     + H::escapaHtml(raiz) + "\">";
    fd += "<label class=\"campo\">" + H::escapaHtml(T("t_web_fd_maquina", "Máquina de origen"))
          + " <select name=\"origenconn\">";
    for (const std::string& c : maquinas) {
        fd += std::string("<option") + (c == conn ? " selected" : "") + ">" + H::escapaHtml(c)
              + "</option>";
    }
    fd += "</select></label>";
    fd += "<label class=\"campo\">" + H::escapaHtml(T("t_web_fd_dir", "Directorio de origen"))
          + " <input name=\"directorio\" required autocomplete=\"off\"></label>";
    fd += "<p class=\"tenue\">"
          + H::escapaHtml(T("t_web_fd_nota",
                            "Su contenido entra en la raíz de este dataset. No borra nada de "
                            "lo que ya haya."))
          + "</p>";
    fd += "<button type=\"submit\">" + H::escapaHtml(T("t_web_fd_hacer", "Traerlo"))
          + "</button></form>";
    h += grupoDeAcciones(T("t_web_fd_grupo", "Desde un directorio"), fd);
    return h;
}

// El menú de una INSTANTÁNEA. En Qt son cinco entradas; aquí están las tres que no piden
// un segundo extremo.
std::string accionesDeInstantanea(const std::string& conn, const std::string& raiz,
                                  const std::string& snap, const DX::Extremo& origen,
                                  const TR::Plan& plan, SY::Fallo falloSync,
                                  const std::string& testigo) {
    std::string h;
    std::string g;
    g += boton(conn, snap, raiz, "clonar", T("t_web_clonar_98a66b", "Clonar"), testigo,
               "<label>en <input name=\"nombre\" placeholder=\"pool/clon\" required></label> ");
    g += enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(snap)
                    + "&que=rollback&raiz=" + H::haciaUrl(raiz),
                T("t_web_rollback_a61c5c", "Rollback…"));
    g += " · ";
    g += enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(snap)
                    + "&que=borrar-instantanea&raiz=" + H::haciaUrl(raiz),
                T("t_web_borrar_6b5f63", "Borrar…"));
    h += grupoDeAcciones(T("t_web_instantanea_659df4", "Instantánea"), g);
    h += grupoDeAcciones(T("t_web_origen_destino", "Con dos extremos"),
                         "<div>" + enlace("/origen?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(snap),
                                          T("t_web_marcar_origen", "Marcar como origen"))
                             + "</div>"
                             + accionesDeDosExtremos(conn, raiz, snap, origen, plan, falloSync,
                                                     testigo));
    return h;
}

// El menú de un POOL: las acciones de mantenimiento que en Qt cuelgan de «Gestión».
std::string accionesDePool(const std::string& conn, const std::string& pool,
                           const std::string& testigo) {
    std::string h;
    std::string g;
    g += boton(conn, pool, pool, "pool-scrub", T("t_web_scrub_d47fb4", "Scrub"), testigo);
    g += boton(conn, pool, pool, "pool-scrub-parar", T("t_web_parar_scrub_ccc4f3", "Parar scrub"), testigo);
    g += boton(conn, pool, pool, "pool-trim", T("t_web_trim_0266ab", "Trim"), testigo);
    // Trim también se puede parar, y no lo ofrecía. Como initialize, su «parar» es `-c`.
    g += boton(conn, pool, pool, "pool-trim-parar", T("t_web_parar_trim", "Parar trim"), testigo);
    // Initialize lleva su parada como el scrub: escribe sobre el espacio libre del pool
    // entero y puede durar horas, así que poder detenerlo es parte de la acción, no un
    // extra. `zpool initialize -s`.
    g += boton(conn, pool, pool, "pool-initialize", T("t_web_initialize", "Initialize"), testigo);
    g += boton(conn, pool, pool, "pool-initialize-parar",
               T("t_web_parar_initialize", "Parar initialize"), testigo);
    g += boton(conn, pool, pool, "pool-sync", T("t_web_sync_905f63", "Sync"), testigo);
    g += boton(conn, pool, pool, "pool-clear", T("t_web_clear_719ea3", "Clear"), testigo);
    h += grupoDeAcciones(T("t_ctx_management001", "Gestión"), g);

    // Upgrade y Reguid van por la página de confirmación, no por botón.
    //
    // No es simetría con Exportar y Destruir: es que **ninguna de las dos se deshace**.
    // `upgrade` sube la versión del pool sin vuelta atrás —el CLI también pregunta—, y
    // `reguid` cambia el identificador único, que es lo que otras máquinas usan para saber
    // que ese pool es ese pool. Un botón de un solo clic para algo irreversible es
    // justamente lo que la página de confirmación existe para evitar.
    h += grupoDeAcciones(
        T("t_web_pool_irrev", "Sin vuelta atrás"),
        "<div>"
            + enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(pool)
                         + "&raiz=" + H::haciaUrl(pool) + "&que=pool-upgrade",
                     T("t_web_pool_upgrade", "Subir la versión del pool…"))
            + "</div><div>"
            + enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(pool)
                         + "&raiz=" + H::haciaUrl(pool) + "&que=pool-reguid",
                     T("t_web_pool_reguid", "Cambiar el identificador único…"))
            + "</div>");

    // Exportar va por la página de confirmación: no borra nada, pero el pool DESAPARECE de
    // esa máquina hasta que alguien lo importe, y lo que esté usándolo se queda sin él.
    h += grupoDeAcciones(
        T("t_web_pool_dispon", "Disponibilidad"),
        "<div>"
            + enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(pool)
                         + "&raiz=" + H::haciaUrl(pool) + "&que=pool-export",
                     T("t_web_pool_exportar", "Exportar este pool…"))
            + "</div><div>"
            + enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(pool)
                         + "&raiz=" + H::haciaUrl(pool) + "&que=pool-destroy",
                     T("t_web_pool_destruir", "DESTRUIR este pool…"))
            + "</div>");
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
        {T("t_poolcrt_auto004", "Nombre"), e->nombre},
        {"GUID", e->guid},
        {T("t_web_usado_7f0217", "Usado"), bytesLegibles(e->usado)},
        {"Referenciado", bytesLegibles(e->referenciado)},
        {"Compresión", e->compresion},
        {"Cifrado", e->cifrado},
        {T("t_web_creacion_4e62d9", "Creación"), fechaLegible(e->creacion)},
    };
    if (!e->esInstantanea()) {
        datos.push_back({"Montado", e->montado});
        datos.push_back({T("t_web_punto_de_monta_70570c", "Punto de montaje"), e->puntoMontaje});
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

// El resultado de lanzar una copia. NO redirige, igual que la instalación del daemon: lo
// que hay que leer aquí es el identificador con el que seguirla, y una redirección lo
// tiraría. Se puede porque lanzar dos veces no es destructivo: el segundo trabajo se
// encuentra el destino ocupado y se para.
// De qué trabajo se está informando. Era un booleano «esNivelar», y en cuanto apareció el
// tercer caso —sincronizar— dejó de dar: un booleano solo sabe contar hasta dos, y lo que
// salía era una página que decía «Copia lanzada» después de sincronizar.
enum class QueTrabajo { Copiar, Nivelar, Sincronizar };

std::string paginaTrabajoLanzado(const std::string& conn, const std::string& origen,
                                 const std::string& destino, const TR::Trabajo& t,
                                 const TR::Reanudacion& reanuda, const std::string& testigo,
                                 QueTrabajo cual) {
    std::string cuerpo;
    if (t.ok()) {
        // Decir «copia» al nivelar no es solo feo: son operaciones distintas —una manda el
        // flujo entero y la otra un incremental— y quien lea esta página tiene que poder
        // fiarse de que dice lo que ha pasado.
        std::string plantilla;
        switch (cual) {
            case QueTrabajo::Nivelar:
                plantilla = T("t_web_job_ok_niv",
                              "Nivelado lanzado: %1 → %2. Lo hace el daemon, así que sigue "
                              "aunque cierre esta página.");
                break;
            case QueTrabajo::Sincronizar:
                plantilla = T("t_web_job_ok_sync",
                              "Sincronización lanzada: %1 → %2. La hace el daemon, así que "
                              "sigue aunque cierre esta página.");
                break;
            case QueTrabajo::Copiar:
                plantilla = T("t_web_job_ok",
                              "Copia lanzada: %1 → %2. La hace el daemon, "
                              "así que sigue aunque cierre esta página.");
                break;
        }
        cuerpo += "<p>" + H::escapaHtml(B::format(plantilla, {origen, destino})) + "</p>";
        cuerpo += "<p class=\"tenue\">" + H::escapaHtml(T("t_web_job_id", "Identificador"))
                  + ": <code>" + H::escapaHtml(t.id) + "</code></p>";
        if (reanuda.hay()) {
            cuerpo += "<div class=\"pendiente\">"
                      + H::escapaHtml(B::format(
                            T("t_web_job_reanuda",
                              "Se ha continuado una transferencia que quedó a medias en %1, en "
                              "vez de mandarlo todo otra vez."),
                            {reanuda.quienLoTiene}))
                      + "</div>";
        }
        cuerpo += "<p>"
                  + enlace("/c/" + H::haciaUrl(conn) + "?log=trabajos%3A" + H::haciaUrl(conn),
                           T("t_jobs_tab_001", "Transferencias"))
                  + "</p>";
    } else {
        cuerpo += "<p>" + H::escapaHtml(TR::etiquetaDe(t.fallo)) + "</p>";
        if (!t.detalle.empty()) {
            cuerpo += "<pre>" + H::escapaHtml(t.detalle) + "</pre>";
        }
    }
    cuerpo += "<p>" + enlace("/c/" + H::haciaUrl(conn), T("t_web_volver_a_e70d48", "Volver a "))
              + "</p>";
    std::string titulo = T("t_web_copiar_t", "Copiar");
    switch (cual) {
        case QueTrabajo::Nivelar:     titulo = T("t_web_nivelar_t", "Nivelar"); break;
        case QueTrabajo::Sincronizar: titulo = T("t_web_sincronizar_t", "Sincronizar"); break;
        case QueTrabajo::Copiar:      break;
    }
    return envuelve(titulo, enlace("/", "ZFSMgr"), cuerpo, testigo);
}

// Una colección de WebDAV, para el navegador.
//
// El mismo listado que los exploradores reciben en XML, pero en HTML y con enlaces. Se
// ordena con los directorios primero y luego por nombre, que es como se lee un directorio.
std::string paginaColeccionDav(const std::string& ruta, const std::vector<D::Recurso>& recursos,
                               const std::string& testigo) {
    std::vector<std::vector<std::string>> filas;
    std::vector<const D::Recurso*> orden;
    for (const D::Recurso& re : recursos) {
        // El primero de la lista es el propio recurso pedido: enseñarlo dentro de sí mismo
        // sería un enlace que no lleva a ninguna parte.
        if (re.href == "/dav/" + ruta + "/" || re.href == "/dav/" + ruta) {
            continue;
        }
        orden.push_back(&re);
    }
    std::sort(orden.begin(), orden.end(), [](const D::Recurso* a, const D::Recurso* b) {
        if (a->coleccion != b->coleccion) {
            return a->coleccion;
        }
        return a->nombre < b->nombre;
    });
    for (const D::Recurso* re : orden) {
        filas.push_back({enlace(re->href, re->nombre + (re->coleccion ? "/" : "")),
                         re->coleccion ? H::escapaHtml(T("t_web_dav_dir", "directorio"))
                                         : H::escapaHtml(T("t_web_dav_fich", "fichero")),
                         re->coleccion ? std::string()
                                         : H::escapaHtml(bytesLegibles(std::to_string(re->tamano)))});
    }
    std::string cuerpo;
    // Subir un nivel. Sin esto, entrar en un directorio es un viaje de ida.
    if (!ruta.empty()) {
        const std::size_t barra = ruta.find_last_of('/');
        cuerpo += "<p>" + enlace("/dav/" + (barra == std::string::npos ? std::string()
                                                                      : ruta.substr(0, barra) + "/"),
                                 T("t_web_dav_arriba", "Subir un nivel"))
                  + "</p>";
    }
    cuerpo += tabla({T("t_web_nombre", "Nombre"), T("t_web_tipo", "Tipo"),
                     T("t_web_usado_7f0217", "Usado")},
                    filas);
    return envuelve("/" + ruta, enlace("/", "ZFSMgr"), cuerpo, testigo);
}

// Los permisos DELEGADOS, en una tabla con lo que se puede quitar y un formulario para
// añadir.
//
// Era un volcado de texto, que para leerlo servía y para cambiarlo no. Y cambiarlo es lo
// que uno viene a hacer aquí: delegar `snapshot` a alguien es lo que le permite hacer copias
// sin ser root, que es el motivo de que esto exista.
//
// El ALCANCE se enseña con todas las letras —«solo aquí», «aquí y en los descendientes»—
// en vez de con la letra de la bandera. Es la parte que se equivoca: en la salida de `zfs
// allow` va en el título de la sección y no en la línea, y conceder a los descendientes lo
// que se quería conceder solo aquí no da ningún error, simplemente pasa.
std::string panelPermisos(const std::string& conn, const std::string& raiz,
                          const std::string& ds, const std::string& salida,
                          const std::string& testigo) {
    const auto entradas = ZA::analiza(salida);
    std::vector<std::vector<std::string>> filas;
    for (std::size_t i = 0; i < entradas.size(); ++i) {
        const ZA::Entrada& e = entradas[i];
        std::string permisos;
        for (const std::string& p : e.permisos) {
            if (!permisos.empty()) {
                permisos += ", ";
            }
            permisos += H::escapaHtml(p);
        }
        // Para retirar hace falta decir EXACTAMENTE la misma entrada, así que se manda su
        // índice y el servidor la vuelve a leer. Mandar los campos sueltos por el
        // formulario dejaría que una recarga vieja retirara algo que ya no es lo mismo.
        filas.push_back({H::escapaHtml(ZA::etiquetaDe(e.quien)),
                         H::escapaHtml(e.nombre.empty() ? std::string("—") : e.nombre),
                         permisos,
                         H::escapaHtml(ZA::etiquetaDe(e.alcance)),
                         boton(conn, ds, raiz, "quitar-permiso", T("t_web_quitar", "Quitar"),
                               testigo,
                               "<input type=\"hidden\" name=\"idx\" value=\"" + std::to_string(i)
                                   + "\">",
                               true)});
    }
    std::string h;
    if (filas.empty()) {
        h += "<p class=\"vacio\">"
             + H::escapaHtml(T("t_web_sin_permisos",
                               "(este dataset no tiene ningún permiso delegado)"))
             + "</p>";
    } else {
        h += tabla({T("t_web_a_quien", "A quién"), T("t_poolcrt_auto004", "Nombre"),
                    T("t_web_permisos_col", "Permisos"), T("t_web_alcance", "Alcance"), ""},
                   filas);
    }

    // El formulario de alta. Los permisos se teclean separados por comas, como los escribe
    // `zfs`: una lista de casillas con los cuarenta y tantos que hay sería peor de usar.
    std::string f = "<div class=\"fila\">";
    f += "<label class=\"campo\">" + H::escapaHtml(T("t_web_a_quien", "A quién"))
         + " <select name=\"quien\">";
    for (const ZA::Quien q : {ZA::Quien::Usuario, ZA::Quien::Grupo, ZA::Quien::Todos}) {
        f += "<option value=\"" + std::string(ZA::claveDe(q)) + "\">"
             + H::escapaHtml(ZA::etiquetaDe(q)) + "</option>";
    }
    f += "</select></label>";
    f += "<label class=\"campo\">" + H::escapaHtml(T("t_poolcrt_auto004", "Nombre"))
         + " <input name=\"nombre\" placeholder=\"linarese\"></label>";
    f += "<label class=\"campo\">" + H::escapaHtml(T("t_web_alcance", "Alcance"))
         + " <select name=\"alcance\">";
    for (const ZA::Alcance a : {ZA::Alcance::LocalYDescendientes, ZA::Alcance::Local,
                                ZA::Alcance::Descendientes, ZA::Alcance::AlCrear}) {
        f += "<option value=\"" + std::string(ZA::claveDe(a)) + "\">"
             + H::escapaHtml(ZA::etiquetaDe(a)) + "</option>";
    }
    f += "</select></label></div>";
    f += "<div class=\"fila\"><label class=\"campo\">"
         + H::escapaHtml(T("t_web_permisos_col", "Permisos"))
         + " <input name=\"permisos\" placeholder=\"snapshot,mount,create\" required></label>";
    f += "</div>";
    h += grupoDeAcciones(T("t_web_delegar", "Delegar permisos"),
                         "<form method=\"post\" action=\"/accion\">" + campoTestigo(testigo)
                             + "<input type=\"hidden\" name=\"c\" value=\"" + H::escapaHtml(conn)
                             + "\"><input type=\"hidden\" name=\"o\" value=\"" + H::escapaHtml(ds)
                             + "\"><input type=\"hidden\" name=\"raiz\" value=\""
                             + H::escapaHtml(raiz)
                             + "\"><input type=\"hidden\" name=\"volver\" value=\"permisos\">"
                               "<input type=\"hidden\" name=\"que\" value=\"dar-permiso\">"
                             + f + "<button type=\"submit\">"
                             + H::escapaHtml(T("t_web_delegar_b", "Delegar")) + "</button></form>");
    return h;
}

// Las RETENCIONES de una o varias instantáneas, de `zfs holds -H`: una línea por
// retención, «instantánea \t etiqueta \t cuándo».
//
// Una retención es lo que impide destruir una instantánea, así que lo que uno viene a
// buscar aquí casi siempre es «por qué no puedo borrar esto». Por eso sale la etiqueta al
// lado del botón de soltarla, y no en una pantalla aparte.
std::map<std::string, std::vector<std::pair<std::string, std::string>>> leeHolds(
    const std::string& salida) {
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> out;
    for (const std::string& linea : B::split(salida, "\n", true)) {
        const std::vector<std::string> col = B::split(linea, "\t", false);
        if (col.size() < 2) {
            continue;
        }
        out[B::trim(col[0])].push_back(
            {B::trim(col[1]), col.size() > 2 ? B::trim(col[2]) : std::string()});
    }
    return out;
}

std::string panelHolds(const std::string& conn, const std::string& raiz, const std::string& snap,
                       const std::string& salida, const std::string& testigo) {
    const auto todas = leeHolds(salida);
    const auto it = todas.find(snap);
    std::vector<std::vector<std::string>> filas;
    if (it != todas.end()) {
        for (const auto& h : it->second) {
            filas.push_back({H::escapaHtml(h.first), H::escapaHtml(h.second),
                             boton(conn, snap, raiz, "soltar-hold",
                                   T("t_web_soltar", "Soltar"), testigo,
                                   "<input type=\"hidden\" name=\"etiqueta\" value=\""
                                       + H::escapaHtml(h.first) + "\">")});
        }
    }
    std::string h;
    if (filas.empty()) {
        h += "<p class=\"vacio\">"
             + H::escapaHtml(T("t_web_sin_holds",
                               "(esta instantánea no tiene ninguna retención: se puede "
                               "destruir)"))
             + "</p>";
    } else {
        h += "<p class=\"tenue\">"
             + H::escapaHtml(T("t_web_holds_expl",
                               "Mientras haya una retención, ZFS se niega a destruir la "
                               "instantánea. Suéltelas todas para poder borrarla."))
             + "</p>";
        h += tabla({T("t_web_etiqueta", "Etiqueta"), T("t_web_desde", "Puesta"), ""}, filas);
    }
    h += grupoDeAcciones(
        T("t_web_nuevo_hold", "Nueva retención"),
        boton(conn, snap, raiz, "poner-hold", T("t_web_retener", "Retener"), testigo,
              "<label class=\"campo\">" + H::escapaHtml(T("t_web_etiqueta", "Etiqueta"))
                  + " <input name=\"etiqueta\" placeholder=\"no-borrar\" required></label> "));
    return h;
}

// El resultado de `zfs diff`, que llega como líneas «<marca>\t<ruta>[\t<ruta nueva>]».
//
// Las marcas son de una letra y no se leen solas: «-» es que ya no está, «+» que es nuevo,
// «M» que cambió y «R» que se renombró. Se traducen aquí en vez de enseñarlas en crudo,
// porque el punto de comparar es entender qué pasó entre los dos momentos.
std::string panelDiff(const std::string& salida, const std::string& desde,
                      const std::string& hasta) {
    std::vector<std::vector<std::string>> filas;
    for (const std::string& linea : B::split(salida, "\n", true)) {
        const std::vector<std::string> col = B::split(linea, "\t", false);
        if (col.size() < 2) {
            continue;
        }
        const std::string marca = B::trim(col[0]);
        std::string que;
        if (marca == "-")      { que = T("t_web_diff_borrado", "borrado"); }
        else if (marca == "+") { que = T("t_web_diff_nuevo", "nuevo"); }
        else if (marca == "M") { que = T("t_web_diff_cambiado", "cambiado"); }
        else if (marca == "R") { que = T("t_web_diff_renombrado", "renombrado"); }
        else                   { que = marca; }
        filas.push_back({H::escapaHtml(que), H::escapaHtml(col[1]),
                         col.size() > 2 ? H::escapaHtml(col[2]) : std::string()});
    }
    std::string h = "<p class=\"tenue\">" + H::escapaHtml(desde) + " → " + H::escapaHtml(hasta)
                    + "</p>";
    if (filas.empty()) {
        return h + "<p class=\"vacio\">"
               + H::escapaHtml(T("t_web_diff_igual", "(no hay ninguna diferencia)")) + "</p>";
    }
    return h + tabla({T("t_web_diff_que", "Qué"), T("t_web_diff_ruta", "Ruta"),
                      T("t_web_diff_ahora", "Ahora se llama")},
                     filas);
}

// Las instantáneas de un dataset, agrupadas por clase del planificador como en la interfaz
// de Qt: horarias, diarias, semanales, mensuales, anuales, y aparte las de mano.
//
// Estaban en el árbol —un nodo «@ instantáneas» por dataset, con un subnodo por grupo— y
// eso metía tres niveles de adorno entre un dataset y su hijo. Aquí caben mejor: se ven
// todas de golpe, con su tamaño y su fecha, y cada una lleva al lado lo que se le puede
// hacer sin tener que navegar hasta ella.
//
// No cuesta ninguna consulta: `--dump-zfs-list-all` ya las trajo con el árbol.
std::string panelInstantaneas(const std::string& conn, const std::string& raiz,
                              const std::string& ds, const Arbol& arbol,
                              const std::string& salidaHolds, const std::string& testigo) {
    const auto itS = arbol.instantaneas.find(ds);
    if (itS == arbol.instantaneas.end() || itS->second.empty()) {
        return "<p class=\"vacio\">(este dataset no tiene instantáneas)</p>";
    }
    std::vector<std::string> cortos;
    std::map<std::string, const L::Entrada*> porCorto;
    for (const L::Entrada& e : itS->second) {
        const std::string corto = e.nombre.substr(e.nombre.find('@') + 1);
        cortos.push_back(corto);
        porCorto[corto] = &e;
    }
    // Las retenciones de TODAS las de este dataset, de una sola consulta. Es lo que
    // convierte esta lista en útil: una instantánea retenida NO se puede destruir, y
    // enterarse al pulsar «Borrar» es enterarse tarde.
    const auto holds = leeHolds(salidaHolds);
    std::string h;
    for (const auto& grupo : B::gsa::agrupaInstantaneas(cortos)) {
        h += "<div class=\"grupotit\">" + H::escapaHtml(etiquetaDeClase(grupo.first)) + " ("
             + std::to_string(grupo.second.size()) + ")</div>";
        std::vector<std::vector<std::string>> filas;
        for (const std::string& corto : grupo.second) {
            const auto it = porCorto.find(corto);
            const std::string entera = ds + "@" + corto;
            const auto itH = holds.find(entera);
            std::string retenida;
            if (itH != holds.end() && !itH->second.empty()) {
                for (const auto& et : itH->second) {
                    if (!retenida.empty()) {
                        retenida += ", ";
                    }
                    retenida += H::escapaHtml(et.first);
                }
                retenida = "<span class=\"malo\">" + retenida + "</span>";
            }
            filas.push_back(
                {enlace(urlDe(conn, raiz, entera, Vista::Resumen), corto),
                 it == porCorto.end() ? std::string()
                                      : H::escapaHtml(bytesLegibles(it->second->usado)),
                 it == porCorto.end() ? std::string()
                                      : H::escapaHtml(fechaLegible(it->second->creacion)),
                 retenida,
                 // Retenida no se borra: ZFS lo impide. Se ofrece el enlace a sus
                 // retenciones en vez del de borrar, que solo llevaría a un error.
                 retenida.empty()
                     ? enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(entera)
                                  + "&que=rollback&raiz=" + H::haciaUrl(raiz),
                              T("t_web_rollback_a61c5c", "Rollback…"))
                           + " · "
                           + enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o="
                                        + H::haciaUrl(entera) + "&que=borrar-instantanea&raiz="
                                        + H::haciaUrl(raiz),
                                    T("t_web_borrar_6b5f63", "Borrar…"))
                     : enlace(urlDe(conn, raiz, entera, Vista::Holds),
                              T("t_web_ver_holds", "Ver sus retenciones…"))});
        }
        h += tabla({T("t_poolcrt_auto004", "Nombre"), T("t_web_usado_7f0217", "Usado"),
                    T("t_web_creacion_4e62d9", "Creación"), T("t_web_holds_tab", "Retenciones"),
                    ""},
                   filas);
    }
    (void)testigo;
    return h;
}

// El registro del daemon, recortado por la cola.
//
// Dos cosas que hay que quitarle. La primera línea es un marcador interno
// —`__ZFSMGR_LOG_OFFSET__:2093152`— que sirve para pedir solo lo nuevo la próxima vez, y no
// tiene nada que ver con lo que pasó en la máquina. Y son 50.000 líneas: enseñarlas todas
// es tirar dos megas por el socket para que nadie mire más allá del final.
//
// Se recorta por la COLA porque un registro se lee por el final: lo último que hizo la
// máquina es lo que uno viene a ver.
std::string panelRegistroDaemon(const std::string& crudo, std::size_t cuantas) {
    std::vector<std::string> lineas;
    for (const std::string& l : B::split(crudo, "\n", false)) {
        if (B::startsWith(l, "__ZFSMGR_LOG_OFFSET__:")) {
            continue;
        }
        lineas.push_back(l);
    }
    while (!lineas.empty() && B::trim(lineas.back()).empty()) {
        lineas.pop_back();
    }
    if (lineas.empty()) {
        return "<p class=\"vacio\">(el registro del daemon está vacío)</p>";
    }
    const std::size_t total = lineas.size();
    std::string h;
    if (total > cuantas) {
        h += "<p class=\"tenue\">Las últimas " + std::to_string(cuantas) + " líneas de "
             + std::to_string(total) + ".</p>";
        lineas.erase(lineas.begin(), lineas.end() - static_cast<long>(cuantas));
    }
    std::string texto;
    for (const std::string& l : lineas) {
        texto += l + "\n";
    }
    return h + "<pre>" + H::escapaHtml(B::trim(texto)) + "</pre>";
}

// Los trabajos del daemon, que llegan como una línea `JOB={…}` por trabajo.
// El panel necesita saber DE QUÉ MÁQUINA son los trabajos y el testigo de la sesión,
// porque desde aquí se pueden cancelar. Antes solo pintaba, y por eso le bastaba el texto.
// Con qué otras máquinas puede hablar el daemon de esta, y con qué nombre se conoce a sí
// misma.
//
// Lo de «a sí misma» va primero y en grande porque su ausencia es un fallo que no se ve: sin
// ese dato, una nivelación GSA contra un dataset de la propia máquina no reconoce el destino
// como propio, se va por el camino remoto y registra «no hay credenciales del par» siendo el
// par uno mismo. Pasó de verdad en esta instalación.
std::string panelPares(const std::string& crudo, const std::string& conn,
                       const std::string& testigo, bool daemonVivo) {
    const PR::Vista v = PR::analiza(crudo);
    std::string h;
    if (v.self.empty()) {
        // Dos causas y no se distinguen desde aquí: que no lo tenga puesto, o que su daemon
        // sea anterior a que se informara de ello. Se dicen las dos.
        h += "<p class=\"malo\">"
             + H::escapaHtml(T("t_web_pares_sin_self",
                               "Esta máquina no ha dicho con qué nombre la llama usted. O no lo "
                               "tiene puesto —y entonces NO puede nivelar contra un dataset suyo "
                               "propio, sin avisar de ello—, o su daemon es anterior a esta "
                               "comprobación. Entregar las credenciales arregla lo primero."))
             + "</p>";
    } else {
        h += "<p>" + H::escapaHtml(T("t_web_pares_self", "Se identifica como:")) + " <b>"
             + H::escapaHtml(v.self) + "</b></p>";
    }
    if (v.pares.empty()) {
        h += "<p class=\"vacio\">"
             + H::escapaHtml(T("t_web_pares_ninguno",
                               "(su daemon no puede llamar a ninguna otra máquina)"))
             + "</p>";
    } else {
        std::vector<std::vector<std::string>> filas;
        for (const PR::Par& par : v.pares) {
            filas.push_back({H::escapaHtml(par.id), H::escapaHtml(par.host),
                             std::to_string(par.puerto)});
        }
        h += tabla({T("t_cab_id", "ID"), T("t_cab_host", "HOST"), T("t_cab_puerto", "PUERTO")},
                   filas);
    }
    if (!daemonVivo) {
        return h;
    }
    // Entregar credenciales va por confirmación: lo que se manda son las claves privadas de
    // las OTRAS máquinas, y con ellas esta puede hablar con todas como si fuera usted.
    h += grupoDeAcciones(
        T("t_web_pares_grupo", "Credenciales"),
        "<div>"
            + enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(conn)
                         + "&raiz=" + H::haciaUrl(conn) + "&que=entregar-pares",
                     T("t_web_pares_entregar", "Entregar las credenciales de las demás…"))
            + "</div>");
    // La dirección de escucha se ofrece como TRES opciones y no como un campo libre: el
    // daemon solo admite esas, porque el cliente llega por un túnel contra 127.0.0.1 y una
    // dirección suelta le cortaría el acceso. Un campo libre solo serviría para escribir algo
    // que va a ser rechazado.
    std::string esc;
    for (const std::string& dir : PR::direccionesDeEscucha()) {
        esc += "<div>"
               + enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(dir)
                            + "&raiz=" + H::haciaUrl(conn) + "&que=escucha-pares",
                        B::format(T("t_web_pares_escucha", "Atender en %1…"), {dir}))
               + "</div>";
    }
    h += grupoDeAcciones(T("t_web_pares_escuchagr", "Dónde atiende su daemon"), esc);
    return h;
}

std::string panelTrabajos(const std::string& crudo, const std::string& conn,
                          const std::string& testigo) {
    std::vector<std::vector<std::string>> filas;
    for (const std::string& l : B::split(crudo, "\n", true)) {
        if (!B::startsWith(B::trim(l), "JOB=")) {
            continue;
        }
        B::json::Value j;
        std::string errJ;
        if (!B::json::parse(B::trim(l).substr(4), j, &errJ)) {
            continue;
        }
        const std::string estado = j["state"].toString();
        // Solo se ofrece cancelar lo que puede cancelarse.
        //
        // Un trabajo terminado, fallado o ya cancelado no tiene nada que parar, y un botón
        // que no hace nada es peor que ninguno: quien lo pulsa cree que ha pasado algo. El
        // daemon rechazaría la orden igual, pero el usuario se enteraría por un error en vez
        // de por la ausencia del botón.
        const std::string idTrabajo = j["id"].toString();
        const std::string cancelar =
            (estado == "running" || estado == "queued")
                ? boton(conn, idTrabajo, idTrabajo, "cancelar-trabajo",
                        T("t_web_job_cancelar", "Cancelar"), testigo, std::string(), true)
                : std::string();
        filas.push_back({H::escapaHtml(j["id"].toString()),
                         H::escapaHtml(j["type"].toString()),
                         H::escapaHtml(j["snap"].toString()),
                         estado == "done" ? std::string("done")
                                          : "<span class=\"malo\">" + H::escapaHtml(estado)
                                                + "</span>",
                         H::escapaHtml(bytesLegibles(std::to_string(j["bytes"].toInt()))),
                         H::escapaHtml(fechaIso(j["started"].toString())),
                         H::escapaHtml(j["error"].toString()),
                         cancelar});
    }
    if (filas.empty()) {
        return "<p class=\"vacio\">(no hay trabajos en esta máquina)</p>";
    }
    return tabla({T("t_web_id_474ae5", "Id"), T("t_tipo_6cc619", "Tipo"), T("t_web_instantanea_659df4", "Instantánea"), T("t_status_001", "Estado"), T("t_web_bytes_8e5fda", "Bytes"), "Empezó", T("t_web_error_7f2f6a", "Error"),
                  T("t_web_job_accion", "")}, filas);
}

// ── La tercera ventana: el REGISTRO ──────────────────────────────────────────
//
// Debajo de las dos columnas, plegable y plegada de fábrica, con las mismas pestañas que la
// interfaz de Qt salvo «Ajustes» y «Cambios pendientes» —que no son registros: uno es
// configuración y el otro un plan de trabajo, y meterlos aquí solo era herencia—.
//
// **Sin JavaScript unas pestañas no son pestañas**: cambiar de pestaña es recargar. Por eso
// cada una es un enlace que pone `?log=<clave>` en la URL, y la activa se pinta distinta.
// La consecuencia buena es que la pestaña abierta va en la dirección: se puede guardar en
// marcadores y sobrevive a una recarga, cosa que en Qt no pasa.
//
// Y el registro NO se refresca solo. Se podría con `<meta http-equiv="refresh">`, que es
// HTML puro y la política de contenido admite, pero recargaría la página entera cada pocos
// segundos: parpadeo, pérdida de lo que uno tuviera abierto y —lo que de verdad importa—
// una consulta a la máquina en cada vuelta por cada marco abierto, que es justo lo que se
// acaba de quitar. Hay un botón de refrescar, que es explícito y cuesta lo que cuesta.

// La pestaña activa, tal como viaja en la URL.
struct PestanaLog {
    std::string clave;      // «combinado», «term:local», «daemon:local», «trabajos:local»
    std::string tipo;       // «combinado», «term», «daemon», «trabajos»
    std::string conexion;   // vacío en «combinado»
};

PestanaLog pestanaDesde(const std::string& crudo) {
    PestanaLog p;
    p.clave = B::trim(crudo);
    const std::size_t i = p.clave.find(':');
    if (i == std::string::npos) {
        p.tipo = p.clave;
        return p;
    }
    p.tipo = p.clave.substr(0, i);
    p.conexion = p.clave.substr(i + 1);
    return p;
}

// El registro de este servidor, en una tabla. `soloDe` vacío = todas las máquinas, que es
// la pestaña «Combinado»; con un identificador dentro es el «Terminal» de esa máquina.
std::string panelApuntes(const std::string& soloDe) {
    std::vector<std::vector<std::string>> filas;
    // Del más reciente al más viejo: lo que uno acaba de hacer es lo que quiere ver, y en
    // un anillo de 500 lo de abajo del todo casi nunca importa.
    for (auto it = g_apuntes.rbegin(); it != g_apuntes.rend(); ++it) {
        if (!soloDe.empty() && it->conexion != soloDe) {
            continue;
        }
        std::string estado;
        if (!it->hablo) {
            estado = "<span class=\"malo\">sin respuesta</span>";
        } else if (it->rc != 0) {
            estado = "<span class=\"malo\">rc=" + std::to_string(it->rc) + "</span>";
        } else {
            estado = "ok";
        }
        filas.push_back({H::escapaHtml(it->cuando),
                         H::escapaHtml(it->conexion),
                         H::escapaHtml(it->verbo),
                         estado,
                         std::to_string(it->ms) + " ms",
                         H::escapaHtml(it->detalle)});
    }
    if (filas.empty()) {
        return "<p class=\"vacio\">(este servidor no ha hablado todavía con ninguna máquina"
               + std::string(soloDe.empty() ? "" : " «" + H::escapaHtml(soloDe) + "»") + ")</p>";
    }
    return tabla({T("t_web_hora_f07dc2", "Hora"), T("t_web_maquina_bf8c85", "Máquina"), "Qué se pidió", T("t_web_resultado_c7589d", "Resultado"), T("t_web_tardo_f56f03", "Tardó"), T("t_conn_agent_detail_001", "Detalle")}, filas);
}

// Las pestañas, en DOS filas, que es la forma que tiene la interfaz de Qt: una por
// conexión arriba, y dentro de la elegida las suyas —Terminal, Daemon, Transferencias—.
//
// Antes estaban las cuatro máquinas a la vez con sus tres pestañas cada una: trece enlaces
// en cinco filas, y ninguno decía cuál se estaba mirando sin buscar el resaltado. Con dos
// niveles la fila de arriba dice DÓNDE se está y la de abajo QUÉ de ahí, que es una
// pregunta cada una.
//
// Al pulsar una máquina se abre su «Terminal» y no su «Daemon»: el Terminal sale de
// memoria y no cuesta nada, y así cambiar de máquina no dispara una consulta que a lo
// mejor no se quería. El daemon está a un clic.
std::string pestanasDelLog(const std::vector<B::ConnectionProfile>& perfiles,
                           const std::string& base, const PestanaLog& activa) {
    const auto una = [&](const std::string& clave, const std::string& texto, bool esta) {
        return "<a class=\"pest" + std::string(esta ? " activa" : "") + "\" href=\""
               + H::escapaHtml(base + "log=" + H::haciaUrl(clave)) + "\">" + H::escapaHtml(texto)
               + "</a>";
    };
    std::string h = "<div class=\"pestanas\">";
    h += una("combinado", T("t_combined_log001", "Log combinado"), activa.tipo == "combinado");
    for (const B::ConnectionProfile& p : perfiles) {
        const std::string id = p.id.empty() ? p.name : p.id;
        h += una("term:" + id, id, activa.conexion == id);
    }
    h += "</div>";
    // La fila de dentro solo cuando hay una máquina elegida: sin ella no hay nada que
    // dividir, y una fila de pestañas que no se puede pulsar es un adorno.
    if (!activa.conexion.empty()) {
        h += "<div class=\"pestanas dentro\">";
        h += una("term:" + activa.conexion, T("t_web_terminal_a1f52c", "Terminal"), activa.tipo == "term");
        h += una("daemon:" + activa.conexion, T("t_conn_agent_001", "Daemon"), activa.tipo == "daemon");
        h += una("trabajos:" + activa.conexion, T("t_jobs_tab_001", "Transferencias"), activa.tipo == "trabajos");
        h += "</div>";
    }
    return h;
}

// La ventana entera. `cargado` es lo que se haya ido a buscar para la pestaña activa; las
// que salen de memoria —Combinado y Terminal— no lo necesitan.
std::string ventanaDelLog(const std::vector<B::ConnectionProfile>& perfiles,
                          const std::string& base, const PestanaLog& activa,
                          const std::string& cargado, const std::string& testigo) {
    std::string cuerpo = pestanasDelLog(perfiles, base, activa);
    cuerpo += "<div class=\"logcuerpo\">";
    if (activa.clave.empty()) {
        cuerpo += "<p class=\"vacio\">(elija una pestaña)</p>";
    } else if (activa.tipo == "combinado") {
        cuerpo += panelApuntes(std::string());
    } else if (activa.tipo == "term") {
        cuerpo += panelApuntes(activa.conexion);
    } else {
        cuerpo += cargado;
    }
    cuerpo += "</div>";
    // Refrescar es explícito: un enlace a esta misma URL. Y para el daemon, el latido, que
    // es el botón que la interfaz de Qt tiene en esa misma pestaña.
    if (!activa.clave.empty()) {
        cuerpo += "<p>" + enlace(base + "log=" + H::haciaUrl(activa.clave), T("t_refresh_conn_ctx001", "Refrescar"));
        if (activa.tipo == "daemon" && !activa.conexion.empty()) {
            cuerpo += " · ";
            cuerpo += "<form class=\"enlinea\" method=\"post\" action=\"/accion\">"
                      + campoTestigo(testigo)
                      + "<input type=\"hidden\" name=\"c\" value=\"" + H::escapaHtml(activa.conexion)
                      + "\"><input type=\"hidden\" name=\"o\" value=\""
                      + H::escapaHtml(activa.conexion)
                      + "\"><input type=\"hidden\" name=\"que\" value=\"latido\">"
                        "<button type=\"submit\">Latido</button></form>";
        }
        cuerpo += "</p>";
    }
    return marco(T("t_web_registro_b6965e", "Registro"), cuerpo, !activa.clave.empty());
}

// Las dos columnas. El árbol va en un panel que se queda quieto al desplazar la derecha:
// perder de vista dónde se está es lo que hace inservible un árbol grande.
std::string envuelveDosPaneles(const std::string& titulo, const std::string& migas,
                               const std::string& izq, const std::string& der,
                               const std::string& abajo, const std::string& aviso,
                               const std::string& testigo) {
    std::string cuerpo = aviso;
    cuerpo += "<div class=\"dos\"><div class=\"izq\">" + izq + "</div>";
    cuerpo += "<div class=\"der\">" + der + "</div></div>";
    // La tercera ventana va DEBAJO de las dos y a todo lo ancho, no dentro de una columna:
    // un registro en media pantalla obliga a desplazarse en horizontal por cada línea.
    cuerpo += "<div class=\"abajo\">" + abajo + "</div>";
    return envuelve(titulo, migas, cuerpo, testigo, true);
}

// ── Las vistas del panel derecho ─────────────────────────────────────────────
//
// Cada una devuelve un TROZO, no una página: quien las llama ya tiene el árbol de la
// izquierda montado y solo necesita el contenido del hueco de la derecha.

// Las propiedades, con las MODIFICABLES editables en su propia fila.
//
// Antes había abajo dos cajas sueltas —«Propiedad» y «Valor»— donde había que teclear el
// nombre a mano. Eso obliga a copiarlo de la tabla de arriba, y un nombre mal escrito no da
// error: `zfs set` crea una propiedad de usuario nueva si lleva dos puntos, y si no, falla
// con un mensaje que no dice cuál de las dos cajas estaba mal.
//
// Ahora cada fila que se puede cambiar trae su valor dentro de un campo, con lo que hay
// ahora mismo, y su botón al lado. El nombre no se teclea: es el de la fila.
//
// **Qué se puede cambiar lo decide `base/zfsprops`**, la misma regla que usa la interfaz de
// Qt para pintar o no una celda editable. No es una lista escrita aquí: estaba TRES veces
// dentro de la GUI y ahora está una vez en la capa base.
std::string panelPropiedades(const std::string& conn, const std::string& raiz,
                             const std::string& objeto, const std::vector<L::Propiedad>& props,
                             bool soloCapacidades, const std::string& tipoDataset,
                             ZP::Plataforma plataforma, bool editables,
                             const std::string& testigo) {
    // El valor de `readonly` que declara el propio ZFS para ESTE objeto: una propiedad
    // editable en general no lo es en un dataset montado de solo lectura.
    std::string readonlyDelObjeto = "off";
    for (const L::Propiedad& pr : props) {
        if (pr.nombre == "readonly") {
            readonlyDelObjeto = pr.valor;
            break;
        }
    }

    std::string h = "<table><thead><tr><th>Propiedad</th><th>Valor</th><th>Origen</th></tr>"
                    "</thead><tbody>";
    std::size_t cuantas = 0;
    for (const L::Propiedad& pr : props) {
        // Las «capacidades» de un pool son sus propiedades `feature@…`, no otra consulta:
        // `zpool get all` ya las trae mezcladas con las demás y separarlas es un filtro.
        const bool esCapacidad = B::startsWith(pr.nombre, "feature@");
        if (esCapacidad != soloCapacidades) {
            continue;
        }
        ++cuantas;
        // `readonly` a sí misma no se le aplica: si estuviera en «on» no habría forma de
        // volver a ponerla en «off» desde aquí.
        const std::string ro = (pr.nombre == "readonly") ? std::string("off") : readonlyDelObjeto;
        const bool sePuede = editables
                             && ZP::editableEnLinea(pr.nombre, tipoDataset, pr.origen, ro,
                                                    plataforma);
        h += "<tr><th class=\"prop\">"
             + H::escapaHtml(soloCapacidades ? pr.nombre.substr(8) : pr.nombre) + "</th><td>";
        if (!sePuede) {
            h += H::escapaHtml(pr.valor);
        } else {
            h += "<form class=\"enlinea\" method=\"post\" action=\"/accion\">"
                 + campoTestigo(testigo)
                 + "<input type=\"hidden\" name=\"c\" value=\"" + H::escapaHtml(conn) + "\">"
                 + "<input type=\"hidden\" name=\"o\" value=\"" + H::escapaHtml(objeto) + "\">"
                 + "<input type=\"hidden\" name=\"raiz\" value=\"" + H::escapaHtml(raiz) + "\">"
                 + "<input type=\"hidden\" name=\"volver\" value=\"props\">"
                 + "<input type=\"hidden\" name=\"que\" value=\"set\">"
                 + "<input type=\"hidden\" name=\"prop\" value=\"" + H::escapaHtml(pr.nombre) + "\">";
            // Con lista cerrada, un desplegable; sin ella, un campo. Un desplegable no
            // deja escribir «lz4x» donde solo cabe «lz4», y de paso enseña qué hay.
            const std::vector<std::string>& valores = ZP::valoresDe(pr.nombre);
            if (!valores.empty()) {
                h += "<select name=\"valor\">";
                bool estaElActual = false;
                for (const std::string& v : valores) {
                    const bool sel = (v == pr.valor);
                    estaElActual = estaElActual || sel;
                    h += "<option" + std::string(sel ? " selected" : "") + " value=\""
                         + H::escapaHtml(v) + "\">" + H::escapaHtml(v) + "</option>";
                }
                // El valor de AHORA, si no está en la lista, se añade y sale elegido: si no,
                // el desplegable enseñaría otro y un descuido lo cambiaría sin querer.
                if (!estaElActual && !pr.valor.empty()) {
                    h += "<option selected value=\"" + H::escapaHtml(pr.valor) + "\">"
                         + H::escapaHtml(pr.valor) + "</option>";
                }
                h += "</select>";
            } else {
                h += "<input name=\"valor\" value=\"" + H::escapaHtml(pr.valor) + "\">";
            }
            h += "<button type=\"submit\">Aplicar</button></form>";
        }
        h += "</td><td class=\"tenue\">" + H::escapaHtml(pr.origen) + "</td></tr>";
    }
    h += "</tbody></table>";
    if (cuantas == 0) {
        return "<p class=\"vacio\">(no hay nada aquí)</p>";
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
    std::string h = tabla({T("t_poolcrt_auto004", "Nombre"), T("t_tipo_6cc619", "Tipo"), T("t_poolcrt_auto018", "Tamaño")}, filas);
    h += "<p>" + enlace("/dav/" + H::haciaUrl(conn) + "/" + H::haciaUrl(objeto) + "/",
                        T("t_web_abrir_por_we_49f221", "Abrir por WebDAV"))
         + " <span class=\"tenue\">— para montarlo en el explorador de archivos</span></p>";
    return h;
}

// La programación de instantáneas: la de ESTE dataset, editable, y las que haya puestas por
// debajo.
//
// Antes era un volcado de `zfs get -r` con una fila por propiedad, y eso lo hacía
// inservible por dos motivos distintos:
//
//   - **Salían las instantáneas.** Una instantánea no programa instantáneas. Aparecían
//     porque las propiedades de usuario se heredan y la consulta es recursiva, así que un
//     solo dataset programado llenaba la tabla con una fila por cada una de sus
//     instantáneas por cada uno de los nueve ajustes.
//   - **Salían las HEREDADAS, y eso no es ruido: engaña.** El planificador del daemon mira
//     `org.fc16.gsa:activado` SOLO si está puesto en local —`daemon_main.cpp:4659`—, así
//     que a un descendiente que hereda «activado=on» no se le hace ninguna instantánea.
//     Enseñarlo como programado promete algo que no va a ocurrir.
//
// Ahora hay una fila por DATASET con programación propia, leída por la misma función que
// usa el planificador para decidir qué hace, y un formulario para cambiarla.

// Los ajustes de un dataset, sacados del volcado `name, property, value, source`.
struct ProgramacionLeida {
    B::gsa::Programacion prog;
    bool local{false};   // puesta AQUÍ, que es lo único que el planificador mira
};

std::map<std::string, ProgramacionLeida> leeProgramaciones(const std::string& salidaTsv) {
    // Las instantáneas se descartan en la puerta: no hay ninguna decisión más abajo que
    // dependa de ellas.
    std::map<std::string, std::map<std::string, std::string>> props;
    std::map<std::string, bool> hayLocal;
    for (const std::string& linea : B::split(salidaTsv, "\n", true)) {
        const std::vector<std::string> col = B::split(linea, "\t", false);
        if (col.size() < 4) {
            continue;
        }
        const std::string& ds = col[0];
        if (ds.find('@') != std::string::npos) {
            continue;
        }
        props[ds][col[1]] = B::trim(col[2]);
        if (B::trim(col[3]) == "local") {
            hayLocal[ds] = true;
        }
    }
    std::map<std::string, ProgramacionLeida> out;
    for (const auto& kv : props) {
        ProgramacionLeida r;
        B::gsa::Motivo porQue;
        if (!B::gsa::desdePropiedades(kv.second, r.prog, porQue)) {
            continue;   // un valor que no se entiende: no se inventa una programación
        }
        r.local = hayLocal[kv.first];
        out[kv.first] = r;
    }
    return out;
}

std::string casilla(const std::string& nombre, const std::string& etiqueta, bool puesta) {
    return "<label class=\"campo\"><input type=\"checkbox\" name=\"" + H::escapaHtml(nombre)
           + "\" value=\"1\"" + (puesta ? " checked" : "") + "> " + H::escapaHtml(etiqueta)
           + "</label>";
}

std::string numero(const std::string& nombre, const std::string& etiqueta, int valor) {
    return "<label class=\"campo\">" + H::escapaHtml(etiqueta)
           + " <input type=\"number\" min=\"0\" name=\"" + H::escapaHtml(nombre) + "\" value=\""
           + std::to_string(valor) + "\"></label>";
}

std::string panelProgramacion(const std::string& conn, const std::string& raiz,
                              const std::string& sel, const std::string& salidaTsv,
                              const std::string& testigo) {
    const auto todas = leeProgramaciones(salidaTsv);
    const auto itYo = todas.find(sel);
    const ProgramacionLeida mia = itYo == todas.end() ? ProgramacionLeida{} : itYo->second;

    std::string h = "<p class=\"tenue\">El planificador solo mira la programación puesta en el "
                    "propio dataset. Lo que se hereda del padre NO se aplica: para que a un "
                    "dataset se le hagan instantáneas, o se programa aquí, o el padre tiene "
                    "puesta la recursiva.</p>";

    std::string f = "<form method=\"post\" action=\"/accion\">" + campoTestigo(testigo)
                    + "<input type=\"hidden\" name=\"c\" value=\"" + H::escapaHtml(conn) + "\">"
                    + "<input type=\"hidden\" name=\"o\" value=\"" + H::escapaHtml(sel) + "\">"
                    + "<input type=\"hidden\" name=\"raiz\" value=\"" + H::escapaHtml(raiz) + "\">"
                    + "<input type=\"hidden\" name=\"volver\" value=\"gsa\">"
                    + "<input type=\"hidden\" name=\"que\" value=\"programar\">";
    f += "<div class=\"fila\">" + casilla("activado", T("t_web_activada_ae4df8", "Activada"), mia.prog.activado)
         + casilla("recursivo", T("t_web_recursiva_cubr_6c95e7", "Recursiva (cubre los descendientes)"), mia.prog.recursivo)
         + "</div>";
    f += "<div class=\"fila\">" + numero("horario", T("t_web_horarias_5399f3", "Horarias"), mia.prog.horario)
         + numero("diario", T("t_web_diarias_31be0d", "Diarias"), mia.prog.diario)
         + numero("semanal", T("t_ctx_snap_group_weekly", "Semanales"), mia.prog.semanal)
         + numero("mensual", T("t_ctx_snap_group_monthly", "Mensuales"), mia.prog.mensual)
         + numero("anual", T("t_ctx_snap_group_yearly", "Anuales"), mia.prog.anual) + "</div>";
    f += "<p class=\"tenue\">Cada número es cuántas se guardan de esa clase. Un cero es «no "
         "hagas ninguna», no «guárdalas todas».</p>";
    f += "<div class=\"fila\">" + casilla("nivelar", T("t_web_nivelar_con_el_e446c5", "Nivelar con el destino"), mia.prog.nivelar)
         + "<label class=\"campo\">" + H::escapaHtml(T("t_web_destino_c1a0f9", "Destino"))
         + " <input name=\"destino\" placeholder=\"zfsm://máquina/pool/dataset\" value=\""
         + H::escapaHtml(B::gsa::destinoComoUrl(mia.prog.destino)) + "\"></label></div>";
    f += "<button type=\"submit\">Guardar</button></form>";
    if (mia.local) {
        f += boton(conn, sel, raiz, "desprogramar", T("t_web_quitarla_de_478a25", "Quitarla de aquí"), testigo, std::string(),
                   true);
    }
    h += grupoDeAcciones(mia.local ? "Programación de " + sel + " (puesta aquí)"
                                   : "Programación de " + sel + " (sin poner)",
                         f);

    // Y las que haya puestas POR DEBAJO, que es lo que uno quiere saber al mirar un pool.
    std::vector<std::vector<std::string>> filas;
    for (const auto& kv : todas) {
        if (!kv.second.local || kv.first == sel) {
            continue;
        }
        const B::gsa::Programacion& p = kv.second.prog;
        filas.push_back({enlace(urlDe(conn, raiz, kv.first, Vista::Programacion), kv.first),
                         p.activado ? "sí" : "no", p.recursivo ? "sí" : "no",
                         std::to_string(p.horario), std::to_string(p.diario),
                         std::to_string(p.semanal), std::to_string(p.mensual),
                         std::to_string(p.anual),
                         p.nivelar ? H::escapaHtml(B::gsa::destinoComoUrl(p.destino))
                                   : std::string()});
    }
    h += "<div class=\"grupotit\">Programaciones puestas por debajo</div>";
    h += tabla({T("t_web_dataset_105268", "Dataset"), T("t_web_activada_ae4df8", "Activada"), T("t_web_recursiva_9c3c8d", "Recursiva"), "Hor.", "Dia.", "Sem.", "Men.", "Anu.",
                "Nivela con"},
               filas);
    return h;
}

// Sincroniza, o cuenta lo que haría si `enSeco`.
//
// La pasada en seco va DIRECTA porque `rsync -n` solo enumera y vuelve enseguida. La de
// verdad va por `--job-submit`: puede tardar horas, y una petición HTTP colgada durante
// horas no es una forma de esperar. El daemon ya admite `--mutate-rsync-local` como trabajo
// —está en su lista de lanzables—, así que esto no necesita nada nuevo en el agente.
// El camino de rsync: dentro de una MISMA máquina Unix, donde está probado, hace delta y no
// hay socket de por medio.
bool lanzaRsync(zfsmgr::cli::Sesion& ses, const B::ConnectionProfile& quienLoHace,
                std::vector<std::string> args, bool enSeco, std::string& salida,
                std::string& err, std::string& idTrabajo) {
    if (!enSeco) {
        // En seco es rápido y su salida es LO QUE SE ENSEÑA, así que va directo. La de
        // verdad puede tardar horas y va como trabajo del daemon.
        args = PET::encola(args);
        if (args.empty()) {
            err = "esa orden no se puede encolar como trabajo";
            return false;
        }
    }
    int rc = 0;
    if (!llamaAgente(ses, quienLoHace, args, salida, err, rc, nullptr, enSeco ? 300000 : 60000)
        || rc != 0) {
        return false;
    }
    if (!enSeco) {
        idTrabajo = idDeTrabajoEn(salida);
        if (idTrabajo.empty()) {
            err = "el daemon aceptó la orden pero no dio identificador de trabajo";
            return false;
        }
    }
    return true;
}

bool sincroniza(zfsmgr::cli::Sesion& ses, const B::ConnectionProfile& perfilOrigen,
                const B::ConnectionProfile& perfilDestino, const SY::Plan& plan,
                bool mismaConexion, bool borrar, bool enSeco, bool verboso,
                std::string& salida, std::string& err, std::string& idTrabajo) {
    idTrabajo.clear();
    // Dentro de una misma máquina UNIX sigue yendo por rsync: está probado, hace delta y no
    // hay socket de por medio. En Windows no existe rsync, así que allí va por el árbol —el
    // daemon conectándose consigo mismo por el bucle local—, que es el mismo camino de entre
    // máquinas. Es la única diferencia que queda, y es por lo que hay en cada plataforma.
    const bool porRsync = mismaConexion && !B::transport::isWindowsConnection(perfilDestino);
    if (porRsync) {
        const std::string carga =
            SY::cargaRsync({{plan.rutaOrigen, plan.rutaDestino}}, borrar, enSeco, "", "");
        if (carga.empty()) {
            err = "no se pudo construir la orden de sincronización";
            return false;
        }
        return lanzaRsync(ses, perfilDestino, PET::copiaConRsync(carga), enSeco, salida, err,
                          idTrabajo);
    }

    // **Entre máquinas no interviene rsync.** El destino abre un puerto y devuelve un testigo
    // de un solo uso; el origen se conecta y le manda el árbol. Es el mismo transporte que
    // usa `zfs send` entre daemons, y por eso funciona igual con un extremo Windows, donde
    // rsync no existe.
    //
    // Los tres pasos —escuchar, averiguar con qué dirección ve el origen al destino, y
    // enviar— ya no se escriben aquí: son `transferencia::lanzaTrabajoDeArbol`, la misma
    // función que usan la ventana y el intérprete. Esta era la TERCERA copia de la
    // coreografía, y la única que además sabía pedir `--delete`; ahora eso es un parámetro.
    std::string salidaEnvio;
    const auto hecho = TR::lanzaTrabajoDeArbol(
        ses.transporte,
        [&ses](const B::ConnectionProfile& maquina, const std::vector<std::string>& args,
               int timeoutMs, std::string& salidaL, std::string& errL, int& rcL) {
            return llamaAgente(ses, maquina, args, salidaL, errL, rcL, nullptr, timeoutMs);
        },
        perfilOrigen, perfilDestino, plan.rutaOrigen, plan.rutaDestino, mismaConexion, verboso,
        /*comoTrabajo=*/!enSeco, borrar, enSeco, &salidaEnvio);
    if (hecho.fallo != TR::FalloTrabajo::Ninguno) {
        err = TR::etiquetaDe(hecho.fallo)
              + (hecho.detalle.empty() ? std::string() : ": " + hecho.detalle);
        return false;
    }
    salida = salidaEnvio;
    idTrabajo = hecho.id;
    return true;
}



// La confirmación de sincronizar, con la pasada EN SECO ya hecha.
//
// Sincronizar es la única de las seis que puede BORRAR en el destino, así que preguntar
// «¿seguro?» a secas no basta: hay que enseñar qué va a pasar. La pasada en seco la hace
// rsync con `-n`, y lo que se ve aquí es su salida literal.
//
// **Lo que se ve es lo que se ejecuta.** El interruptor de borrar no es una casilla del
// formulario: es un ENLACE que recarga esta misma página con la pasada en seco hecha ya con
// ese ajuste. Sin JavaScript no hay forma de rehacer la vista previa al marcar una casilla,
// y una vista previa que no corresponde a lo que se va a ejecutar es peor que ninguna.
std::string paginaConfirmarSincronizar(const std::string& conn, const std::string& destino,
                                       const DX::Extremo& origen, const std::string& raiz,
                                       bool borrar, bool huboFallo,
                                       const std::string& salidaSeco,
                                       const std::string& testigo) {
    std::string cuerpo;
    cuerpo += "<p>"
              + H::escapaHtml(B::format(
                    T("t_web_conf_sync",
                      "Se va a sincronizar «%1» → «%2» en «%3». Copia por ficheros: lo que "
                      "cambie en el origen se escribe encima del destino. Entre máquinas "
                      "viaja por el canal entre daemons, sin rsync."),
                    {origen.objeto, destino, conn}))
              + "</p>";
    if (borrar) {
        cuerpo += "<p class=\"peligro\">"
                  + H::escapaHtml(T("t_web_conf_sync_del",
                                    "Con borrado: lo que exista en el destino y NO exista en el "
                                    "origen se DESTRUYE. No hay vuelta atrás."))
                  + "</p>";
    }
    // El enlace lleva al otro modo, y de paso deja claro en cuál se está.
    cuerpo += "<p>" + H::escapaHtml(T("t_web_sync_modo", "Modo:")) + " <b>"
              + H::escapaHtml(borrar ? T("t_web_sync_con_del", "con borrado")
                                     : T("t_web_sync_sin_del", "sin borrado"))
              + "</b> — "
              + enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(destino)
                           + "&raiz=" + H::haciaUrl(raiz) + "&que=sincronizar-desde-origen"
                           + (borrar ? "" : "&del=1"),
                       borrar ? T("t_web_sync_ver_sin", "ver sin borrado")
                              : T("t_web_sync_ver_con", "ver con borrado"))
              + "</p>";
    if (huboFallo) {
        cuerpo += "<p class=\"peligro\">"
                  + H::escapaHtml(T("t_web_sync_seco_mal",
                                    "La pasada en seco falló, así que no hay nada que enseñar y "
                                    "no se ofrece ejecutarla."))
                  + "</p>";
        cuerpo += "<pre class=\"salida\">" + H::escapaHtml(salidaSeco) + "</pre>";
        cuerpo += "<p>" + enlace("/c/" + H::haciaUrl(conn) + "?sel=" + H::haciaUrl(destino),
                                 T("t_web_no_volver", "No, volver"))
                  + "</p>";
        return envuelve(T("t_web_sincronizar_t", "Sincronizar"), enlace("/", "ZFSMgr"), cuerpo,
                        testigo);
    }
    cuerpo += "<p class=\"tenue\">"
              + H::escapaHtml(T("t_web_sync_seco_es",
                                "Esto es la pasada en seco: lo que se haría, sin tocar nada."))
              + "</p>";
    cuerpo += "<pre class=\"salida\">"
              + H::escapaHtml(B::trim(salidaSeco).empty()
                                  ? T("t_web_sync_nada", "(no hay nada que copiar: ya están igual)")
                                  : salidaSeco)
              + "</pre>";
    cuerpo += boton(conn, destino, raiz, "sincronizar-desde-origen",
                    borrar ? T("t_web_si_sync_del", "Sí, sincronizar Y BORRAR lo que sobre")
                           : T("t_web_si_sync", "Sí, sincronizar"),
                    testigo,
                    borrar ? "<input type=\"hidden\" name=\"del\" value=\"1\">" : std::string(),
                    borrar);
    cuerpo += "<p>" + enlace("/c/" + H::haciaUrl(conn) + "?sel=" + H::haciaUrl(destino),
                             T("t_web_no_volver", "No, volver"))
              + "</p>";
    return envuelve(T("t_web_sincronizar_t", "Sincronizar"), enlace("/", "ZFSMgr"), cuerpo,
                    testigo);
}

// La página de confirmación de algo que destruye.
//
// Existe porque un botón que borra sin preguntar, en una página que se puede recargar, es
// una forma de perder datos por un clic de más. Dice EXACTAMENTE qué se va a hacer y sobre
// qué, igual que el intérprete antes de una orden destructiva.
std::string paginaConfirmar(const std::string& conn, const std::string& objeto,
                            const std::string& que, const std::string& raiz,
                            const std::string& testigo, const DX::Extremo& origen) {
    std::string texto;
    if (que == "borrar-instantanea") {
        texto = B::format(T("t_web_conf_delsnap",
                            "Se va a DESTRUIR la instantánea «%1» en «%2». Lo que hubiera en "
                            "ella se pierde y no hay vuelta atrás."),
                          {objeto, conn});
    } else if (que == "borrar-dataset") {
        texto = B::format(T("t_web_conf_delds",
                            "Se va a DESTRUIR el dataset «%1» en «%2» con todo lo que "
                            "contenga. No hay vuelta atrás."),
                          {objeto, conn});
    } else if (que == "rollback") {
        texto = B::format(T("t_web_conf_rollback",
                            "Se va a volver el dataset al estado de «%1» en «%2». Todo lo "
                            "escrito DESPUÉS de esa instantánea se pierde."),
                          {objeto, conn});
    } else if (que == "mover-desde-origen") {
        // Mover NO destruye nada: es un `zfs rename`, los datos no se copian ni se
        // borran. Pero SÍ cambia de sitio un dataset entero, y con él la ruta de montaje
        // de todo lo que cuelgue, así que se pregunta igual. Lo que se dice aquí es
        // exactamente el nombre que va a tener después, que es lo único que importa.
        texto = B::format(T("t_web_conf_mover",
                            "Se va a MOVER «%1» a «%2», dentro de «%3». Es un renombrado: "
                            "los datos no se copian, pero cambia la ruta de montaje de ese "
                            "dataset y de todo lo que cuelgue de él."),
                          {origen.objeto, DX::destinoDeMover(origen, DX::Extremo{conn, objeto}),
                           conn});
    } else if (que == "pool-destroy") {
        texto = B::format(T("t_web_conf_destroypool",
                            "Se va a DESTRUIR el pool «%1» de «%2» con TODO lo que contenga: "
                            "sus datasets, sus instantáneas y sus datos. No hay vuelta atrás, "
                            "y no es lo mismo que exportarlo — exportar lo deja intacto para "
                            "volver a importarlo, esto no."),
                          {objeto, conn});
    } else if (que == "entregar-pares") {
        texto = B::format(T("t_web_conf_pares",
                            "Se le van a entregar a «%1» las credenciales de las DEMÁS "
                            "conexiones. Con ellas, esa máquina puede hablar con todas como si "
                            "fuera usted: no es una lista de nombres, son las claves privadas. "
                            "Hágalo solo con máquinas suyas."),
                          {conn});
    } else if (que == "escucha-pares") {
        texto = B::format(T("t_web_conf_escucha",
                            "El daemon de «%1» pasará a atender en %2 y SE REINICIARÁ. Mientras "
                            "tanto esa máquina no contesta. Con un comodín, su puerto queda "
                            "alcanzable desde la red —protegido por mTLS con el certificado de "
                            "cliente fijado—."),
                          {conn, objeto});
    } else if (que == "pool-upgrade") {
        texto = B::format(T("t_web_conf_upgrade",
                            "Se va a subir la versión del pool «%1» de «%2», y eso NO se puede "
                            "deshacer. Después, ese pool ya no se podrá importar en una máquina "
                            "con una versión de OpenZFS anterior a la que lo subió."),
                          {objeto, conn});
    } else if (que == "pool-reguid") {
        texto = B::format(T("t_web_conf_reguid",
                            "Se va a cambiar el identificador único del pool «%1» de «%2». Los "
                            "datos no se tocan, pero ese identificador es lo que otras máquinas "
                            "usan para saber que este pool es este: cualquier cosa que lo tuviera "
                            "anotado deja de reconocerlo."),
                          {objeto, conn});
    } else if (que == "pool-export") {
        texto = B::format(T("t_web_conf_export",
                            "Se va a EXPORTAR el pool «%1» de «%2». Los datos no se borran, "
                            "pero el pool deja de estar en esa máquina hasta que alguien lo "
                            "importe, y lo que estuviera usándolo se queda sin él."),
                          {objeto, conn});
    } else if (que == "borrar-conexion") {
        texto = B::format(T("t_web_conf_delconn",
                            "Se va a quitar la conexión «%1» de la lista. No se toca nada en "
                            "esa máquina: ni sus pools, ni sus datos, ni el daemon que tenga "
                            "instalado. Lo que se borra es lo que este cliente sabe de ella, "
                            "incluidas sus credenciales guardadas."),
                          {conn});
    } else if (que == "instalar-daemon") {
        texto = B::format(T("t_web_conf_daemon",
                            "Se va a REEMPLAZAR el binario del daemon en «%1» y reiniciar su "
                            "servicio. Mientras dure, esa máquina deja de contestar; lo que "
                            "estuviera en marcha allí —un scrub, una transferencia— se corta."),
                          {conn});
    } else {
        texto = T("t_web_conf_nada", "Acción desconocida.");
    }
    std::string cuerpo = "<p>" + H::escapaHtml(texto) + "</p>";
    if (que == "instalar-daemon") {
        // La contraseña de sudo, si hace falta, se pide AQUÍ y viaja en el cuerpo del POST
        // —cifrado por TLS—, nunca en la URL: una URL se queda en el historial del
        // navegador y en el registro de cualquier intermediario. Es la misma regla que
        // impide pasarla por argumento al agente, donde la vería cualquier «ps».
        cuerpo += "<p class=\"tenue\">Si la conexión necesita sudo y no tiene la contraseña "
                  "guardada, póngala aquí. Si la tiene guardada, deje el campo vacío.</p>";
        cuerpo += boton(conn, conn, raiz, que, T("t_web_si_instalar_ac8069", "Sí, instalar"), testigo,
                        "<label>Contraseña de sudo "
                        "<input type=\"password\" name=\"sudo\" autocomplete=\"off\"></label> ",
                        true);
        cuerpo += "<p>" + enlace("/c/" + H::haciaUrl(conn), "No, volver") + "</p>";
        return envuelve(T("t_web_confirmar_81b4b6", "Confirmar"), enlace("/", "ZFSMgr"), cuerpo, testigo);
    }
    if (que == "mover-desde-origen") {
        cuerpo += boton(conn, objeto, raiz, que, T("t_web_si_mover", "Sí, moverlo"), testigo,
                        std::string(), true);
        cuerpo += "<p>" + enlace("/c/" + H::haciaUrl(conn) + "?sel=" + H::haciaUrl(objeto),
                                 T("t_web_no_volver", "No, volver"))
                  + "</p>";
        return envuelve(T("t_web_confirmar_81b4b6", "Confirmar"), enlace("/", "ZFSMgr"), cuerpo,
                        testigo);
    }
    if (que == "pool-destroy") {
        // Además de confirmar, hay que escribir el nombre. Es la única acción de la web que
        // borra un pool entero, y un clic de más no debería poder hacerlo.
        cuerpo += boton(conn, objeto, raiz, que, T("t_web_si_destruir_pool", "Sí, destruirlo"),
                        testigo,
                        "<label class=\"campo\">"
                            + H::escapaHtml(B::format(T("t_web_pool_teclee",
                                                        "Escriba «%1» para confirmar"),
                                                      {objeto}))
                            + " <input name=\"confirma\" required autocomplete=\"off\"></label> ",
                        true);
        cuerpo += "<p>" + enlace("/c/" + H::haciaUrl(conn) + "/" + H::haciaUrl(objeto),
                                 T("t_web_no_volver", "No, volver"))
                  + "</p>";
        return envuelve(T("t_web_confirmar_81b4b6", "Confirmar"), enlace("/", "ZFSMgr"), cuerpo,
                        testigo);
    }
    if (que == "pool-export") {
        cuerpo += boton(conn, objeto, raiz, que, T("t_web_si_exportar", "Sí, exportarlo"),
                        testigo, std::string(), true);
        cuerpo += "<p>" + enlace("/c/" + H::haciaUrl(conn) + "/" + H::haciaUrl(objeto),
                                 T("t_web_no_volver", "No, volver"))
                  + "</p>";
        return envuelve(T("t_web_confirmar_81b4b6", "Confirmar"), enlace("/", "ZFSMgr"), cuerpo,
                        testigo);
    }
    if (que == "entregar-pares" || que == "escucha-pares") {
        cuerpo += boton(conn, objeto, raiz, que,
                        que == "entregar-pares" ? T("t_web_si_pares", "Sí, entregarlas")
                                                : T("t_web_si_escucha", "Sí, reiniciarlo"),
                        testigo, std::string(), true);
        cuerpo += "<p>" + enlace("/c/" + H::haciaUrl(conn) + "?v=pares",
                                 T("t_web_no_volver", "No, volver"))
                  + "</p>";
        return envuelve(T("t_web_confirmar_81b4b6", "Confirmar"), enlace("/", "ZFSMgr"), cuerpo,
                        testigo);
    }
    if (que == "pool-upgrade" || que == "pool-reguid") {
        cuerpo += boton(conn, objeto, raiz, que,
                        que == "pool-upgrade" ? T("t_web_si_upgrade", "Sí, subir la versión")
                                              : T("t_web_si_reguid", "Sí, cambiarlo"),
                        testigo, std::string(), true);
        cuerpo += "<p>" + enlace("/c/" + H::haciaUrl(conn) + "/" + H::haciaUrl(objeto),
                                 T("t_web_no_volver", "No, volver"))
                  + "</p>";
        return envuelve(T("t_web_confirmar_81b4b6", "Confirmar"), enlace("/", "ZFSMgr"), cuerpo,
                        testigo);
    }
    if (que == "borrar-conexion") {
        cuerpo += boton(conn, conn, raiz, que, T("t_web_cn_si_borrar", "Sí, quitarla"), testigo,
                        std::string(), true);
        cuerpo += "<p>" + enlace("/c/" + H::haciaUrl(conn), T("t_web_no_volver", "No, volver"))
                  + "</p>";
        return envuelve(T("t_web_confirmar_81b4b6", "Confirmar"), enlace("/", "ZFSMgr"), cuerpo,
                        testigo);
    }
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
                        que == "rollback" ? T("t_web_si_atras", "Sí, volver atrás")
                                          : T("t_web_si_destruir", "Sí, destruirlo"),
                        testigo,
                        extra, true);
    }
    const std::string padre = objeto.find('@') != std::string::npos
                                  ? objeto.substr(0, objeto.find('@'))
                                  : objeto;
    cuerpo += "<p>" + enlace(urlDe(conn, raiz.empty() ? padre : raiz, padre, Vista::Resumen),
                             "No, volver")
              + "</p>";
    return envuelve(T("t_web_confirmar_81b4b6", "Confirmar"), enlace("/", "ZFSMgr"), cuerpo, testigo);
}

// El resultado de instalar el daemon, con la traza de lo que dijo la otra máquina.
//
// Esta NO redirige después del POST, a diferencia de todas las demás acciones, y es a
// propósito: lo que hay que leer aquí es justo lo que una redirección tiraría —qué se
// desplegó, qué contestó systemd o launchd, y en macOS el paso que queda a mano—. El
// motivo por el que se puede: instalar es idempotente. Recargar reinstala lo mismo, que
// no es lo que pasa con «destruir».
std::string paginaInstalacion(const std::string& conn, const B::daemoninstall::Resultado& res,
                              const std::string& traza, const std::string& testigo) {
    std::string cuerpo;
    if (res.ok()) {
        cuerpo += "<p>Daemon instalado en «" + H::escapaHtml(conn) + "», versión "
                  + H::escapaHtml(res.version) + ".</p>";
    } else {
        cuerpo += "<p>No se pudo: " + H::escapaHtml(B::daemoninstall::etiquetaDe(res.fallo));
        if (res.rc != 0) {
            cuerpo += " (código " + std::to_string(res.rc) + ")";
        }
        cuerpo += "</p>";
        if (!res.detalle.empty()) {
            cuerpo += "<pre>" + H::escapaHtml(res.detalle) + "</pre>";
        }
    }
    if (res.versionAtrasada) {
        cuerpo += "<div class=\"pendiente\">El agente empaquetado para esa plataforma es "
                  + H::escapaHtml(res.version) + " y este cliente espera "
                  + H::escapaHtml(B::agentversion::laEsperada())
                  + ": se ha instalado igual, pero la conexión seguirá saliendo "
                    "desactualizada. Hay que recompilar el agente de esa plataforma.</div>";
    }
    // En macOS hace falta UN PASO MÁS, y a mano: sin «Acceso total al disco» el agente
    // arranca, contesta STATUS=OK y no ve los discos, así que no encuentra ningún pool que
    // importar. Todo parece bien salvo el resultado, que es la peor forma de fallar; por
    // eso se dice al instalar y no cuando la lista salga vacía.
    if (res.esMac && res.ok()) {
        cuerpo += "<div class=\"pendiente\">En macOS queda un paso a mano: concederle "
                  "«Acceso total al disco» al agente en Configuración del Sistema → "
                  "Privacidad y Seguridad, añadiendo <code>/usr/local/libexec/zfsmgr-agent</code>. "
                  "Sin eso arranca y contesta, pero no ve los discos.</div>";
    }
    if (!B::trim(traza).empty()) {
        cuerpo += "<h2>Lo que dijo la máquina</h2><pre>" + H::escapaHtml(B::trim(traza)) + "</pre>";
    }
    cuerpo += "<p>" + enlace("/c/" + H::haciaUrl(conn), T("t_web_volver_a_e70d48", "Volver a ") + conn) + " · "
              + enlace("/", T("t_web_conexiones_bf843e", "Conexiones")) + "</p>";
    return envuelve("Instalar el daemon en " + conn, enlace("/", "ZFSMgr"), cuerpo, testigo);
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
    // La ayuda y el error de opción NO se imprimen aquí dentro: se anotan y se atienden
    // después de fijar el idioma. Si no, salían siempre en castellano, porque el idioma se
    // resuelve al terminar de leer los argumentos —`--config-dir` puede cambiar dónde está
    // esa preferencia—. Pedir `--lang en --help` y recibir castellano era desconcertante.
    bool pideAyuda = false;
    std::string opcionMala;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            pideAyuda = true;
            continue;
        }
        if (a == "--config-dir" && i + 1 < argc) { op.dirConfig = argv[++i]; continue; }
        if (a == "--bind" && i + 1 < argc) { op.bind = argv[++i]; continue; }
        if (a == "--port" && i + 1 < argc) { op.puerto = std::atoi(argv[++i]); continue; }
        if (a == "--password-fd" && i + 1 < argc) { op.passwordFd = std::atoi(argv[++i]); continue; }
        if (a == "-v" || a == "--verbose") { op.verboso = true; continue; }
        if (a == "--lang" && i + 1 < argc) { op.idioma = argv[++i]; continue; }
        opcionMala = a;
        break;
    }

    // El idioma de partida: lo que diga `--lang` y, si no, el que tenga puesto la interfaz
    // gráfica. Va DESPUÉS de leer los argumentos porque `--config-dir` puede cambiar dónde
    // está esa preferencia. Es el mismo reparto que hace el intérprete.
    //
    // Los catálogos se buscan al lado del ejecutable y en los sitios donde los deja la
    // instalación: el servidor no enlaza Qt, así que los lee del disco como el intérprete.
    zfsmgr::base::i18n::addSearchPath(zfsmgr::cli::dirDelEjecutable() + "/i18n");
    zfsmgr::base::i18n::addSearchPath(zfsmgr::cli::dirDelEjecutable() + "/../i18n");
    zfsmgr::base::i18n::addSearchPath(zfsmgr::cli::dirDelEjecutable() + "/../share/zfsmgr/i18n");
    zfsmgr::base::i18n::addSearchPath(zfsmgr::cli::dirDelEjecutable() + "/../Resources/i18n");
    std::string idiomaBase = op.idioma;
    if (idiomaBase.empty()) {
        ST::Aviso avisoIdioma;
        const auto raizCfg = ST::leerConfig(op.dirConfig, avisoIdioma);
        idiomaBase = raizCfg["app"]["language"].toString();
        if (idiomaBase.empty()) {
            idiomaBase = raizCfg["ui"]["language"].toString();
        }
    }
    if (!idiomaBase.empty()) {
        zfsmgr::base::i18n::setLanguage(idiomaBase);
    }

    // Ahora sí: la ayuda y el error de opción, ya en el idioma que toca.
    if (!opcionMala.empty()) {
        std::fprintf(stderr, TC("t_web_opcion_mala", "zfsmgr-web: opción desconocida: %s\n"),
                     opcionMala.c_str());
        uso();
        return 2;
    }
    if (pideAyuda) {
        uso();
        return 0;
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
            std::fprintf(stderr, TC("t_web_maestra_mal",
                                "la contraseña maestra no abre la configuración: %s\n"),
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
            std::fprintf(stderr, TC("t_web_cert_mal", "no se pudo emitir el certificado: %s\n"),
                     err.c_str());
            return 1;
        }
        std::fprintf(stderr, TC("t_web_cert_ok", "certificado emitido en %s\n"),
                     rutaCert.c_str());
    }

    // Lo que ya se preguntó a cada máquina, para no volver a esperar por ella en cada
    // recarga. Vive lo que vive el proceso.
    // Lo que se sabe de cada máquina, preguntado UNA vez y recordado durante la vida del
    // proceso —incluido el fallo—. Sin eso, cada página que necesite decidir si se puede
    // copiar volvería a esperar el plazo entero por una máquina apagada.
    struct Salud {
        std::string versionAgente;   // ya marcada con «*» o «+» si procede
        std::string versionZfs;
        bool vivo{false};
        bool admiteTrabajos{false};
    };
    std::map<std::string, Salud> saludPorConexion;

    zfsmgr::web::Sesion sesion;
    sesion.abre();
    if (!sesion.abierta()) {
        std::fputs(TC("t_web_sin_azar",
                      "no hay fuente de azar: no se puede abrir una sesión segura\n"), stderr);
        return 1;
    }

    // La sesión de transporte: la misma que monta el intérprete, con su proveedor de
    // credenciales y su persistencia de TLS. No se duplica el cableado.
    auto sesionZfs = zfsmgr::cli::crearSesion(op.dirConfig, maestra, op.verboso);
    if (!sesionZfs) {
        std::fputs(TC("t_web_sin_transporte", "no se pudo montar la sesión de transporte\n"), stderr);
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
    // El cartel NO se imprime aquí: se imprime cuando el socket ya escucha. Escribirlo
    // antes anunciaba «zfsmgr-web escuchando en …» con su URL de sesión y a continuación
    // «no se pudo escuchar», dejando al usuario con un enlace que nunca funcionó.
    const auto yaEscucha = [&] {
        std::fprintf(stderr, TC("t_web_escuchando", "zfsmgr-web escuchando en https://%s:%d/\n"),
                     op.bind.c_str(),
                     op.puerto);
        std::fprintf(stderr,
                     TC("t_web_url_sesion",
                        "abra esta URL, que lleva la sesión:\n  https://%s:%d/?s=%s\n"),
                     op.bind.c_str(), op.puerto, sesion.id().c_str());
    };

    // El fichero que la petición que se está atendiendo quiere que se sirva por trozos. Lo
    // rellena el camino de WebDAV y lo consume `atiendeChorro`, que corre justo después.
    struct FicheroPedido {
        // El perfil POR VALOR, no un puntero.
        //
        // Con puntero esto era una referencia colgante y se llevó una tarde: apuntaba
        // dentro de `conns`, que es LOCAL a `atiende` y se destruye al volver de ella. El
        // bucle de trozos corre después, así que leía un perfil ya muerto; el primer
        // `std::string` que copiaba de ahí traía un tamaño de basura y el servidor moría
        // con `std::bad_alloc` a mitad de la descarga.
        //
        // Copiar un perfil por fichero servido no cuesta nada, y quita de en medio la
        // pregunta de quién vive más que quién.
        B::ConnectionProfile perfil;
        bool hay{false};
        std::string ruta;
        long long tamano{0};
    };
    FicheroPedido ficheroPedido;

    // Lo que se sabe de una máquina, preguntándoselo la primera vez y recordándolo después.
    // Dos llamadas: `--health` trae la versión del agente y si admite trabajos, y la versión
    // de ZFS hay que pedirla aparte porque `--health` no la lleva.
    const auto saludDe = [&](const B::ConnectionProfile& perfilC) {
        const std::string id = perfilC.id.empty() ? perfilC.name : perfilC.id;
        const auto ya = saludPorConexion.find(id);
        if (ya != saludPorConexion.end()) {
            return ya->second;
        }
        Salud sal;
        std::string salida;
        std::string err;
        int rc = -1;
        std::string version;
        if (llamaAgente(*sesionZfs, perfilC, {"--health"}, salida, err, rc, nullptr, 8000)
            && rc == 0) {
            sal.vivo = true;
            for (const std::string& linea : B::split(salida, "\n", true)) {
                const std::string l = B::trim(linea);
                if (B::startsWith(l, "VERSION=")) {
                    version = B::trim(l.substr(8));
                } else if (B::startsWith(l, "JOBS_SUPPORT=")) {
                    sal.admiteTrabajos = (B::trim(l.substr(13)) == "1");
                }
            }
            std::string salidaZ;
            std::string errZ;
            int rcZ = -1;
            if (llamaAgente(*sesionZfs, perfilC, PET::versionDeZfs(), salidaZ, errZ, rcZ,
                            nullptr, 8000)
                && rcZ == 0) {
                // `zfs version` escribe «zfs-2.4.2-…» en la primera línea.
                const std::string primera = B::trim(B::split(salidaZ, "\n", true).empty()
                                                        ? std::string()
                                                        : B::split(salidaZ, "\n", true).front());
                sal.versionZfs = B::startsWith(primera, "zfs-") ? primera.substr(4) : primera;
            }
        }
        sal.versionAgente = marcaDeVersion(version);
        saludPorConexion[id] = sal;
        return sal;
    };

    const auto atiende = [&](const std::string& crudo, std::string& respuesta) {
        ficheroPedido = FicheroPedido{};
        const H::Peticion p = H::analiza(crudo);
        // El idioma de ESTA petición. Se pone en cada una porque el catálogo es global al
        // proceso y el servidor atiende de una en una: dejarlo puesto de la anterior
        // serviría la página en el idioma de otro navegador.
        {
            const std::string suyo = p.cookie("zfsmgr_idioma");
            zfsmgr::base::i18n::setLanguage(
                (suyo == "es" || suyo == "en" || suyo == "zh") ? suyo : idiomaBase);
        }
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
        // Elegir idioma. Es un GET y no un POST a propósito: no cambia nada de ninguna
        // máquina —solo cómo se lee esta página— así que no necesita testigo, y así el
        // enlace del pie funciona sin formulario.
        if (p.ruta == "/idioma") {
            std::string quiere;
            for (const std::string& par : B::split(p.consulta, "&", true)) {
                const std::size_t i = par.find('=');
                if (i != std::string::npos && H::desdeUrl(par.substr(0, i)) == "a") {
                    quiere = H::desdeUrl(par.substr(i + 1));
                }
            }
            // Solo los tres que hay catálogo. Cualquier otra cosa se ignora en vez de
            // guardarse: una cookie con basura dentro dejaría la página en castellano sin
            // que se entienda por qué.
            if (quiere != "es" && quiere != "en" && quiere != "zh") {
                quiere = "es";
            }
            r.codigo = 302;
            r.cabecerasExtra.push_back("Set-Cookie: zfsmgr_idioma=" + quiere
                                       + "; Path=/; Secure; SameSite=Strict; Max-Age=31536000");
            r.cabecerasExtra.push_back("Location: /");
            r.cuerpo = "";
            respuesta = H::componer(r);
            return true;
        }

        // Marcar o quitar el origen. GET como el idioma, y por lo mismo: no cambia nada de
        // ninguna máquina, solo una marca de este navegador.
        if (p.ruta == "/origen") {
            std::string c;
            std::string o;
            bool quitar = false;
            for (const std::string& par : B::split(p.consulta, "&", true)) {
                const std::size_t i = par.find('=');
                if (i == std::string::npos) {
                    continue;
                }
                const std::string k = H::desdeUrl(par.substr(0, i));
                const std::string v = H::desdeUrl(par.substr(i + 1));
                if (k == "c")            { c = v; }
                else if (k == "o")       { o = v; }
                else if (k == "quitar")  { quitar = true; }
            }
            r.codigo = 302;
            if (quitar || c.empty() || o.empty()) {
                r.cabecerasExtra.push_back("Set-Cookie: zfsmgr_origen=; Path=/; Secure; "
                                           "SameSite=Strict; Max-Age=0");
                r.cabecerasExtra.push_back("Location: /");
            } else {
                r.cabecerasExtra.push_back("Set-Cookie: zfsmgr_origen=" + H::haciaUrl(c) + "%7C"
                                           + H::haciaUrl(o)
                                           + "; Path=/; Secure; SameSite=Strict; Max-Age=86400");
                // Se vuelve al MISMO sitio: marcar un origen no debería sacar a nadie de
                // donde estaba, que es justo desde donde va a ir a buscar el destino.
                r.cabecerasExtra.push_back(
                    "Location: " + urlDe(c, o.substr(0, o.find('/') == std::string::npos
                                                            ? o.size()
                                                            : o.find('/')),
                                         o, Vista::Acciones));
            }
            r.cuerpo = "";
            respuesta = H::componer(r);
            return true;
        }

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
                    if (llamaAgente(*sesionZfs, *perfilD,
                                                    PET::listaDeDatasets(dataset), salidaD,
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
                    objeto.empty() ? PET::listaDePools()
                                   : PET::listaDeDatasets(dataset);
                if (!encontrado
                    || (objeto.empty()
                        && (!llamaAgente(*sesionZfs, *perfilD, verboD, salidaD,
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
                        if (llamaAgente(*sesionZfs, *perfilD,
                                                        PET::contenidoDeDirectorio(rutaFs), salidaF, errF,
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
            // Un FICHERO no se sirve por aquí: se apunta cuál es y lo manda el camino por
            // trozos, que es el único que puede con una imagen de 726 MB sin cargarla en
            // memoria. Aquí solo se comprueba que existe y de qué tamaño es.
            if (!rutaBaseFs.empty() && perfilPedido && !esDirectorioDeVerdad) {
                std::string sal;
                std::string errG;
                int rcG = -1;
                if (llamaAgente(*sesionZfs, *perfilPedido,
                                PET::contenidoDeFichero(rutaBaseFs, 0, 1), sal, errG, rcG, nullptr,
                                60000)
                    && rcG == 0) {
                    long long total = 0;
                    for (const std::string& l : B::split(sal, "\n", true)) {
                        if (B::startsWith(l, "SIZE=")) {
                            total = std::strtoll(l.substr(5).c_str(), nullptr, 10);
                        }
                    }
                    ficheroPedido = {*perfilPedido, true, rutaBaseFs, total};
                    // Lo contesta el camino por trozos, en cuanto esta función devuelva.
                    respuesta.clear();
                    return true;
                }
                // No se pudo leer: se dice POR QUÉ. Antes caía al 404 de abajo y decía «no
                // existe» de un fichero que existía y solo era grande.
                r.codigo = 502;
                r.tipo = "text/plain; charset=utf-8";
                r.cuerpo = "no se pudo leer el fichero: " + B::trim(errG.empty() ? sal : errG)
                           + "\n";
                respuesta = H::componer(r);
                return true;
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
            // Una colección pedida por GET se sirve NAVEGABLE, no con la palabra
            // «coleccion».
            //
            // Los exploradores de archivos usan PROPFIND y por eso funcionaban; un
            // navegador hace un GET normal, y lo que veía era literalmente «coleccion».
            // Con el listado se puede recorrer todo el árbol de ficheros desde el
            // navegador, que es lo que uno espera al pegar una de estas direcciones.
            r.tipo = "text/html; charset=utf-8";
            r.cuerpo = p.metodo == "HEAD" ? std::string()
                                          : paginaColeccionDav(ruta, recursos, sesion.testigo());
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
            // Estas dos van ANTES de exigir perfil y objeto, y no es un capricho: dar de
            // alta una conexión nueva no tiene todavía perfil que buscar, así que con la
            // guarda delante el alta respondía «falta la conexión o el objeto» —un 400 que
            // no explica nada— en vez de crearla.
            if (que == "guardar-conexion") {
                B::ConnectionProfile nuevo;
                nuevo.id = B::trim(p.campo("id"));
                if (nuevo.id.empty()) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(T("t_web_cn_sin_id",
                                             "la conexión necesita un identificador"),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                const B::ConnectionProfile* previo =
                    zfsmgr::cli::buscarConexion(conns, nuevo.id);
                if (previo != nullptr) {
                    // Se parte del perfil que ya estaba: así los campos que este formulario
                    // no enseña —el material TLS, el identificador de máquina— no se pierden
                    // por editar el puerto. Escribir un perfil «nuevo» encima era perderlos.
                    nuevo = *previo;
                }
                nuevo.name = B::trim(p.campo("nombre"));
                if (nuevo.name.empty()) {
                    nuevo.name = nuevo.id;
                }
                nuevo.connType = B::toUpperAscii(B::trim(p.campo("tipo")));
                if (nuevo.connType.empty()) {
                    nuevo.connType = "SSH";
                }
                nuevo.osType = B::trim(p.campo("so"));
                if (nuevo.osType.empty()) {
                    nuevo.osType = "Linux";
                }
                nuevo.host = B::trim(p.campo("host"));
                nuevo.username = B::trim(p.campo("usuario"));
                const std::string puertoTxt = B::trim(p.campo("puerto"));
                nuevo.port = puertoTxt.empty() ? 0 : std::atoi(puertoTxt.c_str());
                nuevo.keyPath = B::trim(p.campo("clave"));
                nuevo.useSudo = (p.campo("sudo") == "1");
                // La contraseña VACÍA no borra la que había: si lo hiciera, cambiar el
                // puerto se llevaría por delante la contraseña de acceso sin decirlo.
                const std::string clave = p.campo("clavesecreta");
                if (!clave.empty()) {
                    nuevo.password = clave;
                }
                std::string errG;
                if (!zfsmgr::cli::guardarConexion(*sesionZfs, nuevo, errG)) {
                    r.codigo = 502;
                    r.cuerpo = paginaError(errG, sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                r.codigo = 302;
                r.cabecerasExtra.push_back("Location: /c/" + H::haciaUrl(nuevo.id)
                                           + "?v=acciones");
                respuesta = H::componer(r);
                return true;
            }
            // Apartar una conexión y volver a traerla.
            //
            // Va aquí arriba, con `borrar-conexion`, porque como ella NO pasa por el agente:
            // apartar una máquina es precisamente lo que se hace cuando esa máquina no
            // contesta, así que exigirle que conteste para poder apartarla sería un círculo.
            // La marca vive en `app.disconnected_connections` del config.json, la misma que
            // leen el intérprete y la interfaz.
            if (que == "conectar" || que == "desconectar") {
                std::string errC;
                if (!zfsmgr::cli::marcarDesconectada(*sesionZfs, conn, que == "desconectar",
                                                     errC)) {
                    r.codigo = 502;
                    r.cuerpo = paginaError(errC, sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                r.codigo = 302;
                r.cabecerasExtra.push_back("Location: /c/" + H::haciaUrl(conn));
                respuesta = H::componer(r);
                return true;
            }
            if (que == "borrar-conexion") {
                std::string errB;
                if (!zfsmgr::cli::borrarConexion(*sesionZfs, conn, errB)) {
                    r.codigo = 502;
                    r.cuerpo = paginaError(errB, sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                // El origen marcado puede ser de la conexión que se acaba de borrar.
                r.codigo = 302;
                r.cabecerasExtra.push_back("Set-Cookie: zfsmgr_origen=; Path=/; Secure; "
                                           "SameSite=Strict; Max-Age=0");
                r.cabecerasExtra.push_back("Location: /");
                respuesta = H::componer(r);
                return true;
            }

            const B::ConnectionProfile* perfil = zfsmgr::cli::buscarConexion(conns, conn);
            if (!perfil || objeto.empty()) {
                r.codigo = 400;
                r.cuerpo = paginaError(T("t_web_falta_la_conex_797512", "falta la conexión o el objeto"), sesion.testigo());
                respuesta = H::componer(r);
                return true;
            }

            // Instalar el daemon NO es un verbo del agente, y no puede serlo: es lo que
            // se hace cuando el agente todavía no está o está viejo. Entra por SSH y scp,
            // que es el problema del huevo y la gallina de siempre. Por eso se atiende
            // aquí, antes del reparto de verbos, y no dentro de él.
            if (que == "instalar-daemon") {
                namespace DI = B::daemoninstall;
                B::ConnectionProfile conSudo = *perfil;
                // La contraseña del formulario solo se usa si el perfil no trae una: un
                // campo vacío no debe BORRAR la que estaba guardada.
                const std::string sudoDelFormulario = p.campo("sudo");
                if (conSudo.password.empty() && !sudoDelFormulario.empty()) {
                    conSudo.password = sudoDelFormulario;
                    conSudo.useSudo = true;
                }
                const std::string plataforma = DI::plataformaDe(conSudo);
                const std::string arq =
                    DI::arquitecturaRemota(sesionZfs->transporte, conSudo, false);
                const std::string binario = zfsmgr::cli::rutaDelAgente(plataforma, arq);
                if (binario.empty()) {
                    r.codigo = 502;
                    r.cuerpo = paginaError(
                        "no hay agente empaquetado para " + plataforma + "/"
                            + (arq.empty() ? std::string("?") : arq)
                            + " en este equipo. No se instala nada: un respaldo por guion no "
                              "habla TLS, y dejarlo puesto daría una máquina que parece "
                              "atendida y no lo está.",
                        sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                std::string traza;
                const DI::Resultado res = DI::instala(
                    sesionZfs->transporte, conSudo, binario,
                    [&traza](const std::string& l) { traza += l + "\n"; }, false);
                // Se olvida la versión recordada de esa máquina: acaba de cambiar, y
                // seguir enseñando la vieja es justo lo contrario de lo que uno espera
                // después de pulsar «instalar».
                saludPorConexion.erase(perfil->id.empty() ? perfil->name : perfil->id);
                r.cuerpo = paginaInstalacion(conn, res, traza, sesion.testigo());
                r.codigo = res.ok() ? 200 : 502;
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
                    r.cuerpo = paginaError(B::format(T("t_web_e_nsnap", "nombre de instantánea no válido: «%1»"), {nombre}),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = INST::argvCrearInstantanea(objeto, nombre, p.campo("rec") == "1");
            } else if (que == "borrar-instantanea") {
                if (objeto.find('@') == std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError("eso no es una instantánea: «" + objeto + "»",
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = INST::argvDestruir(objeto, false, INST::Alcance::Solo);
            } else if (que == "montar" || que == "desmontar") {
                verbo = PET::zfsGenerico(argvEnBase64(
                    que == "montar" ? DS::argvMontar(objeto) : DS::argvDesmontar(objeto)));
            } else if (que == "crear-dataset") {
                const std::string nombre = B::trim(p.campo("nombre"));
                if (nombre.empty() || nombre.find('@') != std::string::npos
                    || nombre.find('/') != std::string::npos
                    || nombre.find(' ') != std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(B::format(T("t_web_e_nds", "nombre de dataset no válido: «%1»"), {nombre}),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = PET::zfsGenerico(
                    argvEnBase64(DS::argvCrear(DS::nombreDeHijo(objeto, nombre))));
            } else if (que == "renombrar") {
                const std::string nombre = B::trim(p.campo("nombre"));
                if (nombre.empty() || nombre.find('@') != std::string::npos
                    || nombre.find(' ') != std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(B::format(T("t_web_e_ndest", "nombre de destino no válido: «%1»"), {nombre}),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                // El nombre completo lo calcula `commands::datasets`: sin barra se
                // conserva el padre, que es lo que el intérprete ya hacía y aquí no. Antes,
                // teclear solo la hoja daba «missing dataset name» y nada explicaba por qué.
                {
                    const std::vector<std::string> a = DS::argvRenombrar(objeto, nombre);
                    if (a.empty()) {
                        r.codigo = 400;
                        r.cuerpo = paginaError(
                            B::format(T("t_web_e_ndest", "nombre de destino no válido: «%1»"),
                                      {nombre}),
                            sesion.testigo());
                        respuesta = H::componer(r);
                        return true;
                    }
                    verbo = PET::zfsGenerico(argvEnBase64(a));
                }
            } else if (que == "clonar") {
                const std::string nombre = B::trim(p.campo("nombre"));
                if (objeto.find('@') == std::string::npos || nombre.empty()
                    || nombre.find(' ') != std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(T("t_web_un_clon_se_hac_cd0391", "un clon se hace de una instantánea a un dataset nuevo"),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                // El argv y las dos comprobaciones —origen instantánea, destino no— salen
                // de `commands::instantaneas`, compartidas con el intérprete.
                verbo = INST::argvClonar(objeto, nombre);
            } else if (que == "borrar-dataset") {
                if (objeto.find('@') != std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(T("t_web_eso_es_una_ins_043b45", "eso es una instantánea, no un dataset"),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = INST::argvDestruir(objeto, false,
                                         p.campo("alcance") == "r" ? INST::Alcance::Descendientes
                                                                   : INST::Alcance::Solo);
            } else if (que == "rollback") {
                if (objeto.find('@') == std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(T("t_web_un_rollback_va_847fee", "un rollback va a una instantánea"), sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = INST::argvRollback(objeto, false,
                                         p.campo("alcance") == "r" ? INST::Alcance::Descendientes
                                                                   : INST::Alcance::Solo);
            } else if (que == "cargar-clave" || que == "descargar-clave") {
                if (que == "descargar-clave") {
                    verbo = PET::zfsGenerico(argvEnBase64({"unload-key", objeto}));
                } else {
                    // La frase va en base64 DENTRO de la carga, cifrada por mTLS, y el
                    // daemon se la pasa a `zfs` por una tubería. Nunca por argumento: eso
                    // sale en el «ps» de las dos máquinas.
                    verbo = PET::cargaClave(objeto, p.campo("frase"));
                }
            } else if (que == "cambiar-clave") {
                const std::string f1 = p.campo("frase");
                const std::string f2 = p.campo("frase2");
                if (f1.empty()) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(T("t_web_e_frase_vacia", "la frase no puede estar vacía"),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                if (f1 != f2) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(T("t_web_e_frase_dif",
                                             "las dos frases no coinciden: no se ha cambiado nada"),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                // Mismo camino que `cargar-clave`: la frase viaja en base64 dentro de la carga
                // —cifrada por mTLS— y el daemon se la da a `zfs` por una tubería, nunca por
                // argumento. El tercer parámetro son las banderas, que aquí van vacías.
                verbo = PET::cambiaClave(objeto, f1, std::string());
            } else if (que == "programar" || que == "desprogramar") {
                // Los nueve ajustes en UNA sola orden: `zfs set a=1 b=2 … dataset`. Nueve
                // llamadas separadas dejarían una programación a medias si fallara la
                // quinta, y a medias es peor que no puesta —hay retenciones nuevas con el
                // «activado» viejo—.
                if (que == "desprogramar") {
                    // `zfs inherit` solo admite UNA propiedad, así que aquí sí van nueve.
                    // Se hacen todas aunque alguna falle: dejar tres puestas y seis
                    // quitadas es exactamente lo que no queremos.
                    bool todoBien = true;
                    std::string ultimoErr;
                    for (const auto& kv : B::gsa::aPropiedades(B::gsa::Programacion{})) {
                        std::string sO;
                        std::string eO;
                        int rO = -1;
                        if (!llamaAgente(*sesionZfs, *perfil,
                                         PET::zfsGenerico(
                                             argvEnBase64({"inherit", kv.first, objeto})),
                                         sO, eO, rO, nullptr, 60000)
                            || rO != 0) {
                            todoBien = false;
                            ultimoErr = B::trim(eO.empty() ? sO : eO);
                        }
                    }
                    if (!todoBien) {
                        r.codigo = 502;
                        r.cuerpo = paginaError("no se pudo quitar la programación: " + ultimoErr,
                                               sesion.testigo());
                        respuesta = H::componer(r);
                        return true;
                    }
                    r.codigo = 302;
                    r.cabecerasExtra.push_back(
                        "Location: " + urlDe(conn, B::trim(p.campo("raiz")).empty()
                                                       ? objeto
                                                       : B::trim(p.campo("raiz")),
                                             objeto, Vista::Programacion));
                    r.cuerpo = "";
                    respuesta = H::componer(r);
                    return true;
                }
                B::gsa::Programacion prog;
                prog.activado = (p.campo("activado") == "1");
                prog.recursivo = (p.campo("recursivo") == "1");
                prog.nivelar = (p.campo("nivelar") == "1");
                // Se teclea como URL y se GUARDA como siempre: el planificador del daemon
                // parte el valor por «::» y está escrito así en datasets que ya existen.
                prog.destino = B::gsa::destinoDesdeUrl(p.campo("destino"));
                const std::pair<const char*, int*> ret[] = {
                    {"horario", &prog.horario}, {"diario", &prog.diario},
                    {"semanal", &prog.semanal}, {"mensual", &prog.mensual},
                    {"anual", &prog.anual},
                };
                bool numerosBien = true;
                for (const auto& c : ret) {
                    const std::string v = B::trim(p.campo(c.first));
                    if (v.empty()) {
                        *c.second = 0;
                        continue;
                    }
                    if (v.find_first_not_of("0123456789") != std::string::npos) {
                        numerosBien = false;
                        break;
                    }
                    *c.second = std::atoi(v.c_str());
                }
                if (!numerosBien) {
                    r.codigo = 400;
                    r.cuerpo = paginaError("las retenciones tienen que ser números enteros "
                                           "mayores o iguales que cero",
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                // La MISMA validación que usa la interfaz, en la capa base: activada sin
                // ninguna retención hace la instantánea y la borra —casi siempre es un
                // olvido, y callarlo deja creyendo que hay copias—, y nivelar sin destino
                // no puede nivelar contra nada.
                B::gsa::Motivo porQue;
                const auto existe = [&conns](const std::string& idConn) {
                    return zfsmgr::cli::buscarConexion(conns, idConn) != nullptr;
                };
                if (!B::gsa::valida(objeto, prog, existe, porQue)) {
                    r.codigo = 400;
                    r.cuerpo = paginaError("no se puede guardar: "
                                               + B::gsa::etiquetaDe(porQue.fallo)
                                               + (porQue.detalle.empty()
                                                      ? std::string()
                                                      : " («" + porQue.detalle + "»)"),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                std::vector<std::string> argv = {"set"};
                for (const auto& kv : B::gsa::aPropiedades(prog)) {
                    argv.push_back(kv.first + "=" + kv.second);
                }
                argv.push_back(objeto);
                verbo = PET::zfsGenerico(argvEnBase64(argv));
            } else if (que == "copiar-desde-origen" || que == "nivelar-desde-origen") {
                const DX::Extremo origen = origenDe(p);
                const B::ConnectionProfile* perfilOrigen =
                    origen.vacio() ? nullptr
                                   : zfsmgr::cli::buscarConexion(conns, origen.conexion);
                if (perfilOrigen == nullptr) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(T("t_web_sin_origen_marcado",
                                             "no hay ningún origen marcado, o su máquina ya no "
                                             "está en la lista"),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                // Se vuelve a planear AQUÍ y no se confía en lo que se pintó: entre que se
                // dibujó la página y se pulsó, el origen pudo cambiar en otra pestaña o una
                // máquina pudo caerse.
                const Salud sO = saludDe(*perfilOrigen);
                const Salud sD = saludDe(*perfil);
                TR::Extremo eO;
                eO.conexion = origen.conexion;
                eO.objeto = origen.objeto;
                eO.esWindows = B::transport::isWindowsConnection(*perfilOrigen);
                eO.tieneDaemon = sO.vivo;
                eO.admiteTrabajos = sO.admiteTrabajos;
                eO.versionZfs = sO.versionZfs;
                TR::Extremo eD;
                eD.conexion = conn;
                eD.objeto = objeto;
                eD.esWindows = B::transport::isWindowsConnection(*perfil);
                eD.tieneDaemon = sD.vivo;
                eD.admiteTrabajos = sD.admiteTrabajos;
                eD.versionZfs = sD.versionZfs;
                const TR::Plan plan = TR::planea(eO, eD, /*exigeAsincrono=*/true);
                if (!plan.sePuede()) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(TR::etiquetaDe(plan.fallo), sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }

                // **Copiar y nivelar NO reciben en el mismo sitio.** Copiar crea el
                // dataset debajo del destino —«<destino>/<hoja del origen>»—; nivelar pone
                // al día el dataset destino EN SÍ. La web usaba la ruta de copiar para las
                // dos, así que «Nivelar» creaba un hijo en vez de nivelar nada. Sale de
                // leer la interfaz de Qt: `mainwindow_transfer.cpp:378` frente a `:1352`.
                const bool esNivelar = (que == "nivelar-desde-origen");
                const std::string destino =
                    esNivelar ? objeto : TR::destinoReal(origen.dataset(), objeto);

                // Nivelar manda un INCREMENTAL, y para eso hace falta la base común. Se
                // busca por GUID —no por nombre— entre las instantáneas de los dos
                // extremos; la regla y sus tres negativas viven en `base/transferencia`.
                //
                // Las dos listas salen de `--dump-zfs-list-all`, que ya trae el GUID, así
                // que esto cuesta una consulta por extremo y ninguna más.
                std::string desdeInstantanea;
                if (esNivelar) {
                    const auto instantaneasDe =
                        [&](const B::ConnectionProfile& maquina, const std::string& ds,
                            std::vector<TR::Instantanea>& out) -> bool {
                        std::string sal;
                        std::string errL;
                        int rcL = 0;
                        if (!llamaAgente(*sesionZfs, maquina, PET::listaDeDatasets(ds), sal,
                                         errL, rcL, nullptr, 60000)
                            || rcL != 0) {
                            return false;
                        }
                        for (const L::Entrada& e : L::entradas(sal)) {
                            const std::size_t i = e.nombre.find('@');
                            if (i == std::string::npos || e.nombre.substr(0, i) != ds) {
                                continue;
                            }
                            out.push_back({e.nombre.substr(i + 1), e.guid});
                        }
                        return true;
                    };
                    std::vector<TR::Instantanea> deOrigen;
                    std::vector<TR::Instantanea> deDestino;
                    if (!instantaneasDe(*perfilOrigen, origen.dataset(), deOrigen)
                        || !instantaneasDe(*perfil, objeto, deDestino)) {
                        r.codigo = 502;
                        r.cuerpo = paginaError(T("t_web_e_niv_lista",
                                                 "no se pudieron leer las instantáneas de los "
                                                 "dos extremos para calcular el incremental"),
                                               sesion.testigo());
                        respuesta = H::componer(r);
                        return true;
                    }
                    const std::string objetivoCorto =
                        origen.objeto.substr(origen.objeto.find('@') + 1);
                    const TR::PlanNivelar pn =
                        TR::planeaNivelar(deOrigen, deDestino, objetivoCorto);
                    if (!pn.sePuede()) {
                        r.codigo = 400;
                        r.cuerpo = paginaError(T("t_web_e_nivelar", "no se puede nivelar: ")
                                                   + TR::etiquetaDe(pn.fallo),
                                               sesion.testigo());
                        respuesta = H::componer(r);
                        return true;
                    }
                    desdeInstantanea = origen.dataset() + "@" + pn.base;
                }
                // El testigo de reanudación, si quedó algo a medias. Con él puesto, el
                // envío continúa desde donde iba en vez de mandarlo todo otra vez.
                const auto reanuda = TR::buscaTestigo(sesionZfs->transporte, *perfil, destino,
                                                      false);
                TR::OpcionesDeEnvio opciones;
                opciones.R = (p.campo("rec") == "1");
                const TR::LlamadaAlAgente llama =
                    [&](const B::ConnectionProfile& maquina,
                        const std::vector<std::string>& args, int timeoutMs, std::string& out,
                        std::string& errL, int& rcL) {
                        return llamaAgente(*sesionZfs, maquina, args, out, errL, rcL, nullptr,
                                           timeoutMs);
                    };
                const auto lanzado = TR::lanzaTrabajo(
                    sesionZfs->transporte, llama, *perfilOrigen, *perfil, origen.objeto, destino,
                    desdeInstantanea, TR::banderasDeEnvio(opciones), reanuda.testigo,
                    origen.conexion == conn, op.verboso);
                r.cuerpo = paginaTrabajoLanzado(conn, origen.objeto, destino, lanzado, reanuda,
                                                sesion.testigo(),
                                                esNivelar ? QueTrabajo::Nivelar
                                                          : QueTrabajo::Copiar);
                r.codigo = lanzado.ok() ? 200 : 502;
                respuesta = H::componer(r);
                return true;
            } else if (que == "sincronizar-desde-origen") {
                const DX::Extremo origen = origenDe(p);
                const B::ConnectionProfile* perfilOrigen =
                    origen.vacio() ? nullptr
                                   : zfsmgr::cli::buscarConexion(conns, origen.conexion);
                if (perfilOrigen == nullptr) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(T("t_web_sin_origen_marcado",
                                             "no hay ningún origen marcado, o su máquina ya no "
                                             "está en la lista"),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                // Se vuelve a planear AQUÍ, montajes incluidos: entre que se vio la pasada
                // en seco y se pulsó, alguien pudo desmontar cualquiera de los dos.
                SY::Extremo eO;
                eO.conexion = origen.conexion;
                eO.objeto = origen.objeto;
                eO.esWindows = B::transport::isWindowsConnection(*perfilOrigen);
                eO.tieneDaemon = saludDe(*perfilOrigen).vivo;
                SY::Extremo eD;
                eD.conexion = conn;
                eD.objeto = objeto;
                eD.esWindows = B::transport::isWindowsConnection(*perfil);
                eD.tieneDaemon = saludDe(*perfil).vivo;
                montajeDeDataset(*sesionZfs, *perfilOrigen, eO.objeto, eO.montado,
                                 eO.puntoMontaje);
                montajeDeDataset(*sesionZfs, *perfil, eD.objeto, eD.montado, eD.puntoMontaje);
                const SY::Plan planS = SY::planea(eO, eD);
                if (!planS.sePuede()) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(T("t_web_e_sync", "no se puede sincronizar: ")
                                               + SY::etiquetaDe(planS.fallo),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                std::string salS;
                std::string errS;
                std::string idT;
                const bool okS = sincroniza(*sesionZfs, *perfilOrigen, *perfil, planS,
                                            origen.conexion == conn, p.campo("del") == "1",
                                            /*enSeco=*/false, op.verboso, salS, errS, idT);
                TR::Trabajo t;
                if (okS) {
                    t.id = idT;
                } else {
                    t.fallo = TR::FalloTrabajo::SinIdentificador;
                    t.detalle = B::trim(errS).empty() ? B::trim(salS) : B::trim(errS);
                }
                r.cuerpo = paginaTrabajoLanzado(conn, origen.objeto, objeto, t, TR::Reanudacion{},
                                                sesion.testigo(), QueTrabajo::Sincronizar);
                r.codigo = okS ? 200 : 502;
                respuesta = H::componer(r);
                return true;
            } else if (que == "mover-desde-origen") {
                const DX::Extremo origen = origenDe(p);
                const DX::Extremo destino{conn, objeto};
                // Se vuelve a comprobar AQUÍ, no solo al pintar el enlace: entre que se
                // dibujó la página y se confirmó, el origen pudo cambiar en otra pestaña.
                // Validar solo donde se pinta no valida nada.
                const DX::NoAplica porQue = DX::compruebo(DX::Accion::Mover, origen, destino);
                if (porQue != DX::NoAplica::Ninguna) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(T("t_web_e_mover", "no se puede mover: ")
                                               + DX::etiquetaDe(porQue),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                const std::string aDonde = DX::destinoDeMover(origen, destino);
                std::string salida;
                std::string errL;
                int rcL = 0;
                const bool ok = llamaAgente(*sesionZfs, *perfil,
                                            DS::argvRenombrar(origen.dataset(), aDonde),
                                            salida, errL, rcL, nullptr, 120000)
                                && rcL == 0;
                if (!ok) {
                    r.codigo = 502;
                    r.cuerpo = paginaError(
                        B::format(T("t_web_e_mover_fallo", "no se pudo mover «%1» a «%2»: %3"),
                                  {origen.dataset(), aDonde,
                                   B::trim(errL).empty() ? B::trim(salida) : B::trim(errL)}),
                        sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                // El origen marcado ya no existe con ese nombre: dejarlo puesto haría que
                // la siguiente acción apuntara a algo que no está.
                r.codigo = 302;
                r.cabecerasExtra.push_back("Set-Cookie: zfsmgr_origen=; Path=/; Secure; "
                                           "SameSite=Strict; Max-Age=0");
                r.cabecerasExtra.push_back("Location: /c/" + H::haciaUrl(conn) + "?sel="
                                           + H::haciaUrl(aDonde));
                respuesta = H::componer(r);
                return true;
            } else if (que == "clonar-desde-origen") {
                const DX::Extremo origen = origenDe(p);
                const DX::Extremo destino{conn, objeto};
                const DX::NoAplica porQue = DX::compruebo(DX::Accion::Clonar, origen, destino);
                // Se vuelve a comprobar AQUÍ y no solo al pintar el botón: entre que se
                // dibujó la página y se pulsó, el origen pudo cambiar en otra pestaña del
                // navegador. Validar solo donde se pinta no valida nada.
                if (porQue != DX::NoAplica::Ninguna) {
                    r.codigo = 400;
                    r.cuerpo = paginaError("no se puede clonar: " + DX::etiquetaDe(porQue),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                const std::string nombre = B::trim(p.campo("nombre"));
                if (nombre.empty() || nombre.find('@') != std::string::npos
                    || nombre.find('/') != std::string::npos
                    || nombre.find(' ') != std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(B::format(T("t_web_e_nclon", "nombre de clon no válido: «%1»"), {nombre}),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = INST::argvClonar(origen.objeto, objeto + "/" + nombre);
            } else if (que == "dar-permiso" || que == "quitar-permiso") {
                if (objeto.find('@') != std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError("los permisos son de un dataset, no de una "
                                           "instantánea",
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                ZA::Entrada entrada;
                if (que == "quitar-permiso") {
                    // Se RELEE la lista y se coge la entrada por su índice, en vez de
                    // fiarse de unos campos ocultos. Entre que se pintó la página y se
                    // pulsó, alguien pudo cambiar los permisos desde otro sitio; con los
                    // campos del formulario se retiraría algo que ya no es lo que se veía.
                    std::string sal;
                    std::string er;
                    int rcP = -1;
                    if (!llamaAgente(*sesionZfs, *perfil, PET::permisosDe(objeto), sal, er,
                                     rcP, nullptr, 30000)
                        || rcP != 0) {
                        r.codigo = 502;
                        r.cuerpo = paginaError(T("t_web_no_se_pudieron_a882e1", "no se pudieron releer los permisos"),
                                               sesion.testigo());
                        respuesta = H::componer(r);
                        return true;
                    }
                    const auto lista = ZA::analiza(sal);
                    const long idx = std::atol(p.campo("idx").c_str());
                    if (idx < 0 || static_cast<std::size_t>(idx) >= lista.size()) {
                        r.codigo = 409;
                        r.cuerpo = paginaError("esos permisos han cambiado mientras mirabas: "
                                               "vuelve a abrirlos",
                                               sesion.testigo());
                        respuesta = H::componer(r);
                        return true;
                    }
                    entrada = lista[static_cast<std::size_t>(idx)];
                } else {
                    entrada.quien = ZA::quienDesde(p.campo("quien"));
                    entrada.alcance = ZA::alcanceDesde(p.campo("alcance"));
                    entrada.nombre = B::trim(p.campo("nombre"));
                    for (const std::string& t : B::split(p.campo("permisos"), ",", true)) {
                        const std::string perm = B::trim(t);
                        // Un permiso viaja hasta un argv de `zfs`: se comprueba que es lo
                        // que dice ser y no una bandera colada.
                        if (perm.empty() || perm[0] == '-' || perm.find(' ') != std::string::npos
                            || perm.find(',') != std::string::npos) {
                            r.codigo = 400;
                            r.cuerpo = paginaError(B::format(T("t_web_e_nperm", "permiso no válido: «%1»"), {perm}),
                                                   sesion.testigo());
                            respuesta = H::componer(r);
                            return true;
                        }
                        entrada.permisos.push_back(perm);
                    }
                    if (entrada.permisos.empty()) {
                        r.codigo = 400;
                        r.cuerpo = paginaError(T("t_web_hay_que_decir_0b8a38", "hay que decir qué permisos se delegan"),
                                               sesion.testigo());
                        respuesta = H::componer(r);
                        return true;
                    }
                    // Un nombre hace falta salvo para «todos» y para «al crear», que no
                    // nombran a nadie. Sin esta comprobación, `zfs` recibiría la lista de
                    // permisos en el sitio del destinatario.
                    const bool nombraAAlguien = entrada.quien != ZA::Quien::Todos
                                                && entrada.alcance != ZA::Alcance::AlCrear;
                    if (nombraAAlguien
                        && (entrada.nombre.empty()
                            || entrada.nombre.find(' ') != std::string::npos)) {
                        r.codigo = 400;
                        r.cuerpo = paginaError(T("t_web_falta_a_quien_22ffab", "falta a quién se le delega, o el nombre no vale"),
                                               sesion.testigo());
                        respuesta = H::componer(r);
                        return true;
                    }
                }
                const std::vector<std::string> argv =
                    que == "dar-permiso" ? ZA::argvConceder(entrada, objeto)
                                         : ZA::argvRetirar(entrada, objeto);
                // Por el verbo de LOTE aunque sea una sola: es el que el daemon expone para
                // esto, y admite varias en una orden el día que se editen en bloque.
                // El lote es una lista de cadenas —cada una, un argv ya empaquetado—,
                // así que se cierra con la misma función.
                const std::vector<std::string> lote = {argvEnBase64(argv)};
                std::string salA;
                std::string errA;
                int rcA = -1;
                if (!llamaAgente(*sesionZfs, *perfil,
                                 PET::permisosEnLote(argvEnBase64(lote)),
                                 salA, errA, rcA, nullptr, 60000)
                    || rcA != 0) {
                    r.codigo = 502;
                    r.cuerpo = paginaError("no se pudo: " + B::trim(errA.empty() ? salA : errA),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                // **Y SE COMPRUEBA QUE PASÓ.**
                //
                // `zfs allow` devuelve CERO cuando el usuario o el grupo no existen: se
                // limita a imprimir su «Usage» por la salida de error. Comprobado a mano —
                // con un grupo inexistente sale rc=0 y no se delega nada—. Así que fiarse
                // del código de salida hacía que la web dijera «hecho» y no hubiera pasado
                // nada, que es la peor forma de fallar.
                //
                // Solo un permiso inventado da rc distinto de cero. Todo lo demás hay que
                // mirarlo releyendo.
                std::string salV;
                std::string errV;
                int rcV = -1;
                if (llamaAgente(*sesionZfs, *perfil, PET::permisosDe(objeto), salV, errV,
                                rcV, nullptr, 30000)
                    && rcV == 0) {
                    bool esta = false;
                    for (const ZA::Entrada& hay : ZA::analiza(salV)) {
                        if (hay.quien != entrada.quien || hay.alcance != entrada.alcance
                            || hay.nombre != entrada.nombre) {
                            continue;
                        }
                        for (const std::string& p1 : entrada.permisos) {
                            esta = esta
                                   || std::find(hay.permisos.begin(), hay.permisos.end(), p1)
                                          != hay.permisos.end();
                        }
                    }
                    if (que == "dar-permiso" && !esta) {
                        r.codigo = 502;
                        r.cuerpo = paginaError(
                            "ZFS aceptó la orden pero no delegó nada. Lo normal es que ese "
                            "usuario o grupo no exista en «" + conn + "»: compruébelo con "
                            "«id " + entrada.nombre + "» en esa máquina.",
                            sesion.testigo());
                        respuesta = H::componer(r);
                        return true;
                    }
                    if (que == "quitar-permiso" && esta) {
                        r.codigo = 502;
                        r.cuerpo = paginaError(T("t_web_zfs_acepto_la_127c02", "ZFS aceptó la orden pero el permiso sigue ahí"),
                                               sesion.testigo());
                        respuesta = H::componer(r);
                        return true;
                    }
                }
                r.codigo = 302;
                r.cabecerasExtra.push_back(
                    "Location: " + urlDe(conn, B::trim(p.campo("raiz")).empty()
                                                   ? objeto
                                                   : B::trim(p.campo("raiz")),
                                         objeto, Vista::Permisos));
                r.cuerpo = "";
                respuesta = H::componer(r);
                return true;
            } else if (que == "poner-hold" || que == "soltar-hold") {
                const std::string etiqueta = B::trim(p.campo("etiqueta"));
                // La etiqueta viaja hasta un argv de `zfs`: un espacio o una arroba dentro
                // convertirían la orden en otra. La comprobación vive en el módulo.
                if (!INST::etiquetaValida(etiqueta)) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(B::format(T("t_web_e_netiq", "etiqueta no válida: «%1»"), {etiqueta}),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                if (objeto.find('@') == std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(T("t_web_las_retencione_58d5a1", "las retenciones son de una instantánea"),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                // La etiqueta va primero, y eso lo decide `commands::instantaneas`: aquí
                // estaba escrito en el orden correcto por costumbre, no por regla.
                verbo = que == "poner-hold" ? INST::argvRetener(etiqueta, objeto)
                                            : INST::argvSoltar(etiqueta, objeto);
            } else if (que == "entregar-pares") {
                // La carga la compone la capa base, la misma que usa el intérprete. Aquí solo
                // se decide a quién y se comprueba que haya algo que entregar.
                const PR::Entrega entrega = PR::componeEntrega(conns.perfiles, conn);
                if (!entrega.sePuede()) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(PR::etiquetaDe(entrega.fallo), sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = PET::fijaPares(entrega.cargaB64);
            } else if (que == "escucha-pares") {
                // El «objeto» es la dirección. Se valida con la MISMA lista que ofrece la
                // vista y que aplica el daemon: aquí solo se evita mandar algo que se sabe
                // que va a ser rechazado.
                if (!PR::direccionDeEscuchaValida(objeto)) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(
                        B::format(T("t_web_e_escucha", "dirección de escucha no admitida: «%1»"),
                                  {objeto}),
                        sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = PET::fijaEscucha(objeto);
            } else if (que == "cancelar-trabajo") {
                // El «objeto» aquí es el identificador del trabajo, no una ruta de ZFS. Se
                // comprueba que lo parezca antes de mandarlo: el daemon lo usa para buscar en
                // su lista, y un identificador con cualquier cosa dentro no encuentra nada
                // pero tampoco debería llegar.
                const std::string idT = B::trim(objeto);
                if (idT.empty()
                    || idT.find_first_not_of("abcdefghijklmnopqrstuvwxyz"
                                             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                             "0123456789_-.") != std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(
                        B::format(T("t_web_e_njob", "identificador de trabajo no válido: «%1»"), {idT}),
                        sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = PET::cancelaTrabajo(idT);
            } else if (que == "latido") {
                verbo = {"--heartbeat"};
            } else if (B::startsWith(que, "pool-")) {
                const std::string op = que.substr(5);
                // Destruir un pool pide además escribir su nombre: es lo único de la web que
                // se lleva un pool entero por delante.
                if (op == "destroy" && B::trim(p.campo("confirma")) != objeto) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(T("t_web_e_confirma",
                                             "el nombre escrito no coincide: no se ha "
                                             "destruido nada"),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                std::vector<std::string> argv;
                // El argv y la traducción de la fase salen de `commands::pools`, que es de
                // donde salen también los del intérprete.
                //
                // **Aquí había un fallo**: «Parar initialize» mandaba `-s`, que en
                // `zpool initialize` no es parar sino SUSPENDER —parar es `-c`—. El botón
                // decía una cosa y hacía otra. La letra no es la misma para las tres
                // operaciones y por eso no puede escribirse a mano en cada cliente.
                using Op = PL::Operacion;
                using Fase = PL::Fase;
                if (op == "scrub")                 { argv = PL::argv(Op::Scrub, objeto); }
                else if (op == "scrub-parar")      { argv = PL::argv(Op::Scrub, objeto, Fase::Parar); }
                else if (op == "trim")             { argv = PL::argv(Op::Trim, objeto); }
                else if (op == "trim-parar")       { argv = PL::argv(Op::Trim, objeto, Fase::Parar); }
                else if (op == "initialize")       { argv = PL::argv(Op::Initialize, objeto); }
                else if (op == "initialize-parar") { argv = PL::argv(Op::Initialize, objeto, Fase::Parar); }
                else if (op == "export")           { argv = PL::argv(Op::Export, objeto); }
                else if (op == "destroy")          { argv = PL::argv(Op::Destroy, objeto); }
                // Al importar, el «objeto» es el nombre del pool que la sonda encontró.
                else if (op == "import")           { argv = PL::argv(Op::Import, objeto); }
                else if (op == "sync")             { argv = PL::argv(Op::Sync, objeto); }
                else if (op == "clear")            { argv = PL::argv(Op::Clear, objeto); }
                else if (op == "upgrade")          { argv = PL::argv(Op::Upgrade, objeto); }
                else if (op == "reguid")           { argv = PL::argv(Op::Reguid, objeto); }
                // Las dos que no se deshacen. Llegan por la página de confirmación, igual
                // que `export` y `destroy`; aquí no hay diferencia porque el despacho es el
                // mismo — quien decide si se pregunta es el enlace que trae hasta aquí.

                else if (op == "import-como") {
                    // `zpool import <viejo> <nuevo>`. La validación del nombre vive en
                    // `commands::pools`, con la del intérprete y la de la interfaz.
                    argv = PL::argvImportarComo(objeto, B::trim(p.campo("nuevo")));
                    if (argv.empty()) {
                        r.codigo = 400;
                        r.cuerpo = paginaError(
                            B::format(T("t_web_e_npool", "nombre de pool no válido: «%1»"),
                                      {B::trim(p.campo("nuevo"))}),
                            sesion.testigo());
                        respuesta = H::componer(r);
                        return true;
                    }
                }
                if (argv.empty()) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(B::format(T("t_web_e_accpool", "acción de pool desconocida: «%1»"), {op}),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = PET::zpoolGenerico(argvEnBase64(argv));
            } else if (que == "reparar-montajes") {
                verbo = PET::reparaMontajesAlternativos({"apply"});
            } else if (que == "desde-dir") {
                const std::string dirOrigen = B::trim(p.campo("directorio"));
                const std::string connOrigen = B::trim(p.campo("origenconn"));
                const B::ConnectionProfile* perfilOrigen =
                    zfsmgr::cli::buscarConexion(conns, connOrigen);
                if (perfilOrigen == nullptr || dirOrigen.empty()) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(T("t_web_e_fd",
                                             "hace falta la máquina de origen y el directorio"),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                // El destino es el punto de montaje del dataset, que en Windows NO es la
                // propiedad `mountpoint`: ver montajeDeDataset.
                bool montado = false;
                std::string puntoDestino;
                if (!montajeDeDataset(*sesionZfs, *perfil, objeto, montado, puntoDestino)
                    || !montado) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(T("t_web_e_fd_mnt",
                                             "el dataset de destino tiene que estar montado"),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                SY::Plan plan;
                plan.rutaOrigen = dirOrigen;
                plan.rutaDestino = puntoDestino;
                std::string salF;
                std::string errF;
                std::string idF;
                // Sin borrado: traer un directorio no es sincronizar, es AÑADIR. Con
                // `--delete` esto se llevaría por delante lo que ya hubiera en el dataset.
                const bool okF = sincroniza(*sesionZfs, *perfilOrigen, *perfil, plan,
                                            connOrigen == conn, /*borrar=*/false,
                                            /*enSeco=*/false, op.verboso, salF, errF, idF);
                TR::Trabajo t;
                if (okF) {
                    t.id = idF;
                } else {
                    t.fallo = TR::FalloTrabajo::SinIdentificador;
                    t.detalle = B::trim(errF).empty() ? B::trim(salF) : B::trim(errF);
                }
                r.cuerpo = paginaTrabajoLanzado(conn, dirOrigen, objeto, t, TR::Reanudacion{},
                                                sesion.testigo(), QueTrabajo::Sincronizar);
                r.codigo = okF ? 200 : 502;
                respuesta = H::componer(r);
                return true;
            } else if (que == "desglosar" || que == "ensamblar" || que == "hacia-dir") {
                // Las tres mueven datos y pueden tardar, así que van como TRABAJO. El daemon
                // ya las admite en su lista de lanzables; el cliente solo recoge el
                // identificador y deja de esperar.
                std::vector<std::string> args;
                if (que == "desglosar") {
                    const std::vector<std::string> dirs = p.campos("subdir");
                    const std::vector<std::string> nombres = p.campos("nombre");
                    // Las filas vacías se saltan: el formulario ofrece tres y casi nunca se
                    // usan las tres. Y una a medias se descarta ENTERA, que es lo que evita
                    // que el verbo —que las lee de dos en dos— desplace todas las siguientes.
                    // Las dos reglas viven en `commands::avanzadas`.
                    std::vector<AV::Desglose> pares;
                    for (std::size_t i = 0; i < dirs.size() && i < nombres.size(); ++i) {
                        pares.push_back(AV::Desglose{dirs[i], nombres[i]});
                    }
                    args = AV::argvDesglosar(objeto, pares);
                    if (args.empty()) {
                        r.codigo = 400;
                        r.cuerpo = paginaError(T("t_web_e_bd",
                                                 "hace falta al menos un subdirectorio con el "
                                                 "nombre del dataset que lo sustituye"),
                                               sesion.testigo());
                        respuesta = H::componer(r);
                        return true;
                    }
                } else if (que == "ensamblar") {
                    // La regla de los nombres completos —y su porqué— vive en
                    // `commands::avanzadas`, que es de donde salen también los del intérprete
                    // y los de la interfaz. Aquí estaba escrita a mano por segunda vez.
                    args = AV::argvEnsamblar(objeto, p.campos("hijo"));
                    if (args.empty()) {
                        r.codigo = 400;
                        r.cuerpo = paginaError(T("t_web_e_as", "hay que elegir al menos un hijo"),
                                               sesion.testigo());
                        respuesta = H::componer(r);
                        return true;
                    }
                } else {
                    const std::string destino = B::trim(p.campo("destino"));
                    if (destino.empty() || destino[0] != '/') {
                        r.codigo = 400;
                        r.cuerpo = paginaError(T("t_web_e_td",
                                                 "el directorio de destino tiene que ser una "
                                                 "ruta absoluta"),
                                               sesion.testigo());
                        respuesta = H::componer(r);
                        return true;
                    }
                    args = AV::argvHaciaDir(objeto, destino, p.campo("borra") == "1");
                    if (args.empty()) {
                        r.codigo = 400;
                        r.cuerpo = paginaError(T("t_web_e_todir_ruta",
                                                 "el directorio de destino tiene que ser una "
                                                 "ruta absoluta"),
                                               sesion.testigo());
                        respuesta = H::componer(r);
                        return true;
                    }
                }
                const std::vector<std::string> conTrabajo = PET::encola(args);
                std::string salT;
                std::string errT;
                int rcT = 0;
                std::string idT;
                if (llamaAgente(*sesionZfs, *perfil, conTrabajo, salT, errT, rcT, nullptr, 60000)
                    && rcT == 0) {
                    idT = idDeTrabajoEn(salT);
                }
                TR::Trabajo t;
                if (!idT.empty()) {
                    t.id = idT;
                } else {
                    t.fallo = TR::FalloTrabajo::SinIdentificador;
                    t.detalle = B::trim(errT).empty() ? B::trim(salT) : B::trim(errT);
                }
                r.cuerpo = paginaTrabajoLanzado(conn, objeto, objeto, t, TR::Reanudacion{},
                                                sesion.testigo(), QueTrabajo::Copiar);
                r.codigo = idT.empty() ? 502 : 200;
                respuesta = H::componer(r);
                return true;
            } else if (que == "crear-pool") {
                const std::string nombre = B::trim(p.campo("nombre"));
                const std::vector<std::string> discos = p.campos("disco");
                if (nombre.empty() || nombre.find('/') != std::string::npos
                    || nombre.find(' ') != std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(B::format(T("t_web_e_npool",
                                                       "nombre de pool no válido: «%1»"),
                                                     {nombre}),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                if (discos.empty()) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(T("t_web_e_sindiscos",
                                             "hay que elegir al menos un dispositivo"),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                std::vector<std::string> argv = {"create", "-f", nombre};
                const std::string red = B::trim(p.campo("redundancia"));
                // «sin redundancia» no es una palabra de ZFS: es la ausencia de una. Se
                // traduce a no poner nada, que es lo que ZFS entiende como conjunto simple.
                if (!red.empty() && red != "sin redundancia") {
                    argv.push_back(red);
                }
                for (const std::string& d : discos) {
                    argv.push_back(d);
                }
                verbo = PET::zpoolGenerico(argvEnBase64(argv));
            } else if (que == "promover") {
                verbo = PET::zfsGenerico(argvEnBase64(DS::argvPromover(objeto)));
            } else if (que == "set") {
                const std::string prop = B::trim(p.campo("prop"));
                const std::string valor = p.campo("valor");
                if (prop.empty() || prop.find('=') != std::string::npos) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(B::format(T("t_web_e_nprop", "propiedad no válida: «%1»"), {prop}), sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = PET::zfsGenerico(argvEnBase64({"set", prop + "=" + valor, objeto}));
            } else {
                r.codigo = 400;
                r.cuerpo = paginaError(B::format(T("t_web_e_acc", "acción desconocida: «%1»"), {que}), sesion.testigo());
                respuesta = H::componer(r);
                return true;
            }

            std::string salidaM;
            std::string errM;
            int rcM = -1;
            std::string motivoM;
            const bool hablo = llamaAgente(*sesionZfs, *perfil, verbo, salidaM,
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
            // El latido es de la MÁQUINA, no de un dataset: se vuelve a su página con la
            // pestaña del daemon abierta, que es donde va a aparecer lo que acaba de
            // provocar. Sin este caso aparte, el reparto de abajo lo mandaba a
            // «/c/local/local» —un pool con el nombre de la máquina— que no existe.
            if (que == "latido") {
                r.codigo = 302;
                r.cabecerasExtra.push_back("Location: /c/" + H::haciaUrl(conn) + "?log=daemon%3A"
                                           + H::haciaUrl(conn));
                r.cuerpo = "";
                respuesta = H::componer(r);
                return true;
            }

            // Se vuelve al MISMO nodo sobre el que se actuó, con su árbol montado. Antes
            // se volvía a la raíz del pool y había que rehacer el camino a mano.
            const std::string volverA = objeto.find('@') != std::string::npos
                                            ? objeto.substr(0, objeto.find('@'))
                                            : objeto;
            const std::string raizV = B::trim(p.campo("raiz"));
            // Y a la MISMA vista de la que se venía cuando se dice: cambiando propiedades
            // se cambian varias seguidas, y volver al resumen cada vez obliga a rehacer el
            // camino hasta la tabla.
            const Vista vistaVuelta = vistaDesde(p.campo("volver"));
            r.codigo = 302;
            r.cabecerasExtra.push_back(
                "Location: " + urlDe(conn, raizV.empty() ? volverA : raizV, objeto, vistaVuelta));
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

        // La pestaña del registro viaja en la MISMA consulta que el resto, aparte de `sel`
        // y `v`: son ejes independientes —uno puede tener abierto el daemon de «oldlau»
        // mientras mira las propiedades de un dataset de «local»— y mezclarlos obligaría a
        // cerrar uno para ver el otro.
        const auto valorDeConsulta = [&p](const std::string& nombre) {
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
        const PestanaLog pestana = pestanaDesde(valorDeConsulta("log"));
        const DX::Extremo origenMarcado = origenDe(p);

        // Lo que la pestaña activa necesite de una máquina. «Combinado» y «Terminal» salen
        // de memoria y no piden nada; el daemon y los trabajos sí, y por eso solo se piden
        // cuando esa pestaña está abierta.
        std::string logCargado;
        if (!pestana.conexion.empty()
            && (pestana.tipo == "daemon" || pestana.tipo == "trabajos")) {
            const B::ConnectionProfile* perfilLog =
                zfsmgr::cli::buscarConexion(conns, pestana.conexion);
            if (perfilLog == nullptr) {
                logCargado = "<p class=\"vacio\">no hay ninguna conexión «"
                             + H::escapaHtml(pestana.conexion) + "»</p>";
            } else {
                std::string sal;
                std::string er;
                int rcL = -1;
                const std::vector<std::string> verboL =
                    pestana.tipo == "daemon" ? PET::registro(0, 0) : PET::listaDeTrabajos();
                if (llamaAgente(*sesionZfs, *perfilLog, verboL, sal, er, rcL, nullptr, 30000)
                    && rcL == 0) {
                    logCargado = pestana.tipo == "daemon" ? panelRegistroDaemon(sal, 300)
                                                          : panelTrabajos(sal, pestana.conexion,
                                                                       sesion.testigo());
                } else {
                    logCargado = "<p class=\"vacio\">no se pudo leer: "
                                 + H::escapaHtml(B::trim(er.empty() ? sal : er)) + "</p>";
                }
            }
        }

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
                versiones.push_back(saludDe(perfilC).versionAgente);
            }
            // La raíz es el MISMO árbol que todo lo demás, con `zfsm://` arriba y las
            // máquinas colgando. Antes era una tabla suelta, y desde ella había que pasar
            // por otra tabla de pools antes de ver un árbol: tres pantallas encadenadas
            // para llegar a un dataset, perdiendo de vista en cada salto de dónde se venía.
            const std::string izqR =
                panelArbol(conns.perfiles, std::string(), std::string(), {}, Arbol{},
                           std::string(), false);
            // En la raíz solo hay una cosa que enseñar, así que no hay barra: una barra de
            // una pestaña es un adorno.
            // Y el alta de una conexión nueva, que es lo que faltaba para poder montar una
            // instalación desde el navegador sin pasar por la interfaz ni por el intérprete.
            // Va plegada: la lista es lo que se viene a mirar, dar de alta es ocasional.
            std::string derR = "<div class=\"detalle\"><h2 class=\"detalletit\">zfsm://</h2>"
                               + panelConexiones(conns.perfiles, versiones)
                               + marco(T("t_web_cn_nueva", "Añadir una conexión"),
                                       formularioConexion(nullptr, sesion.testigo()), false)
                               + "</div>";
            r.cuerpo = envuelveDosPaneles(
                "zfsm://", "ZFSMgr", izqR, derR,
                ventanaDelLog(conns.perfiles, "/?", pestana, logCargado, sesion.testigo()),
                avisoDeOrigen(origenMarcado), sesion.testigo());
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
            // Sincronizar no se confirma a ciegas: esta página ENSEÑA la pasada en seco,
            // que es la única de las seis capaz de borrar en el destino. Sigue siendo un
            // GET honrado, porque `rsync -n` no toca nada.
            if (campoConsulta("que") == "sincronizar-desde-origen") {
                const std::string cS = campoConsulta("c");
                const std::string oS = campoConsulta("o");
                const DX::Extremo origenS = origenDe(p);
                const B::ConnectionProfile* perfilS =
                    zfsmgr::cli::buscarConexion(conns, cS);
                const B::ConnectionProfile* perfilOS =
                    origenS.vacio() ? nullptr
                                    : zfsmgr::cli::buscarConexion(conns, origenS.conexion);
                if (perfilS == nullptr || perfilOS == nullptr) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(T("t_web_sin_origen_marcado",
                                             "no hay ningún origen marcado, o su máquina ya no "
                                             "está en la lista"),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                SY::Extremo eO;
                eO.conexion = origenS.conexion;
                eO.objeto = origenS.objeto;
                eO.esWindows = B::transport::isWindowsConnection(*perfilOS);
                eO.tieneDaemon = saludDe(*perfilOS).vivo;
                SY::Extremo eD;
                eD.conexion = cS;
                eD.objeto = oS;
                eD.esWindows = B::transport::isWindowsConnection(*perfilS);
                eD.tieneDaemon = saludDe(*perfilS).vivo;
                // Los montajes cuestan una consulta por extremo, y por eso se preguntan
                // AQUÍ y no al pintar el menú de acciones.
                montajeDeDataset(*sesionZfs, *perfilOS, eO.objeto, eO.montado, eO.puntoMontaje);
                montajeDeDataset(*sesionZfs, *perfilS, eD.objeto, eD.montado, eD.puntoMontaje);
                const SY::Plan planS = SY::planea(eO, eD);
                if (!planS.sePuede()) {
                    r.codigo = 400;
                    r.cuerpo = paginaError(T("t_web_e_sync", "no se puede sincronizar: ")
                                               + SY::etiquetaDe(planS.fallo),
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                const bool borrar = (campoConsulta("del") == "1");
                std::string salS;
                std::string errS;
                std::string sinUso;
                const bool okSeco = sincroniza(*sesionZfs, *perfilOS, *perfilS, planS,
                                               origenS.conexion == cS, borrar,
                                               /*enSeco=*/true, op.verboso, salS, errS, sinUso);
                r.cuerpo = paginaConfirmarSincronizar(
                    cS, oS, origenS, campoConsulta("raiz"), borrar, !okSeco,
                    okSeco ? salS : (B::trim(errS).empty() ? salS : errS), sesion.testigo());
                respuesta = H::componer(r);
                return true;
            }
            r.cuerpo = paginaConfirmar(campoConsulta("c"), campoConsulta("o"),
                                       campoConsulta("que"), campoConsulta("raiz"),
                                       sesion.testigo(), origenDe(p));
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
            r.cuerpo = paginaError(B::format(T("t_web_e_conn", "no hay ninguna conexión «%1»"), {conn}), sesion.testigo());
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
            return llamaAgente(*sesionZfs, *perfil, args, salida, err, rc,
                                               &motivo, timeoutMs)
                   && rc == 0;
        };

        // La consulta de la URL: qué marco hay que abrir, si alguno.
        const auto campoDeConsulta = [&p](const std::string& nombre) {
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

        if (objeto.empty()) {
            if (!pide(PET::listaDePools(), 20000)) {
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
            // El marco que se pidió abrir, si se pidió alguno. Los tres cuestan una
            // consulta y por eso NO se hacen al entrar: se hacen al abrir el marco.
            const Vista vistaMaquina = vistaDesde(campoDeConsulta("v"));
            std::string cargadoMaquina;
            switch (vistaMaquina) {
                case Vista::Programacion:
                    cargadoMaquina = pide(PET::gsaDeTodosLosPools(), 30000)
                                         ? panelProgramacion(conn, std::string(),
                                                             std::string(), salida,
                                                             sesion.testigo())
                                         : std::string("<p class=\"vacio\">no se pudo leer la "
                                                       "programación</p>");
                    break;
                default:
                    break;   // nada abierto: no se consulta nada
            }

            const std::string izqC = panelArbol(conns.perfiles, conn, std::string(), pools,
                                               Arbol{}, std::string(), false);
            // Los marcos de la MÁQUINA, plegados como los demás. Registro y trabajos se
            // consultan al abrirlos, no al entrar: un registro de miles de líneas por cada
            // vez que uno pasa por la máquina es tiempo pagado para nada.
            // Las mismas pestañas que en un dataset, con lo que le toca a una MÁQUINA.
            //
            // El registro del daemon y los trabajos NO están: son pestañas de la ventana de
            // abajo —«Daemon» y «Transferencias»— y tenerlos en los dos sitios obliga a
            // elegir cuál de los dos se mira, que es una pregunta que no debería existir.
            // La programación sí, porque no está abajo y es de la máquina entera.
            const std::vector<std::pair<std::string, std::vector<Pestana>>> gruposC = {
                {std::string(),
                 {{Vista::Resumen, T("t_web_pools_2fd96d", "Pools")},
                  {Vista::Programacion, T("t_web_programacion_cca584", "Programación")},
                  {Vista::Pares, T("t_web_pares_tab", "Pares")},
                  {Vista::Acciones, T("t_help_actions_001", "Acciones")}}}};
            std::string cuerpoC;
            switch (vistaMaquina) {
                case Vista::Programacion:
                    cuerpoC = cargadoMaquina;
                    break;
                case Vista::Pares: {
                    const B::ConnectionProfile* perfilP = zfsmgr::cli::buscarConexion(conns, conn);
                    std::string salP;
                    std::string erP;
                    int rcP = -1;
                    const bool vivo =
                        perfilP != nullptr
                        && llamaAgente(*sesionZfs, *perfilP, PET::pares(), salP, erP, rcP,
                                       nullptr, 20000)
                        && rcP == 0;
                    if (!vivo) {
                        cuerpoC = "<p class=\"vacio\">"
                                  + H::escapaHtml(T("t_web_pares_sin_daemon",
                                                    "hace falta el daemon de esa máquina para "
                                                    "saber con quién puede hablar"))
                                  + "</p>";
                    } else {
                        cuerpoC = panelPares(salP, conn, sesion.testigo(), true);
                    }
                    break;
                }
                case Vista::Acciones: {
                    // La sonda de pools importables solo se lanza si la piden: recorre discos
                    // y puede tardar, y esta vista se abre a menudo para otras cosas.
                    std::vector<std::pair<std::string, std::string>> importables;
                    bool sondaHecha = false;
                    if (valorDeConsulta("sonda") == "1") {
                        sondaHecha = true;
                        std::string salS;
                        std::string errS;
                        int rcS = 0;
                        if (llamaAgente(*sesionZfs, *perfil, PET::sondaDeImportables(), salS,
                                        errS, rcS, nullptr, 120000)) {
                            importables = poolsImportables(salS);
                        }
                    }
                    std::string discos;
                    if (sondaHecha) {
                        std::string salD;
                        std::string errD;
                        int rcD = 0;
                        if (llamaAgente(*sesionZfs, *perfil, PET::dispositivosDeBloque(), salD, errD,
                                        rcD, nullptr, 60000)
                            && rcD == 0) {
                            discos = salD;
                        }
                    }
                    // Los montajes extraviados se preguntan con la misma sonda: el verbo sin
                    // «apply» no toca nada, así que enseñarlos no cuesta ningún riesgo.
                    std::vector<std::string> extraviados;
                    if (sondaHecha) {
                        std::string salR;
                        std::string errR;
                        int rcR = 0;
                        if (llamaAgente(*sesionZfs, *perfil, PET::reparaMontajesAlternativos({}), salR,
                                        errR, rcR, nullptr, 60000)
                            && rcR == 0) {
                            for (const std::string& l : B::split(salR, "\n", true)) {
                                if (B::startsWith(B::trim(l), "STRANDED=")) {
                                    extraviados.push_back(B::trim(l).substr(9));
                                }
                            }
                        }
                    }
                    cuerpoC = accionesDeMaquina(conn, zfsmgr::cli::buscarConexion(conns, conn),
                                                sesion.testigo(), importables, sondaHecha,
                                                discos, extraviados, conns.desconectada(conn));
                    break;
                }
                default:
                    cuerpoC = panelPools(conn, pools);
                    break;
            }
            const auto urlC = [&](Vista v) {
                return v == Vista::Resumen ? "/c/" + H::haciaUrl(conn)
                                           : "/c/" + H::haciaUrl(conn) + "?v=" + claveDeVista(v);
            };
            std::string derC;
            for (const auto& g : gruposC) {
                derC += "<div class=\"pestanas\">";
                for (const Pestana& t : g.second) {
                    derC += "<a class=\"pest"
                            + std::string(t.vista == vistaMaquina ? " activa" : "") + "\" href=\""
                            + H::escapaHtml(urlC(t.vista)) + "\">" + H::escapaHtml(t.texto)
                            + "</a>";
                }
                derC += "</div>";
            }
            derC += "<div class=\"detalle\"><h2 class=\"detalletit\">" + H::escapaHtml(conn)
                    + "</h2>" + cuerpoC + "</div>";
            // La base para los enlaces de las pestañas: la MISMA URL, conservando lo que
            // ya hubiera abierto. Cambiar de pestaña del registro no debe cerrar el marco
            // que uno estaba mirando.
            const std::string baseLog =
                "/c/" + H::haciaUrl(conn) + "?v=" + claveDeVista(vistaMaquina) + "&";
            r.cuerpo = envuelveDosPaneles(
                conn, enlace("/", "ZFSMgr"), izqC, derC,
                ventanaDelLog(conns.perfiles, baseLog, pestana, logCargado, sesion.testigo()),
                avisoDeOrigen(origenMarcado), sesion.testigo());
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

        // Los pools de esta máquina, para que el árbol enseñe los HERMANOS del que se
        // está mirando. Es una segunda llamada, y barata —`zpool list` no recorre nada—,
        // pero es lo que hace que desde dentro de un pool se pueda saltar a otro sin
        // volver atrás dos pantallas.
        std::vector<L::Pool> poolsDeLaMaquina;
        if (pide(PET::listaDePools(), 20000)) {
            std::string errP;
            L::pools(salida, poolsDeLaMaquina, errP);
        }
        const std::string poolDeLaRaiz = objeto.substr(0, objeto.find('/'));
        if (poolsDeLaMaquina.empty()) {
            L::Pool solo;
            solo.nombre = poolDeLaRaiz;
            poolsDeLaMaquina.push_back(solo);
        }

        if (!pide(PET::listaDeDatasets(objeto), 30000)) {
            r.codigo = 502;
            r.cuerpo = paginaError(B::format(T("t_web_e_list", "no se pudo listar «%1»"), {objeto}), sesion.testigo());
            respuesta = H::componer(r);
            return true;
        }
        const Arbol arbol = construyeArbol(L::entradas(salida), objeto);
        const std::string izq = panelArbol(conns.perfiles, conn, objeto, poolsDeLaMaquina, arbol,
                                           sel, true);
        const auto itSel = arbol.porNombre.find(sel);
        const L::Entrada* entradaSel =
            itSel != arbol.porNombre.end() ? &itSel->second : nullptr;

        // ¿Se puede transferir del origen marcado a lo que se está mirando?
        //
        // `exigeAsincrono` va en TRUE siempre desde aquí: este servidor atiende de una en
        // una y una petición HTTP no puede durar las horas que dura una copia. Solo vale el
        // camino que sostiene el daemon.
        TR::Plan planTransfer;
        // Sin origen marcado no hay nada que sincronizar; «el mismo objeto» es el motivo
        // que ya se pinta para las demás en ese caso.
        SY::Fallo falloSync = SY::Fallo::ElMismoObjeto;
        if (!origenMarcado.vacio()) {
            const B::ConnectionProfile* perfilOrigen =
                zfsmgr::cli::buscarConexion(conns, origenMarcado.conexion);
            if (perfilOrigen != nullptr) {
                const Salud sOrigen = saludDe(*perfilOrigen);
                const Salud sDestino = saludDe(*perfil);
                TR::Extremo eOrigen;
                eOrigen.conexion = origenMarcado.conexion;
                eOrigen.objeto = origenMarcado.objeto;
                eOrigen.esWindows = B::transport::isWindowsConnection(*perfilOrigen);
                eOrigen.tieneDaemon = sOrigen.vivo;
                eOrigen.admiteTrabajos = sOrigen.admiteTrabajos;
                eOrigen.versionZfs = sOrigen.versionZfs;
                TR::Extremo eDestino;
                eDestino.conexion = conn;
                eDestino.objeto = sel;
                eDestino.esWindows = B::transport::isWindowsConnection(*perfil);
                eDestino.tieneDaemon = sDestino.vivo;
                eDestino.admiteTrabajos = sDestino.admiteTrabajos;
                eDestino.versionZfs = sDestino.versionZfs;
                planTransfer = TR::planea(eOrigen, eDestino, /*exigeAsincrono=*/true);

                // Sincronizar tiene su propia regla: no comparte camino con las de
                // transferencia porque no manda bloques, compara ficheros. Aquí solo la
                // parte barata; los montajes se miran al pulsar.
                SY::Extremo sO;
                sO.conexion = origenMarcado.conexion;
                sO.objeto = origenMarcado.objeto;
                sO.esWindows = eOrigen.esWindows;
                sO.tieneDaemon = sOrigen.vivo;
                SY::Extremo sD;
                sD.conexion = conn;
                sD.objeto = sel;
                sD.esWindows = eDestino.esWindows;
                sD.tieneDaemon = sDestino.vivo;
                falloSync = SY::compruebo(sO, sD);
            }
        }

        // La plataforma de la máquina y el tipo del objeto: los dos hacen falta para saber
        // qué propiedad se puede escribir encima. `jailed` solo existe en FreeBSD, y a una
        // instantánea no se le cambia nada de ZFS —solo sus propiedades de usuario—.
        const ZP::Plataforma plataforma = ZP::plataformaDe(perfil->osType, std::string());
        const bool selEsInstantanea = sel.find('@') != std::string::npos;
        const std::string tipoDelObjeto = selEsInstantanea ? "snapshot" : "filesystem";

        // La BARRA de pestañas y, debajo, la elegida. Antes esto era una pila de marcos
        // plegables; con doce sobre un pool la pila era una lista de títulos entre los que
        // buscar, y abrir dos dejaba el segundo fuera de la pantalla.
        const bool esNodoDePool = (sel == objeto && objeto.find('/') == std::string::npos);
        std::vector<std::pair<std::string, std::vector<Pestana>>> grupos;

        // Las del pool van en su propio grupo, y solo cuando lo elegido ES el pool: dentro
        // de un dataset hijo, un «Historial del pool» hablaría de otra cosa. El grupo se
        // nombra porque el nodo del pool va FUNDIDO con su dataset raíz, y sin decirlo no
        // se sabe cuál de los dos objetos toca cada pestaña.
        if (esNodoDePool) {
            grupos.push_back({T("t_tree_pool_prefix_001", "Pool"),
                              {{Vista::Estado, T("t_web_estado_y_dis_70618f", "Estado y dispositivos")},
                               {Vista::PropsPool, T("t_props_tab_001", "Propiedades")},
                               {Vista::Capacidades, T("t_pool_caps_merged_001", "Capacidades")},
                               {Vista::Historial, T("t_pool_history_t1", "Historial")},
                               {Vista::AccionesPool, T("t_help_actions_001", "Acciones")}}});
        }

        std::vector<Pestana> delObjeto = {{Vista::Resumen, T("t_web_ficha_58dc18", "Ficha")},
                                          {Vista::Props, T("t_props_tab_001", "Propiedades")}};
        if (selEsInstantanea) {
            // Solo en instantáneas: `zfs holds` no admite un dataset —contesta «is not a
            // snapshot», comprobado— así que una pestaña ahí no tendría qué enseñar.
            delObjeto.push_back({Vista::Holds, T("t_web_holds_tab", "Retenciones")});
        }
        if (!selEsInstantanea) {
            const auto itS = arbol.instantaneas.find(sel);
            const std::size_t cuantas = itS == arbol.instantaneas.end() ? 0 : itS->second.size();
            delObjeto.push_back({Vista::Permisos, T("t_permissions_node_001", "Permisos")});
            delObjeto.push_back({Vista::Contenido, T("t_content_node_001", "Contenido")});
            delObjeto.push_back({Vista::Programacion, T("t_web_programacion_cca584", "Programación")});
            // Compuesta y no literal: se usa `format` con un hueco, que es como lo hace el
            // intérprete. Concatenar el número al texto deja la etiqueta a medio traducir,
            // y además obliga a que el número vaya siempre al final, cosa que no se puede
            // prometer de todos los idiomas.
            delObjeto.push_back(
                {Vista::Instantaneas,
                 B::format(T("t_web_instantaneas_n", "Instantáneas (%1)"),
                           {std::to_string(cuantas)})});
        }
        // «Comparar» solo sale cuando hay un origen que se puede comparar con esto: una
        // pestaña que al pulsarla dice «no aplica» es una pestaña de más.
        if (DX::compruebo(DX::Accion::Diff, origenMarcado, DX::Extremo{conn, sel})
            == DX::NoAplica::Ninguna) {
            delObjeto.push_back({Vista::Diff, DX::etiquetaDe(DX::Accion::Diff)});
        }
        delObjeto.push_back({Vista::Acciones, T("t_help_actions_001", "Acciones")});
        grupos.push_back({esNodoDePool ? T("t_web_dataset_105268", "Dataset") : std::string(),
                          delObjeto});

        // Una vista que no le corresponde a este objeto —«Capacidades» sobre un dataset,
        // «Contenido» sobre una instantánea— cae a la ficha en vez de enseñar un panel
        // vacío bajo una pestaña que no está en la barra.
        bool laHay = false;
        for (const auto& g : grupos) {
            for (const Pestana& t : g.second) {
                laHay = laHay || (t.vista == vista);
            }
        }
        const Vista vistaFinal = laHay ? vista : Vista::Resumen;

        // El panel derecho: TODOS los marcos que le caben a este nodo, y todos PLEGADOS.
        //
        // Es lo que sustituye a los nodos de adorno que colgaban del árbol. La diferencia
        // que importa no es dónde se pintan sino cuánto cuestan: allí estaban siempre,
        // multiplicados por el número de datasets; aquí el que no se abre no cuesta nada, y
        // el que se abre cuesta UNA consulta.
        //
        // Solo se consulta la vista que pide la URL. Llegar a un dataset no dispara cinco
        // preguntas de las que se van a mirar cero o una.
        std::string der;
        std::string loCargado;   // el cuerpo del marco activo, si lo hay
        bool cargoBien = true;
        const auto pideOFalla = [&](const std::vector<std::string>& args, const char* queCosa) {
            if (pide(args, 30000)) {
                return true;
            }
            // Un fallo al preguntar NO deja la página en blanco: el árbol de la izquierda
            // sigue montado y sirviendo para moverse, y los demás marcos siguen ahí.
            loCargado = "<p class=\"vacio\">no se pudo leer " + H::escapaHtml(queCosa) + ": "
                        + H::escapaHtml(B::trim(err.empty() ? salida : err)) + "</p>";
            cargoBien = false;
            return false;
        };
        const auto propsEn = [&](bool deUnPool, bool soloCapacidades) {
            std::vector<L::Propiedad> props;
            std::string errAnalisis;
            const bool vale = deUnPool ? L::propiedadesDePool(salida, props, errAnalisis)
                                       : L::propiedades(salida, props, errAnalisis);
            if (!vale) {
                loCargado = "<p class=\"vacio\">respuesta ilegible: " + H::escapaHtml(errAnalisis)
                            + "</p>";
                return;
            }
            // Las del POOL no se editan desde aquí: `zpool set` es otro mandato con otras
            // reglas, y ofrecer una caja que llama a `zfs set` sería ofrecer un error.
            loCargado = panelPropiedades(conn, objeto, deUnPool ? objeto : sel, props,
                                         soloCapacidades, deUnPool ? std::string() : tipoDelObjeto,
                                         plataforma, !deUnPool, sesion.testigo());
        };

        switch (vistaFinal) {
            case Vista::Resumen:
            case Vista::Acciones:
            case Vista::AccionesPool:
            // «Pares» es de la CONEXIÓN, no de un pool ni de un dataset. Aquí no puede
            // llegar por una pestaña —no se ofrece— pero sí escribiendo la URL a mano, y el
            // `switch` tiene que ser exhaustivo de todos modos. No se consulta nada.
            case Vista::Pares:
                break;   // nada que consultar: sale del árbol, o es un formulario
            case Vista::Instantaneas: {
                // UNA consulta con todas las instantáneas del dataset dentro. `zfs holds`
                // admite una lista, así que saber cuáles están retenidas cuesta una llamada
                // y no una por instantánea.
                const auto itS = arbol.instantaneas.find(sel);
                if (itS != arbol.instantaneas.end() && !itS->second.empty()) {
                    std::vector<std::string> objetos;
                    for (const L::Entrada& e : itS->second) {
                        objetos.push_back(e.nombre);
                    }
                    const std::vector<std::string> args = PET::holdsDe(objetos);
                    if (pide(args, 30000)) {
                        loCargado = salida;
                    }
                }
                break;
            }
            case Vista::Props:
                if (pideOFalla(PET::propiedadesDeDataset(sel), "las propiedades")) {
                    propsEn(false, false);
                }
                break;
            case Vista::Permisos:
                if (pideOFalla(PET::permisosDe(sel), "los permisos")) {
                    loCargado = panelPermisos(conn, objeto, sel, salida, sesion.testigo());
                }
                break;
            case Vista::Contenido: {
                const std::string punto =
                    entradaSel != nullptr ? B::trim(entradaSel->puntoMontaje) : std::string();
                if (punto.empty() || punto == "none" || punto == "-") {
                    loCargado = "<p class=\"vacio\">«" + H::escapaHtml(sel)
                                + "» no tiene punto de montaje.</p>";
                } else if (pideOFalla(PET::contenidoDeDirectorio(punto), "el contenido")) {
                    loCargado = panelContenido(conn, sel, salida);
                }
                break;
            }
            case Vista::Estado:
                if (pideOFalla(PET::estadoDePool(objeto), "el estado del pool")) {
                    loCargado = panelTexto(salida);
                }
                break;
            case Vista::PropsPool:
                if (pideOFalla(PET::propiedadesDePool(objeto), "las propiedades del pool")) {
                    propsEn(true, false);
                }
                break;
            case Vista::Capacidades:
                if (pideOFalla(PET::propiedadesDePool(objeto), "las capacidades del pool")) {
                    propsEn(true, true);
                }
                break;
            case Vista::Historial:
                if (pideOFalla(PET::historialDePool(objeto), "el historial")) {
                    loCargado = panelTexto(salida);
                }
                break;
            case Vista::Programacion:
                if (pideOFalla(PET::gsaDeDataset(sel), "la programación")) {
                    loCargado = panelProgramacion(conn, objeto, sel, salida,
                                                  sesion.testigo());
                }
                break;
            case Vista::Holds:
                if (pideOFalla(PET::holdsDe({sel}), "las retenciones")) {
                    loCargado = panelHolds(conn, objeto, sel, salida, sesion.testigo());
                }
                break;
            case Vista::Diff:
                // Los dos extremos en el orden que quiere `zfs diff`: primero el más
                // antiguo. Va el ORIGEN marcado contra el destino elegido, que es
                // exactamente lo que la regla acaba de dar por bueno.
                if (pideOFalla(PET::diferenciaEntre(origenMarcado.objeto, sel),
                               "la comparación")) {
                    loCargado = panelDiff(salida, origenMarcado.objeto, sel);
                }
                break;
        }
        (void)cargoBien;

        std::string cuerpo;
        switch (vistaFinal) {
            case Vista::Pares:
                // Se dice dónde vive en vez de dejar el panel en blanco: una página vacía
                // parece un fallo, y quien ha escrito esta URL buscaba algo concreto.
                cuerpo = "<p class=\"vacio\">"
                         + H::escapaHtml(T("t_web_pares_es_de_conn",
                                           "los pares son de la máquina, no de un pool:"))
                         + " " + enlace("/c/" + H::haciaUrl(conn) + "?v=pares",
                                        T("t_web_pares_ir", "verlos en la conexión"))
                         + "</p>";
                break;
            case Vista::Resumen:
                cuerpo = resumenDelNodo(sel, arbol);
                break;
            case Vista::Instantaneas:
                cuerpo = panelInstantaneas(conn, objeto, sel, arbol, loCargado,
                                           sesion.testigo());
                break;
            case Vista::Acciones:
                cuerpo = selEsInstantanea
                             ? accionesDeInstantanea(conn, objeto, sel, origenMarcado,
                                                     planTransfer, falloSync, sesion.testigo())
                             : accionesDeDataset(
                                   conn, objeto, sel, entradaSel, origenMarcado, planTransfer,
                                   falloSync, sesion.testigo(), hijosDirectosDe(arbol, sel),
                                   B::transport::isWindowsConnection(*perfil), nombresDe(conns));
                break;
            case Vista::AccionesPool:
                cuerpo = accionesDePool(conn, objeto, sesion.testigo());
                break;
            case Vista::Props:
            case Vista::Permisos:
            case Vista::Contenido:
            case Vista::Estado:
            case Vista::PropsPool:
            case Vista::Capacidades:
            case Vista::Historial:
            case Vista::Programacion:
            case Vista::Diff:
            case Vista::Holds:
                cuerpo = loCargado;
                break;
        }
        der = barraDePestanas(grupos, conn, objeto, sel, vistaFinal)
              + "<div class=\"detalle\"><h2 class=\"detalletit\">"
              + H::escapaHtml(tituloDeVista(vistaFinal, sel)) + "</h2>" + cuerpo + "</div>";

        const std::string migas = enlace("/", "ZFSMgr") + " / " + enlace("/c/" + H::haciaUrl(conn), conn)
                                  + " / " + enlace(urlDe(conn, objeto, objeto, Vista::Resumen), objeto);
        const std::string baseLog = "/c/" + H::haciaUrl(conn) + "/" + H::haciaUrl(objeto)
                                    + "?sel=" + H::haciaUrl(sel) + "&v=" + claveDeVista(vista)
                                    + "&";
        r.cuerpo = envuelveDosPaneles(
            // `vistaFinal` y no `vista`: si se pidió a mano una que no aplica, el título
            // tiene que decir lo que se está enseñando de verdad.
            tituloDeVista(vistaFinal, sel), migas, izq, der,
            ventanaDelLog(conns.perfiles, baseLog, pestana, logCargado, sesion.testigo()),
            avisoDeOrigen(origenMarcado), sesion.testigo());
        respuesta = H::componer(r);
        return true;
    };

    // Los túneles se cierran AL SALIR, por las dos puertas.
    //
    // Nadie lo hacía en este binario ni en el intérprete —solo la interfaz, en su
    // destructor—, y un `ssh -L` con `setsid()` sobrevive a quien lo montó. En la máquina
    // de desarrollo se habían acumulado 31, el más viejo de cuatro días, y uno se había
    // quedado con el 47654: el puerto por omisión de este mismo servidor, que por eso no
    // arrancaba.
    const auto cierraTuneles = [&] { B::transport::closeAllTunnels(sesionZfs->transporte); };

    // El camino por TROZOS: se prueba antes que `atiende` y solo hace algo cuando aquel ha
    // dejado apuntado un fichero. Se hacen las dos cosas en este orden —resolver primero,
    // servir después— porque resolver qué fichero es cuesta varias llamadas al agente y no
    // se puede hacer con la respuesta ya empezada.
    const auto atiendeChorro = [&](const std::string& crudo,
                                   const B::tlsserver::Escritor& escribe) {
        std::string respuestaCorta;
        if (!atiende(crudo, respuestaCorta)) {
            return false;
        }
        if (!ficheroPedido.hay) {
            // No era un fichero: lo contesta el camino normal, que ya tiene la respuesta
            // compuesta. Se escribe aquí para no atenderla dos veces.
            return respuestaCorta.empty() ? false : escribe(respuestaCorta.data(),
                                                            respuestaCorta.size());
        }
        const H::Peticion p = H::analiza(crudo);
        const long long total = ficheroPedido.tamano;

        // `Range: bytes=a-b`. Se atiende porque es lo que usan los gestores de descarga y
        // los reproductores para saltar por un fichero grande, y porque un explorador que
        // reanuda una copia cortada lo manda. Sin esto habría que volver a mandarlo entero.
        long long desde = 0;
        long long hasta = total > 0 ? total - 1 : 0;
        bool esRango = false;
        const std::string rango = p.cabecera("range");
        if (B::startsWith(B::toLowerAscii(rango), "bytes=")) {
            const std::string v = B::trim(rango.substr(6));
            const std::size_t guion = v.find('-');
            if (guion != std::string::npos) {
                const std::string a = B::trim(v.substr(0, guion));
                const std::string b = B::trim(v.substr(guion + 1));
                if (!a.empty()) {
                    desde = std::strtoll(a.c_str(), nullptr, 10);
                }
                if (!b.empty()) {
                    hasta = std::strtoll(b.c_str(), nullptr, 10);
                }
                if (desde >= 0 && hasta >= desde && desde < total) {
                    hasta = std::min(hasta, total - 1);
                    esRango = true;
                }
            }
        }
        const long long cuantos = (total == 0) ? 0 : (hasta - desde + 1);

        std::string cab = esRango ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
        cab += "Content-Type: application/octet-stream\r\n";
        cab += "Content-Length: " + std::to_string(cuantos) + "\r\n";
        cab += "Accept-Ranges: bytes\r\n";
        if (esRango) {
            cab += "Content-Range: bytes " + std::to_string(desde) + "-" + std::to_string(hasta)
                   + "/" + std::to_string(total) + "\r\n";
        }
        cab += "X-Content-Type-Options: nosniff\r\n";
        cab += "Content-Security-Policy: default-src \'none\'\r\n";
        cab += "Connection: close\r\n\r\n";
        if (!escribe(cab.data(), cab.size())) {
            return true;
        }
        if (p.metodo == "HEAD") {
            return true;
        }

        // De 4 MiB en 4 MiB. Es el tope que admite el daemon por trozo, y el que hace que
        // ni él ni este servidor tengan que sostener el fichero entero: una imagen de 50 GB
        // pasa por aquí con 4 MiB de memoria.
        constexpr long long kTrozo = 4ll * 1024 * 1024;
        long long puesto = 0;
        while (puesto < cuantos) {
            const long long pido = std::min(kTrozo, cuantos - puesto);
            std::string sal;
            std::string errT;
            int rcT = -1;
            if (!llamaAgente(*sesionZfs, ficheroPedido.perfil,
                             PET::contenidoDeFichero(ficheroPedido.ruta, desde + puesto, pido),
                             sal, errT, rcT, nullptr, 120000)
                || rcT != 0) {
                return true;   // ya se mandó la cabecera: cortar es todo lo que queda
            }
            const std::size_t nl = sal.find('\n');
            std::string datos;
            if (nl == std::string::npos
                || !B::base64Decode(B::trim(sal.substr(nl + 1)), datos)) {
                return true;
            }
            if (datos.empty()) {
                break;   // el fichero encogió mientras se servía
            }
            if (!escribe(datos.data(), datos.size())) {
                return true;
            }
            puesto += static_cast<long long>(datos.size());
        }
        return true;
    };

    std::string err;
    if (!B::tlsserver::sirve(op.bind, op.puerto, rutaCert, rutaClave, atiende,
                             [] { return g_vivo.load(); }, err, yaEscucha, atiendeChorro)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        cierraTuneles();
        return 1;
    }
    cierraTuneles();
    return 0;
}
