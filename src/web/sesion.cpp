#include "sesion.h"

#include <openssl/rand.h>

#include <cstdio>

namespace zfsmgr::web {

std::string alAzarHex() {
    unsigned char buf[32];
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        return {};
    }
    std::string out;
    out.reserve(sizeof(buf) * 2);
    char par[3];
    for (const unsigned char c : buf) {
        std::snprintf(par, sizeof(par), "%02x", c);
        out += par;
    }
    return out;
}

void Sesion::abre() {
    m_id = alAzarHex();
    m_testigo = alAzarHex();
    if (m_id.empty() || m_testigo.empty()) {
        cierra();   // sin azar no hay sesión: mejor ninguna que una previsible
    }
}

void Sesion::cierra() {
    m_id.clear();
    m_testigo.clear();
}

namespace {

// Comparación en tiempo constante: no sale antes por el primer byte distinto.
bool igualSinFiltrarTiempo(const std::string& a, const std::string& b) {
    if (a.size() != b.size() || a.empty()) {
        return false;
    }
    unsigned char acumulado = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        acumulado |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return acumulado == 0;
}

}  // namespace

bool Sesion::cookieVale(const std::string& valor) const {
    return abierta() && igualSinFiltrarTiempo(m_id, valor);
}

bool Sesion::testigoVale(const std::string& valor) const {
    return abierta() && igualSinFiltrarTiempo(m_testigo, valor);
}

std::string Sesion::cabeceraCookie() const {
    return "Set-Cookie: zfsmgr_sesion=" + m_id + "; Path=/; HttpOnly; Secure; SameSite=Strict";
}

}  // namespace zfsmgr::web
