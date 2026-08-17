#include "process.h"

#include <cstring>
#include <chrono>
#include <thread>
#include <utility>
#include <cstdio>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace zfsmgr::base {

int decodeWaitStatus(int status) {
#ifndef _WIN32
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 125;
#else
    if (status < 0) {
        return 125;
    }
    // En Windows/system() y pclose() suelen devolver directamente el RC.
    if (status > 255) {
        return (status >> 8) & 0xff;
    }
    return status & 0xff;
#endif
}

int runExecStreaming(const std::string& program, const std::vector<std::string>& args) {
#ifndef _WIN32
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "fork failed\n";
        return 125;
    }
    if (pid == 0) {
        execvp(program.c_str(), argv.data());
        std::perror("execvp");
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        std::perror("waitpid");
        return 125;
    }
    return decodeWaitStatus(status);
#else
    auto quoteArg = [](const std::string& a) -> std::string {
        std::string q = "\"";
        for (char c : a) {
            if (c == '"') {
                q += "\\\"";
            } else {
                q.push_back(c);
            }
        }
        q.push_back('"');
        return q;
    };
    std::string cmd = quoteArg(program);
    for (const std::string& a : args) {
        cmd.push_back(' ');
        cmd += quoteArg(a);
    }
    // cmd.exe quita la PRIMERA y la ÚLTIMA comilla de la línea que recibe, así que
    // "zpool" "--version" le llega como zpool" "--version y responde que no es un
    // comando reconocido. Envolver toda la línea en un par extra hace que lo que
    // elimine sea ese par y el resto quede intacto. Comprobado contra el agente
    // nativo corriendo en un Windows 11 real.
    cmd = "\"" + cmd + "\"";
    const int rc = std::system(cmd.c_str());
    return decodeWaitStatus(rc);
#endif
}

#ifdef _WIN32
// Línea de comandos de Windows a partir de argv.
//
// CreateProcess recibe UNA cadena y es el propio programa quien la vuelve a trocear, así
// que el entrecomillado es responsabilidad de quien la construye. La regla no es la
// intuitiva: las barras invertidas solo se duplican cuando preceden a una comilla.
//
// Vive aquí, en una sola función, porque la usan runExecCapture y
// runExecCaptureWithStdin. Copiada dos veces, una corrección en una de ellas dejaría a
// la otra tratando mal exactamente los mismos argumentos: rutas con espacios y nombres
// de dataset con comillas.
std::string winBuildCommandLine(const std::string& program,
                                       const std::vector<std::string>& args) {
    auto quoteArg = [](const std::string& a) -> std::string {
        if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) {
            return a;
        }
        std::string q = "\"";
        size_t slashes = 0;
        for (char c : a) {
            if (c == '\\') {
                ++slashes;
                q.push_back(c);
                continue;
            }
            if (c == '"') {
                q.append(slashes + 1, '\\');
            }
            slashes = 0;
            q.push_back(c);
        }
        q.append(slashes, '\\');
        q.push_back('"');
        return q;
    };
    std::string cmd = quoteArg(program);
    for (const std::string& a : args) {
        cmd.push_back(' ');
        cmd += quoteArg(a);
    }
    return cmd;
}
#endif

ExecResult runExecCapture(const std::string& program, const std::vector<std::string>& args) {
    ExecResult r;
#ifndef _WIN32
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        r.rc = 125;
        r.err = "pipe failed";
        if (outPipe[0] >= 0) {
            close(outPipe[0]);
        }
        if (outPipe[1] >= 0) {
            close(outPipe[1]);
        }
        if (errPipe[0] >= 0) {
            close(errPipe[0]);
        }
        if (errPipe[1] >= 0) {
            close(errPipe[1]);
        }
        return r;
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        r.rc = 125;
        r.err = "fork failed";
        close(outPipe[0]);
        close(outPipe[1]);
        close(errPipe[0]);
        close(errPipe[1]);
        return r;
    }

    if (pid == 0) {
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(outPipe[0]);
        close(outPipe[1]);
        close(errPipe[0]);
        close(errPipe[1]);
        execvp(program.c_str(), argv.data());
        std::perror("execvp");
        _exit(127);
    }

    close(outPipe[1]);
    close(errPipe[1]);

    auto readAllFd = [](int fd) {
        std::string out;
        char buf[4096];
        while (true) {
            const ssize_t n = read(fd, buf, sizeof(buf));
            if (n == 0) {
                break;
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            out.append(buf, static_cast<std::size_t>(n));
        }
        return out;
    };

    r.out = readAllFd(outPipe[0]);
    r.err = readAllFd(errPipe[0]);
    close(outPipe[0]);
    close(errPipe[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        r.rc = 125;
        r.err += "\nwaitpid failed";
        return r;
    }
    r.rc = decodeWaitStatus(status);
    return r;
#else
    // CreateProcess con dos tuberías, en vez de _popen.
    //
    // _popen obligaba a fusionar stderr en stdout con "2>&1", así que r.err salía
    // SIEMPRE vacío en Windows y los llamantes que discriminan por .err se comportaban
    // distinto que en Unix. Además pasaba por cmd.exe, que se come la primera y la
    // última comilla de la línea, y hacía falta un par extra envolviéndolo todo para
    // compensarlo. CreateProcess no invoca ningún shell: desaparecen las dos cosas.
    //
    const std::string cmd = winBuildCommandLine(program, args);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE outRd = nullptr, outWr = nullptr, errRd = nullptr, errWr = nullptr;
    if (!CreatePipe(&outRd, &outWr, &sa, 0)) {
        r.rc = 125;
        r.err = "CreatePipe failed";
        return r;
    }
    if (!CreatePipe(&errRd, &errWr, &sa, 0)) {
        CloseHandle(outRd);
        CloseHandle(outWr);
        r.rc = 125;
        r.err = "CreatePipe failed";
        return r;
    }
    // Los extremos de lectura NO se heredan: si el hijo se queda con ellos, la tubería
    // nunca da EOF y la lectura se cuelga para siempre.
    SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWr;
    si.hStdError = errWr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    const BOOL started = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr,
                                        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(outWr);
    CloseHandle(errWr);
    if (!started) {
        CloseHandle(outRd);
        CloseHandle(errRd);
        r.rc = 127;
        r.err = "cannot start " + program;
        return r;
    }

    // stderr se drena en otro hilo: leer una tubería entera y luego la otra se bloquea
    // en cuanto el hijo llena la que no se está leyendo.
    auto drain = [](HANDLE h, std::string* into) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
            into->append(buf, n);
        }
        CloseHandle(h);
    };
    std::thread errThread(drain, errRd, &r.err);
    drain(outRd, &r.out);
    errThread.join();

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    r.rc = GetExitCodeProcess(pi.hProcess, &exitCode) ? static_cast<int>(exitCode) : 125;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
#endif
}


// Ejecuta un programa alimentando su entrada estándar y capturando salida y error.
//
// Durante mucho tiempo esto fue POSIX puro y en Windows no existía en absoluto: sus
// llamantes estaban dentro de un `#ifndef _WIN32` que devolvía "not supported". Por eso
// `zfs load-key` y `zfs change-key` no funcionaban allí, y por eso Copiar y Nivelar no
// tenían por dónde empezar: sin forma de escribir en la entrada de un proceso, no hay
// forma de darle un flujo a `zfs recv`.
ExecResult runExecCaptureWithStdin(const std::string& program,
                                           const std::vector<std::string>& args,
                                           const std::string& stdinData) {
    ExecResult r;
#ifndef _WIN32
    int inPipe[2]  = {-1, -1};
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        r.rc = 125; r.err = "pipe failed\n";
        for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) {
            if (fd >= 0) close(fd);
        }
        return r;
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        r.rc = 125; r.err = "fork failed\n";
        for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) close(fd);
        return r;
    }
    if (pid == 0) {
        dup2(inPipe[0],  STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) close(fd);
        execvp(program.c_str(), argv.data());
        std::perror("execvp"); _exit(127);
    }
    close(inPipe[0]); close(outPipe[1]); close(errPipe[1]);

    // Write stdin data, then close write end so child sees EOF
    if (!stdinData.empty()) {
        std::size_t written = 0;
        while (written < stdinData.size()) {
            const ssize_t n = write(inPipe[1], stdinData.data() + written, stdinData.size() - written);
            if (n < 0) { if (errno == EINTR) continue; break; }
            written += static_cast<std::size_t>(n);
        }
    }
    close(inPipe[1]);

    auto readAllFd = [](int fd) {
        std::string out; char buf[4096];
        while (true) {
            const ssize_t n = read(fd, buf, sizeof(buf));
            if (n == 0) break;
            if (n < 0) { if (errno == EINTR) continue; break; }
            out.append(buf, static_cast<std::size_t>(n));
        }
        return out;
    };
    r.out = readAllFd(outPipe[0]);
    r.err = readAllFd(errPipe[0]);
    close(outPipe[0]); close(errPipe[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { r.rc = 125; r.err += "\nwaitpid failed"; return r; }
    r.rc = decodeWaitStatus(status);
    return r;
#else
    // Tres tuberías y CreateProcess. Dos decisiones que no son de estilo:
    //
    // 1. Al hijo se le da SIEMPRE una tubería anónima nuestra, nunca un socket. Medido
    //    contra OpenZFS on Windows: `zfs recv` alimentado por el stdio que entrega sshd
    //    muere con "I/O error" a los 132 KiB, mientras que por una tubería local recibe
    //    el flujo entero con las sumas intactas. Quien porte el receptor por socket debe
    //    bombear del socket a una tubería, no entregarle el descriptor al proceso.
    // 2. La salida se drena en hilos MIENTRAS se escribe la entrada. Escribir todo y
    //    leer después funciona con una contraseña y se abraza a muerte con un flujo: el
    //    hijo llena su tubería de salida, deja de leer la de entrada, y los dos esperan.
    const std::string cmd = winBuildCommandLine(program, args);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE inRd = nullptr, inWr = nullptr;
    HANDLE outRd = nullptr, outWr = nullptr;
    HANDLE errRd = nullptr, errWr = nullptr;
    auto closeIf = [](HANDLE h) { if (h) { CloseHandle(h); } };
    if (!CreatePipe(&inRd, &inWr, &sa, 0)) {
        r.rc = 125; r.err = "CreatePipe failed"; return r;
    }
    if (!CreatePipe(&outRd, &outWr, &sa, 0)) {
        closeIf(inRd); closeIf(inWr);
        r.rc = 125; r.err = "CreatePipe failed"; return r;
    }
    if (!CreatePipe(&errRd, &errWr, &sa, 0)) {
        closeIf(inRd); closeIf(inWr); closeIf(outRd); closeIf(outWr);
        r.rc = 125; r.err = "CreatePipe failed"; return r;
    }
    // Los extremos que se queda el padre NO se heredan: si el hijo conserva una copia,
    // la tubería no da nunca EOF y el drenaje no termina.
    SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRd;
    si.hStdOutput = outWr;
    si.hStdError = errWr;
    PROCESS_INFORMATION pi{};
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    const BOOL started = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr,
                                        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    // Los extremos del hijo se cierran en el padre pase lo que pase: mientras el padre
    // conserve el de escritura de salida, esa tubería tampoco daría EOF.
    CloseHandle(inRd);
    CloseHandle(outWr);
    CloseHandle(errWr);
    if (!started) {
        CloseHandle(inWr); CloseHandle(outRd); CloseHandle(errRd);
        r.rc = 127; r.err = "cannot start " + program; return r;
    }

    auto drain = [](HANDLE h, std::string* into) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
            into->append(buf, n);
        }
        CloseHandle(h);
    };
    std::thread outThread(drain, outRd, &r.out);
    std::thread errThread(drain, errRd, &r.err);

    std::size_t written = 0;
    while (written < stdinData.size()) {
        const std::size_t remaining = stdinData.size() - written;
        const DWORD chunk = (remaining > 65536u) ? 65536u : static_cast<DWORD>(remaining);
        DWORD n = 0;
        if (!WriteFile(inWr, stdinData.data() + written, chunk, &n, nullptr) || n == 0) {
            break;  // el hijo cerró su entrada; lo dirá su código de salida
        }
        written += n;
    }
    // Cerrar la escritura es lo que le da EOF al hijo. Sin esto, `zfs load-key` se queda
    // esperando una línea que no llega y no termina nunca.
    CloseHandle(inWr);

    outThread.join();
    errThread.join();

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    r.rc = GetExitCodeProcess(pi.hProcess, &exitCode) ? static_cast<int>(exitCode) : 125;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
#endif
}

namespace {

// Reparte lo que llega en líneas completas y llama a la función con cada una. Lo que
// quede a medias se guarda para el siguiente trozo.
//
// El retorno de carro se trata como fin de línea igual que el salto: `zfs send` escribe
// el progreso con retornos, sin saltos, y si no se cortara ahí la barra de progreso no
// aparecería hasta el final.
void repartaLineas(std::string& pendiente, const std::string& trozo,
                   const std::function<void(const std::string&)>& fn) {
    if (!fn) {
        return;
    }
    pendiente += trozo;
    std::size_t ini = 0;
    for (std::size_t i = 0; i < pendiente.size(); ++i) {
        const char c = pendiente[i];
        if (c != '\n' && c != '\r') {
            continue;
        }
        if (i > ini) {
            fn(pendiente.substr(ini, i - ini));
        }
        ini = i + 1;
    }
    pendiente.erase(0, ini);
}

void ultimaLinea(std::string& pendiente, const std::function<void(const std::string&)>& fn) {
    if (fn && !pendiente.empty()) {
        fn(pendiente);
    }
    pendiente.clear();
}

}  // namespace

#ifndef _WIN32

ExecResult runExecStream(const std::string& program,
                         const std::vector<std::string>& args,
                         const std::string& stdinData,
                         int timeoutMs,
                         const StreamCallbacks& cb) {
    ExecResult r;
    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        r.rc = 125;
        r.err = "pipe failed";
        for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) {
            if (fd >= 0) {
                close(fd);
            }
        }
        return r;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        r.rc = 125;
        r.err = "fork failed";
        for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) {
            close(fd);
        }
        return r;
    }
    if (pid == 0) {
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) {
            close(fd);
        }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(program.c_str()));
        for (const std::string& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        execvp(program.c_str(), argv.data());
        _exit(127);
    }

    close(inPipe[0]);
    close(outPipe[1]);
    close(errPipe[1]);
    // Sin bloqueo en las tres: con el proceso escribiendo y nosotros escribiendo a la vez,
    // una lectura o escritura bloqueante puede dejar a los dos esperándose.
    for (int fd : {inPipe[1], outPipe[0], errPipe[0]}) {
        const int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }

    std::string porEscribir = stdinData;
    std::string pendOut;
    std::string pendErr;
    bool outAbierto = true;
    bool errAbierto = true;
    bool cancelado = false;
    bool porTiempo = false;

    const auto t0 = std::chrono::steady_clock::now();
    auto transcurrido = [&t0]() {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count());
    };

    char buf[8192];
    while (outAbierto || errAbierto || !porEscribir.empty()) {
        pollfd fds[3];
        int n = 0;
        int idxOut = -1;
        int idxErr = -1;
        int idxIn = -1;
        if (outAbierto) {
            idxOut = n;
            fds[n++] = {outPipe[0], POLLIN, 0};
        }
        if (errAbierto) {
            idxErr = n;
            fds[n++] = {errPipe[0], POLLIN, 0};
        }
        if (!porEscribir.empty() && inPipe[1] >= 0) {
            idxIn = n;
            fds[n++] = {inPipe[1], POLLOUT, 0};
        }
        if (n == 0) {
            break;
        }
        // 120 ms, el mismo intervalo con el que la versión de Qt sondeaba: es lo que hace
        // que la interfaz siga respondiendo sin quemar CPU.
        const int listos = ::poll(fds, static_cast<nfds_t>(n), 120);
        if (listos < 0 && errno == EINTR) {
            continue;  // una señal, no un error
        }

        if (idxOut >= 0 && (fds[idxOut].revents & (POLLIN | POLLHUP))) {
            const ssize_t leidos = read(outPipe[0], buf, sizeof(buf));
            if (leidos > 0) {
                const std::string trozo(buf, static_cast<std::size_t>(leidos));
                r.out += trozo;
                repartaLineas(pendOut, trozo, cb.onStdoutLine);
            } else if (leidos == 0) {
                outAbierto = false;
            }
        }
        if (idxErr >= 0 && (fds[idxErr].revents & (POLLIN | POLLHUP))) {
            const ssize_t leidos = read(errPipe[0], buf, sizeof(buf));
            if (leidos > 0) {
                const std::string trozo(buf, static_cast<std::size_t>(leidos));
                r.err += trozo;
                repartaLineas(pendErr, trozo, cb.onStderrLine);
            } else if (leidos == 0) {
                errAbierto = false;
            }
        }
        if (idxIn >= 0 && (fds[idxIn].revents & POLLOUT)) {
            const ssize_t escritos = write(inPipe[1], porEscribir.data(), porEscribir.size());
            if (escritos > 0) {
                porEscribir.erase(0, static_cast<std::size_t>(escritos));
            } else if (escritos < 0 && errno != EAGAIN && errno != EINTR) {
                porEscribir.clear();  // el otro extremo se fue; no hay a quién escribir
            }
            if (porEscribir.empty() && inPipe[1] >= 0) {
                // Cerrar la entrada es lo que le dice al programa que ya no hay más. Sin
                // esto, `zfs recv` se queda esperando para siempre.
                close(inPipe[1]);
                inPipe[1] = -1;
            }
        }

        if (cb.onTick && !cb.onTick(transcurrido())) {
            cancelado = true;
            break;
        }
        if (timeoutMs > 0 && transcurrido() > timeoutMs) {
            porTiempo = true;
            break;
        }
    }

    ultimaLinea(pendOut, cb.onStdoutLine);
    ultimaLinea(pendErr, cb.onStderrLine);

    if (cancelado || porTiempo) {
        // TERM primero y KILL si no se va: matar de golpe deja los hijos del proceso
        // —el `ssh` que a su vez lanzó otra cosa— huérfanos y vivos.
        kill(pid, SIGTERM);
        for (int i = 0; i < 20; ++i) {
            int st = 0;
            if (waitpid(pid, &st, WNOHANG) == pid) {
                r.rc = cancelado ? 130 : 124;
                goto limpiar;
            }
            usleep(50 * 1000);
        }
        kill(pid, SIGKILL);
    }
    {
        int st = 0;
        waitpid(pid, &st, 0);
        r.rc = (cancelado || porTiempo) ? (cancelado ? 130 : 124) : decodeWaitStatus(st);
    }

limpiar:
    if (inPipe[1] >= 0) {
        close(inPipe[1]);
    }
    close(outPipe[0]);
    close(errPipe[0]);
    return r;
}

#else  // _WIN32

ExecResult runExecStream(const std::string& program,
                         const std::vector<std::string>& args,
                         const std::string& stdinData,
                         int timeoutMs,
                         const StreamCallbacks& cb) {
    ExecResult r;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0)
        || !CreatePipe(&errR, &errW, &sa, 0)) {
        r.rc = 125;
        r.err = "CreatePipe failed";
        return r;
    }
    // Los extremos nuestros NO se heredan: si el hijo los tuviera, la tubería no se
    // cerraría al morir él y la lectura no terminaría nunca.
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = errW;
    PROCESS_INFORMATION pi{};
    std::string cmdline = winBuildCommandLine(program, args);
    std::vector<char> cmdbuf(cmdline.begin(), cmdline.end());
    cmdbuf.push_back('\0');
    if (!CreateProcessA(nullptr, cmdbuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        r.rc = 127;
        r.err = "CreateProcess failed";
        for (HANDLE h : {inR, inW, outR, outW, errR, errW}) {
            CloseHandle(h);
        }
        return r;
    }
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(errW);

    std::string porEscribir = stdinData;
    std::string pendOut;
    std::string pendErr;
    bool cancelado = false;
    bool porTiempo = false;
    const auto t0 = std::chrono::steady_clock::now();
    auto transcurrido = [&t0]() {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count());
    };

    // Sondeo con PeekNamedPipe: las tuberías anónimas de Windows no admiten espera con
    // tiempo límite, así que se mira si hay algo antes de leer. Leer a ciegas bloquearía.
    auto drena = [&](HANDLE h, std::string& acumulado, std::string& pendiente,
                     const std::function<void(const std::string&)>& fn) {
        DWORD disponible = 0;
        while (PeekNamedPipe(h, nullptr, 0, nullptr, &disponible, nullptr) && disponible > 0) {
            char buf[8192];
            DWORD leidos = 0;
            const DWORD pedir = disponible < sizeof(buf) ? disponible : sizeof(buf);
            if (!ReadFile(h, buf, pedir, &leidos, nullptr) || leidos == 0) {
                break;
            }
            const std::string trozo(buf, leidos);
            acumulado += trozo;
            repartaLineas(pendiente, trozo, fn);
        }
    };

    while (true) {
        if (!porEscribir.empty() && inW) {
            DWORD escritos = 0;
            if (WriteFile(inW, porEscribir.data(), static_cast<DWORD>(porEscribir.size()), &escritos,
                          nullptr)
                && escritos > 0) {
                porEscribir.erase(0, escritos);
            } else {
                porEscribir.clear();
            }
            if (porEscribir.empty()) {
                CloseHandle(inW);
                inW = nullptr;
            }
        }
        drena(outR, r.out, pendOut, cb.onStdoutLine);
        drena(errR, r.err, pendErr, cb.onStderrLine);

        const DWORD esperado = WaitForSingleObject(pi.hProcess, 120);
        if (esperado == WAIT_OBJECT_0) {
            // Una última pasada: entre el penúltimo drenaje y su muerte pudo escribir.
            drena(outR, r.out, pendOut, cb.onStdoutLine);
            drena(errR, r.err, pendErr, cb.onStderrLine);
            break;
        }
        if (cb.onTick && !cb.onTick(transcurrido())) {
            cancelado = true;
            break;
        }
        if (timeoutMs > 0 && transcurrido() > timeoutMs) {
            porTiempo = true;
            break;
        }
    }

    ultimaLinea(pendOut, cb.onStdoutLine);
    ultimaLinea(pendErr, cb.onStderrLine);

    if (cancelado || porTiempo) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
        r.rc = cancelado ? 130 : 124;
    } else {
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        r.rc = static_cast<int>(code);
    }
    if (inW) {
        CloseHandle(inW);
    }
    CloseHandle(outR);
    CloseHandle(errR);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return r;
}

#endif  // _WIN32

// --- ChildProcess: un proceso que sigue vivo entre llamadas.

ChildProcess::~ChildProcess() { stop(1500); }

ChildProcess::ChildProcess(ChildProcess&& otro) noexcept { *this = std::move(otro); }

ChildProcess& ChildProcess::operator=(ChildProcess&& otro) noexcept {
    if (this != &otro) {
        stop(1500);
        m_pid = otro.m_pid;
        m_recogido = otro.m_recogido;
#ifdef _WIN32
        m_handle = otro.m_handle;
        otro.m_handle = nullptr;
#endif
        otro.olvida();
    }
    return *this;
}

void ChildProcess::olvida() {
    m_pid = 0;
    m_recogido = true;
#ifdef _WIN32
    m_handle = nullptr;
#endif
}

#ifndef _WIN32

bool ChildProcess::start(const std::string& program, const std::vector<std::string>& args) {
    stop(1500);
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        // Sesión propia: un `ssh -L` que comparta el grupo de procesos recibiría el
        // Ctrl-C del terminal que lanzó la aplicación y el túnel se caería solo.
        setsid();
        execvp(program.c_str(), argv.data());
        _exit(127);
    }
    m_pid = pid;
    m_recogido = false;
    // Si execvp falló, el hijo muere enseguida con 127 y isRunning() lo verá.
    return true;
}

bool ChildProcess::isRunning() {
    if (m_pid <= 0 || m_recogido) {
        return false;
    }
    int st = 0;
    const pid_t r = waitpid(static_cast<pid_t>(m_pid), &st, WNOHANG);
    if (r == 0) {
        return true;  // sigue vivo
    }
    // Murió (o ya no es hijo nuestro): queda recogido, sin zombi.
    m_recogido = true;
    return false;
}

void ChildProcess::stop(int msEspera) {
    if (m_pid <= 0 || m_recogido) {
        olvida();
        return;
    }
    const pid_t pid = static_cast<pid_t>(m_pid);
    ::kill(pid, SIGTERM);
    const auto inicio = std::chrono::steady_clock::now();
    while (true) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) != 0) {
            m_recogido = true;
            break;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - inicio)
                            .count();
        if (ms >= msEspera) {
            // No hizo caso. Sin miramientos, y se recoge: si no, queda un zombi por cada
            // túnel que se resistió.
            ::kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            m_recogido = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    olvida();
}

#else  // _WIN32

bool ChildProcess::start(const std::string& program, const std::vector<std::string>& args) {
    stop(1500);
    std::string cmdline = winBuildCommandLine(program, args);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<char> buf(cmdline.begin(), cmdline.end());
    buf.push_back('\0');
    // CREATE_NO_WINDOW: si no, cada túnel abre una consola negra encima de la ventana.
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    m_handle = pi.hProcess;
    m_pid = static_cast<long long>(pi.dwProcessId);
    m_recogido = false;
    return true;
}

bool ChildProcess::isRunning() {
    if (!m_handle || m_recogido) {
        return false;
    }
    if (WaitForSingleObject(static_cast<HANDLE>(m_handle), 0) == WAIT_TIMEOUT) {
        return true;
    }
    return false;
}

void ChildProcess::stop(int msEspera) {
    if (!m_handle) {
        olvida();
        return;
    }
    HANDLE h = static_cast<HANDLE>(m_handle);
    if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) {
        // En Windows no hay SIGTERM para un proceso sin consola propia: se le da el plazo
        // por si termina solo y luego se corta.
        if (WaitForSingleObject(h, msEspera > 0 ? static_cast<DWORD>(msEspera) : 0)
            == WAIT_TIMEOUT) {
            TerminateProcess(h, 1);
            WaitForSingleObject(h, 2000);
        }
    }
    CloseHandle(h);
    olvida();
}

#endif  // _WIN32

// --- Puertos locales.

namespace {

#ifdef _WIN32
using SockLocal = SOCKET;
constexpr SockLocal kSockLocalInvalido = INVALID_SOCKET;
void cierraLocal(SockLocal s) {
    if (s != kSockLocalInvalido) {
        closesocket(s);
    }
}
void aseguraWinsockLocal() {
    static bool hecho = false;
    if (!hecho) {
        WSADATA d{};
        WSAStartup(MAKEWORD(2, 2), &d);
        hecho = true;
    }
}
#else
using SockLocal = int;
constexpr SockLocal kSockLocalInvalido = -1;
void cierraLocal(SockLocal s) {
    if (s != kSockLocalInvalido) {
        ::close(s);
    }
}
void aseguraWinsockLocal() {}
#endif

}  // namespace

std::uint16_t reserveFreeLocalPort() {
    aseguraWinsockLocal();
    const SockLocal s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kSockLocalInvalido) {
        return 0;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;  // que lo elija el sistema
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(s, 1) != 0) {
        cierraLocal(s);
        return 0;
    }
    sockaddr_in puesto{};
#ifdef _WIN32
    int len = sizeof(puesto);
#else
    socklen_t len = sizeof(puesto);
#endif
    std::uint16_t puerto = 0;
    if (getsockname(s, reinterpret_cast<sockaddr*>(&puesto), &len) == 0) {
        puerto = ntohs(puesto.sin_port);
    }
    cierraLocal(s);
    return puerto;
}

bool canConnectLocal(std::uint16_t port, int timeoutMs) {
    if (port == 0) {
        return false;
    }
    aseguraWinsockLocal();
    const SockLocal s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kSockLocalInvalido) {
        return false;
    }
    // Con tope: contra un puerto que no escucha la respuesta es inmediata, pero contra uno
    // que está a medio montar puede quedarse esperando, y este bucle se recorre muchas
    // veces por segundo.
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    const bool ok = ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    cierraLocal(s);
    return ok;
}

}  // namespace zfsmgr::base
