#include "copyspec.h"

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
            return "falta decir de qué máquina";
        case Failure::BadSection:
            return "esa sección no nombra ficheros: use «#content/…» dentro de un dataset o "
                   "«#path/…» para una ruta suelta";
        case Failure::PathOnDataset:
            return "«#path» cuelga de una máquina, no de un dataset: dentro de un dataset la "
                   "subruta se nombra con «#content/…»";
        case Failure::NotAbsolute:
            return "una ruta suelta tiene que ser absoluta, empezando por «/»";
        case Failure::EscapesTree:
            return "la subruta no puede llevar «..»";
        case Failure::Empty:
            return "no se dijo ni origen ni destino";
    }
    return {};
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
        if (tieneSaltoAtras(resuelta.detail)) {
            r.failure = Failure::EscapesTree;
            return r;
        }
        const std::string ruta = juntaTramos(resuelta.detail);
        if (ruta.empty()) {
            r.failure = Failure::NotAbsolute;
            return r;
        }
        r.end.kind = EndKind::PlainPath;
        r.end.connection = resuelta.connection;
        // La barra inicial se repone: en la URL no se escribe dos veces
        // —`#path/var/tmp` y no `#path//var/tmp`— pero la ruta es absoluta.
        r.end.absolute = "/" + ruta;
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
