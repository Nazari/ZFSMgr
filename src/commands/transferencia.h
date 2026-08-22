#pragma once

#include <functional>
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

// Aquí vivían `Montaje` y `montajeDe`: cuál de las tres formas de juntar los dos lados
// tocaba —tubería local, remoto a remoto directo, o pasando los bytes por este equipo—.
//
// Se retiraron cuando Copiar y Nivelar dejaron de tener respaldos por shell. Las tres
// formas eran formas de encadenar `ssh` y tuberías; con la transferencia hecha por un
// trabajo del daemon no hay nada que montar: el receptor abre un puerto y el emisor se
// conecta. La regla no se ha perdido, ha dejado de existir.

// ── El camino asíncrono: un trabajo en el daemon ─────────────────────────────
//
// Es el único que le sirve al servidor web, porque lo sostiene el daemon y no quien lo
// lanzó. Son tres pasos: el receptor abre un puerto, se averigua con qué dirección tiene
// que volver el emisor, y el emisor arranca el envío y devuelve un identificador.

// Lo que contesta `--zfs-recv-listen`: en qué puerto espera y con qué testigo.
struct EscuchaDelReceptor {
    int puerto{0};
    std::string testigo;

    // El testigo son 64 caracteres. Una respuesta con otra longitud no es que venga
    // recortada: es que no es la respuesta que se esperaba, y seguir con ella dejaría al
    // emisor hablando con quien no debe.
    bool vale() const { return puerto > 0 && testigo.size() == 64; }
};

EscuchaDelReceptor leeEscucha(const std::string& salida);
std::string leeIdentificadorDeTrabajo(const std::string& salida);

// Por qué no arrancó el trabajo. Los cinco puntos donde puede romperse, separados, porque
// cada uno lleva a un sitio distinto: uno es del receptor, otro de la red, otro del emisor.
enum class FalloTrabajo {
    Ninguno,
    ReceptorNoEscucha,
    RespuestaDeEscuchaNoVale,
    SinDireccionDeVuelta,
    EmisorNoArranco,
    SinIdentificador,
};

std::string etiquetaDe(FalloTrabajo f);

struct Trabajo {
    std::string id;
    FalloTrabajo fallo{FalloTrabajo::Ninguno};
    std::string detalle;

    bool ok() const { return fallo == FalloTrabajo::Ninguno && !id.empty(); }
};

// Cómo se le habla al agente de una máquina. **Lo pone quien llama, y no es un capricho.**
//
// Una conexión LOCAL no se alcanza igual que una remota: el RPC por túnel rechaza de
// entrada todo lo que no sea SSH, así que para la local hay que ir por el socket del daemon
// con su material TLS. Cada cliente ya sabe hacerlo —la interfaz con `runAgentCommand`, el
// servidor web con `llamaAgente`— y meter aquí esa distinción obligaría a subir a la capa
// base el descubrimiento del TLS local, que es de otro sitio.
//
// Se perdió al extraer esto de la ventana y lo cazó la primera prueba de verdad: el trabajo
// no arrancaba porque el destino era «Local» y se le estaba hablando como si fuera remoto.
using LlamadaAlAgente = std::function<bool(const ConnectionProfile& maquina,
                                           const std::vector<std::string>& args, int timeoutMs,
                                           std::string& salida, std::string& err, int& rc)>;

// Arranca el trabajo. Devuelve en cuanto lo tiene lanzado: NO espera a que termine, que es
// justo el motivo de que exista.
//
// Con `testigoReanudacion` puesto, la instantánea, la base y las banderas van vacías a
// propósito: `zfs send -t` lleva dentro qué continuar y no admite que se le contradiga.
Trabajo lanzaTrabajo(TransportSession& ses, const LlamadaAlAgente& llama,
                     const ConnectionProfile& origen, const ConnectionProfile& destino,
                     const std::string& instantanea, const std::string& destinoDelRecv,
                     const std::string& desdeInstantanea, const std::string& banderas,
                     const std::string& testigoReanudacion, bool mismaConexion, bool verboso);

// ── Lo que sí va a preguntar a las máquinas ──────────────────────────────────

// Con qué dirección ve el ORIGEN a este equipo.
//
// Se le PREGUNTA a él en vez de deducirlo: la máquina puede tener varias interfaces, estar
// detrás de NAT o llegar por VPN, y solo el otro extremo sabe por dónde entró la conexión.
// Con qué dirección tiene que conectar el ORIGEN para llegar al DESTINO.
//
// Vacío si no se puede averiguar. Lo usan el flujo de `zfs send` y el árbol de ficheros;
// ver el comentario de la implementación para el caso de la conexión Local, que es el que
// se pierde en cuanto alguien copia esta regla en vez de llamarla.
// El mismo baile de tres pasos, pero llevando un ÁRBOL DE FICHEROS en vez de una
// instantánea: el destino escucha, se averigua con qué dirección lo ve el origen, y el
// origen envía.
//
// **Para qué sirve: «Desde Dir» sin tubería de shell.** Esa acción movía los datos con
// `ssh origen 'tar -c' | ssh destino 'agente --mutate-advanced-fromdir'`, o sea pasando
// TODO el contenido por la máquina de quien manda: copiar 100 GB de una máquina a otra
// movía 200 GB por la de en medio. Aquí van de daemon a daemon.
//
// Y de paso se gana lo que ya tenía el flujo de `zfs send`: es un trabajo, así que hay
// progreso, se puede cancelar y sobrevive a que se cierre la ventana. Además la copia es
// incremental —salta lo que ya está igual comparando tamaño y fecha—, mientras que el tar
// reenviaba el árbol entero en cada pasada.
//
// El directorio de destino tiene que EXISTIR: el receptor lo comprueba y falla si no. Para
// crearlo está `avanzadas::argvDesdeDirPreparar`, que además monta el dataset y resuelve su
// punto de montaje real.
//
// Requiere daemon en LAS DOS puntas. El camino del tar solo lo pedía en el destino, así que
// esto no lo sustituye: lo adelanta cuando se puede.
//
// `comoTrabajo` decide si el envío se encola en el daemon —la ventana lo quiere así: no la
// bloquea, se puede cancelar y sigue si se cierra— o si se espera a que termine, que es lo
// que hace el intérprete porque su orden ya devolvía el resultado y un guion detrás cuenta
// con que los ficheros estén.
//
// **Ojo con cómo se comprueba el resultado.** Con `comoTrabajo` falso no hay identificador
// que devolver, así que `ok()` —que exige uno— diría que no aunque todo haya ido bien: ahí
// lo que se mira es `fallo`. `ok()` significa «hay un trabajo al que seguirle la pista», no
// «salió bien».
Trabajo lanzaTrabajoDeArbol(TransportSession& ses, const LlamadaAlAgente& llama,
                            const ConnectionProfile& origen, const ConnectionProfile& destino,
                            const std::string& directorioOrigen,
                            const std::string& directorioDestino, bool mismaConexion,
                            bool verboso, bool comoTrabajo, bool borrarEnDestino = false,
                            bool enSeco = false, std::string* salidaDelEnvio = nullptr);

std::string haciaDondeConecta(TransportSession& ses, const ConnectionProfile& origen,
                              const ConnectionProfile& destino, bool mismaConexion,
                              bool verboso);

std::string comoMeVeElOrigen(TransportSession& ses, const ConnectionProfile& origen,
                             bool verboso);

// El testigo de reanudación que haya en el destino o en sus descendientes.
//
// Son N+1 consultas —una por dataset—, que es lo que hace hoy la interfaz. Se conserva tal
// cual a propósito: esta fase no cambia comportamiento. Con un verbo que leyera una
// propiedad de forma recursiva sería una sola, y está anotado en el diseño.
Reanudacion buscaTestigo(TransportSession& ses, const ConnectionProfile& destino,
                         const std::string& objetivo, bool verboso);

// ---------------------------------------------------------------------------
// Nivelar: poner el destino al día del origen SIN volver a mandarlo todo.
//
// **No es copiar.** Copiar manda un flujo completo y recibe en «<destino>/<hoja>»; nivelar
// manda un INCREMENTAL —`zfs send -I <base> <objetivo>`— y recibe en el dataset destino
// tal cual. Confundirlos no es un matiz: con el destino ya poblado, el flujo completo llega
// con `zfs recv -Fus` y arrastra lo que el origen no tenga.
//
// La base común NO se busca por nombre, se busca por GUID. Dos instantáneas pueden
// llamarse igual en las dos máquinas sin tener nada que ver —basta con que las hayan creado
// por separado— y enviar un incremental contra una base falsa es enviar contra otra
// historia. El GUID ya viene en `--dump-zfs-list-all`, así que no cuesta ninguna consulta.
//
// Las tres negativas son de seguridad y vienen de la interfaz de Qt, que las tiene desde el
// principio: sin ellas, nivelar puede tirar trabajo del destino sin avisar.
struct Instantanea {
    std::string nombre;   // corto, sin «dataset@»
    std::string guid;
};

enum class FalloNivelar {
    Ninguno,
    ObjetivoNoEstaEnOrigen,
    DestinoSinInstantaneas,
    BaseNoEstaEnOrigen,
    DestinoMasNuevo,
    YaNivelado,
};

struct PlanNivelar {
    std::string base;       // desde dónde: el «-I» del envío
    std::string objetivo;   // hasta dónde
    FalloNivelar fallo{FalloNivelar::Ninguno};
    bool sePuede() const { return fallo == FalloNivelar::Ninguno; }
};

// Las dos listas van EN ORDEN DE CREACIÓN, que es como las da `zfs list -t snapshot`. El
// orden es el que decide qué es «más nuevo», así que darlas ordenadas de otra forma no
// devuelve un error: devuelve una respuesta equivocada.
PlanNivelar planeaNivelar(const std::vector<Instantanea>& origen,
                          const std::vector<Instantanea>& destino,
                          const std::string& objetivo);

std::string etiquetaDe(FalloNivelar f);

}  // namespace zfsmgr::base::transferencia
