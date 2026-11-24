#include "network_sync.h"

#include "agentmsg.h"
#include "json.h"
#include "network.h"
#include "osinfo.h"
#include "session.h"
#include "sessionlock.h"
#include "storefiles.h"
#include "strutil.h"
#include "tlsclient.h"
#include "tr.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace zfsmgr::cli {

namespace {

namespace B = zfsmgr::base;
namespace J = zfsmgr::base::json;
namespace NW = zfsmgr::base::network;
namespace ST = zfsmgr::base::store;
namespace fs = std::filesystem;

constexpr int kRpcTimeoutMs = 10000;
constexpr int kPutAttempts = 5;

std::string leeFichero(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool escribeSoloDueno(const fs::path& p, const std::string& texto) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        f << texto;
    }
    fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    fs::rename(tmp, p, ec);
    return !ec;
}

bool leeDoc(const std::string& dir, NW::Document& d) {
    J::Value v;
    std::string err;
    return J::parse(leeFichero(fs::path(dir) / "network.json"), v, &err) && NW::fromJson(v, d, err);
}

bool guardaDoc(const std::string& dir, const NW::Document& d) {
    return escribeSoloDueno(fs::path(dir) / "network.json", J::toIndented(NW::toJson(d)));
}

NW::LocalSettings leeLocal(const std::string& dir) {
    J::Value v;
    J::parse(leeFichero(fs::path(dir) / "local.json"), v, nullptr);
    return NW::localFromJson(v);
}

void guardaLocal(const std::string& dir, const NW::LocalSettings& l) {
    escribeSoloDueno(fs::path(dir) / "local.json", J::toIndented(NW::localToJson(l)));
}

// The document as config.json + trust-store.json, keeping what those files hold that is not
// the network's (the interface's state, the disconnected connections…). `plantilla`: where to
// take that from when the directory is new.
bool materializa(const std::string& dir, const NW::Document& d, const NW::LocalSettings& local,
                 const J::Value* plantilla = nullptr) {
    J::Value cfg;
    J::Value tr;
    NW::toClientConfig(d, local, B::osinfo::machineUuid(), cfg, tr);
    ST::Warning aviso;
    J::Value raiz = plantilla ? *plantilla : ST::readConfig(dir, aviso);
    if (!raiz.isObject()) {
        raiz = J::Value(J::Object{});
    }
    raiz.set("connections", cfg["connections"]);
    raiz.set("master", cfg["master"]);
    if (cfg.contains("master_check")) {
        raiz.set("master_check", cfg["master_check"]);
    }
    ST::Warning a1;
    ST::Warning a2;
    J::Value confianza;
    confianza.set("schema", J::Value(1));
    confianza.set("connections", tr["connections"]);
    return ST::writeConfig(dir, raiz, a1) && ST::writeTrustStore(dir, confianza, a2);
}

struct Respuesta {
    long long revision{-1};
    std::string mac;
    NW::Document doc;  // only when revision > 0 and it decoded
    bool conDoc{false};
};

Respuesta leeGet(const std::string& out) {
    Respuesta r;
    std::string doc;
    std::istringstream in(out);
    for (std::string l; std::getline(in, l);) {
        l = B::trim(l);
        if (l.rfind("REVISION=", 0) == 0) {
            r.revision = std::atoll(l.c_str() + 9);
        } else if (l.rfind("MAC=", 0) == 0) {
            r.mac = l.substr(4);
        } else if (l.rfind("DOC=", 0) == 0) {
            doc = l.substr(4);
        }
    }
    std::string texto;
    J::Value v;
    std::string err;
    if (r.revision > 0 && B::base64Decode(doc, texto) && J::parse(texto, v, &err)
        && NW::fromJson(v, r.doc, err)) {
        r.conDoc = true;
    }
    return r;
}

// The machines whose daemon can be asked: connected, with TLS material (or this computer's).
std::vector<const B::ConnectionProfile*> alcanzables(const Conexiones& conns) {
    std::vector<const B::ConnectionProfile*> v;
    for (const auto& p : conns.perfiles) {
        const std::string id = p.id.empty() ? p.name : p.id;
        const bool local = B::toUpperAscii(p.connType) == "LOCAL";
        if (!conns.desconectada(id) && (local || conns.tieneTls(id))) {
            v.push_back(&p);
        }
    }
    return v;
}

std::string nombre(const B::ConnectionProfile& p) {
    return p.id.empty() ? p.name : p.id;
}

}  // namespace

std::string dirDeRed(const std::string& dirConfig) {
    return (fs::path(dirConfig) / "network").string();
}

bool esDirDeRed(const std::string& dir) {
    std::error_code ec;
    return fs::exists(fs::path(dir) / "network.json", ec);
}

bool tieneRed(const std::string& dirConfig) {
    return esDirDeRed(dirDeRed(dirConfig));
}

bool eligeDirecciones(const std::string& dir) {
    NW::Document d;
    if (!leeDoc(dir, d)) {
        return false;
    }
    NW::LocalSettings local = leeLocal(dir);
    const std::string yo = B::osinfo::machineUuid();
    // Each machine with more than one address, in parallel: the first, in order, that answers
    // on its SSH port. A machine that answers nowhere keeps what it had.
    std::vector<std::string> elegida(d.machines.size());
    std::vector<std::thread> hilos;
    for (std::size_t i = 0; i < d.machines.size(); ++i) {
        const NW::Machine& m = d.machines[i];
        if (m.uid == yo || m.addresses.size() < 2) {
            continue;
        }
        hilos.emplace_back([&elegida, &m, i] {
            for (const std::string& a : m.addresses) {
                if (B::tcpReachable(a, static_cast<unsigned short>(m.sshPort), 1500)) {
                    elegida[i] = a;
                    return;
                }
            }
        });
    }
    for (auto& h : hilos) {
        h.join();
    }
    bool cambia = false;
    for (std::size_t i = 0; i < d.machines.size(); ++i) {
        const NW::Machine& m = d.machines[i];
        if (elegida[i].empty()) {
            continue;
        }
        const auto p = local.preferredAddress.find(m.uid);
        const std::string ahora =
            p != local.preferredAddress.end() ? p->second : m.addresses.front();
        if (ahora == elegida[i]) {
            continue;
        }
        if (elegida[i] == m.addresses.front()) {
            local.preferredAddress.erase(m.uid);
        } else {
            local.preferredAddress[m.uid] = elegida[i];
        }
        cambia = true;
    }
    if (cambia) {
        guardaLocal(dir, local);
        materializa(dir, d, local);
    }
    return cambia;
}

bool sincronizaRed(Sesion& ses, const Conexiones& conns, std::string& aviso) {
    NW::Document base;
    if (!leeDoc(ses.dirConfig, base)) {
        return false;
    }
    for (const B::ConnectionProfile* p : alcanzables(conns)) {
        std::string out;
        std::string err;
        int rc = -1;
        if (!ejecutarAgente(ses, *p, {"--net-get"}, out, err, rc, nullptr, kRpcTimeoutMs) || rc != 0) {
            continue;
        }
        const Respuesta r = leeGet(out);
        if (r.revision <= 0 || !r.conDoc) {
            continue;
        }
        switch (NW::judge(r.doc, ses.maestra, base.revision, base.mac)) {
            case NW::Verdict::Newer: {
                guardaDoc(ses.dirConfig, r.doc);
                materializa(ses.dirConfig, r.doc, leeLocal(ses.dirConfig));
                return true;
            }
            case NW::Verdict::Same:
            case NW::Verdict::Older:
            case NW::Verdict::Diverged:
                return false;
            case NW::Verdict::BadMac:
                aviso = B::format(T("t_red_no_verifica",
                                    "el daemon de %1 sirve una configuración de red que no está "
                                    "firmada con la clave de esta red: se ignora"),
                                  {nombre(*p)});
                continue;
        }
    }
    return false;
}

bool publicaRed(Sesion& ses, const Conexiones& conns, std::string& aviso) {
    const std::string dir = ses.dirConfig;
    NW::Document base;
    if (!leeDoc(dir, base)) {
        return false;
    }
    NW::LocalSettings local = leeLocal(dir);
    ST::Warning a1;
    ST::Warning a2;
    const J::Value cfg = ST::readConfig(dir, a1);
    const J::Value tr = ST::readTrustStore(dir, a2);
    std::vector<std::string> sinUid;
    const NW::Document mia = NW::fromMaterialized(base, cfg, tr, local, sinUid);
    guardaLocal(dir, local);
    if (!sinUid.empty()) {
        aviso = B::format(T("t_red_sin_uid",
                            "%1 todavía no está en la red: hace falta que su daemon diga quién es "
                            "(instálalo, o «refresh»)"),
                          {B::join(sinUid, ", ")});
    }
    if (NW::sameContent(mia, base)) {
        return false;
    }
    for (int intento = 0; intento < kPutAttempts; ++intento) {
        bool llegado = false;
        for (const B::ConnectionProfile* p : alcanzables(conns)) {
            std::string out;
            std::string err;
            int rc = -1;
            if (!ejecutarAgente(ses, *p, {"--net-get"}, out, err, rc, nullptr, kRpcTimeoutMs)
                || rc != 0) {
                continue;
            }
            const Respuesta r = leeGet(out);
            NW::Document suya = base;
            long long suRev = 0;
            if (r.revision > 0) {
                if (!r.conDoc || NW::judge(r.doc, ses.maestra, 0, "") == NW::Verdict::BadMac) {
                    continue;
                }
                suya = r.doc;
                suRev = r.revision;
            }
            llegado = true;
            NW::Document nueva = NW::threeWay(base, mia, suya);
            nueva.revision = std::max(suRev, base.revision) + 1;
            nueva.writtenBy = B::osinfo::machineUuid();
            nueva.writtenAt = static_cast<long long>(std::time(nullptr));
            NW::sign(nueva, ses.maestra);
            const std::string b64 = B::base64Encode(J::toCompact(NW::toJson(nueva)));
            if (ejecutarAgente(ses, *p, {"--net-put", std::to_string(suRev), b64}, out, err, rc,
                               nullptr, kRpcTimeoutMs)
                && rc == 0) {
                guardaDoc(dir, nueva);
                materializa(dir, nueva, local);
                return true;
            }
            if (B::agentmsg::lastCode() == "net_conflict") {
                break;  // someone wrote meanwhile: fetch again and merge on top
            }
            aviso = B::format(T("t_red_no_publica", "no se pudo publicar en la red (%1): %2"),
                              {nombre(*p), B::trim(err)});
            return false;
        }
        if (!llegado) {
            aviso = T("t_red_sin_daemon",
                      "no se llega al daemon de ninguna máquina: el cambio queda en este "
                      "ordenador y se publicará en cuanto se llegue a uno");
            return false;
        }
    }
    aviso = T("t_red_conflictos", "la red cambió varias veces seguidas mientras se publicaba: "
                                  "inténtalo de nuevo");
    return false;
}

int sembrarRed(const std::string& dirConfig, const std::string& dataKey) {
    if (tieneRed(dirConfig)) {
        std::fprintf(stderr, "%s\n",
                     T("t_red_ya", "este ordenador ya está en una red").c_str());
        return 1;
    }
    auto ses = crearSesionDeFondo(dirConfig, dataKey);
    const Conexiones conns = cargarConexiones(dirConfig, dataKey);
    ST::Warning a1;
    ST::Warning a2;
    const J::Value cfg = ST::readConfig(dirConfig, a1);
    const J::Value tr = ST::readTrustStore(dirConfig, a2);
    NW::Document d;
    NW::LocalSettings local;
    std::string err;
    const std::string uid = B::osinfo::machineUuid();
    if (!NW::fromClientConfig(cfg, tr, B::session::hostName(), uid,
                              static_cast<long long>(std::time(nullptr)), d, local, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    NW::sign(d, dataKey);
    const std::string b64 = B::base64Encode(J::toCompact(NW::toJson(d)));
    // None may have a network yet: seeding over one would be two networks.
    std::vector<const B::ConnectionProfile*> destinos;
    for (const B::ConnectionProfile* p : alcanzables(conns)) {
        std::string out;
        std::string e;
        int rc = -1;
        if (!ejecutarAgente(*ses, *p, {"--net-get"}, out, e, rc, nullptr, kRpcTimeoutMs) || rc != 0) {
            std::fprintf(stderr, "%s\n",
                         B::format(T("t_red_no_llega", "%1: no responde, se pondrá al día solo"),
                                   {nombre(*p)})
                             .c_str());
            continue;
        }
        if (leeGet(out).revision > 0) {
            std::fprintf(stderr, "%s\n",
                         B::format(T("t_red_existe",
                                     "%1 ya tiene una red: «network join» para unirse a ella"),
                                   {nombre(*p)})
                             .c_str());
            return 1;
        }
        destinos.push_back(p);
    }
    if (destinos.empty()) {
        std::fprintf(stderr, "%s\n",
                     T("t_red_ninguno", "no se llega al daemon de ninguna máquina").c_str());
        return 1;
    }
    int puestas = 0;
    for (const B::ConnectionProfile* p : destinos) {
        std::string out;
        std::string e;
        int rc = -1;
        // The first one passes it on; the rest may already have it (net_conflict): fine.
        if (ejecutarAgente(*ses, *p, {"--net-put", "0", b64}, out, e, rc, nullptr, kRpcTimeoutMs)
            && rc == 0) {
            ++puestas;
        }
    }
    const std::string dir = dirDeRed(dirConfig);
    guardaLocal(dir, local);
    guardaDoc(dir, d);
    if (!materializa(dir, d, local, &cfg)) {
        std::fprintf(stderr, "%s\n", T("t_red_no_escribe", "no se pudo escribir la red en este ordenador").c_str());
        return 1;
    }
    std::printf("%s\n", B::format(T("t_red_sembrada",
                                    "red creada: %1 máquinas, revisión %2, en %3 daemons (los demás "
                                    "la reciben de ellos)"),
                                  {std::to_string(d.machines.size()), std::to_string(d.revision),
                                   std::to_string(puestas)})
                            .c_str());
    return 0;
}

int unirseARed(const std::string& dirConfig, const std::string& dataKey, const std::string& password) {
    if (tieneRed(dirConfig)) {
        std::fprintf(stderr, "%s\n", T("t_red_ya", "este ordenador ya está en una red").c_str());
        return 1;
    }
    auto ses = crearSesionDeFondo(dirConfig, dataKey);
    const Conexiones conns = cargarConexiones(dirConfig, dataKey);
    Respuesta mejor;
    std::string de;
    for (const B::ConnectionProfile* p : alcanzables(conns)) {
        std::string out;
        std::string e;
        int rc = -1;
        if (!ejecutarAgente(*ses, *p, {"--net-get"}, out, e, rc, nullptr, kRpcTimeoutMs) || rc != 0) {
            continue;
        }
        Respuesta r = leeGet(out);
        if (r.conDoc && r.revision > mejor.revision) {
            mejor = r;
            de = nombre(*p);
        }
    }
    if (!mejor.conDoc) {
        std::fprintf(stderr, "%s\n",
                     T("t_red_no_hay", "ningún daemon al que se llega tiene una red: «network "
                                       "seed» en el ordenador que la tenga")
                         .c_str());
        return 1;
    }
    // Opened with the network's master password, on the network's own slots.
    const std::string dir = dirDeRed(dirConfig);
    NW::LocalSettings local;
    for (const auto& p : conns.perfiles) {
        if (!p.machineUid.empty() && !p.keyPath.empty()) {
            local.keyPaths[p.machineUid] = p.keyPath;
        }
    }
    ST::Warning a;
    const J::Value plantilla = ST::readConfig(dirConfig, a);
    if (!materializa(dir, mejor.doc, local, &plantilla)) {
        std::fprintf(stderr, "%s\n", T("t_red_no_escribe", "no se pudo escribir la red en este ordenador").c_str());
        return 1;
    }
    std::string claveRed;
    ST::Warning aviso;
    if (!ST::unlockWithPassword(dir, password, claveRed, aviso) || !NW::verify(mejor.doc, claveRed)) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        std::fprintf(stderr, "%s\n",
                     T("t_red_no_abre", "esa no es la contraseña maestra de la red (o la red no "
                                        "está firmada con ella): no se ha unido")
                         .c_str());
        return 1;
    }
    guardaLocal(dir, local);
    guardaDoc(dir, mejor.doc);
    std::printf("%s\n", B::format(T("t_red_unido", "unido a la red: %1 máquinas, revisión %2 (de %3)"),
                                  {std::to_string(mejor.doc.machines.size()),
                                   std::to_string(mejor.revision), de})
                            .c_str());
    return 0;
}

}  // namespace zfsmgr::cli
