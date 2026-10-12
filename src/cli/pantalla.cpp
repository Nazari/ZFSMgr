#include "pantalla.h"

#include "secretinput.h"

#include "json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace zfsmgr::cli::pantalla {
namespace {

// Cuántas líneas se guardan para repintar. Una sesión larga no tiene por qué crecer sin fin,
// y repintar al cambiar el tamaño tiene que ser instantáneo.
constexpr std::size_t kMaxHistorial = 5000;

std::mutex g_m;   // TODO lo que escribe en el terminal o toca el estado pasa por aquí
std::atomic<bool> g_activa{false};
std::atomic<bool> g_parar{false};
std::atomic<bool> g_redimensionado{false};

// El terminal de verdad, guardado antes de desviar nada.
int g_fdTerm = -1;
int g_fdOutOriginal = -1;
int g_fdErrOriginal = -1;
int g_tuboOut[2] = {-1, -1};
int g_tuboErr[2] = {-1, -1};
#ifdef _WIN32
HANDLE g_consola = INVALID_HANDLE_VALUE;
#endif
std::thread g_lectorOut;
std::thread g_lectorErr;
std::thread g_reloj;

// Una tabla guardada como DATOS, para componerla otra vez al ancho de cada momento.
struct TablaGuardada {
    std::vector<std::string> cabeceras;
    std::vector<std::vector<std::string>> filas;
    std::vector<bool> derecha;
};

// El historial: líneas LÓGICAS, tal cual llegaron —al repintar las vuelve a partir el propio
// terminal al ancho que tenga— y tablas, que se recomponen.
struct Bloque {
    std::string texto;
    std::shared_ptr<TablaGuardada> tabla;
};
std::deque<Bloque> g_historial;
std::size_t g_lineasHistorial = 0;

// La marca de una tabla mientras se lee: puede llegar partida en varios trozos.
bool g_enMarca = false;
bool g_escVisto = false;
std::string g_marca;
// Lo que llegó de cada salida sin su salto de línea todavía: una pregunta, un avance con «\r».
std::string g_pendiente[2];
bool g_retornoPendiente[2] = {false, false};

bool g_hayEdicion = false;
std::string g_indicador;
std::string g_linea;
std::size_t g_cursor = 0;
std::vector<std::string> g_estado;
bool g_pausado = false;
std::string g_ultimoError;
std::string g_pregunta;

// Lo que ocupa ahora la zona pintada, y en cuál de sus filas está el cursor.
int g_filasZona = 0;
int g_filaCursor = 0;

// ── Medir y trocear texto ───────────────────────────────────────────────────────────────

// Columnas que ocupa: sin los bytes de continuación de UTF-8 ni las secuencias de escape
// (los colores no ocupan sitio).
std::size_t columnas(const std::string& s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x1b && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) {
                ++i;
            }
            continue;
        }
        if ((c & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

// Parte `s` en trozos de `ancho` columnas como mucho, sin cortar un carácter por la mitad.
std::vector<std::string> trocea(const std::string& s, std::size_t ancho) {
    std::vector<std::string> filas;
    std::string fila;
    std::size_t col = 0;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t largo = 1;
        if (c >= 0xF0) largo = 4;
        else if (c >= 0xE0) largo = 3;
        else if (c >= 0xC0) largo = 2;
        if (col == ancho) {
            filas.push_back(fila);
            fila.clear();
            col = 0;
        }
        fila.append(s, i, largo);
        ++col;
        i += largo;
    }
    filas.push_back(fila);
    return filas;
}

std::size_t anchoSinBloqueo() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    HANDLE h = g_consola != INVALID_HANDLE_VALUE ? g_consola : GetStdHandle(STD_ERROR_HANDLE);
    if (GetConsoleScreenBufferInfo(h, &info)) {
        const int w = info.srWindow.Right - info.srWindow.Left + 1;
        if (w > 10) return static_cast<std::size_t>(w);
    }
#else
    winsize w{};
    const int fd = g_fdTerm >= 0 ? g_fdTerm : STDERR_FILENO;
    if (ioctl(fd, TIOCGWINSZ, &w) == 0 && w.ws_col > 10) {
        return w.ws_col;
    }
#endif
    return 80;
}

std::size_t altoSinBloqueo() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    HANDLE h = g_consola != INVALID_HANDLE_VALUE ? g_consola : GetStdHandle(STD_ERROR_HANDLE);
    if (GetConsoleScreenBufferInfo(h, &info)) {
        const int a = info.srWindow.Bottom - info.srWindow.Top + 1;
        if (a > 3) return static_cast<std::size_t>(a);
    }
#else
    winsize w{};
    const int fd = g_fdTerm >= 0 ? g_fdTerm : STDERR_FILENO;
    if (ioctl(fd, TIOCGWINSZ, &w) == 0 && w.ws_row > 3) {
        return w.ws_row;
    }
#endif
    return 24;
}

// ── Escribir en el terminal ─────────────────────────────────────────────────────────────

void escribe(const std::string& s) {
    std::size_t hecho = 0;
    while (hecho < s.size()) {
#ifdef _WIN32
        const int n = _write(g_fdTerm, s.data() + hecho, static_cast<unsigned>(s.size() - hecho));
#else
        const ssize_t n = ::write(g_fdTerm, s.data() + hecho, s.size() - hecho);
#endif
        if (n <= 0) {
#ifndef _WIN32
            if (n < 0 && errno == EINTR) continue;
#endif
            return;
        }
        hecho += static_cast<std::size_t>(n);
    }
}

std::size_t lineasDe(const Bloque& b) { return b.tabla ? b.tabla->filas.size() + 4 : 1; }

void guardaBloque(Bloque b) {
    g_lineasHistorial += lineasDe(b);
    g_historial.push_back(std::move(b));
    while (g_lineasHistorial > kMaxHistorial && g_historial.size() > 1) {
        g_lineasHistorial -= lineasDe(g_historial.front());
        g_historial.pop_front();
    }
}

void guarda(const std::string& linea) { guardaBloque(Bloque{linea, nullptr}); }

bool usaUnicode() {
#ifdef _WIN32
    return GetConsoleOutputCP() == CP_UTF8;
#else
    for (const char* v : {"LC_ALL", "LC_CTYPE", "LANG"}) {
        const char* x = std::getenv(v);
        if (x && *x) {
            std::string s(x);
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s.find("utf-8") != std::string::npos || s.find("utf8") != std::string::npos;
        }
    }
    return false;
#endif
}

void pintaTablaEn(std::string& s, const TablaGuardada& t) {
    for (const std::string& l :
         componTabla(t.cabeceras, t.filas, t.derecha, anchoSinBloqueo(), usaUnicode())) {
        s += l;
        s += "\r\n";
    }
}

// La marca completa: los datos de la tabla, a guardar y pintar.
void tablaRecibida(std::string& s) {
    zfsmgr::base::json::Value v;
    std::string err;
    if (!zfsmgr::base::json::parse(g_marca, v, &err)) {
        return;
    }
    auto t = std::make_shared<TablaGuardada>();
    for (const auto& c : v["h"].toArray()) t->cabeceras.push_back(c.toString());
    for (const auto& d : v["r"].toArray()) t->derecha.push_back(d.toBool(false));
    for (const auto& f : v["f"].toArray()) {
        std::vector<std::string> fila;
        for (const auto& c : f.toArray()) fila.push_back(c.toString());
        t->filas.push_back(std::move(fila));
    }
    pintaTablaEn(s, *t);
    guardaBloque(Bloque{std::string(), t});
}

// Borrar la zona: subir hasta su primera fila y limpiar hasta el final de la pantalla.
void borraZona(std::string& s) {
    if (g_filasZona == 0) {
        return;
    }
    s += "\r";
    if (g_filaCursor > 0) {
        s += "\x1b[" + std::to_string(g_filaCursor) + "A";
    }
    s += "\x1b[J";
    g_filasZona = 0;
    g_filaCursor = 0;
}

// Pintar la zona desde donde está el cursor (principio de una fila): lo pendiente de cada
// salida, la línea que se escribe partida en filas, y el estado. Deja el cursor donde va el
// de la edición.
void pintaZona(std::string& s) {
    if (g_pausado) {
        return;
    }
    const std::size_t ancho = anchoSinBloqueo();
    const std::size_t alto = altoSinBloqueo();
    std::vector<std::string> filas;
    for (const std::string& p : g_pendiente) {
        if (!p.empty()) {
            for (const std::string& f : trocea(p, ancho)) filas.push_back(f);
        }
    }
    int filaEdicion = -1;
    std::size_t colEdicion = 0;
    if (g_hayEdicion) {
        const std::string todo = g_indicador + g_linea;
        const std::size_t hastaCursor = columnas(g_indicador + g_linea.substr(0, g_cursor));
        std::vector<std::string> trozos = trocea(todo, ancho);
        std::size_t filaCursor = hastaCursor / ancho;
        colEdicion = hastaCursor % ancho;
        // El cursor justo al final de una fila llena pasa a la siguiente, que hay que pintar.
        while (trozos.size() <= filaCursor) trozos.push_back(std::string());
        // Si no cabe entero, se enseña la parte del cursor: el estado y una fila de aire.
        const std::size_t maxFilas = alto > g_estado.size() + 2 ? alto - g_estado.size() - 2 : 1;
        std::size_t desde = 0;
        if (trozos.size() > maxFilas) {
            desde = filaCursor + 1 > maxFilas ? filaCursor + 1 - maxFilas : 0;
            trozos = std::vector<std::string>(trozos.begin() + static_cast<long>(desde),
                                              trozos.begin() + static_cast<long>(std::min(trozos.size(), desde + maxFilas)));
        }
        filaEdicion = static_cast<int>(filas.size() + filaCursor - desde);
        for (const std::string& t : trozos) filas.push_back(t);
    }
    for (const std::string& e : g_estado) {
        // El estado no se parte: una fila cada línea, recortada al ancho, y atenuada para que
        // no se confunda con la salida de las órdenes.
        std::vector<std::string> t = trocea(e, ancho > 1 ? ancho - 1 : 1);
        filas.push_back("\x1b[2m" + t.front() + "\x1b[0m");
    }
    if (filas.empty()) {
        return;
    }
    s += "\x1b[?25l";
    for (std::size_t i = 0; i < filas.size(); ++i) {
        if (i) s += "\r\n";
        s += filas[i];
    }
    const int ultima = static_cast<int>(filas.size()) - 1;
    g_filasZona = static_cast<int>(filas.size());
    if (filaEdicion >= 0) {
        if (ultima > filaEdicion) {
            s += "\x1b[" + std::to_string(ultima - filaEdicion) + "A";
        }
        s += "\r";
        if (colEdicion > 0) {
            s += "\x1b[" + std::to_string(colEdicion) + "C";
        }
        g_filaCursor = filaEdicion;
    } else {
        g_filaCursor = ultima;
    }
    s += "\x1b[?25h";
}

void repintaZona() {
    std::string s;
    borraZona(s);
    pintaZona(s);
    escribe(s);
}

// Todo otra vez al ancho de ahora: pantalla y su historial borrados, el historial guardado
// y la zona.
void repintaTodo() {
    std::string s = "\x1b[?25l\x1b[H\x1b[2J\x1b[3J";
    for (const Bloque& b : g_historial) {
        if (b.tabla) {
            pintaTablaEn(s, *b.tabla);
        } else {
            s += b.texto;
            s += "\r\n";
        }
    }
    g_filasZona = 0;
    g_filaCursor = 0;
    pintaZona(s);
    s += "\x1b[?25h";
    escribe(s);
}

// Lo que llega por una de las dos salidas. `cual`: 0 la estándar, 1 la de error.
void llega(int cual, const char* datos, std::size_t n) {
    std::lock_guard<std::mutex> lock(g_m);
    std::string s;
    if (g_pausado) {
        // Una pregunta en curso: lo que llega se escribe tal cual, sin zona, para que la
        // pregunta quede justo donde el usuario va a teclear.
        s.assign(datos, n);
    } else {
        borraZona(s);
    }
    std::string& p = g_pendiente[cual];
    for (std::size_t i = 0; i < n; ++i) {
        const char c = datos[i];
        // La marca de una tabla: «ESC _ ZT <json> ESC \\», solo por la salida estándar.
        if (cual == 0) {
            if (g_escVisto) {
                g_escVisto = false;
                if (!g_enMarca && c == '_') {
                    g_enMarca = true;
                    g_marca.clear();
                    continue;
                }
                if (g_enMarca && c == '\\') {
                    g_enMarca = false;
                    if (g_marca.rfind("ZT", 0) == 0) {
                        g_marca.erase(0, 2);
                        if (!g_pausado) {
                            tablaRecibida(s);
                        }
                    }
                    g_marca.clear();
                    continue;
                }
                if (g_enMarca) {
                    g_marca.push_back('\x1b');
                } else {
                    p.push_back('\x1b');
                }
            }
            if (c == '\x1b') {
                g_escVisto = true;
                continue;
            }
            if (g_enMarca) {
                g_marca.push_back(c);
                continue;
            }
        }
        if (c == '\n') {
            g_retornoPendiente[cual] = false;
            if (!g_pausado) {
                s += p;
                s += "\r\n";
            }
            if (cual == 1 && !p.empty()) {
                g_ultimoError = p;
            }
            guarda(p);
            p.clear();
            continue;
        }
        if (g_retornoPendiente[cual]) {
            // Un «\r» que no venía de un «\r\n»: la línea se reescribe desde el principio,
            // que es como se pinta un avance.
            p.clear();
            g_retornoPendiente[cual] = false;
        }
        if (c == '\r') {
            g_retornoPendiente[cual] = true;
            continue;
        }
        p.push_back(c);
    }
    pintaZona(s);
    escribe(s);
}

// ── Leer las tuberías ───────────────────────────────────────────────────────────────────

void lector(int cual) {
    const int fd = cual == 0 ? g_tuboOut[0] : g_tuboErr[0];
    char buf[16384];
    while (true) {
#ifdef _WIN32
        HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        DWORD hay = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &hay, nullptr)) {
            return;   // cerrada
        }
        if (hay == 0) {
            if (g_parar.load()) return;
            Sleep(15);
            continue;
        }
        const int n = _read(fd, buf, static_cast<unsigned>(std::min<DWORD>(hay, sizeof(buf))));
#else
        pollfd p{fd, POLLIN, 0};
        const int r = ::poll(&p, 1, 100);
        if (r == 0) {
            if (g_parar.load()) return;
            continue;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            return;
        }
        const ssize_t n = ::read(fd, buf, sizeof(buf));
#endif
        if (n <= 0) {
            return;
        }
        llega(cual, buf, static_cast<std::size_t>(n));
    }
}

#ifndef _WIN32
void alCambiarTamano(int) { g_redimensionado.store(true); }
#endif

// Cada 100 ms: ¿ha cambiado el tamaño? Se espera a que deje de cambiar —arrastrar la esquina
// manda decenas de avisos— y se repinta todo una vez.
void reloj() {
    std::size_t ancho = anchoSinBloqueo();
    std::size_t alto = altoSinBloqueo();
    auto ultimoCambio = std::chrono::steady_clock::time_point{};
    bool pendiente = false;
    while (!g_parar.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::size_t a = anchoSinBloqueo();
        const std::size_t h = altoSinBloqueo();
        if (g_redimensionado.exchange(false) || a != ancho || h != alto) {
            ancho = a;
            alto = h;
            ultimoCambio = std::chrono::steady_clock::now();
            pendiente = true;
        }
        if (pendiente && std::chrono::steady_clock::now() - ultimoCambio > std::chrono::milliseconds(150)) {
            pendiente = false;
            std::lock_guard<std::mutex> lock(g_m);
            if (!g_pausado) {
                repintaTodo();
            }
        }
    }
}

bool creaTubo(int t[2]) {
#ifdef _WIN32
    return _pipe(t, 65536, _O_BINARY | _O_NOINHERIT) == 0;
#elif defined(__linux__) || defined(__FreeBSD__)
    return pipe2(t, O_CLOEXEC) == 0;
#else
    if (pipe(t) != 0) return false;
    fcntl(t[0], F_SETFD, FD_CLOEXEC);
    fcntl(t[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}

}  // namespace

bool activa() { return g_activa.load(); }

std::size_t anchoTerminal() { return anchoSinBloqueo(); }
std::size_t altoTerminal() { return altoSinBloqueo(); }

bool arranca() {
    if (g_activa.load() || !hayTerminal()) {
        return false;
    }
    const char* term = std::getenv("TERM");
    if (term && std::string(term) == "dumb") {
        return false;
    }
#ifdef _WIN32
    // Sin el modo de terminal virtual, la consola escribe las secuencias LITERALES.
    g_consola = GetStdHandle(STD_ERROR_HANDLE);
    DWORD modo = 0;
    if (!GetConsoleMode(g_consola, &modo)
        || !SetConsoleMode(g_consola, modo | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        g_consola = INVALID_HANDLE_VALUE;
        return false;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    g_fdTerm = _dup(2);
    g_fdOutOriginal = _dup(1);
    g_fdErrOriginal = _dup(2);
    if (g_fdTerm < 0 || !creaTubo(g_tuboOut) || !creaTubo(g_tuboErr)) {
        return false;
    }
    _dup2(g_tuboOut[1], 1);
    _dup2(g_tuboErr[1], 2);
    _close(g_tuboOut[1]);
    _close(g_tuboErr[1]);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#else
    std::fflush(stdout);
    std::fflush(stderr);
    g_fdTerm = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
    g_fdOutOriginal = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    g_fdErrOriginal = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
    if (g_fdTerm < 0 || !creaTubo(g_tuboOut) || !creaTubo(g_tuboErr)) {
        return false;
    }
    // dup2 deja el descriptor nuevo SIN CLOEXEC: los hijos heredan sus 1 y 2 —y su salida
    // pasa también por aquí—; los extremos de lectura no los hereda nadie.
    ::dup2(g_tuboOut[1], STDOUT_FILENO);
    ::dup2(g_tuboErr[1], STDERR_FILENO);
    ::close(g_tuboOut[1]);
    ::close(g_tuboErr[1]);
    // Una tubería hace que stdio guarde la salida hasta llenar el búfer: por líneas.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    struct sigaction sa {};
    sa.sa_handler = alCambiarTamano;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, nullptr);
#endif
    g_activa.store(true);
    g_parar.store(false);
    g_lectorOut = std::thread(lector, 0);
    g_lectorErr = std::thread(lector, 1);
    g_reloj = std::thread(reloj);
    // Las preguntas de secretinput (s/N, contraseñas) pausan la zona mientras esperan.
    ponGanchosDePregunta([](const std::string& p) { pausa(p); },
                         [](const std::string& eco, bool secreto) { reanuda(eco, secreto); });
    std::atexit([] { para(); });
    return true;
}

void para() {
    if (!g_activa.exchange(false)) {
        return;
    }
    ponGanchosDePregunta(nullptr, nullptr);
    std::fflush(stdout);
    std::fflush(stderr);
    // Se devuelven los descriptores: con eso se cierran los extremos de escritura de este
    // proceso y los lectores reciben el fin… salvo que un hijo siga vivo con uno de ellos, y
    // por eso además se les avisa de que paren.
#ifdef _WIN32
    _dup2(g_fdOutOriginal, 1);
    _dup2(g_fdErrOriginal, 2);
#else
    ::dup2(g_fdOutOriginal, STDOUT_FILENO);
    ::dup2(g_fdErrOriginal, STDERR_FILENO);
#endif
    g_parar.store(true);
    if (g_lectorOut.joinable()) g_lectorOut.join();
    if (g_lectorErr.joinable()) g_lectorErr.join();
    if (g_reloj.joinable()) g_reloj.join();
    std::lock_guard<std::mutex> lock(g_m);
    std::string s;
    borraZona(s);
    for (std::string& p : g_pendiente) {
        if (!p.empty()) {
            s += p + "\r\n";
            p.clear();
        }
    }
    escribe(s);
}

void ponEdicion(const std::string& indicador, const std::string& linea, std::size_t cursor) {
    std::lock_guard<std::mutex> lock(g_m);
    g_hayEdicion = true;
    g_indicador = indicador;
    g_linea = linea;
    g_cursor = std::min(cursor, linea.size());
    repintaZona();
}

void cierraEdicion() {
    std::lock_guard<std::mutex> lock(g_m);
    std::string s;
    borraZona(s);
    if (g_hayEdicion) {
        const std::string todo = g_indicador + g_linea;
        s += todo + "\r\n";
        guarda(todo);
    }
    g_hayEdicion = false;
    pintaZona(s);
    escribe(s);
}

void ponEstado(const std::vector<std::string>& lineas) {
    std::lock_guard<std::mutex> lock(g_m);
    if (lineas == g_estado) {
        return;
    }
    g_estado = lineas;
    repintaZona();
}

// Esperar a que el lector haya pintado lo que quedaba en las tuberías: lo escrito justo antes
// de la pregunta tiene que salir antes que ella.
void vaciaTuberias() {
    std::fflush(stdout);
    std::fflush(stderr);
    for (int i = 0; i < 20; ++i) {
        bool vacias = true;
        for (const int fd : {g_tuboOut[0], g_tuboErr[0]}) {
#ifdef _WIN32
            DWORD hay = 0;
            if (PeekNamedPipe(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), nullptr, 0, nullptr,
                              &hay, nullptr) && hay > 0) {
                vacias = false;
            }
#else
            int hay = 0;
            if (ioctl(fd, FIONREAD, &hay) == 0 && hay > 0) {
                vacias = false;
            }
#endif
        }
        if (vacias) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Lo leído y aún no pintado lo pinta el lector con el cerrojo: tomarlo aquí espera a que
    // acabe.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

void pausa(const std::string& pregunta) {
    vaciaTuberias();
    std::lock_guard<std::mutex> lock(g_m);
    std::string s;
    borraZona(s);
    // Lo pendiente de las salidas se cierra como línea: la pregunta va en la suya.
    for (std::string& p : g_pendiente) {
        if (!p.empty()) {
            s += p + "\r\n";
            guarda(p);
            p.clear();
        }
    }
    s += pregunta;
    g_pregunta = pregunta;
    g_pausado = true;
    escribe(s);
}

void reanuda(const std::string& eco, bool secreto) {
    std::lock_guard<std::mutex> lock(g_m);
    std::string s;
    if (secreto) {
        s += "\r\n";   // sin eco, el Intro no movió el cursor
    }
    guarda(g_pregunta + eco);
    g_pregunta.clear();
    g_pausado = false;
    pintaZona(s);
    escribe(s);
}

void tabla(const std::vector<std::string>& cabeceras,
           const std::vector<std::vector<std::string>>& filas, const std::vector<bool>& derecha) {
    namespace J = zfsmgr::base::json;
    J::Array h;
    for (const auto& c : cabeceras) h.push_back(J::Value(c));
    J::Array r;
    for (const bool d : derecha) r.push_back(J::Value(d));
    J::Array f;
    for (const auto& fila : filas) {
        J::Array a;
        for (const auto& c : fila) a.push_back(J::Value(c));
        f.push_back(J::Value(std::move(a)));
    }
    J::Value v;
    v.set("h", J::Value(std::move(h)));
    v.set("r", J::Value(std::move(r)));
    v.set("f", J::Value(std::move(f)));
    const std::string marca = "\x1b_ZT" + J::toCompact(v) + "\x1b\\";
    std::fwrite(marca.data(), 1, marca.size(), stdout);
    std::fflush(stdout);
}

namespace {

// Parte una celda en líneas de `ancho` columnas: por espacios cuando se puede, y una palabra
// más larga que la columna, por donde caiga.
std::vector<std::string> parteCelda(const std::string& texto, std::size_t ancho) {
    std::vector<std::string> out;
    if (ancho == 0) {
        out.push_back(texto);
        return out;
    }
    std::string linea;
    std::size_t col = 0;
    std::size_t i = 0;
    while (i < texto.size()) {
        // La palabra siguiente, con los espacios que la preceden.
        std::size_t j = i;
        while (j < texto.size() && texto[j] == ' ') ++j;
        std::size_t k = j;
        while (k < texto.size() && texto[k] != ' ') ++k;
        const std::string espacios = texto.substr(i, j - i);
        const std::string palabra = texto.substr(j, k - j);
        const std::size_t cp = columnas(palabra);
        if (col > 0 && col + columnas(espacios) + cp <= ancho) {
            linea += espacios + palabra;
            col += columnas(espacios) + cp;
        } else if (cp <= ancho) {
            if (col > 0) out.push_back(linea);
            linea = palabra;
            col = cp;
        } else {
            // Más larga que la columna: a trozos.
            if (col > 0) {
                out.push_back(linea);
                linea.clear();
                col = 0;
            }
            std::vector<std::string> trozos = trocea(palabra, ancho);
            for (std::size_t t = 0; t + 1 < trozos.size(); ++t) out.push_back(trozos[t]);
            linea = trozos.back();
            col = columnas(linea);
        }
        i = k;
    }
    out.push_back(linea);
    return out;
}

std::string rellena(const std::string& s, std::size_t ancho, bool derecha) {
    const std::size_t c = columnas(s);
    if (c >= ancho) return s;
    const std::string hueco(ancho - c, ' ');
    return derecha ? hueco + s : s + hueco;
}

std::string repite(const std::string& s, std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i < n; ++i) out += s;
    return out;
}

}  // namespace

std::vector<std::string> componTabla(const std::vector<std::string>& cabeceras,
                                     const std::vector<std::vector<std::string>>& filas,
                                     const std::vector<bool>& derecha, std::size_t ancho,
                                     bool unicode) {
    const std::size_t n = cabeceras.size();
    std::vector<std::string> out;
    if (n == 0) {
        return out;
    }
    // Lo natural de cada columna: lo más ancho de su cabecera y sus celdas. Una celda que
    // se extiende hasta el final no cuenta: no es de una columna sola.
    std::vector<std::size_t> w(n, 0);
    for (std::size_t i = 0; i < n; ++i) w[i] = std::max<std::size_t>(1, columnas(cabeceras[i]));
    for (const auto& f : filas) {
        for (std::size_t i = 0; i < f.size() && i < n; ++i) {
            if (f.size() < n && i + 1 == f.size()) continue;
            w[i] = std::max(w[i], columnas(f[i]));
        }
    }
    // Lo que cabe: «│ a │ b │» son 3 por columna más 1.
    const std::size_t marco = 3 * n + 1;
    const std::size_t hueco = ancho > marco ? ancho - marco : n;
    std::size_t total = 0;
    for (const std::size_t x : w) total += x;
    // Se estrecha SIEMPRE la más ancha, una columna cada vez: así se reparte el recorte entre
    // las que pueden permitírselo y las estrechas —IDs, sí/no— se quedan enteras.
    const std::size_t minimo = 4;
    while (total > hueco) {
        std::size_t mayor = 0;
        for (std::size_t i = 1; i < n; ++i) {
            if (w[i] > w[mayor]) mayor = i;
        }
        if (w[mayor] <= minimo) break;
        --w[mayor];
        --total;
    }
    const std::string H = unicode ? "─" : "-";
    // No cabe ni con todas al mínimo: una FICHA por fila, «CABECERA valor», como `\\x` en
    // psql. Partir una tabla de doce columnas en tiras de cuatro letras no se lee.
    if (total > hueco) {
        std::size_t anchoCab = 0;
        for (const auto& c : cabeceras) anchoCab = std::max(anchoCab, columnas(c));
        const std::size_t sangria = 2 + anchoCab + 2;
        const std::size_t paraValor = ancho > sangria + 4 ? ancho - sangria - 1 : 20;
        for (const auto& f : filas) {
            const std::string titulo = f.empty() ? std::string() : f[0];
            std::string cab = (unicode ? "── " : "-- ") + titulo + " ";
            const std::size_t usado = columnas(cab);
            if (usado + 1 < ancho) cab += repite(H, ancho - usado - 1);
            out.push_back(cab);
            for (std::size_t i = 1; i < f.size() && i < n; ++i) {
                const bool extiende = f.size() < n && i + 1 == f.size();
                const std::string nombre = extiende ? std::string() : cabeceras[i];
                const std::vector<std::string> partes = parteCelda(f[i], paraValor);
                for (std::size_t l = 0; l < partes.size(); ++l) {
                    out.push_back("  " + rellena(l == 0 ? nombre : std::string(), anchoCab, false)
                                  + "  " + partes[l]);
                }
            }
        }
        return out;
    }
    const std::string V = unicode ? "│" : "|";
    const auto borde = [&](const char* izq, const char* cruce, const char* der, const char* ai,
                           const char* ac, const char* ad) {
        std::string l = unicode ? izq : ai;
        for (std::size_t i = 0; i < n; ++i) {
            l += repite(H, w[i] + 2);
            l += i + 1 < n ? (unicode ? cruce : ac) : (unicode ? der : ad);
        }
        return l;
    };
    const auto fila = [&](const std::vector<std::string>& celdas, bool cabecera) {
        // Las celdas que pinta esta fila: si trae menos que columnas, la última se extiende.
        std::vector<std::string> c = celdas;
        std::vector<std::size_t> anchos;
        std::vector<bool> der;
        if (c.size() < n && !c.empty()) {
            for (std::size_t i = 0; i + 1 < c.size(); ++i) {
                anchos.push_back(w[i]);
                der.push_back(!cabecera && i < derecha.size() && derecha[i]);
            }
            std::size_t resto = 0;
            for (std::size_t i = c.size() - 1; i < n; ++i) resto += w[i];
            resto += 3 * (n - c.size());
            anchos.push_back(resto);
            der.push_back(false);
        } else {
            c.resize(n);
            for (std::size_t i = 0; i < n; ++i) {
                anchos.push_back(w[i]);
                der.push_back(!cabecera && i < derecha.size() && derecha[i]);
            }
        }
        std::vector<std::vector<std::string>> partes;
        std::size_t alto = 1;
        for (std::size_t i = 0; i < c.size(); ++i) {
            partes.push_back(parteCelda(c[i], anchos[i]));
            alto = std::max(alto, partes.back().size());
        }
        for (std::size_t l = 0; l < alto; ++l) {
            std::string linea = V;
            for (std::size_t i = 0; i < c.size(); ++i) {
                const std::string t = l < partes[i].size() ? partes[i][l] : std::string();
                linea += " " + rellena(t, anchos[i], der[i]) + " " + V;
            }
            out.push_back(linea);
        }
    };
    out.push_back(borde("┌", "┬", "┐", "+", "+", "+"));
    fila(cabeceras, true);
    out.push_back(borde("├", "┼", "┤", "+", "+", "+"));
    for (const auto& f : filas) fila(f, false);
    out.push_back(borde("└", "┴", "┘", "+", "+", "+"));
    return out;
}

void marcaOrden() {
    std::lock_guard<std::mutex> lock(g_m);
    g_ultimoError.clear();
}

void sincroniza() {
    if (activa()) {
        vaciaTuberias();
    }
}

std::string ultimaLineaDeError() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_ultimoError;
}

void limpia() {
    std::lock_guard<std::mutex> lock(g_m);
    g_historial.clear();
    g_lineasHistorial = 0;
    repintaTodo();
}

}  // namespace zfsmgr::cli::pantalla
