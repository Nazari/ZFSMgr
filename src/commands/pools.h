#pragma once

#include <string>
#include <vector>

// Las operaciones de mantenimiento de un pool: `zpool <op> [banderas] <pool> [discos]`.
//
// **Lo que se guarda aquí no es el argv —eso es trivial— sino tres reglas que se han
// aprendido viéndolas fallar.**
//
// 1. `parar` y `pausar` NO son la misma letra en todas. En `scrub` son `-s` y `-p`; en
//    `trim` e `initialize` son `-c` y `-s`. O sea que **`-s` significa «parar» en scrub y
//    «suspender» en initialize**. Un cliente que use la misma letra para las tres pone un
//    botón que dice una cosa y hace otra —pasó: «Parar initialize» mandaba `-s`, que
//    suspende—.
// 2. El orden que pide zpool es BANDERAS, luego el pool, luego los discos. Las dos mitades
//    vienen de verlo fallar: con los discos delante, `trim <pool> <disco>` respondía
//    «invalid character '/' in pool name»; con las banderas detrás del pool, zpool las
//    ignora EN SILENCIO —`trim -r noesunritmo` decía «en marcha» y el historial registraba
//    `zpool trim <pool>` a secas—. Aceptada y no aplicada es la peor de las dos formas de
//    fallar.
// 3. Cuáles hay que confirmar, y no es solo «las que destruyen»: `clear` no borra datos pero
//    borra la CUENTA DE ERRORES del pool, y se teclea queriendo limpiar el terminal —pasó
//    dos veces en una misma sesión de pruebas—. Perder eso sin haberlo pedido es perder
//    justo lo que uno estaba mirando.
namespace zfsmgr::commands::pools {

enum class Operacion {
    Scrub,
    Trim,
    Initialize,
    Clear,
    Sync,       // `zpool sync`, lo que la interfaz llama «Flush»
    Export,
    Import,
    Destroy,
    Upgrade,
    Reguid,
};

enum class Fase {
    Arrancar,
    Parar,     // scrub: -s   trim/initialize: -c
    Pausar,    // scrub: -p   trim/initialize: -s
};

// El subcomando tal y como lo espera `zpool`.
const char* subcomando(Operacion op);

// ¿Admite parar y pausar? Solo las tres que son procesos largos.
bool admiteFase(Operacion op);

// ¿Hay que preguntar antes? Ver la regla 3 de arriba.
bool pideConfirmacion(Operacion op);

// ¿No se puede deshacer? Es un subconjunto de las que se confirman, y sirve para que quien
// pregunte pueda decirlo con las palabras adecuadas en vez de con un «¿seguro?» genérico.
bool esIrreversible(Operacion op);

// `zpool <sub> [fase] [banderas] <pool> [discos]`, en ese orden y por el motivo de la
// regla 2.
//
// Devuelve vacío si el pool no sirve, o si se pide una fase a una operación que no la
// admite: `zpool export -s` no es «parar la exportación», es un error de sintaxis, y vale
// más no mandarlo que traducir el mensaje de zpool.
std::vector<std::string> argv(Operacion op, const std::string& pool, Fase fase = Fase::Arrancar,
                              const std::vector<std::string>& banderas = {},
                              const std::vector<std::string>& discos = {});

// `zpool import <viejo> <nuevo>`: importar cambiando el nombre.
//
// Va aparte porque es la única que lleva DOS nombres de pool, y porque el nuevo hay que
// validarlo: ZFS admite letras, dígitos y `_-.:`, y tiene que empezar por letra.
std::vector<std::string> argvImportarComo(const std::string& pool, const std::string& nombreNuevo);
bool nombreDePoolValido(const std::string& nombre);

}  // namespace zfsmgr::commands::pools
