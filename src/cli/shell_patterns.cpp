// Patterns in URLs (`*`, `?`, `{a,b}`): expanding them into concrete targets.

#include "shell_internal.h"

namespace zfsmgr::cli {

UrlCruda parteCruda(const std::string& absoluta) {
    UrlCruda u;
    std::string resto = absoluta.substr(std::string("zfsm://").size());
    const auto sinVaciosFinales = [](std::vector<std::string> v) {
        while (!v.empty() && v.back().empty()) {
            v.pop_back();
        }
        return v;
    };
    const std::size_t almohadilla = resto.find('#');
    if (almohadilla != std::string::npos) {
        u.haySeccion = true;
        const std::vector<std::string> partes =
            sinVaciosFinales(B::split(resto.substr(almohadilla + 1), "/", false));
        resto = resto.substr(0, almohadilla);
        if (!partes.empty()) {
            u.seccion = partes.front();
            u.detalle.assign(partes.begin() + 1, partes.end());
        }
    }
    const std::size_t barra = resto.find('/');
    u.conexion = barra == std::string::npos ? resto : resto.substr(0, barra);
    std::string ruta = barra == std::string::npos ? std::string() : resto.substr(barra + 1);
    const std::size_t arroba = ruta.find('@');
    if (arroba != std::string::npos) {
        u.hayInstantanea = true;
        u.instantanea = ruta.substr(arroba + 1);
        ruta = ruta.substr(0, arroba);
    }
    if (!ruta.empty()) {
        u.ruta = sinVaciosFinales(B::split(ruta, "/", false));
    }
    return u;
}

// El destino que escribió el usuario, convertido en URL ABSOLUTA cruda: el sitio actual
// delante si es relativo, y los «.» y «..» aplicados sobre los tramos, sin descodificar.
bool absolutaCruda(const Estado& e, const std::string& textoEntrada, UrlCruda& out,
                   std::string& err) {
    const std::string t = B::trim(textoEntrada);
    if (B::startsWith(B::toLowerAscii(t), "zfsm://")) {
        out = parteCruda(t);
        return true;
    }
    if (!t.empty() && (t.front() == '@' || t.front() == '#')) {
        if (e.actual.connection.empty()) {
            err = T("t_falta_estar_ds", "hace falta estar en un dataset");
            return false;
        }
        ZfsmUrl limpia = e.actual;
        limpia.section.clear();
        limpia.detail.clear();
        if (t.front() == '@') {
            limpia.snapshot.clear();
        }
        out = parteCruda(B::formatZfsmUrl(limpia) + t);
        return true;
    }
    std::string cuerpo = t;
    std::string cola;
    const std::size_t almohadilla = cuerpo.find('#');
    if (almohadilla != std::string::npos) {
        cola = cuerpo.substr(almohadilla);
        cuerpo = cuerpo.substr(0, almohadilla);
    }
    std::vector<std::string> piezas;
    if (!cuerpo.empty() && cuerpo.front() == '/') {
        cuerpo = cuerpo.substr(1);
    } else if (!e.actual.connection.empty()) {
        ZfsmUrl base = e.actual;
        base.snapshot.clear();
        base.section.clear();
        base.detail.clear();
        const UrlCruda b = parteCruda(B::formatZfsmUrl(base));
        piezas.push_back(b.conexion);
        piezas.insert(piezas.end(), b.ruta.begin(), b.ruta.end());
    }
    for (const std::string& tr : B::split(cuerpo, "/", true)) {
        if (tr == ".") {
            continue;
        }
        if (tr == "..") {
            if (!piezas.empty()) {
                piezas.pop_back();
            }
            continue;
        }
        piezas.push_back(tr);
    }
    if (piezas.empty()) {
        out = UrlCruda{};
        return true;
    }
    out = parteCruda("zfsm://" + B::join(piezas, "/") + cola);
    return true;
}

std::vector<PiezaCruda> piezasDe(const UrlCruda& u) {
    std::vector<PiezaCruda> v;
    if (u.esRaiz()) {
        return v;
    }
    v.push_back({Pieza::Conexion, u.conexion});
    for (std::size_t i = 0; i < u.ruta.size(); ++i) {
        v.push_back({i == 0 ? Pieza::Pool : Pieza::Hijo, u.ruta[i]});
    }
    if (u.hayInstantanea) {
        v.push_back({Pieza::Instantanea, u.instantanea});
    }
    if (u.haySeccion) {
        v.push_back({Pieza::Seccion, u.seccion});
        for (const std::string& d : u.detalle) {
            v.push_back({Pieza::Detalle, d});
        }
    }
    return v;
}

// Expande el destino contra lo que existe. Con `filtraUltimo`, un patrón en la ÚLTIMA pieza
// no se expande: queda como filtro de filas del listado de su padre.
bool expandeDestinos(Estado& e, const UrlCruda& u, bool filtraUltimo,
                     std::vector<DestinoConcreto>& out, std::string& err) {
    out.clear();
    const std::vector<PiezaCruda> piezas = piezasDe(u);
    for (const PiezaCruda& pz : piezas) {
        if (!B::hasWildcard(pz.crudo)) {
            continue;
        }
        if (pz.clase == Pieza::Seccion) {
            err = T("t_patron_seccion", "el nombre de una sección no admite comodines");
            return false;
        }
        if (!B::checkRawPattern(pz.crudo)) {
            err = B::format(T("t_patron_mal", "patrón mal formado: «%1»"), {pz.crudo});
            return false;
        }
    }
    const std::size_t filtroEn =
        (filtraUltimo && !piezas.empty() && B::hasWildcard(piezas.back().crudo))
            ? piezas.size() - 1
            : std::string::npos;
    Enumerador en(e);
    std::set<std::string> vistos;

    const auto conPieza = [](UrlCruda base, const PiezaCruda& pz, const std::string& valor) {
        switch (pz.clase) {
            case Pieza::Conexion:
                base.conexion = valor;
                break;
            case Pieza::Pool:
            case Pieza::Hijo:
                base.ruta.push_back(valor);
                break;
            case Pieza::Instantanea:
                base.hayInstantanea = true;
                base.instantanea = valor;
                break;
            case Pieza::Seccion:
                base.haySeccion = true;
                base.seccion = valor;
                break;
            case Pieza::Detalle:
                base.detalle.push_back(valor);
                break;
        }
        return base;
    };

    const auto trasComodin = [&](std::size_t i) {
        for (std::size_t k = 0; k < i; ++k) {
            if (B::hasWildcard(piezas[k].crudo)) {
                return true;
            }
        }
        return false;
    };
    std::function<bool(std::size_t, const UrlCruda&)> expande =
        [&](std::size_t i, const UrlCruda& prefijo) -> bool {
        if (i == piezas.size() || i == filtroEn) {
            DestinoConcreto d;
            d.url = prefijo.texto();
            if (i == filtroEn) {
                d.filtro.patron = piezas[i].crudo;
                d.filtro.ocultos = piezas[i].clase == Pieza::Detalle
                                   && (B::toLowerAscii(prefijo.seccion) == "content"
                                       || B::toLowerAscii(prefijo.seccion) == "path");
                d.filtro.clase = piezas[i].clase == Pieza::Hijo ? FiltroFilas::Clase::Hijos
                                 : piezas[i].clase == Pieza::Instantanea
                                     ? FiltroFilas::Clase::Instantaneas
                                     : FiltroFilas::Clase::Todo;
            }
            if (vistos.insert(d.url + "\n" + d.filtro.patron).second) {
                out.push_back(d);
            }
            return true;
        }
        const PiezaCruda& pz = piezas[i];
        // Lo que existe en este nivel, colgando del prefijo.
        const auto candidatosDe = [&](std::vector<std::string>& candidatos) -> bool {
            switch (pz.clase) {
                case Pieza::Conexion:
                    candidatos = en.conexiones();
                    break;
                case Pieza::Pool:
                    candidatos = en.pools(prefijo);
                    break;
                case Pieza::Hijo:
                    candidatos = en.hijos(prefijo);
                    break;
                case Pieza::Instantanea:
                    candidatos = en.instantaneas(prefijo);
                    break;
                case Pieza::Seccion:
                    break;
                case Pieza::Detalle:
                    if (B::toLowerAscii(prefijo.seccion) == "properties") {
                        candidatos = en.propiedades(prefijo);
                    } else if (B::toLowerAscii(prefijo.seccion) == "content"
                               || B::toLowerAscii(prefijo.seccion) == "path") {
                        candidatos =
                            en.contenido(prefijo, /*soloDirectorios=*/i + 1 < piezas.size());
                    } else {
                        err = B::format(
                            T("t_patron_no_seccion", "la sección «%1» no admite comodines"),
                            {prefijo.seccion});
                        return false;
                    }
                    break;
            }
            return true;
        };
        if (!B::hasWildcard(pz.crudo)) {
            // Un tramo LITERAL detrás de un comodín tiene que EXISTIR, como en la shell:
            // `alfa/*/x.log` da solo las rutas donde hay un x.log. Sin esto, «*» casaba con
            // todo lo del nivel —ficheros incluidos— y se le pegaba «x.log» a cada cosa:
            // salían «a.log/x.log» y «b.log/x.log», que no existen.
            if (trasComodin(i) && pz.clase != Pieza::Seccion) {
                std::vector<std::string> candidatos;
                if (!candidatosDe(candidatos)) {
                    return false;
                }
                std::string literal;
                if (!B::rawSegmentToPattern(pz.crudo, literal)) {
                    return true;
                }
                std::string sinEscapes;
                for (std::size_t k = 0; k < literal.size(); ++k) {
                    if (literal[k] == '\\' && k + 1 < literal.size()) {
                        ++k;
                    }
                    sinEscapes.push_back(literal[k]);
                }
                if (std::find(candidatos.begin(), candidatos.end(), sinEscapes)
                    == candidatos.end()) {
                    return true;  // no existe aquí: esta rama no da nada
                }
            }
            return expande(i + 1, conPieza(prefijo, pz, pz.crudo));
        }
        std::vector<std::string> candidatos;
        if (!candidatosDe(candidatos)) {
            return false;
        }
        const bool deFicheros = pz.clase == Pieza::Detalle
                                && (B::toLowerAscii(prefijo.seccion) == "content"
                                    || B::toLowerAscii(prefijo.seccion) == "path");
        for (const std::string& c : candidatos) {
            if (deFicheros && !c.empty() && c[0] == '.' && pz.crudo[0] != '.') {
                continue;  // como la shell: «*» no saca los ocultos
            }
            const bool casa =
                B::matchesRawPattern(pz.crudo, c)
                || (pz.clase == Pieza::Conexion && B::matchesRawPattern(pz.crudo, en.nombreDe(c)));
            if (casa && !expande(i + 1, conPieza(prefijo, pz, B::percentEncodeSegment(c)))) {
                return false;
            }
        }
        return true;
    };
    if (!expande(0, UrlCruda{})) {
        return false;
    }
    if (out.empty()) {
        // Si una enumeración falló —máquina que no contesta—, el error ya está dicho y es
        // ESE el motivo; «nada coincide» lo taparía.
        err = en.fallo() ? std::string() : std::string("-");
        return false;
    }
    return true;
}

// Una ruta de ESTA máquina con comodines —un origen o destino de `cp` que empieza por «/»—,
// expandida contra el disco, como la shell. Ordenada: el orden de un directorio es el que le
// apetece al sistema de ficheros, y un resultado que cambia de una vez a otra no se puede
// comprobar.
bool expandeRutaLocal(const std::string& ruta, std::vector<std::string>& out, std::string& err) {
    out.clear();
    std::vector<std::string> actuales = {"/"};
    const auto une = [](const std::string& a, const std::string& b) {
        return a == "/" ? "/" + b : a + "/" + b;
    };
    for (const std::string& tramo : B::split(ruta, "/", true)) {
        std::vector<std::string> siguientes;
        if (!B::hasWildcard(tramo)) {
            for (const std::string& a : actuales) {
                siguientes.push_back(une(a, tramo));
            }
        } else {
            std::vector<std::string> alternativas;
            if (!B::expandBraces(tramo, alternativas)) {
                err = B::format(T("t_patron_mal", "patrón mal formado: «%1»"), {tramo});
                return false;
            }
            for (const std::string& a : actuales) {
                std::vector<std::string> nombres;
                std::error_code ec;
                for (const auto& ent : std::filesystem::directory_iterator(a, ec)) {
                    const std::string nombre = ent.path().filename().string();
                    if (!nombre.empty() && nombre[0] == '.' && tramo[0] != '.') {
                        continue;  // como la shell: «*» no saca los ocultos
                    }
                    for (const std::string& alt : alternativas) {
                        if (B::globMatch(alt, nombre)) {
                            nombres.push_back(nombre);
                            break;
                        }
                    }
                }
                std::sort(nombres.begin(), nombres.end());
                for (const std::string& n : nombres) {
                    siguientes.push_back(une(a, n));
                }
            }
        }
        actuales = std::move(siguientes);
    }
    out = std::move(actuales);
    return true;
}

// Un patrón que tiene que casar con UNO: el destino de `cp`, `rsync`, `send` y `diff`.
// Como la shell, donde `cp a* dir*` solo hace lo que uno cree si `dir*` es un solo sitio.
// `rutaLocal`: el texto es una ruta de esta máquina (en `cp`, la que empieza por «/»).
bool expandeUnico(Estado& e, const std::string& texto, const std::string& orden, bool rutaLocal,
                  std::string& concreto) {
    std::vector<std::string> candidatos;
    std::string err;
    if (rutaLocal) {
        if (!expandeRutaLocal(texto, candidatos, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
    } else {
        UrlCruda u;
        std::vector<DestinoConcreto> ds;
        bool hecho = absolutaCruda(e, texto, u, err) && expandeDestinos(e, u, false, ds, err);
        // El destino de `send` es un dataset que muchas veces AÚN NO EXISTE: en
        // `send tank/cop*/nuevo` el patrón elige dónde, y «nuevo» es lo que se va a crear.
        // Exigir que exista —como se exige a un tramo literal tras un comodín— hacía
        // imposible justo el caso útil. Se expande la madre y se le pega el nombre.
        if (!hecho && err == "-" && orden == "send" && u.ruta.size() >= 2
            && !B::hasWildcard(u.ruta.back()) && !u.hayInstantanea && !u.haySeccion) {
            UrlCruda madre = u;
            const std::string nuevo = madre.ruta.back();
            madre.ruta.pop_back();
            err.clear();
            hecho = expandeDestinos(e, madre, false, ds, err);
            for (DestinoConcreto& d : ds) {
                d.url += "/" + nuevo;
            }
        }
        if (!hecho) {
            if (err == "-") {
                std::fprintf(stderr, TC("t_nada_coincide", "nada coincide con «%s»\n"),
                             texto.c_str());
            } else if (!err.empty()) {
                std::fprintf(stderr, "%s\n", err.c_str());
            }
            return false;
        }
        for (const DestinoConcreto& d : ds) {
            candidatos.push_back(d.url);
        }
    }
    if (candidatos.empty()) {
        std::fprintf(stderr, TC("t_nada_coincide", "nada coincide con «%s»\n"), texto.c_str());
        return false;
    }
    if (candidatos.size() > 1) {
        std::fprintf(stderr,
                     TC("t_patron_varios_uno", "«%s» necesita UN destino y «%s» casa con %zu:\n"),
                     orden.c_str(), texto.c_str(), candidatos.size());
        for (std::size_t i = 0; i < candidatos.size() && i < 20; ++i) {
            std::fprintf(stderr, "  %s\n", candidatos[i].c_str());
        }
        return false;
    }
    concreto = candidatos.front();
    return true;
}

const std::map<std::string, ReglaPatron>& reglasDePatron() {
    static const std::map<std::string, ReglaPatron> r = {
        {"ls", {Politica::Filtra, {}}},
        {"cd", {Politica::Uno, {}}},
        // Origen y destino emparejados: el origen, si casa uno, como el destino.
        {"send", {Politica::Uno, {}}},
        {"diff", {Politica::Uno, {}}},
        {"get", {Politica::Varios, {}}},
        {"info", {Politica::Varios, {}}},
        {"status", {Politica::Varios, {}}},
        {"history", {Politica::Varios, {}}},
        {"holds", {Politica::Varios, {}}},
        {"schedules", {Politica::Varios, {}}},
        {"log", {Politica::Varios, {}}},
        {"jobs", {Politica::Varios, {}}},
        {"peers", {Politica::Varios, {}}},
        {"devices", {Politica::Varios, {}}},
        {"refresh", {Politica::Varios, {}}},
        {"connect", {Politica::Varios, {}}},
        {"disconnect", {Politica::Varios, {}}},
        {"destroy", {Politica::VariosModifica, {Pieza::Hijo, Pieza::Instantanea}}},
        {"set", {Politica::VariosModifica, {Pieza::Pool, Pieza::Hijo, Pieza::Instantanea}}},
        {"inherit", {Politica::VariosModifica, {Pieza::Pool, Pieza::Hijo, Pieza::Instantanea}}},
        {"mount", {Politica::VariosModifica, {Pieza::Pool, Pieza::Hijo}}},
        {"unmount", {Politica::VariosModifica, {Pieza::Pool, Pieza::Hijo}}},
        {"hold", {Politica::VariosModifica, {Pieza::Instantanea}}},
        {"release", {Politica::VariosModifica, {Pieza::Instantanea}}},
        {"scrub", {Politica::VariosModifica, {Pieza::Pool}}},
        {"install-daemon", {Politica::VariosModifica, {Pieza::Conexion}}},
    };
    return r;
}

// El nombre con el que aterriza un origen de `cp`: el último tramo de su ruta. Sirve para
// negarse ANTES de copiar cuando dos orígenes acabarían con el mismo nombre en el destino:
// el segundo pisaría al primero.
std::string nombreDeLlegada(const std::string& origen) {
    if (!origen.empty() && origen.front() == '/') {
        const std::size_t barra = origen.find_last_of('/');
        return origen.substr(barra + 1);
    }
    ZfsmUrl u;
    std::string err;
    if (!B::parseZfsmUrl(origen, u, err)) {
        return origen;
    }
    if (!u.detail.empty()) {
        return u.detail.back();
    }
    const std::size_t barra = u.dataset.find_last_of('/');
    return barra == std::string::npos ? u.dataset : u.dataset.substr(barra + 1);
}

// La envoltura de cada orden: si su destino lleva un patrón, lo resuelve según la regla de
// la orden; si no, la orden se ejecuta como siempre, sin pasar por nada de esto.
// `install-daemon -a`: el despliegue a todas las conexiones conocidas, que hasta ahora se
// hacía a mano máquina a máquina después de cada versión.
//
// No es `install-daemon zfsm://*` —que reinstala en todas—: se le pregunta antes a cada una
// qué versión tiene, y solo se toca la que lo necesita. Una al día no se reinstala (cuesta
// un reinicio del servicio por nada), y una que va POR DELANTE de este cliente tampoco: sería
// rebajarla, y lo que hay que actualizar ahí es el cliente. Una desconectada se salta: está
// apartada a propósito. Las que no contestan se intentan, porque «no contesta» es justo lo
// que tiene una máquina sin daemon todavía.
bool instalaDaemonEnTodas(Estado& e, const LineaAnalizada& an,
                          const std::function<bool(Estado&, const LineaAnalizada&)>& f) {
    if (!an.objetivo.empty() || an.opciones.count("on") || an.opciones.count("from")) {
        std::fputs(
            TC("t_instala_todas_destino", "-a es «todas las conexiones»: no lleva destino\n"),
            stderr);
        e.ultimoRc = 2;
        return false;
    }
    const std::string esperada = B::agentversion::expected();
    enum class Accion { Instalar, Actualizar, AlDia, PorDelante, Desconectada };
    struct Fila {
        std::string id;
        std::string nombre;
        std::string version;
        Accion accion;
        PlanCli cli;
        const B::ConnectionProfile* perfil{nullptr};
    };
    std::vector<Fila> filas;
    for (const auto& p : e.conns.perfiles) {
        Fila fila;
        fila.perfil = &p;
        fila.id = p.id.empty() ? p.name : p.id;
        fila.nombre = p.name.empty() ? fila.id : p.name;
        if (e.conns.desconectada(fila.id)) {
            fila.accion = Accion::Desconectada;
            filas.push_back(fila);
            continue;
        }
        // Se pregunta de nuevo: lo recordado de la sesión puede ser de antes de otra
        // instalación, y aquí decide si se toca una máquina.
        olvidaEstadoDeConexion(e, p);
        ZfsmUrl u;
        u.kind = ZfsmKind::Connection;
        u.connection = fila.id;
        std::string salida;
        if (agente(e, u, {"--health"}, salida, 8000)) {
            fila.version = clavesDe(salida)["VERSION"];
        }
        if (fila.version.empty()) {
            fila.accion = Accion::Instalar;
        } else {
            switch (B::agentversion::verdictFor(fila.version, esperada)) {
                case B::agentversion::Verdict::UpToDate:
                    fila.accion = Accion::AlDia;
                    break;
                case B::agentversion::Verdict::Update:
                    fila.accion = Accion::Actualizar;
                    break;
                case B::agentversion::Verdict::Ahead:
                    fila.accion = Accion::PorDelante;
                    break;
            }
        }
        fila.cli = planDelCli(e, conSudo(e, p));
        filas.push_back(fila);
    }

    Tabla t;
    t.cabecerasTexto = {T("t_cab_conexion", "CONEXIÓN"), T("t_cab_daemon", "DAEMON"),
                        T("t_cab_accion", "ACCIÓN"), T("t_cab_cli", "CLI"),
                        T("t_cab_accion_cli", "ACCIÓN CLI")};
    t.campos = {"connection", "daemon", "action", "cli", "cli_action"};
    std::vector<const Fila*> aTocar;
    for (const Fila& fila : filas) {
        std::string accion;
        switch (fila.accion) {
            case Accion::Instalar:
                accion = T("t_instala_accion_instalar", "instalar");
                break;
            case Accion::Actualizar:
                accion = T("t_instala_accion_actualizar", "actualizar");
                break;
            case Accion::AlDia:
                accion = T("t_instala_accion_al_dia", "al día: no se toca");
                break;
            case Accion::PorDelante:
                accion = T("t_instala_accion_delante", "va por delante del cliente: no se toca");
                break;
            case Accion::Desconectada:
                accion = T("t_instala_accion_desconectada", "desconectada: se salta");
                break;
        }
        const bool agenteToca =
            fila.accion == Accion::Instalar || fila.accion == Accion::Actualizar;
        const bool cliToca = fila.cli.accion == PlanCli::Accion::Actualizar;
        if (agenteToca || cliToca) {
            aTocar.push_back(&fila);
        }
        const bool desconectada = fila.accion == Accion::Desconectada;
        t.filas.push_back({fila.id, fila.version.empty() ? "-" : fila.version, accion,
                           fila.cli.sonda.found
                               ? (fila.cli.sonda.fullVersion.empty() ? std::string("?")
                                                                     : fila.cli.sonda.fullVersion)
                               : std::string("-"),
                           desconectada ? std::string("-") : textoDePlanCli(fila.cli)});
    }
    t.imprime(e.formato);
    if (aTocar.empty()) {
        std::fprintf(stderr, TC("t_instala_nada", "nada que hacer: todas al día (%s)\n"),
                     esperada.c_str());
        return true;
    }
    if (an.opciones.count("check")) {
        std::fputs(TC("t_patron_check", "--check: no se ha tocado nada\n"), stderr);
        return true;
    }
    if (!confirma(e, B::format(T("t_instala_todas_conf2",
                                 "Se van a poner al día %2 conexiones con %1: el daemon y, donde "
                                 "lo tengan, zfsmgr-cli. ¿Continuar?"),
                               {esperada, std::to_string(aTocar.size())}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        e.ultimoRc = 1;
        return false;
    }
    const bool asumiaSi = e.asumirSi;
    e.asumirSi = true;
    std::size_t bien = 0;
    std::vector<std::string> fallidas;
    for (const Fila* fila : aTocar) {
        const std::string& id = fila->id;
        std::fprintf(stderr, "\n== %s\n", id.c_str());
        // Cada una con su propio código: el fallo de una no se arrastra a la siguiente.
        e.ultimoRc = 0;
        bool ok = false;
        if (fila->accion == Accion::Instalar || fila->accion == Accion::Actualizar) {
            // El daemon, y con él el CLI: `install-daemon` hace las dos cosas.
            LineaAnalizada una = an;
            una.banderas.erase(std::remove(una.banderas.begin(), una.banderas.end(), "-a"),
                               una.banderas.end());
            una.opciones.erase("check");
            una.objetivo = "zfsm://" + B::percentEncodeSegment(id);
            ok = f(e, una);
        } else {
            // El daemon ya está al día: solo el CLI, sin reinstalar el servicio por nada.
            ok = actualizaCli(e, conSudo(e, *fila->perfil), fila->cli);
        }
        if (ok) {
            ++bien;
        } else {
            fallidas.push_back(id);
        }
    }
    e.asumirSi = asumiaSi;
    std::fprintf(stderr, TC("t_patron_resumen", "%s: %zu bien, %zu fallaron\n"), "install-daemon",
                 bien, fallidas.size());
    for (const std::string& id : fallidas) {
        std::fprintf(stderr, TC("t_patron_fallo_uno", "  falló: %s\n"), id.c_str());
    }
    e.ultimoRc = fallidas.empty() ? 0 : 1;
    return fallidas.empty();
}

}  // namespace zfsmgr::cli
