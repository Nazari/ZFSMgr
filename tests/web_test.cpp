// El analizador de HTTP y la sesión, sin levantar un servidor.
//
// Lo que se comprueba aquí no es que «funcione»: es lo que RECHAZA. Un analizador que
// adivina lo que no entiende, mirando lo que manda un navegador, es por donde se cuelan
// las cosas.
#include "dav.h"
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

    // --- WebDAV: el XML del 207.
    {
        namespace D = zfsmgr::web::dav;
        const std::vector<D::Recurso> rs = {
            {"/dav/", "ZFSMgr", true, 0},
            {"/dav/local/", "local", true, 0},
            {"/dav/local/fichero.txt", "fichero.txt", false, 1234},
        };
        const std::string x = D::multiestado(rs, "1");
        comprobar(x.find("<?xml version=\"1.0\" encoding=\"utf-8\"?>") == 0, "dav: la declaracion XML");
        comprobar(x.find("xmlns:D=\"DAV:\"") != std::string::npos, "dav: el espacio de nombres");
        comprobar(x.find("<D:href>/dav/local/</D:href>") != std::string::npos, "dav: los href");
        comprobar(x.find("<D:collection/>") != std::string::npos, "dav: las colecciones se marcan");
        comprobar(x.find("<D:getcontentlength>1234</D:getcontentlength>") != std::string::npos,
                  "dav: y un fichero lleva su tamano");
        // Depth: 0 significa «solo este recurso», no «este y sus hijos». Un explorador que
        // pida 0 y reciba la lista entera se lia.
        const std::string x0 = D::multiestado(rs, "0");
        std::size_t cuantos = 0;
        for (std::size_t i = x0.find("<D:response>"); i != std::string::npos;
             i = x0.find("<D:response>", i + 1)) {
            ++cuantos;
        }
        comprobar(cuantos == 1, "dav: Depth 0 devuelve UN recurso");

        // El escapado: un dataset puede llamarse casi cualquier cosa, y un «&» sin escapar
        // rompe el XML entero — el explorador no ensena nada, no ensena ese nombre mal.
        const std::vector<D::Recurso> raro = {{"/dav/a&b/", "a&b<c>", true, 0}};
        const std::string xr = D::multiestado(raro, "1");
        comprobar(xr.find("a&amp;b") != std::string::npos, "dav: el ampersand del href se escapa");
        comprobar(xr.find("a&amp;b&lt;c&gt;") != std::string::npos, "dav: y el del nombre");
        comprobar(xr.find("<c>") == std::string::npos, "dav: sin dejar etiquetas sueltas");
    }

    // ── 10. Meter un nombre en una URL ──────────────────────────────────────
    //
    // Hace falta desde que la selección del árbol viaja en la consulta: `?sel=<dataset>`.
    // Un nombre de dataset admite casi cualquier cosa, y un «&» ahí dentro no rompe la
    // página — hace algo peor, que es cortar el parámetro y dejar seleccionado OTRO nodo.
    {
        igual(H::haciaUrl("fc16/user"), "fc16/user", "url: lo normal no se toca");
        igual(H::haciaUrl("pool/a-b_c.d:e"), "pool/a-b_c.d:e",
              "url: los signos que ZFS admite tampoco");
        igual(H::haciaUrl("a b"), "a%20b", "url: el espacio");
        igual(H::haciaUrl("a&b"), "a%26b", "url: el ampersand, que es el peligroso");
        igual(H::haciaUrl("a+b"), "a%2Bb", "url: el mas, que al volver seria un espacio");
        igual(H::haciaUrl("d@snap"), "d%40snap", "url: la arroba de las instantaneas");
        igual(H::haciaUrl("a#b"), "a%23b", "url: la almohadilla, que cortaria la URL");
        igual(H::haciaUrl("a%b"), "a%25b", "url: el propio porciento");
        // La vuelta completa: lo que se codifica se tiene que poder descodificar igual.
        const char* const raros[] = {"a b", "a&b", "a+b", "d@s", "a#b", "a%b", "ñ", "a=b"};
        for (const char* r : raros) {
            igual(H::desdeUrl(H::haciaUrl(r)), r, std::string("url: ida y vuelta de «") + r + "»");
        }
        // El control negativo: SIN codificar, «a&b» se parte en dos parametros. Esto es lo
        // que pasaba antes y por lo que la funcion existe.
        const auto p = H::analiza(
            "GET /c/local/pool?sel=a&b&v=props HTTP/1.1\r\nHost: x\r\n\r\n");
        comprobar(p.consulta.find("sel=a&b") != std::string::npos,
                  "url: sin codificar, el nombre se parte (por eso hay que codificar)");
    }

    std::printf(fallos ? "FALLOS: %d\n" : "web_test OK\n", fallos);
    return fallos ? 1 : 0;
}
