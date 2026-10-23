#include "agentmsg.h"

#include "i18n.h"
#include "json.h"

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
    {"receiver_no_verdict",
     "el receptor cerró sin decir cómo le fue: se cortó o murió; el registro de la máquina de "
     "destino dice por qué",
     "the receiver closed without reporting how it went: it dropped or died; check the log on "
     "the target machine"},
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
    {"gsa_pull_origin_here",
     "el origen %1 está en esta misma máquina: para eso, una programación con --to en el origen",
     "the origin %1 is on this same machine: for that, a schedule with --to on the origin"},
    {"gsa_pull_hold_failed", "no se pudo retener %1 en el origen: %2",
     "could not hold %1 on the source: %2"},
    {"gsa_pull_has_own",
     "%1 tiene snapshots propios posteriores al último común con el origen (%2); no se trae "
     "nada para no tocarlos",
     "%1 has snapshots of its own after the last one shared with the source (%2); nothing is "
     "pulled, so as not to touch them"},
    {"gsa_pull_unrelated",
     "%1 existe y no comparte ningún snapshot con el origen; no se trae nada para no tocarlo",
     "%1 exists and shares no snapshot with the source; nothing is pulled, so as not to touch it"},
    {"verb_not_for_puller",
     "«%1» no se le permite a una credencial de traer: solo puede mirar, enviar y retener lo suyo",
     "«%1» is not allowed for a pull credential: it may only look, send and hold its own"},
    {"pull_send_failed", "el envío desde «%1» falló: %2", "the send from «%1» failed: %2"},
    {"pull_recv_failed", "la recepción en %1 falló: %2", "receiving into %1 failed: %2"},
    {"pull_stream_cut", "el flujo desde «%1» se cortó a medias", "the stream from «%1» was cut half-way"},
    {"pull_not_on_windows", "traer un flujo a esta máquina aún no está hecho en Windows",
     "pulling a stream into this machine is not done on Windows yet"},
    {"peer_cannot_listen",
     "el daemon de «%1» no pudo escuchar: %2 (tiene que atender en una dirección alcanzable: "
     "«peers --listen» en esa máquina)",
     "the daemon of «%1» could not listen: %2 (it must listen on a reachable address: "
     "«peers --listen» on it)"},
    // Desglosar y ensamblar
    {"breakdown_windows", "Desglosar no está disponible en Windows: sus datasets se montan planos bajo la letra del pool y no admiten punto de montaje propio, así que el dataset nuevo no quedaría donde estaba el directorio",
     "Breakdown is not available on Windows: its datasets are mounted flat under the pool's drive letter and do not accept their own mountpoint, so the new dataset would not end up where the directory was"},
    {"mountpoint_none", "el dataset tiene mountpoint=none",
     "mountpoint=none"},
    {"breakdown_empty_name", "nombre de dataset vacío para %1",
     "empty dataset name for %1"},
    {"invalid_dataset_name", "nombre de dataset no válido: %1",
     "invalid dataset name: %1"},
    {"name_bad_char", "carácter que ZFS no admite en el nombre: %1",
     "character not allowed by ZFS in the name: %1"},
    {"breakdown_duplicate_name", "dos directorios piden el mismo nombre de dataset: %1",
     "two directories ask for the same dataset name: %1"},
    {"breakdown_no_dirs", "no se ha dado ningún directorio",
     "no directory was given"},
    {"cancelled_before_changes", "cancelado por el usuario antes de cambiar nada",
     "cancelled by the user before changing anything"},
    {"temp_mountpoint_failed", "no se pudo crear el punto de montaje temporal",
     "could not create the temporary mountpoint"},
    {"temp_name_failed", "no se pudo generar el nombre del dataset temporal",
     "could not generate the temporary dataset name"},
    {"temp_not_mounted", "el dataset temporal no quedó montado en ningún sitio",
     "the temporary dataset was not left mounted anywhere"},
    {"verify_failed", "la verificación de %1 falló: quedan %2 elementos por copiar",
     "verification of %1 failed: %2 items pending"},
    {"breakdown_cannot_unmount", "no se pudo desmontar %1, montado en %2, que está dentro de un directorio a convertir. No se ha borrado nada. %3",
     "could not unmount %1, mounted at %2, which is inside a directory to convert. Nothing has been deleted. %3"},
    {"cannot_stat", "no se pudo consultar %1",
     "could not stat %1"},
    {"cannot_remove", "no se pudo borrar %1",
     "could not remove %1"},
    {"breakdown_rename_failed", "%1\nLos datos están a salvo en datasets temporales bajo %2. Renómbrelos a mano; NO los destruya:%3",
     "%1\nThe data is safe in temporary datasets under %2. Rename them by hand; do NOT destroy them:%3"},
    {"breakdown_mountpoint_pending", "%1\nEl dataset %2 tiene los datos; le falta mountpoint=%3",
     "%1\nThe dataset %2 has the data; it still needs mountpoint=%3"},
    {"assemble_not_child", "el dataset %1 no cuelga de %2: no se ensambla",
     "the dataset %1 does not hang from %2; not assembled"},
    {"assemble_outside", "el dataset %1 está montado en %2, fuera de %3: no se ensambla",
     "the dataset %1 is mounted at %2, outside %3; not assembled"},
    {"cancelled_by_user", "cancelado por el usuario",
     "cancelled by the user"},
    {"temp_dir_failed", "no se pudo crear el directorio temporal en %1",
     "could not create the temporary directory in %1"},
    {"assemble_verify_failed", "la verificación de %1 falló: quedan %2 elementos por copiar (el dataset no se destruye)",
     "verification of %1 failed: %2 items pending (the dataset is not destroyed)"},
    {"cannot_find_mountpoint", "no se pudo averiguar el punto de montaje de %1",
     "could not determine the mountpoint of %1"},
    {"cannot_create_dirs", "no se pudieron crear los directorios",
     "could not create the directories"},
    {"assemble_no_children", "ninguno de los nombres dados es un dataset hijo de este; van por su nombre COMPLETO, no relativo",
     "none of the given names is a child dataset of this one; they go by their FULL name, not a relative one"},
    {"invalid_payload", "carga no válida",
     "invalid generic payload"},
    // Hacia Dir, ficheros y montajes temporales
    {"internal", "error interno del daemon: %1",
     "internal error: %1"},
    {"base_not_absolute", "la base tiene que ser una ruta absoluta",
     "the base must be an absolute path"},
    {"snapshot_not_found", "no existe la instantánea %1", "the snapshot %1 does not exist"},
    {"remove_dataset_root",
     "eso es la raíz del dataset: para borrarlo entero, «destroy» del dataset",
     "that is the root of the dataset: to delete all of it, «destroy» the dataset"},
    {"remove_in_zfs_dir", "dentro de .zfs están las instantáneas, que son de solo lectura",
     "inside .zfs are the snapshots, which are read-only"},
    {"remove_not_dataset_mount", "%1 no está montado en %2",
     "%1 is not mounted at %2"},
    {"remove_not_found", "no existe: %1", "does not exist: %1"},
    {"remove_is_dir", "%1 es un directorio: «-r» para borrarlo con lo que tenga dentro",
     "%1 is a directory: «-r» deletes it with everything inside"},
    {"remove_is_mountpoint",
     "en %1 hay montado otro sistema de ficheros (un dataset hijo): eso se destruye como dataset",
     "another filesystem (a child dataset) is mounted at %1: destroy it as a dataset"},
    {"remove_failed", "no se pudo borrar: %1", "could not delete: %1"},
    {"remove_tree_not_absolute", "%1 no es una ruta absoluta limpia: no se borra",
     "%1 is not a clean absolute path: it is not deleted"},
    {"remove_tree_too_high", "%1 está demasiado arriba para borrarlo entero",
     "%1 is too high up to delete whole"},
    {"remove_tree_not_dir", "%1 no es un directorio (o es un enlace): no se borra",
     "%1 is not a directory (or it is a link): it is not deleted"},
    {"remove_tree_in_zfs", "%1 está dentro de un dataset: para convertirlo en uno, «breakdown»",
     "%1 is inside a dataset: to make it one, «breakdown»"},
    {"remove_tree_not_on_windows", "borrar el directorio de origen aún no se hace en Windows",
     "deleting the source directory is not done on Windows yet"},
    {"remove_not_on_windows", "borrar ficheros de un dataset aún no se hace en Windows",
     "deleting files of a dataset is not done on Windows yet"},
    {"bad_relative_part", "la parte relativa no puede estar vacía, ser absoluta ni contener «..»",
     "the relative part cannot be empty, absolute or contain «..»"},
    {"base_not_dir", "la base no existe o no es un directorio: %1",
     "the base does not exist or is not a directory: %1"},
    {"cannot_create", "no se pudo crear %1",
     "could not create %1"},
    {"empty_dataset", "falta el dataset",
     "empty dataset"},
    {"bad_subdir", "subdirectorio relativo no válido",
     "invalid relative subdirectory"},
    {"cannot_resolve_mountpoint", "no se pudo averiguar el punto de montaje efectivo",
     "could not resolve effective mountpoint"},
    {"cannot_create_target_dir", "no se pudo crear el directorio de destino",
     "cannot create destination directory"},
    {"target_not_absolute", "el directorio de destino tiene que ser una ruta absoluta",
     "destination directory must be an absolute path"},
    {"todir_target_is_mountpoint", "el destino %1 es el punto de montaje del dataset %2",
     "the target %1 is the mountpoint of the dataset %2"},
    {"cannot_resolve_source", "no se pudo averiguar el dataset de origen",
     "could not resolve the source dataset"},
    {"todir_outside_descendants", "No se puede convertir %1 en un directorio: tiene descendientes montados FUERA de %2, así que su contenido no iría al destino y además impediría destruirlo:\n%3Mueva o desglose antes esos datasets, o elija otro destino.",
     "Cannot turn %1 into a directory: it has descendants mounted OUTSIDE %2, so their contents would not go to the target and would also prevent destroying it:\n%3Move or break down those datasets first, or choose another target."},
    {"todir_cannot_unmount", "no se pudo desmontar %1, que está dentro de %2; no se ha cambiado nada. %3",
     "could not unmount %1, which is inside %2; nothing has been changed. %3"},
    {"cannot_create_target_parent", "no se pudo crear el directorio padre del destino",
     "cannot create destination parent directory"},
    {"cannot_create_staging", "no se pudo crear el directorio de paso junto al destino",
     "cannot create staging directory next to destination"},
    {"todir_cannot_unmount_target", "no se pudo desmontar %1, que está dentro del destino; no se ha cambiado nada. %2",
     "could not unmount %1, which is inside the target; nothing has been changed. %2"},
    {"cannot_move_aside", "no se pudo apartar el destino que ya existía",
     "cannot move existing destination aside"},
    {"cannot_move_into_place", "no se pudo poner en su sitio lo copiado",
     "cannot move copied data into place"},
    {"todir_cannot_destroy", "%1\nLos datos ya están copiados en %2; el dataset %3 no se pudo destruir y sigue ahí.",
     "%1\nThe data is already copied to %2; the dataset %3 could not be destroyed and is still there."},
    {"todir_cannot_destroy_backup", "%1\nLos datos ya están copiados en %2; el dataset %3 no se pudo destruir y sigue ahí.\nEl contenido anterior de %2 se guardó en %4",
     "%1\nThe data is already copied to %2; the dataset %3 could not be destroyed and is still there.\nThe previous contents of %2 were saved in %4"},
    {"paths_not_absolute", "las rutas tienen que ser absolutas",
     "paths must be absolute"},
    {"cannot_read_dir", "no se pudo leer el directorio: %1",
     "could not read the directory: %1"},
    {"ro_mount_windows", "el montaje temporal de solo lectura no está disponible en Windows",
     "a temporary read-only mount is not available on Windows"},
    {"ro_mount_macos", "el montaje temporal de solo lectura no está disponible en macOS: su mount_zfs ignora el solo lectura y el montaje quedaría escribible",
     "a temporary read-only mount is not available on macOS: its mount_zfs ignores read-only and the mount would be writable"},
    {"need_filesystem", "hace falta un dataset de tipo filesystem",
     "a filesystem dataset is required"},
    {"temp_dir_failed_errno", "no se pudo crear el directorio temporal: %1",
     "cannot create the temporary directory: %1"},
    {"ro_mount_failed", "no se pudo montar %1 en solo lectura: %2",
     "cannot mount %1 read-only: %2"},
    // Ficheros, árboles, dispositivos y configuración
    {"chunk_too_big", "un trozo no puede pasar de 8 MiB",
     "a chunk cannot exceed 8 MiB"},
    {"cannot_open_file", "no se pudo abrir el fichero",
     "could not open the file"},
    {"file_too_big", "el fichero pasa de 8 MiB: pídalo a trozos con «<desde> <cuántos>»",
     "the file exceeds 8 MiB: ask for it in chunks with «<from> <count>»"},
    {"dir_missing", "falta el directorio",
     "the directory is missing"},
    {"not_a_dir", "no es un directorio: %1",
     "not a directory: %1"},
    {"token_failed", "no se pudo generar el testigo de la transferencia",
     "could not generate the transfer token"},
    {"as_needs_name", "--as lleva un nombre, no una ruta: %1",
     "--as takes a plain name, not a path: %1"},
    {"delete_single_file", "--delete no se aplica a un fichero suelto: %1",
     "--delete does not apply to a single file: %1"},
    {"as_single_file", "--as solo se aplica a un fichero suelto: %1",
     "--as only applies to a single file: %1"},
    {"tree_greet_failed", "no se pudo saludar al receptor",
     "could not greet the receiver"},
    {"tree_dropped", "se cortó la conexión con el receptor durante la copia del árbol",
     "the tree transfer dropped"},
    {"tree_partial", "no se aplicaron %1 de %2 operaciones; la primera: %3",
     "%1 of %2 operations could not be applied; the first: %3"},
    {"tree_refused_dir", "no se sustituye el directorio con contenido «%1» sin --delete",
     "not replacing the non-empty directory «%1» without --delete"},
    {"tree_unreadable_source", "no se pudo leer «%1» en el origen; no se borró nada de lo que cuelga de él",
     "could not read «%1» at the source; nothing under it was deleted"},
    {"dir_not_exists", "el directorio no existe: %1",
     "the directory does not exist: %1"},
    {"is_a_dir", "es un directorio: %1",
     "it is a directory: %1"},
    {"file_exists", "el fichero ya existe: %1",
     "the file already exists: %1"},
    {"not_regular_file", "no es un fichero normal: %1",
     "not a regular file: %1"},
    {"lsblk_unreadable", "no se pudo leer la salida de lsblk: %1",
     "could not parse lsblk output: %1"},
    {"devices_not_ported", "la lista de dispositivos aún no está portada a esta plataforma",
     "--dump-block-devices is not ported to this platform yet"},
    {"labelclear_windows", "zpool labelclear no está disponible en Windows",
     "zpool labelclear is not supported on Windows"},
    {"no_such_device", "no existe el dispositivo: %1",
     "no such device: %1"},
    {"not_device_or_file", "no es un dispositivo ni un fichero: %1",
     "not a device or file: %1"},
    {"path_outside_zfs", "la ruta no está dentro de ningún punto de montaje de ZFS",
     "the path is not inside any ZFS mountpoint"},
    {"not_on_windows", "no está disponible en Windows",
     "not supported on Windows"},
    {"peers_bad_json", "peers.json no es JSON válido: %1",
     "peers.json is not valid JSON: %1"},
    {"cannot_write", "no se pudo escribir %1",
     "could not write %1"},
    {"cannot_put_in_place", "no se pudo poner en su sitio %1",
     "could not put in place %1"},
    {"bind_not_allowed", "solo se admiten 0.0.0.0, :: o 127.0.0.1: el cliente llega por un túnel a 127.0.0.1 y cualquier otra dirección le cortaría el acceso",
     "only 0.0.0.0, :: or 127.0.0.1 are allowed: the client arrives through a tunnel to 127.0.0.1 and any other address would cut off its access"},
    {"cannot_read", "no se pudo leer %1",
     "could not read %1"},
    {"copy_verify_failed", "no se pudo verificar la copia",
     "could not verify the copy"},
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

std::string render(const std::string& code, const std::vector<std::string>& args) {
    const std::string frase = inClientLanguage(code, args);
    if (!frase.empty()) {
        return frase;
    }
    std::string out = code;
    for (std::size_t i = 0; i < args.size(); ++i) {
        out += (i ? ", " : ": ") + args[i];
    }
    return out;
}

const std::string& lastCode() { return t_ultimoCodigo; }
void setLastCode(const std::string& code) { t_ultimoCodigo = code; }

std::string renderJsonArgs(const std::string& code, const std::string& argsJson) {
    if (code.empty()) {
        return {};
    }
    std::vector<std::string> args;
    json::Value v;
    std::string err;
    if (json::parse(argsJson.empty() ? "[]" : argsJson, v, &err)) {
        for (const auto& a : v.toArray()) {
            args.push_back(a.toString());
        }
    }
    return render(code, args);
}

}  // namespace zfsmgr::base::agentmsg
