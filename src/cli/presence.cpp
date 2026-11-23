#include "presence.h"

#include "requests.h"
#include "session.h"
#include "sessionlock.h"

#include <chrono>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace zfsmgr::cli {
namespace {

namespace PET = zfsmgr::commands::requests;

std::mutex g_config;
std::string g_kind;
long long g_pid = 0;
long long g_since = 0;

// Long enough for a machine that answers, short enough that ending the shell never waits
// much for one that does not.
constexpr int kAnnounceTimeoutMs = 8000;
constexpr int kLeaveTimeoutMs = 3000;
constexpr auto kEvery = std::chrono::seconds(30);

}  // namespace

void setPresence(const std::string& kind, long long pid) {
    std::lock_guard<std::mutex> lock(g_config);
    g_kind = kind;
    g_pid = pid;
    g_since = static_cast<long long>(std::time(nullptr));
}

bool presenceOn() {
    std::lock_guard<std::mutex> lock(g_config);
    return !g_kind.empty();
}

const std::string& presenceId() {
    static const std::string id = [] {
        std::random_device rd;
        static const char* hex = "0123456789abcdef";
        std::string s;
        for (int i = 0; i < 16; ++i) {
            s.push_back(hex[rd() & 15u]);
        }
        return s;
    }();
    return id;
}

struct Presencia::Estado {
    std::mutex m;
    std::condition_variable cv;
    bool parar{false};
    std::vector<std::thread> hilos;
};

std::string Presencia::huella(const Conexiones& conns) {
    std::string h;
    for (const auto& p : conns.perfiles) {
        const std::string id = p.id.empty() ? p.name : p.id;
        h += id + '\x1f' + p.host + '\x1f' + std::to_string(p.port) + '\x1f' + p.username + '\x1f'
             + p.connType + '\x1f' + (conns.desconectada(id) ? "d" : "c")
             + (conns.secretoSinAbrir(id) ? "s" : "o") + '\x1e';
    }
    return h;
}

Presencia::Presencia(const Sesion& ses, const Conexiones& conns)
    : m_estado(std::make_shared<Estado>()) {
    std::string kind;
    long long pid = 0;
    long long since = 0;
    {
        std::lock_guard<std::mutex> lock(g_config);
        kind = g_kind;
        pid = g_pid;
        since = g_since;
    }
    const auto pedido = PET::sessionAnnounce(presenceId(), kind, zfsmgr::base::session::hostName(),
                                             zfsmgr::base::session::currentUser(), pid, since);
    for (const auto& p : conns.perfiles) {
        const std::string id = p.id.empty() ? p.name : p.id;
        if (conns.desconectada(id) || conns.secretoSinAbrir(id)) {
            continue;
        }
        m_estado->hilos.emplace_back([estado = m_estado, perfil = p, pedido,
                                      dirConfig = ses.dirConfig, maestra = ses.maestra] {
            auto propia = crearSesionDeFondo(dirConfig, maestra);
            std::string out;
            std::string err;
            int rc = 0;
            for (;;) {
                ejecutarAgente(*propia, perfil, pedido, out, err, rc, nullptr, kAnnounceTimeoutMs);
                std::unique_lock<std::mutex> lock(estado->m);
                if (estado->cv.wait_for(lock, kEvery, [&] { return estado->parar; })) {
                    break;
                }
            }
            ejecutarAgente(*propia, perfil, PET::sessionLeave(presenceId()), out, err, rc, nullptr,
                           kLeaveTimeoutMs);
        });
    }
}

Presencia::~Presencia() {
    {
        std::lock_guard<std::mutex> lock(m_estado->m);
        m_estado->parar = true;
    }
    m_estado->cv.notify_all();
    for (auto& h : m_estado->hilos) {
        if (h.joinable()) {
            h.join();
        }
    }
}

std::unique_ptr<Presencia> anunciaPresencia(const Sesion& ses, const Conexiones& conns) {
    if (!presenceOn() || ses.sinSecretos) {
        return nullptr;
    }
    return std::make_unique<Presencia>(ses, conns);
}

}  // namespace zfsmgr::cli
