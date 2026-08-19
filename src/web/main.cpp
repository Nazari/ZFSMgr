// zfsmgr-web — fase 0.
//
// Ver docs/diseno_tecnico_servidor_web.md. Aquí solo hay: TLS, sesión, testigo anti-CSRF y
// UNA página con la lista de conexiones. Nada de mutaciones todavía, a propósito: la fase 0
// existe para dejar bien la superficie, que es donde está el riesgo.
//
// Corre como el USUARIO, no como root, y escucha en 127.0.0.1. El daemon no se toca.
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
#include <string>
#include <vector>

namespace B = zfsmgr::base;
namespace ST = zfsmgr::base::store;
namespace CJ = zfsmgr::base::connjson;
namespace H = zfsmgr::web::http;
namespace L = zfsmgr::base::listados;

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

// La cabecera y el pie, iguales en todas las páginas. Sin CSS externo: la CSP no lo
// permite y una hoja de estilo propia va en la fase 4, cuando haya algo que estilar.
std::string envuelve(const std::string& titulo, const std::string& migas,
                     const std::string& cuerpo, const std::string& testigo) {
    std::string h = "<!doctype html><html lang=\"es\"><head><meta charset=\"utf-8\">";
    h += "<title>" + H::escapaHtml(titulo) + " — ZFSMgr</title></head><body>";
    h += "<p>" + migas + "</p>";
    h += "<h1>" + H::escapaHtml(titulo) + "</h1>";
    h += cuerpo;
    h += "<hr><form method=\"post\" action=\"/salir\">";
    h += "<input type=\"hidden\" name=\"testigo\" value=\"" + H::escapaHtml(testigo) + "\">";
    h += "<button type=\"submit\">Cerrar el servidor</button></form>";
    h += "<p>zfsmgr-web " + H::escapaHtml(B::agentversion::laEsperada()) + "</p>";
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
        return "<p>(no hay nada que enseñar)</p>";
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

std::string paginaConexiones(const std::vector<B::ConnectionProfile>& perfiles,
                             const std::string& testigo) {
    std::vector<std::vector<std::string>> filas;
    for (const B::ConnectionProfile& p : perfiles) {
        const std::string id = p.id.empty() ? p.name : p.id;
        filas.push_back({enlace("/c/" + id, id),
                         H::escapaHtml(p.name),
                         H::escapaHtml(p.connType),
                         H::escapaHtml(p.osType),
                         H::escapaHtml(p.host),
                         H::escapaHtml(p.username)});
    }
    return envuelve("Conexiones", "ZFSMgr",
                    tabla({"ID", "Nombre", "Tipo", "Sistema", "Host", "Usuario"}, filas),
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
    return envuelve(conn, enlace("/", "ZFSMgr"),
                    tabla({"Pool", "Salud", "Tamaño", "Libre", "Uso"}, filas), testigo);
}

std::string paginaDatasets(const std::string& conn, const std::string& raiz,
                           const std::vector<L::Entrada>& entradas, const std::string& testigo) {
    std::vector<std::vector<std::string>> datasets;
    std::vector<std::vector<std::string>> instantaneas;
    for (const L::Entrada& e : entradas) {
        if (e.esInstantanea()) {
            instantaneas.push_back({H::escapaHtml(e.nombre.substr(e.nombre.find('@') + 1)),
                                    H::escapaHtml(bytesLegibles(e.usado)),
                                    H::escapaHtml(e.creacion)});
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
    cuerpo += tabla({"Nombre", "Usado", "Creación"}, instantaneas);
    cuerpo += "<p>" + enlace("/c/" + conn + "/" + raiz + "?props=1", "Ver propiedades") + "</p>";
    const std::string migas = enlace("/", "ZFSMgr") + " / " + enlace("/c/" + conn, conn);
    return envuelve(raiz, migas, cuerpo, testigo);
}

std::string paginaPropiedades(const std::string& conn, const std::string& objeto,
                              const std::vector<L::Propiedad>& props, const std::string& testigo) {
    std::vector<std::vector<std::string>> filas;
    for (const L::Propiedad& p : props) {
        filas.push_back({H::escapaHtml(p.nombre), H::escapaHtml(p.valor), H::escapaHtml(p.origen)});
    }
    const std::string migas = enlace("/", "ZFSMgr") + " / " + enlace("/c/" + conn, conn)
                              + " / " + enlace("/c/" + conn + "/" + objeto, objeto);
    return envuelve("Propiedades de " + objeto, migas,
                    tabla({"Propiedad", "Valor", "Origen"}, filas), testigo);
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
            r.cuerpo = paginaConexiones(conns.perfiles, sesion.testigo());
            respuesta = H::componer(r);
            return true;
        }

        // `/c/<conexión>[/<pool>[/<dataset>]]`. Se trocea a mano y no con una tabla de
        // rutas porque son tres formas y una tabla aquí sería más código que el reparto.
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
        r.cuerpo = paginaDatasets(conn, objeto, L::entradas(salida), sesion.testigo());
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
