// `send`: snapshots between datasets, machines and stream files.

#include "shell_internal.h"

namespace zfsmgr::cli::detail {

// Send una instantánea a otro dataset, en esta máquina o en otra.
//
// Con `--base` es INCREMENTAL: solo viaja lo que cambió desde esa instantánea, que es lo
// que la interfaz llama «Nivelar». Sin ella va el flujo completo.
bool cmdSend(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    // Las banderas de `zfs send` se escriben SUELTAS: `send /otra/x -w -L`.
    //
    // Hubo una opción `--flags "-w -L"` que envolvía lo mismo en una cadena, y sobraba: el
    // léxico ya sabe cuáles son banderas de esta orden porque están declaradas, así que
    // meterlas en un texto entrecomillado solo añadía una forma más de escribir lo mismo —y
    // una que había que validar aparte, porque ese texto llegaba en crudo al argv de un
    // `zfs send` con privilegios: `--flags "tank/otro@ayer"` sacaba por el socket un
    // dataset que no era el pedido—.
    //
    // El daemon sigue comprobando la lista, que es donde tiene que comprobarse: el
    // intérprete es un cliente más.
    std::string banderasSend;
    for (const std::string& n : pet.nativas()) {
        banderasSend += (banderasSend.empty() ? "" : " ") + n;
    }
    if (pet.lista("destino").empty() && pet.valor("to").empty()) {
        std::fputs(TC("t_uso_copy_d_f6efbc",
                      "uso: send <destino> [--from <@snapshot>] [--base <@snapshot>]\n"
                      "          [banderas de zfs send] [--wait]\n"
                      "  El destino es una URL: puede estar en OTRA máquina.\n"
                      "  Sin --from se usa el sitio actual como origen.\n"
                      "  Cualquiera de los dos puede ser un fichero con el flujo («#path/…»).\n"
                      "  Con --base solo viaja lo que cambió desde ahí (lo que la interfaz\n"
                      "  llama «Nivelar»).\n"),
                   stderr);
        return false;
    }
    // Cualquiera de las dos puntas puede ser un FICHERO con un flujo de `zfs send` dentro:
    // el destino, para volcarlo; el origen (`--from`), para recibirlo como si viniera de una
    // instantánea. Un directorio no es un flujo: solo vale si equivale a un dataset o a una
    // instantánea —su punto de montaje, la raíz de `.zfs/snapshot/<s>`—, como en el resto de
    // órdenes.
    ZfsmUrl origen = pet.objetivo;
    std::string ficheroOrigen;
    if (esSeccionDeFicheros(origen)) {
        const std::string escrito = textoDe(origen);
        Ubicacion ub;
        if (!ubicacionDe(e, origen, ub)) {
            return false;
        }
        if (!ub.existe) {
            std::fprintf(stderr, TC("t_send_no_existe", "%s no existe\n"), escrito.c_str());
            e.ultimoRc = 1;
            return false;
        }
        if (ub.esDirectorio) {
            if (!ub.esMontaje || ub.dataset.empty()) {
                std::fprintf(stderr,
                             TC("t_send_origen_dir",
                                "%s es un directorio: el origen es un fichero con el "
                                "flujo, o la raíz de «.zfs/snapshot/<s>»\n"),
                             escrito.c_str());
                e.ultimoRc = 2;
                return false;
            }
            if (!equivaleADataset(e, origen, escrito)) {
                return false;
            }
        } else if (ub.tipo != "f" && ub.tipo != "l") {
            std::fprintf(stderr,
                         TC("t_send_no_es_fichero",
                            "%s no es un fichero normal: el flujo tiene que estar en uno\n"),
                         escrito.c_str());
            e.ultimoRc = 1;
            return false;
        } else {
            ficheroOrigen = ub.ruta;
        }
    }
    if (ficheroOrigen.empty() && origen.snapshot.empty()) {
        std::fprintf(stderr,
                     TC("t_el_origen__886ec5", "el origen tiene que ser un SNAPSHOT (ahora: %s)\n"),
                     textoDe(origen).c_str());
        return false;
    }
    ZfsmUrl destino;
    std::string error;
    if (!resuelve(e, pet.lista("destino").empty() ? pet.valor("to") : pet.uno("destino"), destino,
                  error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    std::string ficheroDestino;
    bool sobrescribir = false;
    if (esSeccionDeFicheros(destino)) {
        const std::string escrito = textoDe(destino);
        if (!ficheroOrigen.empty()) {
            std::fputs(TC("t_send_fichero_a_fichero",
                          "de un fichero a otro no hay nada que enviar: eso es «cp»\n"),
                       stderr);
            e.ultimoRc = 2;
            return false;
        }
        Ubicacion ub;
        if (!ubicacionDe(e, destino, ub)) {
            return false;
        }
        if (ub.existe && ub.esDirectorio) {
            // El punto de montaje de un dataset es ese dataset. Cualquier otro directorio
            // es un sitio donde guardar el fichero, y su nombre no se inventa.
            if (!(ub.esMontaje && !ub.dataset.empty() && ub.instantanea.empty())) {
                std::fprintf(stderr,
                             TC("t_send_destino_dir",
                                "%s es un directorio: el flujo se guarda en un fichero; "
                                "escriba también su nombre\n"),
                             escrito.c_str());
                e.ultimoRc = 2;
                return false;
            }
            if (!equivaleADataset(e, destino, escrito)) {
                return false;
            }
        } else if (ub.existe && ub.tipo != "f") {
            std::fprintf(stderr, TC("t_send_destino_raro", "%s existe y no es un fichero normal\n"),
                         escrito.c_str());
            e.ultimoRc = 2;
            return false;
        } else {
            ficheroDestino = ub.ruta;
            sobrescribir = ub.existe;
            // El directorio tiene que existir: el daemon no crea directorios como root a
            // petición de quien conecta. Se mira AHORA, antes de pedir confirmación.
            std::string padre = ficheroDestino.substr(0, ficheroDestino.find_last_of('/'));
            if (padre.empty() || padre.back() == ':') {
                padre += "/";
            }
            std::string infoPadre;
            B::json::Value v;
            if (!agente(e, destino, PET::pathInfo(padre), infoPadre, 20000)) {
                return false;
            }
            if (!B::json::parse(infoPadre, v, nullptr) || !v["is_dir"].toBool()) {
                std::fprintf(stderr,
                             TC("t_send_sin_directorio", "el directorio %s no existe en %s\n"),
                             padre.c_str(), destino.connection.c_str());
                e.ultimoRc = 1;
                return false;
            }
        }
    }
    if (ficheroDestino.empty() && destino.dataset.empty()) {
        std::fputs(TC("t_el_destino_e077ee", "el destino tiene que ser un dataset\n"), stderr);
        return false;
    }
    // `--comp`: el fichero de destino se guarda comprimido. Solo tiene sentido ahí: al LEER
    // un fichero el formato se reconoce por sus primeros bytes, y un dataset no es un fichero.
    zfsmgr::compresion::Eleccion compresion;
    if (!pet.valor("comp").empty()) {
        std::string errComp;
        if (ficheroDestino.empty()) {
            std::fputs(TC("t_send_comp_sin_fichero",
                          "«--comp» es para un FICHERO de destino; al leer uno con «--from» el "
                          "formato se reconoce solo\n"),
                       stderr);
            e.ultimoRc = 2;
            return false;
        }
        if (!zfsmgr::compresion::leeEleccion(pet.valor("comp"), compresion, errComp)) {
            std::fprintf(stderr, "--comp: %s\n", errComp.c_str());
            e.ultimoRc = 2;
            return false;
        }
        // Con -c o -w los bloques ya van comprimidos o cifrados: comprimir otra vez gasta CPU
        // y apenas gana. Se dice y se hace, que puede haber motivo.
        const std::string banderasConEspacios = " " + banderasSend + " ";
        const bool yaComprimido = banderasConEspacios.find(" -c ") != std::string::npos
                                  || banderasConEspacios.find(" -w ") != std::string::npos
                                  || banderasConEspacios.find("--compressed") != std::string::npos
                                  || banderasConEspacios.find("--raw") != std::string::npos;
        if (compresion.formato != zfsmgr::compresion::Formato::Ninguno && yaComprimido) {
            std::fputs(TC("t_send_comp_ya_comprimido",
                          "aviso: con «-c» o «-w» los bloques ya viajan comprimidos o cifrados; "
                          "volver a comprimirlos apenas gana\n"),
                       stderr);
        }
    }
    const bool comprimido = compresion.formato != zfsmgr::compresion::Formato::Ninguno;
    // Lo que el flujo ya trae no se puede pedir otra vez: un fichero es el flujo que es.
    if (!ficheroOrigen.empty() && (!pet.valor("base").empty() || !banderasSend.empty())) {
        std::fputs(TC("t_send_fichero_sin_banderas",
                      "con un fichero de origen no valen «--base» ni las banderas de zfs send: "
                      "el flujo se hizo al volcarlo\n"),
                   stderr);
        e.ultimoRc = 2;
        return false;
    }
    const auto* pOrigen = perfilVivoDe(e, origen, error);
    if (!pOrigen) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    const auto* pDestino = perfilVivoDe(e, destino, error);
    if (!pDestino) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    // El extremo de ZFS no puede ser Windows: el flujo por socket no está portado allí.
    // Decirlo AQUÍ evita un fallo a mitad de transfer que no se entiende. Un FICHERO en
    // Windows sí vale: guardarlo o leerlo no pasa por `zfs`.
    if ((ficheroOrigen.empty() && T::isWindowsConnection(*pOrigen))
        || (ficheroDestino.empty() && T::isWindowsConnection(*pDestino))) {
        std::fputs(TC("t_la_transfe_6f9799",
                      "la transferencia por socket no está disponible en Windows.\n"
                      "Para llevar datos a o desde una máquina Windows, use «todir» y "
                      "«fromdir».\n"),
                   stderr);
        return false;
    }
    const bool mismaMaquina =
        B::toLowerAscii(origen.connection) == B::toLowerAscii(destino.connection);

    std::string base = pet.valor("base");
    if (!base.empty() && base.front() == '@') {
        base = origen.dataset + base;
    }

    // ¿Una recepción a medias en el destino?
    //
    // Si la hay, `zfs recv` no acepta otro envío hasta que se decida qué hacer con ella, y lo
    // que llegaba al usuario era su error crudo; la salida era `zfs recv -A` a mano en la
    // otra máquina, teniendo allí un daemon como root que sabe hacerlo. Se mira ANTES de
    // preguntar nada, y sin una decisión explícita no se toca.
    const bool reanudar = pet.tiene("--resume");
    const bool descartar = pet.tiene("--discard-partial");
    if (reanudar && descartar) {
        std::fputs(TC("t_send_resume_y_discard",
                      "«--resume» y «--discard-partial» son lo contrario: una de las dos\n"),
                   stderr);
        e.ultimoRc = 2;
        return false;
    }
    // Un fichero no tiene recepciones a medias, y continuar una necesita el `zfs send -t`
    // del origen: un fichero solo puede dar el flujo desde el principio.
    if ((reanudar || descartar) && !ficheroDestino.empty()) {
        std::fputs(TC("t_send_fichero_sin_parcial",
                      "un fichero de destino no tiene recepciones a medias: «--resume» y "
                      "«--discard-partial» no aplican\n"),
                   stderr);
        e.ultimoRc = 2;
        return false;
    }
    if (reanudar && !ficheroOrigen.empty()) {
        std::fputs(TC("t_send_fichero_no_reanuda",
                      "un fichero no puede continuar una recepción a medias: hace falta la "
                      "instantánea de origen, o «--discard-partial» para empezar de cero\n"),
                   stderr);
        e.ultimoRc = 2;
        return false;
    }
    const TR::Resume aMedias =
        !ficheroDestino.empty()
            ? TR::Resume{}
            : TR::findResumeToken(
                  [&](const B::ConnectionProfile& maquina, const std::vector<std::string>& args,
                      int timeoutMs, std::string& out, std::string& err, int& rc) {
                      return ejecutarAgente(*e.ses, maquina, args, out, err, rc, nullptr,
                                            timeoutMs);
                  },
                  *pDestino, destino.dataset);
    std::string testigo;
    if (aMedias.any()) {
        if (!reanudar && !descartar) {
            std::fprintf(
                stderr,
                TC("t_send_hay_parcial",
                   "%s tiene una recepción que se cortó a medias, y no acepta otro envío "
                   "hasta decidir qué hacer con ella:\n"
                   "  --resume            la continúa desde donde quedó\n"
                   "  --discard-partial   la descarta (se pierde lo recibido) y empieza de cero\n"),
                aMedias.heldBy.c_str());
            e.ultimoRc = 1;
            return false;
        }
        if (reanudar) {
            testigo = aMedias.token;
        } else {
            if (!confirma(e, B::format(T("t_conf_descartar_parcial",
                                         "Se va a DESCARTAR la recepción a medias de %1 en %2: "
                                         "se pierde lo ya recibido. ¿Continuar?"),
                                       {aMedias.heldBy, destino.connection}))) {
                std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
                return false;
            }
            std::string out;
            if (!agente(e, destino, PET::abortResumableReceive(aMedias.heldBy), out, 60000)) {
                return false;
            }
            std::fprintf(stderr,
                         TC("t_parcial_descartada", "descartada la recepción a medias de %s\n"),
                         aMedias.heldBy.c_str());
        }
    } else if (reanudar) {
        std::fprintf(stderr,
                     TC("t_send_nada_que_reanudar",
                        "%s no tiene ninguna recepción a medias que continuar\n"),
                     destino.dataset.c_str());
        e.ultimoRc = 1;
        return false;
    }

    if (!testigo.empty()) {
        if (!confirma(
                e, B::format(T("t_conf_reanudar",
                               "Se va a continuar la recepción a medias de %1 en %2. ¿Continuar?"),
                             {aMedias.heldBy, destino.connection}))) {
            std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
            return false;
        }
    } else if (!ficheroDestino.empty()) {
        const std::string que =
            sobrescribir
                ? B::format(
                      T("t_conf_send_fichero_existe",
                        "Se va a volcar el flujo de %1 (en %2) al fichero %3 de %4, que YA "
                        "EXISTE y se sustituye. ¿Continuar?"),
                      {origen.zfsName(), origen.connection, ficheroDestino, destino.connection})
                : B::format(
                      T("t_conf_send_fichero",
                        "Se va a volcar el flujo de %1 (en %2) al fichero %3 de %4. "
                        "¿Continuar?"),
                      {origen.zfsName(), origen.connection, ficheroDestino, destino.connection});
        const std::string queConCompresion =
            comprimido ? B::format(T("t_conf_send_comprimido", "Comprimido con %1.\n"),
                                   {zfsmgr::compresion::textoDe(compresion)})
                             + que
                       : que;
        if (!confirma(e, queConCompresion)) {
            std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
            return false;
        }
    } else if (!ficheroOrigen.empty()) {
        if (!confirma(
                e, B::format(
                       T("t_conf_send_desde_fichero",
                         "Se va a recibir el flujo del fichero %1 (en %2) en %3 de %4.\n"
                         "Si el destino existe y difiere, `zfs recv -F` lo SOBRESCRIBE. "
                         "¿Continuar?"),
                       {ficheroOrigen, origen.connection, destino.dataset, destino.connection}))) {
            std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
            return false;
        }
    } else if (!confirma(
                   e, B::format(T("t_conf_copy",
                                  "Se va a %1 %2 de %3 a %4 en %5.\nSi el destino existe y "
                                  "difiere, `zfs recv -F` lo SOBRESCRIBE. ¿Continuar?"),
                                {base.empty() ? T("t_copiar", "copiar") : T("t_nivelar", "nivelar"),
                                 origen.zfsName(), origen.connection, destino.dataset,
                                 destino.connection}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }

    // Los tres pasos —escuchar, resolver la dirección, enviar— los da `TR::launchJob`.
    //
    // Aquí había una copia entera de esa secuencia, y YA HABÍA DIVERGIDO: resolvía la
    // dirección del destino como «el host del perfil», que cuando el destino es la conexión
    // **Local** vale «localhost» y desde una máquina remota apunta al propio origen. Send
    // de una máquina remota a Local dejaba al emisor conectándose consigo mismo. La capa
    // base pregunta al origen con qué dirección nos ve, que es la única que le sirve para
    // volver, y ese caso es justo el que se perdía al tener dos copias.
    TR::AgentCall llama = [&](const B::ConnectionProfile& maquina,
                              const std::vector<std::string>& args, int timeoutMs, std::string& out,
                              std::string& err, int& rc) {
        std::string motivo;
        return ejecutarAgente(*e.ses, maquina, args, out, err, rc, &motivo, timeoutMs);
    };
    const TR::Job trabajo =
        !ficheroDestino.empty()
            ? TR::launchToFileJob(
                  e.ses->transporte, llama, *pOrigen, *pDestino, origen.zfsName(), ficheroDestino,
                  sobrescribir, base, banderasSend, mismaMaquina, e.ses->verboso,
                  comprimido ? zfsmgr::compresion::textoDe(compresion) : std::string())
        : !ficheroOrigen.empty()
            ? TR::launchFromFileJob(e.ses->transporte, llama, *pOrigen, *pDestino, ficheroOrigen,
                                    destino.dataset, mismaMaquina, e.ses->verboso)
            : TR::launchJob(e.ses->transporte, llama, *pOrigen, *pDestino, origen.zfsName(),
                            destino.dataset, base, banderasSend, testigo, mismaMaquina,
                            e.ses->verboso);
    if (!trabajo.ok()) {
        const std::string detalle = B::trim(trabajo.detail);
        std::fprintf(stderr, "%s%s%s\n", TR::labelOf(trabajo.failure).c_str(),
                     detalle.empty() ? "" : ": ", detalle.c_str());
        e.ultimoRc = 1;
        return false;
    }
    const std::string jobId = trabajo.id;
    e.ultimoRc = 0;
    std::fprintf(stdout, "%s\n", jobId.c_str());
    std::fprintf(stderr,
                 TC("t_transferen_07aa2e", "transferencia en marcha como trabajo %s en %s\n"),
                 jobId.c_str(), origen.connection.c_str());

    // El trabajo es de la MÁQUINA de origen. Con un fichero, la URL del origen es una ruta, y
    // `job --on <ruta>` contestaría que no es una conexión.
    ZfsmUrl maquinaOrigen;
    maquinaOrigen.kind = ZfsmKind::Connection;
    maquinaOrigen.connection = origen.connection;
    if (!pet.tiene("--wait")) {
        std::fprintf(stderr, TC("t_siga_con_j_2debd4", "siga con «job %s --on %s»\n"),
                     jobId.c_str(),
                     textoDe(ficheroOrigen.empty() ? origen : maquinaOrigen).c_str());
        return true;
    }
    // Con --wait se espera aquí. Se sondea cada dos segundos: más a menudo es carga sin
    // información nueva, porque el daemon actualiza el avance a ese ritmo.
    std::string ultimo;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::string est;
        if (!agente(e, maquinaOrigen, PET::jobStatus(jobId), est, 20000)) {
            return false;
        }
        const auto k = clavesDe(est);
        const std::string estado = k.count("STATE") ? k.at("STATE") : std::string();
        const std::string linea = k.count("PROGRESS_LINE") ? k.at("PROGRESS_LINE") : std::string();
        if (!linea.empty() && linea != ultimo) {
            ultimo = linea;
            std::fprintf(stderr, "  %s\n", linea.c_str());
        }
        if (estado != "running") {
            const std::string err = errorDeTrabajo(k);
            std::fprintf(stderr, TC("t_trabajo_estado_fin", "trabajo %s: %s%s\n"), jobId.c_str(),
                         estado.c_str(), err.empty() ? "" : (" — " + err).c_str());
            e.ultimoRc = (estado == "done" || estado == "finished") ? 0 : 1;
            if (e.ultimoRc == 0 && !ficheroDestino.empty()) {
                // Comprimido, el fichero no mide lo que se mandó: basta con que exista con su
                // nombre, que el receptor solo le da al flujo entero y bien comprimido.
                const long long bytes = comprimido         ? -1
                                        : k.count("BYTES") ? std::atoll(k.at("BYTES").c_str())
                                                           : -1;
                if (!ficheroCompleto(e, destino, ficheroDestino, bytes)) {
                    e.ultimoRc = 1;
                }
            }
            return e.ultimoRc == 0;
        }
    }
}

// --- Instalar o actualizar el daemon.
//
// Se sube el binario NATIVO y se instala con el arranque propio de cada sistema: systemd
// en Linux, launchd en macOS, rc.d en FreeBSD, tarea programada en Windows. El binario
// viaja por la ENTRADA ESTÁNDAR en Unix; en Windows va por scp, porque PowerShell no
// devuelve de ReadToEnd() con megabytes y la instalación se quedaba colgada.
//
// **No hay respaldo por guion.** Si no está el binario de esa plataforma, no se instala
// nada: un agente de guion no habla TLS, y dejarlo puesto da una máquina que PARECE
// atendida y no lo está.
// Los trabajos que el daemon de esa máquina tiene corriendo o encolados, por identificador.
// Vacío si no hay ninguno o si no contesta: una máquina sin daemon no tiene trabajos.
std::vector<std::string> trabajosEnCurso(Estado& e, const B::ConnectionProfile& p) {
    std::vector<std::string> ids;
    ZfsmUrl u;
    u.kind = ZfsmKind::Connection;
    u.connection = p.id.empty() ? p.name : p.id;
    std::string out;
    std::string err;
    int rc = -1;
    if (!ejecutarAgente(*e.ses, p, PET::jobList(), out, err, rc, nullptr, 15000) || rc != 0) {
        return ids;
    }
    return H::activeJobIds(out);
}

}  // namespace zfsmgr::cli::detail
