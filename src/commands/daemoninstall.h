#pragma once

#include <functional>
#include <string>

#include "connectionprofile.h"
#include "transportsession.h"

// Instalar o actualizar el daemon en una máquina.
//
// **Es la única operación que NO puede pasar por el daemon**, y no es un descuido: es el
// problema del huevo y la gallina. Se entra por SSH y `scp` porque si ya hubiera un daemon
// atendiendo no haría falta instalarlo. Por eso sigue mandando un guion de shell cuando
// todo lo demás dejó de hacerlo — ver docs/plan_shell_rpc.md.
//
// Estaba dentro de `src/cli/shell.cpp`, mezclado con la confirmación por terminal y los
// `fprintf`. No tenía nada de intérprete: son 200 líneas de decidir qué guion toca según
// el sistema de la otra punta. Aquí no se pregunta ni se imprime — quien llama decide cómo
// confirmar y cómo contarlo, que es lo que permite que lo usen el intérprete Y el servidor
// web sin duplicar el guion.
namespace zfsmgr::base::daemoninstall {

// Por qué no se pudo, TIPADO. Un `bool` obligaba a adivinar entre «no hay binario para esa
// plataforma» —que se arregla compilando— y «la otra máquina lo rechazó», que no.
enum class Fallo {
    Ninguno,
    BinarioIlegible,      // la ruta no existe o el fichero está vacío
    NoSePudoSubir,        // el scp o la copia local fallaron
    LaInstalacionFallo,   // el guion corrió y devolvió algo distinto de 0
};

std::string etiquetaDe(Fallo f);

struct Resultado {
    Fallo fallo{Fallo::Ninguno};
    int rc{0};
    std::string detalle;             // lo que dijo la otra punta, tal cual
    std::string version;             // la que se escribió en agent.conf
    bool versionAtrasada{false};     // el agente empaquetado va por detrás de este cliente
    bool esMac{false};               // quien llama decide si avisar de «Acceso total al disco»

    bool ok() const { return fallo == Fallo::Ninguno; }
};

// «linux», «macos», «freebsd» o «windows», con los nombres que espera quien busca el
// binario empaquetado. Sale del `osType` del perfil, no de una consulta.
std::string plataformaDe(const ConnectionProfile& p);

// La arquitectura del OTRO lado, preguntándosela. En Windows no se pregunta: es x86_64 y
// no hay agente para otra cosa. Vacía si la máquina no contesta.
std::string arquitecturaRemota(TransportSession& ses, const ConnectionProfile& p, bool verboso);

// El guion que se manda, según la plataforma. **Público a propósito**: es la pieza que
// decide dónde va el binario, qué gestor de servicios se usa y qué se comprueba después,
// y así se puede examinar en una prueba sin tener delante una máquina de cada sistema.
std::string guionDeInstalacion(const std::string& plataforma, const std::string& version,
                               const std::string& apiVersion);

// Instala o actualiza y arranca. No pregunta nada: la confirmación es de quien llama,
// porque esto reemplaza un binario y reinicia un servicio en la otra punta.
//
// `traza` recibe cada línea que suelta la instalación, para poder enseñarla mientras pasa.
Resultado instala(TransportSession& ses, const ConnectionProfile& perfil,
                  const std::string& rutaBinario,
                  const std::function<void(const std::string&)>& traza = {},
                  bool verboso = false);

}  // namespace zfsmgr::base::daemoninstall
