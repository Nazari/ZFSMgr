// Copying files: `cp`, `rsync`; and `labelclear` and `info`.

#include "shell_internal.h"

namespace zfsmgr::cli::detail {

// --- `cp`: copiar ficheros y directorios, de donde sea a donde sea.
//
// Lo que no hacía ninguna de las tres órdenes que ya copian: `rsync` exige datasets en los
// dos extremos, `todir` saca de un dataset a un directorio LOCAL, y `fromdir` mete un
// directorio en el dataset actual por una tubería tar. Entre las tres no se podía copiar de
// una ruta suelta a otra, ni un fichero suelto.
//
// No se inventa motor: dentro de una máquina va por el `rsync` del daemon y entre dos por el
// árbol entre agentes, que es lo que ya usan esas órdenes. Lo que aporta esto es el
// DIRECCIONAMIENTO uniforme —la regla vive en `base::copyspec`, con pruebas— y la semántica
// de copiar en vez de espejar.
bool cmdCp(Estado& e, const LineaAnalizada& linea) {
    namespace CS = B::copyspec;
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    // `cp <origen>... <destino>`: varios orígenes, como el `cp` de toda la vida. Es también
    // lo que recibe de la rama de patrones (`cp *.log destino`), que antes llamaba una vez
    // por origen y dejaba un TRABAJO por fichero.
    const std::vector<std::string> args = pet.lista("texto");
    if (args.size() < 2) {
        std::fputs(TC("t_cp_uso_err2", "uso: cp <origen>... <destino> [--as <nombre>]\n"), stderr);
        return false;
    }
    const std::vector<std::string> textosOrigen(args.begin(), args.end() - 1);
    if (textosOrigen.size() > 1 && !pet.valor("as").empty()) {
        std::fputs(TC("t_cp_as_varios",
                      "--as pone UN nombre, y el origen casa con varios: no tiene sentido\n"),
                   stderr);
        e.ultimoRc = 1;
        return false;
    }

    // Cada extremo: se resuelve como URL y se le pasa a la regla junto con el texto crudo.
    // El texto crudo hace falta porque la barra inicial manda —`/var/tmp/x` es una ruta del
    // sistema de ficheros y no el árbol `zfsm://var/tmp/x`— y eso solo se ve en lo escrito.
    const std::string idLocal = idDe(e, ZfsmUrl{});
    const auto extremo = [&](const std::string& texto, const char* cual, CS::End& out) -> bool {
        ZfsmUrl u;
        std::string err;
        if (texto.empty() || texto.front() != '/') {
            if (!resuelve(e, texto, u, err)) {
                std::fprintf(stderr, "%s: %s\n", cual, err.c_str());
                return false;
            }
        }
        const CS::Reading r =
            CS::readEnd(texto, u, idLocal.empty() ? std::string("local") : idLocal);
        if (!r.ok()) {
            std::fprintf(stderr, "%s: %s\n", cual, CS::labelOf(r.failure).c_str());
            return false;
        }
        out = r.end;
        return true;
    };
    // La ruta ABSOLUTA de cada extremo. Un dataset no la tiene hasta que se le pregunta a la
    // máquina dónde está montado: el `mountpoint` de la propiedad no siempre es donde se
    // puede abrir —en Windows dice «/pool/ds», que no existe— y por eso se pregunta.
    const auto rutaReal = [&](const CS::End& x, std::string& out, bool paraEscribir) -> bool {
        if (x.kind == CS::EndKind::PlainPath) {
            out = x.absolute;
            return true;
        }
        ZfsmUrl u;
        u.kind = ZfsmKind::Dataset;
        u.connection = x.connection;
        u.dataset = x.zfsName;
        u.pool = B::split(x.zfsName, "/", false).front();
        std::string base;
        if (!montajeDe(e, u, base, paraEscribir)) {
            return false;
        }
        out = base;
        if (!x.relative.empty()) {
            out += (out.empty() || out.back() == '/' ? "" : "/") + x.relative;
        }
        return true;
    };

    CS::End destino;
    std::string rutaDestino;
    if (!extremo(args.back(), TC("t_cp_destino", "destino"), destino)
        || !rutaReal(destino, rutaDestino, /*paraEscribir=*/true)) {
        return false;
    }

    // **Lo que hace `cp` de toda la vida según lo que haya en el destino:**
    //   - un directorio: se copia DENTRO, con su nombre (`cp a dir` deja `dir/a`);
    //   - nada: el destino ES la copia. Un fichero se copia con ese nombre, y un directorio
    //     crea ese directorio con su contenido. Su padre sí tiene que existir;
    //   - un fichero: un fichero lo sobrescribe; un directorio no puede.
    // Con varios orígenes el destino tiene que ser un directorio que exista.
    //
    // Antes el destino era SIEMPRE el contenedor: si no existía, el `cp` de un fichero
    // fallaba con «no es un directorio» y el de un directorio creaba `destino/origen`.
    std::string llegaComo;          // el nombre lo da el destino, no el origen
    bool destinoEsFichero = false;  // y lo que hay ahí es un fichero que se sobrescribe
    {
        bool existe = false;
        bool esDir = false;
        if (!estadoDeRuta(e, destino.connection, rutaDestino, existe, esDir)) {
            return false;
        }
        if (!existe || !esDir) {
            if (textosOrigen.size() > 1) {
                std::fprintf(stderr,
                             TC("t_cp_varios_sin_dir",
                                "%s no es un directorio: con varios orígenes el destino tiene que "
                                "ser un directorio que exista\n"),
                             rutaDestino.c_str());
                e.ultimoRc = 1;
                return false;
            }
            if (!pet.valor("as").empty()) {
                std::fprintf(stderr,
                             TC("t_cp_as_sin_dir",
                                "%s no es un directorio: el nombre ya lo da el destino, «--as» "
                                "sobra\n"),
                             rutaDestino.c_str());
                e.ultimoRc = 1;
                return false;
            }
            std::string padre;
            std::string hoja;
            bool padreExiste = false;
            bool padreEsDir = false;
            if (!padreYHoja(rutaDestino, padre, hoja)) {
                std::fprintf(stderr, TC("t_cp_destino_sin_nombre", "no se puede copiar sobre %s\n"),
                             rutaDestino.c_str());
                e.ultimoRc = 1;
                return false;
            }
            if (!estadoDeRuta(e, destino.connection, padre, padreExiste, padreEsDir)) {
                return false;
            }
            if (!padreExiste || !padreEsDir) {
                std::fprintf(stderr, TC("t_cp_padre_no_existe", "no existe el directorio %s\n"),
                             padre.c_str());
                e.ultimoRc = 1;
                return false;
            }
            rutaDestino = padre;
            llegaComo = hoja;
            destinoEsFichero = existe;
        }
    }

    // Todos los orígenes resueltos ANTES de copiar nada: un error en el tercero no debe
    // dejar copiados los dos primeros.
    struct Copia {
        std::string maquina;
        std::string ruta;
        std::string nombre;
        std::string rutaFinal;
    };
    std::vector<Copia> copias;
    std::map<std::string, std::string> llegadas;
    for (const std::string& texto : textosOrigen) {
        CS::End origen;
        Copia c;
        if (!extremo(texto, TC("t_cp_origen", "origen"), origen)
            || !rutaReal(origen, c.ruta, /*paraEscribir=*/false)) {
            return false;
        }
        c.maquina = origen.connection;
        // Que el origen EXISTA, antes de lanzar nada: si no, el trabajo se encolaba y fallaba
        // después, o la copia local dejaba un destino a medias.
        {
            bool existe = false;
            bool esDir = false;
            if (!estadoDeRuta(e, c.maquina, c.ruta, existe, esDir)) {
                return false;
            }
            if (!existe) {
                std::fprintf(stderr, TC("t_cp_origen_no_existe", "no existe: %s\n"),
                             c.ruta.c_str());
                e.ultimoRc = 1;
                return false;
            }
            if (destinoEsFichero && esDir) {
                std::fprintf(stderr,
                             TC("t_cp_dir_sobre_fichero",
                                "no se puede sobrescribir el fichero %s con un directorio\n"),
                             (rutaDestino + "/" + llegaComo).c_str());
                e.ultimoRc = 1;
                return false;
            }
        }
        // Con qué nombre aterriza: el suyo (o el de `--as`) si el destino es un directorio, y
        // el del destino si no lo es.
        c.nombre = llegaComo.empty() ? CS::landingName(origen, pet.valor("as")) : llegaComo;
        if (c.nombre.empty()) {
            std::fputs(TC("t_cp_sin_nombre",
                          "no se puede averiguar con qué nombre copiarlo; "
                          "dígalo con «--as»\n"),
                       stderr);
            return false;
        }
        c.rutaFinal =
            rutaDestino + (rutaDestino.empty() || rutaDestino.back() == '/' ? "" : "/") + c.nombre;
        if (B::toLowerAscii(c.maquina) == B::toLowerAscii(destino.connection)
            && c.ruta == c.rutaFinal) {
            std::fputs(TC("t_cp_mismo", "el origen y el destino son el mismo sitio\n"), stderr);
            return false;
        }
        // Dos orígenes con el mismo nombre de llegada: el segundo pisaría al primero.
        const auto ya = llegadas.find(c.nombre);
        if (ya != llegadas.end()) {
            std::fprintf(stderr,
                         TC("t_cp_misma_llegada",
                            "«%s» y «%s» llegarían con el mismo nombre, «%s»: el segundo "
                            "pisaría al primero\n"),
                         ya->second.c_str(), texto.c_str(), c.nombre.c_str());
            e.ultimoRc = 1;
            return false;
        }
        llegadas[c.nombre] = texto;
        copias.push_back(c);
    }

    const bool simula = pet.tiene("--check");
    const bool esperar = pet.tiene("--wait");
    for (const Copia& c : copias) {
        std::fprintf(stderr, TC("t_cp_yendo", "%s  ->  %s\n"), c.ruta.c_str(), c.rutaFinal.c_str());
    }

    // **Todos en la misma máquina que el destino: UNA sola copia, UN solo trabajo.** La
    // carga del motor local ya admite varios pares; repartirla en una llamada por origen
    // dejaba tantos trabajos como ficheros, y un `cp` es una cosa.
    bool todosAqui = copias.size() > 1;
    for (const Copia& c : copias) {
        todosAqui = todosAqui && B::toLowerAscii(c.maquina) == B::toLowerAscii(destino.connection);
    }
    if (todosAqui) {
        std::vector<std::pair<std::string, std::string>> pares;
        for (const Copia& c : copias) {
            pares.emplace_back(c.ruta, c.rutaFinal);
        }
        const std::string carga = SY::copyLocalPayload(pares, /*borra=*/false, simula);
        if (carga.empty()) {
            std::fputs(
                TC("t_cp_rutas_mal", "las rutas de origen y destino tienen que ser absolutas\n"),
                stderr);
            return false;
        }
        ZfsmUrl maquina;
        maquina.kind = ZfsmKind::Connection;
        maquina.connection = destino.connection;
        const std::vector<std::string> argv = PET::copyLocal(carga);
        if (simula || esperar) {
            std::string out;
            if (!agente(e, maquina, argv, out, 0)) {  // sin plazo: mueve datos de verdad
                return false;
            }
            std::fprintf(stdout, "%s", out.c_str());
            return true;
        }
        return enviaComoTrabajo(e, maquina, argv);
    }

    // Entre máquinas, cada origen es su propio envío de agente a agente.
    std::size_t mal = 0;
    for (const Copia& c : copias) {
        if (!copiaArbol(e, c.maquina, c.ruta, destino.connection, rutaDestino, c.nombre, simula,
                        esperar)) {
            ++mal;
            if (copias.size() > 1) {
                std::fprintf(stderr, TC("t_patron_fallo_uno", "  falló: %s\n"), c.ruta.c_str());
            }
        }
    }
    if (copias.size() > 1) {
        std::fprintf(stderr, TC("t_patron_resumen", "%s: %zu bien, %zu fallaron\n"), "cp",
                     copias.size() - mal, mal);
    }
    if (mal > 0) {
        e.ultimoRc = 1;
    }
    return mal == 0;
}

bool cmdRsync(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& origen = pet.objetivo;
    if (pet.lista("destino").empty()) {
        std::fputs(TC("t_uso_rsync", "uso: rsync <destino> [--delete] [--check]\n"), stderr);
        return false;
    }
    // **`#content` restringe el subárbol, y `{a,b}` nombra varios.**
    //
    // `rsync` trabaja sobre FICHEROS, no sobre datasets: el dataset solo dice dónde empieza
    // el árbol. Por eso la sección que admite es `content` —la que nombra ese árbol— y lo
    // que va detrás acota la parte que se sincroniza:
    //
    //     zfsm://c/pool/ds#content/sub          solo «sub»
    //     zfsm://c/pool/ds#content/{a,b,dir}    esos tres, al mismo destino
    //
    // Las otras secciones no nombran ficheros y se rechazan en vez de ignorarse, que es lo
    // que se hacía: una URL con `#properties` sincronizaba el dataset entero sin decir nada.
    //
    // Y `#path`: una ruta suelta de la máquina, en cualquiera de los dos extremos.
    const auto seccionValida = [](const ZfsmUrl& u, const char* cual) {
        if (u.section.empty() || esSeccionDeFicheros(u)) {
            return true;
        }
        std::fprintf(
            stderr,
            TC("t_rsync_seccion", "rsync trabaja sobre ficheros: «#%s» no nombra ninguno (%s)\n"),
            u.section.c_str(), cual);
        return false;
    };
    if (!seccionValida(origen, TC("t_rsync_origen", "origen"))) {
        return false;
    }
    // Un dataset o una ruta suelta; no una máquina ni una instantánea a secas.
    const auto extremoValido = [](const ZfsmUrl& u) {
        if (esRutaSuelta(u) || (nodoDe(u) == Nodo::Dataset)) {
            return true;
        }
        std::fprintf(
            stderr,
            TC("t_rsync_extremo", "rsync necesita un dataset o una ruta «#path/…» (ahora: %s)\n"),
            textoDe(u).c_str());
        return false;
    };
    if (!extremoValido(origen)) {
        return false;
    }
    ZfsmUrl destino;
    std::string err;
    if (!resuelve(e, pet.uno("destino"), destino, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return false;
    }
    if (!seccionValida(destino, TC("t_rsync_destino", "destino"))) {
        return false;
    }
    if (!extremoValido(destino)) {
        return false;
    }
    const bool mismaMaquina =
        B::toLowerAscii(origen.connection) == B::toLowerAscii(destino.connection);
    if (mismaMaquina && esRutaSuelta(origen) == esRutaSuelta(destino)
        && origen.zfsName() == destino.zfsName() && origen.detail == destino.detail) {
        std::fputs(TC("t_rsync_mismo", "el origen y el destino son el mismo dataset\n"), stderr);
        return false;
    }
    // La BASE de cada extremo y la ruta relativa a ella. En un dataset, su punto de montaje
    // y lo que va tras `#content`; en una ruta suelta, la raíz de la máquina («/», o la
    // unidad en Windows) y el resto de la ruta. Así las llaves y la comprobación de «..»
    // valen igual para las dos formas.
    const auto baseYRelativa = [&e](const ZfsmUrl& u, std::string& base, std::string& rel,
                                    bool paraEscribir) {
        if (!esRutaSuelta(u)) {
            rel = B::join(u.detail, "/");
            return montajeDe(e, u, base, paraEscribir);
        }
        std::vector<std::string> tramos = u.detail;
        base = "/";
        if (!tramos.empty() && CS::isDriveSegment(tramos.front())) {
            base = tramos.front() + "/";
            tramos.erase(tramos.begin());
        }
        rel = B::join(tramos, "/");
        return true;
    };
    std::string baseOrigen;
    std::string baseDestino;
    std::string relOrigen;
    std::string relDestinoBase;
    if (!baseYRelativa(origen, baseOrigen, relOrigen, /*paraEscribir=*/false)
        || !baseYRelativa(destino, baseDestino, relDestinoBase, /*paraEscribir=*/true)) {
        return false;
    }
    // Una ruta suelta de destino tiene que EXISTIR y ser un directorio, como en `cp`: no se
    // crea un árbol en cualquier sitio de la máquina por una errata.
    if (esRutaSuelta(destino)) {
        Ubicacion ub;
        if (!ubicacionDe(e, destino, ub)) {
            return false;
        }
        if (!ub.existe || !ub.esDirectorio) {
            std::fprintf(stderr, TC("t_no_es_directorio", "no es un directorio: %s\n"),
                         ub.ruta.c_str());
            e.ultimoRc = 1;
            return false;
        }
    }
    const bool borra = pet.tiene("--delete");
    const bool simula = pet.tiene("--check");

    // Los subárboles del origen. Con `#content/{a,b}` son varios; sin sección, uno solo que
    // es el árbol entero.
    // `detail` viene ya troceado por «/»; se rejunta para volver a tener la ruta relativa
    // tal y como se escribió, que es sobre la que actúan las llaves.
    const auto rutaDe = [&](const ZfsmUrl& u) {
        return &u == &origen ? relOrigen : relDestinoBase;
    };
    // Si un patrón ya las ha expandido (`#content/*.log`), esas; todas a UN solo rsync,
    // como hace la shell, para que `--delete` las considere juntas.
    std::vector<std::string> relativas =
        !e.rsyncRelativas.empty() ? e.rsyncRelativas : AV::contentPaths(relOrigen);
    // Las que vienen de un patrón sobre `#path` llevan la ruta entera desde la raíz; en
    // Windows, con la unidad delante, que es la base y no parte de la ruta relativa.
    if (esRutaSuelta(origen) && !e.rsyncRelativas.empty()) {
        for (std::string& rel : relativas) {
            if (rel.size() >= 2 && CS::isDriveSegment(rel.substr(0, 2))) {
                baseOrigen = rel.substr(0, 2) + "/";
                rel = rel.size() > 3 ? rel.substr(3) : std::string();
            }
        }
    }
    if (relativas.empty()) {
        std::fprintf(stderr,
                     TC("t_rsync_llaves_mal",
                        "«%s» no se entiende: las llaves se escriben «{a,b,c}», sin anidar y "
                        "sin piezas vacías\n"),
                     relOrigen.c_str());
        return false;
    }
    // Sincronizar la raíz entera de una máquina no es algo que nadie quiera por una errata.
    if (esRutaSuelta(origen) && relativas.size() == 1 && relativas.front().empty()) {
        std::fputs(TC("t_rsync_raiz", "rsync: el origen no puede ser la raíz de la máquina\n"),
                   stderr);
        return false;
    }
    const auto dentroDelArbol = [](const std::string& rel, const char* cual) {
        if (AV::isValidContentPath(rel)) {
            return true;
        }
        std::fprintf(stderr,
                     TC("t_rsync_ruta_fuera",
                        "«%s» sale del árbol del dataset (%s): las rutas de «#content» son "
                        "relativas y sin «..»\n"),
                     rel.c_str(), cual);
        return false;
    };
    for (const std::string& rel : relativas) {
        if (!dentroDelArbol(rel, TC("t_rsync_origen", "origen"))) {
            return false;
        }
    }
    if (!dentroDelArbol(rutaDe(destino), TC("t_rsync_destino", "destino"))) {
        return false;
    }

    // **`--delete` con varios orígenes está roto de partida**, y por eso se rechaza.
    //
    // La carga son PARES (origen, destino) y el daemon lanza un rsync por par: con `{a,b}`
    // la segunda pasada borraría en el destino lo que acaba de dejar la primera. Aceptarlo
    // sería prometer una sincronización y entregar la última carpeta a secas.
    // Con patrón cada una aterriza en SU subdirectorio, y ahí `--delete` es lo de rsync:
    // no hay dos pasadas sobre el mismo sitio.
    if (borra && relativas.size() > 1 && !e.rsyncConNombre) {
        std::fputs(TC("t_rsync_borrar_varios",
                      "«--delete» no se puede usar con varios orígenes: cada uno borraría lo "
                      "que dejó el anterior\n"),
                   stderr);
        return false;
    }

    const auto unir = [](const std::string& base, const std::string& rel) {
        if (rel.empty()) {
            return base;
        }
        return base + (base.empty() || base.back() == '/' ? "" : "/") + rel;
    };
    const std::string rutaDestino = unir(baseDestino, rutaDe(destino));
    std::vector<std::pair<std::string, std::string>> pares;
    for (const std::string& rel : relativas) {
        const std::size_t barra = rel.rfind('/');
        pares.emplace_back(
            unir(baseOrigen, rel),
            e.rsyncConNombre
                ? unir(rutaDestino, barra == std::string::npos ? rel : rel.substr(barra + 1))
                : rutaDestino);
    }
    // Confirmación solo si va a BORRAR: sin `--delete` esto añade y actualiza, y no hay
    // nada que perder. Con `--delete` el destino acaba idéntico al origen, borrado incluido.
    if (borra && !simula
        && !confirma(
            e, B::format(T("t_conf_rsync",
                           "%1 va a quedar IDÉNTICO a %2, borrando lo que sobre. ¿Continuar?"),
                         {rutaDestino, pares.front().first}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    // ── Entre máquinas: el árbol entre daemons ───────────────────────────────────
    //
    // `--mutate-copy-local` es local por definición, así que aquí no sirve. El transporte
    // es el mismo que usa «Desde Dir»: el destino escucha y el origen le manda el árbol
    // directamente, sin pasar por esta máquina.
    //
    // **Entre máquinas no es rsync**, aunque la orden se llame así: es la copia propia del
    // agente. Hace delta igual —compara tamaño y fecha—, y admite borrado y pasada en seco,
    // que es lo que esta orden promete.
    if (!mismaMaquina) {
        std::string errSrc;
        std::string errDst;
        const auto* pSrc = perfilDe(e, origen, errSrc);
        const auto* pDst = perfilDe(e, destino, errDst);
        if (!pSrc || !pDst) {
            std::fprintf(stderr, "%s\n", (pSrc ? errDst : errSrc).c_str());
            return false;
        }
        const B::ConnectionProfile src = conSudo(e, *pSrc);
        const B::ConnectionProfile dst = conSudo(e, *pDst);
        const auto llama = [&e](const B::ConnectionProfile& maquina,
                                const std::vector<std::string>& args, int timeoutMs,
                                std::string& salida, std::string& err, int& rc) {
            return ejecutarAgente(*e.ses, maquina, args, salida, err, rc, nullptr, timeoutMs);
        };
        bool todoBien = true;
        for (const auto& par : pares) {
            // Con patrón, cada uno aterriza con su nombre: un directorio en su
            // subdirectorio, y un fichero dentro del destino —el agente de origen lo manda
            // como un árbol de una entrada—. Sin patrón, el contenido va al destino tal cual.
            const bool fichero = e.rsyncConNombre && esFicheroEn(e, origen, par.first);
            std::string relDestino = rutaDe(destino);
            if (e.rsyncConNombre && !fichero) {
                relDestino = unir(relDestino, hojaDe(par.first));
            }
            // El directorio de destino tiene que EXISTIR: el receptor del árbol lo exige.
            // En un dataset, `--mutate-advanced-fromdir-prepare` lo monta, lo resuelve y lo
            // crea. En una ruta suelta ya se comprobó que existe; solo se crea, con
            // `--mutate-prepare-dir`, el subdirectorio con nombre de un patrón.
            std::string prepOut;
            std::string prepErr;
            int prepRc = -1;
            std::string dirPreparado;
            const std::string dirDestino = unir(baseDestino, rutaDe(destino));
            if (esRutaSuelta(destino) && relDestino == rutaDe(destino)) {
                dirPreparado = dirDestino;
            } else {
                const std::vector<std::string> prep =
                    esRutaSuelta(destino) ? AV::argvPrepareDir(dirDestino, hojaDe(par.first))
                                          : AV::argvFromDirPrepare(destino.zfsName(), relDestino);
                if (prep.empty()
                    || !ejecutarAgente(*e.ses, dst, prep, prepOut, prepErr, prepRc, nullptr, 60000)
                    || prepRc != 0) {
                    std::fputs(TC("t_rsync_destino_no", "no se pudo preparar el destino\n"),
                               stderr);
                    return false;
                }
                dirPreparado = B::trim(AV::preparedPath(prepOut));
            }
            std::string salidaEnvio;
            const auto hecho =
                TR::launchTreeJob(e.ses->transporte, llama, src, dst, par.first, dirPreparado,
                                  /*mismaConexion=*/false, e.ses->verboso, /*comoTrabajo=*/false,
                                  borra && !fichero, simula, &salidaEnvio);
            if (hecho.failure != TR::JobFailure::None_) {
                std::fprintf(stderr, "%s: %s\n", TR::labelOf(hecho.failure).c_str(),
                             hecho.detail.c_str());
                todoBien = false;
                break;
            }
            std::fputs(salidaEnvio.c_str(), stdout);
            avisaSinMetadatos(salidaEnvio);
        }
        if (!todoBien) {
            e.ultimoRc = 1;
        }
        return todoBien;
    }

    // Dentro de una máquina, la copia propia del agente: la misma que `cp`, con borrado y
    // pasada en seco. El `rsync` del sistema ya no se usa en ninguna parte.
    const std::string carga = SY::copyLocalPayload(pares, borra, simula);
    if (carga.empty()) {
        std::fputs(
            TC("t_rsync_rutas_mal", "las rutas de origen y destino tienen que ser absolutas\n"),
            stderr);
        return false;
    }
    const std::vector<std::string> argv = PET::copyLocal(carga);
    // Un `--check` es una simulación: no hay nada que mandar al daemon como trabajo, y lo
    // que uno quiere es LEER la salida ahora mismo.
    if (simula) {
        std::string out;
        if (!agente(e, origen, argv, out, 0)) {
            return false;
        }
        std::fprintf(stdout, "%s", out.c_str());
        return true;
    }
    return lanzaOEspera(e, pet, origen, argv);
}

// El detalle de un pool, un dataset, un volumen o un snapshot, en CAMPO/VALOR como el de la
// conexión. Un pool que no está importado pero aparece en la sonda se describe con lo que
// dice `zpool import`, dispositivos incluidos.
bool infoDeObjeto(Estado& e, const ZfsmUrl& destino) {
    const std::string objeto = destino.zfsName();
    Tabla t;
    t.cabecerasTexto = {T("t_cab_campo", "CAMPO"), T("t_cab_valor", "VALOR")};
    t.campos = {"field", "value"};
    t.filas.push_back({"url", textoDe(destino)});
    // Las propiedades que dicen QUÉ es y CÓMO está. Las demás siguen en `ls #properties`.
    const bool esSnapshot = !destino.snapshot.empty();
    const std::vector<std::string> claves =
        esSnapshot
            ? std::vector<std::string>{"type",          "creation", "used",   "referenced",
                                       "compressratio", "userrefs", "clones", "defer_destroy"}
            : std::vector<std::string>{
                  "type",        "creation",        "used",          "available",
                  "referenced",  "usedbysnapshots", "compressratio", "compression",
                  "dedup",       "encryption",      "keystatus",     "mountpoint",
                  "mounted",     "canmount",        "readonly",      "quota",
                  "reservation", "recordsize",      "volsize",       "origin"};
    std::string out;
    std::vector<L::Property> props;
    std::string errProps;
    std::string errZfs;
    const bool hayDataset =
        agente(e, destino, PET::specificProperties(claves, objeto), out, 20000, &errZfs)
        && L::properties(out, props, errProps) && !props.empty();
    // Un pool: además, lo que es del POOL (tamaño, salud, fragmentación…), de `zpool list`.
    const bool esPool = destino.dataset.empty() || destino.dataset == destino.pool;
    if (esPool && !esSnapshot) {
        std::string lista;
        B::json::Value raiz;
        if (agente(e, destino, PET::poolList(), lista) && B::json::parse(lista, raiz, nullptr)) {
            for (const auto& kv : raiz["pools"].toObject()) {
                if (kv.second["name"].toString(kv.first) != destino.pool) {
                    continue;
                }
                t.filas.push_back({"kind", "pool"});
                for (const auto& pr : kv.second["properties"].toObject()) {
                    t.filas.push_back({"pool." + pr.first, pr.second["value"].toString()});
                }
            }
        }
        // No importado: lo que dice la sonda.
        if (!hayDataset && t.filas.size() == 1) {
            e.ultimoRc = 0;
            H::ImportablePoolInfo imp;
            std::map<std::string, std::string> rutas;
            if (!poolImportable(e, subeHasta(destino, Objetivo::Conexion), destino.pool, imp,
                                rutas)) {
                std::fprintf(stderr,
                             TC("t_info_no_existe", "%s no es un pool importado ni importable\n"),
                             destino.pool.c_str());
                e.ultimoRc = 1;
                return false;
            }
            t.filas.push_back({"kind", "pool (importable)"});
            t.filas.push_back({"imported", "no"});
            t.filas.push_back({"guid", imp.guid});
            t.filas.push_back({"state", imp.state});
            if (!imp.reason.empty())
                t.filas.push_back({"status", imp.reason});
            if (!imp.action.empty())
                t.filas.push_back({"action", imp.action});
            for (const H::ImportableDevice& d : imp.devices) {
                if (!d.leaf) {
                    continue;
                }
                const auto ruta = rutas.find(d.name);
                t.filas.push_back(
                    {"device",
                     d.name + "  -> "
                         + (ruta != rutas.end() && !ruta->second.empty() ? ruta->second
                                                                         : std::string("?"))
                         + "  " + d.state + (d.detail.empty() ? "" : "  " + d.detail)});
            }
            t.imprime(e.formato);
            return true;
        }
    }
    if (!hayDataset) {
        if (!errZfs.empty()) {
            std::fprintf(stderr, "%s\n", errZfs.c_str());
        }
        e.ultimoRc = 1;
        return false;
    }
    if (!esPool || esSnapshot) {
        t.filas.push_back({"kind", esSnapshot ? "snapshot" : "dataset"});
    }
    for (const L::Property& pr : props) {
        if (pr.value.empty() || pr.value == "-") {
            continue;  // lo que no aplica (volsize en un sistema de ficheros…) no se enseña
        }
        t.filas.push_back({pr.name, pr.value});
    }
    t.imprime(e.formato);
    return true;
}

// --- `labelclear`: la etiqueta de ZFS de un dispositivo que ya no es de ningún pool.
bool cmdLabelclear(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    ZfsmUrl conexion = subeHasta(pet.objetivo, Objetivo::Conexion);
    // `labelclear zfsm://fc16#path/dev/sda9`: la URL da la máquina y el dispositivo. Con
    // `--on` además, tienen que coincidir.
    std::string dispositivo;
    std::string maquina;
    const bool conOn = !pet.valor("on").empty() || !pet.valor("from").empty();
    if (!rutaDeRanura(e, pet.uno("dispositivo"), conOn ? conexion.connection : std::string(),
                      dispositivo, &maquina)) {
        return false;
    }
    if (!maquina.empty() && B::toLowerAscii(maquina) != B::toLowerAscii(conexion.connection)) {
        conexion = ZfsmUrl{};
        conexion.kind = ZfsmKind::Connection;
        conexion.connection = maquina;
    }
    if (dispositivo.empty()
        || (dispositivo.front() != '/' && !(dispositivo.size() > 1 && dispositivo[1] == ':'))) {
        std::fputs(TC("t_labelclear_ruta",
                      "labelclear necesita la ruta del dispositivo, como "
                      "«/dev/sda9»\n"),
                   stderr);
        e.ultimoRc = 2;
        return false;
    }
    // Lo que se va a perder, con nombre: la confirmación no dice «¿seguro?» a secas.
    if (!confirma(e, B::format(T("t_conf_labelclear",
                                 "Se va a BORRAR la etiqueta de ZFS de %1 en %2. Si era de un "
                                 "pool, ese pool deja de poder importarse desde ese disco. "
                                 "¿Continuar?"),
                               {dispositivo, conexion.connection}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    std::string out;
    if (!agente(e, conexion, PET::labelClear(dispositivo, pet.tiene("-f")), out, 60000)) {
        return false;
    }
    e.importablesCache.clear();
    std::fprintf(stderr, TC("t_labelclear_hecho", "borrada la etiqueta de %s\n"),
                 dispositivo.c_str());
    return true;
}

bool cmdInfo(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (nodoDe(destino) == Nodo::Raiz) {
        std::fprintf(stdout, "kind\troot\nconnections\t%zu\n", e.conns.perfiles.size());
        return true;
    }
    // Un fichero o un directorio: qué es y a qué dataset pertenece.
    if (esSeccionDeFicheros(destino) && (esRutaSuelta(destino) || !destino.detail.empty())) {
        return infoDeRuta(e, destino);
    }
    // **Un pool, un dataset o un snapshot: SU detalle, no el de la conexión.** Antes `info`
    // daba siempre la salud del daemon, pidiera uno lo que pidiera.
    if (nodoDe(destino) != Nodo::Conexion) {
        return infoDeObjeto(e, destino);
    }
    std::string error;
    const auto* p = perfilDe(e, destino, error);
    if (!p) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    std::string out;
    if (!agente(e, destino, {"--health"}, out)) {
        return false;
    }
    // Una sola forma de presentarlo, la misma que `refresh`.
    //
    // Antes se escribían tres líneas propias en «clave<TAB>valor» y después se volcaba TAL
    // CUAL la respuesta del agente, que habla en «CLAVE=valor» y en mayúsculas. La misma
    // salida traía dos convenciones y una línea `CAPS=` de cuatrocientos caracteres por el
    // medio. Y con `--format tsv|json` lo volcado ni siquiera pasaba por el formateador,
    // así que no era analizable por un guion, que es justo para lo que están esos formatos.
    Tabla t;
    t.cabecerasTexto = {T("t_cab_campo", "CAMPO"), T("t_cab_valor", "VALOR")};
    t.campos = {"field", "value"};
    t.filas.push_back({"url", textoDe(destino)});
    t.filas.push_back({"connection", p->id});
    t.filas.push_back({"host", T::isLocalConnection(*p) ? "-" : p->host});
    for (const std::string& linea : B::split(out, "\n", true)) {
        const std::size_t igual = linea.find('=');
        if (igual == std::string::npos) {
            continue;
        }
        t.filas.push_back(
            {B::toLowerAscii(B::trim(linea.substr(0, igual))), B::trim(linea.substr(igual + 1))});
    }
    t.imprime(e.formato);
    return true;
}

// El ancho del terminal, para partir las descripciones de la ayuda. Sin poder
// averiguarlo se toman 80, que es lo que siempre se ha supuesto.
int anchoTerminal() {
    if (pantalla::activa()) {
        return static_cast<int>(pantalla::anchoTerminal());  // stdout es una tubería
    }
#ifndef _WIN32
    winsize w{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 40) {
        return w.ws_col;
    }
#else
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        const int ancho = info.srWindow.Right - info.srWindow.Left + 1;
        if (ancho > 40) {
            return ancho;
        }
    }
#endif
    return 80;
}

}  // namespace zfsmgr::cli::detail
