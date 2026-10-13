#include "shell_internal.h"

#include "json.h"

#include <cstdio>
#include <iostream>

#ifdef _WIN32
#include <io.h>
#define SV_DUP _dup
#define SV_DUP2 _dup2
#define SV_CLOSE _close
#define SV_WRITE(fd, p, n) _write(fd, p, static_cast<unsigned>(n))
#define SV_FILENO _fileno
#else
#include <unistd.h>
#define SV_DUP dup
#define SV_DUP2 dup2
#define SV_CLOSE close
#define SV_WRITE(fd, p, n) ::write(fd, p, n)
#define SV_FILENO fileno
#endif

// The shell as a service: see servirOrdenes in shell.h.
//
// Each command runs exactly as typed in the terminal (ejecutaLinea); what it prints is
// captured and sent back with its exit code. The process runs one command at a time, so
// redirecting fd 1 and 2 around each one is safe here, unlike inside a multi-threaded program.

namespace zfsmgr::cli {

namespace {

namespace J = zfsmgr::base::json;

// The client reads events from the ORIGINAL standard output, kept aside before any capture:
// while a command runs, fd 1 points to its capture file.
int g_eventos = -1;

void emite(const J::Value& v) {
    const std::string linea = J::toCompact(v) + "\n";
    std::size_t hecho = 0;
    while (hecho < linea.size()) {
        const auto n = SV_WRITE(g_eventos, linea.data() + hecho, linea.size() - hecho);
        if (n <= 0) {
            return;
        }
        hecho += static_cast<std::size_t>(n);
    }
}

// Standard output and error of one command, to a temporary file each.
class CapturaDeOrden {
public:
    CapturaDeOrden() {
        std::fflush(stdout);
        std::fflush(stderr);
        out_ = std::tmpfile();
        err_ = std::tmpfile();
        if (!out_ || !err_) {
            return;
        }
        copiaOut_ = SV_DUP(SV_FILENO(stdout));
        copiaErr_ = SV_DUP(SV_FILENO(stderr));
        SV_DUP2(SV_FILENO(out_), SV_FILENO(stdout));
        SV_DUP2(SV_FILENO(err_), SV_FILENO(stderr));
    }
    void termina(std::string& out, std::string& err) {
        if (!out_ || !err_) {
            return;
        }
        std::fflush(stdout);
        std::fflush(stderr);
        SV_DUP2(copiaOut_, SV_FILENO(stdout));
        SV_DUP2(copiaErr_, SV_FILENO(stderr));
        SV_CLOSE(copiaOut_);
        SV_CLOSE(copiaErr_);
        out = lee(out_);
        err = lee(err_);
        out_ = nullptr;
        err_ = nullptr;
    }
    ~CapturaDeOrden() {
        std::string a;
        std::string b;
        termina(a, b);
    }

private:
    static std::string lee(std::FILE* f) {
        std::string texto;
        std::rewind(f);
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            texto.append(buf, n);
        }
        std::fclose(f);
        return texto;
    }
    std::FILE* out_{nullptr};
    std::FILE* err_{nullptr};
    int copiaOut_{-1};
    int copiaErr_{-1};
};

}  // namespace

int servirOrdenes(Sesion& ses) {
    std::fflush(stdout);
    g_eventos = SV_DUP(SV_FILENO(stdout));
    Estado e;
    struct AlSalir {
        Estado& e;
        ~AlSalir() {
            if (!e.montajesLectura.empty()) {
                sueltaMontajesLectura(e, /*todos=*/true);
            }
        }
    } alSalir{e};
    e.ses = &ses;
    e.formato = Formato::Json;
    recarga(e);

    double idActual = 0;
    // A confirmation: the question goes out as an event, the answer comes back as a line.
    e.preguntaServicio = [&idActual](const std::string& texto) {
        J::Value ev;
        ev.set("id", J::Value(idActual));
        ev.set("type", J::Value(std::string("confirm")));
        ev.set("text", J::Value(texto));
        emite(ev);
        std::string respuesta;
        if (!std::getline(std::cin, respuesta)) {
            return false;
        }
        J::Value r;
        std::string err;
        return J::parse(respuesta, r, &err) && r["answer"].toBool(false);
    };

    // A secret: asked as an event, the answer comes back as a line {"id","secret"}.
    e.pideSecretoServicio = [&idActual](const std::string& texto, std::string& secreto) {
        J::Value ev;
        ev.set("id", J::Value(idActual));
        ev.set("type", J::Value(std::string("secret")));
        ev.set("text", J::Value(texto));
        emite(ev);
        std::string respuesta;
        if (!std::getline(std::cin, respuesta)) {
            return false;
        }
        J::Value r;
        std::string err;
        if (!J::parse(respuesta, r, &err) || !r["secret"].isString()) {
            return false;
        }
        secreto = r["secret"].toString();
        return true;
    };

    J::Value listo;
    listo.set("type", J::Value(std::string("ready")));
    if (!e.conns.aviso.empty()) {
        listo.set("warning", J::Value(e.conns.aviso));
    }
    emite(listo);

    std::string peticion;
    while (!e.salir && std::getline(std::cin, peticion)) {
        if (B::trim(peticion).empty()) {
            continue;
        }
        J::Value pet;
        std::string err;
        J::Value res;
        res.set("type", J::Value(std::string("result")));
        if (!J::parse(peticion, pet, &err) || !pet["line"].isString()) {
            res.set("rc", J::Value(2.0));
            res.set("stderr", J::Value(std::string("bad request: ") + err));
            emite(res);
            continue;
        }
        idActual = pet["id"].toDouble();
        res.set("id", pet["id"]);
        e.ultimoRc = 0;
        std::string out;
        std::string errTexto;
        {
            CapturaDeOrden captura;
            ejecutaLinea(e, pet["line"].toString(), nullptr);
            captura.termina(out, errTexto);
        }
        res.set("rc", J::Value(static_cast<double>(e.ultimoRc)));
        res.set("stdout", J::Value(out));
        res.set("stderr", J::Value(errTexto));
        emite(res);
    }
    SV_CLOSE(g_eventos);
    g_eventos = -1;
    return 0;
}

}  // namespace zfsmgr::cli
