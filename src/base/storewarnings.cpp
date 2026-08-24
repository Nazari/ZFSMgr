#include "storewarnings.h"

namespace zfsmgr::base::store {
namespace {

std::string con(const std::string& base, const std::string& detalle) {
    return detalle.empty() ? base : base + ": " + detalle;
}

}  // namespace

std::string labelOf(const Warning& a) {
    switch (a.reason) {
        case Reason::None_:
            return {};
        case Reason::ConfigCannotBeOpened:
            return "no se pudo abrir config.json";
        case Reason::ConfigNotValid:
            return con("config.json no es válido", a.detail);
        case Reason::ConfigDirCannotBeCreated:
            return con("no se pudo crear el directorio de configuración", a.detail);
        case Reason::ConfigCannotBeWritten:
            return con("no se pudo escribir config.json", a.detail);
        case Reason::TrustCannotBeOpened:
            return "no se pudo abrir trust-store.json";
        case Reason::TrustNotValid:
            return con("trust-store.json no es válido", a.detail);
        case Reason::TrustCannotBeWritten:
            return con("no se pudo escribir trust-store.json", a.detail);
        case Reason::MasterPasswordRequired:
            return "hace falta la contraseña maestra";
        case Reason::MasterPasswordRequiredToEncrypt:
            return con("hace falta la contraseña maestra para cifrar", a.field);
        case Reason::NewMasterPasswordEmpty:
            return "la contraseña maestra nueva no puede estar vacía";
        case Reason::CannotEncrypt:
            return con("no se pudo cifrar «" + a.field + "»" + (a.connection.empty() ? "" : " de " + a.connection),
                       a.detail);
        case Reason::CannotDecrypt:
            return con("no se pudo descifrar «" + a.field + "»" + (a.connection.empty() ? "" : " de " + a.connection),
                       a.detail);
        case Reason::WrongField:
            return con("campo incorrecto «" + a.field + "»", a.detail);
        case Reason::EmptyId:
            return "la conexión no tiene identificador";
        case Reason::NameRequired:
            return "hace falta el nombre de la conexión";
        case Reason::HostRequired:
            return "hace falta el host";
        case Reason::UserRequired:
            return "hace falta el usuario";
        case Reason::DuplicateName:
            return con("ya hay una conexión con ese nombre", a.connection);
        case Reason::ConnectionNotSaved:
            return con("no se pudo guardar la conexión", a.detail);
        case Reason::PsrpProfileConverted:
            return con("perfil PSRP convertido a SSH", a.connection);
    }
    return "error al leer la configuración";
}

}  // namespace zfsmgr::base::store
