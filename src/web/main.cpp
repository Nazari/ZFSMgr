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
#include "gsa.h"
#include "i18n.h"
#include "listados.h"
#include "session.h"
#include "secretinput.h"
#include "storefiles.h"
// El `T(clave, castellano)` del intérprete: mismos catálogos, mismas claves, mismo
// ayudante. Un tercer sistema de traducción en el mismo programa acabaría discrepando.
#include "tr.h"
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
namespace ZP = zfsmgr::base::zfsprops;
namespace DX = zfsmgr::base::dosextremos;

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
                 "  --lang es|en|zh       Idioma. Sin él, el de la interfaz gráfica\n"
                 "                        (app.language de config.json). Cada navegador\n"
                 "                        puede elegir el suyo desde el pie de la página.\n"
                 "  -v, --verbose         Cuenta por la salida de error lo que hace el\n"
                 "                        transporte con cada máquina. Sin esto, un fallo\n"
                 "                        de transporte solo se ve como «no se pudo».\n"
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

// Lo que se le puede hacer a una MÁQUINA, que no es lo mismo que a un dataset.
//
// El registro, los trabajos y la programación ya NO están aquí: son marcos propios, porque
// no son acciones sino cosas que se miran. Un enlace que solo enseña algo no pinta en el
// mismo sitio que un botón que cambia la máquina.
std::string accionesDeMaquina(const std::string& conn) {
    return grupoDeAcciones(T("t_conn_agent_001", "Daemon"),
                           enlace("/confirmar?c=" + H::haciaUrl(conn) + "&que=instalar-daemon",
                                  T("t_web_instalar_o_a_81953d", "Instalar o actualizar el daemon…")));
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
    }
    return "";
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
        case Vista::Instantaneas: return "Instantáneas de " + objeto;
        case Vista::Acciones:     return "Acciones sobre " + objeto;
        case Vista::AccionesPool: return "Acciones sobre el pool " + objeto;
        case Vista::Diff:         return "Comparar con " + objeto;
    }
    return objeto;
}

Vista vistaDesde(const std::string& s) {
    static const Vista todas[] = {
        Vista::Resumen,   Vista::Props,      Vista::Permisos,  Vista::Contenido,
        Vista::Estado,    Vista::PropsPool,  Vista::Capacidades, Vista::Historial,
        Vista::Programacion, Vista::Instantaneas, Vista::Acciones, Vista::AccionesPool,
        Vista::Diff,
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
                                  const std::string& testigo) {
    const DX::Extremo destino{conn, sel};
    std::string h;
    for (const DX::Accion a : {DX::Accion::Diff, DX::Accion::Clonar, DX::Accion::Copiar,
                               DX::Accion::Mover, DX::Accion::Sincronizar, DX::Accion::Nivelar}) {
        const DX::NoAplica porQue = DX::compruebo(a, origen, destino);
        const std::string etiqueta = DX::etiquetaDe(a);
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
std::string accionesDeDataset(const std::string& conn, const std::string& raiz,
                              const std::string& ds, const L::Entrada* e,
                              const DX::Extremo& origen, const std::string& testigo) {
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

    if (cifrado) {
        // La frase de cifrado va por un campo de CONTRASEÑA y por POST, nunca en la URL:
        // una URL se queda en el historial del navegador y en el registro de cualquier
        // intermediario. Es la misma regla que impide pasarla por argumento al agente.
        std::string cif;
        cif += boton(conn, ds, raiz, "cargar-clave", T("t_ctx_load_key001", "Cargar clave"), testigo,
                     "<input type=\"password\" name=\"frase\" placeholder=\"frase\" required> ");
        cif += boton(conn, ds, raiz, "descargar-clave", T("t_ctx_unload_key001", "Descargar clave"), testigo);
        h += grupoDeAcciones(T("t_web_clave_de_cif_e5875e", "Clave de cifrado"), cif);
    }

    // Las de DOS extremos. El origen se marca aquí y se usa desde otro nodo.
    h += grupoDeAcciones(T("t_web_origen_destino", "Con dos extremos"),
                         "<div>" + enlace("/origen?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(ds),
                                          T("t_web_marcar_origen", "Marcar como origen"))
                             + "</div>"
                             + accionesDeDosExtremos(conn, raiz, ds, origen, testigo));
    h += "<div class=\"pendiente\">Desglosar, Ensamblar, Desde Dir y Hacia Dir todavía no "
         "están en la web. Se hacen desde la interfaz o desde el intérprete.</div>";
    return h;
}

// El menú de una INSTANTÁNEA. En Qt son cinco entradas; aquí están las tres que no piden
// un segundo extremo.
std::string accionesDeInstantanea(const std::string& conn, const std::string& raiz,
                                  const std::string& snap, const DX::Extremo& origen,
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
                             + accionesDeDosExtremos(conn, raiz, snap, origen, testigo));
    h += "<div class=\"pendiente\">«Nuevo Hold» y «Release» no están: el daemon no tiene "
         "todavía un verbo para leer los holds, y la interfaz los lee por shell — que es "
         "justo lo que este servidor no hace.</div>";
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
    g += boton(conn, pool, pool, "pool-sync", T("t_web_sync_905f63", "Sync"), testigo);
    g += boton(conn, pool, pool, "pool-clear", T("t_web_clear_719ea3", "Clear"), testigo);
    h += grupoDeAcciones(T("t_ctx_management001", "Gestión"), g);
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
                              const std::string& testigo) {
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
    std::string h;
    for (const auto& grupo : B::gsa::agrupaInstantaneas(cortos)) {
        h += "<div class=\"grupotit\">" + H::escapaHtml(etiquetaDeClase(grupo.first)) + " ("
             + std::to_string(grupo.second.size()) + ")</div>";
        std::vector<std::vector<std::string>> filas;
        for (const std::string& corto : grupo.second) {
            const auto it = porCorto.find(corto);
            const std::string entera = ds + "@" + corto;
            filas.push_back(
                {enlace(urlDe(conn, raiz, entera, Vista::Resumen), corto),
                 it == porCorto.end() ? std::string() : H::escapaHtml(bytesLegibles(it->second->usado)),
                 it == porCorto.end() ? std::string() : H::escapaHtml(fechaLegible(it->second->creacion)),
                 enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(entera)
                            + "&que=rollback&raiz=" + H::haciaUrl(raiz), T("t_web_rollback_a61c5c", "Rollback…"))
                     + " · "
                     + enlace("/confirmar?c=" + H::haciaUrl(conn) + "&o=" + H::haciaUrl(entera)
                                  + "&que=borrar-instantanea&raiz=" + H::haciaUrl(raiz), T("t_web_borrar_6b5f63", "Borrar…"))});
        }
        h += tabla({T("t_poolcrt_auto004", "Nombre"), T("t_web_usado_7f0217", "Usado"), T("t_web_creacion_4e62d9", "Creación"), ""}, filas);
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
std::string panelTrabajos(const std::string& crudo) {
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
        filas.push_back({H::escapaHtml(j["id"].toString()),
                         H::escapaHtml(j["type"].toString()),
                         H::escapaHtml(j["snap"].toString()),
                         estado == "done" ? std::string("done")
                                          : "<span class=\"malo\">" + H::escapaHtml(estado)
                                                + "</span>",
                         H::escapaHtml(bytesLegibles(std::to_string(j["bytes"].toInt()))),
                         H::escapaHtml(fechaIso(j["started"].toString())),
                         H::escapaHtml(j["error"].toString())});
    }
    if (filas.empty()) {
        return "<p class=\"vacio\">(no hay trabajos en esta máquina)</p>";
    }
    return tabla({T("t_web_id_474ae5", "Id"), T("t_tipo_6cc619", "Tipo"), T("t_web_instantanea_659df4", "Instantánea"), T("t_status_001", "Estado"), T("t_web_bytes_8e5fda", "Bytes"), "Empezó", T("t_web_error_7f2f6a", "Error")}, filas);
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
         + casilla("recursivo", "Recursiva (cubre los descendientes)", mia.prog.recursivo)
         + "</div>";
    f += "<div class=\"fila\">" + numero("horario", T("t_web_horarias_5399f3", "Horarias"), mia.prog.horario)
         + numero("diario", T("t_web_diarias_31be0d", "Diarias"), mia.prog.diario)
         + numero("semanal", T("t_ctx_snap_group_weekly", "Semanales"), mia.prog.semanal)
         + numero("mensual", T("t_ctx_snap_group_monthly", "Mensuales"), mia.prog.mensual)
         + numero("anual", T("t_ctx_snap_group_yearly", "Anuales"), mia.prog.anual) + "</div>";
    f += "<p class=\"tenue\">Cada número es cuántas se guardan de esa clase. Un cero es «no "
         "hagas ninguna», no «guárdalas todas».</p>";
    f += "<div class=\"fila\">" + casilla("nivelar", "Nivelar con el destino", mia.prog.nivelar)
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
    } else if (que == "instalar-daemon") {
        texto = "Se va a REEMPLAZAR el binario del daemon en «" + conn
                + "» y reiniciar su servicio. Mientras dure, esa máquina deja de contestar; "
                  "lo que estuviera en marcha allí —un scrub, una transferencia— se corta.";
    } else {
        texto = "Acción desconocida.";
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
        if (a == "-v" || a == "--verbose") { op.verboso = true; continue; }
        if (a == "--lang" && i + 1 < argc) { op.idioma = argv[++i]; continue; }
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

    auto sesionZfs = zfsmgr::cli::crearSesion(op.dirConfig, maestra, op.verboso);
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
    // El cartel NO se imprime aquí: se imprime cuando el socket ya escucha. Escribirlo
    // antes anunciaba «zfsmgr-web escuchando en …» con su URL de sesión y a continuación
    // «no se pudo escuchar», dejando al usuario con un enlace que nunca funcionó.
    const auto yaEscucha = [&] {
        std::fprintf(stderr, "zfsmgr-web escuchando en https://%s:%d/\n", op.bind.c_str(),
                     op.puerto);
        std::fprintf(stderr, "abra esta URL, que lleva la sesión:\n  https://%s:%d/?s=%s\n",
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
            // Un FICHERO no se sirve por aquí: se apunta cuál es y lo manda el camino por
            // trozos, que es el único que puede con una imagen de 726 MB sin cargarla en
            // memoria. Aquí solo se comprueba que existe y de qué tamaño es.
            if (!rutaBaseFs.empty() && perfilPedido && !esDirectorioDeVerdad) {
                std::string sal;
                std::string errG;
                int rcG = -1;
                if (llamaAgente(*sesionZfs, *perfilPedido,
                                {"--dump-file", rutaBaseFs, "0", "1"}, sal, errG, rcG, nullptr,
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
            const B::ConnectionProfile* perfil = zfsmgr::cli::buscarConexion(conns, conn);
            if (!perfil || objeto.empty()) {
                r.codigo = 400;
                r.cuerpo = paginaError("falta la conexión o el objeto", sesion.testigo());
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
                versionPorConexion.erase(perfil->id.empty() ? perfil->name : perfil->id);
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
                                         {"--mutate-zfs-generic",
                                          argvEnBase64({"inherit", kv.first, objeto})},
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
                verbo = {"--mutate-zfs-generic", argvEnBase64(argv)};
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
                    r.cuerpo = paginaError("nombre de clon no válido: «" + nombre + "»",
                                           sesion.testigo());
                    respuesta = H::componer(r);
                    return true;
                }
                verbo = {"--mutate-zfs-clone", origen.objeto, objeto + "/" + nombre};
            } else if (que == "latido") {
                verbo = {"--heartbeat"};
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
                    pestana.tipo == "daemon" ? std::vector<std::string>{"--dump-daemon-log", "0"}
                                             : std::vector<std::string>{"--job-list"};
                if (llamaAgente(*sesionZfs, *perfilLog, verboL, sal, er, rcL, nullptr, 30000)
                    && rcL == 0) {
                    logCargado = pestana.tipo == "daemon" ? panelRegistroDaemon(sal, 300)
                                                          : panelTrabajos(sal);
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
                if (llamaAgente(*sesionZfs, perfilC, {"--health"}, salidaH, errH,
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
            // La raíz es el MISMO árbol que todo lo demás, con `zfsm://` arriba y las
            // máquinas colgando. Antes era una tabla suelta, y desde ella había que pasar
            // por otra tabla de pools antes de ver un árbol: tres pantallas encadenadas
            // para llegar a un dataset, perdiendo de vista en cada salto de dónde se venía.
            const std::string izqR =
                panelArbol(conns.perfiles, std::string(), std::string(), {}, Arbol{},
                           std::string(), false);
            // En la raíz solo hay una cosa que enseñar, así que no hay barra: una barra de
            // una pestaña es un adorno.
            std::string derR = "<div class=\"detalle\"><h2 class=\"detalletit\">zfsm://</h2>"
                               + panelConexiones(conns.perfiles, versiones) + "</div>";
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
            // El marco que se pidió abrir, si se pidió alguno. Los tres cuestan una
            // consulta y por eso NO se hacen al entrar: se hacen al abrir el marco.
            const Vista vistaMaquina = vistaDesde(campoDeConsulta("v"));
            std::string cargadoMaquina;
            switch (vistaMaquina) {
                case Vista::Programacion:
                    cargadoMaquina = pide({"--dump-zfs-get-gsa-raw-all-pools"}, 30000)
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
                  {Vista::Acciones, T("t_help_actions_001", "Acciones")}}}};
            std::string cuerpoC;
            switch (vistaMaquina) {
                case Vista::Programacion:
                    cuerpoC = cargadoMaquina;
                    break;
                case Vista::Acciones:
                    cuerpoC = accionesDeMaquina(conn);
                    break;
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
        if (pide({"--dump-zpool-list"}, 20000)) {
            std::string errP;
            L::pools(salida, poolsDeLaMaquina, errP);
        }
        const std::string poolDeLaRaiz = objeto.substr(0, objeto.find('/'));
        if (poolsDeLaMaquina.empty()) {
            L::Pool solo;
            solo.nombre = poolDeLaRaiz;
            poolsDeLaMaquina.push_back(solo);
        }

        if (!pide({"--dump-zfs-list-all", objeto}, 30000)) {
            r.codigo = 502;
            r.cuerpo = paginaError("no se pudo listar «" + objeto + "»", sesion.testigo());
            respuesta = H::componer(r);
            return true;
        }
        const Arbol arbol = construyeArbol(L::entradas(salida), objeto);
        const std::string izq = panelArbol(conns.perfiles, conn, objeto, poolsDeLaMaquina, arbol,
                                           sel, true);
        const auto itSel = arbol.porNombre.find(sel);
        const L::Entrada* entradaSel =
            itSel != arbol.porNombre.end() ? &itSel->second : nullptr;

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
            case Vista::Instantaneas:
            case Vista::Acciones:
            case Vista::AccionesPool:
                break;   // nada que consultar: sale del listado del árbol, o es un formulario
            case Vista::Props:
                if (pideOFalla({"--dump-zfs-get-all", sel}, "las propiedades")) {
                    propsEn(false, false);
                }
                break;
            case Vista::Permisos:
                if (pideOFalla({"--dump-zfs-allow", sel}, "los permisos")) {
                    loCargado = panelTexto(salida);
                }
                break;
            case Vista::Contenido: {
                const std::string punto =
                    entradaSel != nullptr ? B::trim(entradaSel->puntoMontaje) : std::string();
                if (punto.empty() || punto == "none" || punto == "-") {
                    loCargado = "<p class=\"vacio\">«" + H::escapaHtml(sel)
                                + "» no tiene punto de montaje.</p>";
                } else if (pideOFalla({"--dump-dir-list", punto}, "el contenido")) {
                    loCargado = panelContenido(conn, sel, salida);
                }
                break;
            }
            case Vista::Estado:
                if (pideOFalla({"--dump-zpool-status", objeto}, "el estado del pool")) {
                    loCargado = panelTexto(salida);
                }
                break;
            case Vista::PropsPool:
                if (pideOFalla({"--dump-zpool-get-all", objeto}, "las propiedades del pool")) {
                    propsEn(true, false);
                }
                break;
            case Vista::Capacidades:
                if (pideOFalla({"--dump-zpool-get-all", objeto}, "las capacidades del pool")) {
                    propsEn(true, true);
                }
                break;
            case Vista::Historial:
                if (pideOFalla({"--dump-zpool-history", objeto}, "el historial")) {
                    loCargado = panelTexto(salida);
                }
                break;
            case Vista::Programacion:
                if (pideOFalla({"--dump-zfs-get-gsa-raw-recursive", sel}, "la programación")) {
                    loCargado = panelProgramacion(conn, objeto, sel, salida,
                                                  sesion.testigo());
                }
                break;
            case Vista::Diff:
                // Los dos extremos en el orden que quiere `zfs diff`: primero el más
                // antiguo. Va el ORIGEN marcado contra el destino elegido, que es
                // exactamente lo que la regla acaba de dar por bueno.
                if (pideOFalla({"--dump-zfs-diff", origenMarcado.objeto, sel},
                               "la comparación")) {
                    loCargado = panelDiff(salida, origenMarcado.objeto, sel);
                }
                break;
        }
        (void)cargoBien;

        std::string cuerpo;
        switch (vistaFinal) {
            case Vista::Resumen:
                cuerpo = resumenDelNodo(sel, arbol);
                break;
            case Vista::Instantaneas:
                cuerpo = panelInstantaneas(conn, objeto, sel, arbol, sesion.testigo());
                break;
            case Vista::Acciones:
                cuerpo = selEsInstantanea
                             ? accionesDeInstantanea(conn, objeto, sel, origenMarcado,
                                                     sesion.testigo())
                             : accionesDeDataset(conn, objeto, sel, entradaSel, origenMarcado,
                                                 sesion.testigo());
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
                             {"--dump-file", ficheroPedido.ruta, std::to_string(desde + puesto),
                              std::to_string(pido)},
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
