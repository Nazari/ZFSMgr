// zfsmgr-cli: la aplicación por línea de órdenes.
//
// Existe por dos motivos. El práctico: guiones, `cron` e integración continua, donde no
// hay sesión gráfica. Y el que importa aquí: **es la comprobación ejecutable de que la
// lógica está fuera de la interfaz**. Si el CLI puede hacer algo, esa parte ya no depende
// de Qt; si no puede, señala exactamente dónde sigue metida.
//
// Enlaza SOLO contra `zfsmgr_base`. No hay Qt en este binario, y ese es el contrato.
//
// Ver docs/diseno_tecnico_capa_base_sin_qt.md.

#include "connectionjson.h"
#include "secretcipher.h"
#include "secretinput.h"
#include "storefiles.h"
#include "storewarnings.h"
#include "strutil.h"
#include "zfsmurl.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

namespace B = zfsmgr::base;
namespace ST = zfsmgr::base::store;
namespace CJ = zfsmgr::base::connjson;

constexpr const char* kNombre = "zfsmgr-cli";

void uso() {
    std::fprintf(stderr,
                 "Uso: %s [opciones] <orden>\n"
                 "\n"
                 "Órdenes:\n"
                 "  connections list      Lista las conexiones configuradas\n"
                 "  url parse <zfsm://…>  Analiza una URL y enseña qué nombra\n"
                 "  version               Versión de esta herramienta\n"
                 "\n"
                 "Opciones:\n"
                 "  --password-fd <n>     Lee la contraseña maestra de ese descriptor.\n"
                 "                        Sin ella se pregunta por el terminal.\n"
                 "  --config-dir <ruta>   Directorio de configuración (por omisión,\n"
                 "                        ~/.config/ZFSMgr)\n"
                 "  --no-secrets          No descifra nada: no pide contraseña maestra.\n"
                 "                        Los campos cifrados salen como <cifrado>.\n"
                 "  -h, --help            Esta ayuda\n"
                 "\n"
                 "La contraseña maestra NO se pasa por argumento ni por variable de\n"
                 "entorno: las dos cosas quedan visibles en `ps` para cualquier usuario\n"
                 "de la máquina. Con el descriptor se puede usar cualquier gestor de\n"
                 "secretos:\n"
                 "\n"
                 "  %s --password-fd 3 connections list  3< <(pass show zfsmgr)\n",
                 kNombre, kNombre);
}

// El texto de un motivo. La capa base devuelve motivos tipificados a propósito para no
// llevarse consigo el sistema de traducción; aquí se les pone texto, como hace la
// interfaz con el suyo.
std::string textoDe(const ST::Aviso& a) {
    switch (a.motivo) {
        case ST::Motivo::Ninguno: return {};
        case ST::Motivo::ConfigNoSeAbre: return "no se pudo abrir config.json";
        case ST::Motivo::ConfigNoValido:
            return "config.json no es válido" + (a.detalle.empty() ? std::string() : ": " + a.detalle);
        case ST::Motivo::TrustNoSeAbre: return "no se pudo abrir trust-store.json";
        case ST::Motivo::TrustNoValido:
            return "trust-store.json no es válido" + (a.detalle.empty() ? std::string() : ": " + a.detalle);
        default: return "error al leer la configuración";
    }
}

std::string dirConfigPorOmision() {
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
    if (base && *base) {
        return std::string(base) + "/ZFSMgr";
    }
#endif
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::string(home) + "/.config/ZFSMgr";
    }
    return ".config/ZFSMgr";
}

struct Opciones {
    int passwordFd{-1};
    std::string dirConfig;
    bool sinSecretos{false};
    std::vector<std::string> orden;
};

// Descifra si hace falta. Devuelve el valor tal cual si no está cifrado, y una marca
// visible si no se puede abrir: **nunca** el texto cifrado, que es lo que la interfaz
// dejaba y podía acabar enviándose como si fuera una contraseña.
std::string abrir(const std::string& valor, const std::string& maestra, bool sinSecretos) {
    if (!B::SecretCipher::isEncrypted(valor)) {
        return valor;
    }
    if (sinSecretos || maestra.empty()) {
        return "<cifrado>";
    }
    std::string claro;
    std::string err;
    if (!B::SecretCipher::decryptEncv1(valor, maestra, claro, err)) {
        return "<no se pudo descifrar>";
    }
    return claro;
}

int listarConexiones(const Opciones& op, const std::string& maestra) {
    ST::Aviso aviso;
    const auto root = ST::leerConfig(op.dirConfig, aviso);
    if (!aviso.vacio()) {
        std::fprintf(stderr, "%s: %s\n", kNombre, textoDe(aviso).c_str());
        return 1;
    }
    // El material TLS NO vive en config.json sino en trust-store.json, que es un fichero
    // aparte precisamente para separarlo de las contraseñas de acceso. Leer solo el
    // primero hacía que una conexión CON TLS apareciera como si no lo tuviera.
    ST::Aviso avisoTrust;
    const auto trust = ST::leerTrustStore(op.dirConfig, avisoTrust);
    if (!avisoTrust.vacio()) {
        std::fprintf(stderr, "%s: aviso: %s\n", kNombre, textoDe(avisoTrust).c_str());
    }
    std::map<std::string, bool> tlsPorId;
    for (const auto& v : trust["connections"].toArray()) {
        const auto t = CJ::connectionFromJson(v, std::string());
        if (!t.id.empty()) {
            tlsPorId[B::toLowerAscii(t.id)] = CJ::profileHasDaemonTls(t);
        }
    }

    const auto& conns = root["connections"].toArray();
    if (conns.empty()) {
        std::fprintf(stderr, "%s: no hay conexiones configuradas en %s\n", kNombre,
                     ST::rutaConfig(op.dirConfig).c_str());
        return 0;
    }
    // Columnas separadas por tabulador: legible a ojo y troceable con cut/awk sin
    // adivinar anchos. Es lo que espera quien mete esto en un guion.
    std::printf("ID\tNOMBRE\tTIPO\tSO\tDESTINO\tSUDO\tTLS\n");
    for (const auto& v : conns) {
        const auto p = CJ::connectionFromJson(v, std::string());
        const bool local = CJ::isLocalProfile(p);
        const std::string destino =
            local ? std::string("(local)")
                  : abrir(p.username, maestra, op.sinSecretos) + "@" + p.host + ":"
                        + std::to_string(CJ::ensurePort(p.connType, p.port));
        std::printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
                    p.id.c_str(),
                    p.name.c_str(),
                    p.connType.empty() ? "SSH" : p.connType.c_str(),
                    p.osType.empty() ? "-" : p.osType.c_str(),
                    destino.c_str(),
                    p.useSudo ? "sí" : "no",
                    (CJ::profileHasDaemonTls(p)
                     || tlsPorId.count(B::toLowerAscii(p.id)) > 0
                            && tlsPorId.at(B::toLowerAscii(p.id)))
                        ? "sí"
                        : "no");
    }
    return 0;
}

// ¿Hay algún campo cifrado? Si no lo hay, no tiene sentido pedir la contraseña maestra, y
// preguntarla sin necesidad es la clase de fricción que hace que la gente la ponga en un
// alias de shell.
bool hayAlgoCifrado(const std::string& dirConfig) {
    ST::Aviso aviso;
    const auto root = ST::leerConfig(dirConfig, aviso);
    if (!aviso.vacio()) {
        return false;
    }
    for (const auto& v : root["connections"].toArray()) {
        for (const char* campo : {"username", "password", "daemon_tls_server_cert_pem",
                                  "daemon_tls_client_cert_pem", "daemon_tls_client_key_pem"}) {
            if (B::SecretCipher::isEncrypted(v[campo].toString())) {
                return true;
            }
        }
    }
    return false;
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
        if (a == "--password-fd" && i + 1 < argc) {
            op.passwordFd = std::atoi(argv[++i]);
            continue;
        }
        if (a == "--config-dir" && i + 1 < argc) {
            op.dirConfig = argv[++i];
            continue;
        }
        if (a == "--no-secrets") {
            op.sinSecretos = true;
            continue;
        }
        if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "%s: opción desconocida: %s\n", kNombre, a.c_str());
            uso();
            return 2;
        }
        op.orden.push_back(a);
    }
    if (op.orden.empty()) {
        uso();
        return 2;
    }

    if (op.orden[0] == "version") {
#ifdef ZFSMGR_APP_VERSION
        std::printf("%s %s\n", kNombre, ZFSMGR_APP_VERSION);
#else
        std::printf("%s (sin versión)\n", kNombre);
#endif
        return 0;
    }

    if (op.orden.size() >= 3 && op.orden[0] == "url" && op.orden[1] == "parse") {
        zfsmgr::base::ZfsmUrl u;
        std::string err;
        if (!zfsmgr::base::parseZfsmUrl(op.orden[2], u, err)) {
            std::fprintf(stderr, "%s: URL no válida: %s\n", kNombre, err.c_str());
            return 2;
        }
        // Los nombres de campo son parte de la interfaz pública: un guion que haga
        // `grep dataset` depende de ellos, así que van en inglés como la URL. El resto
        // de la salida del CLI sigue en el idioma de la aplicación.
        const char* kind = "?";
        switch (u.kind) {
            case zfsmgr::base::ZfsmKind::Connection: kind = "connection"; break;
            case zfsmgr::base::ZfsmKind::Dataset: kind = "dataset"; break;
            case zfsmgr::base::ZfsmKind::Snapshot: kind = "snapshot"; break;
            default: break;
        }
        std::printf("kind\t%s\n", kind);
        std::printf("connection\t%s\n", u.connection.c_str());
        if (!u.pool.empty()) std::printf("pool\t%s\n", u.pool.c_str());
        // Un pool ES un dataset en ZFS, así que no hay clase aparte: se dice si además
        // es la raíz de su pool.
        if (u.isPoolRoot()) std::printf("pool_root\ttrue\n");
        if (!u.dataset.empty()) std::printf("dataset\t%s\n", u.dataset.c_str());
        if (!u.snapshot.empty()) std::printf("snapshot\t%s\n", u.snapshot.c_str());
        if (!u.zfsName().empty()) std::printf("zfs_name\t%s\n", u.zfsName().c_str());
        if (!u.section.empty()) std::printf("section\t%s\n", u.section.c_str());
        for (const std::string& d : u.detail) std::printf("detail\t%s\n", d.c_str());
        // La forma canónica: es la que hay que guardar o comparar, no la tecleada.
        std::printf("canonical\t%s\n", zfsmgr::base::formatZfsmUrl(u).c_str());
        return 0;
    }

    if (op.orden.size() < 2 || op.orden[0] != "connections" || op.orden[1] != "list") {
        std::fprintf(stderr, "%s: orden desconocida\n", kNombre);
        uso();
        return 2;
    }

    std::string maestra;
    if (!op.sinSecretos && hayAlgoCifrado(op.dirConfig)) {
        std::string err;
        if (op.passwordFd >= 0) {
            if (!zfsmgr::cli::leerSecretoDeDescriptor(op.passwordFd, maestra, err)) {
                std::fprintf(stderr, "%s: %s\n", kNombre, err.c_str());
                return 1;
            }
        } else if (!zfsmgr::cli::preguntarSecretoPorTerminal("Contraseña maestra: ", maestra, err)) {
            std::fprintf(stderr, "%s: %s\n", kNombre, err.c_str());
            return 1;
        }
    }

    const int rc = listarConexiones(op, maestra);
    // El secreto no se queda en memoria más de lo necesario.
    if (!maestra.empty()) {
        volatile char* p = &maestra[0];
        for (std::size_t i = 0; i < maestra.size(); ++i) {
            p[i] = '\0';
        }
    }
    return rc;
}
