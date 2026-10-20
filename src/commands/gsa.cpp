#include "gsa.h"
#include "i18n.h"

#include "strutil.h"
#include "agentmsg.h"
#include "json.h"
#include "zfsmurl.h"

#include <algorithm>
#include <ctime>
#include <cctype>

namespace zfsmgr::base::gsa {
namespace {

std::string bajo(const std::string& s) { return toLowerAscii(trim(s)); }

// «on», «yes», «true» y «1». Cualquier otra cosa es «off»: ante un valor que no se
// entiende, lo conservador es NO programar.
bool esOn(const std::string& v) {
    const std::string s = bajo(v);
    return s == "on" || s == "yes" || s == "true" || s == "1";
}

// Vacío es 0 —una retención sin poner es «no guardes ninguna»—, y lo demás tiene que ser
// un entero >= 0. Un «7d» o un «-1» son un error, no un 7 ni un 0.
bool enteroNoNegativo(const std::string& v, int& out) {
    const std::string s = trim(v);
    if (s.empty() || s == "-") {
        out = 0;
        return true;
    }
    if (s.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    try {
        const long n = std::stol(s);
        if (n < 0 || n > 1000000000L) {
            return false;
        }
        out = static_cast<int>(n);
    } catch (...) {
        return false;
    }
    return true;
}

// Busca la propiedad sin distinguir mayúsculas, que es como lo hacía la interfaz.
std::string valorDe(const std::map<std::string, std::string>& props, const std::string& nombre) {
    const std::string buscado = bajo(nombre);
    for (const auto& kv : props) {
        if (bajo(kv.first) == buscado) {
            return kv.second;
        }
    }
    return {};
}

const char* const kActivado = "org.zfsmgr:schedule-enabled";
const char* const kRecursivo = "org.zfsmgr:schedule-recursive";
const char* const kNivelar = "org.zfsmgr:schedule-level";
const char* const kDestino = "org.zfsmgr:schedule-target";
const char* const kHorario = "org.zfsmgr:schedule-hourly";
const char* const kDiario = "org.zfsmgr:schedule-daily";
const char* const kSemanal = "org.zfsmgr:schedule-weekly";
const char* const kMensual = "org.zfsmgr:schedule-monthly";
const char* const kAnual = "org.zfsmgr:schedule-yearly";

}  // namespace

const char* const kPropertyPrefix = "org.zfsmgr:schedule-";

bool fromProperties(const std::map<std::string, std::string>& props, Schedule& out,
                      Reason& porQue) {
    porQue = Reason{};
    Schedule p;
    p.enabled = esOn(valorDe(props, kActivado));
    p.recursive = esOn(valorDe(props, kRecursivo));
    p.level = esOn(valorDe(props, kNivelar));
    p.target = trim(valorDe(props, kDestino));
    if (p.target == "-") {
        p.target.clear();
    }
    const std::pair<const char*, int*> retenciones[] = {
        {kHorario, &p.hourly}, {kDiario, &p.daily},   {kSemanal, &p.weekly},
        {kMensual, &p.monthly}, {kAnual, &p.yearly},
    };
    for (const auto& r : retenciones) {
        if (!enteroNoNegativo(valorDe(props, r.first), *r.second)) {
            porQue.failure = Failure::RetentionNotAnInteger;
            porQue.detail = r.first;
            return false;
        }
    }
    out = p;
    return true;
}

std::map<std::string, std::string> toProperties(const Schedule& p) {
    return {
        {kActivado, p.enabled ? "on" : "off"},
        {kRecursivo, p.recursive ? "on" : "off"},
        {kNivelar, p.level ? "on" : "off"},
        {kDestino, p.target},
        {kHorario, std::to_string(p.hourly)},
        {kDiario, std::to_string(p.daily)},
        {kSemanal, std::to_string(p.weekly)},
        {kMensual, std::to_string(p.monthly)},
        {kAnual, std::to_string(p.yearly)},
    };
}

bool isValid(const std::string& dataset, const Schedule& p,
            const std::function<bool(const std::string&)>& conexionExiste, Reason& porQue) {
    porQue = Reason{};
    porQue.dataset = dataset;

    // El destino se comprueba si NIVELAR está puesto —que lo exige— o si hay destino
    // escrito estando la programación activada. Un destino escrito con la programación
    // apagada no molesta a nadie.
    const bool hayQueMirarDestino = p.level || (p.enabled && !p.target.empty());
    if (p.level && p.target.empty()) {
        porQue.failure = Failure::LevelWithNoTarget;
        return false;
    }
    if (hayQueMirarDestino) {
        std::string conexion;
        std::string dataset;
        if (!splitTarget(p.target, conexion, dataset)) {
            porQue.failure = Failure::MalformedTarget;
            porQue.detail = p.target;
            return false;
        }
        if (conexionExiste && !conexionExiste(conexion)) {
            porQue.failure = Failure::TargetHasNoConnection;
            porQue.detail = conexion;
            return false;
        }
    }
    // Activada y sin ninguna retención es una programación que no guarda nada: hace la
    // instantánea y la borra. Casi siempre es un olvido, y callarlo deja al usuario
    // creyendo que tiene copias.
    if (p.enabled && p.hasNoRetentions()) {
        porQue.failure = Failure::EnabledWithNoRetention;
        return false;
    }
    porQue.dataset.clear();
    return true;
}

bool isSameOrDescendant(const std::string& dataset, const std::string& ancestro) {
    const std::string d = trim(dataset);
    const std::string a = trim(ancestro);
    return d == a || startsWith(d, a + "/");
}

bool isValidSet(const std::vector<Entry>& delMismoPool, Reason& porQue) {
    porQue = Reason{};
    // Solo las ACTIVADAS chocan: una programación apagada no hace instantáneas, así que
    // solaparse con ella no significa nada.
    std::vector<const Entry*> vivas;
    for (const Entry& e : delMismoPool) {
        if (e.schedule.enabled) {
            vivas.push_back(&e);
        }
    }
    for (std::size_t i = 0; i < vivas.size(); ++i) {
        for (std::size_t j = i + 1; j < vivas.size(); ++j) {
            const Entry& a = *vivas[i];
            const Entry& b = *vivas[j];
            if (trim(a.dataset) == trim(b.dataset)) {
                continue;
            }
            if (a.schedule.recursive && isSameOrDescendant(b.dataset, a.dataset)) {
                porQue.failure = Failure::ClashesWithRecursive;
                porQue.dataset = b.dataset;
                porQue.detail = a.dataset;
                return false;
            }
            if (b.schedule.recursive && isSameOrDescendant(a.dataset, b.dataset)) {
                porQue.failure = Failure::ClashesWithRecursive;
                porQue.dataset = a.dataset;
                porQue.detail = b.dataset;
                return false;
            }
        }
    }
    return true;
}

std::string labelOf(Failure f) {
    switch (f) {
        case Failure::None_:
            return {};
        case Failure::RetentionNotAnInteger:
            return i18n::tr("t_cmd_gsa_retencion_es_entero_mayor", "la retención no es un entero mayor o igual que 0");
        case Failure::EnabledWithNoRetention:
            return i18n::tr("t_cmd_gsa_programacion_esta_activada_guarda", "la programación está activada y no guarda ningún snapshot");
        case Failure::LevelWithNoTarget:
            return i18n::tr("t_cmd_gsa_nivelar_esta_puesto_hay", "nivelar está puesto y no hay destino");
        case Failure::MalformedTarget:
            return i18n::tr("t_cmd_gsa_destino_url", "el destino tiene que ser la URL de un dataset: zfsm://<conexión>/<pool>/<dataset>");
        case Failure::TargetHasNoConnection:
            return i18n::tr("t_cmd_gsa_destino_nombra_conexion_existe", "el destino nombra una conexión que no existe");
        case Failure::ClashesWithRecursive:
            return i18n::tr("t_cmd_gsa_ya_hay_programacion_recursiva", "ya hay una programación recursiva que lo cubre");
    }
    return {};
}

std::string snapshotClass(const std::string& nombre) {
    const std::string s = trim(nombre);
    if (s.size() < 4 || bajo(s.substr(0, 4)) != "gsa-") {
        return {};
    }
    const std::size_t primero = s.find('-');
    const std::size_t segundo = s.find('-', primero + 1);
    if (primero == std::string::npos || segundo == std::string::npos || segundo <= primero + 1) {
        return {};
    }
    return bajo(s.substr(primero + 1, segundo - primero - 1));
}

std::vector<std::pair<std::string, std::vector<std::string>>> groupSnapshots(
    const std::vector<std::string>& nombres) {
    std::vector<std::string> manuales;
    // Un vector de pares y no un mapa: el orden de las clases DESCONOCIDAS tiene que ser
    // el de aparición, y un mapa lo perdería o lo ordenaría alfabéticamente.
    std::vector<std::pair<std::string, std::vector<std::string>>> porClase;
    for (const std::string& raw : nombres) {
        const std::string n = trim(raw);
        if (n.empty()) {
            continue;
        }
        const std::string klass = snapshotClass(n);
        if (klass.empty()) {
            manuales.push_back(n);
            continue;
        }
        auto it = std::find_if(porClase.begin(), porClase.end(),
                               [&klass](const auto& p) { return p.first == klass; });
        if (it == porClase.end()) {
            porClase.push_back({klass, {n}});
        } else {
            it->second.push_back(n);
        }
    }
    // De la hora al año. Lo que no esté en la lista va detrás, en el orden en que apareció.
    static const char* const kOrden[] = {"hourly", "daily", "weekly", "monthly", "yearly"};
    const auto peso = [](const std::string& klass) {
        for (std::size_t i = 0; i < sizeof(kOrden) / sizeof(kOrden[0]); ++i) {
            if (klass == kOrden[i]) {
                return static_cast<int>(i);
            }
        }
        return 1000;
    };
    std::stable_sort(porClase.begin(), porClase.end(),
                     [&peso](const auto& a, const auto& b) { return peso(a.first) < peso(b.first); });

    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    if (!manuales.empty()) {
        out.push_back({std::string(), manuales});
    }
    for (auto& p : porClase) {
        out.push_back(std::move(p));
    }
    return out;
}

}  // namespace zfsmgr::base::gsa

namespace zfsmgr::base::gsa {

}  // namespace zfsmgr::base::gsa

namespace zfsmgr::base::gsa {
namespace {

// «GSA-<clase>-AAAAMMDD-HHMMSS» → el sello, que ordenado como texto es el orden en el
// tiempo. Vacío si el nombre no es de esa clase o el sello no tiene la forma.
std::string selloDe(const std::string& nombre, const std::string& clase) {
    const std::string prefijo = "GSA-" + clase + "-";
    if (nombre.rfind(prefijo, 0) != 0) {
        return {};
    }
    const std::string sello = nombre.substr(prefijo.size());
    if (sello.size() != 15 || sello[8] != '-') {
        return {};
    }
    for (std::size_t i = 0; i < sello.size(); ++i) {
        if (i != 8 && !std::isdigit(static_cast<unsigned char>(sello[i]))) {
            return {};
        }
    }
    return sello;
}

bool esGsa(const std::string& nombre) { return nombre.rfind("GSA-", 0) == 0; }

}  // namespace

bool splitTarget(const std::string& destino, std::string& conexion, std::string& dataset) {
    const std::string t = trim(destino);
    // Un patrón no es un destino: nivelar «a lo que case» no significa nada.
    if (!startsWith(toLowerAscii(t), "zfsm://") || t.find_first_of("*?[") != std::string::npos) {
        return false;
    }
    ZfsmUrl u;
    std::string error;
    if (!parseZfsmUrl(t, u, error) || u.kind != ZfsmKind::Dataset || !u.section.empty()) {
        return false;
    }
    conexion = u.connection;
    dataset = u.dataset;
    return true;
}

bool targetFromUrl(const std::string& texto, std::string& guardado, Reason& porQue) {
    porQue = Reason{};
    guardado.clear();
    std::string conexion;
    std::string dataset;
    if (!splitTarget(texto, conexion, dataset)) {
        porQue.failure = Failure::MalformedTarget;
        porQue.detail = trim(texto);
        return false;
    }
    ZfsmUrl u;
    std::string error;
    parseZfsmUrl(trim(texto), u, error);
    guardado = formatZfsmUrl(u);
    return true;
}

std::string targetForScheduleMachine(const std::string& target, bool targetIsClientLocal,
                                     bool scheduleOnClientLocal, const std::string& clientHost) {
    if (!targetIsClientLocal || scheduleOnClientLocal || trim(clientHost).empty()) {
        return target;
    }
    std::string connection;
    std::string dataset;
    if (!splitTarget(target, connection, dataset)) {
        return target;
    }
    return "zfsm://" + trim(clientHost) + "/" + dataset;
}

std::vector<SnapRef> parseSnapRefs(const std::string& salida, const std::string& dataset) {
    std::vector<SnapRef> out;
    const std::string prefijo = dataset + "@";
    for (const std::string& l : split(salida, "\n", true)) {
        const std::size_t tab = l.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        const std::string nombre = trim(l.substr(0, tab));
        if (nombre.rfind(prefijo, 0) != 0) {
            continue;
        }
        out.push_back({nombre.substr(prefijo.size()), trim(l.substr(tab + 1))});
    }
    return out;
}

std::vector<std::string> stillToPrune(const std::vector<std::string>& planned,
                                      const std::vector<std::string>& present) {
    std::vector<std::string> out;
    for (const std::string& n : planned) {
        if (std::find(present.begin(), present.end(), n) != present.end()) {
            out.push_back(n);
        }
    }
    return out;
}

LevelPlan planLevel(const std::vector<SnapRef>& origen, const std::vector<SnapRef>& destino,
                    bool destinoExiste) {
    LevelPlan plan;
    if (!destinoExiste) {
        plan.kind = LevelPlan::Kind::Full;
        return plan;
    }
    std::map<std::string, std::string> nombreEnOrigen;   // guid -> nombre
    for (const SnapRef& s : origen) {
        nombreEnOrigen[s.guid] = s.name;
    }
    // La base: el más nuevo del destino que el origen también tiene.
    int base = -1;
    for (int i = static_cast<int>(destino.size()) - 1; i >= 0; --i) {
        if (nombreEnOrigen.count(destino[static_cast<std::size_t>(i)].guid)) {
            base = i;
            break;
        }
    }
    if (base < 0) {
        // Existe y no comparte nada: un envío completo exigiría `recv -F` sobre datos que no
        // son nuestros. Se para.
        plan.kind = LevelPlan::Kind::Conflict;
        for (const SnapRef& s : destino) {
            plan.conflicting.push_back(s.name);
        }
        return plan;
    }
    // Lo que el destino tiene POR DETRÁS de la base y el origen no: `recv -F` lo destruiría.
    for (std::size_t i = static_cast<std::size_t>(base) + 1; i < destino.size(); ++i) {
        plan.conflicting.push_back(destino[i].name);
    }
    const SnapRef& b = destino[static_cast<std::size_t>(base)];
    plan.base = nombreEnOrigen[b.guid];
    if (!plan.conflicting.empty()) {
        plan.kind =
            LevelPlan::Kind::Conflict;  // with its base: what tells it from «nothing shared»
        return plan;
    }
    std::map<std::string, bool> nombresDelDestino;
    for (const SnapRef& s : destino) {
        nombresDelDestino[s.name] = true;
    }
    for (int i = 0; i <= base; ++i) {
        const SnapRef& s = destino[static_cast<std::size_t>(i)];
        const auto it = nombreEnOrigen.find(s.guid);
        if (it == nombreEnOrigen.end()) {
            // Ya no está en el origen. Solo se poda si es nuestro, y nunca la base, que por
            // definición sí está.
            if (esGsa(s.name)) {
                plan.pruneOnTarget.push_back(s.name);
            }
            continue;
        }
        // El mismo snapshot con otro nombre: lo renombró el origen (un periodo sin cambios).
        // Si el nombre nuevo ya lo tiene OTRO snapshot del destino, no se toca: renombrar
        // fallaría, y es preferible decirlo a pisar nada.
        if (it->second != s.name && !nombresDelDestino.count(it->second)) {
            plan.renameOnTarget.push_back({s.name, it->second});
        }
    }
    plan.kind = (!origen.empty() && origen.back().guid == b.guid) ? LevelPlan::Kind::UpToDate
                                                                  : LevelPlan::Kind::Incremental;
    return plan;
}

PrunePlan planPrune(const std::vector<std::string>& nombres, const std::string& clase, int keep,
                    const std::string& protegido) {
    PrunePlan plan;
    if (keep <= 0) {
        return plan;
    }
    std::vector<std::pair<std::string, std::string>> deLaClase;   // (sello, nombre)
    for (const std::string& n : nombres) {
        const std::string sello = selloDe(trim(n), clase);
        if (!sello.empty()) {
            deLaClase.push_back({sello, trim(n)});
        }
    }
    std::stable_sort(deLaClase.begin(), deLaClase.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    if (static_cast<int>(deLaClase.size()) <= keep) {
        return plan;
    }
    const std::size_t sobran = deLaClase.size() - static_cast<std::size_t>(keep);
    for (std::size_t i = 0; i < sobran; ++i) {
        if (!protegido.empty() && deLaClase[i].second == protegido) {
            plan.keptBase = true;
            continue;
        }
        plan.destroy.push_back(deLaClase[i].second);
    }
    return plan;
}

std::string newestOfClass(const std::vector<std::string>& nombres, const std::string& clase) {
    std::string mejorSello;
    std::string mejor;
    for (const std::string& n : nombres) {
        const std::string sello = selloDe(trim(n), clase);
        if (!sello.empty() && sello > mejorSello) {
            mejorSello = sello;
            mejor = trim(n);
        }
    }
    return mejor;
}

std::map<std::string, std::map<std::string, std::string>> groupProperties(const std::string& raw) {
    std::map<std::string, std::map<std::string, std::string>> out;
    for (const std::string& linea : split(raw, "\n", true)) {
        const std::vector<std::string> campos = split(linea, "\t", false);
        if (campos.size() < 3) {
            continue;
        }
        const std::string origen = campos.size() > 3 ? toLowerAscii(trim(campos[3])) : "";
        if (startsWith(origen, "inherited")) {
            continue;
        }
        const std::string valor = trim(campos[2]);
        if (valor.empty() || valor == "-") {
            continue;
        }
        out[trim(campos[0])][trim(campos[1])] = valor;
    }
    return out;
}

long long nextPeriodStart(const std::string& klass, long long now) {
    const std::time_t t = static_cast<std::time_t>(now);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    tmv.tm_sec = 0;
    tmv.tm_min = 0;
    if (klass == "hourly") {
        tmv.tm_hour += 1;
    } else {
        tmv.tm_hour = 0;
        if (klass == "daily") {
            tmv.tm_mday += 1;
        } else if (klass == "weekly") {
            const int desdeLunes = (tmv.tm_wday + 6) % 7;  // Monday 0 … Sunday 6
            tmv.tm_mday += 7 - desdeLunes;
        } else if (klass == "monthly") {
            tmv.tm_mday = 1;
            tmv.tm_mon += 1;
        } else {
            tmv.tm_mday = 1;
            tmv.tm_mon = 0;
            tmv.tm_year += 1;
        }
    }
    tmv.tm_isdst = -1;  // let mktime decide across a daylight saving change
    return static_cast<long long>(std::mktime(&tmv));
}

long long soonestNext(const std::vector<ConnectionState>& states, std::string& connection,
                      std::string& dataset) {
    long long best = 0;
    connection.clear();
    dataset.clear();
    for (const ConnectionState& c : states) {
        for (const DatasetState& d : c.datasets) {
            if (d.next > 0 && (best == 0 || d.next < best)) {
                best = d.next;
                connection = c.connection;
                dataset = d.dataset;
            }
        }
    }
    return best;
}

bool parseState(const std::string& texto, ConnectionState& out, std::string& error,
                long long caducaTras) {
    json::Value raiz;
    if (!json::parse(texto, raiz, &error) || !raiz.isObject()) {
        if (error.empty()) {
            error = "not a JSON object";
        }
        return false;
    }
    out.checked = true;
    out.lastPass = raiz["last_pass"].toInt(0);
    out.now = raiz["now"].toInt(0);
    out.datasets.clear();
    for (const auto& kv : raiz["datasets"].toObject()) {
        const json::Value& v = kv.second;
        DatasetState d;
        d.dataset = kv.first;
        d.destination = v["destination"].toString();
        d.level = v["level"].toBool(false);
        d.pending = v["pending"].toBool(false);
        d.lastEval = v["last_eval"].toInt(0);
        d.lastSnapshot = v["snapshot"]["name"].toString();
        d.lastSnapshotTime = v["snapshot"]["time"].toInt(0);
        d.hasLevel = v.contains("level_result");
        d.lastLevelTime = v["level_result"]["time"].toInt(0);
        d.levelOk = v["level_result"]["ok"].toBool(true);
        d.levelCancelled = v["level_result"]["cancelled"].toBool(false);
        d.levelJob = v["level_job"]["id"].toString();
        d.levelJobState = v["level_job"]["state"].toString();
        d.levelJobBytes = static_cast<unsigned long long>(v["level_job"]["bytes"].toInt(0));
        d.next = v["next"].toInt(0);
        // El primer paso que falló, en el orden en que importan.
        const std::pair<const char*, const char*> pasos[] = {
            {"level_result", "level"}, {"snapshot", "snapshot"},
            {"prune", "prune"}, {"target_prune", "target-prune"}};
        for (const auto& p : pasos) {
            const json::Value& r = v[p.first];
            if (r.isObject() && !r["ok"].toBool(true)) {
                d.failedStep = p.second;
                std::vector<std::string> args;
                for (const auto& a : r["error_args"].toArray()) args.push_back(a.toString());
                d.error = agentmsg::render(r["error_code"].toString(), args);
                d.errorTime = r["time"].toInt(0);
                break;
            }
        }
        out.datasets.push_back(std::move(d));
    }
    out.stale = !out.datasets.empty()
                && (out.lastPass <= 0 || (out.now > 0 && out.now - out.lastPass > caducaTras));
    return true;
}

Verdict summarize(const std::vector<ConnectionState>& todas,
                  std::vector<std::pair<std::string, DatasetState>>& mal,
                  std::vector<const ConnectionState*>& sinComprobar) {
    mal.clear();
    sinComprobar.clear();
    bool hayAlguna = false;
    for (const ConnectionState& c : todas) {
        if (!c.checked) {
            sinComprobar.push_back(&c);
            continue;
        }
        for (const DatasetState& d : c.datasets) {
            hayAlguna = true;
            if (c.stale) {
                DatasetState x = d;
                x.failedStep = "scheduler";
                mal.push_back({c.connection, x});
            } else if (!d.failedStep.empty()) {
                mal.push_back({c.connection, d});
            }
        }
    }
    if (!mal.empty()) {
        return Verdict::WithErrors;
    }
    return hayAlguna ? Verdict::AllGreen : Verdict::NoSchedules;
}

LevelAction decideLevel(const LevelTrigger& t) {
    if (!t.levelOn) {
        return LevelAction::Nothing;
    }
    if (t.jobActive) {
        return LevelAction::Wait;
    }
    if (t.forced || t.newSnapshot || t.prunedSource || !t.hasPrevious) {
        return LevelAction::Start;
    }
    if (!t.previousOk && !t.previousCancelled) {
        return LevelAction::Start;
    }
    return LevelAction::Nothing;
}

int levellingsInProgress(const std::vector<ConnectionState>& todas) {
    int n = 0;
    for (const ConnectionState& c : todas) {
        for (const DatasetState& d : c.datasets) {
            if (!d.levelJob.empty()) {
                ++n;
            }
        }
    }
    return n;
}

}  // namespace zfsmgr::base::gsa
