// The agent's entry point: the command-line door (one-shot verbs, `--serve`, `--version`…).
// Every verb that is not specific to the command line goes through the same table as the RPC
// (see verbs.cpp).

#include "agent_internal.h"

using namespace zfsmgr::agent;

int main(int argc, char* argv[]) {
    // Antes que nada: escribir en un socket ya cerrado no puede matar el proceso.
    // Ver zfsmgr::base::ignoreSigpipeExceptInChildren.
    zfsmgr::base::ignoreSigpipeExceptInChildren();
    // El agente escribe en INGLÉS: sus mensajes van a clientes de cualquier idioma, que los
    // traducen por su código (agentmsg). Sin esto, lo que compone la capa base —el cliente TLS
    // con el que habla con otro daemon— salía en castellano, que es su idioma por omisión.
    zfsmgr::base::i18n::setLanguage("en");
    const AgentRuntimeConfig runtimeCfg = loadRuntimeConfig();
    applyRuntimeEnvironment(runtimeCfg);

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i] ? argv[i] : "");
    }

    const std::string cmd = (args.size() > 1) ? args[1] : std::string("--serve");

    if (cmd == "--version" || cmd == "version") {
        std::cout << ZFSMGR_AGENT_VERSION_STRING << '\n';
        return 0;
    }
    if (cmd == "--api-version" || cmd == "api") {
        std::cout << kApiVersion << '\n';
        return 0;
    }
    if (cmd == "--serve" || cmd == "serve") {
        return runServeLoop();
    }
    if (cmd == "--once") {
        writeHeartbeat();
        return 0;
    }
    if (cmd == "--health") {
        // This is a direct CLI invocation — not a live query to a running daemon.
        // Real ZED/cache state is only available via RPC when --serve is running.
        std::cout << "STATUS=OK\n";
        std::cout << "VERSION=" << ZFSMGR_AGENT_VERSION_STRING << "\n";
        std::cout << "API=" << kApiVersion << "\n";
        std::cout << "SERVER=0\n";
        std::cout << "CACHE_ENTRIES=0\n";
        std::cout << "CACHE_MAX_ENTRIES=0\n";
        std::cout << "CACHE_INVALIDATIONS=0\n";
        std::cout << "POOL_INVALIDATIONS=0\n";
        std::cout << "RPC_FAILURES=0\n";
        std::cout << "RPC_COMMANDS=\n";
        std::cout << "ZED_ACTIVE=0\n";
        std::cout << "JOBS_SUPPORT=1\n";
        std::cout << "CAPS=" << agentCapabilityList() << "\n";
        return 0;
    }

    // Solo CLI a propósito: es lo que se ejecuta ANTES de que exista el canal TLS,
    // así que servirlo por RPC no tendría sentido.
    if (cmd == "--ensure-tls") {
        const ExecResult e = runEnsureTlsCapture();
        if (!e.out.empty()) {
            std::cout << e.out;
        }
        if (!e.err.empty()) {
            std::cerr << e.err;
        }
        return e.rc;
    }
    if (cmd == "--dump-daemon-log") {
        long long offset = 0;
        long long maxBytes = 0;
        if (args.size() > 2) {
            try {
                offset = std::stoll(args[2]);
            } catch (...) {
            }
        }
        if (args.size() > 3) {
            try {
                maxBytes = std::stoll(args[3]);
            } catch (...) {
            }
        }
        std::cout << readDaemonLogRange(offset, maxBytes);
        return 0;
    }
    if (cmd == "--heartbeat") {
        const std::string ts = utcNowIsoString();
        daemonLog("INFO", "heartbeat request (cli)");
        std::cout << "ALIVE=yes\n";
        std::cout << "VERSION=" << ZFSMGR_AGENT_VERSION_STRING << "\n";
        std::cout << "TS=" << ts << "\n";
        return 0;
    }

#ifndef _WIN32
    if (cmd == "--mutate-sync-temp-tar-source") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runSyncTempTarSource(std::vector<std::string>(args.begin() + 2, args.end()));
    }
    if (cmd == "--mutate-sync-temp-tar-dest") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runSyncTempTarDest(std::vector<std::string>(args.begin() + 2, args.end()));
    }
    if (cmd == "--mutate-advanced-fromdir") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runMutateAdvancedFromDir(std::vector<std::string>(args.begin() + 2, args.end()));
    }
#endif
    if (cmd == "--mutate-shell-generic") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        return runMutateShellGeneric(args[2]);
    }

    if (cmd == "--tree-recv-listen") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        // En la rama de terminal se espera EN LÍNEA: quien la usa a mano quiere ver el
        // resultado, no que el proceso se muera dejando un hilo suelto detrás.
        const ExecResult e = runTreeRecvListenCapture(args[2], /*waitInline=*/true);
        if (!e.err.empty()) {
            std::cerr << e.err;
        }
        return e.rc;
    }
    // `--zfs-pull-stream <peer> <snap> <base> <flags> <dataset>`: pull a copy from a peer
    // (see tiraFlujoDelPar). The schedule uses the function itself; this door is for testing
    // it by hand, as root, before a schedule exists.
    if (cmd == "--zfs-pull-stream") {
        if (args.size() < 7) {
            std::cerr << "usage: " << args[0]
                      << " --zfs-pull-stream <peer> <snap> <base> <flags> <dataset>\n";
            return 2;
        }
        GsaPeer par;
        if (!gsaCargaPeer(args[2], par)) {
            std::cerr << "no credentials for peer «" << args[2] << "» in the peers file\n";
            return 1;
        }
        std::vector<std::string> recv = {"recv", "-u", "-s"};
        // Incremental: -F, which WITHOUT -R only rolls back what changed after the last
        // snapshot (a mount, an atime); it does not destroy older snapshots.
        if (!trim(args[4]).empty()) {
            recv.push_back("-F");
        }
        for (const std::string& prop : zfsmgr::base::gsa::scheduleProperties()) {
            recv.insert(recv.end(), {"-x", prop});
        }
        recv.push_back(args[6]);
        const ExecResult e = tiraFlujoDelPar(par, {args[3], args[4], args[5]}, recv,
                                             [](std::uint64_t, long, double) { return true; });
        std::cout << e.out;
        if (e.rc != 0) {
            std::cerr << zfsmgr::base::agentmsg::inEnglish(e.code, e.args) << "\n";
        }
        return e.rc;
    }
    if (cmd == "--zfs-recv-listen") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        const bool force = args.size() > 3 && (args[3] == "1" || toLower(args[3]) == "true");
        const ExecResult e = runZfsRecvListenCapture(args[2], force, /*waitInline=*/true);
        if (!e.err.empty())
            std::cerr << e.err;
        return e.rc;
    }
    if (cmd == "--file-recv-listen") {
        if (args.size() < 3) {
            printUsage(args[0].c_str());
            return 2;
        }
        const bool sobrescribir = args.size() > 3 && (args[3] == "1" || toLower(args[3]) == "true");
        const ExecResult e = runFileRecvListenCapture(args[2], sobrescribir, /*waitInline=*/true,
                                                      args.size() > 4 ? args[4] : std::string());
        if (!e.err.empty())
            std::cerr << e.err;
        return e.rc;
    }

    // Todo lo demás, por la MISMA tabla que el RPC.
    if (const Verbo* v = tablaDeVerbos().busca(cmd)) {
        if (v->soloRpc) {
            std::cerr << cmd
                      << " carries a secret and is only served over the daemon's mTLS "
                         "channel, never on the command line\n";
            return 2;
        }
        const std::vector<std::string> params(args.begin() + 2, args.end());
        const ExecResult e = v->haz(Peticion{cmd, params, args[0].c_str()});
        if (!e.out.empty()) {
            std::cout << e.out;
        }
        if (!e.err.empty()) {
            std::cerr << e.err;
        }
        return e.rc;
    }
    printUsage(args[0].c_str());
    return 2;
}
