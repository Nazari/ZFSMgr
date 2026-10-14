// Operations that move dataset contents: temporary mountpoints, sync through tar, ToDir,
// Breakdown and Assemble, the repair of datasets left on a temporary mountpoint, and the copy
// within one machine (copytree).

#include "agent_internal.h"

namespace zfsmgr::agent {

// Cuenta los ficheros que a rsync todavía le quedarían por transferir, en seco.
//
// Es la verificación que hacía el shell antiguo antes de borrar el origen y que se
// perdió al pasar a nativo: la versión intermedia borraba en cuanto rsync devolvía
// 0. Un rsync que termina bien pero deja algo atrás (disco lleno a mitad, un fichero
// que cambió durante la copia) convertía el desglose en pérdida de datos.
//
// El formato itemizado de "-i" pone las banderas en los 11 primeros caracteres;
// ">f" significa fichero que se transferiría. --ignore-existing hace que solo
// aparezca lo que falta, no lo que difiere.
// Lo que todavía FALTA en el destino. Ya NO usa rsync.
//
// Devuelve -1 si no se pudo comprobar, y esa distinción es la que impide perder datos:
// el llamante destruye el origen cuando ve un cero, así que «no lo sé» jamás puede
// contarse como «no falta nada».
int countPendingCopyTransfers(const std::string& srcDir, const std::string& dstDir,
                              const std::vector<std::string>& excludes, bool oneFileSystem) {
    // Las MISMAS exclusiones que la copia: si no, lo que se dejó fuera a propósito
    // contaría como pendiente y la verificación no llegaría nunca a cero.
    zfsmgr::copytree::Options opt;
    opt.oneFileSystem = oneFileSystem;
    opt.excludes = conExclusionesDelSistema(excludes);
    const long long pending = zfsmgr::copytree::countPending(srcDir, dstDir, opt);
    if (pending < 0) {
        return -1;
    }
    return static_cast<int>(pending);
}

// Borrado que NO cruza puntos de montaje. `fs::remove_all` los ignora: si dentro del
// árbol hay algo montado —un subdataset, otro pool, cualquier cosa— entra y borra su
// contenido. Aquí se compara el dispositivo contra el de la raíz y, si cambia, se
// aborta en vez de destruir. Es un tope duro: aunque el desmontaje previo se deje
// alguno, lo peor que pasa es que la operación falle.
#ifndef _WIN32
bool removeTreeWithoutCrossingMounts(const std::filesystem::path& node, dev_t dev,
                                     std::string& err) {
    namespace fs = std::filesystem;
    struct stat st{};
    if (::lstat(node.c_str(), &st) != 0) {
        err = "could not stat " + node.string();
        return false;
    }
    if (st.st_dev != dev) {
        err = "there is a filesystem mounted at " + node.string()
              + "; deletion aborted so as not to destroy its contents";
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        std::error_code ec;
        for (fs::directory_iterator it(node, ec), end; !ec && it != end; it.increment(ec)) {
            if (!removeTreeWithoutCrossingMounts(it->path(), dev, err)) {
                return false;
            }
        }
        if (ec) {
            err = "could not list " + node.string();
            return false;
        }
    }
    if (::remove(node.c_str()) != 0) {
        err = "could not delete " + node.string();
        return false;
    }
    return true;
}
#endif

// Desglosar: un subdirectorio pasa a ser un dataset hijo que ocupa su sitio.
//
// **En Windows no se puede, y por eso aquí se niega.** No es que falte portarlo: el verbo
// funcionaba —creaba los datasets y movía los datos sin perder un byte— pero el resultado NO
// era el que promete. Windows OpenZFS monta los descendientes PLANOS bajo la letra del pool y
// no admite fijarles un punto de montaje: `zfs set mountpoint` allí solo acepta una letra de
// unidad, `none` o `legacy`.
//
// O sea que desglosar «Z:\\des\\fotos» dejaba el dataset `winpool/des/fotos` montado en
// «Z:\\fotos», en la RAÍZ de la unidad. Comprobado contra OldLau: los ficheros seguían ahí
// —`Z:\\fotos\\a.txt`, 8 bytes— pero ya no donde el usuario los tenía, y la operación decía
// «[BREAKDOWN] ok».
//
// Mover los datos de sitio sin decirlo es peor que no hacer nada, así que no se hace. Y como
// el verbo deja de anunciarse en `CAPS`, la interfaz lo enseña deshabilitado con su motivo en
// vez de ofrecer algo que no cumple.
//
// Ensamblar —el camino inverso— SÍ funciona allí y se conserva: verificado en la misma
// máquina, los datasets vuelven a ser directorios en su sitio.
#ifndef _WIN32

// «local», «received», «inherited», «default» —o vacío si no se pudo saber—.
std::string origenDeMountpoint(const std::string& ds) {
    const ExecResult r = runExecCapture("zfs", {"get", "-H", "-o", "source", "mountpoint", ds});
    if (r.rc != 0) {
        return {};
    }
    const std::string o = trim(r.out);
    if (o.rfind("inherited", 0) == 0) {
        return "inherited";
    }
    if (o == "local" || o == "received" || o == "default") {
        return o;
    }
    return {};
}

// Devuelve el punto de montaje como estaba, con su origen, SIN MONTAR NADA.
//
// Cambiar el `mountpoint` de un dataset MONTA todos los de su árbol que lo heredan y tienen
// `canmount=on`, aunque no estuvieran montados: libzfs los recoge en su lista de cambios y
// los monta al final. Medido en fc16 sobre un pool de prueba, y vivido en la VM de FreeBSD:
// devolverle a un entorno de arranque su `/` montó sus hijos nuevos en el /usr y el /var de
// la máquina, y la máquina se quedó sin login ni sshd.
//
// `zfs set -u` cambia sin montar. Pero devolver un valor RECIBIDO o heredado es `zfs inherit`,
// que no tiene `-u`; así que mientras tanto los del árbol con `canmount=on` pasan a `noauto`,
// que no se monta solo, y después vuelven a `on`, que tampoco monta al ponerlo (medido).
// `yaMontados`: los que estaban montados antes de empezar. A esos no se les protege —que ZFS
// los vuelva a montar en su sitio de siempre es lo que tiene que pasar— y así no se les
// cambia `canmount` de «default» a «local» sin necesidad.
bool restauraMountpoint(const std::string& ds, const std::string& valor, const std::string& origen,
                        std::string& err, const std::set<std::string>& yaMontados) {
    ExecResult res;
    if (origen == "received" || origen == "inherited" || origen == "default") {
        std::vector<std::string> protegidos;
        const ExecResult cm = runExecCapture(
            "zfs", {"get", "-H", "-r", "-t", "filesystem", "-o", "name,value", "canmount", ds});
        for (const std::string& linea : splitLines(cm.out)) {
            const std::size_t tab = linea.find('\t');
            if (tab != std::string::npos && trim(linea.substr(tab + 1)) == "on"
                && !yaMontados.count(linea.substr(0, tab))) {
                const std::string nombre = linea.substr(0, tab);
                if (runExecCapture("zfs", {"set", "canmount=noauto", nombre}).rc == 0) {
                    protegidos.push_back(nombre);
                }
            }
        }
        res = origen == "received" ? runExecCapture("zfs", {"inherit", "-S", "mountpoint", ds})
                                   : runExecCapture("zfs", {"inherit", "mountpoint", ds});
        for (const std::string& n : protegidos) {
            (void)runExecCapture("zfs", {"set", "canmount=on", n});
        }
    } else if (!valor.empty() && valor != "-") {
        res = runExecCapture("zfs", {"set", "-u", "mountpoint=" + valor, ds});
    } else {
        err = "no saved mountpoint for " + ds;
        return false;
    }
    // Y se comprueba que quedó el VALOR de antes: `inherit -S` sin valor recibido, por
    // ejemplo, hereda, y eso no es lo que había.
    const ExecResult ahora = getZfsPropertyCapture(ds, "mountpoint");
    if (res.rc == 0 && ahora.rc == 0 && !valor.empty() && valor != "-"
        && trim(ahora.out) != valor) {
        res = runExecCapture("zfs", {"set", "-u", "mountpoint=" + valor, ds});
    }
    if (res.rc != 0) {
        err = trim(res.err.empty() ? res.out : res.err);
        return false;
    }
    return true;
}

// Desmonta lo que esté montado DENTRO de `dir` y cuelgue de `ds` (sus descendientes y él),
// de lo más profundo a lo menos. Hay que hacerlo antes de devolver el punto de montaje: si
// no, ZFS remonta lo montado en el sitio nuevo, y con `mountpoint=/` eso es la raíz de la
// máquina.
void desmontaBajo(const std::string& ds, const std::string& dir) {
    const ExecResult mnt = runExecCapture("zfs", {"mount"});
    if (mnt.rc != 0 || dir.empty()) {
        return;
    }
    std::vector<std::pair<std::string, std::string>> dentro;
    for (const std::string& linea : splitLines(mnt.out)) {
        std::istringstream iss(linea);
        std::string nombre;
        std::string ruta;
        iss >> nombre >> ruta;
        const bool suyo = nombre == ds || nombre.rfind(ds + "/", 0) == 0;
        const bool bajo = ruta == dir || ruta.rfind(dir + "/", 0) == 0;
        if (suyo && bajo) {
            dentro.emplace_back(nombre, ruta);
        }
    }
    std::sort(dentro.begin(), dentro.end(),
              [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });
    // Varias pasadas: un hijo TAPADO por su padre —montado después, encima— no se puede
    // desmontar por su ruta hasta que se quita el padre. En la siguiente pasada ya se ve.
    for (int pasada = 0; pasada < 3 && !dentro.empty(); ++pasada) {
        std::vector<std::pair<std::string, std::string>> quedan;
        for (const auto& d : dentro) {
            if (runExecCapture("zfs", {"unmount", d.first}).rc == 0 || !datasetIsMounted(d.first)) {
                continue;
            }
            // `zfs unmount` del padre intenta antes desmontar lo que cuelga de su ruta, y si
            // eso está tapado, falla. `umount <ruta>` quita el montaje de más arriba en esa
            // ruta, que es el padre; en la pasada siguiente sus hijos ya se ven.
            if (runExecCapture("umount", {d.second}).rc == 0 && !datasetIsMounted(d.first)) {
                continue;
            }
            quedan.push_back(d);
        }
        dentro = quedan;
    }
}

// Los del árbol de `ds` (él incluido) que están montados ahora.
std::set<std::string> montadosDelArbol(const std::string& ds) {
    std::set<std::string> salida;
    const ExecResult mnt = runExecCapture("zfs", {"mount"});
    for (const std::string& linea : splitLines(mnt.out)) {
        std::istringstream iss(linea);
        std::string nombre;
        iss >> nombre;
        if (nombre == ds || nombre.rfind(ds + "/", 0) == 0) {
            salida.insert(nombre);
        }
    }
    return salida;
}

#endif

ExecResult runMutateAdvancedBreakdownCapture(const std::vector<std::string>& params) {
#ifdef _WIN32
    (void)params;
    ExecResult noAqui;
    noAqui.rc = 2;
    ponCodigo(noAqui, "breakdown_windows", {});
    return noAqui;
#endif
    ExecResult r;
    // Pares (directorio, nombre). El nombre es la ruta BAJO `dataset`, y no tiene por
    // qué reflejar la ruta del directorio: crear un dataset solo exige que exista su
    // padre POR NOMBRE, no que el directorio de encima sea un dataset. Así se puede
    // convertir `a/b/c` sin convertir `a` ni `b`, pidiendo el nombre plano `a:b:c`.
    if (params.size() < 3 || (params.size() - 1) % 2 != 0) {
        r.rc = 2;
        r.err =
            "usage: --mutate-advanced-breakdown <dataset> <dir> <name> "
            "[<dir> <name>...]\n";
        return r;
    }
    const std::string dataset = params[0];
    ExecResult mpRes = getDatasetMountpointCapture(dataset);
    if (mpRes.rc != 0) {
        return mpRes;
    }
    std::string mountpoint = trim(mpRes.out);
#ifndef _WIN32
    // Sin montar —un entorno de arranque con `mountpoint=/`, que no se puede montar en su
    // sitio sin taparle la raíz a la máquina—, se monta en un temporal mientras dura y
    // después se le devuelve el suyo. Ver AltMountGuard. Los hijos HEREDAN su punto de
    // montaje, así que al volver el padre a `/` quedan en `/usr`, `/var`…, que es donde los
    // monta el sistema cuando arranca.
    AltMountGuard montajeTemporal;
    std::string mpOriginal;
    if (mountpoint.empty()) {
        const ExecResult mpProp = getZfsPropertyCapture(dataset, "mountpoint");
        mpOriginal = mpProp.rc == 0 ? trim(mpProp.out) : std::string();
        std::string errMontaje;
        if (!montajeTemporal.engage(dataset, mountpoint, errMontaje)) {
            r.rc = 1;
            r.err = dataset + " is not mounted and could not be mounted on a temporary place: "
                    + trim(errMontaje) + "\n";
            return r;
        }
    }
#endif
    if (mountpoint.empty()) {
        r.rc = 1;
        ponCodigo(r, "mountpoint_none", {});
        return r;
    }
    namespace fs = std::filesystem;

    // Antes se convertía cada directorio de arriba abajo, y cada nivel copiaba TODO lo
    // que tenía debajo: un fichero a tres niveles de profundidad se movía tres veces.
    // Medido sobre 28 GB con 5 niveles: 14 minutos y 3,2 veces el volumen, con el pool
    // albergando a la vez el original y las copias intermedias.
    //
    // Ahora cada byte se mueve UNA vez:
    //   1. cada directorio se copia a un dataset con nombre TEMPORAL y plano, excluyendo
    //      los subdirectorios que también van a convertirse;
    //   2. solo cuando TODAS las copias están verificadas se borran los originales;
    //   3. y se renombran los datasets a su nombre definitivo, de arriba abajo.
    //
    // El nombre temporal plano es la pieza que lo hace posible: crear directamente
    // `<ds>/Disks/Bootables/Tools` obliga a `zfs create -p`, que fabrica los datasets
    // intermedios con puntos de montaje heredados que TAPAN los datos aún sin mover.
    //
    // Y borrar al final, en vez de sobre la marcha, hace la operación recuperable: si
    // algo falla a mitad, los originales siguen intactos.
    struct Pending {
        std::string rel;      // ruta relativa dentro del mountpoint
        std::string name;     // nombre elegido, relativo a `dataset` (puede llevar '/')
        std::string tmpName;  // dataset temporal, hijo directo de `dataset`
        std::string tmpMp;    // punto de montaje temporal
        // Cuando el directorio YA es el punto de montaje de un dataset —porque un
        // Ensamblar anterior lo conservó reasignándolo— no hay nada que copiar: solo
        // hay que devolverle su nombre canónico. En ese caso tmpName es el dataset
        // REAL del usuario, así que ni se borra su origen ni se destruye al limpiar.
        bool alreadyDataset{false};
        // Los permisos del DIRECTORIO original, para devolvérselos al dataset que ocupa su
        // sitio. Sin esto, el punto de montaje nuevo sale con lo que ZFS pone por defecto
        // —0755 y root— y un directorio que era `drwxr-s--x linarese` pasaba a
        // `drwxr-xr-x root`. Es el mismo fallo que tenía Ensamblar, en espejo.
        bool permisosLeidos{false};
        unsigned int modo{0};
        unsigned int uid{0};
        unsigned int gid{0};
    };
    std::vector<Pending> pending;

    // Si algo falla, no dejar datasets temporales sueltos por ahí.
    struct TempCleanup {
        std::vector<Pending>* items;
        bool armed{true};
        ~TempCleanup() {
            if (!armed || !items) {
                return;
            }
            for (const Pending& p : *items) {
                if (p.alreadyDataset) {
                    continue;  // es del usuario, no un temporal nuestro
                }
                (void)runExecCapture("zfs", {"destroy", "-r", p.tmpName});
                std::error_code ec;
                std::filesystem::remove_all(p.tmpMp, ec);
            }
        }
    } cleanup{&pending};

    // Los seleccionados, normalizados, para calcular exclusiones entre ellos.
    std::vector<std::string> rels;
    std::vector<std::string> names;
    std::set<std::string> seenNames;
    for (std::size_t i = 1; i + 1 < params.size(); i += 2) {
        const std::string rel = trim(params[i]);
        const std::string name = trim(params[i + 1]);
        if (rel.empty()) {
            continue;
        }
        if (name.empty()) {
            r.rc = 2;
            ponCodigo(r, "breakdown_empty_name", {rel});
            return r;
        }
        // El nombre lo escribe el usuario, así que se valida aquí y no solo en la
        // interfaz: esto llega por RPC y el daemon no puede fiarse del cliente.
        if (name.front() == '/' || name.back() == '/' || name.find("//") != std::string::npos
            || name.find("..") != std::string::npos) {
            r.rc = 2;
            ponCodigo(r, "invalid_dataset_name", {name});
            return r;
        }
        for (const char c : name) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                            || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'
                            || c == ':' || c == ' ' || c == '/';
            if (!ok) {
                r.rc = 2;
                ponCodigo(r, "name_bad_char", {name});
                return r;
            }
        }
        if (!seenNames.insert(name).second) {
            r.rc = 2;
            ponCodigo(r, "breakdown_duplicate_name", {name});
            return r;
        }
        rels.push_back(rel);
        names.push_back(name);
    }
    if (rels.empty()) {
        r.rc = 2;
        ponCodigo(r, "breakdown_no_dirs", {});
        return r;
    }

    // Datasets ya existentes por su PUNTO DE MONTAJE, no por su nombre. Tras un
    // Ensamblar que conservó los subdatasets, estos quedan reasignados al padre y su
    // nombre deja de reflejar la ruta: buscar por nombre no los encontraría y se
    // copiarían sus datos por segunda vez, encima de un punto de montaje vivo.
    // Ojo con el ámbito: recorrer `-r <dataset>` recorre por NOMBRE, y tras un Ensamblar
    // que conservó los subdatasets estos quedan colgando del padre, así que respecto al
    // dataset que ahora los contiene FÍSICAMENTE son hermanos por nombre y no aparecen.
    // Se recorre el pool entero y se filtra por punto de montaje, que es lo único que
    // sigue diciendo la verdad sobre dónde está cada cosa.
    std::map<std::string, std::string> dsByMountpoint;
    {
        const std::string pool = dataset.substr(0, dataset.find('/'));
        const ExecResult ls =
            runExecCapture("zfs", {"list", "-H", "-o", "name,mountpoint", "-r", pool});
        const std::string mpPrefix = mountpoint + "/";
        for (const std::string& line : splitLines(ls.out)) {
            const std::size_t tab = line.find('\t');
            if (tab == std::string::npos) {
                continue;
            }
            const std::string name = trim(line.substr(0, tab));
            const std::string mp = trim(line.substr(tab + 1));
            if (mp.empty() || mp == "-" || mp == "none" || name == dataset) {
                continue;
            }
            if (mp.rfind(mpPrefix, 0) == 0) {  // solo lo que cae dentro de este mountpoint
                dsByMountpoint[mp] = name;
            }
        }
    }

    // ---- Fase 1: copiar cada directorio a su dataset temporal ----
    for (std::size_t i = 0; i < rels.size(); ++i) {
        const std::string& rel = rels[i];
        const fs::path srcPath = fs::path(mountpoint) / fs::path(rel);
        std::error_code ec;
        if (!fs::is_directory(srcPath, ec)) {
            r.out += "skip_not_a_directory=" + rel + "\n";
            continue;
        }
        const std::string& name = names[i];
        const std::string child = dataset + "/" + name;
        if (runExecCapture("zfs", {"list", "-H", "-o", "name", child}).rc == 0) {
            r.out += "child_exists=" + child + "\n";
            continue;
        }
        // ¿Ya es un dataset, solo que con otro nombre? Entonces no hay nada que copiar:
        // basta devolverle el nombre canónico, y eso se hace en la fase 2, cuando sus
        // ascendientes ya tengan el suyo (`zfs rename` exige que el padre exista).
        {
            const auto itMp = dsByMountpoint.find(srcPath.string());
            if (itMp != dsByMountpoint.end()) {
                pending.push_back(Pending{rel, name, itMp->second, std::string(), true});
                r.out += "already_dataset=" + itMp->second + " -> " + child + "\n";
                reportJobProgress("[BREAKDOWN] " + std::to_string(i + 1) + " of "
                                  + std::to_string(rels.size()) + ": " + rel
                                  + " — already a dataset, only renamed");
                continue;
            }
        }

        // Lo que NO hay que copiar aquí: los seleccionados que cuelgan de este.
        std::vector<std::string> excludes;
        for (const std::string& other : rels) {
            if (other.size() > rel.size() + 1 && other.compare(0, rel.size(), rel) == 0
                && other[rel.size()] == '/') {
                excludes.push_back(other.substr(rel.size() + 1));
            }
        }

        // ANTES de copiar, no solo al terminar: un directorio grande puede tardar
        // minutos, y avisando solo al acabar la caja de progreso se queda muda justo
        // cuando más falta hace. Medido: en un ensamblado de 27 G se emitió UNA línea,
        // al final, porque un elemento se llevó el 99 % del tiempo.
        // Salida limpia si se ha pedido cancelar. Aquí es seguro: la fase 1 solo copia
        // a datasets temporales y no ha tocado un solo original; la limpieza RAII se
        // lleva los temporales al volver.
        if (jobCancelRequested()) {
            r.rc = 125;
            ponCodigo(r, "cancelled_before_changes", {});
            return r;
        }
        reportJobProgress("[BREAKDOWN] copying " + std::to_string(i + 1) + " of "
                          + std::to_string(rels.size()) + ": " + rel);
        // El dataset temporal se crea distinto en cada plataforma, y no por gusto.
        //
        // En Unix se le fija un mountpoint que hemos elegido nosotros. En Windows eso NO
        // se puede: `zfs create -o mountpoint=C:\...` responde «'mountpoint' must be an
        // absolute path, 'none', or 'legacy'», porque allí el montaje va por letra de
        // unidad y la propiedad solo admite rutas de estilo Unix. Así que allí se crea
        // sin decirle dónde y DESPUÉS se le pregunta dónde ha quedado, que es la misma
        // regla que arregló el resto de la ruta de Windows: no dictar la ruta, consultarla.
        std::string tmpMp;
        std::string tmpName;
#ifndef _WIN32
        tmpMp = makeTempDir("zfsmgr-breakdown-child-");
        if (tmpMp.empty()) {
            r.rc = 125;
            ponCodigo(r, "temp_mountpoint_failed", {});
            return r;
        }
        // Nombre plano: hijo DIRECTO del dataset base, así no hay intermedios que crear.
        tmpName = dataset + "/" + fs::path(tmpMp).filename().string();
        ExecResult create = runExecCapture("zfs", {"create", "-o", "mountpoint=" + tmpMp, tmpName});
        if (create.rc != 0) {
            std::error_code rmec;
            fs::remove_all(tmpMp, rmec);
            return create;
        }
        (void)runExecCapture("zfs", {"mount", tmpName});
#else
        {
            GUID g;
            if (CoCreateGuid(&g) != S_OK) {
                r.rc = 125;
                ponCodigo(r, "temp_name_failed", {});
                return r;
            }
            char sfx[64];
            std::snprintf(sfx, sizeof(sfx), "%08lX%04X%04X", static_cast<unsigned long>(g.Data1),
                          g.Data2, g.Data3);
            tmpName = dataset + "/zfsmgr-breakdown-child-" + sfx;
        }
        ExecResult create = runExecCapture("zfs", {"create", tmpName});
        if (create.rc != 0) {
            return create;
        }
        (void)runExecCapture("zfs", {"mount", tmpName});
        const ExecResult where = getDatasetMountpointCapture(tmpName);
        tmpMp = (where.rc == 0) ? trim(where.out) : std::string();
        if (tmpMp.empty()) {
            (void)runExecCapture("zfs", {"destroy", tmpName});
            r.rc = 125;
            ponCodigo(r, "temp_not_mounted", {});
            return r;
        }
#endif
        pending.push_back(Pending{rel, name, tmpName, tmpMp, false});

        int left = -1;
        {
            // El dataset temporal es el que crece mientras se copia.
            CopyProgressSampler sampler(tmpName, "[BREAKDOWN] " + std::to_string(i + 1) + " of "
                                                     + std::to_string(rels.size()) + ": " + rel);
            for (int attempt = 0; attempt < 5; ++attempt) {
                // -x: nunca bajar a un dataset montado dentro. Las exclusiones ya cubren los
                // seleccionados, pero esto protege también de los que no lo estén.
                ExecResult rs = runCopyTreeCapture(srcPath.string(), tmpMp, excludes, true);
                if (rs.rc != 0) {
                    return rs;
                }
                left = countPendingCopyTransfers(srcPath.string(), tmpMp, excludes, true);
                if (left == 0) {
                    break;
                }
            }
        }  // el muestreador se detiene aquí
        if (left != 0) {
            r.rc = 42;
            ponCodigo(r, "verify_failed",
                      {child, left < 0 ? std::string("?") : std::to_string(left)});
            return r;
        }
        // Con la marca [BREAKDOWN]: la interfaz FILTRA las líneas de progreso y solo
        // deja pasar las que la llevan. Sin ella se descartaban en silencio y la caja
        // de Progreso seguía vacía aunque el daemon las estuviera emitiendo.
        reportJobProgress("[BREAKDOWN] copied " + std::to_string(i + 1) + " of "
                          + std::to_string(rels.size()) + ": " + rel);
    }

    // ---- Fase 2: todo verificado; ahora sí se borra y se renombra ----
    // Borrado de los originales primero, de más profundo a menos, para que el punto de
    // montaje definitivo esté libre cuando se renombre.
    // No constante: el bucle de borrado anota en cada uno los permisos del directorio
    // original, que es lo último que se sabe de él antes de que desaparezca.
    std::vector<Pending*> byDepth;
    for (Pending& p : pending) {
        byDepth.push_back(&p);
    }
    const auto depthOf = [](const std::string& v) { return std::count(v.begin(), v.end(), '/'); };
    std::sort(byDepth.begin(), byDepth.end(), [&](const Pending* a, const Pending* b) {
        return depthOf(a->rel) > depthOf(b->rel);
    });
    // Un directorio a convertir puede tener DENTRO datasets montados: es exactamente lo
    // que deja un Ensamblar que conservó los subdatasets. `fs::remove_all` no distingue
    // puntos de montaje —entra en ellos y borra datos vivos—, y esto ocurre incluso
    // cuando el subdataset también está seleccionado, porque al padre le toca borrar
    // ANTES de que al hijo le toque renombrarse. Se desmontan antes de tocar nada.
    std::map<std::string, std::string> nestedByMp;  // punto de montaje -> dataset
    for (const Pending* p : byDepth) {
        if (p->alreadyDataset) {
            continue;
        }
        const std::string prefix = (fs::path(mountpoint) / fs::path(p->rel)).string() + "/";
        for (const auto& kv : dsByMountpoint) {
            if (kv.first.rfind(prefix, 0) == 0) {
                nestedByMp.insert(kv);
            }
        }
    }
    std::vector<std::pair<std::string, std::string>> nested(nestedByMp.begin(), nestedByMp.end());
    std::sort(nested.begin(), nested.end(), [](const auto& a, const auto& b) {
        return a.first.size() > b.first.size();  // de más profundo a menos
    });
    std::vector<std::pair<std::string, std::string>> unmounted;
    for (const auto& kv : nested) {
        ExecResult um = runExecCapture("zfs", {"unmount", kv.second});
        if (um.rc != 0) {
            // Nada se ha borrado todavía: se deshace el desmontaje y se aborta. Los
            // datasets temporales los recoge la limpieza, que sigue armada.
            for (auto it = unmounted.rbegin(); it != unmounted.rend(); ++it) {
                (void)runExecCapture("zfs", {"mount", it->second});
            }
            r.rc = 1;
            ponCodigo(r, "breakdown_cannot_unmount", {kv.second, kv.first, trim(um.err)});
            return r;
        }
        unmounted.push_back(kv);
    }

    for (Pending* p : byDepth) {
        if (p->alreadyDataset) {
            continue;  // su "origen" es el propio dataset montado: borrarlo sería perderlo
        }
        const fs::path srcPath = fs::path(mountpoint) / fs::path(p->rel);
#ifndef _WIN32
        struct stat rootSt{};
        if (::lstat(srcPath.c_str(), &rootSt) != 0) {
            r.rc = 1;
            ponCodigo(r, "cannot_stat", {srcPath.string()});
            return r;
        }
        // La misma lectura sirve para dos cosas: el dispositivo, para no cruzar montajes al
        // borrar, y los permisos, para devolvérselos al dataset que va a ocupar este sitio.
        // Se guarda aquí porque es el último momento en que el directorio original existe.
        p->permisosLeidos = true;
        p->modo = static_cast<unsigned int>(rootSt.st_mode & 07777);
        p->uid = static_cast<unsigned int>(rootSt.st_uid);
        p->gid = static_cast<unsigned int>(rootSt.st_gid);
        std::string rmErr;
        if (!removeTreeWithoutCrossingMounts(srcPath, rootSt.st_dev, rmErr)) {
            r.rc = 1;
            r.err = rmErr + "\n";
            return r;
        }
#else
        std::error_code rmec;
        fs::remove_all(srcPath, rmec);
        if (rmec) {
            r.rc = 1;
            ponCodigo(r, "cannot_remove", {srcPath.string()});
            return r;
        }
#endif
    }

    // Renombrado de menos profundo a más, y ahora por profundidad del NOMBRE, no de la
    // ruta: `zfs rename` exige que exista el padre por nombre, y con nombres planos
    // (`a:b:c`) la profundidad de la ruta ya no dice nada sobre ese orden.
    std::sort(byDepth.begin(), byDepth.end(), [&](const Pending* a, const Pending* b) {
        return depthOf(a->name) < depthOf(b->name);
    });
    for (const Pending* p : byDepth) {
        const std::string child = dataset + "/" + p->name;
        const fs::path srcPath = fs::path(mountpoint) / fs::path(p->rel);
        ExecResult ren = runExecCapture("zfs", {"rename", p->tmpName, child});
        if (ren.rc != 0) {
            // A estas alturas los originales YA están borrados, así que los datasets
            // temporales son la única copia. Destruirlos sería perder los datos: se
            // desarma la limpieza y se dice dónde están.
            cleanup.armed = false;
            std::string lista;
            for (const Pending* q : byDepth) {
                lista += "\n  " + q->tmpName + "  ->  " + dataset + "/" + q->name;
            }
            ponCodigo(ren, "breakdown_rename_failed", {trim(ren.err), dataset, lista});
            return ren;
        }
        // El punto de montaje explícito NO viaja con el renombrado: sigue apuntando al
        // temporal, así que hay que fijarlo al definitivo.
        //
        // En Windows NO se fija ninguno, y no es una omisión: allí la propiedad solo
        // admite rutas de estilo Unix —`zfs set mountpoint=Z:\desglose\fotos` responde
        // «'mountpoint' must be an absolute path, 'none', or 'legacy'»— y el montaje va
        // por letra de unidad. El dataset ya queda montado donde OpenZFS lo pone, que es
        // Z:\<último componente>, porque allí los datasets NO se anidan bajo su padre.
        // Consecuencia visible para el usuario: el contenido queda en Z:\fotos, no en
        // Z:\desglose\fotos. Es el modelo de la plataforma, no algo que podamos elegir.
#ifdef _WIN32
        ExecResult setMp;
        setMp.rc = 0;
#else
        ExecResult setMp;
        if (montajeTemporal.active) {
            // Con el padre en el temporal, el hijo HEREDA: así sigue al padre cuando vuelva a
            // su sitio. Si su nombre no es su ruta (`a:b:c` para `a/b/c`), heredar lo dejaría
            // en otro sitio, y el punto definitivo —el original del padre más la ruta— solo se
            // puede fijar con todo desmontado: fijado ahora, lo montaría en esa ruta de ESTA
            // máquina. Hasta entonces, el del temporal.
            const bool heredaBien =
                p->name == p->rel || mpOriginal.empty() || mpOriginal.front() != '/';
            if (heredaBien) {
                setMp = runExecCapture("zfs", {"inherit", "mountpoint", child});
            } else {
                setMp = runExecCapture("zfs", {"set", "mountpoint=" + srcPath.string(), child});
                montajeTemporal.mountpointsAlFinal.emplace_back(
                    child, (fs::path(mpOriginal) / fs::path(p->rel)).string());
            }
        } else {
            setMp = runExecCapture("zfs", {"set", "mountpoint=" + srcPath.string(), child});
        }
#endif
        if (setMp.rc != 0) {
            // Igual que arriba: el dataset ya tiene su nombre bueno y contiene los
            // únicos datos. Solo le falta el punto de montaje.
            cleanup.armed = false;
            ponCodigo(setMp, "breakdown_mountpoint_pending",
                      {trim(setMp.err), child, srcPath.string()});
            return setMp;
        }
        (void)runExecCapture("zfs", {"mount", child});
#ifndef _WIN32
        // Y los permisos del directorio que estaba aquí antes.
        //
        // No se aborta si falla: los datos ya están en su dataset y perder la operación por
        // esto sería un cambio a peor. Se dice y se sigue.
        if (p->permisosLeidos) {
            if (::chmod(srcPath.c_str(), p->modo) != 0) {
                r.out += "[BREAKDOWN] warning: could not restore the original mode on "
                         + srcPath.string() + "\n";
            }
            if (::chown(srcPath.c_str(), p->uid, p->gid) != 0) {
                r.out += "[BREAKDOWN] warning: could not restore the original owner on "
                         + srcPath.string() + "\n";
            }
        }
#endif
        if (!p->tmpMp.empty()) {  // los que ya eran dataset no tienen temporal que limpiar
            std::error_code tmpEc;
            fs::remove_all(p->tmpMp, tmpEc);
        }
        r.out += "[BREAKDOWN] ok " + p->rel + " -> " + child
                 + (p->alreadyDataset ? " (renamed, not copied)" : "") + "\n";
    }

    // Devolver a su sitio los que se desmontaron para poder borrar. Los seleccionados ya
    // se remontaron solos al renombrarlos, y volver a montarlos aquí, con su nombre
    // viejo, fallaría. De menos profundo a más, que es como se pueden montar.
    std::set<std::string> renamedNames;
    for (const Pending& p : pending) {
        if (p.alreadyDataset) {
            renamedNames.insert(p.tmpName);
        }
    }
    for (auto it = unmounted.rbegin(); it != unmounted.rend(); ++it) {
        if (renamedNames.count(it->second)) {
            continue;
        }
        if (!datasetIsMounted(it->second)) {
            const ExecResult rm = runExecCapture("zfs", {"mount", it->second});
            if (rm.rc != 0 && !datasetIsMounted(it->second)) {
                r.out += "[BREAKDOWN] warning: could not remount " + it->second + "\n";
            }
        }
    }
    cleanup.armed = false;
#ifndef _WIN32
    if (montajeTemporal.active) {
        r.out += "[BREAKDOWN] " + dataset + " was not mounted: it was mounted at " + mountpoint
                 + " for the operation and its mountpoint is restored (" + mpOriginal
                 + "); the new datasets inherit theirs and are left unmounted\n";
    }
#endif
    r.rc = 0;
    return r;
}

ExecResult runMutateAdvancedAssembleCapture(const std::vector<std::string>& params) {
    ExecResult r;
    if (params.size() < 2) {
        r.rc = 2;
        ponCodigo(r, "usage", {"--mutate-advanced-assemble <dataset> <child> [child...]"});
        return r;
    }
    const std::string dataset = params[0];
    ExecResult mpRes = getDatasetMountpointCapture(dataset);
    if (mpRes.rc != 0) {
        return mpRes;
    }
    std::string parentMp = trim(mpRes.out);
#ifndef _WIN32
    // Sin montar, en un temporal mientras dura, como Desglosar (ver AltMountGuard).
    //
    // Pero ANTES se mira que ninguno de los que se van a montar —los hijos pedidos y lo que
    // cuelga de ellos— tenga punto de montaje PROPIO. Los que lo heredan siguen al padre al
    // temporal; uno con el suyo —un `/usr` recibido, por ejemplo— se montaría al ensamblar en
    // ese `/usr`, que es el de esta máquina. Se niega entero, sin haber tocado nada.
    AltMountGuard montajeTemporal;
    if (parentMp.empty()) {
        const ExecResult props =
            runExecCapture("zfs", {"get", "-H", "-r", "-t", "filesystem", "-o", "name,value,source",
                                   "mountpoint", dataset});
        if (props.rc != 0) {
            return props;
        }
        for (const std::string& linea : splitLines(props.out)) {
            std::istringstream iss(linea);
            std::string nombre;
            std::string valor;
            std::string origen;
            std::getline(iss, nombre, '\t');
            std::getline(iss, valor, '\t');
            std::getline(iss, origen);
            // Solo cuenta lo que se va a montar: los hijos pedidos y lo que cuelga de ellos.
            bool seMonta = false;
            for (std::size_t k = 1; k < params.size(); ++k) {
                const std::string hijo = trim(params[k]);
                seMonta =
                    seMonta
                    || (!hijo.empty() && (nombre == hijo || nombre.rfind(hijo + "/", 0) == 0));
            }
            if (!seMonta || origen.rfind("inherited", 0) == 0 || valor == "none"
                || valor == "legacy") {
                continue;
            }
            r.rc = 1;
            r.err = dataset + " is not mounted, so it would be mounted on a temporary place; but "
                    + nombre + " has its own mountpoint (" + valor + ", " + trim(origen)
                    + ") and assembling would mount it there, on this machine. Make it inherit "
                      "(zfs inherit mountpoint " + nombre + ") or mount " + dataset
                    + " in its place first\n";
            return r;
        }
        std::string errMontaje;
        if (!montajeTemporal.engage(dataset, parentMp, errMontaje)) {
            r.rc = 1;
            r.err = dataset + " is not mounted and could not be mounted on a temporary place: "
                    + trim(errMontaje) + "\n";
            return r;
        }
    }
#endif
    if (parentMp.empty()) {
        r.rc = 2;
        ponCodigo(r, "mountpoint_none", {});
        return r;
    }
    namespace fs = std::filesystem;
    // De más profundo a menos, no en el orden en que vengan.
    //
    // Absorber un padre CONSERVA sus subdatasets reasignándolos, así que si va primero,
    // los hijos que también se habían pedido cambian de nombre y el bucle los salta como
    // "ya absorbidos". Pasó de verdad: se pidieron 262 y se procesó 1, dejando los otros
    // 261 renombrados a mmela-bin:XXX y sin absorber.
    //
    // Yendo de hojas a raíz, cada hijo se absorbe en el directorio de su padre mientras
    // el padre todavía es un dataset, y al final el padre sube a su vez. La conservación
    // queda entonces para lo que de verdad no se pidió absorber, que es su cometido.
    std::vector<std::string> orderedChildren;
    for (std::size_t i = 1; i < params.size(); ++i) {
        const std::string c = trim(params[i]);
        if (!c.empty()) {
            orderedChildren.push_back(c);
        }
    }
    std::sort(orderedChildren.begin(), orderedChildren.end(),
              [](const std::string& a, const std::string& b) {
                  const auto depth = [](const std::string& v) {
                      return std::count(v.begin(), v.end(), '/');
                  };
                  if (depth(a) != depth(b)) {
                      return depth(a) > depth(b);
                  }
                  return a < b;
              });
    std::size_t hechos = 0;
    std::size_t saltados = 0;
    for (std::size_t i = 1; i <= orderedChildren.size(); ++i) {
        const std::string child = orderedChildren[i - 1];
        if (child.empty()) {
            continue;
        }
        // Puede que ya no exista: la lista que ofrece la interfaz incluye TODOS los
        // niveles, y ensamblar un hijo destruye recursivamente lo que cuelga de él. Si
        // se seleccionan padre e hijo, al llegar al hijo ya está absorbido. Antes eso
        // abortaba la operación entera con error, aunque el resultado fuera correcto.
        //
        // Pero ese mismo salto se tragaba también «me has nombrado algo que NUNCA fue hijo
        // de este dataset»: la orden devolvía rc=0 sin haber hecho nada. Pasó de verdad
        // llamándola con nombres relativos —`fotos` en vez de `tanque/media/fotos`—, que es
        // lo que uno escribe si se fía del texto de ayuda. Decir «hecho» cuando no se ha
        // hecho nada es el peor fallo posible aquí, así que ahora se cuenta y se dice.
        if (runExecCapture("zfs", {"list", "-H", "-o", "name", child}).rc != 0) {
            r.out += "[ASSEMBLE] already absorbed " + child + "\n";
            ++saltados;
            continue;
        }
        (void)runExecCapture("zfs", {"mount", child});
        ExecResult cmpRes = getDatasetMountpointCapture(child);
        if (cmpRes.rc != 0) {
            return cmpRes;
        }
        const std::string childMp = trim(cmpRes.out);
        if (childMp.empty()) {
            continue;
        }
        // Los permisos y el dueño del PUNTO DE MONTAJE, antes de tocar nada.
        //
        // El directorio con el que acaba esta operación es la escala temporal renombrada, y
        // `makeTempDirIn` la crea con `mkdtemp`, que pone 0700 y el usuario del daemon —o
        // sea root—. Resultado: un dataset que cualquiera podía leer se convertía en un
        // directorio `drwx------ root root`, y quien lo usaba se quedaba fuera de sus
        // propios datos sin que nada lo dijera. Comprobado en vivo: tras ensamblar,
        // `ls` sobre el directorio daba «Permiso denegado».
        //
        // Se lee AQUÍ y no después porque después ya no existe: el punto de montaje
        // desaparece con el `zfs destroy`.
        bool permisosLeidos = false;
#ifndef _WIN32
        struct stat permisosOrigen{};
        permisosLeidos = (::stat(childMp.c_str(), &permisosOrigen) == 0);
        if (!permisosLeidos) {
            // No es motivo para abortar —los datos son lo importante— pero sí para decirlo:
            // el directorio quedará con los del temporal y alguien tendrá que arreglarlo.
            r.out += "[ASSEMBLE] warning: could not read the permissions of " + childMp
                     + "; the directory will keep those of the temporary one\n";
        }
#endif
        const std::size_t pos = child.find_last_of('/');
        const std::string bn = (pos == std::string::npos) ? child : child.substr(pos + 1);
        if (bn.empty()) {
            continue;
        }
        // El contenido tiene que volver a DONDE ESTABA MONTADO. Se comprueba aquí,
        // antes de copiar y destruir nada: si el dataset está montado fuera del padre,
        // ensamblarlo escribiría fuera de su sitio.
        //
        // En Windows esa comprobación NO puede aplicarse: allí los datasets no se anidan
        // bajo su padre, se montan planos en la raíz de la unidad —winpool/ens/fotos
        // acaba en Z:\fotos, hermano de Z:\ens— así que «montado fuera del padre» es el
        // estado normal y la comprobación no pasaría jamás. Lo que sí se exige es que sea
        // descendiente por NOMBRE, que es lo que garantiza que el destino calculado
        // pertenezca al padre.
#ifdef _WIN32
        if (child.rfind(dataset + "/", 0) != 0) {
            r.rc = 1;
            ponCodigo(r, "assemble_not_child", {child, dataset});
            return r;
        }
#else
        if (childMp.rfind(parentMp + "/", 0) != 0) {
            r.rc = 1;
            ponCodigo(r, "assemble_outside", {child, childMp, parentMp});
            return r;
        }
#endif
        // En el dataset PADRE, que es donde los datos tienen que acabar. Antes iba al
        // temporal del sistema: en Linux suele ser tmpfs, así que 27 GB de datos se
        // volcaban en RAM y la operación moría con "No space left on device".
        if (jobCancelRequested()) {
            r.rc = 125;
            ponCodigo(r, "cancelled_by_user", {});
            return r;
        }
        reportJobProgress("[ASSEMBLE] assembling " + std::to_string(i) + " of "
                          + std::to_string(orderedChildren.size()) + ": " + bn);
        const std::string tmp = makeTempDirIn(parentMp, ".zfsmgr-assemble-");
        if (tmp.empty()) {
            r.rc = 125;
            ponCodigo(r, "temp_dir_failed", {parentMp});
            return r;
        }
        // La escala se borra pase lo que pase. Solo se limpiaba en el camino bueno, así
        // que cada Ensamblar fallido dejaba un `.zfsmgr-assemble-XXXX` dentro del pool.
        // No es solo basura: un Hacia Dir posterior los copia fielmente al directorio
        // resultante, y aparecen como ficheros de más que no estaban en el origen.
        struct StagingCleanup {
            std::string path;
            bool armed{true};
            ~StagingCleanup() {
                if (armed && !path.empty()) {
                    std::error_code ec;
                    std::filesystem::remove_all(path, ec);
                }
            }
        } stagingCleanup{tmp};
        // Descendientes DIRECTOS que son datasets: se conservan en vez de destruirlos.
        //
        // Antes se hacía `zfs destroy -r`, que se llevaba todo el subárbol: ensamblar
        // un dataset borraba de paso los que colgaban de él. Los datos no se perdían
        // —se copiaban a la escala— pero la estructura sí, y eso obligaba además a
        // ensamblar el subárbol entero desde la interfaz.
        //
        // ZFS no exige que el nombre del dataset refleje su ruta: un dataset puede
        // seguir montado donde está aunque su directorio contenedor pase a ser un
        // directorio normal del padre. Se reasignan al padre conservando su punto de
        // montaje, y así sobreviven.
        std::vector<std::string> directChildren;
        {
            const ExecResult ls = runExecCapture("zfs", {"list", "-H", "-o", "name", "-r", child});
            const std::string prefix = child + "/";
            for (const std::string& line : splitLines(ls.out)) {
                const std::string ds = trim(line);
                if (ds.size() <= prefix.size() || ds.compare(0, prefix.size(), prefix) != 0) {
                    continue;
                }
                // Solo los directos: renombrar uno arrastra su propio subárbol.
                if (ds.find('/', prefix.size()) == std::string::npos) {
                    directChildren.push_back(ds);
                }
            }
        }

        // Desmontar TODO el subárbol antes de reasignar, de más profundo a menos.
        //
        // Se desmontaban solo los hijos DIRECTOS, y uno que tenga a su vez algo montado
        // dentro no se puede desmontar: `zfs rename` lo intenta y falla con
        // «cannot unmount ...: pool or dataset is busy». Pasó con Squirrel.app, que
        // tenía Contents montado debajo, y abortó el Ensamblar entero.
        // Si algo falla a partir de aquí hay que DEVOLVERLOS a su sitio. Sin esto, un
        // Ensamblar abortado dejaba cientos de datasets desmontados —227 en una prueba
        // real— y desde fuera eso se parece demasiado a haber perdido los datos.
        struct RemountGuard {
            std::vector<std::string> unmounted;  // de más profundo a menos
            bool armed{true};
            ~RemountGuard() {
                if (!armed) {
                    return;
                }
                for (auto it = unmounted.rbegin(); it != unmounted.rend(); ++it) {
                    (void)runExecCapture("zfs", {"mount", *it});
                }
            }
        } remountGuard;
        {
            std::vector<std::string> subtree;
            const ExecResult ls = runExecCapture("zfs", {"list", "-H", "-o", "name", "-r", child});
            const std::string prefix = child + "/";
            for (const std::string& line : splitLines(ls.out)) {
                const std::string ds = trim(line);
                if (ds.size() > prefix.size() && ds.compare(0, prefix.size(), prefix) == 0) {
                    subtree.push_back(ds);
                }
            }
            std::sort(subtree.begin(), subtree.end(),
                      [](const std::string& a, const std::string& b) {
                          return std::count(a.begin(), a.end(), '/')
                                 > std::count(b.begin(), b.end(), '/');
                      });
            for (const std::string& ds : subtree) {
                if (datasetIsMounted(ds)) {
                    (void)runExecCapture("zfs", {"unmount", ds});
                    remountGuard.unmounted.push_back(ds);
                }
            }
        }

        std::unique_ptr<CopyProgressSampler> sampler(new CopyProgressSampler(
            dataset, "[ASSEMBLE] " + std::to_string(i) + " of "
                         + std::to_string(orderedChildren.size()) + ": " + bn));
        // -x: copiar SOLO lo propio del dataset. Lo que hay bajo los puntos de montaje
        // de sus hijos pertenece a esos hijos y se queda con ellos.
        ExecResult rsA = runCopyTreeCapture(childMp, tmp, {}, /*oneFileSystem=*/true);
        if (rsA.rc != 0) {
            return rsA;
        }
        // Verificar ANTES de destruir. Esto NO es reponer nada: el shell antiguo
        // tampoco comprobaba, destruía en cuanto rsync devolvía 0. Aquí el riesgo es
        // mayor que en el desglose, porque "zfs destroy -r" no admite vuelta atrás:
        // si la copia quedó incompleta, los datos no están en ningún sitio.
        {
            int pending = countPendingCopyTransfers(childMp, tmp, {}, true);
            for (int attempt = 1; pending != 0 && attempt < 5; ++attempt) {
                ExecResult again = runCopyTreeCapture(childMp, tmp, {}, true);
                if (again.rc != 0) {
                    return again;
                }
                pending = countPendingCopyTransfers(childMp, tmp, {}, true);
            }
            if (pending != 0) {
                r.rc = 42;
                ponCodigo(r, "assemble_verify_failed",
                          {child, pending < 0 ? std::string("?") : std::to_string(pending)});
                return r;
            }
        }
        sampler.reset();  // deja de muestrear antes de destruir

        // Reasignar los hijos ANTES de destruir, y desmontarlos antes de mover nada:
        // renombrar un directorio que contiene un punto de montaje da EBUSY, y el
        // directorio del hijo cuelga justo de lo que vamos a sustituir.
        struct Reparented {
            std::string newName;
            std::string mountpoint;
        };
        std::vector<Reparented> reparented;
        for (const std::string& kid : directChildren) {
            const std::string kidBase = kid.substr(kid.find_last_of('/') + 1);
            // La PROPIEDAD, no el punto de montaje efectivo: getDatasetMountpointCapture
            // devuelve vacío cuando el dataset no está montado, y aquí llegan todos
            // desmontados a propósito —hay que desmontar el subárbol antes de poder
            // renombrar nada—. Lo que hace falta es dónde DEBE quedar, no dónde está.
            ExecResult kmp = getZfsPropertyCapture(kid, "mountpoint");
            std::string kidMp = (kmp.rc == 0) ? trim(kmp.out) : std::string();
            if (kidMp == "-" || kidMp == "none" || kidMp == "legacy") {
                kidMp.clear();
            }
            if (kidMp.empty()) {
                r.rc = 1;
                ponCodigo(r, "cannot_find_mountpoint", {kid});
                return r;
            }
            // Explícito antes de renombrar: si fuera heredado, al cambiar de padre se
            // movería solo y el dataset dejaría de estar donde el usuario lo ve.
            ExecResult fix = runExecCapture("zfs", {"set", "mountpoint=" + kidMp, kid});
            if (fix.rc != 0) {
                return fix;
            }
            (void)runExecCapture("zfs", {"unmount", kid});
            // El nombre codifica DÓNDE queda montado: la ruta bajo el padre con ':' en
            // lugar de '/', porque ZFS no admite '/' dentro de un componente. Antes se
            // usaba solo el último tramo con sufijo -2 al chocar, y eso perdía la ruta:
            // dos hijos homónimos en carpetas distintas quedaban como `X` y `X-2`, sin
            // nada que dijera de dónde venía cada uno. Con la ruta codificada el viaje
            // de ida y vuelta no pierde información y Desglosar puede reconstruirla.
            // (ZFS solo admite [a-zA-Z0-9_.:- ] y espacio: ':' es de los pocos legales
            // que casi no aparece en nombres de directorio.)
            std::string encoded = kidBase;
            if (kidMp.rfind(parentMp + "/", 0) == 0) {
                encoded = kidMp.substr(parentMp.size() + 1);
                std::replace(encoded.begin(), encoded.end(), '/', ':');
            }
            std::string target = dataset + "/" + encoded;
            for (int n = 2; runExecCapture("zfs", {"list", "-H", "-o", "name", target}).rc == 0;
                 ++n) {
                target = dataset + "/" + encoded + "-" + std::to_string(n);
            }
            ExecResult mv = runExecCapture("zfs", {"rename", kid, target});
            if (mv.rc != 0) {
                (void)runExecCapture("zfs", {"mount", kid});
                return mv;
            }
            reparented.push_back(Reparented{target, kidMp});
            r.out += "[ASSEMBLE] kept " + kid + " -> " + target + " (mounted at " + kidMp + ")\n";
        }

        // Sin -r: a estas alturas no le queda ningún descendiente, y un destroy no
        // recursivo NO PUEDE llevarse nada por delante aunque el razonamiento anterior
        // fuera erróneo. Si algo quedó colgando, falla en vez de destruirlo.
        ExecResult destroy = runExecCapture("zfs", {"destroy", child});
        if (destroy.rc != 0) {
            return destroy;
        }
        // El destino es el punto de montaje, NO una ruta derivada del nombre. Mientras
        // el nombre era canónico las dos coincidían y nadie lo notaba; desde que puede
        // codificar la ruta con ':' (`Squirrel.app:Contents`), derivarlo del nombre
        // creaba un directorio con dos puntos colgando del padre —visto en el pool de
        // pruebas— en vez de devolver el contenido a su sitio.
        // En Windows el destino NO puede ser el punto de montaje del hijo.
        //
        // Z:\fotos pertenece a winpool, no a winpool/ens: escribir ahí tras destruir el
        // hijo dejaría los datos en el dataset EQUIVOCADO, y encima con aspecto de haber
        // funcionado. Se construye bajo el padre, que es donde tienen que acabar.
#ifdef _WIN32
        const fs::path dst = fs::path(parentMp) / fs::path(child.substr(dataset.size() + 1));
#else
        const fs::path dst = fs::path(childMp);
#endif
        // Renombrado, no segunda copia: la escala está en el mismo sistema de ficheros
        // que el destino, así que esto es instantáneo. Antes se copiaba TODO otra vez
        // —los datos viajaban dos veces por cada hijo ensamblado—.
        std::error_code rmDst;
        fs::remove(dst, rmDst);  // el punto de montaje vacío que dejó el destroy
        std::error_code rnec;
        fs::rename(tmp, dst, rnec);
        if (rnec) {
            // Red de seguridad por si escala y destino acabaran en sistemas distintos.
            std::error_code mkec;
            fs::create_directories(dst, mkec);
            if (mkec) {
                r.rc = 1;
                ponCodigo(r, "cannot_create_dirs", {});
                return r;
            }
            ExecResult rsB = runCopyTreeCapture(tmp, dst.string());
            if (rsB.rc != 0) {
                return rsB;
            }
            std::error_code rmec;
            fs::remove_all(tmp, rmec);
        }

        // Y se devuelven los permisos del punto de montaje original.
        //
        // Va después de los DOS caminos —el renombrado rápido y el respaldo por rsync—
        // porque los dos acaban con un directorio creado por nosotros: el primero hereda los
        // 0700 del `mkdtemp`, el segundo los de `create_directories`. None_ de los dos son
        // los que tenía el dataset.
        //
        // Si algo falla aquí NO se aborta: los datos ya están en su sitio y bien, y perder
        // la operación entera por unos permisos sería un cambio a peor. Se dice y se sigue.
#ifndef _WIN32
        if (permisosLeidos) {
            if (::chmod(dst.c_str(), permisosOrigen.st_mode & 07777) != 0) {
                r.out += "[ASSEMBLE] warning: could not restore the original mode on "
                         + dst.string() + "\n";
            }
            if (::chown(dst.c_str(), permisosOrigen.st_uid, permisosOrigen.st_gid) != 0) {
                r.out += "[ASSEMBLE] warning: could not restore the original owner on "
                         + dst.string() + "\n";
            }
        }
#endif
        // Con el directorio ya en su sitio, devolver los hijos a su punto de montaje.
        // zfs crea el directorio si falta.
        // Se remonta el subárbol COMPLETO, no solo el hijo directo: antes de reasignar se
        // desmontó entero, y sus descendientes viajaron con él al renombrarlo. De menos
        // profundo a más, que es el único orden en que se pueden montar.
        for (const Reparented& rp : reparented) {
            std::vector<std::string> toMount{rp.newName};
            const ExecResult ls =
                runExecCapture("zfs", {"list", "-H", "-o", "name", "-r", rp.newName});
            const std::string prefix = rp.newName + "/";
            for (const std::string& line : splitLines(ls.out)) {
                const std::string ds = trim(line);
                if (ds.size() > prefix.size() && ds.compare(0, prefix.size(), prefix) == 0) {
                    toMount.push_back(ds);
                }
            }
            std::sort(toMount.begin(), toMount.end(),
                      [](const std::string& a, const std::string& b) {
                          return std::count(a.begin(), a.end(), '/')
                                 < std::count(b.begin(), b.end(), '/');
                      });
            for (const std::string& ds : toMount) {
                if (datasetIsMounted(ds)) {
                    continue;
                }
                const ExecResult mnt = runExecCapture("zfs", {"mount", ds});
                if (mnt.rc != 0 && !datasetIsMounted(ds)) {
                    r.out += "[ASSEMBLE] warning: could not mount " + ds + "\n";
                }
            }
        }
        // Ya están montados con su nombre nuevo: el guard no debe tocarlos.
        remountGuard.armed = false;
        stagingCleanup.armed = false;  // la escala ya se movió a su sitio definitivo
        reportJobProgress("[ASSEMBLE] assembled " + std::to_string(i) + " of "
                          + std::to_string(orderedChildren.size()) + ": " + bn);
        r.out += "[ASSEMBLE] ok " + child + " -> " + dst.string() + "\n";
        ++hechos;
    }
    // Ni uno solo: lo que se ha pedido no era hijo de este dataset. Devolver 0 aquí decía
    // «hecho» sobre una operación que no tocó nada.
    if (hechos == 0) {
        r.rc = 1;
        ponCodigo(r, "assemble_no_children", {});
        return r;
    }
    if (saltados > 0) {
        r.out += "[ASSEMBLE] " + std::to_string(saltados) + " were not there\n";
    }
    r.rc = 0;
    return r;
}

std::string stripQuotes(const std::string& s) {
    std::string v = trim(s);
    if (v.size() >= 2) {
        const char a = v.front();
        const char b = v.back();
        if ((a == '\'' && b == '\'') || (a == '"' && b == '"')) {
            v = v.substr(1, v.size() - 2);
        }
    }
    return trim(v);
}

AgentRuntimeConfig loadRuntimeConfig() {
    AgentRuntimeConfig cfg;
    const char* envPath = std::getenv("ZFSMGR_AGENT_CONFIG");
    const char* configPath = (envPath && *envPath) ? envPath : kDefaultAgentConfigPath;
    std::ifstream f(configPath);
    if (!f.is_open()) {
        return cfg;
    }
    std::string line;
    while (std::getline(f, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') {
            continue;
        }
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = trim(t.substr(0, eq));
        const std::string value = stripQuotes(t.substr(eq + 1));
        if (key == "AGENT_BIND" && !value.empty()) {
            cfg.bind = value;
        } else if (key == "AGENT_PORT" && !value.empty()) {
            try {
                const int parsed = std::stoi(value);
                if (parsed > 0 && parsed <= 65535) {
                    cfg.port = parsed;
                }
            } catch (...) {
            }
        } else if (key == "TLS_CERT" && !value.empty()) {
            cfg.tlsCert = value;
        } else if (key == "TLS_KEY" && !value.empty()) {
            cfg.tlsKey = value;
        } else if (key == "TLS_CLIENT_CERT" && !value.empty()) {
            cfg.tlsClientCert = value;
        } else if (key == "AGENT_PATH" && !value.empty()) {
            cfg.commandPath = value;
        } else if (key == "CACHE_TTL_FAST_MS" && !value.empty()) {
            try {
                const int parsed = std::stoi(value);
                if (parsed >= 0 && parsed <= 600000) {
                    cfg.cacheTtlFastMs = parsed;
                }
            } catch (...) {
            }
        } else if (key == "CACHE_MAX_ENTRIES" && !value.empty()) {
            try {
                const int parsed = std::stoi(value);
                if (parsed >= 0 && parsed <= 100000) {
                    cfg.cacheMaxEntries = parsed;
                }
            } catch (...) {
            }
        } else if (key == "TRANSFER_BIND_ADDR" && !value.empty()) {
            cfg.transferBindAddr = value;
        }
    }
    return cfg;
}

void applyRuntimeEnvironment(const AgentRuntimeConfig& cfg) {
    if (cfg.commandPath.empty()) {
        return;
    }
#ifdef _WIN32
    _putenv_s("PATH", cfg.commandPath.c_str());
#else
    setenv("PATH", cfg.commandPath.c_str(), 1);
#endif
}

std::string readFirstLineFile(const char* path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return {};
    }
    std::string line;
    std::getline(f, line);
    return trim(line);
}

// Lee el log del daemon desde un desplazamiento, o su COLA si se pide un máximo.
//
// Por qué existe: el cliente pide el log desde el desplazamiento 0 en cada arranque, y un
// daemon que lleva días en marcha tiene un latido por minuto. Medido en una VM Windows:
// 27.413 líneas de "HBEAT agent alive" transferidas y procesadas para mostrar las últimas.
//
// Se emite primero `__ZFSMGR_LOG_OFFSET__:<n>` con el desplazamiento ABSOLUTO donde
// termina lo devuelto. Sin eso el cliente no puede continuar: calculaba el siguiente
// desplazamiento sumando el tamaño de lo recibido, y al saltar a la cola esa cuenta
// dejaría de corresponder con el fichero. Un cliente viejo ve una línea que no reconoce.
std::string readDaemonLogRange(long long offset, long long maxBytes) {
    std::ifstream lf(kDaemonLogFile, std::ios::binary);
    if (!lf.is_open()) {
        return std::string();
    }
    lf.seekg(0, std::ios::end);
    const long long size = static_cast<long long>(lf.tellg());
    long long start = (offset > 0 && offset <= size) ? offset : 0;
    if (maxBytes > 0 && (size - start) > maxBytes) {
        start = size - maxBytes;
        // Alinear al siguiente salto de línea para no entregar media línea.
        lf.clear();
        lf.seekg(start);
        std::string partial;
        if (std::getline(lf, partial)) {
            const long long afterLine = static_cast<long long>(lf.tellg());
            if (afterLine >= 0 && afterLine <= size) {
                start = afterLine;
            }
        }
    }
    lf.clear();
    lf.seekg(start);
    std::ostringstream oss;
    oss << lf.rdbuf();
    std::ostringstream head;
    head << "__ZFSMGR_LOG_OFFSET__:" << size << "\n";
    return head.str() + oss.str();
}

std::string utcNowIsoString() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        return {};
    }
    return buf;
}

std::string compactSpaces(std::string s) {
    std::string out;
    out.reserve(s.size());
    bool prevSpace = false;
    for (unsigned char c : s) {
        if (c == '\r' || c == '\n' || std::isspace(c)) {
            if (!prevSpace) {
                out.push_back(' ');
                prevSpace = true;
            }
        } else {
            out.push_back(static_cast<char>(c));
            prevSpace = false;
        }
    }
    return trim(out);
}

int decodeWaitStatus(int status) {
    return zfsmgr::base::decodeWaitStatus(status);
}

// La ejecución de procesos vive en src/base/process.cpp: la usan el agente Y el
// cliente, y tenerla en un solo sitio evita que una corrección en uno deje al otro
// tratando mal exactamente los mismos argumentos.

bool startsWith(const std::string& s, const std::string& pref) {
    return s.size() >= pref.size() && s.compare(0, pref.size(), pref) == 0;
}

void onSignal(int) {
    g_stop.store(true);
}

void writeHeartbeat() {
    std::ofstream f(kHeartbeatPath, std::ios::app);
    if (!f.is_open()) {
        return;
    }
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[64];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        return;
    }
    f << buf << " agent alive\n";
    daemonLog("HBEAT", "agent alive");
}

void rotateDaemonLog() {
    std::ifstream check(kDaemonLogFile, std::ios::ate | std::ios::binary);
    if (!check.is_open())
        return;
    const auto sz = check.tellg();
    check.close();
    if (sz < static_cast<std::streamoff>(kDaemonLogMaxBytes))
        return;
    for (int i = 4; i >= 1; --i) {
        const std::string from = std::string(kDaemonLogFile) + "." + std::to_string(i);
        const std::string to = std::string(kDaemonLogFile) + "." + std::to_string(i + 1);
        std::rename(from.c_str(), to.c_str());
    }
    std::rename(kDaemonLogFile, (std::string(kDaemonLogFile) + ".1").c_str());
}

void daemonLog(const std::string& level, const std::string& msg) {
    const std::string ts = utcNowIsoString();
    if (ts.empty())
        return;
    std::lock_guard<std::mutex> lock(g_daemonLogMutex);
    rotateDaemonLog();
    std::ofstream f(kDaemonLogFile, std::ios::app);
    if (!f.is_open()) {
        // El directorio puede no existir todavía en una instalación recién hecha.
        // Antes se retornaba sin más y el log quedaba mudo para siempre.
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(kDaemonLogFile).parent_path(),
                                            ec);
        f.open(kDaemonLogFile, std::ios::app);
        if (!f.is_open()) {
            return;
        }
    }
    f << ts << " " << level << " " << msg << "\n";
}

std::string detectOsLine() {
#ifdef _WIN32
    // Sin esto la línea de sistema salía siendo el mensaje de error de cmd al no
    // encontrar "uname", que es lo que la ficha de conexión mostraba tal cual. Se lee
    // del registro, que es de donde la propia GUI ya lo sacaba por su cuenta.
    {
        char buf[256];
        DWORD sz = sizeof(buf);
        std::string product;
        std::string display;
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                         "ProductName", RRF_RT_REG_SZ, nullptr, buf, &sz)
            == ERROR_SUCCESS) {
            product = buf;
        }
        sz = sizeof(buf);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                         "DisplayVersion", RRF_RT_REG_SZ, nullptr, buf, &sz)
            != ERROR_SUCCESS) {
            sz = sizeof(buf);
            if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                             "CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, buf, &sz)
                == ERROR_SUCCESS) {
                display = std::string("build ") + buf;
            }
        } else {
            display = buf;
        }
        if (product.empty()) {
            return "Windows";
        }
        return display.empty() ? product : product + " " + display;
    }
#else
    ExecResult unames = runExecCapture("uname", {"-s"});
    const std::string unameS = trim(unames.out);
    if (unameS == "Linux") {
        // El parseo vive en la capa base porque la interfaz de Qt necesita el MISMO sobre el
        // fichero que se trae del otro extremo. Allí lo hacía un guion de shell que no
        // quitaba las comillas ni cuidaba el caso sin `VERSION_ID`.
        std::ifstream f("/etc/os-release");
        if (f.is_open()) {
            std::ostringstream ss;
            ss << f.rdbuf();
            const std::string combined = zfsmgr::base::osinfo::fromOsRelease(ss.str());
            if (!combined.empty()) {
                return combined;
            }
        }
        return "Linux";
    }
    if (unameS == "Darwin") {
        const std::string pn = trim(runExecCapture("sw_vers", {"-productName"}).out);
        const std::string pv = trim(runExecCapture("sw_vers", {"-productVersion"}).out);
        const std::string combined = trim(pn + " " + pv);
        return combined.empty() ? "macOS" : combined;
    }
    if (unameS == "FreeBSD") {
        std::string v = trim(runExecCapture("freebsd-version", {"-k"}).out);
        if (v.empty()) {
            v = trim(runExecCapture("freebsd-version", {}).out);
        }
        if (v.empty()) {
            v = trim(runExecCapture("uname", {"-r"}).out);
        }
        if (!v.empty()) {
            return "FreeBSD " + v;
        }
        return "FreeBSD";
    }
    const std::string ua = trim(runExecCapture("uname", {"-a"}).out);
    return ua.empty() ? unameS : ua;
#endif
}

std::string detectMachineUuid() {
#ifdef _WIN32
    // MachineGuid del registro: es el mismo identificador que la GUI ya leía por su
    // cuenta para deduplicar conexiones, así que ambos lados coinciden.
    {
        char buf[128];
        DWORD sz = sizeof(buf);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid",
                         RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf, &sz)
            == ERROR_SUCCESS) {
            return trim(std::string(buf));
        }
        return std::string();
    }
#else
    std::string id = readFirstLineFile("/etc/machine-id");
    if (!id.empty()) {
        return id;
    }
    id = readFirstLineFile("/var/lib/dbus/machine-id");
    if (!id.empty()) {
        return id;
    }
    const ExecResult io = runExecCapture("ioreg", {"-rd1", "-c", "IOPlatformExpertDevice"});
    for (const std::string& line : splitLines(io.out + "\n" + io.err)) {
        const auto pos = line.find("IOPlatformUUID");
        if (pos == std::string::npos) {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq + 1 >= line.size()) {
            continue;
        }
        std::string val = trim(line.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }
        val = trim(val);
        if (!val.empty()) {
            return val;
        }
    }
    return {};
#endif
}

std::string detectZfsVersionRaw() {
    const std::vector<std::pair<std::string, std::vector<std::string>>> tries = {
        {"zfs", {"version"}},
        {"zfs", {"--version"}},
        {"zpool", {"--version"}},
    };
    for (const auto& t : tries) {
        const ExecResult e = runExecCapture(t.first, t.second);
        const std::string merged = compactSpaces(e.out + "\n" + e.err);
        if (e.rc == 0 && !merged.empty()) {
            return merged;
        }
    }
    return {};
}

// Por qué no hay versión de ZFS, cuando la hay que dar.
//
// `detectZfsVersionRaw()` devuelve vacío si el mandato falla y TIRA lo que dijo. Y lo que
// dice distingue dos cosas que no se parecen en nada:
//
//   - «zfs: command not found»            -> ZFS no está instalado.
//   - «The ZFS modules are not loaded.»   -> está instalado y el módulo no está cargado.
//
// Visto en vivo el 2026-08-25 en «mbp», con un disco ZFS conectado y ningún pool a la
// vista: la interfaz decía «No se detecta OpenZFS. Instálelo…» sobre una máquina que lo
// tenía instalado. Guardar el motivo cuesta una línea y ahorra la búsqueda entera.
std::string detectZfsUnavailableReason() {
    const ExecResult e = runExecCapture("zpool", {"version"});
    if (e.rc == 0) {
        return {};
    }
    return compactSpaces(e.out + "\n" + e.err);
}

bool isAllowedMutationOp(const std::string& tool, const std::string& opRaw) {
    static const std::set<std::string> zfsAllowed = {
        "create",     "destroy",    "rollback", "clone", "rename",  "set",
        "inherit",    "mount",      "unmount",  "hold",  "release", "load-key",
        "unload-key", "change-key", "promote",  "allow", "unallow",
    };
    static const std::set<std::string> zpoolAllowed = {
        "create",     "destroy", "add",     "remove", "attach", "detach",     "replace",
        "offline",    "online",  "clear",   "export", "import", "scrub",      "trim",
        "initialize", "sync",    "upgrade", "reguid", "split",  "checkpoint",
    };
    const std::string op = toLower(trim(opRaw));
    if (tool == "zfs") {
        return zfsAllowed.count(op) > 0;
    }
    if (tool == "zpool") {
        return zpoolAllowed.count(op) > 0;
    }
    return false;
}

int fromBase64(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

bool decodeBase64(const std::string& in, std::string& out) {
    out.clear();
    int val = 0;
    int valb = -8;
    for (unsigned char c : in) {
        if (std::isspace(c)) {
            continue;
        }
        if (c == '=') {
            break;
        }
        const int d = fromBase64(c);
        if (d < 0) {
            return false;
        }
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return true;
}

bool parseJsonStringArray(const std::string& json, std::vector<std::string>& out) {
    out.clear();
    std::size_t i = 0;
    auto skipWs = [&]() {
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) {
            ++i;
        }
    };
    skipWs();
    if (i >= json.size() || json[i] != '[') {
        return false;
    }
    ++i;
    skipWs();
    if (i < json.size() && json[i] == ']') {
        ++i;
        skipWs();
        return i == json.size();
    }

    while (i < json.size()) {
        skipWs();
        if (i >= json.size() || json[i] != '"') {
            return false;
        }
        ++i;
        std::string s;
        while (i < json.size()) {
            const char c = json[i++];
            if (c == '"') {
                break;
            }
            if (c == '\\') {
                if (i >= json.size()) {
                    return false;
                }
                const char e = json[i++];
                switch (e) {
                    case '"':
                        s.push_back('"');
                        break;
                    case '\\':
                        s.push_back('\\');
                        break;
                    case '/':
                        s.push_back('/');
                        break;
                    case 'b':
                        s.push_back('\b');
                        break;
                    case 'f':
                        s.push_back('\f');
                        break;
                    case 'n':
                        s.push_back('\n');
                        break;
                    case 'r':
                        s.push_back('\r');
                        break;
                    case 't':
                        s.push_back('\t');
                        break;
                    case 'u':
                        if (i + 4 > json.size()) {
                            return false;
                        }
                        i += 4;
                        s.push_back('?');
                        break;
                    default:
                        return false;
                }
            } else {
                s.push_back(c);
            }
        }
        out.push_back(s);
        skipWs();
        if (i >= json.size()) {
            return false;
        }
        if (json[i] == ',') {
            ++i;
            continue;
        }
        if (json[i] == ']') {
            ++i;
            skipWs();
            return i == json.size();
        }
        return false;
    }
    return false;
}

// ¿Hay algún motivo para NO intentar cambiar la frase de este dataset?
//
// `zfs change-key` sin banderas NO pregunta si `keylocation` apunta a un fichero o a una
// URL: RELEE esa ubicación y deja la clave como estaba. Y sale con **rc=0**, así que el
// cliente decía «frase cambiada» y la frase vieja seguía abriendo el dataset. Comprobado en
// vivo: tras «cambiarla», la vieja cargaba la clave y la nueva daba «Incorrect key
// provided».
//
// Afectaba por igual a la interfaz y al intérprete, porque los dos llaman aquí con las
// banderas vacías.
//
// No se fuerza `-o keylocation=prompt` a espaldas de nadie: un dataset que toma su clave de
// un fichero está así a propósito —se abre solo al arrancar— y cambiárselo de tapadillo
// rompería ese arranque. Se dice lo que pasa y quien manda decide.
//
// **Vive en una función porque el verbo se despacha en DOS sitios** —la rama de RPC y la de
// línea de órdenes—, y la primera versión de esta comprobación se puso solo en una: por RPC
// avisaba y por CLI seguía mintiendo. Esa asimetría es una trampa conocida de este fichero.
std::string motivoParaNoCambiarClave(const std::string& dataset, const std::string& flagsStr) {
    if (flagsStr.find("keylocation") != std::string::npos) {
        return {};  // quien llama ya ha dicho de dónde saldrá la clave
    }
    const ExecResult kl =
        runExecCapture("zfs", {"get", "-H", "-o", "value", "keylocation", dataset});
    const std::string ubicacion = trim(kl.out);
    if (kl.rc != 0 || ubicacion.empty() || ubicacion == "prompt" || ubicacion == "-") {
        return {};
    }
    return "this dataset takes its key from «" + ubicacion
           + "», not from a typed passphrase: changing the passphrase here would have no effect.\n"
             "Change the contents of that location first, or pass «-o keylocation=prompt» if "
             "you want the key to be prompted for from now on.\n";
}

ExecResult runGenericMutationCapture(const std::string& tool, const std::string& payloadB64) {
    ExecResult r;
    std::string decoded;
    if (!decodeBase64(payloadB64, decoded)) {
        r.rc = 2;
        ponCodigo(r, "invalid_payload", {});
        return r;
    }
    std::vector<std::string> arr;
    if (!parseJsonStringArray(decoded, arr) || arr.empty()) {
        r.rc = 2;
        ponCodigo(r, "invalid_payload", {});
        return r;
    }
    if (!isAllowedMutationOp(tool, arr.front())) {
        r.rc = 2;
        r.err = "unsupported " + tool + " mutation op\n";
        return r;
    }
    return runExecCapture(tool, arr);
}

// --mutate-prepare-dir: crea el directorio donde va a aterrizar una copia.
//
// Hace falta porque el receptor del árbol EXIGE que el directorio exista —«no es un
// directorio: …»— y `cp` aterriza en `<destino>/<nombre>`, que por definición todavía no
// está. Sin esto, copiar entre máquinas obligaría a ir antes a la otra a crear la carpeta a
// mano, que es pedirle al usuario que haga la mitad del trabajo.
//
// **Lo que NO es: un «mkdir» suelto.** Eso sería un verbo capaz de crear un árbol en
// cualquier parte del sistema, y esa superficie es justo la que este proyecto ha decidido no
// tener. Lleva el mismo contrato que `--mutate-advanced-fromdir-prepare`: una BASE que ya
// tiene que existir y ser un directorio, y una parte relativa que no puede escaparse de
// ella. Lo que se puede crear está acotado por lo que ya hay.
ExecResult runPrepareDirCapture(const std::vector<std::string>& params) {
    ExecResult r;
    if (params.size() < 2) {
        r.rc = 2;
        ponCodigo(r, "usage", {"--mutate-prepare-dir <base-absoluta> <relativa>"});
        return r;
    }
    const std::string base = trim(params[0]);
    const std::string rel = trim(params[1]);
    if (base.empty() || base.front() != '/') {
        r.rc = 2;
        ponCodigo(r, "base_not_absolute", {});
        return r;
    }
    if (rel.empty() || rel.front() == '/' || rel.front() == '\\'
        || rel.find("..") != std::string::npos) {
        r.rc = 2;
        ponCodigo(r, "bad_relative_part", {});
        return r;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(base, ec) || ec) {
        r.rc = 2;
        ponCodigo(r, "base_not_dir", {base});
        return r;
    }
    const std::string destino = base + (base.back() == '/' ? "" : "/") + rel;
    std::filesystem::create_directories(destino, ec);
    if (ec || !std::filesystem::is_directory(destino, ec)) {
        r.rc = 1;
        ponCodigo(r, "cannot_create", {destino});
        return r;
    }
    r.rc = 0;
    r.out = "DST=" + destino + "\n";
    return r;
}

// --mutate-advanced-fromdir-prepare: la PRIMERA MITAD de «Desde Dir», sin el tar.
//
// Monta el dataset, resuelve su punto de montaje efectivo, comprueba el subdirectorio y lo
// crea; luego dice en qué ruta absoluta ha quedado. Nada más.
//
// **Por qué existe.** `--mutate-advanced-fromdir` hace esto y además lee un tar por la
// entrada estándar, y el canal RPC no tiene entrada estándar: por eso ese verbo es solo de
// terminal y por eso «Desde Dir» era la única de las cuatro acciones que necesitaba una
// tubería de shell con las dos puntas por SSH.
//
// Partido en dos, la otra mitad la puede hacer el transporte de árbol entre daemons
// —`--tree-recv-listen` en el destino, `--tree-send-to-peer` en el origen—, que es el mismo
// que usa `zfs send`. Con eso los datos van de máquina a máquina en vez de pasar por el
// equipo de quien manda.
//
// `--tree-recv-listen` EXIGE que el directorio ya exista —comprueba `is_directory` y si no
// devuelve 2—: sin este verbo, el árbol solo sabe volcar en la raíz del dataset.
//
// Va fuera del `#ifndef _WIN32` a propósito: no usa tar ni nada de Unix, y
// `getDatasetMountpointCapture` ya consulta los montajes REALES —en Windows la propiedad
// `mountpoint` dice «/pool/ds» y el montaje de verdad es «Z:\ds»—.
ExecResult runMutateAdvancedFromDirPrepareCapture(const std::vector<std::string>& params) {
    ExecResult r;
    if (params.empty()) {
        r.rc = 2;
        ponCodigo(r, "usage", {"--mutate-advanced-fromdir-prepare <dataset> [relative-subdir]"});
        return r;
    }
    const std::string dataset = trim(params[0]);
    const std::string rel = params.size() > 1 ? trim(params[1]) : std::string();
    if (dataset.empty()) {
        r.rc = 2;
        ponCodigo(r, "empty_dataset", {});
        return r;
    }
    // La misma comprobación que hace el verbo con tar, y por el mismo motivo: cualquier cosa
    // que pueda salirse del punto de montaje del dataset. El cliente la hace también antes de
    // pedir nada, pero quien manda no es de fiar y este proceso es root.
    if (!rel.empty()
        && (rel.front() == '/' || rel.front() == '\\' || rel.find("..") != std::string::npos)) {
        r.rc = 2;
        ponCodigo(r, "bad_subdir", {});
        return r;
    }

    (void)runExecCapture("zfs", {"set", "canmount=on", dataset});
    (void)runExecCapture("zfs", {"mount", dataset});
    const ExecResult mpRes = getDatasetMountpointCapture(dataset);
    const std::string mp = (mpRes.rc == 0) ? trim(mpRes.out) : std::string();
    if (mp.empty()) {
        r.rc = 4;
        ponCodigo(r, "cannot_resolve_mountpoint", {});
        return r;
    }

    namespace fs = std::filesystem;
    const fs::path destino = rel.empty() ? fs::path(mp) : (fs::path(mp) / fs::path(rel));
    std::error_code mkec;
    fs::create_directories(destino, mkec);
    if (mkec && !fs::is_directory(destino)) {
        r.rc = 1;
        ponCodigo(r, "cannot_create_target_dir", {});
        return r;
    }
    r.rc = 0;
    r.out = "DST=" + destino.string() + "\n";
    return r;
}

#ifndef _WIN32
// ── Sync via tar with a temporary mountpoint ─────────────────────────────────
// Typed replacement for the buildUnixTemporaryTar{Source,Destination}Script shell
// scripts. A dataset that is not mounted is temporarily relocated to a scratch
// mountpoint so its contents can be streamed, then put back.
//
// The original mountpoint is stashed in a ZFS user property before moving it, exactly
// as the shell helpers did: that way a run killed outright still leaves a breadcrumb
// identifying the dataset and its intended mountpoint.

std::atomic<pid_t> g_streamChildPid{-1};

// Async-signal-safe: only touches an atomic and calls kill(). The actual restore
// happens in AltMountGuard's destructor once the wait below returns.
void onStreamSignal(int) {
    g_stop.store(true);
    const pid_t child = g_streamChildPid.load();
    if (child > 0) {
        kill(child, SIGTERM);
    }
}

// Qué dataset está montado exactamente en esa ruta, o vacío si ninguno. Hace falta
// distinguirlo del simple "sí/no": convertir un dataset en un directorio EN SU SITIO
// tiene como destino su propio punto de montaje, y eso es legítimo; lo que no lo es
// es escribir encima del punto de montaje de OTRO dataset.
std::string datasetMountedExactlyAt(const std::string& path) {
    const ExecResult mounts = runExecCapture("zfs", {"mount"});
    if (mounts.rc != 0) {
        return std::string();
    }
    std::error_code ec;
    const std::filesystem::path want = std::filesystem::weakly_canonical(path, ec);
    const std::string wantStr = ec ? path : want.string();
    for (const std::string& line : splitLines(mounts.out)) {
        const std::size_t sep = line.find_first_of(" \t");
        if (sep == std::string::npos) {
            continue;
        }
        const std::string ds = trim(line.substr(0, sep));
        const std::size_t mpStart = line.find_first_not_of(" \t", sep);
        if (mpStart == std::string::npos) {
            continue;
        }
        const std::string mp = trim(line.substr(mpStart));
        if (mp.empty()) {
            continue;
        }
        std::error_code mec;
        const std::filesystem::path canon = std::filesystem::weakly_canonical(mp, mec);
        if ((mec ? mp : canon.string()) == wantStr) {
            return ds;
        }
    }
    return std::string();
}

ExecResult runMutateAdvancedToDirCapture(const std::vector<std::string>& params) {
    ExecResult r;
    if (params.size() < 3) {
        r.rc = 2;
        ponCodigo(r, "usage", {"--mutate-advanced-todir <dataset> <dst-dir> <delete-source-0|1>"});
        return r;
    }
    namespace fs = std::filesystem;
    const std::string dataset = params[0];
    const std::string dstDir = params[1];
    const bool deleteSource = isTruthyValue(params[2]);

    if (dstDir.empty() || dstDir[0] != '/') {
        r.rc = 2;
        ponCodigo(r, "target_not_absolute", {});
        return r;
    }
    // Que el destino sea el punto de montaje del PROPIO dataset es el caso principal
    // —convertirlo en un directorio en su sitio—, y por eso el guard lo reubica luego a
    // un mountpoint temporal. Rechazarlo hacía imposible justamente eso. Lo que sigue
    // vetado es escribir encima del punto de montaje de otro dataset.
    {
        const std::string occupant = datasetMountedExactlyAt(dstDir);
        if (!occupant.empty() && occupant != dataset) {
            r.rc = 2;
            ponCodigo(r, "todir_target_is_mountpoint", {dstDir, occupant});
            return r;
        }
    }

    // Los datasets que hay que absorber, y su sitio relativo dentro del destino. Se
    // averigua ANTES de reubicar nada, y por PUNTO DE MONTAJE, no por nombre: tras un
    // Ensamblar que conservó los subdatasets, el que está montado aquí dentro puede ser
    // un hermano por nombre. Buscar por nombre no lo encontraría, su contenido no se
    // copiaría, y el borrado se lo llevaría por delante.
    struct Member {
        std::string name;
        std::string rel;  // ruta relativa al mountpoint del dataset raíz
    };
    std::vector<Member> members;
    // Se conserva fuera del bloque: la comprobación de descendientes de más abajo lo
    // necesita para explicar respecto a QUÉ están fuera.
    std::string rootMpForCheck;
    {
        ExecResult mp0 = getDatasetMountpointCapture(dataset);
        const std::string rootMp = (mp0.rc == 0) ? trim(mp0.out) : std::string();
        rootMpForCheck = rootMp;
        if (rootMp.empty()) {
            r.rc = 1;
            ponCodigo(r, "mountpoint_none", {});
            return r;
        }
        const std::string pool = dataset.substr(0, dataset.find('/'));
        const ExecResult ls =
            runExecCapture("zfs", {"list", "-H", "-o", "name,mountpoint", "-r", pool});
        for (const std::string& line : splitLines(ls.out)) {
            const std::size_t tab = line.find('\t');
            if (tab == std::string::npos) {
                continue;
            }
            const std::string name = trim(line.substr(0, tab));
            const std::string mp = trim(line.substr(tab + 1));
            if (mp.empty() || mp == "-" || mp == "none") {
                continue;
            }
            if (mp == rootMp) {
                if (name == dataset) {
                    members.push_back(Member{name, std::string()});
                }
                continue;
            }
            if (mp.rfind(rootMp + "/", 0) == 0) {
                members.push_back(Member{name, mp.substr(rootMp.size() + 1)});
            }
        }
        if (members.empty()) {
            r.rc = 1;
            ponCodigo(r, "cannot_resolve_source", {});
            return r;
        }
        // De hojas hacia arriba. Así cada dataset se copia SOLO con lo suyo (-x) y, al
        // borrar, ningún padre tiene hijos vivos: se destruye sin -r, que no puede
        // llevarse por delante nada que no estuviera en esta lista.
        std::sort(members.begin(), members.end(), [](const Member& a, const Member& b) {
            const auto depth = [](const std::string& v) {
                return v.empty() ? 0 : static_cast<int>(std::count(v.begin(), v.end(), '/')) + 1;
            };
            return depth(a.rel) > depth(b.rel);
        });
    }

    // TODO descendiente por NOMBRE tiene que estar entre los miembros, o no se empieza.
    //
    // Los miembros se eligen por punto de montaje, y eso es correcto —tras un Ensamblar
    // que conservó subdatasets, el montado aquí dentro puede ser un hermano por nombre—,
    // pero deja un hueco: un hijo montado FUERA del subárbol, por ejemplo pool/ds1/ds3
    // con mountpoint /b cuando ds1 está en /a. Su contenido no se copia (correcto: no
    // está en el directorio) pero sigue colgando de ds1 por nombre, así que al borrar,
    // `zfs destroy ds1` falla con "filesystem has children" — cuando sus hermanos YA
    // están destruidos.
    //
    // Eso se sabe ANTES de copiar nada, así que se comprueba aquí y se rechaza con el
    // motivo. Empezar para fallar a mitad de una operación destructiva no es aceptable
    // aunque los datos acaben a salvo.
    {
        const ExecResult kids =
            runExecCapture("zfs", {"list", "-H", "-o", "name,mountpoint", "-r", dataset});
        std::string outside;
        for (const std::string& line : splitLines(kids.out)) {
            const std::size_t tab = line.find('\t');
            const std::string name = trim(tab == std::string::npos ? line : line.substr(0, tab));
            if (name.empty()) {
                continue;
            }
            bool found = false;
            for (const Member& m : members) {
                if (m.name == name) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                const std::string mp =
                    (tab == std::string::npos) ? std::string() : trim(line.substr(tab + 1));
                outside += "  " + name + "  (mounted at " + (mp.empty() ? "?" : mp) + ")\n";
            }
        }
        if (!outside.empty()) {
            r.rc = 2;
            ponCodigo(r, "todir_outside_descendants", {dataset, rootMpForCheck, outside});
            return r;
        }
    }

    // Los subdatasets montados DENTRO impiden reubicar el raíz: `zfs set mountpoint`
    // tiene que desmontarlo y falla con "pool or dataset is busy". Se apartan primero,
    // de hojas a raíz, y se vuelven a montar en cuanto el raíz está en su sitio
    // temporal —hace falta que estén montados para poder copiar sus datos—.
    std::vector<std::string> nestedUnmounted;  // de más profundo a menos
    {
        std::vector<std::string> nested;
        for (const Member& m : members) {
            if (!m.rel.empty()) {
                nested.push_back(m.name);
            }
        }
        std::sort(nested.begin(), nested.end(), [](const std::string& a, const std::string& b) {
            return std::count(a.begin(), a.end(), '/') > std::count(b.begin(), b.end(), '/');
        });
        for (const std::string& ds : nested) {
            if (!datasetIsMounted(ds)) {
                continue;
            }
            const ExecResult um = runExecCapture("zfs", {"unmount", ds});
            if (um.rc != 0 && datasetIsMounted(ds)) {
                for (auto it = nestedUnmounted.rbegin(); it != nestedUnmounted.rend(); ++it) {
                    (void)runExecCapture("zfs", {"mount", *it});
                }
                r.rc = 1;
                ponCodigo(r, "todir_cannot_unmount", {ds, dataset, trim(um.err)});
                return r;
            }
            nestedUnmounted.push_back(ds);
        }
    }

    // Reubicar el dataset a un mountpoint temporal: sin esto, el destino no puede ser
    // la propia ruta donde el dataset estaba montado, que es el caso principal
    // (convertir un dataset en un directorio en su mismo sitio).
    AltMountGuard mountGuard;
    std::string srcMp;
    std::string mountErr;
    if (!mountGuard.engage(dataset, srcMp, mountErr)) {
        r.rc = 1;
        r.err = mountErr.empty() ? "cannot relocate dataset to a temporary mountpoint\n" : mountErr;
        return r;
    }

    // Devueltos a su sitio, ahora bajo el mountpoint temporal si lo heredaban. Sin esto
    // la copia leería directorios vacíos y se perderían sus datos.
    for (auto it = nestedUnmounted.rbegin(); it != nestedUnmounted.rend(); ++it) {
        if (!datasetIsMounted(*it)) {
            (void)runExecCapture("zfs", {"mount", *it});
        }
    }

    const fs::path dst(dstDir);
    const fs::path parent = dst.parent_path();
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
        r.rc = 1;
        ponCodigo(r, "cannot_create_target_parent", {});
        return r;
    }

    // Temporal hermano del destino para que el rename final sea atómico.
    const fs::path staging = parent / (dst.filename().string() + ".zfsmgr-new");
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) {
        r.rc = 1;
        ponCodigo(r, "cannot_create_staging", {});
        return r;
    }

    // Avance. Aquí el destino es un DIRECTORIO, no un dataset, así que no se puede
    // muestrear el dataset de origen —que además no crece, solo se lee—. Se busca el
    // dataset cuyo punto de montaje contiene el destino: es el que engorda mientras se
    // copia. Si el destino no cae dentro de ningún dataset, datasetUsedBytes devuelve
    // -1 y el muestreador se queda callado en vez de mentir.
    //
    // Esto tiene que ir DESPUÉS de reubicar el dataset: mientras estaba en su sitio, el
    // punto de montaje más específico que contiene el destino era él mismo, y se habría
    // muestreado justo el que no crece. Una vez apartado, el que casa es el de encima,
    // que es donde se está escribiendo la escala.
    std::string growingDs;
    {
        const ExecResult ls = runExecCapture("zfs", {"list", "-H", "-o", "name,mountpoint"});
        std::size_t best = 0;
        for (const std::string& line : splitLines(ls.out)) {
            const std::size_t tab = line.find('\t');
            if (tab == std::string::npos) {
                continue;
            }
            const std::string name = trim(line.substr(0, tab));
            const std::string mp = trim(line.substr(tab + 1));
            if (mp.empty() || mp == "-" || mp == "none" || mp[0] != '/') {
                continue;
            }
            const bool contains = (dstDir == mp) || dstDir.rfind(mp + "/", 0) == 0;
            if (contains && mp.size() > best) {  // el más específico gana
                best = mp.size();
                growingDs = name;
            }
        }
    }
    // Una copia por dataset, con -x, de hojas hacia arriba. Antes se hacía UNA sola
    // pasada de rsync sin -x desde el dataset raíz: cruzaba los puntos de montaje de
    // los hijos y arrastraba su contenido, lo cual funcionaba solo si todos seguían
    // montados debajo. Los que tienen mountpoint explícito —los que deja un Ensamblar
    // que los conservó— NO se mueven con el padre al reubicarlo, así que su contenido
    // se quedaba sin copiar y el `destroy -r` posterior lo destruía.
    for (std::size_t i = 0; i < members.size(); ++i) {
        const Member& m = members[i];
        ExecResult mpRes = getDatasetMountpointCapture(m.name);
        const std::string from = (mpRes.rc == 0) ? trim(mpRes.out) : std::string();
        if (from.empty()) {
            fs::remove_all(staging, ec);
            r.rc = 1;
            ponCodigo(r, "cannot_find_mountpoint", {m.name});
            return r;
        }
        const fs::path to = m.rel.empty() ? staging : (staging / fs::path(m.rel));
        fs::create_directories(to, ec);
        if (ec) {
            fs::remove_all(staging, ec);
            r.rc = 1;
            ponCodigo(r, "cannot_create", {to.string()});
            return r;
        }
        if (jobCancelRequested()) {
            fs::remove_all(staging, ec);
            r.rc = 125;
            ponCodigo(r, "cancelled_before_changes", {});
            return r;
        }
        const std::string label = "[TODIR] " + std::to_string(i + 1) + " of "
                                  + std::to_string(members.size()) + ": "
                                  + (m.rel.empty() ? std::string(".") : m.rel);
        reportJobProgress(label);
        int left = -1;
        {
            CopyProgressSampler sampler(growingDs, label);
            ExecResult rs = runCopyTreeCapture(from, to.string(), {}, true);
            if (rs.rc != 0) {
                fs::remove_all(staging, ec);
                return rs;
            }
            // Verificado antes de que nadie destruya nada, con reintentos: si la copia
            // quedó corta, los datos solo están en el dataset original.
            left = countPendingCopyTransfers(from, to.string(), {}, true);
            for (int attempt = 1; left != 0 && attempt < 5; ++attempt) {
                ExecResult again = runCopyTreeCapture(from, to.string(), {}, true);
                if (again.rc != 0) {
                    fs::remove_all(staging, ec);
                    return again;
                }
                left = countPendingCopyTransfers(from, to.string(), {}, true);
            }
        }
        if (left != 0) {
            fs::remove_all(staging, ec);
            r.rc = 42;
            ponCodigo(r, "verify_failed",
                      {m.name, left < 0 ? std::string("?") : std::to_string(left)});
            return r;
        }
    }
    reportJobProgress("[TODIR] copy finished; putting it in place");

    // Desmontar TODOS los miembros antes de tocar el destino. Los que tienen mountpoint
    // explícito no se movieron al reubicar el raíz, así que siguen montados dentro del
    // destino; y renombrar un directorio que contiene un punto de montaje da EBUSY. Se
    // hace aquí, con las copias ya verificadas, para que desmontar no pueda perder nada.
    for (const Member& m : members) {
        const ExecResult um = runExecCapture("zfs", {"unmount", m.name});
        const ExecResult still = getZfsPropertyCapture(m.name, "mounted");
        if (um.rc != 0 && still.rc == 0 && trim(still.out) == "yes") {
            fs::remove_all(staging, ec);
            r.rc = 1;
            ponCodigo(r, "todir_cannot_unmount_target", {m.name, trim(um.err)});
            return r;
        }
    }

    // Apartar el destino existente; el guard lo repone si algo falla a partir de aquí.
    DestBackupGuard backupGuard;
    backupGuard.target = dst;
    if (fs::exists(dst, ec)) {
        fs::path candidate = parent / (dst.filename().string() + ".zfsmgr-bak");
        for (int i = 1; fs::exists(candidate, ec); ++i) {
            candidate = parent / (dst.filename().string() + ".zfsmgr-bak-" + std::to_string(i));
        }
        fs::rename(dst, candidate, ec);
        if (ec) {
            fs::remove_all(staging, ec);
            r.rc = 1;
            ponCodigo(r, "cannot_move_aside", {});
            return r;
        }
        backupGuard.backup = candidate;
        backupGuard.active = true;
    }

    fs::rename(staging, dst, ec);
    if (ec) {
        fs::remove_all(staging, ec);
        r.rc = 1;
        ponCodigo(r, "cannot_move_into_place", {});
        return r;  // el guard repone el destino anterior
    }

    // A PARTIR DE AQUÍ NO SE REVIERTE EL DESTINO. Y es lo que impide perder datos.
    //
    // El commit estaba al final, después del borrado. Con él allí, cualquier fallo del
    // bucle de destrucción hacía que el guard repusiera el destino anterior... borrando
    // la copia recién puesta, cuando los datasets ya destruidos solo existían ahí.
    //
    // Medido: con hdtest/ds1 en /a, ds2 heredando /a/ds2 y ds3 con mountpoint /b, el
    // borrado destruye ds2 (hoja, correcto) y falla al destruir ds1 —«filesystem has
    // children: ds3»—. Al volver, el guard revertía /a y el contenido de ds2 desaparecía
    // de los dos sitios a la vez.
    //
    // Revertir es correcto mientras solo se han movido directorios; deja de serlo en
    // cuanto se destruye el primer dataset. Ese es el punto de no retorno, y aquí está.
    backupGuard.commit();

    if (deleteSource) {
        // De hojas hacia arriba y SIN -r. `members` ya viene en ese orden, así que
        // cuando le toca a un padre no le queda ningún hijo vivo. Sin -r, la orden no
        // puede arrastrar nada que no estuviera en la lista que se copió y verificó;
        // si quedara algo colgando, falla en vez de destruirlo.
        for (const Member& m : members) {
            ExecResult d = runExecCapture("zfs", {"destroy", m.name});
            if (d.rc != 0) {
                // El respaldo NO se borra al salir por error, y se dice dónde está: es el
                // contenido anterior del destino y ya nadie va a reponerlo.
                if (!backupGuard.backup.empty()) {
                    ponCodigo(d, "todir_cannot_destroy_backup",
                              {trim(d.err), dstDir, m.name, backupGuard.backup.string()});
                } else {
                    ponCodigo(d, "todir_cannot_destroy", {trim(d.err), dstDir, m.name});
                }
                return d;
            }
            if (m.name == dataset) {
                mountGuard.active = false;  // ya no existe: no hay nada que restaurar
            }
        }
        mountGuard.release();
    } else {
        // canmount=off en TODOS los miembros, no solo en el raíz: ya están desmontados,
        // y sin esto cualquiera de ellos volvería a montarse encima del directorio
        // recién creado en el siguiente arranque y taparía justo lo que se copió.
        for (const Member& m : members) {
            (void)runExecCapture("zfs", {"set", "canmount=off", m.name});
        }
        mountGuard.release();  // devuelve el dataset raíz a su mountpoint original
    }

    // A partir de aquí la operación es definitiva: se descarta el respaldo.
    // El commit ya se hizo antes del borrado; aquí solo se retira el respaldo, que a
    // estas alturas es basura porque todo ha salido bien.
    if (!backupGuard.backup.empty()) {
        fs::remove_all(backupGuard.backup, ec);
    }
    r.rc = 0;
    r.out = deleteSource ? "[TODIR] ok\n" : "[TODIR] ok (dataset preserved unmounted)\n";
    return r;
}

std::vector<StrandedAltMount> findStrandedAltMounts() {
    std::vector<StrandedAltMount> out;
    // -s local restricts this to datasets where we explicitly set the property.
    const ExecResult r = runExecCapture(
        "zfs", {"get", "-H", "-o", "name,value", "-s", "local", kSavedMountpointProp});
    if (r.rc != 0) {
        return out;
    }
    std::istringstream iss(r.out);
    std::string line;
    while (std::getline(iss, line)) {
        const std::string trimmedLine = trim(line);
        if (trimmedLine.empty()) {
            continue;
        }
        const std::size_t tab = trimmedLine.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        StrandedAltMount s;
        s.dataset = trim(trimmedLine.substr(0, tab));
        s.savedMp = trim(trimmedLine.substr(tab + 1));
        if (s.dataset.empty()) {
            continue;
        }
        const ExecResult cur = getZfsPropertyCapture(s.dataset, "mountpoint");
        s.currentMp = (cur.rc == 0) ? trim(cur.out) : std::string();
        out.push_back(std::move(s));
    }
    return out;
}

// --repair-alt-mountpoints [apply]
// Without "apply" it only reports, so it is safe to call for diagnosis.
ExecResult runRepairAltMountpointsCapture(bool apply) {
    ExecResult r;
    const std::vector<StrandedAltMount> stranded = findStrandedAltMounts();
    std::string out;
    int repaired = 0;
    int failed = 0;
    for (const StrandedAltMount& s : stranded) {
        out += "STRANDED=" + s.dataset + " saved=" + s.savedMp + " current=" + s.currentMp + "\n";
        if (!apply) {
            continue;
        }
        // Mirror the guard's restore order: unmount what hangs from the temporary place,
        // then put the mountpoint back —with its source—, then drop the breadcrumb.
        desmontaBajo(s.dataset, s.currentMp);
        (void)runExecCapture("zfs", {"unmount", s.dataset});
        const ExecResult src = getZfsPropertyCapture(s.dataset, kSavedMountpointSourceProp);
        std::string origen = (src.rc == 0) ? trim(src.out) : std::string();
        if (origen == "-") {
            origen.clear();
        }
        std::string errRest;
        const bool ok = restauraMountpoint(s.dataset, s.savedMp, origen, errRest);
        if (!ok) {
            r.err += "cannot restore mountpoint for " + s.dataset + ": " + errRest + "\n";
        }
        if (ok) {
            // Only drop the breadcrumb once the mountpoint is actually back, so a
            // failed repair stays discoverable on the next run.
            (void)runExecCapture("zfs", {"inherit", kSavedMountpointProp, s.dataset});
            (void)runExecCapture("zfs", {"inherit", kSavedMountpointSourceProp, s.dataset});
            // Y el directorio temporal, si es nuestro y quedó vacío.
            if (s.currentMp.rfind("/tmp/zfsmgr-sync-", 0) == 0) {
                rmdir(s.currentMp.c_str());
            }
            out += "REPAIRED=" + s.dataset + " -> " + s.savedMp + "\n";
            ++repaired;
        } else {
            ++failed;
        }
    }
    out += "STRANDED_COUNT=" + std::to_string(stranded.size()) + "\n";
    out += "REPAIRED_COUNT=" + std::to_string(repaired) + "\n";
    out += "FAILED_COUNT=" + std::to_string(failed) + "\n";
    r.out = out;
    r.rc = (failed > 0) ? 1 : 0;
    return r;
}

// Runs `producer | consumer`, both execvp'd, with the outer ends left on our own
// stdin/stdout so the tar stream flows through the SSH pipeline.
int runStreamPair(const std::vector<std::string>& producer,
                  const std::vector<std::string>& consumer) {
    auto toArgv = [](const std::vector<std::string>& v) {
        std::vector<char*> a;
        a.reserve(v.size() + 1);
        for (const std::string& s : v) {
            a.push_back(const_cast<char*>(s.c_str()));
        }
        a.push_back(nullptr);
        return a;
    };
    if (consumer.empty()) {
        std::vector<char*> a = toArgv(producer);
        const pid_t pid = fork();
        if (pid < 0)
            return 125;
        if (pid == 0) {
            execvp(producer[0].c_str(), a.data());
            _exit(127);
        }
        g_streamChildPid.store(pid);
        int st = 0;
        waitpid(pid, &st, 0);
        g_streamChildPid.store(-1);
        return decodeWaitStatus(st);
    }
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0)
        return 125;
    std::vector<char*> pa = toArgv(producer);
    std::vector<char*> ca = toArgv(consumer);
    const pid_t p1 = fork();
    if (p1 < 0) {
        close(fds[0]);
        close(fds[1]);
        return 125;
    }
    if (p1 == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execvp(producer[0].c_str(), pa.data());
        _exit(127);
    }
    const pid_t p2 = fork();
    if (p2 < 0) {
        close(fds[0]);
        close(fds[1]);
        int st = 0;
        waitpid(p1, &st, 0);
        return 125;
    }
    if (p2 == 0) {
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        close(fds[1]);
        execvp(consumer[0].c_str(), ca.data());
        _exit(127);
    }
    close(fds[0]);
    close(fds[1]);
    g_streamChildPid.store(p1);
    int st1 = 0;
    int st2 = 0;
    waitpid(p1, &st1, 0);
    waitpid(p2, &st2, 0);
    g_streamChildPid.store(-1);
    const int rc2 = decodeWaitStatus(st2);
    return (rc2 != 0) ? rc2 : decodeWaitStatus(st1);
}

bool validSyncCodec(const std::string& codec) {
    return codec == "none" || codec == "zstd" || codec == "gzip";
}

// --mutate-sync-temp-tar-source <dataset> [none|zstd|gzip]
int runSyncTempTarSource(const std::vector<std::string>& params) {
    if (params.empty()) {
        std::cerr << "usage: --mutate-sync-temp-tar-source <dataset> [none|zstd|gzip]\n";
        return 2;
    }
    const std::string dataset = trim(params[0]);
    const std::string codec = params.size() > 1 ? toLower(trim(params[1])) : std::string("none");
    if (dataset.empty() || !validSyncCodec(codec)) {
        std::cerr << "invalid dataset or codec\n";
        return 2;
    }
    std::signal(SIGINT, onStreamSignal);
    std::signal(SIGTERM, onStreamSignal);

    AltMountGuard guard;
    const ExecResult mpRes = getDatasetMountpointCapture(dataset);
    std::string mp = (mpRes.rc == 0) ? trim(mpRes.out) : std::string();
    if (mp.empty()) {
        std::string errMsg;
        if (!guard.engage(dataset, mp, errMsg)) {
            std::cerr << errMsg;
            return 41;
        }
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    if (mp.empty() || !fs::is_directory(fs::path(mp), ec)) {
        std::cerr << "could not resolve a usable mountpoint\n";
        return 41;
    }
    const std::vector<std::string> tarCmd = {"tar", "--acls", "--xattrs", "-cpf",
                                             "-",   "-C",     mp,         "."};
    if (codec == "zstd") {
        return runStreamPair(tarCmd, {"zstd", "-1", "-T0", "-q", "-c"});
    }
    if (codec == "gzip") {
        return runStreamPair(tarCmd, {"gzip", "-1", "-c"});
    }
    return runStreamPair(tarCmd, {});
}

// --mutate-sync-temp-tar-dest <dataset> [none|zstd|gzip]
int runSyncTempTarDest(const std::vector<std::string>& params) {
    if (params.empty()) {
        std::cerr << "usage: --mutate-sync-temp-tar-dest <dataset> [none|zstd|gzip]\n";
        return 2;
    }
    const std::string dataset = trim(params[0]);
    const std::string codec = params.size() > 1 ? toLower(trim(params[1])) : std::string("none");
    if (dataset.empty() || !validSyncCodec(codec)) {
        std::cerr << "invalid dataset or codec\n";
        return 2;
    }
    std::signal(SIGINT, onStreamSignal);
    std::signal(SIGTERM, onStreamSignal);

    AltMountGuard guard;
    const ExecResult mpRes = getDatasetMountpointCapture(dataset);
    std::string mp = (mpRes.rc == 0) ? trim(mpRes.out) : std::string();
    if (mp.empty()) {
        std::string errMsg;
        if (!guard.engage(dataset, mp, errMsg)) {
            std::cerr << errMsg;
            return 41;
        }
    }
    if (mp.empty()) {
        std::cerr << "could not resolve a usable mountpoint\n";
        return 41;
    }
    namespace fs = std::filesystem;
    std::error_code mkec;
    fs::create_directories(fs::path(mp), mkec);
    const std::vector<std::string> tarCmd = {"tar", "--acls", "--xattrs", "-xpf", "-", "-C", mp};
    if (codec == "zstd") {
        return runStreamPair({"zstd", "-d", "-q", "-c", "-"}, tarCmd);
    }
    if (codec == "gzip") {
        return runStreamPair({"gzip", "-d", "-c", "-"}, tarCmd);
    }
    return runStreamPair(tarCmd, {});
}

// --mutate-advanced-fromdir: typed replacement for the "Desde Dir" destination shell
// script. Mounts the dataset, resolves its effective mountpoint, creates the target
// subdirectory and extracts the tar stream arriving on stdin — all with execvp, no
// shell. Completes the set alongside --mutate-advanced-breakdown/assemble/todir.
//
// CLI only, deliberately: this reads the tar stream from stdin, and the RPC channel
// has no stdin. The GUI invokes it as the destination end of an SSH pipeline.
int runMutateAdvancedFromDir(const std::vector<std::string>& params) {
    if (params.empty()) {
        std::cerr << "usage: --mutate-advanced-fromdir <dataset> [relative-subdir]\n";
        return 2;
    }
    const std::string dataset = trim(params[0]);
    const std::string rel = params.size() > 1 ? trim(params[1]) : std::string();
    if (dataset.empty()) {
        std::cerr << "empty dataset\n";
        return 2;
    }
    // Refuse anything that could escape the dataset's mountpoint.
    if (!rel.empty() && (rel.front() == '/' || rel.find("..") != std::string::npos)) {
        std::cerr << "invalid relative subdirectory\n";
        return 2;
    }

    (void)runExecCapture("zfs", {"set", "canmount=on", dataset});
    (void)runExecCapture("zfs", {"mount", dataset});
    const ExecResult mpRes = getDatasetMountpointCapture(dataset);
    const std::string mp = (mpRes.rc == 0) ? trim(mpRes.out) : std::string();
    if (mp.empty()) {
        std::cerr << "could not resolve effective mountpoint\n";
        return 4;
    }

    const std::string dst = rel.empty() ? mp : (mp + "/" + rel);
    namespace fs = std::filesystem;
    std::error_code mkec;
    fs::create_directories(fs::path(dst), mkec);
    if (mkec) {
        std::cerr << "cannot create destination directory\n";
        return 1;
    }
    std::cout << "[FROMDIR] tar recv -> " << dst << "\n";
    std::cout.flush();
    // stdin stays inherited so tar consumes the incoming stream.
    return runExecStreaming("tar", {"--acls", "--xattrs", "-xpf", "-", "-C", dst});
}

// --mutate-remove-content <dataset> <mountpoint> <rel> <0|1>: `destroy ds#content/<rel>`.
//
// Deleting as root, so nothing is taken on the client's word: `rel` passes the same rule the
// client applies (no root, no `..`, nothing under `.zfs`), `mountpoint` must be where
// `dataset` is really mounted, and the deletion never crosses into another filesystem. A
// symbolic link is deleted itself, never followed.
ExecResult runRemoveContentCapture(const std::vector<std::string>& params) {
    namespace fs = std::filesystem;
    ExecResult r;
    if (params.size() < 4) {
        r.rc = 2;
        ponCodigo(r, "usage", {"--mutate-remove-content <dataset> <mountpoint> <rel> <0|1>"});
        return r;
    }
    const std::string dataset = trim(params[0]);
    const std::string mountpoint = trim(params[1]);
    const std::string rel = trim(params[2]);
    const bool recursive = trim(params[3]) == "1";
    if (const std::string no = zfsmgr::commands::advanced::contentRemovalRefusal(rel);
        !no.empty()) {
        r.rc = 2;
        ponCodigo(r, no, {});
        return r;
    }
    if (mountpoint.empty() || mountpoint.front() != '/'
        || datasetMountedExactlyAt(mountpoint) != dataset) {
        r.rc = 2;
        ponCodigo(r, "remove_not_dataset_mount", {dataset, mountpoint});
        return r;
    }
    struct stat raiz{};
    if (::stat(mountpoint.c_str(), &raiz) != 0) {
        r.rc = 1;
        ponCodigo(r, "remove_not_found", {mountpoint});
        return r;
    }
    const fs::path target = fs::path(mountpoint) / rel;
    struct stat st{};
    if (::lstat(target.c_str(), &st) != 0) {
        r.rc = 1;
        ponCodigo(r, "remove_not_found", {target.string()});
        return r;
    }
    if (st.st_dev != raiz.st_dev) {
        r.rc = 2;
        ponCodigo(r, "remove_is_mountpoint", {target.string()});
        return r;
    }
    if (S_ISDIR(st.st_mode) && !recursive) {
        r.rc = 2;
        ponCodigo(r, "remove_is_dir", {target.string()});
        return r;
    }
    std::string err;
    if (S_ISDIR(st.st_mode)) {
        if (!removeTreeWithoutCrossingMounts(target, raiz.st_dev, err)) {
            r.rc = 1;
            ponCodigo(r, "remove_failed", {err});
            return r;
        }
    } else if (::unlink(target.c_str()) != 0) {
        r.rc = 1;
        ponCodigo(r, "remove_failed", {target.string() + ": " + std::strerror(errno)});
        return r;
    }
    daemonLog("INFO", "removed " + target.string() + (S_ISDIR(st.st_mode) ? " (tree)" : ""));
    r.rc = 0;
    r.out = "REMOVED=" + target.string() + "\n";
    return r;
}

// --mutate-remove-tree <path>: the source directory of `fromdir --delete-source`, once the
// client has verified the copy. Outside ZFS only —inside a dataset the tool is `breakdown`,
// which moves nothing—, never the root or a first-level directory, never through a link
// and never into another filesystem mounted below.
ExecResult runRemoveTreeCapture(const std::vector<std::string>& params) {
    ExecResult r;
    const std::string ruta = params.empty() ? std::string() : trim(params[0]);
    if (const std::string no = zfsmgr::commands::advanced::treeRemovalRefusal(ruta); !no.empty()) {
        r.rc = 2;
        ponCodigo(r, no, {ruta});
        return r;
    }
    struct stat st{};
    if (::lstat(ruta.c_str(), &st) != 0) {
        r.rc = 1;
        ponCodigo(r, "remove_not_found", {ruta});
        return r;
    }
    if (!S_ISDIR(st.st_mode)) {
        r.rc = 2;
        ponCodigo(r, "remove_tree_not_dir", {ruta});
        return r;
    }
    if (runExecCapture("zfs", {"list", "-H", "-o", "name", ruta}).rc == 0) {
        r.rc = 2;
        ponCodigo(r, "remove_tree_in_zfs", {ruta});
        return r;
    }
    std::string err;
    if (!removeTreeWithoutCrossingMounts(std::filesystem::path(ruta), st.st_dev, err)) {
        r.rc = 1;
        ponCodigo(r, "remove_failed", {err});
        return r;
    }
    daemonLog("INFO", "removed source tree " + ruta);
    r.rc = 0;
    r.out = "REMOVED=" + ruta + "\n";
    return r;
}
#else
ExecResult runRemoveTreeCapture(const std::vector<std::string>&) {
    ExecResult r;
    r.rc = 2;
    ponCodigo(r, "remove_tree_not_on_windows", {});
    return r;
}
ExecResult runRemoveContentCapture(const std::vector<std::string>&) {
    ExecResult r;
    r.rc = 2;
    ponCodigo(r, "remove_not_on_windows", {});
    return r;
}
#endif  // _WIN32

// Fuera del bloque de Unix de arriba, A PROPÓSITO: la usan los dos despachadores en todas
// las plataformas. Dentro de él, el agente de Windows dejaba de compilar —«not declared
// in this scope»— y no se vio hasta la siguiente compilación de las cinco plataformas,
// porque ese día solo se recompilaron los de Linux y macOS.
// Añade al listado una columna más: si el VOLUMEN tiene su dispositivo expuesto.
//
// `zfs list` no sabe contestarlo. Lo más cerca está `volmode`, y no sirve: el valor que trae
// casi siempre es `default` —«lo que diga el parámetro del módulo»— que no dice si el
// dispositivo está o no. Medido en fc16: el pool entero tiene `volmode=default`.
//
// Lo que sí es un hecho comprobable es que exista el nodo. `back/tooling` tiene
// `volmode=none` puesto a mano y `/dev/zvol/back/tooling` NO existe; si lo cambiara a `full`
// aparecería. Esto mira el nodo, no la intención, y por eso contesta siempre algo cierto.
//
// Va como columna al final, igual que `type` y por el mismo motivo: un cliente anterior a
// esto lee por índice y la ignora.
//
// Dónde está el dispositivo NO es lo mismo en cada sistema, y esto se midió, no se supuso:
//
//   Linux y FreeBSD  `/dev/zvol/<pool>/<ds>`, un enlace con el nombre del dataset.
//   macOS            NO existe `/dev/zvol` —comprobado en mbp, con zfs-2.4.3-rc3—. El zvol
//                    sale como disco entero suelto, `/dev/disk3`, cuyo nombre de medio es
//                    «ZVOL <pool>/<ds>». Se pregunta al registro de entrada/salida, que lo
//                    enumera en 27 ms y lo nombra igual que el dataset. Con `volmode=none`
//                    el disco desaparece de ahí, que es justo lo que se quiere saber.
//   Windows          un zvol no se expone de ninguna de las dos formas: no se afirma nada.
#ifdef __APPLE__
// Los zvols expuestos AHORA MISMO, por nombre de dataset. Una sola llamada por listado: el
// coste no puede crecer con el número de filas.
std::set<std::string> zvolsExpuestosMacos() {
    std::set<std::string> v;
    const ExecResult r = runExecCapture("ioreg", {"-c", "IOMedia", "-r"});
    if (r.rc != 0) {
        return v;
    }
    for (const std::string& linea : splitLines(r.out)) {
        // «+-o ZVOL zvtest/vol Media  <class IOMedia, id 0x…»
        const std::size_t ini = linea.find("ZVOL ");
        if (ini == std::string::npos) {
            continue;
        }
        const std::size_t desde = ini + 5;
        const std::size_t fin = linea.find(" Media", desde);
        if (fin == std::string::npos || fin <= desde) {
            continue;
        }
        v.insert(trim(linea.substr(desde, fin - desde)));
    }
    return v;
}
#endif

ExecResult conDispositivoDeVolumen(ExecResult lista) {
    if (lista.rc != 0 || lista.out.empty()) {
        return lista;
    }
#ifdef __APPLE__
    // Perezoso: si el listado no trae ni un volumen, no se llama a `ioreg`.
    bool consultado = false;
    std::set<std::string> expuestos;
#endif
    std::ostringstream salida;
    for (const std::string& linea : splitLines(lista.out)) {
        if (trim(linea).empty()) {
            continue;
        }
        const std::vector<std::string> c = zfsmgr::base::split(linea, "\t", false);
        // La columna 10 es `type`. Sin ella no se sabe qué es, y entonces no se añade nada:
        // vale más una columna ausente que una inventada.
        const std::string tipo = c.size() > 10 ? toLower(trim(c[10])) : std::string();
        std::string marca = "-";
        if (tipo == "volume" && !c.empty()) {
            const std::string nombre = trim(c[0]);
#if defined(__APPLE__)
            if (!consultado) {
                expuestos = zvolsExpuestosMacos();
                consultado = true;
            }
            marca = expuestos.count(nombre) != 0 ? "device" : "no-device";
#elif !defined(_WIN32)
            std::error_code ec;
            marca =
                std::filesystem::exists("/dev/zvol/" + nombre, ec) && !ec ? "device" : "no-device";
#endif
        }
        salida << linea << "\t" << marca << "\n";
    }
    lista.out = salida.str();
    return lista;
}

// --mutate-zfs-allow-batch: typed replacement for the old semicolon-joined shell
// batch that used to be routed through --mutate-shell-generic. Payload is
// base64(JSON array); each element is base64(JSON array) holding one
// `zfs allow|unallow ...` argv (without the leading "zfs"). Only allow/unallow
// ops are accepted, and every entry runs via execvp — never through a shell.
bool decodeZfsAllowBatch(const std::string& payloadB64, std::vector<std::vector<std::string>>& out,
                         std::string& errMsg) {
    out.clear();
    std::string decoded;
    if (!decodeBase64(payloadB64, decoded)) {
        errMsg = "invalid allow-batch payload\n";
        return false;
    }
    std::vector<std::string> items;
    if (!parseJsonStringArray(decoded, items) || items.empty()) {
        errMsg = "invalid allow-batch payload\n";
        return false;
    }
    for (const std::string& itemB64 : items) {
        std::string innerJson;
        std::vector<std::string> argv;
        if (!decodeBase64(itemB64, innerJson) || !parseJsonStringArray(innerJson, argv)
            || argv.empty()) {
            errMsg = "invalid allow-batch entry\n";
            return false;
        }
        const std::string op = toLower(trim(argv.front()));
        if (op != "allow" && op != "unallow") {
            errMsg = "unsupported allow-batch op: " + argv.front() + "\n";
            return false;
        }
        out.push_back(std::move(argv));
    }
    return true;
}

ExecResult runZfsAllowBatchCapture(const std::string& payloadB64) {
    ExecResult r;
    std::vector<std::vector<std::string>> batch;
    std::string errMsg;
    if (!decodeZfsAllowBatch(payloadB64, batch, errMsg)) {
        r.rc = 2;
        r.err = errMsg;
        return r;
    }
    r.rc = 0;
    for (const std::vector<std::string>& argv : batch) {
        const ExecResult sub = runExecCapture("zfs", argv);
        r.out += sub.out;
        r.err += sub.err;
        if (sub.rc != 0 && r.rc == 0) {
            r.rc = sub.rc;  // run all entries; surface the first failure
        }
    }
    return r;
}

// --mutate-copy-local: copiar dentro de esta máquina con el motor del proyecto.
//
// El mismo `copytree` que ya usan `todir`, `breakdown` y `assemble` —ficheros dispersos,
// fechas con nanosegundos, enlaces duros, dueño y modo— pero expuesto como verbo, para que
// `cp` no tenga que llamar al `rsync` del sistema. Una dependencia externa menos, y un solo
// motor local en vez de dos que hacen casi lo mismo de maneras distintas.
//
// La carga es `[borrar, enSeco, origen1, destino1, …]`, la de rsync sin los dos campos que
// solo rsync entiende.
ExecResult runCopyLocalCapture(const std::string& payloadB64) {
    ExecResult r;
    std::string decoded;
    if (!zfsmgr::base::base64Decode(payloadB64, decoded)) {
        r.rc = 2;
        ponCodigo(r, "internal", {"unreadable payload"});
        return r;
    }
    zfsmgr::base::json::Value raiz;
    std::string errJson;
    if (!zfsmgr::base::json::parse(decoded, raiz, &errJson) || !raiz.isArray()) {
        r.rc = 2;
        ponCodigo(r, "internal", {std::string("payload is not a list: ") + errJson});
        return r;
    }
    const auto campos = raiz.toArray();
    if (campos.size() < 4 || (campos.size() - 2) % 2 != 0) {
        r.rc = 2;
        ponCodigo(r, "internal", {"the payload needs [delete, dryRun, source, target, …]"});
        return r;
    }
    zfsmgr::copytree::Options opt;
    opt.deleteExtraneous = campos[0].toString() == "1";
    opt.dryRun = campos[1].toString() == "1";
    // `oneFileSystem` NO se activa: `cp` copia lo que le digan, y saltarse un dataset
    // montado dentro dejaría parte del árbol sin copiar en silencio. `todir` sí lo activa,
    // porque allí los hijos se tratan uno a uno.
    //
    // **El progreso, mientras dura.** Un trabajo de copia salía en `jobs` con 0 B, 0 MiB/s
    // y 0 segundos hasta el final, y ni siquiera entonces se anotaba el total: solo quedaba
    // en la línea de salida. El motor cuenta los bytes en un contador compartido, y un hilo
    // lo pasa al registro del trabajo cada segundo. Fuera de un trabajo —con `--wait`—
    // `reportJobCounters` no hace nada.
    std::atomic<std::uint64_t> progreso{0};
    opt.progress = &progreso;
    const auto inicio = std::chrono::steady_clock::now();
    const auto anota = [&]() {
        const double secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - inicio).count();
        const std::uint64_t b = progreso.load(std::memory_order_relaxed);
        reportJobCounters(b, secs > 0.0 ? (static_cast<double>(b) / secs) / (1024.0 * 1024.0) : 0.0,
                          static_cast<long>(secs));
    };
    std::atomic<bool> parar{false};
    const std::string trabajo = t_currentJobId;
    std::thread muestreo([&, trabajo]() {
        t_currentJobId = trabajo;  // es thread_local: en un hilo nuevo nace vacío
        while (!parar.load()) {
            for (int i = 0; i < 10 && !parar.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            anota();
        }
    });
    struct AlSalir {
        std::atomic<bool>& parar;
        std::thread& hilo;
        std::function<void()> ultima;
        ~AlSalir() {
            parar = true;
            if (hilo.joinable()) {
                hilo.join();
            }
            ultima();  // el total, ya con la copia terminada
        }
    } alSalir{parar, muestreo, anota};
    r.rc = 0;
    for (std::size_t i = 2; i + 1 < campos.size(); i += 2) {
        const std::string origen = trim(campos[i].toString());
        const std::string destino = trim(campos[i + 1].toString());
        if (origen.empty() || origen.front() != '/' || destino.empty() || destino.front() != '/') {
            r.rc = 2;
            ponCodigo(r, "paths_not_absolute", {});
            return r;
        }
        // En SECO, la lista de lo que cambiaría, una línea por entrada: es para lo que se
        // pide una pasada en seco —ver qué va a borrar `--delete` antes de dejarle—, y la
        // cuenta sola no lo dice. Un carácter de clase y la ruta relativa:
        //   + nuevo   ! cambia de tipo   > contenido   . solo metadatos   - se borra
        if (opt.dryRun) {
            opt.itemize = [&r](zfsmgr::copytree::Change c, const std::string& rel) {
                using C = zfsmgr::copytree::Change;
                const char* marca = c == C::Created    ? "+ "
                                    : c == C::Replaced ? "! "
                                    : c == C::Content  ? "> "
                                    : c == C::Metadata ? ". "
                                                       : "- ";
                r.out += marca + rel + "\n";
            };
        }
        const zfsmgr::copytree::Result cr = zfsmgr::copytree::copyTree(origen, destino, opt);
        r.out += "FILES=" + std::to_string(cr.filesCopied) + " DIRS="
                 + std::to_string(cr.dirsCreated) + " SYMLINKS=" + std::to_string(cr.symlinksCopied)
                 + " SPECIALS=" + std::to_string(cr.specialsCreated)
                 + " HARDLINKS=" + std::to_string(cr.hardLinksRecreated) + " BYTES="
                 + std::to_string(cr.bytesWritten) + " UNCHANGED=" + std::to_string(cr.filesSkipped)
                 + " DELETED=" + std::to_string(cr.entriesDeleted) + "\n";
        // Un fallo no corta la copia —se sigue con lo demás, como rsync—, pero sí el resto
        // de pares, y se dicen TODOS los motivos: con solo el primero, arreglarlo destapaba
        // el siguiente en otra pasada.
        if (!cr.ok) {
            r.rc = 1;
            for (const std::string& e : cr.errors) {
                r.err += e + "\n";
            }
            return r;
        }
    }
    return r;
}

#ifndef _WIN32
// --zfs-pipe-local: typed same-host `zfs send ... | zfs recv ...`. Replaces the
// shell pipeline that used to be routed through --mutate-shell-generic. Payload is
// base64(JSON array) of exactly two elements, each base64(JSON array) holding one
// argv without the leading "zfs": entry 0 must start with "send", entry 1 with
// "recv". The daemon wires the pipe itself and execvp()s both ends, so no shell is
// involved and nothing but zfs send/recv can be expressed.
bool decodeZfsPipeLocal(const std::string& payloadB64, std::vector<std::string>& sendArgv,
                        std::vector<std::string>& recvArgv, std::string& errMsg) {
    sendArgv.clear();
    recvArgv.clear();
    std::string decoded;
    if (!decodeBase64(payloadB64, decoded)) {
        errMsg = "invalid pipe-local payload\n";
        return false;
    }
    std::vector<std::string> items;
    if (!parseJsonStringArray(decoded, items) || items.size() != 2) {
        errMsg = "invalid pipe-local payload\n";
        return false;
    }
    auto decodeOne = [&](const std::string& b64, const char* expectOp,
                         std::vector<std::string>& out) -> bool {
        std::string json;
        if (!decodeBase64(b64, json) || !parseJsonStringArray(json, out) || out.empty()) {
            errMsg = "invalid pipe-local entry\n";
            return false;
        }
        if (toLower(trim(out.front())) != expectOp) {
            errMsg = std::string("pipe-local entry must start with ") + expectOp + "\n";
            return false;
        }
        return true;
    };
    if (!decodeOne(items[0], "send", sendArgv)) {
        return false;
    }
    if (!decodeOne(items[1], "recv", recvArgv)) {
        return false;
    }
    return true;
}

// El encadenado en sí, ya con los dos argv resueltos.
//
// Está separado de la decodificación para que lo pueda usar quien YA tiene los argumentos y
// no una carga en base64: dentro del propio daemon, empaquetar para desempaquetar acto
// seguido es dar un rodeo. Lo usa la nivelación de GSA, que hasta ahora hacía este mismo
// encadenado con `std::system()` y una tubería de shell montada por concatenación.
ExecResult ejecutaTuberiaZfsLocal(const std::vector<std::string>& sendArgv,
                                  const std::vector<std::string>& recvArgv) {
    ExecResult r;

    int dataPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (pipe(dataPipe) != 0) {
        r.rc = 125;
        ponCodigo(r, "internal", {"pipe failed"});
        return r;
    }
    if (pipe(errPipe) != 0) {
        close(dataPipe[0]);
        close(dataPipe[1]);
        r.rc = 125;
        ponCodigo(r, "internal", {"pipe failed"});
        return r;
    }

    auto buildArgv = [](const std::vector<std::string>& v) {
        std::vector<char*> a;
        a.reserve(v.size() + 2);
        a.push_back(const_cast<char*>("zfs"));
        for (const std::string& s : v) {
            a.push_back(const_cast<char*>(s.c_str()));
        }
        a.push_back(nullptr);
        return a;
    };
    std::vector<char*> sendA = buildArgv(sendArgv);
    std::vector<char*> recvA = buildArgv(recvArgv);

    auto closeAll = [&]() {
        close(dataPipe[0]);
        close(dataPipe[1]);
        close(errPipe[0]);
        close(errPipe[1]);
    };

    const pid_t sendPid = fork();
    if (sendPid < 0) {
        closeAll();
        r.rc = 125;
        ponCodigo(r, "internal", {"fork failed"});
        return r;
    }
    if (sendPid == 0) {
        dup2(dataPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        closeAll();
        execvp("zfs", sendA.data());
        _exit(127);
    }

    const pid_t recvPid = fork();
    if (recvPid < 0) {
        closeAll();
        int st = 0;
        waitpid(sendPid, &st, 0);
        r.rc = 125;
        ponCodigo(r, "internal", {"fork failed"});
        return r;
    }
    if (recvPid == 0) {
        dup2(dataPipe[0], STDIN_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        closeAll();
        execvp("zfs", recvA.data());
        _exit(127);
    }

    // Parent must drop both data-pipe ends so recv sees EOF when send finishes.
    close(dataPipe[0]);
    close(dataPipe[1]);
    close(errPipe[1]);

    std::string errAccum;
    char buf[4096];
    while (true) {
        const ssize_t n = read(errPipe[0], buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        errAccum.append(buf, static_cast<std::size_t>(n));
    }
    close(errPipe[0]);

    int sendStatus = 0;
    int recvStatus = 0;
    waitpid(sendPid, &sendStatus, 0);
    waitpid(recvPid, &recvStatus, 0);
    const int sendRc = decodeWaitStatus(sendStatus);
    const int recvRc = decodeWaitStatus(recvStatus);

    r.err = errAccum;
    r.rc = (recvRc != 0) ? recvRc : sendRc;
    return r;
}

// El verbo: decodifica y delega. Lo único suyo es el formato de la carga.
ExecResult runZfsPipeLocalCapture(const std::string& payloadB64) {
    std::vector<std::string> sendArgv;
    std::vector<std::string> recvArgv;
    std::string errMsg;
    if (!decodeZfsPipeLocal(payloadB64, sendArgv, recvArgv, errMsg)) {
        ExecResult r;
        r.rc = 2;
        r.err = errMsg;
        return r;
    }
    return ejecutaTuberiaZfsLocal(sendArgv, recvArgv);
}
#else
// En Windows no existe la tubería tipada: se monta con `fork()` y `pipe()`, y
// `--zfs-pipe-local` no está entre los verbos que el agente de allí sirve.
//
// El hueco se rellena DICIÉNDOLO. Lo que había antes en el sitio que llama a esto era un
// `std::system()` con `sh -lc` dentro, que en Windows no podía funcionar —allí no hay
// `sh`— y devolvía un número de error sin explicar por qué: quien leyera el registro vería
// «exit 1» y no el motivo. Un motivo escrito vale más que un fallo silencioso.
ExecResult ejecutaTuberiaZfsLocal(const std::vector<std::string>&,
                                  const std::vector<std::string>&) {
    ExecResult r;
    r.rc = 2;
    ponCodigo(r, "local_pipe_windows", {});
    return r;
}
#endif  // _WIN32

}  // namespace zfsmgr::agent
