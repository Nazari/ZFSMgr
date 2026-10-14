// Listing and navigation: connections, pools, datasets, properties, permissions and
// file contents (`#content`, `#path`); `ls`, `cd` and the mounts they need.

#include "shell_internal.h"

namespace zfsmgr::cli::detail {

bool listaConexiones(Estado& e, const Peticion& pet) {
    (void)pet;
    Tabla t = tablaDeConexiones(e.conns);
    t.cabecerasTexto.push_back(T("t_cab_daemon", "DAEMON"));
    t.campos.push_back("daemon");
    t.tipos.push_back(Tipo::Cadena);
    t.cabecerasTexto.push_back(T("t_cab_avisos", "AVISOS"));
    t.campos.push_back("warnings");
    t.tipos.push_back(Tipo::Cadena);
    const std::string esperada = B::agentversion::expected();
    for (std::size_t i = 0; i < t.filas.size() && i < e.conns.perfiles.size(); ++i) {
        const auto& p = e.conns.perfiles[i];
        const std::string id = p.id.empty() ? p.name : p.id;
        std::string version;
        // Una desconectada no se sondea: está apartada a propósito y preguntarle sería
        // saltarse esa marca, además de costar el plazo entero.
        if (!e.conns.desconectada(id)) {
            const auto ya = e.versionDaemon.find(id);
            if (ya != e.versionDaemon.end()) {
                version = ya->second;
            } else {
                ZfsmUrl u;
                u.kind = ZfsmKind::Connection;
                u.connection = id;
                std::string salida;
                if (agente(e, u, {"--health"}, salida, 8000)) {
                    auto claves = clavesDe(salida);
                    version = claves["VERSION"];
                    if (claves["DISK_ACCESS"] == "denied") {
                        e.avisoDaemon[id] = claves["DISK_ACCESS_RESTART"] == "1"
                                                ? T("t_aviso_disco_reiniciar",
                                                    "sin acceso al disco: reiniciar el agente")
                                                : T("t_aviso_disco", "sin acceso al disco");
                    }
                }
                // Se recuerda durante la sesión, incluido el fallo: repetir `ls --daemon`
                // no vuelve a esperar por una máquina que ya no contestó.
                e.versionDaemon[id] = version;
            }
        }
        // El « * » distingue VIEJA de NUEVA, que no es lo mismo aunque las dos difieran.
        //
        // Se comparaba con `!=`, así que un agente más nuevo que este cliente salía
        // marcado igual que uno anticuado, y el usuario no podía saber cuál de las dos
        // mitades hay que actualizar. La comparación ordenada es la que ya usaba la
        // interfaz; ahora es la misma para las dos.
        if (version.empty()) {
            version = "-";
        } else {
            // Por la versión de publicación, no por la cadena entera: el cuarto número es
            // una huella y compararlo como número marcaba « + » a un agente de otra
            // compilación de la misma versión, que es justo uno que hay que actualizar.
            switch (B::agentversion::verdictFor(version, esperada)) {
                case B::agentversion::Verdict::UpToDate:
                    break;
                case B::agentversion::Verdict::Update:
                    version += " *";
                    break;  // a actualizar
                case B::agentversion::Verdict::Ahead:
                    version += " +";
                    break;  // el viejo es el cliente
            }
        }
        t.filas[i].push_back(version);
        const auto aviso = e.avisoDaemon.find(id);
        t.filas[i].push_back(aviso == e.avisoDaemon.end() ? std::string("-") : aviso->second);
    }
    if (!aplicaFiltro(e, t, {0, 1})) {
        return false;
    }
    t.imprime(e.formato);
    // Y qué hacer, una vez, debajo de la tabla: la columna dice QUÉ pasa, esto CÓMO se arregla.
    for (const auto& fila : t.filas) {
        if (fila.size() >= 2 && fila.back() != "-") {
            std::fputs(
                TC("t_aviso_disco_remedio",
                   "aviso: un agente de macOS sin «Acceso total al disco» no ve los pools por "
                   "importar.\n  Configuración del Sistema > Privacidad y Seguridad > Acceso "
                   "total al disco > /usr/local/libexec/zfsmgr-agent,\n  y después reiniciar "
                   "el agente en esa máquina: sudo launchctl kickstart -k "
                   "system/org.zfsmgr.agent\n"),
                stderr);
            break;
        }
    }
    return true;
}

// Qué se dice cuando la sonda de importables no encuentra nada.
//
// «No hay ninguno» y «no puedo mirar» se leen igual en la salida de `zpool import`, y aquí
// se decidía solo el primero de los dos casos: se buscaba la marca de discos ilegibles y se
// soltaba el aviso de macOS, sin mirar el tercer estado —el permiso concedido DESPUÉS de
// arrancar el agente, que en macOS no surte efecto hasta reiniciarlo—. La base ya sabe
// distinguirlos y la interfaz gráfica ya los distinguía; esto pone al CLI de acuerdo con
// las dos, y en un único sitio para las dos órdenes que preguntan lo mismo: `ls` e `import`.
void avisaSondaImportables(const std::string& sonda, bool huboPools) {
    if (huboPools) {
        return;  // hay importables: no hay nada que explicar
    }
    const auto lectura = H::readImportProbe(sonda, false);
    using D = H::ImportProbeDiagnosis;
    if (lectura.diagnosis != D::DisksUnreadable && lectura.diagnosis != D::ProbablyNeedsRestart) {
        return;  // se puede mirar y no hay ninguno: NO se avisa de nada
    }
    const std::string donde =
        lectura.device.empty() ? T("t_los_discos", "los discos") : lectura.device;
    std::fprintf(stderr,
                 TC("t_sonda_ilegible",
                    "aviso: el agente no puede leer %s, así que un "
                    "pool sin importar NO aparece aquí.\n"),
                 donde.c_str());
    if (lectura.diagnosis == D::ProbablyNeedsRestart) {
        std::fputs(TC("t_sonda_reinicio_cli",
                      "  El permiso del sistema cambió DESPUÉS de que el "
                      "agente arrancara, y macOS lo decide al arrancar el proceso:\n"
                      "  reinícielo en esa máquina: sudo launchctl kickstart -k "
                      "system/org.zfsmgr.agent\n"),
                   stderr);
    } else {
        std::fputs(
            TC("t_sonda_permiso_cli",
               "  En macOS: Configuración del Sistema > Privacidad y "
               "Seguridad > Acceso total al disco >\n"
               "  /usr/local/libexec/zfsmgr-agent, y después reiniciar el agente en esa máquina:\n"
               "  sudo launchctl kickstart -k system/org.zfsmgr.agent\n"),
            stderr);
    }
}

// Los pools de una conexión, del JSON de `zpool list`.
bool listaPools(Estado& e, const ZfsmUrl& destino) {
    std::string out;
    if (!agente(e, destino, PET::poolList(), out)) {
        return false;
    }
    B::json::Value raiz;
    std::string err;
    // Sin pools, `zpool list -j` NO devuelve lo mismo en todas partes: en Linux imprime un
    // objeto con «pools» vacío, y el OpenZFS de macOS —2.4.1— no imprime nada y sale con 0.
    // Tratar eso como JSON ilegible decía «respuesta ilegible de zpool list: texto JSON
    // vacío» en una máquina donde lo único que pasa es que todavía no hay ningún pool.
    if (B::trim(out).empty()) {
        raiz = B::json::Value(B::json::Object{});
    } else if (!B::json::parse(out, raiz, &err)) {
        std::fprintf(stderr, TC("t_respuesta__74ad0d", "respuesta ilegible de zpool list: %s\n"),
                     err.c_str());
        return false;
    }
    // **Un pool es también un dataset: el raíz.** Por eso esta tabla lleva las dos mitades,
    // las columnas de POOL —estado, tamaño, libre, uso, salud, importado— y las de DATASET
    // —tipo, usado, compresión, montado, punto de montaje—.
    //
    // Antes solo llevaba las primeras, y las segundas se enseñaban añadiendo la fila del
    // propio pool DENTRO de su listado, al hacer `cd`. Eso confundía: al entrar en un pool
    // aparecía él mismo junto a sus hijos, y no era obvio si esa fila era el pool o un
    // dataset con el mismo nombre. Los datos son los mismos; el sitio, no.
    Tabla t;
    t.nombreJson = "pools";
    t.cabecerasTexto = {
        T("t_cab_nombre", "NOMBRE"), T("t_cab_estado", "ESTADO"), T("t_cab_tamano", "TAMAÑO"),
        T("t_cab_libre", "LIBRE"), T("t_cab_uso", "USO"), T("t_cab_dedup", "DEDUP"),
        T("t_cab_ram_ddt", "RAM DDT"), T("t_cab_salud", "SALUD"), T("t_cab_importado", "IMPORTADO"),
        T("t_cab_tipo", "TIPO"), T("t_cab_usado", "USADO"),
        // MONTADO, y no ESTADO como en el listado de datasets: esta tabla
        // YA tiene una columna ESTADO —la del pool: ONLINE, FAULTED— y dos
        // con el mismo nombre no se leen. El motivo de ESTADO allí era que
        // un volumen no se monta sino que se expone; un pool raíz es siempre
        // un sistema de ficheros, así que aquí la pregunta vieja sigue valiendo.
        T("t_cab_compr", "COMPR"), T("t_cab_montado", "MONTADO"),
        T("t_cab_punto_de_montaje", "PUNTO DE MONTAJE")};
    // `dedupratio` es del POOL, no de un dataset, y por eso va en ESTA tabla y no en la de
    // datasets: la DDT es una sola para el pool entero y ZFS no reparte el ahorro entre sus
    // datasets. Lo único que hay por dataset es `dedup = off|on|sha256…`, que es un AJUSTE
    // —si lo que se escriba de ahora en adelante se deduplica— y no una medida de nada.
    //
    // No cuesta una llamada más: `zpool list -j` ya la devuelve y el agente ya la reenviaba
    // entera; era el cliente el que la tiraba.
    t.campos = {"name",   "state",    "size", "free", "capacity",      "dedupratio", "ddtmemory",
                "health", "imported", "type", "used", "compressratio", "mounted",    "mountpoint"};
    // USADO va como CADENA, no como `Tipo::Bytes`, y no es un descuido.
    //
    // `Tipo::Bytes` promete que el valor de dentro está en bytes: el texto lo formatea y el
    // tsv y el json emiten el número. La fuente de esta columna es `zfs get`, que da «336K»
    // ya formateado, y el agente no sirve esa propiedad en crudo. Declararla `Bytes` hacía
    // que el json emitiera `336` —`atoll("336K")`— y perdiera la unidad.
    //
    // Reconvertir «336K» a bytes aquí sería inventarse una precisión que la fuente no dio.
    // La consecuencia, que conviene saber: en el listado de DATASETS esta columna sí va en
    // bytes, porque allí la fuente es `zfs list -p`. Igualarlas pide un verbo de propiedades
    // en crudo en el agente.
    // RAM DDT va como `Bytes`: en texto se lee «292M» y en tsv y json salen los bytes
    // exactos, que es lo que un guion puede comparar contra un umbral.
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena,   Tipo::Cadena,
               Tipo::Cadena, Tipo::Bytes,  Tipo::Cadena, Tipo::Booleano, Tipo::Cadena,
               Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    // json only: the pool's guid (also of one not imported, to import it by guid) and why
    // an importable one cannot be imported.
    t.campos.insert(t.campos.end(), {"guid", "reason"});
    t.tipos.insert(t.tipos.end(), {Tipo::Cadena, Tipo::Cadena});

    // Los campos de dataset del pool raíz, uno por pool.
    //
    // Es una consulta por pool, no recursiva: `--dump-zfs-get-json` con las cinco propiedades
    // que hacen falta. Pedir el árbol entero —`--dump-zfs-list-all`— traería todos los
    // descendientes para quedarse con la primera fila.
    //
    // Si un pool no contesta, sus celdas van vacías y el resto de la tabla se enseña igual:
    // que falte el «usado» no es motivo para no enseñar el estado.
    const auto camposDeDataset = [&](const std::string& pool) {
        std::array<std::string, 5> v{};
        std::string salidaProps;
        if (!agente(e, destino,
                    PET::specificProperties(
                        {"type", "used", "compressratio", "mounted", "mountpoint"}, pool),
                    salidaProps, 15000)) {
            return v;
        }
        std::vector<L::Property> props;
        std::string errProps;
        if (!L::properties(salidaProps, props, errProps)) {
            return v;
        }
        for (const L::Property& pr : props) {
            if (pr.name == "type") {
                // La MISMA palabra que usa el listado de datasets. `zfs` dice «filesystem» y
                // allí se escribe «dataset»: dos tablas con la columna TIPO que no significan
                // lo mismo se leen peor que una columna menos.
                v[0] = pr.value == "snapshot" ? "snapshot" : "dataset";
            } else if (pr.name == "used") {
                v[1] = pr.value;
            } else if (pr.name == "compressratio") {
                // Sin la «x» final: `zfs get` la pone y `zfs list -p` no, y esta columna sale
                // en las dos tablas.
                v[2] = (!pr.value.empty() && pr.value.back() == 'x')
                           ? pr.value.substr(0, pr.value.size() - 1)
                           : pr.value;
            } else if (pr.name == "mounted") {
                v[3] = pr.value;
            } else if (pr.name == "mountpoint") {
                v[4] = pr.value;
            }
        }
        return v;
    };
    const auto prop = [](const B::json::Value& p, const char* k) {
        return p["properties"][k]["value"].toString();
    };
    std::set<std::string> yaEstan;
    for (const auto& kv : raiz["pools"].toObject()) {
        const auto& p = kv.second;
        const std::string nombre = p["name"].toString(kv.first);
        yaEstan.insert(nombre);
        const auto ds = camposDeDataset(nombre);
        // Sin la «x» final, igual que COMPR: las dos salen de sitios que la ponen o no
        // según la orden, y en una misma tabla no pueden escribirse distinto.
        std::string dedup = prop(p, "dedupratio");
        if (!dedup.empty() && dedup.back() == 'x') {
            dedup.pop_back();
        }
        // Lo que la DDT cuesta en MEMORIA. Es una llamada por pool, y se paga porque se
        // midió lo que vale: 0,03 s sobre una tabla de 1,15 M de entradas, lo mismo que un
        // `zpool status` a secas, porque el histograma sale de contadores acumulados.
        //
        // Vacía cuando el pool no tiene tabla, que es el caso normal, y también cuando no
        // se pudo preguntar. Las dos se enseñan como «-»: ninguna de las dos es «0 bytes»,
        // y escribir un cero diría que la deduplicación está puesta y no cuesta nada.
        std::string ramDdt;
        std::string salidaDedup;
        if (agente(e, destino, PET::poolDedupStatus(nombre), salidaDedup, 15000)) {
            const H::DedupDdt ddt = H::parseDedupDdt(salidaDedup);
            if (ddt.hayTabla) {
                ramDdt = std::to_string(ddt.bytesEnMemoria);
            }
        }
        std::string guid = p["pool_guid"].toString();
        if (guid.empty()) {
            guid = prop(p, "guid");
        }
        t.filas.push_back({nombre, p["state"].toString(), prop(p, "size"), prop(p, "free"),
                           prop(p, "capacity"), dedup, ramDdt, prop(p, "health"), "true", ds[0],
                           ds[1], ds[2], ds[3], ds[4], guid, ""});
    }

    // Los que están AHÍ pero sin importar. `zpool list` no los ve —solo enumera los
    // importados— y por eso un pool perfectamente visible en la interfaz no salía en `ls`:
    // no es que estuviera oculto, es que se preguntaba por otra cosa.
    //
    // La sonda es una orden aparte y puede fallar sin que eso invalide lo ya listado (falta
    // de permisos sobre los discos, un agente sin «Acceso total al disco» en macOS): si no
    // responde, se enseña lo importado y se sigue, en vez de no enseñar nada.
    std::string sonda;
    if (agente(e, destino, PET::importableProbe(), sonda, 25000)) {
        const std::vector<H::ImportablePoolInfo> sondados = H::parseZpoolImportOutput(sonda);
        avisaSondaImportables(sonda, !sondados.empty());
        for (const H::ImportablePoolInfo& imp : sondados) {
            // El mismo conjunto sirve para dos cosas: no repetir uno ya importado, y no
            // repetirlo consigo mismo. La sonda ejecuta `zpool import` Y `zpool import -s`
            // y pega las dos salidas, así que un pool que aparece en las dos —lo normal—
            // llegaba aquí dos veces y salía dos veces en el listado.
            if (imp.pool.empty() || !yaEstan.insert(imp.pool).second) {
                continue;
            }
            // Sin tamaño ni uso: eso solo se sabe una vez importado, y rellenarlo con ceros
            // diría que el pool está vacío.
            // Un pool sin importar no tiene campos de dataset que enseñar: sus datasets no
            // están montados ni se pueden consultar hasta importarlo.
            t.filas.push_back({imp.pool, imp.state, "", "", "", "", "", imp.state, "false", "", "",
                               "", "", "", imp.guid, imp.reason});
        }
    }
    if (!aplicaFiltro(e, t)) {
        return false;
    }
    t.imprime(e.formato);
    return true;
}

// Los hijos y las instantáneas de un dataset. Sale de `--dump-zfs-list-all`, que es TSV con
// las columnas name,guid,used,compressratio,encryption,creation,referenced,montados,
// mountpoint,canmount.
bool listaDataset(Estado& e, const ZfsmUrl& destino, bool recursivo) {
    std::string out;
    if (!agente(e, destino, PET::datasetList(destino.dataset), out)) {
        return false;
    }
    Tabla t;
    t.nombreJson = "entries";
    t.cabecerasTexto = {
        T("t_cab_nombre", "NOMBRE"),    T("t_cab_tipo", "TIPO"),
        T("t_cab_usado", "USADO"),      T("t_cab_compr", "COMPR"),
        T("t_cab_estado_ds", "ESTADO"), T("t_cab_punto_de_montaje", "PUNTO DE MONTAJE")};
    // El campo se llama `status` y no `mounted`, y eso es a propósito: su significado ha
    // cambiado, no solo su presentación. Un guion que leyera `mounted` esperando «yes»/«no»
    // debe encontrarse con que ese campo YA NO ESTÁ, en vez de con uno que se llama igual y
    // contesta otra cosa. Fallar a la cara vale más que mentir con el mismo nombre.
    t.campos = {"name", "type", "used", "compressratio", "status", "mountpoint"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Bytes, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    if (recursivo) {
        // What a program needs to rebuild the tree in one call: json only.
        t.campos.insert(t.campos.end(),
                        {"guid", "encryption", "creation", "referenced", "mounted", "canmount"});
        t.tipos.insert(t.tipos.end(), {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Bytes,
                                       Tipo::Cadena, Tipo::Cadena});
    }
    // Aquí se añadía la fila del PROPIO pool cuando lo listado era un pool, para que sus
    // campos de dataset se vieran en alguna parte. Confundía: al entrar en un pool aparecía
    // él mismo junto a sus hijos, y no había forma de saber si esa fila era el pool o un
    // dataset con su mismo nombre.
    //
    // Esos campos están ahora donde corresponde: en el listado de POOLS, que lleva las dos
    // mitades —las columnas de pool y las de dataset— porque un pool es también el dataset
    // raíz. Aquí solo salen los hijos.
    const std::string prefijo = destino.dataset + "/";
    for (const std::string& linea : B::split(out, "\n", true)) {
        const std::vector<std::string> c = B::split(linea, "\t", false);
        if (c.size() < 12) {
            continue;
        }
        const std::string& nombre = c[0];
        const std::size_t arroba = nombre.find('@');
        const bool esSnap = arroba != std::string::npos;
        if (recursivo) {
            const std::string base = esSnap ? nombre.substr(0, arroba) : nombre;
            if (base != destino.dataset && !B::startsWith(base, prefijo)) {
                continue;
            }
        } else if (nombre == destino.dataset) {
            continue;  // uno no es hijo de sí mismo
        }
        // Solo los hijos DIRECTOS y las instantáneas propias: `list-all` es recursivo, y
        // volcar el árbol entero convierte un `ls` en un listado de miles de líneas.
        if (recursivo) {
            // every one of them
        } else if (esSnap) {
            if (nombre.substr(0, arroba) != destino.dataset) {
                continue;
            }
        } else {
            if (!B::startsWith(nombre, prefijo)
                || nombre.find('/', prefijo.size()) != std::string::npos) {
                continue;
            }
        }
        // El TIPO lo dice ZFS, no se adivina.
        //
        // Se deducía del nombre —«lleva @, luego es una instantánea; si no, un dataset»— y
        // así un VOLUMEN salía como «dataset». Visto en vivo: `back/tooling` es un zvol de
        // 78 G y el listado lo enseñaba igual que un sistema de ficheros, con el punto de
        // montaje a «-» como única pista. No es un detalle de rótulo: en un volumen no se
        // puede entrar, no tiene hijos que listar y no se monta.
        const std::string tipoCrudo = B::toLowerAscii(B::trim(c[10]));
        const std::string tipo = tipoCrudo == "filesystem" ? "dataset" : tipoCrudo;
        // El ESTADO: una sola columna que contesta «¿puedo llegar a esto ahora mismo?» para
        // las dos clases de cosa.
        //
        // Antes era MONTADO, y para un volumen decía «-»: nada. Lo que un volumen tiene no
        // es montaje sino DISPOSITIVO, y si está expuesto o no es justo lo que uno quiere
        // saber. No se usa `volmode` para decirlo: su valor habitual es `default` —«lo que
        // diga el parámetro del módulo»— y eso no contesta la pregunta. El agente mira si
        // existe el nodo, que es un hecho y no una intención.
        const std::string dispositivo = B::toLowerAscii(B::trim(c[11]));
        std::string estado;
        if (esSnap || tipo == "snapshot") {
            estado = "-";  // una instantánea ni se monta ni se expone
        } else if (tipo == "volume") {
            estado = dispositivo.empty() || dispositivo == "-" ? "?" : dispositivo;
        } else {
            estado = B::toLowerAscii(B::trim(c[7])) == "yes" ? "mounted" : "unmounted";
        }
        if (recursivo) {
            t.filas.push_back({nombre, esSnap ? std::string("snapshot") : tipo, c[2], c[3], estado,
                               c[8], c[1], c[4], c[5], c[6], c[7], c[9]});
            continue;
        }
        t.filas.push_back({esSnap ? "@" + nombre.substr(arroba + 1) : nombre.substr(prefijo.size()),
                           tipo, c[2], c[3], estado, c[8]});
    }
    if (!aplicaFiltro(e, t)) {
        return false;
    }
    t.imprime(e.formato);
    return true;
}

// Las propiedades: `#properties`, y `#properties/<nombre>` para una sola.
bool listaPropiedades(Estado& e, const ZfsmUrl& destino, bool delPool) {
    const std::string objetivo = destino.zfsName();
    std::string out;
    // `get tank compr*`: la propiedad es un patrón. Se piden TODAS —una sola llamada— y se
    // filtran aquí. (Por la URL, `#properties/compr*`, ya llega como filtro desde
    // ejecutaConPatrones; esto cubre el segundo argumento de `get`, que no pasa por ella.)
    // Con VARIOS destinos (`get tank/{a,b} compression`), el valor pelado de una sola
    // propiedad dejaría secciones sueltas «destino: valor». Se pide la tabla y se filtra por
    // el nombre exacto: así sale UNA tabla con DESTINO, PROPIEDAD, VALOR y ORIGEN.
    const bool variosDestinos = acumuladorActivo() != nullptr;
    if (!destino.detail.empty() && e.filtro.patron.empty()
        && (B::hasWildcard(destino.detail.front()) || variosDestinos)) {
        e.filtro.patron = B::hasWildcard(destino.detail.front())
                              ? destino.detail.front()
                              : B::percentEncodeSegment(destino.detail.front());
        e.filtro.clase = FiltroFilas::Clase::Todo;
        e.filtro.texto = destino.detail.front();
        ZfsmUrl todas = destino;
        todas.detail.clear();
        const bool ok = listaPropiedades(e, todas);
        e.filtro = FiltroFilas{};
        return ok;
    }
    if (!destino.detail.empty()) {
        if (!agente(e, destino, PET::datasetProperty(destino.detail.front(), objetivo), out)) {
            return false;
        }
        std::fprintf(stdout, "%s\n", B::trim(out).c_str());
        return true;
    }
    if (delPool && destino.pool.empty()) {
        std::fputs(TC("t_props_pool_sin_pool", "«--pool» es para un pool\n"), stderr);
        return false;
    }
    if (!agente(e, destino,
                delPool ? PET::poolProperties(destino.pool) : PET::datasetProperties(objetivo),
                out)) {
        return false;
    }
    B::json::Value raiz;
    std::string err;
    if (!B::json::parse(out, raiz, &err)) {
        std::fprintf(stderr, TC("t_respuesta__6a8a80", "respuesta ilegible de zfs get: %s\n"),
                     err.c_str());
        return false;
    }
    Tabla t;
    t.nombreJson = "properties";
    t.cabecerasTexto = {T("t_cab_propiedad", "PROPIEDAD"), T("t_cab_valor", "VALOR"),
                        T("t_cab_origen", "ORIGEN")};
    t.campos = {"property", "value", "source"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    for (const auto& ds : raiz[delPool ? "pools" : "datasets"].toObject()) {
        for (const auto& pr : ds.second["properties"].toObject()) {
            t.filas.push_back({pr.first, pr.second["value"].toString(),
                               B::toLowerAscii(pr.second["source"]["type"].toString())});
        }
    }
    if (!aplicaFiltro(e, t)) {
        return false;
    }
    t.imprime(e.formato);
    return true;
}

// Los permisos delegados: `#permissions`, o la orden `allow` sin argumentos.
//
// `zfs allow` NO tiene salida tabulada: escribe un bloque para leer, con secciones y
// entradas indentadas. Se analiza aquí para poder darlo en las tres formas, que es lo que
// permite que un guion compruebe quién tiene qué sin leer prosa.
//
//     ---- Permissions on fc16/work ----
//     Local+Descendent permissions:
//     \tuser linarese snapshot
bool listaPermisos(Estado& e, const ZfsmUrl& destino) {
    const std::string objetivo = destino.zfsName();
    if (objetivo.empty()) {
        std::fputs(TC("t_hace_falta_dc4ecf", "hace falta un dataset\n"), stderr);
        return false;
    }
    // Una INSTANTÁNEA no tiene permisos delegados: son del dataset. Se corta AQUÍ y no se
    // pregunta.
    //
    // Sin esto llegaba al usuario el error crudo de ZFS —«cannot open 'x@s': snapshot
    // delimiter '@' is not expected here» y un «Failed to open dataset» detrás—, que habla
    // de la sintaxis de una orden que él no ha escrito. Y hay una trampa peor: medido en
    // unib, `zfs allow` sobre una instantánea devuelve **rc=0** y escribe el error por la
    // salida, así que mirar el código de retorno no sirve para detectarlo.
    if (!destino.snapshot.empty()) {
        ZfsmUrl deDataset = destino;
        deDataset.snapshot.clear();
        deDataset.section.clear();
        deDataset.detail.clear();
        std::fprintf(stderr,
                     TC("t_permisos_de_instantanea",
                        "un snapshot no tiene permisos delegados: son del dataset. "
                        "Pruebe «ls #permissions --on %s»\n"),
                     B::formatZfsmUrl(deDataset).c_str());
        return false;
    }
    std::string out;
    if (!agente(e, destino, PET::permissionsOf(objetivo), out)) {
        return false;
    }
    Tabla t;
    t.nombreJson = "permissions";
    t.cabecerasTexto = {T("t_cab_alcance", "ALCANCE"), T("t_cab_clase", "CLASE"),
                        T("t_cab_quien", "QUIÉN"), T("t_cab_permisos", "PERMISOS")};
    t.campos = {"scope", "kind", "who", "permissions"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    // El análisis vive en `base/zfsallow`, no aquí.
    //
    // Estaba escrito dos veces sobre un formato que tiene una trampa: el ALCANCE va en el título de la sección y no en la
    // línea, así que una copia que se despiste concede a los descendientes lo que se quería
    // conceder solo aquí. Una sola copia, con pruebas sobre salida real.
    namespace ZA = zfsmgr::base::zfsallow;
    for (const ZA::Entry& en : ZA::parse(out)) {
        const std::string quien = en.name.empty() ? std::string(ZA::zfsToken(en.who)) : en.name;
        // Con los TEXTOS DE ZFS y no con los legibles: esta salida la leen guiones, y ya
        // decía «Local+Descendent permissions» y «user». Cambiarlos por algo más bonito
        // rompería un guion sin avisar.
        t.filas.push_back({ZA::zfsSectionTitle(en.scope), ZA::zfsToken(en.who), quien,
                           B::join(en.permissions, ",")});
    }
    t.imprime(e.formato);
    return true;
}

// Delegar o retirar permisos. Un solo cambio por orden, con el verbo genérico: el verbo por
// lotes existe para cuando la interfaz aplica una tabla entera de golpe.
bool cmdPermisos(Estado& e, const LineaAnalizada& linea, bool conceder) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    const std::string objetivo = destino.zfsName();
    if (objetivo.empty()) {
        std::fprintf(stderr, TC("t_hace_falta_6afff6", "hace falta un dataset (ahora: %s)\n"),
                     textoDe(destino).c_str());
        return false;
    }
    // Sin argumentos, `allow` LISTA. Es lo que hace `zfs allow` a secas, y quien viene de
    // ahí lo espera.
    if (conceder && pet.lista("texto").empty() && pet.valor("user").empty()
        && pet.valor("group").empty() && !pet.tiene("--everyone")) {
        ZfsmUrl conSeccion = destino;
        conSeccion.section = B::zfsmSection::kPermissions;
        return listaPermisos(e, conSeccion);
    }

    std::vector<std::string> argv{conceder ? "allow" : "unallow"};
    if (pet.tiene("-r")) {
        argv.push_back("-r");  // solo unallow: recursivo
    }
    if (pet.tiene("--local")) {
        argv.push_back("-l");
    }
    if (pet.tiene("--descend")) {
        argv.push_back("-d");
    }
    if (pet.tiene("--create")) {
        argv.push_back("-c");
    }
    int aQuien = 0;
    if (!pet.valor("user").empty()) {
        argv.push_back("-u");
        argv.push_back(pet.valor("user"));
        ++aQuien;
    }
    if (!pet.valor("group").empty()) {
        argv.push_back("-g");
        argv.push_back(pet.valor("group"));
        ++aQuien;
    }
    if (pet.tiene("--everyone")) {
        argv.push_back("-e");
        ++aQuien;
    }
    if (!pet.valor("set").empty()) {
        argv.push_back("-s");
        argv.push_back(pet.valor("set"));
        ++aQuien;
    }
    // `--create` names nobody: it is for whoever creates a descendant (`zfs allow -c`).
    if (aQuien == 0 && !pet.tiene("--create")) {
        std::fprintf(stderr,
                     TC("t_uso_s_user_a24007",
                        "uso: %s --user <u> | --group <g> | --everyone | --set @<nombre>\n"
                        "         <permiso>[,<permiso>...] [--local] [--descend] [--create]%s\n"
                        "  Sin nada, «allow» lista los permisos delegados.\n"),
                     conceder ? "allow" : "unallow", conceder ? "" : " [-r]");
        return false;
    }
    if (aQuien > 1) {
        // zfs acepta uno solo, y pasarle dos da un error suyo que no dice cuál sobra.
        std::fputs(TC("t_elija_uno__6ccf67", "elija UNO: --user, --group, --everyone o --set\n"),
                   stderr);
        return false;
    }
    // Los permisos. En `unallow` pueden omitirse: entonces se quitan TODOS los de ese
    // usuario, que es lo que hace zfs y conviene decir en la confirmación.
    if (!pet.lista("texto").empty()) {
        argv.push_back(B::join(pet.lista("texto"), ","));
    } else if (conceder) {
        std::fputs(TC("t_hace_falta_7ead59", "hace falta al menos un permiso\n"), stderr);
        return false;
    }
    argv.push_back(objetivo);

    if (!conceder
        && !confirma(e, pet.lista("texto").empty()
                            ? B::format(T("t_conf_unallow_todos",
                                          "Se van a quitar TODOS los permisos delegados de ese "
                                          "destinatario en %1. ¿Continuar?"),
                                        {objetivo})
                            : B::format(T("t_conf_unallow",
                                          "Se van a quitar los permisos %1 en %2. ¿Continuar?"),
                                        {B::join(pet.lista("texto"), ","), objetivo}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    if (!zfsGeneric(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr,
                 conceder ? TC("t_permisos_delegados_en", "permisos delegados en %s\n")
                          : TC("t_permisos_retirados_en", "permisos retirados en %s\n"),
                 objetivo.c_str());
    return true;
}

// La ruta REAL que hay detrás de `#content`, que no es la del dataset.
//
// Vive aparte de quien la lista porque hay dos que la necesitan: la orden `ls` y el
// TABULADOR, que completa nombres de fichero sin enseñar nada. `callado` es para el
// segundo: una pulsación de tabulador no debe escribir errores por pantalla.
// --- Dónde está montado un dataset, DE VERDAD.
//
// La propiedad `mountpoint` dice dónde se montaría, no dónde está. Fiarse de ella daba dos
// fallos, los dos peores que un error porque contestan algo creíble y ajeno:
//   - Sin montar, la ruta de la propiedad puede existir y ser de OTRO: un entorno de
//     arranque con `mountpoint=/` hacía que `cp …kali#content/etc/hostname` leyera el
//     /etc/hostname de la máquina, y que copiar HACIA él escribiera en el /etc de la máquina.
//   - Montado en otro sitio —a mano, o por nosotros en un temporal—, la propiedad sigue
//     diciendo `/`.
// Así que se pregunta a `zfs mount`. Y un dataset sin montar, para LEER, se monta en solo
// lectura en un temporal sin tocar sus propiedades (el verbo del agente lo explica); para
// ESCRIBIR no se monta nada: eso es `mount`, y lo decide quien lo administra.

// El montaje temporal de esta sesión para ese dataset, o vacío.
std::string montajeLecturaDe(const Estado& e, const std::string& conexion,
                             const std::string& dataset) {
    for (const auto& m : e.montajesLectura) {
        if (B::toLowerAscii(m.conexion) == B::toLowerAscii(conexion) && m.dataset == dataset) {
            return m.dir;
        }
    }
    return {};
}

// Dónde está montado según `zfs mount -j`, o vacío si no aparece.
std::string montajeReal(Estado& e, const ZfsmUrl& u, const std::string& dataset) {
    std::string out;
    if (!agente(e, u, PET::mounts(), out, 20000)) {
        return {};
    }
    B::json::Value v;
    if (!B::json::parse(out, v, nullptr)) {
        return {};
    }
    std::string mp = v["datasets"][dataset]["mountpoint"].toString();
    while (mp.size() > 1 && (mp.back() == '/' || mp.back() == '\\') && mp[mp.size() - 2] != ':') {
        mp.pop_back();
    }
    return mp;
}

// La ruta donde se abre `dataset`. `paraEscribir` no monta nada; `callado` (el tabulador) no
// monta ni dice nada: pulsar TAB no debería dejar cosas montadas.
bool dondeEstaMontado(Estado& e, const ZfsmUrl& u, const std::string& dataset, bool paraEscribir,
                      bool callado, std::string& ruta) {
    ruta = montajeLecturaDe(e, u.connection, dataset);
    if (!ruta.empty()) {
        if (paraEscribir) {
            if (!callado) {
                std::fprintf(stderr,
                             TC("t_montaje_ro_no_escribe",
                                "%s no está montado: solo se ha montado en solo lectura "
                                "para mirarlo, y ahí no se escribe. Para escribir, móntelo "
                                "en su sitio con «mount»\n"),
                             dataset.c_str());
            }
            e.ultimoRc = 1;
            return false;
        }
        return true;
    }
    std::string montado;
    if (!agente(e, u, PET::datasetProperty("mounted", dataset), montado)) {
        return false;
    }
    if (B::trim(montado) != "no") {
        ruta = montajeReal(e, u, dataset);
        if (!ruta.empty()) {
            return true;
        }
        // No aparece en `zfs mount` aunque diga que está montado: se recurre a la
        // propiedad, que es lo que se hacía siempre.
        std::string mp;
        if (!agente(e, u, PET::datasetProperty("mountpoint", dataset), mp)) {
            return false;
        }
        ruta = B::trim(mp);
        if (ruta.empty() || ruta == "none" || ruta == "-" || ruta == "legacy") {
            if (!callado) {
                std::fprintf(stderr,
                             TC("t_s_no_est_m_7fb2be", "%s no está montado en ningún sitio\n"),
                             dataset.c_str());
            }
            e.ultimoRc = 1;
            return false;
        }
        return true;
    }
    // Sin montar.
    std::string error;
    const auto* p = perfilVivoDe(e, u, error);
    const bool esWindows = p && T::isWindowsConnection(*p);
    if (paraEscribir || callado || esWindows) {
        if (callado) {
        } else if (paraEscribir) {
            std::fprintf(stderr,
                         TC("t_no_montado_escribir",
                            "%s no está montado: no se escribe en él. Para escribir, "
                            "móntelo con «mount»\n"),
                         dataset.c_str());
        } else {
            std::fprintf(stderr,
                         TC("t_no_montado_content",
                            "%s no está montado: no hay contenido que "
                            "listar\n"),
                         dataset.c_str());
        }
        e.ultimoRc = 1;
        return false;
    }
    std::string out;
    std::string fallo;
    if (!agente(e, u, PET::mountReadOnlyTemporarily(dataset), out, 60000, &fallo)) {
        std::fprintf(stderr,
                     TC("t_montaje_ro_fallo",
                        "%s no está montado, y no se pudo montar en solo lectura para "
                        "mirarlo: %s\n"),
                     dataset.c_str(), fallo.c_str());
        return false;
    }
    const auto claves = clavesDe(out);
    ruta = claves.count("MOUNTPOINT") ? B::trim(claves.at("MOUNTPOINT")) : std::string();
    if (ruta.empty()) {
        std::fprintf(stderr, "%s\n", B::trim(out).c_str());
        e.ultimoRc = 1;
        return false;
    }
    if (claves.count("TEMPORARY") && B::trim(claves.at("TEMPORARY")) == "1") {
        e.montajesLectura.push_back({u.connection, dataset, ruta});
        std::fprintf(stderr,
                     TC("t_montaje_ro_hecho",
                        "%s no está montado: se monta en solo lectura en %s mientras se "
                        "use, sin cambiar su punto de montaje\n"),
                     dataset.c_str(), ruta.c_str());
    }
    return true;
}

// Suelta los montajes temporales que ya no hacen falta: todos, o los que no contienen la
// posición. Uno ocupado —un trabajo de copia que aún lee de ahí— se queda para la próxima.
void sueltaMontajesLectura(Estado& e, bool todos) {
    std::vector<Estado::MontajeLectura> quedan;
    for (const auto& m : e.montajesLectura) {
        const bool dentro = B::toLowerAscii(e.actual.connection) == B::toLowerAscii(m.conexion)
                            && e.actual.dataset == m.dataset;
        if (!todos && dentro) {
            quedan.push_back(m);
            continue;
        }
        ZfsmUrl u;
        u.kind = ZfsmKind::Connection;
        u.connection = m.conexion;
        std::string out;
        std::string fallo;
        if (!agente(e, u, PET::unmountReadOnlyTemporary(m.dir), out, 30000, &fallo)) {
            if (todos) {
                std::fprintf(stderr,
                             TC("t_montaje_ro_queda",
                                "%s sigue montado en solo lectura en %s (%s); se suelta "
                                "con «umount %s» o al reiniciar\n"),
                             m.dataset.c_str(), m.dir.c_str(), fallo.c_str(), m.dir.c_str());
            } else {
                quedan.push_back(m);
            }
        }
    }
    e.montajesLectura = quedan;
}

bool rutaDeContenido(Estado& e, const ZfsmUrl& destino, std::string& base, bool& esWindows,
                     bool callado) {
    std::string error;
    const auto* p = perfilVivoDe(e, destino, error);
    if (!p) {
        if (!callado) {
            std::fprintf(stderr, "%s\n", error.c_str());
        }
        return false;
    }
    // Dónde empieza la ruta. Y aquí Windows NO se parece a nada de lo demás.
    //
    // En Unix es el `mountpoint` del dataset y ya está. En Windows ese mismo `mountpoint`
    // dice «/winpool/sa» y ESA RUTA NO EXISTE: el pool se monta en una letra de unidad y
    // los descendientes heredan la del POOL, no la suya. `winpool/sa` vive en `Z:\sa`.
    // Comprobado contra OldLau: `Test-Path /winpool/sa` da False y `Test-Path Z:\sa` da
    // True. Preguntar `driveletter` al dataset tampoco vale — hay que preguntárselo al
    // pool, que es de donde sale la letra.
    // ¿Está MONTADO? Un dataset sin montar conserva su `mountpoint`, y ese directorio
    // puede existir y tener cosas dentro que no son suyas: `fc16/user` dice
    // `/Users/linarese` estando desmontado, así que `ls #content` enseñaba el /Users de la
    // máquina como si fuera el contenido del dataset. Es peor que un error: es una lista
    // creíble y ajena.
    //
    // Se pregunta por el DATASET aunque se venga de una instantánea: el contenido de una
    // instantánea se lee bajo el `.zfs` del dataset, que también hace falta montado.
    base.clear();
    esWindows = T::isWindowsConnection(*p);
    if (esWindows) {
        std::string montado;
        if (!agente(e, destino, PET::datasetProperty("mounted", destino.dataset), montado)) {
            return false;
        }
        if (B::trim(montado) == "no") {
            if (!callado) {
                std::fprintf(stderr,
                             TC("t_no_montado_content",
                                "%s no está montado: no hay contenido que "
                                "listar\n"),
                             destino.dataset.c_str());
            }
            return false;
        }
        std::string letra;
        if (!agente(e, destino, PET::datasetProperty("driveletter", destino.pool), letra)) {
            return false;
        }
        letra = B::trim(letra);
        if (letra.empty() || letra == "-" || letra == "none") {
            if (!callado) {
                std::fprintf(
                    stderr,
                    TC("t_el_pool_s__020fd2", "el pool %s no tiene letra de unidad asignada\n"),
                    destino.pool.c_str());
            }
            return false;
        }
        base = letra;
        // Lo que cuelga del pool, con las barras de Windows. Si el dataset ES el pool, no
        // cuelga nada y la ruta es la raíz de la unidad.
        if (destino.dataset.size() > destino.pool.size()) {
            std::string rel = destino.dataset.substr(destino.pool.size() + 1);
            B::replaceAll(rel, "/", "\\");
            base += "\\" + rel;
        } else {
            base += "\\";
        }
        if (!destino.snapshot.empty()) {
            base += "\\.zfs\\snapshot\\" + destino.snapshot;
        }
        for (const std::string& d : destino.detail) {
            std::string t = d;
            B::replaceAll(t, "/", "\\");
            base += "\\" + t;
        }
    } else {
        // A QUIÉN se le pregunta el punto de montaje. Para una INSTANTÁNEA, al DATASET y no
        // a ella: `zfs get mountpoint pool/ds@s` contesta «-» SIEMPRE, y ese «-» caía en la
        // rama de abajo y salía «no está montado en ningún sitio» aunque el contenido fuera
        // perfectamente alcanzable. Medido en unib: con `ppal/user/srvs` montado en
        // /home/linarese/srvs, `/home/linarese/srvs/.zfs/snapshot/--1` existe y se recorre.
        //
        // El apaño de más abajo —añadir «/.zfs/snapshot/<nombre>»— ya estaba escrito, pero
        // era inalcanzable: la consulta fallaba antes de llegar a él.
        // Un VOLUMEN no es que no esté montado: es que no se monta nunca. Es un dispositivo
        // de bloques, y lo que haya dentro —si es que hay un sistema de ficheros— lo sabe
        // quien lo formateó, no ZFS. Decir «no está montado» manda a buscar un `zfs mount`
        // que no existe para esto. Para una instantánea se pregunta por su DATASET.
        std::string tipo;
        if (agente(e, destino, PET::datasetProperty("type", destino.dataset), tipo)
            && B::toLowerAscii(B::trim(tipo)) == "volume") {
            if (!callado) {
                std::fprintf(stderr,
                             TC("t_es_un_volumen",
                                "%s es un VOLUMEN: un dispositivo de bloques, no tiene "
                                "ficheros que listar\n"),
                             destino.zfsName().c_str());
            }
            return false;
        }
        // Dónde está montado DE VERDAD el dataset —también el de una instantánea, cuyo
        // contenido se lee bajo su `.zfs`—. Si no lo está, se monta en solo lectura en un
        // temporal para mirarlo: ver dondeEstaMontado.
        if (!dondeEstaMontado(e, destino, destino.dataset, /*paraEscribir=*/false, callado, base)) {
            return false;
        }
        // En una instantánea el contenido vive bajo el directorio oculto `.zfs`.
        if (!destino.snapshot.empty()) {
            base += "/.zfs/snapshot/" + destino.snapshot;
        }
        for (const std::string& d : destino.detail) {
            base += "/" + d;
        }
    }
    return true;
}

// =====================================================================================
// --- Ficheros: `#content/…` dentro de un dataset y `#path/…` en la máquina.
//
// Las dos secciones nombran un sitio del sistema de ficheros. `ubicacionDe` es la única
// puerta para saber qué hay ahí y a qué dataset pertenece: la usan `ls`, `cd`, `info` y la
// equivalencia con un dataset de `prepara`.

bool esSeccionDeFicheros(const ZfsmUrl& u) {
    const std::string sec = B::toLowerAscii(u.section);
    return sec == B::zfsmSection::kContent || sec == B::zfsmSection::kPath;
}

bool esRutaSuelta(const ZfsmUrl& u) {
    return B::toLowerAscii(u.section) == B::zfsmSection::kPath;
}

// La ruta en la máquina de una URL de ficheros.
bool rutaDeFicheros(Estado& e, const ZfsmUrl& u, std::string& ruta, bool callado) {
    if (esRutaSuelta(u)) {
        const CS::LoosePath lp = CS::loosePathOf(u);
        if (!lp.ok()) {
            if (!callado) {
                std::fprintf(stderr, "%s\n", CS::labelOf(lp.failure).c_str());
            }
            e.ultimoRc = 1;
            return false;
        }
        ruta = lp.absolute;
        return true;
    }
    bool esWindows = false;
    return rutaDeContenido(e, u, ruta, esWindows, callado);
}

bool ubicacionDe(Estado& e, const ZfsmUrl& u, Ubicacion& ub) {
    ub = Ubicacion{};
    if (!rutaDeFicheros(e, u, ub.ruta, /*callado=*/false)) {
        return false;
    }
    std::string out;
    if (!agente(e, u, PET::pathInfo(ub.ruta), out, 20000)) {
        return false;
    }
    B::json::Value v;
    std::string err;
    if (!B::json::parse(out, v, &err)) {
        std::fprintf(stderr, TC("t_contenido_ilegible", "respuesta ilegible al listar %s: %s\n"),
                     ub.ruta.c_str(), err.c_str());
        e.ultimoRc = 1;
        return false;
    }
    ub.existe = v["exists"].toBool();
    ub.esDirectorio = v["is_dir"].toBool();
    ub.tipo = v["type"].toString();
    ub.tam = v["size"].toInt();
    ub.mtime = v["mtime"].toInt();
    ub.modo = v["mode"].toInt();
    ub.dueno = v["owner"].toString();
    ub.grupo = v["group"].toString();
    ub.enlace = v["target"].toString();
    ub.dataset = v["dataset"].toString();
    ub.relativa = v["relative"].toString();
    ub.montaje = v["mountpoint"].toString();
    ub.instantanea = v["snapshot"].toString();
    ub.esMontaje = v["exact_mountpoint"].toBool();
    return true;
}

// La URL de un dataset (o de su instantánea) en una conexión.
ZfsmUrl urlDeDataset(const std::string& conexion, const std::string& dataset,
                     const std::string& instantanea) {
    ZfsmUrl d;
    d.connection = conexion;
    d.dataset = dataset;
    d.pool = dataset.substr(0, dataset.find('/'));
    d.snapshot = instantanea;
    d.kind = instantanea.empty() ? ZfsmKind::Dataset : ZfsmKind::Snapshot;
    return d;
}

// Un fichero donde la orden quiere un dataset, un pool o una instantánea.
//
// **Solo vale si ES el punto de montaje** (o la raíz de `.zfs/snapshot/<s>`), y la
// traducción se enseña antes de actuar. Estar DENTRO de un dataset no basta y nunca se sube
// al contenedor: `destroy` de un subdirectorio no puede acabar destruyendo el dataset. El
// error da la alternativa con `#content`, que es como se nombra esa ruta dentro de él.
//
// Lo mismo vale para `#content/sub` escrito a mano: antes `destroy ds#content/fotos` se
// leía como `destroy ds`, porque la sección no se miraba.
bool equivaleADataset(Estado& e, ZfsmUrl& u, const std::string& comoSeEscribio) {
    const std::string escrito = comoSeEscribio.empty() ? textoDe(u) : comoSeEscribio;
    if (!esRutaSuelta(u)) {
        if (u.detail.empty()) {
            u.section.clear();  // `ds#content` es la raíz del dataset: el dataset mismo
            return true;
        }
        ZfsmUrl d = u;
        d.section.clear();
        d.detail.clear();
        std::fprintf(
            stderr, TC("t_ruta_no_es_ds_dentro", "%s no es un dataset; es una ruta dentro de %s\n"),
            escrito.c_str(), textoDe(d).c_str());
        e.ultimoRc = 1;
        return false;
    }
    Ubicacion ub;
    if (!ubicacionDe(e, u, ub)) {
        return false;
    }
    if (!ub.dataset.empty() && ub.esMontaje) {
        const ZfsmUrl d = urlDeDataset(u.connection, ub.dataset, ub.instantanea);
        std::fprintf(stderr, "%s → %s\n", escrito.c_str(), textoDe(d).c_str());
        u = d;
        return true;
    }
    if (!ub.dataset.empty()) {
        ZfsmUrl d = urlDeDataset(u.connection, ub.dataset, ub.instantanea);
        ZfsmUrl c = d;
        c.section = B::zfsmSection::kContent;
        c.detail = B::split(ub.relativa, "/", true);
        std::fprintf(
            stderr,
            TC("t_ruta_no_es_ds_alternativa", "%s no es un dataset; está dentro de %s: %s\n"),
            escrito.c_str(),
            (ub.instantanea.empty() ? ub.dataset : ub.dataset + "@" + ub.instantanea).c_str(),
            textoDe(c).c_str());
    } else {
        std::fprintf(stderr,
                     TC("t_ruta_fuera_de_ds",
                        "%s no es un dataset: no está dentro de ningún dataset montado\n"),
                     escrito.c_str());
    }
    e.ultimoRc = 1;
    return false;
}

// Una ruta de la máquina escrita donde la orden espera una ruta: «/var/tmp» o
// `zfsm://maq#path/var/tmp`. Con la URL, la máquina va dentro, y tiene que ser la misma en
// la que actúa la orden (`maquina`); vacía, se acepta cualquiera y se devuelve en
// `conexion`. El texto plano se devuelve tal cual: es la ruta en la máquina de la orden.
bool rutaDeRanura(Estado& e, const std::string& textoEntrada, const std::string& maquina,
                  std::string& ruta, std::string* conexion) {
    const std::string texto = B::trim(textoEntrada);
    if (conexion) {
        *conexion = maquina;
    }
    if (!B::startsWith(B::toLowerAscii(texto), "zfsm://")
        && (texto.empty() || texto.front() != '#')) {
        ruta = texto;
        return true;
    }
    ZfsmUrl u;
    std::string err;
    if (!resuelve(e, texto, u, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        e.ultimoRc = 2;
        return false;
    }
    if (!esRutaSuelta(u)) {
        std::fprintf(stderr,
                     TC("t_ranura_no_es_ruta",
                        "«%s» no es una ruta de la máquina: se escribe «/ruta» o "
                        "«zfsm://<máquina>#path/ruta»\n"),
                     texto.c_str());
        e.ultimoRc = 2;
        return false;
    }
    const CS::LoosePath lp = CS::loosePathOf(u);
    if (!lp.ok()) {
        std::fprintf(stderr, "%s\n", CS::labelOf(lp.failure).c_str());
        e.ultimoRc = 2;
        return false;
    }
    if (!maquina.empty() && B::toLowerAscii(lp.connection) != B::toLowerAscii(maquina)) {
        std::fprintf(stderr, TC("t_ranura_otra_maquina", "«%s» es de %s, y la orden actúa en %s\n"),
                     texto.c_str(), lp.connection.c_str(), maquina.c_str());
        e.ultimoRc = 2;
        return false;
    }
    ruta = lp.absolute;
    if (conexion) {
        *conexion = lp.connection;
    }
    return true;
}

// Discos o ficheros de un pool: cada uno, como `rutaDeRanura` en la máquina del pool. Las
// palabras de topología —`mirror`, `raidz2`, `log`…— pasan tal cual, igual que «/dev/sdb».
bool discosDe(Estado& e, const std::vector<std::string>& textos, const std::string& maquina,
              std::vector<std::string>& out) {
    out.clear();
    for (const std::string& t : textos) {
        std::string ruta;
        if (!rutaDeRanura(e, t, maquina, ruta)) {
            return false;
        }
        out.push_back(ruta);
    }
    return true;
}

// Los permisos como los enseña `ls -l`: «drwxr-xr-x».
std::string modoComoLs(const std::string& tipo, long long modo) {
    std::string r(1, tipo.empty() || tipo == "f" ? '-' : tipo[0]);
    const char* letras = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i) {
        r += (modo & (1LL << (8 - i))) ? letras[i] : '-';
    }
    return r;
}

std::string fechaCorta(long long segundos) {
    if (segundos <= 0) {
        return {};
    }
    const std::time_t t = static_cast<std::time_t>(segundos);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::string nombreDeTipo(const std::string& letra) {
    switch (letra.empty() ? 'o' : letra[0]) {
        case 'd':
            return "directory";
        case 'f':
            return "file";
        case 'l':
            return "symlink";
        case 'b':
            return "block";
        case 'c':
            return "char";
        case 'p':
            return "fifo";
        case 's':
            return "socket";
        default:
            return "other";
    }
}

// La tabla de un directorio cualquiera, o la fila de un fichero.
bool imprimeFicheros(Estado& e, const std::vector<B::json::Value>& entradas) {
    Tabla t;
    t.nombreJson = "entries";
    t.cabecerasTexto = {T("t_cab_nombre", "NOMBRE"),        T("t_cab_tipo", "TIPO"),
                        T("t_cab_tamano", "TAMAÑO"),        T("t_cab_permisos", "PERMISOS"),
                        T("t_cab_dueno", "DUEÑO"),          T("t_cab_grupo", "GRUPO"),
                        T("t_cab_modificado", "MODIFICADO")};
    t.campos = {"name", "type", "size", "mode", "owner", "group", "modified"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Bytes, Tipo::Cadena,
               Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    std::vector<B::json::Value> orden = entradas;
    std::sort(orden.begin(), orden.end(), [](const B::json::Value& a, const B::json::Value& b) {
        return a["name"].toString() < b["name"].toString();
    });
    for (const B::json::Value& v : orden) {
        const std::string tipo = v["type"].toString();
        std::string nombre = v["name"].toString();
        if (!v["target"].toString().empty()) {
            nombre += " -> " + v["target"].toString();
        }
        t.filas.push_back(
            {nombre, nombreDeTipo(tipo), tipo == "f" ? std::to_string(v["size"].toInt()) : "",
             // Sin bits de modo —Windows— se deja en blanco: «----------»
             // diría «sin permisos», que es otra cosa.
             v["mode"].toInt() != 0 ? modoComoLs(tipo, v["mode"].toInt()) : "",
             v["owner"].toString(), v["group"].toString(), fechaCorta(v["mtime"].toInt())});
    }
    if (!aplicaFiltro(e, t)) {
        return false;
    }
    t.imprime(e.formato);
    return true;
}

// `ls` de `#path/…` o de `#content/…`: el directorio, o la fila del fichero si es un
// fichero. Las dos secciones se listan igual —nombre, tipo, tamaño, permisos, dueño, grupo y
// fecha—: antes `#content` daba solo las tres primeras y no sabía listar un fichero.
bool listaFicheros(Estado& e, const ZfsmUrl& destino) {
    std::string ruta;
    if (!rutaDeFicheros(e, destino, ruta, /*callado=*/false)) {
        return false;
    }
    std::string out;
    std::string errOrden;
    if (agente(e, destino, PET::pathContents(ruta), out, 60000, &errOrden)) {
        B::json::Value raiz;
        std::string err;
        if (!B::json::parse(out, raiz, &err)) {
            std::fprintf(stderr,
                         TC("t_contenido_ilegible", "respuesta ilegible al listar %s: %s\n"),
                         ruta.c_str(), err.c_str());
            e.ultimoRc = 1;
            return false;
        }
        std::vector<B::json::Value> entradas;
        for (const auto& v : raiz["entries"].toArray()) {
            entradas.push_back(v);
        }
        return imprimeFicheros(e, entradas);
    }
    // No es un directorio: la fila del fichero, como `ls` de un fichero en una shell.
    Ubicacion ub;
    e.ultimoRc = 0;
    if (!ubicacionDe(e, destino, ub)) {
        return false;
    }
    if (!ub.existe) {
        std::fprintf(stderr, TC("t_no_existe__608f9e", "no existe: %s\n"), ruta.c_str());
        e.ultimoRc = 1;
        return false;
    }
    B::json::Value v;
    const std::size_t barra = ruta.find_last_of("/\\");
    v.set("name", B::json::Value(barra == std::string::npos ? ruta : ruta.substr(barra + 1)));
    v.set("type", B::json::Value(ub.tipo));
    v.set("size", B::json::Value(ub.tam));
    v.set("mode", B::json::Value(ub.modo));
    v.set("owner", B::json::Value(ub.dueno));
    v.set("group", B::json::Value(ub.grupo));
    v.set("mtime", B::json::Value(ub.mtime));
    if (!ub.enlace.empty()) {
        v.set("target", B::json::Value(ub.enlace));
    }
    return imprimeFicheros(e, {v});
}

// `info` de un fichero: qué es, de quién, y a qué dataset pertenece.
bool infoDeRuta(Estado& e, const ZfsmUrl& destino) {
    Ubicacion ub;
    if (!ubicacionDe(e, destino, ub)) {
        return false;
    }
    if (!ub.existe) {
        std::fprintf(stderr, TC("t_no_existe__608f9e", "no existe: %s\n"), ub.ruta.c_str());
        e.ultimoRc = 1;
        return false;
    }
    Tabla t;
    t.cabecerasTexto = {T("t_cab_campo", "CAMPO"), T("t_cab_valor", "VALOR")};
    t.campos = {"field", "value"};
    t.filas.push_back({"url", textoDe(destino)});
    t.filas.push_back({"path", ub.ruta});
    t.filas.push_back({"kind", nombreDeTipo(ub.tipo)});
    if (!ub.enlace.empty()) {
        t.filas.push_back({"target", ub.enlace});
    }
    if (ub.tipo == "f") {
        t.filas.push_back({"size", std::to_string(ub.tam)});
    }
    if (ub.modo != 0) {
        t.filas.push_back({"mode", modoComoLs(ub.tipo, ub.modo)});
    }
    if (!ub.dueno.empty()) {
        t.filas.push_back({"owner", ub.dueno});
    }
    if (!ub.grupo.empty()) {
        t.filas.push_back({"group", ub.grupo});
    }
    t.filas.push_back({"modified", fechaCorta(ub.mtime)});
    if (!ub.dataset.empty()) {
        const ZfsmUrl d = urlDeDataset(destino.connection, ub.dataset, ub.instantanea);
        t.filas.push_back({"dataset", textoDe(d)});
        if (ub.esMontaje) {
            t.filas.push_back({"mountpoint_of", textoDe(d)});
        } else {
            ZfsmUrl c = d;
            c.section = B::zfsmSection::kContent;
            c.detail = B::split(ub.relativa, "/", true);
            t.filas.push_back({"content_url", textoDe(c)});
        }
    }
    t.imprime(e.formato);
    return true;
}

bool cmdLs(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    const std::string sec = B::toLowerAscii(destino.section);
    if (sec == B::zfsmSection::kContent || sec == B::zfsmSection::kPath) {
        return listaFicheros(e, destino);
    }
    if (sec == B::zfsmSection::kProperties) {
        return listaPropiedades(e, destino, pet.tiene("--pool"));
    }
    if (sec == B::zfsmSection::kPermissions) {
        return listaPermisos(e, destino);
    }
    if (!sec.empty()) {
        std::fprintf(stderr, TC("t_no_s_lista_57ee7d", "no sé listar la sección «%s»\n"),
                     sec.c_str());
        return false;
    }
    switch (nodoDe(destino)) {
        case Nodo::Raiz:
            return listaConexiones(e, pet);
        case Nodo::Conexion:
            return listaPools(e, destino);
        case Nodo::Dataset:
            return listaDataset(e, destino, pet.tiene("-r"));
        case Nodo::Snapshot:
            // Una instantánea no tiene hijos; lo interesante es lo que hay dentro.
            std::fputs(TC("t_una_instan_e727ff",
                          "un snapshot no tiene hijos: pruebe «ls #content» o «ls #properties»\n"),
                       stderr);
            return false;
    }
    return false;
}

// Mueve el sitio actual, sin pasar por la línea. Lo usan las órdenes que tienen que
// reubicarse tras destruir algo: `destroy` sobre donde uno está deja el sitio apuntando a
// lo que ya no existe.
//
// Existe porque `cmdCd(e, {".."})` dejó de significar lo que parecía en cuanto `cmdCd` pasó
// a recibir la línea analizada: `".."` se convertía en el `bool vacia` de la estructura, así
// que la llamada no movía nada. El compilador solo lo dijo como aviso de estrechamiento, y
// se vio en el registro del cruce a Windows, no aquí.
void vaA(Estado& e, const std::string& destino) {
    ZfsmUrl u;
    std::string err;
    if (resuelve(e, destino, u, err)) {
        e.anterior = e.actual;
        e.hayAnterior = true;
        e.actual = u;
    }
}

bool cmdCd(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    std::string texto = linea.objetivo;
    if (texto.empty()) {
        texto = pet.valor("on").empty() ? pet.valor("from") : pet.valor("on");
    }
    if (texto.empty()) {
        texto = "/";
    }
    ZfsmUrl destino;
    std::string error;
    if (!resuelve(e, texto, destino, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    // Se COMPRUEBA que existe, como haría el `cd` de cualquier intérprete. Sin esto uno se
    // queda apuntando a algo que no está y la siguiente orden falla por un motivo que no
    // parece tener nada que ver con el `cd` que la precedió.
    //
    // Cuesta una ida y vuelta, pero por el túnel ya montado son un par de milisegundos, y
    // en la raíz y en las conexiones no hace falta preguntar a nadie.
    if (nodoDe(destino) == Nodo::Dataset || nodoDe(destino) == Nodo::Snapshot) {
        std::string errPerfil;
        const auto* p = perfilVivoDe(e, destino, errPerfil);
        if (!p) {
            std::fprintf(stderr, "%s\n", errPerfil.c_str());
            return false;
        }
        // Se llama al agente DIRECTAMENTE y no por `agente()`: aquí un rc distinto de cero
        // no es un fallo, es la respuesta —«no existe»— y confundir las dos cosas daría un
        // mensaje de error donde hace falta uno que diga qué pasa.
        std::string out;
        std::string err;
        int rc = -1;
        std::string motivo;
        if (!ejecutarAgente(*e.ses, *p, PET::datasetExists(destino.zfsName()), out, err, rc,
                            &motivo, 20000)) {
            std::fprintf(stderr, TC("t_no_se_pudo_1253da", "no se pudo comprobar %s: %s\n"),
                         destino.zfsName().c_str(), motivo.c_str());
            e.ultimoRc = 1;
            return false;
        }
        if (rc != 0 || B::contains(out, "EXISTS=no")) {
            std::fprintf(stderr, TC("t_no_existe__608f9e", "no existe: %s\n"),
                         destino.zfsName().c_str());
            e.ultimoRc = 1;
            return false;
        }
    }
    // Dentro de los ficheros, lo mismo que haría cualquier shell: que exista y que sea un
    // directorio. Antes `cd zfsm://m#path/loquesea` se aceptaba sin mirar.
    if (esSeccionDeFicheros(destino)) {
        Ubicacion ub;
        if (!ubicacionDe(e, destino, ub)) {
            return false;
        }
        if (!ub.existe) {
            std::fprintf(stderr, TC("t_no_existe__608f9e", "no existe: %s\n"), ub.ruta.c_str());
            e.ultimoRc = 1;
            return false;
        }
        if (!ub.esDirectorio) {
            std::fprintf(stderr, TC("t_no_es_directorio", "no es un directorio: %s\n"),
                         ub.ruta.c_str());
            e.ultimoRc = 1;
            return false;
        }
    }
    e.anterior = e.actual;
    e.hayAnterior = true;
    e.actual = destino;
    return true;
}

// --- Acciones sobre datasets, todas por `--mutate-zfs-generic`, que recibe el argv de
// `zfs` en JSON y solo admite una lista cerrada de operaciones.
bool zfsGeneric(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv) {
    // El empaquetado lo hace la capa base: es cómo espera el daemon los argumentos.
    std::string out;
    return agente(e, destino, PET::zfsGeneric(B::helpers::agentArgv(argv)), out);
}

bool exigeDataset(const ZfsmUrl& u) {
    if (nodoDe(u) != Nodo::Dataset) {
        std::fprintf(stderr,
                     TC("t_hace_falta_fdcdb6", "hace falta estar en un dataset (ahora: %s)\n"),
                     textoDe(u).c_str());
        return false;
    }
    return true;
}

// Crear una INSTANTÁNEA. No es una orden suelta: es la rama de `create` para cuando el
// nombre empieza por `@`.
//
// Antes tenía verbo propio, `snapshot`, heredado de que en la interfaz hay un botón
// distinto. Pero el modelo del intérprete no es el de la interfaz: aquí `create` significa
// «crea un nodo donde estás» —una conexión en la raíz, un pool en una conexión, un hijo en
// un dataset—, y una instantánea es otro nodo que se crea donde estás. El `@` ya es el
// marcador que la distingue en la propia URL, y `ls` dentro de un dataset ya lista hijos e
// instantáneas como hermanos.
//
// Lo que había antes era el peor de los dos mundos: `create @x` construía el nombre
// `tank/datos/@x` y lo mandaba a ZFS, que respondía «snapshot delimiter '@' is not
// expected here». El programa tenía delante todo lo necesario para saber qué se le pedía y
// devolvía un mensaje sobre delimitadores.
bool creaInstantanea(Estado& e, const Peticion& pet, const ZfsmUrl& destinoEntrada) {
    ZfsmUrl destino = destinoEntrada;
    // Estando en una instantánea, `create @hoy` se entiende sobre SU dataset: de una
    // instantánea no cuelga nada. Lo hacía el preámbulo, que subía hasta el dataset porque
    // la orden pedía uno; ahora `create` no pide nivel —vale en todos—, así que sube aquí.
    if (nodoDe(destino) == Nodo::Snapshot) {
        destino.snapshot.clear();
        destino.kind = ZfsmKind::Dataset;
    }
    if (!exigeDataset(destino)) {
        return false;
    }
    std::string nombre = pet.lista("texto").empty() ? std::string() : pet.uno("texto");
    if (!nombre.empty() && nombre.front() == '@') {
        nombre = nombre.substr(1);
    }
    if (nombre.empty()) {
        std::fputs(TC("t_hace_falta_638a1d", "hace falta un nombre: create @<nombre>\n"), stderr);
        return false;
    }
    const bool recursivo = pet.tiene("-r");
    // Una instantánea no pasa por un argv de `zfs`, sino por un verbo del daemon que solo
    // entiende «recursiva o no». Las demás banderas de `create` —las del pool y las del
    // dataset— no tienen dónde ir, así que se DICE. Callarlas sería justo el fallo que se
    // está persiguiendo: aceptar algo y no hacerle caso.
    for (const std::string& n : pet.nativas()) {
        std::fprintf(stderr,
                     TC("t_bandera_no_instantanea",
                        "«%s» no vale al crear un snapshot: "
                        "solo -r\n"),
                     n.c_str());
        return false;
    }
    std::string out;
    if (!agente(e, destino, INST::argvCreateSnapshot(destino.dataset, nombre, recursivo), out)) {
        return false;
    }
    std::fprintf(stderr, TC("t_creada_s_s_d15312", "creada %s@%s\n"), destino.dataset.c_str(),
                 nombre.c_str());
    return true;
}

}  // namespace zfsmgr::cli::detail
