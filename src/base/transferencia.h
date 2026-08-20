#pragma once

#include <string>
#include <vector>

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

    bool esInstantanea() const { return objeto.find('@') != std::string::npos; }
    std::string dataset() const {
        const std::size_t i = objeto.find('@');
        return i == std::string::npos ? objeto : objeto.substr(0, i);
    }
};

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

// De la salida de `zfs get -H -o name,value -r receive_resume_token <objetivo>`: una línea
// por dataset, con «-» donde no hay ninguno.
//
// UNA consulta para todo el subárbol, y no una por descendiente como hacía la interfaz: la
// regla de cuál gana es la misma —el propio objetivo primero, y si no el primer
// descendiente que tenga uno— pero se paga una llamada en vez de N.
Reanudacion testigoDeReanudacion(const std::string& objetivo, const std::string& salidaTsv);

}  // namespace zfsmgr::base::transferencia
