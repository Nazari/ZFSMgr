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
#include "json.h"
#include "secretcipher.h"
#include "secretinput.h"
#include "storefiles.h"
#include "storewarnings.h"
#include "strutil.h"
#include "zfsmurl.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
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
                 "  --format text|tsv|json\n"
                 "                        text (por omisión) es para leer; tsv es para\n"
                 "                        guiones: sin encabezado, tabuladores, columnas\n"
                 "                        fijas en inglés y «-» donde no hay valor, igual\n"
                 "                        que `zfs list -H`; json es para programas: los\n"
                 "                        números salen como números, los booleanos como\n"
                 "                        booleanos y lo que no aplica como null.\n"
                 "                        connections list: id, name, type, os, user,\n"
                 "                        host, port, sudo, tls\n"
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

// Cómo se saca lo que se lista.
//
// `Texto` es para leer: columnas alineadas y encabezados en el idioma de la aplicación.
// `Tsv` es para guiones: **sin encabezado**, separado por tabuladores, columnas fijas y
// en inglés. Es la misma convención que `zfs list -H`, que quien use esta herramienta ya
// conoce, y por eso no lleva encabezado: una línea de más que hay que saltarse.
//
// Los campos vacíos salen como «-», también como en `zfs`: así el número de columnas no
// cambia y `cut -f4` sigue apuntando a lo mismo.
//
// `Json` es para programas, y aporta lo ÚNICO que tsv no puede dar: **tipos**. Un puerto
// sale como número y no como «22», `sudo` como booleano y no como «true», y lo que no
// aplica sale como `null` en vez de «-». Si se emitiera todo como cadenas, JSON no
// añadiría nada sobre tsv salvo comillas y una dependencia más de quien lo lea.
enum class Formato { Texto, Tsv, Json };

// De qué tipo es una columna. Solo lo usa JSON: texto y tsv lo sacan todo como texto,
// que es lo que son.
enum class Tipo { Cadena, Booleano, Entero };

struct Opciones {
    int passwordFd{-1};
    std::string dirConfig;
    bool sinSecretos{false};
    Formato formato{Formato::Texto};
    std::vector<std::string> orden;
};

// Una tabla que se sabe imprimir de las tres maneras. Existe para que añadir una orden de
// listado no obligue a escribir tres veces la misma salida y que se separen con el tiempo.
//
// **Las celdas se guardan SIEMPRE en forma canónica**, no en la de salida: un booleano se
// guarda como «true», y es el impresor de texto quien lo traduce a «sí». Antes lo decidía
// quien rellenaba la fila mirando el formato, lo que obligaba a cada orden de listado a
// conocer los tres y a acertar en todos.
struct Tabla {
    std::string nombreJson;                   // la clave de la colección: "connections"
    std::vector<std::string> cabecerasTexto;  // en el idioma de la aplicación
    std::vector<std::string> campos;          // estables, en inglés; los usan tsv y json
    std::vector<Tipo> tipos;                  // por columna; solo lo mira json
    std::vector<std::vector<std::string>> filas;

    Tipo tipoDe(std::size_t i) const { return i < tipos.size() ? tipos[i] : Tipo::Cadena; }

    // Una celda tal y como la ve una PERSONA. Los booleanos van en el idioma de la
    // aplicación; en tsv y en json no, porque «sí» depende del idioma y un guion no
    // debería tener que saberlo.
    std::string celdaTexto(std::size_t col, const std::string& v) const {
        if (tipoDe(col) == Tipo::Booleano) {
            return v == "true" ? "sí" : (v == "false" ? "no" : v);
        }
        return v;
    }

    void imprimeJson() const {
        // Array VACÍO, no un Value nulo: sin filas se quedaría en `null`, y
        // `jq '.connections[]'` sobre null no da cero elementos, da un error. Es una lista
        // siempre, tenga cero o mil.
        B::json::Value filasJson{B::json::Array{}};
        for (const auto& fila : filas) {
            B::json::Value o;
            for (std::size_t i = 0; i < fila.size() && i < campos.size(); ++i) {
                // Vacío es `null`, NO «-»: el guion es una convención de columnas, que en
                // JSON no hace falta. Un programa que leyera «-» en `port` tendría que
                // saberlo, y es justo lo que este formato viene a evitar.
                if (fila[i].empty()) {
                    o.set(campos[i], B::json::Value());
                    continue;
                }
                switch (tipoDe(i)) {
                    case Tipo::Booleano:
                        o.set(campos[i], B::json::Value(fila[i] == "true"));
                        break;
                    case Tipo::Entero:
                        o.set(campos[i], B::json::Value(std::atoll(fila[i].c_str())));
                        break;
                    case Tipo::Cadena:
                        o.set(campos[i], B::json::Value(fila[i]));
                        break;
                }
            }
            filasJson.push(std::move(o));
        }
        // Un objeto en la raíz y no un array suelto: deja sitio para añadir después algo
        // al lado —avisos, la versión— sin que a quien ya lo lea se le rompa el guion.
        B::json::Value raiz;
        raiz.set(nombreJson.empty() ? std::string("items") : nombreJson, std::move(filasJson));
        std::printf("%s", B::json::toIndented(raiz).c_str());
    }

    void imprime(Formato f) const {
        if (f == Formato::Json) {
            imprimeJson();
            return;
        }
        if (f == Formato::Tsv) {
            for (const auto& fila : filas) {
                for (std::size_t i = 0; i < fila.size(); ++i) {
                    std::printf("%s%s", i ? "\t" : "", fila[i].empty() ? "-" : fila[i].c_str());
                }
                std::printf("\n");
            }
            return;
        }
        // Anchos por columna, contando CARACTERES y no bytes: «sí» ocupa tres bytes y
        // dos columnas, y con printf("%-*s") las últimas columnas salían desplazadas.
        const auto anchoVisible = [](const std::string& s) {
            std::size_t n = 0;
            for (const char c : s) {
                if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
                    ++n;  // los bytes de continuación de UTF-8 no ocupan columna
                }
            }
            return n;
        };
        std::vector<std::size_t> ancho(cabecerasTexto.size(), 0);
        for (std::size_t i = 0; i < cabecerasTexto.size(); ++i) {
            ancho[i] = anchoVisible(cabecerasTexto[i]);
        }
        for (const auto& fila : filas) {
            for (std::size_t i = 0; i < fila.size() && i < ancho.size(); ++i) {
                const std::string c = celdaTexto(i, fila[i]);
                ancho[i] = std::max(ancho[i], anchoVisible(c.empty() ? "-" : c));
            }
        }
        auto linea = [&](const std::vector<std::string>& celdas, bool esCabecera) {
            for (std::size_t i = 0; i < celdas.size(); ++i) {
                const bool ultima = (i + 1 == celdas.size());
                const std::string bruta = esCabecera ? celdas[i] : celdaTexto(i, celdas[i]);
                const std::string celda = bruta.empty() ? std::string("-") : bruta;
                std::printf("%s", celda.c_str());
                if (!ultima) {
                    std::printf("%*s", static_cast<int>(ancho[i] - anchoVisible(celda) + 2), "");
                }
            }
            std::printf("\n");
        };
        linea(cabecerasTexto, true);
        for (const auto& fila : filas) {
            linea(fila, false);
        }
    }
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
    // Sin conexiones NO se sale antes de tiempo: en json hay que sacar igualmente
    // `{"connections": []}`, o un guion que haga `jq \'.connections | length\'` recibe una
    // entrada vacía y falla en vez de leer cero.
    if (conns.empty()) {
        std::fprintf(stderr, "%s: no hay conexiones configuradas en %s\n", kNombre,
                     ST::rutaConfig(op.dirConfig).c_str());
    }
    Tabla t;
    t.nombreJson = "connections";
    t.cabecerasTexto = {"ID", "NOMBRE", "TIPO", "SO", "USUARIO", "HOST", "PUERTO", "SUDO", "TLS"};
    t.campos = {"id", "name", "type", "os", "user", "host", "port", "sudo", "tls"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena,
               Tipo::Cadena, Tipo::Entero, Tipo::Booleano, Tipo::Booleano};
    for (const auto& v : conns) {
        const auto p = CJ::connectionFromJson(v, std::string());
        const bool local = CJ::isLocalProfile(p);
        const bool tls = CJ::profileHasDaemonTls(p)
                      || (tlsPorId.count(B::toLowerAscii(p.id)) > 0
                          && tlsPorId.at(B::toLowerAscii(p.id)));
        // Forma canónica, no la de salida: es la Tabla quien sabe cómo se enseña cada
        // tipo en cada formato. Aquí no hay que saber en cuál estamos.
        const auto boole = [](bool b) -> std::string { return b ? "true" : "false"; };
        t.filas.push_back({
            p.id,
            p.name,
            p.connType.empty() ? std::string("SSH") : p.connType,
            p.osType,
            local ? std::string() : abrir(p.username, maestra, op.sinSecretos),
            local ? std::string() : p.host,
            local ? std::string() : std::to_string(CJ::ensurePort(p.connType, p.port)),
            boole(p.useSudo),
            boole(tls),
        });
    }
    t.imprime(op.formato);
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
        if (a == "--format" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v == "tsv") {
                op.formato = Formato::Tsv;
            } else if (v == "json") {
                op.formato = Formato::Json;
            } else if (v == "text") {
                op.formato = Formato::Texto;
            } else {
                std::fprintf(stderr, "%s: formato desconocido: %s (use text, tsv o json)\n",
                             kNombre, v.c_str());
                return 2;
            }
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
