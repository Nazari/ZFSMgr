// `overview [-a | <connection>]`: the state of one machine, or of all, in one answer — what a
// status panel shows. Each machine says whether it answers, its daemon's version, its pools
// with health and space, its schedules and its jobs in progress; and the warnings worth a
// person's attention, already written. The CLI decides what is a warning, so no client has
// to: a panel only shows them.

#include "sessionlock.h"
#include "shell_internal.h"

#include <algorithm>
#include <map>
#include <set>
#include <thread>

namespace zfsmgr::cli::detail {

namespace {

namespace J = B::json;

struct Warning {
    std::string level;  // «error» or «warning»
    std::string connection;
    std::string subject;  // «machine», «daemon», «pool <name>», «schedule <dataset>»
    std::string text;
    // For subject «session»: the session's id. Asked machine by machine, the same session
    // comes back from each one that knows it; a client shows it once.
    std::string session{};
};

struct MachineView {
    J::Value json{J::Object{}};
    std::vector<Warning> warnings;
    // The interactive sessions its daemon knows of (presence.h), merged afterwards.
    std::vector<zfsmgr::commands::sessions::Session> sessions;
};

// «70%» → 70; -1 when it is not a percentage.
int percentOf(const std::string& text) {
    std::string digits;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            digits += c;
        } else if (c == '%') {
            break;
        } else if (!digits.empty()) {
            break;
        }
    }
    try {
        return digits.empty() ? -1 : std::stoi(digits);
    } catch (...) {
        return -1;
    }
}

std::map<std::string, std::string> keysOf(const std::string& out) {
    std::map<std::string, std::string> k;
    for (const std::string& line : B::split(out, "\n", true)) {
        const auto eq = line.find('=');
        if (eq != std::string::npos) {
            k[B::trim(line.substr(0, eq))] = B::trim(line.substr(eq + 1));
        }
    }
    return k;
}

// One machine. Its own session (`ses`), so several can run at once.
MachineView viewOf(Sesion& ses, const B::ConnectionProfile& p) {
    MachineView m;
    const std::string id = p.id.empty() ? p.name : p.id;
    auto warn = [&](const std::string& level, const std::string& subject, const std::string& text) {
        m.warnings.push_back({level, id, subject, text});
    };
    m.json.set("connection", J::Value(id));
    m.json.set("os", J::Value(p.osType));

    std::string out;
    std::string err;
    int rc = -1;
    std::string why;
    if (!ejecutarAgente(ses, p, {"--health"}, out, err, rc, &why, 8000) || rc != 0) {
        const std::string reason = !why.empty()           ? why
                                   : B::trim(err).empty() ? B::trim(out)
                                                          : B::trim(err);
        m.json.set("reachable", J::Value(false));
        m.json.set("reason", J::Value(reason));
        m.json.set("status", J::Value("error"));
        warn("error", "machine",
             B::format(T("t_ov_no_responde", "%1 no responde: %2"), {id, reason}));
        return m;
    }
    m.json.set("reachable", J::Value(true));
    const auto health = keysOf(out);
    const std::string version = health.count("VERSION") ? health.at("VERSION") : std::string();
    const std::string expected = B::agentversion::expected();
    std::string daemon = "unknown";
    if (!version.empty()) {
        switch (B::agentversion::verdictFor(version, expected)) {
            case B::agentversion::Verdict::UpToDate:
                daemon = "ok";
                break;
            case B::agentversion::Verdict::Update:
                // Older than this client, but the newest there is to install: nothing to
                // update, as `ls` of a machine already says. Each build has its own
                // fingerprint, and the warning stayed after «update the daemon» had sent
                // the very same binary again.
                if (version == versionParaInstalar(ses, p)) {
                    daemon = "latest";
                    break;
                }
                daemon = "outdated";
                warn("warning", "daemon",
                     B::format(T("t_ov_daemon_viejo",
                                 "el daemon de %1 es la %2 y este cliente la %3: actualízalo "
                                 "con «install-daemon --on zfsm://%1»"),
                               {id, version, expected}));
                break;
            case B::agentversion::Verdict::Ahead:
                daemon = "ahead";
                warn("warning", "daemon",
                     B::format(T("t_ov_daemon_nuevo",
                                 "el daemon de %1 (%2) es más nuevo que este cliente (%3): "
                                 "actualiza el cliente"),
                               {id, version, expected}));
                break;
        }
    }
    m.json.set("daemon_version", J::Value(version));
    m.json.set("daemon", J::Value(daemon));
    if (health.count("DISK_ACCESS") && health.at("DISK_ACCESS") == "denied") {
        warn("warning", "daemon",
             B::format(T("t_ov_sin_disco",
                         "el daemon de %1 no tiene acceso a los discos: en macOS, «Acceso "
                         "total al disco» para /usr/local/libexec/zfsmgr-agent y reiniciarlo"),
                       {id}));
    }

    // Pools: health and space.
    J::Value pools{J::Array{}};
    out.clear();
    err.clear();
    if (ejecutarAgente(ses, p, PET::poolList(), out, err, rc, &why, 20000) && rc == 0) {
        J::Value root;
        if (!B::trim(out).empty() && J::parse(out, root)) {
            for (const auto& kv : root["pools"].toObject()) {
                const J::Value& pj = kv.second;
                const auto prop = [&](const char* k) {
                    return pj["properties"][k]["value"].toString();
                };
                const std::string name = pj["name"].toString(kv.first);
                const std::string h =
                    prop("health").empty() ? pj["state"].toString() : prop("health");
                const int cap = percentOf(prop("capacity"));
                std::string status = "ok";
                const std::string subject = "pool " + name;
                if (h == "DEGRADED") {
                    status = "warning";
                    warn("warning", subject,
                         B::format(T("t_ov_pool_degradado",
                                     "el pool %1 de %2 funciona con algún disco que falta o "
                                     "falla: sigue en marcha, pero con menos protección"),
                                   {name, id}));
                } else if (!h.empty() && h != "ONLINE") {
                    status = "error";
                    warn("error", subject,
                         B::format(T("t_ov_pool_caido", "el pool %1 de %2 no está disponible (%3)"),
                                   {name, id, h}));
                }
                if (cap >= 90) {
                    status = "error";
                    warn(
                        "error", subject,
                        B::format(T("t_ov_pool_lleno", "el pool %1 de %2 está al %3 %: casi lleno"),
                                  {name, id, std::to_string(cap)}));
                } else if (cap >= 80) {
                    if (status == "ok") {
                        status = "warning";
                    }
                    warn("warning", subject,
                         B::format(T("t_ov_pool_casi", "el pool %1 de %2 está al %3 %"),
                                   {name, id, std::to_string(cap)}));
                }
                J::Value pv{J::Object{}};
                pv.set("name", J::Value(name));
                pv.set("health", J::Value(h));
                pv.set("size", J::Value(prop("size")));
                pv.set("free", J::Value(prop("free")));
                pv.set("capacity", cap >= 0 ? J::Value(cap) : J::Value());
                pv.set("status", J::Value(status));
                pools.push(pv);
            }
        }
    }
    m.json.set("pools", pools);

    // Schedules, from the daemon's last pass: the same reading as `schedule status`.
    J::Value schedules{J::Array{}};
    const B::gsa::ConnectionState cs = estadoDeProgramaciones(ses, p, 20000);
    m.json.set("schedules_checked", J::Value(cs.checked));
    if (!cs.checked) {
        m.json.set("schedules_reason", J::Value(cs.whyNotChecked));
    }
    // What each schedule keeps, from its properties: the daemon's state says what happened,
    // not what was asked. Also the ones switched off (--off), which the scheduler skips and
    // a panel must still show, as paused.
    std::map<std::string, B::gsa::Schedule> programadas;
    {
        // Pool by pool with the verb `schedule` uses: the all-pools one brings only four
        // properties and not their source.
        for (const J::Value& pv : pools.toArray()) {
            std::string raw;
            std::string errRaw;
            int rcRaw = -1;
            if (!ejecutarAgente(ses, p, PET::gsaOfDataset(pv["name"].toString()), raw, errRaw,
                                rcRaw, &why, 20000)
                || rcRaw != 0) {
                continue;
            }
            for (const auto& kv : B::gsa::groupProperties(raw)) {
                B::gsa::Schedule sch;
                B::gsa::Reason r;
                if (B::gsa::fromProperties(kv.second, sch, r)) {
                    programadas[kv.first] = sch;
                }
            }
        }
    }
    const auto guarda = [&programadas](const std::string& ds) {
        J::Value k{J::Object{}};
        const auto it = programadas.find(ds);
        if (it != programadas.end()) {
            k.set("hourly", J::Value(it->second.hourly));
            k.set("daily", J::Value(it->second.daily));
            k.set("weekly", J::Value(it->second.weekly));
            k.set("monthly", J::Value(it->second.monthly));
            k.set("yearly", J::Value(it->second.yearly));
        }
        return k;
    };
    std::set<std::string> vistas;
    for (const B::gsa::DatasetState& d : cs.datasets) {
        vistas.insert(d.dataset);
        std::string state = "ok";
        std::string text;
        if (cs.stale || !d.failedStep.empty()) {
            state = "error";
            B::gsa::DatasetState x = d;
            if (cs.stale) {
                x.failedStep = "scheduler";
            }
            text = motivoDeFallo(id, x, cs.lastPass);
            warn("error", "schedule " + d.dataset, text);
        } else if (d.pending) {
            state = "pending";
        }
        J::Value sv{J::Object{}};
        sv.set("dataset", J::Value(d.dataset));
        sv.set("kind", J::Value(d.pull ? "pull" : d.level ? "push" : "local"));
        sv.set("peer", J::Value(d.destination));
        sv.set("state", J::Value(state));
        sv.set("last_snapshot", J::Value(d.lastSnapshot));
        sv.set("last_snapshot_time", J::Value(d.lastSnapshotTime));
        sv.set("last_level_time", J::Value(d.lastLevelTime));
        sv.set("job", J::Value(d.levelJob));
        sv.set("error", J::Value(text));
        sv.set("keep", guarda(d.dataset));
        schedules.push(sv);
    }
    for (const auto& [ds, sch] : programadas) {
        if (sch.enabled || vistas.count(ds) != 0) {
            continue;
        }
        J::Value sv{J::Object{}};
        sv.set("dataset", J::Value(ds));
        sv.set("kind", J::Value(!sch.origin.empty() ? "pull" : sch.level ? "push" : "local"));
        sv.set("peer", J::Value(!sch.origin.empty() ? sch.origin : sch.target));
        sv.set("state", J::Value("paused"));
        sv.set("last_snapshot", J::Value(""));
        sv.set("last_snapshot_time", J::Value(0));
        sv.set("last_level_time", J::Value(0));
        sv.set("job", J::Value(""));
        sv.set("error", J::Value(""));
        sv.set("keep", guarda(ds));
        schedules.push(sv);
    }
    m.json.set("schedules", schedules);

    // Jobs in progress (running or queued).
    J::Value jobs{J::Array{}};
    out.clear();
    err.clear();
    if (ejecutarAgente(ses, p, PET::jobList(), out, err, rc, &why, 20000) && rc == 0) {
        for (const std::string& line : B::split(out, "\n", true)) {
            if (!B::startsWith(line, "JOB=")) {
                continue;
            }
            J::Value j;
            if (!J::parse(line.substr(4), j)) {
                continue;
            }
            const std::string st = j["state"].toString();
            if (st != "running" && st != "queued") {
                continue;
            }
            J::Value jv{J::Object{}};
            for (const char* k : {"id", "state", "type", "snap", "description"}) {
                jv.set(k, J::Value(j[k].toString()));
            }
            jv.set("bytes", J::Value(j["bytes"].toInt()));
            jv.set("rate", J::Value(j["rate"].toDouble()));
            jv.set("elapsed", J::Value(j["elapsed"].toInt()));
            jobs.push(jv);
        }
    }
    m.json.set("jobs", jobs);

    // Who has ZFSMgr open and reaches this machine. Read here, with the rest of the machine;
    // the warnings are written once all machines are in (cmdOverview), one per session.
    out.clear();
    err.clear();
    if (ejecutarAgente(ses, p, PET::sessionList(), out, err, rc, &why, 8000) && rc == 0) {
        m.sessions = zfsmgr::commands::sessions::parseList(out, id);
    }

    std::string status = "ok";
    for (const Warning& w : m.warnings) {
        if (w.level == "error") {
            status = "error";
            break;
        }
        status = "warning";
    }
    m.json.set("status", J::Value(status));
    return m;
}

}  // namespace

bool cmdOverview(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const std::string actual = subeHasta(pet.objetivo, Objetivo::Conexion).connection;
    if (!pet.tiene("-a") && actual.empty()) {
        std::fputs(TC("t_ov_falta_conexion",
                      "hace falta estar en una conexión, o «overview -a» para todas\n"),
                   stderr);
        return false;
    }
    std::vector<B::ConnectionProfile> perfiles;
    if (pet.tiene("-a")) {
        for (const auto& p : e.conns.perfiles) {
            const std::string id = p.id.empty() ? p.name : p.id;
            if (!e.conns.desconectada(id)) {
                perfiles.push_back(p);
            }
        }
    } else {
        const auto* p = buscarConexion(e.conns, actual);
        if (!p) {
            std::fprintf(
                stderr, "%s\n",
                B::format(T("t_no_conn_nombre", "no hay ninguna conexión llamada «%1»"), {actual})
                    .c_str());
            return false;
        }
        perfiles.push_back(*p);
    }

    // One machine: this session. Several: one thread and one background session each, so a
    // machine that is off costs its timeout once, not once per machine after it.
    std::vector<MachineView> views(perfiles.size());
    if (perfiles.size() == 1) {
        views[0] = viewOf(*e.ses, perfiles[0]);
    } else {
        const std::string dirConfig = e.ses->dirConfig;
        const std::string maestra = e.ses->maestra;
        std::vector<std::thread> hilos;
        for (std::size_t i = 0; i < perfiles.size(); ++i) {
            hilos.emplace_back([&views, &perfiles, &dirConfig, &maestra, i]() {
                auto propia = crearSesionDeFondo(dirConfig, maestra);
                views[i] = viewOf(*propia, perfiles[i]);
            });
        }
        for (auto& h : hilos) {
            h.join();
        }
    }

    std::vector<Warning> warnings;
    std::vector<zfsmgr::commands::sessions::Session> sessions;
    for (const MachineView& v : views) {
        warnings.insert(warnings.end(), v.warnings.begin(), v.warnings.end());
        zfsmgr::commands::sessions::merge(sessions, v.sessions);
    }
    // ZFSMgr open on another machine: one warning per session, however many machines know
    // it. The one on this machine is this one (one session per machine).
    sessions = zfsmgr::commands::sessions::othersThan(sessions, zfsmgr::base::session::hostName());
    for (const auto& s : sessions) {
        warnings.push_back({"warning", s.machines.empty() ? std::string() : s.machines.front(),
                            "session", avisoDeSesion(s), s.id});
    }
    std::stable_sort(warnings.begin(), warnings.end(), [](const Warning& a, const Warning& b) {
        return a.level == "error" && b.level != "error";
    });

    if (e.formato == Formato::Json) {
        J::Value doc{J::Object{}};
        J::Value machines{J::Array{}};
        for (const MachineView& v : views) {
            machines.push(v.json);
        }
        doc.set("machines", machines);
        J::Value ws{J::Array{}};
        for (const Warning& w : warnings) {
            J::Value wv{J::Object{}};
            wv.set("level", J::Value(w.level));
            wv.set("connection", J::Value(w.connection));
            wv.set("subject", J::Value(w.subject));
            wv.set("text", J::Value(w.text));
            if (!w.session.empty()) {
                wv.set("session", J::Value(w.session));
            }
            ws.push(wv);
        }
        doc.set("warnings", ws);
        J::Value ss{J::Array{}};
        for (const auto& s : sessions) {
            J::Value sv{J::Object{}};
            sv.set("id", J::Value(s.id));
            sv.set("kind", J::Value(s.kind));
            sv.set("host", J::Value(s.host));
            sv.set("user", J::Value(s.user));
            sv.set("pid", J::Value(static_cast<double>(s.pid)));
            sv.set("since", J::Value(static_cast<double>(s.since)));
            sv.set("seen_ago", J::Value(static_cast<double>(s.seenAgo)));
            J::Value ms{J::Array{}};
            for (const std::string& m : s.machines) {
                ms.push(J::Value(m));
            }
            sv.set("machines", ms);
            ss.push(sv);
        }
        doc.set("sessions", ss);
        std::printf("%s\n", J::toIndented(doc).c_str());
        return true;
    }

    // Text (and tsv, which has no better shape for a nested answer): a line per machine, a
    // line per pool, and the warnings at the end.
    for (const MachineView& v : views) {
        const J::Value& m = v.json;
        const std::string id = m["connection"].toString();
        if (!m["reachable"].toBool()) {
            std::printf("%-10s %s\n", id.c_str(), T("t_ov_txt_no_responde", "no responde").c_str());
            continue;
        }
        int ok = 0;
        int mal = 0;
        for (const J::Value& s : m["schedules"].toArray()) {
            (s["state"].toString() == "error" ? mal : ok)++;
        }
        std::printf("%-10s %s %s · %s\n", id.c_str(), T("t_ov_txt_daemon", "daemon").c_str(),
                    m["daemon_version"].toString("-").c_str(),
                    B::format(T("t_ov_txt_resumen", "%1 programaciones bien, %2 mal, %3 trabajos"),
                              {std::to_string(ok), std::to_string(mal),
                               std::to_string(m["jobs"].toArray().size())})
                        .c_str());
        for (const J::Value& pv : m["pools"].toArray()) {
            std::printf("  %-12s %-9s %6s %4s\n", pv["name"].toString().c_str(),
                        pv["health"].toString().c_str(), pv["size"].toString().c_str(),
                        pv["capacity"].isNull()
                            ? "-"
                            : (std::to_string(pv["capacity"].toInt()) + "%").c_str());
        }
    }
    if (!warnings.empty()) {
        std::printf("\n%s\n", T("t_ov_txt_avisos", "Avisos:").c_str());
        for (const Warning& w : warnings) {
            std::printf("  %s %s\n", w.level == "error" ? "✕" : "▲", w.text.c_str());
        }
    }
    return true;
}

}  // namespace zfsmgr::cli::detail
