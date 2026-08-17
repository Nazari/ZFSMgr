#pragma once

#include <functional>
#include <string>
#include <vector>

// Ejecutar un programa: sin shell, sin Qt y en las cuatro plataformas.
//
// Es el código del agente, que llevaba tiempo haciendo esto sin Qt y está probado contra
// Linux, macOS, FreeBSD y Windows. Se saca aquí porque el CLIENTE lo necesita igual: el
// transporte usaba `QProcess`, y eso es lo que impedía que la capa de red viviera fuera
// de Qt.
//
// NUNCA hay un intérprete de por medio: se pasa argv y se ejecuta directamente. Es lo que
// hace que un nombre de dataset con `;` o con comillas no pueda convertirse en otra
// orden.
//
// Ver docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base {

// Traduce el estado que devuelve `wait` a un código de salida: el del programa, o
// 128+señal si murió por una, que es la convención de los shell.
int decodeWaitStatus(int status);

struct ExecResult {
    int rc{1};
    std::string out;
    std::string err;
};

// Ejecuta y captura salida y error por separado.
ExecResult runExecCapture(const std::string& program, const std::vector<std::string>& args);

// Ejecuta heredando la salida del proceso actual: para lo que va a la consola tal cual.
int runExecStreaming(const std::string& program, const std::vector<std::string>& args);

// Ejecuta alimentando la entrada estándar. Es lo que permite darle un flujo a `zfs recv`
// o una passphrase a `zfs load-key` sin que pase por la línea de órdenes —donde sería
// visible en `ps`—.
ExecResult runExecCaptureWithStdin(const std::string& program,
                                   const std::vector<std::string>& args,
                                   const std::string& stdinData);

// --- Ejecución con retroalimentación, para operaciones largas.
//
// `runExecCapture` basta para una orden que responde y termina. Lo que NO cubre es lo que
// necesita una transferencia: enseñar las líneas según llegan, avisar de cuánto queda y
// poder cancelar. Eso lo hacía `QProcess` bombeando el bucle de eventos de Qt, y es lo
// que ataba el transporte a la interfaz.
struct StreamCallbacks {
    // Se llaman con cada línea COMPLETA, sin el salto final. Lo que quede sin terminar en
    // línea al acabar el proceso se entrega igualmente: `zfs send` escribe el progreso
    // con retornos de carro y no siempre cierra la última.
    std::function<void(const std::string& linea)> onStdoutLine;
    std::function<void(const std::string& linea)> onStderrLine;

    // Se llama cada pocos milisegundos aunque no llegue nada. **Devolver false CANCELA**:
    // el proceso se termina y el resultado sale con el código correspondiente.
    //
    // Un solo punto de enganche para las tres cosas que hacía el bucle de Qt: dejar
    // respirar a la interfaz, contar cuánto queda, y mirar si el usuario canceló. Quien
    // no tenga interfaz simplemente no lo pone.
    std::function<bool(int msTranscurridos)> onTick;
};

// Ejecuta con retroalimentación. `timeoutMs <= 0` significa SIN límite, que es lo que
// necesita una transferencia larga; el control queda entonces en manos de `onTick`.
//
// `out` y `err` del resultado traen además el texto completo, para quien lo quiera al
// final sin haber ido acumulando.
ExecResult runExecStream(const std::string& program,
                         const std::vector<std::string>& args,
                         const std::string& stdinData,
                         int timeoutMs,
                         const StreamCallbacks& cb);

#ifdef _WIN32
// CreateProcess recibe UNA cadena y es el propio programa quien la vuelve a trocear, así
// que el entrecomillado es responsabilidad de quien la construye. La regla no es la
// intuitiva: las barras invertidas solo se duplican cuando preceden a una comilla.
std::string winBuildCommandLine(const std::string& program,
                                const std::vector<std::string>& args);
#endif

}  // namespace zfsmgr::base
