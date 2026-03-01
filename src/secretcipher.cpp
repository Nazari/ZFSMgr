#include "secretcipher.h"

#include <QByteArray>
#include <QDateTime>
#include <QStringList>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace {

QByteArray fromBase64Url(QString value) {
    value.replace('-', '+');
    value.replace('_', '/');
    while ((value.size() % 4) != 0) {
        value.append('=');
    }
    return QByteArray::fromBase64(value.toUtf8());
}

QString toBase64Url(const QByteArray& data) {
    QByteArray b64 = data.toBase64();
    b64.replace('+', '-');
    b64.replace('/', '_');
    while (b64.endsWith('=')) {
        b64.chop(1);
    }
    return QString::fromUtf8(b64);
}

bool constantTimeEq(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

} // namespace

bool SecretCipher::isEncrypted(const QString& value) {
    return value.startsWith(QStringLiteral("encv1$"));
}

bool SecretCipher::encryptEncv1(const QString& plaintext, const QString& masterPassword, QString& output, QString& error) {
    output.clear();
    error.clear();
    if (masterPassword.isEmpty()) {
        error = QStringLiteral("Password maestro vacío");
        return false;
    }

    QByteArray salt(16, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(salt.data()), salt.size()) != 1) {
        error = QStringLiteral("No se pudo generar salt");
        return false;
    }

    QByteArray key(32, '\0');
    const QByteArray passUtf8 = masterPassword.toUtf8();
    const int pbkdf2Ok = PKCS5_PBKDF2_HMAC(
        passUtf8.constData(),
        passUtf8.size(),
        reinterpret_cast<const unsigned char*>(salt.constData()),
        salt.size(),
        390000,
        EVP_sha256(),
        key.size(),
        reinterpret_cast<unsigned char*>(key.data()));
    if (pbkdf2Ok != 1) {
        error = QStringLiteral("PBKDF2 falló");
        return false;
    }

    const QByteArray signingKey = key.left(16);
    const QByteArray encKey = key.mid(16, 16);

    QByteArray iv(16, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(iv.data()), iv.size()) != 1) {
        error = QStringLiteral("No se pudo generar IV");
        return false;
    }

    const QByteArray plain = plaintext.toUtf8();
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        error = QStringLiteral("No se pudo crear contexto AES");
        return false;
    }

    QByteArray ciphertext(plain.size() + 32, '\0');
    int outLen1 = 0;
    int outLen2 = 0;
    bool ok = EVP_EncryptInit_ex(
                  ctx,
                  EVP_aes_128_cbc(),
                  nullptr,
                  reinterpret_cast<const unsigned char*>(encKey.constData()),
                  reinterpret_cast<const unsigned char*>(iv.constData()))
              == 1;
    if (ok) {
        ok = EVP_EncryptUpdate(
                 ctx,
                 reinterpret_cast<unsigned char*>(ciphertext.data()),
                 &outLen1,
                 reinterpret_cast<const unsigned char*>(plain.constData()),
                 plain.size())
             == 1;
    }
    if (ok) {
        ok = EVP_EncryptFinal_ex(
                 ctx,
                 reinterpret_cast<unsigned char*>(ciphertext.data()) + outLen1,
                 &outLen2)
             == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        error = QStringLiteral("No se pudo cifrar token");
        return false;
    }
    ciphertext.resize(outLen1 + outLen2);

    QByteArray msg;
    msg.reserve(1 + 8 + iv.size() + ciphertext.size());
    msg.append(char(0x80));

    quint64 ts = static_cast<quint64>(QDateTime::currentSecsSinceEpoch());
    QByteArray tsBytes(8, '\0');
    for (int i = 7; i >= 0; --i) {
        tsBytes[i] = static_cast<char>(ts & 0xFFu);
        ts >>= 8;
    }
    msg.append(tsBytes);
    msg.append(iv);
    msg.append(ciphertext);

    unsigned int hlen = 0;
    unsigned char hbuf[EVP_MAX_MD_SIZE] = {0};
    HMAC(
        EVP_sha256(),
        signingKey.constData(),
        signingKey.size(),
        reinterpret_cast<const unsigned char*>(msg.constData()),
        msg.size(),
        hbuf,
        &hlen);
    const QByteArray sig(reinterpret_cast<const char*>(hbuf), static_cast<int>(hlen));
    msg.append(sig);

    output = QStringLiteral("encv1$%1$%2")
                 .arg(toBase64Url(salt), toBase64Url(msg));
    return true;
}

bool SecretCipher::decryptEncv1(const QString& input, const QString& masterPassword, QString& output, QString& error) {
    output.clear();
    error.clear();

    const QStringList parts = input.split('$');
    if (parts.size() != 3 || parts[0] != QStringLiteral("encv1")) {
        error = QStringLiteral("Formato encv1 inválido");
        return false;
    }

    const QByteArray salt = fromBase64Url(parts[1]);
    const QByteArray token = fromBase64Url(parts[2]);
    if (salt.isEmpty() || token.size() < (1 + 8 + 16 + 16 + 32)) {
        error = QStringLiteral("Salt/token inválidos");
        return false;
    }

    QByteArray key(32, '\0');
    const int pbkdf2Ok = PKCS5_PBKDF2_HMAC(
        masterPassword.toUtf8().constData(),
        masterPassword.toUtf8().size(),
        reinterpret_cast<const unsigned char*>(salt.constData()),
        salt.size(),
        390000,
        EVP_sha256(),
        key.size(),
        reinterpret_cast<unsigned char*>(key.data()));
    if (pbkdf2Ok != 1) {
        error = QStringLiteral("PBKDF2 falló");
        return false;
    }

    const QByteArray signingKey = key.left(16);
    const QByteArray encKey = key.mid(16, 16);
    const QByteArray msg = token.left(token.size() - 32);
    const QByteArray sig = token.right(32);

    unsigned int hlen = 0;
    unsigned char hbuf[EVP_MAX_MD_SIZE] = {0};
    HMAC(
        EVP_sha256(),
        signingKey.constData(),
        signingKey.size(),
        reinterpret_cast<const unsigned char*>(msg.constData()),
        msg.size(),
        hbuf,
        &hlen);
    const QByteArray calcSig(reinterpret_cast<const char*>(hbuf), static_cast<int>(hlen));
    if (!constantTimeEq(sig, calcSig)) {
        error = QStringLiteral("Firma inválida (password maestro incorrecto)");
        return false;
    }

    const unsigned char version = static_cast<unsigned char>(token[0]);
    if (version != 0x80) {
        error = QStringLiteral("Versión Fernet no soportada");
        return false;
    }

    const QByteArray iv = token.mid(9, 16);
    const QByteArray ciphertext = token.mid(25, token.size() - 25 - 32);
    if (ciphertext.isEmpty() || (ciphertext.size() % 16) != 0) {
        error = QStringLiteral("Ciphertext inválido");
        return false;
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        error = QStringLiteral("No se pudo crear contexto AES");
        return false;
    }

    QByteArray plain(ciphertext.size() + 16, '\0');
    int outLen1 = 0;
    int outLen2 = 0;
    bool ok = EVP_DecryptInit_ex(
                  ctx,
                  EVP_aes_128_cbc(),
                  nullptr,
                  reinterpret_cast<const unsigned char*>(encKey.constData()),
                  reinterpret_cast<const unsigned char*>(iv.constData()))
              == 1;
    if (ok) {
        ok = EVP_DecryptUpdate(
                 ctx,
                 reinterpret_cast<unsigned char*>(plain.data()),
                 &outLen1,
                 reinterpret_cast<const unsigned char*>(ciphertext.constData()),
                 ciphertext.size())
             == 1;
    }
    if (ok) {
        ok = EVP_DecryptFinal_ex(
                 ctx,
                 reinterpret_cast<unsigned char*>(plain.data()) + outLen1,
                 &outLen2)
             == 1;
    }
    EVP_CIPHER_CTX_free(ctx);

    if (!ok) {
        error = QStringLiteral("No se pudo descifrar token");
        return false;
    }

    plain.resize(outLen1 + outLen2);
    output = QString::fromUtf8(plain);
    return true;
}
