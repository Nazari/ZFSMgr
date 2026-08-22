#pragma once

#include <QString>

class SecretCipher {
public:
    static bool isEncrypted(const QString& value);
    static bool encryptEncv1(const QString& plaintext, const QString& masterPassword, QString& output, QString& error);
    static bool decryptEncv1(const QString& input, const QString& masterPassword, QString& output, QString& error);
};
