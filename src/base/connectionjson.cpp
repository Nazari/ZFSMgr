#include "connectionjson.h"

#include "strutil.h"

#include <regex>

namespace zfsmgr::base::connjson {
namespace {

// Comparación sin distinguir caja para las etiquetas ASCII de este fichero: «LOCAL»,
// «SSH», «PSRP». Todas lo son, así que la variante ASCII es la correcta aquí.
bool igualNoCase(const std::string& a, const std::string& b) {
    return toLowerAscii(a) == toLowerAscii(b);
}

int aHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string leeCadena(const json::Value& obj, const std::string& clave,
                      const std::string& porOmision = {}) {
    const json::Value& v = obj[clave];
    return v.isString() ? v.toString() : porOmision;
}

}  // namespace

int ensurePort(const std::string& connType, int port) {
    (void)connType;
    return port > 0 ? port : 22;
}

bool isLocalProfile(const ConnectionProfile& p) {
    return igualNoCase(trim(p.id), "local") || igualNoCase(trim(p.connType), "LOCAL");
}

bool shouldForceLocalSudo(const ConnectionProfile& p) {
    if (!isLocalProfile(p)) {
        return false;
    }
    return !contains(toLowerAscii(trim(p.osType)), "windows");
}

bool profileHasDaemonTls(const ConnectionProfile& p) {
    return !trim(p.daemonTlsServerCertPem).empty() || !trim(p.daemonTlsClientCertPem).empty()
        || !trim(p.daemonTlsClientKeyPem).empty();
}

bool migratePsrpProfileToSsh(ConnectionProfile& p) {
    if (!igualNoCase(trim(p.connType), "PSRP")) {
        return false;
    }
    p.connType = "SSH";
    p.osType = "Windows";
    p.useSudo = false;
    if (p.port == 5986 || p.port <= 0) {
        p.port = 22;
    }
    return true;
}

std::string decodeHexAsciiIfUuid(const std::string& raw) {
    // Se descartan los caracteres no hexadecimales y, si quedan impares, se antepone un
    // '0': es lo que hace QByteArray::fromHex, y hay identificadores guardados que
    // dependen de ello.
    std::string digitos;
    digitos.reserve(raw.size());
    for (const char c : trim(raw)) {
        if (aHex(c) >= 0) {
            digitos.push_back(c);
        }
    }
    if (digitos.empty()) {
        return std::string();
    }
    if ((digitos.size() % 2) != 0) {
        digitos.insert(digitos.begin(), '0');
    }
    std::string bytes;
    bytes.reserve(digitos.size() / 2);
    for (std::size_t i = 0; i + 1 < digitos.size(); i += 2) {
        bytes.push_back(static_cast<char>((aHex(digitos[i]) << 4) | aHex(digitos[i + 1])));
    }
    const std::string decoded = trim(bytes);
    static const std::regex uuidRx(
        "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    return std::regex_search(decoded, uuidRx) ? decoded : std::string();
}

std::string normalizeMachineUidForStorage(const ConnectionProfile& p,
                                          std::string raw,
                                          const std::string& uidLocal) {
    raw = trim(raw);
    if (!shouldForceLocalSudo(p)) {
        return raw;
    }
    if (raw.empty()) {
        return uidLocal;
    }
    const std::string decoded = decodeHexAsciiIfUuid(raw);
    if (!decoded.empty()) {
        return decoded;
    }
    return raw;
}

namespace {

// Lo común a los dos ficheros. Cambia solo lo que cada uno añade: la contraseña en
// config.json y el material TLS en el almacén de confianza.
json::Value baseComun(const ConnectionProfile& inProfile, const std::string& uidLocal,
                      ConnectionProfile& pOut) {
    ConnectionProfile p = inProfile;
    p.machineUid = normalizeMachineUidForStorage(p, p.machineUid, uidLocal);
    if (shouldForceLocalSudo(p)) {
        p.useSudo = true;
    }
    const std::string sshFamily = toLowerAscii(trim(p.sshAddressFamily));
    json::Value obj;
    obj.set("id", json::Value(trim(p.id)));
    obj.set("name", json::Value(trim(p.name)));
    obj.set("machine_uid", json::Value(p.machineUid));
    obj.set("conn_type", json::Value(trim(p.connType).empty() ? std::string("SSH") : trim(p.connType)));
    obj.set("os_type", json::Value(trim(p.osType).empty() ? std::string("Linux") : trim(p.osType)));
    obj.set("host", json::Value(trim(p.host)));
    obj.set("port", json::Value(ensurePort(p.connType, p.port)));
    obj.set("ssh_address_family",
            json::Value((sshFamily == "ipv4" || sshFamily == "ipv6") ? sshFamily
                                                                    : std::string("auto")));
    obj.set("username", json::Value(p.username));
    obj.set("key_path", json::Value(trim(p.keyPath)));
    obj.set("use_sudo", json::Value(p.useSudo));
    pOut = p;
    return obj;
}

}  // namespace

json::Value connectionToJson(const ConnectionProfile& inProfile, const std::string& uidLocal) {
    ConnectionProfile p;
    json::Value obj = baseComun(inProfile, uidLocal, p);
    obj.set("password", json::Value(p.password));
    return obj;
}

json::Value connectionTrustToJson(const ConnectionProfile& inProfile, const std::string& uidLocal) {
    ConnectionProfile p;
    json::Value obj = baseComun(inProfile, uidLocal, p);
    // El almacén de confianza NO lleva contraseña: separar el material TLS del secreto de
    // acceso es la razón de que exista como fichero aparte.
    obj.remove("password");
    obj.set("daemon_tls_server_cert_pem", json::Value(p.daemonTlsServerCertPem));
    obj.set("daemon_tls_client_cert_pem", json::Value(p.daemonTlsClientCertPem));
    obj.set("daemon_tls_client_key_pem", json::Value(p.daemonTlsClientKeyPem));
    obj.set("daemon_tls_port", json::Value(p.daemonTlsPort > 0 ? p.daemonTlsPort : 47653));
    return obj;
}

ConnectionProfile connectionFromJson(const json::Value& obj, const std::string& uidLocal) {
    ConnectionProfile p;
    p.id = trim(leeCadena(obj, "id"));
    p.name = leeCadena(obj, "name");
    p.machineUid = leeCadena(obj, "machine_uid");
    p.connType = leeCadena(obj, "conn_type");
    p.osType = leeCadena(obj, "os_type");
    p.host = leeCadena(obj, "host");
    p.port = static_cast<int>(obj["port"].isNumber() ? obj["port"].toInt() : 22);
    p.sshAddressFamily = toLowerAscii(trim(leeCadena(obj, "ssh_address_family", "auto")));
    p.username = leeCadena(obj, "username");
    p.password = leeCadena(obj, "password");
    p.keyPath = leeCadena(obj, "key_path");
    p.useSudo = obj["use_sudo"].toBool(false);
    p.daemonTlsServerCertPem = leeCadena(obj, "daemon_tls_server_cert_pem");
    p.daemonTlsClientCertPem = leeCadena(obj, "daemon_tls_client_cert_pem");
    p.daemonTlsClientKeyPem = leeCadena(obj, "daemon_tls_client_key_pem");
    p.daemonTlsPort =
        static_cast<int>(obj["daemon_tls_port"].isNumber() ? obj["daemon_tls_port"].toInt() : 47653);
    if (p.daemonTlsPort <= 0 || p.daemonTlsPort > 65535) {
        p.daemonTlsPort = 47653;
    }
    p.machineUid = normalizeMachineUidForStorage(p, p.machineUid, uidLocal);
    if (shouldForceLocalSudo(p)) {
        p.useSudo = true;
    }
    return p;
}

long long indexOfConnectionById(const json::Array& connections, const std::string& id) {
    const std::string target = trim(id);
    if (target.empty()) {
        return -1;
    }
    for (std::size_t i = 0; i < connections.size(); ++i) {
        if (igualNoCase(trim(leeCadena(connections[i], "id")), target)) {
            return static_cast<long long>(i);
        }
    }
    return -1;
}

bool upsertConnectionJson(json::Array& connections,
                          const ConnectionProfile& p,
                          const std::string& uidLocal) {
    if (trim(p.id).empty()) {
        return false;
    }
    const long long idx = indexOfConnectionById(connections, p.id);
    json::Value obj = connectionToJson(p, uidLocal);
    if (idx >= 0) {
        connections[static_cast<std::size_t>(idx)] = std::move(obj);
    } else {
        connections.push_back(std::move(obj));
    }
    return true;
}

}  // namespace zfsmgr::base::connjson
