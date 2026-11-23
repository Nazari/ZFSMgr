// `sessions`: who else has ZFSMgr open —an interface or a CLI shell on a terminal—, as the
// daemons of every connected machine say. And the same warning at the start of the shell and
// in `overview`. The daemons know it because every session announces itself to them
// (presence.h); there is one session per machine (base/sessionlock.h), so the one on this
// machine is this one.

#include "presence.h"
#include "sessionlock.h"
#include "shell_internal.h"

#include <ctime>
#include <thread>

namespace zfsmgr::cli::detail {

namespace SS = zfsmgr::commands::sessions;

namespace {

// «17:41» today, «2026-11-22 17:41» another day.
std::string hora(long long epoch) {
    if (epoch <= 0) {
        return "?";
    }
    const std::time_t t = static_cast<std::time_t>(epoch);
    const std::time_t ahora = std::time(nullptr);
    std::tm cuando{};
    std::tm hoy{};
#ifdef _WIN32
    localtime_s(&cuando, &t);
    localtime_s(&hoy, &ahora);
#else
    localtime_r(&t, &cuando);
    localtime_r(&ahora, &hoy);
#endif
    char buf[32];
    const bool mismoDia = cuando.tm_year == hoy.tm_year && cuando.tm_yday == hoy.tm_yday;
    std::strftime(buf, sizeof buf, mismoDia ? "%H:%M" : "%Y-%m-%d %H:%M", &cuando);
    return buf;
}

std::string juntas(const std::vector<std::string>& v, const char* sep = ", ") {
    std::string out;
    for (const std::string& x : v) {
        out += (out.empty() ? "" : sep) + x;
    }
    return out;
}

}  // namespace

std::vector<SS::Session> sesionesEnLaRed(const Sesion& ses, const Conexiones& conns,
                                         int timeoutMs) {
    std::vector<B::ConnectionProfile> perfiles;
    for (const auto& p : conns.perfiles) {
        const std::string id = p.id.empty() ? p.name : p.id;
        if (!conns.desconectada(id) && !conns.secretoSinAbrir(id)) {
            perfiles.push_back(p);
        }
    }
    std::vector<std::vector<SS::Session>> porMaquina(perfiles.size());
    std::vector<std::thread> hilos;
    for (std::size_t i = 0; i < perfiles.size(); ++i) {
        hilos.emplace_back([&, i] {
            auto propia = crearSesionDeFondo(ses.dirConfig, ses.maestra);
            std::string out;
            std::string err;
            int rc = -1;
            const std::string id = perfiles[i].id.empty() ? perfiles[i].name : perfiles[i].id;
            if (ejecutarAgente(*propia, perfiles[i], PET::sessionList(), out, err, rc, nullptr,
                               timeoutMs)
                && rc == 0) {
                porMaquina[i] = SS::parseList(out, id);
            }
        });
    }
    for (auto& h : hilos) {
        h.join();
    }
    std::vector<SS::Session> todas;
    for (const auto& v : porMaquina) {
        SS::merge(todas, v);
    }
    return todas;
}

std::string describeSesion(const SS::Session& s) {
    const std::vector<std::string> a = {s.user, s.host, std::to_string(s.pid), hora(s.since)};
    if (s.kind == "gui") {
        return B::format(T("t_ses_red_gui", "la interfaz gráfica de %1 en %2 (PID %3, desde %4)"),
                         a);
    }
    return B::format(
        T("t_ses_red_cli", "el intérprete de zfsmgr-cli de %1 en %2 (PID %3, desde %4)"), a);
}

std::string avisoDeSesion(const SS::Session& s) {
    // Without the machines it reaches: asked one machine at a time —the interface does—, the
    // list would be only that one. `sessions` says them all.
    return B::format(T("t_ses_red_aviso2",
                       "ZFSMgr también está abierto en otra máquina: %1; se supo de ella hace "
                       "%2 s. Lo que haga una sesión puede cambiar lo que ve la otra."),
                     {describeSesion(s), std::to_string(s.seenAgo)});
}

bool cmdSessions(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const std::string propia = zfsmgr::base::session::hostName();
    const std::vector<SS::Session> todas = sesionesEnLaRed(*e.ses, e.conns, 8000);
    Tabla t;
    t.nombreJson = "sessions";
    t.cabecerasTexto = {T("t_cab_que", "QUÉ"),         T("t_cab_maquina", "MÁQUINA"),
                        T("t_cab_usuario", "USUARIO"), T("t_cab_pid", "PID"),
                        T("t_cab_desde", "DESDE"),     T("t_cab_visto", "VISTA HACE"),
                        T("t_cab_llega", "LLEGA A"),   T("t_cab_esta", "ESTA")};
    t.campos = {"kind", "host", "user", "pid", "since", "seen_ago", "machines", "this", "id"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena,   Tipo::Entero, Tipo::Entero,
               Tipo::Entero, Tipo::Lista,  Tipo::Booleano, Tipo::Cadena};
    const bool texto = e.formato == Formato::Texto;
    for (const SS::Session& s : todas) {
        const std::string que = !texto            ? s.kind
                                : s.kind == "gui" ? T("t_ses_que_gui", "interfaz")
                                                  : T("t_ses_que_cli", "intérprete");
        t.filas.push_back({que, s.host, s.user, std::to_string(s.pid),
                           texto ? hora(s.since) : std::to_string(s.since),
                           texto ? std::to_string(s.seenAgo) + " s" : std::to_string(s.seenAgo),
                           // A list cell is split on bare commas for json.
                           juntas(s.machines, ","), s.host == propia ? "true" : "false", s.id});
    }
    t.imprime(e.formato);
    return true;
}

AvisoDeSesiones::AvisoDeSesiones(const Sesion& ses, const Conexiones& conns) {
    // Copies: the thread outlives nothing it borrows.
    auto propia = crearSesionDeFondo(ses.dirConfig, ses.maestra);
    m_hilo = std::thread([sesion = std::shared_ptr<Sesion>(std::move(propia)), conns] {
        const auto otras = SS::othersThan(sesionesEnLaRed(*sesion, conns, 8000),
                                          zfsmgr::base::session::hostName());
        for (const SS::Session& s : otras) {
            encolaAviso(avisoDeSesion(s));
        }
    });
}

AvisoDeSesiones::~AvisoDeSesiones() {
    if (m_hilo.joinable()) {
        m_hilo.join();
    }
}

}  // namespace zfsmgr::cli::detail
