// Confirmations: the question asked before something a person should think about first.
//
// It says what is about to happen, what is lost (computed from the machine where it can be:
// size, children, snapshots made by hand, where it is mounted) and what stays (the copies
// other machines keep). For what cannot be undone, the answer is the NAME typed out, not a
// «y»: a yes/no gets answered without reading. The terminal and --serve show the same
// question; the client never rewrites it.

#include "sessionlock.h"
#include "shell_internal.h"

#include <set>

namespace zfsmgr::cli::detail {

// «a, b, c y 4 más»: a list that does not flood the question.
std::string algunos(const std::vector<std::string>& nombres, std::size_t cuantos) {
    std::vector<std::string> v(nombres.begin(),
                               nombres.begin() + std::min(cuantos, nombres.size()));
    std::string s = B::join(v, ", ");
    if (nombres.size() > cuantos) {
        s += B::format(T("t_pq_y_mas", " y %1 más"), {std::to_string(nombres.size() - cuantos)});
    }
    return s;
}

std::string palabraDe(const std::string& nombre) {
    std::string n = nombre;
    const auto arroba = n.find('@');
    if (arroba != std::string::npos) {
        n = n.substr(0, arroba);
    }
    const auto barra = n.find_last_of('/');
    return barra == std::string::npos ? n : n.substr(barra + 1);
}

std::string textoDePregunta(const Pregunta& p) {
    std::string t = p.titulo + "\n";
    if (p.irreversible) {
        t += T("t_pq_irreversible", "Esto no se puede deshacer.") + "\n";
    }
    const auto lista = [&t](const std::string& cabecera, const std::vector<std::string>& v) {
        if (v.empty()) {
            return;
        }
        t += "\n" + cabecera + "\n";
        for (const std::string& l : v) {
            t += "  · " + l + "\n";
        }
    };
    lista(p.irreversible ? T("t_pq_se_pierde_siempre", "Se pierde para siempre:")
                         : T("t_pq_se_pierde", "Se pierde:"),
          p.sePierde);
    lista(T("t_pq_se_conserva", "Se conserva:"), p.seConserva);
    if (!p.notas.empty()) {
        t += "\n";
        for (const std::string& n : p.notas) {
            t += n + "\n";
        }
    }
    return t;
}

namespace {

// ZFSMgr open on another machine that reaches what this question changes: one note each, so
// the person knows someone else will see it change. Asked only of those machines' daemons, with
// a short timeout: a daemon that does not answer only leaves the question without the note.
std::vector<std::string> notasDeOtrasSesiones(const Estado& e, const Pregunta& p) {
    if (p.maquinas.empty() || !e.ses || e.ses->sinSecretos) {
        return {};
    }
    Conexiones afectadas = e.conns;
    afectadas.perfiles.clear();
    for (const std::string& m : p.maquinas) {
        if (const auto* perfil = buscarConexion(e.conns, m)) {
            afectadas.perfiles.push_back(*perfil);
        }
    }
    if (afectadas.perfiles.empty()) {
        return {};
    }
    const auto otras = zfsmgr::commands::sessions::othersThan(
        sesionesEnLaRed(*e.ses, afectadas, 5000), zfsmgr::base::session::hostName());
    std::vector<std::string> notas;
    for (const auto& s : otras) {
        std::string llega;
        for (const std::string& m : s.machines) {
            llega += (llega.empty() ? "" : ", ") + m;
        }
        notas.push_back(B::format(T("t_pq_otra_sesion",
                                    "Ojo: ZFSMgr también está abierto en otra máquina que llega a "
                                    "%1: %2. Verá este cambio sin haberlo pedido; «sessions close "
                                    "%3» le pide que se cierre."),
                                  {llega, describeSesion(s), s.host}));
    }
    return notas;
}

}  // namespace

bool confirma(const Estado& e, const Pregunta& pregunta) {
    if (e.asumirSi) {
        return true;
    }
    // Only when there is someone to ask: without a terminal nor a client it is refused below
    // anyway, and asking the daemons would be time thrown away.
    Pregunta p = pregunta;
    if (e.preguntaServicio || hayTerminal()) {
        for (std::string& n : notasDeOtrasSesiones(e, p)) {
            p.notas.push_back(std::move(n));
        }
    }
    if (e.preguntaServicio) {
        return e.preguntaServicio(p);
    }
    if (!hayTerminal()) {
        std::fprintf(stderr, "%s\n",
                     B::format(T("t_pq_sin_terminal",
                                 "%1: hace falta confirmarlo y no hay terminal; usa -y si "
                                 "estás seguro"),
                               {p.titulo})
                         .c_str());
        return false;
    }
    std::fputs(textoDePregunta(p).c_str(), stderr);
    std::string resp;
    std::string err;
    if (!p.escribir.empty()) {
        if (!preguntarPorTerminal(
                "\n" + B::format(T("t_pq_escribe", "Escribe «%1» para confirmar: "), {p.escribir}),
                resp, err)) {
            return false;
        }
        if (B::trim(resp) != p.escribir) {
            if (!B::trim(resp).empty()) {
                std::fprintf(
                    stderr, "%s\n",
                    B::format(T("t_pq_no_coincide", "no es «%1»: no se hace nada"), {p.escribir})
                        .c_str());
            }
            return false;
        }
        return true;
    }
    if (!preguntarPorTerminal("\n" + T("t_pq_seguir", "¿Seguir? [s/N]: "), resp, err)) {
        return false;
    }
    const std::string r = B::toLowerAscii(B::trim(resp));
    return r == "s" || r == "si" || r == "sí" || r == "y" || r == "yes";
}

std::string impactoDeDestruir(Estado& e, const ZfsmUrl& donde, const std::string& ds,
                              bool recursivo, Pregunta& p) {
    std::string out;
    // Quiet: a failure here only leaves the question shorter; the destroy itself will say
    // what is wrong.
    std::string err;
    int rc = -1;
    const auto* perfil = buscarConexion(e.conns, donde.connection);
    if (!perfil
        || !ejecutarAgente(*e.ses, *perfil, PET::datasetList(ds), out, err, rc, nullptr, 20000)
        || rc != 0) {
        return {};
    }
    const auto filas = L::entries(out);
    const std::string prefijo = ds + "/";
    std::string usado;
    std::vector<std::string> hijos;
    std::vector<std::string> aMano;
    std::vector<std::string> montados;
    std::size_t snaps = 0;
    bool loImpide = false;
    for (const auto& f : filas) {
        const std::string base = f.isSnapshot() ? f.name.substr(0, f.name.find('@')) : f.name;
        const bool propio = base == ds;
        const bool debajo = B::startsWith(base, prefijo);
        if (!recursivo && ((propio && f.isSnapshot()) || (debajo && !f.isSnapshot()))) {
            loImpide = true;
        }
        if (!propio && !(recursivo && debajo)) {
            continue;
        }
        if (f.isSnapshot()) {
            ++snaps;
            const std::string corto = f.name.substr(f.name.find('@') + 1);
            if (!B::startsWith(corto, "GSA-")) {
                aMano.push_back((propio ? std::string() : base.substr(prefijo.size())) + "@"
                                + corto);
            }
            continue;
        }
        if (propio) {
            usado = f.used;
        } else {
            hijos.push_back(f.name.substr(prefijo.size()));
        }
        if (f.mounted == "yes" && !f.mountpoint.empty() && f.mountpoint != "-") {
            montados.push_back(f.mountpoint);
        }
    }
    if (loImpide) {
        return B::format(T("t_pq_hace_falta_r",
                           "%1 tiene snapshots o datasets hijos, y ZFS no lo destruye sin "
                           "ellos: para llevártelo todo, «destroy -r»"),
                         {ds});
    }
    if (!usado.empty()) {
        p.sePierde.push_back(
            hijos.empty()
                ? B::format(T("t_pq_datos", "%1 de datos"), {B::humanSize(usado)})
                : B::format(T("t_pq_datos_hijos", "%1 de datos, con sus %2 datasets hijos (%3)"),
                            {B::humanSize(usado), std::to_string(hijos.size()), algunos(hijos)}));
    }
    if (snaps > 0) {
        std::string linea =
            snaps == 1 ? T("t_pq_snap_uno", "su snapshot")
                       : B::format(T("t_pq_snaps", "sus %1 snapshots"), {std::to_string(snaps)});
        if (aMano.size() == 1) {
            linea += B::format(T("t_pq_snaps_mano_uno", ", uno hecho a mano: %1"), {aMano[0]});
        } else if (!aMano.empty()) {
            linea += B::format(T("t_pq_snaps_mano", ", %1 hechos a mano: %2"),
                               {std::to_string(aMano.size()), algunos(aMano)});
        }
        p.sePierde.push_back(linea);
    }
    // Only the top ones: a child mounted under its parent says nothing new.
    {
        std::vector<std::string> arriba;
        for (const std::string& m : montados) {
            bool debajoDeOtro = false;
            for (const std::string& o : montados) {
                if (o != m && B::startsWith(m, o == "/" ? o : o + "/")) {
                    debajoDeOtro = true;
                }
            }
            if (!debajoDeOtro) {
                arriba.push_back(m);
            }
        }
        montados = arriba;
    }
    if (!montados.empty()) {
        p.sePierde.push_back(B::format(T("t_pq_montado",
                                         "está montado en %1: lo que use esos ficheros dejará "
                                         "de encontrarlos"),
                                       {algunos(montados, 2)}));
    }

    // The copies other machines keep: those that pull it (their holds say so) and the one it
    // is sent to, if it has a schedule that levels.
    out.clear();
    if (ejecutarAgente(*e.ses, *perfil, PET::gsaPullers(ds), out, err, rc, nullptr, 20000)
        && rc == 0) {
        std::set<std::string> vistas;
        for (const std::string& l : B::split(out, "\n", true)) {
            const std::vector<std::string> c = B::split(l, "\t", false);
            if (c.empty() || !B::startsWith(c[0], B::gsa::kPullHoldPrefix)) {
                continue;
            }
            const std::string resto = c[0].substr(std::string(B::gsa::kPullHoldPrefix).size());
            const std::size_t dos = resto.find(':');
            std::string maquina = resto.substr(0, dos);
            for (const auto& q : e.conns.perfiles) {
                if (!q.machineUid.empty() && q.machineUid == maquina) {
                    maquina = q.id.empty() ? q.name : q.id;
                }
            }
            const std::string copia =
                dos == std::string::npos ? std::string() : resto.substr(dos + 1);
            if (vistas.insert(maquina + copia).second) {
                p.seConserva.push_back(B::format(
                    T("t_pq_copia_traida", "la copia que trae %1 (%2)"), {maquina, copia}));
            }
        }
    }
    out.clear();
    if (ejecutarAgente(*e.ses, *perfil, PET::gsaOfDataset(ds), out, err, rc, nullptr, 20000)
        && rc == 0) {
        for (const auto& kv : B::gsa::groupProperties(out)) {
            B::gsa::Schedule pr;
            B::gsa::Reason por;
            if (!B::gsa::fromProperties(kv.second, pr, por)) {
                continue;
            }
            if (pr.level && !pr.target.empty()
                && (kv.first == ds || (recursivo && B::startsWith(kv.first, prefijo)))) {
                p.seConserva.push_back(
                    B::format(T("t_pq_copia_enviada", "la copia que se envía a %1"), {pr.target}));
            }
        }
    }
    return {};
}

}  // namespace zfsmgr::cli::detail
