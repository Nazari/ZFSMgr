// `probe <connection>`: whether a connection works, and what is on the other side — before
// relying on it. Reachable, which system (and whether it is the one the connection says),
// whether sudo takes the stored password, which OpenZFS, and which daemon. The new-connection
// assistant of an interface asks exactly this; here it is one command, so no client has to
// open its own ssh or read os-release itself.
//
// `create` without `--os` uses the same detection (detectarSistema).

#include "shell_internal.h"

#include "osinfo.h"

namespace zfsmgr::cli::detail {

namespace {

namespace J = B::json;

constexpr int kSondaMs = 12000;

// Runs a command on the other side as a plain shell command: no agent RPC (there may be no
// daemon yet) and nothing in the log unless verbose.
bool corre(Sesion& ses, const B::ConnectionProfile& p, const std::string& orden, std::string& out,
           std::string& err, int& rc) {
    return T::runSsh(ses.transporte, p, orden, kSondaMs, out, err, rc, {}, {}, {}, {},
                     /*allowAgentRpc=*/false, /*echoOutputToLog=*/ses.verboso);
}

// The blocks of the Unix probe, split by the «@@» lines it prints between them.
std::vector<std::string> bloques(const std::string& out) {
    std::vector<std::string> b(1);
    for (const std::string& l : B::split(out, "\n", false)) {
        if (B::trim(l) == "@@") {
            b.emplace_back();
        } else {
            b.back() += l + "\n";
        }
    }
    return b;
}

bool sistemaUnix(Sesion& ses, B::ConnectionProfile p, Sistema& s, std::string& motivo) {
    // One round trip for all of it. Each part prints nothing where it does not apply.
    p.osType = "Linux";  // no PowerShell wrapping: this is the probe for a Unix shell
    std::string out;
    std::string err;
    int rc = -1;
    if (!corre(ses, p,
               "uname -s; echo @@; cat /etc/os-release 2>/dev/null; echo @@; "
               "sw_vers -productVersion 2>/dev/null; echo @@; freebsd-version 2>/dev/null; :",
               out, err, rc)
        || rc == 255 || rc == 130) {
        // 255 is ssh's own failure, 130 the probe cut with Ctrl-C. Any other code means a
        // shell answered —on Windows, one that is not sh—, and the Windows probe follows.
        motivo = rc == 130 ? T("t_pr_interrumpida", "interrumpida")
                           : B::trim(H::oneLine(err.empty() ? out : err));
        s.alcanzable = false;
        return false;
    }
    s.alcanzable = true;
    const auto b = bloques(out);
    const std::string uname = B::trim(b[0]);
    const auto parte = [&b](std::size_t i) { return i < b.size() ? B::trim(b[i]) : std::string(); };
    if (uname == "Linux") {
        s.os = "Linux";
        s.detalle = B::osinfo::fromOsRelease(parte(1));
        if (s.detalle.empty()) {
            s.detalle = "Linux";
        }
        return true;
    }
    if (uname == "Darwin") {
        s.os = "macOS";
        s.detalle = parte(2).empty() ? "macOS" : "macOS " + parte(2);
        return true;
    }
    if (uname == "FreeBSD") {
        s.os = "FreeBSD";
        s.detalle = parte(3).empty() ? "FreeBSD" : "FreeBSD " + parte(3);
        return true;
    }
    motivo = B::trim(H::oneLine(err.empty() ? out : err));
    return false;
}

bool sistemaWindows(Sesion& ses, B::ConnectionProfile p, Sistema& s, std::string& motivo) {
    // The transport wraps it in PowerShell for a Windows profile, whatever shell sshd starts.
    p.osType = "Windows";
    std::string out;
    std::string err;
    int rc = -1;
    if (!corre(ses, p,
               "$cv=Get-ItemProperty 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion'; "
               "$v=$cv.DisplayVersion; if([string]::IsNullOrWhiteSpace($v)){$v=$cv.ReleaseId}; "
               // Windows 11 still says «Windows 10» in ProductName; its builds start at 22000.
               "$n=$cv.ProductName; if([int]$cv.CurrentBuild -ge 22000)"
               "{$n=$n -replace 'Windows 10','Windows 11'}; "
               "Write-Output ('ZM-WIN ' + ($n + ' ' + $v).Trim())",
               out, err, rc)
        || rc != 0) {
        motivo = B::trim(H::oneLine(err.empty() ? out : err));
        return false;
    }
    for (const std::string& l : B::split(out, "\n", true)) {
        if (B::startsWith(B::trim(l), "ZM-WIN")) {
            s.os = "Windows";
            s.detalle = B::trim(B::trim(l).substr(6));
            if (s.detalle.empty()) {
                s.detalle = "Windows";
            }
            return true;
        }
    }
    motivo = B::trim(H::oneLine(err.empty() ? out : err));
    return false;
}

// «windows 11» and «Windows» are the same system; so are «macos» and «macOS».
std::string familia(const std::string& os) {
    const std::string l = B::toLowerAscii(B::trim(os));
    for (const char* f : {"linux", "macos", "freebsd", "windows"}) {
        if (B::startsWith(l, f)) {
            return f;
        }
    }
    if (B::startsWith(l, "darwin") || B::startsWith(l, "mac")) {
        return "macos";
    }
    return l;
}

struct Aviso {
    std::string nivel;  // «error» or «warning»
    std::string asunto;
    std::string texto;
};

}  // namespace

bool detectarSistema(Sesion& ses, const B::ConnectionProfile& p, Sistema& s, std::string& motivo) {
    s = Sistema{};
    motivo.clear();
    if (T::isLocalConnection(p)) {
#ifdef _WIN32
        s.alcanzable = true;
        s.os = "Windows";
        s.detalle = "Windows";
        return true;
#else
        return sistemaUnix(ses, p, s, motivo);
#endif
    }
    if (sistemaUnix(ses, p, s, motivo)) {
        return true;
    }
    // ssh got there but the answer was not a Unix one: Windows' sshd starts cmd or PowerShell.
    if (s.alcanzable && sistemaWindows(ses, p, s, motivo)) {
        return true;
    }
    return false;
}

bool cmdProbe(Estado& e, const LineaAnalizada& linea) {
    Peticion pet;
    if (!prepara(e, linea, pet)) {
        return false;
    }
    const std::string id = subeHasta(pet.objetivo, Objetivo::Conexion).connection;
    if (id.empty()) {
        std::fputs(TC("t_pr_falta_conexion", "hace falta una conexión: probe zfsm://<conexión>\n"),
                   stderr);
        return false;
    }
    const auto* guardado = buscarConexion(e.conns, id);
    if (!guardado) {
        std::fprintf(
            stderr, "%s\n",
            B::format(T("t_no_conn_nombre", "no hay ninguna conexión llamada «%1»"), {id}).c_str());
        return false;
    }
    // Disconnected or not: checking a connection is how one decides to connect it.
    const B::ConnectionProfile p = conSudo(e, *guardado);
    std::vector<Aviso> avisos;
    J::Value doc{J::Object{}};
    doc.set("connection", J::Value(id));
    doc.set("configured_os", J::Value(p.osType));

    Sistema s;
    std::string motivo;
    const bool conSistema = detectarSistema(*e.ses, p, s, motivo);
    doc.set("reachable", J::Value(s.alcanzable));
    if (!s.alcanzable) {
        doc.set("reason", J::Value(motivo));
        avisos.push_back({"error", "machine",
                          B::format(T("t_ov_no_responde", "%1 no responde: %2"), {id, motivo})});
    } else {
        doc.set("os", J::Value(s.os));
        doc.set("os_detail", J::Value(s.detalle));
        const bool coincide = conSistema && familia(s.os) == familia(p.osType);
        doc.set("os_matches", J::Value(coincide));
        if (!conSistema) {
            avisos.push_back(
                {"error", "os",
                 B::format(T("t_pr_os_desconocido", "no se reconoce el sistema de %1: %2"),
                           {id, motivo})});
        } else if (!coincide) {
            avisos.push_back({"error", "os",
                              B::format(T("t_pr_os_distinto",
                                          "%1 es %2, pero la conexión dice %3: corrígelo con «edit "
                                          "zfsm://%1 --os %2»"),
                                        {id, s.os, p.osType})});
        }

        // sudo, with the system the machine really has.
        B::ConnectionProfile real = p;
        if (conSistema) {
            real.osType = s.os;
        }
        std::string sudo = "not_used";
        if (!H::isWindowsOsType(real.osType) && real.useSudo) {
            std::string out;
            std::string err;
            int rc = -1;
            const bool llego = corre(*e.ses, real, H::withSudoCommand(real, "true"), out, err, rc);
            const std::string dicho = B::trim(H::oneLine(err.empty() ? out : err));
            if (llego && rc == 0) {
                sudo = "ok";
            } else if (H::looksLikeSudoMissing(err + "\n" + out)) {
                sudo = "missing";
                avisos.push_back({"error", "sudo", remedioSinSudo(real)});
            } else if (real.password.empty()) {
                sudo = "needs_password";
                avisos.push_back({"error", "sudo",
                                  B::format(T("t_pr_sudo_sin_clave",
                                              "sudo en %1 pide una contraseña y la conexión no "
                                              "tiene: «edit zfsm://%1» para darla"),
                                            {id})});
            } else if (H::looksLikeSudoAuthFailure(dicho)) {
                sudo = "wrong_password";
                avisos.push_back(
                    {"error", "sudo",
                     B::format(T("t_pr_sudo_mala", "sudo en %1 no acepta la contraseña guardada"),
                               {id})});
            } else {
                sudo = "could_not_check";
                avisos.push_back(
                    {"warning", "sudo",
                     B::format(T("t_pr_sudo_no_comprobado", "no se pudo comprobar sudo en %1: %2"),
                               {id, dicho})});
            }
            doc.set("sudo_detail", J::Value(sudo == "ok" ? std::string() : dicho));
        }
        doc.set("sudo", J::Value(sudo));

        // OpenZFS, asked over SSH: on a new machine there is no daemon yet to ask.
        const auto zfs = B::daemoninstall::remoteOpenZfs(e.ses->transporte, real, e.ses->verboso);
        std::string estadoZfs = "ok";
        switch (zfs.soporte) {
            case H::OpenZfsSoporte::Ok:
                break;
            case H::OpenZfsSoporte::DemasiadoAntigua:
                estadoZfs = "too_old";
                break;
            case H::OpenZfsSoporte::SinCargar:
                estadoZfs = "not_loaded";
                break;
            case H::OpenZfsSoporte::NoHay:
                estadoZfs = "missing";
                break;
            case H::OpenZfsSoporte::SinRespuesta:
                estadoZfs = "unknown";
                break;
        }
        doc.set("openzfs_version", J::Value(zfs.version));
        doc.set("openzfs", J::Value(estadoZfs));
        if (estadoZfs != "ok") {
            avisos.push_back({estadoZfs == "unknown" ? "warning" : "error", "openzfs",
                              id + ": " + H::textoDeSoporteOpenZfs(zfs)});
        }

        // The daemon.
        std::string out;
        std::string err;
        int rc = -1;
        std::string daemon = "unreachable";
        std::string version;
        std::string porque;
        if (ejecutarAgente(*e.ses, real, {"--health"}, out, err, rc, &porque, 8000) && rc == 0) {
            for (const std::string& l : B::split(out, "\n", true)) {
                if (B::startsWith(l, "VERSION=")) {
                    version = B::trim(l.substr(8));
                }
            }
            daemon = "unknown";
            if (!version.empty()) {
                switch (B::agentversion::verdictFor(version, B::agentversion::expected())) {
                    case B::agentversion::Verdict::UpToDate:
                        daemon = "ok";
                        break;
                    case B::agentversion::Verdict::Update:
                        daemon = "outdated";
                        break;
                    case B::agentversion::Verdict::Ahead:
                        daemon = "ahead";
                        break;
                }
            }
        }
        // Older than this client, but the newest there is to install: nothing to update. A
        // development build of the CLI is newer than every daemon it carries, and «outdated»
        // there only made «install» look like it did nothing.
        if (daemon == "outdated" && version == versionParaInstalar(e, real)) {
            daemon = "latest";
        }
        doc.set("daemon_version", J::Value(version));
        doc.set("daemon", J::Value(daemon));
        // With sudo failing, that is the reason and it was said just above; a second warning
        // about the daemon told it again and suggested installing one that may be there.
        const bool sudoFalla = sudo == "needs_password" || sudo == "wrong_password";
        if (daemon == "unreachable") {
            // Not «there is none»: without sudo its credentials cannot be read, and a daemon
            // that is stopped looks the same from here.
            if (porque.empty()) {
                porque = B::trim(H::oneLine(err.empty() ? out : err));
            }
            doc.set("daemon_detail", J::Value(porque));
        }
        if (daemon == "unreachable" && !sudoFalla) {
            avisos.push_back({"warning", "daemon",
                              B::format(T("t_pr_sin_daemon",
                                          "no se pudo hablar con el daemon de %1 (%2): si no "
                                          "está instalado, «install-daemon --on zfsm://%1»"),
                                        {id, porque})});
        } else if (daemon == "outdated") {
            avisos.push_back({"warning", "daemon",
                              B::format(T("t_ov_daemon_viejo",
                                          "el daemon de %1 es la %2 y este cliente la %3: "
                                          "actualízalo con «install-daemon --on zfsm://%1»"),
                                        {id, version, B::agentversion::expected()})});
        }
    }

    std::stable_sort(avisos.begin(), avisos.end(), [](const Aviso& a, const Aviso& b) {
        return a.nivel == "error" && b.nivel != "error";
    });
    std::string estado = "ok";
    for (const Aviso& a : avisos) {
        if (a.nivel == "error") {
            estado = "error";
            break;
        }
        estado = "warning";
    }
    doc.set("status", J::Value(estado));
    J::Value ws{J::Array{}};
    for (const Aviso& a : avisos) {
        J::Value w{J::Object{}};
        w.set("level", J::Value(a.nivel));
        w.set("subject", J::Value(a.asunto));
        w.set("text", J::Value(a.texto));
        ws.push(w);
    }
    doc.set("warnings", ws);
    if (estado == "error") {
        e.ultimoRc = 1;
    }

    if (e.formato == Formato::Json) {
        std::printf("%s\n", J::toIndented(doc).c_str());
        return estado != "error";
    }
    const auto fila = [](const std::string& k, const std::string& v) {
        std::printf("%-10s %s\n", k.c_str(), v.empty() ? "-" : v.c_str());
    };
    fila(T("t_pr_txt_conexion", "conexión"), id);
    if (!s.alcanzable) {
        fila(T("t_pr_txt_estado", "estado"), T("t_ov_txt_no_responde", "no responde"));
    } else {
        fila(T("t_pr_txt_sistema", "sistema"), s.detalle);
        fila("sudo", doc["sudo"].toString());
        const std::string zv = doc["openzfs_version"].toString();
        const std::string ze = doc["openzfs"].toString();
        fila("OpenZFS", zv.empty() ? ze : (ze == "ok" ? zv : zv + " (" + ze + ")"));
        fila("daemon",
             doc["daemon_version"].toString().empty()
                 ? doc["daemon"].toString()
                 : doc["daemon_version"].toString() + " (" + doc["daemon"].toString() + ")");
    }
    for (const Aviso& a : avisos) {
        std::printf("%s %s\n", a.nivel == "error" ? "✕" : "▲", a.texto.c_str());
    }
    return estado != "error";
}

std::string remedioSinSudo(const B::ConnectionProfile& p) {
    const std::string id = p.id.empty() ? p.name : p.id;
    const std::string usuario = p.username.empty() ? std::string("?") : p.username;
    std::string como;
    if (B::toLowerAscii(p.osType).find("freebsd") != std::string::npos) {
        como = B::format(T("t_sin_sudo_freebsd",
                           " En FreeBSD, como root: «pkg install sudo», y con «visudo» quitar el "
                           "# de la línea «%wheel ALL=(ALL:ALL) ALL» (%1 tiene que estar en el "
                           "grupo wheel: «pw groupmod wheel -m %1»)."),
                         {usuario});
    }
    return B::format(T("t_sin_sudo",
                       "%1 no tiene sudo instalado, y la conexión dice que lo necesita: instálalo "
                       "y deja que %2 lo use, o quita sudo de la conexión («edit zfsm://%1 "
                       "--no-sudo») si %2 ya puede manejar ZFS.%3"),
                     {id, usuario, como});
}

}  // namespace zfsmgr::cli::detail
