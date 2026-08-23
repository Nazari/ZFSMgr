#include "avanzadas.h"

#include <map>
#include <set>

#include "strutil.h"

namespace zfsmgr::commands::avanzadas {

namespace {

// Un dataset o un subdirectorio vacío no se manda: no nombra nada.
bool utilizable(const std::string& s) {
    return !zfsmgr::base::trim(s).empty();
}

}  // namespace

std::vector<std::string> argvDesglosar(const std::string& dataset,
                                       const std::vector<Desglose>& pares) {
    const std::string ds = zfsmgr::base::trim(dataset);
    if (!utilizable(ds)) {
        return {};
    }
    std::vector<std::string> argv{"--mutate-advanced-breakdown", ds};
    for (const Desglose& p : pares) {
        const std::string sub = zfsmgr::base::trim(p.subdirectorio);
        const std::string nuevo = zfsmgr::base::trim(p.datasetNuevo);
        // Los dos, o ninguno: el verbo los lee de dos en dos, así que un par a medias
        // desplazaría todos los siguientes y el daemon acabaría creando un dataset con el
        // nombre de un directorio.
        if (!utilizable(sub) || !utilizable(nuevo)) {
            continue;
        }
        argv.push_back(sub);
        argv.push_back(nuevo);
    }
    if (argv.size() < 4) {
        return {};
    }
    return argv;
}

std::string hijoConNombreCompleto(const std::string& dataset, const std::string& hijo) {
    const std::string h = zfsmgr::base::trim(hijo);
    if (h.empty()) {
        return {};
    }
    if (h.find('/') != std::string::npos) {
        return h;  // ya viene completo, o nombra un nieto
    }
    const std::string ds = zfsmgr::base::trim(dataset);
    if (ds.empty()) {
        return h;
    }
    return ds + "/" + h;
}

std::vector<std::string> argvEnsamblar(const std::string& dataset,
                                       const std::vector<std::string>& hijos) {
    const std::string ds = zfsmgr::base::trim(dataset);
    if (!utilizable(ds)) {
        return {};
    }
    std::vector<std::string> argv{"--mutate-advanced-assemble", ds};
    for (const std::string& hijo : hijos) {
        const std::string completo = hijoConNombreCompleto(ds, hijo);
        if (!completo.empty()) {
            argv.push_back(completo);
        }
    }
    if (argv.size() < 3) {
        return {};
    }
    return argv;
}

bool rutaDeDestinoValida(const std::string& directorio) {
    const std::string d = zfsmgr::base::trim(directorio);
    if (d.empty()) {
        return false;
    }
    // Unix: barra inicial. Windows: letra de unidad. Es la misma comprobación que hace
    // `sincronizacion::rutaUsable`, y por el mismo motivo —una ruta que el otro extremo no
    // pueda abrir no es un destino—, pero aquí no vale «none» ni «legacy»: eso son
    // respuestas de ZFS sobre un punto de montaje, y esto es un directorio llano.
    return d[0] == '/' || d.find(':') != std::string::npos;
}

std::vector<std::string> argvHaciaDir(const std::string& dataset, const std::string& directorio,
                                      bool destruyeOrigen) {
    const std::string ds = zfsmgr::base::trim(dataset);
    const std::string dir = zfsmgr::base::trim(directorio);
    if (!utilizable(ds) || !rutaDeDestinoValida(dir)) {
        return {};
    }
    return {"--mutate-advanced-todir", ds, dir, destruyeOrigen ? "1" : "0"};
}

bool subdirectorioRelativoValido(const std::string& rel) {
    const std::string r = zfsmgr::base::trim(rel);
    if (r.empty()) {
        return true;  // la raíz del dataset
    }
    if (r.front() == '/' || r.front() == '\\') {
        return false;  // absoluto: no es «dentro del dataset»
    }
    if (r.find("..") != std::string::npos) {
        return false;  // saldría del punto de montaje
    }
    // Un salto de línea o un tabulador dentro del nombre no rompe nada aquí, pero sí la
    // lectura del registro y la vista previa, que es lo que alguien mira antes de aceptar.
    return r.find('\n') == std::string::npos && r.find('\r') == std::string::npos
           && r.find('\t') == std::string::npos;
}

std::vector<std::string> argvDesdeDir(const std::string& dataset, const std::string& rel) {
    const std::string ds = zfsmgr::base::trim(dataset);
    const std::string r = zfsmgr::base::trim(rel);
    if (!utilizable(ds) || !subdirectorioRelativoValido(r)) {
        return {};
    }
    std::vector<std::string> argv{"--mutate-advanced-fromdir", ds};
    if (!r.empty()) {
        argv.push_back(r);
    }
    return argv;
}

namespace {

// El último tramo de una ruta, con las dos formas de separador.
std::string nombreDeLaRuta(const std::string& ruta, bool windows) {
    std::string p = zfsmgr::base::trim(ruta);
    if (windows) {
        for (char& c : p) {
            if (c == '\\') {
                c = '/';
            }
        }
    }
    while (!p.empty() && p.back() == '/') {
        p.pop_back();
    }
    const std::size_t barra = p.rfind('/');
    return barra == std::string::npos ? p : p.substr(barra + 1);
}

// Lo que queda de un nombre cuando se le quita lo que no puede llevar un directorio.
//
// Se sustituye en vez de rechazar: quien eligió el directorio no eligió su nombre, y
// negarse a copiarlo por llamarse como se llama no le sirve de nada.
std::string nombreUsable(const std::string& bruto) {
    std::string out;
    out.reserve(bruto.size());
    for (const char c : bruto) {
        const bool malo = (c == '/' || c == '\\' || c == '\n' || c == '\r' || c == '\t'
                           || c == ':' || static_cast<unsigned char>(c) < 0x20);
        out.push_back(malo ? '_' : c);
    }
    // Ningún «..», en ninguna posición. No es por el nombre en sí sino porque el receptor
    // rechaza el destino entero si lo lleva —y lo rechaza con el tar ya corriendo—, así que
    // un directorio que se llame «copia..vieja» tumbaría la operación a mitad.
    for (std::size_t i = out.find(".."); i != std::string::npos; i = out.find("..", i)) {
        out[i] = '_';
    }
    // Y «.» a secas es el directorio actual: no sirve de nombre.
    if (out == ".") {
        return "_";
    }
    return out;
}

}  // namespace

std::vector<std::string> subdirectoriosDeDestino(const std::vector<OrigenDesdeDir>& origenes) {
    std::vector<std::string> salida(origenes.size());
    if (origenes.empty()) {
        return salida;
    }
    // Un solo origen: su contenido ES el del dataset. Poner un nivel de más reconstruiría
    // una jerarquía que nadie pidió —el dataset ya es «el directorio»—.
    if (origenes.size() == 1) {
        return salida;
    }

    std::map<std::string, int> cuantosConEseNombre;
    std::vector<std::string> nombres(origenes.size());
    for (std::size_t i = 0; i < origenes.size(); ++i) {
        nombres[i] = nombreDeLaRuta(origenes[i].ruta, origenes[i].windows);
        ++cuantosConEseNombre[nombres[i]];
    }

    std::set<std::string> yaUsados;
    for (std::size_t i = 0; i < origenes.size(); ++i) {
        std::string base = nombres[i];
        const std::string maquina = zfsmgr::base::trim(origenes[i].maquina);
        // Una ruta que es solo el separador —«/», «C:\»— no deja nombre detrás. Sin esto,
        // ese origen se iría a la raíz mientras los demás van a su subdirectorio, y dos así
        // se pisarían.
        if (base.empty()) {
            base = maquina.empty() ? std::string("origen") : maquina;
        } else if (cuantosConEseNombre[nombres[i]] > 1 && !maquina.empty()) {
            base = maquina + "-" + base;
        }
        base = nombreUsable(base);
        if (base.empty()) {
            base = "origen";
        }
        // Y el desempate final, que es el que faltaba: anteponer la máquina no separa dos
        // directorios del MISMO equipo con el mismo nombre.
        std::string candidato = base;
        int sufijo = 2;
        while (yaUsados.count(candidato) > 0) {
            candidato = base + "-" + std::to_string(sufijo);
            ++sufijo;
        }
        yaUsados.insert(candidato);
        salida[i] = candidato;
    }
    return salida;
}

std::vector<std::string> argvDesdeDirPreparar(const std::string& dataset,
                                              const std::string& rel) {
    const std::string ds = zfsmgr::base::trim(dataset);
    const std::string r = zfsmgr::base::trim(rel);
    if (!utilizable(ds) || !subdirectorioRelativoValido(r)) {
        return {};
    }
    std::vector<std::string> argv{"--mutate-advanced-fromdir-prepare", ds};
    if (!r.empty()) {
        argv.push_back(r);
    }
    return argv;
}

std::string rutaPreparada(const std::string& salida) {
    // Se busca la línea, no el principio de la salida: por SSH puede venir precedida de un
    // aviso del propio shell, y quedarse con lo primero que llega daría una ruta que no es.
    for (const std::string& linea : zfsmgr::base::split(salida, "\n", true)) {
        const std::string l = zfsmgr::base::trim(linea);
        if (l.rfind("DST=", 0) == 0) {
            return zfsmgr::base::trim(l.substr(4));
        }
    }
    return {};
}

bool puedeIrPorElArbol(bool origenTieneDaemon, bool destinoTieneDaemon) {
    return origenTieneDaemon && destinoTieneDaemon;
}

std::vector<std::string> rutasDeContenido(const std::string& ruta) {
    const std::string r = zfsmgr::base::trim(ruta);
    const std::size_t ab = r.find('{');
    if (ab == std::string::npos) {
        if (r.find('}') != std::string::npos) {
            return {};  // cierre sin apertura
        }
        return {r};
    }
    const std::size_t ce = r.find('}', ab);
    if (ce == std::string::npos) {
        return {};  // sin cerrar
    }
    const std::string dentro = r.substr(ab + 1, ce - ab - 1);
    if (dentro.find('{') != std::string::npos || dentro.find('}') != std::string::npos) {
        return {};  // anidadas: no
    }
    if (r.find('{', ce) != std::string::npos) {
        return {};  // más de un grupo: tampoco
    }
    const std::string antes = r.substr(0, ab);
    const std::string despues = r.substr(ce + 1);
    std::vector<std::string> salida;
    for (const std::string& pieza : zfsmgr::base::split(dentro, ",", false)) {
        const std::string t = zfsmgr::base::trim(pieza);
        if (t.empty()) {
            return {};  // «{a,,b}» o «{}»
        }
        salida.push_back(antes + t + despues);
    }
    return salida;
}

bool rutaDeContenidoValida(const std::string& ruta) {
    const std::string r = zfsmgr::base::trim(ruta);
    if (r.empty()) {
        return true;
    }
    if (r.front() == '/' || r.front() == '\\') {
        return false;
    }
    if (r.find("..") != std::string::npos) {
        return false;
    }
    return r.find('\n') == std::string::npos && r.find('\r') == std::string::npos
           && r.find('\t') == std::string::npos;
}

}  // namespace zfsmgr::commands::avanzadas
