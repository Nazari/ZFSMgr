#include "ayuda.h"

#include "zfsprops.h"

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
//
// Un salto de línea ESCRITO se respeta y empieza renglón. Sin esto, un ejemplo de dos
// líneas dentro de una explicación se fundía con la prosa y salía con la sangría rota, que
// es justo lo contrario de lo que un ejemplo viene a hacer.
std::vector<std::string> parte(const std::string& texto, std::size_t ancho) {
    if (texto.find('\n') != std::string::npos) {
        std::vector<std::string> lineas;
        for (const std::string& trozo : B::split(texto, "\n", false)) {
            for (const std::string& l : parte(trozo, ancho)) {
                lineas.push_back(l);
            }
        }
        return lineas;
    }
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

// La línea que enumera las banderas del mandato original. Se genera de la lista, no se
// escribe a mano: si se escribiera, sería una segunda copia que se separa de lo que el
// programa acepta de verdad —que es exactamente el fallo que estas listas vienen a quitar—.
std::string lineaDeNativas(const Orden& o) {
    if (o.nativas.empty()) {
        return {};
    }
    return T("t_admite_nativas", "Admite además las banderas del mandato original. Las cortas "
             "sin valor se pueden agrupar, como en OpenZFS: «-wLec» es «-w -L -e -c».");
}

// Cada bandera nativa con lo que hace, una por renglón.
//
// Antes salían todas apelmazadas en una línea —«-D -L -P -b -c…»—, que dice cuáles se
// aceptan y ninguna otra cosa: para saber qué hace cada una había que ir al manual de
// OpenZFS, y entonces la ayuda no servía de nada donde más falta hace.
void imprimeNativas(const Orden& o, int ancho) {
    for (const Nativa& n : o.nativas) {
        const std::string forma = std::string(n.forma) + (n.valor ? " <v>" : "");
        fila(forma, T(n.que.clave, n.que.es), 6, ancho);
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
    // El `--on <url>` universal, GENERADO: vale en toda orden que actúe sobre un sitio.
    //
    // Estaba solo en el pie de la ayuda general, así que quien buscaba «cómo le digo a
    // `breakdown` que actúe sobre otro dataset» miraba `help breakdown`, no lo veía, y
    // concluía —razonablemente— que la orden depende del sitio donde uno esté. Escribirlo
    // en las 47 fichas a mano sería copiarlo 47 veces; sale de si la orden tiene objetivo.
    // Salvo que la orden YA la documente por su cuenta: `clone` explica «--from <@inst>»
    // como «cuál se clona», y debajo salía la línea genérica diciendo que «--from» es lo
    // mismo que «--on». Las dos son ciertas y juntas se leen como una contradicción.
    bool yaLaDice = false;
    for (const Parametro& par : o.params) {
        const std::string forma = T(par.forma.clave, par.forma.es);
        if (forma.find("--on") != std::string::npos || forma.find("--from") != std::string::npos) {
            yaLaDice = true;
        }
    }
    if (conDetalle && !yaLaDice && o.objetivo != Objetivo::Ninguno) {
        fila(T("t_on_url_generado", "--on <url>"),
             T("t_on_url_generado_q", "Sobre QUÉ actúa. Sin ella, el sitio actual. «--from» es "
               "lo mismo."),
             6, ancho);
    }
    if (conDetalle) {
        const std::string nativas = lineaDeNativas(o);
        if (!nativas.empty()) {
            std::fprintf(stderr, "\n");
            for (const std::string& t :
                 parte(nativas, static_cast<std::size_t>(ancho > 24 ? ancho - 4 : 60))) {
                std::fprintf(stderr, "  %s\n", t.c_str());
            }
            imprimeNativas(o, ancho);
        }
        for (const Texto& d : o.detalle) {
            std::fprintf(stderr, "\n");
            for (const std::string& t :
                 parte(T(d.clave, d.es), static_cast<std::size_t>(ancho > 24 ? ancho - 4 : 60))) {
                std::fprintf(stderr, "  %s\n", t.c_str());
            }
        }
    }
}

// Las banderas de `zfs send`, tomadas de la capa base.
//
// Copiarlas aquí sería tener dos listas: la que el intérprete acepta y la que el daemon
// deja pasar. Se desincronizarían en el primer cambio, y la que manda es la del daemon,
// así que el usuario vería aceptada una bandera que luego muere al otro lado.
std::vector<Nativa> nativasDeSend() {
    std::vector<Nativa> out;
    for (const auto& b : zfsmgr::base::zfsprops::banderasDeSend()) {
        out.push_back({b.forma, b.valor, {b.clave, b.que}});
    }
    return out;
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
      {"t_se_comprue_d745a0", "Se comprueba que el destino EXISTA, como el cd de cualquier intérprete."}},
     Objetivo::Cualquiera,
     {}},
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
      "letra de unidad y los descendientes heredan la del POOL. Se traduce solo."}},
     Objetivo::Cualquiera,
     {}},
    {"info", {"t_navegaci_n_60cb06", "Navegación"}, {"t_destino_132a32", "[destino]"}, {"t_qu_hay_aqu_66e605", "Qué hay aquí y estado del daemon."}, {}, {},
     Objetivo::Cualquiera, {}},

    // --- Conexiones
    {"create", {"t_grupo_crear", "Crear y destruir, en cualquier nivel"}, {"t_create_uso2", "<nombre>|@<nombre> …"},
     {"t_create_res2", "Crea un nodo DONDE ESTÁS: en la raíz una conexión, en una conexión un pool, en un "
     "dataset un hijo, y con « @ » delante una instantánea."},
     {{{"t_name_type__f51f24", "--name <n> / --type <LOCAL|SSH> / --os <s>"}, {"t_conexi_n_n_42c7eb", "Conexión: nombre visible, LOCAL o SSH, sistema."}},
      {{"t_host_port__2ddcc9", "--host <h> / --port <n> / --user <u> / --key <ruta>"}, {"t_conexi_n_c_c0e9ec", "Conexión: cómo se llega a la máquina."}},
      {{"t_sudo_d34723", "--sudo"}, {"t_conexi_n_l_0e9498", "Conexión: la máquina necesita elevar."}},
      {{"t_password_f_33329f", "--password-fd <n>"}, {"t_conexi_n_l_ffa8ae", "Conexión: la contraseña, por descriptor."}},
      {{"t_dispositiv_538150", "<dispositivo>..."}, {"t_pool_en_cu_472c48", "Pool: en cuáles se crea. SE ESCRIBEN."}},
      {{"t_o_p_v_o_p__3f8555", "-o p=v / -O p=v / --mountpoint <ruta>"}, {"t_pool_propi_383914", "Pool: propiedades y punto de montaje."}},
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
      "instantánea en la URL, así que no hay una regla nueva que recordar."}},
     // `create` vale en TODOS los niveles —raíz, conexión, dataset— y decide por dónde se
     // está. Declarando `Dataset`, el preámbulo exigía estar en uno y crear un pool desde
     // el intérprete era imposible: `create <pool> <disco>` en una conexión moría con
     // «hace falta un dataset» antes de llegar a la orden.
     Objetivo::Cualquiera,
     {{"texto", Ranura::Tipo::Texto, Ranura::Cuantas::UnaOMas}},
     // De `zpool create [-fnd] [-R root] [-t nombre]` y de
     // `zfs create [-Pnpuv] [-b bloque] [-V tamaño]`, menos las que la orden ya declara
     // arriba —`-f`, `-o`, `-O`, `--mountpoint`, `-r`—. Es la UNIÓN de los dos mandatos
     // porque `create` es uno solo: cuál se usa lo decide dónde se está, y si una bandera
     // no vale en ese nivel lo dice el propio zfs/zpool, que es quien lo sabe.
     {{"-n", false, {"t_nat_create_n", "Ensayo: enseña lo que haría y no lo hace."}}, {"-d", false, {"t_nat_create_d", "Pool: sin activar ninguna característica."}}, {"-R", true, {"t_nat_create_Rmay", "Pool: raíz alternativa donde montarlo."}}, {"-t", true, {"t_nat_create_t", "Pool: nombre temporal, solo hasta el próximo arranque."}},
      {"-p", false, {"t_nat_create_p", "Dataset: crea también los padres que falten."}}, {"-u", false, {"t_nat_create_u", "Dataset: no lo monta al crearlo."}}, {"-v", false, {"t_nat_create_v", "Dataset: cuenta lo que va haciendo."}}, {"-P", false, {"t_nat_create_Pmay", "Dataset: ensayo, en formato analizable."}},
      {"-s", false, {"t_nat_create_s", "Volumen: sin reservar el espacio por adelantado."}}, {"-b", true, {"t_nat_create_b", "Volumen: tamaño de bloque."}}, {"-V", true, {"t_nat_create_Vmay", "Crea un VOLUMEN de ese tamaño, no un sistema de ficheros."}}}},
    {"destroy", {"t_grupo_crear", "Crear y destruir, en cualquier nivel"}, {"t_destino_r__8b306f", "[destino] [-r|-R] [-f]"},
     {"t_destruye_l_77f9b5", "Destruye lo que hay DONDE ESTÁS. Pide confirmación siempre."},
     {{{"t_r_90cdb7", "-r"}, {"t_con_sus_de_7eb43b", "Con sus descendientes."}},
      {{"t_r_24fd93", "-R"}, {"t_con_sus_de_b90f87", "Con sus descendientes y lo que dependa de ellos."}},
      {{"t_f_0abbcb", "-f"}, {"t_fuerza_aun_e2d851", "Fuerza aunque esté en uso."}}},
     {{"t_en_una_con_d7ff22", "En una CONEXIÓN la quita de la configuración y no toca nada en la máquina. En un "
      "POOL es `zpool destroy` — `zfs destroy` sobre el dataset raíz de un pool no "
      "funciona—. En un dataset o instantánea, `zfs destroy`."}},
     // Lo mismo que `create`: en una conexión la quita, en la raíz de un pool destruye el
     // pool, y en un dataset o una instantánea es `zfs destroy` —lo que dice el párrafo de
     // aquí arriba—. Pidiendo un dataset no se llegaba nunca a las dos primeras.
     Objetivo::Cualquiera,
     {},
     {{"-f", false, {"t_nat_destroy_f", "Fuerza el desmontaje de lo que esté en uso."}}, {"-n", false, {"t_nat_destroy_n", "Ensayo: no borra nada, dice qué borraría."}}, {"-p", false, {"t_nat_destroy_p", "Con las estadísticas en formato analizable."}}, {"-R", false, {"t_nat_destroy_Rmay", "También los clones que dependan de ello."}}, {"-r", false, {"t_nat_destroy_r", "También sus descendientes."}}, {"-v", false, {"t_nat_destroy_v", "Cuenta lo que va borrando."}}, {"-d", false, {"t_nat_destroy_d", "Lo marca para borrarlo cuando se suelte la última retención."}}}},
    {"devices", {"t_conexiones_3785cd", "Conexiones"}, {"t_devices_uso", "[--free]"},
     {"t_devices_res", "Los discos de la máquina, para elegir dónde crear un pool."},
     {{{"t_devices_free", "--free"}, {"t_devices_free_q", "Solo los que no están en uso."}}},
     {{"t_devices_det", "Salen todos, con la columna OCUPADO, y no solo los libres: esconder los "
      "ocupados escondería justo el disco que uno va a reutilizar a propósito —el de un "
      "pool viejo, por ejemplo—."},
      {"t_devices_det2", "OCUPADO es una comodidad, no un veredicto: dice que el dispositivo o "
      "alguno de sus hijos tiene sistema de ficheros o está montado. Quien vaya a escribir "
      "en él decide."}},
     Objetivo::Conexion,
     {}},
    {"edit", {"t_conexiones_3785cd", "Conexiones"}, {"t_name_host_73f07e", "[--name <n>] [--host <h>] …"},
     {"t_cambia_una_58f563", "Cambia una conexión. Pulsar Intro conserva el valor actual."},
     // Cada opción con su «<...>»: es lo que le dice al léxico que se lleva por delante el
     // componente siguiente. Sin eso, `edit prueba --name Renombrada` no era «cambia el
     // nombre»: «--name» quedaba como bandera suelta y «Renombrada» como una segunda
     // palabra que la orden no admite, así que respondía con la línea de uso. La orden
     // entera era inservible fuera del modo interactivo.
     {{{"t_edit_name", "--name <n> / --type <LOCAL|SSH> / --os <s>"},
       {"t_edit_name_q", "Nombre visible, tipo y sistema."}},
      {{"t_edit_host", "--host <h> / --port <n> / --user <u> / --key <ruta>"},
       {"t_edit_host_q", "Cómo se llega a la máquina."}},
      {{"t_edit_sudo", "--sudo / --no-sudo"}, {"t_edit_sudo_q", "Si la máquina necesita elevar."}},
      {{"t_password_76e3cd", "--password"}, {"t_pide_una_c_dbc5c6", "Pide una contraseña nueva. Sin ella, se conserva la que había."}},
      {{"t_edit_pfd", "--password-fd <n>"}, {"t_edit_pfd_q", "La contraseña, por descriptor."}}},
     {},
     Objetivo::Conexion,
     {}},
    {"connect", {"t_conexiones_3785cd", "Conexiones"}, {"t_destino_132a32", "[destino]"}, {"t_marca_la_c_c52a74", "Marca la conexión como usable."}, {}, {},
     Objetivo::Conexion,
     {}},
    {"disconnect", {"t_conexiones_3785cd", "Conexiones"}, {"t_destino_132a32", "[destino]"},
     {"t_la_aparta__62eeb8", "La aparta: el intérprete deja de hablar con ella y se cierra su túnel."},
     {},
     {{"t_es_la_mism_796aa1", "Es la MISMA marca que usa la interfaz gráfica. Navegar hasta una conexión apartada sí "
      "se permite, porque hay que poder llegar para volver a conectarla."}},
     Objetivo::Conexion,
     {}},
    {"refresh", {"t_conexiones_3785cd", "Conexiones"}, {"t_destino_132a32", "[destino]"},
     {"t_suelta_t_n_bedff9", "Suelta túnel, material TLS y castigos, relee la configuración y vuelve a sondear."},
     {},
     {{"t_no_es_un_l_2b8c41", "No es un listado: es lo que hay que hacer cuando algo se ha quedado colgado."}},
     Objetivo::Conexion,
     {}},

    // --- Dataset
    {"rename", {"t_dataset_105268", "Datasets"}, {"t_nuevo_dcceab", "<nuevo>"}, {"t_renombra_e_e71b10", "Renombra el dataset."}, {}, {},
     Objetivo::DatasetOInstantanea,
     {{"texto", Ranura::Tipo::Texto, Ranura::Cuantas::Una}},
     {{"-f", false, {"t_nat_rename_f", "Fuerza el desmontaje del destino si hace falta."}}, {"-p", false, {"t_nat_rename_p", "Crea los padres que falten en el nombre nuevo."}}, {"-u", false, {"t_nat_rename_u", "No vuelve a montarlo después."}}, {"-r", false, {"t_nat_rename_r", "Renombra la instantánea en todos los descendientes."}}}},
    {"mount", {"t_dataset_105268", "Datasets"}, {"t_f_9bd72b", "[-f]"}, {"t_lo_monta_6d9042", "Lo monta."}, {}, {},
     Objetivo::Dataset,
     {},
     {{"-f", false, {"t_nat_mount_f", "Fuerza el montaje."}}, {"-l", false, {"t_nat_mount_l", "Carga antes la clave de cifrado si hace falta."}}, {"-v", false, {"t_nat_mount_v", "Cuenta lo que va haciendo."}}, {"-O", false, {"t_nat_mount_Omay", "Monta encima aunque el punto de montaje no esté vacío."}}, {"-o", true, {"t_nat_mount_o", "Opciones de montaje, solo para esta vez."}}, {"-a", false, {"t_nat_mount_a", "Todos los datasets que deban montarse."}}}},
    {"unmount", {"t_dataset_105268", "Datasets"}, {"t_f_9bd72b", "[-f]"}, {"t_lo_desmont_a9975c", "Lo desmonta."}, {}, {},
     Objetivo::Dataset,
     {},
     {{"-f", false, {"t_nat_unmount_f", "Fuerza aunque haya ficheros abiertos."}}, {"-u", false, {"t_nat_unmount_u", "Descarga además la clave de cifrado."}}, {"-a", false, {"t_nat_unmount_a", "Todos los que estén montados."}}}},
    {"promote", {"t_dataset_105268", "Datasets"}, {"", ""}, {"t_promueve_u_eb988f", "Promueve un clon a dataset independiente."}, {}, {},
     Objetivo::Dataset,
     {}},
    {"get", {"t_dataset_105268", "Datasets"}, {"t_propiedad_77632b", "[propiedad]"}, {"t_lee_las_pr_521610", "Lee las propiedades. Sin nombre, todas."}, {},
     {{"t_tab_props", "El tabulador completa el nombre PREGUNTÁNDOSELO a la máquina, así que ofrece "
      "las que ese dataset tiene de verdad y no una lista escrita aquí que envejecería con "
      "cada versión de OpenZFS."}},
     Objetivo::DatasetOInstantanea,
     {{"propiedad", Ranura::Tipo::Texto, Ranura::Cuantas::Opcional}}},
    {"set", {"t_dataset_105268", "Datasets"}, {"t_prop_valor_b7871d", "<prop>=<valor> [más...]"}, {"t_escribe_pr_b449c1", "Escribe propiedades."}, {},
     {{"t_tab_valores", "El tabulador completa el nombre y, tras el « = », los valores posibles de "
      "las que tienen lista cerrada —`compression`, `canmount`, `sync`…—. Para `quota` o "
      "`mountpoint` no ofrece nada, que es mejor que inventar."}},
     Objetivo::DatasetOInstantanea,
     {{"props", Ranura::Tipo::Propiedad, Ranura::Cuantas::UnaOMas}}},
    {"load-key", {"t_dataset_105268", "Datasets"}, {"", ""}, {"t_carga_la_c_0013a3", "Carga la clave de cifrado. La frase se teclea."}, {}, {},
     Objetivo::Dataset, {}},
    {"unload-key", {"t_dataset_105268", "Datasets"}, {"", ""}, {"t_descarga_l_d86fbd", "Descarga la clave de cifrado."}, {}, {},
     Objetivo::Dataset, {}},
    {"schedule", {"t_dataset_105268", "Datasets"},
     {"t_sch_uso", "[--daily <n>] [--recursive] [--to <Con::pool/ds>] [--off|--clear]"},
     {"t_sch_res", "La programación de instantáneas del dataset. Sin opciones, la enseña."},
     {{{"t_sch_ret", "--hourly <n> / --daily <n> / --weekly <n> / --monthly <n> / --yearly <n>"},
       {"t_sch_ret_q", "Cuántas guardar de cada clase. 0 es «ninguna de esas»."}},
      {{"t_sch_rec", "--recursive / --no-recursive"},
       {"t_sch_rec_q", "Si cubre también a los descendientes."}},
      {{"t_sch_to", "--to <Conexión::pool/dataset>"},
       {"t_sch_to_q", "A dónde se nivela lo programado."}},
      {{"t_sch_level", "--level / --no-level"},
       {"t_sch_level_q", "Nivelar contra el destino después de cada instantánea. Exige --to."}},
      {{"t_sch_off", "--off"}, {"t_sch_off_q", "La apaga CONSERVANDO lo configurado."}},
      {{"t_sch_clear", "--clear"}, {"t_sch_clear_q", "Borra la programación: como si nunca la hubiera tenido."}}},
     {{"t_sch_det1", "Fijar cualquier valor la ACTIVA: `schedule --daily 7` quiere decir «guarda siete "
       "diarias», y programarla apagada no significaría nada. Para apagarla sin perder lo "
       "puesto, «--off»; para quitarla del todo, «--clear»."},
      {"t_sch_det2", "La programación no es un fichero de este programa: son PROPIEDADES del dataset "
       "—`org.fc16.gsa:*`—, así que sobrevive a reinstalar el cliente y viaja con el pool "
       "si se exporta. Se pueden ver con «ls #properties»."},
      {"t_sch_det3", "Quien hace las instantáneas es el agente GSA de esa máquina, no este programa: "
       "si no está instalado, la programación queda escrita y no pasa nada. Las reglas se "
       "comprueban ANTES de escribir nada: una activada sin ninguna retención mayor que 0 "
       "haría instantáneas y las borraría."}},
     Objetivo::Dataset, {}},
    {"schedules", {"t_dataset_105268", "Datasets"}, {"t_schs_uso", "[--all]"},
     {"t_schs_res", "Qué hay programado en esta máquina."},
     {{{"t_schs_all", "--all"}, {"t_schs_all_q", "En todas las conexiones, no solo en esta."}}},
     {{"t_schs_det", "Por omisión mira SOLO la máquina actual: con --all hay que preguntarle a cada "
       "una, y una apagada se cobra su plazo de espera entero — justo cuando uno está "
       "mirando por qué algo no salió."}},
     Objetivo::Conexion, {}},
    {"change-key", {"t_dataset_105268", "Datasets"}, {"t_ck_uso", "[--password-fd <n>]"},
     {"t_ck_res", "Cambia la frase de paso de un dataset cifrado."},
     {{{"t_edit_pfd", "--password-fd <n>"}, {"t_ck_fd_q", "La frase nueva, por descriptor. Sin ella se "
       "teclea, y se pide dos veces."}}},
     {{"t_ck_det1", "La frase NUNCA viaja por argumento: iría en el argv del proceso y se vería con "
       "`ps` en las dos máquinas. Va cifrada dentro de la petición al daemon, que se la da "
       "a `zfs change-key` por la entrada estándar."},
      {"t_ck_det2", "El dataset tiene que tener la clave CARGADA: sobre uno bloqueado, `zfs` no puede "
       "cambiar nada. Si hace falta, primero «load-key»."},
      {"t_ck_det3", "Cambia la frase de ESTE dataset. Los que heredan su clave la siguen heredando; "
       "para eso no hay que hacer nada más."}},
     Objetivo::Dataset, {}},

    // --- Instantáneas
    {"rollback", {"t_instant_ne_bff51f", "Instantáneas"}, {"t_nombre_f_r_74bf0b", "[@<nombre>] [-f|-r|-R]"},
     {"t_vuelve_el__e58a57", "Vuelve el dataset al estado de una instantánea, DESCARTANDO lo posterior."}, {}, {},
     Objetivo::Instantanea,
     {},
     {{"-r", false, {"t_nat_rollback_r", "Destruye las instantáneas posteriores a esa."}}, {"-R", false, {"t_nat_rollback_Rmay", "Y además los clones que dependan de ellas."}}, {"-f", false, {"t_nat_rollback_f", "Fuerza el desmontaje de los clones."}}}},
    {"clone", {"t_instant_ne_bff51f", "Instantáneas"}, {"t_nuevo_from_463e13", "<nuevo> [--from <@instantánea>]"},
     {"t_crea_un_da_97befd", "Crea un dataset a partir de una instantánea."},
     {{{"t_from_inst_17782f", "--from <@inst>"}, {"t_cu_l_se_cl_b311bf", "Cuál se clona. Sin ella, el sitio actual."}}},
     {{"t_clone_det1", "Un nombre RELATIVO cuelga del dataset de la instantánea: estando en "
       "`tank/datos@ayer`, «clone copia» deja `tank/datos/copia`. Para ponerlo en otro "
       "sitio se da el nombre ZFS entero: «clone tank/copia»."},
      {"t_clone_det2", "El clon comparte los bloques con la instantánea, así que al principio no ocupa "
       "casi nada — y esa instantánea NO se puede destruir mientras el clon exista. Para "
       "romper esa atadura está «promote»."},
      {"t_clone_det3", "Ejemplo:\n"
       "  cd /local/tank/datos@ayer\n"
       "  clone recuperado\n"
       "o desde cualquier sitio: «clone tank/recuperado --from /local/tank/datos@ayer»."}},
     Objetivo::Instantanea,
     {{"texto", Ranura::Tipo::Texto, Ranura::Cuantas::UnaOMas}}},
    {"holds", {"t_instant_ne_bff51f", "Instantáneas"}, {"t_destino_132a32", "[destino]"}, {"t_las_retenc_db1367", "Las retenciones de una instantánea."}, {}, {},
     Objetivo::Instantanea,
     {}},
    {"hold", {"t_instant_ne_bff51f", "Instantáneas"}, {"t_etiqueta_r_8becce", "<etiqueta> [-r]"},
     {"t_pone_una_r_c46735", "Pone una retención: impide borrarla hasta quitarla."}, {}, {},
     Objetivo::Instantanea,
     {{"etiqueta", Ranura::Tipo::Texto, Ranura::Cuantas::Una}},
     {{"-r", false, {"t_nat_hold_r", "También en las instantáneas de los descendientes."}}}},
    {"release", {"t_instant_ne_bff51f", "Instantáneas"}, {"t_etiqueta_r_8becce", "<etiqueta> [-r]"}, {"t_quita_una__478a77", "Quita una retención."}, {}, {},
     Objetivo::Instantanea,
     {{"etiqueta", Ranura::Tipo::Texto, Ranura::Cuantas::Una}},
     {{"-r", false, {"t_nat_release_r", "También en las instantáneas de los descendientes."}}}},
    {"diff", {"t_instant_ne_bff51f", "Instantáneas"}, {"t_hasta_from_64dcd2", "<@hasta> [--from <@desde>]"},
     {"t_qu_cambi_e_bca99a", "Qué cambió entre dos puntos del mismo dataset."},
     {{{"t_from_inst_17782f", "--from <@inst>"}, {"t_el_punto_d_3efe61", "El punto de partida. Sin ella, el sitio actual."}}}, {},
     Objetivo::DatasetOInstantanea,
     {{"destino", Ranura::Tipo::Url, Ranura::Cuantas::Una, Objetivo::Dataset}}},

    // --- Pools
    {"status", {"t_pools_2fd96d", "Pools"}, {"t_pool_destino", "[<pool>]"}, {"t_el_estado__f8428b", "El estado detallado del pool, tal y como lo da zpool."}, {}, {},
     Objetivo::Pool,
     {}},
    {"history", {"t_pools_2fd96d", "Pools"}, {"t_pool_destino", "[<pool>]"}, {"t_qu_se_le_h_a13b5d", "Qué se le ha hecho al pool y cuándo."}, {}, {},
     Objetivo::Pool,
     {}},
    {"scrub", {"t_pools_2fd96d", "Pools"}, {"t_pool_stop_pause", "[<pool>] [stop|pause]"}, {"t_verifica_t_9c1250", "Verifica todo el contenido del pool."}, {}, {},
     Objetivo::Pool,
     {{"fase", Ranura::Tipo::Palabra, Ranura::Cuantas::Opcional, Objetivo::Ninguno,
       {"start", "stop", "cancel", "pause", "suspend"}}},
     {{"-e", false, {"t_nat_scrub_e", "Solo los ficheros con errores ya conocidos."}}, {"-s", false, {"t_nat_scrub_s", "Para el que esté en marcha."}}, {"-p", false, {"t_nat_scrub_p", "Lo pausa; se reanuda volviendo a lanzarlo."}}, {"-C", false, {"t_nat_scrub_Cmay", "Continúa desde el último punto guardado."}}, {"-E", true, {"t_nat_scrub_Emay", "Hasta esa fecha («AAAA-MM-DD [HH:MM]»)."}}, {"-S", true, {"t_nat_scrub_Smay", "Desde esa fecha («AAAA-MM-DD [HH:MM]»)."}}, {"-w", false, {"t_nat_scrub_w", "Espera aquí a que termine."}}, {"-a", false, {"t_nat_scrub_a", "En todos los pools de la máquina."}}}},
    {"trim", {"t_pools_2fd96d", "Pools"}, {"t_stop_vdev_on", "[stop|pause] [<vdev>] [--on <pool>]"}, {"t_avisa_a_lo_5d27bd", "Avisa a los discos de qué bloques sobran."},
     {}, {},
     Objetivo::Pool,
     {{"fase", Ranura::Tipo::Palabra, Ranura::Cuantas::Opcional, Objetivo::Ninguno,
       {"start", "stop", "cancel", "pause", "suspend"}},
      {"disco", Ranura::Tipo::Vdev, Ranura::Cuantas::Opcional}},
     {{"-d", false, {"t_nat_trim_d", "Borrado SEGURO: pide al disco que borre de verdad."}}, {"-w", false, {"t_nat_trim_w", "Espera aquí a que termine."}}, {"-r", true, {"t_nat_trim_r", "Ritmo máximo, en bytes por segundo."}}, {"-c", false, {"t_nat_trim_c", "Cancela el que esté en marcha."}}, {"-s", false, {"t_nat_trim_s", "Lo suspende."}}, {"-a", false, {"t_nat_trim_a", "En todos los pools de la máquina."}}}},
    {"initialize", {"t_pools_2fd96d", "Pools"}, {"t_stop_vdev_on", "[stop|pause] [<vdev>] [--on <pool>]"}, {"t_escribe_en_8e9d25", "Escribe en el espacio no usado."}, {},
     {},
     Objetivo::Pool,
     {{"fase", Ranura::Tipo::Palabra, Ranura::Cuantas::Opcional, Objetivo::Ninguno,
       {"start", "stop", "cancel", "pause", "suspend"}},
      {"disco", Ranura::Tipo::Vdev, Ranura::Cuantas::Opcional}},
     {{"-c", false, {"t_nat_initialize_c", "Cancela la que esté en marcha."}}, {"-s", false, {"t_nat_initialize_s", "La suspende."}}, {"-u", false, {"t_nat_initialize_u", "Deshace la marca de inicializado."}}, {"-w", false, {"t_nat_initialize_w", "Espera aquí a que termine."}}, {"-a", false, {"t_nat_initialize_a", "En todos los pools de la máquina."}}}},
    {"clear", {"t_pools_2fd96d", "Pools"}, {"t_vdev_on", "[<vdev>] [--on <pool>]"}, {"t_pone_a_cer_41359a", "Pone a cero los errores contados."}, {},
     {{"t_clear_det", "PREGUNTA antes, aunque no destruya datos: se escribe `clear` queriendo limpiar el "
      "terminal, y entonces se pierde la cuenta de errores de un pool —que es justo lo que "
      "uno estaba mirando— sin haberlo pedido. Para limpiar la pantalla, «cls»."}},
     Objetivo::Pool,
     {{"disco", Ranura::Tipo::Vdev, Ranura::Cuantas::Opcional}},
     {{"--power", false, {"t_nat_clear_power", "Enciende el disco por su indicador de fallo, si el equipo lo permite."}}, {"-n", false, {"t_nat_clear_n", "Ensayo: dice si se podría recuperar, sin tocar nada."}}, {"-F", false, {"t_nat_clear_Fmay", "Modo recuperación: descarta las últimas transacciones."}}}},
    {"flush", {"t_pools_2fd96d", "Pools"}, {"t_pool_destino", "[<pool>]"},
     {"t_flush_res", "Fuerza la escritura de lo pendiente del pool (`zpool sync`)."},
     {}, {{"t_flush_det", "Se llamaba `sync`, que es como se llama en `zpool`. Se cambió porque en la "
      "interfaz «Sincronizar» es OTRA cosa —copiar el contenido de un dataset a otro con "
      "rsync—, y tener la misma palabra para las dos era una trampa: la de pool tarda un "
      "instante y la otra puede tardar horas."},
      {"t_pool_arg_det", "Todas las órdenes de pool aceptan el pool como primer argumento; sin él actúan "
      "sobre el sitio actual, que tiene que ser un pool. Se reconoce PREGUNTANDO qué pools "
      "hay en la máquina, no por la forma del argumento: así `scrub stop` sigue siendo la "
      "palabra «stop» y `clear sda1` sigue siendo un vdev."}},
     Objetivo::Pool,
     {}},
    {"upgrade", {"t_pools_2fd96d", "Pools"}, {"t_pool_destino", "[<pool>]"}, {"t_sube_la_ve_b9cca7", "Sube la versión del pool. NO se puede deshacer."}, {}, {},
     Objetivo::Pool,
     {},
     {{"-v", false, {"t_nat_upgrade_v", "Enseña qué características admite esta versión."}}, {"-a", false, {"t_nat_upgrade_a", "En todos los pools de la máquina."}}}},
    {"reguid", {"t_pools_2fd96d", "Pools"}, {"t_pool_destino", "[<pool>]"}, {"t_cambia_el__4a3340", "Cambia el identificador único del pool."}, {}, {},
     Objetivo::Pool,
     {},
     {{"-g", true, {"t_nat_reguid_g", "El identificador nuevo, en vez de uno al azar."}}}},
    {"export", {"t_pools_2fd96d", "Pools"}, {"t_pool_f", "[<pool>] [-f]"}, {"t_lo_desmont_64239f", "Lo desmonta y lo suelta, para llevarlo a otra máquina."}, {},
     {},
     Objetivo::Pool,
     {},
     {{"-a", false, {"t_nat_export_a", "Exporta todos los pools importados."}}, {"-f", false, {"t_nat_export_f", "Fuerza el desmontaje de sus datasets."}}}},
    {"import", {"t_pools_2fd96d", "Pools"}, {"t_pool_as_nu_2706a5", "[<pool>] [--as <nuevo>] [-f]"},
     {"t_importa_un_2c9f21", "Importa un pool. Sin nombre, enseña los que hay disponibles."},
     {{{"t_as_nuevo_c017c7", "--as <nuevo>"}, {"t_lo_importa_bd9394", "Lo importa con otro nombre."}}}, {},
     Objetivo::Conexion,
     {{"texto", Ranura::Tipo::Texto, Ranura::Cuantas::Una}},
     {{"-d", true, {"t_nat_import_d", "Dónde buscar: un directorio o un dispositivo."}}, {"-D", false, {"t_nat_import_Dmay", "Solo los pools destruidos."}}, {"-o", true, {"t_nat_import_o", "Opciones de montaje, o «propiedad=valor» del pool."}}, {"-c", true, {"t_nat_import_c", "Buscar en ese fichero de caché en vez de en los discos."}}, {"-l", false, {"t_nat_import_l", "Pide las claves de cifrado que hagan falta."}}, {"-f", false, {"t_nat_import_f", "Fuerza aunque parezca en uso por otra máquina."}}, {"-m", false, {"t_nat_import_m", "Admite importarlo con el log ausente."}}, {"-N", false, {"t_nat_import_Nmay", "Lo importa SIN montar ningún sistema de ficheros."}}, {"-R", true, {"t_nat_import_Rmay", "Raíz alternativa donde montarlo."}}, {"-F", false, {"t_nat_import_Fmay", "Modo recuperación: descarta las últimas transacciones."}}, {"-n", false, {"t_nat_import_n", "Con -F, ensayo: dice si se podría, sin hacerlo."}}, {"-t", false, {"t_nat_import_t", "El nombre nuevo es temporal, solo hasta el próximo arranque."}}, {"--rewind-to-checkpoint", false, {"t_nat_import_rewind_mayto_maycheckpoint", "Vuelve al punto de control guardado en el pool."}}}},

    // --- Permisos
    {"allow", {"t_permisos_d_3db5da", "Permisos delegados"}, {"t_user_u_per_9cf888", "[--user <u>] <permisos...>"},
     {"t_delega_per_60be91", "Delega permisos. Sin argumentos, los LISTA."},
     {{{"t_user_u_gro_b011b8", "--user <u> / --group <g>"}, {"t_a_qui_n_1cc417", "A quién."}},
      {{"t_everyone_0de5e3", "--everyone"}, {"t_a_todos_2ed0f3", "A todos."}},
      {{"t_set_nombre_828345", "--set @<nombre>"}, {"t_a_un_conju_58094e", "A un conjunto con nombre."}},
      {{"t_local_91441b", "--local"}, {"t_solo_en_es_4019d0", "Solo en este dataset."}},
      {{"t_descend_8f0ee4", "--descend"}, {"t_solo_en_lo_9b33ba", "Solo en los descendientes."}},
      {{"t_create_488177", "--create"}, {"t_solo_en_lo_9dcb23", "Solo en los que se creen a partir de ahora."}}},
     {},
     Objetivo::Dataset,
     {{"texto", Ranura::Tipo::Texto, Ranura::Cuantas::UnaOMas}}},
    {"unallow", {"t_permisos_d_3db5da", "Permisos delegados"}, {"t_user_u_per_e1742b", "[--user <u>] [permisos...]"},
     {"t_retira_per_6104b6", "Retira permisos. Sin lista de permisos, TODOS los de ese destinatario."},
     {{{"t_r_90cdb7", "-r"}, {"t_tambi_n_en_33e099", "También en los descendientes."}}}, {},
     Objetivo::Dataset,
     {{"texto", Ranura::Tipo::Texto, Ranura::Cuantas::UnaOMas}}},

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
      "interfaz usa `tar` sobre SSH, que no está portado aquí."}},
     Objetivo::Dataset,
     {{"destino", Ranura::Tipo::Url, Ranura::Cuantas::Una, Objetivo::Dataset}}},
    {"breakdown", {"t_acciones_79bd0e", "Acciones"},
     {"t_bd_uso", "<subdir> <dataset-nuevo> [<subdir> <dataset-nuevo>...]"},
     {"t_bd_res", "Convierte un SUBDIRECTORIO en un dataset hijo que ocupa su sitio."},
     {{{"t_bd_subdir", "<subdir>"},
       {"t_bd_subdir_q", "Un directorio CORRIENTE de dentro del dataset, no un dataset. Se escribe "
        "relativo a su punto de montaje."}},
      {{"t_bd_nuevo", "<dataset-nuevo>"},
       {"t_bd_nuevo_q", "El dataset hijo que va a sustituirlo, relativo al dataset actual."}},
      {{"t_wait_flag", "--wait"}, {"t_wait_flag_q", "Espera aquí a que termine, en vez de devolver el trabajo."}}},
     {{"t_bd_det1", "Los argumentos van en PARES: cada subdirectorio con el nombre del dataset que "
       "lo sustituye. El contenido no se mueve de sitio para quien mira desde fuera: donde "
       "había un directorio queda un dataset con lo mismo dentro, y ya con sus propiedades, "
       "sus instantáneas y su cuota."},
      {"t_bd_det2", "El nombre NO tiene que reflejar la ruta: crear un dataset solo exige que exista "
       "su padre por nombre, así que `breakdown a/b/c a:b:c` convierte el de tres niveles "
       "sin convertir `a` ni `b`."},
      {"t_bd_det3", "Cada byte se mueve UNA vez y los originales se borran al final, cuando todas "
       "las copias están verificadas: si algo falla a mitad, lo original sigue intacto. Por "
       "eso conviene dejarlo como TRABAJO —sin --wait— cuando hay volumen: se sigue con "
       "«job <id>» y el intérprete queda libre."},
      {"t_bd_det4", "Ejemplo, estando en zfsm://local/tank/media:\n"
       "  breakdown fotos fotos videos videos\n"
       "deja tank/media/fotos y tank/media/videos donde había dos directorios."}},
     Objetivo::Dataset,
     {{"texto", Ranura::Tipo::Texto, Ranura::Cuantas::UnaOMas}}},
    {"assemble", {"t_acciones_79bd0e", "Acciones"}, {"t_hijo_hijo_9a4172", "<dataset-hijo> [<dataset-hijo>...]"},
     {"t_lo_contrar_b80d3f", "Lo contrario de breakdown: devuelve un dataset hijo a ser un directorio."},
     {{{"t_wait_flag", "--wait"}, {"t_wait_flag_q", "Espera aquí a que termine, en vez de devolver el trabajo."}}},
     {{"t_los_hijos__aa9ae2", "Los hijos se pueden dar con nombre relativo: se completan con el dataset actual."}},
     Objetivo::Dataset,
     {{"texto", Ranura::Tipo::Texto, Ranura::Cuantas::UnaOMas}}},
    {"todir", {"t_acciones_79bd0e", "Acciones"}, {"t_directorio_9d9336", "<directorio-destino>"},
     {"t_vuelca_el__e8430a", "Vuelca el contenido del dataset a un directorio corriente."},
     {{{"t_delete_sou_f4bf01", "--delete-source"}, {"t_destruye_e_228abc", "Destruye el dataset de origen al terminar."}},
      {{"t_wait_flag", "--wait"}, {"t_wait_flag_q", "Espera aquí a que termine, en vez de devolver el trabajo."}}},
     {},
     Objetivo::Dataset,
     {{"ruta", Ranura::Tipo::Ruta, Ranura::Cuantas::Una}}},
    {"fromdir", {"t_acciones_79bd0e", "Acciones"}, {"t_directorio_09a389", "<directorio-origen>"},
     {"t_vuelca_un__c2bd33", "Vuelca un directorio DENTRO del dataset actual. El origen puede estar en otra máquina."},
     {{{"t_from_url_325aa2", "--from <url>"}, {"t_la_m_quina_cdb6da", "La máquina de la que sale el directorio."}},
      {{"t_subdir_rel_363111", "--subdir <rel>"}, {"t_d_nde_deja_f3173a", "Dónde dejarlo dentro del dataset."}}},
     {{"t_no_es_la_i_ace907", "NO es la inversa de todir, aunque el nombre lo sugiera: la inversa de todir es "
      "assemble. Esto crea el contenido de un dataset A PARTIR de un directorio."},
      {"t_va_como_un_682722", "Va como una tubería tar entre las dos máquinas, no por RPC: el verbo del agente lee "
      "el tar por la entrada estándar y el canal RPC no tiene stdin."}},
     Objetivo::Dataset,
     {{"ruta", Ranura::Tipo::Ruta, Ranura::Cuantas::Una}}},

    // --- Transferencias
    {"copy", {"t_transferen_bb3ab8", "Transferencias entre máquinas"}, {"t_destino_bb3347", "<destino>"},
     {"t_manda_una__2d0418", "Manda una instantánea a otro dataset, aquí o en otra máquina."},
     {{{"t_from_insta_ce64b3", "--from <@instantánea>"}, {"t_qu_se_mand_f75c70", "Qué se manda. Sin ella, el sitio actual."}},
      {{"t_base_insta_074a40", "--base <@instantánea>"}, {"t_solo_viaja_e40f1d", "Solo viaja lo que cambió desde ahí («Nivelar»)."}},
      {{"t_wait_604867", "--wait"}, {"t_espera_aqu_fba1e7", "Espera aquí a que termine, en vez de devolver el trabajo."}}},
     {{"t_el_destino_bfb232", "El destino es una URL: puede estar en OTRA máquina."},
      {"t_va_como_tr_731e1f", "Va como TRABAJO del daemon, que es lo que permite mandar terabytes y cerrar la "
      "sesión. Se sigue con «job <id>» en la máquina de ORIGEN."},
      {"t_ninguno_de_0490e5", "Ninguno de los dos extremos puede ser Windows: el flujo por socket no está portado "
      "allí. Para eso están todir y fromdir."}},
     Objetivo::Instantanea,
     {{"destino", Ranura::Tipo::Url, Ranura::Cuantas::Una, Objetivo::Dataset}},
     nativasDeSend()},

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
      "ESTADO, para no tener que aprender un vocabulario aparte."}},
     Objetivo::Conexion, {}},
    {"job", {"t_trabajos_e_ae8ad9", "Trabajos en segundo plano"}, {"t_id_cancel__aab412", "<id> | cancel <id>"},
     {"t_el_estado__9c5ecc", "El estado de un trabajo, o su cancelación."},
     {},
     {{"t_cancelar_n_391b27", "Cancelar no deshace lo que ya se hizo."}},
     Objetivo::Conexion,
     {{"texto", Ranura::Tipo::Texto, Ranura::Cuantas::Una}}},

    // --- Daemon
    {"log", {"t_daemon_48e665", "Daemon"}, {"t_log_uso", "[--lines <n>]"},
     {"t_log_res", "El registro del daemon de esta máquina."},
     {{{"t_log_lines", "--lines <n>"}, {"t_log_lines_q", "Cuántas líneas del final. Por omisión, 200."}}},
     {{"t_log_det", "Es donde queda lo que el daemon hizo por su cuenta: las órdenes que sirvió, los "
       "trabajos que arrancó y por qué falló alguno. Con la aplicación gráfica se veía en su "
       "pestaña; desde el intérprete no había forma de mirarlo."}},
     Objetivo::Conexion, {}},
    {"install-daemon", {"t_daemon_48e665", "Daemon"}, {"t_on_url_b3e711", "[--on <url>]"},
     {"t_instala_o__7022d3", "Instala o actualiza el daemon y lo arranca con el gestor de servicios del sistema."},
     {},
     {{"t_no_hay_res_b51cef", "No hay respaldo por guion: si falta el binario nativo de esa plataforma no se "
      "instala nada. Un agente de guion no habla TLS, y dejarlo puesto da una máquina que "
      "PARECE atendida y no lo está."},
      {"t_mac_acceso_disco_ayuda", "En macOS queda UN PASO a mano: concederle «Acceso total al disco» al agente en "
      "Configuración del Sistema → Privacidad y Seguridad, añadiendo "
      "/usr/local/libexec/zfsmgr-agent. Sin eso el agente arranca y contesta STATUS=OK, "
      "pero no ve los discos: no encuentra ningún pool que importar y todo parece bien "
      "menos el resultado."}},
     Objetivo::Conexion, {}},

    // --- Del intérprete
    {"format", {"t_del_int_rp_d5d82a", "Del intérprete"}, {"t_text_tsv_j_5794d8", "[text|tsv|json]"},
     {"t_cambia_el__e00af9", "Cambia el formato de los listados. Sin argumento, dice cuál está puesto."},
     {},
     {{"t_text_es_pa_52ce5c", "text es para leer: columnas alineadas y tamaños legibles. tsv es para guiones: sin "
      "encabezado, tabuladores y columnas fijas en inglés. json añade TIPOS: los números "
      "son números y lo que no aplica es null."}}},
    {"cls", {"t_del_int_rp_d5d82a", "Del intérprete"}, {"", ""},
     {"t_cls_res", "Limpia la pantalla."},
     {}, {{"t_cls_det", "Se llama «cls» y no «clear» porque `clear` ya existe y es la del pool: pone a "
      "cero sus contadores de error. Escribir `clear` esperando limpiar el terminal es "
      "fácil y no era inocuo, así que aquélla pregunta antes."}}},
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
