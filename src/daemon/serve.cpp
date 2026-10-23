// The daemon itself: job persistence, long mutations as background jobs, the RPC serve loop
// over mTLS and the ZED event watcher.

#include "agent_internal.h"

namespace zfsmgr::agent {

// ── Job persistence ──────────────────────────────────────────────────────────

const char* jobStateName(JobState s) {
    switch (s) {
        case JobState::Queued:
            return "queued";
        case JobState::Running:
            return "running";
        case JobState::Done:
            return "done";
        case JobState::Failed:
            return "failed";
        case JobState::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

JobState jobStateFromName(const std::string& s) {
    if (s == "queued")
        return JobState::Queued;
    if (s == "running")
        return JobState::Running;
    if (s == "done")
        return JobState::Done;
    if (s == "cancelled")
        return JobState::Cancelled;
    return JobState::Failed;
}

// Called under g_jobsMutex. Serialises g_jobs to kJobsFilePath.
// Retains at most 20 jobs; prunes oldest terminal (Done/Failed/Cancelled) entries first.
void persistJobsLocked() {
    // Collect ordered by startedAtUtc for pruning
    std::vector<std::pair<std::string, const DaemonJob*>> all;
    all.reserve(g_jobs.size());
    for (const auto& kv : g_jobs)
        all.push_back({kv.first, &kv.second});

    // Prune old terminal jobs beyond 20 total
    while (all.size() > 20) {
        // find oldest terminal
        auto oldest = all.end();
        for (auto it = all.begin(); it != all.end(); ++it) {
            const JobState st = it->second->state;
            if (st == JobState::Done || st == JobState::Failed || st == JobState::Cancelled) {
                if (oldest == all.end() || it->second->startedAtUtc < oldest->second->startedAtUtc)
                    oldest = it;
            }
        }
        if (oldest == all.end())
            break;
        g_jobs.erase(oldest->first);
        all.erase(oldest);
    }

    std::ofstream f(kJobsFilePath, std::ios::trunc);
    if (!f.is_open())
        return;
    f << "[\n";
    bool first = true;
    for (const auto& kv : g_jobs) {
        const DaemonJob& j = kv.second;
        if (!first)
            f << ",\n";
        first = false;
        f << "{\"id\":\"" << jsonEscape(j.id) << "\",\"type\":\"" << jsonEscape(j.type)
          << "\",\"state\":\"" << jobStateName(j.state) << "\",\"snap\":\"" << jsonEscape(j.snap)
          << "\",\"peerHost\":\"" << jsonEscape(j.peerHost) << "\",\"peerPort\":" << j.peerPort
          << ",\"baseSnap\":\"" << jsonEscape(j.baseSnap) << "\",\"sendFlags\":\""
          << jsonEscape(j.sendFlags) << "\",\"pipeCmd\":\"" << jsonEscape(j.pipeCmd)
          << "\",\"dstDataset\":\"" << jsonEscape(j.dstDataset)
          << "\",\"bytes\":" << j.bytesTransferred << ",\"rateMiBs\":" << j.rateMiBs
          << ",\"elapsedSecs\":" << j.elapsedSecs << ",\"startedAt\":\""
          << jsonEscape(j.startedAtUtc) << "\",\"finishedAt\":\"" << jsonEscape(j.finishedAtUtc)
          << "\",\"error_code\":\"" << jsonEscape(j.errorCode)
          << "\",\"error_args\":" << argsJson(j.errorArgs) << ",\"desc\":\""
          << jsonEscape(j.description) << "\"}";
    }
    f << "\n]\n";
}

// Called once at daemon startup, before the accept loop. Reads persisted jobs;
// any job left in Running state is marked Failed (daemon restarted mid-transfer).
void loadPersistedJobsAtStartup() {
    std::ifstream f(kJobsFilePath, std::ios::binary);
    if (!f.is_open())
        return;
    const std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    // Se analiza con el JSON de la capa base, que el agente ya enlaza.
    //
    // Antes había aquí un analizador a mano con dos fallos que se sumaban:
    //
    // - **No DESESCAPABA.** El texto se guardaba con `jsonEscape` y se leía crudo, así que
    //   un error con un salto de línea volvía como `\n` literal, y al siguiente guardado se
    //   volvía a escapar: `\\n`, `\\\\n`… Las barras se duplicaban en CADA arranque, sin
    //   tope. Se ve en máquinas reales: «pool or dataset is busy» seguido de treinta barras.
    // - **Cortaba el registro en el primer `}`** y el valor en la primera comilla, así que
    //   un mensaje de error que contuviera una llave o una comilla escapada partía el
    //   registro por la mitad y arrastraba la basura al siguiente.
    //
    // Los dos desaparecen usando el mismo analizador que el resto del programa, en vez de
    // uno propio que solo sabe leer lo que él mismo escribe cuando no lleva nada raro.
    zfsmgr::base::json::Value raiz;
    std::string errJson;
    if (!zfsmgr::base::json::parse(raw, raiz, &errJson)) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_jobsMutex);
    for (const auto& v : raiz.toArray()) {
        DaemonJob j;
        j.id = v["id"].toString();
        j.type = v["type"].toString();
        j.state = jobStateFromName(v["state"].toString());
        j.snap = v["snap"].toString();
        j.peerHost = v["peerHost"].toString();
        j.peerPort = static_cast<int>(v["peerPort"].toInt());
        j.baseSnap = v["baseSnap"].toString();
        j.sendFlags = v["sendFlags"].toString();
        j.pipeCmd = v["pipeCmd"].toString();
        j.dstDataset = v["dstDataset"].toString();
        j.description = v["desc"].toString();
        j.bytesTransferred = static_cast<uint64_t>(v["bytes"].toInt());
        j.rateMiBs = v["rateMiBs"].toDouble();
        j.elapsedSecs = static_cast<long>(v["elapsedSecs"].toInt());
        j.startedAtUtc = v["startedAt"].toString();
        j.finishedAtUtc = v["finishedAt"].toString();
        j.errorCode = v["error_code"].toString();
        for (const auto& a : v["error_args"].toArray())
            j.errorArgs.push_back(a.toString());

        if (j.id.empty())
            continue;

        // Los que seguían corriendo cuando el daemon paró no se pueden recuperar.
        if (j.state == JobState::Running || j.state == JobState::Queued) {
            j.state = JobState::Failed;
            ponErrorDeTrabajo(j, "job_daemon_restarted", {});
            j.finishedAtUtc = utcNowIsoString();
        }
        g_jobs[j.id] = std::move(j);
    }
}

bool parseJsonStringAt(const std::string& s, std::size_t& i, std::string& out) {
    out.clear();
    if (i >= s.size() || s[i] != '"') {
        return false;
    }
    ++i;
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"') {
            return true;
        }
        if (c == '\\') {
            if (i >= s.size()) {
                return false;
            }
            const char e = s[i++];
            switch (e) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u':
                    if (i + 4 > s.size())
                        return false;
                    i += 4;
                    out.push_back('?');
                    break;
                default:
                    return false;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

bool parseJsonRpcRequest(const std::string& line, std::string& cmd,
                         std::vector<std::string>& args) {
    cmd.clear();
    args.clear();
    std::size_t i = 0;
    auto skipWs = [&]() {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
    };
    skipWs();
    if (i >= line.size() || line[i] != '{')
        return false;
    ++i;
    while (i < line.size()) {
        skipWs();
        if (i < line.size() && line[i] == '}') {
            ++i;
            break;
        }
        std::string key;
        if (!parseJsonStringAt(line, i, key))
            return false;
        skipWs();
        if (i >= line.size() || line[i] != ':')
            return false;
        ++i;
        skipWs();
        if (key == "cmd") {
            if (!parseJsonStringAt(line, i, cmd))
                return false;
        } else if (key == "args") {
            std::size_t arrStart = i;
            int depth = 0;
            bool inString = false;
            bool escape = false;
            for (; i < line.size(); ++i) {
                const char ch = line[i];
                if (inString) {
                    if (escape) {
                        escape = false;
                    } else if (ch == '\\') {
                        escape = true;
                    } else if (ch == '"') {
                        inString = false;
                    }
                    continue;
                }
                if (ch == '"') {
                    inString = true;
                    continue;
                }
                if (ch == '[') {
                    ++depth;
                } else if (ch == ']') {
                    --depth;
                    if (depth == 0) {
                        ++i;
                        break;
                    }
                }
            }
            if (i > line.size())
                return false;
            if (arrStart >= line.size())
                return false;
            if (!parseJsonStringArray(line.substr(arrStart, i - arrStart), args))
                return false;
        } else {
            // Consumir valor genérico simple para mantener tolerancia.
            if (i < line.size() && line[i] == '"') {
                std::string tmp;
                if (!parseJsonStringAt(line, i, tmp))
                    return false;
            } else {
                while (i < line.size() && line[i] != ',' && line[i] != '}')
                    ++i;
            }
        }
        skipWs();
        if (i < line.size() && line[i] == ',') {
            ++i;
            continue;
        }
        if (i < line.size() && line[i] == '}') {
            ++i;
            break;
        }
    }
    return !cmd.empty();
}

bool isDumpCommand(const std::string& cmd) {
    return startsWith(cmd, "--dump-");
}

bool isMutateCommand(const std::string& cmd) {
    return startsWith(cmd, "--mutate-");
}

std::string makeRpcCacheKey(const std::string& cmd, const std::vector<std::string>& args) {
    std::string key = cmd;
    key.push_back('\n');
    for (const std::string& a : args) {
        key += a;
        key.push_back('\n');
    }
    return key;
}

std::string dumpClassForCommand(const std::string& cmd) {
    if (cmd == "--dump-zfs-get-gsa-raw-all-pools" || cmd == "--dump-zfs-get-gsa-raw-recursive") {
        return "gsa";
    }
    if (startsWith(cmd, "--dump-zfs-")) {
        return "zfs";
    }
    if (startsWith(cmd, "--dump-zpool-")) {
        return "zpool";
    }
    if (cmd == "--dump-refresh-basics" || cmd == "--dump-tool-availability") {
        return "system";
    }
    return "other";
}

// ── Long mutations as background jobs ────────────────────────────────────────
// These operations copy real data and routinely outlast any RPC read timeout. Run
// synchronously they forced the client to wait, and a timeout there is ambiguous:
// the daemon keeps working, so the client cannot safely retry. Submitting them as
// jobs makes the RPC answer immediate and turns "did it finish?" into a question
// with an actual answer (--job-status).
// Qué mutaciones se pueden encolar como trabajo.
//
// La lista vive en `commands/requests` y NO se copia aquí: los clientes necesitan la misma
// respuesta para elegir camino antes de pedir nada, y dos listas iguales que alguien tiene
// que acordarse de tocar a la vez son dos listas que acaban diciendo cosas distintas. El
// daemon enlaza `zfsmgr_commands`, así que puede preguntar en vez de repetir.
// La descripción legible de un trabajo, a partir de lo que se pidió. Rutas y nombres, sin
// prosa: es un dato, y el cliente lo enseña tal cual en cualquier idioma.
std::string describeJob(const std::string& cmd, const std::vector<std::string>& params) {
    // Las cargas de copia son un array JSON en base64: [borrar, enSeco, (rsh, host,)
    // origen1, destino1, …]. `inicio` es dónde empiezan los pares.
    const auto pares = [&](const char* verbo, std::size_t inicio) {
        std::string json;
        zfsmgr::base::json::Value v;
        if (params.empty() || !zfsmgr::base::base64Decode(params[0], json)
            || !zfsmgr::base::json::parse(json, v, nullptr) || !v.isArray()) {
            return std::string(verbo);
        }
        const auto a = v.toArray();
        if (a.size() < inicio + 2) {
            return std::string(verbo);
        }
        std::string d =
            std::string(verbo) + " " + a[inicio].toString() + " -> " + a[inicio + 1].toString();
        const std::size_t mas = (a.size() - inicio) / 2 - 1;
        if (mas > 0) {
            d += " (+" + std::to_string(mas) + ")";
        }
        if (a[0].toString() == "1") {
            d += " --delete";
        }
        return d;
    };
    if (cmd == "--mutate-copy-local") {
        return pares("cp", 2);
    }
    if (cmd == "--tree-send-to-peer" && params.size() >= 3) {
        return "tree " + params[0] + " -> " + params[1] + ":" + params[2];
    }
    // breakdown/assemble/todir: el verbo sin prefijo y sus argumentos.
    std::string d = cmd;
    for (const char* pre : {"--mutate-advanced-", "--mutate-"}) {
        if (d.rfind(pre, 0) == 0) {
            d = d.substr(std::strlen(pre));
            break;
        }
    }
    for (const std::string& x : params) {
        d += " " + x;
    }
    return d.size() > 300 ? d.substr(0, 300) + "…" : d;
}

bool isAsyncSubmittableCommand(const std::string& cmd) {
    return zfsmgr::commands::requests::canEnqueue(cmd);
}

void runSubmittedMutationJob(const std::string& jobId, const std::string& cmd,
                             const std::vector<std::string>& params) {
    const ExecResult r = executeAgentCommandCapture(cmd, params, "zfsmgr-agent");
    std::lock_guard<std::mutex> lock(g_jobsMutex);
    auto it = g_jobs.find(jobId);
    if (it == g_jobs.end()) {
        return;
    }
    DaemonJob& j = it->second;
    if (j.state != JobState::Cancelled) {
        j.state = (r.rc == 0) ? JobState::Done : JobState::Failed;
    }
    j.resultRc = r.rc;
    j.resultOut = r.out;
    if (r.rc != 0 && j.errorCode.empty()) {
        if (!r.code.empty()) {
            ponErrorDeTrabajo(j, r.code, r.args);
        } else {
            ponErrorDeTrabajo(j, "",
                              {trim(r.err).empty() ? "exit " + std::to_string(r.rc) : trim(r.err)});
        }
    }
    j.finishedAtUtc = utcNowIsoString();
    persistJobsLocked();
    daemonLog("INFO", "job " + jobId + " finished rc=" + std::to_string(r.rc) + " cmd=" + cmd);
}

#ifndef _WIN32
// El lector de `zpool events -f`, en SU PROPIO grupo de procesos.
//
// Con `popen` no había forma de pararlo. `zpool events -f` no termina nunca si no llegan
// eventos, y va dentro de un `sh -c` («stdbuf … || …»): aunque se matara el `sh`, el `zpool`
// nieto seguía con la tubería abierta y el `fgets` del hilo de ZED no volvía. Al recibir
// SIGTERM, el agente salía del bucle principal y se quedaba para siempre en el `join()` de
// ese hilo. En Linux no se notaba: systemd manda la señal a TODOS los procesos del servicio,
// `zpool` incluido, y el hilo se desbloqueaba solo (medido: `systemctl restart` en 0 s). El
// `rc.d` de FreeBSD solo señala el PID de su pidfile, y `service zfsmgr_agent stop` esperaba
// sin límite: la primera actualización del daemon en un FreeBSD real se quedó ahí colgada.
//
// Con el grupo propio, parar es matar el grupo entero: `zpool` incluido, la tubería se
// cierra, `fgets` devuelve EOF y el hilo termina.
std::atomic<pid_t> g_zedGrupo{0};

FILE* abreZpoolEvents(pid_t& grupo) {
    grupo = 0;
    int tubo[2];
    if (pipe(tubo) != 0) {
        return nullptr;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(tubo[0]);
        close(tubo[1]);
        return nullptr;
    }
    if (pid == 0) {
        // Hijo: solo llamadas seguras tras fork() en un proceso con hilos.
        setpgid(0, 0);
        dup2(tubo[1], STDOUT_FILENO);
        const int nulo = open("/dev/null", O_RDWR);
        if (nulo >= 0) {
            dup2(nulo, STDIN_FILENO);
            dup2(nulo, STDERR_FILENO);
        }
        close(tubo[0]);
        close(tubo[1]);
        execl("/bin/sh", "sh", "-c",
              "PATH=\"$PATH:/sbin:/usr/sbin:/usr/local/sbin:/usr/local/bin\""
              " stdbuf -oL zpool events -f -H -v 2>/dev/null"
              " || zpool events -f -H -v 2>/dev/null",
              static_cast<char*>(nullptr));
        _exit(127);
    }
    // También desde el padre: si no, entre el fork y el setpgid del hijo hay un instante en
    // que el grupo no existe y un kill(-pid) fallaría.
    setpgid(pid, pid);
    close(tubo[1]);
    fcntl(tubo[0], F_SETFD, FD_CLOEXEC);
    FILE* fp = fdopen(tubo[0], "r");
    if (!fp) {
        close(tubo[0]);
        kill(-pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        return nullptr;
    }
    grupo = pid;
    return fp;
}

void cierraZpoolEvents(FILE* fp, pid_t grupo) {
    if (grupo > 0) {
        kill(-grupo, SIGTERM);
    }
    if (fp) {
        fclose(fp);
    }
    if (grupo > 0) {
        waitpid(grupo, nullptr, 0);
    }
}
#endif

// The daemon's runtime state, shared by the accept loop, the connection threads and the ZED
// watcher thread.
struct ServerState {
    int cacheMaxEntries{0};  // from the configuration, for `--health`
    // Bumped (under runtimeMutex) whenever entries are dropped. A read that started before a
    // mutation must not store its now stale answer after the mutation dropped the cache.
    unsigned long long cacheGen{0};
    std::atomic<int> activeConnections{0};
    struct CacheEntry {
        ExecResult result;
        std::chrono::steady_clock::time_point ts;
        std::string cmd;
        std::vector<std::string> args;
        std::string dumpClass;
    };
    std::unordered_map<std::string, CacheEntry> rpcCache;
    std::mutex runtimeMutex;
    std::set<std::string> rpcCommands;
    std::atomic<long long> rpcFailures{0};
    std::atomic<long long> cacheInvalidations{0};
    std::atomic<long long> poolInvalidations{0};
    std::atomic<long long> zedRestarts{0};
    std::atomic<bool> zedActive{false};
    std::atomic<bool> zedInvalidateRequested{false};
    std::atomic<long long> zedEventSeq{0};  // monotonically increments on each ZED event
    std::string zedLastEventUtc;
};

// Watches `zpool events -f` and asks the accept loop to drop the RPC cache when the
// tree changes (ZED). Runs on its own thread for the life of the daemon.
void watchZedEvents(ServerState& st) {
#ifndef _WIN32
    // Tree-modifying operations we care about (from history_internal_name).
    static const std::set<std::string> kTreeOps = {
        "snapshot", "destroy", "create", "clone",   "rename",
        "rollback", "receive", "hold",   "release",
    };

    while (!g_stop.load()) {
        // stdbuf -oL forces line-buffered output so each event flushes immediately
        // when stdout is a pipe (default full-buffering hides events until 4-8KB fills).
        pid_t grupoZed = 0;
        FILE* fp = abreZpoolEvents(grupoZed);
        g_zedGrupo.store(grupoZed);
        if (!fp) {
            st.zedActive.store(false);
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        // Phase 1: drain historical events that arrive immediately after popen.
        // zpool events dumps the full event log on start — we skip this initial
        // burst by reading with a short select() timeout. Once no data arrives
        // within 300 ms, the historical flush is considered complete.
        {
            int fd = fileno(fp);
            char drainBuf[4096];
            bool eof = false;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
            while (!eof && !g_stop.load()) {
                auto rem = std::chrono::duration_cast<std::chrono::microseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
                if (rem <= 0)
                    break;
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(fd, &rfds);
                struct timeval tv;
                tv.tv_sec = 0;
                tv.tv_usec = static_cast<suseconds_t>(std::min<long long>(rem, 100000));
                const int ready = select(fd + 1, &rfds, nullptr, nullptr, &tv);
                if (ready <= 0)
                    break;  // timeout — no more immediate data
                if (!fgets(drainBuf, sizeof(drainBuf), fp)) {
                    eof = true;
                }
                // reset deadline on each line received
                deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
            }
            if (eof) {
                g_zedGrupo.store(0);
                cierraZpoolEvents(fp, grupoZed);
                st.zedActive.store(false);
                if (!g_stop.load()) {
                    st.zedRestarts.fetch_add(1);
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                continue;
            }
        }

        st.zedActive.store(true);
        daemonLog("INFO", "ZED thread active (zpool events running)");

        // Phase 2: block-read new events and fire for relevant ones.
        // Header lines start with any non-whitespace character (format varies
        // by platform: ISO "2026-..." or locale "Apr 25 2026...").
        // Class is always the last tab-separated field on the header line.
        std::string evClass, evOp;
        bool evClassRelevant = false;
        // Debounce: coalesce bursts (e.g. "zfs snap -r" fires one event per
        // child dataset). Fire at most once per 2 seconds.
        // Initialize far enough in the past so the first event always fires.
        // Do NOT use time_point::min() — subtracting it from now() wraps to negative.
        auto lastFireTime = std::chrono::steady_clock::now() - std::chrono::seconds(10);

        auto isHistoryClass = [](const std::string& cls) {
            return cls == "history_event" || cls == "sysevent.fs.zfs.history_event";
        };

        auto fireIfRelevant = [&]() {
            if (evClass.empty())
                return;
            bool fire = false;
            if (evClass.compare(0, 24, "sysevent.fs.zfs.dataset_") == 0
                || evClass.compare(0, 25, "sysevent.fs.zfs.snapshot_") == 0) {
                fire = true;
            } else if (isHistoryClass(evClass) && kTreeOps.count(evOp) > 0) {
                fire = true;
            }
            if (fire) {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastFireTime < std::chrono::seconds(2))
                    return;
                lastFireTime = now;
                st.zedInvalidateRequested.store(true);
                st.zedEventSeq.fetch_add(1);
                const std::string nowUtc = utcNowIsoString();
                if (!nowUtc.empty()) {
                    std::lock_guard<std::mutex> lock(st.runtimeMutex);
                    st.zedLastEventUtc = nowUtc;
                }
                daemonLog("INFO", "ZED event cls=" + evClass + " op=" + evOp + " ts=" + nowUtc);
            }
        };

        char line[4096];
        while (!g_stop.load()) {
            if (!fgets(line, sizeof(line), fp))
                break;
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
                s.pop_back();
            // Blank line = event separator: fire now rather than waiting for next
            // event's header (which may never arrive, e.g. last event in stream).
            if (s.empty()) {
                fireIfRelevant();
                evClass.clear();
                evOp.clear();
                evClassRelevant = false;
                continue;
            }

            if (!std::isspace(static_cast<unsigned char>(s[0]))) {
                // New event header — also fire for any preceding event not yet
                // fired (guards against streams without blank line separators).
                fireIfRelevant();
                evClass.clear();
                evOp.clear();
                evClassRelevant = false;
                // Class is the last tab-separated token (works for both ISO
                // "TS\tCLASS" and locale "Apr DD YYYY HH:MM:SS\tCLASS" formats).
                const size_t tab = s.rfind('\t');
                evClass = (tab != std::string::npos) ? s.substr(tab + 1) : "";
                while (!evClass.empty() && (evClass.back() == '\r' || evClass.back() == ' ')) {
                    evClass.pop_back();
                }
                evClassRelevant = isHistoryClass(evClass)
                                  || (evClass.compare(0, 24, "sysevent.fs.zfs.dataset_") == 0)
                                  || (evClass.compare(0, 25, "sysevent.fs.zfs.snapshot_") == 0);
            } else if (evClassRelevant && isHistoryClass(evClass) && evOp.empty()
                       && s.find("history_internal_name") != std::string::npos) {
                // Extract the operation name from the quoted value.
                const size_t q1 = s.find('"');
                const size_t q2 =
                    (q1 != std::string::npos) ? s.find('"', q1 + 1) : std::string::npos;
                if (q1 != std::string::npos && q2 != std::string::npos) {
                    evOp = s.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }
        fireIfRelevant();

        g_zedGrupo.store(0);
        cierraZpoolEvents(fp, grupoZed);
        evClass.clear();
        evOp.clear();
        evClassRelevant = false;
        st.zedActive.store(false);
        if (!g_stop.load()) {
            st.zedRestarts.fetch_add(1);
            daemonLog("WARN", "ZED thread restart (zpool events exited)");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
#endif
}

// `--heartbeat`, answered by the daemon itself.
ExecResult rpcHeartbeat(ServerState&, const std::string&, const std::vector<std::string>&) {
    ExecResult exec;
    const std::string ts = utcNowIsoString();
    daemonLog("INFO", "heartbeat via RPC");
    std::ostringstream bs;
    bs << "ALIVE=yes\n";
    bs << "VERSION=" << ZFSMGR_AGENT_VERSION_STRING << "\n";
    bs << "TS=" << ts << "\n";
    exec.rc = 0;
    exec.out = bs.str();
    return exec;
}

// `--dump-daemon-log`, answered by the daemon itself.
ExecResult rpcDumpDaemonLog(ServerState&, const std::string&,
                            const std::vector<std::string>& rpcArgs) {
    ExecResult exec;
    long long offset = 0;
    long long maxBytes = 0;
    if (!rpcArgs.empty()) {
        try {
            offset = std::stoll(rpcArgs[0]);
        } catch (...) {
        }
    }
    if (rpcArgs.size() > 1) {
        try {
            maxBytes = std::stoll(rpcArgs[1]);
        } catch (...) {
        }
    }
    exec.rc = 0;
    exec.out = readDaemonLogRange(offset, maxBytes);
    return exec;
}

// `--health`, answered by the daemon itself.
ExecResult rpcHealth(ServerState& st, const std::string&, const std::vector<std::string>&) {
    ExecResult exec;
    long long cacheEntries = 0;
    std::string commandList;
    std::string lastEvent;
    daemonLog("DEBUG", "healthcheck via RPC");
    {
        std::lock_guard<std::mutex> lock(st.runtimeMutex);
        cacheEntries = static_cast<long long>(st.rpcCache.size());
        bool first = true;
        for (const std::string& c : st.rpcCommands) {
            if (!first) {
                commandList.push_back(',');
            }
            commandList += c;
            first = false;
        }
        lastEvent = st.zedLastEventUtc;
    }
    std::ostringstream hs;
    hs << "STATUS=OK\n";
    hs << "VERSION=" << ZFSMGR_AGENT_VERSION_STRING << "\n";
    hs << "API=" << kApiVersion << "\n";
    hs << "SERVER=1\n";
    hs << "CACHE_ENTRIES=" << cacheEntries << "\n";
    hs << "CACHE_MAX_ENTRIES=" << st.cacheMaxEntries << "\n";
    hs << "CACHE_INVALIDATIONS=" << st.cacheInvalidations.load() << "\n";
    hs << "POOL_INVALIDATIONS=" << st.poolInvalidations.load() << "\n";
    hs << "RPC_FAILURES=" << st.rpcFailures.load() << "\n";
    hs << "RPC_COMMANDS=" << commandList << "\n";
    hs << "ZED_ACTIVE=" << (st.zedActive.load() ? 1 : 0) << "\n";
    hs << "ZED_RESTARTS=" << st.zedRestarts.load() << "\n";
    if (!lastEvent.empty()) {
        hs << "ZED_LAST_EVENT_UTC=" << lastEvent << "\n";
    }
    hs << "JOBS_SUPPORT=1\n";
#ifdef __APPLE__
    // Si puede leer las particiones ZFS. Sin «Acceso total al disco» el agente
    // contesta, lista lo importado y no ve ni un pool por importar: el cliente
    // tiene que poder DECIRLO en el listado de la conexión, no dejar adivinar.
    {
        const std::string ilegible = discoIlegible();
        hs << "DISK_ACCESS=" << (ilegible.empty() ? "ok" : "denied") << "\n";
        if (!ilegible.empty()) {
            hs << "DISK_UNREADABLE=" << ilegible << "\n";
            if (permisosCambiaronTrasArranque()) {
                hs << "DISK_ACCESS_RESTART=1\n";
            }
        }
    }
#endif
    hs << "CAPS=" << agentCapabilityList() << "\n";
    exec.rc = 0;
    exec.out = hs.str();
    return exec;
}

// `--wait-for-event`, answered by the daemon itself.
ExecResult rpcWaitForEvent(ServerState& st, const std::string&,
                           const std::vector<std::string>& rpcArgs) {
    ExecResult exec;
    int timeoutSecs = 30;
    if (!rpcArgs.empty()) {
        try {
            timeoutSecs = std::stoi(rpcArgs[0]);
        } catch (...) {
        }
    }
    timeoutSecs = std::max(1, std::min(timeoutSecs, 120));
    const long long initialSeq = st.zedEventSeq.load();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSecs);
    while (std::chrono::steady_clock::now() < deadline && !g_stop.load()) {
        if (st.zedEventSeq.load() != initialSeq) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (st.zedEventSeq.load() != initialSeq) {
        std::string lastEvent;
        {
            std::lock_guard<std::mutex> lock(st.runtimeMutex);
            lastEvent = st.zedLastEventUtc;
        }
        std::ostringstream ws;
        ws << "EVENT_TYPE=zed\n";
        if (!lastEvent.empty()) {
            ws << "EVENT_AT=" << lastEvent << "\n";
        }
        exec.rc = 0;
        exec.out = ws.str();
    } else {
        exec.rc = 1;
        exec.out = "TIMEOUT=1\n";
    }
    // Los trabajos en segundo plano YA valen en Windows: se ejecutan con
    // std::thread, no con fork —eso siempre fue así—, y lo que de verdad los
    // ataba a Unix era el relé del emisor, /dev/urandom para el identificador y
    // la ruta del fichero de estado. Las tres cosas están resueltas.
    return exec;
}

// `--zfs-send-to-peer-async`, answered by the daemon itself.
// The snapshot of a send, or its incremental base, that does not exist; empty if both do.
// A resumed send has neither: its token says what it sends.
std::string instantaneaQueFalta(const std::vector<std::string>& rpcArgs) {
    const bool reanuda = rpcArgs.size() > 6 && !trim(rpcArgs[6]).empty();
    if (reanuda) {
        return {};
    }
    const std::string snap = rpcArgs.empty() ? std::string() : trim(rpcArgs[0]);
    for (std::size_t i : {std::size_t(0), std::size_t(4)}) {
        if (i >= rpcArgs.size() || trim(rpcArgs[i]).empty()) {
            continue;
        }
        std::string s = trim(rpcArgs[i]);
        // `zfs send -I` takes the base short («@base», «#bookmark») from the same dataset.
        if ((s.front() == '@' || s.front() == '#') && snap.find('@') != std::string::npos) {
            s = snap.substr(0, snap.find('@')) + s;
        }
        if (runExecCapture("zfs", {"list", "-H", "-o", "name", "-t", "snapshot,bookmark", s}).rc
            != 0) {
            return s;
        }
    }
    return {};
}

ExecResult rpcZfsSendToPeerAsync(ServerState&, const std::string&,
                                 const std::vector<std::string>& rpcArgs) {
    ExecResult exec;
    // args: snap peerHost peerPort token [baseSnap [sendFlags]]
    std::string malaBandera;
    if (rpcArgs.size() < 4) {
        exec.rc = 1;
        ponCodigo(exec, "usage",
                  {"--zfs-send-to-peer-async <snap> <peerHost> <peerPort> <token> [<baseSnap> "
                   "[<sendFlags>]]"});
        // Aquí se comprueba otra vez, aunque el envío ya lo haga: si no, la orden
        // se acepta, devuelve un identificador de trabajo y el trabajo muere al
        // instante. El error hay que darlo donde se pidió.
    } else if (rpcArgs.size() > 5
               && !zfsmgr::base::zfsprops::areValidSendFlags(rpcArgs[5], malaBandera)) {
        exec.rc = 2;
        ponCodigo(exec, "send_flag_not_allowed", {malaBandera});
    } else if (const std::string falta = instantaneaQueFalta(rpcArgs); !falta.empty()) {
        // Said here, where the send was asked for: as a job it would be accepted, connect to
        // the receiver and only then find out.
        exec.rc = 1;
        ponCodigo(exec, "snapshot_not_found", {falta});
    } else {
        DaemonJob job;
        job.id = generateJobId();
        job.type = "send-to-peer";
        job.snap = rpcArgs[0];
        job.peerHost = rpcArgs[1];
        try {
            job.peerPort = std::stoi(rpcArgs[2]);
        } catch (...) {
        }
        job.token = rpcArgs[3];
        job.baseSnap = (rpcArgs.size() > 4) ? rpcArgs[4] : "";
        job.sendFlags = (rpcArgs.size() > 5) ? rpcArgs[5] : "";
        job.resumeToken = (rpcArgs.size() > 6) ? rpcArgs[6] : "";
        job.description = "send " + (job.resumeToken.empty() ? job.snap : std::string("(resume)"))
                          + (job.baseSnap.empty() ? std::string() : " since " + job.baseSnap)
                          + " -> " + job.peerHost + ":" + std::to_string(job.peerPort);
        job.state = JobState::Running;
        job.startedAtUtc = utcNowIsoString();
        const std::string jobId = job.id;
        {
            std::lock_guard<std::mutex> jlock(g_jobsMutex);
            g_jobs[jobId] = job;
            persistJobsLocked();
        }
        daemonLog("INFO", "job " + jobId + " started type=send-to-peer snap=" + job.snap);
        std::thread([jobId]() { runZfsSendToPeerAsync(jobId); }).detach();
        exec.rc = 0;
        exec.out = "JOB_ID=" + jobId + "\nSTATE=running\n";
    }
    return exec;
}

// `--file-send-to-peer-async`, answered by the daemon itself.
ExecResult rpcFileSendToPeerAsync(ServerState&, const std::string&,
                                  const std::vector<std::string>& rpcArgs) {
    ExecResult exec;
    // args: fichero peerHost peerPort token
    //
    // El mismo trabajo que el de arriba, con un fichero de flujo en lugar de un
    // `zfs send`: comparte relé, avance, cancelación y la regla del envío vacío.
    std::string errFichero;
    SpawnedStdout prueba;
    if (rpcArgs.size() < 4) {
        exec.rc = 1;
        ponCodigo(exec, "usage",
                  {"--file-send-to-peer-async <file> <peerHost> <peerPort> <token>"});
        // Que el fichero exista y se pueda leer se dice AQUÍ: si no, la orden devuelve
        // un identificador de trabajo y el trabajo muere al instante.
    } else if (!(prueba = abreFicheroComoFlujo(rpcArgs[0], errFichero)).ok) {
        exec.rc = 1;
        exec.err = errFichero + "\n";
    } else {
        waitSpawnedStdout(prueba);
        DaemonJob job;
        job.id = generateJobId();
        job.type = "file-to-peer";
        job.snap = rpcArgs[0];
        job.peerHost = rpcArgs[1];
        try {
            job.peerPort = std::stoi(rpcArgs[2]);
        } catch (...) {
        }
        job.token = rpcArgs[3];
        job.description =
            "send file " + job.snap + " -> " + job.peerHost + ":" + std::to_string(job.peerPort);
        job.state = JobState::Running;
        job.startedAtUtc = utcNowIsoString();
        const std::string jobId = job.id;
        {
            std::lock_guard<std::mutex> jlock(g_jobsMutex);
            g_jobs[jobId] = job;
            persistJobsLocked();
        }
        daemonLog("INFO", "job " + jobId + " started type=file-to-peer file=" + job.snap);
        std::thread([jobId]() { runZfsSendToPeerAsync(jobId); }).detach();
        exec.rc = 0;
        exec.out = "JOB_ID=" + jobId + "\nSTATE=running\n";
    }
    return exec;
}

// `--job-submit`, answered by the daemon itself.
ExecResult rpcJobSubmit(ServerState&, const std::string& cmd,
                        const std::vector<std::string>& rpcArgs) {
    ExecResult exec;
    if (rpcArgs.empty() || !isAsyncSubmittableCommand(rpcArgs[0])) {
        exec.rc = 2;
        ponCodigo(exec, "usage", {"--job-submit <async-submittable-mutation> [args...]"});
    } else {
        DaemonJob job;
        job.id = generateJobId();
        job.type = "mutation";
        job.submittedCmd = rpcArgs[0];
        job.description =
            describeJob(rpcArgs[0], std::vector<std::string>(rpcArgs.begin() + 1, rpcArgs.end()));
        job.state = JobState::Running;
        job.startedAtUtc = utcNowIsoString();
        const std::string jobId = job.id;
        const std::string subCmd = rpcArgs[0];
        const std::vector<std::string> subParams(rpcArgs.begin() + 1, rpcArgs.end());
        {
            std::lock_guard<std::mutex> jlock(g_jobsMutex);
            g_jobs[jobId] = job;
            persistJobsLocked();
        }
        daemonLog("INFO", "job " + jobId + " started type=mutation cmd=" + subCmd);
        std::thread([jobId, subCmd, subParams]() {
            // Declarar el trabajo del hilo para que la operación pueda
            // informar de su avance con reportJobProgress().
            t_currentJobId = jobId;
            runSubmittedMutationJob(jobId, subCmd, subParams);
            t_currentJobId.clear();
        }).detach();
        exec.rc = 0;
        exec.out = "JOB_ID=" + jobId + "\nSTATE=running\n";
    }
    return exec;
}

// `--job-status`, answered by the daemon itself.
ExecResult rpcJobStatus(ServerState&, const std::string&, const std::vector<std::string>& rpcArgs) {
    ExecResult exec;
    if (rpcArgs.empty()) {
        exec.rc = 1;
        ponCodigo(exec, "usage", {"--job-status <jobId>"});
    } else {
        std::lock_guard<std::mutex> jlock(g_jobsMutex);
        auto it = g_jobs.find(rpcArgs[0]);
        if (it == g_jobs.end()) {
            exec.rc = 1;
            exec.out = "ERROR=job not found\n";
        } else {
            const DaemonJob& j = it->second;
            std::ostringstream js;
            js << "JOB_ID=" << j.id << "\n"
               << "STATE=" << jobStateName(j.state) << "\n"
               << "BYTES=" << j.bytesTransferred << "\n"
               << "RATE_MIB_S=" << j.rateMiBs << "\n"
               << "ELAPSED_SECS=" << j.elapsedSecs << "\n"
               << "STARTED_AT=" << j.startedAtUtc << "\n"
               << "FINISHED_AT=" << j.finishedAtUtc << "\n"
               << "ERROR_CODE=" << j.errorCode << "\n"
               << "ERROR_ARGS=" << argsJson(j.errorArgs) << "\n"
               << "DESCRIPTION=" << j.description << "\n";
            if (!j.progressLines.empty())
                js << "PROGRESS_LINE=" << j.progressLines.back() << "\n";
            if (j.type == "mutation") {
                js << "CMD=" << j.submittedCmd << "\n"
                   << "RC=" << j.resultRc << "\n";
                // The transport is line based, so fold the captured output
                // onto a single line rather than truncating it.
                std::string flat = j.resultOut;
                for (char& ch : flat) {
                    if (ch == '\n' || ch == '\r') {
                        ch = ' ';
                    }
                }
                js << "OUT=" << trim(flat) << "\n";
            }
            exec.rc = 0;
            exec.out = js.str();
        }
    }
    return exec;
}

// `--job-list`, answered by the daemon itself.
ExecResult rpcJobList(ServerState&, const std::string&, const std::vector<std::string>&) {
    ExecResult exec;
    std::lock_guard<std::mutex> jlock(g_jobsMutex);
    std::ostringstream jl;
    // Sort by startedAt descending
    std::vector<const DaemonJob*> sorted;
    sorted.reserve(g_jobs.size());
    for (const auto& kv : g_jobs)
        sorted.push_back(&kv.second);
    std::sort(sorted.begin(), sorted.end(), [](const DaemonJob* a, const DaemonJob* b) {
        return a->startedAtUtc > b->startedAtUtc;
    });
    int count = 0;
    for (const DaemonJob* j : sorted) {
        if (count >= 20)
            break;
        jl << "JOB={\"id\":\"" << jsonEscape(j->id) << "\",\"state\":\"" << jobStateName(j->state)
           << "\",\"type\":\"" << jsonEscape(j->type) << "\",\"snap\":\"" << jsonEscape(j->snap)
           << "\",\"bytes\":" << j->bytesTransferred << ",\"rate\":" << j->rateMiBs
           << ",\"elapsed\":" << j->elapsedSecs << ",\"started\":\"" << jsonEscape(j->startedAtUtc)
           << "\",\"finished\":\"" << jsonEscape(j->finishedAtUtc) << "\",\"error_code\":\""
           << jsonEscape(j->errorCode) << "\",\"error_args\":" << argsJson(j->errorArgs)
           << ",\"desc\":\"" << jsonEscape(j->description) << "\"}\n";
        ++count;
    }
    jl << "JOBS_COUNT=" << count << "\n";
    exec.rc = 0;
    exec.out = jl.str();
    return exec;
}

// `--job-cancel`, answered by the daemon itself.
ExecResult rpcJobCancel(ServerState&, const std::string&, const std::vector<std::string>& rpcArgs) {
    ExecResult exec;
    if (rpcArgs.empty()) {
        exec.rc = 1;
        ponCodigo(exec, "usage", {"--job-cancel <jobId>"});
    } else {
        std::lock_guard<std::mutex> jlock(g_jobsMutex);
        auto it = g_jobs.find(rpcArgs[0]);
        if (it == g_jobs.end()) {
            exec.rc = 1;
            exec.out = "ERROR=job not found\n";
        } else {
            DaemonJob& j = it->second;
            // Una nivelación del planificador puede estar EN COLA esperando turno:
            // cancelarla ahí es lo mismo, que no llegue a empezar.
            const bool enColaGsa = j.state == JobState::Queued && j.type == "gsa-level";
            if (j.state != JobState::Running && !enColaGsa) {
                exec.rc = 1;
                exec.out = "ERROR=job not running\n";
            } else {
#ifndef _WIN32
                // Marcar el estado basta para que el relé se detenga solo.
                // Esto es un empujón extra que solo existe en Unix.
                if (j.sendPid > 0) {
                    kill(j.sendPid, SIGTERM);
                }
#endif
                j.state = JobState::Cancelled;
                j.finishedAtUtc = utcNowIsoString();
                persistJobsLocked();
                daemonLog("INFO", "job " + j.id + " cancelled");
                exec.rc = 0;
                exec.out = "JOB_ID=" + j.id + "\nCANCELLED=1\n";
            }
        }
    }
    return exec;
}

// Any other verb: through the verb table, with the cache of reads in front (when it is
// enabled) and the cache invalidated after a mutation.
ExecResult runThroughCache(ServerState& st, const AgentRuntimeConfig& cfg, const std::string& cmd,
                           const std::vector<std::string>& rpcArgs) {
    const auto now = std::chrono::steady_clock::now();
    ExecResult exec;
    daemonLog("DEBUG", "rpc cmd=" + cmd);
    const bool dumpCmd = isDumpCommand(cmd);
    const bool mutateCmd = isMutateCommand(cmd);
    struct InvalidationProfile {
        std::vector<std::string> hints;
        std::set<std::string> dumpClasses;
    };
    const auto collectInvalidationProfile = [&](const std::string& mutateCmdName,
                                                const std::vector<std::string>& mutateArgs) {
        InvalidationProfile p;
        if ((mutateCmdName == "--mutate-zfs-snapshot" || mutateCmdName == "--mutate-zfs-destroy"
             || mutateCmdName == "--mutate-zfs-rollback")
            && !mutateArgs.empty()) {
            p.dumpClasses.insert("zfs");
            p.dumpClasses.insert("zpool");
            std::string target = mutateArgs[0];
            const std::size_t atPos = target.find('@');
            if (atPos != std::string::npos) {
                target = target.substr(0, atPos);
            }
            target = trim(target);
            if (!target.empty()) {
                p.hints.push_back(target);
                const std::size_t slashPos = target.find('/');
                if (slashPos != std::string::npos) {
                    const std::string pool = trim(target.substr(0, slashPos));
                    if (!pool.empty()) {
                        p.hints.push_back(pool);
                    }
                } else {
                    p.hints.push_back(target);
                }
            } else {
                p.hints.push_back("*");
            }
            return p;
        }
        // Para mutaciones genéricas preferimos limpieza global segura.
        p.hints.push_back("*");
        return p;
    };
    const auto cacheEntryMatchesHint = [&](const ServerState::CacheEntry& entry,
                                           const std::string& hintRaw) {
        const std::string hint = trim(hintRaw);
        if (hint.empty()) {
            return false;
        }
        if (hint == "*") {
            return true;
        }
        for (const std::string& aRaw : entry.args) {
            std::string a = trim(aRaw);
            if (a.empty()) {
                continue;
            }
            const std::size_t atPos = a.find('@');
            if (atPos != std::string::npos) {
                a = a.substr(0, atPos);
            }
            if (a == hint || startsWith(a, hint + "/") || startsWith(hint, a + "/")) {
                return true;
            }
        }
        return false;
    };
    const auto invalidateCacheByProfile = [&](const InvalidationProfile& profile) {
        if (profile.hints.empty()) {
            return;
        }
        const bool wipeAll = std::find(profile.hints.begin(), profile.hints.end(), std::string("*"))
                             != profile.hints.end();
        std::lock_guard<std::mutex> lock(st.runtimeMutex);
        ++st.cacheGen;
        if (wipeAll) {
            if (!st.rpcCache.empty()) {
                st.cacheInvalidations.fetch_add(1);
            }
            st.rpcCache.clear();
            return;
        }
        bool anyDropped = false;
        bool anyPoolHint = false;
        for (auto it = st.rpcCache.begin(); it != st.rpcCache.end();) {
            if (!profile.dumpClasses.empty()
                && profile.dumpClasses.count(it->second.dumpClass) == 0) {
                ++it;
                continue;
            }
            bool drop = false;
            for (const std::string& h : profile.hints) {
                if (h.find('/') == std::string::npos && h != "*") {
                    anyPoolHint = true;
                }
                if (cacheEntryMatchesHint(it->second, h)) {
                    drop = true;
                    break;
                }
            }
            if (drop) {
                anyDropped = true;
                it = st.rpcCache.erase(it);
            } else {
                ++it;
            }
        }
        if (anyDropped) {
            st.cacheInvalidations.fetch_add(1);
            if (anyPoolHint) {
                st.poolInvalidations.fetch_add(1);
            }
        }
    };
    std::vector<std::string> pedido{cmd};
    pedido.insert(pedido.end(), rpcArgs.begin(), rpcArgs.end());
    if (dumpCmd && cfg.cacheTtlFastMs > 0 && !zfsmgr::commands::requests::mustBeReadFresh(pedido)) {
        const std::string cacheKey = makeRpcCacheKey(cmd, rpcArgs);
        unsigned long long genAtMiss = 0;
        {
            std::lock_guard<std::mutex> lock(st.runtimeMutex);
            genAtMiss = st.cacheGen;
            const auto it = st.rpcCache.find(cacheKey);
            if (it != st.rpcCache.end()) {
                const auto ageMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.ts)
                        .count();
                if (ageMs <= cfg.cacheTtlFastMs) {
                    exec = it->second.result;
                } else {
                    st.rpcCache.erase(it);
                }
            }
        }
        if (exec.out.empty() && exec.err.empty() && exec.rc == 1) {
            exec = executeAgentCommandCapture(cmd, rpcArgs, "zfsmgr-agent");
            std::lock_guard<std::mutex> lock(st.runtimeMutex);
            if (exec.rc == 0 && st.cacheGen == genAtMiss) {
                if (cfg.cacheMaxEntries > 0
                    && static_cast<int>(st.rpcCache.size()) >= cfg.cacheMaxEntries) {
                    auto victim = st.rpcCache.begin();
                    for (auto it = st.rpcCache.begin(); it != st.rpcCache.end(); ++it) {
                        if (it->second.ts < victim->second.ts) {
                            victim = it;
                        }
                    }
                    if (victim != st.rpcCache.end()) {
                        st.rpcCache.erase(victim);
                    }
                }
                st.rpcCache[cacheKey] =
                    ServerState::CacheEntry{exec, now, cmd, rpcArgs, dumpClassForCommand(cmd)};
            }
        }
    } else {
        exec = executeAgentCommandCapture(cmd, rpcArgs, "zfsmgr-agent");
    }
    if (mutateCmd && exec.rc == 0) {
        invalidateCacheByProfile(collectInvalidationProfile(cmd, rpcArgs));
    }
    if (exec.rc != 0) {
        st.rpcFailures.fetch_add(1);
    }
    return exec;
}

// One RPC request, already read: what to answer. A few verbs are answered by the daemon
// itself (they need its state: health, jobs, the event wait); the rest go through the verb
// table, with the cache in front.
ExecResult handleRpcRequest(ServerState& st, const AgentRuntimeConfig& cfg,
                            const std::string& reqLine, bool gotLine) {
    std::string cmd;
    std::vector<std::string> rpcArgs;
    if (!gotLine || !parseJsonRpcRequest(reqLine, cmd, rpcArgs)) {
        ExecResult exec;
        exec.rc = 2;
        ponCodigo(exec, "internal", {"invalid rpc request"});
        st.rpcFailures.fetch_add(1);
        return exec;
    }
    {
        std::lock_guard<std::mutex> lock(st.runtimeMutex);
        st.rpcCommands.insert(cmd);
    }
    using DaemonVerb =
        ExecResult (*)(ServerState&, const std::string&, const std::vector<std::string>&);
    static const std::map<std::string, DaemonVerb> kDaemonVerbs = {
        {"--heartbeat", rpcHeartbeat},
        {"--dump-daemon-log", rpcDumpDaemonLog},
        {"--health", rpcHealth},
        {"--wait-for-event", rpcWaitForEvent},
        {"--zfs-send-to-peer-async", rpcZfsSendToPeerAsync},
        {"--file-send-to-peer-async", rpcFileSendToPeerAsync},
        {"--job-submit", rpcJobSubmit},
        {"--job-status", rpcJobStatus},
        {"--job-list", rpcJobList},
        {"--job-cancel", rpcJobCancel},
    };
    if (const auto it = kDaemonVerbs.find(cmd); it != kDaemonVerbs.end()) {
        return it->second(st, cmd, rpcArgs);
    }
    return runThroughCache(st, cfg, cmd, rpcArgs);
}

// Every read and write on a client socket gives up after this long. A connection that opens
// and says nothing must cost one thread for a while, not the daemon forever.
constexpr int kClientIoTimeoutMs = 15000;

void setClientTimeouts(NativeSock fd, int ms) {
#ifdef _WIN32
    const DWORD v = static_cast<DWORD>(ms);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof(v));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&v), sizeof(v));
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// Writes all of `datos`, or fails.
bool sslEscribeTodo(SSL* ssl, const char* datos, std::size_t len) {
    while (len > 0) {
        const int n = SSL_write(ssl, datos, static_cast<int>(std::min<std::size_t>(len, 1u << 20)));
        if (n <= 0) {
            return false;
        }
        datos += n;
        len -= static_cast<std::size_t>(n);
    }
    return true;
}

// `--zfs-send-stream <snap> <base> <flags> [<resume token>]`: `zfs send` written on THIS
// connection (see kTramoFlujoMax). Answers itself, so it never goes through the cache.
void serveZfsSendStream(SSL* ssl, NativeSock clientFd, const std::vector<std::string>& args) {
    const auto respondeError = [&](const char* codigo, const std::vector<std::string>& a) {
        std::string linea = "{\"rc\":1,\"stdout\":\"\",\"stderr\":\"\",\"code\":\""
                            + jsonEscape(codigo) + "\",\"args\":[";
        for (std::size_t i = 0; i < a.size(); ++i) {
            linea += (i ? ",\"" : "\"") + jsonEscape(a[i]) + "\"";
        }
        linea += "]}\n";
        (void)sslEscribeTodo(ssl, linea.data(), linea.size());
    };
    const std::string snap = args.size() > 0 ? trim(args[0]) : std::string();
    const std::string base = args.size() > 1 ? trim(args[1]) : std::string();
    const std::string flags = args.size() > 2 ? trim(args[2]) : std::string();
    const std::string resume = args.size() > 3 ? trim(args[3]) : std::string();
    std::string malaBandera;
    if (snap.empty() && resume.empty()) {
        respondeError("usage", {"--zfs-send-stream <snap> <base> <flags> [<resume token>]"});
        return;
    }
    if (!zfsmgr::base::zfsprops::areValidSendFlags(flags, malaBandera)) {
        respondeError("send_flag_not_allowed", {malaBandera});
        return;
    }
    // The same check as --zfs-send-to-peer-async, which takes snap at 0, base at 4, token at 6.
    if (const std::string falta = instantaneaQueFalta({snap, "", "", "", base, flags, resume});
        !falta.empty()) {
        respondeError("snapshot_not_found", {falta});
        return;
    }
    SpawnedStdout hijo = spawnReadingStdout("zfs", argvDeZfsSend(snap, base, flags, resume));
    if (!hijo.ok) {
        respondeError("send_cannot_launch", {});
        return;
    }
    // From here on the connection carries data: a slow receiver must not cut it after the
    // 15 s that a request line gets.
    setClientTimeouts(clientFd, 300000);
    const std::string cabecera = "{\"rc\":0,\"stream\":1}\n";
    bool escribe = sslEscribeTodo(ssl, cabecera.data(), cabecera.size());
    std::vector<char> buf(kTramoFlujoMax + 4);
    std::uint64_t total = 0;
    while (escribe) {
        const long n = readSpawnedStdout(hijo, buf.data() + 4, kTramoFlujoMax);
        if (n <= 0) {
            break;
        }
        const auto len = static_cast<std::uint32_t>(n);
        buf[0] = static_cast<char>((len >> 24) & 0xFF);
        buf[1] = static_cast<char>((len >> 16) & 0xFF);
        buf[2] = static_cast<char>((len >> 8) & 0xFF);
        buf[3] = static_cast<char>(len & 0xFF);
        escribe = sslEscribeTodo(ssl, buf.data(), static_cast<std::size_t>(n) + 4);
        total += static_cast<std::uint64_t>(n);
    }
    if (!escribe) {
        // The puller went away: nobody reads, so the send is stopped.
        matarSpawnedStdout(hijo);
    }
    std::string errSend;
    const int rc = waitSpawnedStdout(hijo, &errSend);
    if (escribe) {
        const char fin[4] = {0, 0, 0, 0};
        const std::string cola = "{\"rc\":" + std::to_string(rc) + ",\"stderr\":\""
                                 + jsonEscape(trim(errSend)) + "\"}\n";
        (void)(sslEscribeTodo(ssl, fin, 4) && sslEscribeTodo(ssl, cola.data(), cola.size()));
    }
    daemonLog(rc == 0 && escribe ? "INFO" : "WARN",
              "send-stream " + (snap.empty() ? std::string("(resume)") : snap)
                  + (base.empty() ? std::string() : " since " + base) + ": " + std::to_string(total)
                  + " bytes, rc=" + std::to_string(rc)
                  + (escribe ? std::string() : std::string(", the puller went away")));
}

// One client connection: TLS handshake, the request line, the answer.
void serveConnection(SSL_CTX* ctx, NativeSock clientFd, ServerState& st,
                     const AgentRuntimeConfig& cfg) {
    setClientTimeouts(clientFd, kClientIoTimeoutMs);
    SSL* ssl = SSL_new(ctx);
    if (!ssl) {
        closeSock(clientFd);
        return;
    }
    SSL_set_fd(ssl, static_cast<int>(clientFd));
    if (SSL_accept(ssl) != 1) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        closeSock(clientFd);
        return;
    }

    std::string reqLine;
    reqLine.reserve(1024);
    char buf[2048];
    bool gotLine = false;
    while (reqLine.size() < 1024 * 1024) {
        const int n = SSL_read(ssl, buf, static_cast<int>(sizeof(buf)));
        if (n <= 0) {
            break;
        }
        reqLine.append(buf, static_cast<std::size_t>(n));
        const std::size_t nl = reqLine.find('\n');
        if (nl != std::string::npos) {
            reqLine = reqLine.substr(0, nl);
            gotLine = true;
            break;
        }
    }

    // A stream is answered on this same connection, not with one JSON line.
    {
        std::string cmd;
        std::vector<std::string> rpcArgs;
        if (gotLine && parseJsonRpcRequest(reqLine, cmd, rpcArgs) && cmd == "--zfs-send-stream") {
            {
                std::lock_guard<std::mutex> lock(st.runtimeMutex);
                st.rpcCommands.insert(cmd);
            }
            serveZfsSendStream(ssl, clientFd, rpcArgs);
            SSL_shutdown(ssl);
            SSL_free(ssl);
            closeSock(clientFd);
            return;
        }
    }
    ExecResult exec = handleRpcRequest(st, cfg, reqLine, gotLine);

    // Un fallo del propio agente va como "code" y "args" EN VEZ de la frase: stderr solo
    // lleva lo que no es suyo (la salida de `zfs` sin código alrededor).
    std::string codigo;
    if (!exec.code.empty()) {
        exec.err.clear();
        codigo = ",\"code\":\"" + jsonEscape(exec.code) + "\",\"args\":[";
        for (std::size_t i = 0; i < exec.args.size(); ++i) {
            codigo += (i ? ",\"" : "\"") + jsonEscape(exec.args[i]) + "\"";
        }
        codigo += "]";
    }
    const std::string response = std::string("{\"rc\":") + std::to_string(exec.rc)
                                 + ",\"stdout\":\"" + jsonEscape(exec.out) + "\"" + ",\"stderr\":\""
                                 + jsonEscape(exec.err) + "\"" + codigo + "}\n";
    (void)SSL_write(ssl, response.data(), static_cast<int>(response.size()));
    SSL_shutdown(ssl);
    SSL_free(ssl);
    closeSock(clientFd);
}

// The server TLS context: TLS 1.2+, our certificate, and client certificates required and
// checked against the client CA. Null if any piece is missing.
SSL_CTX* createServerTlsContext(const AgentRuntimeConfig& cfg) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        return nullptr;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_file(ctx, cfg.tlsCert.c_str(), SSL_FILETYPE_PEM) != 1
        || SSL_CTX_use_PrivateKey_file(ctx, cfg.tlsKey.c_str(), SSL_FILETYPE_PEM) != 1
        || SSL_CTX_check_private_key(ctx) != 1
        || SSL_CTX_load_verify_locations(ctx, cfg.tlsClientCert.c_str(), nullptr) != 1) {
        SSL_CTX_free(ctx);
        return nullptr;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
    return ctx;
}

// The listening socket on the configured address and port, kept out of child processes.
// kInvalidSock if it cannot be opened.
NativeSock openListener(const std::string& bindHost, const std::string& bindPort) {
    NativeSock listenFd = kInvalidSock;
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(bindHost.c_str(), bindPort.c_str(), &hints, &res) != 0) {
        return kInvalidSock;
    }
    for (struct addrinfo* it = res; it; it = it->ai_next) {
        const NativeSock fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == kInvalidSock)
            continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
        if (bind(fd, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen)) == 0
            && listen(fd, 32) == 0) {
            listenFd = fd;
#ifndef _WIN32
            // Keep the listening socket out of child processes: the ZED watcher runs
            // `zpool events` through popen(), which would otherwise inherit this fd.
            // A surviving child then keeps the port bound after the daemon exits, and
            // the next start fails to bind for no visible reason.
            const int fdFlags = fcntl(fd, F_GETFD, 0);
            if (fdFlags >= 0) {
                fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC);
            }
#endif
            break;
        }
        closeSock(fd);
    }
    freeaddrinfo(res);
    return listenFd;
}

// How many clients are served at once; one more is refused until a thread finishes.
constexpr int kMaxConnections = 32;

int runServeLoop() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return 2;
#endif
    const AgentRuntimeConfig cfg = loadRuntimeConfig();
    applyRuntimeEnvironment(cfg);
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    SSL_CTX* ctx = createServerTlsContext(cfg);
    const std::string bindHost = cfg.bind.empty() ? std::string(kDefaultBind) : cfg.bind;
    const std::string bindPort = std::to_string(cfg.port > 0 ? cfg.port : kDefaultPort);
    const NativeSock listenFd = ctx ? openListener(bindHost, bindPort) : kInvalidSock;
    if (listenFd == kInvalidSock) {
        if (ctx) {
            SSL_CTX_free(ctx);
        }
#ifdef _WIN32
        WSACleanup();
#endif
        return 2;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#ifndef _WIN32
    std::signal(SIGHUP, onSignal);
#endif
    loadPersistedJobsAtStartup();
    daemonLog("INFO", std::string("daemon started version=") + ZFSMGR_AGENT_VERSION_STRING
                          + " api=" + kApiVersion + " bind=" + bindHost + ":" + bindPort);
    auto lastHeartbeat = std::chrono::steady_clock::now() - std::chrono::minutes(2);
    ServerState st;
    st.cacheMaxEntries = cfg.cacheMaxEntries;
    std::thread zedThread([&st]() { watchZedEvents(st); });

    std::thread gsaThread(runGsaSchedulerThread);

    while (!g_stop.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastHeartbeat >= std::chrono::seconds(60)) {
            writeHeartbeat();
            lastHeartbeat = now;
        }
        if (st.zedInvalidateRequested.exchange(false)) {
            std::lock_guard<std::mutex> lock(st.runtimeMutex);
            ++st.cacheGen;
            if (!st.rpcCache.empty()) {
                st.cacheInvalidations.fetch_add(1);
            }
            st.rpcCache.clear();
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listenFd, &rfds);
        struct timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        const int sel = select(static_cast<int>(listenFd + 1), &rfds, nullptr, nullptr, &tv);
        if (sel <= 0 || !FD_ISSET(listenFd, &rfds)) {
            continue;
        }
        const NativeSock clientFd = accept(listenFd, nullptr, nullptr);
        if (clientFd == kInvalidSock) {
            continue;
        }

        // Each connection on its own thread: one slow verb, or a `--wait-for-event`, must not
        // keep every other client waiting. The cap bounds what a flood of connections costs.
        if (st.activeConnections.load() >= kMaxConnections) {
            daemonLog("WARN", "too many connections; one refused");
            closeSock(clientFd);
            continue;
        }
        st.activeConnections.fetch_add(1);
        std::thread([ctx, clientFd, &st, &cfg]() {
            serveConnection(ctx, clientFd, st, cfg);
            st.activeConnections.fetch_sub(1);
        }).detach();
    }
    // The connection threads use `st`, `cfg` and `ctx`. They notice g_stop or time out; if
    // one still runs after this wait, the context is left alone: the process is exiting.
    for (int i = 0; i < 100 && st.activeConnections.load() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const bool connectionsDone = st.activeConnections.load() == 0;
#ifndef _WIN32
    // Sin esto el join no vuelve nunca: el hilo está en un fgets sobre `zpool events -f`.
    if (const pid_t g = g_zedGrupo.load(); g > 0) {
        kill(-g, SIGTERM);
    }
#endif
    if (zedThread.joinable()) {
        zedThread.join();
    }
    if (gsaThread.joinable()) {
        gsaThread.join();
    }
    closeSock(listenFd);
    if (connectionsDone) {
        SSL_CTX_free(ctx);
    }
    EVP_cleanup();
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

void printUsage(const char* argv0) {
    std::cerr << "usage: " << argv0
              << " [--version|--api-version|--serve|--health|--dump-*|--mutate-*]\n";
}

}  // namespace zfsmgr::agent
