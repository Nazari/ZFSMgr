#include "network_sync.h"
#include "presence.h"
#include "shell_internal.h"

#include <mutex>

#include "json.h"
#include "processes.h"
#include "storefiles.h"

#include <cstdio>
#include <filesystem>
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

// What config.json looks like on disk: its modification time and size. Another process (a
// terminal, the GUI's other shells) may add a connection or disconnect one while this service
// runs; comparing this before each request is how the service notices without polling.
struct HuellaConfig {
    std::filesystem::file_time_type when{};
    std::uintmax_t size{0};
    bool exists{false};
    bool operator==(const HuellaConfig& o) const {
        return exists == o.exists && when == o.when && size == o.size;
    }
};

HuellaConfig huellaConfig(const std::string& dirConfig) {
    HuellaConfig h;
    std::error_code ec;
    const std::filesystem::path p = zfsmgr::base::store::configPath(dirConfig);
    h.when = std::filesystem::last_write_time(p, ec);
    if (ec) {
        return h;
    }
    h.size = std::filesystem::file_size(p, ec);
    h.exists = !ec;
    return h;
}

// The client reads events from the ORIGINAL standard output, kept aside before any capture:
// while a command runs, fd 1 points to its capture file.
int g_eventos = -1;

// Events may come from another thread too (presence: close asked): one line at a time.
std::mutex g_emiteMutex;

void emite(const J::Value& v) {
    const std::string linea = J::toCompact(v) + "\n";
    std::lock_guard<std::mutex> lock(g_emiteMutex);
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
        out_ = zfsmgr::base::temporaryFile();
        err_ = zfsmgr::base::temporaryFile();
        if (!out_ || !err_) {
            if (out_) {
                std::fclose(out_);
            }
            if (err_) {
                std::fclose(err_);
            }
            out_ = nullptr;
            err_ = nullptr;
            return;
        }
        ok_ = true;
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
    // Without it the command's output would go into the events stream, which the client
    // skips: the command must not run (it looked like rc 0 with nothing to say).
    bool ok() const { return ok_; }
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
    bool ok_{false};
    std::FILE* err_{nullptr};
    int copiaOut_{-1};
    int copiaErr_{-1};
};

}  // namespace

// `network seed` / `network join` from a client (the installer): on this computer's own
// configuration, never on a network's.
static int ordenDeRed(Estado& e, bool sembrar) {
    if (esDirDeRed(e.ses->dirConfig)) {
        std::fputs(TC("t_serve_ya_en_red", "este ordenador ya está en una red\n"), stderr);
        return 1;
    }
    if (sembrar) {
        return sembrarRed(e.ses->dirConfig, e.ses->maestra);
    }
    std::string contrasena;
    if (!e.pideSecretoServicio
        || !e.pideSecretoServicio(T("t_serve_maestra_red",
                                    "La contraseña maestra de la red (la del ordenador que la creó)"),
                                  contrasena)) {
        return 1;
    }
    const int r = unirseARed(e.ses->dirConfig, e.ses->maestra, contrasena);
    volatile char* q = contrasena.empty() ? nullptr : &contrasena[0];
    for (std::size_t i = 0; i < contrasena.size(); ++i) {
        q[i] = '\0';
    }
    return r;
}

int servirOrdenes(Sesion& ses, bool anunciaClave) {
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
    // The network, caught up once per interface: by its general service, the one announcing
    // its presence; the machine services it starts have nothing to add.
    if (presenceOn()) {
        redAlArrancar(e);
    } else {
        e.generacionVista = B::store::writeGeneration();
    }
    HuellaConfig leida = huellaConfig(ses.dirConfig);

    double idActual = 0;
    // A confirmation: the question goes out as an event, the answer comes back as a line.
    e.preguntaServicio = [&idActual](const Pregunta& p) {
        J::Value ev;
        ev.set("id", J::Value(idActual));
        ev.set("type", J::Value(std::string("confirm")));
        ev.set("text", J::Value(textoDePregunta(p)));
        ev.set("title", J::Value(p.titulo));
        ev.set("irreversible", J::Value(p.irreversible));
        const auto lista = [](const std::vector<std::string>& v) {
            J::Value a{J::Array{}};
            for (const std::string& x : v) {
                a.push(J::Value(x));
            }
            return a;
        };
        ev.set("lose", lista(p.sePierde));
        ev.set("keep", lista(p.seConserva));
        ev.set("notes", lista(p.notas));
        ev.set("type_to_confirm", J::Value(p.escribir));
        emite(ev);
        std::string respuesta;
        if (!std::getline(std::cin, respuesta)) {
            return false;
        }
        J::Value r;
        std::string err;
        if (!J::parse(respuesta, r, &err) || !r["answer"].toBool(false)) {
            return false;
        }
        // What has to be typed is checked HERE, not trusted to the client: a client that
        // answers «true» without asking does not get past an irreversible question.
        if (!p.escribir.empty() && r["typed"].toString() != p.escribir) {
            std::fprintf(
                stderr, "%s\n",
                B::format(T("t_pq_no_coincide", "no es «%1»: no se hace nada"), {p.escribir})
                    .c_str());
            return false;
        }
        return true;
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
    // Which zfsmgr-cli this is, with its build: the interface shows it beside its own.
    listo.set("version", J::Value(B::agentversion::expected()));
    if (!e.conns.aviso.empty()) {
        listo.set("warning", J::Value(e.conns.aviso));
    }
    // No master password yet —a first run, or a configuration that never had one—: the
    // interface offers to create it. Without one no password can be stored, not even the
    // sudo one of this computer.
    if (e.ses->maestra.empty() && !B::store::hasSomethingEncrypted(e.ses->dirConfig)) {
        listo.set("no_master", J::Value(true));
    }
    if (anunciaClave && !e.ses->maestra.empty()) {
        listo.set("data_key", J::Value(e.ses->maestra));
    }
    // Whether this computer has joined a network (network_sync.h): the installer offers to
    // create one, or to join one, when it has not.
    listo.set("network", J::Value(esDirDeRed(ses.dirConfig)));
    emite(listo);

    // Asked from another machine to close (`sessions close`): the interface decides —it tells
    // the person and closes—; this service only passes it on.
    setOnCloseAsked([](const std::string& host, const std::string& user) {
        J::Value ev;
        ev.set("type", J::Value(std::string("close_asked")));
        ev.set("host", J::Value(host));
        ev.set("user", J::Value(user));
        emite(ev);
    });
    // The interface's presence in the daemons it reaches (presence.h): only for the service
    // the interface started with --presence, and rebuilt when the connections change.
    std::unique_ptr<Presencia> presencia = anunciaPresencia(ses, e.conns);
    std::string huellaPresencia = Presencia::huella(e.conns);
    const auto revisaPresencia = [&] {
        if (!presenceOn()) {
            return;
        }
        if (const std::string h = Presencia::huella(e.conns); h != huellaPresencia) {
            presencia.reset();
            presencia = anunciaPresencia(ses, e.conns);
            huellaPresencia = h;
        }
    };

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
        // The configuration changed under this service: read it again before running.
        if (const HuellaConfig ahora = huellaConfig(ses.dirConfig); !(ahora == leida)) {
            recarga(e);
            leida = huellaConfig(ses.dirConfig);
        }
        idActual = pet["id"].toDouble();
        res.set("id", pet["id"]);
        e.ultimoRc = 0;
        std::string out;
        std::string errTexto;
        // An optional "format" for this request only: "text" for what is read as it is
        // (history, status), the session's (json) otherwise.
        const Formato formatoSesion = e.formato;
        if (pet["format"].isString()) {
            const std::string f = pet["format"].toString();
            e.formato = f == "text" ? Formato::Texto : f == "tsv" ? Formato::Tsv : Formato::Json;
        }
        {
            CapturaDeOrden captura;
            const std::string linea = B::trim(pet["line"].toString());
            if (captura.ok() && (linea == "network seed" || linea == "network join")) {
                // The two orders of main() on this computer's own configuration, for a client
                // that already holds the secrets open: the service ENDS after them, because
                // its files are not the network's ones. The client starts another.
                e.ultimoRc = ordenDeRed(e, linea == "network seed");
                captura.termina(out, errTexto);
                e.salir = true;
            } else if (captura.ok()) {
                ejecutaLinea(e, pet["line"].toString(), nullptr);
                redTrasOrden(e);
                captura.termina(out, errTexto);
            } else {
                e.ultimoRc = 1;
                errTexto = T("t_serve_sin_temporal",
                             "no se pudo crear un fichero temporal para la salida de la orden");
            }
        }
        e.formato = formatoSesion;
        // Changed while the command ran —by the command itself or by another process—: read it
        // again now, so the next request never runs on what was there before.
        if (const HuellaConfig ahora = huellaConfig(ses.dirConfig); !(ahora == leida)) {
            recarga(e);
            leida = huellaConfig(ses.dirConfig);
        }
        revisaPresencia();
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
