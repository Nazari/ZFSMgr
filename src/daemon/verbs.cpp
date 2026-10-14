// The verb table: ONE table for both doors of the agent, the RPC and the command line.

#include "agent_internal.h"

#include <algorithm>
#include <mutex>
#include <set>
#ifndef _WIN32
#include <grp.h>
#include <pwd.h>
#endif

namespace zfsmgr::agent {

const TablaDeVerbos& tablaDeVerbos() {
    static const TablaDeVerbos tabla = [] {
        TablaDeVerbos t;

        t.pon({"--health"}, [](const Peticion&) -> ExecResult {
            ExecResult r;
            r.rc = 0;
            r.out =
                "STATUS=OK\n"
                "VERSION=" ZFSMGR_AGENT_VERSION_STRING
                "\n"
                "API=3\n"
                "SERVER=1\n"
                "CACHE_ENTRIES=0\n"
                "CACHE_MAX_ENTRIES=0\n"
                "CACHE_INVALIDATIONS=0\n"
                "POOL_INVALIDATIONS=0\n"
                "RPC_FAILURES=0\n"
                "RPC_COMMANDS=\n"
                "ZED_ACTIVE=0\n";
            r.out += "CAPS=" + agentCapabilityList() + "\n";
            return r;
        });
        t.pon({"--dump-refresh-basics"},
              [](const Peticion&) -> ExecResult { return runDumpRefreshBasicsCapture(); });
        t.pon({"--dump-tool-availability"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --dump-tool-availability <payload-b64>"});
                return r;
            }
            return runDumpToolAvailabilityCapture(p.params[0]);
        });
        t.pon({"--dump-zfs-version"}, [](const Peticion&) -> ExecResult {
            ExecResult r;
            r = runExecCapture("zfs", {"version"});
            if (r.rc != 0 || trim(r.out).empty()) {
                r = runExecCapture("zfs", {"--version"});
            }
            if (r.rc != 0 || trim(r.out + "\n" + r.err).empty()) {
                r = runExecCapture("zpool", {"--version"});
            }
            return r;
        });
        t.pon({"--dump-zfs-mount"}, [](const Peticion& p) -> ExecResult {
            return runExecCapture("zfs", {"mount", "-j"});
        });
        t.pon({"--dump-block-devices"},
              [](const Peticion& p) -> ExecResult { return runDumpBlockDevicesCapture(); });
        t.pon({"--dump-zpool-list"}, [](const Peticion& p) -> ExecResult {
            return runExecCapture("zpool", {"list", "-j"});
        });
        t.pon({"--dump-zpool-import-probe"}, [](const Peticion&) -> ExecResult {
            ExecResult r;
            ExecResult a = runExecCapture("zpool", {"import"});
            ExecResult b = runExecCapture("zpool", {"import", "-s"});
            r.rc = b.rc;
            r.out = a.out + b.out;
            if (const std::string ilegible = discoIlegible(); !ilegible.empty()) {
                r.out += kMarcaDiscosIlegibles;
                r.out += kMarcaDiscoIlegible + ilegible + "\n";
                if (permisosCambiaronTrasArranque()) {
                    r.out += kMarcaTccTrasArranque;
                }
            }
            r.err = a.err + b.err;
            return r;
        });
        t.pon({"--dump-zpool-guid-status-batch"},
              [](const Peticion& p) -> ExecResult { return runDumpZpoolGuidStatusBatchCapture(); });
        t.pon({"--dump-resolve-devices"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            r.rc = 0;
            for (const std::string& n : p.params) {
                r.out += n + "=" + resolveDeviceName(n) + "\n";
            }
            return r;
        });
        t.pon({"--mutate-zpool-labelclear"},
              [](const Peticion& p) -> ExecResult { return runZpoolLabelclear(p.params); });
        t.pon({"--dump-zpool-guid"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage", {std::string(p.argv0) + " --dump-zpool-guid <pool>"});
                return r;
            }
            return runExecCapture("zpool", {"get", "-H", "-o", "value", "guid", p.params[0]});
        });
        t.pon({"--dump-zpool-status"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage", {std::string(p.argv0) + " --dump-zpool-status <pool>"});
                return r;
            }
            return runExecCapture("zpool", {"status", "-v", p.params[0]});
        });
        t.pon({"--dump-zpool-status-p"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage", {std::string(p.argv0) + " --dump-zpool-status-p <pool>"});
                return r;
            }
            return runExecCapture("zpool", {"status", "-P", p.params[0]});
        });
        // La DDT: cuántas entradas tiene y cuánta memoria cuesta.
        //
        // Es un verbo aparte y no un indicador del de arriba porque sirve para otra cosa: aquel
        // es el estado de los discos y este el coste de la deduplicación. Se mide: sobre una
        // DDT de 1,15 M de entradas tarda 0,03 s, lo mismo que `zpool status` a secas, porque
        // el histograma sale de contadores acumulados y no de recorrer la tabla.
        //
        // En Linux hay además `/proc/spl/kstat/zfs/<pool>/ddt_stats_*`, que no se usa: es solo
        // de Linux, y esto tiene que contestar igual en las cuatro plataformas.
        t.pon({"--dump-zpool-status-dedup"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage", {std::string(p.argv0) + " --dump-zpool-status-dedup <pool>"});
                return r;
            }
            return runExecCapture("zpool", {"status", "-D", p.params[0]});
        });
        t.pon({"--dump-zpool-history"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage", {std::string(p.argv0) + " --dump-zpool-history <pool>"});
                return r;
            }
            return runExecCapture("zpool", {"history", p.params[0]});
        });
        t.pon({"--dump-zpool-get-all"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage", {std::string(p.argv0) + " --dump-zpool-get-all <pool>"});
                return r;
            }
            return runExecCapture("zpool", {"get", "-j", "all", p.params[0]});
        });
        t.pon({"--dump-zfs-list-all"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage", {std::string(p.argv0) + " --dump-zfs-list-all <dataset>"});
                return r;
            }
            return conDispositivoDeVolumen(runExecCapture(
                "zfs",
                {"list", "-H", "-p", "-t", "filesystem,volume,snapshot", "-o",
                 // `type` AL FINAL, y por eso es compatible: el cliente corta por tabuladores y
                 // lee por índice, así que uno anterior a esto
                 // sigue leyendo las diez primeras y la ignora.
                 // Sin ella, un VOLUMEN no se distingue de un
                 // sistema de ficheros: el tipo se adivinaba
                 // mirando si el nombre llevaba «@», o sea que lo
                 // único que se distinguía era la instantánea.
                 "name,guid,used,compressratio,encryption,creation,referenced,mounted,mountpoint,"
                 "canmount,type",
                 "-r", p.params[0]}));
        });
        // --- El contenido de un dataset, TIPADO.
        //
        // Hasta ahora esto se hacía mandando `sh -lc '<guion>'` por SSH, con la ruta metida
        // dentro de una cadena de shell. Era el último camino de shell que quedaba: quien elige
        // la ruta no debe poder elegir además lo que se ejecuta.
        //
        // La ruta se valida contra los PUNTOS DE MONTAJE reales. El daemon corre como root, así
        // que sin eso «léeme un fichero» sería «léeme /etc/shadow». Estar bajo un punto de
        // montaje de ZFS es lo que separa «el contenido que la aplicación enseña» de «cualquier
        // cosa de la máquina».
        t.pon({"--dump-path-list", "--dump-path-info"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.empty()) {
                r.rc = 2;
                r.err = std::string("usage: ") + p.argv0 + " " + p.cmd + " <path>\n";
                return r;
            }
            return p.cmd == "--dump-path-list" ? listaRuta(p.params[0]) : describeRuta(p.params[0]);
        });
        t.pon({"--mutate-mount-ro-temp", "--mutate-unmount-ro-temp"},
              [](const Peticion& p) -> ExecResult {
                  ExecResult r;
                  if (p.params.empty()) {
                      r.rc = 2;
                      r.err = std::string("usage: ") + p.argv0 + " " + p.cmd
                              + (p.cmd == "--mutate-mount-ro-temp" ? " <dataset>\n" : " <dir>\n");
                      return r;
                  }
                  return p.cmd == "--mutate-mount-ro-temp"
                             ? montaSoloLecturaTemporal(p.params[0])
                             : desmontaSoloLecturaTemporal(p.params[0]);
              });
        t.pon({"--dump-dir-list", "--dump-file"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.empty()) {
                r.rc = 2;
                r.err = std::string("usage: ") + p.argv0 + " " + p.cmd + " <path>\n";
                return r;
            }
            if (!rutaDentroDeUnMountpoint(p.params[0])) {
                r.rc = 13;
                ponCodigo(r, "path_outside_zfs", {});
                return r;
            }
            if (p.cmd == "--dump-dir-list") {
                return listaDirectorio(p.params[0]);
            }
            const std::uintmax_t desde =
                p.params.size() > 1 ? std::strtoull(p.params[1].c_str(), nullptr, 10) : 0;
            const std::uintmax_t cuanto =
                p.params.size() > 2 ? std::strtoull(p.params[2].c_str(), nullptr, 10) : 0;
            return leeFichero(p.params[0], desde, cuanto);
        });
        // The machine's user or group names, one per line, sorted: who a permission can be
        // delegated to. Read from the system's account database, not from a shell: on Unix
        // getpwent/getgrent (which also see LDAP/NIS through NSS, as `getent` does), on
        // Windows the local accounts.
        t.pon({"--dump-accounts"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            const std::string clase = p.params.empty() ? std::string() : p.params[0];
            if (clase != "user" && clase != "group") {
                r.rc = 2;
                ponCodigo(r, "usage", {std::string(p.argv0) + " --dump-accounts <user|group>"});
                return r;
            }
            std::set<std::string> nombres;
#ifndef _WIN32
            // getpwent keeps its position in global state: one reader at a time.
            static std::mutex m;
            std::lock_guard<std::mutex> lock(m);
            if (clase == "user") {
                setpwent();
                while (const passwd* pw = getpwent()) {
                    if (pw->pw_name && *pw->pw_name) {
                        nombres.insert(pw->pw_name);
                    }
                }
                endpwent();
            } else {
                setgrent();
                while (const group* gr = getgrent()) {
                    if (gr->gr_name && *gr->gr_name) {
                        nombres.insert(gr->gr_name);
                    }
                }
                endgrent();
            }
#else
            // The agent runs as a scheduled task, where powershell is not on the PATH.
            const std::string guion = clase == "user"
                                          ? "Get-LocalUser | ForEach-Object { $_.Name }"
                                          : "Get-LocalGroup | ForEach-Object { $_.Name }";
            std::vector<std::string> candidatos;
            if (const char* raiz = std::getenv("SystemRoot")) {
                candidatos.push_back(std::string(raiz)
                                     + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
            }
            candidatos.push_back("C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
            candidatos.push_back("powershell");
            ExecResult ps;
            ps.rc = 1;
            for (const std::string& exe : candidatos) {
                ps = runExecCapture(exe, {"-NoProfile", "-NonInteractive", "-Command", guion});
                if (ps.rc == 0) {
                    break;
                }
            }
            if (ps.rc != 0) {
                return ps;
            }
            for (const std::string& l : splitLines(ps.out)) {
                if (!trim(l).empty()) {
                    nombres.insert(trim(l));
                }
            }
#endif
            for (const std::string& n : nombres) {
                r.out += n + "\n";
            }
            r.rc = 0;
            return r;
        });
        // `-s createtxg` and not `creation`: two snapshots of the same second keep their order.
        t.pon({"--dump-zfs-snapshot-guids"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --dump-zfs-snapshot-guids <dataset>"});
                return r;
            }
            return runExecCapture("zfs", {"list", "-H", "-p", "-t", "snapshot", "-o", "name,guid",
                                          "-s", "createtxg", "-d", "1", p.params[0]});
        });
        t.pon({"--dump-zfs-guid-map"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage", {std::string(p.argv0) + " --dump-zfs-guid-map <dataset>"});
                return r;
            }
            return runExecCapture("zfs",
                                  {"get", "-H", "-o", "name,value", "guid", "-r", p.params[0]});
        });
        // Las letras de unidad de un pool, CON su origen.
        //
        // El origen hace falta para distinguir la letra PUESTA en un dataset de la HEREDADA del
        // pool: en Windows los descendientes heredan la del pool y se montan planos bajo esa
        // unidad, así que dos datasets con la misma letra heredada es el funcionamiento normal.
        // Sin el origen, cualquier pool con más de un dataset parecía tener letras duplicadas, y
        // ese aviso llenaba el registro en cada refresco tapando los de verdad.
        //
        // El verbo existe en las dos plataformas aunque solo Windows tenga letras. Fuera de
        // Windows `zfs` contesta que la propiedad no existe —en macOS, «invalid property
        // 'driveletter'», comprobado— con código distinto de cero, y el cliente lo lee como «no
        // hay letras». Un verbo que solo existiera en una plataforma obligaría a quien llama a
        // saber en cuál está, y eso es justo lo que se está quitando de la aplicación.
        t.pon({"--dump-zfs-driveletters"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage", {std::string(p.argv0) + " --dump-zfs-driveletters <pool>"});
                return r;
            }
            return runExecCapture(
                "zfs", {"get", "-H", "-o", "name,value,source", "-r", "driveletter", p.params[0]});
        });
        t.pon({"--dump-zfs-list-children"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --dump-zfs-list-children <dataset>"});
                return r;
            }
            return runExecCapture("zfs", {"list", "-H", "-o", "name", "-r", p.params[0]});
        });
        t.pon({"--dump-advanced-breakdown-list"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(
                    r, "usage",
                    {std::string(p.argv0) + " --dump-advanced-breakdown-list <dataset> [<root>]"});
                return r;
            }
            return runDumpAdvancedBreakdownListCapture(
                p.params[0], p.params.size() > 1 ? p.params[1] : std::string());
        });
        t.pon({"--dump-zfs-get-prop"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 2) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --dump-zfs-get-prop <prop> <dataset>"});
                return r;
            }
            return runExecCapture("zfs", {"get", "-H", "-o", "value", p.params[0], p.params[1]});
        });
        t.pon({"--dump-zfs-get-all"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage", {std::string(p.argv0) + " --dump-zfs-get-all <dataset>"});
                return r;
            }
            return runExecCapture("zfs", {"get", "-j", "all", p.params[0]});
        });
        t.pon({"--dump-zfs-get-json"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 2) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --dump-zfs-get-json <props> <dataset>"});
                return r;
            }
            return runExecCapture("zfs", {"get", "-j", p.params[0], p.params[1]});
        });
        t.pon({"--dump-zfs-diff"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 2) {
                r.rc = 2;
                ponCodigo(r, "usage", {std::string(p.argv0) + " --dump-zfs-diff <snap1> <snap2>"});
                return r;
            }
            // -H: salida tabulada de verdad. Sin él, zfs imprime los renombrados como
            // "viejo -> nuevo" en un solo campo, y el parseo de la GUI —escrito para
            // campos separados por tabulador— se quedaba sin la ruta anterior.
            return runExecCapture("zfs", {"diff", "-H", p.params[0], p.params[1]});
        });
        t.pon({"--dump-zfs-allow"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage", {std::string(p.argv0) + " --dump-zfs-allow <dataset>"});
                return r;
            }
            return runExecCapture("zfs", {"allow", p.params[0]});
        });
        // Los HOLDS de una instantánea: qué retenciones tiene puestas.
        //
        // La interfaz los lee mandando `zfs holds -H` por shell —de lo poco que quedaba—. Aquí
        // es un verbo tipado como los demás.
        //
        // Admite VARIAS instantáneas en una llamada, y no un dataset.
        //
        // `zfs holds` solo acepta instantáneas —comprobado: con un dataset contesta «is not a
        // snapshot», y `-r` tampoco lo cambia— así que pedir «los holds de este dataset» no
        // existe como orden. Pero sí acepta una lista, y el cliente ya tiene los nombres de las
        // instantáneas del listado del árbol: una llamada por dataset en vez de una por
        // instantánea.
        t.pon({"--dump-zfs-holds"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.empty()) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --dump-zfs-holds <snapshot> [snapshot...]"});
                return r;
            }
            std::vector<std::string> a = {"holds", "-H"};
            for (const auto& sn : p.params) {
                a.push_back(sn);
            }
            return runExecCapture("zfs", a);
        });
        // Poner una retención. `zfs hold <tag> <snapshot>`: la etiqueta primero, igual que
        // release. Verbo propio y no `--mutate-zfs-generic` por lo mismo que el otro: así el
        // daemon ve los dos argumentos por separado y el registro se lee.
        t.pon({"--mutate-zfs-hold"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 2) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --mutate-zfs-hold <tag> <snapshot>"});
                return r;
            }
            return runExecCapture("zfs", {"hold", p.params[0], p.params[1]});
        });
        // Y soltar uno. `zfs release <tag> <snapshot>`: la etiqueta primero.
        //
        // No pasa por `--mutate-zfs-generic` aunque «release» esté en su lista: así el daemon
        // ve los DOS argumentos por separado y no una carga en base64 que hay que decodificar
        // para saber sobre qué actúa. Un verbo propio se lee en el registro.
        t.pon({"--mutate-zfs-release"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 2) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --mutate-zfs-release <tag> <snapshot>"});
                return r;
            }
            return runExecCapture("zfs", {"release", p.params[0], p.params[1]});
        });

        // Mover un dataset de sitio DENTRO de su pool.
        //
        // Es `zfs rename` a secas, no una copia: el dataset cambia de sitio en el árbol y los
        // datos no se mueven, así que es instantáneo y no hay nada que destruir después. Lo
        // que la interfaz llama «Mover aquí» es exactamente esto.
        //
        // Verbo propio y no `--mutate-zfs-generic` por lo mismo que hold y release: así el
        // daemon ve los dos nombres por separado y el registro dice qué se movió y a dónde,
        // en vez de una carga en base64 que hay que descifrar para saberlo.
        t.pon({"--mutate-zfs-rename"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 2) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --mutate-zfs-rename <source> <target> [r]"});
                return r;
            }
            // Tercer parámetro opcional «r»: `rename -r`, que con un snapshot renombra el mismo en
            // todos los descendientes. Lo usa la nivelación de una programación recursiva.
            if (p.params.size() > 2 && p.params[2] == "r") {
                return runExecCapture("zfs", {"rename", "-r", p.params[0], p.params[1]});
            }
            return runExecCapture("zfs", {"rename", p.params[0], p.params[1]});
        });
        t.pon({"--mutate-gsa-run-once"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.empty()) {
                r.rc = 2;
                ponCodigo(r, "usage", {std::string(p.argv0) + " --mutate-gsa-run-once <dataset>"});
                return r;
            }
            return runGsaRunOnceCapture(p.params[0]);
        });
        t.pon({"--dump-gsa-status"},
              [](const Peticion&) -> ExecResult { return runDumpGsaStatusCapture(); });

        // Descartar una recepción a medias. `zfs recv -A` tira el estado suspenso y su
        // testigo; a partir de ahí el destino vuelve a aceptar un flujo nuevo.
        t.pon({"--mutate-zfs-recv-abort"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --mutate-zfs-recv-abort <dataset>"});
                return r;
            }
            return runExecCapture("zfs", {"recv", "-A", p.params[0]});
        });

        t.pon({"--dump-zfs-allow-batch"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.empty()) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --dump-zfs-allow-batch <ds1> [ds2...]"});
                return r;
            }
            r.rc = 0;
            for (const auto& ds : p.params) {
                r.out += "__ZFSMGR_ALLOW_BEGIN__ " + ds + "\n";
                ExecResult res = runExecCapture("zfs", {"allow", ds});
                r.out += res.out;
                if (!res.err.empty())
                    r.out += res.err;
                r.out += "__ZFSMGR_ALLOW_RC__ " + ds + " " + std::to_string(res.rc) + "\n";
                r.out += "__ZFSMGR_ALLOW_END__ " + ds + "\n";
                if (res.rc != 0)
                    r.rc = res.rc;
            }
            return r;
        });
        t.pon({"--dump-zfs-get-gsa-raw-all-pools"},
              [](const Peticion& p) -> ExecResult { return runDumpGsaRawAllPoolsCapture(); });
        t.pon({"--dump-zfs-get-gsa-raw-recursive"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --dump-zfs-get-gsa-raw-recursive <dataset>"});
                return r;
            }
            return runExecCapture(
                "zfs", {"get", "-H", "-o", "name,property,value,source", "-r",
                        "org.zfsmgr:schedule-enabled,org.zfsmgr:schedule-recursive,org.zfsmgr:"
                        "schedule-hourly,org.zfsmgr:schedule-daily,org.zfsmgr:schedule-weekly,org."
                        "zfsmgr:schedule-monthly,org.zfsmgr:schedule-yearly,org.zfsmgr:schedule-"
                        "level,org.zfsmgr:schedule-target",
                        p.params[0]});
        });

        t.pon({"--mutate-zfs-snapshot"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 2) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --mutate-zfs-snapshot <target> <recursive>"});
                return r;
            }
            const bool recursive = (p.params[1] == "1" || toLower(p.params[1]) == "true"
                                    || toLower(p.params[1]) == "on");
#ifdef HAVE_LIBZFS_CORE
            if (!recursive) {
                ExecResult lr = lzcSnapshotOne(p.params[0]);
                if (lr.rc == 0)
                    return lr;
            }
#endif
            std::vector<std::string> a = {"snapshot"};
            if (recursive)
                a.push_back("-r");
            a.push_back(p.params[0]);
            return runExecCapture("zfs", a);
        });
        t.pon({"--mutate-zfs-destroy"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 3) {
                r.rc = 2;
                ponCodigo(
                    r, "usage",
                    {std::string(p.argv0) + " --mutate-zfs-destroy <target> <force> <scope>"});
                return r;
            }
            const bool force = (p.params[1] == "1" || toLower(p.params[1]) == "true"
                                || toLower(p.params[1]) == "on");
            const std::string& scope = p.params[2];
            const bool isSnap = p.params[0].find('@') != std::string::npos;
            const bool noScope = (scope != "R" && scope != "r");
#ifdef HAVE_LIBZFS_CORE
            // lzc_destroy_snaps handles single snapshots without scope/force cleanly
            if (isSnap && noScope && !force) {
                ExecResult lr = lzcDestroyOneSnap(p.params[0]);
                if (lr.rc == 0)
                    return lr;
            }
#endif
            std::vector<std::string> a = {"destroy"};
            if (force)
                a.push_back("-f");
            if (scope == "R")
                a.push_back("-R");
            else if (scope == "r")
                a.push_back("-r");
            a.push_back(p.params[0]);
            return runExecCapture("zfs", a);
        });
        t.pon({"--mutate-zfs-rollback"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 3) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --mutate-zfs-rollback <snap> <force> <scope>"});
                return r;
            }
            const bool force = (p.params[1] == "1" || toLower(p.params[1]) == "true"
                                || toLower(p.params[1]) == "on");
            const std::string& scope = p.params[2];
            const bool noScope = (scope != "R" && scope != "r");
#ifdef HAVE_LIBZFS_CORE
            if (!force && noScope) {
                const auto atPos = p.params[0].find('@');
                if (atPos != std::string::npos) {
                    ExecResult lr = lzcRollbackTo(p.params[0].substr(0, atPos), p.params[0]);
                    if (lr.rc == 0)
                        return lr;
                }
            }
#endif
            std::vector<std::string> a = {"rollback"};
            if (force)
                a.push_back("-f");
            if (scope == "R")
                a.push_back("-R");
            else if (scope == "r")
                a.push_back("-r");
            a.push_back(p.params[0]);
            return runExecCapture("zfs", a);
        });
        t.pon({"--dump-zfs-exists"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage", {std::string(p.argv0) + " --dump-zfs-exists <dataset>"});
                return r;
            }
#ifdef HAVE_LIBZFS_CORE
            const bool exists = lzcExists(p.params[0]);
            r.rc = exists ? 0 : 1;
            r.out = exists ? "EXISTS=yes\n" : "EXISTS=no\n";
            return r;
#endif
            ExecResult sub = runExecCapture("zfs", {"list", "-H", "-o", "name", p.params[0]});
            r.rc = (sub.rc == 0) ? 0 : 1;
            r.out = (sub.rc == 0) ? "EXISTS=yes\n" : "EXISTS=no\n";
            return r;
        });
        t.pon({"--mutate-zfs-clone"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 2) {
                r.rc = 2;
                ponCodigo(
                    r, "usage",
                    {std::string(p.argv0) + " --mutate-zfs-clone <origin-snap> <new-dataset>"});
                return r;
            }
#ifdef HAVE_LIBZFS_CORE
            ExecResult lr = lzcClone(p.params[0], p.params[1]);
            if (lr.rc == 0)
                return lr;
#endif
            return runExecCapture("zfs", {"clone", p.params[0], p.params[1]});
        });
        t.pon({"--mutate-zfs-create"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
#ifndef _WIN32
            // La frase de cifrado viaja DENTRO de la carga —cifrada por mTLS— y se le pasa
            // a `zfs` por una tubería, no como argumento. Antes esto era un script de shell
            // (`printf '%s\\n%s\\n' 'FRASE' 'FRASE' | zfs create ...`) enviado por SSH: la
            // frase quedaba en argv, visible en `ps` de las dos máquinas, y ningún patrón de
            // enmascarado la tapaba ni en el registro ni en la confirmación.
            if (p.params.size() < 2) {
                r.rc = 2;
                r.err = std::string("usage: ") + p.argv0
                        + " --mutate-zfs-create <b64-json-argv> <b64-passphrase>\n";
                return r;
            }
            std::string argvJson;
            std::string passphrase;
            if (!decodeBase64(p.params[0], argvJson) || !decodeBase64(p.params[1], passphrase)) {
                ponFallo(r, 2, "internal", {"invalid base64 argument"});
                return r;
            }
            std::vector<std::string> zfsArgs;
            if (!parseJsonStringArray(argvJson, zfsArgs) || zfsArgs.empty()) {
                ponFallo(r, 2, "internal", {"invalid argv payload"});
                return r;
            }
            // El primer elemento decide la operación: solo create y snapshot, para que esto
            // no acabe siendo otro `zfs` genérico con la puerta abierta.
            const std::string op = toLower(trim(zfsArgs.front()));
            if (op != "create" && op != "snapshot") {
                r.rc = 2;
                ponCodigo(r, "internal",
                          {"unsupported op for --mutate-zfs-create: " + zfsArgs.front()});
                return r;
            }
            ExecResult cr = passphrase.empty()
                                ? runExecCapture("zfs", zfsArgs)
                                // `zfs create -o keyformat=passphrase` la pide dos veces.
                                : runExecCaptureWithStdin("zfs", zfsArgs,
                                                          passphrase + "\n" + passphrase + "\n");
            if (cr.rc != 0 || op != "create") {
                return cr;
            }
            // Decir DÓNDE quedó montado. Un dataset creado bajo un pool con mountpoint=none
            // lo hereda y no se puede montar: "Desde Dir" lo creaba, la tubería de tar no
            // tenía dónde escribir y se quedaba colgada moviendo cero bytes sin explicar
            // nada. Con esto el cliente puede plantarse antes de arrancar la copia.
            const std::string created = trim(zfsArgs.back());
            // Unless the caller said how it mounts: `-u` (do not mount) or its own canmount.
            bool dichoMontaje = false;
            for (const std::string& a : zfsArgs) {
                dichoMontaje = dichoMontaje || a == "-u" || a.rfind("canmount=", 0) == 0;
            }
            if (!dichoMontaje) {
                (void)runExecCapture("zfs", {"set", "canmount=on", created});
                (void)runExecCapture("zfs", {"mount", created});
            }
            const ExecResult mp = getDatasetMountpointCapture(created);
            cr.out += "MOUNTPOINT=" + ((mp.rc == 0) ? trim(mp.out) : std::string()) + "\n";
            return cr;
#else
        ponFallo(r, 2, "not_on_windows", {}); return r;
#endif
        });
        t.pon({"--mutate-zfs-load-key"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 2) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0)
                           + " --mutate-zfs-load-key <dataset-b64> <passphrase-b64>"});
                return r;
            }
            std::string dataset, passphrase;
            if (!decodeBase64(p.params[0], dataset) || !decodeBase64(p.params[1], passphrase)) {
                ponFallo(r, 2, "internal", {"invalid base64 argument"});
                return r;
            }
            dataset = trim(dataset);
            if (dataset.empty()) {
                r.rc = 2;
                ponCodigo(r, "empty_dataset", {});
                return r;
            }
            return runExecCaptureWithStdin("zfs", {"load-key", dataset}, passphrase + "\n");
        });
        // Las credenciales de los pares, que las escribe el cliente.
        //
        // Llega en base64 por lo mismo que la frase de paso: dentro van CLAVES PRIVADAS, y un
        // argumento se ve en `ps`. El fichero queda 600 y de root, igual que el material TLS
        // de al lado; con permisos más abiertos, cualquier usuario de la máquina podría
        // hacerse pasar por ella ante las demás.
        t.pon({"--mutate-set-peers"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage", {std::string(p.argv0) + " --mutate-set-peers <json-b64>"});
                return r;
            }
            std::string contenido;
            if (!decodeBase64(p.params[0], contenido)) {
                ponFallo(r, 2, "internal", {"invalid base64 argument"});
                return r;
            }
            zfsmgr::base::json::Value comprobacion;
            std::string errJson;
            if (!zfsmgr::base::json::parse(contenido, comprobacion, &errJson)) {
                ponFallo(r, 2, "peers_bad_json", {errJson});
                return r;
            }
            const std::string tmp = std::string(kPeersPath) + ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f.is_open()) {
                    ponFallo(r, 1, "cannot_write", {tmp});
                    return r;
                }
                f << contenido;
            }
#ifndef _WIN32
            ::chmod(tmp.c_str(), 0600);
#endif
            std::error_code ec;
            std::filesystem::rename(tmp, kPeersPath, ec);
            if (ec) {
                std::filesystem::remove(tmp, ec);
                ponFallo(r, 1, "cannot_put_in_place", {kPeersPath});
                return r;
            }
            r.rc = 0;
            r.out = std::string("PEERS=") + kPeersPath + "\n";
            daemonLog("INFO", "peers updated by the client");
            return r;
        });
        // En qué dirección escucha el daemon. Hace falta para que otro daemon pueda pedirle que
        // reciba una nivelación: por omisión solo atiende en 127.0.0.1, y ahí solo llega el
        // cliente porque abre un túnel SSH — de madrugada no hay quien lo abra.
        //
        // Solo se admite un COMODÍN. La aplicación llega al daemon por un túnel contra
        // 127.0.0.1, así que atarlo a una sola dirección de la LAN le cortaría el acceso a la
        // máquina entera; y este servidor abre un socket, no una lista.
        //
        // El puerto queda entonces alcanzable desde la red, protegido por mTLS con el
        // certificado de cliente FIJADO. No es una categoría nueva de exposición: el puerto de
        // transfer ya escucha en 0.0.0.0 con un testigo de un solo uso.
        t.pon({"--mutate-set-bind"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --mutate-set-bind <0.0.0.0|::|127.0.0.1>"});
                return r;
            }
            const std::string dir = trim(p.params[0]);
            if (dir != "0.0.0.0" && dir != "::" && dir != "127.0.0.1") {
                r.rc = 2;
                ponCodigo(r, "bind_not_allowed", {});
                return r;
            }
            std::ifstream in(kDefaultAgentConfigPath);
            if (!in.is_open()) {
                r.rc = 1;
                ponCodigo(r, "cannot_read", {kDefaultAgentConfigPath});
                return r;
            }
            std::vector<std::string> lineas;
            std::string l;
            bool puesta = false;
            while (std::getline(in, l)) {
                if (l.rfind("AGENT_BIND=", 0) == 0) {
                    lineas.push_back("AGENT_BIND='" + dir + "'");
                    puesta = true;
                } else {
                    lineas.push_back(l);
                }
            }
            in.close();
            if (!puesta) {
                lineas.push_back("AGENT_BIND='" + dir + "'");
            }
            const std::string tmp = std::string(kDefaultAgentConfigPath) + ".tmp";
            {
                std::ofstream out(tmp, std::ios::trunc);
                if (!out.is_open()) {
                    ponFallo(r, 1, "cannot_write", {tmp});
                    return r;
                }
                for (const std::string& x : lineas) {
                    out << x << "\n";
                }
            }
#ifndef _WIN32
            ::chmod(tmp.c_str(), 0600);
#endif
            std::error_code ec;
            std::filesystem::rename(tmp, kDefaultAgentConfigPath, ec);
            if (ec) {
                std::filesystem::remove(tmp, ec);
                r.rc = 1;
                ponCodigo(r, "cannot_put_in_place", {kDefaultAgentConfigPath});
                return r;
            }
            daemonLog("INFO", "AGENT_BIND changed to " + dir + "; restarting to apply it");
            r.rc = 0;
            r.out = "BIND=" + dir + "\nRESTARTING=1\n";
            // Se responde ANTES de salir, y se sale en otro hilo: el gestor de servicios lo
            // levanta —`Restart=always`— con la dirección nueva. Salir aquí mismo dejaría al
            // cliente sin respuesta y con cara de que ha fallado.
            std::thread([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
                g_stop.store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                std::_Exit(0);
            }).detach();
            return r;
        });
        t.pon({"--dump-peers"}, [](const Peticion&) -> ExecResult {
            ExecResult r;
            // SOLO los nombres y a donde apuntan: los certificados y la clave privada no salen
            // de la maquina ni para mirarlos.
            std::ifstream f(kPeersPath);
            if (!f.is_open()) {
                r.rc = 0;
                return r;
            }
            std::ostringstream ss;
            ss << f.rdbuf();
            zfsmgr::base::json::Value raiz;
            std::string errJson;
            if (!zfsmgr::base::json::parse(ss.str(), raiz, &errJson)) {
                ponFallo(r, 1, "peers_bad_json", {errJson});
                return r;
            }
            std::ostringstream out;
            // Con quién se identifica esta máquina, PRIMERO y en su propia línea.
            //
            // Antes no salía, y su ausencia es el fallo silencioso de este fichero: sin `self`,
            // una nivelación GSA contra un dataset de esta misma máquina no reconoce el destino
            // como propio, se va por el camino remoto y registra «no hay credenciales del par»
            // —siendo el par uno mismo—. La rama local no se ejecuta jamás.
            //
            // Sin esta línea no había forma de diagnosticarlo desde fuera: el cliente listaba los
            // pares y todo parecía correcto. Ahora se ve, y se ve vacío cuando falta.
            out << "SELF\t" << trim(raiz["self"].toString()) << "\n";
            // El uid que se le entregó Y el que la máquina lee de sí misma. Los dos, porque lo
            // que importa es si COINCIDEN: si no, esta configuración se escribió mirando a otra
            // máquina —un perfil copiado de otro equipo— y hasta ahora eso no se veía.
            out << "SELF_UID\t" << trim(raiz["self_uid"].toString()) << "\n";
            out << "REAL_UID\t" << trim(detectMachineUuid()) << "\n";
            for (const auto& v : raiz["peers"].toArray()) {
                out << trim(v["id"].toString()) << "\t" << trim(v["host"].toString()) << "\t"
                    << v["port"].toInt(0) << "\t" << trim(v["uid"].toString()) << "\n";
            }
            r.rc = 0;
            r.out = out.str();
            return r;
        });
        t.pon({"--mutate-zfs-change-key"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 3) {
                r.rc = 2;
                ponCodigo(
                    r, "usage",
                    {std::string(p.argv0)
                     + " --mutate-zfs-change-key <dataset-b64> <passphrase-b64> <flags-b64>"});
                return r;
            }
            std::string dataset, passphrase, flagsStr;
            if (!decodeBase64(p.params[0], dataset) || !decodeBase64(p.params[1], passphrase)
                || !decodeBase64(p.params[2], flagsStr)) {
                ponFallo(r, 2, "internal", {"invalid base64 argument"});
                return r;
            }
            dataset = trim(dataset);
            flagsStr = trim(flagsStr);
            if (dataset.empty()) {
                r.rc = 2;
                ponCodigo(r, "empty_dataset", {});
                return r;
            }
            std::vector<std::string> changeArgs = {"change-key"};
            {
                std::istringstream iss(flagsStr);
                std::string tok;
                while (iss >> tok)
                    changeArgs.push_back(tok);
            }
            changeArgs.push_back(dataset);

            {
                const std::string motivo = motivoParaNoCambiarClave(dataset, flagsStr);
                if (!motivo.empty()) {
                    ExecResult neg;
                    neg.rc = 2;
                    neg.err = motivo;
                    return neg;
                }
            }
            return runExecCaptureWithStdin("zfs", changeArgs,
                                           passphrase + "\n" + passphrase + "\n");
        });
        t.pon({"--mutate-zfs-generic"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --mutate-zfs-generic <payload-b64>"});
                return r;
            }
            return runGenericMutationCapture("zfs", p.params[0]);
        });
        t.pon({"--mutate-advanced-breakdown"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 3) {
                r.rc = 2;
                r.err = std::string("usage: ") + p.argv0
                    + " --mutate-advanced-breakdown <dataset> <dir> <nombre> "
                      "[<dir> <nombre>...]\n";
                return r;
            }
            return runMutateAdvancedBreakdownCapture(p.params);
        });
        t.pon({"--mutate-advanced-assemble"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 2) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0)
                           + " --mutate-advanced-assemble <dataset> <child> [child...]"});
                return r;
            }
            return runMutateAdvancedAssembleCapture(p.params);
        });
#ifndef _WIN32
        // todir usa AltMountGuard, que solo existe en Unix. En Windows la GUI no pasa
        // por el agente para esta acción: usa su propio script de PowerShell.
        t.pon({"--mutate-advanced-todir"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 3) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0)
                           + " --mutate-advanced-todir <dataset> <dst-dir> <delete-source-0|1>"});
                return r;
            }
            return runMutateAdvancedToDirCapture(p.params);
        });
#endif
        t.pon({"--mutate-zpool-generic"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --mutate-zpool-generic <payload-b64>"});
                return r;
            }
            return runGenericMutationCapture("zpool", p.params[0]);
        });
        t.pon({"--mutate-zfs-allow-batch"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --mutate-zfs-allow-batch <payload-b64>"});
                return r;
            }
            return runZfsAllowBatchCapture(p.params[0]);
        });
        t.pon({"--mutate-copy-local"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --mutate-copy-local <payload-b64>"});
                return r;
            }
            return runCopyLocalCapture(p.params[0]);
        });
        // --mutate-shell-generic is deliberately NOT served over RPC: it runs an arbitrary
        // shell as root, so exposing it here would let any client holding the mTLS
        // certificate bypass every typed --mutate-* whitelist. The GUI only ever invokes
        // it as a CLI command over SSH+sudo (see daemonizeShellMutationArgs), where the
        // caller already has root and it grants no extra privilege. The CLI handler in
        // main() stays for those flows.

        // Copia de árboles sin rsync, en las dos plataformas.
        //
        // Los DOS verbos van juntos a propósito: la verificación no es un extra sino la red
        // que sostiene Desglosar, Ensamblar y Hacia Dir, que borran el origen. Exponer la
        // copia sin con qué comprobarla sería exponer justo la mitad peligrosa.
        t.pon({"--mutate-copy-tree", "--dump-copy-tree-pending"},
              [](const Peticion& p) -> ExecResult {
                  ExecResult r;
                  if (p.params.size() < 2) {
                      r.rc = 2;
                      r.err = std::string("usage: ") + p.argv0 + " " + p.cmd
                    + " <src> <dst> [--one-file-system] [--delete] [--dry-run]"
                      " [--no-skip-unchanged] [--exclude=<name>]...\n";
                      return r;
                  }
                  zfsmgr::copytree::Options opt;
                  for (std::size_t i = 2; i < p.params.size(); ++i) {
                      const std::string& a = p.params[i];
                      if (a == "--one-file-system") {
                          opt.oneFileSystem = true;
                      } else if (a == "--delete") {
                          // Borra del destino lo que no está en el origen. Solo para sincronizar.
                          opt.deleteExtraneous = true;
                      } else if (a == "--dry-run") {
                          opt.dryRun = true;
                      } else if (a == "--no-skip-unchanged") {
                          // Rehacer la copia entera aunque ya coincida. Por omisión se salta lo
                          // igual, que es lo que hace rsync.
                          opt.skipUnchanged = false;
                      } else if (a.rfind("--exclude=", 0) == 0) {
                          const std::string name = trim(a.substr(10));
                          if (!name.empty()) {
                              opt.excludes.push_back(name);
                          }
                      } else {
                          ponFallo(r, 2, "internal", {"unknown argument: " + a});
                          return r;
                      }
                  }
                  if (p.cmd == "--dump-copy-tree-pending") {
                      const long long pending =
                          zfsmgr::copytree::countPending(p.params[0], p.params[1], opt);
                      if (pending < 0) {
                          // NO se responde 0: el llamante borra el origen cuando ve un cero, así
                          // que «no se pudo comprobar» tiene que llegar como fallo.
                          ponFallo(r, 1, "copy_verify_failed", {});
                          return r;
                      }
                      r.rc = 0;
                      r.out = "PENDING=" + std::to_string(pending) + "\n";
                      return r;
                  }
                  const zfsmgr::copytree::Result cr =
                      zfsmgr::copytree::copyTree(p.params[0], p.params[1], opt);
                  if (!cr.ok) {
                      r.rc = 1;
                      r.err = cr.error + "\n";
                      return r;
                  }
                  r.rc = 0;
                  r.out = "FILES=" + std::to_string(cr.filesCopied)
                          + "\nDIRS=" + std::to_string(cr.dirsCreated)
                          + "\nSYMLINKS=" + std::to_string(cr.symlinksCopied)
                          + "\nHARDLINKS=" + std::to_string(cr.hardLinksRecreated)
                          + "\nSKIPPED=" + std::to_string(cr.filesSkipped)
                          + "\nDELETED=" + std::to_string(cr.entriesDeleted)
                          + "\nBYTES=" + std::to_string(cr.bytesWritten) + "\n";
                  return r;
              });

        // Recibir y emitir funcionan ya en Windows (fases 1 y 2): el daemon bombea entre el
        // socket y una tubería propia, en vez de entregarle el descriptor del socket a `zfs`.
        t.pon({"--zfs-send-to-peer"},
              [](const Peticion& p) -> ExecResult { return runZfsSendToPeerCapture(p.params); });
        // El árbol de ficheros por el mismo transporte que el flujo de ZFS. Es lo que le da a
        // Windows una sincronización con borrado y con pasada en seco, que por el respaldo de
        // tar no tenía. Ver el bloque grande de arriba.
        // Va por RPC —al contrario que `--mutate-advanced-fromdir`, que es solo de terminal—
        // porque no lee nada por la entrada estándar: prepara el destino y dice dónde quedó.
        t.pon({"--mutate-advanced-fromdir-prepare"}, [](const Peticion& p) -> ExecResult {
            return runMutateAdvancedFromDirPrepareCapture(p.params);
        });
        t.pon({"--mutate-prepare-dir"},
              [](const Peticion& p) -> ExecResult { return runPrepareDirCapture(p.params); });
        t.pon({"--mutate-remove-content"},
              [](const Peticion& p) -> ExecResult { return runRemoveContentCapture(p.params); });
        t.pon({"--tree-recv-listen"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.empty()) {
                r.rc = 2;
                ponCodigo(r, "usage", {std::string(p.argv0) + " --tree-recv-listen <dir>"});
                return r;
            }
            return runTreeRecvListenCapture(p.params[0]);
        });
        t.pon({"--tree-send-to-peer"},
              [](const Peticion& p) -> ExecResult { return runTreeSendToPeerCapture(p.params); });

        t.pon({"--zfs-recv-listen"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            // p.params: dataset [force=0|1]
            if (p.params.empty()) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0) + " --zfs-recv-listen <dataset> [force=1]"});
                return r;
            }
            const bool force =
                p.params.size() > 1 && (p.params[1] == "1" || toLower(p.params[1]) == "true");
            return runZfsRecvListenCapture(p.params[0], force);
        });
        t.pon({"--file-recv-listen"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            // p.params: fichero [sobrescribir=0|1 [compresión]]
            if (p.params.empty()) {
                r.rc = 2;
                ponCodigo(r, "usage",
                          {std::string(p.argv0)
                           + " --file-recv-listen <file> [overwrite=1 [gzip|lz4|zstd[:level]]]"});
                return r;
            }
            const bool sobrescribir =
                p.params.size() > 1 && (p.params[1] == "1" || toLower(p.params[1]) == "true");
            return runFileRecvListenCapture(p.params[0], sobrescribir, false,
                                            p.params.size() > 2 ? p.params[2] : std::string());
        });

#ifndef _WIN32
        t.pon({"--zfs-pipe-local"}, [](const Peticion& p) -> ExecResult {
            ExecResult r;
            if (p.params.size() < 1) {
                r.rc = 2;
                ponCodigo(r, "usage", {std::string(p.argv0) + " --zfs-pipe-local <payload-b64>"});
                return r;
            }
            return runZfsPipeLocalCapture(p.params[0]);
        });
        t.pon({"--repair-alt-mountpoints"}, [](const Peticion& p) -> ExecResult {
            // Fully typed and argument-free apart from the apply switch: it only ever acts
            // on datasets carrying our own savedmountpoint property, so it is safe to
            // serve over RPC.
            const bool apply = !p.params.empty() && toLower(trim(p.params[0])) == "apply";
            return runRepairAltMountpointsCapture(apply);
        });
#endif
        for (const std::string& v : zfsmgr::base::transport::agentVerbsCarryingSecrets()) {
            t.soloPorRpc(v);
        }
        return t;
    }();
    return tabla;
}

ExecResult executeAgentCommandCapture(const std::string& cmd,
                                      const std::vector<std::string>& params, const char* argv0) {
    if (const Verbo* v = tablaDeVerbos().busca(cmd)) {
        return v->haz(Peticion{cmd, params, argv0});
    }
    return fallo(2, "unknown_command", {cmd});
}

// Los argumentos de un código, como array JSON en UNA línea (el estado de un trabajo va por
// líneas «CLAVE=valor»).
std::string argsJson(const std::vector<std::string>& args) {
    std::string out = "[";
    for (std::size_t i = 0; i < args.size(); ++i) {
        out += (i ? ",\"" : "\"") + jsonEscape(args[i]) + "\"";
    }
    return out + "]";
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

}  // namespace zfsmgr::agent
