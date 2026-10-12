#pragma once

// What the interpreter's translation units (src/cli/shell_*.cpp) share. Internal to the
// shell: nothing outside them includes it. Produced by splitting the former single-file
// shell.cpp, keeping its order. What used to be its anonymous namespace is `detail`, made
// visible in `zfsmgr::cli` with a using-directive, which is what an anonymous namespace is:
// names here still hide same-named ones from other CLI headers.

#include "shell.h"

#include "agentversion.h"

#include "daemoninstall.h"
#include "daemonpayload.h"
#include "ayuda.h"
#include "creacion.h"
#include "gramatica_cli.h"
#include "connectionjson.h"
#include "gsa.h"
#include "copyspec.h"
#include "compresion.h"
#include "refreshparse.h"
#include "storefiles.h"
#include "syncing.h"
#include "transfer.h"
#include "zfsallow.h"
#include "zfsprops.h"
#include "helpers.h"
#include "linea.h"
#include "pantalla.h"
#include "agentmsg.h"
#include "json.h"
#include "processes.h"
#include "secretinput.h"
#include "strutil.h"
#include "tr.h"
#include "peers.h"
#include "advanced.h"
#include "listings.h"
#include "requests.h"
#include "pools.h"
#include "snapshots.h"
#include "datasets.h"
#include "transportcmd.h"
#include "transporttunnel.h"
#include "transportrpc.h"
#include "zfsmurl.h"

#ifndef _WIN32
#include <sys/ioctl.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <array>
#include <set>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <ctime>
#include <sstream>
#include <thread>
#include <string>
#include <vector>

// Duplicar descriptores, para capturar la salida de una orden. Mismas llamadas con otro
// nombre en Windows.
#ifdef _WIN32
#include <io.h>
#define DUP_ _dup
#define DUP2_ _dup2
#define CLOSE_ _close
#define FILENO_ _fileno
#else
#include <unistd.h>
#define DUP_ dup
#define DUP2_ dup2
#define CLOSE_ close
#define FILENO_ fileno
#endif

namespace zfsmgr::cli {
namespace detail {

namespace B = zfsmgr::base;

namespace H = zfsmgr::base::helpers;

namespace T = zfsmgr::base::transport;

namespace PR = zfsmgr::base::peers;

namespace AV = zfsmgr::commands::advanced;

namespace PL = zfsmgr::commands::pools;

// `INST` y no `IN`: en Windows `IN` es un MACRO de `windows.h`, y
// `namespace IN = …` no compila allí. Lo cazó el cruce de MinGW.
namespace INST = zfsmgr::commands::snapshots;

namespace DS = zfsmgr::commands::datasets;

namespace TR = zfsmgr::base::transfer;

namespace L = zfsmgr::base::listings;

namespace CS = zfsmgr::base::copyspec;

namespace PET = zfsmgr::commands::requests;

namespace SY = zfsmgr::base::syncing;

using B::ZfsmUrl;

using B::ZfsmKind;

// Qué clase de sitio es el actual. La RAÍZ no es una URL válida —`zfsm://` sin conexión no
// nombra nada—, así que se representa con la conexión vacía y solo existe dentro del
// intérprete, donde sí hace falta: es lo que da un sitio al que subir desde una conexión.
enum class Nodo { Raiz, Conexion, Dataset, Snapshot };

// El filtro de filas de `ls` cuando el ÚLTIMO tramo del destino es un patrón:
// `ls tank/user/M*` lista tank/user y deja solo las filas que casan con «M*». Ver
// ejecutaConPatrones.
struct FiltroFilas {
    // Qué filas mira. Un patrón en un tramo de ruta es de HIJOS: «*» no debe sacar las
    // instantáneas, que no son hijos sino `tank/x@algo`. Uno tras la `@` es de
    // INSTANTÁNEAS. El resto —conexiones, pools, propiedades, ficheros— es de todo.
    enum class Clase { Todo, Hijos, Instantaneas };
    std::string patron;  // el tramo CRUDO, con sus comodines; vacío = sin filtro
    Clase clase{Clase::Todo};
    std::string texto;  // lo que escribió el usuario, para el mensaje de «nada coincide»
    // Ficheros: como la shell, «*» no casa con los que empiezan por «.» salvo que el patrón
    // empiece también por «.».
    bool ocultos{false};
    // Con VARIOS destinos (`ls ds#content/*/x*`), una sección sin coincidencias no es un
    // error: se calla y lo apunta aquí. «nada coincide» se dice una vez, si no casa ninguna.
    bool enVarios{false};
    bool quedoVacio{false};
};

struct Estado {
    FiltroFilas filtro;
    // Las rutas del origen de `rsync` cuando las ha expandido un patrón (`#content/*.log`).
    // Vacío = las que diga la URL. Ver ejecutaConPatrones.
    std::vector<std::string> rsyncRelativas;
    // Y entonces cada una aterriza DENTRO del destino con su nombre, como
    // `rsync -a src/s* dst/` deja dst/s1 y dst/s2. Sin patrón, `#content/{a,b}` vuelca el
    // CONTENIDO de cada una en el destino, que es lo que ha hecho siempre.
    bool rsyncConNombre = false;
    // La versión de agente de cada conexión, recordada durante la sesión. Ver
    // listaConexiones: preguntarla cuesta una ida y vuelta por máquina.
    std::map<std::string, std::string> versionDaemon;
    // Lo que el daemon dice que le impide trabajar bien, por conexión: hoy, en macOS, que no
    // puede leer los discos («sin acceso al disco»). Vacío si nada.
    std::map<std::string, std::string> avisoDaemon;
    // Los pools importables por conexión, para el tabulador de `import`: la sonda tarda un
    // par de segundos y pulsar el tabulador dos veces no tiene por qué esperar dos veces.
    std::map<std::string,
             std::pair<std::chrono::steady_clock::time_point, std::vector<std::string>>>
        importablesCache;
    // Los pools de cada conexión. Ver destinoDePool: se usan para decidir si el primer
    // argumento suelto nombra un pool o es un argumento de la orden.
    std::map<std::string, std::set<std::string>> poolsPorConexion;
    // Las propiedades de cada sitio, para el completado. Ver propiedadesDe.
    std::map<std::string, std::vector<std::string>> propsPorSitio;
    // Datasets sin montar que esta sesión ha montado EN SOLO LECTURA en un temporal para
    // leer dentro (ver dondeEstaMontado). Se sueltan al salir de ellos y al terminar.
    struct MontajeLectura {
        std::string conexion;
        std::string dataset;
        std::string dir;
    };
    std::vector<MontajeLectura> montajesLectura;
    Sesion* ses{nullptr};
    Conexiones conns;
    ZfsmUrl actual;  // conexión vacía = raíz
    ZfsmUrl anterior;
    bool hayAnterior{false};
    Formato formato{Formato::Texto};
    bool asumirSi{false};  // `-y`: no preguntar antes de lo destructivo
    bool salir{false};
    int ultimoRc{0};
};

Nodo nodoDe(const ZfsmUrl& u);
std::string textoDe(const ZfsmUrl& u);
std::vector<std::string> tramosDe(const std::string& texto);
ZfsmUrl base_conexion_de(const Estado& e);
bool resuelveCrudo(const Estado& e, const std::string& textoEntrada, ZfsmUrl& out,
                   std::string& error);
bool resuelve(const Estado& e, const std::string& texto, ZfsmUrl& out, std::string& error);
B::ConnectionProfile conSudo(Estado& e, const B::ConnectionProfile& p);
std::string idDe(const Estado& e, const ZfsmUrl& u);
bool sshClaveFalta(const B::ConnectionProfile& p, std::string& ruta);
void recarga(Estado& e);
bool sshClaveFalta(const B::ConnectionProfile& p, std::string& ruta);
bool estaApartada(const Estado& e, const std::string& id);
const B::ConnectionProfile* perfilDe(const Estado& e, const ZfsmUrl& u, std::string& error);
const B::ConnectionProfile* perfilVivoDe(const Estado& e, const ZfsmUrl& u, std::string& error);
// Una operación de `zfs` por el verbo genérico, que recibe el argv en JSON y solo admite
// una lista cerrada de operaciones. Declarada aquí porque la usan órdenes de más arriba.
// Lo que una orden recibe: el destino ya resuelto y las ranuras que reconoció la gramática.
struct Peticion {
    ZfsmUrl objetivo;
    const LineaAnalizada* l{nullptr};
    const Orden* orden{nullptr};

    // Las banderas del mandato original que se han escrito, listas para añadir al argv.
    //
    // Se reconstruyen desde lo DECLARADO y no copiando lo que llegó: así lo que se le pasa
    // a `zfs`/`zpool` no puede ser nada que no esté en la lista, por mucho que cambie el
    // analizador.
    std::vector<std::string> nativas() const {
        std::vector<std::string> out;
        if (!orden) {
            return out;
        }
        for (const Nativa& n : orden->nativas) {
            const std::string corta = n.forma;
            const std::string larga =
                corta.size() > 2 && corta[1] == '-' ? corta.substr(2) : corta.substr(1);
            if (n.valor) {
                const auto it = l->opciones.find(larga);
                if (it != l->opciones.end()) {
                    out.push_back(corta);
                    out.push_back(it->second);
                }
                continue;
            }
            for (const std::string& b : l->banderas) {
                if (b == corta) {
                    out.push_back(corta);
                }
            }
            if (l->opciones.count(larga) > 0 && corta.rfind("--", 0) == 0) {
                out.push_back(corta);
            }
        }
        return out;
    }

    // Las ranuras llegan con NOMBRE desde la gramática; ya no hay que contar posiciones.
    std::vector<std::string> lista(const std::string& n) const { return l->lista(n); }
    std::string uno(const std::string& n) const { return l->uno(n); }
    bool tiene(const std::string& b) const { return l->tiene(b); }
    std::string valor(const std::string& k) const {
        std::string limpia = k;
        while (!limpia.empty() && limpia.front() == '-') {
            limpia.erase(limpia.begin());
        }
        const auto it = l->opciones.find(limpia);
        return it == l->opciones.end() ? std::string() : it->second;
    }
};

bool zfsGeneric(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv);
void avisaSinMetadatos(const std::string& salida);
bool zpoolGeneric(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv);
bool cmdCrearPool(Estado& e, const Peticion& pet, const ZfsmUrl& destino,
                  const std::string& nombre);
bool enviaComoTrabajo(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv);
bool lanzaOEspera(Estado& e, const Peticion& pet, const ZfsmUrl& destino,
                  const std::vector<std::string>& argv);
std::map<std::string, std::string> clavesDe(const std::string& texto);
bool agente(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& args,
            std::string& out, int timeoutMs = 60000, std::string* errorDeOrden = nullptr);
bool pide(const std::string& etiqueta, const std::string& porOmision, std::string& out);
bool encaja(const ZfsmUrl& u, Objetivo pedido);
std::string nombreDe(Objetivo pedido);
ZfsmUrl subeHasta(const ZfsmUrl& u, Objetivo pedido);
bool esSeccionDeFicheros(const ZfsmUrl& u);
bool esRutaSuelta(const ZfsmUrl& u);
bool equivaleADataset(Estado& e, ZfsmUrl& u, const std::string& comoSeEscribio);
bool prepara(Estado& e, const LineaAnalizada& linea, Peticion& p);
bool confirma(const Estado& e, const std::string& que);
bool aplicaFiltro(Estado& e, Tabla& t, std::initializer_list<std::size_t> columnas = {0});
void olvidaVersionDaemon(Estado& e, const B::ConnectionProfile& p);
void olvidaEstadoDeConexion(Estado& e, const B::ConnectionProfile& p);
bool listaConexiones(Estado& e, const Peticion& pet);
void avisaSondaImportables(const std::string& sonda, bool huboPools);
bool listaPools(Estado& e, const ZfsmUrl& destino);
bool listaDataset(Estado& e, const ZfsmUrl& destino);
bool listaPropiedades(Estado& e, const ZfsmUrl& destino);
bool listaPermisos(Estado& e, const ZfsmUrl& destino);
bool cmdPermisos(Estado& e, const LineaAnalizada& linea, bool conceder);
std::string montajeLecturaDe(const Estado& e, const std::string& conexion,
                             const std::string& dataset);
std::string montajeReal(Estado& e, const ZfsmUrl& u, const std::string& dataset);
bool dondeEstaMontado(Estado& e, const ZfsmUrl& u, const std::string& dataset, bool paraEscribir,
                      bool callado, std::string& ruta);
void sueltaMontajesLectura(Estado& e, bool todos);
bool rutaDeContenido(Estado& e, const ZfsmUrl& destino, std::string& base, bool& esWindows,
                     bool callado);
bool esSeccionDeFicheros(const ZfsmUrl& u);
bool esRutaSuelta(const ZfsmUrl& u);
bool rutaDeFicheros(Estado& e, const ZfsmUrl& u, std::string& ruta, bool callado);
struct Ubicacion {
    std::string ruta;
    bool existe{false};
    bool esDirectorio{false};  // siguiendo enlaces
    std::string tipo;          // d f l b c p s o
    long long tam{0};
    long long mtime{0};
    long long modo{0};
    std::string dueno;
    std::string grupo;
    std::string enlace;
    // El dataset montado que la contiene, si lo hay.
    std::string dataset;
    std::string relativa;
    std::string montaje;
    std::string instantanea;
    bool esMontaje{false};  // la ruta ES el punto de montaje (o la raíz de la instantánea)
};

bool ubicacionDe(Estado& e, const ZfsmUrl& u, Ubicacion& ub);
ZfsmUrl urlDeDataset(const std::string& conexion, const std::string& dataset,
                     const std::string& instantanea);
bool equivaleADataset(Estado& e, ZfsmUrl& u, const std::string& comoSeEscribio);
bool rutaDeRanura(Estado& e, const std::string& textoEntrada, const std::string& maquina,
                  std::string& ruta, std::string* conexion = nullptr);
bool discosDe(Estado& e, const std::vector<std::string>& textos, const std::string& maquina,
              std::vector<std::string>& out);
std::string modoComoLs(const std::string& tipo, long long modo);
std::string fechaCorta(long long segundos);
std::string nombreDeTipo(const std::string& letra);
bool imprimeFicheros(Estado& e, const std::vector<B::json::Value>& entradas);
bool listaFicheros(Estado& e, const ZfsmUrl& destino);
bool infoDeRuta(Estado& e, const ZfsmUrl& destino);
bool cmdLs(Estado& e, const LineaAnalizada& linea);
void vaA(Estado& e, const std::string& destino);
bool cmdCd(Estado& e, const LineaAnalizada& linea);
bool zfsGeneric(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv);
bool exigeDataset(const ZfsmUrl& u);
bool creaInstantanea(Estado& e, const Peticion& pet, const ZfsmUrl& destinoEntrada);
bool cmdDestroy(Estado& e, const LineaAnalizada& linea);
bool cmdRollback(Estado& e, const LineaAnalizada& linea);
bool cmdClone(Estado& e, const LineaAnalizada& linea);
bool cmdCrearConexion(Estado& e, const Peticion& pet);
bool cmdEditarConexion(Estado& e, const Peticion& pet, const ZfsmUrl& destino);
bool cmdEdit(Estado& e, const LineaAnalizada& linea);
bool cmdCreate(Estado& e, const LineaAnalizada& linea);
bool cmdRename(Estado& e, const LineaAnalizada& linea);
bool cmdMontaje(Estado& e, const LineaAnalizada& linea, bool montar);
bool cmdPromote(Estado& e, const LineaAnalizada& linea);
bool cmdSet(Estado& e, const LineaAnalizada& linea);
bool cmdGet(Estado& e, const LineaAnalizada& linea);
std::map<std::string, std::map<std::string, std::string>> agrupaGsa(const std::string& crudo);
bool leeProgramaciones(Estado& e, const ZfsmUrl& destino, const std::string& raiz,
                       std::vector<B::gsa::Entry>& out);
Tabla tablaDeProgramaciones(const std::vector<std::pair<std::string, B::gsa::Entry>>& filas);
const B::ConnectionProfile* laOtra(Estado& e, const Peticion& pet, std::string& error);
bool cmdAuthorizeKey(Estado& e, const LineaAnalizada& linea);
bool cmdExportTrust(Estado& e, const LineaAnalizada& linea);
bool cmdRepairMounts(Estado& e, const LineaAnalizada& linea);
bool cmdPeers(Estado& e, const LineaAnalizada& linea);
bool cmdLog(Estado& e, const LineaAnalizada& linea);
bool cmdSchedules(Estado& e, const LineaAnalizada& linea);
B::gsa::ConnectionState estadoDeProgramaciones(Sesion& ses, const B::ConnectionProfile& p,
                                               int timeoutMs);
std::string horaLocal(long long epoch);
std::string motivoDeFallo(const std::string& conexion, const B::gsa::DatasetState& d,
                          long long ultimaPasada);
bool cmdScheduleStatus(Estado& e, const LineaAnalizada& linea);
// The STARTUP check: the state of every connection's schedules, in the background, and one
// line that sums it up. Otherwise a levelling failing for days only shows in the agent's log.
//
// Each connection in its own thread with its OWN transport session, so the interpreter's
// tunnels are left alone and a machine that is off does not delay the others. Those sessions
// are muted and never prompt (crearSesionDeFondo): anything they printed or asked would land
// in the middle of the line being typed; what matters goes in the summary.
//
// The result goes to the line editor's notice queue (`encolaAviso`), which knows how to print
// it without breaking what is being typed.
class ComprobacionDeArranque {
public:
    ComprobacionDeArranque(const Sesion& ses, const Conexiones& conns) {
        std::vector<B::ConnectionProfile> perfiles;
        for (const auto& p : conns.perfiles) {
            const std::string id = p.id.empty() ? p.name : p.id;
            if (!conns.desconectada(id) && !conns.secretoSinAbrir(id)) {
                perfiles.push_back(p);
            }
        }
        if (perfiles.empty()) {
            return;
        }
        const std::string dirConfig = ses.dirConfig;
        const std::string maestra = ses.maestra;
        m_hilo = std::thread([perfiles, dirConfig, maestra]() {
            std::vector<B::gsa::ConnectionState> estados(perfiles.size());
            std::vector<std::thread> hilos;
            for (std::size_t i = 0; i < perfiles.size(); ++i) {
                hilos.emplace_back([&estados, &perfiles, &dirConfig, &maestra, i]() {
                    auto propia = crearSesionDeFondo(dirConfig, maestra);
                    estados[i] = estadoDeProgramaciones(*propia, perfiles[i], 10000);
                });
            }
            for (auto& h : hilos) {
                h.join();
            }
            encolaAviso(resumen(estados));
        });
    }
    // Al salir se espera a que acabe: cada pregunta tiene su plazo, y un hilo vivo mientras
    // el proceso destruye lo global acabaría mal.
    ~ComprobacionDeArranque() {
        if (m_hilo.joinable()) {
            m_hilo.join();
        }
    }

    static std::string resumen(const std::vector<B::gsa::ConnectionState>& estados) {
        std::vector<std::pair<std::string, B::gsa::DatasetState>> mal;
        std::vector<const B::gsa::ConnectionState*> sinComprobar;
        const B::gsa::Verdict v = B::gsa::summarize(estados, mal, sinComprobar);
        std::string texto;
        const int enCurso = B::gsa::levellingsInProgress(estados);
        if (v == B::gsa::Verdict::AllGreen) {
            texto = T("t_sch_arr_verde", "Todas las programaciones en verde");
            if (enCurso > 0) {
                texto += " "
                         + B::format(T("t_sch_arr_en_curso", "(%1 nivelándose)"),
                                     {std::to_string(enCurso)});
            }
        } else if (v == B::gsa::Verdict::NoSchedules) {
            texto = T("t_sch_arr_ninguna", "No hay programaciones");
        } else {
            texto = T("t_sch_arr_mal", "Programaciones con errores:");
            for (const auto& m : mal) {
                long long pasada = 0;
                for (const auto& c : estados) {
                    if (c.connection == m.first) {
                        pasada = c.lastPass;
                    }
                }
                texto += "\n  " + motivoDeFallo(m.first, m.second, pasada);
            }
        }
        for (const auto* c : sinComprobar) {
            texto += "\n  "
                     + B::format(T("t_sch_arr_sin", "sin comprobar: %1 (%2)"),
                                 {c->connection, c->whyNotChecked});
        }
        if (v == B::gsa::Verdict::WithErrors) {
            texto += "\n  " + T("t_sch_arr_detalle", "«schedule status -a» da el detalle");
        }
        return texto;
    }

private:
    std::thread m_hilo;
};

// La ZONA DE ESTADO de la pantalla: debajo de la línea de órdenes, siempre a la vista.
//
//   zfsm://local/back · programaciones en verde · ✗ <el último error>
//   trabajos: unib send tank/x@s 1.2G 85.0 MiB/s · local gsa-level back/x 300M
//
// Cada conexión tiene su hilo y su PROPIA sesión, como la comprobación del arranque: una
// máquina apagada no retrasa a las demás ni pisa los túneles del intérprete. Los trabajos se
// preguntan cada 2 s mientras haya alguno en marcha y cada 30 s si no; las programaciones,
// al arrancar y cada 5 minutos. Sustituye a la comprobación del arranque cuando hay
// pantalla: el veredicto se ve aquí, y si hay fallos se escriben además una vez.
class MonitorDeEstado {
public:
    MonitorDeEstado(const Sesion& ses, const Conexiones& conns, bool programaciones)
        : m_dirConfig(ses.dirConfig), m_maestra(ses.maestra), m_programaciones(programaciones) {
        for (const auto& p : conns.perfiles) {
            const std::string id = p.id.empty() ? p.name : p.id;
            if (!conns.desconectada(id) && !conns.secretoSinAbrir(id)) {
                m_perfiles.push_back(p);
            }
        }
        m_estado.resize(m_perfiles.size());
        for (std::size_t i = 0; i < m_perfiles.size(); ++i) {
            m_estado[i].prog.connection =
                m_perfiles[i].id.empty() ? m_perfiles[i].name : m_perfiles[i].id;
            m_hilos.emplace_back([this, i] { hilo(i); });
        }
        compone();
    }
    ~MonitorDeEstado() {
        {
            std::lock_guard<std::mutex> lock(m_m);
            m_parar = true;
        }
        m_cv.notify_all();
        for (auto& h : m_hilos) {
            if (h.joinable())
                h.join();
        }
    }

    // Tras cada orden: dónde se está y, si falló, su último error. Sale bien → se quita.
    void ponSitio(const std::string& url, int rc, const std::string& ultimoError,
                  bool tocoProgramaciones) {
        {
            std::lock_guard<std::mutex> lock(m_m);
            m_sitio = url;
            if (rc == 0) {
                m_error.clear();
            } else if (!ultimoError.empty()) {
                m_error = ultimoError;
            }
            // Una orden puede haber lanzado un trabajo: se pregunta YA, no a los 30 s. Y una
            // `schedule` cambia lo programado: también eso.
            ++m_aviso;
            if (tocoProgramaciones) {
                ++m_avisoProg;
            }
        }
        m_cv.notify_all();
        compone();
    }

private:
    struct PorConexion {
        std::vector<std::string> trabajos;
        B::gsa::ConnectionState prog;
        bool progHecha{false};
    };

    void hilo(std::size_t i) {
        auto ses = crearSesionDeFondo(m_dirConfig, m_maestra);
        const B::ConnectionProfile& p = m_perfiles[i];
        auto proxTrabajos = std::chrono::steady_clock::now();
        auto proxProg = proxTrabajos;
        unsigned avisoVisto = 0;
        unsigned avisoProgVisto = 0;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(m_m);
                if (m_parar)
                    return;
                if (m_aviso != avisoVisto) {
                    avisoVisto = m_aviso;
                    proxTrabajos = std::chrono::steady_clock::now();
                }
                if (m_avisoProg != avisoProgVisto) {
                    avisoProgVisto = m_avisoProg;
                    proxProg = std::chrono::steady_clock::now();
                }
            }
            const auto ahora = std::chrono::steady_clock::now();
            if (ahora >= proxTrabajos) {
                std::vector<std::string> trabajos;
                std::string out, err;
                int rc = -1;
                if (ejecutarAgente(*ses, p, PET::jobList(), out, err, rc, nullptr, 4000)
                    && rc == 0) {
                    for (const std::string& l : B::split(out, "\n", true)) {
                        if (!B::startsWith(l, "JOB="))
                            continue;
                        B::json::Value j;
                        std::string e;
                        if (!B::json::parse(l.substr(4), j, &e))
                            continue;
                        const std::string est = j["state"].toString();
                        if (est != "running" && est != "queued")
                            continue;
                        char ritmo[32];
                        std::snprintf(ritmo, sizeof(ritmo), "%.1f MiB/s", j["rate"].toDouble());
                        std::string t = m_estado[i].prog.connection + " " + j["type"].toString();
                        if (!j["snap"].toString().empty())
                            t += " " + j["snap"].toString();
                        if (est == "queued") {
                            t += " " + T("t_est_en_cola", "en cola");
                        } else {
                            t += " " + B::humanSize(std::to_string(j["bytes"].toInt())) + " "
                                 + ritmo;
                        }
                        trabajos.push_back(t);
                    }
                }
                bool acaboNivelacion = false;
                {
                    std::lock_guard<std::mutex> lock(m_m);
                    const auto nivelando = [](const std::vector<std::string>& v) {
                        for (const auto& x : v) {
                            if (x.find(" gsa-level ") != std::string::npos)
                                return true;
                        }
                        return false;
                    };
                    acaboNivelacion = nivelando(m_estado[i].trabajos) && !nivelando(trabajos);
                    m_estado[i].trabajos = trabajos;
                }
                // Una nivelación que acaba cambia el estado de su programación: se pregunta ya,
                // no dentro de cinco minutos.
                if (acaboNivelacion) {
                    proxProg = ahora;
                }
                proxTrabajos = ahora + std::chrono::seconds(trabajos.empty() ? 30 : 2);
                compone();
            }
            if (m_programaciones && ahora >= proxProg) {
                B::gsa::ConnectionState c = estadoDeProgramaciones(*ses, p, 8000);
                bool primeraRonda = false;
                {
                    std::lock_guard<std::mutex> lock(m_m);
                    m_estado[i].prog = c;
                    if (!m_estado[i].progHecha) {
                        m_estado[i].progHecha = true;
                        ++m_progHechas;
                        primeraRonda = m_progHechas == m_estado.size();
                    }
                }
                proxProg = ahora + std::chrono::minutes(5);
                compone();
                // Con fallos, la primera vez se escriben también: en la zona solo cabe el
                // resumen, y el detalle tiene que quedar en el historial.
                if (primeraRonda) {
                    std::vector<B::gsa::ConnectionState> todas;
                    {
                        std::lock_guard<std::mutex> lock(m_m);
                        for (const auto& x : m_estado)
                            todas.push_back(x.prog);
                    }
                    std::vector<std::pair<std::string, B::gsa::DatasetState>> mal;
                    std::vector<const B::gsa::ConnectionState*> sin;
                    if (B::gsa::summarize(todas, mal, sin) == B::gsa::Verdict::WithErrors) {
                        encolaAviso(ComprobacionDeArranque::resumen(todas));
                    }
                }
            }
            std::unique_lock<std::mutex> lock(m_m);
            m_cv.wait_for(lock, std::chrono::seconds(1),
                          [this, &avisoVisto] { return m_parar || m_aviso != avisoVisto; });
        }
    }

    void compone() {
        std::vector<std::string> lineas;
        {
            std::lock_guard<std::mutex> lock(m_m);
            std::string l = m_sitio.empty() ? std::string("zfsm://") : m_sitio;
            if (m_programaciones) {
                l += " · ";
                if (m_progHechas < m_estado.size()) {
                    l += T("t_est_prog_comprobando", "programaciones: comprobando…");
                } else {
                    std::vector<B::gsa::ConnectionState> todas;
                    for (const auto& x : m_estado)
                        todas.push_back(x.prog);
                    std::vector<std::pair<std::string, B::gsa::DatasetState>> mal;
                    std::vector<const B::gsa::ConnectionState*> sin;
                    const B::gsa::Verdict v = B::gsa::summarize(todas, mal, sin);
                    if (v == B::gsa::Verdict::AllGreen) {
                        l += T("t_est_prog_verde", "programaciones en verde");
                    } else if (v == B::gsa::Verdict::NoSchedules) {
                        l += T("t_est_prog_ninguna", "sin programaciones");
                    } else {
                        l += B::format(T("t_est_prog_mal", "programaciones con errores: %1"),
                                       {std::to_string(mal.size())});
                    }
                    if (!sin.empty()) {
                        std::string nombres;
                        for (const auto* c : sin)
                            nombres += (nombres.empty() ? "" : ", ") + c->connection;
                        l += " · "
                             + B::format(T("t_est_sin_comprobar", "sin comprobar: %1"), {nombres});
                    }
                }
            }
            if (!m_error.empty()) {
                l += " · ✗ " + m_error;
            }
            lineas.push_back(l);
            std::string t;
            for (const auto& x : m_estado) {
                for (const auto& tr : x.trabajos)
                    t += (t.empty() ? "" : " · ") + tr;
            }
            if (!t.empty()) {
                lineas.push_back(T("t_est_trabajos", "trabajos: ") + t);
            }
        }
        pantalla::ponEstado(lineas);
    }

    std::string m_dirConfig;
    std::string m_maestra;
    bool m_programaciones;
    std::vector<B::ConnectionProfile> m_perfiles;
    std::vector<PorConexion> m_estado;
    std::size_t m_progHechas{0};
    std::vector<std::thread> m_hilos;
    std::mutex m_m;
    std::condition_variable m_cv;
    bool m_parar{false};
    unsigned m_aviso{0};
    unsigned m_avisoProg{0};
    std::string m_sitio;
    std::string m_error;
};

std::string errorDeTrabajo(const std::string& codigo, const std::string& argsJson);
std::string errorDeTrabajo(const std::map<std::string, std::string>& k);
bool cmdScheduleRun(Estado& e, const ZfsmUrl& destino, bool esperar);
bool cmdSchedule(Estado& e, const LineaAnalizada& linea);
bool cmdClaves(Estado& e, const LineaAnalizada& linea, const char* op);
std::string preparaMontajeTemporal(Estado& e, const ZfsmUrl& d, bool creaHijos, bool& rechaza);
bool cmdBreakdown(Estado& e, const LineaAnalizada& linea);
bool cmdAssemble(Estado& e, const LineaAnalizada& linea);
bool cmdToDir(Estado& e, const LineaAnalizada& linea);
bool cmdFromDir(Estado& e, const LineaAnalizada& linea);
bool ficheroCompleto(Estado& e, const ZfsmUrl& donde, const std::string& ruta, long long bytes);
bool cmdSend(Estado& e, const LineaAnalizada& linea);
std::vector<std::string> trabajosEnCurso(Estado& e, const B::ConnectionProfile& p);
// --- El CLI de la otra máquina, junto con su daemon.
//
// Un daemon nuevo con un `zfsmgr-cli` viejo al lado es una máquina a medio actualizar, así
// que `install-daemon` pone al día también el CLI. Solo donde ya hay uno: instalarlo donde
// nadie lo pidió no es actualizar.
struct PlanCli {
    enum class Accion { NoHay, Windows, Enlace, EsEste, AlDia, PorDelante, Actualizar, SinBinario };
    Accion accion{Accion::NoHay};
    B::daemoninstall::CliProbe sonda;
    std::string binario;  // el de su plataforma, en este equipo
};

PlanCli planDelCli(Estado& e, const B::ConnectionProfile& perfil);
std::string textoDePlanCli(const PlanCli& plan);
bool actualizaCli(Estado& e, const B::ConnectionProfile& perfilConSudo, const PlanCli& plan);
bool cmdInstalarDaemon(Estado& e, const LineaAnalizada& linea);
std::map<std::string, std::string> clavesDe(const std::string& texto);
bool cmdJobs(Estado& e, const LineaAnalizada& linea);
bool cmdJob(Estado& e, const LineaAnalizada& linea);
bool lanzaOEspera(Estado& e, const Peticion& pet, const ZfsmUrl& destino,
                  const std::vector<std::string>& argv);
bool enviaComoTrabajo(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv);
bool zpoolGeneric(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& argv);
bool cmdMantenimientoPool(Estado& e, const LineaAnalizada& linea, const char* op);
bool cmdPoolSimple(Estado& e, const LineaAnalizada& linea, const char* verbo, const char* op,
                   const char* aviso);
bool poolImportable(Estado& e, const ZfsmUrl& conexion, const std::string& nombre,
                    H::ImportablePoolInfo& info, std::map<std::string, std::string>& rutas);
bool cmdStatus(Estado& e, const LineaAnalizada& linea);
bool cmdHistory(Estado& e, const LineaAnalizada& linea);
bool cmdImport(Estado& e, const LineaAnalizada& linea);
bool cmdCrearPool(Estado& e, const Peticion& pet, const ZfsmUrl& destino,
                  const std::string& nombre);
bool cmdEditarConexion(Estado& e, const Peticion& pet, const ZfsmUrl& destino);
std::string idDe(const Estado& e, const ZfsmUrl& u);
bool cmdConectar(Estado& e, const LineaAnalizada& linea, bool conectar);
bool cmdRefrescar(Estado& e, const LineaAnalizada& linea);
bool cmdHolds(Estado& e, const LineaAnalizada& linea);
bool cmdRetencion(Estado& e, const LineaAnalizada& linea, bool poner);
bool cmdDiff(Estado& e, const LineaAnalizada& lineaOriginal);
bool cmdDevices(Estado& e, const LineaAnalizada& linea);
bool montajeDe(Estado& e, const ZfsmUrl& u, std::string& out, bool paraEscribir);
void avisaSinMetadatos(const std::string& salida);
std::string hojaDe(const std::string& ruta);
bool estadoDeRuta(Estado& e, const std::string& conexion, const std::string& ruta, bool& existe,
                  bool& esDirectorio);
bool padreYHoja(const std::string& ruta, std::string& padre, std::string& hoja);
bool esFicheroEn(Estado& e, const ZfsmUrl& u, const std::string& ruta);
bool copiaArbol(Estado& e, const std::string& maqOrigen, const std::string& rutaOrigen,
                const std::string& maqDestino, const std::string& baseDestino,
                const std::string& nombre, bool simula, bool esperar);
bool cmdCp(Estado& e, const LineaAnalizada& linea);
bool cmdRsync(Estado& e, const LineaAnalizada& linea);
bool infoDeObjeto(Estado& e, const ZfsmUrl& destino);
bool cmdLabelclear(Estado& e, const LineaAnalizada& linea);
bool cmdInfo(Estado& e, const LineaAnalizada& linea);
int anchoTerminal();

}  // namespace detail
using namespace detail;

std::vector<std::string> hijosDe(Estado& e, const ZfsmUrl& u);
const std::vector<std::string>& propiedadesDe(Estado& e, const ZfsmUrl& donde);
std::vector<std::string> nombresDeContenido(Estado& e, const ZfsmUrl& destino);
std::vector<std::string> completaEn(Estado& e, const std::string& linea, std::size_t cursor,
                                    std::size_t& desde, bool& puedeSeguir);
// Una URL absoluta partida en sus piezas SIN descodificar.
struct UrlCruda {
    std::string conexion;
    std::vector<std::string> ruta;  // el pool y los datasets
    bool hayInstantanea{false};
    std::string instantanea;
    bool haySeccion{false};
    std::string seccion;
    std::vector<std::string> detalle;

    bool esRaiz() const { return conexion.empty(); }

    std::string texto() const {
        if (conexion.empty()) {
            return std::string();  // la raíz no es una URL; quien la use pone «/»
        }
        std::string t = "zfsm://" + conexion;
        if (!ruta.empty()) {
            t += "/" + B::join(ruta, "/");
        }
        if (hayInstantanea) {
            t += "@" + instantanea;
        }
        if (haySeccion) {
            t += "#" + seccion;
            for (const std::string& d : detalle) {
                t += "/" + d;
            }
        }
        return t;
    }
};

UrlCruda parteCruda(const std::string& absoluta);
bool absolutaCruda(const Estado& e, const std::string& textoEntrada, UrlCruda& out,
                   std::string& err);
// Qué es cada pieza de la URL, para saber qué hay que enumerar en ella y qué permite cada
// orden.
enum class Pieza { Conexion, Pool, Hijo, Instantanea, Seccion, Detalle };

// Lo que existe en cada nivel, para casarlo con un patrón. Una llamada al agente por
// nivel y padre, recordada mientras dura la expansión.
class Enumerador {
public:
    explicit Enumerador(Estado& e) : e_(e) {}

    std::vector<std::string> conexiones() {
        std::vector<std::string> v;
        for (const auto& p : e_.conns.perfiles) {
            v.push_back(p.id.empty() ? p.name : p.id);
        }
        return v;
    }

    // El nombre visible de una conexión, para casar también por él.
    std::string nombreDe(const std::string& id) {
        const auto* p = buscarConexion(e_.conns, id);
        return p ? p->name : std::string();
    }

    std::vector<std::string> pools(const UrlCruda& prefijo) {
        return enCache("pools|" + prefijo.texto(), [&](std::vector<std::string>& v) {
            ZfsmUrl u;
            std::string out;
            if (!resolver(prefijo, u) || !agente(e_, u, PET::poolList(), out)) {
                return false;
            }
            B::json::Value raiz;
            if (!B::trim(out).empty() && B::json::parse(out, raiz, nullptr)) {
                for (const auto& kv : raiz["pools"].toObject()) {
                    v.push_back(kv.second["name"].toString(kv.first));
                }
            }
            return true;
        });
    }

    std::vector<std::string> hijos(const UrlCruda& prefijo) { return deListado(prefijo, false); }
    std::vector<std::string> instantaneas(const UrlCruda& prefijo) {
        return deListado(prefijo, true);
    }

    std::vector<std::string> propiedades(const UrlCruda& prefijo) {
        return enCache("props|" + prefijo.texto(), [&](std::vector<std::string>& v) {
            ZfsmUrl u;
            std::string out;
            if (!resolver(prefijo, u) || !agente(e_, u, PET::datasetProperties(u.zfsName()), out)) {
                return false;
            }
            B::json::Value raiz;
            if (B::json::parse(out, raiz, nullptr)) {
                for (const auto& ds : raiz["datasets"].toObject()) {
                    for (const auto& pr : ds.second["properties"].toObject()) {
                        v.push_back(pr.first);
                    }
                }
            }
            return true;
        });
    }

    // `soloDirectorios`: cuando detrás quedan más tramos, un fichero no puede ser parte del
    // camino —`alfa/*/x.log`—, y pedir su contenido daba «Not a directory» por cada uno.
    std::vector<std::string> contenido(const UrlCruda& prefijo, bool soloDirectorios = false) {
        const std::vector<std::string> todos = contenidoTipado(prefijo);
        std::vector<std::string> v;
        for (const std::string& t : todos) {
            const bool esDir = !t.empty() && t.back() == '/';
            if (soloDirectorios && !esDir) {
                continue;
            }
            v.push_back(esDir ? t.substr(0, t.size() - 1) : t);
        }
        return v;
    }

    // Los nombres de un directorio, con «/» al final los que son directorios.
    std::vector<std::string> contenidoTipado(const UrlCruda& prefijo) {
        return enCache("cont|" + prefijo.texto(), [&](std::vector<std::string>& v) {
            ZfsmUrl u;
            if (!resolver(prefijo, u)) {
                return false;
            }
            // `#content` o `#path`: la ruta sale de rutaDeFicheros y los nombres, del mismo
            // verbo para las dos.
            std::string base;
            std::string out;
            if (!rutaDeFicheros(e_, u, base, /*callado=*/false)
                || !agente(e_, u, PET::pathContents(base), out)) {
                return false;
            }
            std::vector<L::DirectoryEntry> entradas;
            std::string errJson;
            if (L::directoryContents(out, entradas, errJson)) {
                for (const L::DirectoryEntry& ent : entradas) {
                    v.push_back(ent.directory ? ent.name + "/" : ent.name);
                }
            }
            return true;
        });
    }

    bool fallo() const { return fallo_; }

private:
    bool resolver(const UrlCruda& prefijo, ZfsmUrl& u) {
        std::string err;
        if (!resuelve(e_, prefijo.texto(), u, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
        return true;
    }

    template <typename F>
    std::vector<std::string> enCache(const std::string& clave, F&& llena) {
        const auto ya = cache_.find(clave);
        if (ya != cache_.end()) {
            return ya->second;
        }
        std::vector<std::string> v;
        if (!llena(v)) {
            fallo_ = true;
        }
        cache_[clave] = v;
        return v;
    }

    // Los hijos directos o las instantáneas de un dataset, del mismo listado recursivo que
    // usa `ls`: una sola llamada sirve para los dos.
    std::vector<std::string> deListado(const UrlCruda& prefijo, bool snaps) {
        const std::string clave = std::string(snaps ? "snaps|" : "hijos|") + prefijo.texto();
        return enCache(clave, [&](std::vector<std::string>& v) {
            ZfsmUrl u;
            std::string out;
            if (!resolver(prefijo, u) || !agente(e_, u, PET::datasetList(u.dataset), out)) {
                return false;
            }
            const std::string padre = u.dataset;
            for (const std::string& linea : B::split(out, "\n", true)) {
                const std::string nombre = B::trim(B::split(linea, "\t", false).front());
                if (snaps) {
                    if (B::startsWith(nombre, padre + "@")) {
                        v.push_back(nombre.substr(padre.size() + 1));
                    }
                } else if (B::startsWith(nombre, padre + "/")) {
                    const std::string resto = nombre.substr(padre.size() + 1);
                    if (resto.find('/') == std::string::npos
                        && resto.find('@') == std::string::npos) {
                        v.push_back(resto);
                    }
                }
            }
            return true;
        });
    }

    Estado& e_;
    std::map<std::string, std::vector<std::string>> cache_;
    bool fallo_{false};
};

// Un destino ya concreto, con el filtro de filas que lleve (vacío si no lleva).
struct DestinoConcreto {
    std::string url;  // vacío = la raíz
    FiltroFilas filtro;
    std::string etiqueta() const {
        return url.empty() ? std::string("zfsm://") : url.substr(std::string("zfsm://").size());
    }
};

struct PiezaCruda {
    Pieza clase;
    std::string crudo;
};

std::vector<PiezaCruda> piezasDe(const UrlCruda& u);
bool expandeDestinos(Estado& e, const UrlCruda& u, bool filtraUltimo,
                     std::vector<DestinoConcreto>& out, std::string& err);
bool expandeRutaLocal(const std::string& ruta, std::vector<std::string>& out, std::string& err);
bool expandeUnico(Estado& e, const std::string& texto, const std::string& orden, bool rutaLocal,
                  std::string& concreto);
// Qué hace cada orden con un patrón en su destino. Las que no están, lo rechazan.
enum class Politica { Filtra, Uno, Varios, VariosModifica };

struct ReglaPatron {
    Politica politica;
    // En las que modifican: en qué pieza puede ir el patrón. Es la regla que impide un
    // `destroy` sobre varios pools o un `scrub` sobre varias máquinas por un despiste.
    std::set<Pieza> piezas;
};

const std::map<std::string, ReglaPatron>& reglasDePatron();
// Lo que una orden escribe en la salida estándar mientras se ejecuta, capturado. Hace falta
// para repetir una orden sobre varios destinos: lo que imprime con `Tabla` se junta en una
// tabla, y lo que imprime a mano —`status`, `history`— se enseña en una sección con su
// destino delante. Sin capturar no se sabría cuál de las dos cosas hizo cada repetición.
class CapturaSalida {
public:
    CapturaSalida() {
        std::fflush(stdout);
        tmp_ = std::tmpfile();
        if (!tmp_) {
            return;
        }
        copia_ = DUP_(FILENO_(stdout));
        DUP2_(FILENO_(tmp_), FILENO_(stdout));
    }
    std::string termina() {
        if (!tmp_) {
            return {};
        }
        std::fflush(stdout);
        DUP2_(copia_, FILENO_(stdout));
        CLOSE_(copia_);
        std::string texto;
        std::rewind(tmp_);
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), tmp_)) > 0) {
            texto.append(buf, n);
        }
        std::fclose(tmp_);
        tmp_ = nullptr;
        return texto;
    }
    ~CapturaSalida() { termina(); }

private:
    std::FILE* tmp_{nullptr};
    int copia_{-1};
};

std::string nombreDeLlegada(const std::string& origen);
bool instalaDaemonEnTodas(Estado& e, const LineaAnalizada& an,
                          const std::function<bool(Estado&, const LineaAnalizada&)>& f);
bool ejecutaConPatrones(Estado& e, const std::string& orden, const LineaAnalizada& original,
                        const std::function<bool(Estado&, const LineaAnalizada&)>& f);
int ejecutarShell(Sesion& ses, Formato formato, const std::string& urlInicial, bool asumirSi,
                  bool compruebaProgramaciones, bool pantallaCompleta);

}  // namespace zfsmgr::cli
