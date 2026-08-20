#pragma once

#include <string>
#include <vector>

#include "connectionprofile.h"
#include "transportsession.h"

// Mover DATOS entre dos extremos: por dónde van los bytes y desde dónde se reanuda.
//
// Aquí NO hay transferencia: hay las DECISIONES de una transferencia. Vive en la capa base
// porque la interfaz de Qt y el servidor web tienen que tomar las mismas —cuál de los tres
// caminos, y por qué no se puede cuando no se puede— y una segunda copia de esas reglas se
// desincroniza en el primer arreglo.
//
// Ver docs/diseno_tecnico_transferencias.md. Esta es la fase 0: los tipos y la elección,
// que se pueden probar sin mover un solo byte.
namespace zfsmgr::base::transferencia {

// Por dónde van los bytes. El orden de la enumeración ES el de preferencia.
//
// **Esta lista salió de LEER el código, y corrigió el diseño**: allí se habían apuntado tres
// caminos con el respaldo por tar dentro. No es así. El tar es cosa de Sincronizar —que
// mueve FICHEROS con rsync y tar, no `zfs send`— y Copiar no lo tiene: cuando no hay
// tubería que montar, se para y lo dice.
enum class Camino {
    // Lo lanza `--job-submit` y lo sostiene el daemon. Sobrevive a que se cierre el
    // cliente, y es el ÚNICO que le sirve al servidor web.
    TrabajoAsincrono,
    // `--zfs-recv-listen` en el destino y `--zfs-send-to-peer` en el origen. Sin shell y
    // sin que los bytes pasen por el cliente, pero lo sostiene quien lo lanzó.
    DaemonADaemon,
    // `ssh origen 'zfs send' | ssh destino 'zfs recv'`, en sus variantes. No necesita
    // daemon en ningún extremo: es lo que queda cuando no hay.
    TuberiaSsh,
    Ninguno,
};

// Por qué no se puede. TIPIFICADO porque es lo que hay que enseñar: «no disponible» sin
// decir cuál de los seis motivos es deja al usuario probando combinaciones.
enum class Fallo {
    Ninguno,
    ElMismoObjeto,
    OrigenNoEsInstantanea,
    DestinoNoEsDataset,
    ExtremoWindows,          // el agente de Windows no transmite por tubería todavía
    SinTrabajos,             // hace falta el camino asíncrono y algún extremo no lo admite
    ZfsDemasiadoViejo,       // por debajo de 2.3.3 no se transfiere
};

const char* claveDe(Camino c);
const char* claveDe(Fallo f);
std::string etiquetaDe(Camino c);
std::string etiquetaDe(Fallo f);

// Lo que hay que saber de un extremo para decidir. No se consulta nada desde aquí: lo trae
// quien llama, que es el que tiene la sesión de transporte.
struct Extremo {
    std::string conexion;
    std::string objeto;          // dataset, o dataset@instantánea en el origen
    bool esWindows{false};
    bool tieneDaemon{false};
    bool admiteTrabajos{false};  // `JOBS_SUPPORT=1` en su `--health`
    std::string versionZfs;      // «2.3.3», «2.2.99-1», … vacía si no se sabe

    bool esInstantanea() const { return objeto.find('@') != std::string::npos; }
    std::string dataset() const {
        const std::size_t i = objeto.find('@');
        return i == std::string::npos ? objeto : objeto.substr(0, i);
    }
};

// ¿Esta versión de OpenZFS puede transferir?
//
// Por debajo de **2.3.3** no. Es una regla del proyecto, no de ZFS, y estaba escrita dentro
// de la ventana. Una versión vacía o que no se entiende NO bloquea: no saber la versión es
// distinto de saber que es vieja, y bloquear por no saber dejaría sin copiar a una máquina
// que quizá puede.
bool versionAdmiteTransferencia(const std::string& version);

// Las banderas de `zfs send`, en el orden en que las escribe el programa.
struct OpcionesDeEnvio {
    bool w{false};   // crudo: manda el dataset cifrado tal cual, sin descifrarlo
    bool L{false};   // bloques grandes
    bool e{false};   // «embedded»: aprovecha los bloques ya comprimidos
    bool c{false};   // comprimido
    bool R{false};   // toda la jerarquía, con sus instantáneas
};

// «-wLR», o vacío si no hay ninguna. Vacío y no «-»: un guion con un «-» suelto en medio
// es un argumento que `zfs` no entiende.
std::string banderasDeEnvio(const OpcionesDeEnvio& o);

// Los caminos que se pueden probar, EN ORDEN, y no uno solo.
//
// Porque así es como funciona: se intenta el primero y, si no se puede montar, se cae al
// siguiente. Y eso se decide en marcha —el `recv-listen` puede fallar en el destino— no
// aquí. Lo que se decide aquí es cuáles tiene sentido intentar.
struct Plan {
    std::vector<Camino> caminos;
    Fallo fallo{Fallo::Ninguno};

    bool sePuede() const { return !caminos.empty(); }
};

// Qué caminos tiene sentido probar entre estos dos extremos.
//
// `exigeAsincrono` lo pone quien NO puede sostener la transferencia mientras dure: el
// servidor web atiende de una en una y una petición no puede durar cuatro horas, así que
// para él solo vale el camino por trabajos. La interfaz puede esperar y no lo exige.
Plan planea(const Extremo& origen, const Extremo& destino, bool exigeAsincrono);

// El testigo de reanudación que ZFS dejó en el destino, si hay alguno.
//
// **Se busca en el objetivo Y en sus descendientes**, y ese detalle no es un adorno: las
// copias van con `-R`, o sea toda la jerarquía en un solo flujo, y al cortarse ZFS deja el
// testigo en el dataset que estaba recibiendo en ese momento, que casi nunca es la raíz.
// Medido cortando una copia de 3,4 GB: el padre quedó completo y el testigo apareció en el
// hijo. Mirar solo la raíz decía «no hay nada que reanudar» con 247 MB ya transferidos.
struct Reanudacion {
    std::string testigo;
    std::string quienLoTiene;   // el dataset donde estaba

    bool hay() const { return !testigo.empty(); }
};

// La REGLA de cuál gana, separada de ir a buscarlos.
//
// Recibe líneas «dataset<TAB>testigo», con «-» donde no hay ninguno. Quien las junta es
// `buscaTestigo`, más abajo; aquí solo se decide, y por eso se puede probar sin máquina.
Reanudacion testigoDeReanudacion(const std::string& objetivo, const std::string& salidaTsv);

// La dirección con la que el ORIGEN ve a este equipo, sacada de lo que devuelve
// `echo $SSH_CLIENT`. Vacía si no vale.
//
// **Admite IPv6 CON zona**: sshd puede contestar `fe80::d11d:24e3:5547:cbd6%enp1s0f0`, que
// es justo lo que devolvió la máquina de pruebas. Una validación de solo hexadecimal y
// puntos lo rechazaba y dejaba la copia sin dirección a la que volver.
std::string direccionDeSshClient(const std::string& salida);

// ── Cómo se compone la orden de copiar ───────────────────────────────────────

// Dónde se recibe de verdad.
//
// No es el dataset sobre el que se pulsó: se le añade el NOMBRE DEL ORIGEN, para que copiar
// «datos» sobre «respaldos» deje «respaldos/datos» y no vuelque encima. Salvo que el destino
// ya acabe en ese nombre, en cuyo caso se toma tal cual — o si no, copiar dos veces al mismo
// sitio crearía «respaldos/datos/datos».
//
// Ese detalle es también el que hace que buscar el testigo de reanudación sobre el dataset
// pulsado no encuentre nada: hay que buscarlo sobre ESTE.
std::string destinoReal(const std::string& origenDataset, const std::string& destinoElegido);

// `zfs send [banderas] <instantánea>` y `zfs recv -Fus <destino>`, sin envolver.
//
// El `-Fus` del receptor no es decorativo: la «s» es lo que hace que un corte deje un envío
// EN SUSPENSO con su testigo, en vez de basura. Sin ella no habría reanudación posible y
// cada corte obligaría a mandarlo todo otra vez.
std::string ordenDeEnvio(const std::string& instantanea, const std::string& banderas);
std::string ordenDeRecepcion(const std::string& destino);

// Cómo se juntan los dos lados. Es lo que cambia según dónde estén los extremos.
enum class Montaje {
    MismaConexion,      // los dos en la misma máquina: una tubería local
    RemotoARemotoDirecto,  // los dos remotos por SSH: el origen se conecta al destino
    PorElCliente,       // `ssh origen … | ssh destino …`: los bytes pasan por aquí
};

// Cuál de los tres toca. `PorElCliente` es el que siempre vale y el más caro: los datos dan
// un rodeo por esta máquina.
Montaje montajeDe(const ConnectionProfile& origen, const ConnectionProfile& destino,
                  bool mismaConexion);

// ── Lo que sí va a preguntar a las máquinas ──────────────────────────────────

// Con qué dirección ve el ORIGEN a este equipo.
//
// Se le PREGUNTA a él en vez de deducirlo: la máquina puede tener varias interfaces, estar
// detrás de NAT o llegar por VPN, y solo el otro extremo sabe por dónde entró la conexión.
std::string comoMeVeElOrigen(TransportSession& ses, const ConnectionProfile& origen,
                             bool verboso);

// El testigo de reanudación que haya en el destino o en sus descendientes.
//
// Son N+1 consultas —una por dataset—, que es lo que hace hoy la interfaz. Se conserva tal
// cual a propósito: esta fase no cambia comportamiento. Con un verbo que leyera una
// propiedad de forma recursiva sería una sola, y está anotado en el diseño.
Reanudacion buscaTestigo(TransportSession& ses, const ConnectionProfile& destino,
                         const std::string& objetivo, bool verboso);

}  // namespace zfsmgr::base::transferencia
