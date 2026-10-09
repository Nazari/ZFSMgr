#include "strutil.h"

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace zfsmgr::base {

std::string trim(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) {
        ++a;
    }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
        --b;
    }
    return s.substr(a, b - a);
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return;
    }
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        // Se avanza por DETRÁS de lo insertado: si no, un `to` que contuviera a `from`
        // se volvería a encontrar y esto no terminaría nunca.
        pos += to.size();
    }
}

std::string format(const std::string& tmpl, const std::vector<std::string>& args) {
    std::string out;
    out.reserve(tmpl.size() + 64);
    for (std::size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] != '%') {
            out.push_back(tmpl[i]);
            continue;
        }
        // Se lee el número MÁS LARGO posible (hasta dos cifras): con trece argumentos
        // conviven %1 y %10, y quedarse con %1 dejaría un '0' suelto pegado detrás.
        std::size_t j = i + 1;
        std::size_t fin = j;
        while (fin < tmpl.size() && fin - j < 2
               && std::isdigit(static_cast<unsigned char>(tmpl[fin]))) {
            ++fin;
        }
        if (fin == j) {
            out.push_back('%');
            continue;
        }
        const int n = std::stoi(tmpl.substr(j, fin - j));
        if (n >= 1 && static_cast<std::size_t>(n) <= args.size()) {
            out += args[static_cast<std::size_t>(n) - 1];
            i = fin - 1;
        } else {
            // Fuera de rango: se deja literal, como hace Qt.
            out.push_back('%');
        }
    }
    return out;
}

std::string shSingleQuote(const std::string& s) {
    std::string out = s;
    replaceAll(out, "'", "'\"'\"'");
    return "'" + out + "'";
}

std::string simplify(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool enEspacio = true;  // arranca en true para comerse el espacio inicial
    for (const char ch : s) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!enEspacio) {
                out.push_back(' ');
                enEspacio = true;
            }
            continue;
        }
        out.push_back(ch);
        enEspacio = false;
    }
    if (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::string toUpperAscii(const std::string& s) {
    std::string out = s;
    for (char& ch : out) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return out;
}

bool contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

bool startsWith(const std::string& s, const std::string& pre) {
    return s.size() >= pre.size() && s.compare(0, pre.size(), pre) == 0;
}

bool endsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

long long indexOf(const std::string& s, const std::string& sub) {
    const std::size_t p = s.find(sub);
    return p == std::string::npos ? -1 : static_cast<long long>(p);
}

long long lastIndexOf(const std::string& s, const std::string& sub) {
    const std::size_t p = s.rfind(sub);
    return p == std::string::npos ? -1 : static_cast<long long>(p);
}

std::size_t byteOfChar(const std::string& s, std::size_t nChars) {
    std::size_t i = 0;
    std::size_t vistos = 0;
    while (i < s.size() && vistos < nChars) {
        // Los bytes de continuación de UTF-8 son 10xxxxxx: no empiezan carácter.
        ++i;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
            ++i;
        }
        ++vistos;
    }
    return i;
}

std::string left(const std::string& s, std::size_t nChars) {
    return s.substr(0, byteOfChar(s, nChars));
}

std::string mid(const std::string& s, std::size_t posChars) {
    const std::size_t b = byteOfChar(s, posChars);
    return b >= s.size() ? std::string() : s.substr(b);
}

std::string mid(const std::string& s, std::size_t posChars, std::size_t nChars) {
    const std::size_t b = byteOfChar(s, posChars);
    if (b >= s.size()) {
        return std::string();
    }
    const std::string resto = s.substr(b);
    return resto.substr(0, byteOfChar(resto, nChars));
}

std::vector<std::string> split(const std::string& s, const std::string& sep, bool skipEmpty) {
    std::vector<std::string> out;
    if (sep.empty()) {
        out.push_back(s);
        return out;
    }
    std::size_t ini = 0;
    while (true) {
        const std::size_t p = s.find(sep, ini);
        std::string trozo = (p == std::string::npos) ? s.substr(ini) : s.substr(ini, p - ini);
        if (!skipEmpty || !trozo.empty()) {
            out.push_back(std::move(trozo));
        }
        if (p == std::string::npos) {
            break;
        }
        ini = p + sep.size();
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out += sep;
        }
        out += parts[i];
    }
    return out;
}

namespace {

// Decodifica el carácter que empieza en `pos`. Devuelve el punto de código y, por
// `largo`, cuántos bytes ocupa. Ante un byte inválido devuelve ese byte con largo 1,
// para que el recorrido siempre avance y nunca se atasque.
unsigned decodeUtf8(const std::string& s, std::size_t pos, std::size_t& largo) {
    const unsigned char c0 = static_cast<unsigned char>(s[pos]);
    auto cont = [&](std::size_t k) {
        return pos + k < s.size() && (static_cast<unsigned char>(s[pos + k]) & 0xC0) == 0x80;
    };
    if (c0 < 0x80) { largo = 1; return c0; }
    if ((c0 & 0xE0) == 0xC0 && cont(1)) {
        largo = 2;
        return ((c0 & 0x1Fu) << 6) | (static_cast<unsigned char>(s[pos + 1]) & 0x3Fu);
    }
    if ((c0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
        largo = 3;
        return ((c0 & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[pos + 1]) & 0x3Fu) << 6)
             | (static_cast<unsigned char>(s[pos + 2]) & 0x3Fu);
    }
    if ((c0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
        largo = 4;
        return ((c0 & 0x07u) << 18) | ((static_cast<unsigned char>(s[pos + 1]) & 0x3Fu) << 12)
             | ((static_cast<unsigned char>(s[pos + 2]) & 0x3Fu) << 6)
             | (static_cast<unsigned char>(s[pos + 3]) & 0x3Fu);
    }
    largo = 1;
    return c0;
}

void encodeUtf8(unsigned cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

unsigned aMinuscula(unsigned cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp == 0x0178) return 0x00FF;  // Ÿ está fuera de su bloque, su minúscula es ÿ
    // Suplemento Latin-1: se salta 0xD7, que es el signo de multiplicar, no una letra.
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 32;
    // Latin Extended-A: pares (mayúscula, minúscula) consecutivos, con dos tramos que
    // van desfasados por una letra suelta en medio.
    if (cp >= 0x0100 && cp <= 0x0137) return (cp % 2 == 0) ? cp + 1 : cp;
    if (cp >= 0x0139 && cp <= 0x0148) return (cp % 2 == 1) ? cp + 1 : cp;
    if (cp >= 0x014A && cp <= 0x0177) return (cp % 2 == 0) ? cp + 1 : cp;
    if (cp >= 0x0179 && cp <= 0x017E) return (cp % 2 == 1) ? cp + 1 : cp;
    return cp;
}

unsigned aMayuscula(unsigned cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    if (cp == 0x00FF) return 0x0178;  // la mayúscula de ÿ no es contigua
    // 0xDF es la ß, cuya mayúscula es «SS» y por tanto cambia de longitud: se deja.
    // 0xF7 es el signo de dividir.
    if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) return cp - 32;
    if (cp >= 0x0100 && cp <= 0x0137) return (cp % 2 == 1) ? cp - 1 : cp;
    if (cp >= 0x0139 && cp <= 0x0148) return (cp % 2 == 0) ? cp - 1 : cp;
    if (cp >= 0x014A && cp <= 0x0177) return (cp % 2 == 1) ? cp - 1 : cp;
    if (cp >= 0x0179 && cp <= 0x017E) return (cp % 2 == 0) ? cp - 1 : cp;
    return cp;
}

bool esLetra(unsigned cp) {
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return true;
    // Indicadores ordinales (ª º) y el signo micro: Qt los cuenta como letras, y los dos
    // primeros salen en texto español corriente.
    if (cp == 0xAA || cp == 0xBA || cp == 0xB5) return true;
    if (cp >= 0xC0 && cp <= 0xFF && cp != 0xD7 && cp != 0xF7) return true;
    return cp >= 0x0100 && cp <= 0x017F;
}

std::string mapearCaja(const std::string& s, unsigned (*f)(unsigned)) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t largo = 1;
        const unsigned cp = decodeUtf8(s, i, largo);
        if (largo == 1 && cp > 0x7F) {
            out.push_back(s[i]);  // byte inválido: se conserva tal cual
        } else {
            encodeUtf8(f(cp), out);
        }
        i += largo;
    }
    return out;
}

}  // namespace

std::string toLowerUtf8(const std::string& s) { return mapearCaja(s, aMinuscula); }
std::string toUpperUtf8(const std::string& s) { return mapearCaja(s, aMayuscula); }

bool isLetterAt(const std::string& s, std::size_t pos) {
    if (pos >= s.size()) {
        return false;
    }
    std::size_t largo = 1;
    const unsigned cp = decodeUtf8(s, pos, largo);
    if (largo == 1 && cp > 0x7F) {
        return false;
    }
    return esLetra(cp);
}

namespace {

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int deBase64(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

std::string base64Encode(const std::string& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < data.size()) {
        const unsigned v = (static_cast<unsigned char>(data[i]) << 16)
                         | (static_cast<unsigned char>(data[i + 1]) << 8)
                         | static_cast<unsigned char>(data[i + 2]);
        out.push_back(kB64[(v >> 18) & 0x3F]);
        out.push_back(kB64[(v >> 12) & 0x3F]);
        out.push_back(kB64[(v >> 6) & 0x3F]);
        out.push_back(kB64[v & 0x3F]);
        i += 3;
    }
    const std::size_t resto = data.size() - i;
    if (resto == 1) {
        const unsigned v = static_cast<unsigned>(static_cast<unsigned char>(data[i])) << 16;
        out.push_back(kB64[(v >> 18) & 0x3F]);
        out.push_back(kB64[(v >> 12) & 0x3F]);
        out += "==";
    } else if (resto == 2) {
        const unsigned v = (static_cast<unsigned>(static_cast<unsigned char>(data[i])) << 16)
                         | (static_cast<unsigned>(static_cast<unsigned char>(data[i + 1])) << 8);
        out.push_back(kB64[(v >> 18) & 0x3F]);
        out.push_back(kB64[(v >> 12) & 0x3F]);
        out.push_back(kB64[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

bool base64Decode(const std::string& text, std::string& out) {
    out.clear();
    int val = 0;
    int bits = -8;
    for (const char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isspace(c)) {
            continue;
        }
        if (c == '=') {
            break;
        }
        const int d = deBase64(c);
        if (d < 0) {
            return false;
        }
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return true;
}

}  // namespace zfsmgr::base

namespace zfsmgr::base {

std::string humanSize(const std::string& v) {
    char* fin = nullptr;
    const double n = std::strtod(v.c_str(), &fin);
    if (!fin || *fin != '\0' || v.empty()) {
        return v;  // no es un número: se deja como vino
    }
    static const char* kUnidades[] = {"B", "K", "M", "G", "T", "P", "E"};
    double x = n;
    int u = 0;
    while (x >= 1024.0 && u < 6) {
        x /= 1024.0;
        ++u;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), (u == 0 || x >= 10.0) ? "%.0f%s" : "%.1f%s", x, kUnidades[u]);
    return buf;
}

bool hasWildcard(const std::string& rawSegment) {
    return rawSegment.find_first_of("*?[{") != std::string::npos;
}

bool expandBraces(const std::string& pattern, std::vector<std::string>& out) {
    out.clear();
    // La primera llave de nivel superior; lo de dentro se reparte por comas de su nivel.
    std::size_t abre = std::string::npos;
    int nivel = 0;
    std::size_t cierra = std::string::npos;
    std::vector<std::size_t> comas;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '\\' && i + 1 < pattern.size()) {
            ++i;  // lo escapado no abre ni cierra nada
            continue;
        }
        if (c == '{') {
            if (nivel == 0 && abre == std::string::npos) {
                abre = i;
            }
            ++nivel;
        } else if (c == '}') {
            if (nivel == 0) {
                return false;  // cierra sin abrir
            }
            --nivel;
            if (nivel == 0 && cierra == std::string::npos && abre != std::string::npos) {
                cierra = i;
                break;
            }
        } else if (c == ',' && nivel == 1 && abre != std::string::npos) {
            comas.push_back(i);
        }
    }
    if (abre == std::string::npos) {
        if (nivel != 0) {
            return false;
        }
        out.push_back(pattern);
        return true;
    }
    if (cierra == std::string::npos) {
        return false;  // abre sin cerrar
    }
    const std::string antes = pattern.substr(0, abre);
    const std::string despues = pattern.substr(cierra + 1);
    std::vector<std::string> alternativas;
    std::size_t desde = abre + 1;
    for (const std::size_t coma : comas) {
        alternativas.push_back(trim(pattern.substr(desde, coma - desde)));
        desde = coma + 1;
    }
    alternativas.push_back(trim(pattern.substr(desde, cierra - desde)));
    for (const std::string& alt : alternativas) {
        std::vector<std::string> sub;
        if (!expandBraces(antes + alt + despues, sub)) {
            return false;
        }
        for (std::string& x : sub) {
            if (std::find(out.begin(), out.end(), x) == out.end()) {
                out.push_back(std::move(x));
            }
        }
    }
    return true;
}

namespace {
// Una clase `[...]` que empieza en `p[i]` (el `[`). Devuelve dónde acaba (el `]`) o npos si
// no está cerrada, y en `casa` si `c` es de la clase.
std::size_t claseCasa(const std::string& p, std::size_t i, char c, bool& casa) {
    std::size_t j = i + 1;
    bool niega = false;
    if (j < p.size() && (p[j] == '!' || p[j] == '^')) {
        niega = true;
        ++j;
    }
    bool dentro = false;
    bool primero = true;
    while (j < p.size() && (p[j] != ']' || primero)) {
        primero = false;
        char a = p[j];
        if (a == '\\' && j + 1 < p.size()) {
            a = p[++j];
        }
        if (j + 2 < p.size() && p[j + 1] == '-' && p[j + 2] != ']') {
            char b = p[j + 2];
            j += 2;
            if (b == '\\' && j + 1 < p.size()) {
                b = p[++j];
            }
            if (c >= a && c <= b) {
                dentro = true;
            }
        } else if (c == a) {
            dentro = true;
        }
        ++j;
    }
    if (j >= p.size()) {
        return std::string::npos;
    }
    casa = dentro != niega;
    return j;
}
}  // namespace

bool globMatch(const std::string& pattern, const std::string& text) {
    // Con vuelta atrás solo hasta el último `*`: lineal en la práctica, sin recursión.
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t estrellaP = std::string::npos;
    std::size_t estrellaT = 0;
    while (t < text.size()) {
        if (p < pattern.size()) {
            const char c = pattern[p];
            if (c == '*') {
                estrellaP = p++;
                estrellaT = t;
                continue;
            }
            if (c == '?') {
                ++p;
                ++t;
                continue;
            }
            if (c == '[') {
                bool casa = false;
                const std::size_t fin = claseCasa(pattern, p, text[t], casa);
                if (fin == std::string::npos) {
                    // `[` sin cerrar: literal.
                    if (text[t] == '[') {
                        ++p;
                        ++t;
                        continue;
                    }
                } else if (casa) {
                    p = fin + 1;
                    ++t;
                    continue;
                }
            } else {
                char lit = c;
                std::size_t avance = 1;
                if (c == '\\' && p + 1 < pattern.size()) {
                    lit = pattern[p + 1];
                    avance = 2;
                }
                if (lit == text[t]) {
                    p += avance;
                    ++t;
                    continue;
                }
            }
        }
        if (estrellaP == std::string::npos) {
            return false;
        }
        p = estrellaP + 1;
        t = ++estrellaT;
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

bool rawSegmentToPattern(const std::string& rawSegment, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < rawSegment.size(); ++i) {
        const char c = rawSegment[i];
        if (c != '%') {
            out.push_back(c);  // en crudo, un comodín sin escapar ES comodín
            continue;
        }
        if (i + 2 >= rawSegment.size()) {
            return false;
        }
        // `%XX`, escrito aquí: el `percentDecode` de las URL vive en una biblioteca de
        // encima de esta, y la base no puede depender de ella.
        const auto hex = [](char h) -> int {
            if (h >= '0' && h <= '9') return h - '0';
            if (h >= 'a' && h <= 'f') return h - 'a' + 10;
            if (h >= 'A' && h <= 'F') return h - 'A' + 10;
            return -1;
        };
        const int alto = hex(rawSegment[i + 1]);
        const int bajo = hex(rawSegment[i + 2]);
        if (alto < 0 || bajo < 0) {
            return false;
        }
        const char d = static_cast<char>((alto << 4) | bajo);
        // Lo que venía escapado se queda LITERAL: con `\` delante para el emparejador.
        if (d == '*' || d == '?' || d == '[' || d == ']' || d == '{' || d == '}' || d == ','
            || d == '\\') {
            out.push_back('\\');
        }
        out.push_back(d);
        i += 2;
    }
    return true;
}

bool checkRawPattern(const std::string& rawSegment) {
    std::vector<std::string> alternativas;
    if (!expandBraces(rawSegment, alternativas)) {
        return false;
    }
    std::string p;
    for (const std::string& a : alternativas) {
        if (!rawSegmentToPattern(a, p)) {
            return false;
        }
    }
    return true;
}

bool matchesRawPattern(const std::string& rawSegment, const std::string& text) {
    std::vector<std::string> alternativas;
    if (!expandBraces(rawSegment, alternativas)) {
        return false;
    }
    std::string p;
    for (const std::string& a : alternativas) {
        if (rawSegmentToPattern(a, p) && globMatch(p, text)) {
            return true;
        }
    }
    return false;
}

bool parseHumanSize(const std::string& v, unsigned long long& out) {
    const std::string t = trim(v);
    if (t.empty()) {
        return false;
    }
    char* fin = nullptr;
    const double n = std::strtod(t.c_str(), &fin);
    if (fin == t.c_str() || n < 0.0) {
        return false;
    }
    std::string sufijo = trim(std::string(fin));
    // `zfs` escribe «512B», «512» y «1.5G»; algunas versiones, «1.5GB». Las tres se leen.
    if (sufijo.size() == 2 && (sufijo[1] == 'B' || sufijo[1] == 'b')) {
        sufijo.resize(1);
    }
    if (sufijo.empty()) {
        sufijo = "B";
    }
    if (sufijo.size() != 1) {
        return false;
    }
    static const char kUnidades[] = "BKMGTPE";
    const char* pos = std::strchr(kUnidades, std::toupper(static_cast<unsigned char>(sufijo[0])));
    if (pos == nullptr) {
        return false;
    }
    double x = n;
    for (int i = 0; i < static_cast<int>(pos - kUnidades); ++i) {
        x *= 1024.0;
    }
    out = static_cast<unsigned long long>(x);
    return true;
}

}  // namespace zfsmgr::base
