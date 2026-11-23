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

namespace {

std::string minusculas(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

// `sessions close <machine> [--force]`: asks the session open on that machine to close, or ends
// it there if it is hung. One session per machine, so the machine names it.
bool cierraSesion(Estado& e, const Peticion& pet, const std::string& maquina) {
    const std::string propia = zfsmgr::base::session::hostName();
    if (minusculas(maquina) == minusculas(propia)) {
        std::fprintf(
            stderr, "%s\n",
            T("t_ses_es_esta", "la sesión de esta máquina es esta: para cerrarla, «exit»").c_str());
        return false;
    }
    const std::vector<SS::Session> todas = sesionesEnLaRed(*e.ses, e.conns, 8000);
    const SS::Session* s = nullptr;
    for (const SS::Session& x : todas) {
        if (minusculas(x.host) == minusculas(maquina)) {
            s = &x;
        }
    }
    if (!s) {
        std::fprintf(stderr, "%s\n",
                     B::format(T("t_ses_no_hay",
                                 "ninguna máquina conectada sabe de una sesión abierta en %1 "
                                 "(«sessions» dice cuáles hay)"),
                               {maquina})
                         .c_str());
        return false;
    }
    const bool fuerza = pet.tiene("--force");
    Pregunta p;
    if (fuerza) {
        p.titulo = B::format(T("t_ses_q_fuerza", "Cerrar a la fuerza %1"), {describeSesion(*s)});
        p.sePierde = {T("t_ses_q_pierde", "lo que esa sesión estuviera haciendo o escribiendo")};
        p.seConserva = {T("t_ses_q_trabajos",
                          "los trabajos y programaciones de los daemons, que siguen solos")};
        p.notas = {T("t_ses_q_fuerza_nota",
                     "Es para una sesión colgada: lo hace el daemon de su máquina, y solo si "
                     "ese proceso sigue siendo ZFSMgr. Si responde, mejor pedírselo sin "
                     "--force.")};
    } else {
        p.titulo = B::format(T("t_ses_q_pedir", "Pedir que se cierre %1"), {describeSesion(*s)});
        p.notas = {T("t_ses_q_pedir_nota",
                     "Se entera en menos de 30 s y se cierra ella misma, diciéndole a quien la "
                     "use quién lo pidió. Si no lo hace, está colgada: «sessions close <máquina> "
                     "--force».")};
    }
    if (!confirma(e, p)) {
        return false;
    }
    const std::vector<std::string> pedido =
        fuerza ? PET::sessionKill(s->id)
               : PET::sessionAskClose(s->id, propia, zfsmgr::base::session::currentUser());
    bool hecho = false;
    std::vector<std::string> fallos;
    for (const std::string& m : s->machines) {
        const auto* perfil = buscarConexion(e.conns, m);
        if (!perfil) {
            continue;
        }
        std::string out;
        std::string err;
        int rc = -1;
        std::string why;
        if (!ejecutarAgente(*e.ses, *perfil, pedido, out, err, rc, &why, 20000)) {
            fallos.push_back(m + ": " + why);
            continue;
        }
        if (rc == 0) {
            hecho = true;
            // Ended by the one daemon on its machine: no need to ask the others.
            if (fuerza) {
                break;
            }
            continue;
        }
        // Not its machine: another daemon of the list is.
        if (zfsmgr::base::agentmsg::lastCode() != "session_not_here") {
            fallos.push_back(m + ": " + B::trim(err));
        }
    }
    if (!hecho) {
        if (fallos.empty() && fuerza) {
            std::fprintf(stderr, "%s\n",
                         B::format(T("t_ses_sin_daemon_suyo",
                                     "ninguna conexión llega al daemon de %1, que es el único que "
                                     "puede cerrarla: hace falta una conexión a esa máquina"),
                                   {s->host})
                             .c_str());
        }
        for (const std::string& f : fallos) {
            std::fprintf(stderr, "%s\n", f.c_str());
        }
        return false;
    }
    std::printf("%s\n", (fuerza ? T("t_ses_cerrada", "cerrada")
                                : T("t_ses_pedido", "pedido: se cerrará en menos de 30 s"))
                            .c_str());
    return true;
}

}  // namespace

bool cmdSessions(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const std::vector<std::string> args = pet.lista("texto");
    if (!args.empty()) {
        if (args.size() == 2 && args[0] == "close") {
            return cierraSesion(e, pet, args[1]);
        }
        std::fprintf(stderr, "%s\n",
                     T("t_ses_uso_mal", "uso: sessions [close <máquina> [--force]]").c_str());
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
