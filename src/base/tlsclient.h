#pragma once

#include <functional>
#include <string>

// Cliente TLS con autenticación mutua, sobre OpenSSL y sin Qt.
//
// Es la tercera pieza que ataba el transporte a Qt, y la única que había que escribir:
// el agente ya tenía el lado SERVIDOR con OpenSSL, y la ejecución de procesos también
// existía. Esto es su contraparte.
//
// **LA VALIDACIÓN ES POR FIJACIÓN DE CERTIFICADO, NO POR CA**, y no es un atajo. Es la
// misma decisión que ya tomó la versión con Qt, y su motivo está medido: en macOS,
// SecureTransport nunca validaba la cadena («The root CA certificate is not trusted for
// this purpose») ni siquiera con subjectAltName, extendedKeyUsage, keyUsage y
// basicConstraints correctos. Como el certificado del daemon se trae POR SSH y se guarda,
// comparar contra ESE certificado exacto es más estricto que confiar en una cadena.
//
// La autenticación mutua se mantiene entera: el cliente envía su certificado y el daemon
// lo exige con SSL_VERIFY_PEER.
//
// Ver docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base {

struct TlsClientConfig {
    std::string host;
    unsigned short port{0};
    // El certificado del daemon, en PEM. Es contra ESTE contra el que se compara.
    std::string serverCertPem;
    std::string clientCertPem;
    std::string clientKeyPem;
    int connectTimeoutMs{8000};
    int ioTimeoutMs{30000};
};

// En qué punto falló. Se devuelve APARTE del texto porque quien llama toma decisiones
// distintas según cuál sea, y decidirlas buscando subcadenas en un mensaje es frágil: no
// llegar a conectar y que el saludo TLS falle apuntan a causas opuestas —transporte frente
// a certificados—, y confundirlos lleva a reaprovisionar el TLS para arreglar un túnel.
enum class TlsFailure {
    None,
    BadMaterial,  // el PEM que se nos dio no es válido
    Connect,      // no se llegó a abrir el socket
    Handshake,    // TLS falló
    Pinning,      // el certificado NO es el esperado. Nunca es un fallo pasajero.
    Write,
    Read,
};

// Enganches para quien necesita más que «manda y espera».
struct TlsRequestHooks {
    // Se llama JUSTO ANTES de escribir el primer byte. Es el punto a partir del cual la
    // orden puede haber llegado al otro lado, y por tanto a partir del cual REENVIARLA
    // sería ejecutarla dos veces. Va antes y no después porque una escritura parcial
    // también llega.
    std::function<void()> onBeforeWrite;

    // Se llama mientras se espera la respuesta, cada pocos cientos de milisegundos.
    // **Devolver false ABANDONA la espera.** Es lo que permite salir en cuanto el proceso
    // del túnel muere, en vez de aguardar al plazo entero.
    std::function<bool()> keepWaiting;
};

// Manda una petición y devuelve la respuesta hasta el primer salto de línea, que es el
// protocolo del daemon: una línea JSON de ida, una de vuelta.
//
// Devuelve false y describe el fallo en `error` si no se pudo conectar, si el certificado
// presentado NO es el esperado, o si la conversación se cortó. Ante cualquier duda, false:
// quien llama debe poder distinguir «no se pudo» de «respondió que no».
bool tlsRequestLine(const TlsClientConfig& cfg,
                    const std::string& requestLine,
                    std::string& responseLine,
                    std::string& error);

// La misma, diciendo además en qué punto falló y admitiendo enganches.
bool tlsRequestLine(const TlsClientConfig& cfg,
                    const std::string& requestLine,
                    std::string& responseLine,
                    std::string& error,
                    TlsFailure& failure,
                    const TlsRequestHooks& hooks);

// ¿Es esto un certificado / una clave privada de verdad?
//
// Se comprueba ANTES de montar nada. Descubrirlo dentro del saludo TLS costaría el túnel
// entero —casi un segundo— y, peor, el fallo se leería como un problema de red cuando lo
// que pasa es que el material guardado no sirve.
bool pemCertificateIsValid(const std::string& pem);
bool pemPrivateKeyIsValid(const std::string& pem);

}  // namespace zfsmgr::base
