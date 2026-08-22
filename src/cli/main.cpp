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
#include "i18n.h"
#include "session.h"

#include "transporttunnel.h"
#include "shell.h"
#include "tabla.h"
#include "json.h"
#include "secretcipher.h"
#include "secretinput.h"
#include "storefiles.h"
#include "storewarnings.h"
#include "strutil.h"
#include "tr.h"
#include "zfsmurl.h"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {

namespace B = zfsmgr::base;
using zfsmgr::cli::Formato;
using zfsmgr::cli::Tabla;
using zfsmgr::cli::Tipo;
namespace ST = zfsmgr::base::store;
namespace CJ = zfsmgr::base::connjson;

constexpr const char* kNombre = "zfsmgr-cli";

void uso() {
    std::fprintf(stderr,
                 TC("t_uso_s_opci_010447", "Uso: %s [opciones] <orden>\n"
                 "\n"
                 "Sin ninguna orden entra en MODO INTERACTIVO: un intérprete donde la\n"
                 "posición es una URL zfsm:// y todas las órdenes actúan sobre ella.\n"
                 "\n"
                 "Órdenes:\n"
                 "  (ninguna)             Modo interactivo, empezando en la raíz (zfsm://)\n"
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
                 "  --format text|tsv|json\n"
                 "                        text (por omisión) es para leer; tsv es para\n"
                 "                        guiones: sin encabezado, tabuladores, columnas\n"
                 "                        fijas en inglés y «-» donde no hay valor, igual\n"
                 "                        que `zfs list -H`; json es para programas: los\n"
                 "                        números salen como números, los booleanos como\n"
                 "                        booleanos y lo que no aplica como null.\n"
                 "                        connections list: id, name, type, os, user,\n"
                 "                        host, port, sudo, tls, connected\n"
                 "  --url <zfsm://…>      Dónde empezar en el modo interactivo\n"
                 "  --lang es|en|zh       Idioma de los mensajes. Sin él, el que use la\n"
                 "                        interfaz gráfica (app.language de config.json).\n"
                 "  -v, --verbose         Cuenta por la salida de error lo que hace el\n"
                 "                        transporte con cada máquina\n"
                 "  -y, --yes             No preguntar antes de las acciones destructivas\n"
                 "  -h, --help            Esta ayuda\n"
                 "\n"
                 "La contraseña maestra NO se pasa por argumento ni por variable de\n"
                 "entorno: las dos cosas quedan visibles en `ps` para cualquier usuario\n"
                 "de la máquina. Con el descriptor se puede usar cualquier gestor de\n"
                 "secretos:\n"
                 "\n"
                 "  %s --password-fd 3 connections list  3< <(pass show zfsmgr)\n"),
                 kNombre, kNombre);
}


// El directorio de configuración, que tiene que ser EL MISMO que usa la interfaz.
//
// `<home>/.config/ZFSMgr` en las cuatro plataformas, Windows incluido — que es lo que hace
// `ConnectionStore::configDir()`. Aquí se prefería `%APPDATA%\ZFSMgr` en Windows, que es
// lo idiomático allí pero NO es donde está el fichero: el CLI no veía ninguna conexión y
// arrancaba en la raíz aunque la interfaz tuviera media docena configuradas.
//
// El orden para averiguar el «home» imita al de `QDir::homePath()`, para que las dos
// mitades del programa no puedan discrepar: HOME, luego USERPROFILE, luego
// HOMEDRIVE+HOMEPATH.
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
    const char* unidad = std::getenv("HOMEDRIVE");
    const char* ruta = std::getenv("HOMEPATH");
    if (unidad && *unidad && ruta && *ruta) {
        return std::string(unidad) + ruta + "/.config/ZFSMgr";
    }
#endif
    return ".config/ZFSMgr";
}

struct Opciones {
    int passwordFd{-1};
    std::string dirConfig;
    bool sinSecretos{false};
    Formato formato{Formato::Texto};
    std::string urlInicial;
    std::string idioma;
    bool verboso{false};
    bool asumirSi{false};
    std::vector<std::string> orden;
};


// La lista de conexiones. La tabla se construye en `session.cpp`, en el mismo sitio que la
// del `ls` de la raíz del intérprete: eran dos, con columnas distintas, y la misma pregunta
// se contestaba de dos maneras según por dónde se preguntara.
//
// De paso desaparece de aquí la lectura de config.json y del almacén de confianza, que
// también estaba duplicada — y con ella el fallo de mirar solo el primero, que hacía que
// una conexión CON TLS apareciera como si no lo tuviera.
int listarConexiones(const Opciones& op, const std::string& maestra) {
    const auto conns = zfsmgr::cli::cargarConexiones(op.dirConfig, maestra);
    if (!conns.aviso.empty()) {
        std::fprintf(stderr, "%s: %s\n", kNombre, conns.aviso.c_str());
        return 1;
    }
    if (conns.perfiles.empty()) {
        std::fprintf(stderr, TC("t_s_no_hay_c_8978da", "%s: no hay conexiones configuradas en %s\n"), kNombre, ST::rutaConfig(op.dirConfig).c_str());
    }
    zfsmgr::cli::tablaDeConexiones(conns).imprime(op.formato);
    return 0;
}


// Las dos preguntas sobre la maestra —si hace falta y si abre— las contesta la capa base,
// que es donde las usa también la interfaz. Aquí solo se traduce el motivo.
//
// Antes esto miraba UN campo cifrado, el primero, con el argumento de que con Fernet basta
// para saber si la clave es la buena. Es cierto, pero deja pasar una configuración a MEDIO
// ROTAR —unos campos con la clave nueva y otros con la vieja—, que es exactamente lo que
// puede dejar una rotación interrumpida. Ahora se recorren todos.
bool hayAlgoCifrado(const std::string& dirConfig) {
    return ST::hayAlgoCifrado(dirConfig);
}

bool maestraAbre(const std::string& dirConfig, const std::string& maestra) {
    ST::Aviso aviso;
    return ST::maestraAbreTodo(dirConfig, maestra, aviso);
}

}  // namespace

int main(int argc, char** argv) {
    // La consola de Windows interpreta lo que le llega con la página de códigos OEM (850
    // en un Windows en español), y todo lo que escribe este programa es UTF-8: sin esto,
    // «— «help»» salía como «ÔÇö ┬½help┬½». Se ajustan las dos direcciones, porque por la
    // entrada llegan nombres de dataset con acentos.
    //
    // Si la salida está redirigida no hay consola y las llamadas fallan sin efecto, que es
    // lo correcto: a un fichero van los bytes UTF-8 tal cual.
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    Opciones op;
    op.dirConfig = dirConfigPorOmision();
    // Los catálogos de traducción, donde estén: junto al ejecutable y en el árbol de
    // compilación. Se busca antes de leer nada para que hasta el mensaje de una opción
    // desconocida salga en el idioma que toque.
    zfsmgr::base::i18n::addSearchPath(zfsmgr::cli::dirDelEjecutable() + "/i18n");
    zfsmgr::base::i18n::addSearchPath(zfsmgr::cli::dirDelEjecutable() + "/../i18n");
    zfsmgr::base::i18n::addSearchPath(zfsmgr::cli::dirDelEjecutable() + "/../share/zfsmgr/i18n");
    zfsmgr::base::i18n::addSearchPath(zfsmgr::cli::dirDelEjecutable() + "/../Resources/i18n");
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
        if (a == "--lang" && i + 1 < argc) {
            op.idioma = argv[++i];
            continue;
        }
        if (a == "--url" && i + 1 < argc) {
            op.urlInicial = argv[++i];
            continue;
        }
        if (a == "-v" || a == "--verbose") {
            op.verboso = true;
            continue;
        }
        if (a == "-y" || a == "--yes") {
            op.asumirSi = true;
            continue;
        }
        if (a == "--format" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v == "tsv") {
                op.formato = Formato::Tsv;
            } else if (v == "json") {
                op.formato = Formato::Json;
            } else if (v == "text") {
                op.formato = Formato::Texto;
            } else {
                std::fprintf(stderr, TC("t_s_formato__3b41bf", "%s: formato desconocido: %s (use text, tsv o json)\n"), kNombre, v.c_str());
                return 2;
            }
            continue;
        }
        if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, TC("t_opcion_desconocida", "%s: opción desconocida: %s\n"), kNombre, a.c_str());
            uso();
            return 2;
        }
        op.orden.push_back(a);
    }

    // El idioma: lo que diga --lang y, si no, el de la interfaz gráfica. Va DESPUÉS de
    // leer los argumentos porque --config-dir puede cambiar dónde está esa preferencia.
    {
        std::string idioma = op.idioma;
        if (idioma.empty()) {
            ST::Aviso aviso;
            const auto root = ST::leerConfig(op.dirConfig, aviso);
            idioma = root["app"]["language"].toString();
            if (idioma.empty()) {
                idioma = root["ui"]["language"].toString();
            }
        }
        if (!idioma.empty()) {
            zfsmgr::base::i18n::setLanguage(idioma);
        }
    }

    // Sin ninguna orden se entra en el intérprete, así que a partir de aquí NO se puede
    // indexar `op.orden` sin comprobar: quitar la guarda que había y dejar op.orden[0]
    // fue exactamente lo que hizo que el binario se cayera al arrancar sin argumentos.
    if (!op.orden.empty() && op.orden[0] == "version") {
#ifdef ZFSMGR_APP_VERSION
        std::printf("%s %s\n", kNombre, ZFSMGR_APP_VERSION);
#else
        std::printf(TC("t_sin_version", "%s (sin versión)\n"), kNombre);
#endif
        return 0;
    }

    if (op.orden.size() >= 3 && op.orden[0] == "url" && op.orden[1] == "parse") {
        zfsmgr::base::ZfsmUrl u;
        std::string err;
        if (!zfsmgr::base::parseZfsmUrl(op.orden[2], u, err)) {
            std::fprintf(stderr, TC("t_s_url_no_v_46bbde", "%s: URL no válida: %s\n"), kNombre, err.c_str());
            return 2;
        }
        // Los nombres de campo son parte de la interfaz pública: un guion que haga
        // `grep dataset` depende de ellos, así que van en inglés como la URL. El resto
        // de la salida del CLI sigue en el idioma de la aplicación.
        //
        // Aquí `text` y `tsv` son LA MISMA salida, y a propósito: siendo campo/valor, no
        // hay columnas que alinear ni encabezado que quitar, así que distinguirlos sería
        // dar a elegir entre dos cosas iguales.
        //
        // `json` sí es distinto, y por dos motivos concretos: `detail` es una LISTA —en
        // campo/valor sale como varias líneas con la misma clave, que es justo lo que un
        // guion no sabe leer— y `pool_root` es un booleano que aquí solo aparece cuando es
        // cierto, de modo que su ausencia significa «false» y hay que saberlo de antemano.
        const char* kind = "?";
        switch (u.kind) {
            case zfsmgr::base::ZfsmKind::Connection: kind = "connection"; break;
            case zfsmgr::base::ZfsmKind::Dataset: kind = "dataset"; break;
            case zfsmgr::base::ZfsmKind::Snapshot: kind = "snapshot"; break;
            default: break;
        }
        if (op.formato == Formato::Json) {
            // En json las claves están TODAS, con `null` donde no hay valor: así
            // `.snapshot` siempre se puede leer y no hay que comprobar si la clave existe.
            const auto oNulo = [](const std::string& v) {
                return v.empty() ? B::json::Value() : B::json::Value(v);
            };
            B::json::Value o;
            o.set("kind", B::json::Value(kind));
            o.set("connection", oNulo(u.connection));
            o.set("pool", oNulo(u.pool));
            o.set("pool_root", B::json::Value(u.isPoolRoot()));
            o.set("dataset", oNulo(u.dataset));
            o.set("snapshot", oNulo(u.snapshot));
            o.set("zfs_name", oNulo(u.zfsName()));
            o.set("section", oNulo(u.section));
            B::json::Value detalle;
            for (const std::string& d : u.detail) {
                detalle.push(B::json::Value(d));
            }
            // Vacío tiene que salir como `[]` y no como `null`: `detail` es una lista
            // siempre, y así se puede recorrer sin comprobar nada.
            if (u.detail.empty()) {
                detalle = B::json::Value(B::json::Array{});
            }
            o.set("detail", std::move(detalle));
            o.set("canonical", B::json::Value(zfsmgr::base::formatZfsmUrl(u)));
            std::printf("%s", B::json::toIndented(o).c_str());
            return 0;
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

    if (!op.orden.empty()
        && (op.orden.size() < 2 || op.orden[0] != "connections" || op.orden[1] != "list")) {
        std::fprintf(stderr, TC("t_s_orden_de_81e64c", "%s: orden desconocida\n"), kNombre);
        uso();
        return 2;
    }

    std::string maestra;
    // Si se da un descriptor, se lee SIEMPRE, aunque todavía no haya nada cifrado. Antes
    // solo se leía cuando ya lo había, y eso es un pez que se muerde la cola: para dar de
    // alta la PRIMERA conexión con contraseña hace falta una maestra, y no se pedía porque
    // aún no había nada que descifrar.
    if (!op.sinSecretos && (op.passwordFd >= 0 || hayAlgoCifrado(op.dirConfig))) {
        std::string err;
        if (op.passwordFd >= 0) {
            if (!zfsmgr::cli::leerSecretoDeDescriptor(op.passwordFd, maestra, err)) {
                std::fprintf(stderr, "%s: %s\n", kNombre, err.c_str());
                return 1;
            }
            // Por descriptor no se puede volver a preguntar —ya se leyó—, así que se
            // termina con error en vez de seguir con secretos que no abren. Un guion se
            // entera por el código de salida, que es como se entera un guion de todo.
            if (!maestraAbre(op.dirConfig, maestra)) {
                std::fputs(TC("t_maestra_no_abre", "contraseña maestra incorrecta: no abre los "
                             "secretos guardados\n"), stderr);
                return 1;
            }
        } else {
            // Con terminal se vuelve a preguntar, como hace la interfaz gráfica. Tres
            // intentos: los suficientes para un dedazo, no tantos como para que esto sirva
            // de banco de pruebas a nadie.
            bool abierta = false;
            for (int intento = 0; intento < 3 && !abierta; ++intento) {
                if (!zfsmgr::cli::preguntarSecretoPorTerminal(T("t_p_maestra", "Contraseña maestra: "),
                                                              maestra, err)) {
                    std::fprintf(stderr, "%s: %s\n", kNombre, err.c_str());
                    return 1;
                }
                abierta = maestraAbre(op.dirConfig, maestra);
                if (!abierta) {
                    std::fputs(TC("t_maestra_no_abre", "contraseña maestra incorrecta: no abre los "
                                 "secretos guardados\n"), stderr);
                }
            }
            if (!abierta) {
                return 1;
            }
        }
    }

    int rc = 0;
    if (op.orden.empty()) {
        // Sin orden, el intérprete. Es la puerta por la que el CLI pasa de leer la
        // configuración a hablar con las máquinas.
        auto ses = zfsmgr::cli::crearSesion(op.dirConfig, maestra, op.verboso, op.sinSecretos);
        rc = zfsmgr::cli::ejecutarShell(*ses, op.formato, op.urlInicial, op.asumirSi);
        zfsmgr::base::transport::closeAllTunnels(ses->transporte);
    } else {
        rc = listarConexiones(op, maestra);
    }
    // El secreto no se queda en memoria más de lo necesario.
    //
    // Y los túneles tampoco se quedan vivos: un `ssh -L` lleva `setsid()` —para que el
    // Ctrl-C del terminal no se lo lleve por delante— y eso hace que sobreviva al
    // intérprete que lo montó. Cada ejecución dejaba uno atrás, reteniendo un puerto.
    if (!maestra.empty()) {
        volatile char* p = &maestra[0];
        for (std::size_t i = 0; i < maestra.size(); ++i) {
            p[i] = '\0';
        }
    }
    return rc;
}
