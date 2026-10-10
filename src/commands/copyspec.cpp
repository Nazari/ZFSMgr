#include "copyspec.h"

#include <cctype>
#include "i18n.h"

#include "strutil.h"

namespace zfsmgr::base::copyspec {

namespace {

// ¿Hay algún tramo que sea «..»? Se mira TRAMO A TRAMO y no buscando la subcadena: un
// directorio que se llame «..algo» o «uno..dos» es legítimo, y rechazarlo sería prohibir
// nombres válidos por parecerse a lo que se quiere impedir.
bool tieneSaltoAtras(const std::vector<std::string>& tramos) {
    for (const std::string& t : tramos) {
        if (trim(t) == "..") {
            return true;
        }
    }
    return false;
}

std::string juntaTramos(const std::vector<std::string>& tramos) {
    std::string out;
    for (const std::string& t : tramos) {
        if (t.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += "/";
        }
        out += t;
    }
    return out;
}

}  // namespace

std::string labelOf(Failure f) {
    switch (f) {
        case Failure::None_:
            return {};
        case Failure::NoConnection:
            return i18n::tr("t_cmd_copyspec_falta_decir_maquina", "falta decir de qué máquina");
        case Failure::BadSection:
            return i18n::tr("t_cmd_copyspec_esa_seccion_nombra_ficheros", "esa sección no nombra ficheros: use «#content/…» dentro de un dataset o "
                   "«#path/…» para una ruta suelta");
        case Failure::PathOnDataset:
            return i18n::tr("t_cmd_copyspec_path_cuelga_maquina_dataset", "«#path» cuelga de una máquina, no de un dataset: dentro de un dataset la "
                   "subruta se nombra con «#content/…»");
        case Failure::NotAbsolute:
            return i18n::tr("t_cmd_copyspec_ruta_suelta_tiene_ser", "una ruta suelta tiene que ser absoluta, empezando por «/»");
        case Failure::EscapesTree:
            return i18n::tr("t_cmd_copyspec_subruta_puede_llevar", "la subruta no puede llevar «..»");
        case Failure::Empty:
            return i18n::tr("t_cmd_copyspec_dijo_ni_origen_ni", "no se dijo ni origen ni destino");
    }
    return {};
}

bool isDriveSegment(const std::string& tramo) {
    return tramo.size() == 2 && tramo[1] == ':'
           && ((tramo[0] >= 'A' && tramo[0] <= 'Z') || (tramo[0] >= 'a' && tramo[0] <= 'z'));
}

LoosePath loosePathOf(const ZfsmUrl& url) {
    LoosePath r;
    if (url.connection.empty()) {
        r.failure = Failure::NoConnection;
        return r;
    }
    if (!url.dataset.empty()) {
        r.failure = Failure::PathOnDataset;
        return r;
    }
    if (tieneSaltoAtras(url.detail)) {
        r.failure = Failure::EscapesTree;
        return r;
    }
    r.connection = url.connection;
    std::vector<std::string> tramos;
    for (const std::string& t : url.detail) {
        if (!t.empty() && t != ".") {
            tramos.push_back(t);
        }
    }
    // La barra inicial se repone: en la URL no se escribe dos veces —`#path/var/tmp` y no
    // `#path//var/tmp`— pero la ruta es absoluta. En Windows la raíz es la unidad.
    if (!tramos.empty() && isDriveSegment(tramos.front())) {
        std::string unidad = tramos.front();
        unidad[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(unidad[0])));
        tramos.erase(tramos.begin());
        r.absolute = unidad + "/" + juntaTramos(tramos);
    } else {
        r.absolute = "/" + juntaTramos(tramos);
    }
    return r;
}

ZfsmUrl urlOfLoosePath(const std::string& connection, const std::string& absolute) {
    ZfsmUrl u;
    u.kind = ZfsmKind::Connection;
    u.connection = connection;
    u.section = zfsmSection::kPath;
    std::string t = absolute;
    for (char& c : t) {
        if (c == '\\') {
            c = '/';
        }
    }
    for (const std::string& tramo : split(t, "/", false)) {
        if (!tramo.empty() && tramo != ".") {
            u.detail.push_back(tramo);
        }
    }
    return u;
}

Reading readEnd(const std::string& texto, const ZfsmUrl& resuelta,
                const std::string& conexionLocal) {
    Reading r;
    const std::string crudo = trim(texto);
    if (crudo.empty()) {
        r.failure = Failure::Empty;
        return r;
    }

    // El atajo: una ruta absoluta suelta es de la máquina de aquí. Es el mismo trato que ya
    // dan `todir` y `fromdir`, y por eso no sorprende: donde esas órdenes aceptan
    // «/mnt/salida», ésta también.
    //
    // Se mira ANTES que la URL a propósito. `resuelve()` lee una barra inicial como ruta
    // ABSOLUTA DEL ÁRBOL —«/local/pool/ds»—, así que «/var/tmp/x» acabaría siendo la
    // conexión «var», el pool «tmp» y el dataset «x». Dentro de `cp` eso no es lo que nadie
    // quiere decir.
    if (crudo.front() == '/') {
        if (conexionLocal.empty()) {
            r.failure = Failure::NoConnection;
            return r;
        }
        const std::vector<std::string> tramos = split(crudo, "/", false);
        if (tieneSaltoAtras(tramos)) {
            r.failure = Failure::EscapesTree;
            return r;
        }
        r.end.kind = EndKind::PlainPath;
        r.end.connection = conexionLocal;
        r.end.absolute = "/" + juntaTramos(tramos);
        return r;
    }

    if (resuelta.connection.empty()) {
        r.failure = Failure::NoConnection;
        return r;
    }
    const std::string seccion = toLowerAscii(trim(resuelta.section));
    const bool hayDataset = !resuelta.dataset.empty();

    if (seccion == "path") {
        if (hayDataset) {
            r.failure = Failure::PathOnDataset;
            return r;
        }
        const LoosePath suelta = loosePathOf(resuelta);
        if (!suelta.ok()) {
            r.failure = suelta.failure;
            return r;
        }
        // Copiar «/» entero no es algo que nadie quiera por una errata.
        if (juntaTramos(resuelta.detail).empty()) {
            r.failure = Failure::NotAbsolute;
            return r;
        }
        r.end.kind = EndKind::PlainPath;
        r.end.connection = suelta.connection;
        r.end.absolute = suelta.absolute;
        return r;
    }

    if (!hayDataset) {
        r.failure = Failure::NoConnection;
        return r;
    }
    if (!seccion.empty() && seccion != "content") {
        r.failure = Failure::BadSection;
        return r;
    }
    if (tieneSaltoAtras(resuelta.detail)) {
        r.failure = Failure::EscapesTree;
        return r;
    }
    r.end.kind = EndKind::Dataset;
    r.end.connection = resuelta.connection;
    r.end.zfsName = resuelta.zfsName();
    r.end.relative = juntaTramos(resuelta.detail);
    return r;
}

std::string landingName(const End& origen, const std::string& comoSeLlama) {
    const std::string pedido = trim(comoSeLlama);
    if (!pedido.empty()) {
        return pedido;
    }
    // La última pieza de lo que se copia. De una ruta suelta, su último tramo; de un
    // dataset con subruta, el último de la subruta; y de un dataset entero, la hoja de su
    // nombre ZFS —«tank/datos/fotos» aterriza como «fotos»—.
    const std::string base = origen.kind == EndKind::PlainPath
                                 ? origen.absolute
                                 : (origen.relative.empty() ? origen.zfsName : origen.relative);
    const std::vector<std::string> tramos = split(base, "/", false);
    for (auto it = tramos.rbegin(); it != tramos.rend(); ++it) {
        if (!trim(*it).empty()) {
            return trim(*it);
        }
    }
    return {};
}

}  // namespace zfsmgr::base::copyspec
