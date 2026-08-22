#include "http.h"

#include "strutil.h"

#include <cctype>
#include <cstdlib>

namespace zfsmgr::web::http {
namespace {

namespace B = zfsmgr::base;

std::string bajo(const std::string& s) { return B::toLowerAscii(s); }

// Parte por el primer separador. El resto se devuelve entero, sin volver a partir.
bool parteEn(const std::string& s, const std::string& sep, std::string& izq, std::string& der) {
    const std::size_t i = s.find(sep);
    if (i == std::string::npos) {
        return false;
    }
    izq = s.substr(0, i);
    der = s.substr(i + sep.size());
    return true;
}

}  // namespace

std::string Peticion::cabecera(const std::string& nombre) const {
    const auto it = cabeceras.find(bajo(nombre));
    return it == cabeceras.end() ? std::string() : it->second;
}

std::string Peticion::cookie(const std::string& nombre) const {
    const std::string todas = cabecera("cookie");
    for (const std::string& trozo : B::split(todas, ";", true)) {
        std::string k;
        std::string v;
        if (!parteEn(B::trim(trozo), "=", k, v)) {
            continue;
        }
        if (B::trim(k) == nombre) {
            return B::trim(v);
        }
    }
    return {};
}

std::string Peticion::campo(const std::string& nombre) const {
    for (const std::string& par : B::split(cuerpo, "&", true)) {
        std::string k;
        std::string v;
        if (!parteEn(par, "=", k, v)) {
            continue;
        }
        if (desdeUrl(k) == nombre) {
            return desdeUrl(v);
        }
    }
    return {};
}

std::vector<std::string> Peticion::campos(const std::string& nombre) const {
    std::vector<std::string> out;
    for (const std::string& par : B::split(cuerpo, "&", true)) {
        std::string k;
        std::string v;
        if (!parteEn(par, "=", k, v)) {
            continue;
        }
        if (desdeUrl(k) == nombre) {
            out.push_back(desdeUrl(v));
        }
    }
    return out;
}

Peticion analiza(const std::string& crudo) {
    Peticion p;
    const std::size_t finCabeceras = crudo.find("\r\n\r\n");
    if (finCabeceras == std::string::npos) {
        p.porQueNoVale = "no llegan las cabeceras completas";
        return p;
    }
    const std::string bloque = crudo.substr(0, finCabeceras);
    const std::vector<std::string> lineas = B::split(bloque, "\r\n", true);
    if (lineas.empty()) {
        p.porQueNoVale = "sin línea de petición";
        return p;
    }
    const std::vector<std::string> partes = B::split(lineas.front(), " ", true);
    if (partes.size() != 3) {
        p.porQueNoVale = "la línea de petición no tiene tres partes";
        return p;
    }
    p.metodo = partes[0];
    p.version = partes[2];
    if (p.version.rfind("HTTP/", 0) != 0) {
        p.porQueNoVale = "versión desconocida";
        return p;
    }
    std::string ruta = partes[1];
    const std::size_t interr = ruta.find('?');
    if (interr != std::string::npos) {
        p.consulta = ruta.substr(interr + 1);
        ruta = ruta.substr(0, interr);
    }
    p.ruta = desdeUrl(ruta);

    for (std::size_t i = 1; i < lineas.size(); ++i) {
        std::string k;
        std::string v;
        if (!parteEn(lineas[i], ":", k, v)) {
            continue;   // una cabecera sin dos puntos se ignora, no tumba la petición
        }
        p.cabeceras[bajo(B::trim(k))] = B::trim(v);
    }

    const std::string largoTexto = p.cabecera("content-length");
    std::size_t largo = 0;
    if (!largoTexto.empty()) {
        for (const char c : largoTexto) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                p.porQueNoVale = "Content-Length no es un número";
                return p;
            }
        }
        largo = static_cast<std::size_t>(std::strtoul(largoTexto.c_str(), nullptr, 10));
    }
    const std::string resto = crudo.substr(finCabeceras + 4);
    if (resto.size() < largo) {
        p.porQueNoVale = "el cuerpo es más corto de lo que anuncia Content-Length";
        return p;
    }
    p.cuerpo = resto.substr(0, largo);
    p.valida = true;
    return p;
}

std::string desdeUrl(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
            continue;
        }
        if (s[i] != '%' || i + 2 >= s.size()
            || !std::isxdigit(static_cast<unsigned char>(s[i + 1]))
            || !std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(s[i]);
            continue;
        }
        const std::string hex = s.substr(i + 1, 2);
        out.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
        i += 2;
    }
    return out;
}

// La lista de lo que NO se codifica es corta A PROPOSITO: alfanuméricos y los cuatro
// signos que ZFS admite en un nombre —«-», «_», «.» y «:»—, más la barra, que separa el
// dataset de su padre y se lee mejor sin codificar. Todo lo demás va en %XX, incluida la
// «@» de las instantáneas: en la consulta es legal, pero codificarla no cuesta nada y
// evita tener que acordarse de en qué parte de la URL cada carácter es especial.
std::string haciaUrl(const std::string& s) {
    static const char* const kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-' || c == '_' || c == '.' || c == ':' || c == '/') {
            out.push_back(c);
            continue;
        }
        out.push_back('%');
        out.push_back(kHex[u >> 4]);
        out.push_back(kHex[u & 0x0F]);
    }
    return out;
}

std::string escapaHtml(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string componer(const Respuesta& r) {
    const char* texto = "OK";
    switch (r.codigo) {
        case 200: texto = "OK"; break;
        case 207: texto = "Multi-Status"; break;
        case 502: texto = "Bad Gateway"; break;
        case 302: texto = "Found"; break;
        case 400: texto = "Bad Request"; break;
        case 403: texto = "Forbidden"; break;
        case 404: texto = "Not Found"; break;
        case 405: texto = "Method Not Allowed"; break;
        default: texto = "OK"; break;
    }
    std::string out = "HTTP/1.1 " + std::to_string(r.codigo) + " " + texto + "\r\n";
    out += "Content-Type: " + r.tipo + "\r\n";
    out += "Content-Length: " + std::to_string(r.cuerpo.size()) + "\r\n";
    // Que el navegador no haga nada por su cuenta:
    //  - `nosniff`: no adivinar el tipo mirando el contenido.
    //  - `DENY`: esta página no se mete en un marco ajeno, que es la mitad del clickjacking.
    //  - CSP sin `unsafe-inline` ni orígenes externos: si algún día se cuela texto sin
    //    escapar, el navegador aún se niega a ejecutarlo.
    out += "X-Content-Type-Options: nosniff\r\n";
    out += "X-Frame-Options: DENY\r\n";
    out += "Content-Security-Policy: default-src 'none'; style-src 'self'; form-action 'self'\r\n";
    out += "Referrer-Policy: no-referrer\r\n";
    out += "Cache-Control: no-store\r\n";
    out += "Connection: close\r\n";
    for (const std::string& c : r.cabecerasExtra) {
        out += c + "\r\n";
    }
    out += "\r\n";
    out += r.cuerpo;
    return out;
}

}  // namespace zfsmgr::web::http
