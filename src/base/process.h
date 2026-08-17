#pragma once

#include <cstdint>
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

// --- Un proceso que se queda VIVO entre llamadas.
//
// Todo lo de arriba lanza algo, espera y recoge. Un túnel `ssh -L` no es eso: se levanta,
// se usa muchas veces y se cierra cuando ya no hace falta. Eso era lo último que obligaba
// a que los túneles fueran `QProcess` colgados de un objeto con bucle de eventos.
//
// **El destructor lo mata.** Un `ssh -L` que sobrevive a quien lo creó deja un puerto
// escuchando y una conexión abierta contra la otra máquina, y nadie vuelve a cerrarlos.
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();
    // Ni copiable ni asignable: dos objetos con el mismo hijo lo matarían dos veces.
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& otro) noexcept;
    ChildProcess& operator=(ChildProcess&& otro) noexcept;

    // Lanza. Devuelve false si no se pudo. Como en el resto del fichero, SIN intérprete.
    bool start(const std::string& program, const std::vector<std::string>& args);

    // ¿Sigue vivo? No bloquea, y además RECOGE al hijo si acaba de morir: sin esto, cada
    // túnel cerrado dejaría un zombi.
    bool isRunning();

    // Termina con educación y, si no hace caso en `msEspera`, sin ella. Es idempotente.
    void stop(int msEspera = 1500);

    long long pid() const { return m_pid; }

private:
    void olvida();
    long long m_pid{0};
#ifdef _WIN32
    void* m_handle{nullptr};
#endif
    bool m_recogido{true};
};

// --- Puertos locales.

// Reserva un puerto libre en 127.0.0.1 y lo suelta. Devuelve 0 si no hay ninguno.
//
// **Hay una carrera y es inevitable**: entre soltarlo y que `ssh -L` lo tome, otro proceso
// podría cogerlo. Es lo mismo que hacía la versión con Qt, y la alternativa —pasarle a ssh
// un descriptor ya abierto— no existe en su línea de órdenes. Si ocurre, `ssh` falla al
// reenviar y el túnel no se da por bueno, que es el comportamiento correcto.
std::uint16_t reserveFreeLocalPort();

// ¿Acepta ya conexiones ese puerto en 127.0.0.1? Es la pregunta que hay que hacerle a un
// túnel recién montado: conectarse antes de tiempo da ECONNREFUSED, y quien llama lo
// contaba como fallo del saludo TLS y castigaba la conexión sin motivo.
bool canConnectLocal(std::uint16_t port, int timeoutMs);

#ifdef _WIN32
// CreateProcess recibe UNA cadena y es el propio programa quien la vuelve a trocear, así
// que el entrecomillado es responsabilidad de quien la construye. La regla no es la
// intuitiva: las barras invertidas solo se duplican cuando preceden a una comilla.
std::string winBuildCommandLine(const std::string& program,
                                const std::vector<std::string>& args);
#endif

}  // namespace zfsmgr::base
