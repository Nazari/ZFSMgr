// The interpreter itself: running a line (with patterns) and the read-eval loop.

#include "shell_internal.h"

namespace zfsmgr::cli {

bool ejecutaConPatrones(Estado& e, const std::string& orden, const LineaAnalizada& original,
                        const std::function<bool(Estado&, const LineaAnalizada&)>& f) {
    LineaAnalizada an = original;
    if (orden == "install-daemon" && an.tiene("-a")) {
        return instalaDaemonEnTodas(e, an, f);
    }

    // --- Ejecutar sobre varios destinos que MODIFICAN: la lista exacta SIEMPRE —también con
    // -y, para que quede en el registro qué se hizo—, `--check` para verla sin tocar nada,
    // una sola confirmación, y una por una sin volver a preguntar.
    const auto modificaVarios = [&](const std::vector<std::string>& destinos,
                                    const std::string& texto,
                                    const std::function<bool(const std::string&)>& unoSolo) {
        std::fprintf(stderr, TC("t_patron_lista", "%s: %zu destinos casan con «%s»:\n"),
                     orden.c_str(), destinos.size(), texto.c_str());
        for (const std::string& d : destinos) {
            std::fprintf(stderr, "  %s\n", d.c_str());
        }
        if (an.opciones.count("check")) {
            std::fputs(TC("t_patron_check", "--check: no se ha tocado nada\n"), stderr);
            return true;
        }
        if (!confirma(e, B::format(T("t_patron_confirma", "¿%1 sobre estos %2?"),
                                   {orden, std::to_string(destinos.size())}))) {
            e.ultimoRc = 1;
            return false;
        }
        const bool asumiaSi = e.asumirSi;
        e.asumirSi = true;
        std::size_t bien = 0;
        std::size_t mal = 0;
        for (const std::string& d : destinos) {
            if (unoSolo(d)) {
                ++bien;
            } else {
                ++mal;
                std::fprintf(stderr, TC("t_patron_fallo_uno", "  falló: %s\n"), d.c_str());
            }
        }
        e.asumirSi = asumiaSi;
        std::fprintf(stderr, TC("t_patron_resumen", "%s: %zu bien, %zu fallaron\n"), orden.c_str(),
                     bien, mal);
        if (mal > 0) {
            e.ultimoRc = 1;
        }
        return mal == 0;
    };
    const auto sinZfsm = [](const std::string& url) {
        return B::startsWith(url, "zfsm://") ? url.substr(7) : url;
    };

    // --- El DESTINO de `rsync`, `send` y `diff`: un patrón vale si casa con UNO, como en la
    // shell. Va antes que el origen porque es igual para todas.
    for (auto it = an.ranuras.begin(); it != an.ranuras.end(); ++it) {
        if (it->first == "destino" && B::hasWildcard(it->second)) {
            std::string concreto;
            if (!expandeUnico(e, it->second, orden, false, concreto)) {
                e.ultimoRc = 1;
                return false;
            }
            it->second = concreto;
        }
    }

    // --- `cp`: los ORÍGENES se expanden a varios; el DESTINO, solo si casa con uno. Igual que
    // `cp *.log /tmp/` en la shell: cada origen aterriza en el destino con su nombre.
    if (orden == "cp") {
        const std::vector<std::string> args = an.lista("texto");
        if (args.size() != 2 || (!B::hasWildcard(args[0]) && !B::hasWildcard(args[1]))) {
            return f(e, an);  // sin patrones, o mal formado: que lo diga `cp`
        }
        std::string destino = args[1];
        if (B::hasWildcard(destino)
            && !expandeUnico(e, destino, orden, !destino.empty() && destino.front() == '/',
                             destino)) {
            e.ultimoRc = 1;
            return false;
        }
        std::vector<std::string> origenes;
        std::string err;
        if (!B::hasWildcard(args[0])) {
            origenes = {args[0]};
        } else if (args[0].front() == '/') {
            if (!expandeRutaLocal(args[0], origenes, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                e.ultimoRc = 1;
                return false;
            }
        } else {
            UrlCruda u;
            std::vector<DestinoConcreto> ds;
            if (!absolutaCruda(e, args[0], u, err) || !expandeDestinos(e, u, false, ds, err)) {
                if (err == "-") {
                    origenes.clear();
                } else {
                    if (!err.empty()) {
                        std::fprintf(stderr, "%s\n", err.c_str());
                    }
                    e.ultimoRc = 1;
                    return false;
                }
            }
            for (const DestinoConcreto& d : ds) {
                origenes.push_back(d.url);
            }
        }
        if (origenes.empty()) {
            std::fprintf(stderr, TC("t_nada_coincide", "nada coincide con «%s»\n"),
                         args[0].c_str());
            e.ultimoRc = 1;
            return false;
        }
        if (origenes.size() > 1 && an.opciones.count("as")) {
            std::fputs(TC("t_cp_as_varios",
                          "--as pone UN nombre, y el origen casa con varios: no tiene sentido\n"),
                       stderr);
            e.ultimoRc = 1;
            return false;
        }
        // Dos orígenes con el mismo nombre de llegada: el segundo pisaría al primero.
        std::map<std::string, std::string> llegadas;
        for (const std::string& o : origenes) {
            const std::string nombre = nombreDeLlegada(o);
            const auto ya = llegadas.find(nombre);
            if (ya != llegadas.end()) {
                std::fprintf(stderr,
                             TC("t_cp_misma_llegada",
                                "«%s» y «%s» llegarían con el mismo nombre, «%s»: el segundo "
                                "pisaría al primero\n"),
                             sinZfsm(ya->second).c_str(), sinZfsm(o).c_str(), nombre.c_str());
                e.ultimoRc = 1;
                return false;
            }
            llegadas[nombre] = o;
        }
        // Como `cp` en la shell, no se pide confirmación: copiar a un sitio no destruye lo
        // que se copia. Pero la lista sí se enseña, y `--check` sigue valiendo.
        if (origenes.size() > 1) {
            std::fprintf(stderr, TC("t_patron_lista", "%s: %zu destinos casan con «%s»:\n"),
                         orden.c_str(), origenes.size(), args[0].c_str());
            for (const std::string& o : origenes) {
                std::fprintf(stderr, "  %s\n", sinZfsm(o).c_str());
            }
        }
        // Todos de una vez a `cp`, que los junta en UNA copia si están en la misma máquina.
        LineaAnalizada an2 = an;
        an2.ranuras.erase("texto");
        for (const std::string& o : origenes) {
            an2.ranuras.emplace("texto", o);
        }
        an2.ranuras.emplace("texto", destino);
        return f(e, an2);
    }

    const auto it = an.opciones.find("on");
    const auto itFrom = an.opciones.find("from");
    const std::string texto = it != an.opciones.end()       ? it->second
                              : itFrom != an.opciones.end() ? itFrom->second
                                                            : an.objetivo;
    if (texto.empty() || !B::hasWildcard(texto)) {
        return f(e, an);
    }

    // --- El ORIGEN de `rsync`.
    if (orden == "rsync") {
        UrlCruda u;
        std::string err;
        if (!absolutaCruda(e, texto, u, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            e.ultimoRc = 1;
            return false;
        }
        bool enRuta = false;
        bool enContenido = false;  // con `*`, `?` o `[` en #content; las llaves solas no
        for (const PiezaCruda& pz : piezasDe(u)) {
            if (pz.clase == Pieza::Detalle) {
                enContenido = enContenido || pz.crudo.find_first_of("*?[") != std::string::npos;
            } else if (B::hasWildcard(pz.crudo)) {
                enRuta = true;
            }
        }
        // `#content/{a,b,dir}` sin comodines es lo que `rsync` ya entendía por sí mismo: tal cual.
        if (!enRuta && !enContenido) {
            return f(e, an);
        }
        if (enRuta && enContenido) {
            std::fprintf(stderr,
                         TC("t_rsync_patron_doble",
                            "un patrón en el dataset de origen Y en su contenido a la vez no "
                            "se admite: «%s»\n"),
                         texto.c_str());
            e.ultimoRc = 1;
            return false;
        }
        std::vector<DestinoConcreto> ds;
        if (!expandeDestinos(e, u, false, ds, err)) {
            if (err == "-") {
                std::fprintf(stderr, TC("t_nada_coincide", "nada coincide con «%s»\n"),
                             texto.c_str());
            } else if (!err.empty()) {
                std::fprintf(stderr, "%s\n", err.c_str());
            }
            e.ultimoRc = 1;
            return false;
        }
        if (enContenido) {
            // Como `rsync -a src/*.log src/s* dst/` en la shell: TODAS las rutas en UNA orden,
            // y cada una aterriza en el destino con su nombre —un fichero como fichero, un
            // directorio como subdirectorio—, así que `--delete` solo toca lo de cada una.
            std::vector<std::string> relativas;
            for (const DestinoConcreto& d : ds) {
                ZfsmUrl c;
                if (B::parseZfsmUrl(d.url, c, err)) {
                    relativas.push_back(B::join(c.detail, "/"));
                }
            }
            UrlCruda base = u;
            base.detalle.clear();
            LineaAnalizada an2 = an;
            an2.opciones.erase("on");
            an2.opciones.erase("from");
            an2.objetivo = base.texto();
            e.rsyncRelativas = relativas;
            e.rsyncConNombre = true;
            const bool ok = f(e, an2);
            e.rsyncRelativas.clear();
            e.rsyncConNombre = false;
            return ok;
        }
        // Varios datasets de origen al mismo destino: varios rsync seguidos. Con `--delete`,
        // cada uno borraría lo que dejó el anterior, así que se niega.
        if (ds.size() > 1 && an.opciones.count("delete")) {
            std::fprintf(stderr,
                         TC("t_rsync_delete_varios",
                            "con --delete y %zu orígenes, cada rsync borraría lo que copió el "
                            "anterior en el mismo destino: no se hace\n"),
                         ds.size());
            e.ultimoRc = 1;
            return false;
        }
        if (ds.size() == 1) {
            LineaAnalizada an2 = an;
            an2.opciones.erase("on");
            an2.opciones.erase("from");
            an2.objetivo = ds.front().url;
            return f(e, an2);
        }
        std::vector<std::string> urls;
        for (const DestinoConcreto& d : ds) {
            urls.push_back(sinZfsm(d.url));
        }
        return modificaVarios(urls, texto, [&](const std::string& etiqueta) {
            LineaAnalizada an2 = an;
            an2.opciones.erase("on");
            an2.opciones.erase("from");
            an2.opciones.erase("check");
            an2.objetivo = "zfsm://" + etiqueta;
            return f(e, an2);
        });
    }

    const auto& reglas = reglasDePatron();
    const auto regla = reglas.find(orden);
    if (regla == reglas.end()) {
        std::fprintf(stderr,
                     TC("t_patron_no_admite",
                        "«%s» no admite patrones en el destino: actúa sobre uno solo (%s)\n"),
                     orden.c_str(), texto.c_str());
        e.ultimoRc = 1;
        return false;
    }
    const Politica politica = regla->second.politica;

    // Una lista `{…}` va DENTRO de un tramo: no puede contener `/`. Se dice con todas las
    // letras, porque lo que sale si no —«patrón mal formado: «{server»»— no explica nada.
    {
        int nivel = 0;
        for (const char c : texto) {
            if (c == '{') {
                ++nivel;
            } else if (c == '}') {
                nivel = nivel > 0 ? nivel - 1 : 0;
            } else if (c == '/' && nivel > 0) {
                std::fprintf(stderr,
                             TC("t_patron_llave_barra",
                                "una lista {…} no puede contener «/»: cada tramo lleva la suya, "
                                "como en zfsm://{server,freebsd}/* (%s)\n"),
                             texto.c_str());
                e.ultimoRc = 1;
                return false;
            }
        }
    }

    UrlCruda u;
    std::string err;
    if (!absolutaCruda(e, texto, u, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        e.ultimoRc = 1;
        return false;
    }
    const std::vector<PiezaCruda> piezas = piezasDe(u);

    // En las que modifican, el patrón SOLO en el último tramo, y en la clase de pieza que
    // la orden permite. Se mira ANTES de preguntar nada a ninguna máquina.
    if (politica == Politica::VariosModifica) {
        for (std::size_t i = 0; i + 1 < piezas.size(); ++i) {
            if (B::hasWildcard(piezas[i].crudo)) {
                std::fprintf(stderr,
                             TC("t_patron_solo_ultimo",
                                "«%s» solo admite el patrón en el último tramo (%s): no se "
                                "abarcan varias máquinas o pools de una vez\n"),
                             orden.c_str(), texto.c_str());
                e.ultimoRc = 1;
                return false;
            }
        }
        if (piezas.empty() || regla->second.piezas.count(piezas.back().clase) == 0) {
            std::fprintf(
                stderr,
                TC("t_patron_pieza_no", "«%s» no admite un patrón en esa parte del destino (%s)\n"),
                orden.c_str(), texto.c_str());
            e.ultimoRc = 1;
            return false;
        }
    }

    std::vector<DestinoConcreto> destinos;
    if (!expandeDestinos(e, u, politica == Politica::Filtra, destinos, err)) {
        if (err == "-") {
            std::fprintf(stderr, TC("t_nada_coincide", "nada coincide con «%s»\n"), texto.c_str());
        } else if (!err.empty()) {
            std::fprintf(stderr, "%s\n", err.c_str());
        }
        e.ultimoRc = 1;
        return false;
    }

    bool vacio = false;  // la última sección se quedó sin filas tras el filtro
    const auto corre = [&](const DestinoConcreto& d, bool enVarios = false) {
        LineaAnalizada an2 = an;
        an2.opciones.erase("on");
        an2.opciones.erase("from");
        an2.opciones.erase("check");
        an2.objetivo = d.url.empty() ? std::string("/") : d.url;
        e.filtro = d.filtro;
        e.filtro.texto = texto;
        e.filtro.enVarios = enVarios;
        const bool ok = f(e, an2);
        vacio = e.filtro.quedoVacio;
        e.filtro = FiltroFilas{};
        return ok;
    };

    if (politica == Politica::Uno) {
        if (destinos.size() != 1) {
            std::fprintf(
                stderr,
                TC("t_patron_varios_uno", "«%s» necesita UN destino y «%s» casa con %zu:\n"),
                orden.c_str(), texto.c_str(), destinos.size());
            for (std::size_t i = 0; i < destinos.size() && i < 20; ++i) {
                std::fprintf(stderr, "  %s\n", destinos[i].etiqueta().c_str());
            }
            e.ultimoRc = 1;
            return false;
        }
        return corre(destinos.front());
    }

    if (politica == Politica::VariosModifica) {
        std::vector<std::string> etiquetas;
        std::map<std::string, DestinoConcreto> porEtiqueta;
        for (const DestinoConcreto& d : destinos) {
            etiquetas.push_back(d.etiqueta());
            porEtiqueta[d.etiqueta()] = d;
        }
        return modificaVarios(etiquetas, texto, [&](const std::string& etiqueta) {
            return corre(porEtiqueta[etiqueta]);
        });
    }

    // Filtra y Varios. Con un solo destino, tal cual.
    if (destinos.size() == 1) {
        return corre(destinos.front());
    }
    Acumulador acu;
    acumuladorActivo() = &acu;
    bool todoBien = true;
    std::size_t vacias = 0;
    for (const DestinoConcreto& d : destinos) {
        acu.destinoActual = d.etiqueta();
        CapturaSalida captura;
        bool ok = corre(d, /*enVarios=*/true);
        if (!ok && vacio) {
            ok = true;
            e.ultimoRc = 0;
            ++vacias;
        }
        const std::string suelto = captura.termina();
        if (!suelto.empty()) {
            // Lo que la orden imprimió a mano: en su sección, con el destino delante.
            acumuladorActivo() = nullptr;
            std::fprintf(stdout, "%s:\n%s%s\n", d.etiqueta().c_str(), suelto.c_str(),
                         suelto.back() == '\n' ? "" : "\n");
            acumuladorActivo() = &acu;
        }
        todoBien = todoBien && ok;
    }
    acumuladorActivo() = nullptr;
    if (vacias == destinos.size()) {
        std::fprintf(stderr, TC("t_nada_coincide", "nada coincide con «%s»\n"), texto.c_str());
        e.ultimoRc = 1;
        return false;
    }
    vuelcaAcumulador(acu, e.formato);
    if (!todoBien) {
        e.ultimoRc = 1;
    }
    return todoBien;
}

using Manejador = std::function<bool(Estado&, const LineaAnalizada&)>;

// The shell's commands, by name. The ones the line loop answers itself (exit, help, cls,
// master-password, pwd, format, yes) are in ejecutaLinea.
const std::map<std::string, Manejador>& ordenesDelInterprete() {
    static const std::map<std::string, Manejador> ordenes = {
        {"cd", cmdCd},
        {"ls", cmdLs},
        {"info", cmdInfo},
        {"create", cmdCreate},
        {"devices", cmdDevices},
        {"labelclear", cmdLabelclear},
        {"cp", cmdCp},
        {"rsync", cmdRsync},
        {"rename", cmdRename},
        {"destroy", cmdDestroy},
        {"mount", [](Estado& s, const LineaAnalizada& l) { return cmdMontaje(s, l, true); }},
        {"unmount", [](Estado& s, const LineaAnalizada& l) { return cmdMontaje(s, l, false); }},
        {"promote", cmdPromote},
        {"connect", [](Estado& s, const LineaAnalizada& l) { return cmdConectar(s, l, true); }},
        {"disconnect", [](Estado& s, const LineaAnalizada& l) { return cmdConectar(s, l, false); }},
        {"refresh", cmdRefrescar},
        {"edit", cmdEdit},
        {"holds", cmdHolds},
        {"hold", [](Estado& s, const LineaAnalizada& l) { return cmdRetencion(s, l, true); }},
        {"release", [](Estado& s, const LineaAnalizada& l) { return cmdRetencion(s, l, false); }},
        {"diff", cmdDiff},
        {"scrub",
         [](Estado& s, const LineaAnalizada& l) { return cmdMantenimientoPool(s, l, "scrub"); }},
        {"trim",
         [](Estado& s, const LineaAnalizada& l) { return cmdMantenimientoPool(s, l, "trim"); }},
        {"initialize",
         [](Estado& s, const LineaAnalizada& l) {
             return cmdMantenimientoPool(s, l, "initialize");
         }},
        {"flush",
         [](Estado& s, const LineaAnalizada& l) {
             return cmdPoolSimple(s, l, "flush", "sync", nullptr);
         }},
        {"upgrade",
         [](Estado& s, const LineaAnalizada& l) {
             return cmdPoolSimple(
                 s, l, "upgrade", "upgrade",
                 TC("t_aviso_pool_upgrade",
                    "Se va a subir la versión del pool, y eso NO se puede deshacer"));
         }},
        {"reguid",
         [](Estado& s, const LineaAnalizada& l) {
             return cmdPoolSimple(
                 s, l, "reguid", "reguid",
                 TC("t_aviso_pool_reguid", "Se va a cambiar el identificador único del pool"));
         }},
        // `clear` PREGUNTA. No borra datos, pero borra la cuenta de errores del pool, y se
        // teclea queriendo limpiar el terminal —dos veces en una misma sesión de pruebas—.
        // Perder eso sin haberlo pedido es perder justo lo que uno estaba mirando.
        {"clear",
         [](Estado& s, const LineaAnalizada& l) {
             return cmdPoolSimple(s, l, "clear", "clear",
                                  TC("t_aviso_pool_clear",
                                     "Se van a poner a cero los errores contados del pool (para "
                                     "limpiar la PANTALLA la orden es «cls»)"));
         }},
        // Las dos vacían la lista de importables del tabulador: lo que era importable ya no
        // lo es, y al revés.
        {"export",
         [](Estado& s, const LineaAnalizada& l) {
             s.importablesCache.clear();
             return cmdPoolSimple(s, l, "export", "export",
                                  TC("t_aviso_pool_export",
                                     "Se va a exportar el pool y dejará de estar disponible"));
         }},
        {"import",
         [](Estado& s, const LineaAnalizada& l) {
             s.importablesCache.clear();
             return cmdImport(s, l);
         }},
        {"status", cmdStatus},
        {"jobs", cmdJobs},
        {"job", cmdJob},
        {"install-daemon", cmdInstalarDaemon},
        {"send", cmdSend},
        {"history", cmdHistory},
        {"allow", [](Estado& s, const LineaAnalizada& l) { return cmdPermisos(s, l, true); }},
        {"unallow", [](Estado& s, const LineaAnalizada& l) { return cmdPermisos(s, l, false); }},
        {"rollback", cmdRollback},
        {"clone", cmdClone},
        {"get", cmdGet},
        {"set", cmdSet},
        {"inherit", cmdInherit},
        {"load-key",
         [](Estado& s, const LineaAnalizada& l) { return cmdClaves(s, l, "load-key"); }},
        {"unload-key",
         [](Estado& s, const LineaAnalizada& l) { return cmdClaves(s, l, "unload-key"); }},
        {"change-key",
         [](Estado& s, const LineaAnalizada& l) { return cmdClaves(s, l, "change-key"); }},
        {"schedule", cmdSchedule},
        {"schedules", cmdSchedules},
        {"log", cmdLog},
        {"peers", cmdPeers},
        {"accounts", cmdAccounts},
        {"repair-mounts", cmdRepairMounts},
        {"authorize-key", cmdAuthorizeKey},
        {"export-trust", cmdExportTrust},
        {"breakdown", cmdBreakdown},
        {"assemble", cmdAssemble},
        {"todir", cmdToDir},
        {"fromdir", cmdFromDir},
    };
    return ordenes;
}

// One line of the shell, as it was typed. Leaves its result in e.ultimoRc and returns whether,
// in a script, the line must stop the session (a syntax error, an unknown command, a command
// that failed): going on over a state that is not the expected one is how what nobody wanted
// to touch gets destroyed. The interactive loop and the service (--serve) both run lines
// through here.
bool ejecutaLinea(Estado& e, const std::string& linea, LectorDeLinea* lector) {
    const std::string recortada = B::trim(linea);
    if (recortada.empty() || recortada.front() == '#') {
        return false;
    }
    // Se ANALIZA con la gramática, una sola vez. Antes se troceaba como un shell POSIX
    // y cada orden repartía los trozos por su cuenta, con cuatro criterios distintos
    // conviviendo; de ahí salían los argumentos ignorados en silencio.
    if (lector) {
        lector->recuerda(recortada);
    }
    e.lineaEnCurso = recortada;
    e.puedeProponer = lector != nullptr && LectorDeLinea::editaEnTerminal();
    const LineaAnalizada an = analizaLinea(recortada);
    if (!an.error.empty()) {
        std::fprintf(stderr, "%s\n", an.error.c_str());
        e.ultimoRc = 2;
        // En un guion, un error de sintaxis DETIENE la sesión, como una orden
        // desconocida. Antes seguía con la línea siguiente y, al acabarse la entrada,
        // salía con 0: `set atime=off tank` —con el destino detrás— decía «uso: …» y el
        // guion se daba por bueno sin haber cambiado nada.
        return true;
    }
    if (an.vacia) {
        return false;
    }
    const std::string orden = B::toLowerAscii(an.verbo);
    const std::vector<std::string> args = an.lista("texto");

    if (orden == "exit" || orden == "quit") {
        e.salir = true;
        return false;
    }
    if (orden == "help" || orden == "?") {
        // `help load-key --on ..` enseña la ayuda y NO ejecuta nada. Se dice, porque lo
        // que se escribió detrás parece una orden que uno esperaba ver hecha.
        // `--on`/`--from` y las opciones no llegan como texto: se reconstruyen de lo
        // analizado para enseñar la orden tal y como se escribió.
        std::string resto;
        for (std::size_t i = 1; i < args.size(); ++i) {
            resto += " " + args[i];
        }
        if (!an.objetivo.empty()) {
            resto += " --on " + an.objetivo;
        }
        for (const auto& kv : an.opciones) {
            resto += " --" + kv.first + (kv.second.empty() ? "" : " " + kv.second);
        }
        for (const std::string& b : an.banderas) {
            resto += " " + b;
        }
        if (!args.empty() && !resto.empty()) {
            std::fprintf(stderr,
                         TC("t_help_no_ejecuta",
                            "help solo enseña la ayuda y no ejecuta nada: ¿quería «%s%s»?\n"),
                         args.front().c_str(), resto.c_str());
        }
        if (!args.empty()) {
            if (!imprimeAyudaDe(args.front(), anchoTerminal())) {
                std::fprintf(stderr, TC("t_no_hay_nin_fea411", "no hay ninguna orden «%s»\n"),
                             args.front().c_str());
            }
        } else {
            imprimeAyuda(anchoTerminal());
        }
        return false;
    }
    // Limpiar la pantalla. Con la secuencia ANSI y no llamando a `clear(1)`: no hay que
    // lanzar un proceso para esto, y en Windows ese binario no existe.
    //
    // Se borra también el HISTORIAL de desplazamiento —`3J`—, que es lo que uno espera
    // de limpiar: sin eso queda todo ahí arriba y basta con subir para verlo.
    if (orden == "cls") {
        if (pantalla::activa()) {
            pantalla::limpia();  // y lo guardado para repintar, que si no volvería
        } else if (hayTerminal()) {
            std::fputs("\033[H\033[2J\033[3J", stdout);
            std::fflush(stdout);
        }
        return false;
    }
    // Cambiar la clave maestra. La rotación la hace la capa base; aquí se piden las dos
    // claves sin que asomen por el argv y se recarga lo que la sesión tenía en memoria.
    if (orden == "master-password") {
        std::string vieja;
        std::string nueva;
        std::string err;
        const auto opcionDe = [&an](const char* clave) {
            const auto it = an.opciones.find(clave);
            return it == an.opciones.end() ? std::string() : it->second;
        };
        const std::string fdVieja = opcionDe("old-fd");
        const std::string fdNueva = opcionDe("new-fd");
        if (!fdVieja.empty()) {
            if (!leerSecretoDeDescriptor(std::atoi(fdVieja.c_str()), vieja, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                e.ultimoRc = 2;
                return false;
            }
        } else {
            vieja = e.ses->maestra;  // la de esta sesión: ya se comprobó al entrar
        }
        if (!fdNueva.empty()) {
            if (!leerSecretoDeDescriptor(std::atoi(fdNueva.c_str()), nueva, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                e.ultimoRc = 2;
                return false;
            }
        } else {
            if (!hayTerminal()) {
                std::fputs(TC("t_mp_sin_terminal",
                              "sin terminal: use --new-fd para dar la contraseña nueva\n"),
                           stderr);
                e.ultimoRc = 2;
                return false;
            }
            std::string otra;
            if (!preguntarSecretoPorTerminal(T("t_mp_p1", "Contraseña maestra nueva: "), nueva, err)
                || !preguntarSecretoPorTerminal(T("t_mp_p2", "Repítala: "), otra, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                e.ultimoRc = 2;
                return false;
            }
            if (nueva != otra) {
                std::fputs(TC("t_mp_no_coincide", "las dos no coinciden: no se ha cambiado nada\n"),
                           stderr);
                e.ultimoRc = 2;
                return false;
            }
        }
        if (nueva.empty()) {
            std::fputs(TC("t_mp_vacia", "la contraseña maestra no puede quedar vacía\n"), stderr);
            e.ultimoRc = 2;
            return false;
        }
        std::string copia;
        B::store::Warning aviso;
        if (!B::store::rotateMasterKey(e.ses->dirConfig, vieja, nueva, copia, aviso)) {
            std::fprintf(stderr, "%s\n", B::store::labelOf(aviso).c_str());
            e.ultimoRc = 1;
            return false;
        }
        // La sesión sigue abierta, así que su copia de la maestra tiene que moverse con
        // el fichero: si no, la siguiente escritura cifraría con la vieja.
        e.ses->maestra = nueva;
        recarga(e);
        std::fprintf(stdout,
                     TC("t_mp_ok", "contraseña maestra cambiada; copia de seguridad en «*%s»\n"),
                     copia.c_str());
        return false;
    }
    if (orden == "pwd") {
        std::fprintf(stdout, "%s\n", textoDe(e.actual).c_str());
        return false;
    }
    if (orden == "format") {
        if (args.empty()) {
            std::fprintf(stdout, "%s\n",
                         e.formato == Formato::Tsv    ? "tsv"
                         : e.formato == Formato::Json ? "json"
                                                      : "text");
            return false;
        }
        const std::string v = B::toLowerAscii(args.front());
        if (v == "tsv") {
            e.formato = Formato::Tsv;
        } else if (v == "json") {
            e.formato = Formato::Json;
        } else if (v == "text") {
            e.formato = Formato::Texto;
        } else {
            std::fprintf(
                stderr,
                TC("t_formato_de_9610b7", "formato desconocido: %s (use text, tsv o json)\n"),
                v.c_str());
        }
        return false;
    }
    if (orden == "yes") {
        e.asumirSi = args.empty() || B::toLowerAscii(args.front()) != "off";
        std::fputs(e.asumirSi ? TC("t_confirm_off", "confirmaciones desactivadas\n")
                              : TC("t_confirm_on", "confirmaciones activadas\n"),
                   stderr);
        return false;
    }

    const auto it = ordenesDelInterprete().find(orden);
    if (it == ordenesDelInterprete().end()) {
        // Basta con las primeras letras de una orden mientras no haya dos que empiecen
        // igual. Cuando las hay, decir «desconocida» es mentir: la orden existe, lo que
        // pasa es que hay varias. Se enumeran, que es lo que hace falta para elegir.
        // Del CATÁLOGO, no del mapa de este bucle: hay órdenes que se atienden antes de
        // llegar a él —`cls`, `format`, `yes`, `exit`— y no estarían en la lista. Es
        // además la misma función que usa el TABULADOR, así que lo que se ofrece al
        // completar y lo que se enumera al abreviar no pueden discrepar.
        std::vector<std::string> candidatas = nombresQueEmpiezanPor(orden);
        if (candidatas.size() > 1) {
            std::sort(candidatas.begin(), candidatas.end());
            std::string lista;
            for (const std::string& c : candidatas) {
                if (!lista.empty()) {
                    lista += ", ";
                }
                lista += c;
            }
            std::fprintf(stderr, TC("t_orden_ambigua", "«%s» es ambigua: %s\n"), orden.c_str(),
                         lista.c_str());
            e.ultimoRc = 127;
            return true;
        }
        std::fprintf(stderr, TC("t_orden_desc_b05fd4", "orden desconocida: %s (pruebe «help»)\n"),
                     orden.c_str());
        e.ultimoRc = 127;
        // En un guion, una orden que no existe DETIENE la sesión, igual que una que
        // falla. Antes seguía adelante y su 127 lo pisaba la siguiente orden que
        // saliera bien, así que una errata terminaba con código 0: el guion se daba por
        // bueno habiendo hecho otra cosa. Es el peor final posible para una errata.
        return true;
    }
    const bool bien = ejecutaConPatrones(e, orden, an, it->second);
    // Los montajes de solo lectura que ya no contienen la posición se sueltan aquí,
    // tras cada orden: un `ls` de un dataset sin montar no deja nada montado detrás.
    if (!e.montajesLectura.empty()) {
        const int rcOrden = e.ultimoRc;
        sueltaMontajesLectura(e, /*todos=*/false);
        e.ultimoRc = rcOrden;
    }
    if (!bien) {
        if (e.ultimoRc == 0) {
            e.ultimoRc = 1;
        }
        // En un guion —sin terminal— un fallo detiene la sesión: seguir ejecutando
        // órdenes sobre un estado que no es el previsto es cómo se destruye lo que no
        // se quería tocar.
        return true;
    }
    return false;
}

int ejecutarShell(Sesion& ses, Formato formato, const std::string& urlInicial, bool asumirSi,
                  bool compruebaProgramaciones, bool pantallaCompleta) {
    Estado e;
    // Al salir —por donde sea: `exit`, fin de la entrada o una orden que falla en un guion—
    // se sueltan los montajes de solo lectura que queden.
    struct AlSalir {
        Estado& e;
        ~AlSalir() {
            if (!e.montajesLectura.empty()) {
                sueltaMontajesLectura(e, /*todos=*/true);
            }
        }
    } alSalir{e};
    e.ses = &ses;
    e.formato = formato;
    e.asumirSi = asumirSi;
    recarga(e);
    if (!e.conns.aviso.empty()) {
        std::fprintf(stderr, "%s\n", e.conns.aviso.c_str());
    }

    // Se empieza en la RAÍZ, no en «Local».
    //
    // Antes se arrancaba en `zfsm://Local` por parecer el sitio útil, pero da una primera
    // impresión falsa: hace creer que esa máquina es el punto de partida obligado, y en un
    // cliente cuyo trabajo es gobernar VARIAS máquinas eso es lo contrario de lo que se
    // quiere enseñar. Desde la raíz, `ls` lista las conexiones y se ve el mapa entero antes
    // de bajar a ninguna; `cd local` sigue estando a una orden de distancia.
    //
    // Una URL dada en la línea de órdenes SÍ se respeta: quien la escribe sabe dónde quiere
    // empezar. Y por eso el aviso de fallo solo tiene sentido en ese caso —arrancar en la
    // raíz porque no se pidió otra cosa no es un error del que avisar—.
    //
    // Se resuelve con `resuelve()` y no con `parseZfsmUrl()` a secas para que el nombre de
    // la conexión quede normalizado a su IDENTIFICADOR desde el primer momento: si no, el
    // indicador diría «zfsm://Local» y «zfsm://local/...» tras el primer `cd`, que parecen
    // dos sitios distintos.
    if (urlInicial.empty()) {
        e.actual = ZfsmUrl{};
    } else {
        std::string errInicio;
        if (!resuelve(e, urlInicial, e.actual, errInicio)
            || buscarConexion(e.conns, e.actual.connection) == nullptr) {
            std::fprintf(stderr, TC("t_no_se_pudo_3b2240", "no se pudo empezar en %s: %s\n"),
                         urlInicial.c_str(), errInicio.c_str());
            e.actual = ZfsmUrl{};
        }
    }

    const bool interactivo = hayTerminal();
    // La pantalla: línea multilínea, repintado al cambiar el tamaño y zona de estado. Antes
    // que nada que escriba, para que entre en su historial.
    if (interactivo && pantallaCompleta) {
        pantalla::arranca();
    }
    struct ParaPantalla {
        ~ParaPantalla() { pantalla::para(); }
    } paraPantalla;
    // El estado de las programaciones, en segundo plano. Solo con terminal: en un guion o en
    // una tubería, una línea de más en la salida de error que nadie ha pedido estorba.
    // Con pantalla, la zona de estado (que lleva también el veredicto de las programaciones);
    // sin ella, la comprobación del arranque de siempre, en una línea.
    std::unique_ptr<ComprobacionDeArranque> comprobacion;
    std::unique_ptr<MonitorDeEstado> monitor;
    std::string huellaMonitor;
    if (interactivo && pantalla::activa() && !ses.sinSecretos) {
        monitor = std::make_unique<MonitorDeEstado>(ses, e.conns, compruebaProgramaciones);
        huellaMonitor = MonitorDeEstado::huella(e.conns);
    } else if (interactivo && compruebaProgramaciones && !ses.sinSecretos) {
        comprobacion = std::make_unique<ComprobacionDeArranque>(ses, e.conns);
    }
    if (interactivo) {
        std::fputs(TC("t_zfsmgr_cli_44809f",
                      "zfsmgr-cli — «help» lista las órdenes y «help <orden>» explica una.\n"
                      "El tabulador completa órdenes y URL; las flechas recorren el historial.\n"),
                   stderr);
    }

    LectorDeLinea lector;
    lector.setCompletador([&e](const std::string& l, std::size_t c, std::size_t& d, bool& sigue) {
        return completaEn(e, l, c, d, sigue);
    });

    std::string linea;
    std::string ultimaOrden;
    // La primera palabra de una línea: basta para saber de qué orden se trata.
    const auto recortadaPrimera = [](const std::string& l) {
        const std::string t = B::trim(l);
        const std::size_t sp = t.find(' ');
        return sp == std::string::npos ? t : t.substr(0, sp);
    };
    while (!e.salir) {
        if (monitor) {
            pantalla::sincroniza();
            // The connections changed under it: a new monitor over the current ones.
            if (const std::string h = MonitorDeEstado::huella(e.conns); h != huellaMonitor) {
                monitor.reset();
                monitor = std::make_unique<MonitorDeEstado>(ses, e.conns, compruebaProgramaciones);
                huellaMonitor = h;
            }
            monitor->trasOrden(e.ultimoRc, pantalla::ultimaLineaDeError(),
                               B::startsWith(ultimaOrden, "schedule"));
        }
        if (!lector.lee(textoDe(e.actual) + "> ", linea)) {
            break;
        }
        pantalla::marcaOrden();
        ultimaOrden = B::toLowerAscii(B::trim(recortadaPrimera(linea)));
        // Con la zona de estado, cada orden empieza en 0: las que se atienden en este mismo
        // bucle —`pwd`, `help`, `cls`— no lo tocan, y el error de la anterior seguiría a la
        // vista después de una que fue bien.
        if (monitor) {
            e.ultimoRc = 0;
        }
        if (ejecutaLinea(e, linea, &lector) && !interactivo) {
            return e.ultimoRc;
        }
        if (!e.lineaPropuesta.empty()) {
            lector.propone(e.lineaPropuesta);
            e.lineaPropuesta.clear();
        }
    }
    return 0;
}

}  // namespace zfsmgr::cli
