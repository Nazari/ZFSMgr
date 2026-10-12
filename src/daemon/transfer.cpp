// Moving data between machines: `zfs send`/`recv` over the daemon-to-daemon socket, stream
// files (with compression) and the file tree transfer (remotetree).

#include "agent_internal.h"

namespace zfsmgr::agent {

// --- El veredicto del receptor.
//
// El trabajo de una transferencia es del EMISOR, y antes acababa en cuanto había mandado el
// último byte: un `zfs recv` que rechazaba el flujo al final, o un fichero de destino que no
// se podía escribir, dejaban el trabajo como «done». Así que el receptor, al terminar,
// devuelve por el mismo socket cómo le fue —«RC=<n>» y, si falló, «ERR=<motivo>»—, y el
// emisor lo espera antes de darse por bueno.
//
// Un receptor que cierra SIN decir cómo le fue es un fallo: o murió, o se cortó la conexión.
void informaAlEmisor(TransferSocket s, int rc, const std::string& motivo) {
    std::string linea = "RC=" + std::to_string(rc) + "\n";
    if (rc != 0) {
        std::string unaLinea = trim(motivo);
        for (char& c : unaLinea) {
            if (c == '\n' || c == '\r') {
                c = ' ';
            }
        }
        if (unaLinea.size() > 900) {
            unaLinea.resize(900);
        }
        if (!unaLinea.empty()) {
            linea += "ERR=" + unaLinea + "\n";
        }
    }
    (void)send(s, linea.c_str(), static_cast<int>(linea.size()), 0);
}

// Cierra el sentido de ida —es el fin de datos del receptor— y espera su veredicto. El plazo
// es largo a propósito: tras el último byte, `zfs recv` todavía tiene que asentar lo
// recibido, y con flujos grandes eso no son segundos. `sigue` se consulta cada segundo para
// atender una cancelación.
VeredictoDelReceptor leeVeredicto(TransferSocket s, const std::function<bool()>& sigue,
                                  std::chrono::milliseconds plazo) {
    VeredictoDelReceptor v;
    std::string recibido;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < plazo) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        struct timeval tv{};
        tv.tv_sec = 1;
        const int sel = select(static_cast<int>(s) + 1, &rfds, nullptr, nullptr, &tv);
        if (sel < 0) {
#ifndef _WIN32
            if (errno == EINTR) {
                continue;
            }
#endif
            break;
        }
        if (sel == 0) {
            if (sigue && !sigue()) {
                break;
            }
            continue;
        }
        char buf[1024];
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        recibido.append(buf, static_cast<std::size_t>(n));
        if (recibido.size() > 4096) {
            break;
        }
    }
    std::istringstream lineas(recibido);
    std::string l;
    while (std::getline(lineas, l)) {
        if (l.rfind("RC=", 0) == 0) {
            try {
                v.rc = std::stoi(l.substr(3));
                v.hayVeredicto = true;
            } catch (...) {
            }
        } else if (l.rfind("ERR=", 0) == 0) {
            v.motivo = l.substr(4);
        }
    }
    return v;
}

VeredictoDelReceptor esperaVeredicto(TransferSocket s, const std::function<bool()>& sigue) {
#ifdef _WIN32
    shutdown(s, SD_SEND);
#else
    shutdown(s, SHUT_WR);
#endif
    return leeVeredicto(s, sigue, std::chrono::minutes(30));
}

// ---------------------------------------------------------------------------
// Descriptores de transfer que NO se heredan.
//
// Esto arregla un cuelgue medido, y la causa no está donde parece. El daemon es
// multihilo: mientras un hilo atiende una recepción, otro puede estar lanzando un
// `zfs send`. Todo descriptor abierto en ese instante lo hereda el hijo. Así que el
// `zfs send` acababa con el socket ACEPTADO POR EL RECEPTOR entre sus descriptores.
//
// Consecuencia: cuando el `zfs recv` moría —por ejemplo, porque la copia estaba
// duplicada— el daemon cerraba su copia del socket y la conexión NO se cerraba, porque
// seguía sujeta por el `zfs send`. El emisor llenaba el búfer del receptor y se quedaba
// dentro de send() para siempre. Sin error, sin fin de fichero, sin nada que mirar.
// Comprobado con ss: «zfs pid=128516 fd=7» sujetando el extremo receptor.
//
// Por eso se marcan AL CREARLOS y no después: entre socket()/accept() y un fcntl
// posterior cabe justo el fork del otro hilo, que es la carrera que se quiere cerrar.
// Donde no hay forma atómica (macOS no tiene accept4) queda el fcntl, que estrecha la
// ventana aunque no la cierre del todo.
// ---------------------------------------------------------------------------
void noHeredarTransferSocket(TransferSocket fd) {
#ifdef _WIN32
    SetHandleInformation(reinterpret_cast<HANDLE>(fd), HANDLE_FLAG_INHERIT, 0);
#else
    const int f = fcntl(fd, F_GETFD, 0);
    if (f >= 0) {
        fcntl(fd, F_SETFD, f | FD_CLOEXEC);
    }
#endif
}

TransferSocket creaTransferSocket(int family, int type, int proto) {
#if defined(__linux__) || defined(__FreeBSD__)
    return socket(family, type | SOCK_CLOEXEC, proto);
#else
    const TransferSocket fd = socket(family, type, proto);
#ifdef _WIN32
    if (fd != INVALID_SOCKET) {
        noHeredarTransferSocket(fd);
    }
#else
    if (fd >= 0) {
        noHeredarTransferSocket(fd);
    }
#endif
    return fd;
#endif
}

TransferSocket aceptaTransferSocket(TransferSocket listenFd) {
#if defined(__linux__) || defined(__FreeBSD__)
    return accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
#else
    const TransferSocket fd = accept(listenFd, nullptr, nullptr);
#ifdef _WIN32
    if (fd != INVALID_SOCKET) {
        noHeredarTransferSocket(fd);
    }
#else
    if (fd >= 0) {
        noHeredarTransferSocket(fd);
    }
#endif
    return fd;
#endif
}

void ponPlazoDeEnvio(TransferSocket fd, int ms) {
#ifdef _WIN32
    DWORD v = static_cast<DWORD>(ms);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&v), sizeof(v));
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// ¿El send() volvió sin escribir porque venció el plazo, o porque la conexión se rompió?
// Distinguirlo es lo que separa «espera y reintenta» de «esto ya no va a ir».
bool envioSoloAgotoElPlazo() {
#ifdef _WIN32
    const int e = WSAGetLastError();
    return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

// Conecta con el otro extremo de una transfer, sea lo que sea lo que vaya a viajar.
//
// Lo usan los DOS emisores —el del flujo de `zfs send` y el del árbol de ficheros— y era la
// misma secuencia escrita dos veces: resolver con las dos familias, crear el socket sin
// herencia, probar cada dirección hasta que una conecte. Con dos copias, un arreglo como el
// de la doble pila IPv6 entra en una y se olvida en la otra.
//
// Devuelve el socket ya con su plazo de envío puesto y SIN herencia, o inválido.
//
// Las dos cosas van aquí y no en quien llama, porque las dos existen por un fallo medido y
// olvidarlas es silencioso:
//
//  - Sin plazo, `send()` se queda dentro del núcleo PARA SIEMPRE en cuanto el receptor deja
//    de leer: ni se detecta el fallo, ni se atiende la cancelación, ni se llega a matar al
//    hijo. Medido con una copia duplicada: «ESTAB Send-Q 2514232» y el emisor parado ahí.
//  - Sin CLOEXEC, el `zfs send` que se lanza después hereda este socket, y entonces cerrarlo
//    aquí no lo cierra de verdad: el receptor nunca ve el fin de los datos. Visto en el
//    mismo caso, con el fd abierto en los dos procesos.
TransferSocket conectaConElPar(const std::string& host, const std::string& puerto) {
#ifdef _WIN32
    ensureWinsock();
#endif
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), puerto.c_str(), &hints, &res) != 0 || res == nullptr) {
        return kSocketInvalido;
    }
    TransferSocket sock = kSocketInvalido;
    for (struct addrinfo* it = res; it; it = it->ai_next) {
        const TransferSocket fd =
            creaTransferSocket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == kSocketInvalido) {
            continue;
        }
        if (connect(fd, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
            sock = fd;
            break;
        }
        closeTransferSocket(fd);
    }
    freeaddrinfo(res);
    if (sock != kSocketInvalido) {
        ponPlazoDeEnvio(sock, kEsperaEnvioMs);
        noHeredarTransferSocket(sock);
    }
    return sock;
}

SpawnedStdout spawnReadingStdout(const std::string& program, const std::vector<std::string>& args) {
    SpawnedStdout s;
#ifndef _WIN32
    int fds[2];
    if (pipe(fds) != 0) {
        return s;
    }
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return s;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execvp(program.c_str(), argv.data());
        _exit(127);
    }
    close(fds[1]);
    s.readEnd = fds[0];
    s.pid = pid;
    s.ok = true;
#else
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    HANDLE errRd = nullptr, errWr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        return s;
    }
    if (!CreatePipe(&errRd, &errWr, &sa, 0)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return s;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;
    si.hStdOutput = wr;
    si.hStdError = errWr;
    PROCESS_INFORMATION pi{};
    const std::string cmdLine = winBuildCommandLine(program, args);
    std::vector<char> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back('\0');
    const BOOL started = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    CloseHandle(errWr);
    if (!started) {
        CloseHandle(rd);
        CloseHandle(errRd);
        return s;
    }
    CloseHandle(pi.hThread);
    s.readEnd = rd;
    s.process = pi.hProcess;
    s.errText = std::make_shared<std::string>();
    s.errThread = std::make_shared<std::thread>([errRd, txt = s.errText]() {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(errRd, buf, sizeof(buf), &n, nullptr) && n > 0) {
            txt->append(buf, n);
        }
        CloseHandle(errRd);
    });
    s.ok = true;
#endif
    return s;
}

// --- Ficheros con un flujo de `zfs send` dentro.
//
// `send` puede volcar el flujo en un fichero y recibirlo de uno. Las dos puntas usan el
// mismo protocolo que una transferencia entre datasets —testigo y flujo por el socket—, así
// que el extremo de ZFS no cambia: el que recibe en un dataset no sabe si al otro lado hay un
// `zfs send` o un fichero, y el que emite no sabe si al otro lado hay un `zfs recv`.

// Una ruta absoluta de esta máquina, sin «..»: «/x/y», o en Windows «C:/x» o «C:\x».
bool rutaDeFlujoValida(const std::string& ruta, std::string& err) {
    bool absoluta = !ruta.empty() && ruta.front() == '/';
#ifdef _WIN32
    absoluta = absoluta
               || (ruta.size() >= 3 && std::isalpha(static_cast<unsigned char>(ruta[0]))
                   && ruta[1] == ':' && (ruta[2] == '/' || ruta[2] == '\\'));
#endif
    if (!absoluta) {
        err = "not an absolute path: " + ruta;
        return false;
    }
    std::string tramo;
    for (std::size_t i = 0; i <= ruta.size(); ++i) {
        if (i == ruta.size() || ruta[i] == '/' || ruta[i] == '\\') {
            if (tramo == "..") {
                err = "«..» is not allowed in the path: " + ruta;
                return false;
            }
            tramo.clear();
        } else {
            tramo.push_back(ruta[i]);
        }
    }
    return true;
}

// Abre un fichero de flujo para leerlo como si fuera la salida de un `zfs send`: el relé del
// emisor no distingue. Sin proceso detrás, matarlo no hace nada y esperarlo devuelve 0.
//
// Sin herencia, como todo descriptor del daemon (ver más arriba): un hijo lanzado por otro
// hilo no tiene por qué quedarse con el fichero abierto.
SpawnedStdout abreFicheroComoFlujo(const std::string& ruta, std::string& err) {
    SpawnedStdout s;
    if (!rutaDeFlujoValida(ruta, err)) {
        return s;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(ruta, ec)) {
        err = std::filesystem::exists(ruta, ec) ? "not a regular file: " + ruta
                                                : "the file does not exist: " + ruta;
        return s;
    }
#ifndef _WIN32
    const int fd = ::open(ruta.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        err = "cannot open " + ruta + ": " + std::strerror(errno);
        return s;
    }
    s.readEnd = fd;
#else
    const HANDLE h =
        CreateFileW(std::filesystem::path(ruta).wstring().c_str(), GENERIC_READ, FILE_SHARE_READ,
                    nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "cannot open " + ruta + " (error " + std::to_string(GetLastError()) + ")";
        return s;
    }
    s.readEnd = h;
#endif
    s.ok = true;
    return s;
}

// Devuelve bytes leídos, 0 en fin de datos, negativo en error.
long readSpawnedStdout(SpawnedStdout& s, char* buf, std::size_t len) {
#ifndef _WIN32
    for (;;) {
        const ssize_t n = read(s.readEnd, buf, len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return static_cast<long>(n);
    }
#else
    DWORD n = 0;
    if (!ReadFile(s.readEnd, buf, static_cast<DWORD>(len), &n, nullptr)) {
        // Una tubería cuyo escritor terminó contesta ERROR_BROKEN_PIPE: es su fin de datos.
        // Cualquier otro fallo —leyendo un fichero de flujo, por ejemplo— es un error, y
        // tomarlo por el final daría por bueno un flujo cortado.
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    }
    return static_cast<long>(n);
#endif
}

// Mata al hijo, sin esperarlo.
//
// Hace falta porque cerrar la tubería NO basta. `waitSpawnedStdout` cierra el extremo de
// lectura contando con que el hijo reciba un SIGPIPE en su siguiente escritura y muera; pero
// `zfs send` pasa ratos largos dentro del núcleo sin escribir nada —recorriendo el árbol de
// bloques— y ahí no hay ninguna escritura que falle. Entonces el `waitpid` de abajo, que es
// bloqueante, se queda esperando para siempre.
//
// Y lo que se queda esperando no es solo el hilo: es el POOL. Un `zfs send` vivo lo retiene,
// así que ni se puede exportar ni destruir. Medido: tras un relé fallido quedó un
// «zfs send -R worg/datos@uno» corriendo, y el pool no se dejó destruir hasta matarlo a mano.
void matarSpawnedStdout(SpawnedStdout& s) {
#ifndef _WIN32
    if (s.pid > 0) {
        kill(s.pid, SIGTERM);
    }
#else
    if (s.process) {
        TerminateProcess(s.process, 1);
    }
#endif
}

int waitSpawnedStdout(SpawnedStdout& s, std::string* errOut) {
#ifndef _WIN32
    if (s.readEnd >= 0) {
        close(s.readEnd);
        s.readEnd = -1;
    }
    int status = 0;
    if (s.pid > 0) {
        waitpid(s.pid, &status, 0);
        s.pid = -1;
    }
    return decodeWaitStatus(status);
#else
    if (s.readEnd) {
        CloseHandle(s.readEnd);
        s.readEnd = nullptr;
    }
    if (s.errThread && s.errThread->joinable()) {
        s.errThread->join();
    }
    if (errOut && s.errText) {
        *errOut = *s.errText;
    }
    DWORD code = 0;
    if (s.process) {
        WaitForSingleObject(s.process, INFINITE);
        GetExitCodeProcess(s.process, &code);
        CloseHandle(s.process);
        s.process = nullptr;
    }
    return static_cast<int>(code);
#endif
}

// Espera al emisor, comprueba el testigo y le da el flujo a `zfs recv`.
//
// Se ejecuta en un hilo suelto y no informa a nadie: quien quiera saber si la recepción
// fue bien lo consulta después por el estado del dataset.
void runTransferReceiveSession(TransferSocket listenFd, const std::string& token,
                               const std::string& dataset, bool force) {
    // Hasta 300 s esperando a que el emisor se conecte. Sin plazo, un emisor que nunca
    // llega deja el puerto abierto y el hilo vivo para siempre.
    struct timeval tv{};
    tv.tv_sec = 300;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(listenFd, &rfds);
    const int sel = select(static_cast<int>(listenFd) + 1, &rfds, nullptr, nullptr, &tv);
    if (sel <= 0) {
        closeTransferSocket(listenFd);
        return;
    }

    const TransferSocket clientFd = aceptaTransferSocket(listenFd);
    closeTransferSocket(listenFd);
#ifdef _WIN32
    if (clientFd == INVALID_SOCKET) {
        return;
    }
#else
    if (clientFd < 0) {
        return;
    }
#endif

    // Línea del testigo: 64 caracteres hexadecimales y un salto. Se lee con recv(), que
    // vale igual en las dos plataformas —read() no acepta sockets en Windows—.
    std::string recvToken;
    recvToken.reserve(65);
    char ch = 0;
    while (recvToken.size() < 65) {
        const int n = recv(clientFd, &ch, 1, 0);
        if (n <= 0) {
            break;
        }
        if (ch == '\n') {
            break;
        }
        recvToken.push_back(ch);
    }
    if (recvToken != token) {
        closeTransferSocket(clientFd);
        return;
    }

    std::vector<std::string> args = {"recv"};
    if (force) {
        args.push_back("-F");
    }
    args.push_back("-u");
    args.push_back("-s");
    args.push_back(dataset);

#ifndef _WIN32
    // De qué descriptor lee `zfs recv`, y por qué no da igual.
    //
    // En Linux y FreeBSD el socket ES la entrada del hijo: un dup2 y listo, sin copiar un
    // byte. En **macOS no**, y esto no es cautela: en Windows ya estaba medido —el
    // descriptor de sshd, también un socket, mata el recv a los 132 KiB— y el 2026-08-25
    // apareció la variante de macOS, peor. Ahí `zfs recv` alimentado por algo que no es
    // una tubería se comporta mal de dos formas distintas: desde un FICHERO normal
    // rechaza el flujo con «invalid backup stream» —comprobado con el mismo fichero, mismo
    // md5, que por `cat |` entra sin una queja—, y desde un SOCKET se lleva la máquina por
    // delante. Cuatro cuelgues de un MacBook, con tres versiones distintas de OpenZFS
    // —2.3.1, 2.4.1 y 2.4.3rc2, esta última con kernel panic— y siempre por este camino;
    // por tubería, las mismas versiones reciben perfectamente.
    //
    // Así que en macOS se BOMBEA, igual que en Windows: el daemon lee del socket y escribe
    // en una tubería anónima, que es la única entrada que ese port digiere bien.
#ifdef __APPLE__
    int inPipe[2] = {-1, -1};
    if (pipe(inPipe) != 0) {
        daemonLog("ERROR", "recv-listen: could not create the input pipe");
        closeTransferSocket(clientFd);
        return;
    }
    // Si el hijo muere antes de tiempo, la escritura en la tubería levanta un SIGPIPE que
    // se llevaría al daemon entero. Se ignora aquí para que `write` devuelva EPIPE y el
    // bombeo termine como lo que es: el receptor se ha ido.
    void (*sigpipePrevio)(int) = std::signal(SIGPIPE, SIG_IGN);
#endif
    std::vector<char*> argv2;
    argv2.push_back(const_cast<char*>("zfs"));
    for (const std::string& a : args) {
        argv2.push_back(const_cast<char*>(a.c_str()));
    }
    argv2.push_back(nullptr);
    // La salida de error del hijo se recoge. Antes se iba a la del daemon —o sea, a
    // ninguna parte cuando corre como servicio— y el motivo por el que `zfs recv` no
    // aceptaba el flujo se perdía. Al emisor solo le llega que la conexión se cortó, así
    // que si aquí tampoco queda constancia no hay dónde mirar.
    int errPipe[2] = {-1, -1};
    const bool hayTuberia = (pipe(errPipe) == 0);
    const pid_t pid = fork();
    if (pid == 0) {
#ifdef __APPLE__
        close(inPipe[1]);
        dup2(inPipe[0], STDIN_FILENO);
        close(inPipe[0]);
#else
        dup2(clientFd, STDIN_FILENO);
#endif
        if (hayTuberia) {
            dup2(errPipe[1], STDERR_FILENO);
            close(errPipe[0]);
            close(errPipe[1]);
        }
        close(clientFd);
        execvp("zfs", argv2.data());
        _exit(127);
    }
#ifdef __APPLE__
    // El bombeo va ANTES de vaciar la salida de error: mientras dure, el hijo está
    // recibiendo. Y su fin —socket cerrado por el emisor, o escritura imposible porque el
    // hijo ya no está— es lo que cierra la entrada de `zfs recv` y le dice que se acabó.
    close(inPipe[0]);
    if (pid > 0) {
        std::vector<char> buf(256 * 1024);
        for (;;) {
            const ssize_t leidos = ::recv(clientFd, buf.data(), buf.size(), 0);
            if (leidos <= 0) {
                break;
            }
            ssize_t escritos = 0;
            while (escritos < leidos) {
                const ssize_t n = ::write(inPipe[1], buf.data() + escritos,
                                          static_cast<size_t>(leidos - escritos));
                if (n <= 0) {
                    if (n < 0 && errno == EINTR) {
                        continue;
                    }
                    break;
                }
                escritos += n;
            }
            if (escritos < leidos) {
                break;
            }
        }
    }
    close(inPipe[1]);
    std::signal(SIGPIPE, sigpipePrevio);
#endif
    // El socket se queda abierto hasta saber cómo acabó `zfs recv`: por él va el veredicto
    // al emisor (ver informaAlEmisor). Si el hijo muere antes de tiempo no hay espera de
    // más: la salida de error se cierra con él, el waitpid vuelve al momento, y al cerrar
    // aquí con datos sin leer el emisor recibe el corte como antes.
    std::string recvErr;
    if (hayTuberia) {
        close(errPipe[1]);
        // Se vacía ANTES del waitpid: si la tubería se llenase con el hijo aún vivo y
        // nosotros esperando, se quedarían los dos bloqueados el uno por el otro.
        char eb[512];
        ssize_t en = 0;
        while ((en = read(errPipe[0], eb, sizeof(eb))) > 0) {
            recvErr.append(eb, static_cast<size_t>(en));
        }
        close(errPipe[0]);
    }
    int recvStatus = 0;
    if (pid > 0) {
        waitpid(pid, &recvStatus, 0);
    }
    const int recvRc = pid > 0 ? decodeWaitStatus(recvStatus) : 127;
    if (recvRc != 0) {
        daemonLog("ERROR", "recv-listen: zfs recv on «" + dataset
                               + "» failed (rc=" + std::to_string(recvRc) + ") " + trim(recvErr));
    }
    informaAlEmisor(clientFd, recvRc,
                    "zfs recv on " + dataset + " failed (exit " + std::to_string(recvRc)
                        + "): " + trim(recvErr));
    close(clientFd);
#else
    // En Windows NO se le puede pasar el socket como entrada al proceso.
    //
    // Esto no es cautela: está medido. `zfs recv` alimentado por el descriptor que
    // entrega sshd —también un socket— muere con «cannot receive: I/O error» a los
    // 132 KiB, mientras que por una tubería anónima recibe el flujo entero con las
    // sumas de verificación intactas. Así que aquí se BOMBEA: el daemon lee del socket
    // y escribe en una tubería que es la que ve `zfs`.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE inRd = nullptr, inWr = nullptr;
    HANDLE outRd = nullptr, outWr = nullptr;
    if (!CreatePipe(&inRd, &inWr, &sa, 0)) {
        closeTransferSocket(clientFd);
        return;
    }
    if (!CreatePipe(&outRd, &outWr, &sa, 0)) {
        CloseHandle(inRd);
        CloseHandle(inWr);
        closeTransferSocket(clientFd);
        return;
    }
    SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRd;
    // Salida y error a una tubería propia: el daemon puede correr como servicio, sin
    // consola donde heredar nada, y lo que diga `zfs recv` al fallar es justo lo que
    // hace falta para saber por qué. Se registra al terminar.
    si.hStdOutput = outWr;
    si.hStdError = outWr;
    PROCESS_INFORMATION pi{};
    const std::string cmdLine = winBuildCommandLine("zfs", args);
    std::vector<char> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back('\0');
    const BOOL started = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(inRd);
    CloseHandle(outWr);
    if (!started) {
        CloseHandle(inWr);
        CloseHandle(outRd);
        closeTransferSocket(clientFd);
        daemonLog("ERROR", "recv-listen: could not launch zfs recv");
        return;
    }

    std::string childOutput;
    std::thread drainThread([outRd, &childOutput]() {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(outRd, buf, sizeof(buf), &n, nullptr) && n > 0) {
            childOutput.append(buf, n);
        }
        CloseHandle(outRd);
    });

    // El bombeo. 64 KiB por vuelta: el flujo de `zfs send` son megas o gigas, y un búfer
    // de 4 KiB multiplica por dieciséis las idas y venidas sin ganar nada.
    std::vector<char> buf(65536);
    std::uint64_t moved = 0;
    while (true) {
        const int n = recv(clientFd, buf.data(), static_cast<int>(buf.size()), 0);
        if (n <= 0) {
            break;
        }  // 0 = el emisor terminó; <0 = se cortó
        int off = 0;
        bool writeFailed = false;
        while (off < n) {
            DWORD written = 0;
            if (!WriteFile(inWr, buf.data() + off, static_cast<DWORD>(n - off), &written, nullptr)
                || written == 0) {
                writeFailed = true;  // `zfs recv` cerró su entrada: no hay a quién escribir
                break;
            }
            off += static_cast<int>(written);
        }
        if (writeFailed) {
            break;
        }
        moved += static_cast<std::uint64_t>(n);
    }
    // Cerrar la escritura es lo que le da fin de datos a `zfs recv`. Sin esto se queda
    // esperando más flujo y no termina nunca.
    CloseHandle(inWr);

    drainThread.join();
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    informaAlEmisor(clientFd, static_cast<int>(exitCode),
                    "zfs recv on " + dataset + " failed (exit " + std::to_string(exitCode)
                        + "): " + trim(childOutput));
    closeTransferSocket(clientFd);
    daemonLog(exitCode == 0 ? "INFO" : "ERROR",
              "recv-listen " + dataset + ": rc=" + std::to_string(exitCode)
                  + " bytes=" + std::to_string(moved)
                  + (childOutput.empty() ? std::string() : (" " + trim(childOutput))));
#endif
}

// Abre un puerto efímero, espera al emisor y le entrega el flujo a `zfs recv`.
//
// Servido por RPC devuelve PORT y TOKEN de inmediato y recibe en un hilo suelto, porque
// el daemon sigue vivo para atenderlo.
//
// `waitInline` es para la invocación por línea de comandos, donde ese reparto no vale:
// el proceso terminaría en cuanto imprimiera el puerto y se llevaría por delante el hilo
// que iba a recibir. Ahí se emiten PORT y TOKEN por la salida —para que el emisor pueda
// leerlos y conectarse— y se atiende la sesión sin soltar el proceso.
// Abre el puerto por el que va a entrar una transfer, y dice cuál le tocó.
//
// Está aparte porque lo usan DOS receptores —el de `zfs recv` y el del árbol de ficheros— y
// dentro hay dos decisiones que costaron encontrar: la doble pila (se escucha en IPv6 con
// V6ONLY desactivado, que acepta también IPv4) y que «0.0.0.0» es el valor por omisión de
// la configuración y no una elección del usuario. Tenerlo dos veces sería tener esas dos
// decisiones en un sitio y no en el otro.
bool abrePuertoDeTransferencia(TransferSocket& listenFd, int& puerto, std::string& error) {
    const AgentRuntimeConfig cfg2 = loadRuntimeConfig();
#ifdef _WIN32
    ensureWinsock();
    listenFd = INVALID_SOCKET;
#else
    listenFd = -1;
#endif
    bool boundV6 = false;
    // "0.0.0.0" es el valor POR DEFECTO de la estructura, no una decisión del usuario:
    // transferBindAddr nunca llega vacío. Tratarlo como explícito dejaba el receptor en
    // IPv4 siempre, que es justo lo que rompía la copia entre máquinas por IPv6.
    const bool wantsExplicitBind =
        !cfg2.transferBindAddr.empty() && cfg2.transferBindAddr != "0.0.0.0";

    auto tryBind = [&](int family, const char* addr) -> bool {
        struct addrinfo hints2{};
        hints2.ai_family = family;
        hints2.ai_socktype = SOCK_STREAM;
        hints2.ai_flags = AI_PASSIVE;
        struct addrinfo* res2 = nullptr;
        if (getaddrinfo(addr, nullptr, &hints2, &res2) != 0 || !res2) {
            return false;
        }
        TransferSocket fd =
            creaTransferSocket(res2->ai_family, res2->ai_socktype, res2->ai_protocol);
#ifdef _WIN32
        const bool bad = (fd == INVALID_SOCKET);
#else
        const bool bad = (fd < 0);
#endif
        if (bad) {
            freeaddrinfo(res2);
            return false;
        }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
        if (family == AF_INET6) {
            int off = 0;
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off),
                       sizeof(off));
        }
        if (bind(fd, res2->ai_addr, static_cast<int>(res2->ai_addrlen)) != 0
            || listen(fd, 1) != 0) {
            closeTransferSocket(fd);
            freeaddrinfo(res2);
            return false;
        }
        freeaddrinfo(res2);
        listenFd = fd;
        boundV6 = (family == AF_INET6);
        return true;
    };

    if (wantsExplicitBind) {
        // Si el usuario fijó una dirección, se respeta tal cual y no se elige familia.
        if (!tryBind(AF_UNSPEC, cfg2.transferBindAddr.c_str())) {
            error = "could not listen on " + cfg2.transferBindAddr;
            return false;
        }
    } else if (!tryBind(AF_INET6, "::")) {
        // Sin IPv6 en la máquina, IPv4 a secas: es mejor que no escuchar.
        if (!tryBind(AF_INET, "0.0.0.0")) {
            error = "could not open the transfer port";
            return false;
        }
    }

    struct sockaddr_storage bound{};
    socklen_t blen = sizeof(bound);
    getsockname(listenFd, reinterpret_cast<struct sockaddr*>(&bound), &blen);
    puerto =
        static_cast<int>(ntohs(boundV6 ? reinterpret_cast<struct sockaddr_in6*>(&bound)->sin6_port
                                       : reinterpret_cast<struct sockaddr_in*>(&bound)->sin_port));
    return true;
}

// Escribe todo o falla. `send` puede colocar menos de lo que se le pide.
bool mandaTodo(TransferSocket s, const char* datos, std::size_t cuantos) {
    std::size_t puesto = 0;
    while (puesto < cuantos) {
        const int n = send(s, datos + puesto, static_cast<int>(cuantos - puesto), 0);
        if (n <= 0) {
            return false;
        }
        puesto += static_cast<std::size_t>(n);
    }
    return true;
}

bool mandaTodo(TransferSocket s, const std::string& t) {
    return mandaTodo(s, t.data(), t.size());
}

// Lee exactamente `cuantos` bytes, o falla. Un `recv` corto no es un error: es lo normal.
bool leeExacto(TransferSocket s, char* destino, std::size_t cuantos) {
    std::size_t leido = 0;
    while (leido < cuantos) {
        const int n = recv(s, destino + leido, static_cast<int>(cuantos - leido), 0);
        if (n <= 0) {
            return false;
        }
        leido += static_cast<std::size_t>(n);
    }
    return true;
}

// Una línea terminada en salto. Se lee byte a byte a propósito: detrás de la línea vienen
// datos en crudo, y leer de más se los comería.
bool leeLinea(TransferSocket s, std::string& salida, std::size_t tope) {
    salida.clear();
    char c = 0;
    while (salida.size() < tope) {
        const int n = recv(s, &c, 1, 0);
        if (n <= 0) {
            return false;
        }
        if (c == '\n') {
            return true;
        }
        salida.push_back(c);
    }
    return false;
}

// El manifiesto entero, que termina en una línea «E».
bool leeManifiesto(TransferSocket s, std::vector<AR::Entry>& salida, std::string& error) {
    std::string acumulado;
    for (;;) {
        std::string linea;
        if (!leeLinea(s, linea)) {
            error = "the connection dropped while reading the manifest";
            return false;
        }
        acumulado += linea;
        acumulado += '\n';
        if (linea == "E") {
            break;
        }
        // Tras la cabecera vienen la ruta, el destino y —si la línea los declara— el dueño y
        // el grupo por nombre. Estos dos se añadieron al manifiesto y aquí no se contaban:
        // sus bytes se quedaban en el socket, la línea siguiente empezaba por «root…» y la
        // copia entre máquinas moría con «línea de manifiesto ilegible» en cuanto el destino
        // tenía algo dentro. Con el destino vacío el manifiesto es solo «E» y no se notaba.
        std::size_t largo = 0;
        if (!AR::manifestNamesLength(linea, largo)) {
            error = "unreadable manifest line";
            return false;
        }
        std::string nombres(largo, '\0');
        if (largo > 0 && !leeExacto(s, nombres.data(), largo)) {
            error = "the connection dropped while reading a name";
            return false;
        }
        acumulado += nombres;
    }
    return AR::parseManifest(acumulado, salida, error);
}

// The receiving end of a tree transfer: it says which system it is, sends what it has and
// applies what it is told.
//
// Everything written goes through a TEMPORARY file and a rename, as in copytree: writing over
// the file dragged the target's hard links along, and a cut halfway left the good file
// destroyed. Directory metadata goes last, innermost first: creating or renaming inside a
// directory changes its date, and a mode 000 set earlier would stop it being filled.
//
// A failed operation is counted and reported, but does not stop the rest: a link that cannot
// be created is no reason to leave ten thousand files uncopied. What does stop it is the
// connection dropping, because then there is nothing left to read.
class TreeReceiver {
public:
    TreeReceiver(TransferSocket sock, const std::string& root)
        : sock_(sock),
          root_(root),
          base_(root),
          baseText_(std::filesystem::path(root).lexically_normal().generic_string()) {}

    void run() {
        if (!exchangeOwnerNames() || !sendManifest() || !answerSignatureRequests()) {
            return;
        }
        applyOperations();
        applyDirectoryMetadata();
        reply();
    }

private:
    TransferSocket sock_;
    std::string root_;
    std::filesystem::path base_;
    std::string baseText_;
    std::vector<std::string> sourceUsers_;
    std::vector<std::string> sourceGroups_;
    // The block size of each file whose signatures were sent: without it, «copy block N»
    // means nothing.
    std::map<std::string, std::size_t> blockSizes_;
    // Directories whose metadata is applied at the end.
    std::vector<std::pair<std::string, AR::Entry>> directories_;
    std::uint64_t done_{0};
    std::uint64_t failed_{0};
    std::string firstReason_;
    // Set when the transfer cannot go on (the connection dropped, a malformed stream).
    std::string failure_;

    // ── Protocol phases ────────────────────────────────────────────────────────────────

    // The owner and group names the source uses; the answer is which number each one has
    // here, so the sender compares what will really end up on disk (see OwnerMap).
    bool exchangeOwnerNames() {
        for (;;) {
            std::string line;
            if (!leeLinea(sock_, line)) {
                return false;
            }
            if (line == "E") {
                break;
            }
            std::istringstream iss(line);
            char which = 0;
            std::size_t length = 0;
            if (!(iss >> which >> length) || (which != 'u' && which != 'g') || length > 1024) {
                return false;
            }
            std::string name(length, '\0');
            if (length > 0 && !leeExacto(sock_, name.data(), length)) {
                return false;
            }
            (which == 'u' ? sourceUsers_ : sourceGroups_).push_back(name);
        }
        return true;
    }

    // Which system this is —the sender only sends the attributes and ACLs that fit here, and
    // counts the rest to warn about them—, the numbers of those names here, and the manifest
    // of what this end already has (its root included, so the root is only touched when it
    // differs).
    bool sendManifest() {
        std::string hello =
            std::string("R ") + zfsmgr::fsmeta::nameOf(zfsmgr::fsmeta::localFamily()) + "\n";
        const AR::OwnerMap here = AR::resolveHere(sourceUsers_, sourceGroups_);
        for (const auto& kv : here.users) {
            hello += "u " + std::to_string(kv.second) + " " + std::to_string(kv.first.size()) + "\n"
                     + kv.first;
        }
        for (const auto& kv : here.groups) {
            hello += "g " + std::to_string(kv.second) + " " + std::to_string(kv.first.size()) + "\n"
                     + kv.first;
        }
        hello += "E\n";
        if (!mandaTodo(sock_, hello)) {
            return false;
        }
        std::vector<AR::Entry> mine;
        std::string err;
        if (!AR::walk(root_, mine, err)) {
            daemonLog("ERROR", "tree-recv: could not walk «" + root_ + "»: " + err);
            (void)mandaTodo(sock_, "E\n");
            return false;
        }
        AR::Entry self;
        if (AR::describePath(root_, self, err)) {
            self.path = ".";
            mine.insert(mine.begin(), self);
        }
        return mandaTodo(sock_, AR::serializeManifest(mine));
    }

    // The sender asks for signatures of the large files this end already has, and with them
    // sends a PATCH instead of the whole file.
    bool answerSignatureRequests() {
        for (;;) {
            std::string line;
            if (!leeLinea(sock_, line)) {
                return false;
            }
            if (line == "E") {
                break;
            }
            std::istringstream iss(line);
            char letter = 0;
            std::size_t length = 0;
            if (!(iss >> letter >> length) || letter != 'S') {
                return false;
            }
            std::string path(length, '\0');
            if (length > 0 && !leeExacto(sock_, path.data(), length)) {
                return false;
            }
            const std::filesystem::path local =
                (base_ / std::filesystem::path(path)).lexically_normal();
            std::error_code ec;
            const auto size = static_cast<std::uint64_t>(std::filesystem::file_size(local, ec));
            if (ec) {
                continue;  // gone: the sender will send it whole
            }
            const std::size_t blockSize = AR::blockSize(size);
            std::vector<AR::Signature> signatures;
            std::string err;
            if (!AR::signaturesOf(local.string(), blockSize, signatures, err)) {
                continue;
            }
            const std::string raw = AR::serializeSignatures(signatures);
            blockSizes_[path] = blockSize;
            if (!mandaTodo(sock_, "G " + std::to_string(blockSize) + " "
                                      + std::to_string(raw.size()) + " "
                                      + std::to_string(path.size()) + "\n")
                || !mandaTodo(sock_, path) || !mandaTodo(sock_, raw)) {
                return false;
            }
        }
        return mandaTodo(sock_, "E\n");
    }

    void applyOperations() {
        for (;;) {
            std::string line;
            if (!leeLinea(sock_, line)) {
                failure_ = "the connection dropped";
                return;
            }
            if (line == "E" || !applyOperation(line) || !failure_.empty()) {
                return;
            }
        }
    }

    // Innermost first: setting a child's date changes its parent's.
    void applyDirectoryMetadata() {
        std::stable_sort(directories_.begin(), directories_.end(),
                         [](const auto& a, const auto& b) {
                             const auto depth = [](const std::string& s) {
                                 return std::count(s.begin(), s.end(), '/');
                             };
                             return depth(a.second.path) > depth(b.second.path);
                         });
        for (const auto& d : directories_) {
            (void)applyMetadata(d.first, d.second);
        }
    }

    void reply() {
        if (failure_.empty() && failed_ > 0) {
            daemonLog("WARN", "tree-recv: " + std::to_string(failed_)
                                  + " operations could not be applied; the first: " + firstReason_);
            (void)mandaTodo(sock_, "PARCIAL " + std::to_string(done_) + " "
                                       + std::to_string(failed_) + " " + firstReason_ + "\n");
        } else if (failure_.empty()) {
            (void)mandaTodo(sock_, "OK " + std::to_string(done_) + "\n");
        } else {
            daemonLog("ERROR", "tree-recv: " + failure_);
            (void)mandaTodo(sock_, "ERR " + failure_ + "\n");
        }
    }

    // ── One operation ──────────────────────────────────────────────────────────────────

    // False only when nothing more can be read.
    bool applyOperation(std::string line) {
        // A PATCH carries the same header as a file, with «P» and the hash of the result in
        // the place of the link target.
        const bool isPatch = !line.empty() && line[0] == 'P';
        if (isPatch) {
            line[0] = 'F';
        }
        AR::Operation op;
        AR::HeaderLengths lengths;
        if (!AR::parseHeader(line, op, lengths, failure_)) {
            return false;
        }
        std::string names(lengths.total(), '\0');
        if (lengths.total() > 0 && !leeExacto(sock_, names.data(), lengths.total())) {
            failure_ = "the connection dropped while reading a name";
            return false;
        }
        if (!AR::takeNames(names, lengths, op, failure_)) {
            return false;
        }
        // A name this system cannot represent —bytes that are not UTF-8, on Windows— makes
        // building the path THROW. Uncaught, the exception left the thread and took the whole
        // daemon down. It is a failure of that entry: it is counted and its bytes are still
        // read, so the stream does not get out of step.
        std::filesystem::path target;
        bool unrepresentable = false;
        try {
            if (!inside(op.entry.path, target)) {
                failure_ = "path outside the tree: " + op.entry.path;
                return false;
            }
            (void)target.string();
        } catch (const std::exception&) {
            unrepresentable = true;
        }
        if (unrepresentable) {
            noteFailure("«" + op.entry.path + "» is not a valid name on this system");
            return !(isPatch || op.action == AR::Action::Send) || discard(isPatch);
        }
        try {
            const bool ok = isPatch ? applyPatch(op, target) : applyAction(op, target);
            if (ok && failure_.empty()) {
                ++done_;
            }
        } catch (const std::exception& ex) {
            // Any other exception stops the transfer with its reason, but not the daemon.
            failure_ = "«" + op.entry.path + "»: " + ex.what();
        }
        return failure_.empty();
    }

    bool applyAction(const AR::Operation& op, const std::filesystem::path& target) {
        const std::string targetText = target.string();
        std::error_code ec;
        switch (op.action) {
            case AR::Action::MakeDirectory:
                if (!clearPlace(target, AR::EntryKind::Directory)) {
                    return false;
                }
                std::filesystem::create_directory(target, ec);
                if (ec) {
                    noteFailure("could not create the directory «" + op.entry.path
                                + "»: " + ec.message());
                    return false;
                }
                directories_.emplace_back(targetText, op.entry);
                return true;
            case AR::Action::Metadata_: {
                const auto st = std::filesystem::symlink_status(target, ec);
                if (!ec && std::filesystem::is_directory(st) && !std::filesystem::is_symlink(st)) {
                    directories_.emplace_back(targetText, op.entry);
                    return true;
                }
                return applyMetadata(targetText, op.entry);
            }
            case AR::Action::Send:
                return receiveFile(op, target);
            case AR::Action::Symlink_:
                if (!clearPlace(target, AR::EntryKind::Symlink)) {
                    return false;
                }
                // On Windows creating a symbolic link needs SeCreateSymbolicLinkPrivilege or
                // developer mode. Without it this fails, and it has to be SAID.
                std::filesystem::create_symlink(op.entry.target, target, ec);
                if (ec) {
                    noteFailure("could not create the symbolic link «" + op.entry.path
                                + "»: " + ec.message());
                    return false;
                }
                return applyMetadata(targetText, op.entry);
            case AR::Action::HardLink_: {
                std::filesystem::path existing;
                if (!inside(op.entry.target, existing)) {
                    failure_ = "path outside the tree: " + op.entry.target;
                    return false;
                }
                std::string err;
                if (!clearPlace(target, AR::EntryKind::HardLink)
                    || !AR::makeHardLink(existing.string(), targetText, err)) {
                    if (!err.empty()) {
                        noteFailure(err);
                    }
                    return false;
                }
                return true;
            }
            case AR::Action::Special_: {
                std::string err;
                if (!clearPlace(target, AR::EntryKind::Special)) {
                    return false;
                }
                if (!AR::makeSpecial(targetText, op.entry, err)) {
                    noteFailure(err);
                    return false;
                }
                return applyMetadata(targetText, op.entry);
            }
            case AR::Action::Delete:
                // What is no longer there (gone with a replaced directory) is not a failure.
                std::filesystem::remove_all(target, ec);
                if (ec) {
                    noteFailure("could not delete «" + op.entry.path + "»: " + ec.message());
                    return false;
                }
                return true;
        }
        return false;
    }

    // The contents arrive in STRETCHES: of data, with their bytes behind, and of hole, which
    // is only a length. If it cannot be written, the bytes are STILL READ and thrown away:
    // the sender has already put them on the wire, and skipping them would leave the stream
    // out of step for the next header.
    bool receiveFile(const AR::Operation& op, const std::filesystem::path& target) {
        const bool placeFree = clearPlace(target, AR::EntryKind::File);
        const std::string temp = AR::tempPathNextTo(target.string());
        AR::SparseWriter w;
        std::string err;
        bool writing = placeFree && w.open(temp, err);
        if (placeFree && !writing) {
            noteFailure("could not write «" + op.entry.path + "»: " + err);
        }
        std::vector<char> buf(65536);
        std::error_code ec;
        for (;;) {
            std::string stretch;
            if (!leeLinea(sock_, stretch)) {
                failure_ = "the connection dropped while writing " + op.entry.path;
                break;
            }
            if (stretch == "e") {
                break;
            }
            std::istringstream iss(stretch);
            char kind = 0;
            std::uint64_t n = 0;
            if (!(iss >> kind >> n) || (kind != 'd' && kind != 'h')) {
                failure_ = "unreadable stretch of " + op.entry.path;
                break;
            }
            if (kind == 'h') {
                w.skip(n);
                continue;
            }
            while (n > 0) {
                const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(n, buf.size()));
                if (!leeExacto(sock_, buf.data(), chunk)) {
                    failure_ = "the connection dropped while writing " + op.entry.path;
                    break;
                }
                if (writing && !w.write(buf.data(), chunk)) {
                    writing = false;
                    noteFailure("could not write all of «" + op.entry.path + "»");
                }
                n -= chunk;
            }
            if (!failure_.empty()) {
                break;
            }
        }
        if (!failure_.empty()) {
            std::filesystem::remove(temp, ec);
            return false;
        }
        if (writing && w.position() != op.entry.size) {
            writing = false;
            noteFailure("«" + op.entry.path + "» arrived with a different size");
        }
        if (writing && !w.finish(err)) {
            writing = false;
            noteFailure(err);
        }
        if (!writing) {
            std::filesystem::remove(temp, ec);
            return false;
        }
        return putInPlace(temp, target, op.entry);
    }

    // Rebuilt on a new file by reading pieces of the old one, and only put in place if the
    // hash of the WHOLE result matches the one the source sends. Otherwise the original is
    // NOT touched: an old file is worth more than a half-made one.
    bool applyPatch(const AR::Operation& op, const std::filesystem::path& target) {
        const std::string targetText = target.string();
        const auto known = blockSizes_.find(op.entry.path);
        const std::size_t blockSize =
            known != blockSizes_.end() ? known->second : AR::blockSize(op.entry.size);
        const std::string temp = AR::tempPathNextTo(targetText);
        AR::SparseWriter w;
        std::string err;
        bool writing = w.open(temp, err);
        std::FILE* old = std::fopen(targetText.c_str(), "rb");
        writing = writing && old != nullptr;
        std::vector<char> piece(65536);
        for (;;) {
            std::string li;
            if (!leeLinea(sock_, li)) {
                failure_ = "dropped while reading the patch";
                break;
            }
            if (li == "Z") {
                break;
            }
            std::istringstream iss(li);
            char q = 0;
            if (li[0] == 'C') {
                std::uint64_t block = 0;
                std::uint64_t count = 0;
                iss >> q >> block >> count;
                for (std::uint64_t k = 0; k < count && writing; ++k) {
                    if (std::fseek(old, static_cast<long>((block + k) * blockSize), SEEK_SET)
                        != 0) {
                        writing = false;
                        break;
                    }
                    std::size_t left = blockSize;
                    while (left > 0) {
                        const std::size_t n =
                            std::fread(piece.data(), 1, std::min(left, piece.size()), old);
                        if (n == 0) {
                            break;  // the last block can be short
                        }
                        if (!w.write(piece.data(), n)) {
                            writing = false;
                            break;
                        }
                        left -= n;
                    }
                }
            } else if (li[0] == 'L') {
                std::size_t count = 0;
                iss >> q >> count;
                while (count > 0) {
                    const std::size_t t = std::min(count, piece.size());
                    if (!leeExacto(sock_, piece.data(), t)) {
                        failure_ = "dropped while reading patch data";
                        break;
                    }
                    if (writing && !w.write(piece.data(), t)) {
                        writing = false;
                    }
                    count -= t;
                }
                if (!failure_.empty()) {
                    break;
                }
            } else {
                failure_ = "unknown patch instruction";
                break;
            }
        }
        if (old != nullptr) {
            std::fclose(old);
        }
        writing = writing && w.finish(err);
        std::string hash;
        if (failure_.empty() && writing && AR::fileHash(temp, hash, err)
            && hash == op.entry.target) {
            return putInPlace(temp, target, op.entry);
        }
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        if (failure_.empty()) {
            noteFailure("the patch for «" + op.entry.path + "» did not rebuild the expected file");
        }
        return false;
    }

    // ── Helpers ────────────────────────────────────────────────────────────────────────

    void noteFailure(const std::string& reason) {
        ++failed_;
        if (firstReason_.empty()) {
            firstReason_ = reason;
        }
    }

    // The path comes from the OTHER end, so it is checked before touching the disk: without
    // this, a «../..» would write outside the tree being synced.
    bool inside(const std::string& rel, std::filesystem::path& out) const {
        out = (base_ / std::filesystem::path(rel)).lexically_normal();
        return out.generic_string().rfind(baseText_, 0) == 0;
    }

    // Whatever is in the place and is of ANOTHER kind, out. The sender has already decided it
    // may (a directory with contents, only with deletion). A file where a file goes stays:
    // the temporary's rename replaces it without leaving the place empty.
    bool clearPlace(const std::filesystem::path& p, AR::EntryKind incoming) {
        std::error_code ec;
        const auto st = std::filesystem::symlink_status(p, ec);
        if (ec || !std::filesystem::exists(st)) {
            return true;
        }
        const bool isDir = std::filesystem::is_directory(st) && !std::filesystem::is_symlink(st);
        const bool isFile = std::filesystem::is_regular_file(st);
        if ((incoming == AR::EntryKind::Directory && isDir)
            || (incoming == AR::EntryKind::File && isFile)) {
            return true;
        }
        std::filesystem::remove_all(p, ec);
        if (ec) {
            noteFailure("could not remove «" + p.string() + "»: " + ec.message());
            return false;
        }
        return true;
    }

    bool applyMetadata(const std::string& path, const AR::Entry& e) {
        if (AR::applyMetadata(path, e) > 0) {
            noteFailure("could not apply all the metadata of «" + e.path + "»");
            return false;
        }
        return true;
    }

    // The written temporary, into its place: metadata BEFORE the rename, so the file never
    // appears in its place with the wrong owner or mode.
    bool putInPlace(const std::string& temp, const std::filesystem::path& p, const AR::Entry& e) {
        (void)applyMetadata(temp, e);
        std::error_code ec;
        std::filesystem::rename(temp, p, ec);
        if (ec) {
            std::filesystem::remove(temp, ec);
            noteFailure("could not put «" + e.path + "» in place: " + ec.message());
            return false;
        }
        return true;
    }

    // Reads and throws away the contents of an operation that will not be applied: the
    // stretches of a file, or the instructions of a patch.
    bool discard(bool patch) {
        std::vector<char> junk(65536);
        for (;;) {
            std::string li;
            if (!leeLinea(sock_, li)) {
                failure_ = "the connection dropped";
                return false;
            }
            if ((patch && li == "Z") || (!patch && li == "e")) {
                return true;
            }
            std::istringstream iss(li);
            char q = 0;
            std::uint64_t n = 0;
            if (!(iss >> q)) {
                failure_ = "unreadable data";
                return false;
            }
            const bool withBytes = patch ? (q == 'L') : (q == 'd');
            if (!withBytes) {
                continue;  // a hole, or «copy block N»: no bytes behind
            }
            iss >> n;
            while (n > 0) {
                const auto c = static_cast<std::size_t>(std::min<std::uint64_t>(n, junk.size()));
                if (!leeExacto(sock_, junk.data(), c)) {
                    failure_ = "the connection dropped";
                    return false;
                }
                n -= c;
            }
        }
    }
};

// Waits for the sender (up to five minutes), checks its single-use token and hands the
// connection to a TreeReceiver.
void runTreeReceiveSession(TransferSocket listenFd, const std::string& token,
                           const std::string& root) {
    struct timeval tv{};
    tv.tv_sec = 300;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(listenFd, &rfds);
    if (select(static_cast<int>(listenFd) + 1, &rfds, nullptr, nullptr, &tv) <= 0) {
        closeTransferSocket(listenFd);
        return;
    }
    const TransferSocket client = aceptaTransferSocket(listenFd);
    closeTransferSocket(listenFd);
#ifdef _WIN32
    if (client == INVALID_SOCKET) {
        return;
    }
#else
    if (client < 0) {
        return;
    }
#endif
    ponPlazoDeEnvio(client, kEsperaEnvioMs);

    std::string received;
    if (!leeLinea(client, received, 128) || received != token) {
        closeTransferSocket(client);
        daemonLog("WARN", "tree-recv: token does not match; connection rejected");
        return;
    }
    TreeReceiver(client, root).run();
    closeTransferSocket(client);
}

ExecResult runTreeRecvListenCapture(const std::string& raiz, bool waitInline) {
    ExecResult r;
    if (trim(raiz).empty()) {
        r.rc = 2;
        ponCodigo(r, "dir_missing", {});
        return r;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(raiz, ec)) {
        r.rc = 2;
        ponCodigo(r, "not_a_dir", {raiz});
        return r;
    }
    const std::string token = generateTransferToken();
    if (token.empty()) {
        r.rc = 1;
        ponCodigo(r, "token_failed", {});
        return r;
    }
    TransferSocket listenFd{};
    int puerto = 0;
    std::string errPuerto;
    if (!abrePuertoDeTransferencia(listenFd, puerto, errPuerto)) {
        r.rc = 1;
        r.err = errPuerto + "\n";
        return r;
    }
    if (waitInline) {
        std::cout << "PORT=" << puerto << "\nTOKEN=" << token << "\n" << std::flush;
        runTreeReceiveSession(listenFd, token, raiz);
        r.rc = 0;
        return r;
    }
    std::thread([listenFd, token, raiz]() {
        runTreeReceiveSession(listenFd, token, raiz);
    }).detach();
    r.rc = 0;
    r.out = "PORT=" + std::to_string(puerto) + "\nTOKEN=" + token + "\n";
    return r;
}

// El emisor: pide el manifiesto, decide y manda.
ExecResult runTreeSendToPeerCapture(const std::vector<std::string>& params) {
    ExecResult r;
    if (params.size() < 4) {
        r.rc = 2;
        r.err =
            "usage: --tree-send-to-peer <dir> <host> <port> <token> [--delete] [--dry-run] "
            "[--as <name>]\n";
        return r;
    }
    std::string raiz = params[0];
    const std::string peerHost = params[1];
    const std::string portStr = params[2];
    const std::string token = params[3];
    bool borra = false;
    bool enSeco = false;
    // `--as`: con qué nombre aterriza un fichero suelto. Es el `cp a.txt otra:/dir/b.txt` de
    // toda la vida; sin él llegaba siempre como «a.txt».
    std::string comoLlega;
    for (std::size_t i = 4; i < params.size(); ++i) {
        if (params[i] == "--delete") {
            borra = true;
        } else if (params[i] == "--dry-run") {
            enSeco = true;
        } else if (params[i] == "--as" && i + 1 < params.size()) {
            comoLlega = params[++i];
        }
    }
    if (!comoLlega.empty()
        && (comoLlega.find_first_of("/\\") != std::string::npos || comoLlega == "."
            || comoLlega == "..")) {
        r.rc = 2;
        ponCodigo(r, "as_needs_name", {comoLlega});
        return r;
    }
    // La ruta en ESTA máquina del fichero suelto, cuando llega con otro nombre: el manifiesto
    // lleva el nombre de llegada y ya no sirve para abrirlo aquí.
    std::string ficheroSuelto;

    std::vector<AR::Entry> mias;
    std::vector<std::string> ilegibles;
    std::string err;
    // Un FICHERO suelto viaja como un árbol de una sola entrada que cuelga de su directorio:
    // el receptor, que está en el directorio donde aterriza, no nota la diferencia.
    // `--delete` no tiene sentido aquí —borraría todo lo demás del directorio de destino,
    // que no forma parte de lo que se manda— y se rechaza.
    std::error_code ecTipo;
    if (std::filesystem::is_regular_file(std::filesystem::symlink_status(raiz, ecTipo))
        && !ecTipo) {
        if (borra) {
            r.rc = 2;
            ponCodigo(r, "delete_single_file", {raiz});
            return r;
        }
        AR::Entry unico;
        if (!AR::walkSingleFile(raiz, unico, err)) {
            r.rc = 2;
            r.err = err + "\n";
            return r;
        }
        if (!comoLlega.empty()) {
            unico.path = comoLlega;
            ficheroSuelto = raiz;
        }
        mias.push_back(unico);
        raiz = std::filesystem::path(raiz).parent_path().string();
    } else if (!comoLlega.empty()) {
        r.rc = 2;
        ponCodigo(r, "as_single_file", {raiz});
        return r;
    } else if (!AR::walk(raiz, mias, err, false, &ilegibles)) {
        r.rc = 2;
        r.err = err + "\n";
        return r;
    } else {
        // La RAÍZ también viaja (ruta «.»), con su dueño, modo, fecha y atributos, como en la
        // copia dentro de una máquina: si no, el directorio de llegada se quedaba como lo creó
        // el agente —root:root 0755— aunque todo lo de dentro conservara lo suyo. El receptor
        // la pone en su manifiesto, así que solo se toca cuando difiere.
        AR::Entry propia;
        if (AR::describePath(raiz, propia, err) && propia.kind == AR::EntryKind::Directory) {
            propia.path = ".";
            mias.insert(mias.begin(), propia);
        }
    }

    const TransferSocket sock = conectaConElPar(peerHost, portStr);
    if (sock == kSocketInvalido) {
        ponFallo(r, 1, "peer_cannot_connect", {peerHost + ":" + portStr});
        return r;
    }
    const auto cortado = [&]() {
        closeTransferSocket(sock);
        r.rc = 1;
        ponCodigo(r, "tree_dropped", {});
        return r;
    };
    std::string nombres;
    {
        std::vector<std::string> usuarios;
        std::vector<std::string> grupos;
        AR::namesUsed(mias, usuarios, grupos);
        for (const std::string& n : usuarios) {
            nombres += "u " + std::to_string(n.size()) + "\n" + n;
        }
        for (const std::string& n : grupos) {
            nombres += "g " + std::to_string(n.size()) + "\n" + n;
        }
        nombres += "E\n";
    }
    if (!mandaTodo(sock, token + "\n") || !mandaTodo(sock, nombres)) {
        closeTransferSocket(sock);
        r.rc = 1;
        ponCodigo(r, "tree_greet_failed", {});
        return r;
    }

    // Qué sistema es el otro, y de ahí qué atributos y ACL puede guardar; y qué número tiene
    // allí cada nombre de dueño y de grupo.
    std::string saludo;
    if (!leeLinea(sock, saludo) || saludo.rfind("R ", 0) != 0) {
        return cortado();
    }
    AR::OwnerMap alli;
    for (;;) {
        std::string linea;
        if (!leeLinea(sock, linea)) {
            return cortado();
        }
        if (linea == "E") {
            break;
        }
        std::istringstream iss(linea);
        char cual = 0;
        std::uint32_t id = 0;
        std::size_t largo = 0;
        if (!(iss >> cual >> id >> largo) || largo > 1024) {
            return cortado();
        }
        std::string nombre(largo, '\0');
        if (largo > 0 && !leeExacto(sock, nombre.data(), largo)) {
            return cortado();
        }
        (cual == 'u' ? alli.users : alli.groups)[nombre] = id;
    }
    AR::expectAtReceiver(mias, alli);
    const zfsmgr::fsmeta::Family suFamilia = zfsmgr::fsmeta::familyFromName(trim(saludo.substr(2)));
    // Lo que no cabe allí no viaja: se cuenta y se avisa, en vez de perderlo en silencio o de
    // intentar ponerlo y fallar en cada pasada.
    std::size_t sinMetadatos = 0;
    for (AR::Entry& e : mias) {
        bool perdido = false;
        e.meta = zfsmgr::fsmeta::fitFor(e.meta, suFamilia, &perdido);
        e.metaDigest = zfsmgr::fsmeta::digestOf(e.meta);
        if (perdido && e.kind != AR::EntryKind::HardLink) {
            ++sinMetadatos;
        }
    }

    std::vector<AR::Entry> suyas;
    if (!leeManifiesto(sock, suyas, err)) {
        closeTransferSocket(sock);
        r.rc = 1;
        r.err = err + "\n";
        return r;
    }

    AR::Plan plan = AR::makePlan(mias, suyas, borra, ilegibles);

    // ¿De qué ficheros compensa pedir firmas? Los que el destino YA TIENE y son grandes.
    std::map<std::string, const AR::Entry*> suyasPorRuta;
    for (const AR::Entry& e : suyas) {
        suyasPorRuta.emplace(e.path, &e);
    }
    std::vector<std::string> pidoFirmas;
    if (!enSeco) {
        for (const AR::Operation& o : plan.operations) {
            if (o.action != AR::Action::Send || o.entry.size < AR::kMinSizeForDelta) {
                continue;
            }
            const auto it = suyasPorRuta.find(o.entry.path);
            if (it != suyasPorRuta.end() && it->second->kind == AR::EntryKind::File
                && it->second->size > 0) {
                pidoFirmas.push_back(o.entry.path);
            }
        }
    }
    for (const std::string& cual : pidoFirmas) {
        if (!mandaTodo(sock, "S " + std::to_string(cual.size()) + "\n") || !mandaTodo(sock, cual)) {
            return cortado();
        }
    }
    if (!mandaTodo(sock, "E\n")) {
        return cortado();
    }

    // Las firmas que conteste el destino, por ruta.
    std::map<std::string, std::pair<std::size_t, std::vector<AR::Signature>>> firmas;
    for (;;) {
        std::string linea;
        if (!leeLinea(sock, linea)) {
            return cortado();
        }
        if (linea == "E") {
            break;
        }
        std::istringstream iss(linea);
        char letra = 0;
        std::size_t tamBloque = 0;
        std::size_t bytes = 0;
        std::size_t lr = 0;
        if (!(iss >> letra >> tamBloque >> bytes >> lr) || letra != 'G') {
            return cortado();
        }
        std::string ruta(lr, '\0');
        std::string crudas(bytes, '\0');
        if ((lr > 0 && !leeExacto(sock, ruta.data(), lr))
            || (bytes > 0 && !leeExacto(sock, crudas.data(), bytes))) {
            return cortado();
        }
        std::vector<AR::Signature> fs;
        std::string errF;
        if (AR::parseSignatures(crudas, fs, errF) && tamBloque > 0) {
            firmas.emplace(ruta, std::make_pair(tamBloque, std::move(fs)));
        }
    }

    // La lista de cambios —las mismas marcas que la copia dentro de una máquina— y lo que no
    // se pudo planear: un directorio con contenido donde ahora va otra cosa, sin borrado, y
    // los directorios del origen que no se pudieron leer.
    std::string informe;
    std::size_t pendientes = 0;
    for (const AR::Operation& o : plan.operations) {
        const std::string l = AR::describe(o);
        if (!l.empty()) {
            informe += l + "\n";
            ++pendientes;
        }
    }
    // Lo que no se pudo planear, como código del catálogo: el primero, y cuántos más.
    std::string codigoProblema;
    std::vector<std::string> argsProblema;
    const auto conMas = [](const std::vector<std::string>& v) {
        std::string s = v.front().empty() ? std::string(".") : v.front();
        if (v.size() > 1) {
            s += " (+" + std::to_string(v.size() - 1) + ")";
        }
        return s;
    };
    if (!plan.refused.empty()) {
        codigoProblema = "tree_refused_dir";
        argsProblema = {conMas(plan.refused)};
    } else if (!ilegibles.empty()) {
        codigoProblema = "tree_unreadable_source";
        argsProblema = {conMas(ilegibles)};
    }
    const std::string lineaMetadatos =
        sinMetadatos > 0 ? "SIN_ACL_XATTR=" + std::to_string(sinMetadatos) + "\n" : std::string();

    if (enSeco) {
        // No se manda nada: se cierra con «E» para que el otro extremo no espere.
        (void)mandaTodo(sock, "E\n");
        closeTransferSocket(sock);
        r.rc = 0;
        if (!codigoProblema.empty()) {
            ponFallo(r, 1, codigoProblema, argsProblema);
        }
        r.out = informe + "PENDIENTES=" + std::to_string(pendientes)
                + "\nBYTES=" + std::to_string(plan.bytes)
                + "\nIGUALES=" + std::to_string(plan.unchanged) + "\n" + lineaMetadatos;
        return r;
    }

    const std::filesystem::path base(raiz);
    const auto rutaLocalDe = [&](const std::string& enManifiesto) {
        if (!ficheroSuelto.empty() && enManifiesto == comoLlega) {
            return ficheroSuelto;
        }
        return (base / std::filesystem::path(enManifiesto)).lexically_normal().string();
    };
    bool bien = true;
    std::uint64_t bytesLiteralesTotal = 0;
    std::uint64_t bytesAhorrados = 0;
    std::string errLectura;
    for (const AR::Operation& o : plan.operations) {
        // ¿Hay firmas de este fichero? Entonces va como PARCHE y no entero.
        const auto itF = (o.action == AR::Action::Send) ? firmas.find(o.entry.path) : firmas.end();
        if (itF != firmas.end()) {
            const std::size_t tamBloque = itF->second.first;
            std::vector<AR::Instruction> instrucciones;
            std::uint64_t literales = 0;
            std::string errD;
            std::string hashEntero;
            const std::string rutaLocal = rutaLocalDe(o.entry.path);
            if (AR::delta(rutaLocal, itF->second.second, tamBloque, instrucciones, literales, errD)
                && AR::fileHash(rutaLocal, hashEntero, errD)) {
                bytesLiteralesTotal += literales;
                bytesAhorrados += (o.entry.size > literales) ? (o.entry.size - literales) : 0;
                // La cabecera de siempre, con «P» y el hash del resultado como destino.
                AR::Operation parche = o;
                parche.entry.target = hashEntero;
                std::string cab = AR::headerOf(parche);
                cab[0] = 'P';
                bien = mandaTodo(sock, cab);
                for (const AR::Instruction& in : instrucciones) {
                    if (!bien) {
                        break;
                    }
                    if (in.kind == AR::InstructionKind::Send) {
                        bien = mandaTodo(sock, "C " + std::to_string(in.block) + " "
                                                   + std::to_string(in.howMany) + "\n");
                    } else {
                        bien = mandaTodo(sock, "L " + std::to_string(in.data.size()) + "\n")
                               && mandaTodo(sock, in.data);
                    }
                }
                if (bien) {
                    bien = mandaTodo(sock, "Z\n");
                }
                if (!bien) {
                    break;
                }
                continue;
            }
            // Si el delta no se pudo calcular, se manda entero: es lento, no es incorrecto.
        }
        if (!mandaTodo(sock, AR::headerOf(o))) {
            bien = false;
            break;
        }
        if (o.action != AR::Action::Send) {
            continue;
        }
        // El contenido por tramos: los huecos y los bloques de ceros viajan como una
        // longitud, no como bytes.
        bien = AR::readStretches(
            rutaLocalDe(o.entry.path), o.entry.size,
            [&](const char* datos, std::size_t n) {
                if (datos == nullptr) {
                    return mandaTodo(sock, "h " + std::to_string(n) + "\n");
                }
                return mandaTodo(sock, "d " + std::to_string(n) + "\n")
                       && mandaTodo(sock, datos, n);
            },
            errLectura);
        if (bien) {
            bien = mandaTodo(sock, "e\n");
        }
        if (!bien) {
            break;
        }
    }
    if (bien) {
        bien = mandaTodo(sock, "E\n");
    }
    if (!bien) {
        closeTransferSocket(sock);
        r.rc = 1;
        if (!errLectura.empty() && errLectura != "the connection dropped") {
            r.err = errLectura + "\n";
            return r;
        }
        ponCodigo(r, "tree_dropped", {});
        return r;
    }

    std::string respuesta;
    if (!leeLinea(sock, respuesta)) {
        respuesta = "ERR no answer from the receiver";
    }
    closeTransferSocket(sock);
    if (respuesta.rfind("PARCIAL ", 0) == 0) {
        // Aplicado a medias. NO se devuelve 0: quien lo lea tiene que enterarse de que su
        // árbol no quedó igual, aunque la mayoría de las operaciones sí entraran.
        r.rc = 1;
        r.out = informe;
        // «PARCIAL <hechas> <fallidas> <primer motivo>», dicho en una frase.
        std::istringstream iss(respuesta.substr(8));
        std::uint64_t hechas = 0;
        std::uint64_t fallidas = 0;
        std::string motivo;
        iss >> hechas >> fallidas;
        std::getline(iss, motivo);
        ponCodigo(r, "tree_partial",
                  {std::to_string(fallidas), std::to_string(hechas + fallidas), trim(motivo)});
        return r;
    }
    if (respuesta.rfind("OK ", 0) != 0) {
        r.rc = 1;
        r.err = trim(respuesta) + "\n";
        return r;
    }
    r.rc = 0;
    if (!codigoProblema.empty()) {
        ponFallo(r, 1, codigoProblema, argsProblema);
    }
    r.out = informe + "APLICADAS=" + respuesta.substr(3) + "\nBYTES=" + std::to_string(plan.bytes)
            + "\nIGUALES=" + std::to_string(plan.unchanged);
    if (bytesAhorrados > 0) {
        // Lo que el delta ha evitado mandar. Es el único número que dice si sirvió.
        r.out += "\nAHORRADOS=" + std::to_string(bytesAhorrados)
                 + "\nENVIADOS=" + std::to_string(bytesLiteralesTotal);
    }
    r.out += "\n" + lineaMetadatos;
    return r;
}

ExecResult runZfsRecvListenCapture(const std::string& dataset, bool force, bool waitInline) {
    ExecResult r;
    if (trim(dataset).empty()) {
        r.rc = 2;
        ponCodigo(r, "empty_dataset", {});
        return r;
    }
    const std::string token = generateTransferToken();
    if (token.empty()) {
        r.rc = 1;
        ponCodigo(r, "token_failed", {});
        return r;
    }

    TransferSocket listenFd{};
    int assignedPort = 0;
    {
        std::string errPuerto;
        if (!abrePuertoDeTransferencia(listenFd, assignedPort, errPuerto)) {
            r.rc = 1;
            r.err = errPuerto + "\n";
            return r;
        }
    }

    if (waitInline) {
        // El emisor necesita el puerto ANTES de que empiece la espera, así que se emite
        // y se vacía el búfer aquí mismo.
        std::cout << "PORT=" << assignedPort << "\nTOKEN=" << token << "\n" << std::flush;
        runTransferReceiveSession(listenFd, token, dataset, force);
        r.rc = 0;
        return r;
    }

    std::thread([listenFd, token, dataset, force]() {
        runTransferReceiveSession(listenFd, token, dataset, force);
    }).detach();

    r.rc = 0;
    r.out = "PORT=" + std::to_string(assignedPort) + "\nTOKEN=" + token + "\n";
    return r;
}

// Espera al emisor, comprueba el testigo y escribe el flujo en un fichero.
//
// Se escribe en «<ruta>.zfsmgr-part» y solo al final se le da su nombre: un fichero con el
// nombre pedido es SIEMPRE un flujo entero. Lo que decide si lo es es cómo cierra el emisor
// —un cierre normal si su `zfs send` acabó bien, abortado si no (ver cierraConRechazo)—, así
// que un `recv` que devuelve error deja el parcial fuera y lo borra.
void runFileReceiveSession(TransferSocket listenFd, const std::string& token,
                           const std::string& ruta, const zfsmgr::compresion::Eleccion& comp) {
    struct timeval tv{};
    tv.tv_sec = 300;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(listenFd, &rfds);
    const int sel = select(static_cast<int>(listenFd) + 1, &rfds, nullptr, nullptr, &tv);
    if (sel <= 0) {
        closeTransferSocket(listenFd);
        daemonLog("ERROR", "file-recv-listen " + ruta + ": the sender never connected");
        return;
    }
    const TransferSocket clientFd = aceptaTransferSocket(listenFd);
    closeTransferSocket(listenFd);
    if (clientFd == kSocketInvalido) {
        return;
    }
    std::string recvToken;
    recvToken.reserve(65);
    char ch = 0;
    while (recvToken.size() < 65) {
        const int n = recv(clientFd, &ch, 1, 0);
        if (n <= 0 || ch == '\n') {
            break;
        }
        recvToken.push_back(ch);
    }
    if (recvToken != token) {
        closeTransferSocket(clientFd);
        return;
    }

    const std::string parcial = ruta + ".zfsmgr-part";
    // Solo para root: el flujo lleva dentro los datos del dataset, sean de quien sean.
#ifndef _WIN32
    const int fd = ::open(parcial.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    const bool abierto = fd >= 0;
    const std::string errApertura = abierto ? std::string() : std::strerror(errno);
#else
    const HANDLE fh = CreateFileW(std::filesystem::path(parcial).wstring().c_str(), GENERIC_WRITE,
                                  0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool abierto = fh != INVALID_HANDLE_VALUE;
    const std::string errApertura =
        abierto ? std::string() : "error " + std::to_string(GetLastError());
#endif
    if (!abierto) {
        // Abortado: el emisor tiene que enterarse de que aquí no se guarda nada.
        informaAlEmisor(clientFd, 1, "cannot create " + parcial + ": " + errApertura);
        cierraConRechazo(clientFd);
        daemonLog("ERROR", "file-recv-listen: cannot create " + parcial + ": " + errApertura);
        return;
    }

    // `--comp`: se comprime aquí, al escribir, y el emisor de `zfs send` no se entera.
    std::string errCompresor;
    std::unique_ptr<zfsmgr::compresion::Compresor> compresor;
    if (comp.formato != zfsmgr::compresion::Formato::Ninguno) {
        compresor = zfsmgr::compresion::creaCompresor(comp, errCompresor);
    }
    std::uint64_t escritos = 0;
    // Todo lo que va al fichero pasa por aquí: sin comprimir, los bytes del socket; con
    // compresión, lo que devuelve el compresor.
    const auto escribe = [&](const char* datos, std::size_t n) -> bool {
        std::size_t hecho = 0;
        while (hecho < n) {
#ifndef _WIN32
            const ssize_t w = ::write(fd, datos + hecho, n - hecho);
            if (w < 0 && errno == EINTR) {
                continue;
            }
            if (w <= 0) {
                return false;
            }
            hecho += static_cast<std::size_t>(w);
#else
            DWORD w = 0;
            const DWORD ahora = static_cast<DWORD>(std::min<std::size_t>(n - hecho, 1u << 30));
            if (!WriteFile(fh, datos + hecho, ahora, &w, nullptr) || w == 0) {
                return false;
            }
            hecho += w;
#endif
        }
        escritos += n;
        return true;
    };

    std::vector<char> buf(256 * 1024);
    std::string comprimido;
    std::uint64_t movidos = 0;
    bool recepcionRota = false;
    bool escrituraRota = false;
    bool compresionRota = comp.formato != zfsmgr::compresion::Formato::Ninguno && !compresor;
    for (;;) {
        if (compresionRota) {
            break;
        }
        const int n = recv(clientFd, buf.data(), static_cast<int>(buf.size()), 0);
        if (n == 0) {
            break;
        }  // fin normal: el flujo está entero
        if (n < 0) {
#ifndef _WIN32
            if (errno == EINTR) {
                continue;
            }
#endif
            recepcionRota = true;
            break;
        }
        if (compresor) {
            comprimido.clear();
            if (!compresor->comprime(buf.data(), static_cast<std::size_t>(n), comprimido,
                                     errCompresor)) {
                compresionRota = true;
                break;
            }
            if (!escribe(comprimido.data(), comprimido.size())) {
                escrituraRota = true;
                break;
            }
        } else if (!escribe(buf.data(), static_cast<std::size_t>(n))) {
            escrituraRota = true;
            break;
        }
        movidos += static_cast<std::uint64_t>(n);
    }
    // El final del frame, solo si el flujo llegó entero: uno cortado no se cierra, se tira.
    if (compresor && !recepcionRota && !escrituraRota && !compresionRota) {
        comprimido.clear();
        if (!compresor->termina(comprimido, errCompresor)) {
            compresionRota = true;
        } else if (!escribe(comprimido.data(), comprimido.size())) {
            escrituraRota = true;
        }
    }
    std::string errEscritura = escrituraRota ? "write failed" : std::string();
    if (compresionRota) {
        escrituraRota = true;
        errEscritura = "compression failed: " + errCompresor;
    }
#ifndef _WIN32
    if (escrituraRota && !compresionRota) {
        errEscritura = std::string("write failed: ") + std::strerror(errno);
    }
    // A disco antes de darle el nombre: si no, un corte de luz justo después podría dejar
    // con el nombre bueno un fichero al que le falta el final.
    if (!escrituraRota && !recepcionRota && ::fsync(fd) != 0) {
        escrituraRota = true;
        errEscritura = std::string("fsync failed: ") + std::strerror(errno);
    }
    ::close(fd);
#else
    if (!escrituraRota && !recepcionRota && !FlushFileBuffers(fh)) {
        escrituraRota = true;
        errEscritura = "flush failed";
    }
    CloseHandle(fh);
#endif
    std::error_code ec;
    if (recepcionRota || escrituraRota) {
        std::filesystem::remove(parcial, ec);
        const std::string motivo =
            (recepcionRota ? std::string("the stream arrived incomplete") : errEscritura)
            + " after " + std::to_string(movidos) + " bytes; nothing was kept";
        daemonLog("ERROR", "file-recv-listen " + ruta + ": " + motivo);
        // Con la escritura rota, el emisor puede seguir mandando: se le dice por qué y se
        // corta, para que su envío dé error en vez de terminar como si se hubiera guardado.
        informaAlEmisor(clientFd, 1, "cannot save " + ruta + ": " + motivo);
        cierraConRechazo(clientFd);
        return;
    }
#ifndef _WIN32
    std::filesystem::rename(parcial, ruta, ec);
    const bool renombrado = !ec;
    const std::string errRenombrado = ec ? ec.message() : std::string();
#else
    // MoveFileEx y no std::filesystem::rename: aquí sí hay que reemplazar un fichero que ya
    // existe —se pidió sobrescribirlo—, y eso es lo que garantiza MOVEFILE_REPLACE_EXISTING.
    const bool renombrado = MoveFileExW(std::filesystem::path(parcial).wstring().c_str(),
                                        std::filesystem::path(ruta).wstring().c_str(),
                                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                            != 0;
    const std::string errRenombrado =
        renombrado ? std::string() : "error " + std::to_string(GetLastError());
#endif
    if (!renombrado) {
        std::filesystem::remove(parcial, ec);
        daemonLog("ERROR", "file-recv-listen: cannot rename " + parcial + " to " + ruta + ": "
                               + errRenombrado);
        informaAlEmisor(clientFd, 1,
                        "cannot rename " + parcial + " to " + ruta + ": " + errRenombrado);
        closeTransferSocket(clientFd);
        return;
    }
    daemonLog("INFO", "file-recv-listen " + ruta + ": " + std::to_string(movidos) + " bytes"
                          + (compresor ? " -> " + std::to_string(escritos) + " "
                                             + zfsmgr::compresion::textoDe(comp)
                                       : std::string()));
    informaAlEmisor(clientFd, 0, std::string());
    closeTransferSocket(clientFd);
}

// Abre un puerto para recibir un flujo de `zfs send` y guardarlo en `ruta`.
//
// Lo que se puede comprobar antes se comprueba aquí, y no al llegar el flujo: el directorio
// tiene que existir —crear directorios como root a petición de quien conecta no es cosa del
// receptor, igual que en el árbol de ficheros—, la ruta no puede ser un directorio, y un
// fichero que ya existe solo se reemplaza si se pide.
ExecResult runFileRecvListenCapture(const std::string& ruta, bool sobrescribir, bool waitInline,
                                    const std::string& compTexto) {
    ExecResult r;
    std::string err;
    if (!rutaDeFlujoValida(ruta, err)) {
        r.rc = 2;
        r.err = err + "\n";
        return r;
    }
    // La compresión se valida ANTES de abrir el puerto: con un formato mal escrito, el emisor
    // no tiene que llegar a conectar.
    zfsmgr::compresion::Eleccion comp;
    if (!compTexto.empty() && !zfsmgr::compresion::leeEleccion(compTexto, comp, err)) {
        r.rc = 2;
        r.err = err + "\n";
        return r;
    }
    std::error_code ec;
    const std::filesystem::path p(ruta);
    if (!std::filesystem::is_directory(p.parent_path(), ec)) {
        r.rc = 1;
        ponCodigo(r, "dir_not_exists", {p.parent_path().string()});
        return r;
    }
    if (std::filesystem::is_directory(p, ec)) {
        r.rc = 1;
        ponCodigo(r, "is_a_dir", {ruta});
        return r;
    }
    if (std::filesystem::exists(p, ec) && !sobrescribir) {
        r.rc = 1;
        ponCodigo(r, "file_exists", {ruta});
        return r;
    }
    if (std::filesystem::exists(p, ec) && !std::filesystem::is_regular_file(p, ec)) {
        r.rc = 1;
        ponCodigo(r, "not_regular_file", {ruta});
        return r;
    }
    const std::string token = generateTransferToken();
    if (token.empty()) {
        r.rc = 1;
        ponCodigo(r, "token_failed", {});
        return r;
    }
    TransferSocket listenFd{};
    int puerto = 0;
    if (!abrePuertoDeTransferencia(listenFd, puerto, err)) {
        r.rc = 1;
        r.err = err + "\n";
        return r;
    }
    if (waitInline) {
        std::cout << "PORT=" << puerto << "\nTOKEN=" << token << "\n" << std::flush;
        runFileReceiveSession(listenFd, token, ruta, comp);
        r.rc = 0;
        return r;
    }
    std::thread([listenFd, token, ruta, comp]() {
        runFileReceiveSession(listenFd, token, ruta, comp);
    }).detach();
    r.rc = 0;
    r.out = "PORT=" + std::to_string(puerto) + "\nTOKEN=" + token + "\n";
    return r;
}

// Conecta con el receptor y le entrega el flujo de `zfs send`.
//
// Portable desde la fase 2. La versión POSIX ya bombeaba —lee de una tubería y escribe
// al socket— en vez de colgar el socket de la salida del hijo, así que la estructura
// vale tal cual; lo único que cambiaba era crear el proceso y leer de la tubería, y eso
// vive ahora en spawnReadingStdout.
// Devuelve false para pedir que la transfer se interrumpa. Se llama en cada vuelta
// del relé, igual que la comprobación de cancelación que hacía el camino asíncrono.
ExecResult runZfsSendToPeerCapture(const std::vector<std::string>& params,
                                   const TransferProgressFn& onProgress,
                                   const std::string& ficheroOrigen) {
    ExecResult r;
    const bool desdeFichero = !ficheroOrigen.empty();
    // params: snap peer_host peer_port token [base_snap [send_flags]]
    if (params.size() < 4) {
        r.rc = 2;
        ponCodigo(r, "usage",
                  {"zfsmgr-agent --zfs-send-to-peer <snap> <peer_host> <peer_port> <token> "
                   "[<base_snap> [<send_flags>]]"});
        return r;
    }
    const std::string snap = params[0];
    const std::string peerHost = params[1];
    int peerPort = 0;
    try {
        peerPort = std::stoi(params[2]);
    } catch (...) {
    }
    const std::string token = params[3];
    const std::string baseSnap = (params.size() > 4 && !params[4].empty()) ? params[4] : "";
    const std::string flagsStr = params.size() > 5 ? params[5] : "";
    // Las banderas se comprueban AQUÍ, contra la lista de la capa base, y no solo en quien
    // llama: lo que se escribe abajo es el argv de un `zfs send` que corre con privilegios,
    // así que cualquier componente que no sea una bandera conocida —un `tank/otro@ayer`
    // suelto, por ejemplo— sacaría por el socket un dataset que nadie pidió.
    {
        std::string mala;
        if (!zfsmgr::base::zfsprops::areValidSendFlags(flagsStr, mala)) {
            r.rc = 2;
            ponFallo(r, 2, "send_flag_not_allowed", {mala});
            return r;
        }
    }
    // Séptimo parámetro opcional: el testigo de reanudación que devuelve el receptor
    // cuando una transfer se cortó a medias. Con él se emite `zfs send -t`, que
    // continúa desde donde se quedó en vez de mandarlo todo otra vez.
    //
    // Es un añadido al final a propósito: un agente anterior lo ignora y manda el flujo
    // completo, y entonces el receptor —que conserva el envío a medias— lo rechaza con
    // un error de ZFS. Es decir, falla de forma visible en lugar de estropear nada, así
    // que no hace falta cambiar la versión del esquema ni reinstalar agentes por esto.
    const std::string resumeToken = (params.size() > 6) ? trim(params[6]) : "";
    if (peerHost.empty() || peerPort <= 0 || token.size() != 64) {
        ponFallo(r, 2, "internal", {"invalid arguments for --zfs-send-to-peer"});
        return r;
    }
    if (snap.empty() && resumeToken.empty() && !desdeFichero) {
        ponFallo(r, 2, "internal",
                 {"invalid arguments for --zfs-send-to-peer: neither snapshot nor token"});
        return r;
    }
    // El fichero se abre ANTES de conectar: si no existe, el receptor no tiene por qué
    // quedarse esperando un flujo que no va a llegar.
    SpawnedStdout child;
    if (desdeFichero) {
        std::string errFichero;
        child = abreFicheroComoFlujo(ficheroOrigen, errFichero);
        if (!child.ok) {
            r.rc = 1;
            r.err = errFichero + "\n";
            return r;
        }
    }

    // Resolver, crear el socket sin herencia y conectar: lo hace `conectaConElPar`, que es
    // la misma secuencia que usa el emisor del árbol de ficheros. Estaba escrita dos veces,
    // y con dos copias un arreglo como el de la doble pila IPv6 entra en una y se olvida en
    // la otra. El plazo de envío y el CLOEXEC los pone también él.
    const std::string portStr = std::to_string(peerPort);
    const TransferSocket sockFd = conectaConElPar(peerHost, portStr);
    if (sockFd == kSocketInvalido) {
        if (desdeFichero) {
            waitSpawnedStdout(child);
        }
        ponFallo(r, 1, "peer_cannot_connect", {peerHost + ":" + portStr});
        return r;
    }

    // send() en vez de write(): vale para sockets en las dos plataformas, y en Windows
    // write() ni siquiera acepta un descriptor de socket.
    const std::string tokenLine = token + "\n";
    (void)send(sockFd, tokenLine.c_str(), static_cast<int>(tokenLine.size()), 0);

    std::vector<std::string> sendArgs = {"send"};
    if (!resumeToken.empty()) {
        // `zfs send -t` lleva dentro del testigo qué snapshot y desde dónde continuar,
        // así que no admite ni snapshot ni base ni banderas: ponerlas sería un error.
        sendArgs.push_back("-t");
        sendArgs.push_back(resumeToken);
    } else {
        if (!flagsStr.empty()) {
            std::istringstream iss(flagsStr);
            std::string tok;
            while (iss >> tok) {
                sendArgs.push_back(tok);
            }
        }
        if (!baseSnap.empty()) {
            sendArgs.push_back("-I");
            sendArgs.push_back(baseSnap);
        }
        sendArgs.push_back(snap);
    }

    if (!desdeFichero) {
        child = spawnReadingStdout("zfs", sendArgs);
    }
    if (!child.ok) {
        closeTransferSocket(sockFd);
        ponFallo(r, 1, "send_cannot_launch", {});
        return r;
    }

    std::uint64_t totalBytes = 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto lastReport = t0;
    std::vector<char> buf(65536);
    bool relayOk = true;
    bool atascado = false;
    auto ultimoAvance = t0;
    auto reportProgress = [&](bool done) {
        const auto now = std::chrono::steady_clock::now();
        const long elapsed =
            static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(now - t0).count());
        const double mib = static_cast<double>(totalBytes) / (1024.0 * 1024.0);
        const double rate = elapsed > 0 ? mib / static_cast<double>(elapsed) : mib;
        char line[256];
        snprintf(line, sizeof(line), "BYTES=%llu  %.1f MiB  @ %.1f MiB/s  elapsed %lds%s\n",
                 (unsigned long long)totalBytes, mib, rate, elapsed, done ? " [done]" : "");
        std::cout << line << std::flush;
        lastReport = now;
    };
    bool cancelled = false;
    bool lecturaFallida = false;
    // Un fichero de flujo COMPRIMIDO se descomprime aquí, antes de mandarlo: el `zfs recv`
    // del otro lado recibe el flujo crudo y no se entera. El formato se reconoce por los
    // primeros bytes, no por la extensión, que puede mentir; lo que no es gzip, lz4 ni zstd
    // se manda tal cual.
    std::unique_ptr<zfsmgr::compresion::Descompresor> descompresor;
    zfsmgr::compresion::Formato formatoFichero = zfsmgr::compresion::Formato::Ninguno;
    bool primerTrozo = true;
    std::string descomprimido;
    std::string motivoLectura;
    // El siguiente trozo que hay que mandar, ya descomprimido. 0 al acabar, negativo si falla
    // la lectura o el fichero comprimido está dañado o cortado (el motivo, en `motivoLectura`).
    const auto siguienteTrozo = [&](const char*& datos) -> long {
        for (;;) {
            const long leidos = readSpawnedStdout(child, buf.data(), buf.size());
            if (leidos < 0) {
                return leidos;
            }
            if (desdeFichero && primerTrozo) {
                primerTrozo = false;
                formatoFichero =
                    zfsmgr::compresion::detecta(reinterpret_cast<const unsigned char*>(buf.data()),
                                                static_cast<std::size_t>(leidos));
                if (formatoFichero != zfsmgr::compresion::Formato::Ninguno) {
                    std::string errDesc;
                    descompresor = zfsmgr::compresion::creaDescompresor(formatoFichero, errDesc);
                    if (!descompresor) {
                        motivoLectura = errDesc;
                        return -1;
                    }
                    daemonLog("INFO", "file-to-peer " + ficheroOrigen + ": "
                                          + zfsmgr::compresion::nombreDe(formatoFichero)
                                          + " file, decompressing");
                }
            }
            if (!descompresor) {
                datos = buf.data();
                return leidos;
            }
            if (leidos == 0) {
                // Fin del fichero: tiene que acabar justo al final de un frame. Si no, está
                // cortado, y mandarlo así daría al otro lado un flujo a medias.
                if (!descompresor->completo()) {
                    motivoLectura = std::string("the ")
                                    + zfsmgr::compresion::nombreDe(formatoFichero)
                                    + " file is cut short: it ends in the middle of a frame";
                    return -1;
                }
                return 0;
            }
            descomprimido.clear();
            std::string errDesc;
            if (!descompresor->descomprime(buf.data(), static_cast<std::size_t>(leidos),
                                           descomprimido, errDesc)) {
                motivoLectura = errDesc;
                return -1;
            }
            if (!descomprimido.empty()) {
                datos = descomprimido.data();
                return static_cast<long>(descomprimido.size());
            }
            // Este trozo no dio nada todavía (una cabecera, un bloque a medias): otro.
        }
    };
    for (;;) {
        const char* datos = nullptr;
        const long n = siguienteTrozo(datos);
        if (n < 0) {
            lecturaFallida = true;
        }
        if (n <= 0) {
            break;
        }
        long done = 0;
        while (done < n) {
            const int w =
                send(sockFd, datos + done, static_cast<int>(std::min<long>(n - done, 1L << 30)), 0);
            if (w > 0) {
                done += w;
                ultimoAvance = std::chrono::steady_clock::now();
                continue;
            }
            if (!envioSoloAgotoElPlazo()) {
                relayOk = false;
                break;
            }

            // Venció el plazo sin colocar un byte: el receptor no está leyendo. Aquí es
            // donde antes se perdía el control para siempre. Ahora se aprovecha para
            // atender la cancelación —que con el envío atascado NO llegaba nunca, porque
            // onProgress solo se llamaba tras escribir— y para contar cuánto lleva parado.
            const auto ahora = std::chrono::steady_clock::now();
            const long parado = static_cast<long>(
                std::chrono::duration_cast<std::chrono::seconds>(ahora - ultimoAvance).count());
            if (onProgress) {
                const long transcurrido = static_cast<long>(
                    std::chrono::duration_cast<std::chrono::seconds>(ahora - t0).count());
                const double mib = static_cast<double>(totalBytes) / (1024.0 * 1024.0);
                const double rate =
                    transcurrido > 0 ? mib / static_cast<double>(transcurrido) : mib;
                if (!onProgress(totalBytes, transcurrido, rate)) {
                    cancelled = true;
                    break;
                }
            }
            if (parado >= kAtascoMaximoS) {
                relayOk = false;
                atascado = true;
                break;
            }
        }
        if (!relayOk || cancelled) {
            break;
        }
        totalBytes += static_cast<std::uint64_t>(n);
        const auto now = std::chrono::steady_clock::now();
        const long elapsed =
            static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(now - t0).count());
        if (onProgress) {
            const double mib = static_cast<double>(totalBytes) / (1024.0 * 1024.0);
            const double rate = elapsed > 0 ? mib / static_cast<double>(elapsed) : mib;
            if (!onProgress(totalBytes, elapsed, rate)) {
                cancelled = true;
                break;
            }
        } else if (std::chrono::duration_cast<std::chrono::seconds>(now - lastReport).count()
                   >= 2) {
            reportProgress(false);
        }
    }
    // Si el relé no llegó a su fin —el otro extremo rechazó, se cortó la red, o alguien
    // canceló— hay que MATAR al emisor antes de esperarlo. Cerrar la tubería no basta:
    // `zfs send` puede estar dentro del núcleo sin escribir nada, y entonces el `waitpid`
    // de dentro de `waitSpawnedStdout` no vuelve nunca y el pool se queda retenido.
    //
    // Cuando el relé terminó bien no se toca: ahí el hijo ya ha salido solo, y mandarle una
    // señal sería pedirle que muera a algo que ya está muerto.
    if (!relayOk || cancelled || lecturaFallida) {
        matarSpawnedStdout(child);
    }
    std::string childErr;
    r.rc = waitSpawnedStdout(child, &childErr);

    // El socket se cierra DESPUÉS de saber cómo acabó el emisor, porque el cierre es lo que
    // le dice al receptor si el flujo está entero. Un cierre normal es su fin de datos; uno
    // abortado (RST) le llega como un error. Antes se cerraba siempre igual, así que un
    // `zfs send` que fallaba a la mitad se leía al otro lado como un flujo terminado: un
    // `zfs recv` lo rechazaba por su cuenta, pero un FICHERO de destino se habría quedado
    // con medio flujo dándolo por bueno.
    VeredictoDelReceptor veredicto;
    if (!relayOk && !atascado && !cancelled) {
        // El receptor cortó. Si fue porque `zfs recv` rechazó el flujo, antes de cerrar dejó
        // dicho por qué, y eso explica mucho más que «se cortó la conexión».
        veredicto = leeVeredicto(sockFd, nullptr, std::chrono::milliseconds(1500));
    }
    if (relayOk && !cancelled && !lecturaFallida && r.rc == 0) {
        veredicto = esperaVeredicto(sockFd, [&]() {
            if (!onProgress) {
                return true;
            }
            const long transcurrido =
                static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::steady_clock::now() - t0)
                                      .count());
            const double mib = static_cast<double>(totalBytes) / (1024.0 * 1024.0);
            const bool sigue =
                onProgress(totalBytes, transcurrido,
                           transcurrido > 0 ? mib / static_cast<double>(transcurrido) : mib);
            if (!sigue) {
                cancelled = true;
            }
            return sigue;
        });
        closeTransferSocket(sockFd);
    } else {
        cierraConRechazo(sockFd);
    }
    if (!onProgress) {
        reportProgress(true);
    }
    if (cancelled) {
        // Cancelar no es fallar: quien pidió parar ya sabe por qué, y `zfs send` sale
        // con error simplemente porque se le cerró la tubería debajo.
        r.rc = 0;
        r.out = "CANCELLED=1\n";
        return r;
    }
    // El orden importa. Cuando el relé falla se MATA al `zfs send`, así que su código de
    // salida pasa a ser 143 (SIGTERM) — un código que hemos provocado nosotros. Mirarlo
    // primero hacía que el trabajo se apuntase como «zfs send failed (exit 143)», que manda
    // a investigar el emisor cuando el problema estaba en el receptor. La causa que se
    // conoce gana a la consecuencia.
    if (atascado) {
        // Distinto de un fallo de escritura: aquí la conexión NUNCA se rompió, sencillamente
        // el otro extremo dejó de leer. Decirlo con esas palabras evita que se busque el
        // problema en la red, que es donde no está.
        ponFallo(r, 1, "receiver_stalled", {std::to_string(kAtascoMaximoS)});
    } else if (!relayOk && veredicto.hayVeredicto && veredicto.rc != 0) {
        ponFallo(
            r, 1, "receiver_failed",
            {veredicto.motivo.empty() ? "exit " + std::to_string(veredicto.rc) : veredicto.motivo});
    } else if (!relayOk) {
        ponFallo(r, 1, "receiver_dropped", {});
    } else if (lecturaFallida) {
        ponFallo(r, 1, "stream_file_unreadable", {ficheroOrigen, motivoLectura});
    } else if (r.rc != 0) {
        ponFallo(r, r.rc, "zfs_send_failed", {std::to_string(r.rc), trim(childErr)});
    } else if (veredicto.hayVeredicto && veredicto.rc != 0) {
        // Todo salió de aquí, y el otro lado dice que no lo pudo usar.
        ponFallo(
            r, 1, "receiver_failed",
            {veredicto.motivo.empty() ? "exit " + std::to_string(veredicto.rc) : veredicto.motivo});
    } else if (!veredicto.hayVeredicto) {
        ponFallo(r, 1, "receiver_no_verdict", {});
    }
    return r;
}

// El mismo relé, en segundo plano y volcando el avance en g_jobs.
//
// Era una copia casi literal de runZfsSendToPeerCapture: resolver, conectar, lanzar
// `zfs send`, bombear, informar. Ahora lo reutiliza. Eso quita ~130 líneas duplicadas,
// y sobre todo quita la posibilidad de arreglar un fallo del relé en una sola de las
// dos copias, que es como se acumulan las diferencias silenciosas entre caminos.
//
// De paso funciona en Windows, porque el relé ya es portable desde la fase 2.
// Los dispositivos de bloque de esta máquina, para poder elegir dónde crear un pool.
//
// Existe porque hasta ahora esto era una TUBERÍA DE SHELL que el cliente enviaba a la otra
// máquina —`lsblk`/`diskutil`/`awk` encadenados, ochenta líneas en
// `mainwindow_pool_create.cpp`—. Además de ser justo lo que se está quitando, no funciona
// en Windows y no había forma de pedirlo desde el CLI.
//
// Devuelve SIEMPRE la misma forma, venga de donde venga, para que quien lo lea no tenga
// que saber en qué sistema corre:
//
//   {"devices":[{"path":…,"size":<bytes>,"fstype":…,"mountpoint":…,"type":…,"inuse":bool}]}
//
// `inuse` es una comodidad, no un veredicto: dice que el dispositivo o alguno de sus hijos
// tiene sistema de ficheros o está montado. Quien vaya a escribir en él decide.
ExecResult runDumpBlockDevicesCapture() {
    ExecResult r;
#if defined(__linux__)
    // `-J` da JSON y `-b` tamaños en bytes: nada que analizar a mano ni unidades que
    // interpretar. Es un árbol —los discos llevan sus particiones dentro—, y se aplana
    // conservando de quién cuelga cada uno.
    ExecResult l =
        runExecCapture("lsblk", {"-J", "-b", "-o", "NAME,PATH,SIZE,FSTYPE,MOUNTPOINT,TYPE"});
    if (l.rc != 0) {
        r.rc = l.rc;
        r.err = l.err.empty() ? std::string("lsblk not available\n") : l.err;
        return r;
    }
    zfsmgr::base::json::Value raiz;
    std::string errJson;
    if (!zfsmgr::base::json::parse(l.out, raiz, &errJson)) {
        r.rc = 1;
        ponCodigo(r, "lsblk_unreadable", {errJson});
        return r;
    }
    zfsmgr::base::json::Array salida;
    // Un dispositivo está en uso si él o CUALQUIER descendiente lo está: un disco con una
    // partición montada no es candidato para un pool nuevo, aunque el disco en sí no tenga
    // sistema de ficheros.
    std::function<bool(const zfsmgr::base::json::Value&)> ocupado =
        [&ocupado](const zfsmgr::base::json::Value& d) -> bool {
        if (!d["fstype"].toString().empty() || !d["mountpoint"].toString().empty()) {
            return true;
        }
        for (const auto& h : d["children"].toArray()) {
            if (ocupado(h)) {
                return true;
            }
        }
        return false;
    };
    std::function<void(const zfsmgr::base::json::Value&, const std::string&)> recorre =
        [&](const zfsmgr::base::json::Value& d, const std::string& padre) {
            zfsmgr::base::json::Value fila;
            fila.set("path", zfsmgr::base::json::Value(d["path"].toString()));
            fila.set("size", zfsmgr::base::json::Value(d["size"].toInt()));
            fila.set("fstype", zfsmgr::base::json::Value(d["fstype"].toString()));
            fila.set("mountpoint", zfsmgr::base::json::Value(d["mountpoint"].toString()));
            fila.set("type", zfsmgr::base::json::Value(d["type"].toString()));
            fila.set("parent", zfsmgr::base::json::Value(padre));
            fila.set("inuse", zfsmgr::base::json::Value(ocupado(d)));
            salida.push_back(fila);
            for (const auto& h : d["children"].toArray()) {
                recorre(h, d["path"].toString());
            }
        };
    for (const auto& d : raiz["blockdevices"].toArray()) {
        // Los `loop` son imágenes montadas, no discos donde crear un pool. Se dejan fuera
        // aquí y no en el cliente: si no, cada cliente tendría que saberlo por su cuenta.
        if (d["type"].toString() == "loop") {
            continue;
        }
        recorre(d, std::string());
    }
    // Los alias por identificador, y su destino real.
    //
    // **No es un adorno**: un pool creado con `/dev/sdb` se rompe si mañana el kernel llama
    // `sdc` a ese disco, y con el alias no. Por eso el diálogo los ofrece, y por eso el verbo
    // tiene que darlos: sin ellos, cambiarlo por el verbo habría sido una regresión callada.
    //
    // Van como entradas aparte, con `resolved` apuntando al dispositivo de verdad, que es lo
    // que ya esperaba quien los lee.
    for (const char* dir : {"/dev/disk/by-id"}) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(
                 dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) {
                break;
            }
            std::error_code rc2;
            const std::filesystem::path destino = std::filesystem::canonical(e.path(), rc2);
            if (rc2) {
                continue;
            }
            zfsmgr::base::json::Value v;
            v.set("path", zfsmgr::base::json::Value(e.path().string()));
            v.set("resolved", zfsmgr::base::json::Value(destino.string()));
            v.set("alias", zfsmgr::base::json::Value(true));
            salida.push_back(v);
        }
    }

    zfsmgr::base::json::Value raizSalida;
    raizSalida.set("devices", zfsmgr::base::json::Value(std::move(salida)));
    r.rc = 0;
    r.out = zfsmgr::base::json::toCompact(raizSalida) + "\n";
    return r;
#elif defined(__APPLE__)
    // macOS no tiene `lsblk`. `diskutil list` da los identificadores y `diskutil info` los
    // datos de cada uno en líneas «clave: valor», que es lo más estable que ofrece: la salida
    // de `diskutil list` a secas es una tabla con columnas alineadas y nombres de volumen que
    // pueden llevar espacios, así que analizarla entera sería adivinar dónde acaba cada campo.
    //
    // Se piden los bytes EXACTOS, no el «494.4 GB»: la cifra redondeada no sirve para decidir
    // si un disco cabe en un pool, y reconvertirla es inventarse precisión que no había.
    ExecResult lista = runExecCapture("diskutil", {"list"});
    if (lista.rc != 0) {
        r.rc = lista.rc;
        r.err = lista.err.empty() ? std::string("diskutil not available\n") : lista.err;
        return r;
    }
    std::vector<std::string> ids;
    for (const std::string& linea : splitLines(lista.out)) {
        // El identificador es el último campo, y tiene forma «diskN» o «diskNsM». Filtrar por
        // la FORMA y no por la posición es lo que hace que un nombre de volumen con espacios
        // dentro no descoloque el análisis.
        std::string ultimo;
        {
            std::istringstream iss(linea);
            std::string tok;
            while (iss >> tok) {
                ultimo = tok;
            }
        }
        if (ultimo.rfind("disk", 0) != 0) {
            continue;
        }
        bool forma = ultimo.size() > 4;
        for (std::size_t i = 4; i < ultimo.size() && forma; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(ultimo[i])) && ultimo[i] != 's') {
                forma = false;
            }
        }
        if (forma && std::find(ids.begin(), ids.end(), ultimo) == ids.end()) {
            ids.push_back(ultimo);
        }
    }

    struct Disp {
        std::string ruta;
        std::string padre;
        std::string tipo;
        std::string fs;
        std::string montaje;
        unsigned long long tamano{0};
        bool enUso{false};
    };
    std::vector<Disp> disp;
    for (const std::string& id : ids) {
        const ExecResult info = runExecCapture("diskutil", {"info", id});
        if (info.rc != 0) {
            continue;
        }
        Disp d;
        d.ruta = "/dev/" + id;
        bool entero = false;
        bool montado = false;
        std::string tipoParticion;
        std::string contenido;
        for (const std::string& linea : splitLines(info.out)) {
            const std::size_t dosp = linea.find(':');
            if (dosp == std::string::npos) {
                continue;
            }
            const std::string clave = trim(linea.substr(0, dosp));
            const std::string valor = trim(linea.substr(dosp + 1));
            if (clave == "Whole") {
                entero = isTruthyValue(valor) || valor == "Yes";
            } else if (clave == "Part of Whole" && !valor.empty()) {
                d.padre = "/dev/" + valor;
            } else if (clave == "Mount Point") {
                d.montaje = valor;
            } else if (clave == "File System Personality") {
                // «Not applicable (no file system)» no es un sistema de ficheros.
                d.fs = valor.rfind("Not applicable", 0) == 0 ? std::string() : valor;
            } else if (clave == "Mounted") {
                montado = isTruthyValue(valor) || valor == "Yes";
            } else if (clave == "Content (IOContent)") {
                // Para un disco ENTERO no hay «Partition Type»: lo que dice si tiene algo
                // encima es su contenido. Un esquema de particiones ya es algo.
                contenido = valor;
            } else if (clave == "Partition Type") {
                // **Esto es lo que salva el disco de arranque.** Una partición que aloja un
                // contenedor APFS no tiene punto de montaje ni sistema de ficheros propios
                // —`diskutil` contesta «Not applicable»—, así que con la regla de «montado o
                // con fs» salía como LIBRE. Y su disco entero también, por herencia. Ofrecerlo
                // para un pool nuevo era ofrecer borrar el arranque del equipo. Comprobado en
                // mmela: disk0s2 es «Apple_APFS» y así aparecía.
                //
                // Cualquier tipo de partición que no sea espacio libre significa que ahí hay
                // algo, aunque este mandado no sepa qué.
                tipoParticion = valor;
            } else if (clave == "Disk Size") {
                // «494.4 GB (494384795648 Bytes) (exactly …)»: los de dentro del primer
                // paréntesis son los buenos.
                const std::size_t ab = valor.find('(');
                const std::size_t by = valor.find(" Bytes");
                if (ab != std::string::npos && by != std::string::npos && by > ab) {
                    d.tamano =
                        std::strtoull(valor.substr(ab + 1, by - ab - 1).c_str(), nullptr, 10);
                }
            }
        }
        d.tipo = entero ? "disk" : "part";
        if (entero) {
            d.padre.clear();
        }
        // Montado es estar en uso. Y tener sistema de ficheros encima también: un volumen
        // desmontado con datos dentro no es un candidato libre para un pool nuevo.
        auto significaAlgo = [](const std::string& v) {
            return !v.empty() && v != "Empty" && v != "None" && v != "-"
                   && v.rfind("Not applicable", 0) != 0;
        };
        d.enUso = montado || !d.montaje.empty() || !d.fs.empty() || significaAlgo(tipoParticion)
                  || significaAlgo(contenido);
        disp.push_back(d);
    }
    // **Y lo que diga ZFS manda.** Un disco que respalda un pool importado puede no tener
    // sistema de ficheros, ni montaje, ni tipo de partición: `diskutil` de él solo dice que
    // es un disco. Comprobado en mmela con el que respalda `mpool`: salía como LIBRE, y
    // ofrecerlo para un pool nuevo era ofrecer destruir el que ya estaba encima.
    //
    // A ZFS se le pregunta por sus vdev y se marcan todos. Es la respuesta autoritativa: no
    // hay heurística de nombres ni de contenido que la iguale.
    {
        const ExecResult est = runExecCapture("zpool", {"status", "-P"});
        if (est.rc == 0) {
            for (Disp& d2 : disp) {
                if (!d2.enUso && est.out.find(d2.ruta) != std::string::npos) {
                    d2.enUso = true;
                }
            }
        }
    }

    // Un disco entero está en uso si CUALQUIER parte suya lo está. Sin esto, un disco con una
    // partición montada aparecería como libre y ofrecerlo para un pool nuevo sería ofrecer
    // borrar lo que hay dentro.
    for (Disp& d : disp) {
        if (d.tipo != "disk") {
            continue;
        }
        for (const Disp& hija : disp) {
            if (hija.padre == d.ruta && hija.enUso) {
                d.enUso = true;
                break;
            }
        }
    }

    zfsmgr::base::json::Array salida;
    for (const Disp& d : disp) {
        zfsmgr::base::json::Value v;
        v.set("path", zfsmgr::base::json::Value(d.ruta));
        v.set("size", zfsmgr::base::json::Value(static_cast<double>(d.tamano)));
        v.set("fstype", zfsmgr::base::json::Value(d.fs == "-" ? std::string() : d.fs));
        v.set("mountpoint", zfsmgr::base::json::Value(d.montaje));
        v.set("type", zfsmgr::base::json::Value(d.tipo));
        v.set("parent", zfsmgr::base::json::Value(d.padre));
        v.set("inuse", zfsmgr::base::json::Value(d.enUso));
        salida.push_back(v);
    }
    // Los alias por identificador, igual que en Linux pero donde los pone macOS. Ver el
    // comentario de allí: un pool creado con el nombre del dispositivo se rompe si mañana
    // cambia, y con el alias no.
    for (const char* dir : {"/var/run/disk/by-id", "/private/var/run/disk/by-id"}) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(
                 dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) {
                break;
            }
            std::error_code rc2;
            const std::filesystem::path destino = std::filesystem::canonical(e.path(), rc2);
            if (rc2) {
                continue;
            }
            zfsmgr::base::json::Value v;
            v.set("path", zfsmgr::base::json::Value(e.path().string()));
            v.set("resolved", zfsmgr::base::json::Value(destino.string()));
            v.set("alias", zfsmgr::base::json::Value(true));
            salida.push_back(v);
        }
    }

    zfsmgr::base::json::Value raizSalida;
    raizSalida.set("devices", zfsmgr::base::json::Value(std::move(salida)));
    r.rc = 0;
    r.out = zfsmgr::base::json::toCompact(raizSalida) + "\n";
    return r;
#elif defined(_WIN32)
    // Windows no tiene `lsblk` ni `diskutil`: su herramienta de consulta de discos es
    // PowerShell. Se le llama con `execvp` y un guion CONSTANTE —no se interpola nada que
    // venga de quien pide—, igual que en macOS se llama a `diskutil`. La regla que importa
    // sigue en pie: quien elige la ruta no elige además lo que se ejecuta.
    static const char* kGuion =
        "$ErrorActionPreference='SilentlyContinue'\n"
        "foreach ($d in Get-Disk) {\n"
        "  Write-Output (\"D`t\" + $d.Number + \"`t\" + $d.Size + \"`t\" + $d.PartitionStyle)\n"
        "}\n"
        "foreach ($p in Get-Partition) {\n"
        "  $fs = ''\n"
        "  $mp = ''\n"
        "  if ($p.DriveLetter) { $mp = ([string]$p.DriveLetter + ':\\') }\n"
        "  $v = Get-Volume -Partition $p\n"
        "  if ($v -and $v.FileSystem) { $fs = [string]$v.FileSystem }\n"
        "  Write-Output (\"P`t\" + $p.DiskNumber + \"`t\" + $p.PartitionNumber + \"`t\""
        " + $p.Size + \"`t\" + $fs + \"`t\" + $mp + \"`t\" + $p.Type)\n"
        "}\n";
    // **Con la ruta absoluta.** El agente corre como tarea programada en la sesión de
    // servicios, y allí `powershell` no está en el PATH: la primera versión de esto contestó
    // «cannot start powershell» contra OldLau. Se prueban las rutas conocidas y, como último
    // recurso, el nombre pelado por si alguien lo tiene en el PATH.
    std::vector<std::string> candidatos;
    if (const char* raiz = std::getenv("SystemRoot")) {
        candidatos.push_back(std::string(raiz)
                             + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
    }
    candidatos.push_back("C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
    candidatos.push_back("powershell");
    ExecResult ps;
    ps.rc = 1;
    for (const std::string& exe : candidatos) {
        ps = runExecCapture(exe, {"-NoProfile", "-NonInteractive", "-Command", kGuion});
        if (ps.rc == 0) {
            break;
        }
    }
    if (ps.rc != 0) {
        r.rc = ps.rc;
        r.err = ps.err.empty() ? std::string("could not query the disks\n") : ps.err;
        return r;
    }

    struct Disp {
        std::string ruta;
        std::string padre;
        std::string tipo;
        std::string fs;
        std::string montaje;
        unsigned long long tamano{0};
        bool enUso{false};
        int disco{-1};
    };
    std::vector<Disp> disp;
    for (const std::string& linea : splitLines(ps.out)) {
        std::vector<std::string> c;
        {
            std::string campo;
            for (const char ch : linea) {
                if (ch == '\t') {
                    c.push_back(campo);
                    campo.clear();
                } else if (ch != '\r') {
                    campo.push_back(ch);
                }
            }
            c.push_back(campo);
        }
        if (c.empty()) {
            continue;
        }
        if (c[0] == "D" && c.size() >= 4) {
            Disp d;
            d.disco = std::atoi(c[1].c_str());
            d.ruta = "\\\\.\\PhysicalDrive" + c[1];
            d.tipo = "disk";
            d.tamano = std::strtoull(c[2].c_str(), nullptr, 10);
            // `RAW` es «sin esquema de particiones». No significa libre —un disco entero
            // entregado a ZFS sale RAW— pero sí que no hay particiones que mirar.
            d.enUso = (c[3] != "RAW" && !c[3].empty());
            disp.push_back(d);
        } else if (c[0] == "P" && c.size() >= 7) {
            Disp d;
            d.disco = std::atoi(c[1].c_str());
            // `\\?\Harddisk<N>Partition<M>`, que es el nombre que ZFS usa para sus vdev en
            // Windows —lo que sale en `zpool status`— y el ÚNICO que `zpool create` acepta.
            //
            // Aquí se componía `\\.\PhysicalDrive<N>\Partition<M>` «para no cambiar lo que se
            // le pasa después a zpool create». Ese formato zpool lo RECHAZA:
            //
            //     wosix_open(\\.\PhysicalDrive1\Partition1): error 31 / 0x1f
            //     cannot open '\\.\PhysicalDrive1\Partition1': Invalid argument
            //
            // Comprobado contra OldLau el 2026-08-25: con este otro formato el pool se crea.
            // El disco entero sigue siendo `\\.\PhysicalDrive<N>`, que sí vale.
            d.ruta = "\\\\?\\Harddisk" + c[1] + "Partition" + c[2];
            d.padre = "\\\\.\\PhysicalDrive" + c[1];
            d.tipo = "part";
            d.tamano = std::strtoull(c[3].c_str(), nullptr, 10);
            d.fs = c[4];
            d.montaje = c[5];
            // Una partición con letra, con sistema de ficheros o con un tipo que no sea
            // desconocido tiene algo dentro. «System», «Reserved» y «Recovery» son del
            // arranque: ofrecerlas para un pool nuevo sería ofrecer romper el equipo.
            d.enUso = !d.montaje.empty() || !d.fs.empty() || (!c[6].empty() && c[6] != "Unknown");
            disp.push_back(d);
        }
    }

    // **El cruce con ZFS necesita traducir.** En Windows los vdev no se llaman como los
    // dispositivos: `zpool status -P` de un pool sobre el disco 2 dice
    // «/dev/Harddisk2Partition0», no «\\.\PhysicalDrive2». Comprobado contra OldLau. Buscar
    // la ruta tal cual —que es lo que hace la rama de macOS— no encontraría nada, y el disco
    // que respalda un pool importado aparecería libre.
    {
        const ExecResult est = runExecCapture("zpool", {"status", "-P"});
        if (est.rc == 0) {
            std::set<int> ocupados;
            const std::string marca = "Harddisk";
            std::size_t pos = est.out.find(marca);
            while (pos != std::string::npos) {
                std::size_t i = pos + marca.size();
                std::string num;
                while (i < est.out.size() && std::isdigit(static_cast<unsigned char>(est.out[i]))) {
                    num.push_back(est.out[i]);
                    ++i;
                }
                if (!num.empty()) {
                    ocupados.insert(std::atoi(num.c_str()));
                }
                pos = est.out.find(marca, i);
            }
            for (Disp& d : disp) {
                if (d.disco >= 0 && ocupados.count(d.disco) > 0) {
                    d.enUso = true;
                }
            }
        }
    }

    // Un disco entero está en uso si CUALQUIER partición suya lo está.
    for (Disp& d : disp) {
        if (d.tipo != "disk") {
            continue;
        }
        for (const Disp& hija : disp) {
            if (hija.padre == d.ruta && hija.enUso) {
                d.enUso = true;
                break;
            }
        }
    }

    zfsmgr::base::json::Array salida;
    for (const Disp& d : disp) {
        zfsmgr::base::json::Value v;
        v.set("path", zfsmgr::base::json::Value(d.ruta));
        v.set("size", zfsmgr::base::json::Value(static_cast<double>(d.tamano)));
        v.set("fstype", zfsmgr::base::json::Value(d.fs));
        v.set("mountpoint", zfsmgr::base::json::Value(d.montaje));
        v.set("type", zfsmgr::base::json::Value(d.tipo));
        v.set("parent", zfsmgr::base::json::Value(d.padre));
        v.set("inuse", zfsmgr::base::json::Value(d.enUso));
        salida.push_back(v);
    }
    zfsmgr::base::json::Value raizSalida;
    raizSalida.set("devices", zfsmgr::base::json::Value(std::move(salida)));
    r.rc = 0;
    r.out = zfsmgr::base::json::toCompact(raizSalida) + "\n";
    return r;
#else
    // Se dice que no está portado en vez de devolver una lista vacía, que se leería como
    // «esta máquina no tiene discos».
    r.rc = 2;
    ponCodigo(r, "devices_not_ported", {});
    return r;
#endif
}

void runZfsSendToPeerAsync(const std::string& jobId) {
    std::string snap, peerHost, baseSnap, sendFlags, token, resumeToken;
    int peerPort = 0;
    bool desdeFichero = false;
    {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        auto it = g_jobs.find(jobId);
        if (it == g_jobs.end()) {
            return;
        }
        desdeFichero = it->second.type == "file-to-peer";
        snap = it->second.snap;
        peerHost = it->second.peerHost;
        peerPort = it->second.peerPort;
        token = it->second.token;
        baseSnap = it->second.baseSnap;
        sendFlags = it->second.sendFlags;
        resumeToken = it->second.resumeToken;
    }

    // Avance y cancelación. Devolver false interrumpe el relé: antes se hacía matando
    // el proceso por su PID, que solo vale en Unix; cerrar la tubería es equivalente y
    // sirve en las dos plataformas.
    auto onProgress = [&jobId](std::uint64_t bytes, long elapsed, double rate) -> bool {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        auto it = g_jobs.find(jobId);
        if (it == g_jobs.end()) {
            return false;
        }
        if (it->second.state == JobState::Cancelled) {
            return false;
        }
        // El CONTADOR se actualiza siempre; lo que se limita es la línea de REGISTRO.
        //
        // Estaban juntos dentro del «solo si cambió el segundo», y eso hacía que una
        // transferencia terminada en menos de un segundo dejara `bytesTransferred` en cero
        // —el segundo nunca llegaba a cambiar—. Con la regla que marca fallido un envío
        // completo sin un solo byte, TODA transferencia rápida se declaraba fallida aunque
        // hubiera llegado entera. No se vio con 3,35 GiB, que tardan minuto y medio; se ve
        // con cualquier cosa pequeña, que es justo lo que se prueba primero.
        it->second.bytesTransferred = bytes;
        it->second.rateMiBs = rate;
        // Se informa cada dos segundos: escribir una línea por cada 64 KiB llenaría el
        // registro y no diría nada nuevo.
        if (elapsed != it->second.elapsedSecs) {
            char line[256];
            snprintf(line, sizeof(line), "BYTES=%llu  %.1f MiB  @ %.1f MiB/s  elapsed %lds",
                     (unsigned long long)bytes, static_cast<double>(bytes) / (1024.0 * 1024.0),
                     rate, elapsed);
            it->second.elapsedSecs = elapsed;
            it->second.progressLines.push_back(line);
            if (it->second.progressLines.size() > 5) {
                it->second.progressLines.erase(it->second.progressLines.begin());
            }
        }
        return true;
    };

    // Desde un fichero, `snap` es su ruta: lo que se manda es el flujo ya volcado.
    const std::vector<std::string> params =
        desdeFichero
            ? std::vector<std::string>{std::string(), peerHost, std::to_string(peerPort), token}
            : std::vector<std::string>{snap,       peerHost, std::to_string(peerPort),
                                       token,      baseSnap, sendFlags,
                                       resumeToken};
    const ExecResult r =
        runZfsSendToPeerCapture(params, onProgress, desdeFichero ? snap : std::string());

    std::lock_guard<std::mutex> lock(g_jobsMutex);
    auto it = g_jobs.find(jobId);
    if (it == g_jobs.end()) {
        return;
    }
    if (it->second.state == JobState::Cancelled) {
        it->second.finishedAtUtc = utcNowIsoString();
        persistJobsLocked();
        daemonLog("INFO", "job " + jobId + " cancelado");
        return;
    }
    // Salir con 0 no basta para decir que se copió algo. Un envío COMPLETO que no movió
    // un solo byte no ha copiado nada, y anotarlo como hecho es indistinguible de una
    // transferencia que funcionó. Pasó el 2026-08-25: el receptor se cayó nada más
    // empezar y este trabajo quedó como «done, rc=0, bytes=0» en un segundo.
    const bool vacioSinSerlo = (r.rc == 0)
                               && zfsmgr::base::transfer::fullSendMovedNothing(
                                   it->second.bytesTransferred, baseSnap, resumeToken);
    if (r.rc == 0 && !vacioSinSerlo) {
        it->second.state = JobState::Done;
    } else {
        it->second.state = JobState::Failed;
        if (vacioSinSerlo) {
            ponErrorDeTrabajo(it->second, "send_moved_nothing", {});
        } else if (!r.code.empty()) {
            ponErrorDeTrabajo(it->second, r.code, r.args);
        } else {
            ponErrorDeTrabajo(it->second, "", {trim(r.err)});
        }
    }
    it->second.finishedAtUtc = utcNowIsoString();
    persistJobsLocked();
    daemonLog((r.rc == 0 && !vacioSinSerlo) ? "INFO" : "ERROR",
              "job " + jobId + " finished rc=" + std::to_string(r.rc)
                  + " bytes=" + std::to_string(it->second.bytesTransferred)
                  + (vacioSinSerlo ? " (complete without moving a byte: marked as failed)" : ""));
}

// La ruta real de un dispositivo tal y como lo nombra `zpool`: «sda1», un PARTUUID
// («b9333c91-…»), «media-…» en macOS, «gpt/etiqueta» en FreeBSD… Se busca en los
// directorios donde cada sistema pone esos nombres y se resuelven los enlaces. Vacío si no
// está en esta máquina.
std::string resolveDeviceName(const std::string& nombre) {
    if (nombre.empty() || nombre.find("..") != std::string::npos) {
        return {};
    }
    std::vector<std::string> cands;
    if (nombre.front() == '/') {
        cands.push_back(nombre);
    } else {
#if defined(__APPLE__)
        for (const char* d : {"/var/run/disk/by-id/", "/var/run/disk/by-serial/",
                              "/var/run/disk/by-path/", "/dev/"}) {
            cands.push_back(std::string(d) + nombre);
        }
#elif defined(__FreeBSD__)
        for (const char* d : {"/dev/", "/dev/gpt/", "/dev/gptid/", "/dev/diskid/", "/dev/label/"}) {
            cands.push_back(std::string(d) + nombre);
        }
#else
        for (const char* d : {"/dev/", "/dev/disk/by-partuuid/", "/dev/disk/by-id/",
                              "/dev/disk/by-uuid/", "/dev/disk/by-partlabel/", "/dev/disk/by-path/",
                              "/dev/disk/by-vdev/", "/dev/mapper/"}) {
            cands.push_back(std::string(d) + nombre);
        }
#endif
    }
    for (const std::string& c : cands) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec) && !ec) {
            const auto real = std::filesystem::canonical(c, ec);
            return ec ? c : real.string();
        }
    }
    return {};
}

// --mutate-zpool-labelclear: borrar la etiqueta de ZFS de un dispositivo. Para lo que se
// queda atrás —un pool viejo en un disco reutilizado, que `zpool import` sigue enseñando—.
//
// **Nunca sobre algo que esté en uso.** Se niega si el dispositivo, o una partición suya,
// pertenece a un pool importado, aunque se pida «force»: `zpool labelclear -f` sí lo haría,
// y borrar la etiqueta de un disco vivo es perder el pool. «force» solo sirve para lo que
// zpool considera «podría estar en uso» sin estarlo de verdad, como un pool exportado.
ExecResult runZpoolLabelclear(const std::vector<std::string>& params) {
    ExecResult r;
#ifdef _WIN32
    // En Windows no hay rutas de dispositivo que zpool labelclear acepte por esta vía.
    (void)params;
    r.rc = 2;
    ponCodigo(r, "labelclear_windows", {});
    return r;
#else
    if (params.empty() || params[0].empty() || params[0].front() != '/') {
        r.rc = 2;
        ponCodigo(r, "usage", {"--mutate-zpool-labelclear <absolute device path> [force]"});
        return r;
    }
    std::error_code ec;
    const std::string dev = std::filesystem::canonical(params[0], ec).string();
    if (ec) {
        r.rc = 2;
        ponCodigo(r, "no_such_device", {params[0]});
        return r;
    }
    struct ::stat st{};
    if (::stat(dev.c_str(), &st) != 0
        || !(S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode) || S_ISREG(st.st_mode))) {
        r.rc = 2;
        ponCodigo(r, "not_device_or_file", {dev});
        return r;
    }
    // ¿Lo usa algún pool importado? `-P` da rutas completas y `-L` resuelve los enlaces.
    const ExecResult estado = runExecCapture("zpool", {"status", "-P", "-L"});
    for (const std::string& linea : zfsmgr::base::split(estado.out, "\n", true)) {
        std::istringstream iss(linea);
        std::string tok;
        iss >> tok;
        if (tok.empty() || tok.front() != '/') {
            continue;
        }
        std::error_code ec2;
        const auto realTok = std::filesystem::canonical(tok, ec2);
        const std::string usado = ec2 ? tok : realTok.string();
        // El mismo dispositivo, o una partición suya (/dev/sda → /dev/sda1, /dev/nvme0n1p2).
        if (usado == dev || usado.rfind(dev, 0) == 0) {
            r.rc = 1;
            r.err =
                dev + " is part of an imported pool (" + usado + "): its label is not cleared\n";
            return r;
        }
    }
    std::vector<std::string> args{"labelclear"};
    if (params.size() > 1 && params[1] == "force") {
        args.push_back("-f");
    }
    args.push_back(dev);
    r = runExecCapture("zpool", args);
    if (r.rc == 0) {
        r.out = "CLEARED=" + dev + "\n";
    }
    return r;
#endif
}

}  // namespace zfsmgr::agent
