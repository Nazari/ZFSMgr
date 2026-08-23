#include "linea.h"

#include "secretinput.h"

#include <algorithm>
#include <cstdio>
#include <iostream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace zfsmgr::cli {
namespace {

#ifdef _WIN32
// No está en las cabeceras de MinGW más antiguas, y su valor es fijo desde Windows 10.
#ifdef ENABLE_VIRTUAL_TERMINAL_INPUT
constexpr DWORD kEntradaVirtual = ENABLE_VIRTUAL_TERMINAL_INPUT;
#else
constexpr DWORD kEntradaVirtual = 0x0200;
#endif
#endif

// El terminal en modo CRUDO mientras se edita: sin él, el sistema no entrega nada hasta el
// Intro y no hay forma de ver un tabulador. Se restaura SIEMPRE, también al salir por
// error: dejar un terminal sin eco deja al usuario escribiendo a ciegas en su propia shell.
class ModoCrudo {
public:
    ModoCrudo() {
#ifdef _WIN32
        m_h = GetStdHandle(STD_INPUT_HANDLE);
        m_ok = GetConsoleMode(m_h, &m_viejo) != 0;
        if (m_ok) {
            const DWORD base = m_viejo & ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
            // **Las flechas no son caracteres, y sin esto la consola NO las entrega.**
            //
            // Quitar ENABLE_LINE_INPUT basta para recibir letra a letra, pero solo llega lo
            // que produce un CARÁCTER. Una flecha, Inicio, Fin o Suprimir son teclas
            // virtuales sin carácter asociado: `fgetc` no ve absolutamente nada y el
            // historial parecía no existir. En Linux y macOS el terminal las manda como
            // secuencias de escape desde siempre, así que el fallo era solo de Windows.
            //
            // Con ENABLE_VIRTUAL_TERMINAL_INPUT la consola las traduce a esas mismas
            // secuencias (`ESC [ A`), que es justo lo que el analizador de abajo ya sabe
            // leer: no hay un segundo camino que mantener.
            //
            // Si la consola es demasiado vieja para admitirlo, `SetConsoleMode` falla y se
            // vuelve a poner el modo sin la bandera: se pierden las flechas —como hasta
            // ahora— pero escribir y borrar siguen funcionando.
            if (!SetConsoleMode(m_h, base | kEntradaVirtual)) {
                SetConsoleMode(m_h, base);
            }
        }
        // **Entrada en BINARIO mientras se edita, y esta es la parte que no es obvia.**
        //
        // En modo texto, la biblioteca de C traduce CRLF a LF, y para saber si un `\r` va
        // seguido de `\n` tiene que MIRAR EL SIGUIENTE CARÁCTER. Al pulsar Intro la consola
        // entrega solo `\r`, así que `fgetc` se quedaba esperando otra tecla: había que
        // pulsar Intro DOS VECES para que se ejecutara una orden.
        //
        // Se cambia solo aquí y se restaura al salir: en binario, `getline` sobre una
        // tubería de Windows dejaría un `\r` al final de cada línea, y una contraseña leída
        // con ese `\r` pegado sería otra contraseña.
        m_modoViejo = _setmode(_fileno(stdin), _O_BINARY);
#else
        m_ok = ::tcgetattr(STDIN_FILENO, &m_viejo) == 0;
        if (m_ok) {
            termios nuevo = m_viejo;
            nuevo.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
            nuevo.c_cc[VMIN] = 1;
            nuevo.c_cc[VTIME] = 0;
            ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &nuevo);
        }
#endif
    }
    ~ModoCrudo() {
        if (!m_ok) {
            return;
        }
#ifdef _WIN32
        SetConsoleMode(m_h, m_viejo);
        if (m_modoViejo != -1) {
            _setmode(_fileno(stdin), m_modoViejo);
        }
#else
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &m_viejo);
#endif
    }
    bool ok() const { return m_ok; }

private:
    bool m_ok{false};
#ifdef _WIN32
    HANDLE m_h{nullptr};
    DWORD m_viejo{0};
    int m_modoViejo{-1};
#else
    termios m_viejo{};
#endif
};

// Cuántas COLUMNAS ocupa: los bytes de continuación de UTF-8 no ocupan ninguna. Sin esto,
// el cursor se descoloca en cuanto se escribe un nombre con tilde.
std::size_t columnas(const std::string& s) {
    std::size_t n = 0;
    for (const char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

// El byte donde empieza el carácter anterior a `pos`. Borrar un byte partiría una letra
// acentuada por la mitad y dejaría basura en la línea.
std::size_t bytesAtras(const std::string& s, std::size_t pos) {
    if (pos == 0) {
        return 0;
    }
    std::size_t i = pos - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
        --i;
    }
    return pos - i;
}

std::size_t bytesAdelante(const std::string& s, std::size_t pos) {
    if (pos >= s.size()) {
        return 0;
    }
    std::size_t i = pos + 1;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
        ++i;
    }
    return i - pos;
}

// El prefijo más largo que comparten todas: es lo que se escribe cuando hay varias
// opciones, para no obligar a teclear lo que ya se sabe.
std::string prefijoComun(const std::vector<std::string>& v) {
    if (v.empty()) {
        return {};
    }
    std::string p = v.front();
    for (const std::string& s : v) {
        std::size_t i = 0;
        while (i < p.size() && i < s.size() && p[i] == s[i]) {
            ++i;
        }
        p.resize(i);
    }
    return p;
}

}  // namespace

void LectorDeLinea::recuerda(const std::string& linea) {
    if (linea.empty() || (!m_historial.empty() && m_historial.back() == linea)) {
        return;
    }
    m_historial.push_back(linea);
    // Un tope: una sesión larga no tiene por qué crecer sin fin.
    if (m_historial.size() > 500) {
        m_historial.erase(m_historial.begin());
    }
}

void LectorDeLinea::repinta(const std::string& indicador, const std::string& linea,
                            std::size_t cursor, std::size_t& anchoPintado) {
    // **Sin una sola secuencia de escape**: solo retorno de carro y espacios.
    //
    // La consola de Windows no interpreta ANSI si no se le activa el modo de terminal
    // virtual, y sin eso `\033[K` se imprime LITERAL: la pantalla se llenaba de «[K». Se
    // podría activar ese modo, pero repintar con `\r` funciona en cualquier terminal, en
    // cualquier versión, sin preguntar nada — y una cosa que funciona siempre vale más que
    // dos caminos según la plataforma.
    //
    // Se reescribe la línea ENTERA en vez de llevar la cuenta de lo que cambió, que es
    // donde se cuelan los desajustes; y el cursor se coloca reescribiendo hasta él, no
    // moviéndolo.
    const std::string completa = indicador + linea;
    const std::size_t ancho = columnas(completa);
    std::string salida = "\r" + completa;
    if (anchoPintado > ancho) {
        salida += std::string(anchoPintado - ancho, ' ');
    }
    salida += "\r" + indicador + linea.substr(0, cursor);
    std::fputs(salida.c_str(), stderr);
    std::fflush(stderr);
    anchoPintado = ancho;
}

bool LectorDeLinea::completa(std::string& linea, std::size_t& cursor) {
    if (!m_completa) {
        return false;
    }
    std::size_t desde = cursor;
    bool puedeSeguir = false;
    const std::vector<std::string> opciones = m_completa(linea, cursor, desde, puedeSeguir);
    if (opciones.empty() || desde > cursor) {
        return false;
    }
    const std::string parcial = linea.substr(desde, cursor - desde);
    const std::string comun = prefijoComun(opciones);
    if (comun.size() > parcial.size()) {
        linea = linea.substr(0, desde) + comun + linea.substr(cursor);
        cursor = desde + comun.size();
        // Una sola opción: se cierra con un espacio, que es lo que uno iba a teclear. Salvo
        // que acabe en `=`, donde lo siguiente es el VALOR y va pegado: `set atime= on` son
        // dos componentes y no una asignación, así que el espacio rompería la orden.
        //
        // Y salvo que a lo completado se le pueda seguir pegando texto, que lo dice quien
        // completa: un directorio acabado en `/`, un dataset al que sigue «/hijo», una
        // sección a la que sigue «/ruta». Cerrar ahí con un espacio obliga a borrarlo para
        // seguir bajando, que es justo lo contrario de lo que hace el tabulador.
        if (opciones.size() == 1 && comun.back() != '=' && !puedeSeguir) {
            linea.insert(cursor, " ");
            ++cursor;
        }
        return true;
    }
    if (opciones.size() > 1) {
        // Ya no se puede alargar: se enseñan, que es la otra cosa que puede querer quien
        // pulsa el tabulador dos veces.
        std::fprintf(stderr, "\n");
        for (const std::string& o : opciones) {
            std::fprintf(stderr, "  %s", o.c_str());
        }
        std::fprintf(stderr, "\n");
    }
    return true;
}

bool LectorDeLinea::lee(const std::string& indicador, std::string& out) {
    out.clear();
    if (!hayTerminal()) {
        // Sin terminal no hay nada que editar: se lee la línea y ya.
        return static_cast<bool>(std::getline(std::cin, out));
    }
    ModoCrudo crudo;
    if (!crudo.ok()) {
        return static_cast<bool>(std::getline(std::cin, out));
    }

    std::string linea;
    std::size_t cursor = 0;
    std::size_t enHistorial = m_historial.size();  // uno más allá = la línea que se escribe
    std::string enCurso;
    std::size_t anchoPintado = 0;
    repinta(indicador, linea, cursor, anchoPintado);

    while (true) {
        const int c = std::fgetc(stdin);
        if (c == EOF) {
            std::fprintf(stderr, "\n");
            return false;
        }
        if (c == '\n' || c == '\r') {
            std::fprintf(stderr, "\n");
            out = linea;
            return true;
        }
        if (c == 0) {
            continue;  // relleno de algunas consolas
        }
        if (c == 4) {  // Ctrl-D
            if (linea.empty()) {
                std::fprintf(stderr, "\n");
                return false;
            }
            continue;
        }
        if (c == 3) {  // Ctrl-C: se abandona la línea, no la sesión
            std::fprintf(stderr, "^C\n");
            out.clear();
            return true;
        }
        if (c == 1) {  // Ctrl-A
            cursor = 0;
            repinta(indicador, linea, cursor, anchoPintado);
            continue;
        }
        if (c == 5) {  // Ctrl-E
            cursor = linea.size();
            repinta(indicador, linea, cursor, anchoPintado);
            continue;
        }
        if (c == 21) {  // Ctrl-U: borra hasta el principio
            linea.erase(0, cursor);
            cursor = 0;
            repinta(indicador, linea, cursor, anchoPintado);
            continue;
        }
        if (c == 127 || c == 8) {  // Retroceso
            const std::size_t n = bytesAtras(linea, cursor);
            if (n > 0) {
                linea.erase(cursor - n, n);
                cursor -= n;
            }
            repinta(indicador, linea, cursor, anchoPintado);
            continue;
        }
        if (c == '\t') {
            if (completa(linea, cursor)) {
                // Si se enseñó la lista de opciones, hubo un salto de línea: lo pintado
                // antes ya no está donde estaba y no hay cola que borrar.
                anchoPintado = 0;
                repinta(indicador, linea, cursor, anchoPintado);
            }
            continue;
        }
        if (c == 27) {  // secuencias de escape: flechas, Inicio, Fin, Suprimir
            const int a = std::fgetc(stdin);
            if (a != '[' && a != 'O') {
                continue;
            }
            const int b = std::fgetc(stdin);
            if (b == 'C') {  // derecha
                cursor += bytesAdelante(linea, cursor);
            } else if (b == 'D') {  // izquierda
                cursor -= bytesAtras(linea, cursor);
            } else if (b == 'A' || b == 'B') {  // arriba y abajo: historial
                if (enHistorial == m_historial.size()) {
                    enCurso = linea;
                }
                if (b == 'A' && enHistorial > 0) {
                    --enHistorial;
                } else if (b == 'B' && enHistorial < m_historial.size()) {
                    ++enHistorial;
                }
                linea = enHistorial < m_historial.size() ? m_historial[enHistorial] : enCurso;
                cursor = linea.size();
            } else if (b == 'H') {
                cursor = 0;
            } else if (b == 'F') {
                cursor = linea.size();
            } else if (b >= '0' && b <= '9') {
                // Inicio/Fin/Suprimir largos: «[1~», «[4~», «[3~».
                const int t = std::fgetc(stdin);
                (void)t;
                if (b == '1') {
                    cursor = 0;
                } else if (b == '4') {
                    cursor = linea.size();
                } else if (b == '3') {
                    const std::size_t n = bytesAdelante(linea, cursor);
                    linea.erase(cursor, n);
                }
            }
            repinta(indicador, linea, cursor, anchoPintado);
            continue;
        }
        if (c < 32) {
            continue;  // el resto de los caracteres de control no significan nada aquí
        }
        linea.insert(cursor, 1, static_cast<char>(c));
        ++cursor;
        repinta(indicador, linea, cursor, anchoPintado);
    }
}

}  // namespace zfsmgr::cli
