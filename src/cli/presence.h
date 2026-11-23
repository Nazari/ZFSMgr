#pragma once

// The presence of this session in the daemons it reaches (src/daemon/serve.cpp, «Presence»):
// one thread per machine announces it every 30 s and says goodbye at the end, so any other
// session can know it is not alone. Only for an interactive session —the shell on a terminal,
// or the interface through the service it starts with `--presence gui:<pid>`—, never for a
// script.
//
// Each machine gets its own thread and its OWN background session, as the status monitor
// does: one that is off costs its own timeout, never the others' nor the shell's.

#include <memory>
#include <string>

namespace zfsmgr::cli {

struct Sesion;
struct Conexiones;

// Who this process announces: «cli» with its own PID, or «gui» with the interface's. Called
// once by main; until then nothing is announced.
void setPresence(const std::string& kind, long long pid);
bool presenceOn();
// The same for the whole process, across rebuilds: the daemons see one session, not one per
// change of connections.
const std::string& presenceId();

class Presencia {
public:
    // What it watches, as one string: when it changes the presence is rebuilt over the
    // current connections.
    static std::string huella(const Conexiones& conns);

    Presencia(const Sesion& ses, const Conexiones& conns);
    // Says goodbye to every machine (a few seconds at most) and ends the threads.
    ~Presencia();
    Presencia(const Presencia&) = delete;
    Presencia& operator=(const Presencia&) = delete;

private:
    struct Estado;
    std::shared_ptr<Estado> m_estado;
};

// Null when presence is off or the secrets are not open.
std::unique_ptr<Presencia> anunciaPresencia(const Sesion& ses, const Conexiones& conns);

}  // namespace zfsmgr::cli
