#pragma once

#include <string>

// Qué crea `create`, y dónde.
//
// **Por qué es una función y no un `if` dentro de `cmdCreate`.** La orden es la misma en
// los tres niveles —«crea un nodo donde estás»— y la decisión se tomaba mirando SOLO dónde
// estaba uno parado, sin leer el argumento. Desde la raíz, `create unibody/sback/tmp`
// intentaba dar de alta una CONEXIÓN llamada «unibody/sback/tmp»: un nombre con barras,
// que no es un identificador de conexión ni puede serlo.
//
// La regla que faltaba es que **la forma del nombre también decide**: un nombre con barra
// nombra algo que cuelga de otra cosa, así que no puede ser una conexión —ni un pool, si
// lleva más de una—. Escrita aquí se puede recorrer entera en una prueba; escrita dentro
// de una función de cuarenta líneas que además habla con el daemon, no.
namespace zfsmgr::cli::creacion {

// Dónde está uno al escribir la orden.
enum class Nivel { Raiz, Conexion, Dataset };

// Qué hay que crear.
enum class Objeto { Conexion, Pool, Dataset, Instantanea };

struct Decision {
    Objeto que{Objeto::Conexion};
    // La ruta que hay que resolver para saber en qué MÁQUINA se crea. Vacía si es la del
    // sitio actual.
    std::string ruta;
    // El nombre del objeto, ya sin la parte que era ruta.
    std::string nombre;
};

// `texto` es el primer argumento de `create`, tal y como se tecleó.
//
// No comprueba que la conexión exista ni que el nombre valga: eso es de quien llama, que
// es el único que puede preguntarlo. Aquí solo se reparte por la FORMA.
Decision queSeCrea(Nivel donde, const std::string& texto);

}  // namespace zfsmgr::cli::creacion
