#include "secretinput.h"

#include "tr.h"

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
            error = std::string("no se pudo leer del descriptor: ") + std::strerror(errno);
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

bool preguntarSecretoPorTerminal(const std::string& aviso, std::string& out, std::string& error) {
    out.clear();
    error.clear();
    if (!hayTerminal()) {
        error = T("t_no_hay_term_fd", "no hay terminal: use --password-fd");
        return false;
    }
    // El aviso va por la salida de ERROR, no por la estándar: así se puede canalizar la
    // salida del programa a un fichero sin que el mensaje acabe dentro.
    std::fputs(aviso.c_str(), stderr);
    std::fflush(stderr);

    bool ok = false;
#ifdef _WIN32
    const HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD modo = 0;
    const bool teniaModo = GetConsoleMode(h, &modo) != 0;
    if (teniaModo) {
        SetConsoleMode(h, modo & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
    }
#else
    termios viejo{};
    const bool teniaModo = ::tcgetattr(STDIN_FILENO, &viejo) == 0;
    if (teniaModo) {
        termios nuevo = viejo;
        nuevo.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &nuevo);
    }
#endif
    {
        std::string linea;
        int c = 0;
        while ((c = std::fgetc(stdin)) != EOF && c != '\n') {
            if (c != '\r') {
                linea.push_back(static_cast<char>(c));
            }
        }
        ok = !linea.empty() || c != EOF;
        out = linea;
        limpia(linea);
    }
    // El modo se restaura SIEMPRE, también si la lectura falló: dejar el terminal sin eco
    // deja al usuario escribiendo a ciegas en su propia shell.
#ifdef _WIN32
    if (teniaModo) {
        SetConsoleMode(h, modo);
    }
#else
    if (teniaModo) {
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &viejo);
    }
#endif
    std::fputs("\n", stderr);  // el salto que el usuario no vio al pulsar Intro
    if (!ok) {
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
    std::fputs(aviso.c_str(), stderr);
    std::fflush(stderr);
    std::string linea;
    int c = 0;
    while ((c = std::fgetc(stdin)) != EOF && c != '\n') {
        if (c != '\r') {
            linea.push_back(static_cast<char>(c));
        }
    }
    if (linea.empty() && c == EOF) {
        error = T("t_no_se_leyo_nada", "no se leyó nada");
        return false;
    }
    out = linea;
    return true;
}

}  // namespace zfsmgr::cli
