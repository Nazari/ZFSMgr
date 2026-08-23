#pragma once

#include <string>
#include <vector>

// El catálogo de órdenes, como DATOS y no como un bloque de texto.
//
// Esto empezó siendo un `fprintf` de sesenta líneas con la alineación puesta a mano, que
// se descuadraba en cuanto una orden crecía. Pasarlo a datos arregla eso y además da dos
// cosas gratis que con texto no se podían: `help <orden>` y el completado con el
// tabulador, que necesitan saber qué órdenes hay y qué acepta cada una.
namespace zfsmgr::cli {

// Un texto traducible: la clave y el castellano, JUNTOS.
//
// Van en una estructura y no en una lista plana de cadenas alternas a propósito: con la
// lista, olvidar una clave no daba error de compilación, solo hacía que un párrafo
// desapareciera de la ayuda. Pasó, y lo cazó el contraste de la salida en castellano — que
// es justo el fallo que no debe depender de que alguien mire.
struct Texto {
    const char* clave;
    const char* es;
};

struct Parametro {
    // «--from <@instantánea>»: va ENTERA por el catálogo. Mezcla el nombre de la opción
    // —que no cambia nunca— con un marcador que sí se lee; quien traduzca deja el nombre de
    // la opción tal cual y traduce el marcador. Partirla en dos campos obligaría a
    // recomponerla al imprimir sin ganar nada.
    Texto forma;
    Texto que;  // qué hace
};

// Sobre qué nodo actúa una orden. Es la primera mitad de su FIRMA.
//
// Existe porque hasta ahora cada orden resolvía su destino por su cuenta, con cuatro
// convenciones distintas conviviendo: 27 órdenes troceaban a mano, 10 usaban una función,
// 5 usaban otra que NO miraba los argumentos sueltos —de ahí que `install-daemon oldlau`
// reinstalara en la máquina local sin decir nada— y 4 una tercera.
//
// Ver docs/gramatica_cli.md.
enum class Objetivo {
    Ninguno,     // la orden no actúa sobre un nodo (help, exit, format…)
    Cualquiera,  // vale donde sea, incluida la raíz (info, ls, cd)
    Conexion,    // la máquina
    Pool,        // la raíz de un pool
    Dataset,     // un dataset, pool incluido
    Instantanea,
    DatasetOInstantanea,
};

// Una ranura posicional de la orden. La segunda mitad de la firma.
struct Ranura {
    enum class Tipo {
        Url,        // se resuelve y se COMPRUEBA que el nodo es del tipo pedido
        Palabra,    // de un conjunto cerrado: stop, pause, start
        Vdev,       // ruta de dispositivo
        Propiedad,  // nombre=valor
        Ruta,       // una ruta del sistema de ficheros de la máquina
        Texto,      // cualquier cosa
    };
    enum class Cuantas { Una, Opcional, CeroOMas, UnaOMas };

    const char* nombre;
    Tipo tipo{Tipo::Texto};
    Cuantas cuantas{Cuantas::Una};
    Objetivo nodo{Objetivo::Ninguno};             // solo si tipo == Url
    std::vector<const char*> palabras;            // solo si tipo == Palabra
};

// Una bandera del programa ORIGINAL —`zfs`, `zpool`— que esta orden acepta y pasa tal cual.
//
// Se declaran porque las órdenes puras deben admitir lo mismo que el mandato de OpenZFS al
// que envuelven, y porque lo que NO esté aquí tiene que rechazarse. Sin la lista, `import
// apar -N` se tragaba la bandera sin protestar y sin pasarla: ni hacía lo pedido ni lo
// decía.
//
// No se traducen ni se documentan una a una: son de OpenZFS, están en su manual, y
// repetirlas en la ayuda solo crearía una segunda copia que envejece. La ayuda las enumera
// en una línea.
struct Nativa {
    const char* forma;   // "-d", "--power"
    bool valor{false};   // ¿lleva un valor detrás? («-d <dir>»)
    // Qué hace, en una línea. Va aquí y no en un texto suelto porque la ayuda de estas
    // banderas se GENERA de esta misma lista: una descripción escrita aparte se separaría
    // de lo que el programa acepta, que es justo lo que estas listas vienen a evitar.
    Texto que{"", ""};
};

// Una línea de ejemplo: lo que se teclea y qué enseña.
//
// **La orden NO se traduce y el porqué sí.** Lo de la izquierda es sintaxis —se copia, se
// pega y se ejecuta—, así que traducirla la rompería; lo de la derecha es prosa y tiene que
// leerse en el idioma de quien mira.
//
// Que haya un ejemplo POR OPCIÓN no es una aspiración escrita en un comentario: lo
// comprueba `ayuda_test`, que recorre el catálogo y falla si una opción declarada no
// aparece en ningún ejemplo de su orden. Sin esa prueba, la lista se quedaría en las
// órdenes que existían el día que se escribió.
struct Ejemplo {
    const char* orden;
    Texto que;
};

struct Orden {
    const char* nombre;  // el VERBO: no se traduce, es lo que se teclea
    Texto grupo;
    Texto uso;  // la línea de invocación: ver Parametro::forma
    Texto resumen;
    std::vector<Parametro> params;
    // Lo que solo sale con `help <orden>`: el porqué, las trampas, los ejemplos. En la
    // lista general estorbaría; buscándola a propósito es justo lo que hace falta.
    std::vector<Texto> detalle;

    // --- La FIRMA. Lo que convierte este catálogo de documentación en fuente de verdad.
    //
    // Con ella, un único preámbulo resuelve el destino y reparte los argumentos, y **lo
    // que sobra es un error**. Esa regla es la que mata de golpe la familia de fallos de
    // «acepta un argumento y no le hace caso».
    Objetivo objetivo{Objetivo::Ninguno};
    std::vector<Ranura> ranuras;

    // Las banderas del mandato original que esta orden pasa tal cual. Ver Nativa.
    std::vector<Nativa> nativas;
};

// Los ejemplos de una orden, o vacío. Viven en una tabla aparte y no dentro de `Orden`
// porque así se leen JUNTOS —«¿tiene cada opción el suyo?» se contesta de un vistazo— y
// porque el catálogo se inicializa por posición: meter un campo en medio obligaría a tocar
// las 61 entradas. La prueba de cobertura es lo que impide que las dos listas se separen.
const std::vector<Ejemplo>& ejemplosDe(const std::string& orden);

// Todas, en el orden en que se enseñan.
const std::vector<Orden>& ordenes();

// La orden con ese nombre, o nullptr.
const Orden* ordenPorNombre(const std::string& nombre);

// La ayuda general, agrupada. `ancho` es el del terminal, para partir las descripciones.
void imprimeAyuda(int ancho);

// La de una sola orden, con su detalle. Devuelve false si no existe.
bool imprimeAyudaDe(const std::string& nombre, int ancho);

// Los nombres que empiezan por ese prefijo. Lo usa el completado.
std::vector<std::string> nombresQueEmpiezanPor(const std::string& prefijo);

// Las opciones de una orden que empiezan por ese prefijo, con el guion delante.
std::vector<std::string> opcionesQueEmpiezanPor(const std::string& orden, const std::string& prefijo);

}  // namespace zfsmgr::cli
