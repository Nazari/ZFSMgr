#include "storewarnings.h"

#include "i18n.h"
#include "strutil.h"

namespace zfsmgr::base::store {
namespace {

std::string con(const std::string& base, const std::string& detalle) {
    return detalle.empty() ? base : base + ": " + detalle;
}

// Los textos van por el catálogo, como el resto de la capa base. Antes salían siempre en
// castellano, también en una sesión en inglés.
std::string t(const char* clave, const char* es) {
    return i18n::tr(clave, es);
}

}  // namespace

std::string labelOf(const Warning& a) {
    switch (a.reason) {
        case Reason::None_:
            return {};
        case Reason::ConfigCannotBeOpened:
            return t("t_sw_config_no_abre", "no se pudo abrir config.json");
        case Reason::TrustCannotBeOpened:
            return t("t_sw_trust_no_abre", "no se pudo abrir trust-store.json");
        case Reason::MasterPasswordRequired:
            return t("t_sw_maestra", "hace falta la contraseña maestra");
        case Reason::NewMasterPasswordEmpty:
            return t("t_sw_maestra_vacia", "la contraseña maestra nueva no puede estar vacía");
        case Reason::MasterChangedElsewhere:
            return t("t_sw_maestra_otra", "la contraseña maestra de esta sesión ya no abre la configuración: se cambió en otra (otro zfsmgr-cli o la interfaz). No se ha escrito nada; salga y vuelva a entrar con la nueva");
        case Reason::EmptyId:
            return t("t_sw_sin_id", "la conexión no tiene identificador");
        case Reason::NameRequired:
            return t("t_sw_nombre", "hace falta el nombre de la conexión");
        case Reason::HostRequired:
            return t("t_sw_host", "hace falta el host");
        case Reason::UserRequired:
            return t("t_sw_usuario", "hace falta el usuario");
        case Reason::ConfigNotValid:
            return con(t("t_sw_config_no_valido", "config.json no es válido"), a.detail);
        case Reason::ConfigDirCannotBeCreated:
            return con(t("t_sw_dir_no_crea", "no se pudo crear el directorio de configuración"), a.detail);
        case Reason::ConfigCannotBeWritten:
            return con(t("t_sw_config_no_escribe", "no se pudo escribir config.json"), a.detail);
        case Reason::TrustNotValid:
            return con(t("t_sw_trust_no_valido", "trust-store.json no es válido"), a.detail);
        case Reason::TrustCannotBeWritten:
            return con(t("t_sw_trust_no_escribe", "no se pudo escribir trust-store.json"), a.detail);
        case Reason::MasterPasswordRequiredToEncrypt:
            return con(t("t_sw_maestra_cifrar", "hace falta la contraseña maestra para cifrar"), a.field);
        case Reason::DuplicateName:
            return con(t("t_sw_duplicado", "ya hay una conexión con ese nombre"), a.connection);
        case Reason::ConnectionNotSaved:
            return con(t("t_sw_no_guardada", "no se pudo guardar la conexión"), a.detail);
        case Reason::PsrpProfileConverted:
            return con(t("t_sw_psrp", "perfil PSRP convertido a SSH"), a.connection);
        case Reason::WrongField:
            return con(format(t("t_sw_campo", "campo incorrecto «%1»"), {a.field}), a.detail);
        case Reason::CannotEncrypt:
            return con(format(t("t_sw_no_cifra", "no se pudo cifrar «%1»"), {a.field})
                           + (a.connection.empty() ? std::string()
                                                   : format(t("t_sw_de_conexion", " de %1"), {a.connection})),
                       a.detail);
        case Reason::CannotDecrypt:
            return con(format(t("t_sw_no_descifra", "no se pudo descifrar «%1»"), {a.field})
                           + (a.connection.empty() ? std::string()
                                                   : format(t("t_sw_de_conexion", " de %1"), {a.connection})),
                       a.detail);
    }
    return t("t_sw_error_leer", "error al leer la configuración");
}

}  // namespace zfsmgr::base::store
