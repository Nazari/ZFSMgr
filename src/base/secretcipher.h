#pragma once

#include <string>

// Cifrado de los campos sensibles de la configuración, sin Qt.
//
// El formato es `encv1$<sal>$<token>`, con las dos partes en base64url sin relleno. El
// token es **Fernet**: byte de versión 0x80, marca de tiempo de 8 bytes big-endian, IV
// de 16, texto cifrado con AES-128-CBC y HMAC-SHA256 de 32 al final. La clave sale de
// PBKDF2-HMAC-SHA256 con 390.000 iteraciones sobre una sal de 16 bytes PROPIA DE CADA
// VALOR, y los 32 bytes resultantes se parten en firma (16) y cifrado (16), como manda
// la especificación de Fernet.
//
// **El formato no se toca.** Hay ficheros ya escritos así en las máquinas de los
// usuarios; cualquier cambio exigiría una migración.
//
// Sobre el coste: 390.000 iteraciones son unos 40 ms por derivación, y con sal por valor
// eso se paga en CADA campo cifrado —hasta cinco por conexión— tanto al cargar como al
// guardar. Con un puñado de conexiones no se nota; se deja así a propósito porque una
// sal única por fichero sería más rápida pero menos conservadora, y cambiarla obligaría
// a migrar.
//
// Ver docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base {

class SecretCipher {
public:
    static bool isEncrypted(const std::string& value);

    // `masterPassword` vacía es un error: cifrar con clave vacía daría una falsa
    // sensación de protección.
    static bool encryptEncv1(const std::string& plaintext,
                             const std::string& masterPassword,
                             std::string& output,
                             std::string& error);

    // Verifica el HMAC ANTES de mirar la versión y de descifrar, y compara las firmas en
    // tiempo constante. Si falla, `output` queda vacío: quien llame NO debe usar la
    // entrada cifrada como si fuera el valor en claro.
    static bool decryptEncv1(const std::string& input,
                             const std::string& masterPassword,
                             std::string& output,
                             std::string& error);
};

}  // namespace zfsmgr::base
