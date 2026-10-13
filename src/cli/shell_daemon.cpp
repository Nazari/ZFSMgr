// The agent on every machine: `install-daemon`, and its jobs (`jobs`, `job`).

#include "shell_internal.h"

namespace zfsmgr::cli::detail {

PlanCli planDelCli(Estado& e, const B::ConnectionProfile& perfil) {
    namespace DI = B::daemoninstall;
    PlanCli plan;
    if (T::isWindowsConnection(perfil)) {
        plan.accion = PlanCli::Accion::Windows;
        return plan;
    }
    plan.sonda = DI::probeRemoteCli(e.ses->transporte, perfil, e.ses->verboso);
    if (!plan.sonda.found) {
        return plan;
    }
    if (plan.sonda.isLink) {
        plan.accion = PlanCli::Accion::Enlace;  // el de un .app: lo actualiza su instalador
        return plan;
    }
    // El que se está ejecutando no se sustituye a sí mismo a mitad de una orden.
    if (T::isLocalConnection(perfil)) {
        std::error_code ec;
        const auto a = std::filesystem::canonical(plan.sonda.path, ec);
        const auto b = std::filesystem::canonical(dirDelEjecutable() + "/zfsmgr-cli", ec);
        if (!a.empty() && a == b) {
            plan.accion = PlanCli::Accion::EsEste;
            return plan;
        }
    }
    if (!plan.sonda.fullVersion.empty()) {
        switch (B::agentversion::verdictFor(plan.sonda.fullVersion, B::agentversion::expected())) {
            case B::agentversion::Verdict::UpToDate:
                plan.accion = PlanCli::Accion::AlDia;
                return plan;
            case B::agentversion::Verdict::Ahead:
                plan.accion = PlanCli::Accion::PorDelante;
                return plan;
            case B::agentversion::Verdict::Update:
                break;
        }
    }
    plan.binario = rutaDelCli(DI::platformOf(perfil),
                              DI::remoteArchitecture(e.ses->transporte, perfil, e.ses->verboso));
    plan.accion = plan.binario.empty() || dirDeAgentes().empty() || dirDeCatalogos().empty()
                      ? PlanCli::Accion::SinBinario
                      : PlanCli::Accion::Actualizar;
    return plan;
}

std::string textoDePlanCli(const PlanCli& plan) {
    switch (plan.accion) {
        case PlanCli::Accion::NoHay:
            return T("t_cli_plan_no_hay", "no tiene: no se instala");
        case PlanCli::Accion::Windows:
            return T("t_cli_plan_windows", "Windows: no se toca");
        case PlanCli::Accion::Enlace:
            return T("t_cli_plan_enlace", "es el del .app: no se toca");
        case PlanCli::Accion::EsEste:
            return T("t_cli_plan_este", "es el que está en marcha: no se toca");
        case PlanCli::Accion::AlDia:
            return T("t_instala_accion_al_dia", "al día: no se toca");
        case PlanCli::Accion::PorDelante:
            return T("t_instala_accion_delante", "va por delante del cliente: no se toca");
        case PlanCli::Accion::Actualizar:
            return T("t_instala_accion_actualizar", "actualizar");
        case PlanCli::Accion::SinBinario:
            return T("t_cli_plan_sin_binario", "a actualizar, pero este equipo no tiene su CLI");
    }
    return {};
}

// Lo pone al día según el plan. Cuenta lo que hace; `false` solo si se intentó y falló.
bool actualizaCli(Estado& e, const B::ConnectionProfile& perfilConSudo, const PlanCli& plan) {
    namespace DI = B::daemoninstall;
    const std::string quien = perfilConSudo.name.empty() ? perfilConSudo.id : perfilConSudo.name;
    if (plan.accion != PlanCli::Accion::Actualizar) {
        if (plan.accion != PlanCli::Accion::NoHay && plan.accion != PlanCli::Accion::Windows) {
            std::fprintf(stderr, TC("t_cli_de", "zfsmgr-cli de %s: %s\n"), quien.c_str(),
                         textoDePlanCli(plan).c_str());
        }
        return plan.accion != PlanCli::Accion::SinBinario;
    }
    std::fprintf(stderr, TC("t_cli_desplegando", "actualizando zfsmgr-cli en %s (%s)...\n"),
                 quien.c_str(), plan.sonda.path.c_str());
    std::vector<std::pair<std::string, std::string>> todos;
    for (const auto& pa : std::vector<std::pair<std::string, std::string>>{{"linux", "x86_64"},
                                                                           {"freebsd", "x86_64"},
                                                                           {"macos", "amd64"},
                                                                           {"macos", "arm64"},
                                                                           {"windows", "x86_64"}}) {
        const std::string ruta = rutaDelCli(pa.first, pa.second);
        if (!ruta.empty()) {
            todos.emplace_back(pa.first + "-" + pa.second, ruta);
        }
    }
    const DI::Result r = DI::installCli(
        e.ses->transporte, perfilConSudo, plan.binario, dirDeCatalogos(), dirDeAgentes(), todos,
        plan.sonda.path, [](const std::string& l) { std::fprintf(stderr, "  %s\n", l.c_str()); },
        e.ses->verboso);
    if (!r.ok()) {
        std::fprintf(stderr, TC("t_cli_fallo", "no se pudo actualizar zfsmgr-cli en %s: %s\n"),
                     quien.c_str(), r.detail.c_str());
        return false;
    }
    if (r.versionBehind) {
        std::fprintf(stderr,
                     TC("t_cli_empaquetado_viejo",
                        "aviso: el zfsmgr-cli empaquetado para esa máquina es %s y este es %s\n"),
                     r.version.c_str(), B::agentversion::expected().c_str());
    }
    std::fprintf(stderr, TC("t_cli_actualizado", "zfsmgr-cli actualizado en %s\n"), quien.c_str());
    return true;
}

bool cmdInstalarDaemon(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    std::string error;
    const auto* p = perfilVivoDe(e, pet.objetivo, error);
    if (!p) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    const std::string quien = p->name.empty() ? p->id : p->name;

    // **Con trabajos en curso no se reinstala.** Reinstalar reinicia el servicio, y lo que
    // estuviera copiando muere a medias: dos copias de 16 y 50 GB quedaron por la mitad en
    // fc16 el 2026-10-10 por una reinstalación. Se dice cuáles y se para; `--force` lo hace
    // de todas formas, sabiéndolo.
    if (!pet.tiene("--force")) {
        const std::vector<std::string> enCurso = trabajosEnCurso(e, conSudo(e, *p));
        if (!enCurso.empty()) {
            std::fprintf(stderr,
                         TC("t_instala_hay_trabajos",
                            "%s tiene %zu trabajos en curso, y reinstalar el daemon los "
                            "cortaría a medias:\n"),
                         quien.c_str(), enCurso.size());
            for (const std::string& id : enCurso) {
                std::fprintf(stderr, "  %s\n", id.c_str());
            }
            std::fputs(TC("t_instala_hay_trabajos_2",
                          "Espere a que terminen («jobs»), o repita con --force para cortarlos.\n"),
                       stderr);
            e.ultimoRc = 1;
            return false;
        }
    }

    if (!confirma(e, B::format(T("t_conf_install_daemon",
                                 "Se va a instalar o actualizar el daemon en %1 y arrancarlo "
                                 "con el gestor de servicios del sistema. ¿Continuar?"),
                               {quien}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }

    // El guion de instalación vive ahora en la capa base —`base/daemoninstall`— porque no
    // tenía nada de intérprete: son 200 líneas de decidir qué toca según el sistema de la
    // otra punta. Aquí queda lo que sí es del intérprete: preguntar antes y contar después.
    namespace DI = B::daemoninstall;
    const B::ConnectionProfile perfil = conSudo(e, *p);
    const std::string plataforma = DI::platformOf(perfil);
    // La arquitectura del OTRO lado, no la de aquí: se despliega a máquinas distintas.
    const std::string arq = DI::remoteArchitecture(e.ses->transporte, perfil, e.ses->verboso);
    const std::string binario = rutaDelAgente(plataforma, arq);
    if (binario.empty()) {
        std::fprintf(stderr,
                     TC("t_no_se_enco_393030",
                        "no se encontró el binario del daemon para %s/%s en este equipo.\n"
                        "No se instala nada: el respaldo por guion no habla TLS, y dejarlo puesto\n"
                        "daría una máquina que parece atendida y no lo está.\n"),
                     plataforma.c_str(), arq.empty() ? "?" : arq.c_str());
        return false;
    }
    // La versión de OpenZFS se mira ANTES de anunciar el despliegue. `install()` la vuelve a
    // mirar y es quien de verdad manda —así la interfaz gráfica no puede saltársela—, pero
    // si solo estuviera allí, esto diría «desplegando…» y acto seguido «no se instala nada»,
    // que es contarlo al revés.
    {
        const auto zfs = DI::remoteOpenZfs(e.ses->transporte, perfil, e.ses->verboso);
        if (zfs.soporte != B::helpers::OpenZfsSoporte::Ok) {
            std::fprintf(stderr, TC("t_zfs_no_vale", "no se instala nada en %s: %s\n"),
                         quien.c_str(), B::helpers::textoDeSoporteOpenZfs(zfs).c_str());
            return false;
        }
    }
    std::fprintf(stderr, TC("t_desplegand_79081c", "desplegando %s en %s...\n"), binario.c_str(),
                 quien.c_str());

    const DI::Result r = DI::install(
        e.ses->transporte, perfil, binario,
        [](const std::string& l) { std::fprintf(stderr, "  %s\n", l.c_str()); }, e.ses->verboso);

    if (r.versionBehind) {
        std::fprintf(stderr,
                     TC("t_agente_empaquetado_viejo",
                        "aviso: el agente empaquetado para %s es %s y este cliente espera %s; "
                        "se instala igual, pero la conexión seguirá saliendo desactualizada\n"),
                     plataforma.c_str(), r.version.c_str(), B::agentversion::expected().c_str());
    }
    if (!r.ok()) {
        // La versión de OpenZFS se dice ENTERA y sin envolverla en «la instalación falló»:
        // no ha fallado nada, es que no se ha intentado, y lo que hay que hacer está en el
        // propio texto.
        if (r.fallo == DI::Failure::UnsupportedOpenZfs) {
            std::fprintf(stderr, TC("t_zfs_no_vale", "no se instala nada en %s: %s\n"),
                         quien.c_str(), r.detail.c_str());
            return false;
        }
        if (r.fallo == DI::Failure::UnreadableBinary) {
            std::fprintf(stderr,
                         TC("t_el_binario_ce2086", "el binario del daemon está vacío: %s\n"),
                         r.detail.c_str());
            return false;
        }
        std::fprintf(stderr, TC("t_la_instala_2b9e0d", "la instalación falló (código %d): %s\n"),
                     r.rc, r.detail.c_str());
        e.ultimoRc = r.rc == 0 ? 1 : r.rc;
        return false;
    }
    // Lo que la sesión recordaba de esta máquina ya no vale —es otro daemon, quizá con otro
    // material TLS—, y se le PREGUNTA al nuevo antes de dar la instalación por buena.
    //
    // Con reintentos, porque el guion de instalación da el servicio por arrancado en cuanto
    // el PROCESO existe, no cuando ya escucha: la primera pregunta podía llegar antes de
    // tiempo, fallar, y la sesión recordaba ese fallo —el «-» de la columna DAEMON— hasta
    // salir. Medido en FreeBSD 15.1: el `ls` de justo después fallaba con «unexpected eof» en
    // el saludo TLS, y uno lanzado unos segundos más tarde contestaba.
    std::string versionNueva;
    std::string ultimoMotivo;
    for (int intento = 0; intento < 8 && versionNueva.empty(); ++intento) {
        if (intento > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        // Cada intento desde cero: sin esto, el primer fallo dejaba la conexión penalizada
        // 30 s y los siguientes ni siquiera llegaban a preguntar.
        olvidaEstadoDeConexion(e, *p);
        std::string salida;
        std::string err;
        int rcSalud = -1;
        if (ejecutarAgente(*e.ses, *p, {"--health"}, salida, err, rcSalud, &ultimoMotivo, 4000)
            && rcSalud == 0) {
            auto claves = clavesDe(salida);
            versionNueva = claves["VERSION"];
            // Lo que el listado enseñará en AVISOS: sin esto, la versión quedaba recordada
            // y el aviso no, así que `ls` en la misma sesión no volvía a preguntar.
            const std::string id = p->id.empty() ? p->name : p->id;
            e.avisoDaemon.erase(id);
            if (claves["DISK_ACCESS"] == "denied") {
                e.avisoDaemon[id] = T("t_aviso_disco", "sin acceso al disco");
            }
        }
    }
    if (versionNueva.empty()) {
        olvidaEstadoDeConexion(e, *p);  // que el fallo no quede recordado
        std::fprintf(stderr,
                     TC("t_daemon_instalado_mudo",
                        "daemon instalado en %s, pero todavía no contesta (%s); «refresh» "
                        "vuelve a preguntar\n"),
                     quien.c_str(), ultimoMotivo.c_str());
    } else {
        e.versionDaemon[p->id.empty() ? p->name : p->id] = versionNueva;
        std::fprintf(
            stderr,
            TC("t_daemon_instalado_contesta", "daemon instalado en %s: contesta, versión %s\n"),
            quien.c_str(), versionNueva.c_str());
    }
    // En macOS hace falta UN PASO MÁS, y a mano: sin «Acceso total al disco» el agente
    // arranca, contesta y responde STATUS=OK, pero no ve los discos, así que no encuentra
    // ningún pool para importar. Todo parece bien salvo el resultado, que es la peor forma
    // de fallar; por eso se dice aquí, al instalar, y no cuando la lista salga vacía. La
    // interfaz gráfica ya lo avisa —`mainwindow_connections.cpp`—, y el intérprete callaba.
    // Y el CLI de esa máquina, si tiene uno. Un fallo aquí no deshace el daemon, pero se
    // cuenta: la máquina queda a medias y la orden lo dice.
    const bool cliBien = actualizaCli(e, perfil, planDelCli(e, perfil));
    if (r.isMac) {
        std::fputs(
            TC("t_mac_acceso_disco",
               "\nEn macOS hace falta concederle «Acceso total al disco» al agente, o no verá\n"
               "los discos y no encontrará pools que importar:\n"
               "  Configuración del Sistema → Privacidad y Seguridad → Acceso total al\n"
               "  disco → añadir /usr/local/libexec/zfsmgr-agent\n"),
            stderr);
    }
    if (!cliBien) {
        e.ultimoRc = 1;
    }
    return cliBien;
}

// --- Trabajos en segundo plano.
//
// El daemon puede ejecutar las operaciones LARGAS sin que nadie espere al otro lado:
// breakdown, assemble, todir y rsync. Es lo que permite lanzar una copia de horas y cerrar
// la sesión sin matarla.
//
// Se pide con `--job` en esas órdenes; aquí están las de consultar y cancelar.

// Una línea «CLAVE=valor» en un mapa, que es el formato con el que contesta el agente.
std::map<std::string, std::string> clavesDe(const std::string& texto) {
    std::map<std::string, std::string> m;
    for (const std::string& linea : B::split(texto, "\n", true)) {
        const std::size_t igual = linea.find('=');
        if (igual != std::string::npos) {
            m[B::trim(linea.substr(0, igual))] = B::trim(linea.substr(igual + 1));
        }
    }
    return m;
}

bool cmdJobs(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    // La conexión del sitio actual (o de --on). No se exige en la declaración porque con
    // `-a` vale desde cualquier sitio, también desde la raíz.
    const ZfsmUrl destino = subeHasta(pet.objetivo, Objetivo::Conexion);
    if (!pet.tiene("-a") && destino.connection.empty()) {
        std::fputs(TC("t_jobs_falta_conexion",
                      "hace falta estar en una conexión, o «jobs -a» para todas\n"),
                   stderr);
        return false;
    }
    // Por omisión, SOLO lo que está corriendo. Un daemon que lleva meses en pie acumula
    // trabajos terminados, y enseñarlos todos convierte «¿qué está pasando ahora?» —que es
    // la pregunta que uno tiene al teclear `jobs`— en buscar entre decenas de líneas.
    //
    // El filtro se pide por el NOMBRE DEL ESTADO, que es el mismo que sale en la columna:
    // así no hay que aprender un vocabulario aparte ni mantener una tabla de traducción
    // entre lo que se escribe y lo que devuelve el daemon.
    std::set<std::string> estados;
    if (!pet.tiene("--history")) {
        for (const char* est : {"running", "queued", "done", "failed", "cancelled"}) {
            if (pet.tiene(std::string("--") + est)) {
                estados.insert(est);
            }
        }
        if (estados.empty()) {
            estados.insert("running");
            estados.insert("queued");  // encolado es «todavía va a pasar», no historia
        }
    }
    // `-a`: los de TODAS las conexiones, una detrás de otra en la misma tabla, que por eso
    // lleva la columna CONEXIÓN. Sin `-a`, los de la conexión en la que se está.
    const bool todas = pet.tiene("-a");
    // `--detail`: qué hace cada uno —«cp /origen -> /destino», «send pool/ds@s -> host»—,
    // que el agente compone al recibirlo. El tipo solo («mutation») no lo dice.
    const bool detalle = pet.tiene("--detail");
    std::vector<std::pair<std::string, ZfsmUrl>> maquinas;  // (id, url)
    if (todas) {
        for (const auto& p : e.conns.perfiles) {
            const std::string id = p.id.empty() ? p.name : p.id;
            if (e.conns.desconectada(id)) {
                continue;
            }
            ZfsmUrl u;
            u.kind = ZfsmKind::Connection;
            u.connection = id;
            maquinas.emplace_back(id, u);
        }
    } else {
        maquinas.emplace_back(destino.connection, destino);
    }
    Tabla t;
    t.nombreJson = "jobs";
    t.cabecerasTexto = {T("t_cab_conexion", "CONEXIÓN"),    T("t_cab_id", "ID"),
                        T("t_cab_estado", "ESTADO"),        T("t_cab_tipo", "TIPO"),
                        T("t_cab_instantanea", "SNAPSHOT"), T("t_cab_bytes", "BYTES"),
                        T("t_cab_mib_s", "MiB/s"),          T("t_cab_segundos", "SEGUNDOS"),
                        T("t_cab_error", "ERROR")};
    t.campos = {"connection", "id", "state", "type", "snap", "bytes", "rate", "elapsed", "error"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena,
               Tipo::Bytes,  Tipo::Cadena, Tipo::Entero, Tipo::Cadena};
    if (detalle) {
        t.cabecerasTexto.push_back(T("t_cab_que_hace", "QUÉ HACE"));
        t.campos.push_back("description");
        t.tipos.push_back(Tipo::Cadena);
    }
    bool algunaFallo = false;
    for (const auto& [idMaquina, url] : maquinas) {
        std::string out;
        if (!agente(e, url, PET::jobList(), out, 30000)) {
            // Con -a, una máquina que no contesta no tapa a las demás: se dice y se sigue.
            if (!todas) {
                return false;
            }
            algunaFallo = true;
            continue;
        }
        // Cada línea es «JOB={…json…}», que se analiza con el JSON de la capa base.
        for (const std::string& linea : B::split(out, "\n", true)) {
            if (!B::startsWith(linea, "JOB=")) {
                continue;
            }
            B::json::Value j;
            std::string err;
            if (!B::json::parse(linea.substr(4), j, &err)) {
                continue;
            }
            if (!estados.empty() && estados.count(j["state"].toString()) == 0) {
                continue;
            }
            // El ritmo con DOS decimales: `std::to_string` de un double da seis, y «0.000000»
            // ocupa una columna entera para no decir nada.
            char ritmo[32];
            std::snprintf(ritmo, sizeof(ritmo), "%.2f", j["rate"].toDouble());
            // El texto de error puede traer saltos de línea; en una tabla los rompe.
            std::string textoErr =
                errorDeTrabajo(j["error_code"].toString(), B::json::toCompact(j["error_args"]));
            B::replaceAll(textoErr, "\n", " ");
            B::replaceAll(textoErr, "\r", " ");
            std::vector<std::string> fila{idMaquina,
                                          j["id"].toString(),
                                          j["state"].toString(),
                                          j["type"].toString(),
                                          j["snap"].toString(),
                                          std::to_string(j["bytes"].toInt()),
                                          ritmo,
                                          std::to_string(j["elapsed"].toInt()),
                                          B::trim(textoErr)};
            if (detalle) {
                const std::string d = j["desc"].toString();
                fila.push_back(d.empty() ? std::string("-") : d);
            }
            t.filas.push_back(fila);
        }
    }
    if (algunaFallo) {
        e.ultimoRc = 1;
    }
    if (t.filas.empty()) {
        // Con filtro puesto, «no hay trabajos» es engañoso: puede haber diez terminados. Se
        // dice cuál era el filtro para que la respuesta signifique lo que parece.
        std::fputs(estados.empty() ? TC("t_no_hay_tra_f891dc", "no hay trabajos\n")
                                   : TC("t_no_hay_tra_filtro2",
                                        "no hay trabajos en curso (pruebe «jobs --history»)\n"),
                   stderr);
    }
    t.imprime(e.formato);
    return true;
}

bool cmdJob(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (pet.lista("texto").empty()) {
        std::fputs(TC("t_uso_job_id_7597d2", "uso: job <id> | job cancel <id>\n"), stderr);
        return false;
    }
    const bool cancelar = B::toLowerAscii(pet.uno("texto")) == "cancel";
    if (cancelar && pet.lista("texto").size() < 2) {
        std::fputs(TC("t_uso_job_ca_1e9ae2", "uso: job cancel <id>\n"), stderr);
        return false;
    }
    const std::string id = cancelar ? pet.lista("texto")[1] : pet.uno("texto");
    if (cancelar) {
        if (!confirma(e, B::format(T("t_conf_job_cancel",
                                     "Se va a cancelar el trabajo %1. Lo que ya haya hecho NO "
                                     "se deshace. ¿Continuar?"),
                                   {id}))) {
            std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
            return false;
        }
        std::string out;
        if (!agente(e, destino, PET::cancelJob(id), out, 30000)) {
            return false;
        }
        std::fprintf(stderr, TC("t_cancelado__b0d1d4", "cancelado el trabajo %s\n"), id.c_str());
        return true;
    }
    std::string out;
    if (!agente(e, destino, PET::jobStatus(id), out, 30000)) {
        return false;
    }
    Tabla t;
    t.nombreJson = "job";
    t.cabecerasTexto = {T("t_cab_campo", "CAMPO"), T("t_cab_valor", "VALOR")};
    t.campos = {"field", "value"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena};
    const auto claves = clavesDe(out);
    for (const auto& kv : claves) {
        // El código del error y sus argumentos no se enseñan sueltos: dan la fila «error» en
        // el idioma de quien lo lee.
        if (kv.first == "ERROR_ARGS") {
            continue;
        }
        if (kv.first == "ERROR_CODE") {
            t.filas.push_back({"error", errorDeTrabajo(claves)});
            continue;
        }
        t.filas.push_back({B::toLowerAscii(kv.first), kv.second});
    }
    t.imprime(e.formato);
    return true;
}

// Manda una operación larga al daemon en vez de esperarla. Devuelve el identificador, que
// es con lo que después se consulta o se cancela.
// Las cuatro órdenes que mueven datos de verdad se comportan IGUAL: van al daemon como
// trabajo, y `--wait` es lo que pide esperar aquí a que terminen.
//
// Antes no era así: `send` iba como trabajo y se esperaba con `--wait`, mientras que
// `breakdown`, `assemble` y `todir` eran síncronas y `--job` las mandaba al daemon. Dos
// banderas para la misma idea, con el valor por omisión INVERTIDO entre ellas: había que
// recordar cuál era cuál para cada orden.
//
// El que manda es el trabajo: son operaciones de horas, y ese es además el camino que usa
// la interfaz. Esperar aquí es lo excepcional —un guion que quiere el resultado antes de
// seguir—, y por eso es lo que lleva bandera.
bool lanzaOEspera(Estado& e, const Peticion& pet, const ZfsmUrl& destino,
                  const std::vector<std::string>& argv) {
    if (!pet.tiene("--wait")) {
        return enviaComoTrabajo(e, destino, argv);
    }
    // SIN PLAZO: mueve datos de verdad, y matarlo a mitad es peor que aguardar.
    std::string out;
    if (!agente(e, destino, argv, out, 0)) {
        return false;
    }
    std::fprintf(stdout, "%s", out.c_str());
    return true;
}

bool enviaComoTrabajo(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv) {
    // `enqueue` además comprueba que el daemon sepa encolar ESE verbo: mandarle uno que no
    // está en su lista es un viaje para recibir un rc=2.
    const std::vector<std::string> conJob = PET::enqueue(argv);
    if (conJob.empty()) {
        std::fputs(TC("t_no_encolable", "esa orden no se puede encolar como trabajo\n"), stderr);
        return false;
    }
    std::string out;
    if (!agente(e, destino, conJob, out, 60000)) {
        return false;
    }
    const auto claves = clavesDe(out);
    const auto it = claves.find("JOB_ID");
    if (it == claves.end()) {
        std::fputs(TC("t_el_daemon__64a9ed", "el daemon no devolvió identificador de trabajo\n"),
                   stderr);
        return false;
    }
    imprimeTrabajo(e, it->second, destino.connection);
    std::fprintf(stderr,
                 TC("t_en_marcha__e494cb", "en marcha como trabajo %s; siga con «job %s»\n"),
                 it->second.c_str(), it->second.c_str());
    return true;
}

// --- Pools.
//
// Todo por `--mutate-zpool-generic`, que recibe el argv de `zpool` en JSON y solo admite
// una lista cerrada de operaciones: nunca hay un intérprete de por medio.

bool zpoolGeneric(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv) {
    std::string out;
    if (!agente(e, destino, PET::zpoolGeneric(B::helpers::agentArgv(argv)), out, 0)) {
        return false;
    }
    if (!B::trim(out).empty()) {
        std::fprintf(stdout, "%s", out.c_str());
    }
    return true;
}

}  // namespace zfsmgr::cli::detail
