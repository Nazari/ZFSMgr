// Commands on datasets and snapshots (`destroy`, `rollback`, `clone`, `create`,
// `rename`, `mount`, `promote`, `set`, `get`) and creating or editing connections.

#include "shell_internal.h"

namespace zfsmgr::cli::detail {

// `destroy ds#content/<rel>`: a file, or with -r a directory, inside a mounted dataset. The
// agent checks everything again (see --mutate-remove-content); asking here first is only so
// that a refusal comes before the question, not after it.
bool borraContenido(Estado& e, const Peticion& pet, const ZfsmUrl& destino) {
    if (!destino.snapshot.empty()) {
        std::fprintf(stderr, "%s\n", B::agentmsg::render("remove_in_zfs_dir", {}).c_str());
        e.ultimoRc = 1;
        return false;
    }
    const std::string rel = B::join(destino.detail, "/");
    if (const std::string no = AV::contentRemovalRefusal(rel); !no.empty()) {
        std::fprintf(stderr, "%s\n", B::agentmsg::render(no, {}).c_str());
        e.ultimoRc = 1;
        return false;
    }
    std::string error;
    const auto* perfil = perfilVivoDe(e, destino, error);
    if (!perfil) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    if (T::isWindowsConnection(*perfil)) {
        std::fprintf(stderr, "%s\n", B::agentmsg::render("remove_not_on_windows", {}).c_str());
        e.ultimoRc = 1;
        return false;
    }
    std::string montaje;
    if (!dondeEstaMontado(e, destino, destino.dataset, /*paraEscribir=*/true, /*callado=*/false,
                          montaje)) {
        return false;
    }
    const bool recursivo = pet.tiene("-r") || pet.tiene("-R");
    if (!confirma(
            e, B::format(T("t_conf_destroy_contenido",
                           "Se va a BORRAR %1%2 del dataset %3 en %4. ¿Continuar?"),
                         {rel,
                          recursivo ? T("t_conf_con_lo_de_dentro", " con todo lo que tenga dentro")
                                    : std::string(),
                          destino.dataset, destino.connection}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    std::string out;
    if (!agente(e, destino, AV::argvRemoveContent(destino.dataset, montaje, rel, recursivo), out,
                300000)) {
        return false;
    }
    std::fprintf(stderr, TC("t_borrado_contenido", "borrado %s\n"), textoDe(destino).c_str());
    // Deleting the directory one stands in: go up to its parent.
    if (e.actual.connection == destino.connection && e.actual.dataset == destino.dataset
        && e.actual.section == destino.section && e.actual.detail.size() >= destino.detail.size()
        && std::equal(destino.detail.begin(), destino.detail.end(), e.actual.detail.begin())) {
        ZfsmUrl arriba = destino;
        arriba.detail.pop_back();
        e.actual = arriba;
    }
    return true;
}

bool cmdDestroy(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (B::toLowerAscii(destino.section) == B::zfsmSection::kContent && !destino.detail.empty()) {
        return borraContenido(e, pet, destino);
    }
    // En una CONEXIÓN, `destroy` la quita de la configuración. Es lo único que se puede
    // «destruir» ahí, y la pregunta lo deja claro: no se toca nada en la máquina.
    if (nodoDe(destino) == Nodo::Conexion) {
        const std::string id = idDe(e, destino);
        if (!confirma(e, B::format(T("t_conf_quitar_conn",
                                     "Se va a quitar la conexión %1 de la configuración. NO "
                                     "se toca nada en la máquina. ¿Continuar?"),
                                   {id}))) {
            std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
            return false;
        }
        std::string error;
        if (!borrarConexion(*e.ses, id, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
        recarga(e);
        std::fprintf(stderr, TC("t_quitada_la_2486c6", "quitada la conexión %s\n"), id.c_str());
        vaA(e, "/");
        return true;
    }
    // En un POOL, `destroy` es `zpool destroy`: `zfs destroy` sobre el dataset raíz de un
    // pool no funciona, así que es la única lectura posible.
    if (nodoDe(destino) == Nodo::Dataset && destino.isPoolRoot() && destino.snapshot.empty()) {
        if (!confirma(e, B::format(T("t_conf_destroy_pool",
                                     "Se va a DESTRUIR EL POOL %1 en %2, con todos sus "
                                     "datasets y todos sus datos. ¿Continuar?"),
                                   {destino.pool, destino.connection}))) {
            std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
            return false;
        }
        std::vector<std::string> argv{"destroy"};
        if (pet.tiene("-f")) {
            argv.push_back("-f");
        }
        argv.push_back(destino.pool);
        if (!zpoolGeneric(e, destino, argv)) {
            return false;
        }
        std::fprintf(stderr, TC("t_destruido__a157ec", "destruido el pool %s\n"),
                     destino.pool.c_str());
        vaA(e, "..");
        return true;
    }
    const std::string objetivo = destino.zfsName();
    if (objetivo.empty()) {
        std::fprintf(stderr, TC("t_no_hay_nad_da1f25", "no hay nada que borrar en %s\n"),
                     textoDe(destino).c_str());
        return false;
    }
    const bool recursivo = pet.tiene("-r") || pet.tiene("-R");
    // La pregunta dice QUÉ se va a borrar y con qué alcance: «¿seguro?» a secas es lo que
    // hace que se conteste que sí sin leer.
    if (!confirma(e, B::format(T("t_conf_destroy_ds", "Se va a DESTRUIR %1%2 en %3. ¿Continuar?"),
                               {objetivo,
                                recursivo ? T("t_conf_y_desc", " y todos sus descendientes")
                                          : std::string(),
                                destino.connection}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    std::string out;
    if (!agente(e, destino,
                INST::argvDestroy(objetivo, pet.tiene("-f"),
                                  pet.tiene("-R")   ? INST::Scope::Dependents
                                  : pet.tiene("-r") ? INST::Scope::Descendants
                                                    : INST::Scope::Only),
                out)) {
        return false;
    }
    std::fprintf(stderr, TC("t_destruido__e8a7d6", "destruido %s\n"), objetivo.c_str());
    // Si se ha borrado justo donde estábamos, se sube: quedarse apuntando a algo que ya no
    // existe hace que la siguiente orden falle sin que se entienda por qué.
    if (destino.zfsName() == e.actual.zfsName() && destino.connection == e.actual.connection) {
        vaA(e, "..");
    }
    return true;
}

bool cmdRollback(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;

    if (!confirma(e, B::format(T("t_conf_rollback",
                                 "Se va a volver %1 al estado de @%2, DESCARTANDO todo lo "
                                 "posterior. ¿Continuar?"),
                               {destino.dataset, destino.snapshot}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    std::string out;
    if (!agente(e, destino,
                INST::argvRollback(destino.zfsName(), pet.tiene("-f"),
                                   pet.tiene("-R")   ? INST::Scope::Dependents
                                   : pet.tiene("-r") ? INST::Scope::Descendants
                                                     : INST::Scope::Only),
                out)) {
        return false;
    }
    std::fprintf(stderr, "%s vuelto a @%s\n", destino.dataset.c_str(), destino.snapshot.c_str());
    return true;
}

bool cmdClone(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    if (pet.lista("texto").empty()) {
        std::fputs(
            TC("t_uso_clone2",
               "uso: clone <nuevo-dataset> [prop=valor...] [-p] [-u] [--from <@snapshot>]\n"),
            stderr);
        return false;
    }
    const ZfsmUrl& origen = pet.objetivo;
    if (origen.snapshot.empty()) {
        std::fprintf(stderr,
                     TC("t_el_origen__604ce4", "el origen tiene que ser un snapshot (ahora: %s)\n"),
                     textoDe(origen).c_str());
        return false;
    }
    // El nombre se toma como hijo del sitio actual salvo que ya venga con ruta, igual que en
    // `create`. Sin esto había que escribir el nombre ZFS COMPLETO —`clone clonado`
    // respondía «missing dataset name», que es del propio zfs y no dice qué falta— y la
    // ayuda anunciaba `clone <nuevo>` como si un nombre suelto valiera. Venía de antes.
    const std::string nombre = pet.uno("texto");
    const std::string nuevo =
        nombre.find('/') == std::string::npos ? origen.dataset + "/" + nombre : nombre;
    // `-p`, `-u` and `prop=valor` after the name, as in `create`: they go by the generic
    // path, the only one that takes flags. Without them, the typed verb.
    std::vector<std::string> banderas = pet.nativas();
    const std::vector<std::string> textos = pet.lista("texto");
    for (std::size_t i = 1; i < textos.size(); ++i) {
        banderas.push_back("-o");
        banderas.push_back(textos[i]);
    }
    std::string out;
    if (banderas.empty()) {
        if (!agente(e, origen, INST::argvClone(origen.zfsName(), nuevo), out)) {
            return false;
        }
    } else if (!zfsGeneric(e, origen, INST::argvZfsClone(origen.zfsName(), nuevo, banderas))) {
        return false;
    }
    std::fprintf(stderr, TC("t_clonado", "clonado %s -> %s\n"), origen.zfsName().c_str(),
                 nuevo.c_str());
    return true;
}

// Alta de una conexión. Es `create` estando en la RAÍZ, por la misma regla que hace que
// `create` en un dataset cree un hijo: se crea un nodo donde uno está.
//
// Los campos se piden por el terminal si no vienen por opciones, y **la contraseña NUNCA
// por argumento**: iría en `argv` y se vería en `ps` para cualquier usuario de la máquina.
// O se teclea, o entra por un descriptor con --password-fd.
// `--family auto|ipv4|ipv6`: which addresses ssh tries. Untouched when not given.
static bool familiaDeDireccion(const Peticion& pet, std::string& familia) {
    const std::string v = B::toLowerAscii(pet.valor("family"));
    if (v.empty()) {
        return true;
    }
    if (v != "auto" && v != "ipv4" && v != "ipv6") {
        std::fprintf(stderr,
                     TC("t_family_invalida", "«%s» no vale para --family: auto, ipv4 o ipv6\n"),
                     v.c_str());
        return false;
    }
    familia = v;
    return true;
}

bool cmdCrearConexion(Estado& e, const Peticion& pet) {
    if (pet.lista("texto").empty()) {
        std::fputs(TC("t_uso_create_ac2703",
                      "uso: create <identificador> [--name <n>] [--type LOCAL|SSH] [--os <so>]\n"
                      "            [--host <h>] [--port <p>] [--user <u>] [--key <ruta>]\n"
                      "            [--sudo] [--password-fd <n>]\n"
                      "  Da de alta una conexión. Lo que no se dé por opciones se pregunta.\n"),
                   stderr);
        return false;
    }
    B::ConnectionProfile p;
    p.id = B::trim(pet.uno("texto"));
    if (buscarConexion(e.conns, p.id) != nullptr) {
        std::fprintf(stderr, TC("t_ya_existe__484f91", "ya existe una conexión «%s»\n"),
                     p.id.c_str());
        return false;
    }
    const bool interactivo = hayTerminal();

    p.name = pet.valor("name");
    p.connType = B::toUpperAscii(pet.valor("type"));
    p.osType = pet.valor("os");
    p.host = pet.valor("host");
    p.username = pet.valor("user");
    p.keyPath = pet.valor("key");
    p.useSudo = pet.tiene("--sudo");
    const std::string puertoTexto = pet.valor("port");
    if (!familiaDeDireccion(pet, p.sshAddressFamily)) {
        return false;
    }

    if (interactivo) {
        if (p.name.empty() && !pide(TC("t_p_nombre_visible", "Nombre visible"), p.id, p.name))
            return false;
        if (p.connType.empty()
            && !pide(TC("t_p_tipo_conexion", "Tipo (LOCAL/SSH)"), "SSH", p.connType))
            return false;
        p.connType = B::toUpperAscii(p.connType);
        if (p.osType.empty()
            && !pide(TC("t_p_sistema_operativo", "Sistema operativo"), "Linux", p.osType))
            return false;
    }
    if (p.name.empty())
        p.name = p.id;
    if (p.connType.empty())
        p.connType = "SSH";
    if (p.osType.empty())
        p.osType = "Linux";

    const bool local = B::toLowerAscii(p.connType) == "local";
    if (!local) {
        if (p.host.empty() && interactivo && !pide(TC("t_p_host", "Host"), "", p.host))
            return false;
        if (p.host.empty()) {
            std::fputs(TC("t_una_conexi_86be2f", "una conexión SSH necesita un host\n"), stderr);
            return false;
        }
        if (p.username.empty() && interactivo
            && !pide(TC("t_p_usuario", "Usuario"), "", p.username))
            return false;
        if (p.keyPath.empty() && interactivo
            && !pide(TC("t_p_ruta_clave", "Ruta de clave SSH (vacío = contraseña)"), "",
                     p.keyPath)) {
            return false;
        }
        // Una clave que no está no se da de alta: la conexión «funcionaría» entrando por
        // otro lado y arrastraría la errata hasta el día que deje de hacerlo.
        std::string ruta;
        if (sshClaveFalta(p, ruta)) {
            std::fprintf(stderr,
                         TC("t_clave_no_existe_alta",
                            "la clave SSH «%s» no existe o no se puede leer: no se da de alta la "
                            "conexión\n"),
                         ruta.c_str());
            e.ultimoRc = 1;
            return false;
        }
    }
    p.port = puertoTexto.empty() ? 0 : std::atoi(puertoTexto.c_str());

    // El sudo se pregunta ANTES que la contraseña, porque es lo que decide si hace falta.
    //
    // Estaba después, y con una clave SSH eso dejaba la conexión sin salida: `quiereClave`
    // miraba `useSudo`, que todavía era falso porque la respuesta no había llegado, así que
    // no se pedía contraseña; y acto seguido se contestaba que sí usa sudo. La conexión
    // quedaba con sudo y sin contraseña que darle, sin que nada lo dijera. Es justo el caso
    // de una máquina a la que se entra por clave y que necesita elevar.
    if (!pet.tiene("--sudo") && interactivo) {
        std::string resp;
        if (!pide(TC("t_p_sudo_n", "¿Usa sudo? (s/N)"), "n", resp))
            return false;
        const std::string r = B::toLowerAscii(resp);
        p.useSudo = (r == "s" || r == "si" || r == "sí" || r == "y" || r == "yes");
    }

    // La contraseña. Por descriptor si se dio, y si no por el terminal con el eco apagado.
    // Solo hace falta si no hay clave SSH, o si la máquina va a necesitar sudo.
    //
    // **Se comprueba ANTES si hay dónde cifrarla.** Guardar una contraseña de acceso en
    // claro no se hace, así que sin contraseña maestra la creación fracasaría — y hacerla
    // teclear para tirarla después es la peor forma de contarlo. Si hay terminal se pide la
    // maestra en ese momento; si no, se avisa y se crea sin contraseña.
    const std::string fdTexto = pet.valor("password-fd");
    const bool quiereClave = !fdTexto.empty() || (p.keyPath.empty() || p.useSudo || local);
    std::string err;
    if (quiereClave && e.ses->maestra.empty()) {
        if (interactivo) {
            std::fputs(TC("t_para_guard_a024db",
                          "Para guardar la contraseña de una conexión hace falta una "
                          "contraseña maestra:\n"
                          "es con la que se cifra en config.json, y sin ella no se guarda en "
                          "claro.\n"),
                       stderr);
            // Es una maestra NUEVA —si hubiera otra, ya se habría pedido al entrar—, así que
            // se pide dos veces, como en `master-password`: sin eco, un dedazo dejaría
            // cifrado con algo que nadie conoce.
            std::string maestra;
            std::string otra;
            if (!preguntarSecretoPorTerminal(T("t_p_maestra_nueva", "Contraseña maestra nueva: "),
                                             maestra, err)
                || !preguntarSecretoPorTerminal(T("t_mp_p2", "Repítala: "), otra, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return false;
            }
            const bool iguales = maestra == otra;
            for (char& c : otra) {
                c = '\0';
            }
            if (!iguales || maestra.empty()) {
                for (char& c : maestra) {
                    c = '\0';
                }
                std::fputs(iguales
                               ? TC("t_mp_vacia", "la contraseña maestra no puede quedar vacía\n")
                               : TC("t_maestras_no_coinciden",
                                    "las dos contraseñas no coinciden; no se ha creado nada\n"),
                           stderr);
                return false;
            }
            e.ses->maestra = maestra;
        } else {
            std::fputs(TC("t_aviso_sin__287990",
                          "aviso: sin contraseña maestra (--password-fd) la conexión se crea "
                          "SIN contraseña\n"),
                       stderr);
        }
    }
    if (!fdTexto.empty()) {
        if (!leerSecretoDeDescriptor(std::atoi(fdTexto.c_str()), p.password, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
    } else if ((interactivo || e.pideSecretoServicio) && quiereClave && !e.ses->maestra.empty()) {
        // Asked through the service too: the graphical interface answers it.
        std::string clave;
        if (!pideSecreto(e, T("t_p_pass_vacio", "Contraseña (vacío = ninguna): "), clave, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
        p.password = clave;
    }
    std::string error;
    if (!guardarConexion(*e.ses, p, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        // La contraseña no se queda en memoria si no se llegó a guardar.
        for (char& c : p.password) {
            c = 0;
        }
        return false;
    }
    for (char& c : p.password) {
        c = 0;
    }
    // Se recargan: a partir de ahora se puede navegar a ella.
    recarga(e);
    std::fprintf(stderr, TC("t_creada_la__d038ed", "creada la conexión %s (%s)\n"), p.id.c_str(),
                 local ? "local" : (p.username + "@" + p.host).c_str());
    return true;
}

bool cmdEditarConexion(Estado& e, const Peticion& pet, const ZfsmUrl& destino) {
    const std::string id = idDe(e, destino);
    const auto* actual = buscarConexion(e.conns, id);
    if (!actual) {
        std::fprintf(stderr,
                     TC("t_hace_falta_2bed6d", "hace falta estar en una conexión (ahora: %s)\n"),
                     textoDe(destino).c_str());
        return false;
    }
    B::ConnectionProfile p = *actual;
    // El de ANTES, para limpiar lo que la sesión recordaba de él: si cambia la dirección, lo
    // recordado va con la clave vieja y su túnel seguiría abierto contra la máquina vieja.
    const B::ConnectionProfile antes = *actual;
    const bool interactivo = hayTerminal();
    const auto toma = [&](const char* clave, std::string& campo, const char* etiqueta) {
        const std::string v = pet.valor(clave);
        if (!v.empty()) {
            campo = v;
            return true;
        }
        if (!interactivo) {
            return true;
        }
        std::string leido;
        if (!pide(etiqueta, campo, leido)) {
            return false;
        }
        campo = leido;
        return true;
    };
    if (!toma("name", p.name, "Nombre visible"))
        return false;
    if (!toma("type", p.connType, "Tipo (LOCAL/SSH)"))
        return false;
    p.connType = B::toUpperAscii(p.connType);
    if (!toma("os", p.osType, "Sistema operativo"))
        return false;
    if (B::toLowerAscii(p.connType) != "local") {
        if (!toma("host", p.host, "Host"))
            return false;
        if (!toma("user", p.username, "Usuario"))
            return false;
        if (!toma("key", p.keyPath, "Ruta de clave SSH"))
            return false;
        if (pet.tiene("--no-key")) {
            p.keyPath.clear();
        }
        if (!familiaDeDireccion(pet, p.sshAddressFamily)) {
            return false;
        }
        std::string ruta;
        if (sshClaveFalta(p, ruta)) {
            std::fprintf(stderr,
                         TC("t_clave_no_existe_edit",
                            "la clave SSH «%s» no existe o no se puede leer: no se guarda el "
                            "cambio\n"),
                         ruta.c_str());
            e.ultimoRc = 1;
            return false;
        }
        std::string puerto = std::to_string(p.port);
        if (!toma("port", puerto, "Puerto"))
            return false;
        p.port = std::atoi(puerto.c_str());
    }
    if (pet.tiene("--sudo")) {
        p.useSudo = true;
    } else if (pet.tiene("--no-sudo")) {
        p.useSudo = false;
    } else if (interactivo) {
        std::string resp;
        if (!pide(TC("t_p_sudo", "¿Usa sudo? (s/n)"), p.useSudo ? "s" : "n", resp))
            return false;
        const std::string r = B::toLowerAscii(resp);
        p.useSudo = (r == "s" || r == "si" || r == "sí" || r == "y" || r == "yes");
    }

    // La contraseña, PREGUNTADA como los demás campos.
    //
    // Antes solo se tocaba con `--password-fd` o con `--password`, así que un `edit mbp` a
    // secas recorría nombre, tipo, sistema, host, usuario, clave, puerto y sudo… y se
    // saltaba la contraseña sin decir nada. Quien quería cambiarla —el caso normal: la de
    // sudo de una máquina cambió y esta copia se quedó con la vieja— repasaba el
    // cuestionario entero, leía «actualizada la conexión mbp» y seguía con la de antes.
    //
    // El motivo que había era bueno y se conserva: en una edición, dejarla en blanco tiene
    // que CONSERVARLA, no borrarla. Pero eso no obliga a no preguntar, obliga a que el
    // vacío signifique «la de antes» — que es justo lo que `pide()` hace con todos los
    // demás campos. La diferencia es que aquí no se puede enseñar el valor actual entre
    // corchetes, así que lo que se dice es si HAY una guardada o no.
    const std::string fdTexto = pet.valor("password-fd");
    if (!fdTexto.empty()) {
        std::string err;
        if (!leerSecretoDeDescriptor(std::atoi(fdTexto.c_str()), p.password, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
    } else if (interactivo || e.pideSecretoServicio) {
        std::string err;
        std::string clave;
        const std::string aviso =
            p.password.empty()
                ? T("t_p_pass_nueva_sin",
                    "Contraseña (no hay ninguna guardada; Intro = seguir sin ella): ")
                : T("t_p_pass_nueva_con", "Contraseña (Intro = conservar la guardada): ");
        if (!pideSecreto(e, aviso, clave, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
        if (!clave.empty()) {
            p.password = clave;
        }
    }

    std::string error;
    if (!guardarConexion(*e.ses, p, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        for (char& c : p.password) {
            c = 0;
        }
        return false;
    }
    olvidaEstadoDeConexion(e, antes);
    olvidaVersionDaemon(e, p);

    // Si lo editado es la conexión LOCAL, guardar no basta: hay dos copias vivas de esas
    // credenciales y las dos se quedarían con las de antes.
    //
    //  - La sesión las recuerda para no preguntar en cada orden que eleve.
    //  - Y el material TLS del daemon local se leyó ELEVANDO con la contraseña anterior,
    //    así que el RPC seguiría usando lo cacheado.
    //
    // Sin esto, cambiar la contraseña de sudo parecía no surtir efecto hasta salir y
    // volver a entrar — que es justo el fallo que la interfaz comenta en su propio código.
    if (B::connjson::isLocalProfile(p)) {
        e.ses->sudoUsuario = p.username;
        e.ses->sudoClave = p.password;
        e.ses->sudoResuelto = !p.password.empty();
        T::clearLocalDaemonTlsCache();
    }
    const bool eraLocal = B::connjson::isLocalProfile(p);
    for (char& c : p.password) {
        c = 0;
    }
    recarga(e);
    std::fprintf(stderr, TC("t_actualizad_41b05c", "actualizada la conexión %s\n"), id.c_str());

    // Y se COMPRUEBAN, que es lo que uno quiere saber al cambiarlas: si la contraseña
    // nueva no eleva, enterarse ahora y no la próxima vez que haga falta de verdad.
    //
    // Esto solo se hacía en la conexión LOCAL, y es en las de SSH donde más falta hace. Una
    // contraseña de sudo equivocada en una máquina remota no da ningún error al guardarla:
    // lo que da es que MÁS TARDE no se pueda leer el material TLS de su daemon, y ese
    // mensaje —«no se pudo leer el material TLS del daemon»— no menciona sudo por ninguna
    // parte. Es exactamente lo que pasó con mbp visto desde mmela: la contraseña de esa
    // máquina había cambiado, la copia de mmela se quedó con la vieja, y el síntoma salía
    // tres pantallas más allá y hablando de otra cosa.
    const bool haySudo = p.useSudo || eraLocal;
    if (haySudo && (eraLocal ? e.ses->sudoResuelto : true)) {
        std::string out;
        std::string err;
        int rc = -1;
        const auto* vivo = buscarConexion(e.conns, id);
        if (vivo) {
            const B::ConnectionProfile perfil = conSudo(e, *vivo);
            // On Windows there is no sudo nor `id`: «elevating» means the SSH user is an
            // administrator, which OpenSSH turns into an elevated session. Asked with `id -u`
            // it always said «they do NOT elevate» (a1278 and w11, 2026-10-20).
            const bool windows = T::isWindowsConnection(perfil);
            const std::string prueba =
                windows ? std::string(
                              "([Security.Principal.WindowsPrincipal]"
                              "[Security.Principal.WindowsIdentity]::GetCurrent())"
                              ".IsInRole([Security.Principal.WindowsBuiltInRole]::"
                              "Administrator)")
                        : H::withSudoCommand(perfil, "id -u");
            if (T::runSsh(e.ses->transporte, perfil, prueba, 15000, out, err, rc, {}, {}, {}, {},
                          /*allowAgentRpc=*/false, /*echoOutputToLog=*/false)
                && rc == 0 && B::trim(out) == (windows ? "True" : "0")) {
                std::fputs(TC("t_sudo_ok", "las credenciales de sudo elevan correctamente\n"),
                           stderr);
            } else {
                // Con el motivo, que es lo que convierte el aviso en algo accionable.
                //
                // La clave es NUEVA a propósito. La de antes decía «en esta máquina» y no
                // llevaba ningún `%s`; reutilizarla con dos marcas de formato haría que el
                // catálogo —que manda sobre el literal— devolviera el texto viejo y
                // `fprintf` se pusiera a leer argumentos que ese texto no pide. Es el mismo
                // fallo que se quitó de cuatro sitios hace dos días.
                const std::string detalle = B::trim(H::oneLine(err));
                const std::string cola = detalle.empty() ? std::string() : ": " + detalle;
                std::fprintf(stderr,
                             TC("t_sudo_mal_en",
                                "aviso: con esas credenciales NO se "
                                "consigue elevar en %s%s\n"),
                             id.c_str(), cola.c_str());
            }
        }
    }
    return true;
}

bool cmdEdit(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    return cmdEditarConexion(e, pet, pet.objetivo);
}

bool cmdCreate(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    ZfsmUrl destino = pet.objetivo;
    if (pet.lista("texto").empty() && nodoDe(destino) != Nodo::Raiz) {
        std::fputs(TC("t_uso_create_c8fa17", "uso: create <nombre> [prop=valor...]\n"), stderr);
        return false;
    }
    // El mismo verbo en los tres niveles, porque es la misma idea —crear un nodo donde uno
    // está—: en la RAÍZ una conexión, en una CONEXIÓN un pool, en un DATASET un hijo.
    //
    // Pero **el nivel no lo decide solo dónde uno está: también la forma del nombre**. Un
    // nombre con barra no puede ser el de una conexión, así que desde la raíz
    // `create unibody/sback/tmp` no es «una conexión que se llama así» —lo era, y se
    // intentaba dar de alta— sino «el dataset sback/tmp en unibody». La regla vive en
    // `creacion::queSeCrea`, entera y con sus pruebas.
    namespace CR = creacion;
    const CR::Nivel nivel = nodoDe(destino) == Nodo::Raiz       ? CR::Nivel::Raiz
                            : nodoDe(destino) == Nodo::Conexion ? CR::Nivel::Conexion
                                                                : CR::Nivel::Dataset;
    const CR::Decision d = CR::queSeCrea(nivel, pet.uno("texto"));
    if (!d.ruta.empty()) {
        // Solo hay ruta que resolver cuando el nombre traía delante el tramo de la
        // máquina. Se resuelve con la MISMA función que `cd`, para que «la conexión no
        // existe» se diga igual en las dos.
        ZfsmUrl base;
        std::string error;
        if (!resuelve(e, d.ruta, base, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
        destino = base;
    }
    if (d.que == CR::Objeto::Conexion) {
        return cmdCrearConexion(e, pet);
    }
    if (d.que == CR::Objeto::Pool) {
        return cmdCrearPool(e, pet, destino, d.ruta.empty() ? std::string() : d.nombre);
    }
    if (d.que == CR::Objeto::Dataset && destino.dataset.empty()) {
        // El destino es una CONEXIÓN —se llegó desde la raíz con ruta, o se estaba ya en
        // ella—, así que se le cuelga el nombre ZFS completo y el resto del camino, que ya
        // sabe tratar un nombre con ruta, funciona igual. La condición mira el destino y
        // no si hubo ruta: estando en `zfsm://unibody`, `create sback/tmp` no trae ruta
        // que resolver y necesita esto lo mismo.
        destino.dataset = d.nombre;
    }
    // Un nombre que empieza por `@` nombra una INSTANTÁNEA, no un hijo. Se decide por el
    // marcador y no por una opción, porque `@` es lo que ya distingue una instantánea en la
    // URL: `zfsm://local/tank/datos@ayer`.
    if (pet.uno("texto").rfind('@', 0) == 0) {
        return creaInstantanea(e, pet, destino);
    }
    if (!exigeDataset(destino)) {
        return false;
    }
    std::vector<std::string> argv{"create"};
    for (const std::string& n : pet.nativas()) {
        argv.push_back(n);
    }
    for (std::size_t i = 1; i < pet.lista("texto").size(); ++i) {
        argv.push_back("-o");
        argv.push_back(pet.lista("texto")[i]);
    }
    // El nombre se toma como HIJO del sitio actual salvo que ya venga con ruta: escribir
    // `create datos` estando en `tank/casa` debe crear `tank/casa/datos`.
    const std::string hijo = d.nombre;
    argv.push_back(hijo.find('/') == std::string::npos ? destino.dataset + "/" + hijo : hijo);
    // Encrypted with a passphrase that `zfs` would ask for: the shell asks it (twice, as
    // `change-key`) or reads it from --password-fd, and the daemon's create verb hands it to
    // `zfs` down a pipe. Never in argv.
    std::map<std::string, std::string> props;
    const std::vector<std::string> textos = pet.lista("texto");  // a copy: lista() returns one
    for (std::size_t i = 1; i < textos.size(); ++i) {
        const std::string& asig = textos[i];
        const auto igual = asig.find('=');
        if (igual != std::string::npos) {
            props[B::toLowerAscii(asig.substr(0, igual))] = B::toLowerAscii(asig.substr(igual + 1));
        }
    }
    const std::string cifrado = props["encryption"];
    const bool pideFrase = (cifrado == "on" || cifrado.rfind("aes-", 0) == 0)
                           && props["keyformat"] == "passphrase"
                           && (props["keylocation"].empty() || props["keylocation"] == "prompt");
    if (pideFrase) {
        std::string frase;
        std::string err;
        const std::string fd = pet.valor("password-fd");
        if (!fd.empty()) {
            if (!leerSecretoDeDescriptor(std::atoi(fd.c_str()), frase, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return false;
            }
        } else {
            std::string otra;
            if (!pideSecreto(e, T("t_p_frase_nueva", "Frase de paso nueva: "), frase, err)
                || !pideSecreto(e, T("t_p_frase_rep", "Repítala: "), otra, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return false;
            }
            const bool iguales = frase == otra;
            std::fill(otra.begin(), otra.end(), '\0');
            if (!iguales) {
                std::fill(frase.begin(), frase.end(), '\0');
                std::fputs(TC("t_frases_no_coinciden_crear",
                              "las dos frases no coinciden; no se ha creado nada\n"),
                           stderr);
                return false;
            }
        }
        if (frase.empty()) {
            std::fputs(TC("t_frase_vacia", "la frase de paso no puede estar vacía\n"), stderr);
            return false;
        }
        std::string out;
        const bool ok =
            agente(e, destino, PET::createDataset(B::helpers::agentArgv(argv), frase), out);
        std::fill(frase.begin(), frase.end(), '\0');
        if (!ok) {
            return false;
        }
    } else if (!zfsGeneric(e, destino, argv)) {
        return false;
    }
    // With `-n`/`-P` zfs only says what it would do: «created» there would be false.
    if (!pet.tiene("-n") && !pet.tiene("-P")) {
        std::fprintf(stderr, TC("t_creado_s_ea96d0", "creado %s\n"), argv.back().c_str());
    }
    return true;
}

bool cmdRename(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    if (pet.lista("texto").empty()) {
        std::fputs(TC("t_uso_rename_c4838f", "uso: rename <nuevo-nombre>\n"), stderr);
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (!exigeDataset(destino)) {
        return false;
    }
    // La regla —sin barra se conserva el padre— vive en `commands::datasets`.
    const std::vector<std::string> argvRen = DS::argvRename(destino.dataset, pet.uno("texto"));
    if (argvRen.empty()) {
        std::fputs(TC("t_rename_invalido", "ese nombre no sirve para renombrar\n"), stderr);
        return false;
    }
    const std::string completo = argvRen.back();
    if (!zfsGeneric(e, destino, argvRen)) {
        return false;
    }
    std::fprintf(stderr, TC("t_renombrado", "renombrado %s -> %s\n"), destino.dataset.c_str(),
                 completo.c_str());
    return true;
}

bool cmdMontaje(Estado& e, const LineaAnalizada& linea, bool montar) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    std::vector<std::string> argv{montar ? "mount" : "unmount"};
    if (pet.tiene("-f")) {
        argv.push_back("-f");
    }
    argv.push_back(destino.dataset);
    if (!zfsGeneric(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, "%s %s\n", montar ? "montado" : "desmontado", destino.dataset.c_str());
    return true;
}

bool cmdPromote(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (!zfsGeneric(e, destino, {"promote", destino.dataset})) {
        return false;
    }
    std::fprintf(stderr, TC("t_promovido", "promovido %s\n"), destino.dataset.c_str());
    return true;
}

bool cmdSet(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    const std::string objetivo = destino.zfsName();
    std::vector<std::string> argv{"set"};
    // La firma ya ha exigido que sean `prop=valor` y que haya al menos una: la ranura es
    // de tipo Propiedad y de cardinalidad UnaOMas.
    for (const auto& asig : pet.lista("props")) {
        argv.push_back(asig);
    }
    argv.push_back(objetivo);
    if (!zfsGeneric(e, destino, argv)) {
        return false;
    }
    std::fprintf(stderr, TC("t_set_aplicadas", "aplicadas %zu propiedades a %s\n"),
                 pet.lista("props").size(), objetivo.c_str());
    return true;
}

// `inherit <prop>…`: the properties go back to what they inherit, or to their default. It is
// what the graphical interface does when a property's «inherited» box is ticked again.
bool cmdInherit(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    const std::string objetivo = destino.zfsName();
    for (const auto& prop : pet.lista("props")) {
        std::vector<std::string> argv{"inherit"};
        if (pet.tiene("-r")) {
            argv.push_back("-r");
        }
        argv.push_back(prop);
        argv.push_back(objetivo);
        if (!zfsGeneric(e, destino, argv)) {
            return false;
        }
    }
    std::fprintf(stderr, TC("t_inherit_hecho", "heredadas %zu propiedades en %s\n"),
                 pet.lista("props").size(), objetivo.c_str());
    return true;
}

bool cmdGet(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    ZfsmUrl destino = pet.objetivo;
    destino.section = B::zfsmSection::kProperties;
    destino.detail.clear();
    if (!pet.uno("propiedad").empty()) {
        destino.detail = {pet.uno("propiedad")};
    }
    return listaPropiedades(e, destino);
}

// --- Las instantáneas PROGRAMADAS (GSA).
//
// La programación son propiedades del dataset, y las REGLAS de lo que es válido están en
// la capa base —`src/base/gsa.h`—, compartidas con la interfaz gráfica. Aquí solo se
// hablan: leer lo que hay, escribir lo que se pide y enseñarlo.

// Lo programado bajo un dataset o un pool, ya convertido a estructuras.
bool leeProgramaciones(Estado& e, const ZfsmUrl& destino, const std::string& raiz,
                       std::vector<B::gsa::Entry>& out) {
    std::string crudo;
    if (!agente(e, destino, PET::gsaOfDataset(raiz), crudo, 30000)) {
        return false;
    }
    for (const auto& kv : B::gsa::groupProperties(crudo)) {
        B::gsa::Schedule p;
        B::gsa::Reason m;
        if (!B::gsa::fromProperties(kv.second, p, m)) {
            // Un valor que no es un entero está PUESTO en la máquina: no se puede corregir
            // desde aquí, pero callarlo dejaría una fila que miente.
            std::fprintf(stderr,
                         TC("t_sch_valor_malo",
                            "aviso: %s tiene %s con un valor que no es un "
                            "número; se ignora esa programación\n"),
                         kv.first.c_str(), m.detail.c_str());
            continue;
        }
        out.push_back({kv.first, p});
    }
    return true;
}

Tabla tablaDeProgramaciones(const std::vector<std::pair<std::string, B::gsa::Entry>>& filas) {
    Tabla t;
    t.nombreJson = "schedules";
    t.cabecerasTexto = {T("t_cab_conexion", "CONEXIÓN"), T("t_cab_dataset", "DATASET"),
                        T("t_cab_activa", "ACTIVA"),     T("t_cab_recursiva", "RECURSIVA"),
                        T("t_cab_hora", "HORA"),         T("t_cab_dia", "DÍA"),
                        T("t_cab_sem", "SEM"),           T("t_cab_mes", "MES"),
                        T("t_cab_ano", "AÑO"),           T("t_cab_destino", "DESTINO")};
    t.campos = {"connection", "dataset", "enabled", "recursive",   "hourly", "daily",
                "weekly",     "monthly", "yearly",  "destination", "level"};  // json only
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Booleano, Tipo::Booleano,
               Tipo::Entero, Tipo::Entero, Tipo::Entero,   Tipo::Entero,
               Tipo::Entero, Tipo::Cadena, Tipo::Booleano};
    for (const auto& f : filas) {
        const B::gsa::Schedule& p = f.second.schedule;
        t.filas.push_back({f.first, f.second.dataset, p.enabled ? "true" : "false",
                           p.recursive ? "true" : "false", std::to_string(p.hourly),
                           std::to_string(p.daily), std::to_string(p.weekly),
                           std::to_string(p.monthly), std::to_string(p.yearly),
                           p.target.empty() ? "-" : p.target, p.level ? "true" : "false"});
    }
    return t;
}

// La OTRA máquina que nombra una de estas órdenes de dos extremos.
const B::ConnectionProfile* laOtra(Estado& e, const Peticion& pet, std::string& error) {
    const std::string nombre = pet.uno("texto");
    const auto* p = buscarConexion(e.conns, nombre);
    if (!p) {
        error = B::format(T("t_no_conn_nombre", "no hay ninguna conexión llamada «%1»"), {nombre});
        return nullptr;
    }
    return p;
}

}  // namespace zfsmgr::cli::detail
