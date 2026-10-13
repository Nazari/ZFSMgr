// Encryption keys and the operations that move dataset contents: `breakdown`,
// `assemble`, `todir` and `fromdir`.

#include "shell_internal.h"

namespace zfsmgr::cli::detail {

bool cmdClaves(Estado& e, const LineaAnalizada& linea, const char* op) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (std::string(op) == "load-key") {
        // Antes de pedir NADA se mira el dataset. Si no, la frase se tecleaba para nada y
        // después salía el error crudo de `zfs`: en un dataset sin cifrar, con la clave ya
        // cargada, o en uno que hereda la clave de su raíz de cifrado.
        const auto prop = [&](const char* nombre) {
            std::string v;
            (void)agente(e, destino, PET::datasetProperty(nombre, destino.dataset), v);
            return B::trim(v);
        };
        const std::string cifrado = prop("encryption");
        if (cifrado.empty() || cifrado == "off" || cifrado == "-") {
            std::fprintf(stderr, TC("t_lk_sin_cifrar", "%s no está cifrado: no tiene clave\n"),
                         destino.dataset.c_str());
            e.ultimoRc = 1;
            return false;
        }
        // La clave de un dataset que hereda el cifrado es la de su RAÍZ de cifrado, y `zfs`
        // solo acepta cargarla ahí. Cargar esa es lo que se quiere decir y no borra ni cambia
        // nada, así que se hace diciéndolo.
        std::string raiz = prop("encryptionroot");
        if (raiz.empty() || raiz == "-") {
            raiz = destino.dataset;
        }
        ZfsmUrl objetivo = destino;
        if (raiz != destino.dataset) {
            std::fprintf(
                stderr,
                TC("t_lk_raiz", "la clave de %s es la de su raíz de cifrado, %s: se carga esa\n"),
                destino.dataset.c_str(), raiz.c_str());
            objetivo.dataset = raiz;
        }
        const auto propRaiz = [&](const char* nombre) {
            std::string v;
            (void)agente(e, objetivo, PET::datasetProperty(nombre, objetivo.dataset), v);
            return B::trim(v);
        };
        if (propRaiz("keystatus") == "available") {
            std::fprintf(stderr, TC("t_lk_ya_cargada", "la clave de %s ya está cargada\n"),
                         objetivo.dataset.c_str());
            return true;
        }
        // La frase de paso NUNCA por argumento: iría en `argv` y se vería en `ps` en las
        // dos máquinas. El verbo dedicado la lleva en base64 dentro de la petición RPC.
        //
        // Con `keylocation` en un fichero no hay nada que preguntar: `zfs` la lee de ahí y
        // no mira la entrada estándar.
        std::string frase;
        std::string err;
        const std::string dondeEsta = propRaiz("keylocation");
        if (dondeEsta == "prompt" || dondeEsta.empty() || dondeEsta == "-") {
            const std::string fd = pet.valor("password-fd");
            const bool leida = !fd.empty()
                                   ? leerSecretoDeDescriptor(std::atoi(fd.c_str()), frase, err)
                                   : pideSecreto(e, T("t_p_frase", "Frase de paso: "), frase, err);
            if (!leida) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return false;
            }
        } else {
            std::fprintf(stderr, TC("t_lk_de_fichero", "la clave se lee de %s\n"),
                         dondeEsta.c_str());
        }
        std::string out;
        const bool ok = agente(e, objetivo, PET::loadKey(objetivo.dataset, frase), out);
        for (char& c : frase) {
            c = '\0';
        }
        if (!ok) {
            return false;
        }
        std::fprintf(stderr, TC("t_clave_carg_09d2f4", "clave cargada en %s\n"),
                     objetivo.dataset.c_str());
        return true;
    }
    // Cambiar la frase. Va aparte de `zfsGeneric` por lo mismo que `load-key`: la frase no
    // puede ir en el argv, así que la lleva el verbo dedicado del daemon —cifrada dentro de
    // la petición— y allí se le entrega a `zfs` por la entrada estándar.
    if (std::string(op) == "change-key") {
        std::string frase;
        std::string err;
        const std::string fd = pet.valor("password-fd");
        if (!fd.empty()) {
            if (!leerSecretoDeDescriptor(std::atoi(fd.c_str()), frase, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return false;
            }
        } else {
            // Se pide DOS veces: sin eco, un dedazo dejaría el dataset con una frase que
            // nadie conoce, y eso no tiene vuelta atrás.
            std::string otra;
            if (!pideSecreto(e, T("t_p_frase_nueva", "Frase de paso nueva: "), frase, err)
                || !pideSecreto(e, T("t_p_frase_rep", "Repítala: "), otra, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return false;
            }
            if (frase != otra) {
                std::fputs(TC("t_frases_no_coinciden",
                              "las dos frases no coinciden; no se ha cambiado nada\n"),
                           stderr);
                for (char& c : frase) {
                    c = '\0';
                }
                for (char& c : otra) {
                    c = '\0';
                }
                return false;
            }
            for (char& c : otra) {
                c = '\0';
            }
        }
        if (frase.empty()) {
            std::fputs(TC("t_frase_vacia", "la frase de paso no puede estar vacía\n"), stderr);
            return false;
        }
        std::string out;
        const bool ok =
            agente(e, destino, PET::changeKey(destino.dataset, frase, std::string()), out);
        for (char& c : frase) {
            c = '\0';
        }
        if (!ok) {
            return false;
        }
        std::fprintf(stderr, TC("t_clave_cambiada", "cambiada la frase de paso de %s\n"),
                     destino.dataset.c_str());
        return true;
    }
    if (!zfsGeneric(e, destino, {op, destino.dataset})) {
        return false;
    }
    std::fprintf(stderr, TC("t_s_en_s_35a806", "%s en %s\n"), op, destino.dataset.c_str());
    return true;
}

// --- Las cuatro acciones de Avanzado.

// Lo que hay que decir antes de Desglosar o Ensamblar un dataset SIN MONTAR, y prepararlo.
//
// El agente lo monta en un temporal mientras dura y le devuelve después su punto de montaje
// (ver AltMountGuard en el daemon). Aquí se suelta antes el montaje de solo lectura que esta
// sesión pudiera tener sobre él —estando dentro con `cd`—, que estorbaría: el agente lo vería
// «montado» en un sitio donde no se puede escribir.
//
// Y en Desglosar, el aviso acordado con el usuario: si el padre tiene `mountpoint=/` y su
// pool se importa solo al arrancar esta máquina, `zfs mount -a` montaría los hijos nuevos
// —que heredan /usr, /var…— encima de los de este sistema. No se toca el pool: se dice, con
// las dos salidas.
std::string preparaMontajeTemporal(Estado& e, const ZfsmUrl& d, bool creaHijos, bool& rechaza) {
    rechaza = false;
    {
        std::vector<Estado::MontajeLectura> quedan;
        for (const auto& m : e.montajesLectura) {
            if (B::toLowerAscii(m.conexion) == B::toLowerAscii(d.connection)
                && m.dataset == d.dataset) {
                std::string out;
                std::string fallo;
                ZfsmUrl u;
                u.kind = ZfsmKind::Connection;
                u.connection = m.conexion;
                if (!agente(e, u, PET::unmountReadOnlyTemporary(m.dir), out, 30000, &fallo)) {
                    quedan.push_back(m);
                }
                continue;
            }
            quedan.push_back(m);
        }
        e.montajesLectura = quedan;
    }
    std::string montado;
    if (!agente(e, d, PET::datasetProperty("mounted", d.dataset), montado)
        || B::trim(montado) != "no") {
        return {};
    }
    std::string error;
    const auto* p = perfilVivoDe(e, d, error);
    if (!p || T::isWindowsConnection(*p)) {
        return {};
    }
    // Con canmount=off no se monta de ninguna forma, y suele ser un contenedor sin ficheros
    // propios: lo normal es que se quisiera uno de sus hijos. Se dice ANTES de confirmar,
    // no después de que el agente falle.
    std::string canmount;
    (void)agente(e, d, PET::datasetProperty("canmount", d.dataset), canmount);
    if (B::trim(canmount) == "off") {
        std::fputs(B::format(T("t_bd_canmount_off",
                               "%1 tiene canmount=off: no se puede montar y no guarda ficheros "
                               "propios. Si se refería a uno de sus hijos, indíquelo con «--on» "
                               "o entre en él con «cd».\n"),
                             {d.dataset})
                       .c_str(),
                   stderr);
        rechaza = true;
        return {};
    }
    std::string mp;
    (void)agente(e, d, PET::datasetProperty("mountpoint", d.dataset), mp);
    mp = B::trim(mp);
    std::string texto = B::format(T("t_bd_montaje_temporal",
                                    "%1 no está montado: se montará en un directorio temporal "
                                    "mientras dure la operación, y después se le devolverá su "
                                    "punto de montaje (%2)."),
                                  {d.dataset, mp});
    if (creaHijos) {
        texto += " "
                 + T("t_bd_hijos_heredan",
                     "Los datasets nuevos heredan el suyo (canmount=on) y quedan sin montar.");
    }
    if (creaHijos && mp == "/") {
        std::string props;
        std::string cache;
        B::json::Value v;
        if (agente(e, d, PET::poolProperties(d.pool), props, 20000)
            && B::json::parse(props, v, nullptr)) {
            cache = v["pools"][d.pool]["properties"]["cachefile"]["value"].toString();
        }
        if (cache.empty() || cache == "-") {
            texto +=
                "\n"
                + B::format(
                    T("t_bd_riesgo_arranque",
                      "AVISO: el pool %1 se importa solo al arrancar %2 (cachefile por omisión), y "
                      "entonces «zfs mount -a» montaría los datasets nuevos —que heredan /usr, "
                      "/var…— encima de los de ese sistema. Para evitarlo: impórtelo con «zpool "
                      "import -N %1», o quítelo de la caché con «zpool set cachefile=none %1»."),
                    {d.pool, d.connection});
        }
    }
    return texto;
}

bool cmdBreakdown(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (pet.lista("texto").size() < 2 || pet.lista("texto").size() % 2 != 0) {
        std::fputs(TC("t_uso_breakd_14be27",
                      "uso: breakdown <directorio> <nombre-hijo> [<directorio> <nombre-hijo>...]\n"
                      "  Convierte cada directorio del dataset en un dataset hijo.\n"),
                   stderr);
        return false;
    }
    bool rechaza = false;
    const std::string sinMontar = preparaMontajeTemporal(e, destino, /*creaHijos=*/true, rechaza);
    if (rechaza) {
        return false;
    }
    if (!confirma(
            e, (sinMontar.empty() ? std::string() : sinMontar + "\n")
                   + B::format(T("t_conf_breakdown",
                                 "Se van a convertir %1 directorios de %2 en datasets hijos. "
                                 "¿Continuar?"),
                               {std::to_string(pet.lista("texto").size() / 2), destino.dataset}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    // La lista llega en pares (subdirectorio, dataset nuevo); `argvBreakdown` los empareja y
    // descarta entero el que venga a medias, que es lo que desplazaría a todos los demás.
    std::vector<AV::Breakdown> pares;
    const auto& xs = pet.lista("texto");
    for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
        pares.push_back(AV::Breakdown{xs[i], xs[i + 1]});
    }
    const std::vector<std::string> argv = AV::argvBreakdown(destino.dataset, pares);
    if (argv.empty()) {
        std::fputs(TC("t_nada_que_desglosar",
                      "los subdirectorios van en pares: <subdir> <dataset-nuevo>\n"),
                   stderr);
        return false;
    }
    return lanzaOEspera(e, pet, destino, argv);
}

bool cmdAssemble(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (pet.lista("texto").empty()) {
        std::fputs(TC("t_uso_assemb_8aa75c",
                      "uso: assemble <hijo> [<hijo>...]\n"
                      "  Deshace lo de breakdown: devuelve cada dataset hijo a directorio.\n"),
                   stderr);
        return false;
    }
    bool rechaza = false;
    const std::string sinMontar = preparaMontajeTemporal(e, destino, /*creaHijos=*/false, rechaza);
    if (rechaza) {
        return false;
    }
    if (!confirma(e,
                  (sinMontar.empty() ? std::string() : sinMontar + "\n")
                      + B::format(T("t_conf_assemble",
                                    "Se van a reintegrar %1 datasets hijos en %2 como "
                                    "directorios. ¿Continuar?"),
                                  {std::to_string(pet.lista("texto").size()), destino.dataset}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    // La regla de los nombres completos vive en `commands::advanced`, con su porqué y sus
    // pruebas. Aquí estaba escrita en un comentario, y resuelta de otra manera en la
    // interfaz.
    const std::vector<std::string> argv = AV::argvAssemble(destino.dataset, pet.lista("texto"));
    if (argv.empty()) {
        std::fputs(TC("t_nada_que_ensamblar", "ninguno de los nombres dados sirve\n"), stderr);
        return false;
    }
    return lanzaOEspera(e, pet, destino, argv);
}

bool cmdToDir(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (pet.lista("ruta").empty()) {
        std::fputs(TC("t_uso_todir__d626ba",
                      "uso: todir <directorio-destino> [--delete-source]\n"
                      "  Vuelca el contenido del dataset a un directorio.\n"),
                   stderr);
        return false;
    }
    const bool borraOrigen = pet.tiene("--delete-source");
    // El directorio puede venir como `zfsm://<misma máquina>#path/…`: `todir` vuelca dentro
    // de la máquina del dataset, así que otra máquina es un error.
    std::string dirDestino;
    if (!rutaDeRanura(e, pet.uno("ruta"), destino.connection, dirDestino)) {
        return false;
    }
    if (!confirma(e, B::format(T("t_conf_todir", "Se va a volcar %1 a %2%3. ¿Continuar?"),
                               {destino.dataset, dirDestino,
                                borraOrigen ? T("t_conf_y_destruir_origen",
                                                " y DESTRUIR el dataset de origen")
                                            : std::string()}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    const std::vector<std::string> argvTodir =
        AV::argvToDir(destino.dataset, dirDestino, borraOrigen);
    if (argvTodir.empty()) {
        std::fputs(
            TC("t_ruta_no_absoluta", "el directorio de destino tiene que ser una ruta absoluta\n"),
            stderr);
        return false;
    }
    return lanzaOEspera(e, pet, destino, argvTodir);
}

// `fromdir` NO es la inversa de `todir`, aunque el nombre lo sugiera.
//
// Es «Desde Dir»: crea el contenido de un dataset A PARTIR de uno o varios directorios,
// que pueden estar en OTRA máquina. Por eso no puede ser un RPC como sus tres hermanas: el
// verbo del agente lee un tar por la entrada estándar y el canal RPC no tiene stdin —lo
// dice el propio daemon en su comentario—. Se arma la misma tubería que la interfaz:
//
//     <ssh origen> tar -c ...  |  <ssh destino> zfsmgr-agent --mutate-advanced-fromdir
//
// Se ejecuta EN ESTA máquina, que es la que tiene las credenciales de las dos puntas.
bool cmdFromDir(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    if (pet.lista("ruta").empty()) {
        std::fputs(TC("t_uso_fromdi_d4b854",
                      "uso: fromdir <directorio-origen> [--from <url-origen>] [--subdir <rel>]\n"
                      "  Vuelca un directorio dentro del dataset actual. El origen puede estar\n"
                      "  en otra máquina: --from acepta cualquier URL zfsm://.\n"),
                   stderr);
        return false;
    }
    // El DESTINO es donde estamos. `--from` nombra el origen, que aquí es otra máquina y no
    // otro sitio: es la única orden en la que las dos opciones no son sinónimas.
    ZfsmUrl destino = e.actual;
    if (!pet.valor("on").empty()) {
        std::string error;
        if (!resuelve(e, pet.valor("on"), destino, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
    }
    if (!exigeDataset(destino)) {
        return false;
    }
    std::string errDst;
    const auto* pDst = perfilDe(e, destino, errDst);
    if (!pDst) {
        std::fprintf(stderr, "%s\n", errDst.c_str());
        return false;
    }

    const B::ConnectionProfile* pSrc = pDst;
    ZfsmUrl origen = destino;
    // El origen como `zfsm://maq#path/dir`: la URL ya dice de qué máquina viene, y con ella
    // `--from` sobra. Si se dan las dos, tienen que nombrar la misma máquina.
    std::string dir;
    std::string maquinaDeLaRuta;
    if (!rutaDeRanura(e, pet.uno("ruta"), "", dir, &maquinaDeLaRuta)) {
        return false;
    }
    if (!maquinaDeLaRuta.empty() && pet.valor("from").empty()) {
        origen = ZfsmUrl{};
        origen.kind = ZfsmKind::Connection;
        origen.connection = maquinaDeLaRuta;
        std::string error;
        pSrc = perfilDe(e, origen, error);
        if (!pSrc) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
    }
    if (!pet.valor("from").empty()) {
        std::string error;
        if (!resuelve(e, pet.valor("from"), origen, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
        pSrc = perfilDe(e, origen, error);
        if (!pSrc) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
    }

    if (!maquinaDeLaRuta.empty() && !pet.valor("from").empty()
        && B::toLowerAscii(maquinaDeLaRuta) != B::toLowerAscii(origen.connection)) {
        std::fprintf(
            stderr, TC("t_fromdir_dos_maquinas", "el origen dice %s y --from dice %s: sobra uno\n"),
            maquinaDeLaRuta.c_str(), origen.connection.c_str());
        return false;
    }
    const std::string rel = pet.valor("subdir");
    // El subdirectorio se comprueba AQUÍ, antes de preguntar y antes de abrir la tubería.
    // Lo miraba solo el daemon, al otro extremo, y para entonces el tar del origen ya estaba
    // corriendo: la operación moría a mitad con parte del contenido ya fuera de su máquina.
    if (!AV::isValidRelativeSubdir(rel)) {
        std::fprintf(stderr,
                     TC("t_fromdir_subdir_malo",
                        "«%s» no sirve como subdirectorio: tiene que ser relativo al dataset y "
                        "sin «..»\n"),
                     rel.c_str());
        return false;
    }
    if (!confirma(e, B::format(T("t_conf_fromdir",
                                 "Se va a volcar %1 de %2 dentro de %3 en %4. ¿Continuar?"),
                               {dir, origen.connection, destino.dataset, destino.connection}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }

    const B::ConnectionProfile src = conSudo(e, *pSrc);
    const B::ConnectionProfile dst = conSudo(e, *pDst);

    // Sin compresión: elegirla exige saber qué hay instalado en LAS DOS puntas, que es una
    // sonda por máquina. La interfaz la hace porque ya tiene esos datos cacheados.
    const std::string tarOrigen =
        H::buildTarSourceCommand(T::isWindowsConnection(src), dir, H::StreamCodec::None);
    // ── Primero, sin tubería: el árbol entre daemons ─────────────────────────────
    //
    // El destino prepara el directorio y se pone a escuchar; el origen le manda el árbol
    // directamente. Los datos NO pasan por esta máquina, y la copia es incremental.
    //
    // Se INTENTA en vez de comprobarse antes: aquí no hay estado cacheado de qué máquina
    // tiene daemon —eso lo lleva la ventana—, y una llamada que falla dice exactamente lo
    // mismo que diría la comprobación, sin costar un viaje de más cuando sí se puede.
    {
        const std::vector<std::string> prepArgv = AV::argvFromDirPrepare(destino.dataset, rel);
        std::string pOut;
        std::string pErr;
        int pRc = -1;
        const bool preparado =
            !prepArgv.empty()
            && ejecutarAgente(*e.ses, dst, prepArgv, pOut, pErr, pRc, nullptr, 60000) && pRc == 0;
        const std::string dirDestino = preparado ? AV::preparedPath(pOut) : std::string();
        if (!dirDestino.empty()) {
            const auto llama = [&e](const B::ConnectionProfile& maquina,
                                    const std::vector<std::string>& args, int timeoutMs,
                                    std::string& salida, std::string& err, int& rc) {
                return ejecutarAgente(*e.ses, maquina, args, salida, err, rc, nullptr, timeoutMs);
            };
            std::string salidaEnvio;
            const auto hecho = TR::launchTreeJob(
                e.ses->transporte, llama, src, dst, dir, dirDestino,
                origen.connection == destino.connection, e.ses->verboso,
                /*comoTrabajo=*/false, /*borrarEnDestino=*/false, /*enSeco=*/false, &salidaEnvio);
            // `fallo` y no `ok()`: sin encolar no hay identificador, y `ok()` exige uno.
            if (hecho.failure == TR::JobFailure::None_) {
                std::fputs(salidaEnvio.c_str(), stdout);
                avisaSinMetadatos(salidaEnvio);
                return true;
            }
            // Que no se pueda por aquí no es un error de la orden: se sigue por la tubería,
            // que solo pide daemon en el destino. Se dice por qué, para que no parezca que
            // se eligió el camino lento porque sí.
            std::fprintf(stderr,
                         TC("t_fromdir_arbol_no",
                            "el árbol entre daemons no pudo (%s); se sigue "
                            "por la tubería\n"),
                         TR::labelOf(hecho.failure).c_str());
        }
    }

    // El argv lo compone el módulo, que es el mismo que usa la interfaz. La cadena se deriva
    // de él —nunca al revés—: aquí hace falta porque lo que se ejecuta es una tubería de
    // shell, y esa es justamente la punta que no puede ser un RPC.
    const std::vector<std::string> fdArgv = AV::argvFromDir(destino.dataset, rel);
    if (fdArgv.empty()) {
        std::fputs(TC("t_fromdir_destino_malo", "destino no válido para fromdir\n"), stderr);
        return false;
    }
    std::string recibe = B::daemonpayload::unixBinPath();
    for (const std::string& a : fdArgv) {
        recibe += " " + B::shSingleQuote(a);
    }

    // `sshExecFromLocal`: para una conexión LOCAL la orden se queda tal cual; para una SSH
    // se envuelve en `ssh host '...'`. Así la misma tubería vale para local->local,
    // local->remoto, remoto->local y remoto->remoto.
    const auto porSsh = [](const B::ConnectionProfile& p, const std::string& cmd) {
        if (T::isLocalConnection(p)) {
            return cmd;
        }
        return H::sshBaseCommand(p) + " " + B::shSingleQuote(H::sshUserHost(p)) + " "
               + B::shSingleQuote(T::wrapRemoteCommand(p, cmd));
    };
    const std::string tuberia = porSsh(src, H::withSudoCommand(src, tarOrigen)) + " | "
                                + porSsh(dst, H::withSudoStreamInputCommand(dst, recibe));

    // Se ejecuta EN ESTA máquina: es la que tiene las credenciales de las dos puntas.
    B::ConnectionProfile aqui;
    aqui.id = "local";
    aqui.connType = "LOCAL";
    aqui.osType = "Linux";
    std::string out;
    std::string err;
    int rc = -1;
    if (!T::runSsh(
            e.ses->transporte, aqui, tuberia, 0, out, err, rc,
            [](const std::string& l) { std::fprintf(stdout, "%s\n", l.c_str()); },
            [](const std::string& l) { std::fprintf(stderr, "%s\n", l.c_str()); }, {}, {},
            /*allowAgentRpc=*/false, /*echoOutputToLog=*/e.ses->verboso)
        || rc != 0) {
        std::fprintf(stderr, TC("t_fromdir_fa_67991a", "fromdir falló (código %d)\n"), rc);
        e.ultimoRc = rc == 0 ? 1 : rc;
        return false;
    }
    return true;
}

// --- Transferencias entre máquinas.
//
// El protocolo tiene DOS EXTREMOS y no es una tubería:
//
//   1. En el DESTINO, `--zfs-recv-listen <dataset> 1` abre un puerto y devuelve
//      `PORT=` y `TOKEN=`.
//   2. En el ORIGEN, `--zfs-send-to-peer-async <snap> <host> <puerto> <testigo> …` conecta
//      con ese puerto y devuelve un `JOB_ID=`.
//   3. El avance se consulta con `job <id>` en la máquina de ORIGEN.
//
// Que sea un trabajo y no una espera es lo que permite mandar terabytes y cerrar la
// sesión. Con `--wait` se espera aquí, sondeando.

// Que el fichero de destino de un `send` está entero en su sitio.
//
// El trabajo es del EMISOR, y que acabe bien dice que mandó todo, no que el otro lado lo
// guardara: el último tramo puede estar aún en el búfer del receptor cuando falla al
// escribirlo, y entonces el receptor borra el parcial sin que el emisor se entere. Así que
// con `--wait` se mira el fichero: que exista con su nombre —el receptor solo se lo da a un
// flujo entero— y que mida lo que se mandó. El nombre se lo pone tras cerrar, así que se le
// dan unos segundos.
bool ficheroCompleto(Estado& e, const ZfsmUrl& donde, const std::string& ruta, long long bytes) {
    long long tam = -1;
    for (int intento = 0; intento < 20; ++intento) {
        std::string out;
        B::json::Value v;
        if (agente(e, donde, PET::pathInfo(ruta), out, 20000) && B::json::parse(out, v, nullptr)
            && v["exists"].toBool()) {
            tam = v["size"].toInt();
            if (bytes < 0 || tam == bytes) {
                std::fprintf(stderr, TC("t_send_fichero_guardado", "guardado en %s (%s)\n"),
                             ruta.c_str(), B::humanSize(std::to_string(tam)).c_str());
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (tam < 0) {
        std::fprintf(stderr,
                     TC("t_send_fichero_no_quedo",
                        "el envío terminó, pero %s no está en %s: el receptor no pudo "
                        "guardarlo (su registro dice por qué)\n"),
                     ruta.c_str(), donde.connection.c_str());
    } else {
        std::fprintf(stderr, TC("t_send_fichero_corto", "%s mide %lld bytes y se mandaron %lld\n"),
                     ruta.c_str(), tam, bytes);
    }
    return false;
}

}  // namespace zfsmgr::cli::detail
