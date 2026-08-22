#include "tlsserver.h"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketT = SOCKET;
#define CIERRA_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
using SocketT = int;
#define INVALID_SOCKET (-1)
#define CIERRA_SOCKET ::close
#endif

namespace zfsmgr::base::tlsserver {
namespace {

// Los sockets NO se heredan. Quien pide algo por aquí acaba provocando que se lance un
// proceso —`zfs`, casi siempre—, y todo descriptor abierto en ese momento se lo lleva el
// hijo. Un hijo que se quede con la conexión del cliente la mantiene viva aunque el
// servidor la cierre, así que el otro extremo no ve nunca el final.
//
// No es hipotético: el mismo descuido en los sockets de transferencia dejaba un
// `zfs send` sujetando el extremo del receptor y colgaba la copia para siempre.
// Se marca AL CREAR y AL ACEPTAR, que es cuando puede hacerse de forma atómica: entre un
// socket() y un fcntl() posterior cabe justo el fork de otro hilo, que es la carrera que
// se quiere cerrar.
void noHeredar(SocketT s) {
#ifdef _WIN32
    SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
#else
    const int f = ::fcntl(s, F_GETFD, 0);
    if (f >= 0) { ::fcntl(s, F_SETFD, f | FD_CLOEXEC); }
#endif
}

SocketT creaSocketSinHerencia(int familia, int tipo, int proto) {
#if defined(__linux__) || defined(__FreeBSD__)
    return ::socket(familia, tipo | SOCK_CLOEXEC, proto);
#else
    const SocketT s = ::socket(familia, tipo, proto);
    if (s != INVALID_SOCKET) { noHeredar(s); }
    return s;
#endif
}

SocketT aceptaSinHerencia(SocketT escucha) {
#if defined(__linux__) || defined(__FreeBSD__)
    return ::accept4(escucha, nullptr, nullptr, SOCK_CLOEXEC);
#else
    // macOS y Windows no tienen accept4: ahí queda la ventana entre aceptar y marcar.
    const SocketT s = ::accept(escucha, nullptr, nullptr);
    if (s != INVALID_SOCKET) { noHeredar(s); }
    return s;
#endif
}

std::string errorDeOpenssl() {
    const unsigned long e = ERR_get_error();
    if (e == 0) {
        return {};
    }
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    return buf;
}

}  // namespace

bool escribeParAutofirmado(const std::string& rutaCert, const std::string& rutaClave,
                           const std::string& commonName, bool paraServidor,
                           const std::string& altNames, std::string& error) {
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    if (!pkey) {
        error = "no se pudo generar la clave RSA";
        return false;
    }
    X509* x = X509_new();
    if (!x) {
        EVP_PKEY_free(pkey);
        error = "no se pudo crear el certificado";
        return false;
    }
    X509_set_version(x, 2);  // v3
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_get_notBefore(x), 0);
    X509_gmtime_adj(X509_get_notAfter(x), 60L * 60 * 24 * 3650);
    X509_set_pubkey(x, pkey);

    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(commonName.c_str()), -1, -1, 0);
    X509_set_issuer_name(x, name);  // autofirmado: emisor = sujeto

    auto addExt = [&](int nid, const char* value) {
        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, x, x, nullptr, nullptr, 0);
        X509_EXTENSION* ex = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
        if (ex) {
            X509_add_ext(x, ex, -1);
            X509_EXTENSION_free(ex);
        }
    };
    addExt(NID_basic_constraints, "critical,CA:TRUE");
    addExt(NID_subject_alt_name, altNames.c_str());
    addExt(NID_key_usage, "critical,digitalSignature,keyEncipherment,keyCertSign");
    addExt(NID_ext_key_usage, paraServidor ? "serverAuth" : "clientAuth");

    if (!X509_sign(x, pkey, EVP_sha256())) {
        X509_free(x);
        EVP_PKEY_free(pkey);
        error = "no se pudo firmar el certificado";
        return false;
    }

    bool ok = false;
    if (FILE* cf = std::fopen(rutaCert.c_str(), "wb")) {
        ok = PEM_write_X509(cf, x) == 1;
        std::fclose(cf);
    }
    if (ok) {
        ok = false;
        if (FILE* kf = std::fopen(rutaClave.c_str(), "wb")) {
            ok = PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1;
            std::fclose(kf);
        }
    }
    X509_free(x);
    EVP_PKEY_free(pkey);
    if (!ok) {
        error = "no se pudieron escribir los ficheros PEM";
        return false;
    }
#ifndef _WIN32
    ::chmod(rutaCert.c_str(), 0600);
    ::chmod(rutaClave.c_str(), 0600);
#endif
    return true;
}

bool sirve(const std::string& bind, int puerto, const std::string& rutaCert,
           const std::string& rutaClave,
           const std::function<bool(const std::string&, std::string&)>& atiende,
           const std::function<bool()>& sigueVivo, std::string& error,
           const std::function<void()>& alEscuchar,
           const std::function<bool(const std::string&, const Escritor&)>& atiendeChorro) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        error = "no se pudo crear el contexto TLS: " + errorDeOpenssl();
        return false;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_file(ctx, rutaCert.c_str(), SSL_FILETYPE_PEM) != 1
        || SSL_CTX_use_PrivateKey_file(ctx, rutaClave.c_str(), SSL_FILETYPE_PEM) != 1) {
        error = "el certificado o la clave no sirven: " + errorDeOpenssl();
        SSL_CTX_free(ctx);
        return false;
    }

    const SocketT escucha = creaSocketSinHerencia(AF_INET, SOCK_STREAM, 0);
    if (escucha == INVALID_SOCKET) {
        error = "no se pudo crear el socket";
        SSL_CTX_free(ctx);
        return false;
    }
    int uno = 1;
    ::setsockopt(escucha, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&uno), sizeof(uno));
    sockaddr_in dir{};
    dir.sin_family = AF_INET;
    dir.sin_port = htons(static_cast<unsigned short>(puerto));
    if (::inet_pton(AF_INET, bind.c_str(), &dir.sin_addr) != 1) {
        error = "dirección de escucha no válida: " + bind;
        CIERRA_SOCKET(escucha);
        SSL_CTX_free(ctx);
        return false;
    }
    if (::bind(escucha, reinterpret_cast<sockaddr*>(&dir), sizeof(dir)) != 0
        || ::listen(escucha, 16) != 0) {
        // El MOTIVO, no solo el hecho. «No se pudo escuchar» a secas obliga a adivinar
        // entre un puerto cogido, un permiso y una dirección que no es de esta máquina, y
        // el sistema acaba de decir cuál de las tres es.
#ifdef _WIN32
        const int codigo = WSAGetLastError();
        const bool cogido = (codigo == WSAEADDRINUSE);
        const std::string porQue = "código " + std::to_string(codigo);
#else
        const int codigo = errno;
        const bool cogido = (codigo == EADDRINUSE);
        const std::string porQue = std::strerror(codigo);
#endif
        error = "no se pudo escuchar en " + bind + ":" + std::to_string(puerto) + ": " + porQue;
        if (cogido) {
            error += "\n"
                     "Ese puerto ya lo tiene otro proceso: otra copia de este mismo servidor,\n"
                     "o un túnel SSH que quedó suelto de una ejecución anterior. Use\n"
                     "«--port <otro>», o mire quién lo tiene con «ss -ltnp | grep "
                   + std::to_string(puerto) + "».";
        }
        CIERRA_SOCKET(escucha);
        SSL_CTX_free(ctx);
        return false;
    }
    if (alEscuchar) {
        alEscuchar();
    }

    while (!sigueVivo || sigueVivo()) {
        const SocketT cliente = aceptaSinHerencia(escucha);
        if (cliente == INVALID_SOCKET) {
            continue;
        }
        SSL* ssl = SSL_new(ctx);
        SSL_set_fd(ssl, static_cast<int>(cliente));
        if (SSL_accept(ssl) == 1) {
            // La petición, hasta el final de las cabeceras y su cuerpo si lo anuncia.
            std::string peticion;
            char buf[4096];
            while (true) {
                const int n = SSL_read(ssl, buf, sizeof(buf));
                if (n <= 0) {
                    break;
                }
                peticion.append(buf, static_cast<std::size_t>(n));
                const std::size_t fin = peticion.find("\r\n\r\n");
                if (fin == std::string::npos) {
                    continue;
                }
                // Con cuerpo: se sigue leyendo hasta completar Content-Length.
                std::size_t largo = 0;
                const std::string cabeceras = peticion.substr(0, fin);
                std::size_t p = cabeceras.find("Content-Length:");
                if (p == std::string::npos) {
                    p = cabeceras.find("content-length:");
                }
                if (p != std::string::npos) {
                    largo = static_cast<std::size_t>(std::atol(cabeceras.c_str() + p + 15));
                }
                if (peticion.size() >= fin + 4 + largo) {
                    break;
                }
            }
            bool yaContestado = false;
            if (!peticion.empty() && atiendeChorro) {
                const Escritor escribe = [ssl](const char* datos, std::size_t cuantos) {
                    std::size_t puesto = 0;
                    while (puesto < cuantos) {
                        const int n = SSL_write(ssl, datos + puesto,
                                                static_cast<int>(cuantos - puesto));
                        if (n <= 0) {
                            return false;   // el otro colgó: no tiene sentido seguir leyendo
                        }
                        puesto += static_cast<std::size_t>(n);
                    }
                    return true;
                };
                yaContestado = atiendeChorro(peticion, escribe);
            }
            std::string respuesta;
            if (!yaContestado && !peticion.empty() && atiende(peticion, respuesta)
                && !respuesta.empty()) {
                SSL_write(ssl, respuesta.data(), static_cast<int>(respuesta.size()));
            }
        }
        SSL_shutdown(ssl);
        SSL_free(ssl);
        CIERRA_SOCKET(cliente);
    }
    CIERRA_SOCKET(escucha);
    SSL_CTX_free(ctx);
    return true;
}

}  // namespace zfsmgr::base::tlsserver
