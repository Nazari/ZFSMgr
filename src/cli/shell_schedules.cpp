// Scheduled snapshots: `schedules`, `schedule` (status, run and settings), the
// start-up check of every schedule and the status area monitor.

#include "shell_internal.h"

namespace zfsmgr::cli::detail {

bool cmdSchedules(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    std::vector<std::pair<std::string, B::gsa::Entry>> filas;
    std::vector<std::string> maquinas;
    // `-a`, como en `jobs -a` e `install-daemon -a`: todas las conexiones, desde cualquier
    // sitio. Sin él, la conexión en la que se está.
    const std::string actual = subeHasta(pet.objetivo, Objetivo::Conexion).connection;
    if (!pet.tiene("-a") && actual.empty()) {
        std::fputs(TC("t_schs_falta_conexion",
                      "hace falta estar en una conexión, o «schedules -a» para todas\n"),
                   stderr);
        return false;
    }
    if (pet.tiene("-a")) {
        for (const auto& p : e.conns.perfiles) {
            const std::string id = p.id.empty() ? p.name : p.id;
            if (!e.conns.desconectada(id)) {
                maquinas.push_back(id);
            }
        }
    } else {
        maquinas.push_back(actual);
    }
    for (const std::string& id : maquinas) {
        ZfsmUrl u;
        u.kind = ZfsmKind::Connection;
        u.connection = id;
        std::string listaPools;
        if (!agente(e, u, PET::poolList(), listaPools, 20000)) {
            continue;  // con --all, una máquina que no contesta no invalida a las demás
        }
        B::json::Value raiz;
        std::string err;
        if (B::trim(listaPools).empty() || !B::json::parse(listaPools, raiz, &err)) {
            continue;
        }
        for (const auto& kv : raiz["pools"].toObject()) {
            std::vector<B::gsa::Entry> deEstePool;
            if (!leeProgramaciones(e, u, kv.first, deEstePool)) {
                continue;
            }
            for (const B::gsa::Entry& en : deEstePool) {
                filas.push_back({id, en});
            }
        }
    }
    tablaDeProgramaciones(filas).imprime(e.formato);
    return true;
}

// ── El estado de las programaciones ──────────────────────────────────────────────────
//
// Lo que pasó en la última pasada del planificador de cada máquina, según su daemon
// (`--dump-gsa-status`). Lo usan `schedule status` y la comprobación del arranque, con la
// MISMA lectura y las mismas frases, para que las dos no puedan contar cosas distintas.

// La de una conexión. No escribe nada: si no se pudo preguntar, lo dice en `whyNotChecked`.
B::gsa::ConnectionState estadoDeProgramaciones(Sesion& ses, const B::ConnectionProfile& p,
                                               int timeoutMs) {
    B::gsa::ConnectionState c;
    c.connection = p.id.empty() ? p.name : p.id;
    std::string out;
    std::string err;
    int rc = -1;
    std::string motivo;
    if (!ejecutarAgente(ses, p, PET::gsaStatus(), out, err, rc, &motivo, timeoutMs)) {
        c.whyNotChecked = motivo.empty() ? T("t_sch_no_contesta", "no contestó") : motivo;
        return c;
    }
    if (rc != 0) {
        const std::string detalle = B::trim(err).empty() ? B::trim(out) : B::trim(err);
        c.whyNotChecked = detalle;
        return c;
    }
    std::string errJson;
    if (!B::gsa::parseState(out, c, errJson)) {
        c.checked = false;
        c.whyNotChecked = T("t_sch_estado_ilegible", "estado ilegible: ") + errJson;
    }
    return c;
}

std::string horaLocal(long long epoch) {
    if (epoch <= 0) {
        return "-";
    }
    const std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32] = {0};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

// La frase de una programación en rojo: dónde, qué paso, cuándo y por qué.
std::string motivoDeFallo(const std::string& conexion, const B::gsa::DatasetState& d,
                          long long ultimaPasada) {
    const std::string url = "zfsm://" + conexion + "/" + d.dataset;
    if (d.failedStep == "scheduler") {
        return B::format(T("t_sch_mal_hilo",
                           "%1 — el planificador no pasa desde %2: ¿está "
                           "funcionando el daemon?"),
                         {url, horaLocal(ultimaPasada)});
    }
    std::string que;
    if (d.failedStep == "level") {
        que = B::format(T("t_sch_mal_nivel", "la nivelación a %1"), {d.destination});
    } else if (d.failedStep == "snapshot") {
        que = T("t_sch_mal_snap", "el snapshot");
    } else if (d.failedStep == "prune") {
        que = T("t_sch_mal_poda", "la poda");
    } else {
        que = B::format(T("t_sch_mal_poda_dst", "la poda de %1"), {d.destination});
    }
    return B::format(T("t_sch_mal_fila", "%1 — %2 falló (%3): %4"),
                     {url, que, horaLocal(d.errorTime), d.error});
}

// `schedule status [-a | <conexión>]`.
//
// No pasa por `prepara`: `schedule` actúa sobre un dataset, y el estado se pide desde
// cualquier sitio, también desde la raíz. Por eso se comprueban aquí sus argumentos, con el
// mismo criterio: lo que sobra es un error.
bool cmdScheduleStatus(Estado& e, const LineaAnalizada& linea) {
    const std::vector<std::string> palabras = linea.lista("texto");
    for (const std::string& b : linea.banderas) {
        if (b != "-a") {
            std::fprintf(stderr,
                         TC("t_sch_status_bandera",
                            "«%s» no es una bandera de «schedule "
                            "status»\n"),
                         b.c_str());
            return false;
        }
    }
    if (!linea.opciones.empty() || !linea.objetivo.empty() || palabras.size() > 2) {
        std::fputs(TC("t_sch_status_uso", "uso: schedule status [-a | <conexión>]\n"), stderr);
        return false;
    }
    const bool todas = linea.tiene("-a");
    std::vector<const B::ConnectionProfile*> maquinas;
    if (todas) {
        if (palabras.size() > 1) {
            std::fputs(TC("t_sch_status_uso", "uso: schedule status [-a | <conexión>]\n"), stderr);
            return false;
        }
        for (const auto& p : e.conns.perfiles) {
            const std::string id = p.id.empty() ? p.name : p.id;
            if (!e.conns.desconectada(id)) {
                maquinas.push_back(&p);
            }
        }
    } else {
        const std::string nombre =
            palabras.size() > 1 ? palabras[1] : subeHasta(e.actual, Objetivo::Conexion).connection;
        if (nombre.empty()) {
            std::fputs(TC("t_sch_status_falta",
                          "hace falta estar en una conexión, nombrarla o "
                          "«schedule status -a» para todas\n"),
                       stderr);
            return false;
        }
        const auto* p = buscarConexion(e.conns, nombre);
        if (!p) {
            std::fprintf(
                stderr, "%s\n",
                B::format(T("t_no_conn_nombre", "no hay ninguna conexión llamada «%1»"), {nombre})
                    .c_str());
            return false;
        }
        maquinas.push_back(p);
    }

    Tabla t;
    t.nombreJson = "schedule_status";
    t.cabecerasTexto = {T("t_cab_conexion", "CONEXIÓN"),        T("t_cab_dataset", "DATASET"),
                        T("t_cab_ult_snap", "ÚLTIMO SNAPSHOT"), T("t_cab_nivelacion", "NIVELACIÓN"),
                        T("t_cab_destino", "DESTINO"),          T("t_cab_estado", "ESTADO")};
    t.campos = {"connection", "dataset", "last_snapshot", "level", "destination", "state"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    std::vector<std::string> motivos;
    bool algunFallo = false;
    for (const auto* p : maquinas) {
        const B::gsa::ConnectionState c = estadoDeProgramaciones(*e.ses, *p, 20000);
        if (!c.checked) {
            motivos.push_back(B::format(T("t_sch_sin_comprobar", "%1: no se pudo comprobar: %2"),
                                        {c.connection, c.whyNotChecked}));
            algunFallo = true;
            continue;
        }
        for (const B::gsa::DatasetState& d : c.datasets) {
            std::string nivel = "-";
            if (!d.levelJob.empty()) {
                nivel =
                    d.levelJobState == "queued"
                        ? B::format(T("t_sch_nivel_cola", "en cola (job %1)"), {d.levelJob})
                        : B::format(T("t_sch_nivel_curso", "en curso %1 (job %2)"),
                                    {B::humanSize(std::to_string(d.levelJobBytes)), d.levelJob});
            } else if (d.level) {
                nivel = !d.hasLevel ? T("t_sch_nivel_nunca", "aún no")
                        : d.levelOk ? T("t_sch_nivel_ok", "ok") + std::string(" ")
                                          + horaLocal(d.lastLevelTime)
                                    : T("t_sch_nivel_mal", "error");
            }
            std::string estado = T("t_sch_verde", "verde");
            if (c.stale || !d.failedStep.empty()) {
                estado = T("t_sch_rojo", "rojo");
                B::gsa::DatasetState x = d;
                if (c.stale) {
                    x.failedStep = "scheduler";
                }
                motivos.push_back(motivoDeFallo(c.connection, x, c.lastPass));
                algunFallo = true;
            } else if (d.pending) {
                estado = T("t_sch_pendiente", "pendiente");
            }
            t.filas.push_back({c.connection, d.dataset,
                               d.lastSnapshot.empty() ? "-" : d.lastSnapshot, nivel,
                               d.destination.empty() ? "-" : d.destination, estado});
        }
    }
    if (t.filas.empty() && motivos.empty()) {
        std::fputs(TC("t_sch_ninguna_prog", "No hay programaciones\n"), stderr);
        return true;
    }
    if (!t.filas.empty()) {
        t.imprime(e.formato);
        // La tabla va por la salida estándar y los motivos por la de error: sin vaciarla,
        // en una tubería los motivos salían ANTES que la tabla que explican.
        std::fflush(stdout);
    }
    for (const std::string& m : motivos) {
        std::fprintf(stderr, "%s\n", m.c_str());
    }
    e.ultimoRc = algunFallo ? 1 : 0;
    return !algunFallo;
}

// El error de un trabajo en el idioma de quien lo lee: su código y sus argumentos (agentmsg).
std::string errorDeTrabajo(const std::string& codigo, const std::string& argsJson) {
    if (codigo.empty()) {
        return {};
    }
    std::vector<std::string> args;
    B::json::Value v;
    std::string err;
    if (B::json::parse(argsJson.empty() ? "[]" : argsJson, v, &err)) {
        for (const auto& a : v.toArray())
            args.push_back(a.toString());
    }
    return B::agentmsg::render(codigo, args);
}

std::string errorDeTrabajo(const std::map<std::string, std::string>& k) {
    const auto de = [&](const char* c) { return k.count(c) ? k.at(c) : std::string(); };
    return errorDeTrabajo(de("ERROR_CODE"), de("ERROR_ARGS"));
}

// `schedule run`: una pasada del planificador para ESTE dataset, ahora. Lo que el daemon
// registró se enseña aquí mismo. La nivelación, si toca, sale como TRABAJO: sin `--wait` se
// dice cuál y se vuelve, como `send`; con `--wait` se sigue hasta que acaba.
bool cmdScheduleRun(Estado& e, const ZfsmUrl& destino, bool esperar) {
    std::string out;
    std::string errorDeOrden;
    const bool ok =
        agente(e, destino, PET::gsaRunOnce(destino.dataset), out, 600000, &errorDeOrden);
    std::string trabajo;
    for (const std::string& l : B::split(out, "\n", true)) {
        if (B::startsWith(l, "JOB_ID=")) {
            trabajo = B::trim(l.substr(7));
            continue;
        }
        if (B::startsWith(l, "ALREADY_RUNNING=")) {
            continue;
        }
        std::fprintf(stderr, "%s\n", l.c_str());
    }
    if (!ok) {
        if (!errorDeOrden.empty()) {
            std::fprintf(stderr, "%s\n", errorDeOrden.c_str());
        }
        return false;
    }
    if (trabajo.empty()) {
        return true;
    }
    ZfsmUrl maquina;
    maquina.kind = ZfsmKind::Connection;
    maquina.connection = destino.connection;
    std::fprintf(stderr, TC("t_sch_run_trabajo", "nivelación en marcha como trabajo %s en %s\n"),
                 trabajo.c_str(), destino.connection.c_str());
    if (!esperar) {
        std::fprintf(stderr, TC("t_siga_con_j_2debd4", "siga con «job %s --on %s»\n"),
                     trabajo.c_str(), textoDe(maquina).c_str());
        return true;
    }
    // Cada dos segundos, que es el ritmo al que el daemon actualiza el avance. «queued» sigue
    // siendo esperar: la nivelación aguarda turno.
    std::string ultimo;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::string est;
        if (!agente(e, maquina, PET::jobStatus(trabajo), est, 20000)) {
            return false;
        }
        const auto k = clavesDe(est);
        const std::string estado = k.count("STATE") ? k.at("STATE") : std::string();
        const std::string linea = k.count("PROGRESS_LINE") ? k.at("PROGRESS_LINE") : std::string();
        if (!linea.empty() && linea != ultimo) {
            ultimo = linea;
            std::fprintf(stderr, "  %s\n", linea.c_str());
        }
        if (estado == "running" || estado == "queued") {
            continue;
        }
        const std::string err = errorDeTrabajo(k);
        std::fprintf(stderr, TC("t_trabajo_estado_fin", "trabajo %s: %s%s\n"), trabajo.c_str(),
                     estado.c_str(), err.empty() ? "" : (" — " + err).c_str());
        e.ultimoRc = (estado == "done" || estado == "finished") ? 0 : 1;
        return e.ultimoRc == 0;
    }
}

bool cmdSchedule(Estado& e, const LineaAnalizada& linea) {
    const std::vector<std::string> palabras = linea.lista("texto");
    if (!palabras.empty() && B::toLowerAscii(palabras[0]) == "status") {
        return cmdScheduleStatus(e, linea);
    }
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (!exigeDataset(destino)) {
        return false;
    }
    const bool ejecutar = !palabras.empty() && B::toLowerAscii(palabras[0]) == "run";
    if ((!palabras.empty() && !ejecutar) || palabras.size() > 1 || pet.tiene("-a")
        || (pet.tiene("--wait") && !ejecutar)) {
        std::fprintf(
            stderr,
            TC("t_sch_sobra",
               "«%s» sobra: «schedule status» y «schedule run» son "
               "las dos formas con palabra\n"),
            palabras.empty() ? (pet.tiene("-a") ? "-a" : "--wait") : palabras.back().c_str());
        return false;
    }
    if (ejecutar) {
        return cmdScheduleRun(e, destino, pet.tiene("--wait"));
    }
    // Lo que hay puesto AHORA. Se lee siempre, también para escribir: fijar `--daily` no
    // debe borrar el resto de la programación.
    std::vector<B::gsa::Entry> aqui;
    if (!leeProgramaciones(e, destino, destino.dataset, aqui)) {
        return false;
    }
    B::gsa::Schedule actual;
    for (const B::gsa::Entry& en : aqui) {
        if (en.dataset == destino.dataset) {
            actual = en.schedule;
        }
    }

    // Quitarla del todo: se heredan las propiedades, que es lo contrario de escribirlas.
    if (pet.tiene("--clear")) {
        if (!confirma(e, B::format(T("t_conf_sch_clear",
                                     "Se va a borrar la programación de %1. "
                                     "¿Continuar?"),
                                   {destino.dataset}))) {
            std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
            return false;
        }
        for (const auto& kv : B::gsa::toProperties(B::gsa::Schedule{})) {
            std::string sinUsar;
            if (!zfsGeneric(e, destino, {"inherit", kv.first, destino.dataset})) {
                return false;
            }
        }
        std::fprintf(stderr, TC("t_sch_borrada", "borrada la programación de %s\n"),
                     destino.dataset.c_str());
        return true;
    }

    // ¿Se pide algún cambio? Sin ninguno, esta orden solo ENSEÑA.
    const char* const kOpciones[] = {"hourly", "daily", "weekly", "monthly", "yearly", "to"};
    bool cambia = pet.tiene("--off") || pet.tiene("--recursive") || pet.tiene("--no-recursive")
                  || pet.tiene("--level") || pet.tiene("--no-level");
    for (const char* o : kOpciones) {
        if (!pet.valor(o).empty()) {
            cambia = true;
        }
    }
    if (!cambia) {
        std::vector<std::pair<std::string, B::gsa::Entry>> filas;
        if (!aqui.empty()) {
            for (const B::gsa::Entry& en : aqui) {
                filas.push_back({destino.connection, en});
            }
        }
        if (filas.empty()) {
            std::fprintf(stderr, TC("t_sch_ninguna", "%s no tiene programación\n"),
                         destino.dataset.c_str());
            return true;
        }
        tablaDeProgramaciones(filas).imprime(e.formato);
        return true;
    }

    B::gsa::Schedule nueva = actual;
    const auto retencion = [&](const char* opcion, int& campo) {
        const std::string v = pet.valor(opcion);
        if (v.empty()) {
            return true;
        }
        std::map<std::string, std::string> uno{{std::string(B::gsa::kPropertyPrefix) + "daily", v}};
        B::gsa::Schedule tmp;
        B::gsa::Reason m;
        if (!B::gsa::fromProperties(uno, tmp, m)) {
            std::fprintf(stderr,
                         TC("t_sch_no_entero",
                            "«%s» no es un número de snapshots válido "
                            "para --%s\n"),
                         v.c_str(), opcion);
            return false;
        }
        campo = tmp.daily;
        return true;
    };
    if (!retencion("hourly", nueva.hourly) || !retencion("daily", nueva.daily)
        || !retencion("weekly", nueva.weekly) || !retencion("monthly", nueva.monthly)
        || !retencion("yearly", nueva.yearly)) {
        return false;
    }
    if (pet.tiene("--recursive")) {
        nueva.recursive = true;
    }
    if (pet.tiene("--no-recursive")) {
        nueva.recursive = false;
    }
    if (pet.tiene("--level")) {
        nueva.level = true;
    }
    if (pet.tiene("--no-level")) {
        nueva.level = false;
    }
    if (!pet.valor("to").empty()) {
        // La URL de un dataset: así se escribe, se guarda y la lee el daemon.
        B::gsa::Reason m;
        if (!B::gsa::targetFromUrl(pet.valor("to"), nueva.target, m)) {
            std::fprintf(stderr, "--to %s: %s\n", pet.valor("to").c_str(),
                         B::gsa::labelOf(m.failure).c_str());
            return false;
        }
    }
    // Fijar cualquier valor la ACTIVA: programar algo apagado no significa nada. `--off`
    // es lo que la apaga, y por eso se aplica al final.
    nueva.enabled = !pet.tiene("--off");

    // Y ahora las reglas, ANTES de escribir. Primero la programación en sí.
    const auto conexionExiste = [&](const std::string& nombre) {
        return buscarConexion(e.conns, nombre) != nullptr;
    };
    B::gsa::Reason motivo;
    if (!B::gsa::isValid(destino.dataset, nueva, conexionExiste, motivo)) {
        std::fprintf(stderr, "%s: %s\n", destino.dataset.c_str(),
                     B::gsa::labelOf(motivo.failure).c_str());
        if (!motivo.detail.empty()) {
            std::fprintf(stderr, "  %s\n", motivo.detail.c_str());
        }
        return false;
    }
    // Y después el conjunto: dos activadas del mismo pool no pueden solaparse si una es
    // recursiva. Hace falta lo que ya hay programado en el POOL, no solo aquí debajo.
    std::vector<B::gsa::Entry> delPool;
    ZfsmUrl raizPool = destino;
    raizPool.dataset = destino.pool;
    if (leeProgramaciones(e, destino, destino.pool, delPool)) {
        bool sustituido = false;
        for (B::gsa::Entry& en : delPool) {
            if (en.dataset == destino.dataset) {
                en.schedule = nueva;
                sustituido = true;
            }
        }
        if (!sustituido) {
            delPool.push_back({destino.dataset, nueva});
        }
        if (!B::gsa::isValidSet(delPool, motivo)) {
            std::fprintf(stderr, "%s: %s (%s)\n", motivo.dataset.c_str(),
                         B::gsa::labelOf(motivo.failure).c_str(), motivo.detail.c_str());
            return false;
        }
    }

    std::vector<std::string> argv{"set"};
    for (const auto& kv : B::gsa::toProperties(nueva)) {
        argv.push_back(kv.first + "=" + kv.second);
    }
    argv.push_back(destino.dataset);
    if (!zfsGeneric(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, TC("t_sch_puesta", "programación puesta en %s\n"),
                 destino.dataset.c_str());
    return true;
}

}  // namespace zfsmgr::cli::detail
