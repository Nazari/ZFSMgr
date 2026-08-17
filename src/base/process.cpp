#include "process.h"

#include <cstring>
#include <cstdio>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
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

}  // namespace zfsmgr::base
