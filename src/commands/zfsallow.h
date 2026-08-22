#pragma once

#include <string>
#include <vector>

// Los permisos DELEGADOS de un dataset: leer lo que dice `zfs allow` y componer las órdenes
// que lo cambian.
//
// El formato de salida de `zfs allow` es de los que parecen fáciles hasta que se miran: son
// secciones con título, sangría con tabulador, y el ALCANCE —local, a los descendientes, a
// los dos, al crear— va en el título de la sección y no en la línea. Perder ese detalle
// significa conceder a los descendientes lo que se quería conceder solo aquí.
//
// Vive en la capa base porque es una regla de formato, no de interfaz, y porque la escriben
// dos clientes: la ventana de Qt y el servidor web.
namespace zfsmgr::base::zfsallow {

// Dónde se aplica. Es lo que distingue las secciones de la salida, y lo que decide qué
// bandera lleva la orden.
enum class Alcance {
    Local,                 // -l : solo este dataset
    Descendientes,         // -d : solo los de debajo
    LocalYDescendientes,   //      sin bandera: los dos
    AlCrear,               // -c : lo que recibe quien crea un descendiente
    Conjunto,              // -s : un @conjunto de permisos con nombre
};

// A quién.
enum class Quien {
    Usuario,     // -u
    Grupo,       // -g
    Todos,       // -e : «everyone»
    Conjunto,    // el propio @nombre, en la sección de conjuntos
};

struct Entrada {
    Alcance alcance{Alcance::LocalYDescendientes};
    Quien quien{Quien::Usuario};
    std::string nombre;                 // vacío cuando `quien` es Todos
    std::vector<std::string> permisos;  // «create», «mount», «@basico»…
};

const char* claveDe(Alcance a);

// El TÍTULO exacto que escribe `zfs allow` para esa sección, y la palabra exacta que usa
// para el destinatario. No son para leer: son para las salidas de máquina —el tsv y el json
// del intérprete— que ya usaban esos textos y que un guion puede estar comparando. Cambiar
// esas cadenas por unas más bonitas rompería el guion sin decir nada.
const char* seccionZfs(Alcance a);
const char* tokenZfs(Quien q);
const char* claveDe(Quien q);
std::string etiquetaDe(Alcance a);
std::string etiquetaDe(Quien q);
Alcance alcanceDesde(const std::string& clave);
Quien quienDesde(const std::string& clave);

// Lo que dice `zfs allow <dataset>`, convertido en entradas. Una salida vacía —un dataset
// sin nada delegado— devuelve una lista vacía y NO es un error.
std::vector<Entrada> analiza(const std::string& salida);

// El argv de `zfs allow` y el de `zfs unallow` para una entrada, con el dataset al final.
//
// Se devuelve el argv y no una cadena a propósito: quien lo ejecuta es el daemon con
// `execvp`, y una cadena habría que volver a partirla —y un nombre de usuario con un
// espacio dentro rompería el reparto.
std::vector<std::string> argvConceder(const Entrada& e, const std::string& dataset);
std::vector<std::string> argvRetirar(const Entrada& e, const std::string& dataset);

}  // namespace zfsmgr::base::zfsallow
