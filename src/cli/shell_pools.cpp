// Pool commands: maintenance (scrub, trim…), `status`, `history`, `import` and creating a
// pool.

#include "shell_internal.h"

namespace zfsmgr::cli::detail {

bool cmdMantenimientoPool(Estado& e, const LineaAnalizada& linea, const char* op) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    // La traducción de la fase a su letra —que NO es la misma para las tres operaciones— y
    // el orden de los argumentos viven en `commands::pools`.
    // Aquí solo se decide QUÉ operación y con qué palabras la pidió el usuario.
    const std::string fase = B::toLowerAscii(pet.uno("fase"));
    PL::Phase faseOp = PL::Phase::Start;
    if (fase == "stop" || fase == "cancel") {
        faseOp = PL::Phase::Stop;
    } else if (fase == "pause" || fase == "suspend") {
        faseOp = PL::Phase::Pause;
    }
    const std::string sub(op);
    const PL::Operation opPool = sub == "scrub"  ? PL::Operation::Scrub
                                 : sub == "trim" ? PL::Operation::Trim
                                                 : PL::Operation::Initialize;
    std::vector<std::string> discos;
    if (!discosDe(e, pet.lista("disco"), destino.connection, discos)) {
        return false;
    }
    std::vector<std::string> argv = PL::argv(opPool, destino.pool, faseOp, pet.nativas(), discos);
    if (argv.empty()) {
        std::fputs(TC("t_pool_op_invalida", "no se puede pedir eso sobre ese pool\n"), stderr);
        return false;
    }
    if (!zpoolGeneric(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, TC("t_s_s_en_mar_afe49e", "%s: %s en marcha\n"), destino.pool.c_str(), op);
    return true;
}

bool cmdPoolSimple(Estado& e, const LineaAnalizada& linea, const char* verbo, const char* op,
                   const char* aviso) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (aviso
        && !confirma(
            e, B::format(T("t_conf_en_pool", "%1 en %2. ¿Continuar?"), {aviso, destino.pool}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    std::vector<std::string> argv{op};
    // Las banderas del mandato original, tal cual. La lista está declarada en el catálogo y
    // es la MISMA con la que se validan, así que aceptar y pasar no se pueden separar.
    for (const std::string& b : pet.nativas()) {
        argv.push_back(b);
    }
    argv.push_back(destino.pool);
    // `clear` admite un dispositivo detrás del pool. Al declararlo como ranura dejó de
    // llegar aquí como suelto, así que hay que pasarlo a mano: consumido y no usado es
    // exactamente el fallo que este preámbulo viene a quitar.
    std::vector<std::string> discos;
    if (!discosDe(e, pet.lista("disco"), destino.connection, discos)) {
        return false;
    }
    for (const std::string& d : discos) {
        argv.push_back(d);
    }
    if (!zpoolGeneric(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, TC("t_pool_op_hecha", "%s: %s hecho\n"), destino.pool.c_str(), op);
    return true;
}

// El estado detallado. Es texto para leer y se saca tal cual: fingir columnas donde
// `zpool status` dibuja un árbol de vdevs sería inventarse una estructura que no tiene.
// Un pool que NO está importado pero aparece en la sonda: su estado, su motivo y sus
// dispositivos, cada uno con la ruta real en esa máquina. Es lo que hace falta para saber,
// por ejemplo, de qué disco sale una etiqueta vieja antes de limpiarla. `false` si el pool
// no aparece.
bool poolImportable(Estado& e, const ZfsmUrl& conexion, const std::string& nombre,
                    H::ImportablePoolInfo& info, std::map<std::string, std::string>& rutas) {
    std::string sonda;
    if (!agente(e, conexion, PET::importableProbe(), sonda, 60000)) {
        return false;
    }
    bool hay = false;
    for (const H::ImportablePoolInfo& imp : H::parseZpoolImportOutput(sonda)) {
        if (imp.pool == nombre) {
            // `zpool import` y `zpool import -s` pueden dar el mismo pool dos veces: se
            // queda el que trae más dispositivos.
            if (!hay || imp.devices.size() > info.devices.size()) {
                info = imp;
            }
            hay = true;
        }
    }
    if (!hay) {
        return false;
    }
    std::vector<std::string> nombres;
    for (const H::ImportableDevice& d : info.devices) {
        if (d.leaf) {
            nombres.push_back(d.name);
        }
    }
    std::string out;
    if (!nombres.empty() && agente(e, conexion, PET::resolveDevices(nombres), out, 20000)) {
        for (const auto& kv : clavesDe(out)) {
            rutas[kv.first] = kv.second;
        }
    }
    return true;
}

bool cmdStatus(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (destino.pool.empty()) {
        std::fprintf(stderr, TC("t_hace_falta_8acbda", "hace falta un pool (ahora: %s)\n"),
                     textoDe(destino).c_str());
        return false;
    }
    std::string out;
    std::string errZpool;
    if (!agente(e, destino, PET::poolStatus(destino.pool), out, 60000, &errZpool)) {
        // ¿No está importado? Si aparece entre los importables, se cuenta lo que se sabe de
        // él en vez de dejar el «no such pool» de zpool: esa respuesta es cierta y no ayuda.
        H::ImportablePoolInfo imp;
        std::map<std::string, std::string> rutas;
        if (!poolImportable(e, subeHasta(destino, Objetivo::Conexion), destino.pool, imp, rutas)) {
            if (!errZpool.empty()) {
                std::fprintf(stderr, "%s\n", errZpool.c_str());
            }
            e.ultimoRc = 1;
            return false;
        }
        e.ultimoRc = 0;
        std::fprintf(
            stdout,
            TC("t_status_importable", "  pool: %s  (NO importado)\n    id: %s\n state: %s\n"),
            imp.pool.c_str(), imp.guid.c_str(), imp.state.c_str());
        if (!imp.reason.empty()) {
            std::fprintf(stdout, "status: %s\n", imp.reason.c_str());
        }
        if (!imp.action.empty()) {
            std::fprintf(stdout, "action: %s\n", imp.action.c_str());
        }
        std::fputs(TC("t_status_dispositivos", "dispositivos:\n"), stdout);
        for (const H::ImportableDevice& d : imp.devices) {
            const std::string sangria(static_cast<std::size_t>(2 + 2 * d.depth), ' ');
            const auto ruta = rutas.find(d.name);
            const std::string donde = !d.leaf ? std::string()
                                      : (ruta != rutas.end() && !ruta->second.empty())
                                          ? "  -> " + ruta->second
                                          : std::string("  -> ?");
            std::fprintf(stdout, "%s%s%s  %s  %s\n", sangria.c_str(), d.name.c_str(), donde.c_str(),
                         d.state.c_str(), d.detail.c_str());
        }
        return true;
    }
    std::fprintf(stdout, "%s", out.c_str());
    if (!out.empty() && out.back() != '\n') {
        std::fprintf(stdout, "\n");
    }
    return true;
}

bool cmdHistory(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (destino.pool.empty()) {
        std::fprintf(stderr, TC("t_hace_falta_8acbda", "hace falta un pool (ahora: %s)\n"),
                     textoDe(destino).c_str());
        return false;
    }
    std::string out;
    if (!agente(e, destino, PET::poolHistory(destino.pool), out, 60000)) {
        return false;
    }
    Tabla t;
    t.nombreJson = "history";
    t.cabecerasTexto = {T("t_cab_cuando", "CUÁNDO"), T("t_cab_orden", "ORDEN")};
    t.campos = {"when", "command"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena};
    for (const std::string& linea : B::split(out, "\n", true)) {
        if (B::startsWith(linea, "History for")) {
            continue;
        }
        // «2026-02-04.11:23:49 zpool create …»: la marca de tiempo es el primer campo.
        const std::size_t esp = linea.find(' ');
        if (esp == std::string::npos) {
            t.filas.push_back({std::string(), B::trim(linea)});
        } else {
            t.filas.push_back({linea.substr(0, esp), B::trim(linea.substr(esp + 1))});
        }
    }
    t.imprime(e.formato);
    return true;
}

// Importar. Sin nombre, ENSEÑA lo que hay para importar en vez de fallar: es la pregunta
// que uno tiene antes de poder contestar la otra.
bool cmdImport(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (destino.connection.empty()) {
        std::fputs(TC("t_hace_falta_6a3acd", "hace falta una conexión\n"), stderr);
        return false;
    }
    if (pet.lista("texto").empty()) {
        std::string out;
        if (!agente(e, destino, PET::importableProbe(), out, 60000)) {
            return false;
        }
        // La salida CRUDA de la sonda no vale como respuesta. La sonda ejecuta `zpool
        // import` Y `zpool import -s` y pega las dos salidas, así que un pool que aparece
        // en las dos —lo normal— salía DOS VECES, en dos bloques idénticos de siete líneas
        // cada uno; y con un pool de verdad detrás, quien lea la lista no puede saber si
        // son dos pools distintos con el mismo nombre o el mismo contado dos veces. `ls` ya
        // lo analizaba y lo unificaba por nombre; esto hace lo mismo, y además es lo que
        // permite dar la lista en tsv o en json como el resto de las órdenes.
        Tabla t;
        t.nombreJson = "importable_pools";
        t.cabecerasTexto = {T("t_cab_pool", "POOL"), T("t_cab_id", "ID"),
                            T("t_cab_estado", "ESTADO"), T("t_cab_motivo", "MOTIVO")};
        t.campos = {"pool", "guid", "state", "reason"};
        std::set<std::string> yaEstan;
        for (const H::ImportablePoolInfo& imp : H::parseZpoolImportOutput(out)) {
            if (imp.pool.empty() || !yaEstan.insert(imp.pool).second) {
                continue;
            }
            t.filas.push_back({imp.pool, imp.guid, imp.state, imp.reason});
        }
        avisaSondaImportables(out, !t.filas.empty());
        t.imprime(e.formato);
        return true;
    }
    // Las banderas de `zpool import` que la orden declara, TODAS: antes se construía
    // `import [-f] <pool>` y `-d`, `-R`, `-N`… se aceptaban sin pasarse. `-d` admite además
    // `zfsm://<esta máquina>#path/dir`.
    std::vector<std::string> argv{"import"};
    const std::vector<std::string> nat = pet.nativas();
    for (std::size_t i = 0; i < nat.size(); ++i) {
        if (nat[i] == "-d" && i + 1 < nat.size()) {
            std::string dir;
            if (!rutaDeRanura(e, nat[i + 1], destino.connection, dir)) {
                return false;
            }
            argv.push_back("-d");
            argv.push_back(dir);
            ++i;
            continue;
        }
        argv.push_back(nat[i]);
    }
    argv.push_back(pet.uno("texto"));
    if (!pet.valor("as").empty()) {
        argv.push_back(pet.valor("as"));  // importar con otro nombre
    }
    if (!zpoolGeneric(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, "importado %s\n", pet.uno("texto").c_str());
    return true;
}

// Crear un pool. Es la orden MÁS destructiva de todas: escribe en los dispositivos que se
// le den, y si alguno tenía datos, desaparecen. La confirmación los enumera uno a uno.
bool cmdCrearPool(Estado& e, const Peticion& pet, const ZfsmUrl& destino,
                  const std::string& nombre) {
    if (pet.lista("texto").size() < 2) {
        std::fputs(TC("t_uso_create_63b88e",
                      "uso: create <pool> <dispositivo> [<dispositivo>...] [-f]\n"
                      "            [-o prop=valor] [-O prop-fs=valor] [--mountpoint <ruta>]\n"
                      "  Los dispositivos se ESCRIBEN: lo que hubiera en ellos se pierde.\n"),
                   stderr);
        return false;
    }
    // Una sola copia de la lista, y los iteradores DE ESA. `lista()` devuelve por valor, así
    // que `pet.lista(...).begin()` y `pet.lista(...).end()` eran extremos de dos vectores
    // distintos: el rango no significaba nada y el intérprete se caía al construir los
    // nombres. No se veía porque hasta ahora no se llegaba aquí: el preámbulo exigía estar
    // en un dataset y crear un pool era imposible, así que el fallo estaba tapado.
    const std::vector<std::string> textos = pet.lista("texto");
    // El nombre viene DADO y no se toma de textos.front(): desde la raíz, `create
    // unibody/apar sda` lo trae ya separado de la máquina en la que se crea.
    const std::string pool = nombre.empty() ? textos.front() : nombre;
    std::vector<std::string> vdevs;
    if (!discosDe(e, std::vector<std::string>(textos.begin() + 1, textos.end()), destino.connection,
                  vdevs)) {
        return false;
    }
    if (!confirma(e, B::format(T("t_conf_create_pool",
                                 "Se va a crear el pool %1 en %2 ESCRIBIENDO en: %3.\nLo que "
                                 "hubiera en esos dispositivos SE PIERDE. ¿Continuar?"),
                               {pool, destino.connection, B::join(vdevs, ", ")}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    std::vector<std::string> argv{"create"};
    // Las banderas nativas van DELANTE del nombre del pool. Detrás, zpool las toma por
    // dispositivos o las ignora sin decir nada: `zpool trim pruebacli -r 100M` se tragó el
    // ritmo en silencio, y ese fue el fallo que enseñó dónde tienen que ir.
    for (const std::string& n : pet.nativas()) {
        argv.push_back(n);
    }
    if (pet.tiene("-f")) {
        argv.push_back("-f");
    }
    if (!pet.valor("mountpoint").empty()) {
        argv.push_back("-m");
        argv.push_back(pet.valor("mountpoint"));
    }
    for (const auto& kv : pet.l->repetidas) {
        argv.push_back(kv.first == "-o" ? "-o" : "-O");
        argv.push_back(kv.second);
    }
    argv.push_back(pool);
    for (const auto& v : vdevs) {
        argv.push_back(v);
    }
    if (!zpoolGeneric(e, destino, argv)) {
        return false;
    }
    // Con `-n` no se ha creado nada: zpool solo enseña la disposición que saldría. Decir
    // «creado el pool» ahí sería mentir sobre lo único que importa de esta orden.
    if (!pet.tiene("-n")) {
        std::fprintf(stderr, TC("t_creado_el__2de5da", "creado el pool %s\n"), pool.c_str());
    }
    return true;
}

// --- Conectar, desconectar y refrescar.

std::string idDe(const Estado& e, const ZfsmUrl& u) {
    const auto* p = buscarConexion(e.conns, u.connection);
    return p ? (p->id.empty() ? p->name : p->id) : std::string();
}

}  // namespace zfsmgr::cli::detail
