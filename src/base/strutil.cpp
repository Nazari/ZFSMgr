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

}  // namespace zfsmgr::base
