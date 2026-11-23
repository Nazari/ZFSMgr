#pragma once

// FIDO2 security keys (a YubiKey, a Nitrokey… any with hmac-secret) for opening ZFSMgr: the only code that talks to libfido2.
//
// What is asked of a key is its hmac-secret: for a credential registered here and a salt, the
// key gives 32 bytes, always the same, and only when touched. Those 32 bytes wrap the data key
// in a slot of config.json (store::addKeySlot); the file never holds them.
//
// Every call that waits for a touch can be cut short from another thread with cancel().

#include <string>
#include <vector>

namespace zfsmgr::cli::fido {

// The relying party every credential of ZFSMgr belongs to.
inline constexpr const char* kRelyingParty = "zfsmgr";

struct Device {
    std::string path;
    std::string product;  // «YubiKey OTP+FIDO+CCID», «Windows Hello»…
    bool windowsHello = false;
};

// The keys connected now. On Windows there is one entry, Windows Hello, which stands for any
// key: without being administrator Windows does not let a program talk to them directly.
std::vector<Device> devices();

enum class Result {
    Ok,
    NoDevice,     // no key connected
    NotTouched,   // it waited and nobody touched it
    PinRequired,  // the key wants its PIN: ask for it and call again with it
    PinInvalid,
    PinBlocked,
    NotRegistered,  // none of the credentials is on this key
    Unsupported,    // the key has no hmac-secret
    Cancelled,
    Failed,  // anything else: `detail` says what
};

// Registers a new credential with hmac-secret and asks for its secret straight away: two
// touches. `resident` comes out true when it had to be stored in the key (Windows Hello
// ignores hmac-secret otherwise).
Result registerKey(const std::string& salt, const std::string& pin, std::string& credentialId,
                   bool& resident, std::string& secret, std::string& product, std::string& detail);

// Asks a key for the secret of any of these credentials: one touch. `used` says which.
Result secret(const std::vector<std::string>& credentialIds, const std::string& salt,
              const std::string& pin, std::string& used, std::string& secret, std::string& detail);

// Cuts short whatever registerKey or secret is waiting for, from another thread.
void cancel();

}  // namespace zfsmgr::cli::fido
