#include "secretcipher.h"

#include "strutil.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <cstdint>
#include <ctime>
#include <vector>

namespace zfsmgr::base {
namespace {

constexpr int kIteraciones = 390000;
constexpr std::size_t kSalLen = 16;
constexpr std::size_t kClaveLen = 32;   // 16 de firma + 16 de cifrado
constexpr std::size_t kIvLen = 16;
constexpr std::size_t kFirmaLen = 32;
constexpr unsigned char kVersionFernet = 0x80;

// Borra el contenido antes de soltar la memoria. No lo hacía la versión con QByteArray,
// y aquí se aprovecha el puerto: son claves derivadas y textos en claro.
struct Zona {
    std::string s;
    explicit Zona(std::size_t n = 0) : s(n, '\0') {}
    ~Zona() {
        if (!s.empty()) {
            OPENSSL_cleanse(s.data(), s.size());
        }
    }
    unsigned char* u() { return reinterpret_cast<unsigned char*>(s.data()); }
    const unsigned char* cu() const { return reinterpret_cast<const unsigned char*>(s.data()); }
};

std::string aBase64Url(const std::string& datos) {
    std::string b64 = base64Encode(datos);
    for (char& c : b64) {
        if (c == '+') { c = '-'; }
        else if (c == '/') { c = '_'; }
    }
    while (!b64.empty() && b64.back() == '=') {
        b64.pop_back();
    }
    return b64;
}

bool deBase64Url(std::string valor, std::string& out) {
    for (char& c : valor) {
        if (c == '-') { c = '+'; }
        else if (c == '_') { c = '/'; }
    }
    while ((valor.size() % 4) != 0) {
        valor.push_back('=');
    }
    return base64Decode(valor, out);
}

bool derivaClave(const std::string& masterPassword, const std::string& sal, Zona& clave) {
    return PKCS5_PBKDF2_HMAC(masterPassword.data(),
                             static_cast<int>(masterPassword.size()),
                             reinterpret_cast<const unsigned char*>(sal.data()),
                             static_cast<int>(sal.size()),
                             kIteraciones,
                             EVP_sha256(),
                             static_cast<int>(kClaveLen),
                             clave.u())
        == 1;
}

std::string hmacSha256(const std::string& clave, const std::string& datos) {
    unsigned char buf[EVP_MAX_MD_SIZE] = {0};
    unsigned int len = 0;
    HMAC(EVP_sha256(),
         clave.data(),
         static_cast<int>(clave.size()),
         reinterpret_cast<const unsigned char*>(datos.data()),
         datos.size(),
         buf,
         &len);
    return std::string(reinterpret_cast<const char*>(buf), len);
}

bool igualTiempoConstante(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char dif = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        dif |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return dif == 0;
}

}  // namespace

bool SecretCipher::isEncrypted(const std::string& value) {
    return startsWith(value, "encv1$");
}

bool SecretCipher::encryptEncv1(const std::string& plaintext,
                                const std::string& masterPassword,
                                std::string& output,
                                std::string& error) {
    output.clear();
    error.clear();
    if (masterPassword.empty()) {
        error = "Password maestro vacío";
        return false;
    }

    std::string sal(kSalLen, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(sal.data()), static_cast<int>(sal.size())) != 1) {
        error = "No se pudo generar salt";
        return false;
    }

    Zona clave(kClaveLen);
    if (!derivaClave(masterPassword, sal, clave)) {
        error = "PBKDF2 falló";
        return false;
    }
    const std::string claveFirma = clave.s.substr(0, 16);
    const std::string claveCifrado = clave.s.substr(16, 16);

    std::string iv(kIvLen, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(iv.data()), static_cast<int>(iv.size())) != 1) {
        error = "No se pudo generar IV";
        return false;
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        error = "No se pudo crear contexto AES";
        return false;
    }
    std::string cifrado(plaintext.size() + 32, '\0');
    int n1 = 0;
    int n2 = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
                                 reinterpret_cast<const unsigned char*>(claveCifrado.data()),
                                 reinterpret_cast<const unsigned char*>(iv.data()))
           == 1;
    if (ok) {
        ok = EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(cifrado.data()), &n1,
                               reinterpret_cast<const unsigned char*>(plaintext.data()),
                               static_cast<int>(plaintext.size()))
          == 1;
    }
    if (ok) {
        ok = EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(cifrado.data()) + n1, &n2) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        error = "No se pudo cifrar token";
        return false;
    }
    cifrado.resize(static_cast<std::size_t>(n1 + n2));

    std::string msg;
    msg.reserve(1 + 8 + iv.size() + cifrado.size());
    msg.push_back(static_cast<char>(kVersionFernet));
    std::uint64_t ts = static_cast<std::uint64_t>(std::time(nullptr));
    char tsBytes[8];
    for (int i = 7; i >= 0; --i) {
        tsBytes[i] = static_cast<char>(ts & 0xFFu);
        ts >>= 8;
    }
    msg.append(tsBytes, 8);
    msg += iv;
    msg += cifrado;
    msg += hmacSha256(claveFirma, msg);

    output = "encv1$" + aBase64Url(sal) + "$" + aBase64Url(msg);
    return true;
}

bool SecretCipher::decryptEncv1(const std::string& input,
                                const std::string& masterPassword,
                                std::string& output,
                                std::string& error) {
    output.clear();
    error.clear();

    const std::vector<std::string> partes = split(input, "$", false);
    if (partes.size() != 3 || partes[0] != "encv1") {
        error = "Formato encv1 inválido";
        return false;
    }

    std::string sal;
    std::string token;
    if (!deBase64Url(partes[1], sal) || !deBase64Url(partes[2], token)) {
        error = "Salt/token inválidos";
        return false;
    }
    if (sal.empty() || token.size() < (1 + 8 + kIvLen + 16 + kFirmaLen)) {
        error = "Salt/token inválidos";
        return false;
    }

    Zona clave(kClaveLen);
    if (!derivaClave(masterPassword, sal, clave)) {
        error = "PBKDF2 falló";
        return false;
    }
    const std::string claveFirma = clave.s.substr(0, 16);
    const std::string claveCifrado = clave.s.substr(16, 16);

    // Primero se AUTENTICA y luego se interpreta: mirar la versión o el IV antes de
    // comprobar la firma sería analizar datos que todavía no se sabe si son nuestros.
    const std::string msg = token.substr(0, token.size() - kFirmaLen);
    const std::string firma = token.substr(token.size() - kFirmaLen);
    if (!igualTiempoConstante(firma, hmacSha256(claveFirma, msg))) {
        error = "Firma inválida (password maestro incorrecto)";
        return false;
    }

    if (static_cast<unsigned char>(token[0]) != kVersionFernet) {
        error = "Versión Fernet no soportada";
        return false;
    }

    const std::string iv = token.substr(9, kIvLen);
    const std::string cifrado = token.substr(25, token.size() - 25 - kFirmaLen);
    if (cifrado.empty() || (cifrado.size() % 16) != 0) {
        error = "Ciphertext inválido";
        return false;
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        error = "No se pudo crear contexto AES";
        return false;
    }
    Zona plano(cifrado.size() + 16);
    int n1 = 0;
    int n2 = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
                                 reinterpret_cast<const unsigned char*>(claveCifrado.data()),
                                 reinterpret_cast<const unsigned char*>(iv.data()))
           == 1;
    if (ok) {
        ok = EVP_DecryptUpdate(ctx, plano.u(), &n1,
                               reinterpret_cast<const unsigned char*>(cifrado.data()),
                               static_cast<int>(cifrado.size()))
          == 1;
    }
    if (ok) {
        ok = EVP_DecryptFinal_ex(ctx, plano.u() + n1, &n2) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        error = "No se pudo descifrar token";
        return false;
    }

    output.assign(plano.s.data(), static_cast<std::size_t>(n1 + n2));
    return true;
}

}  // namespace zfsmgr::base
