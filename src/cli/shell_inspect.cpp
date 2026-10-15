// Holds, `diff` and `devices`.

#include "shell_internal.h"

#include <set>

namespace zfsmgr::cli::detail {

// `zfs holds` NO tiene verbo tipado en el agente: se lee con una orden corriente, igual
// que hace la interfaz. Es una lectura, no una mutación.
bool cmdHolds(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;

    std::string error;
    const auto* p = perfilVivoDe(e, destino, error);
    if (!p) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    const B::ConnectionProfile perfil = conSudo(e, *p);
    std::string out;
    std::string err;
    int rc = -1;
    if (!T::runSsh(
            e.ses->transporte, perfil,
            H::withSudoCommand(perfil, "zfs holds -H " + B::shSingleQuote(destino.zfsName())),
            20000, out, err, rc, {}, {}, {}, {}, false, e.ses->verboso)
        || rc != 0) {
        const std::string detalle = B::trim(err).empty() ? B::trim(out) : B::trim(err);
        std::fprintf(stderr, "%s\n",
                     detalle.empty() ? TC("t_holds_ilegibles", "no se pudieron leer los holds")
                                     : detalle.c_str());
        e.ultimoRc = rc == 0 ? 1 : rc;
        return false;
    }
    Tabla t;
    t.nombreJson = "holds";
    t.cabecerasTexto = {T("t_cab_instantanea", "SNAPSHOT"), T("t_cab_etiqueta", "ETIQUETA"),
                        T("t_cab_creada", "CREADA")};
    t.campos = {"snapshot", "tag", "created"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    for (const std::string& linea : B::split(out, "\n", true)) {
        const std::vector<std::string> c = B::split(linea, "\t", false);
        if (c.size() >= 3) {
            t.filas.push_back({c[0], c[1], c[2]});
        }
    }
    t.imprime(e.formato);
    return true;
}

bool cmdRetencion(Estado& e, const LineaAnalizada& linea, bool poner) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    // El orden —etiqueta primero— y la validación viven en `commands::snapshots`. Se usa la
    // forma de argv de `zfs` y no el verbo tipado
    // porque el intérprete admite `-r`, y el tipado NO: ese lee exactamente dos parámetros.
    const std::vector<std::string> argv =
        poner ? INST::argvZfsHold(pet.uno("etiqueta"), destino.zfsName(), pet.tiene("-r"))
              : INST::argvZfsRelease(pet.uno("etiqueta"), destino.zfsName(), pet.tiene("-r"));
    if (argv.empty()) {
        std::fputs(TC("t_retencion_invalida",
                      "la etiqueta no puede llevar espacios, arrobas ni barras, y hay que "
                      "nombrar un snapshot\n"),
                   stderr);
        return false;
    }
    if (!zfsGeneric(e, destino, argv)) {
        return false;
    }
    // Una frase entera por caso: «puesta»/«quitada» iban como literales sin traducir, y en
    // inglés salía «puesta the hold».
    std::fprintf(stderr,
                 poner ? TC("t_hold_puesto", "puesto el hold «%s» en %s\n")
                       : TC("t_hold_quitado", "quitado el hold «%s» en %s\n"),
                 pet.uno("etiqueta").c_str(), destino.zfsName().c_str());
    return true;
}

// Qué cambió entre dos instantáneas del mismo dataset.
bool cmdDiff(Estado& e, const LineaAnalizada& lineaOriginal) {
    // Un «@snapshot» suelto en un extremo se completa con el dataset del OTRO, no con el
    // sitio actual: `diff` compara dos puntos del MISMO dataset, así que es el único que
    // puede ser. Antes `diff zfsm://c/tank/ds@b --from @a` desde la raíz contestaba «hace
    // falta estar en un dataset», con el dataset escrito en la propia orden.
    LineaAnalizada linea = lineaOriginal;
    {
        const auto itFrom = linea.opciones.find("from");
        auto itDest = linea.ranuras.find("destino");
        const bool fromSuelto = itFrom != linea.opciones.end() && !itFrom->second.empty()
                                && itFrom->second.front() == '@';
        const bool destSuelto = itDest != linea.ranuras.end() && !itDest->second.empty()
                                && itDest->second.front() == '@';
        const auto completaCon = [&](const std::string& otroTexto, const std::string& suelto,
                                     std::string& completo) {
            ZfsmUrl otro;
            std::string err;
            if (!resuelve(e, otroTexto, otro, err) || otro.zfsName().empty()) {
                return false;
            }
            otro.snapshot.clear();
            otro.section.clear();
            otro.detail.clear();
            completo = B::formatZfsmUrl(otro) + suelto;
            return true;
        };
        std::string completo;
        if (fromSuelto && itDest != linea.ranuras.end() && !destSuelto
            && completaCon(itDest->second, itFrom->second, completo)) {
            linea.opciones["from"] = completo;
        } else if (destSuelto && itFrom != linea.opciones.end() && !fromSuelto
                   && completaCon(itFrom->second, itDest->second, completo)) {
            itDest->second = completo;
        }
    }
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    if (pet.lista("destino").empty()) {
        std::fputs(TC("t_uso_diff_i_10a43a",
                      "uso: diff <@snapshot-o-dataset> [--from <@snapshot>]\n"
                      "  Compara dos puntos del MISMO dataset. Sin --from se usa el sitio\n"
                      "  actual como origen.\n"),
                   stderr);
        return false;
    }
    const ZfsmUrl& origen = pet.objetivo;
    ZfsmUrl hasta;
    std::string error;
    if (!resuelve(e, pet.uno("destino"), hasta, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    if (origen.snapshot.empty()) {
        std::fprintf(stderr,
                     TC("t_el_origen__604ce4", "el origen tiene que ser un snapshot (ahora: %s)\n"),
                     textoDe(origen).c_str());
        return false;
    }
    std::string out;
    if (!agente(e, origen, PET::diffBetween(origen.zfsName(), hasta.zfsName()), out, 120000)) {
        return false;
    }
    Tabla t;
    t.nombreJson = "changes";
    t.cabecerasTexto = {T("t_cab_cambio", "CAMBIO"), T("t_cab_ruta", "RUTA"),
                        T("t_cab_renombrado_a", "RENOMBRADO A")};
    t.campos = {"change", "path", "renamed_to"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    // `zfs diff -H` da: <marca>\t<ruta>[\t<ruta nueva>]. La marca es +, -, M o R.
    for (const std::string& linea : B::split(out, "\n", true)) {
        const std::vector<std::string> c = B::split(linea, "\t", false);
        if (c.size() >= 2) {
            t.filas.push_back({c[0], c[1], c.size() >= 3 ? c[2] : std::string()});
        }
    }
    t.imprime(e.formato);
    return true;
}

// The alias to SHOW for a device: the one a person can read. On Linux a disk has several
// by-id names; the model-and-serial one (`ata-…`, `nvme-Samsung…`, `mmc-…`) says which disk
// it is, the `wwn-…` and `nvme-eui.…` ones only a number. Whole, as a path: it is what gets
// copied into `create <pool> …`.
static std::string readableAlias(const std::vector<std::string>& aliases) {
    for (const std::string& a : aliases) {
        const std::size_t barra = a.rfind('/');
        const std::string n = barra == std::string::npos ? a : a.substr(barra + 1);
        if (n.rfind("wwn-", 0) != 0 && n.rfind("nvme-eui.", 0) != 0
            && n.rfind("nvme-nvme.", 0) != 0) {
            return a;
        }
    }
    return aliases.empty() ? std::string() : aliases.front();
}

// --- Información suelta.

bool cmdDevices(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (destino.connection.empty()) {
        std::fputs(TC("t_falta_conexion", "hace falta estar en una conexión"), stderr);
        std::fputc('\n', stderr);
        return false;
    }
    std::string out;
    if (!agente(e, destino, PET::blockDevices(), out, 25000)) {
        return false;
    }
    B::json::Value raiz;
    std::string err;
    if (!B::json::parse(out, raiz, &err)) {
        std::fprintf(stderr,
                     TC("t_resp_devices", "respuesta ilegible al listar dispositivos: %s\n"),
                     err.c_str());
        return false;
    }
    // Salen TODOS, con la columna que dice si están ocupados, y no solo los libres.
    // Esconder los ocupados escondería justo el disco que uno va a reutilizar a propósito
    // —un `zfs_member` de un pool viejo, por ejemplo—, y son pocos: aquí caben en pantalla.
    const bool soloLibres = pet.tiene("--free");

    // --- Un dispositivo REAL por fila, y sus alias debajo.
    //
    // El agente manda los `/dev/disk/by-id/...` como entradas aparte, cada una con
    // `resolved` apuntando al dispositivo de verdad. El cliente tiraba ese campo y las
    // pintaba como si fueran discos más: en esta máquina salían 36 filas de las que 27 eran
    // el MISMO disco visto por otro nombre, con tamaño 0B y sin tipo ni sistema de ficheros.
    // Tres cuartas partes del listado eran ruido, y el ruido tapaba justo lo que se viene a
    // ver: qué discos hay y cuáles están libres.
    //
    // Los alias NO se tiran, se subordinan. Son la forma correcta de nombrar un disco al
    // crear un pool —`/dev/sdb` puede ser `sdc` en el siguiente arranque y el pool se rompe;
    // el alias por identificador no cambia—, así que esconderlos sería cambiar un defecto
    // por otro.
    struct Fila {
        std::string path, size, type, fstype, mountpoint, parent;
        bool ocupado = false;
        std::vector<std::string> alias;
    };
    std::vector<Fila> filas;
    std::map<std::string, std::size_t> porRuta;  // ruta real -> posición en `filas`
    std::vector<B::json::Value> sueltos;         // alias cuyo destino no está en la lista

    for (const auto& d : raiz["devices"].toArray()) {
        if (!d["resolved"].toString().empty()) {
            continue;  // los alias en la segunda pasada, cuando ya están todos los reales
        }
        Fila f;
        f.path = d["path"].toString();
        f.size = std::to_string(d["size"].toInt());
        f.type = d["type"].toString();
        f.fstype = d["fstype"].toString();
        f.mountpoint = d["mountpoint"].toString();
        f.parent = d["parent"].toString();
        f.ocupado = d["inuse"].toBool();
        porRuta[f.path] = filas.size();
        filas.push_back(f);
    }
    for (const auto& d : raiz["devices"].toArray()) {
        const std::string destino = d["resolved"].toString();
        if (destino.empty()) {
            continue;
        }
        const auto it = porRuta.find(destino);
        if (it == porRuta.end()) {
            // Un alias que apunta a algo que no está en la lista NO se pierde: se queda como
            // fila propia. Perder una entrada en silencio al reorganizar un listado es
            // exactamente la clase de arreglo que crea un problema peor que el que resuelve.
            sueltos.push_back(d);
            continue;
        }
        filas[it->second].alias.push_back(d["path"].toString());
    }
    for (const auto& d : sueltos) {
        Fila f;
        f.path = d["path"].toString();
        f.size = std::to_string(d["size"].toInt());
        f.type = d["type"].toString();
        f.fstype = d["fstype"].toString();
        f.mountpoint = d["mountpoint"].toString();
        f.ocupado = d["inuse"].toBool();
        filas.push_back(f);
    }
    // Orden estable dentro de cada dispositivo: los alias llegan en el orden del directorio,
    // que es el de creación del sistema de ficheros y no significa nada.
    for (Fila& f : filas) {
        std::sort(f.alias.begin(), f.alias.end());
    }

    Tabla t;
    t.nombreJson = "devices";
    t.cabecerasTexto = {T("t_cab_ruta", "RUTA"),
                        T("t_cab_tamano", "TAMAÑO"),
                        T("t_cab_tipo", "TIPO"),
                        T("t_cab_fs", "FS"),
                        T("t_cab_punto_de_montaje", "PUNTO DE MONTAJE"),
                        T("t_cab_ocupado", "OCUPADO")};
    t.campos = {"path", "size", "type", "fstype", "mountpoint", "inuse"};
    t.tipos = {Tipo::Cadena, Tipo::Bytes, Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Booleano};

    // To READ, one row per device and an ID column with ONE stable name, as a full path: the
    // aliases used to go below, one line each, and in the boxed table each one crossed the
    // whole width —two or three by-id lines per disk buried the six columns that matter—.
    // For a SCRIPT every alias goes in a column, which tsv can cut and json turns into an
    // array.
    const bool paraLeer = (e.formato == Formato::Texto);
    if (paraLeer) {
        t.cabecerasTexto.push_back(T("t_cab_id", "ID"));
        t.campos.push_back("id");
    } else {
        t.cabecerasTexto.push_back(T("t_cab_alias", "ALIAS"));
        t.campos.push_back("aliases");
        t.tipos.push_back(Tipo::Lista);
        // json only: the disk a partition is on, and which pool claims the device —an
        // imported one, or the label of one that is not imported (what labelclear is for)—.
        t.campos.insert(t.campos.end(), {"parent", "pool", "pool_state", "pool_imported"});
        t.tipos.insert(t.tipos.end(), {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena, Tipo::Booleano});
    }
    struct Dueno {
        std::string pool, estado;
        bool importado = false;
    };
    std::map<std::string, Dueno> duenos;  // real path -> the pool that claims it
    if (!paraLeer) {
        const auto apunta = [&](const H::ImportablePoolInfo& info, bool importado) {
            std::vector<std::string> nombres;
            for (const H::ImportableDevice& d : info.devices) {
                if (d.leaf) {
                    nombres.push_back(d.name);
                }
            }
            std::string resueltos;
            if (nombres.empty()
                || !agente(e, destino, PET::resolveDevices(nombres), resueltos, 20000)) {
                return;
            }
            for (const auto& kv : clavesDe(resueltos)) {
                for (const std::string& ruta : {kv.first, kv.second}) {
                    if (!ruta.empty() && !duenos.count(ruta)) {
                        duenos[ruta] = Dueno{info.pool, info.state, importado};
                    }
                }
            }
        };
        std::set<std::string> importados;
        std::string lista;
        B::json::Value pools;
        if (agente(e, destino, PET::poolList(), lista, 20000)
            && B::json::parse(lista, pools, nullptr)) {
            for (const auto& kv : pools["pools"].toObject()) {
                importados.insert(kv.first);
                std::string estado;
                if (agente(e, destino, PET::poolStatus(kv.first), estado, 20000)) {
                    for (const H::ImportablePoolInfo& info : H::parseZpoolImportOutput(estado)) {
                        apunta(info, true);
                    }
                }
            }
        }
        std::string sonda;
        if (agente(e, destino, PET::importableProbe(), sonda, 60000)) {
            for (const H::ImportablePoolInfo& info : H::parseZpoolImportOutput(sonda)) {
                if (!importados.count(info.pool)) {
                    apunta(info, false);
                }
            }
        }
        // A partition of a pool claims its disk too: the disk is not free.
        for (const Fila& f : filas) {
            const auto it = duenos.find(f.path);
            if (it != duenos.end() && !f.parent.empty() && !duenos.count(f.parent)) {
                duenos[f.parent] = it->second;
            }
        }
    }
    const auto duenoDe = [&](const Fila& f) -> Dueno {
        if (const auto it = duenos.find(f.path); it != duenos.end()) {
            return it->second;
        }
        for (const std::string& a : f.alias) {
            if (const auto it = duenos.find(a); it != duenos.end()) {
                return it->second;
            }
        }
        return {};
    };
    for (const Fila& f : filas) {
        if (soloLibres && f.ocupado) {
            continue;
        }
        std::vector<std::string> fila{f.path,   f.size,       f.type,
                                      f.fstype, f.mountpoint, f.ocupado ? "true" : "false"};
        if (!paraLeer) {
            fila.push_back(B::join(f.alias, ","));
            const Dueno d = duenoDe(f);
            fila.insert(fila.end(), {f.parent, d.pool, d.estado,
                                     d.pool.empty() ? "" : (d.importado ? "true" : "false")});
            t.filas.push_back(fila);
            continue;
        }
        fila.push_back(readableAlias(f.alias));
        t.filas.push_back(fila);
    }
    t.imprime(e.formato);
    return true;
}

// El punto de montaje de un dataset. Solo Unix: en Windows la ruta no es la que dice
// `mountpoint` —el pool se monta en una letra de unidad— y ahí la sincronización va por
// otro camino, que no está portado al intérprete.
// Dónde se abre el dataset de un extremo de `cp` o `rsync`. Se pregunta a `zfs mount`, no a
// la propiedad: sin montar, la ruta de la propiedad puede ser de otro (ver dondeEstaMontado).
// Para leer, un dataset sin montar se monta en solo lectura; para escribir, no.
bool montajeDe(Estado& e, const ZfsmUrl& u, std::string& out, bool paraEscribir) {
    return dondeEstaMontado(e, u, u.zfsName(), paraEscribir, /*callado=*/false, out);
}

// La copia entre máquinas conserva ACL y atributos extendidos, pero no todos caben en un
// sistema de otra familia —una ACL POSIX de Linux no significa nada en FreeBSD, y en Windows
// no cabe ninguno—. El agente que envía cuenta las entradas que llevaban algo que el destino
// no puede guardar (`SIN_ACL_XATTR=n`), y aquí se dice en el idioma de quien lo lee.
void avisaSinMetadatos(const std::string& salida) {
    const std::size_t pos = salida.find("SIN_ACL_XATTR=");
    if (pos == std::string::npos) {
        return;
    }
    const std::size_t ini = pos + std::string("SIN_ACL_XATTR=").size();
    const std::size_t fin = salida.find('\n', ini);
    const std::string n =
        B::trim(salida.substr(ini, fin == std::string::npos ? std::string::npos : fin - ini));
    if (n.empty() || n == "0") {
        return;
    }
    std::fprintf(stderr,
                 TC("t_aviso_meta_no_cabe",
                    "aviso: %s entradas llevan ACL o atributos extendidos que la máquina de "
                    "destino, de otro sistema, no puede guardar; esos no se han copiado\n"),
                 n.c_str());
}

// «Sincronizar» de la interfaz: copia el CONTENIDO de un dataset a otro con rsync.
//
// Faltaba entera en el intérprete, y encima el hueco estaba disimulado: existía un `sync`
// que es `zpool sync` —forzar la escritura pendiente, cosa de un instante— con el mismo
// nombre que la acción de la interfaz, que puede tardar horas. Ese se llama ahora `flush`.
// Copiar un árbol de una ruta a otra, eligiendo motor según si cruzan máquina.
//
// Los dos motores ya existían y los usa `rsync`: dentro de una máquina, el `rsync` del
// daemon; entre dos, el árbol entre agentes. Esto no añade ninguno, solo los pone detrás de
// una misma llamada para que `cp` no tenga que saber de los dos.
// El último tramo de una ruta, con «/» o con «\».
std::string hojaDe(const std::string& ruta) {
    const std::size_t barra = ruta.find_last_of("/\\");
    return barra == std::string::npos ? ruta : ruta.substr(barra + 1);
}

// ¿Qué hay en `ruta` de la máquina `conexion`? Lo contesta `--dump-path-info`, que mira
// cualquier ruta y no solo las de dentro de un montaje. Falso si no se pudo preguntar (el
// motivo ya está dicho); `existe` falso si se preguntó y no hay nada.
bool estadoDeRuta(Estado& e, const std::string& conexion, const std::string& ruta, bool& existe,
                  bool& esDirectorio) {
    existe = false;
    esDirectorio = false;
    ZfsmUrl u;
    u.kind = ZfsmKind::Connection;
    u.connection = conexion;
    std::string out;
    if (!agente(e, u, PET::pathInfo(ruta), out, 20000)) {
        return false;
    }
    B::json::Value v;
    std::string err;
    if (!B::json::parse(out, v, &err)) {
        std::fprintf(stderr, TC("t_contenido_ilegible", "respuesta ilegible al listar %s: %s\n"),
                     ruta.c_str(), err.c_str());
        e.ultimoRc = 1;
        return false;
    }
    existe = v["exists"].toBool();
    esDirectorio = v["is_dir"].toBool();
    return true;
}

// ¿El agente de `conexion` anuncia `capacidad` en su `--health`? Para lo que un agente
// anterior no rechazaría sino que IGNORARÍA en silencio, como el `--as` del emisor del árbol.

// `ruta` partida en su directorio y su último tramo. En Windows el padre de «C:/x» es «C:/»,
// no «C:», que sería «el directorio actual de la unidad C».
bool padreYHoja(const std::string& ruta, std::string& padre, std::string& hoja) {
    std::string r = ruta;
    while (r.size() > 1 && (r.back() == '/' || r.back() == '\\')) {
        r.pop_back();
    }
    const std::size_t barra = r.find_last_of("/\\");
    if (barra == std::string::npos || barra + 1 >= r.size()) {
        return false;
    }
    hoja = r.substr(barra + 1);
    padre = barra == 0 ? std::string("/") : r.substr(0, barra);
    if (padre.size() == 2 && padre[1] == ':') {
        padre += "/";
    }
    return hoja != "." && hoja != "..";
}

// ¿`ruta` es un fichero —y no un directorio— en la máquina de `u`? Se mira en el listado de
// su directorio padre, que es lo que el agente ya sabe contestar. Si no se puede saber, se
// dice que no: el camino de siempre, el del árbol.
bool esFicheroEn(Estado& e, const ZfsmUrl& u, const std::string& ruta) {
    const std::size_t barra = ruta.find_last_of("/\\");
    if (barra == std::string::npos || barra + 1 >= ruta.size()) {
        return false;
    }
    const std::string padre = barra == 0 ? std::string("/") : ruta.substr(0, barra);
    const std::string hoja = ruta.substr(barra + 1);
    std::string out;
    std::vector<L::DirectoryEntry> entradas;
    std::string errJson;
    // `pathContents` y no `directoryContents`: este último se queda dentro de los montajes de
    // ZFS, y un extremo `#path` puede estar en cualquier sitio. Con él, un fichero suelto
    // fuera de ZFS se tomaba por directorio.
    if (!agente(e, u, PET::pathContents(padre), out)
        || !L::directoryContents(out, entradas, errJson)) {
        return false;
    }
    for (const L::DirectoryEntry& ent : entradas) {
        if (ent.name == hoja) {
            return !ent.directory;
        }
    }
    return false;
}

bool copiaArbol(Estado& e, const std::string& maqOrigen, const std::string& rutaOrigen,
                const std::string& maqDestino, const std::string& baseDestino,
                const std::string& nombre, bool simula, bool esperar) {
    const std::string rutaDestino =
        baseDestino + (baseDestino.empty() || baseDestino.back() == '/' ? "" : "/") + nombre;
    const bool mismaMaquina = B::toLowerAscii(maqOrigen) == B::toLowerAscii(maqDestino);
    ZfsmUrl uOrigen;
    uOrigen.kind = ZfsmKind::Connection;
    uOrigen.connection = maqOrigen;

    if (mismaMaquina) {
        // El motor PROPIO del proyecto, no el `rsync` del sistema.
        //
        // `copytree` es el que ya usan `todir`, `breakdown` y `assemble`, y hace todo lo que
        // hace falta: ficheros dispersos —un disco de máquina virtual de 100 GB con 8 GB
        // usados no llega ocupando 100—, fechas con nanosegundos, enlaces duros, dueño y
        // modo. Llamar al `rsync` del sistema era una dependencia externa para hacer lo
        // mismo, y dos motores locales que se comportan parecido pero no igual.
        //
        // La orden `rsync` sigue usando el suyo: ahí lo que se pide es ESPEJAR, con
        // `--delete`, y eso es otra conversación que no toca cambiar de paso.
        const std::string carga =
            SY::copyLocalPayload({{rutaOrigen, rutaDestino}}, /*borra=*/false, simula);
        if (carga.empty()) {
            std::fputs(
                TC("t_cp_rutas_mal", "las rutas de origen y destino tienen que ser absolutas\n"),
                stderr);
            return false;
        }
        const std::vector<std::string> argv = PET::copyLocal(carga);
        // Una simulación se LEE ahora: no hay nada que encolar y lo que uno quiere es verlo.
        if (simula) {
            std::string out;
            if (!agente(e, uOrigen, argv, out, 0)) {
                return false;
            }
            std::fprintf(stdout, "%s", out.c_str());
            return true;
        }
        // Lo mismo que hace `lanzaOEspera`, pero sin fabricar una `Peticion` falsa solo
        // para que lea de ella una bandera que ya tenemos aquí como un `bool`.
        if (!esperar) {
            return enviaComoTrabajo(e, uOrigen, argv);
        }
        std::string out;
        if (!agente(e, uOrigen, argv, out, 0)) {  // sin plazo: mueve datos de verdad
            return false;
        }
        std::fprintf(stdout, "%s", out.c_str());
        return true;
    }

    // Entre máquinas: el destino escucha y el origen le manda el árbol directamente, sin
    // pasar por aquí. El directorio de destino tiene que EXISTIR —lo exige el receptor— y
    // eso no se arregla creándolo: crear rutas arbitrarias en una máquina remota es
    // justamente la superficie que este proyecto ha decidido no tener.
    std::string errSrc;
    std::string errDst;
    ZfsmUrl uDestino;
    uDestino.kind = ZfsmKind::Connection;
    uDestino.connection = maqDestino;
    const auto* pSrc = perfilDe(e, uOrigen, errSrc);
    const auto* pDst = perfilDe(e, uDestino, errDst);
    if (!pSrc || !pDst) {
        std::fprintf(stderr, "%s\n", (pSrc ? errDst : errSrc).c_str());
        return false;
    }
    const B::ConnectionProfile src = conSudo(e, *pSrc);
    const B::ConnectionProfile dst = conSudo(e, *pDst);
    const auto llama = [&e](const B::ConnectionProfile& maquina,
                            const std::vector<std::string>& argv, int timeoutMs,
                            std::string& salida, std::string& error, int& rc) {
        return ejecutarAgente(*e.ses, maquina, argv, salida, error, rc, nullptr, timeoutMs);
    };
    // ¿Es un FICHERO? Entonces el receptor se pone en el directorio de destino —que ya
    // existe— y el origen manda un árbol de una sola entrada con el nombre del fichero.
    //
    // Hacía falta saberlo AQUÍ, antes de preparar nada: el paso siguiente creaba en el
    // destino un directorio con el nombre del fichero, y el envío fallaba después con «no es
    // un directorio». Quedaba basura con forma de éxito a medias.
    const bool esFichero = esFicheroEn(e, uOrigen, rutaOrigen);
    // El directorio donde aterriza tiene que EXISTIR antes: el receptor del árbol no lo
    // crea y se planta con «no es un directorio». Se crea con un verbo que solo puede crear
    // DENTRO de algo que ya está, no en cualquier parte del sistema.
    //
    // En una simulación no se crea nada: `--check` promete no tocar, y crear un directorio
    // es tocar. Un fichero no lo necesita: aterriza en `baseDestino`, que ya existe.
    if (!simula && !esFichero) {
        std::string prepOut;
        std::string prepErr;
        int prepRc = -1;
        const std::vector<std::string> prep = AV::argvPrepareDir(baseDestino, nombre);
        if (prep.empty()
            || !ejecutarAgente(*e.ses, dst, prep, prepOut, prepErr, prepRc, nullptr, 60000)
            || prepRc != 0) {
            std::fprintf(stderr, TC("t_cp_destino_no", "no se pudo preparar el destino: %s\n"),
                         B::trim(B::helpers::oneLine(prepErr)).c_str());
            return false;
        }
    }
    std::string salidaEnvio;
    const auto hecho = TR::launchTreeJob(
        e.ses->transporte, llama, src, dst, rutaOrigen, esFichero ? baseDestino : rutaDestino,
        /*mismaConexion=*/false, e.ses->verboso,
        /*comoTrabajo=*/!esperar && !simula, /*borra=*/false, simula, &salidaEnvio,
        esFichero && nombre != hojaDe(rutaOrigen) ? nombre : std::string());
    if (hecho.failure != TR::JobFailure::None_) {
        std::fprintf(stderr, "%s: %s\n", TR::labelOf(hecho.failure).c_str(), hecho.detail.c_str());
        e.ultimoRc = 1;
        return false;
    }
    std::fputs(salidaEnvio.c_str(), stdout);
    avisaSinMetadatos(salidaEnvio);
    return true;
}

}  // namespace zfsmgr::cli::detail
