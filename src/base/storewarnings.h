#pragma once

#include <string>
#include <vector>

// Motivos tipificados del almacén de conexiones.
//
// La capa base NO traduce: devuelve un motivo y los datos que lo acompañan, y quien
// tiene interfaz decide cómo se dice. Es el mismo reparto que ya hacía
// `connectioncapabilities`, y es lo que permite sacar `ConnectionStore` de Qt sin
// arrastrar con él el sistema de traducción.
//
// Ver docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::store {

enum class Motivo {
    Ninguno = 0,

    // --- Ficheros
    ConfigNoSeAbre,
    ConfigNoValido,        // detalle: el error del analizador
    ConfigDirNoSeCrea,
    ConfigNoSeEscribe,
    TrustNoSeAbre,
    TrustNoValido,         // detalle: el error del analizador
    TrustNoSeEscribe,

    // --- Clave maestra y cifrado
    ClaveMaestraRequerida,
    ClaveMaestraRequeridaParaCifrar,  // campo
    NuevaClaveMaestraVacia,
    NoSeCifra,                        // campo, detalle
    // Un campo cifrado que no se pudo abrir. OJO: cuando esto ocurre el campo CONSERVA
    // el texto cifrado, así que quien lo reciba no debe usarlo como si fuera el valor en
    // claro. Ver la nota del diseño sobre pam_faillock.
    NoSeDescifra,                     // conexion, campo, detalle
    CampoIncorrecto,                  // conexion, campo

    // --- Validación
    IdVacio,
    NombreRequerido,
    HostRequerido,
    UsuarioRequerido,
    NombreDuplicado,
    NoSeGuardaConexion,

    // --- Avisos informativos
    PerfilPsrpConvertido,             // conexion
};

// El motivo con sus datos. Campos con nombre, no una lista de argumentos: así el sitio
// que lo construye se lee solo y quien traduce no puede intercambiarlos de orden.
struct Aviso {
    Motivo motivo{Motivo::Ninguno};
    std::string conexion;  // nombre de la conexión, o su id si no tiene nombre
    std::string campo;     // el campo afectado, cuando el motivo distingue uno
    std::string detalle;   // el error subyacente, cuando lo hay

    bool vacio() const { return motivo == Motivo::Ninguno; }
};

using Avisos = std::vector<Aviso>;

}  // namespace zfsmgr::base::store
