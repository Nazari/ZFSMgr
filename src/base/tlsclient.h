#pragma once

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

}  // namespace zfsmgr::base
