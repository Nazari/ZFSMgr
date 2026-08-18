// La gramática del intérprete, contrastada contra el catálogo REAL de órdenes.
//
// Comprueba dos cosas distintas:
//
//  1. Que cada línea se analiza como se cree —qué es el destino y qué es cada ranura—. Son
//     justo los casos que la versión escrita a mano resolvía con reglas de precedencia y
//     que fallaban en silencio: `get compression` preguntaba por el dataset
//     `tank/datos/compression`; `trim <pool> <disco>` mandaba el disco donde iba el pool.
//  2. Que TODA orden del catálogo con firma declarada encaja en alguna forma de la
//     gramática. Sin esto, declarar una firma que la gramática no contempla no rompería la
//     compilación: rompería al teclear la orden.
#include "ayuda.h"
#include "gramatica_cli.h"

#include <cstdio>
#include <string>

namespace {
int fallos = 0;

void comprueba(bool ok, const std::string& que) {
    if (!ok) {
        std::printf("FALLO: %s\n", que.c_str());
        ++fallos;
    }
}

void igual(const std::string& a, const std::string& b, const std::string& que) {
    if (a != b) {
        std::printf("FALLO: %s\n   esperado: «%s»\n   obtenido: «%s»\n", que.c_str(), b.c_str(),
                    a.c_str());
        ++fallos;
    }
}
}  // namespace

// Un relleno para las ranuras OBLIGATORIAS de una orden. Sin él, `create --name x` o
// `rename -f` fallan por la RANURA y la prueba culparía a la opción.
static std::string rellenoDe(const zfsmgr::cli::Orden& o) {
    std::string relleno;
    for (const zfsmgr::cli::Ranura& r : o.ranuras) {
        if (r.cuantas != zfsmgr::cli::Ranura::Cuantas::Una
            && r.cuantas != zfsmgr::cli::Ranura::Cuantas::UnaOMas) {
            continue;
        }
        switch (r.tipo) {
            case zfsmgr::cli::Ranura::Tipo::Url: relleno += " /a/b"; break;
            case zfsmgr::cli::Ranura::Tipo::Propiedad: relleno += " k=v"; break;
            default: relleno += " relleno"; break;
        }
    }
    return relleno;
}

int main() {
    using zfsmgr::cli::analizaLinea;

    // --- El destino frente a la ranura: los casos que rompían antes.
    {
        const auto a = analizaLinea("scrub stop");
        igual(a.objetivo, "", "«scrub stop»: stop NO es el destino");
        igual(a.uno("fase"), "stop", "«scrub stop»: stop es la fase");
    }
    {
        const auto a = analizaLinea("flush /local/tank");
        igual(a.objetivo, "/local/tank", "«flush /local/tank»: la URL es el destino");
    }
    {
        const auto a = analizaLinea("get compression");
        igual(a.objetivo, "", "«get compression»: la propiedad no es el destino");
        igual(a.uno("propiedad"), "compression", "«get compression»: es la propiedad");
    }
    {
        // Esto NO se podía escribir con la versión anterior: el destino tenía que ir en
        // --on porque la ranura de texto se lo tragaba.
        const auto a = analizaLinea("get /local/tank/x compression");
        igual(a.objetivo, "/local/tank/x", "«get <url> <prop>»: la URL es el destino");
        igual(a.uno("propiedad"), "compression", "«get <url> <prop>»: y la palabra la propiedad");
    }
    {
        const auto a = analizaLinea("trim stop /dev/sda1");
        igual(a.uno("fase"), "stop", "«trim stop <disco>»: la fase");
        igual(a.uno("disco"), "/dev/sda1", "«trim stop <disco>»: el disco");
    }
    {
        const auto a = analizaLinea("set compression=lz4 atime=off");
        comprueba(a.lista("props").size() == 2, "«set a=b c=d»: dos propiedades");
    }
    {
        const auto a = analizaLinea("copy /oldlau/winpool/sa --base @ayer --wait");
        igual(a.uno("destino"), "/oldlau/winpool/sa", "«copy»: el destino es la URL");
        igual(a.opciones.at("base"), "@ayer", "«copy --base»: lleva valor");
        comprueba(a.tiene("wait"), "«copy --wait»: es una bandera");
    }
    {
        const auto a = analizaLinea("install-daemon oldlau");
        igual(a.objetivo, "oldlau", "«install-daemon oldlau»: una conexión se nombra sin barra");
    }
    {
        // Las comillas son del léxico: una ruta con espacios es UN componente.
        const auto a = analizaLinea("todir \"/mnt/con espacios/x\"");
        igual(a.uno("ruta"), "/mnt/con espacios/x", "«todir» con espacios entrecomillados");
    }
    {
        const auto a = analizaLinea("");
        comprueba(a.vacia, "una línea vacía es válida y no es un error");
    }

    // --- NINGUNA orden del catálogo puede quedarse sin su producción.
    //
    // El léxico reconoce los verbos con una tabla; si alguien añade una orden al catálogo y
    // olvida la fila, la línea se analiza como «orden desconocida» y el usuario recibe un
    // error raro. Esto lo caza aquí, que es donde se quiere que se cace.
    for (const zfsmgr::cli::Orden& o : zfsmgr::cli::ordenes()) {
        const auto a = analizaLinea(std::string(o.nombre) + " --on /x/y");
        comprueba(!a.verboDesconocido,
                  std::string("«") + o.nombre + "» no la reconoce el léxico: ¿falta su fila en "
                  "kVerbos y su producción en gramatica.y?");
    }

    // --- Toda firma declarada tiene que encajar en la gramática, en las dos direcciones.
    //
    // Con ranuras OBLIGATORIAS, la orden sola tiene que FALLAR —`create` sin nombre no es
    // una orden— y además decir qué falta. Sin ellas, tiene que analizarse.
    for (const zfsmgr::cli::Orden& o : zfsmgr::cli::ordenes()) {
        if (o.objetivo == zfsmgr::cli::Objetivo::Ninguno && o.ranuras.empty()) {
            continue;  // sin firma declarada todavía
        }
        bool obligatoria = false;
        for (const zfsmgr::cli::Ranura& r : o.ranuras) {
            obligatoria = obligatoria || r.cuantas == zfsmgr::cli::Ranura::Cuantas::Una
                          || r.cuantas == zfsmgr::cli::Ranura::Cuantas::UnaOMas;
        }
        const auto a = analizaLinea(o.nombre);
        if (obligatoria) {
            comprueba(!a.error.empty(),
                      std::string("«") + o.nombre + "» sola debería faltarle una ranura");
            comprueba(!a.faltaRanura.empty(),
                      std::string("«") + o.nombre + "»: y decir CUÁL falta, no «syntax error»");
        } else {
            comprueba(a.error.empty(),
                      std::string("la orden «") + o.nombre + "» sola no se analiza: " + a.error);
            igual(a.verbo, o.nombre, std::string("«") + o.nombre + "»: el verbo");
        }
    }

    // --- Cada orden con ranura tiene que emitirla con SU nombre.
    //
    // Es el fallo que aparece al migrar: la gramática llama «destino» a la URL de `copy` y
    // la orden la buscaba como «texto», así que el mensaje salía vacío y el argumento se
    // perdía. Aquí se fija el contrato: qué nombre emite cada una.
    {
        struct { const char* linea; const char* ranura; const char* valor; } casos[] = {
            {"copy /a/b/c", "destino", "/a/b/c"},
            {"rsync /a/b/c", "destino", "/a/b/c"},
            {"diff /a/b/c", "destino", "/a/b/c"},
            {"todir /mnt/x", "ruta", "/mnt/x"},
            {"fromdir /mnt/x", "ruta", "/mnt/x"},
            {"hold etiq", "etiqueta", "etiq"},
            {"release etiq", "etiqueta", "etiq"},
            {"get compression", "propiedad", "compression"},
            {"set a=b", "props", "a=b"},
            {"clear /dev/sda1", "disco", "/dev/sda1"},
            {"scrub stop", "fase", "stop"},
            {"rename nuevo", "texto", "nuevo"},
            {"import tank", "texto", "tank"},
            {"job abc123", "texto", "abc123"},
            {"create hijo", "texto", "hijo"},
        };
        for (const auto& c : casos) {
            const auto a = analizaLinea(c.linea);
            igual(a.uno(c.ranura), c.valor,
                  std::string("«") + c.linea + "»: la ranura se llama «" + c.ranura + "»");
        }
    }

    // --- Toda bandera nativa declarada tiene que DECIR qué hace.
    //
    // La ayuda de estas banderas se genera de la propia lista, así que una sin descripción
    // sale como un renglón mudo: el usuario ve que se acepta «-P» y nada más, que es lo
    // que había antes de describirlas y por lo que había que ir al manual de OpenZFS.
    {
        int mudas = 0;
        for (const zfsmgr::cli::Orden& o : zfsmgr::cli::ordenes()) {
            for (const zfsmgr::cli::Nativa& n : o.nativas) {
                if (!n.que.es || !*n.que.es) {
                    std::fprintf(stderr, "  sin descripción: %s %s\n", o.nombre, n.forma);
                    ++mudas;
                }
            }
        }
        comprueba(mudas == 0, "todas las banderas nativas describen qué hacen");
    }

    // --- Las banderas cortas AGRUPADAS: `-wLecR` son cinco.
    //
    // Es como se escriben en el manual de OpenZFS y como las acepta getopt. Se reparten
    // solo si todas las letras están declaradas para esa orden y ninguna lleva valor: un
    // grupo con una que sí lo lleva —`trim -rd`, donde `-r` quiere un ritmo— se deja
    // entero, porque dónde va el valor no se puede adivinar sin inventar.
    {
        const auto a = analizaLinea("copy /otra/x -wLec");
        int hay = 0;
        for (const char* f : {"-w", "-L", "-e", "-c"}) {
            hay += a.tiene(f) ? 1 : 0;
        }
        comprueba(hay == 4, "«copy -wLec»: se reparte en cuatro banderas");
        comprueba(!a.tiene("-wLec"), "«copy -wLec»: y el grupo ya no está entero");
        // Control: una letra que no existe deja el grupo SIN repartir, para que el error
        // hable de lo que el usuario escribió y no de una letra suelta que él no puso.
        const auto b = analizaLinea("copy /otra/x -wLZ");
        comprueba(b.tiene("-wLZ"), "«copy -wLZ»: con una letra ajena el grupo se deja entero");
        comprueba(!b.tiene("-w"), "«copy -wLZ»: y no se reparte a medias");
        // Control: con una que lleva valor tampoco se reparte.
        const auto c = analizaLinea("trim -rd");
        comprueba(c.tiene("-rd"), "«trim -rd»: -r lleva valor, así que el grupo no se parte");
    }

    // --- La POSICIÓN de una opción no significa nada.
    //
    // `allow --user u perms` y `allow perms --user u` son la misma orden. Cuando las
    // opciones estaban en la gramática solo se admitían al final, así que la primera forma
    // —la que documenta la propia ayuda— no se analizaba. Por eso las recoge el léxico.
    {
        const auto a = analizaLinea("allow --user linarese snapshot,mount");
        const auto b = analizaLinea("allow snapshot,mount --user linarese");
        igual(a.opciones.count("user") ? a.opciones.at("user") : "", "linarese",
              "«allow --user u perms»: la opción delante");
        igual(a.uno("texto"), "snapshot,mount", "«allow --user u perms»: y el argumento detrás");
        igual(b.opciones.count("user") ? b.opciones.at("user") : "", "linarese",
              "«allow perms --user u»: la opción detrás");
        igual(b.uno("texto"), "snapshot,mount", "«allow perms --user u»: mismo argumento");
    }
    // --- El valor de una opción, ENTRECOMILLADO, es uno solo aunque lleve espacios.
    //
    // Sin esto se cortaba en el primer espacio: `--flags "-w -L"` dejaba el valor en «"-w»
    // y «-L"» se leía como un componente suelto de la orden. Es el mismo trato que ya
    // tenían los componentes corrientes, que a un punto de montaje con espacios le quitan
    // las comillas desde el principio.
    {
        const auto a = analizaLinea("edit prueba --name \"Casa Mia\"");
        igual(a.opciones.count("name") ? a.opciones.at("name") : "", "Casa Mia",
              "«--name \"Casa Mia\"»: un solo valor, sin comillas");
        igual(a.objetivo, "prueba", "«--name \"Casa Mia\"»: y el destino sigue en su sitio");
        const auto b = analizaLinea("create datos --mountpoint \"/mnt/con espacio\"");
        igual(b.opciones.count("mountpoint") ? b.opciones.at("mountpoint") : "",
              "/mnt/con espacio", "«--mountpoint \"/mnt/con espacio\"»: el espacio no parte el valor");
        // Control: sin comillas el valor es el componente siguiente y nada más.
        const auto c = analizaLinea("edit prueba --name Casa");
        igual(c.opciones.count("name") ? c.opciones.at("name") : "", "Casa",
              "«--name Casa»: sin comillas, el valor es el componente");
    }
    {
        // Una opción SIN valor no se traga el componente siguiente.
        const auto a = analizaLinea("rsync --check /a/b");
        igual(a.uno("destino"), "/a/b", "«rsync --check <url>»: --check no se come la URL");
        comprueba(a.tiene("--check"), "«rsync --check <url>»: y --check se reconoce");
    }

    // --- Toda opción DECLARADA tiene que reconocerse al escribirla.
    //
    // Es el fallo que se coló entero en la migración a la gramática: el léxico guarda las
    // opciones largas SIN los dos guiones, y el código preguntaba `tiene("--wait")`, que
    // devolvía siempre false. NINGUNA opción larga funcionaba —`--daemon`, `--delete`,
    // `--check`, `--wait`, `--all`…— y no fallaba nada: la orden seguía como si no se
    // hubiera escrito. Lo encontró el usuario, no las pruebas.
    for (const zfsmgr::cli::Orden& o : zfsmgr::cli::ordenes()) {
        for (const zfsmgr::cli::Parametro& par : o.params) {
            const std::string forma = par.forma.es;
            // La línea puede ser «--delete», «--name / --type / --os» o «--password-fd <n>».
            std::size_t i = 0;
            while ((i = forma.find("--", i)) != std::string::npos) {
                std::size_t fin = forma.find_first_of(" /<", i);
                const std::string opcion =
                    forma.substr(i, fin == std::string::npos ? std::string::npos : fin - i);
                i += 2;
                if (opcion.size() <= 2) {
                    continue;
                }
                const auto a = analizaLinea(std::string(o.nombre) + rellenoDe(o) + " " + opcion + " x");
                comprueba(a.tiene(opcion),
                          std::string("«") + o.nombre + " " + opcion + "»: la opción no se "
                          "reconoce al preguntarla con guiones");
                // Y si LLEVA valor, que lo capture. Quién lleva valor lo decide el léxico
                // consultando esta misma línea del catálogo, así que aquí se contrasta la
                // decisión con lo que está escrito.
                const bool conValor = forma.find('<') != std::string::npos;
                if (conValor && a.error.empty()) {
                    const auto it = a.opciones.find(opcion.substr(2));
                    comprueba(it != a.opciones.end() && it->second == "x",
                              std::string("«") + o.nombre + " " + opcion + " x»: no capturó el "
                              "valor «x»");
                }
            }
        }
    }

    // --- Las banderas del mandato ORIGINAL de zfs/zpool.
    //
    // Las órdenes puras deben admitir lo que admite el mandato al que envuelven, y su
    // valor tiene que capturarse. `import apar -N` se tragaba la bandera sin protestar y
    // sin pasarla; `trim -r 100M` mandaba «100M» a la ranura del disco.
    for (const zfsmgr::cli::Orden& o : zfsmgr::cli::ordenes()) {
        for (const zfsmgr::cli::Nativa& n : o.nativas) {
            const std::string forma = n.forma;
            const std::string linea =
                std::string(o.nombre) + rellenoDe(o) + " " + forma + (n.valor ? " v" : "");
            const auto a = analizaLinea(linea);
            comprueba(a.error.empty(), std::string("«") + linea + "» no se analiza: " + a.error);
            comprueba(a.tiene(forma),
                      std::string("«") + linea + "»: la bandera nativa no se reconoce");
            if (n.valor) {
                std::string pelada = forma;
                while (!pelada.empty() && pelada.front() == '-') {
                    pelada.erase(pelada.begin());
                }
                const auto it = a.opciones.find(pelada);
                comprueba(it != a.opciones.end() && it->second == "v",
                          std::string("«") + linea + "»: no capturó el valor de la bandera");
            }
        }
    }

    std::printf(fallos ? "FALLOS: %d\n" : "gramatica_cli_test OK\n", fallos);
    return fallos ? 1 : 0;
}
