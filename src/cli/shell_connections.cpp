// `connect`, `disconnect` and `refresh`.

#include "shell_internal.h"

namespace zfsmgr::cli::detail {

bool cmdConectar(Estado& e, const LineaAnalizada& linea, bool conectar) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    const std::string id = idDe(e, destino);
    std::string error;
    // Con la clave sin estar, conectarla solo serviría para que `recarga` la vuelva a apartar.
    if (conectar) {
        const auto* perfil = buscarConexion(e.conns, id);
        std::string ruta;
        if (perfil && sshClaveFalta(*perfil, ruta)) {
            std::fprintf(stderr,
                         TC("t_clave_falta_connect",
                            "la clave SSH de «%s» sigue sin existir (%s): corríjala con «edit» o "
                            "ponga el fichero, y vuelva a conectarla\n"),
                         id.c_str(), ruta.c_str());
            e.ultimoRc = 1;
            return false;
        }
    }
    if (!marcarDesconectada(*e.ses, id, !conectar, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    if (!conectar) {
        // Se cierran el túnel y la caché TLS: dejar una conexión abierta contra una máquina
        // que se acaba de marcar como desconectada es exactamente lo contrario de lo que se
        // ha pedido.
        T::closeTunnelForConnection(e.ses->transporte, *buscarConexion(e.conns, id));
        T::clearRemoteDaemonTlsCacheForConnection(*buscarConexion(e.conns, id));
    }
    recarga(e);
    std::fprintf(stderr,
                 conectar ? TC("t_marcada_conectada", "%s marcada como conectada\n")
                          : TC("t_marcada_desconectada", "%s marcada como desconectada\n"),
                 id.c_str());
    return true;
}

// Refrescar: soltar lo que se tenía guardado de esa máquina y volver a preguntárselo.
//
// No es solo un listado: se cierra el túnel, se vacía la caché del material TLS, se quita
// el castigo por fallos recientes y se releen las conexiones del disco —por si la interfaz
// las ha cambiado mientras tanto—. Después se sondea. Es lo que uno espera de «refrescar»
// cuando algo se ha quedado colgado.
bool cmdRefrescar(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const ZfsmUrl& destino = pet.objetivo;
    const std::string id = idDe(e, destino);
    if (id.empty()) {
        std::fprintf(stderr, TC("t_hace_falta_901c78", "hace falta una conexión (ahora: %s)\n"),
                     textoDe(destino).c_str());
        return false;
    }
    {
        const auto* p = buscarConexion(e.conns, id);
        olvidaEstadoDeConexion(e, *p);
    }
    recarga(e);

    // Y ahora se pregunta a la máquina. Los dos verbos que la interfaz usa para lo mismo.
    std::string basicos;
    if (!agente(e, destino, PET::refreshBasics(), basicos, 30000)) {
        return false;
    }
    std::string salud;
    std::string err;
    int rc = -1;
    std::string motivo;
    const auto* p = buscarConexion(e.conns, id);
    ejecutarAgente(*e.ses, *p, {"--health"}, salud, err, rc, &motivo, 20000);

    // El IDENTIFICADOR DE MÁQUINA se guarda, no solo se enseña.
    //
    // El daemon lo publica en cada refresco y los dos clientes lo tiraban: lo leían, lo
    // pintaban y se olvidaban. Resultado, en esta instalación: `local` y `mmela` tenían uid
    // —puesto a mano hace tiempo— y `unib` y `mbp` lo tenían VACÍO.
    //
    // Hace falta porque es la única identidad de una máquina que no depende de quién la
    // mire. El nombre no lo es: `local` significa una máquina distinta según desde dónde se
    // pregunte, y la misma máquina aparece como `unib` en una lista y `unibody` en otra. Es
    // lo que `peers` necesita para que dos daemons sepan de quién hablan.
    {
        const auto claves = clavesDe(basicos);
        const auto it = claves.find("MACHINE_UUID");
        const std::string uid =
            it == claves.end() ? std::string() : B::refresh::extractMachineUuid(it->second);
        const auto* vivo = buscarConexion(e.conns, id);
        if (!uid.empty() && vivo && B::trim(vivo->machineUid) != uid && !e.ses->sinSecretos) {
            B::ConnectionProfile copia = *vivo;
            copia.machineUid = uid;
            std::string errGuardar;
            if (guardarConexion(*e.ses, copia, errGuardar)) {
                recarga(e);
            } else {
                // Se avisa y se sigue: no poder guardarlo no invalida el refresco, pero
                // callarlo dejaría a `peers` sin identidad sin que nadie supiera por qué.
                std::fprintf(stderr,
                             TC("t_uid_no_guardado",
                                "aviso: no se pudo guardar el identificador de máquina "
                                "de %s: %s\n"),
                             id.c_str(), errGuardar.c_str());
            }
        }
    }

    Tabla t;
    t.nombreJson = "refresh";
    t.cabecerasTexto = {T("t_cab_campo", "CAMPO"), T("t_cab_valor", "VALOR")};
    t.campos = {"field", "value"};
    t.tipos = {Tipo::Cadena, Tipo::Cadena};
    t.filas.push_back({"connection", id});
    t.filas.push_back({"url", textoDe(destino)});
    // Las dos salidas son «CLAVE=valor» por línea, que es el formato del agente.
    for (const std::string& bloque : {basicos, salud}) {
        for (const std::string& linea : B::split(bloque, "\n", true)) {
            const std::size_t igual = linea.find('=');
            if (igual == std::string::npos) {
                continue;
            }
            t.filas.push_back({B::toLowerAscii(B::trim(linea.substr(0, igual))),
                               B::trim(linea.substr(igual + 1))});
        }
    }
    t.imprime(e.formato);
    return true;
}

// --- Retenciones (holds) y comparación de instantáneas.

}  // namespace zfsmgr::cli::detail
