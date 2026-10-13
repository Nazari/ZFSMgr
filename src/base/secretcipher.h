#pragma once

#include <string>
#include <vector>

// Encryption of the configuration's sensitive fields, without Qt.
//
// The format is `encv1$<salt>$<token>`, both halves in base64url without padding. The token
// is **Fernet**: version byte 0x80, an 8-byte big-endian timestamp, a 16-byte IV, ciphertext
// under AES-128-CBC and a 32-byte HMAC-SHA256 at the end. The key comes from
// PBKDF2-HMAC-SHA256 with 390,000 iterations over a 16-byte salt that is PER VALUE, and the
// resulting 32 bytes are split into signing (16) and encryption (16), as the Fernet
// specification requires.
//
// **The format is not to be touched.** There are files already written this way on users'
// machines; any change would require a migration.
//
// On the cost: 390,000 iterations are about 40 ms per derivation, and with a per-value salt
// that is paid on EVERY encrypted field —up to five per connection—. It is left this way on
// purpose (a single per-file salt would be less conservative and force a migration); what
// keeps start-up fast is opening each value once per process and all of them in parallel
// (openInParallel).
//
// See docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base {

class SecretCipher {
public:
    static bool isEncrypted(const std::string& value);

    // An empty `masterPassword` is an error: encrypting with an empty key would give a
    // false sense of protection.
    static bool encryptEncv1(const std::string& plaintext,
                             const std::string& masterPassword,
                             std::string& output,
                             std::string& error);

    // It verifies the HMAC BEFORE looking at the version and before decrypting, and it
    // compares the signatures in constant time. On failure, `output` is left empty: the
    // caller must NOT use the encrypted input as though it were the plaintext value.
    static bool decryptEncv1(const std::string& input,
                             const std::string& masterPassword,
                             std::string& output,
                             std::string& error);

    // Opens all of `inputs` at once, one derivation per core, and keeps them so the
    // decryptEncv1() calls that follow find them done. Failures are left for those calls to
    // report: this only saves time.
    static void openInParallel(const std::vector<std::string>& inputs,
                               const std::string& masterPassword);
};

}  // namespace zfsmgr::base
