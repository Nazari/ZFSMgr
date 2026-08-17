#include "ayuda.h"

#include "strutil.h"
#include "tr.h"

#include <cstdio>
#include <cstring>

namespace zfsmgr::cli {
namespace {

namespace B = zfsmgr::base;

// El ancho de la columna de la izquierda. Fijo y no calculado sobre la orden más larga:
// con `create <pool> <dispositivo>...` la columna se comería media pantalla y todo lo
// demás quedaría desplazado por una sola línea.
constexpr int kColumna = 34;

// Cuántas COLUMNAS ocupa un texto. Contando caracteres, no bytes: «instantánea» tiene
// tildes, y alinear por bytes descuadra justo las descripciones en castellano — es el mismo
// error que ya apareció al imprimir tablas.
std::size_t anchoVisible(const std::string& s) {
    std::size_t n = 0;
    for (const char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

// Parte un texto en líneas que quepan, sin cortar palabras.
std::vector<std::string> parte(const std::string& texto, std::size_t ancho) {
    std::vector<std::string> lineas;
    std::string actual;
    for (const std::string& palabra : B::split(texto, " ", true)) {
        if (!actual.empty() && anchoVisible(actual) + 1 + anchoVisible(palabra) > ancho) {
            lineas.push_back(actual);
            actual.clear();
        }
        if (!actual.empty()) {
            actual += " ";
        }
        actual += palabra;
    }
    if (!actual.empty()) {
        lineas.push_back(actual);
    }
    if (lineas.empty()) {
        lineas.push_back(std::string());
    }
    return lineas;
}

// Una entrada: a la izquierda la invocación, a la derecha la explicación partida. Si la
// izquierda se pasa de la columna, la explicación baja a la línea siguiente en vez de
// empujarla: descuadrar una fila es peor que gastar un renglón.
void fila(const std::string& izquierda, const std::string& derecha, int sangria, int ancho) {
    const std::size_t hueco = static_cast<std::size_t>(kColumna - sangria);
    const std::size_t anchoDer =
        static_cast<std::size_t>(ancho > kColumna + 20 ? ancho - kColumna - 1 : 40);
    const std::vector<std::string> trozos = parte(derecha, anchoDer);
    const std::string margen(static_cast<std::size_t>(sangria), ' ');
    if (anchoVisible(izquierda) >= hueco) {
        std::fprintf(stderr, "%s%s\n", margen.c_str(), izquierda.c_str());
        for (const std::string& t : trozos) {
            if (!t.empty()) {
                std::fprintf(stderr, "%*s%s\n", kColumna, "", t.c_str());
            }
        }
        return;
    }
    std::fprintf(stderr, "%s%s%*s%s\n", margen.c_str(), izquierda.c_str(),
                 static_cast<int>(hueco - anchoVisible(izquierda)), "",
                 trozos.empty() ? "" : trozos.front().c_str());
    for (std::size_t i = 1; i < trozos.size(); ++i) {
        std::fprintf(stderr, "%*s%s\n", kColumna, "", trozos[i].c_str());
    }
}

void imprimeOrden(const Orden& o, int ancho, bool conDetalle) {
    const std::string uso =
        std::string(o.nombre)
        + (o.uso.es && *o.uso.es ? " " + T(o.uso.clave, o.uso.es) : std::string());
    fila(uso, T(o.resumen.clave, o.resumen.es), 2, ancho);
    // Los parámetros van DEBAJO y tabulados, uno por línea. Metidos en la misma línea que
    // la orden, una con cinco opciones ocupaba tres renglones sin que se viera cuál es
    // cuál.
    for (const Parametro& p : o.params) {
        fila(T(p.forma.clave, p.forma.es), T(p.que.clave, p.que.es), 6, ancho);
    }
    if (conDetalle) {
        for (const Texto& d : o.detalle) {
            std::fprintf(stderr, "\n");
            for (const std::string& t :
                 parte(T(d.clave, d.es), static_cast<std::size_t>(ancho > 24 ? ancho - 4 : 60))) {
                std::fprintf(stderr, "  %s\n", t.c_str());
            }
        }
    }
}

const std::vector<Orden> kOrdenes = {
    // --- Navegación
    {"cd", {"t_navegaci_n_60cb06", "Navegación"}, {"t_destino_132a32", "[destino]"}, {"t_cambia_de__33cb85", "Cambia de sitio. Sin argumento, a la raíz."},
     {{{"t_destino_bb3347", "<destino>"},
       {"t_ruta_relat_fb879d", "Ruta relativa, absoluta (/OldLau/winpool), URL completa, «..», «.» o «-» (el sitio "
       "anterior)."}}},
     {{"t_la_posici__ad7fe5", "La posición es una URL, y todas las órdenes actúan sobre ella. Eso hace que "
      "cualquier orden se pueda copiar del historial, ponerle --on <url> y ejecutarla "
      "suelta."},
      {"t_dos_reglas_8d26b6", "Dos reglas quitan ambigüedad a una ruta relativa: si el primer tramo nombra una "
      "CONEXIÓN, la ruta es absoluta; y si es el POOL en el que ya estás, es el nombre ZFS "
      "completo."},
      {"t_se_comprue_d745a0", "Se comprueba que el destino EXISTA, como el cd de cualquier intérprete."}}},
    {"pwd", {"t_navegaci_n_60cb06", "Navegación"}, {"", ""}, {"t_la_url_act_1bd7d8", "La URL actual."}, {}, {}},
    {"ls", {"t_navegaci_n_60cb06", "Navegación"}, {"t_destino_132a32", "[destino]"},
     {"t_lista_lo_q_661d7f", "Lista lo que hay. En la raíz las conexiones, en una conexión los pools, en un dataset "
     "sus hijos e instantáneas."},
     {{{"t_content_ru_7a5f3d", "#content[/ruta]"}, {"t_los_ficher_2b8359", "Los ficheros de dentro."}},
      {{"t_properties_5ce56d", "#properties[/prop]"}, {"t_las_propie_137429", "Las propiedades."}},
      {{"t_permission_226be2", "#permissions"}, {"t_los_permis_7d7f8b", "Los permisos delegados."}},
      {{"t_ls_daemon", "--daemon"},
       {"t_ls_daemon_q", "En la raíz: la versión del agente de cada máquina, con « * » si no es la esperada."}}},
     {{"t_en_windows_42a2d3", "En Windows el contenido no está donde dice el «mountpoint»: el pool se monta en una "
      "letra de unidad y los descendientes heredan la del POOL. Se traduce solo."}}},
    {"info", {"t_navegaci_n_60cb06", "Navegación"}, {"t_destino_132a32", "[destino]"}, {"t_qu_hay_aqu_66e605", "Qué hay aquí y estado del daemon."}, {}, {}},

    // --- Conexiones
    {"create", {"t_conexiones_3785cd", "Conexiones y pools"}, {"t_create_uso2", "<nombre>|@<nombre> …"},
     {"t_create_res2", "Crea un nodo DONDE ESTÁS: en la raíz una conexión, en una conexión un pool, en un "
     "dataset un hijo, y con « @ » delante una instantánea."},
     {{{"t_name_type__f51f24", "--name / --type / --os"}, {"t_conexi_n_n_42c7eb", "Conexión: nombre visible, LOCAL o SSH, sistema."}},
      {{"t_host_port__2ddcc9", "--host / --port / --user / --key"}, {"t_conexi_n_c_c0e9ec", "Conexión: cómo se llega a la máquina."}},
      {{"t_sudo_d34723", "--sudo"}, {"t_conexi_n_l_0e9498", "Conexión: la máquina necesita elevar."}},
      {{"t_password_f_33329f", "--password-fd <n>"}, {"t_conexi_n_l_ffa8ae", "Conexión: la contraseña, por descriptor."}},
      {{"t_dispositiv_538150", "<dispositivo>..."}, {"t_pool_en_cu_472c48", "Pool: en cuáles se crea. SE ESCRIBEN."}},
      {{"t_o_p_v_o_p__3f8555", "-o p=v / -O p=v / --mountpoint"}, {"t_pool_propi_383914", "Pool: propiedades y punto de montaje."}},
      {{"t_f_0abbcb", "-f"}, {"t_pool_fuerz_68fe4a", "Pool: fuerza aunque parezcan en uso."}},
      {{"t_prop_valor_4ef240", "prop=valor"}, {"t_dataset_pr_545aca", "Dataset: propiedades del hijo."}},
      {{"t_create_arroba", "@<nombre>"}, {"t_create_arroba_q", "Instantánea del dataset donde está."}},
      {{"t_r_90cdb7", "-r"}, {"t_tambi_n_de_5714fd", "Instantánea: también de los descendientes."}}},
     {{"t_la_contras_46d37f", "La contraseña NUNCA se pasa por argumento: iría en argv y se vería en `ps` para "
      "cualquier usuario de la máquina. O se teclea, o entra por un descriptor."},
      {"t_se_guarda__a5a7bd", "Se guarda cifrada con la contraseña maestra. Sin ella no se guarda en claro."},
      {"t_crear_un_p_9b7a8c", "Crear un POOL es la orden más destructiva de todas: escribe en los dispositivos que "
      "se le den y lo que hubiera en ellos se pierde. La confirmación los enumera uno a "
      "uno."},
      {"t_create_arroba_det", "Un nombre con « @ » delante crea una instantánea y no un hijo: `create @ayer` "
      "sobre `tank/datos` deja `tank/datos@ayer`. Es el mismo marcador que distingue una "
      "instantánea en la URL, así que no hay una regla nueva que recordar."}}},
    {"devices", {"t_conexiones_3785cd", "Conexiones y pools"}, {"t_devices_uso", "[--free]"},
     {"t_devices_res", "Los discos de la máquina, para elegir dónde crear un pool."},
     {{{"t_devices_free", "--free"}, {"t_devices_free_q", "Solo los que no están en uso."}}},
     {{"t_devices_det", "Salen todos, con la columna OCUPADO, y no solo los libres: esconder los "
      "ocupados escondería justo el disco que uno va a reutilizar a propósito —el de un "
      "pool viejo, por ejemplo—."},
      {"t_devices_det2", "OCUPADO es una comodidad, no un veredicto: dice que el dispositivo o "
      "alguno de sus hijos tiene sistema de ficheros o está montado. Quien vaya a escribir "
      "en él decide."}}},
    {"edit", {"t_conexiones_3785cd", "Conexiones y pools"}, {"t_name_host_73f07e", "[--name …] [--host …] …"},
     {"t_cambia_una_58f563", "Cambia una conexión. Pulsar Intro conserva el valor actual."},
     {{{"t_password_76e3cd", "--password"}, {"t_pide_una_c_dbc5c6", "Pide una contraseña nueva. Sin ella, se conserva la que había."}}},
     {}},
    {"destroy", {"t_conexiones_3785cd", "Conexiones y pools"}, {"t_destino_r__8b306f", "[destino] [-r|-R] [-f]"},
     {"t_destruye_l_77f9b5", "Destruye lo que hay DONDE ESTÁS. Pide confirmación siempre."},
     {{{"t_r_90cdb7", "-r"}, {"t_con_sus_de_7eb43b", "Con sus descendientes."}},
      {{"t_r_24fd93", "-R"}, {"t_con_sus_de_b90f87", "Con sus descendientes y lo que dependa de ellos."}},
      {{"t_f_0abbcb", "-f"}, {"t_fuerza_aun_e2d851", "Fuerza aunque esté en uso."}}},
     {{"t_en_una_con_d7ff22", "En una CONEXIÓN la quita de la configuración y no toca nada en la máquina. En un "
      "POOL es `zpool destroy` — `zfs destroy` sobre el dataset raíz de un pool no "
      "funciona—. En un dataset o instantánea, `zfs destroy`."}}},
    {"connect", {"t_conexiones_3785cd", "Conexiones y pools"}, {"t_destino_132a32", "[destino]"}, {"t_marca_la_c_c52a74", "Marca la conexión como usable."}, {}, {}},
    {"disconnect", {"t_conexiones_3785cd", "Conexiones y pools"}, {"t_destino_132a32", "[destino]"},
     {"t_la_aparta__62eeb8", "La aparta: el intérprete deja de hablar con ella y se cierra su túnel."},
     {},
     {{"t_es_la_mism_796aa1", "Es la MISMA marca que usa la interfaz gráfica. Navegar hasta una conexión apartada sí "
      "se permite, porque hay que poder llegar para volver a conectarla."}}},
    {"refresh", {"t_conexiones_3785cd", "Conexiones y pools"}, {"t_destino_132a32", "[destino]"},
     {"t_suelta_t_n_bedff9", "Suelta túnel, material TLS y castigos, relee la configuración y vuelve a sondear."},
     {},
     {{"t_no_es_un_l_2b8c41", "No es un listado: es lo que hay que hacer cuando algo se ha quedado colgado."}}},

    // --- Dataset
    {"rename", {"t_dataset_105268", "Dataset"}, {"t_nuevo_dcceab", "<nuevo>"}, {"t_renombra_e_e71b10", "Renombra el dataset."}, {}, {}},
    {"mount", {"t_dataset_105268", "Dataset"}, {"t_f_9bd72b", "[-f]"}, {"t_lo_monta_6d9042", "Lo monta."}, {}, {}},
    {"unmount", {"t_dataset_105268", "Dataset"}, {"t_f_9bd72b", "[-f]"}, {"t_lo_desmont_a9975c", "Lo desmonta."}, {}, {}},
    {"promote", {"t_dataset_105268", "Dataset"}, {"", ""}, {"t_promueve_u_eb988f", "Promueve un clon a dataset independiente."}, {}, {}},
    {"get", {"t_dataset_105268", "Dataset"}, {"t_propiedad_77632b", "[propiedad]"}, {"t_lee_las_pr_521610", "Lee las propiedades. Sin nombre, todas."}, {}, {}},
    {"set", {"t_dataset_105268", "Dataset"}, {"t_prop_valor_b7871d", "<prop>=<valor> [más...]"}, {"t_escribe_pr_b449c1", "Escribe propiedades."}, {}, {}},
    {"load-key", {"t_dataset_105268", "Dataset"}, {"", ""}, {"t_carga_la_c_0013a3", "Carga la clave de cifrado. La frase se teclea."}, {}, {}},
    {"unload-key", {"t_dataset_105268", "Dataset"}, {"", ""}, {"t_descarga_l_d86fbd", "Descarga la clave de cifrado."}, {}, {}},

    // --- Instantáneas
    {"rollback", {"t_instant_ne_bff51f", "Instantáneas"}, {"t_nombre_f_r_74bf0b", "[@<nombre>] [-f|-r|-R]"},
     {"t_vuelve_el__e58a57", "Vuelve el dataset al estado de una instantánea, DESCARTANDO lo posterior."}, {}, {}},
    {"clone", {"t_instant_ne_bff51f", "Instantáneas"}, {"t_nuevo_from_463e13", "<nuevo> [--from <@instantánea>]"},
     {"t_crea_un_da_97befd", "Crea un dataset a partir de una instantánea."},
     {{{"t_from_inst_17782f", "--from <@inst>"}, {"t_cu_l_se_cl_b311bf", "Cuál se clona. Sin ella, el sitio actual."}}}, {}},
    {"holds", {"t_instant_ne_bff51f", "Instantáneas"}, {"t_destino_132a32", "[destino]"}, {"t_las_retenc_db1367", "Las retenciones de una instantánea."}, {}, {}},
    {"hold", {"t_instant_ne_bff51f", "Instantáneas"}, {"t_etiqueta_r_8becce", "<etiqueta> [-r]"},
     {"t_pone_una_r_c46735", "Pone una retención: impide borrarla hasta quitarla."}, {}, {}},
    {"release", {"t_instant_ne_bff51f", "Instantáneas"}, {"t_etiqueta_r_8becce", "<etiqueta> [-r]"}, {"t_quita_una__478a77", "Quita una retención."}, {}, {}},
    {"diff", {"t_instant_ne_bff51f", "Instantáneas"}, {"t_hasta_from_64dcd2", "<@hasta> [--from <@desde>]"},
     {"t_qu_cambi_e_bca99a", "Qué cambió entre dos puntos del mismo dataset."},
     {{{"t_from_inst_17782f", "--from <@inst>"}, {"t_el_punto_d_3efe61", "El punto de partida. Sin ella, el sitio actual."}}}, {}},

    // --- Pools
    {"status", {"t_pools_2fd96d", "Pools"}, {"", ""}, {"t_el_estado__f8428b", "El estado detallado del pool, tal y como lo da zpool."}, {}, {}},
    {"history", {"t_pools_2fd96d", "Pools"}, {"", ""}, {"t_qu_se_le_h_a13b5d", "Qué se le ha hecho al pool y cuándo."}, {}, {}},
    {"scrub", {"t_pools_2fd96d", "Pools"}, {"t_stop_pause_28a6de", "[stop|pause]"}, {"t_verifica_t_9c1250", "Verifica todo el contenido del pool."}, {}, {}},
    {"trim", {"t_pools_2fd96d", "Pools"}, {"t_stop_pause_fda1c2", "[stop|pause] [<vdev>]"}, {"t_avisa_a_lo_5d27bd", "Avisa a los discos de qué bloques sobran."},
     {}, {}},
    {"initialize", {"t_pools_2fd96d", "Pools"}, {"t_stop_pause_fda1c2", "[stop|pause] [<vdev>]"}, {"t_escribe_en_8e9d25", "Escribe en el espacio no usado."}, {},
     {}},
    {"clear", {"t_pools_2fd96d", "Pools"}, {"t_vdev_a3e4bf", "[<vdev>]"}, {"t_pone_a_cer_41359a", "Pone a cero los errores contados."}, {}, {}},
    {"flush", {"t_pools_2fd96d", "Pools"}, {"", ""},
     {"t_flush_res", "Fuerza la escritura de lo pendiente del pool (`zpool sync`)."},
     {}, {{"t_flush_det", "Se llamaba `sync`, que es como se llama en `zpool`. Se cambió porque en la "
      "interfaz «Sincronizar» es OTRA cosa —copiar el contenido de un dataset a otro con "
      "rsync—, y tener la misma palabra para las dos era una trampa: la de pool tarda un "
      "instante y la otra puede tardar horas."}}},
    {"upgrade", {"t_pools_2fd96d", "Pools"}, {"", ""}, {"t_sube_la_ve_b9cca7", "Sube la versión del pool. NO se puede deshacer."}, {}, {}},
    {"reguid", {"t_pools_2fd96d", "Pools"}, {"", ""}, {"t_cambia_el__4a3340", "Cambia el identificador único del pool."}, {}, {}},
    {"export", {"t_pools_2fd96d", "Pools"}, {"t_f_9bd72b", "[-f]"}, {"t_lo_desmont_64239f", "Lo desmonta y lo suelta, para llevarlo a otra máquina."}, {},
     {}},
    {"import", {"t_pools_2fd96d", "Pools"}, {"t_pool_as_nu_2706a5", "[<pool>] [--as <nuevo>] [-f]"},
     {"t_importa_un_2c9f21", "Importa un pool. Sin nombre, enseña los que hay disponibles."},
     {{{"t_as_nuevo_c017c7", "--as <nuevo>"}, {"t_lo_importa_bd9394", "Lo importa con otro nombre."}}}, {}},

    // --- Permisos
    {"allow", {"t_permisos_d_3db5da", "Permisos delegados"}, {"t_user_u_per_9cf888", "[--user <u>] <permisos...>"},
     {"t_delega_per_60be91", "Delega permisos. Sin argumentos, los LISTA."},
     {{{"t_user_u_gro_b011b8", "--user <u> / --group <g>"}, {"t_a_qui_n_1cc417", "A quién."}},
      {{"t_everyone_0de5e3", "--everyone"}, {"t_a_todos_2ed0f3", "A todos."}},
      {{"t_set_nombre_828345", "--set @<nombre>"}, {"t_a_un_conju_58094e", "A un conjunto con nombre."}},
      {{"t_local_91441b", "--local"}, {"t_solo_en_es_4019d0", "Solo en este dataset."}},
      {{"t_descend_8f0ee4", "--descend"}, {"t_solo_en_lo_9b33ba", "Solo en los descendientes."}},
      {{"t_create_488177", "--create"}, {"t_solo_en_lo_9dcb23", "Solo en los que se creen a partir de ahora."}}},
     {}},
    {"unallow", {"t_permisos_d_3db5da", "Permisos delegados"}, {"t_user_u_per_e1742b", "[--user <u>] [permisos...]"},
     {"t_retira_per_6104b6", "Retira permisos. Sin lista de permisos, TODOS los de ese destinatario."},
     {{{"t_r_90cdb7", "-r"}, {"t_tambi_n_en_33e099", "También en los descendientes."}}}, {}},

    // --- Acciones
    {"rsync", {"t_acciones_79bd0e", "Acciones"}, {"t_rsync_uso", "<destino> [--delete] [--check] [--wait]"},
     {"t_rsync_res", "Sincroniza el CONTENIDO de este dataset con otro («Sincronizar» de la interfaz)."},
     {{{"t_rsync_del", "--delete"}, {"t_rsync_del_q", "Borra en el destino lo que ya no está en el origen."}},
      {{"t_rsync_check", "--check"}, {"t_rsync_check_q", "Simula y enseña qué haría, sin tocar nada."}},
      {{"t_wait_flag", "--wait"}, {"t_wait_flag_q", "Espera aquí a que termine, en vez de devolver el trabajo."}}},
     {{"t_rsync_det", "Copia FICHEROS, no instantáneas: los dos extremos tienen que estar montados. Para "
      "mandar una instantánea está `copy`."},
      {"t_rsync_det2", "Sin `--delete` solo añade y actualiza; lo que sobre en el destino se queda. Con "
      "`--delete` el destino acaba idéntico al origen, y eso INCLUYE borrar."},
      {"t_rsync_det3", "Los dos extremos han de estar en la misma máquina: entre máquinas distintas la "
      "interfaz usa `tar` sobre SSH, que no está portado aquí."}}},
    {"breakdown", {"t_acciones_79bd0e", "Acciones"}, {"t_directorio_0a6bbc", "<directorio> <hijo> [<directorio> <hijo>...]"},
     {"t_convierte__9a9063", "Convierte directorios del dataset en datasets hijos."},
     {{{"t_wait_flag", "--wait"}, {"t_wait_flag_q", "Espera aquí a que termine, en vez de devolver el trabajo."}}}, {}},
    {"assemble", {"t_acciones_79bd0e", "Acciones"}, {"t_hijo_hijo_9a4172", "<hijo> [<hijo>...]"},
     {"t_lo_contrar_b80d3f", "Lo contrario de breakdown: devuelve datasets hijos a directorios."},
     {{{"t_wait_flag", "--wait"}, {"t_wait_flag_q", "Espera aquí a que termine, en vez de devolver el trabajo."}}},
     {{"t_los_hijos__aa9ae2", "Los hijos se pueden dar con nombre relativo: se completan con el dataset actual."}}},
    {"todir", {"t_acciones_79bd0e", "Acciones"}, {"t_directorio_9d9336", "<directorio-destino>"},
     {"t_vuelca_el__e8430a", "Vuelca el contenido del dataset a un directorio corriente."},
     {{{"t_delete_sou_f4bf01", "--delete-source"}, {"t_destruye_e_228abc", "Destruye el dataset de origen al terminar."}},
      {{"t_wait_flag", "--wait"}, {"t_wait_flag_q", "Espera aquí a que termine, en vez de devolver el trabajo."}}},
     {}},
    {"fromdir", {"t_acciones_79bd0e", "Acciones"}, {"t_directorio_09a389", "<directorio-origen>"},
     {"t_vuelca_un__c2bd33", "Vuelca un directorio DENTRO del dataset actual. El origen puede estar en otra máquina."},
     {{{"t_from_url_325aa2", "--from <url>"}, {"t_la_m_quina_cdb6da", "La máquina de la que sale el directorio."}},
      {{"t_subdir_rel_363111", "--subdir <rel>"}, {"t_d_nde_deja_f3173a", "Dónde dejarlo dentro del dataset."}}},
     {{"t_no_es_la_i_ace907", "NO es la inversa de todir, aunque el nombre lo sugiera: la inversa de todir es "
      "assemble. Esto crea el contenido de un dataset A PARTIR de un directorio."},
      {"t_va_como_un_682722", "Va como una tubería tar entre las dos máquinas, no por RPC: el verbo del agente lee "
      "el tar por la entrada estándar y el canal RPC no tiene stdin."}}},

    // --- Transferencias
    {"copy", {"t_transferen_bb3ab8", "Transferencias entre máquinas"}, {"t_destino_bb3347", "<destino>"},
     {"t_manda_una__2d0418", "Manda una instantánea a otro dataset, aquí o en otra máquina."},
     {{{"t_from_insta_ce64b3", "--from <@instantánea>"}, {"t_qu_se_mand_f75c70", "Qué se manda. Sin ella, el sitio actual."}},
      {{"t_base_insta_074a40", "--base <@instantánea>"}, {"t_solo_viaja_e40f1d", "Solo viaja lo que cambió desde ahí («Nivelar»)."}},
      {{"t_flags_b14893", "--flags <...>"}, {"t_banderas_q_714c60", "Banderas que se pasan a zfs send."}},
      {{"t_wait_604867", "--wait"}, {"t_espera_aqu_fba1e7", "Espera aquí a que termine, en vez de devolver el trabajo."}}},
     {{"t_el_destino_bfb232", "El destino es una URL: puede estar en OTRA máquina."},
      {"t_va_como_tr_731e1f", "Va como TRABAJO del daemon, que es lo que permite mandar terabytes y cerrar la "
      "sesión. Se sigue con «job <id>» en la máquina de ORIGEN."},
      {"t_ninguno_de_0490e5", "Ninguno de los dos extremos puede ser Windows: el flujo por socket no está portado "
      "allí. Para eso están todir y fromdir."}}},

    // --- Trabajos
    {"jobs", {"t_trabajos_e_ae8ad9", "Trabajos en segundo plano"}, {"t_jobs_uso", "[--all|--<estado>...]"},
     {"t_jobs_res", "Los trabajos EN CURSO de la máquina."},
     {{{"t_jobs_all", "--all"}, {"t_jobs_all_q", "Todos, incluidos los terminados."}},
      {{"t_jobs_est", "--running / --queued / --done / --failed / --cancelled"},
       {"t_jobs_est_q", "Solo esos estados. Se pueden combinar."}}},
     {{"t_jobs_det", "Sin argumentos salen los que están corriendo o encolados, que es lo que uno "
      "pregunta al teclear «jobs». Un daemon lleva meses en pie y acumula trabajos "
      "terminados: enseñarlos todos convierte la pregunta en buscar entre decenas de "
      "líneas."},
      {"t_jobs_det2", "El filtro se pide por el NOMBRE DEL ESTADO, el mismo que sale en la columna "
      "ESTADO, para no tener que aprender un vocabulario aparte."}}},
    {"job", {"t_trabajos_e_ae8ad9", "Trabajos en segundo plano"}, {"t_id_cancel__aab412", "<id> | cancel <id>"},
     {"t_el_estado__9c5ecc", "El estado de un trabajo, o su cancelación."},
     {},
     {{"t_cancelar_n_391b27", "Cancelar no deshace lo que ya se hizo."}}},

    // --- Daemon
    {"install-daemon", {"t_daemon_48e665", "Daemon"}, {"t_on_url_b3e711", "[--on <url>]"},
     {"t_instala_o__7022d3", "Instala o actualiza el daemon y lo arranca con el gestor de servicios del sistema."},
     {},
     {{"t_no_hay_res_b51cef", "No hay respaldo por guion: si falta el binario nativo de esa plataforma no se "
      "instala nada. Un agente de guion no habla TLS, y dejarlo puesto da una máquina que "
      "PARECE atendida y no lo está."}}},

    // --- Del intérprete
    {"format", {"t_del_int_rp_d5d82a", "Del intérprete"}, {"t_text_tsv_j_5794d8", "[text|tsv|json]"},
     {"t_cambia_el__e00af9", "Cambia el formato de los listados. Sin argumento, dice cuál está puesto."},
     {},
     {{"t_text_es_pa_52ce5c", "text es para leer: columnas alineadas y tamaños legibles. tsv es para guiones: sin "
      "encabezado, tabuladores y columnas fijas en inglés. json añade TIPOS: los números "
      "son números y lo que no aplica es null."}}},
    {"yes", {"t_del_int_rp_d5d82a", "Del intérprete"}, {"t_on_off_14009f", "[on|off]"},
     {"t_deja_de_pr_b69a39", "Deja de preguntar antes de lo destructivo, o vuelve a hacerlo."}, {}, {}},
    {"help", {"t_del_int_rp_d5d82a", "Del intérprete"}, {"t_orden_f088c5", "[orden]"},
     {"t_esta_ayuda_346008", "Esta ayuda. Con el nombre de una orden, la suya con todo el detalle."}, {}, {}},
    {"exit", {"t_del_int_rp_d5d82a", "Del intérprete"}, {"", ""}, {"t_salir_tamb_d12c74", "Salir. También «quit» y Ctrl-D."}, {}, {}},
};

}  // namespace

const std::vector<Orden>& ordenes() { return kOrdenes; }

const Orden* ordenPorNombre(const std::string& nombre) {
    const std::string n = B::toLowerAscii(B::trim(nombre));
    for (const Orden& o : kOrdenes) {
        if (n == o.nombre) {
            return &o;
        }
    }
    return nullptr;
}

void imprimeAyuda(int ancho) {
    std::string grupoActual;
    for (const Orden& o : kOrdenes) {
        if (grupoActual != o.grupo.es) {
            grupoActual = o.grupo.es;
            std::fprintf(stderr, "\n%s:\n", TC(o.grupo.clave, o.grupo.es));
        }
        imprimeOrden(o, ancho, false);
    }
    std::fprintf(stderr, "\n%s\n",
                 TC("t_todas_las__c1ee0e",
                    "Todas las órdenes admiten --on <url> (o --from, que es lo mismo) para\n"
                    "actuar sobre otro sitio sin moverse. Sin ella se usa el sitio actual.\n"
                    "«help <orden>» da el detalle de una. El tabulador completa órdenes y URL."));
}

bool imprimeAyudaDe(const std::string& nombre, int ancho) {
    const Orden* o = ordenPorNombre(nombre);
    if (!o) {
        return false;
    }
    std::fprintf(stderr, "\n");
    imprimeOrden(*o, ancho, true);
    std::fprintf(stderr, "\n");
    return true;
}

std::vector<std::string> nombresQueEmpiezanPor(const std::string& prefijo) {
    const std::string p = B::toLowerAscii(prefijo);
    std::vector<std::string> out;
    for (const Orden& o : kOrdenes) {
        if (B::startsWith(o.nombre, p)) {
            out.push_back(o.nombre);
        }
    }
    // «quit» no está en el catálogo —es sinónimo de exit— pero se completa igual.
    if (B::startsWith(std::string("quit"), p)) {
        out.push_back("quit");
    }
    return out;
}

std::vector<std::string> opcionesQueEmpiezanPor(const std::string& orden,
                                                const std::string& prefijo) {
    std::vector<std::string> out;
    const Orden* o = ordenPorNombre(orden);
    if (!o) {
        return out;
    }
    const auto anota = [&](const std::string& forma) {
        // De «--user <u> / --group <g>» salen dos opciones; de «-r», una.
        for (const std::string& trozo : B::split(forma, " ", true)) {
            if (trozo.size() >= 2 && trozo[0] == '-' && B::startsWith(trozo, prefijo)) {
                bool repetida = false;
                for (const auto& x : out) {
                    repetida = repetida || x == trozo;
                }
                if (!repetida) {
                    out.push_back(trozo);
                }
            }
        }
    };
    for (const Parametro& p : o->params) {
        anota(p.forma.es);
    }
    anota(o->uso.es);
    // `--on` y `--from` valen en todas.
    for (const char* comun : {"--on", "--from"}) {
        if (B::startsWith(std::string(comun), prefijo)) {
            bool repetida = false;
            for (const auto& x : out) {
                repetida = repetida || x == comun;
            }
            if (!repetida) {
                out.push_back(comun);
            }
        }
    }
    return out;
}

}  // namespace zfsmgr::cli
