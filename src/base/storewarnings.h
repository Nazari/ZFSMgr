#pragma once

#include <string>

#include <string>
#include <vector>

// Typed reasons from the connection store.
//
// The base layer does NOT translate: it returns a reason and the data that goes with it, and
// whoever has an interface decides how it is worded. It is the same split
// `connectioncapabilities` already made, and it is what lets `ConnectionStore` be pulled out
// of Qt without dragging the translation system along with it.
//
// See docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::store {

enum class Reason {
    None_ = 0,

    // --- Files
    ConfigCannotBeOpened,
    ConfigNotValid,                  // detail: the parser's error
    ConfigDirCannotBeCreated,
    ConfigCannotBeWritten,
    TrustCannotBeOpened,
    TrustNotValid,                   // detail: the parser's error
    TrustCannotBeWritten,

    // --- Master password and encryption
    MasterPasswordRequired,
    MasterPasswordRequiredToEncrypt,  // field
    NewMasterPasswordEmpty,
    CannotEncrypt,                   // field, detail
    // An encrypted field that could not be opened. CAREFUL: when this happens the field
    // KEEPS the ciphertext, so whoever receives it must not use it as though it were the
    // plaintext value. See the design note on pam_faillock.
    CannotDecrypt,                   // connection, field, detail
    WrongField,                      // connection, field

    // --- Validation
    EmptyId,
    NameRequired,
    HostRequired,
    UserRequired,
    DuplicateName,
    ConnectionNotSaved,

    // --- Informational warnings
    PsrpProfileConverted,            // connection
};

// The reason with its data. Named fields, not a list of arguments: that way the place that
// builds it reads on its own, and whoever translates cannot swap their order.
struct Warning {
    Reason reason{Reason::None_};
    std::string connection;  // the connection's name, or its id when it has no name
    std::string field;       // the affected field, when the reason singles one out
    std::string detail;      // the underlying error, when there is one

    bool empty() const { return reason == Reason::None_; }
};

using Warnings = std::vector<Warning>;

// The fallback Spanish wording of a warning, with its data already filled in, for whoever
// has no catalogue of their own. The interface and the shell have theirs and word it their
// own way; this keeps a new reason from coming out as a number or, worse, silently.
std::string labelOf(const Warning& w);

}  // namespace zfsmgr::base::store
