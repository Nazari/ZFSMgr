// Administration of a connection: `authorize-key`, `export-trust`, `repair-mounts`,
// `peers` and `log`.

#include "shell_internal.h"

namespace zfsmgr::cli::detail {

// Autorizar la clave SSH de esta máquina en otra.
//
// NO pasa por el daemon, y no puede: esto es lo que hay que hacer ANTES de que haya
// daemon, porque `install-daemon` entra por SSH. Es de las pocas cosas que siguen
// mandando shell, y por un motivo que no va a desaparecer.
bool cmdAuthorizeKey(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    std::string error;
    const auto* origen = perfilVivoDe(e, pet.objetivo, error);
    if (!origen) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    const auto* destino = laOtra(e, pet, error);
    if (!destino) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    if (B::toLowerAscii(origen->id) == B::toLowerAscii(destino->id)) {
        std::fputs(TC("t_ak_misma", "el origen y el destino son la misma conexión\n"), stderr);
        return false;
    }

    // 1) La clave pública del ORIGEN. Se prueban las de siempre, en el orden en que las
    //    genera la gente.
    std::string clave;
    std::string err;
    int rc = -1;
    const std::string leerClave =
        "cat ~/.ssh/id_ed25519.pub 2>/dev/null "
        "|| cat ~/.ssh/id_rsa.pub 2>/dev/null "
        "|| cat ~/.ssh/id_ecdsa.pub 2>/dev/null "
        "|| cat ~/.ssh/id_*.pub 2>/dev/null | head -1";
    if (!T::runSsh(e.ses->transporte, *origen, H::withUnixSearchPathCommand(leerClave), 15000,
                   clave, err, rc, {}, {}, {}, {}, /*allowAgentRpc=*/false,
                   /*echoOutputToLog=*/e.ses->verboso)
        || rc != 0 || B::trim(clave).empty()) {
        std::fprintf(stderr,
                     TC("t_ak_sin_clave",
                        "%s no tiene clave pública SSH.\n"
                        "  Genérela allí con: ssh-keygen -t ed25519\n"),
                     origen->id.c_str());
        return false;
    }
    clave = B::trim(B::split(B::trim(clave), "\n", true).front());

    if (!confirma(e, B::format(T("t_conf_authorize_key",
                                 "Se va a autorizar la clave SSH de %1 en %2: después, %1 podrá "
                                 "entrar en %2 sin contraseña. ¿Continuar?"),
                               {origen->id, destino->id}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }

    // 2) Y se añade allí. Idempotente: si ya está, no se duplica.
    const std::string instalar =
        "mkdir -p ~/.ssh && chmod 700 ~/.ssh && "
        "(grep -qF " + B::shSingleQuote(clave) + " ~/.ssh/authorized_keys 2>/dev/null "
        " || printf '\\n%s\\n' " + B::shSingleQuote(clave) + " >> ~/.ssh/authorized_keys) && "
        "chmod 600 ~/.ssh/authorized_keys";
    std::string out;
    if (!T::runSsh(e.ses->transporte, *destino, H::withUnixSearchPathCommand(instalar), 15000, out,
                   err, rc, {}, {}, {}, {}, /*allowAgentRpc=*/false,
                   /*echoOutputToLog=*/e.ses->verboso)
        || rc != 0) {
        const std::string detalle = B::trim(err).empty() ? B::trim(out) : B::trim(err);
        std::fprintf(stderr, TC("t_ak_fallo", "no se pudo autorizar la clave en %s: %s\n"),
                     destino->id.c_str(), detalle.c_str());
        return false;
    }
    std::fprintf(stderr, TC("t_ak_hecho", "clave de %s autorizada en %s\n"), origen->id.c_str(),
                 destino->id.c_str());
    return true;
}

// El almacén de confianza, a otra máquina.
//
// Es para el CLIENTE que alguien abra allí, no para su daemon: eso es «peers --push». Van
// a sitios distintos —el perfil del usuario frente a /etc— y sirven para cosas distintas.
bool cmdExportTrust(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    std::string error;
    const auto* destino = laOtra(e, pet, error);
    if (!destino) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    if (T::isLocalConnection(*destino)) {
        std::fputs(TC("t_et_local", "la conexión Local ya usa el almacén de esta máquina\n"),
                   stderr);
        return false;
    }
    B::store::Warning aviso;
    const B::json::Value almacen = B::store::readTrustStore(e.ses->dirConfig, aviso);
    if (!aviso.empty()) {
        std::fputs(TC("t_et_sin_almacen", "no se pudo leer el almacén de confianza\n"), stderr);
        return false;
    }
    const std::string carga = B::json::toIndented(almacen);
    if (B::trim(carga).empty() || carga == "{}") {
        std::fputs(TC("t_et_vacio", "el almacén de confianza está vacío\n"), stderr);
        return false;
    }
    if (!confirma(e, B::format(T("t_conf_export_trust",
                                 "Se va a copiar el almacén de confianza a %1. Lleva CLAVES "
                                 "PRIVADAS: quien las tenga podrá hablar con esos daemons como "
                                 "usted. ¿Continuar?"),
                               {destino->id}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    // Por la ENTRADA ESTÁNDAR, no por argumento: el contenido lleva claves y un argumento
    // se ve en `ps` en la máquina de destino.
    const std::string guion =
        "cfg=\"${XDG_CONFIG_HOME:-$HOME/.config}/ZFSMgr\"; mkdir -p \"$cfg\" && "
        "umask 077 && cat > \"$cfg/trust-store.json\" && chmod 600 \"$cfg/trust-store.json\" && "
        "printf 'TRUST=%s\\n' \"$cfg/trust-store.json\"";
    std::string out;
    std::string err;
    int rc = -1;
    // El sexto parámetro opcional es la ENTRADA ESTÁNDAR; los tres anteriores son
    // devoluciones de llamada que aquí no se usan.
    if (!T::runSsh(e.ses->transporte, *destino, H::withUnixSearchPathCommand(guion), 20000, out,
                   err, rc, {}, {}, {}, carga, /*allowAgentRpc=*/false,
                   /*echoOutputToLog=*/false)
        || rc != 0) {
        const std::string detalle = B::trim(err).empty() ? B::trim(out) : B::trim(err);
        std::fprintf(stderr, TC("t_et_fallo", "no se pudo copiar el almacén a %s: %s\n"),
                     destino->id.c_str(), detalle.c_str());
        return false;
    }
    std::fprintf(stderr, TC("t_et_hecho", "almacén de confianza copiado a %s\n"),
                 destino->id.c_str());
    return true;
}

// Los datasets que se quedaron en un punto de montaje temporal.
//
// Sin `--apply` no toca nada, y eso no es una comodidad: es que la orden útil de verdad es
// la que se ejecuta ANTES de decidir. El verbo del daemon ya está partido así.
bool cmdRepairMounts(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const bool aplicar = pet.tiene("--apply");
    if (aplicar
        && !confirma(e, B::format(T("t_conf_repair_mounts",
                                    "Se van a devolver a su punto de montaje los datasets que se "
                                    "quedaron en uno temporal en %1. ¿Continuar?"),
                                  {pet.objetivo.connection}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    std::string out;
    std::vector<std::string> argv = PET::repairAltMountpoints({});
    if (aplicar) {
        argv.push_back("apply");
    }
    if (!agente(e, pet.objetivo, argv, out, 60000)) {
        return false;
    }
    // «STRANDED=<dataset> saved=<punto> current=<punto>», una línea por dataset.
    Tabla t;
    t.nombreJson = "stranded";
    t.cabecerasTexto = {T("t_cab_dataset", "DATASET"), T("t_cab_deberia", "DEBERÍA ESTAR EN"),
                        T("t_cab_esta_en", "ESTÁ EN")};
    t.campos = {"dataset", "saved", "current"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Cadena};
    for (const std::string& l : B::split(out, "\n", true)) {
        if (!B::startsWith(l, "STRANDED=")) {
            continue;
        }
        const std::string resto = l.substr(9);
        const std::size_t s = resto.find(" saved=");
        const std::size_t c = resto.find(" current=");
        if (s == std::string::npos || c == std::string::npos || c < s) {
            continue;
        }
        t.filas.push_back({B::trim(resto.substr(0, s)), B::trim(resto.substr(s + 7, c - s - 7)),
                           B::trim(resto.substr(c + 9))});
    }
    if (t.filas.empty()) {
        std::fprintf(stderr,
                     TC("t_rm_ninguno",
                        "no hay ningún dataset en un punto de montaje "
                        "temporal en %s\n"),
                     pet.objetivo.connection.c_str());
        return true;
    }
    t.imprime(e.formato);
    if (aplicar) {
        std::fprintf(stderr, TC("t_rm_hecho", "devueltos a su sitio: %zu\n"), t.filas.size());
    } else {
        std::fputs(TC("t_rm_ensayo",
                      "no se ha tocado nada: para devolverlos, «repair-mounts "
                      "--apply»\n"),
                   stderr);
    }
    return true;
}

// Las credenciales de los pares: qué otras máquinas sabe alcanzar este daemon.
//
// Hace falta para NIVELAR una programación contra otra máquina, porque eso lo hace el
// daemon solo, de madrugada, sin nadie delante: las credenciales tienen que estar en la
// máquina. Y son las de la OTRA punta —cada daemon verifica a sus clientes contra su
// propio certificado—, que es justo lo que el cliente ya guarda por conexión.
bool cmdPeers(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    if (!pet.valor("listen").empty()) {
        const std::string dir = pet.valor("listen");
        if (!confirma(e, B::format(T("t_conf_peers_listen",
                                     "El daemon de %1 pasará a atender en %2 y se reiniciará.\n"
                                     "Con un comodín, su puerto queda alcanzable desde la red "
                                     "—protegido por mTLS con el certificado de cliente fijado—. "
                                     "¿Continuar?"),
                                   {destino.connection, dir}))) {
            std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
            return false;
        }
        std::string out;
        if (!agente(e, destino, PET::setBindAddress(dir), out, 20000)) {
            return false;
        }
        std::fprintf(stderr,
                     TC("t_peers_bind",
                        "%s atiende ahora en %s; su daemon se está "
                        "reiniciando\n"),
                     destino.connection.c_str(), dir.c_str());
        return true;
    }
    if (!pet.tiene("--push")) {
        std::string out;
        if (!agente(e, destino, PET::peerList(), out, 20000)) {
            return false;
        }
        const PR::View vista = PR::parse(out);
        // Quién cree ser esa máquina va PRIMERO, y se dice también cuando falta.
        //
        // Su ausencia no rompe nada visible: rompe la nivelación GSA contra un dataset de la
        // propia máquina, en silencio y con un mensaje que culpa al par. Sin enseñarlo aquí
        // no había forma de dar con ello.
        if (vista.self.empty()) {
            std::fputs(TC("t_peers_sin_self2",
                          "AVISO: esta máquina no tiene credenciales entregadas: no puede nivelar\n"
                          "       contra otras ni contra sí misma. «peers --push» sobre ella.\n"),
                       stderr);
        } else {
            std::fprintf(stderr, TC("t_peers_self", "se identifica como: %s\n"),
                         vista.self.c_str());
        }
        // La IDENTIDAD, que es lo que de verdad decide. El nombre de arriba es una etiqueta
        // de quien entregó las credenciales —por eso una máquina puede llamarse «local» a sí
        // misma y otra también—; esto no lo elige nadie.
        if (!vista.realUid.empty()) {
            if (vista.selfUid.empty()) {
                std::fprintf(stderr,
                             TC("t_peers_uid_sin2",
                                "AVISO: esta máquina es %s, pero no se le entregó su "
                                "identidad: no sabrá nivelar contra sí misma. «peers "
                                "--push» sobre ella.\n"),
                             vista.realUid.c_str());
            } else if (B::toLowerAscii(vista.selfUid) == B::toLowerAscii(vista.realUid)) {
                std::fprintf(stderr, TC("t_peers_uid_ok", "identidad de la máquina: %s\n"),
                             vista.realUid.c_str());
            } else {
                // Las dos no coinciden. Esa configuración se escribió mirando a OTRA
                // máquina —un perfil copiado de otro equipo es la forma habitual— y hasta
                // ahora no había manera de verlo desde fuera.
                std::fprintf(stderr,
                             TC("t_peers_uid_choca",
                                "AVISO: esta máquina es %s, pero las credenciales que "
                                "tiene se entregaron a %s.\n"
                                "       Son máquinas distintas: vuelva a hacer "
                                "«peers --push» sobre ella.\n"),
                             vista.realUid.c_str(), vista.selfUid.c_str());
            }
        }
        Tabla t;
        t.nombreJson = "peers";
        t.cabecerasTexto = {T("t_cab_id", "ID"), T("t_cab_host", "HOST"),
                            T("t_cab_puerto", "PUERTO"), T("t_cab_identidad", "IDENTIDAD")};
        t.campos = {"id", "host", "port", "uid"};
        t.tipos = {Tipo::Cadena, Tipo::Cadena, Tipo::Entero, Tipo::Cadena};
        for (const PR::Peer& par : vista.pares) {
            t.filas.push_back({par.id, par.host, std::to_string(par.puerto), par.uid});
        }
        t.imprime(e.formato);
        return true;
    }

    // Componer la carga es cosa de la capa base: la interfaz necesita exactamente lo mismo.
    // El uid de la máquina a la que se entrega. Sale de su perfil, que lo rellena `refresh`
    // preguntándoselo a ella. Es OBLIGATORIO: «¿este destino soy yo?» se decide por él y solo
    // por él.
    std::string uidDeDestino;
    {
        const auto* pd = buscarConexion(e.conns, destino.connection);
        if (pd) {
            uidDeDestino = B::trim(pd->machineUid);
        }
        if (uidDeDestino.empty()) {
            std::fputs(TC("t_push_sin_uid2",
                          "no se conoce el identificador de máquina de este destino: un «refresh»\n"
                          "sobre esa conexión lo averigua, y después «peers --push».\n"),
                       stderr);
            return false;
        }
    }
    const PR::Handover entrega =
        PR::composeHandover(e.conns.perfiles, destino.connection, uidDeDestino);
    if (!entrega.ok()) {
        std::fprintf(stderr, "%s\n", PR::labelOf(entrega.failure).c_str());
        return false;
    }
    const std::vector<std::string>& nombres = entrega.names;
    if (!confirma(e, B::format(T("t_conf_peers",
                                 "Se van a entregar a %1 las credenciales de: %2.\n"
                                 "Con ellas, esa máquina puede hablar con las otras como si fuera "
                                 "usted. ¿Continuar?"),
                               {destino.connection, B::join(nombres, ", ")}))) {
        std::fputs(TC("t_cancelado_329c0e", "cancelado\n"), stderr);
        return false;
    }
    std::string out;
    if (!agente(e, destino, PET::setPeers(entrega.payloadB64), out, 30000)) {
        return false;
    }
    std::fprintf(stderr, TC("t_peers_puestos", "entregadas %zu credenciales a %s\n"),
                 nombres.size(), destino.connection.c_str());
    return true;
}

// El registro del daemon.
//
// Se pide por BYTES desde el final y no por líneas: el verbo del agente lee un rango del
// fichero —para que la interfaz pueda ir siguiéndolo sin releerlo entero— y no sabe contar
// renglones. Se pide de sobra y se recorta aquí, que es donde se sabe cuántas se querían.
bool cmdLog(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    long lineas = 200;
    if (!pet.valor("lines").empty()) {
        const std::string v = pet.valor("lines");
        if (v.find_first_not_of("0123456789") != std::string::npos) {
            std::fprintf(stderr, TC("t_log_no_entero", "«%s» no es un número de líneas\n"),
                         v.c_str());
            return false;
        }
        lineas = std::atol(v.c_str());
    }
    // Un renglón del registro anda por los 120 bytes; se piden con holgura y sobra poco.
    const long bytes = (lineas + 20) * 400;
    std::string out;
    if (!agente(e, pet.objetivo, PET::daemonLog(0, bytes), out, 30000)) {
        return false;
    }
    std::vector<std::string> todas = B::split(out, "\n", false);
    while (!todas.empty() && B::trim(todas.back()).empty()) {
        todas.pop_back();
    }
    if (todas.empty()) {
        std::fprintf(stderr, TC("t_log_vacio", "el registro del daemon de %s está vacío\n"),
                     pet.objetivo.connection.c_str());
        return true;
    }
    const std::size_t desde =
        todas.size() > static_cast<std::size_t>(lineas) ? todas.size() - lineas : 0;
    for (std::size_t i = desde; i < todas.size(); ++i) {
        std::fprintf(stdout, "%s\n", todas[i].c_str());
    }
    return true;
}

}  // namespace zfsmgr::cli::detail
