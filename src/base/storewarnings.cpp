#include "storewarnings.h"

namespace zfsmgr::base::store {
namespace {

std::string con(const std::string& base, const std::string& detalle) {
    return detalle.empty() ? base : base + ": " + detalle;
}

}  // namespace

std::string etiquetaDe(const Aviso& a) {
    switch (a.motivo) {
        case Motivo::Ninguno:
            return {};
        case Motivo::ConfigNoSeAbre:
            return "no se pudo abrir config.json";
        case Motivo::ConfigNoValido:
            return con("config.json no es válido", a.detalle);
        case Motivo::ConfigDirNoSeCrea:
            return con("no se pudo crear el directorio de configuración", a.detalle);
        case Motivo::ConfigNoSeEscribe:
            return con("no se pudo escribir config.json", a.detalle);
        case Motivo::TrustNoSeAbre:
            return "no se pudo abrir trust-store.json";
        case Motivo::TrustNoValido:
            return con("trust-store.json no es válido", a.detalle);
        case Motivo::TrustNoSeEscribe:
            return con("no se pudo escribir trust-store.json", a.detalle);
        case Motivo::ClaveMaestraRequerida:
            return "hace falta la contraseña maestra";
        case Motivo::ClaveMaestraRequeridaParaCifrar:
            return con("hace falta la contraseña maestra para cifrar", a.campo);
        case Motivo::NuevaClaveMaestraVacia:
            return "la contraseña maestra nueva no puede estar vacía";
        case Motivo::NoSeCifra:
            return con("no se pudo cifrar «" + a.campo + "»" + (a.conexion.empty() ? "" : " de " + a.conexion),
                       a.detalle);
        case Motivo::NoSeDescifra:
            return con("no se pudo descifrar «" + a.campo + "»" + (a.conexion.empty() ? "" : " de " + a.conexion),
                       a.detalle);
        case Motivo::CampoIncorrecto:
            return con("campo incorrecto «" + a.campo + "»", a.detalle);
        case Motivo::IdVacio:
            return "la conexión no tiene identificador";
        case Motivo::NombreRequerido:
            return "hace falta el nombre de la conexión";
        case Motivo::HostRequerido:
            return "hace falta el host";
        case Motivo::UsuarioRequerido:
            return "hace falta el usuario";
        case Motivo::NombreDuplicado:
            return con("ya hay una conexión con ese nombre", a.conexion);
        case Motivo::NoSeGuardaConexion:
            return con("no se pudo guardar la conexión", a.detalle);
        case Motivo::PerfilPsrpConvertido:
            return con("perfil PSRP convertido a SSH", a.conexion);
    }
    return "error al leer la configuración";
}

}  // namespace zfsmgr::base::store
