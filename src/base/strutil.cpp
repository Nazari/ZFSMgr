#include "strutil.h"

#include <cctype>

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

}  // namespace zfsmgr::base
