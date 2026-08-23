#include "storewarnings.h"

namespace zfsmgr::base::store {
namespace {

std::string con(const std::string& base, const std::string& detalle) {
    return detalle.empty() ? base : base + ": " + detalle;
}

}  // namespace

std::string labelOf(const Aviso& a) {
    switch (a.motivo) {
        case Reason::Ninguno:
            return {};
        case Reason::ConfigNoSeAbre:
            return "no se pudo abrir config.json";
        case Reason::ConfigNoValido:
            return con("config.json no es válido", a.detalle);
        case Reason::ConfigDirNoSeCrea:
            return con("no se pudo crear el directorio de configuración", a.detalle);
        case Reason::ConfigNoSeEscribe:
            return con("no se pudo escribir config.json", a.detalle);
        case Reason::TrustNoSeAbre:
            return "no se pudo abrir trust-store.json";
        case Reason::TrustNoValido:
            return con("trust-store.json no es válido", a.detalle);
        case Reason::TrustNoSeEscribe:
            return con("no se pudo escribir trust-store.json", a.detalle);
        case Reason::ClaveMaestraRequerida:
            return "hace falta la contraseña maestra";
        case Reason::ClaveMaestraRequeridaParaCifrar:
            return con("hace falta la contraseña maestra para cifrar", a.campo);
        case Reason::NuevaClaveMaestraVacia:
            return "la contraseña maestra nueva no puede estar vacía";
        case Reason::NoSeCifra:
            return con("no se pudo cifrar «" + a.campo + "»" + (a.conexion.empty() ? "" : " de " + a.conexion),
                       a.detalle);
        case Reason::NoSeDescifra:
            return con("no se pudo descifrar «" + a.campo + "»" + (a.conexion.empty() ? "" : " de " + a.conexion),
                       a.detalle);
        case Reason::CampoIncorrecto:
            return con("campo incorrecto «" + a.campo + "»", a.detalle);
        case Reason::IdVacio:
            return "la conexión no tiene identificador";
        case Reason::NombreRequerido:
            return "hace falta el nombre de la conexión";
        case Reason::HostRequerido:
            return "hace falta el host";
        case Reason::UsuarioRequerido:
            return "hace falta el usuario";
        case Reason::NombreDuplicado:
            return con("ya hay una conexión con ese nombre", a.conexion);
        case Reason::NoSeGuardaConexion:
            return con("no se pudo guardar la conexión", a.detalle);
        case Reason::PerfilPsrpConvertido:
            return con("perfil PSRP convertido a SSH", a.conexion);
    }
    return "error al leer la configuración";
}

}  // namespace zfsmgr::base::store
