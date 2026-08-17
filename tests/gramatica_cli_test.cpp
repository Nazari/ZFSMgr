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

    std::printf(fallos ? "FALLOS: %d\n" : "gramatica_cli_test OK\n", fallos);
    return fallos ? 1 : 0;
}
