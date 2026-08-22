#include "arbolremoto.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <sstream>
#include <system_error>
#include <unordered_map>

#include <openssl/evp.h>
#include <openssl/sha.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace zfsmgr::arbolremoto {
namespace {

// La fecha, en segundos desde el epoch, leída del sistema y no de `std::filesystem`.
//
// `fs::last_write_time` devuelve un `file_time_type` cuyo epoch NO está especificado, y el
// truco habitual en C++17 para pasarlo a tiempo del sistema —restar un `now()` y sumar
// otro— mete un desfase distinto en cada máquina. Aquí eso importa: los dos extremos tienen
// que obtener el MISMO número para el mismo fichero, o la comparación no vale nada. Se
// pregunta al sistema, que sí tiene epoch definido.
std::int64_t fechaDe(const fs::path& p, bool& ok) {
    ok = false;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &d)) {
        return 0;
    }
    ULARGE_INTEGER t;
    t.LowPart = d.ftLastWriteTime.dwLowDateTime;
    t.HighPart = d.ftLastWriteTime.dwHighDateTime;
    // FILETIME cuenta intervalos de 100 ns desde 1601; el epoch de Unix está 11644473600
    // segundos después.
    ok = true;
    return static_cast<std::int64_t>(t.QuadPart / 10000000ULL) - 11644473600LL;
#else
    struct stat st {};
    if (::lstat(p.c_str(), &st) != 0) {
        return 0;
    }
    ok = true;
    return static_cast<std::int64_t>(st.st_mtime);
#endif
}

std::string conBarras(const std::string& s) {
    std::string r = s;
    std::replace(r.begin(), r.end(), '\\', '/');
    return r;
}

char letraDe(Tipo t) {
    switch (t) {
        case Tipo::Directorio: return 'd';
        case Tipo::Fichero:    return 'f';
        case Tipo::Enlace:     return 'l';
        case Tipo::EnlaceDuro: return 'h';
    }
    return 'f';
}

bool tipoDe(char c, Tipo& out) {
    switch (c) {
        case 'd': out = Tipo::Directorio; return true;
        case 'f': out = Tipo::Fichero;    return true;
        case 'l': out = Tipo::Enlace;     return true;
        case 'h': out = Tipo::EnlaceDuro; return true;
        default:  return false;
    }
}

char letraDe(Accion a) {
    switch (a) {
        case Accion::CrearDirectorio: return 'D';
        case Accion::Copiar:          return 'F';
        case Accion::Enlazar:         return 'L';
        case Accion::EnlazarDuro:     return 'H';
        case Accion::Borrar:          return 'X';
    }
    return 'F';
}

bool accionDe(char c, Accion& out) {
    switch (c) {
        case 'D': out = Accion::CrearDirectorio; return true;
        case 'F': out = Accion::Copiar;          return true;
        case 'L': out = Accion::Enlazar;         return true;
        case 'H': out = Accion::EnlazarDuro;     return true;
        case 'X': out = Accion::Borrar;          return true;
        default:  return false;
    }
}

// Lo que NO forma parte del árbol aunque esté dentro, y solo en el primer nivel.
//
// `$RECYCLE.BIN` y `System Volume Information` son del VOLUMEN, no de quien lo usa: Windows
// los pone en la raíz de cada unidad. Sincronizar la raíz de un volumen con borrado se los
// llevaba por delante —visto en una pasada en seco contra una unidad real, que proponía
// borrar la papelera entera— y traerlos al otro extremo tampoco tiene sentido.
//
// Anclados al primer nivel a propósito: un directorio del usuario que se llame igual más
// abajo sí es suyo, y no se toca. Es la misma regla que las exclusiones de `copytree`.
bool esDelVolumen(const std::string& rutaRelativa) {
    if (rutaRelativa.find('/') != std::string::npos) {
        return false;  // no está en el primer nivel
    }
    std::string b;
    b.reserve(rutaRelativa.size());
    for (char c : rutaRelativa) {
        b.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }
    return b == "$recycle.bin" || b == "system volume information"
           || b == "$recycle.bin/" || b == ".fseventsd" || b == ".spotlight-v100"
           || b == ".trashes";
}

// Cuántos niveles tiene una ruta. Sirve para borrar de dentro hacia fuera.
std::size_t hondura(const std::string& ruta) {
    return static_cast<std::size_t>(std::count(ruta.begin(), ruta.end(), '/'));
}

}  // namespace

bool recorre(const std::string& raiz, std::vector<Entrada>& salida, std::string& error,
             bool unSoloSistema) {
    salida.clear();
    error.clear();
    std::error_code ec;
    const fs::path base = fs::path(raiz);
    if (!fs::is_directory(base, ec) || ec) {
        error = "no es un directorio: " + raiz;
        return false;
    }

#ifndef _WIN32
    struct stat raizSt {};
    const bool haySt = (::stat(base.c_str(), &raizSt) == 0);
    // Identidad de cada fichero con varios nombres. Se rellena durante el recorrido pero
    // NO se decide aquí cuál es el original: eso se hace al final, con la lista ordenada.
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> identidad;
#endif

    fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        error = "no se pudo recorrer " + raiz + ": " + ec.message();
        return false;
    }
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            error = "no se pudo recorrer " + raiz + ": " + ec.message();
            return false;
        }
        const fs::path& p = it->path();
        // `lexically_relative` y NO `fs::relative`.
        //
        // `fs::relative` canonicaliza, y canonicalizar SIGUE LOS ENLACES SIMBÓLICOS: un
        // enlace llamado «enlace» que apunta a «a.txt» salía del recorrido con la ruta
        // «a.txt», o sea con el nombre de su destino. Resultado medido: el enlace pisaba al
        // fichero real en la lista y el fichero acababa marcado como enlace duro de sí
        // mismo. Sincronizar así habría destrozado cualquier árbol con enlaces dentro.
        Entrada en;
        en.ruta = conBarras(p.lexically_relative(base).generic_string());
        if (en.ruta.empty() || en.ruta == ".") {
            continue;
        }
        if (esDelVolumen(en.ruta)) {
            std::error_code eV;
            if (it->is_directory(eV)) {
                it.disable_recursion_pending();
            }
            continue;
        }

        std::error_code e3;
        const bool esEnlace = fs::is_symlink(p, e3);
        if (esEnlace) {
            en.tipo = Tipo::Enlace;
            std::error_code e4;
            en.destino = conBarras(fs::read_symlink(p, e4).generic_string());
            salida.push_back(en);
            // No se desciende por un enlace: se copia el enlace, no lo que hay al otro lado.
            if (it->is_directory(e4)) {
                it.disable_recursion_pending();
            }
            continue;
        }

        std::error_code e5;
        if (fs::is_directory(p, e5)) {
            en.tipo = Tipo::Directorio;
            bool okF = false;
            en.fecha = fechaDe(p, okF);
#ifndef _WIN32
            if (unSoloSistema && haySt) {
                struct stat st {};
                if (::stat(p.c_str(), &st) == 0 && st.st_dev != raizSt.st_dev) {
                    // Otro sistema de ficheros: en ZFS, otro dataset montado dentro. No se
                    // baja, y tampoco se anota, porque no forma parte de este árbol.
                    it.disable_recursion_pending();
                    continue;
                }
            }
#else
            (void)unSoloSistema;
#endif
            salida.push_back(en);
            continue;
        }

        std::error_code e6;
        if (!fs::is_regular_file(p, e6)) {
            // Zócalos, tuberías con nombre y dispositivos: no se copian, y decirlo por
            // omisión es peor que no copiarlos. Se saltan en silencio, como hace rsync sin
            // `--devices`/`--specials`.
            continue;
        }
        en.tipo = Tipo::Fichero;
        std::error_code e7;
        en.tamano = static_cast<std::uint64_t>(fs::file_size(p, e7));
        if (e7) {
            en.tamano = 0;
        }
        bool okF = false;
        en.fecha = fechaDe(p, okF);
#ifndef _WIN32
        struct stat st {};
        if (::lstat(p.c_str(), &st) == 0) {
            en.modo = static_cast<std::uint32_t>(st.st_mode & 07777);
            if (st.st_nlink > 1) {
                identidad.emplace(en.ruta,
                                  std::make_pair(static_cast<std::uint64_t>(st.st_dev),
                                                 static_cast<std::uint64_t>(st.st_ino)));
            }
        }
#endif
        salida.push_back(en);
    }

    std::sort(salida.begin(), salida.end(),
              [](const Entrada& a, const Entrada& b) { return a.ruta < b.ruta; });

#ifndef _WIN32
    // Los enlaces duros se resuelven AHORA, sobre la lista ya ordenada, no durante el
    // recorrido.
    //
    // Durante el recorrido, «cuál de los dos nombres es el fichero y cuál el enlace» lo
    // decidía el orden en que el sistema devuelve el directorio, y ese orden no es el mismo
    // en dos máquinas. El resultado habría sido que los dos extremos describen el mismo
    // árbol de forma distinta y la comparación manda rehacer enlaces que ya están bien.
    // Con la lista ordenada, el original es siempre el primero por orden alfabético, y eso
    // sí coincide en los dos lados.
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::string> primero;
    for (Entrada& e : salida) {
        const auto it = identidad.find(e.ruta);
        if (it == identidad.end()) {
            continue;
        }
        const auto ya = primero.find(it->second);
        if (ya == primero.end()) {
            primero.emplace(it->second, e.ruta);
            continue;
        }
        e.tipo = Tipo::EnlaceDuro;
        e.destino = ya->second;
        e.tamano = 0;
    }
#endif
    return true;
}

std::string serializaManifiesto(const std::vector<Entrada>& entradas) {
    std::string out;
    for (const Entrada& e : entradas) {
        out += letraDe(e.tipo);
        out += ' ';
        out += std::to_string(e.tamano);
        out += ' ';
        out += std::to_string(e.fecha);
        out += ' ';
        out += std::to_string(e.modo);
        out += ' ';
        out += std::to_string(e.ruta.size());
        out += ' ';
        out += std::to_string(e.destino.size());
        out += '\n';
        out += e.ruta;
        out += e.destino;
    }
    out += "E\n";
    return out;
}

bool analizaManifiesto(const std::string& texto, std::vector<Entrada>& salida,
                       std::string& error) {
    salida.clear();
    error.clear();
    std::size_t i = 0;
    while (i < texto.size()) {
        const std::size_t fin = texto.find('\n', i);
        if (fin == std::string::npos) {
            error = "manifiesto truncado";
            return false;
        }
        const std::string linea = texto.substr(i, fin - i);
        i = fin + 1;
        if (linea == "E") {
            return true;
        }
        std::istringstream iss(linea);
        char letra = 0;
        std::uint64_t tam = 0;
        std::int64_t fecha = 0;
        std::uint32_t modo = 0;
        std::size_t lr = 0;
        std::size_t ld = 0;
        if (!(iss >> letra >> tam >> fecha >> modo >> lr >> ld)) {
            error = "línea de manifiesto ilegible: " + linea;
            return false;
        }
        Entrada e;
        if (!tipoDe(letra, e.tipo)) {
            error = "tipo desconocido en el manifiesto: " + std::string(1, letra);
            return false;
        }
        if (i + lr + ld > texto.size()) {
            error = "manifiesto truncado en los nombres";
            return false;
        }
        e.tamano = tam;
        e.fecha = fecha;
        e.modo = modo;
        e.ruta = texto.substr(i, lr);
        e.destino = texto.substr(i + lr, ld);
        i += lr + ld;
        salida.push_back(e);
    }
    error = "el manifiesto no termina en «E»";
    return false;
}

Plan planea(const std::vector<Entrada>& origen, const std::vector<Entrada>& destino,
            bool borraLoQueSobra) {
    Plan plan;
    std::map<std::string, const Entrada*> enDestino;
    for (const Entrada& e : destino) {
        enDestino.emplace(e.ruta, &e);
    }

    std::vector<std::string> vistas;
    vistas.reserve(origen.size());
    for (const Entrada& e : origen) {
        vistas.push_back(e.ruta);
        const auto it = enDestino.find(e.ruta);
        const Entrada* alli = (it == enDestino.end()) ? nullptr : it->second;

        switch (e.tipo) {
            case Tipo::Directorio:
                if (alli == nullptr || alli->tipo != Tipo::Directorio) {
                    plan.operaciones.push_back({Accion::CrearDirectorio, e});
                } else {
                    ++plan.iguales;
                }
                break;
            case Tipo::Enlace:
                if (alli == nullptr || alli->tipo != Tipo::Enlace || alli->destino != e.destino) {
                    plan.operaciones.push_back({Accion::Enlazar, e});
                } else {
                    ++plan.iguales;
                }
                break;
            case Tipo::EnlaceDuro:
                // Un enlace duro se rehace siempre que no esté ya como tal: comprobar que
                // los dos nombres comparten inodo EN EL DESTINO costaría otro manifiesto,
                // y rehacerlo es barato porque no mueve datos.
                if (alli == nullptr || alli->tipo != Tipo::EnlaceDuro
                    || alli->destino != e.destino) {
                    plan.operaciones.push_back({Accion::EnlazarDuro, e});
                } else {
                    ++plan.iguales;
                }
                break;
            case Tipo::Fichero:
                // La misma regla que `copytree` en local: tamaño y fecha. La fecha va en
                // segundos enteros porque es lo único que dos sistemas de ficheros
                // distintos pueden comparar; ver el comentario de `Entrada::fecha`.
                if (alli != nullptr && alli->tipo == Tipo::Fichero && alli->tamano == e.tamano
                    && alli->fecha == e.fecha) {
                    ++plan.iguales;
                } else {
                    plan.operaciones.push_back({Accion::Copiar, e});
                    plan.bytes += e.tamano;
                }
                break;
        }
    }

    if (!borraLoQueSobra) {
        return plan;
    }
    std::map<std::string, bool> enOrigen;
    for (const std::string& r : vistas) {
        enOrigen.emplace(r, true);
    }
    std::vector<Entrada> sobran;
    for (const Entrada& e : destino) {
        if (enOrigen.find(e.ruta) == enOrigen.end()) {
            sobran.push_back(e);
        }
    }
    // De más hondo a menos hondo: un directorio se borra DESPUÉS de lo que tiene dentro.
    std::sort(sobran.begin(), sobran.end(), [](const Entrada& a, const Entrada& b) {
        const std::size_t ha = hondura(a.ruta);
        const std::size_t hb = hondura(b.ruta);
        if (ha != hb) {
            return ha > hb;
        }
        return a.ruta > b.ruta;
    });
    for (const Entrada& e : sobran) {
        plan.operaciones.push_back({Accion::Borrar, e});
    }
    return plan;
}

std::string describe(const Operacion& o) {
    switch (o.accion) {
        case Accion::CrearDirectorio: return "cd+++++++++ " + o.entrada.ruta + "/";
        case Accion::Copiar:          return ">f+++++++++ " + o.entrada.ruta;
        case Accion::Enlazar:         return "cL+++++++++ " + o.entrada.ruta + " -> "
                                             + o.entrada.destino;
        case Accion::EnlazarDuro:     return "hf+++++++++ " + o.entrada.ruta + " => "
                                             + o.entrada.destino;
        case Accion::Borrar:          return "*deleting   " + o.entrada.ruta;
    }
    return {};
}

std::string cabeceraDe(const Operacion& o) {
    std::string h;
    h += letraDe(o.accion);
    h += ' ';
    h += std::to_string(o.entrada.modo);
    h += ' ';
    h += std::to_string(o.entrada.fecha);
    h += ' ';
    h += std::to_string(o.entrada.tamano);
    h += ' ';
    h += std::to_string(o.entrada.ruta.size());
    h += ' ';
    h += std::to_string(o.entrada.destino.size());
    h += '\n';
    return h;
}

bool analizaCabecera(const std::string& linea, Operacion& salida, std::size_t& largoRuta,
                     std::size_t& largoDestino, std::string& error) {
    error.clear();
    std::istringstream iss(linea);
    char letra = 0;
    std::uint32_t modo = 0;
    std::int64_t fecha = 0;
    std::uint64_t tam = 0;
    std::size_t lr = 0;
    std::size_t ld = 0;
    if (!(iss >> letra >> modo >> fecha >> tam >> lr >> ld)) {
        error = "cabecera ilegible: " + linea;
        return false;
    }
    if (!accionDe(letra, salida.accion)) {
        error = "acción desconocida: " + std::string(1, letra);
        return false;
    }
    salida.entrada = Entrada{};
    salida.entrada.modo = modo;
    salida.entrada.fecha = fecha;
    salida.entrada.tamano = tam;
    largoRuta = lr;
    largoDestino = ld;
    return true;
}

// --- Delta ------------------------------------------------------------------

std::size_t tamanoDeBloque(std::uint64_t tamanoFichero) {
    if (tamanoFichero < 16ULL * 1024 * 1024) {
        return 8 * 1024;
    }
    if (tamanoFichero < 1024ULL * 1024 * 1024) {
        return 64 * 1024;
    }
    return 256 * 1024;
}

std::uint32_t sumaRodante(const unsigned char* datos, std::size_t n) {
    // La de rsync: una suma de los bytes y otra ponderada por la posición. La segunda es la
    // que hace que dos trozos con los mismos bytes en distinto orden NO coincidan.
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    for (std::size_t i = 0; i < n; ++i) {
        a += datos[i];
        b += static_cast<std::uint32_t>(n - i) * datos[i];
    }
    return (a & 0xffff) | ((b & 0xffff) << 16);
}

std::string hashFuerteHex(const unsigned char* datos, std::size_t n) {
    unsigned char h[SHA256_DIGEST_LENGTH];
    SHA256(datos, n, h);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(sizeof(h) * 2);
    for (unsigned char c : h) {
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 0x0f]);
    }
    return out;
}

bool hashDeFichero(const std::string& ruta, std::string& hexOut, std::string& error) {
    hexOut.clear();
    std::FILE* f = std::fopen(ruta.c_str(), "rb");
    if (f == nullptr) {
        error = "no se pudo abrir " + ruta;
        return false;
    }
    // EVP y no `SHA256_Init/Update/Final`: esos están obsoletos desde OpenSSL 3.0 y avisan
    // en cada compilación.
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr || EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        if (ctx != nullptr) { EVP_MD_CTX_free(ctx); }
        std::fclose(f);
        error = "no se pudo iniciar el resumen";
        return false;
    }
    std::vector<unsigned char> buf(65536);
    for (;;) {
        const std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n == 0) {
            break;
        }
        EVP_DigestUpdate(ctx, buf.data(), n);
    }
    std::fclose(f);
    unsigned char h[SHA256_DIGEST_LENGTH];
    unsigned int largo = 0;
    EVP_DigestFinal_ex(ctx, h, &largo);
    EVP_MD_CTX_free(ctx);
    static const char* hex = "0123456789abcdef";
    for (unsigned char c : h) {
        hexOut.push_back(hex[c >> 4]);
        hexOut.push_back(hex[c & 0x0f]);
    }
    return true;
}

bool firmasDe(const std::string& ruta, std::size_t tamBloque, std::vector<Firma>& salida,
              std::string& error) {
    salida.clear();
    std::FILE* f = std::fopen(ruta.c_str(), "rb");
    if (f == nullptr) {
        error = "no se pudo abrir " + ruta;
        return false;
    }
    std::vector<unsigned char> buf(tamBloque);
    for (;;) {
        const std::size_t n = std::fread(buf.data(), 1, tamBloque, f);
        if (n == 0) {
            break;
        }
        Firma fi;
        fi.debil = sumaRodante(buf.data(), n);
        unsigned char h[SHA256_DIGEST_LENGTH];
        SHA256(buf.data(), n, h);
        std::memcpy(fi.fuerte, h, sizeof(fi.fuerte));
        salida.push_back(fi);
        if (n < tamBloque) {
            break;  // el último bloque puede ser corto
        }
    }
    std::fclose(f);
    return true;
}

std::string serializaFirmas(const std::vector<Firma>& f) {
    std::string out;
    out.reserve(f.size() * (4 + 16));
    for (const Firma& x : f) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<char>((x.debil >> (8 * i)) & 0xff));
        }
        out.append(reinterpret_cast<const char*>(x.fuerte), sizeof(x.fuerte));
    }
    return out;
}

bool analizaFirmas(const std::string& datos, std::vector<Firma>& salida, std::string& error) {
    salida.clear();
    if (datos.size() % 20 != 0) {
        error = "las firmas llegan a medias";
        return false;
    }
    for (std::size_t i = 0; i < datos.size(); i += 20) {
        Firma f;
        f.debil = 0;
        for (int k = 0; k < 4; ++k) {
            f.debil |= static_cast<std::uint32_t>(static_cast<unsigned char>(datos[i + k]))
                       << (8 * k);
        }
        std::memcpy(f.fuerte, datos.data() + i + 4, sizeof(f.fuerte));
        salida.push_back(f);
    }
    return true;
}

bool delta(const std::string& ruta, const std::vector<Firma>& firmas, std::size_t tamBloque,
           std::vector<Instruccion>& salida, std::uint64_t& bytesLiterales,
           std::string& error) {
    salida.clear();
    bytesLiterales = 0;
    if (tamBloque == 0) {
        error = "tamaño de bloque cero";
        return false;
    }
    std::FILE* f = std::fopen(ruta.c_str(), "rb");
    if (f == nullptr) {
        error = "no se pudo abrir " + ruta;
        return false;
    }

    // Suma débil → qué bloques la tienen. Varios pueden compartirla; por eso el hash fuerte.
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> porDebil;
    for (std::size_t i = 0; i < firmas.size(); ++i) {
        porDebil[firmas[i].debil].push_back(i);
    }

    // El buffer lleva SIEMPRE al menos una ventana entera por delante mientras quede
    // fichero. Sin ese arrastre habría que releer, y la búsqueda dejaría de ser de una sola
    // pasada.
    const std::size_t capacidad = std::max<std::size_t>(tamBloque * 4, 1024 * 1024);
    std::vector<unsigned char> buf(capacidad);
    std::size_t ini = 0;
    std::size_t fin = 0;
    bool acabado = false;
    std::string literal;

    const auto rellena = [&]() {
        if (ini > 0) {
            std::memmove(buf.data(), buf.data() + ini, fin - ini);
            fin -= ini;
            ini = 0;
        }
        while (!acabado && fin < buf.size()) {
            const std::size_t n = std::fread(buf.data() + fin, 1, buf.size() - fin, f);
            if (n == 0) {
                acabado = true;
                break;
            }
            fin += n;
        }
    };

    const auto sueltaLiteral = [&]() {
        if (literal.empty()) {
            return;
        }
        Instruccion in;
        in.tipo = TipoInstruccion::Literal;
        in.datos = literal;
        bytesLiterales += literal.size();
        salida.push_back(std::move(in));
        literal.clear();
    };

    rellena();
    std::uint32_t suma = 0;
    bool haySuma = false;
    // El relleno va ANTES de comprobar si queda algo, y no al revés.
    //
    // Con `while (ini < fin)` delante, un consumo que caía justo en el final del búfer
    // dejaba `ini == fin` y el bucle salía SIN volver a leer: el delta describía solo el
    // primer búfer del fichero y el resto desaparecía. Con 8 KiB de bloque el búfer es de
    // 1 MiB, así que cualquier fichero mayor se truncaba. No se vio en las aserciones
    // porque allí el delta se comprobaba en memoria; lo cazó la verificación del hash del
    // fichero entero al aplicarlo, que para eso está.
    for (;;) {
        if (fin - ini < tamBloque && !acabado) {
            rellena();
            haySuma = false;
        }
        if (ini >= fin) {
            break;
        }
        const std::size_t ventana = std::min<std::size_t>(tamBloque, fin - ini);
        if (!haySuma || ventana != tamBloque) {
            suma = sumaRodante(buf.data() + ini, ventana);
            haySuma = true;
        }

        bool casó = false;
        const auto it = porDebil.find(suma);
        if (it != porDebil.end()) {
            unsigned char h[SHA256_DIGEST_LENGTH];
            SHA256(buf.data() + ini, ventana, h);
            // Entre varios bloques que valgan, se prefiere EL SIGUIENTE al último copiado.
            //
            // No es un capricho: en un fichero con trozos repetidos —ceros, relleno, un
            // disco virtual medio vacío— todos esos bloques tienen la misma firma, y coger
            // siempre el primero de la lista genera una instrucción por bloque en vez de
            // una para todos. Medido: 300 KB de un solo byte repetido daban 37
            // instrucciones; con esto, una.
            std::vector<std::size_t> candidatos = it->second;
            if (!salida.empty() && salida.back().tipo == TipoInstruccion::Copiar) {
                const std::size_t siguiente =
                    static_cast<std::size_t>(salida.back().bloque + salida.back().cuantos);
                for (std::size_t k = 0; k < candidatos.size(); ++k) {
                    if (candidatos[k] == siguiente) {
                        std::swap(candidatos[0], candidatos[k]);
                        break;
                    }
                }
            }
            for (const std::size_t idx : candidatos) {
                if (std::memcmp(firmas[idx].fuerte, h, sizeof(firmas[idx].fuerte)) != 0) {
                    continue;
                }
                sueltaLiteral();
                // Bloques seguidos se juntan en una sola instrucción: un fichero que no ha
                // cambiado nada se resuelve con UNA, no con una por bloque.
                if (!salida.empty() && salida.back().tipo == TipoInstruccion::Copiar
                    && salida.back().bloque + salida.back().cuantos == idx) {
                    ++salida.back().cuantos;
                } else {
                    Instruccion in;
                    in.tipo = TipoInstruccion::Copiar;
                    in.bloque = idx;
                    in.cuantos = 1;
                    salida.push_back(in);
                }
                ini += ventana;
                haySuma = false;
                casó = true;
                break;
            }
        }
        if (casó) {
            continue;
        }

        // No casó: este byte es literal y la ventana avanza UNO. Aquí es donde la suma
        // rodante se gana su nombre: se actualiza sin volver a recorrer la ventana.
        literal.push_back(static_cast<char>(buf[ini]));
        const unsigned char sale = buf[ini];
        ++ini;
        if (haySuma && ventana == tamBloque && ini + tamBloque <= fin) {
            const unsigned char entra = buf[ini + tamBloque - 1];
            std::uint32_t a = suma & 0xffff;
            std::uint32_t b = (suma >> 16) & 0xffff;
            a = (a - sale + entra) & 0xffff;
            b = (b - static_cast<std::uint32_t>(tamBloque) * sale + a) & 0xffff;
            suma = a | (b << 16);
        } else {
            haySuma = false;
        }
    }
    sueltaLiteral();
    std::fclose(f);
    return true;
}

bool ponFecha(const std::string& ruta, std::int64_t segundos) {
#ifdef _WIN32
    HANDLE h = CreateFileW(fs::path(ruta).wstring().c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    ULARGE_INTEGER t;
    t.QuadPart = (static_cast<unsigned long long>(segundos) + 11644473600ULL) * 10000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = t.LowPart;
    ft.dwHighDateTime = t.HighPart;
    const BOOL ok = SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    return ok != 0;
#else
    struct timeval tv[2];
    tv[0].tv_sec = static_cast<time_t>(segundos);
    tv[0].tv_usec = 0;
    tv[1] = tv[0];
    return ::utimes(ruta.c_str(), tv) == 0;
#endif
}

bool ponModo(const std::string& ruta, std::uint32_t modo) {
#ifdef _WIN32
    (void)ruta;
    (void)modo;
    // NTFS no tiene modo de Unix, y emularlo con ACL sería una aproximación justo donde
    // más se nota. Se deja como está: mejor no tocar los permisos que ponerlos mal.
    return true;
#else
    if (modo == 0) {
        return true;
    }
    return ::chmod(ruta.c_str(), static_cast<mode_t>(modo & 07777)) == 0;
#endif
}

std::int64_t fechaDeFichero(const std::string& ruta, bool& ok) {
    return fechaDe(fs::path(ruta), ok);
}

}  // namespace zfsmgr::arbolremoto
