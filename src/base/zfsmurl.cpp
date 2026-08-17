#include "zfsmurl.h"

#include "strutil.h"

#include <cctype>

namespace zfsmgr::base {
namespace {

constexpr const char* kEsquema = "zfsm://";

int valorHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Los que RFC 3986 deja pasar sin codificar dentro de un tramo. Se codifica todo lo
// demás, incluidos el espacio —que ZFS SÍ admite en los nombres— y los tres caracteres
// que esta sintaxis usa como separadores: `/`, `@` y `#`.
bool seDejaTalCual(unsigned char c) {
    if (std::isalnum(c)) {
        return true;
    }
    switch (c) {
        case '-': case '.': case '_': case '~':   // los «no reservados» del RFC
        case ':': case '+': case ',': case '=':   // legales en un tramo y usados por ZFS
            return true;
        default:
            return false;
    }
}

// Trocea CONSERVANDO los vacíos: `a//b` tiene que poder rechazarse, y con skipEmpty se
// colaba como `a/b`. La única barra que se perdona es la final, que es tolerancia
// deliberada —quien copia del árbol la arrastra sin querer—.
std::vector<std::string> troceaYDescodifica(const std::string& sEntrada, bool& ok) {
    ok = true;
    std::string s = sEntrada;
    if (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    if (s.empty()) {
        return {};
    }
    std::vector<std::string> out;
    for (const std::string& crudo : split(s, "/", false)) {
        std::string trozo;
        if (!percentDecode(crudo, trozo)) {
            ok = false;
            return {};
        }
        out.push_back(trozo);
    }
    return out;
}

}  // namespace

std::string percentEncodeSegment(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (seDejaTalCual(c)) {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

bool percentDecode(const std::string& s, std::string& out) {
    out.clear();
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out.push_back(s[i]);
            continue;
        }
        // Un `%` suelto es un error y no un `%` literal: si se dejara pasar, «100%» y
        // «100%41» significarían cosas distintas según el humor del analizador.
        if (i + 2 >= s.size()) {
            out.clear();
            return false;
        }
        const int alto = valorHex(s[i + 1]);
        const int bajo = valorHex(s[i + 2]);
        if (alto < 0 || bajo < 0) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<char>((alto << 4) | bajo));
        i += 2;
    }
    return true;
}

std::string ZfsmUrl::zfsName() const {
    if (dataset.empty()) {
        return {};
    }
    return snapshot.empty() ? dataset : dataset + "@" + snapshot;
}

bool parseZfsmUrl(const std::string& texto, ZfsmUrl& out, std::string& error) {
    out = ZfsmUrl{};
    error.clear();
    const std::string t = trim(texto);
    if (!startsWith(toLowerAscii(t), kEsquema)) {
        error = "no empieza por zfsm://";
        return false;
    }
    std::string resto = t.substr(std::char_traits<char>::length(kEsquema));

    // El fragmento primero: es lo último de la URL y lo que va después NO se vuelve a
    // mirar. Separarlo antes evita que un `@` o una `/` dentro de un nombre de fichero
    // se confundan con los separadores de la parte ZFS.
    std::string fragmento;
    bool habiaFragmento = false;
    const std::size_t almohadilla = resto.find('#');
    if (almohadilla != std::string::npos) {
        habiaFragmento = true;
        fragmento = resto.substr(almohadilla + 1);
        resto = resto.substr(0, almohadilla);
    }

    // Autoridad: hasta la primera barra.
    std::string autoridadCruda = resto;
    std::string rutaCruda;
    const std::size_t barra = resto.find('/');
    if (barra != std::string::npos) {
        autoridadCruda = resto.substr(0, barra);
        rutaCruda = resto.substr(barra + 1);
    }
    if (autoridadCruda.empty()) {
        error = "falta la conexión";
        return false;
    }
    if (!percentDecode(autoridadCruda, out.connection) || out.connection.empty()) {
        error = "conexión mal codificada";
        return false;
    }

    // El snapshot se separa ANTES de trocear la ruta: el `@` pertenece al último tramo.
    std::string snapCrudo;
    const std::size_t arroba = rutaCruda.find('@');
    if (arroba != std::string::npos) {
        snapCrudo = rutaCruda.substr(arroba + 1);
        rutaCruda = rutaCruda.substr(0, arroba);
        if (snapCrudo.find('@') != std::string::npos) {
            error = "dos '@' en la misma URL";
            return false;
        }
        if (snapCrudo.find('/') != std::string::npos) {
            error = "el snapshot no puede llevar '/'";
            return false;
        }
        if (snapCrudo.empty()) {
            error = "'@' sin nombre de snapshot";
            return false;
        }
    }

    bool ok = false;
    const std::vector<std::string> tramos = troceaYDescodifica(rutaCruda, ok);
    if (!ok) {
        error = "ruta mal codificada";
        return false;
    }
    for (const std::string& tr : tramos) {
        if (tr.empty()) {
            error = "tramo de ruta vacío";
            return false;
        }
    }

    if (!snapCrudo.empty()) {
        if (tramos.empty()) {
            // Lo único que no puede llevar snapshot es la conexión. El POOL sí: en ZFS
            // es un dataset como cualquier otro y `zfs snapshot winpool@snap1` funciona.
            error = "un snapshot necesita un dataset";
            return false;
        }
        if (!percentDecode(snapCrudo, out.snapshot) || out.snapshot.empty()) {
            error = "snapshot mal codificado";
            return false;
        }
    }

    if (!tramos.empty()) {
        out.pool = tramos.front();
        out.dataset = join(tramos, "/");
    }

    if (habiaFragmento) {
        // Un `#` sin nada detrás se rechaza en vez de ignorarse: es un error de quien
        // escribe, y tragárselo daría por buena una URL que no nombra lo que su autor
        // creía.
        bool okFrag = false;
        const std::vector<std::string> partes = troceaYDescodifica(fragmento, okFrag);
        if (!okFrag) {
            error = "fragmento mal codificado";
            return false;
        }
        if (partes.empty()) {
            error = "'#' sin sección";
            return false;
        }
        out.section = toLowerAscii(partes.front());
        out.detail.assign(partes.begin() + 1, partes.end());
    }

    if (!out.snapshot.empty()) {
        out.kind = ZfsmKind::Snapshot;
    } else if (!tramos.empty()) {
        // Uno o veinte tramos, es un dataset: el pool también lo es.
        out.kind = ZfsmKind::Dataset;
    } else {
        out.kind = ZfsmKind::Connection;
    }
    return true;
}

std::string formatZfsmUrl(const ZfsmUrl& u) {
    if (!u.isValid() || u.connection.empty()) {
        return {};
    }
    std::string s = kEsquema;
    s += percentEncodeSegment(u.connection);
    if (!u.dataset.empty()) {
        for (const std::string& tramo : split(u.dataset, "/", true)) {
            s += "/";
            s += percentEncodeSegment(tramo);
        }
    }
    if (!u.snapshot.empty()) {
        s += "@";
        s += percentEncodeSegment(u.snapshot);
    }
    if (!u.section.empty()) {
        s += "#";
        s += percentEncodeSegment(u.section);
        for (const std::string& d : u.detail) {
            s += "/";
            s += percentEncodeSegment(d);
        }
    }
    return s;
}

}  // namespace zfsmgr::base
