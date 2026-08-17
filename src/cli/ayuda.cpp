#include "ayuda.h"

#include "strutil.h"

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
        std::string(o.nombre) + (o.uso && *o.uso ? std::string(" ") + o.uso : std::string());
    fila(uso, o.resumen, 2, ancho);
    // Los parámetros van DEBAJO y tabulados, uno por línea. Metidos en la misma línea que
    // la orden, una con cinco opciones ocupaba tres renglones sin que se viera cuál es
    // cuál.
    for (const Parametro& p : o.params) {
        fila(p.forma, p.que, 6, ancho);
    }
    if (conDetalle) {
        for (const char* d : o.detalle) {
            std::fprintf(stderr, "\n");
            for (const std::string& t : parte(d, static_cast<std::size_t>(ancho > 24 ? ancho - 4 : 60))) {
                std::fprintf(stderr, "  %s\n", t.c_str());
            }
        }
    }
}

const std::vector<Orden> kOrdenes = {
    // --- Navegación
    {"cd", "Navegación", "[destino]", "Cambia de sitio. Sin argumento, a la raíz.",
     {{"<destino>",
       "Ruta relativa, absoluta (/OldLau/winpool), URL completa, «..», «.» o «-» (el sitio "
       "anterior)."}},
     {"La posición es una URL, y todas las órdenes actúan sobre ella. Eso hace que "
      "cualquier orden se pueda copiar del historial, ponerle --on <url> y ejecutarla "
      "suelta.",
      "Dos reglas quitan ambigüedad a una ruta relativa: si el primer tramo nombra una "
      "CONEXIÓN, la ruta es absoluta; y si es el POOL en el que ya estás, es el nombre ZFS "
      "completo.",
      "Se comprueba que el destino EXISTA, como el cd de cualquier intérprete."}},
    {"pwd", "Navegación", "", "La URL actual.", {}, {}},
    {"ls", "Navegación", "[destino]",
     "Lista lo que hay. En la raíz las conexiones, en una conexión los pools, en un dataset "
     "sus hijos e instantáneas.",
     {{"#content[/ruta]", "Los ficheros de dentro."},
      {"#properties[/prop]", "Las propiedades."},
      {"#permissions", "Los permisos delegados."}},
     {"En Windows el contenido no está donde dice el «mountpoint»: el pool se monta en una "
      "letra de unidad y los descendientes heredan la del POOL. Se traduce solo."}},
    {"info", "Navegación", "[destino]", "Qué hay aquí y estado del daemon.", {}, {}},

    // --- Conexiones
    {"create", "Conexiones y pools", "<nombre> …",
     "Crea un nodo DONDE ESTÁS: en la raíz una conexión, en una conexión un pool, en un "
     "dataset un hijo.",
     {{"--name / --type / --os", "Conexión: nombre visible, LOCAL o SSH, sistema."},
      {"--host / --port / --user / --key", "Conexión: cómo se llega a la máquina."},
      {"--sudo", "Conexión: la máquina necesita elevar."},
      {"--password-fd <n>", "Conexión: la contraseña, por descriptor."},
      {"<dispositivo>...", "Pool: en cuáles se crea. SE ESCRIBEN."},
      {"-o p=v / -O p=v / --mountpoint", "Pool: propiedades y punto de montaje."},
      {"-f", "Pool: fuerza aunque parezcan en uso."},
      {"prop=valor", "Dataset: propiedades del hijo."}},
     {"La contraseña NUNCA se pasa por argumento: iría en argv y se vería en `ps` para "
      "cualquier usuario de la máquina. O se teclea, o entra por un descriptor.",
      "Se guarda cifrada con la contraseña maestra. Sin ella no se guarda en claro.",
      "Crear un POOL es la orden más destructiva de todas: escribe en los dispositivos que "
      "se le den y lo que hubiera en ellos se pierde. La confirmación los enumera uno a "
      "uno."}},
    {"edit", "Conexiones y pools", "[--name …] [--host …] …",
     "Cambia una conexión. Pulsar Intro conserva el valor actual.",
     {{"--password", "Pide una contraseña nueva. Sin ella, se conserva la que había."}},
     {}},
    {"destroy", "Conexiones y pools", "[destino] [-r|-R] [-f]",
     "Destruye lo que hay DONDE ESTÁS. Pide confirmación siempre.",
     {{"-r", "Con sus descendientes."},
      {"-R", "Con sus descendientes y lo que dependa de ellos."},
      {"-f", "Fuerza aunque esté en uso."}},
     {"En una CONEXIÓN la quita de la configuración y no toca nada en la máquina. En un "
      "POOL es `zpool destroy` — `zfs destroy` sobre el dataset raíz de un pool no "
      "funciona—. En un dataset o instantánea, `zfs destroy`."}},
    {"connect", "Conexiones y pools", "[destino]", "Marca la conexión como usable.", {}, {}},
    {"disconnect", "Conexiones y pools", "[destino]",
     "La aparta: el intérprete deja de hablar con ella y se cierra su túnel.",
     {},
     {"Es la MISMA marca que usa la interfaz gráfica. Navegar hasta una conexión apartada sí "
      "se permite, porque hay que poder llegar para volver a conectarla."}},
    {"refresh", "Conexiones y pools", "[destino]",
     "Suelta túnel, material TLS y castigos, relee la configuración y vuelve a sondear.",
     {},
     {"No es un listado: es lo que hay que hacer cuando algo se ha quedado colgado."}},

    // --- Dataset
    {"rename", "Dataset", "<nuevo>", "Renombra el dataset.", {}, {}},
    {"mount", "Dataset", "[-f]", "Lo monta.", {}, {}},
    {"unmount", "Dataset", "[-f]", "Lo desmonta.", {}, {}},
    {"promote", "Dataset", "", "Promueve un clon a dataset independiente.", {}, {}},
    {"get", "Dataset", "[propiedad]", "Lee las propiedades. Sin nombre, todas.", {}, {}},
    {"set", "Dataset", "<prop>=<valor> [más...]", "Escribe propiedades.", {}, {}},
    {"load-key", "Dataset", "", "Carga la clave de cifrado. La frase se teclea.", {}, {}},
    {"unload-key", "Dataset", "", "Descarga la clave de cifrado.", {}, {}},

    // --- Instantáneas
    {"snapshot", "Instantáneas", "@<nombre> [-r]", "Crea una instantánea.",
     {{"-r", "También de los descendientes."}}, {}},
    {"rollback", "Instantáneas", "[@<nombre>] [-f|-r|-R]",
     "Vuelve el dataset al estado de una instantánea, DESCARTANDO lo posterior.", {}, {}},
    {"clone", "Instantáneas", "<nuevo> [--from <@instantánea>]",
     "Crea un dataset a partir de una instantánea.",
     {{"--from <@inst>", "Cuál se clona. Sin ella, el sitio actual."}}, {}},
    {"holds", "Instantáneas", "[destino]", "Las retenciones de una instantánea.", {}, {}},
    {"hold", "Instantáneas", "<etiqueta> [-r]",
     "Pone una retención: impide borrarla hasta quitarla.", {}, {}},
    {"release", "Instantáneas", "<etiqueta> [-r]", "Quita una retención.", {}, {}},
    {"diff", "Instantáneas", "<@hasta> [--from <@desde>]",
     "Qué cambió entre dos puntos del mismo dataset.",
     {{"--from <@inst>", "El punto de partida. Sin ella, el sitio actual."}}, {}},

    // --- Pools
    {"status", "Pools", "", "El estado detallado del pool, tal y como lo da zpool.", {}, {}},
    {"history", "Pools", "", "Qué se le ha hecho al pool y cuándo.", {}, {}},
    {"scrub", "Pools", "[stop|pause]", "Verifica todo el contenido del pool.", {}, {}},
    {"trim", "Pools", "[stop|pause] [<vdev>]", "Avisa a los discos de qué bloques sobran.",
     {}, {}},
    {"initialize", "Pools", "[stop|pause] [<vdev>]", "Escribe en el espacio no usado.", {},
     {}},
    {"clear", "Pools", "[<vdev>]", "Pone a cero los errores contados.", {}, {}},
    {"sync", "Pools", "", "Fuerza la escritura de lo pendiente.", {}, {}},
    {"upgrade", "Pools", "", "Sube la versión del pool. NO se puede deshacer.", {}, {}},
    {"reguid", "Pools", "", "Cambia el identificador único del pool.", {}, {}},
    {"export", "Pools", "[-f]", "Lo desmonta y lo suelta, para llevarlo a otra máquina.", {},
     {}},
    {"import", "Pools", "[<pool>] [--as <nuevo>] [-f]",
     "Importa un pool. Sin nombre, enseña los que hay disponibles.",
     {{"--as <nuevo>", "Lo importa con otro nombre."}}, {}},

    // --- Permisos
    {"allow", "Permisos delegados", "[--user <u>] <permisos...>",
     "Delega permisos. Sin argumentos, los LISTA.",
     {{"--user <u> / --group <g>", "A quién."},
      {"--everyone", "A todos."},
      {"--set @<nombre>", "A un conjunto con nombre."},
      {"--local", "Solo en este dataset."},
      {"--descend", "Solo en los descendientes."},
      {"--create", "Solo en los que se creen a partir de ahora."}},
     {}},
    {"unallow", "Permisos delegados", "[--user <u>] [permisos...]",
     "Retira permisos. Sin lista de permisos, TODOS los de ese destinatario.",
     {{"-r", "También en los descendientes."}}, {}},

    // --- Acciones
    {"breakdown", "Acciones", "<directorio> <hijo> [<directorio> <hijo>...]",
     "Convierte directorios del dataset en datasets hijos.",
     {{"--job", "Lo manda al daemon en vez de esperarlo."}}, {}},
    {"assemble", "Acciones", "<hijo> [<hijo>...]",
     "Lo contrario de breakdown: devuelve datasets hijos a directorios.",
     {{"--job", "Lo manda al daemon en vez de esperarlo."}},
     {"Los hijos se pueden dar con nombre relativo: se completan con el dataset actual."}},
    {"todir", "Acciones", "<directorio-destino>",
     "Vuelca el contenido del dataset a un directorio corriente.",
     {{"--delete-source", "Destruye el dataset de origen al terminar."},
      {"--job", "Lo manda al daemon en vez de esperarlo."}},
     {}},
    {"fromdir", "Acciones", "<directorio-origen>",
     "Vuelca un directorio DENTRO del dataset actual. El origen puede estar en otra máquina.",
     {{"--from <url>", "La máquina de la que sale el directorio."},
      {"--subdir <rel>", "Dónde dejarlo dentro del dataset."}},
     {"NO es la inversa de todir, aunque el nombre lo sugiera: la inversa de todir es "
      "assemble. Esto crea el contenido de un dataset A PARTIR de un directorio.",
      "Va como una tubería tar entre las dos máquinas, no por RPC: el verbo del agente lee "
      "el tar por la entrada estándar y el canal RPC no tiene stdin."}},

    // --- Transferencias
    {"copy", "Transferencias entre máquinas", "<destino>",
     "Manda una instantánea a otro dataset, aquí o en otra máquina.",
     {{"--from <@instantánea>", "Qué se manda. Sin ella, el sitio actual."},
      {"--base <@instantánea>", "Solo viaja lo que cambió desde ahí («Nivelar»)."},
      {"--flags <...>", "Banderas que se pasan a zfs send."},
      {"--wait", "Espera aquí a que termine, en vez de devolver el trabajo."}},
     {"El destino es una URL: puede estar en OTRA máquina.",
      "Va como TRABAJO del daemon, que es lo que permite mandar terabytes y cerrar la "
      "sesión. Se sigue con «job <id>» en la máquina de ORIGEN.",
      "Ninguno de los dos extremos puede ser Windows: el flujo por socket no está portado "
      "allí. Para eso están todir y fromdir."}},

    // --- Trabajos
    {"jobs", "Trabajos en segundo plano", "", "Los trabajos que hay en la máquina.", {}, {}},
    {"job", "Trabajos en segundo plano", "<id> | cancel <id>",
     "El estado de un trabajo, o su cancelación.",
     {},
     {"Cancelar no deshace lo que ya se hizo."}},

    // --- Daemon
    {"install-daemon", "Daemon", "[--on <url>]",
     "Instala o actualiza el daemon y lo arranca con el gestor de servicios del sistema.",
     {},
     {"No hay respaldo por guion: si falta el binario nativo de esa plataforma no se "
      "instala nada. Un agente de guion no habla TLS, y dejarlo puesto da una máquina que "
      "PARECE atendida y no lo está."}},

    // --- Del intérprete
    {"format", "Del intérprete", "[text|tsv|json]",
     "Cambia el formato de los listados. Sin argumento, dice cuál está puesto.",
     {},
     {"text es para leer: columnas alineadas y tamaños legibles. tsv es para guiones: sin "
      "encabezado, tabuladores y columnas fijas en inglés. json añade TIPOS: los números "
      "son números y lo que no aplica es null."}},
    {"yes", "Del intérprete", "[on|off]",
     "Deja de preguntar antes de lo destructivo, o vuelve a hacerlo.", {}, {}},
    {"help", "Del intérprete", "[orden]",
     "Esta ayuda. Con el nombre de una orden, la suya con todo el detalle.", {}, {}},
    {"exit", "Del intérprete", "", "Salir. También «quit» y Ctrl-D.", {}, {}},
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
        if (grupoActual != o.grupo) {
            grupoActual = o.grupo;
            std::fprintf(stderr, "\n%s:\n", grupoActual.c_str());
        }
        imprimeOrden(o, ancho, false);
    }
    std::fprintf(stderr,
                 "\nTodas las órdenes admiten --on <url> (o --from, que es lo mismo) para\n"
                 "actuar sobre otro sitio sin moverse. Sin ella se usa el sitio actual.\n"
                 "«help <orden>» da el detalle de una. El tabulador completa órdenes y URL.\n");
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
        anota(p.forma);
    }
    anota(o->uso);
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
