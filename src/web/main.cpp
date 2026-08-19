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

std::string paginaConexiones(const std::vector<B::ConnectionProfile>& perfiles,
                             const std::string& testigo) {
    std::string h;
    h += "<!doctype html><html lang=\"es\"><head><meta charset=\"utf-8\">";
    h += "<title>ZFSMgr</title></head><body>";
    h += "<h1>Conexiones</h1>";
    h += "<table><thead><tr><th>ID</th><th>Nombre</th><th>Tipo</th><th>Sistema</th>"
         "<th>Host</th><th>Usuario</th></tr></thead><tbody>";
    for (const B::ConnectionProfile& p : perfiles) {
        h += "<tr>";
        // TODO lo que viene de la configuración se escapa: el nombre de una conexión lo
        // escribe una persona, y un «<script>» ahí no puede acabar ejecutándose.
        h += "<td>" + H::escapaHtml(p.id) + "</td>";
        h += "<td>" + H::escapaHtml(p.name) + "</td>";
        h += "<td>" + H::escapaHtml(p.connType) + "</td>";
        h += "<td>" + H::escapaHtml(p.osType) + "</td>";
        h += "<td>" + H::escapaHtml(p.host) + "</td>";
        h += "<td>" + H::escapaHtml(p.username) + "</td>";
        h += "</tr>";
    }
    h += "</tbody></table>";
    // El testigo ya viaja en la página aunque todavía no haya nada que mutar: así la fase 2
    // no tiene que inventarse el mecanismo, solo usarlo.
    h += "<form method=\"post\" action=\"/salir\">";
    h += "<input type=\"hidden\" name=\"testigo\" value=\"" + H::escapaHtml(testigo) + "\">";
    h += "<button type=\"submit\">Cerrar el servidor</button></form>";
    h += "<p>zfsmgr-web " + H::escapaHtml(B::agentversion::laEsperada()) + "</p>";
    h += "</body></html>";
    return h;
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
        if (p.ruta != "/") {
            r.codigo = 404;
            r.tipo = "text/plain; charset=utf-8";
            r.cuerpo = "no hay nada aqui\n";
            respuesta = H::componer(r);
            return true;
        }

        // La configuración se relee en cada petición: es barato y evita servir una lista
        // rancia si se edita por el intérprete mientras esto corre.
        ST::Aviso aviso;
        const B::json::Value raiz = ST::leerConfig(op.dirConfig, aviso);
        std::vector<B::ConnectionProfile> perfiles;
        ST::Avisos avisos;
        for (const B::json::Value& v : raiz["connections"].toArray()) {
            B::ConnectionProfile perfil = CJ::connectionFromJson(v, std::string());
            CJ::abreSecretos(perfil, maestra, avisos);
            perfiles.push_back(perfil);
        }
        CJ::aseguraPerfilLocal(perfiles, std::string());
        r.cuerpo = paginaConexiones(perfiles, sesion.testigo());
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
