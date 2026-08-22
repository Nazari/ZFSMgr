#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Sincronizar un árbol de ficheros ENTRE DOS MÁQUINAS, por el socket entre daemons.
//
// Es el hermano remoto de `copytree`. Existe porque entre máquinas no había forma decente
// de sincronizar con un extremo Windows: rsync no está allí, y el respaldo por tar copia
// pero NO borra ni sabe simular, así que no sincroniza. Ver la evaluación en
// docs/diseno_tecnico_transferencias.md.
//
// **Por qué no rsync.** rsync entre máquinas lo lanzaría el daemon del origen, que
// necesitaría SSH propio contra el destino —clave o contraseña suyas, no las del cliente—
// y rsync instalado en los dos lados. Por el socket entre daemons no hace falta ninguna de
// las dos cosas: a cada daemon se le habla por su canal mTLS, y el socket de datos se
// autentica con un testigo de un solo uso.
//
// **El reparto**: aquí está todo lo que se puede comprobar sin tocar la red —recorrer,
// comparar, decidir qué hacer, y el formato de cable—. Los sockets los pone el daemon,
// que ya tiene el relé montado y endurecido.
namespace zfsmgr::arbolremoto {

enum class Tipo {
    Directorio,
    Fichero,
    Enlace,      // simbólico; viaja su destino, no su contenido
    EnlaceDuro,  // otra ruta del MISMO fichero, ya enviada antes
};

struct Entrada {
    // Relativa a la raíz, y SIEMPRE con «/». Windows usa «\» en disco, pero el cable no:
    // si cada extremo mandara su separador, ninguna comparación casaría.
    std::string ruta;
    Tipo tipo{Tipo::Fichero};
    std::uint64_t tamano{0};
    // Segundos desde el epoch, enteros.
    //
    // **No es pereza, es lo único comparable.** NTFS guarda 100 ns, ext4 nanosegundos y
    // HFS+ un segundo: comparar la marca exacta entre dos de ellos da «distinto» siempre,
    // y cada pasada volvería a copiar el árbol entero. Es la misma granularidad que usa
    // `rsync --modify-window=1`, y tiene su misma consecuencia: un cambio dentro del mismo
    // segundo que además conserve el tamaño no se detecta.
    std::int64_t fecha{0};
    std::uint32_t modo{0};
    // Para `Enlace`, a dónde apunta. Para `EnlaceDuro`, la ruta hermana ya enviada.
    std::string destino;
};

// Recorre `raiz` y devuelve su contenido, ordenado por ruta.
//
// Los enlaces duros se detectan por (dispositivo, inodo) y se devuelven como `EnlaceDuro`
// apuntando a la primera ruta que los trajo. En Windows NO se detectan y van como ficheros
// sueltos: la API existe pero es cara, y allí son raros. Se dice aquí para que quien lea el
// resultado no crea que se han preservado.
bool recorre(const std::string& raiz, std::vector<Entrada>& salida, std::string& error,
             bool unSoloSistema = false);

// El manifiesto: qué tiene ya el destino. Una línea por entrada.
std::string serializaManifiesto(const std::vector<Entrada>& entradas);
bool analizaManifiesto(const std::string& texto, std::vector<Entrada>& salida,
                       std::string& error);

enum class Accion {
    CrearDirectorio,
    Copiar,
    Enlazar,
    EnlazarDuro,
    Borrar,
};

struct Operacion {
    Accion accion{Accion::Copiar};
    Entrada entrada;
};

struct Plan {
    std::vector<Operacion> operaciones;
    std::uint64_t bytes{0};      // lo que habría que transferir
    std::uint64_t iguales{0};    // lo que ya estaba bien y no se toca
};

// Qué hay que hacer para que el destino quede como el origen.
//
// El borrado va AL FINAL y de más hondo a menos hondo, para que un directorio se borre
// después de su contenido. Si se hiciera al revés, borrar un directorio con cosas dentro
// falla y el error no explica por qué.
Plan planea(const std::vector<Entrada>& origen, const std::vector<Entrada>& destino,
            bool borraLoQueSobra);

// Una línea legible por operación, al estilo de `rsync -i`. Es lo que ve quien pide la
// pasada en seco, así que dice QUÉ y sobre qué, no cuántos.
std::string describe(const Operacion& o);

// La cabecera de una operación en el cable: una línea de texto y, si es un fichero, sus
// bytes en crudo detrás.
//
// Formato: `<letra> <modo> <fecha> <tamaño> <largoRuta> <largoDestino>\n` y a continuación
// la ruta y el destino pegados, sin separador. Las longitudes van explícitas porque un
// nombre de fichero puede llevar dentro saltos de línea y espacios.
std::string cabeceraDe(const Operacion& o);
bool analizaCabecera(const std::string& linea, Operacion& salida, std::size_t& largoRuta,
                     std::size_t& largoDestino, std::string& error);

// ---------------------------------------------------------------------------
// Transferencia DELTA: mandar solo lo que cambió dentro de un fichero.
//
// Es el algoritmo de rsync, y NO xdelta. xdelta calcula la diferencia entre dos ficheros
// que están los dos en la misma máquina; aquí ninguna de las dos las tiene, que es el
// problema entero. El de rsync está pensado justo para esta forma:
//
//   1. El DESTINO parte su copia en bloques y manda, por bloque, una suma débil rodante y
//      un hash fuerte.
//   2. El ORIGEN desliza una ventana byte a byte sobre su versión. La suma débil se
//      actualiza en O(1) por byte —ese es el truco—, y cuando coincide con alguna conocida
//      se confirma con el hash fuerte.
//   3. Manda instrucciones: «copia N bloques tuyos desde el índice i» o «aquí van estos
//      bytes».
//
// Byte a byte y no bloque a bloque a propósito: si alguien inserta un byte al principio del
// fichero, comparar bloques alineados no reconocería ni uno solo, y la ventana deslizante
// los reconoce todos desplazados.
// ---------------------------------------------------------------------------

// A partir de qué tamaño compensa. Por debajo, las firmas y la vuelta de red cuestan más
// que mandar el fichero entero; rsync aplica un umbral por lo mismo.
constexpr std::uint64_t kMinimoParaDelta = 1024 * 1024;

struct Firma {
    std::uint32_t debil{0};
    // SHA-256 recortado. Recortar está bien porque el hash fuerte solo confirma una
    // coincidencia que la suma débil ya propuso, y además al final se comprueba el fichero
    // ENTERO: una colisión aquí se detecta allí en vez de corromper en silencio.
    unsigned char fuerte[16]{};
};

// Cuánto mide un bloque para un fichero de ese tamaño. Por tramos y no por raíz cuadrada:
// es predecible, y que los dos extremos calculen lo MISMO es más importante que afinarlo.
std::size_t tamanoDeBloque(std::uint64_t tamanoFichero);

// La suma rodante de rsync sobre un trozo.
std::uint32_t sumaRodante(const unsigned char* datos, std::size_t n);

std::string hashFuerteHex(const unsigned char* datos, std::size_t n);
// El hash del fichero entero, para comprobar que lo reconstruido es lo que tenía que ser.
bool hashDeFichero(const std::string& ruta, std::string& hexOut, std::string& error);

bool firmasDe(const std::string& ruta, std::size_t tamBloque, std::vector<Firma>& salida,
              std::string& error);
std::string serializaFirmas(const std::vector<Firma>& f);
bool analizaFirmas(const std::string& datos, std::vector<Firma>& salida, std::string& error);

enum class TipoInstruccion { Copiar, Literal };

struct Instruccion {
    TipoInstruccion tipo{TipoInstruccion::Literal};
    std::uint64_t bloque{0};   // Copiar: primer bloque del destino
    std::uint64_t cuantos{0};  // Copiar: cuántos bloques seguidos
    std::string datos;         // Literal: los bytes
};

// Qué hay que mandar para que el destino reconstruya `ruta` a partir de lo que ya tiene.
//
// `bytesLiterales` es lo que de verdad viajaría: si sale casi igual al tamaño del fichero,
// el delta no ha servido de nada y quien llama puede preferir mandarlo entero.
bool delta(const std::string& ruta, const std::vector<Firma>& firmas, std::size_t tamBloque,
           std::vector<Instruccion>& salida, std::uint64_t& bytesLiterales, std::string& error);

// Poner en el destino la fecha y el modo que traía el origen.
//
// La fecha hay que ponerla SIEMPRE tras escribir un fichero: si se deja la del momento de
// la copia, la siguiente pasada lo verá distinto y lo volverá a traer entero. Es la
// diferencia entre sincronizar y copiar una y otra vez.
bool ponFecha(const std::string& ruta, std::int64_t segundos);
bool ponModo(const std::string& ruta, std::uint32_t modo);

// La fecha de un fichero en segundos desde el epoch, tal y como la ve el sistema.
// Expuesta para poder comprobar que lo escrito quedó con la fecha que tenía que quedar.
std::int64_t fechaDeFichero(const std::string& ruta, bool& ok);

}  // namespace zfsmgr::arbolremoto
