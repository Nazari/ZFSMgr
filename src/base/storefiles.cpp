#include "storefiles.h"

#include "secretcipher.h"
#include "strutil.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#endif

namespace zfsmgr::base::store {
namespace {

namespace fs = std::filesystem;

void soloElDueno(const std::string& ruta) {
#ifdef _WIN32
    // En Windows los permisos POSIX no significan lo mismo; el fichero queda bajo el
    // perfil del usuario, que ya es privado. std::filesystem no puede expresar una ACL.
    (void)ruta;
#else
    (void)::chmod(ruta.c_str(), S_IRUSR | S_IWUSR);
#endif
}

json::Value leerFichero(const std::string& ruta,
                        Reason motivoAbrir,
                        Reason motivoInvalido,
                        Warning& aviso) {
    aviso = Warning{};
    std::error_code ec;
    if (!fs::exists(ruta, ec) || ec) {
        // Primer arranque: no hay nada que leer y no hay nada que avisar.
        return json::Value(json::Object{});
    }
    std::ifstream f(ruta, std::ios::binary);
    if (!f) {
        aviso.reason = motivoAbrir;
        return json::Value(json::Object{});
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!f && !f.eof()) {
        aviso.reason = motivoAbrir;
        return json::Value(json::Object{});
    }
    json::Value v;
    std::string err;
    if (!json::parse(ss.str(), v, &err) || !v.isObject()) {
        aviso.reason = motivoInvalido;
        aviso.detail = err;
        return json::Value(json::Object{});
    }
    return v;
}

bool asegurarDirectorio(const std::string& dirConfig, Warning& aviso) {
    std::error_code ec;
    if (!fs::exists(dirConfig, ec) && !fs::create_directories(dirConfig, ec)) {
        // create_directories devuelve false también cuando el directorio ya existía por
        // una carrera; solo es fallo si de verdad no está.
        if (!fs::exists(dirConfig, ec)) {
            aviso.reason = Reason::ConfigDirCannotBeCreated;
            return false;
        }
    }
    return true;
}

// Se escribe ENTERO en un temporal del mismo directorio y se renombra encima.
//
// Antes se truncaba el fichero y se volcaba en el sitio, y con varios procesos a la vez —dos
// `zfsmgr-cli`, o uno y la interfaz— el que leía justo entonces encontraba el fichero vacío o
// a medias y contestaba «config.json no es válido». El renombrado es atómico: quien lee ve el
// fichero viejo o el nuevo, nunca uno a medio escribir. Y un corte a mitad de escritura deja
// el viejo intacto, en vez de un fichero cortado con todas las conexiones dentro.
bool escribirFichero(const std::string& dirConfig,
                     const std::string& ruta,
                     const json::Value& root,
                     Reason motivoEscribir,
                     Warning& aviso) {
    aviso = Warning{};
    if (!asegurarDirectorio(dirConfig, aviso)) {
        return false;
    }
    const ConfigLock cerrojo(dirConfig);
    const std::string texto = json::toIndented(root);
#ifndef _WIN32
    const std::string temporal = ruta + ".tmp-" + std::to_string(::getpid());
    // Con el permiso de solo el dueño DESDE que se crea: los secretos cifrados no pasan ni un
    // instante por un fichero con los permisos que dejara la umask.
    const int fd = ::open(temporal.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                          S_IRUSR | S_IWUSR);
    if (fd < 0) {
        aviso.reason = motivoEscribir;
        aviso.detail = std::strerror(errno);
        return false;
    }
    soloElDueno(temporal);   // por si ya existía con otros permisos
    std::size_t hecho = 0;
    bool bien = true;
    while (hecho < texto.size()) {
        const ssize_t n = ::write(fd, texto.data() + hecho, texto.size() - hecho);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            bien = false;
            break;
        }
        hecho += static_cast<std::size_t>(n);
    }
    // A disco antes de renombrar: si no, un corte de luz justo después podría dejar con el
    // nombre bueno un fichero vacío.
    bien = bien && ::fsync(fd) == 0;
    const int errEscritura = errno;
    ::close(fd);
    if (!bien || ::rename(temporal.c_str(), ruta.c_str()) != 0) {
        const int e = bien ? errno : errEscritura;
        ::unlink(temporal.c_str());
        aviso.reason = motivoEscribir;
        aviso.detail = std::strerror(e);
        return false;
    }
#else
    const std::string temporal = ruta + ".tmp-" + std::to_string(::_getpid());
    {
        std::ofstream f(fs::path(temporal), std::ios::binary | std::ios::trunc);
        if (!f) {
            aviso.reason = motivoEscribir;
            return false;
        }
        f.write(texto.data(), static_cast<std::streamsize>(texto.size()));
        f.flush();
        if (!f) {
            f.close();
            std::error_code ec;
            fs::remove(fs::path(temporal), ec);
            aviso.reason = motivoEscribir;
            return false;
        }
    }
    // MoveFileEx y no std::filesystem::rename: hay que REEMPLAZAR el que existe, y eso es lo
    // que garantiza MOVEFILE_REPLACE_EXISTING.
    if (!MoveFileExW(fs::path(temporal).wstring().c_str(), fs::path(ruta).wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        aviso.detail = "error " + std::to_string(GetLastError());
        std::error_code ec;
        fs::remove(fs::path(temporal), ec);
        aviso.reason = motivoEscribir;
        return false;
    }
#endif
    return true;
}

}  // namespace

// --- El cerrojo entre procesos.
//
// Uno por proceso y por todo el proceso: `flock` es por descripción de fichero abierta, así
// que dos aperturas del mismo fichero en el MISMO proceso se bloquearían entre sí. Por eso el
// contador: quien ya lo tiene —saveProfile, que dentro escribe los dos ficheros— no lo vuelve
// a pedir al sistema. El mutex recursivo ordena además a los hilos del propio proceso.
namespace {
std::recursive_mutex g_cerrojoMutex;
int g_cerrojoProfundidad = 0;
#ifndef _WIN32
int g_cerrojoFd = -1;
#else
HANDLE g_cerrojoFd = INVALID_HANDLE_VALUE;
#endif
constexpr auto kEsperaCerrojo = std::chrono::seconds(15);
}  // namespace

ConfigLock::ConfigLock(const std::string& dirConfig) {
    g_cerrojoMutex.lock();
    if (g_cerrojoProfundidad++ > 0) {
        m_tenido = true;
        return;
    }
    Warning sinUso;
    if (!asegurarDirectorio(dirConfig, sinUso)) {
        return;
    }
    const std::string ruta = dirConfig + "/.config.lock";
    // Si en el plazo no se consigue, se sigue SIN él, como antes de que existiera: otro
    // proceso tan lento es un proceso colgado, y negarse a guardar por eso sería peor. `flock`
    // se suelta solo cuando el dueño muere, así que no hay cerrojos huérfanos que esperar.
    const auto limite = std::chrono::steady_clock::now() + kEsperaCerrojo;
#ifndef _WIN32
    g_cerrojoFd = ::open(ruta.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (g_cerrojoFd < 0) {
        return;
    }
    while (::flock(g_cerrojoFd, LOCK_EX | LOCK_NB) != 0) {
        if (errno != EWOULDBLOCK && errno != EINTR) {
            return;
        }
        if (std::chrono::steady_clock::now() >= limite) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#else
    g_cerrojoFd = CreateFileW(fs::path(ruta).wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (g_cerrojoFd == INVALID_HANDLE_VALUE) {
        return;
    }
    for (;;) {
        OVERLAPPED ov{};
        if (LockFileEx(g_cerrojoFd, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                       &ov)) {
            break;
        }
        if (std::chrono::steady_clock::now() >= limite) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#endif
    m_tenido = true;
}

ConfigLock::~ConfigLock() {
    if (--g_cerrojoProfundidad == 0) {
#ifndef _WIN32
        if (g_cerrojoFd >= 0) {
            ::flock(g_cerrojoFd, LOCK_UN);
            ::close(g_cerrojoFd);
            g_cerrojoFd = -1;
        }
#else
        if (g_cerrojoFd != INVALID_HANDLE_VALUE) {
            OVERLAPPED ov{};
            UnlockFileEx(g_cerrojoFd, 0, 1, 0, &ov);
            CloseHandle(g_cerrojoFd);
            g_cerrojoFd = INVALID_HANDLE_VALUE;
        }
#endif
    }
    g_cerrojoMutex.unlock();
}

std::string configPath(const std::string& dirConfig) {
    return dirConfig + "/config.json";
}

std::string trustStorePath(const std::string& dirConfig) {
    return dirConfig + "/trust-store.json";
}

json::Value readConfig(const std::string& dirConfig, Warning& aviso) {
    return leerFichero(configPath(dirConfig), Reason::ConfigCannotBeOpened, Reason::ConfigNotValid, aviso);
}

json::Value readTrustStore(const std::string& dirConfig, Warning& aviso) {
    return leerFichero(trustStorePath(dirConfig), Reason::TrustCannotBeOpened, Reason::TrustNotValid, aviso);
}

bool writeConfig(const std::string& dirConfig, const json::Value& root, Warning& aviso) {
    return escribirFichero(dirConfig, configPath(dirConfig), root, Reason::ConfigCannotBeWritten, aviso);
}

bool writeTrustStore(const std::string& dirConfig, const json::Value& root, Warning& aviso) {
    return escribirFichero(dirConfig, trustStorePath(dirConfig), root, Reason::TrustCannotBeWritten, aviso);
}

namespace {

// Los campos que van cifrados con la maestra, tal y como se llaman en el JSON. Están aquí
// y no repartidos porque la rotación tiene que tocarlos TODOS: olvidar uno deja la
// configuración medio cifrada con la clave vieja, que es la forma de romperlo sin que se
// note hasta el arranque siguiente.
const char* const kCamposSecretos[] = {
    "password",
    "daemon_tls_server_cert_pem",
    "daemon_tls_client_cert_pem",
    "daemon_tls_client_key_pem",
};

std::string nombreDe(const json::Value& conexion) {
    const std::string n = conexion["name"].isString() ? conexion["name"].toString() : std::string();
    if (!n.empty()) {
        return n;
    }
    return conexion["id"].isString() ? conexion["id"].toString() : std::string("(sin nombre)");
}

// Un campo: se abre con la vieja —si estaba cifrado— y se cierra con la nueva. Un campo en
// claro se CIFRA, que es lo que hace que una configuración a medias quede entera después.
bool rotaCampo(json::Value& conexion, const char* campo, const std::string& vieja,
               const std::string& nueva, Warning& aviso) {
    if (!conexion[campo].isString()) {
        return true;
    }
    const std::string valor = conexion[campo].toString();
    if (valor.empty()) {
        return true;
    }
    std::string claro = valor;
    if (SecretCipher::isEncrypted(valor)) {
        std::string err;
        if (!SecretCipher::decryptEncv1(valor, vieja, claro, err)) {
            aviso = Warning{Reason::CannotDecrypt, nombreDe(conexion), campo, err};
            return false;
        }
    }
    std::string cifrado;
    std::string err;
    if (!SecretCipher::encryptEncv1(claro, nueva, cifrado, err)) {
        aviso = Warning{Reason::CannotEncrypt, nombreDe(conexion), campo, err};
        return false;
    }
    conexion.set(campo, json::Value(cifrado));
    return true;
}

bool rotaConexiones(json::Value& raiz, const std::string& vieja, const std::string& nueva,
                    Warning& aviso) {
    if (!raiz["connections"].isArray()) {
        return true;
    }
    json::Array salida;
    for (const json::Value& original : raiz["connections"].toArray()) {
        json::Value conexion = original;
        for (const char* campo : kCamposSecretos) {
            if (!rotaCampo(conexion, campo, vieja, nueva, aviso)) {
                return false;
            }
        }
        salida.push_back(conexion);
    }
    raiz.set("connections", json::Value(salida));
    return true;
}

}  // namespace

namespace {

// Recorre los campos secretos de los dos ficheros. `porCada` decide si se sigue.
bool recorreSecretos(const std::string& dirConfig,
                     const std::function<bool(const json::Value&, const char*, const std::string&)>& porCada,
                     Warning& aviso) {
    for (int cual = 0; cual < 2; ++cual) {
        Warning propio;
        const json::Value raiz = (cual == 0) ? readConfig(dirConfig, propio)
                                             : readTrustStore(dirConfig, propio);
        if (!propio.empty()) {
            aviso = propio;
            return false;
        }
        for (const json::Value& conexion : raiz["connections"].toArray()) {
            for (const char* campo : kCamposSecretos) {
                if (!conexion[campo].isString()) {
                    continue;
                }
                const std::string valor = conexion[campo].toString();
                if (valor.empty() || !SecretCipher::isEncrypted(valor)) {
                    continue;
                }
                if (!porCada(conexion, campo, valor)) {
                    return false;
                }
            }
        }
    }
    return true;
}

}  // namespace

namespace {

// Cifra un campo si va en claro. Sin maestra no se escribe: se dice y se para.
bool cifraSiHace(std::string& valor, const char* campo, const std::string& maestra, Warning& aviso) {
    if (valor.empty() || SecretCipher::isEncrypted(valor)) {
        return true;
    }
    if (maestra.empty()) {
        aviso = Warning{Reason::MasterPasswordRequiredToEncrypt, {}, campo, {}};
        return false;
    }
    std::string cifrado;
    std::string err;
    if (!SecretCipher::encryptEncv1(valor, maestra, cifrado, err)) {
        aviso = Warning{Reason::CannotEncrypt, {}, campo, err};
        return false;
    }
    valor = cifrado;
    return true;
}

// ¿La maestra de ESTA sesión es la del fichero?
//
// Cada proceso guarda la maestra en memoria desde que arranca. Si otro —otro `zfsmgr-cli`, o
// la interfaz— la cambia, éste sigue con la vieja, y su siguiente escritura cifraría con ella
// lo que añadiera: el fichero quedaría con campos bajo dos claves distintas, que es justo lo
// que la rotación hace lo imposible por no dejar. Así que antes de escribir se abre UN campo
// cifrado del fichero con la maestra de la sesión. Uno basta: la rotación los cambia todos.
// Cuesta una derivación, unos 40 ms, y solo al guardar.
//
// Sin maestra no hay nada que comprobar: quien escribe sin ella no cifra nada, y lo que haga
// falta cifrar ya lo rechaza cifraSiHace.
bool maestraAbreElFichero(const std::string& dirConfig, const std::string& maestra,
                          Warning& aviso) {
    if (maestra.empty()) {
        return true;
    }
    bool abre = true;
    Warning lectura;
    recorreSecretos(
        dirConfig,
        [&](const json::Value& conexion, const char* campo, const std::string& valor) {
            std::string claro;
            std::string err;
            if (!SecretCipher::decryptEncv1(valor, maestra, claro, err)) {
                abre = false;
                aviso = Warning{Reason::MasterChangedElsewhere, nombreDe(conexion), campo, {}};
            }
            return false;   // con uno basta
        },
        lectura);
    return abre;
}

// ¿Apunta el perfil AL MISMO SITIO que el que ya estaba guardado?
//
// El usuario se compara descifrado: guardado va cifrado, y compararlo tal cual daría
// «cambió» siempre, con lo que se tiraría el TLS en cada guardado.
bool mismoExtremo(const ConnectionProfile& nuevo, const ConnectionProfile& viejo,
                  const std::string& maestra) {
    std::string usuarioViejo = viejo.username;
    if (SecretCipher::isEncrypted(usuarioViejo) && !maestra.empty()) {
        std::string claro;
        std::string err;
        if (SecretCipher::decryptEncv1(usuarioViejo, maestra, claro, err)) {
            usuarioViejo = claro;
        }
    }
    return toLowerAscii(trim(viejo.host)) == toLowerAscii(trim(nuevo.host))
           && connjson::ensurePort(viejo.connType, viejo.port)
                  == connjson::ensurePort(nuevo.connType, nuevo.port)
           && toLowerAscii(trim(usuarioViejo)) == toLowerAscii(trim(nuevo.username))
           && trim(viejo.keyPath) == trim(nuevo.keyPath);
}

}  // namespace

bool saveProfile(const std::string& dirConfig, const ConnectionProfile& p,
                  const std::string& maestra, Warning& aviso) {
    aviso = Warning{};
    const ConfigLock cerrojo(dirConfig);
    if (!maestraAbreElFichero(dirConfig, maestra, aviso)) {
        return false;
    }
    if (trim(p.id).empty()) {
        aviso = Warning{Reason::EmptyId, {}, {}, {}};
        return false;
    }
    json::Value root = readConfig(dirConfig, aviso);
    if (!aviso.empty()) {
        return false;
    }
    ConnectionProfile guardado = p;
    guardado.port = connjson::ensurePort(guardado.connType, guardado.port);

    // El extremo se compara contra lo que hay en `config.json` —host, puerto, usuario y
    // clave viven ahí—, pero el MATERIAL TLS vive en el almacén de confianza, que es un
    // fichero aparte precisamente para separarlo del secreto de acceso. Mirar el material
    // en config.json no encuentra nada: `connectionToJson` ni siquiera lo escribe.
    const json::Array actuales = root["connections"].toArray();
    bool habia = false;
    bool mismoSitio = true;
    for (const json::Value& v : actuales) {
        const ConnectionProfile existente = connjson::connectionFromJson(v, std::string());
        if (toLowerAscii(trim(existente.id)) != toLowerAscii(trim(guardado.id))) {
            continue;
        }
        habia = true;
        mismoSitio = mismoExtremo(guardado, existente, maestra);
        break;
    }

    Warning avisoTrust;
    json::Value trust = readTrustStore(dirConfig, avisoTrust);
    if (!avisoTrust.empty()) {
        aviso = avisoTrust;
        return false;
    }
    ConnectionProfile enElAlmacen;
    bool estabaEnElAlmacen = false;
    for (const json::Value& v : trust["connections"].toArray()) {
        const ConnectionProfile t = connjson::connectionFromJson(v, std::string());
        if (toLowerAscii(trim(t.id)) == toLowerAscii(trim(guardado.id))) {
            enElAlmacen = t;
            estabaEnElAlmacen = true;
            break;
        }
    }
    if (habia && !mismoSitio) {
        // El extremo cambió: el certificado fijado para el sitio ANTERIOR no vale para el
        // nuevo, y arrastrarlo dejaría al cliente fiándose de una máquina que no es.
        guardado.daemonTlsServerCertPem.clear();
        guardado.daemonTlsClientCertPem.clear();
        guardado.daemonTlsClientKeyPem.clear();
        estabaEnElAlmacen = false;
    } else if (estabaEnElAlmacen) {
        if (trim(guardado.daemonTlsServerCertPem).empty()) {
            guardado.daemonTlsServerCertPem = enElAlmacen.daemonTlsServerCertPem;
        }
        if (trim(guardado.daemonTlsClientCertPem).empty()) {
            guardado.daemonTlsClientCertPem = enElAlmacen.daemonTlsClientCertPem;
        }
        if (trim(guardado.daemonTlsClientKeyPem).empty()) {
            guardado.daemonTlsClientKeyPem = enElAlmacen.daemonTlsClientKeyPem;
        }
        if (guardado.daemonTlsPort <= 0 || guardado.daemonTlsPort > 65535) {
            guardado.daemonTlsPort = (enElAlmacen.daemonTlsPort > 0 && enElAlmacen.daemonTlsPort <= 65535)
                                         ? enElAlmacen.daemonTlsPort
                                         : 47653;
        }
    }
    if (guardado.daemonTlsPort <= 0 || guardado.daemonTlsPort > 65535) {
        guardado.daemonTlsPort = 47653;
    }

    if (!cifraSiHace(guardado.password, "password", maestra, aviso)
        || !cifraSiHace(guardado.daemonTlsServerCertPem, "daemon_tls_server_cert_pem", maestra, aviso)
        || !cifraSiHace(guardado.daemonTlsClientCertPem, "daemon_tls_client_cert_pem", maestra, aviso)
        || !cifraSiHace(guardado.daemonTlsClientKeyPem, "daemon_tls_client_key_pem", maestra, aviso)) {
        aviso.connection = !p.name.empty() ? p.name : p.id;
        return false;
    }

    json::Array salida;
    bool sustituida = false;
    for (const json::Value& v : actuales) {
        const ConnectionProfile existente = connjson::connectionFromJson(v, std::string());
        if (toLowerAscii(trim(existente.id)) == toLowerAscii(trim(guardado.id))) {
            salida.push_back(connjson::connectionToJson(guardado, std::string()));
            sustituida = true;
        } else {
            salida.push_back(v);
        }
    }
    if (!sustituida) {
        salida.push_back(connjson::connectionToJson(guardado, std::string()));
    }
    root.set("connections", json::Value(salida));
    if (!writeConfig(dirConfig, root, aviso)) {
        return false;
    }

    // Y el almacén de confianza: se reescribe la entrada, o se quita si el extremo cambió.
    json::Array salidaTrust;
    for (const json::Value& v : trust["connections"].toArray()) {
        const ConnectionProfile t = connjson::connectionFromJson(v, std::string());
        if (toLowerAscii(trim(t.id)) != toLowerAscii(trim(guardado.id))) {
            salidaTrust.push_back(v);
        }
    }
    if (connjson::profileHasDaemonTls(guardado)) {
        salidaTrust.push_back(connjson::connectionTrustToJson(guardado, std::string()));
    }
    if (salidaTrust.empty() && !trust["connections"].isArray()) {
        return true;   // no había almacén y no hay nada que poner en él
    }
    trust.set("schema", json::Value(1));
    trust.set("created_by", json::Value(std::string("ZFSMgr")));
    trust.set("connections", json::Value(salidaTrust));
    return writeTrustStore(dirConfig, trust, aviso);
}

bool saveTlsToStore(const std::string& dirConfig, const ConnectionProfile& p,
                        const std::string& maestra, Warning& aviso) {
    aviso = Warning{};
    if (trim(p.id).empty() || connjson::isLocalProfile(p) || !connjson::profileHasDaemonTls(p)) {
        return true;   // nada que fijar
    }
    const ConfigLock cerrojo(dirConfig);
    if (!maestraAbreElFichero(dirConfig, maestra, aviso)) {
        return false;
    }
    ConnectionProfile guardado = p;
    if (!cifraSiHace(guardado.daemonTlsServerCertPem, "daemon_tls_server_cert_pem", maestra, aviso)
        || !cifraSiHace(guardado.daemonTlsClientCertPem, "daemon_tls_client_cert_pem", maestra, aviso)
        || !cifraSiHace(guardado.daemonTlsClientKeyPem, "daemon_tls_client_key_pem", maestra, aviso)) {
        aviso.connection = !p.name.empty() ? p.name : p.id;
        return false;
    }
    json::Value trust = readTrustStore(dirConfig, aviso);
    if (!aviso.empty()) {
        return false;
    }
    json::Array salida;
    bool sustituida = false;
    for (const json::Value& v : trust["connections"].toArray()) {
        const ConnectionProfile t = connjson::connectionFromJson(v, std::string());
        if (toLowerAscii(trim(t.id)) == toLowerAscii(trim(guardado.id))) {
            salida.push_back(connjson::connectionTrustToJson(guardado, std::string()));
            sustituida = true;
        } else {
            salida.push_back(v);
        }
    }
    if (!sustituida) {
        salida.push_back(connjson::connectionTrustToJson(guardado, std::string()));
    }
    trust.set("schema", json::Value(1));
    trust.set("created_by", json::Value(std::string("ZFSMgr")));
    trust.set("connections", json::Value(salida));
    return writeTrustStore(dirConfig, trust, aviso);
}

bool encryptWhatIsMissing(const std::string& dirConfig, const std::string& maestra, Warning& aviso) {
    aviso = Warning{};
    if (maestra.empty()) {
        aviso = Warning{Reason::MasterPasswordRequired, {}, {}, {}};
        return false;
    }
    const ConfigLock cerrojo(dirConfig);
    // La maestra se comprueba solo si de verdad hay algo que cifrar: sin nada en claro no se
    // escribe nada, y lo ya cifrado se deja como está aunque sea de otra clave.
    bool comprobada = false;
    for (int cual = 0; cual < 2; ++cual) {
        json::Value raiz = (cual == 0) ? readConfig(dirConfig, aviso) : readTrustStore(dirConfig, aviso);
        if (!aviso.empty()) {
            return false;
        }
        json::Array salida;
        bool tocado = false;
        for (const json::Value& v : raiz["connections"].toArray()) {
            json::Value conexion = v;
            for (const char* campo : kCamposSecretos) {
                if (!conexion[campo].isString()) {
                    continue;
                }
                const std::string valor = conexion[campo].toString();
                if (valor.empty() || SecretCipher::isEncrypted(valor)) {
                    continue;
                }
                if (!comprobada) {
                    if (!maestraAbreElFichero(dirConfig, maestra, aviso)) {
                        return false;
                    }
                    comprobada = true;
                }
                std::string cifrado;
                std::string err;
                if (!SecretCipher::encryptEncv1(valor, maestra, cifrado, err)) {
                    aviso = Warning{Reason::CannotEncrypt, nombreDe(conexion), campo, err};
                    return false;
                }
                conexion.set(campo, json::Value(cifrado));
                tocado = true;
            }
            salida.push_back(conexion);
        }
        if (!tocado) {
            continue;   // nada en claro: no se reescribe el fichero por gusto
        }
        raiz.set("connections", json::Value(salida));
        const bool ok = (cual == 0) ? writeConfig(dirConfig, raiz, aviso)
                                    : writeTrustStore(dirConfig, raiz, aviso);
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool deleteProfile(const std::string& dirConfig, const std::string& id, Warning& aviso) {
    aviso = Warning{};
    const ConfigLock cerrojo(dirConfig);
    const std::string buscado = toLowerAscii(trim(id));
    if (buscado.empty()) {
        aviso = Warning{Reason::EmptyId, {}, {}, {}};
        return false;
    }
    json::Value root = readConfig(dirConfig, aviso);
    if (!aviso.empty()) {
        return false;
    }
    json::Array salida;
    bool encontrada = false;
    for (const json::Value& v : root["connections"].toArray()) {
        const ConnectionProfile p = connjson::connectionFromJson(v, std::string());
        if (toLowerAscii(trim(p.id)) == buscado) {
            encontrada = true;
            continue;
        }
        salida.push_back(v);
    }
    if (!encontrada) {
        aviso = Warning{Reason::ConnectionNotSaved, id, {}, {}};
        return false;
    }
    root.set("connections", json::Value(salida));
    if (!writeConfig(dirConfig, root, aviso)) {
        return false;
    }

    // Y del almacén de confianza, o la conexión vuelve sola.
    Warning avisoTrust;
    json::Value trust = readTrustStore(dirConfig, avisoTrust);
    if (!avisoTrust.empty()) {
        aviso = avisoTrust;
        return false;
    }
    json::Array salidaTrust;
    bool habiaEnElAlmacen = false;
    for (const json::Value& v : trust["connections"].toArray()) {
        const ConnectionProfile t = connjson::connectionFromJson(v, std::string());
        if (toLowerAscii(trim(t.id)) == buscado) {
            habiaEnElAlmacen = true;
            continue;
        }
        salidaTrust.push_back(v);
    }
    if (!habiaEnElAlmacen) {
        return true;
    }
    trust.set("connections", json::Value(salidaTrust));
    return writeTrustStore(dirConfig, trust, aviso);
}

bool hasSomethingEncrypted(const std::string& dirConfig) {
    bool alguno = false;
    Warning aviso;
    recorreSecretos(dirConfig,
                    [&alguno](const json::Value&, const char*, const std::string&) {
                        alguno = true;
                        return false;   // con uno basta
                    },
                    aviso);
    return alguno;
}

bool masterOpensEverything(const std::string& dirConfig, const std::string& maestra, Warning& aviso) {
    aviso = Warning{};
    bool ok = true;
    Warning avisoLectura;
    const bool leido = recorreSecretos(
        dirConfig,
        [&](const json::Value& conexion, const char* campo, const std::string& valor) {
            if (maestra.empty()) {
                aviso = Warning{Reason::MasterPasswordRequired, nombreDe(conexion), campo, {}};
                ok = false;
                return false;
            }
            std::string claro;
            std::string err;
            if (!SecretCipher::decryptEncv1(valor, maestra, claro, err)) {
                aviso = Warning{Reason::CannotDecrypt, nombreDe(conexion), campo, err};
                ok = false;
                return false;
            }
            return true;
        },
        avisoLectura);
    if (!leido && ok) {
        aviso = avisoLectura;
        return false;
    }
    return ok;
}

bool rotateMasterKey(const std::string& dirConfig, const std::string& vieja,
                      const std::string& nueva, std::string& copiaSufijo, Warning& aviso) {
    aviso = Warning{};
    copiaSufijo.clear();
    const ConfigLock cerrojo(dirConfig);
    if (nueva.empty()) {
        aviso = Warning{Reason::NewMasterPasswordEmpty, {}, {}, {}};
        return false;
    }
    json::Value config = readConfig(dirConfig, aviso);
    if (!aviso.empty()) {
        return false;
    }
    Warning avisoTrust;
    json::Value trust = readTrustStore(dirConfig, avisoTrust);
    if (!avisoTrust.empty()) {
        aviso = avisoTrust;
        return false;
    }

    // La copia va ANTES de tocar nada, y de los dos ficheros: si la rotación se parte por
    // la mitad, lo que queda en disco no sirve ni con la clave vieja ni con la nueva.
    copiaSufijo = ".antes-de-rotar";
    std::error_code ec;
    for (const std::string& ruta : {configPath(dirConfig), trustStorePath(dirConfig)}) {
        if (!fs::exists(ruta, ec)) {
            continue;
        }
        fs::copy_file(ruta, ruta + copiaSufijo, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            aviso = Warning{Reason::ConfigCannotBeWritten, {}, ruta + copiaSufijo, ec.message()};
            copiaSufijo.clear();
            return false;
        }
    }

    if (!rotaConexiones(config, vieja, nueva, aviso)) {
        return false;
    }
    if (!rotaConexiones(trust, vieja, nueva, aviso)) {
        return false;
    }
    if (!writeConfig(dirConfig, config, aviso)) {
        return false;
    }
    if (trust["connections"].isArray() && !trust["connections"].toArray().empty()) {
        trust.set("schema", json::Value(1));
        trust.set("created_by", json::Value(std::string("ZFSMgr")));
        if (!writeTrustStore(dirConfig, trust, aviso)) {
            return false;
        }
    }
    return true;
}

}  // namespace zfsmgr::base::store
