#include "linea.h"

#include "pantalla.h"
#include "secretinput.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace zfsmgr::cli {
namespace {

// Los avisos que dejan otros hilos para el terminal (el estado de las programaciones al
// arrancar). Se pintan desde AQUÍ, el único que sabe qué hay escrito en la línea, para no
// partir por la mitad lo que el usuario está tecleando.
std::mutex g_avisosMutex;
std::vector<std::string> g_avisos;

std::vector<std::string> sacaAvisos() {
    std::lock_guard<std::mutex> lock(g_avisosMutex);
    std::vector<std::string> v;
    v.swap(g_avisos);
    return v;
}

// Un byte del terminal en modo crudo.
//
// En POSIX con `read` y no con `fgetc`: la espera de abajo pregunta con `poll` al
// DESCRIPTOR si hay algo, y lo que `stdio` tuviera ya leído en su búfer no lo vería —un texto
// pegado se quedaría a medias hasta la siguiente tecla—. Sin búfer de por medio, lo que dice
// `poll` es la verdad.
int leeByte() {
#ifdef _WIN32
    return std::fgetc(stdin);
#else
    unsigned char c = 0;
    ssize_t n = 0;
    do {
        n = ::read(STDIN_FILENO, &c, 1);
    } while (n < 0 && errno == EINTR);
    return n == 1 ? c : EOF;
#endif
}

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
            // Sin ISIG: Ctrl-C tiene que llegar como el carácter 3 —abandonar la línea, como
            // en bash— y no como una SIGINT que mataba el intérprete entero mientras se
            // escribía. Fuera del editor el terminal vuelve a su modo y Ctrl-C es lo de siempre.
            nuevo.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG);
            nuevo.c_cc[VMIN] = 1;
            nuevo.c_cc[VTIME] = 0;
            // TCSADRAIN y no TCSAFLUSH: este cambia el modo TIRANDO lo tecleado y aún no
            // leído, y lo que se escribía mientras acababa la orden anterior desaparecía.
            ::tcsetattr(STDIN_FILENO, TCSADRAIN, &nuevo);
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
        ::tcsetattr(STDIN_FILENO, TCSADRAIN, &m_viejo);
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

// El ancho del terminal en columnas, o 80 si no se puede saber.
std::size_t anchoDelTerminal() {
    if (pantalla::activa()) {
        return pantalla::anchoTerminal();   // stderr es ahora una tubería
    }
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &info)) {
        const int w = info.srWindow.Right - info.srWindow.Left + 1;
        if (w > 10) {
            return static_cast<std::size_t>(w);
        }
    }
#else
    winsize w{};
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 10) {
        return w.ws_col;
    }
#endif
    return 80;
}

// Los bytes donde empieza cada carácter de `s`, y uno más con el final.
std::vector<std::size_t> inicios(const std::string& s) {
    std::vector<std::size_t> v;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            v.push_back(i);
        }
    }
    v.push_back(s.size());
    return v;
}

// El byte donde empieza el carácter anterior a `pos`. Delete un byte partiría una letra
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

// Lo que se ENSEÑA de cada opción cuando hay varias: solo lo que la distingue, desde el
// último separador que comparten todas. Como bash con los directorios: tras
// `cd local/back/<tab>` se ve «libvirt-images  tooling  user», no tres veces
// «local/back/» delante. Tras una «@» se deja la arroba, para que se lea como instantánea;
// tras un «=» quedan los valores de la propiedad.
std::vector<std::string> loQueSeEnsena(const std::vector<std::string>& opciones) {
    const std::string comun = prefijoComun(opciones);
    const std::size_t sep = comun.find_last_of("/=@");
    if (sep == std::string::npos) {
        return opciones;
    }
    const std::size_t corte = comun[sep] == '@' ? sep : sep + 1;
    std::vector<std::string> out;
    for (const std::string& o : opciones) {
        // Si una opción es justo el prefijo, cortada no quedaría nada: se enseña entera.
        out.push_back(o.size() > corte ? o.substr(corte) : o);
    }
    return out;
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
    // Con la pantalla activa la línea la pinta ella, partida en filas como en bash; lo de
    // abajo es el camino sin ella (un terminal sin secuencias de escape, `--no-tui`).
    if (pantalla::activa()) {
        pantalla::ponEdicion(indicador, linea, cursor);
        return;
    }
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
    //
    // **Y nunca más ancho que el terminal.** Con `\r` solo se vuelve al principio de la fila
    // FÍSICA: si la línea se partía en dos, cada repintado empezaba en la segunda y dejaba la
    // primera atrás, así que al pegar una orden larga se imprimía una copia entera por cada
    // carácter. Se pinta solo el trozo que cabe, siguiendo al cursor —como `readline` en una
    // sola línea—, con «<» cuando hay texto escondido a la izquierda.
    const std::size_t anchoTerminal = anchoDelTerminal();
    const std::size_t anchoIndicador = columnas(indicador);
    // Una columna menos que el terminal: escribir en la última hace saltar de fila a algunos.
    const std::size_t hueco =
        anchoTerminal > anchoIndicador + 11 ? anchoTerminal - anchoIndicador - 1 : 10;
    const std::vector<std::size_t> pos = inicios(linea);
    const std::size_t total = pos.size() - 1;  // caracteres de la línea
    std::size_t enCursor = 0;                 // caracteres antes del cursor
    while (enCursor < total && pos[enCursor] < cursor) {
        ++enCursor;
    }
    std::size_t desde = 0;
    std::string marca;
    if (total >= hueco && enCursor >= hueco) {
        // El cursor al borde derecho, y «<» ocupando la primera columna.
        desde = enCursor - (hueco - 2);
        marca = "<";
    }
    const std::size_t caben = hueco - marca.size();
    const std::size_t hasta = std::min(total, desde + caben);
    const std::string visible = linea.substr(pos[desde], pos[hasta] - pos[desde]);
    const std::string hastaCursor = linea.substr(pos[desde], pos[enCursor] - pos[desde]);

    const std::string completa = indicador + marca + visible;
    const std::size_t ancho = columnas(completa);
    std::string salida = "\r" + completa;
    if (anchoPintado > ancho) {
        salida += std::string(anchoPintado - ancho, ' ');
    }
    salida += "\r" + indicador + marca + hastaCursor;
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
        // Con la pantalla, la lista sale por encima de la zona: no hace falta salir antes de
        // la línea que se está escribiendo.
        if (!pantalla::activa()) {
            std::fprintf(stderr, "\n");
        }
        for (const std::string& o : loQueSeEnsena(opciones)) {
            std::fprintf(stderr, "  %s", o.c_str());
        }
        std::fprintf(stderr, "\n");
    }
    return true;
}

// La línea se da por terminada: con la pantalla pasa a su historial; sin ella, un salto.
void terminaLinea(const char* marca) {
    if (pantalla::activa()) {
        pantalla::cierraEdicion();
        return;
    }
    std::fprintf(stderr, "%s\n", marca);
}

void encolaAviso(const std::string& texto) {
    // Con la pantalla, lo que se escribe desde cualquier hilo ya se pinta por encima de la
    // zona sin partir lo que se teclea —también en Windows—: no hace falta esperar al editor.
    if (pantalla::activa()) {
        std::fprintf(stderr, "%s\n", texto.c_str());
        return;
    }
    std::lock_guard<std::mutex> lock(g_avisosMutex);
    g_avisos.push_back(texto);
}

bool LectorDeLinea::editaEnTerminal() {
    return hayTerminal();
}

bool LectorDeLinea::lee(const std::string& indicador, std::string& out) {
    out.clear();
    if (!hayTerminal()) {
        m_propuesta.clear();
        // Sin terminal no hay nada que editar: se lee la línea y ya.
        //
        // Salvo el BOM de UTF-8 al principio de la entrada. PowerShell lo antepone a lo que
        // pasa por una tubería cuando su codificación de salida es UTF-8 con BOM —una
        // configuración corriente—, y entonces la primera orden llegaba como «\xEF\xBB\xBFls»:
        // «orden desconocida», con un carácter invisible delante. Igual con un guion
        // guardado así por un editor de Windows.
        static bool primera = true;
        const bool leida = static_cast<bool>(std::getline(std::cin, out));
        if (leida && primera && out.rfind("\xEF\xBB\xBF", 0) == 0) {
            out.erase(0, 3);
        }
        primera = false;
        return leida;
    }
    ModoCrudo crudo;
    if (!crudo.ok()) {
        return static_cast<bool>(std::getline(std::cin, out));
    }

    std::string linea = m_propuesta;
    m_propuesta.clear();
    std::size_t cursor = linea.size();
    std::size_t enHistorial = m_historial.size();  // uno más allá = la línea que se escribe
    std::string enCurso;
    std::size_t anchoPintado = 0;
    for (const std::string& a : sacaAvisos()) {
        std::fprintf(stderr, "%s\n", a.c_str());
    }
    repinta(indicador, linea, cursor, anchoPintado);

    while (true) {
#ifndef _WIN32
        // Esperar a la tecla en tramos cortos, y entre tramo y tramo, pintar los avisos que
        // hayan llegado: se borra la línea, se escribe el aviso y se vuelve a pintar el
        // indicador con lo tecleado, donde estaba.
        //
        // En Windows no: la consola entrega por `ReadConsole` y la biblioteca de C guarda en
        // su búfer lo que sobra, así que esperar al manejador no sabría si ya hay teclas
        // leídas. Allí el aviso sale al pintar el siguiente indicador.
        while (true) {
            pollfd pfd{};
            pfd.fd = STDIN_FILENO;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 200) != 0) {
                break;
            }
            const std::vector<std::string> avisos = sacaAvisos();
            if (avisos.empty()) {
                continue;
            }
            std::fputs("\r\x1b[K", stderr);
            for (const std::string& a : avisos) {
                std::fprintf(stderr, "%s\n", a.c_str());
            }
            anchoPintado = 0;
            repinta(indicador, linea, cursor, anchoPintado);
        }
#endif
        const int c = leeByte();
        if (c == EOF) {
            terminaLinea("");
            return false;
        }
        if (c == '\n' || c == '\r') {
            terminaLinea("");
            out = linea;
            return true;
        }
        if (c == 0) {
            continue;  // relleno de algunas consolas
        }
        if (c == 4) {  // Ctrl-D
            if (linea.empty()) {
                terminaLinea("");
                return false;
            }
            continue;
        }
        if (c == 3) {  // Ctrl-C: se abandona la línea, no la sesión
            if (pantalla::activa()) {
                pantalla::ponEdicion(indicador, linea + "^C", linea.size() + 2);
            }
            terminaLinea("^C");
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
            const int a = leeByte();
            if (a != '[' && a != 'O') {
                continue;
            }
            const int b = leeByte();
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
                const int t = leeByte();
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
