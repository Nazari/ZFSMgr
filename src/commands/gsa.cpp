#include "gsa.h"

#include "strutil.h"

#include <algorithm>
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

const char* const kActivado = "org.fc16.gsa:activado";
const char* const kRecursivo = "org.fc16.gsa:recursivo";
const char* const kNivelar = "org.fc16.gsa:nivelar";
const char* const kDestino = "org.fc16.gsa:destino";
const char* const kHorario = "org.fc16.gsa:horario";
const char* const kDiario = "org.fc16.gsa:diario";
const char* const kSemanal = "org.fc16.gsa:semanal";
const char* const kMensual = "org.fc16.gsa:mensual";
const char* const kAnual = "org.fc16.gsa:anual";

}  // namespace

const char* const kPrefijo = "org.fc16.gsa:";

bool desdePropiedades(const std::map<std::string, std::string>& props, Programacion& out,
                      Motivo& porQue) {
    porQue = Motivo{};
    Programacion p;
    p.activado = esOn(valorDe(props, kActivado));
    p.recursivo = esOn(valorDe(props, kRecursivo));
    p.nivelar = esOn(valorDe(props, kNivelar));
    p.destino = trim(valorDe(props, kDestino));
    if (p.destino == "-") {
        p.destino.clear();
    }
    const std::pair<const char*, int*> retenciones[] = {
        {kHorario, &p.horario}, {kDiario, &p.diario},   {kSemanal, &p.semanal},
        {kMensual, &p.mensual}, {kAnual, &p.anual},
    };
    for (const auto& r : retenciones) {
        if (!enteroNoNegativo(valorDe(props, r.first), *r.second)) {
            porQue.fallo = Fallo::RetencionNoEntera;
            porQue.detalle = r.first;
            return false;
        }
    }
    out = p;
    return true;
}

std::map<std::string, std::string> aPropiedades(const Programacion& p) {
    return {
        {kActivado, p.activado ? "on" : "off"},
        {kRecursivo, p.recursivo ? "on" : "off"},
        {kNivelar, p.nivelar ? "on" : "off"},
        {kDestino, p.destino},
        {kHorario, std::to_string(p.horario)},
        {kDiario, std::to_string(p.diario)},
        {kSemanal, std::to_string(p.semanal)},
        {kMensual, std::to_string(p.mensual)},
        {kAnual, std::to_string(p.anual)},
    };
}

bool valida(const std::string& dataset, const Programacion& p,
            const std::function<bool(const std::string&)>& conexionExiste, Motivo& porQue) {
    porQue = Motivo{};
    porQue.dataset = dataset;

    // El destino se comprueba si NIVELAR está puesto —que lo exige— o si hay destino
    // escrito estando la programación activada. Un destino escrito con la programación
    // apagada no molesta a nadie.
    const bool hayQueMirarDestino = p.nivelar || (p.activado && !p.destino.empty());
    if (p.nivelar && p.destino.empty()) {
        porQue.fallo = Fallo::NivelarSinDestino;
        return false;
    }
    if (hayQueMirarDestino) {
        const std::size_t dosPuntos = p.destino.find("::");
        if (dosPuntos == std::string::npos) {
            porQue.fallo = Fallo::DestinoMalFormado;
            porQue.detalle = p.destino;
            return false;
        }
        const std::string conexion = trim(p.destino.substr(0, dosPuntos));
        if (conexionExiste && !conexionExiste(conexion)) {
            porQue.fallo = Fallo::DestinoSinConexion;
            porQue.detalle = conexion;
            return false;
        }
    }
    // Activada y sin ninguna retención es una programación que no guarda nada: hace la
    // instantánea y la borra. Casi siempre es un olvido, y callarlo deja al usuario
    // creyendo que tiene copias.
    if (p.activado && p.sinRetenciones()) {
        porQue.fallo = Fallo::ActivadaSinRetencion;
        return false;
    }
    porQue.dataset.clear();
    return true;
}

bool esMismoODescendiente(const std::string& dataset, const std::string& ancestro) {
    const std::string d = trim(dataset);
    const std::string a = trim(ancestro);
    return d == a || startsWith(d, a + "/");
}

bool validaConjunto(const std::vector<Entrada>& delMismoPool, Motivo& porQue) {
    porQue = Motivo{};
    // Solo las ACTIVADAS chocan: una programación apagada no hace instantáneas, así que
    // solaparse con ella no significa nada.
    std::vector<const Entrada*> vivas;
    for (const Entrada& e : delMismoPool) {
        if (e.prog.activado) {
            vivas.push_back(&e);
        }
    }
    for (std::size_t i = 0; i < vivas.size(); ++i) {
        for (std::size_t j = i + 1; j < vivas.size(); ++j) {
            const Entrada& a = *vivas[i];
            const Entrada& b = *vivas[j];
            if (trim(a.dataset) == trim(b.dataset)) {
                continue;
            }
            if (a.prog.recursivo && esMismoODescendiente(b.dataset, a.dataset)) {
                porQue.fallo = Fallo::ChocaConRecursiva;
                porQue.dataset = b.dataset;
                porQue.detalle = a.dataset;
                return false;
            }
            if (b.prog.recursivo && esMismoODescendiente(a.dataset, b.dataset)) {
                porQue.fallo = Fallo::ChocaConRecursiva;
                porQue.dataset = a.dataset;
                porQue.detalle = b.dataset;
                return false;
            }
        }
    }
    return true;
}

std::string etiquetaDe(Fallo f) {
    switch (f) {
        case Fallo::Ninguno:
            return "sin fallo";
        case Fallo::RetencionNoEntera:
            return "la retención no es un entero mayor o igual que 0";
        case Fallo::ActivadaSinRetencion:
            return "la programación está activada y no guarda ninguna instantánea";
        case Fallo::NivelarSinDestino:
            return "nivelar está puesto y no hay destino";
        case Fallo::DestinoMalFormado:
            return "el destino tiene que ser «Conexión::Pool/Dataset»";
        case Fallo::DestinoSinConexion:
            return "el destino nombra una conexión que no existe";
        case Fallo::ChocaConRecursiva:
            return "ya hay una programación recursiva que lo cubre";
    }
    return "sin fallo";
}

std::string claseDeInstantanea(const std::string& nombre) {
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

std::vector<std::pair<std::string, std::vector<std::string>>> agrupaInstantaneas(
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
        const std::string klass = claseDeInstantanea(n);
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

std::string destinoComoUrl(const std::string& destino) {
    const std::string t = trim(destino);
    const std::size_t sep = t.find("::");
    if (sep == std::string::npos || sep == 0 || sep + 2 >= t.size()) {
        return t;
    }
    return "zfsm://" + t.substr(0, sep) + "/" + t.substr(sep + 2);
}

std::string destinoDesdeUrl(const std::string& texto) {
    std::string t = trim(texto);
    if (!startsWith(toLowerAscii(t), "zfsm://")) {
        return t;   // ya viene en el formato que se guarda, o es basura que validará otro
    }
    t = t.substr(7);
    const std::size_t barra = t.find('/');
    if (barra == std::string::npos || barra == 0 || barra + 1 >= t.size()) {
        return trim(texto);   // «zfsm://maquina» no nombra ningún dataset: que falle la
                              // validación con su motivo, no aquí en silencio
    }
    return t.substr(0, barra) + "::" + t.substr(barra + 1);
}

}  // namespace zfsmgr::base::gsa
