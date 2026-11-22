#include "secretinput.h"

#include "tr.h"

#include <csignal>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace zfsmgr::cli {
namespace {

volatile std::sig_atomic_t g_interrumpido = 0;

#ifndef _WIN32
void alInterrumpir(int) {
    g_interrumpido = 1;
}

// The terminal of a question with Ctrl-C as a CHARACTER that ends the line (VEOL), not as a
// signal: the answer comes back with a \x03 in it and is taken as cancelled. Restored on
// destruction whatever happens.
struct CtrlCComoTecla {
    termios viejo{};
    bool ok{false};
    explicit CtrlCComoTecla(bool sinEco) {
        ok = ::tcgetattr(STDIN_FILENO, &viejo) == 0;
        if (!ok) {
            return;
        }
        termios nuevo = viejo;
        nuevo.c_lflag &= ~static_cast<tcflag_t>(ISIG);
        if (sinEco) {
            nuevo.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        }
        nuevo.c_cc[VEOL] = 3;
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &nuevo);
    }
    ~CtrlCComoTecla() {
        if (ok) {
            ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &viejo);
        }
    }
};
#endif

// Borra el contenido antes de soltarlo. Mismo criterio que en la capa base: un secreto no
// se queda en memoria más de lo necesario.
void limpia(std::string& s) {
    if (!s.empty()) {
        // volatile para que el compilador no elimine el borrado por considerarlo inútil,
        // que es lo que pasa con un memset sobre memoria que va a morir.
        volatile char* p = &s[0];
        for (std::size_t i = 0; i < s.size(); ++i) {
            p[i] = '\0';
        }
    }
}

}  // namespace

bool hayTerminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}

bool leerSecretoDeDescriptor(int fd, std::string& out, std::string& error) {
    out.clear();
    error.clear();
    if (fd < 0) {
        error = T("t_desc_no_valido", "descriptor no válido");
        return false;
    }
    char buf[512];
    while (true) {
#ifdef _WIN32
        const int n = _read(fd, buf, static_cast<unsigned>(sizeof(buf)));
#else
        const ssize_t n = ::read(fd, buf, sizeof(buf));
#endif
        if (n < 0) {
            error = T("t_secreto_fd_ilegible", "no se pudo leer del descriptor: ")
                    + std::strerror(errno);
            limpia(out);
            out.clear();
            return false;
        }
        if (n == 0) {
            break;
        }
        out.append(buf, static_cast<std::size_t>(n));
        // El primer salto de línea termina el secreto: así funciona con `3< <(pass show …)`
        // y con un fichero que acabe en salto.
        const std::size_t nl = out.find('\n');
        if (nl != std::string::npos) {
            out.resize(nl);
            break;
        }
    }
    // Solo el retorno de carro de un fichero con finales de línea de Windows. NADA más se
    // recorta: una contraseña puede llevar espacios en los extremos.
    if (!out.empty() && out.back() == '\r') {
        out.pop_back();
    }
    // Los bytes leídos de más siguen en el buffer de la pila; se limpian.
    volatile char* p = buf;
    for (std::size_t i = 0; i < sizeof(buf); ++i) {
        p[i] = '\0';
    }
    return true;
}

namespace {
AntesDePreguntar g_antes = nullptr;
DespuesDePreguntar g_despues = nullptr;
}  // namespace

void ponGanchosDePregunta(AntesDePreguntar antes, DespuesDePreguntar despues) {
    g_antes = antes;
    g_despues = despues;
}

bool preguntarSecretoPorTerminal(const std::string& aviso, std::string& out, std::string& error) {
    out.clear();
    error.clear();
    if (!hayTerminal()) {
        error = T("t_no_hay_term_fd", "no hay terminal: use --password-fd");
        return false;
    }
    // El aviso va por la salida de ERROR, no por la estándar: así se puede canalizar la
    // salida del programa a un fichero sin que el mensaje acabe dentro.
    if (g_antes) {
        g_antes(aviso);
    } else {
        std::fputs(aviso.c_str(), stderr);
        std::fflush(stderr);
    }

    bool ok = false;
#ifdef _WIN32
    const HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD modo = 0;
    const bool teniaModo = GetConsoleMode(h, &modo) != 0;
    if (teniaModo) {
        SetConsoleMode(h, modo & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
    }
#endif
    bool cancelado = false;
    {
#ifndef _WIN32
        // Without echo, and Ctrl-C as a key that cancels. Its destructor restores the mode
        // ALWAYS, also if reading failed: a terminal left without echo has the user typing
        // blind in their own shell.
        const CtrlCComoTecla modoPregunta(/*sinEco=*/true);
#endif
        std::string linea;
        int c = 0;
        while ((c = std::fgetc(stdin)) != EOF && c != '\n' && c != 3) {
            if (c != '\r') {
                linea.push_back(static_cast<char>(c));
            }
        }
        cancelado = c == 3;
        ok = !cancelado && (!linea.empty() || c != EOF);
        out = cancelado ? std::string() : linea;
        limpia(linea);
    }
#ifdef _WIN32
    if (teniaModo) {
        SetConsoleMode(h, modo);
    }
#endif
    if (g_despues) {
        g_despues(std::string(), /*secreto=*/true);
    } else {
        std::fputs("\n", stderr);  // el salto que el usuario no vio al pulsar Intro
    }
    if (cancelado) {
        error = T("t_pq_ctrl_c", "cancelado con Ctrl-C");
    } else if (!ok) {
        error = T("t_no_se_leyo_nada", "no se leyó nada");
    }
    return ok;
}

bool preguntarPorTerminal(const std::string& aviso, std::string& out, std::string& error) {
    out.clear();
    error.clear();
    if (!hayTerminal()) {
        error = T("t_no_hay_term_preg", "no hay terminal para preguntar");
        return false;
    }
    // Igual que la de arriba menos el apagado del eco, y por el mismo motivo el aviso va
    // por la salida de ERROR: así la estándar se puede canalizar a un fichero.
    if (g_antes) {
        g_antes(aviso);
    } else {
        std::fputs(aviso.c_str(), stderr);
        std::fflush(stderr);
    }
    std::string linea;
    int c = 0;
    {
#ifndef _WIN32
        const CtrlCComoTecla modoPregunta(/*sinEco=*/false);
#endif
        while ((c = std::fgetc(stdin)) != EOF && c != '\n' && c != 3) {
            if (c != '\r') {
                linea.push_back(static_cast<char>(c));
            }
        }
    }
    if (c == 3) {
        // Ctrl-C: the answer is abandoned, as in the line editor.
        if (g_despues) {
            g_despues(linea + "^C", /*secreto=*/false);
        } else {
            std::fputs("\n", stderr);
        }
        error = T("t_pq_ctrl_c", "cancelado con Ctrl-C");
        return false;
    }
    if (g_despues) {
        g_despues(linea, /*secreto=*/false);
    }
    if (linea.empty() && c == EOF) {
        error = T("t_no_se_leyo_nada", "no se leyó nada");
        return false;
    }
    out = linea;
    return true;
}

void instalaInterrupcion() {
#ifndef _WIN32
    struct sigaction sa{};
    sa.sa_handler = alInterrumpir;
    sigemptyset(&sa.sa_mask);
    // SA_RESTART: nothing here relies on an interrupted call; what stops is the children
    // (they get the terminal's SIGINT) and the waits that look at the flag.
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
#endif
}

bool interrumpido() {
    return g_interrumpido != 0;
}

void limpiaInterrupcion() {
    g_interrumpido = 0;
}

}  // namespace zfsmgr::cli
