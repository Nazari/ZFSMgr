#pragma once

#include <string>

// Por qué no se pudo hablar con el daemon, TIPIFICADO.
//
// Antes esto era una frase en castellano, y el código decidía leyéndola:
//
//     if (contains(motivo, "handshake tls daemon-rpc") || contains(motivo, "conexión ..."))
//         tryReviveRemoteDaemonService(p);
//
// Eso ataba tres cosas que no tienen por qué ir juntas: cómo se le cuenta el fallo a una
// persona, en qué idioma, y qué hace el programa a continuación. Cambiar una coma de la
// frase apagaba el reintento; traducirla al inglés lo apagaba entero y **en silencio**,
// porque no hay forma de que eso falle ruidosamente: la función simplemente deja de
// llamarse. Lo mismo valía para comparar contra `rpcTunnelBusyReason()`, donde «ocupado»
// —que NO es un fallo— se habría empezado a tratar como conexión rota, con su castigo de
// 30 s incluido.
//
// Ahora la capa base devuelve QUÉ pasó y quien tiene interfaz decide cómo se dice. Es el
// mismo reparto que ya hacía `store::Motivo` con los avisos del almacén.
//
// Ver docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::transport {

enum class Fallo {
    Ninguno = 0,

    // --- Antes de llegar a intentarlo
    // Ocupado NO es roto: el túnel se está montando en un marco anterior de la pila. Esta
    // llamada se salta el RPC y sale por el camino de siempre, sin castigar a la conexión.
    TunelOcupado,
    FueraDelHiloDeTuneles,
    ArgumentosVacios,
    ConexionNoSsh,
    EnEspera,  // castigo activo. detalle: los segundos que quedan

    // --- El material TLS del daemon remoto
    MaterialNoSeLee,           // detalle: lo que dijo la otra máquina
    MaterialIncompleto,        // llegó respuesta, pero sin las tres piezas
    ClaveClienteNoDisponible,  // ni local ni remota; el daemon solo la entrega una vez
    CertificadosInvalidos,
    ClaveClienteInvalida,

    // --- El túnel SSH
    TunelNoSeMonta,

    // --- La sesión TLS contra el daemon
    // El socket no llegó a abrirse. NO es un fallo de saludo: contarlo como tal apuntaría
    // el diagnóstico a los certificados y dispararía un reaprovisionamiento incapaz de
    // arreglar un problema de transporte.
    ConexionRechazada,      // detalle: el error del socket
    CertificadoNoCoincide,  // fijación: el daemon presenta otro certificado
    EnvioFallido,
    TunelCortadoEnEspera,
    HandshakeFallido,   // detalle: el error de TLS
    RespuestaNoValida,  // detalle: el error del analizador, si lo hay

    // Falló sin dejar dicho por qué. Existe para no tener que distinguir «no falló» de
    // «falló y no lo contó», que es justo donde se colaba un motivo vacío.
    NoEspecificado,
};

// El motivo con lo que lo acompaña. Ver `store::Aviso`: campo con nombre y no una lista de
// argumentos, para que el sitio que lo construye se lea solo.
struct MotivoFallo {
    Fallo fallo{Fallo::Ninguno};
    std::string detalle;

    bool vacio() const { return fallo == Fallo::Ninguno; }
};

// --- Las decisiones que antes se tomaban leyendo la frase.
//
// Las tres se implementan con un `switch` SIN `default`: así, el día que se añada un
// motivo nuevo, el compilador obliga a pasar por aquí y decidir. Con la comparación de
// texto, un motivo nuevo simplemente no casaba con nada y nadie se enteraba.

// ¿Este fallo pinta a daemon caído, y por tanto merece intentar levantarlo antes de
// reintentar? Los de certificado NO: si el material está mal, revivir el servicio no
// arregla nada y encima gasta una conexión SSH.
bool sugiereRevivirDaemon(Fallo f);

// ¿Es cosa del material TLS o del saludo? Lo usa la interfaz para decidir si enseña el
// castigo como «TLS en espera» o se lo calla.
bool esDeTls(Fallo f);

// ¿Merece castigar a la conexión 30 s? Ocupado y «fuera del hilo» no: no dicen nada sobre
// si el daemon está vivo.
bool mereceCastigo(Fallo f);

// Una etiqueta ASCII estable para el REGISTRO. No es texto para leer: es lo que se busca
// con grep en un log que puede venir de una máquina en otro idioma. El texto para personas
// lo pone quien tiene interfaz.
const char* etiquetaDe(Fallo f);

}  // namespace zfsmgr::base::transport
