// El analizador de HTTP y la sesión, sin levantar un servidor.
//
// Lo que se comprueba aquí no es que «funcione»: es lo que RECHAZA. Un analizador que
// adivina lo que no entiende, mirando lo que manda un navegador, es por donde se cuelan
// las cosas.
#include "http.h"
#include "sesion.h"

#include <cstdio>
#include <string>

namespace {
int fallos = 0;

void comprobar(bool ok, const std::string& que) {
    if (!ok) {
        std::printf("FALLO: %s\n", que.c_str());
        ++fallos;
    }
}

void igual(const std::string& a, const std::string& b, const std::string& que) {
    if (a != b) {
        std::printf("FALLO: %s\n   esperado: «%s»\n   obtenido: «%s»\n", que.c_str(), b.c_str(), a.c_str());
        ++fallos;
    }
}
}  // namespace

int main() {
    namespace H = zfsmgr::web::http;

    // --- Lo que se acepta.
    {
        const auto p = H::analiza("GET /?s=abc HTTP/1.1\r\nHost: x\r\nCookie: a=1; zfsmgr_sesion=zzz\r\n\r\n");
        comprobar(p.valida, "una peticion normal se analiza");
        igual(p.metodo, "GET", "el metodo");
        igual(p.ruta, "/", "la ruta, sin la consulta");
        igual(p.consulta, "s=abc", "y la consulta aparte");
        igual(p.cabecera("HOST"), "x", "las cabeceras no distinguen mayusculas");
        igual(p.cookie("zfsmgr_sesion"), "zzz", "la cookie que interesa, no la primera");
        igual(p.cookie("no-esta"), "", "y una que no esta sale vacia");
    }
    {
        const auto p = H::analiza("POST /salir HTTP/1.1\r\nContent-Length: 11\r\n\r\ntestigo=abc");
        comprobar(p.valida, "un POST con cuerpo se analiza");
        igual(p.campo("testigo"), "abc", "y su campo se lee");
        igual(p.campo("otro"), "", "un campo que no esta sale vacio");
    }

    // --- Lo que se RECHAZA, que es el punto.
    comprobar(!H::analiza("GET / HTTP/1.1\r\n").valida, "sin el final de cabeceras no vale");
    comprobar(!H::analiza("\r\n\r\n").valida, "sin linea de peticion no vale");
    comprobar(!H::analiza("GET /\r\n\r\n").valida, "sin version no vale");
    comprobar(!H::analiza("GET / SPDY/1\r\n\r\n").valida, "una version que no es HTTP no vale");
    comprobar(!H::analiza("POST / HTTP/1.1\r\nContent-Length: diez\r\n\r\n").valida,
              "un Content-Length que no es numero no vale");
    // Este es el importante: si el cuerpo llega corto y se aceptara, se leerian campos a
    // medias — o los de la peticion siguiente, si alguien encadena.
    comprobar(!H::analiza("POST / HTTP/1.1\r\nContent-Length: 50\r\n\r\ncorto").valida,
              "un cuerpo mas corto de lo anunciado no vale");

    // --- Escapado: lo que impide que un nombre de dataset acabe ejecutandose.
    igual(H::escapaHtml("<script>alert(1)</script>"),
          "&lt;script&gt;alert(1)&lt;/script&gt;", "las etiquetas se escapan");
    igual(H::escapaHtml("a\"b'c&d"), "a&quot;b&#39;c&amp;d", "y las comillas y el ampersand");
    igual(H::desdeUrl("a+b%2Fc"), "a b/c", "se descodifica %XX y el mas");
    igual(H::desdeUrl("100%"), "100%", "un % suelto se deja: perder el dato es peor");

    // --- La respuesta lleva SIEMPRE las cabeceras que atan al navegador.
    {
        H::Respuesta r;
        r.cuerpo = "hola";
        const std::string s = H::componer(r);
        comprobar(s.find("HTTP/1.1 200 OK") == 0, "la linea de estado");
        comprobar(s.find("Content-Length: 4\r\n") != std::string::npos, "la longitud");
        comprobar(s.find("X-Content-Type-Options: nosniff") != std::string::npos, "nosniff");
        comprobar(s.find("X-Frame-Options: DENY") != std::string::npos, "no meterse en un marco");
        comprobar(s.find("Content-Security-Policy:") != std::string::npos, "y la CSP");
        comprobar(s.rfind("hola") == s.size() - 4, "el cuerpo, al final");
    }

    // --- La sesion y el testigo.
    {
        zfsmgr::web::Sesion s;
        s.abre();
        comprobar(s.abierta(), "la sesion se abre");
        comprobar(s.id().size() == 64 && s.testigo().size() == 64, "32 bytes en hexadecimal cada uno");
        comprobar(s.id() != s.testigo(), "el identificador y el testigo NO son el mismo");
        comprobar(s.cookieVale(s.id()), "su cookie vale");
        comprobar(!s.cookieVale(""), "una vacia no");
        comprobar(!s.cookieVale(s.id().substr(0, 63)), "ni una a la que le falte un byte");
        comprobar(!s.cookieVale(s.testigo()), "ni el testigo como cookie");
        comprobar(s.testigoVale(s.testigo()), "el testigo vale");
        comprobar(!s.testigoVale(s.id()), "y la cookie no vale como testigo");

        // Dos sesiones seguidas NO comparten nada: si esto fallara, el identificador seria
        // previsible y la sesion no seria una sesion.
        zfsmgr::web::Sesion otra;
        otra.abre();
        comprobar(otra.id() != s.id(), "dos sesiones no comparten identificador");
        comprobar(!s.cookieVale(otra.id()), "y la de una no abre la otra");

        const std::string c = s.cabeceraCookie();
        comprobar(c.find("HttpOnly") != std::string::npos, "la cookie es HttpOnly");
        comprobar(c.find("Secure") != std::string::npos, "y Secure");
        comprobar(c.find("SameSite=Strict") != std::string::npos, "y SameSite=Strict");

        s.cierra();
        comprobar(!s.abierta() && !s.cookieVale(s.id()), "cerrada no vale nada");
    }

    std::printf(fallos ? "FALLOS: %d\n" : "web_test OK\n", fallos);
    return fallos ? 1 : 0;
}
