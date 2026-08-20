#include "tlsclient.h"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SockT = SOCKET;
constexpr SockT kSockInvalido = INVALID_SOCKET;
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using SockT = int;
constexpr SockT kSockInvalido = -1;
#endif

namespace zfsmgr::base {
namespace {

void cierra(SockT s) {
    if (s == kSockInvalido) {
        return;
    }
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

#ifdef _WIN32
// Winsock necesita arrancarse una vez por proceso. Se hace aquí y no en quien llama para
// que nadie tenga que acordarse.
void aseguraWinsock() {
    static bool hecho = false;
    if (!hecho) {
        WSADATA d{};
        WSAStartup(MAKEWORD(2, 2), &d);
        hecho = true;
    }
}
#endif

std::string ultimoErrorOpenssl() {
    const unsigned long e = ERR_get_error();
    if (e == 0) {
        return {};
    }
    char buf[256] = {0};
    ERR_error_string_n(e, buf, sizeof(buf));
    return buf;
}

void ponTiempoLimite(SockT s, int ms) {
#ifdef _WIN32
    DWORD v = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof(v));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&v), sizeof(v));
#else
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// Conecta por TCP, probando todas las direcciones que devuelva la resolución: una máquina
// con IPv6 e IPv4 puede tener la primera caída y la segunda no.
SockT conecta(const std::string& host, unsigned short port, int timeoutMs, std::string& error) {
#ifdef _WIN32
    aseguraWinsock();
#endif
    addrinfo pistas{};
    pistas.ai_family = AF_UNSPEC;
    pistas.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string puerto = std::to_string(port);
    if (getaddrinfo(host.c_str(), puerto.c_str(), &pistas, &res) != 0 || !res) {
        error = "no se pudo resolver " + host;
        return kSockInvalido;
    }
    SockT s = kSockInvalido;
    for (addrinfo* it = res; it; it = it->ai_next) {
        s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (s == kSockInvalido) {
            continue;
        }
        ponTiempoLimite(s, timeoutMs);
        if (::connect(s, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
            break;
        }
        cierra(s);
        s = kSockInvalido;
    }
    freeaddrinfo(res);
    if (s == kSockInvalido) {
        error = "no se pudo conectar a " + host + ":" + puerto;
    }
    return s;
}

// Lee un certificado en PEM. Solo el PRIMERO: el material del daemon lleva uno.
X509* leeCertificado(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) {
        return nullptr;
    }
    X509* c = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return c;
}

EVP_PKEY* leeClave(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) {
        return nullptr;
    }
    // Sin distinguir RSA de EC: PEM_read_bio_PrivateKey reconoce las dos, cosa que en la
    // versión con Qt había que intentar por separado.
    EVP_PKEY* k = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return k;
}

}  // namespace

bool pemCertificateIsValid(const std::string& pem) {
    X509* c = leeCertificado(pem);
    if (!c) {
        return false;
    }
    X509_free(c);
    return true;
}

bool pemPrivateKeyIsValid(const std::string& pem) {
    EVP_PKEY* k = leeClave(pem);
    if (!k) {
        return false;
    }
    EVP_PKEY_free(k);
    return true;
}

bool tlsRequestLine(const TlsClientConfig& cfg,
                    const std::string& requestLine,
                    std::string& responseLine,
                    std::string& error) {
    TlsFailure f = TlsFailure::None;
    return tlsRequestLine(cfg, requestLine, responseLine, error, f, TlsRequestHooks{});
}

bool tlsRequestLine(const TlsClientConfig& cfg,
                    const std::string& requestLine,
                    std::string& responseLine,
                    std::string& error,
                    TlsFailure& failure,
                    const TlsRequestHooks& hooks) {
    responseLine.clear();
    error.clear();
    failure = TlsFailure::None;
    if (cfg.host.empty() || cfg.port == 0) {
        error = "destino sin host o sin puerto";
        failure = TlsFailure::Connect;
        return false;
    }

    X509* certEsperado = leeCertificado(cfg.serverCertPem);
    X509* certCliente = leeCertificado(cfg.clientCertPem);
    EVP_PKEY* claveCliente = leeClave(cfg.clientKeyPem);
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    SockT sock = kSockInvalido;
    bool ok = false;

    // Un solo punto de salida: con OpenSSL de por medio, repartir la liberación entre
    // veinte returns es como se filtran descriptores y contextos.
    do {
        if (!certEsperado) {
            error = "el certificado del daemon no es un PEM válido";
            failure = TlsFailure::BadMaterial;
            break;
        }
        if (!certCliente || !claveCliente) {
            error = "el certificado o la clave del cliente no son PEM válidos";
            failure = TlsFailure::BadMaterial;
            break;
        }
        ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) {
            error = "no se pudo crear el contexto TLS";
            failure = TlsFailure::Handshake;
            break;
        }
        // TLS 1.2 como mínimo, igual que la versión con Qt.
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        // La validación de cadena se apaga A PROPÓSITO: se valida por fijación, más
        // abajo. Ver la explicación en la cabecera — no es un descuido.
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        if (SSL_CTX_use_certificate(ctx, certCliente) != 1
            || SSL_CTX_use_PrivateKey(ctx, claveCliente) != 1) {
            error = "el certificado y la clave del cliente no casan: " + ultimoErrorOpenssl();
            failure = TlsFailure::BadMaterial;
            break;
        }

        sock = conecta(cfg.host, cfg.port, cfg.connectTimeoutMs, error);
        if (sock == kSockInvalido) {
            failure = TlsFailure::Connect;
            break;
        }
        ssl = SSL_new(ctx);
        if (!ssl) {
            error = "no se pudo crear la sesión TLS";
            failure = TlsFailure::Handshake;
            break;
        }
        SSL_set_fd(ssl, static_cast<int>(sock));
        // SNI: el daemon no lo exige, pero un intermediario que lo mire debe ver el
        // nombre real y no quedarse esperando.
        SSL_set_tlsext_host_name(ssl, cfg.host.c_str());
        if (SSL_connect(ssl) != 1) {
            error = "no se pudo negociar TLS: " + ultimoErrorOpenssl();
            failure = TlsFailure::Handshake;
            break;
        }

        // --- LA FIJACIÓN. Es la única validación del par, y va ANTES de escribir nada.
        X509* certPar = SSL_get1_peer_certificate(ssl);
        if (!certPar) {
            error = "el daemon no presentó certificado";
            failure = TlsFailure::Pinning;
            break;
        }
        const bool esElEsperado = X509_cmp(certPar, certEsperado) == 0;
        X509_free(certPar);
        if (!esElEsperado) {
            error = "el certificado del daemon no es el esperado";
            failure = TlsFailure::Pinning;
            break;
        }

        if (hooks.onBeforeWrite) {
            hooks.onBeforeWrite();
        }
        std::string peticion = requestLine;
        if (peticion.empty() || peticion.back() != '\n') {
            peticion.push_back('\n');
        }
        std::size_t escrito = 0;
        while (escrito < peticion.size()) {
            const int n = SSL_write(ssl, peticion.data() + escrito,
                                    static_cast<int>(peticion.size() - escrito));
            if (n <= 0) {
                error = "se cortó al enviar la petición";
                failure = TlsFailure::Write;
                break;
            }
            escrito += static_cast<std::size_t>(n);
        }
        if (escrito < peticion.size()) {
            break;
        }

        // La respuesta es UNA línea. Se lee hasta el salto o hasta que el otro cierre; el
        // tope evita que un daemon estropeado nos haga crecer sin fin.
        //
        // Con `keepWaiting` puesto la espera se trocea: el socket lleva un plazo CORTO y
        // se vuelve a preguntar entre lectura y lectura. Sin eso, un `ioTimeoutMs` de cero
        // —«sin límite», que es lo que piden las operaciones que copian datos de verdad—
        // dejaría la lectura bloqueada para siempre y nunca se llegaría a mirar si el
        // túnel sigue vivo.
        constexpr std::size_t kTope = 64u * 1024u * 1024u;
        constexpr int kTrozoMs = 300;
        // **La espera se trocea SIEMPRE, haya o no `keepWaiting`.**
        //
        // Antes solo se rearmaba el plazo cuando había a quién preguntar, y sin eso el
        // socket se quedaba con el que le puso `conecta()`: el de CONEXIÓN, que para el
        // daemon local vale como mucho 700 ms. O sea que cualquier respuesta que tardara
        // más de esos 700 ms se daba por perdida —«el daemon no respondió»— por muy alto
        // que fuera el plazo pedido, que para una mutación son 120 segundos.
        //
        // No es un caso raro: `zpool sync` sobre un pool de 2,46 TB tarda 2,4 s, y
        // arrancar un scrub otro tanto. La orden LLEGABA y el daemon la ejecutaba —queda
        // en su registro—; lo que fallaba era el cliente al recoger la respuesta, así que
        // la acción se hacía y se contaba como error. Es la peor mezcla de las dos.
        ponTiempoLimite(sock, kTrozoMs);
        const auto inicioLectura = std::chrono::steady_clock::now();
        const auto agotado = [&]() {
            if (cfg.ioTimeoutMs <= 0) {
                return false;  // sin límite
            }
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - inicioLectura)
                       .count()
                   > cfg.ioTimeoutMs;
        };
        bool abandonado = false;
        char buf[8192];
        while (responseLine.size() < kTope) {
            const int n = SSL_read(ssl, buf, static_cast<int>(sizeof(buf)));
            if (n > 0) {
                responseLine.append(buf, static_cast<std::size_t>(n));
                const std::size_t nl = responseLine.find('\n');
                if (nl != std::string::npos) {
                    responseLine.resize(nl);
                    break;
                }
                continue;
            }
            const int motivo = SSL_get_error(ssl, n);
            // Al agotarse el trozo, OpenSSL suele decir WANT_READ; pero según la
            // plataforma puede reportar SSL_ERROR_SYSCALL con EAGAIN, que significa lo
            // mismo. Tomarlo por un corte cerraría la espera antes de tiempo, y eso en una
            // transferencia larga es dar por muerta una operación que va bien.
            const bool esperaDelSistema =
                (motivo == SSL_ERROR_SYSCALL
                 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
            const bool soloEsperando =
                (motivo == SSL_ERROR_WANT_READ || motivo == SSL_ERROR_WANT_WRITE
                 || esperaDelSistema);
            if (!soloEsperando) {
                break;  // el otro cerró de verdad: se acabó
            }
            // Se agotó el trozo, no la espera. Aquí se decide si tiene sentido seguir: lo
            // manda `ioTimeoutMs`, que es el plazo que pidió quien llamó, y no el del
            // socket, que ahora solo sirve para volver a pasar por aquí.
            if (hooks.keepWaiting && !hooks.keepWaiting()) {
                abandonado = true;
                break;
            }
            if (agotado()) {
                break;
            }
        }
        if (abandonado) {
            error = "se abandonó la espera de la respuesta";
            failure = TlsFailure::Read;
            break;
        }
        if (responseLine.empty()) {
            error = "el daemon no respondió";
            failure = TlsFailure::Read;
            break;
        }
        ok = true;
    } while (false);

    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    cierra(sock);
    if (ctx) {
        SSL_CTX_free(ctx);
    }
    if (certEsperado) {
        X509_free(certEsperado);
    }
    if (certCliente) {
        X509_free(certCliente);
    }
    if (claveCliente) {
        EVP_PKEY_free(claveCliente);
    }
    if (!ok) {
        responseLine.clear();
    }
    return ok;
}

}  // namespace zfsmgr::base
