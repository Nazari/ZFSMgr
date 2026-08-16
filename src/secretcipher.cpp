#include "secretcipher.h"

#include "base/secretcipher.h"

// Adaptador. La lógica está en `src/base/secretcipher.cpp`, que no depende de Qt; aquí
// solo se convierte en la frontera.
//
// El formato `encv1$…` no cambia: está comprobado que lo que cifra una implementación lo
// descifra la otra, en los dos sentidos. Ver docs/diseno_tecnico_capa_base_sin_qt.md.
namespace B = zfsmgr::base;

namespace {
inline QString q(const std::string& s) { return QString::fromStdString(s); }
inline std::string b(const QString& s) { return s.toStdString(); }
}  // namespace

bool SecretCipher::isEncrypted(const QString& value) {
    return B::SecretCipher::isEncrypted(b(value));
}

bool SecretCipher::encryptEncv1(const QString& plaintext, const QString& masterPassword,
                                QString& output, QString& error) {
    std::string o;
    std::string e;
    const bool ok = B::SecretCipher::encryptEncv1(b(plaintext), b(masterPassword), o, e);
    output = q(o);
    error = q(e);
    return ok;
}

bool SecretCipher::decryptEncv1(const QString& input, const QString& masterPassword,
                                QString& output, QString& error) {
    std::string o;
    std::string e;
    const bool ok = B::SecretCipher::decryptEncv1(b(input), b(masterPassword), o, e);
    output = q(o);
    error = q(e);
    return ok;
}
