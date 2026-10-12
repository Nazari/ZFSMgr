#include "agentmsg.h"

#include "i18n.h"

#include <cstring>

namespace zfsmgr::base::agentmsg {
namespace {

// El catálogo. El código no cambia nunca una vez publicado: es el contrato con los clientes
// que ya están instalados. El texto sí puede cambiar.
struct Mensaje {
    const char* codigo;
    const char* es;
    const char* en;
};

const Mensaje kMensajes[] = {
    // Genéricos
    {"unknown_command", "el daemon no conoce la orden %1: es anterior a esta versión",
     "unknown command: %1"},
    {"usage", "llamada mal formada al daemon: %1", "usage: %1"},
    // Planificador
    {"gsa_no_schedule", "%1 no tiene ninguna programación activada",
     "%1 has no schedule enabled"},
    {"gsa_covered_by_recursive", "%1 lo cubre la programación recursiva de %2: hágalo allí",
     "%1 is covered by the recursive schedule of %2; run it there"},
    // Texto de otro programa (`zfs`), que no se puede traducir: viaja tal cual.
    {"text", "%1", "%1"},
    {"cancelled", "cancelado", "cancelled"},
    {"daemon_stopping", "el daemon se está parando", "the daemon is stopping"},
    {"local_pipe_windows", "la tubería local «zfs send | zfs recv» no está portada a Windows",
     "the local «zfs send | zfs recv» pipe is not ported to Windows"},
    // Planificador: snapshots y poda
    {"gsa_list_snapshots_failed", "no se pudieron listar los snapshots de %1: %2",
     "cannot list the snapshots of %1: %2"},
    {"gsa_snapshot_failed", "falló el snapshot de %1: %2", "zfs snapshot of %1 failed: %2"},
    {"gsa_destroy_failed", "no se pudo destruir %1: %2", "cannot destroy %1: %2"},
    // Planificador: nivelación
    {"gsa_bad_destination", "destino no válido: «%1»", "invalid destination «%1»"},
    {"gsa_source_no_snapshots", "el origen no tiene snapshots que mandar",
     "the source has no snapshots to send"},
    {"gsa_no_peer_credentials",
     "no hay credenciales para la conexión «%1» en %2: haga «peers --push» sobre esta máquina",
     "no credentials for the connection «%1» in %2; run «peers --push» on this machine"},
    {"gsa_cannot_discard_partial", "no se pudo descartar la recepción a medias en %1: %2",
     "cannot discard the partial receive on %1: %2"},
    {"gsa_resume_failed", "falló la continuación de la recepción a medias en %1: %2",
     "resuming the partial receive on %1 failed: %2"},
    {"gsa_cannot_look_at", "no se pudo consultar %1: %2", "cannot look at %1: %2"},
    {"gsa_target_unrelated",
     "%1 existe y no tiene ningún snapshot en común con el origen; no se ha mandado nada "
     "(recibir lo sobrescribiría)",
     "%1 exists and has no snapshot in common with the source; nothing was sent (receiving "
     "would overwrite it)"},
    {"gsa_target_has_own",
     "%1 tiene snapshots que el origen no tiene, por detrás del último común (%2); no se ha "
     "mandado nada: «zfs recv -F» los destruiría",
     "%1 has snapshots the source does not have after the last common one (%2); nothing was "
     "sent, «zfs recv -F» would destroy them"},
    {"gsa_send_failed", "falló el envío a %1: %2", "sending to %1 failed: %2"},
    {"gsa_target_destroy_failed", "no se pudo destruir %1 en «%2»: %3",
     "cannot destroy %1 on «%2»: %3"},
    {"gsa_cancelled_by_user", "cancelada por el usuario (trabajo %1)",
     "cancelled by the user (job %1)"},
    {"gsa_daemon_stopped", "el daemon se paró en plena nivelación",
     "the daemon stopped while levelling"},
    {"gsa_daemon_restarted", "el daemon se reinició en plena nivelación",
     "the daemon restarted while levelling"},
    // El cliente TLS de la capa base: lo usan el CLI y el daemon, cada uno en su idioma (el
    // daemon no tiene: inglés).
    {"tls_cannot_resolve", "no se pudo resolver %1", "cannot resolve %1"},
    {"tls_cannot_connect", "no se pudo conectar a %1", "cannot connect to %1"},
    {"tls_no_target", "destino sin host o sin puerto", "target without host or port"},
    {"tls_bad_server_cert", "el certificado del daemon no es un PEM válido", "the daemon certificate is not a valid PEM"},
    {"tls_bad_client_cert", "el certificado o la clave del cliente no son PEM válidos", "the client certificate or key are not valid PEM"},
    {"tls_no_context", "no se pudo crear el contexto TLS", "could not create the TLS context"},
    {"tls_key_mismatch", "el certificado y la clave del cliente no casan: %1", "the client certificate and key do not match: %1"},
    {"tls_no_session", "no se pudo crear la sesión TLS", "could not create the TLS session"},
    {"tls_handshake", "no se pudo negociar TLS: %1", "TLS handshake failed: %1"},
    {"tls_no_peer_cert", "el daemon no presentó certificado", "the daemon presented no certificate"},
    {"tls_cert_mismatch", "el certificado del daemon no es el esperado", "the daemon certificate is not the expected one"},
    {"tls_write_cut", "se cortó al enviar la petición", "the connection dropped while sending the request"},
    {"tls_wait_abandoned", "se abandonó la espera de la respuesta", "gave up waiting for the answer"},
    {"tls_no_answer", "el daemon no respondió", "the daemon did not answer"},
    // Pares y transferencias
    {"peer_unreachable", "no se llega al daemon de «%1» en %2: %3",
     "cannot reach the daemon of «%1» at %2: %3"},
    {"peer_bad_answer", "respuesta ilegible del daemon de «%1»: %2",
     "unreadable answer from the daemon of «%1»: %2"},
    {"peer_refused", "el daemon de «%1» contestó %2", "the daemon of «%1» answered %2"},
    {"peer_no_port", "el daemon de «%1» no devolvió puerto ni testigo",
     "the daemon of «%1» returned no port and token"},
    {"send_flag_not_allowed", "bandera de envío no permitida: %1", "send flag not allowed: %1"},
    {"peer_cannot_connect", "no se pudo conectar con el par %1", "cannot connect to peer %1"},
    {"send_cannot_launch", "no se pudo lanzar zfs send", "could not launch zfs send"},
    {"receiver_stalled", "el receptor dejó de leer: %1 s sin poder mandar un solo byte",
     "the receiver stopped reading: %1 s without being able to send a single byte"},
    {"receiver_failed", "el receptor falló: %1", "the receiver failed: %1"},
    {"receiver_dropped",
     "se cortó la conexión con el receptor; el registro de la máquina de destino dice por qué "
     "falló zfs recv",
     "the connection to the receiver dropped; check the log on the target machine to see why "
     "zfs recv failed"},
    {"stream_file_unreadable", "no se pudo leer el fichero de flujo %1: %2",
     "could not read the stream file %1: %2"},
    {"zfs_send_failed", "zfs send falló (salida %1): %2", "zfs send failed (exit %1): %2"},
    {"send_moved_nothing",
     "el envío terminó sin error pero no movió un solo byte: el receptor no recibió nada",
     "the send finished without error but did not move a single byte: the receiver never "
     "received anything"},
    {"job_daemon_restarted", "el daemon se reinició con el trabajo en marcha",
     "daemon restarted while running"},
    {"peer_cannot_listen",
     "el daemon de «%1» no pudo escuchar: %2 (tiene que atender en una dirección alcanzable: "
     "«peers --listen» en esa máquina)",
     "the daemon of «%1» could not listen: %2 (it must listen on a reachable address: "
     "«peers --listen» on it)"},
};

const Mensaje* busca(const std::string& codigo) {
    for (const Mensaje& m : kMensajes) {
        if (codigo == m.codigo) {
            return &m;
        }
    }
    return nullptr;
}

// «%1», «%2»… por los argumentos. De mayor a menor no hace falta: se recorre una vez.
std::string compone(const char* plantilla, const std::vector<std::string>& args) {
    std::string out;
    for (const char* p = plantilla; *p; ++p) {
        if (*p == '%' && p[1] >= '1' && p[1] <= '9') {
            std::size_t n = static_cast<std::size_t>(p[1] - '0');
            const char* q = p + 2;
            if (*q >= '0' && *q <= '9') {   // %10 y siguientes
                n = n * 10 + static_cast<std::size_t>(*q - '0');
                ++q;
            }
            if (n >= 1 && n <= args.size()) {
                out += args[n - 1];
            }
            p = q - 1;
            continue;
        }
        out.push_back(*p);
    }
    return out;
}

thread_local std::string t_ultimoCodigo;

}  // namespace

std::string inEnglish(const std::string& code, const std::vector<std::string>& args) {
    const Mensaje* m = busca(code);
    return m ? compone(m->en, args) : std::string();
}

std::string inClientLanguage(const std::string& code, const std::vector<std::string>& args) {
    const Mensaje* m = busca(code);
    if (!m) {
        return {};
    }
    // El castellano está aquí mismo; el inglés también. Otro idioma, por el catálogo, con el
    // inglés detrás si no lo tiene.
    const std::string& idioma = i18n::language();
    if (idioma == "es") {
        return compone(m->es, args);
    }
    if (idioma.empty() || idioma == "en") {
        return compone(m->en, args);
    }
    return compone(i18n::tr(std::string("t_ag_") + m->codigo, m->en).c_str(), args);
}

bool isKnown(const std::string& code) { return busca(code) != nullptr; }

const std::string& lastCode() { return t_ultimoCodigo; }
void setLastCode(const std::string& code) { t_ultimoCodigo = code; }

}  // namespace zfsmgr::base::agentmsg
