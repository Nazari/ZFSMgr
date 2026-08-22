#pragma once

#include <string>
#include <vector>

// Lo que se le PIDE al agente, una función por cosa.
//
// **Por qué existe.** El nombre de un verbo —«--dump-zpool-status»— es un contrato entre el
// daemon y sus tres clientes, y hasta ahora se escribía a mano en cada uno. Medido antes de
// escribir esto: de 59 verbos, **34 aparecían literalmente en dos o tres clientes**. Veinte
// de ellos en los tres.
//
// Eso no es feo, es frágil de una manera concreta: con el verbo se reparte también **cuántos
// argumentos lleva y en qué orden**, y eso no está escrito en ninguna parte. Cuando un verbo
// gana un argumento, el cliente que no se entera no falla al compilar: falla en ejecución,
// contra una máquina, y con suerte.
//
// Aquí no se decide nada más. No hay transporte, no hay sudo, no hay formato de salida: solo
// el argv. Quién lo ejecuta y cómo es cosa de cada cliente, porque una conexión Local no se
// alcanza igual que una remota.
//
// Las mutaciones con reglas propias NO están aquí: viven donde vive su regla —`pools`,
// `instantaneas`, `datasets`, `avanzadas`, `zfsallow`—, porque componer su argv exige saber
// qué significa cada bandera. Esto es para lo que no tiene más regla que su nombre.
namespace zfsmgr::commands::peticiones {

// ── Lecturas de pool ─────────────────────────────────────────────────────────

std::vector<std::string> listaDePools();
std::vector<std::string> estadoDePool(const std::string& pool);
// La variante `-p`: los mismos datos sin redondear. Son dos verbos y no una bandera porque
// el daemon los sirve por separado.
std::vector<std::string> estadoDePoolCrudo(const std::string& pool);
std::vector<std::string> historialDePool(const std::string& pool);
std::vector<std::string> propiedadesDePool(const std::string& pool);
std::vector<std::string> guidDePool(const std::string& pool);
// Los pools que se podrían importar. No lleva argumentos: pregunta por todos.
std::vector<std::string> sondaDeImportables();

// ── Lecturas de dataset ──────────────────────────────────────────────────────

// El árbol entero bajo un objeto, en TSV de diez columnas. Ver `listados::entradas`.
std::vector<std::string> listaDeDatasets(const std::string& objeto);
// Solo los nombres, recursivo.
std::vector<std::string> nombresDeDescendientes(const std::string& objeto);
// Los directorios que Desglosar puede convertir en datasets. Contesta «__MP__=<punto>» y
// luego una ruta relativa por línea. Vale en las dos plataformas: resuelve el punto de
// montaje con los montajes REALES, que en Windows es una letra de unidad.
std::vector<std::string> listaDeDesglose(const std::string& dataset);
std::vector<std::string> propiedadesDeDataset(const std::string& objeto);
std::vector<std::string> propiedadDeDataset(const std::string& propiedad,
                                            const std::string& objeto);
std::vector<std::string> existeDataset(const std::string& objeto);
std::vector<std::string> mapaDeGuids(const std::string& objeto);
// Varias propiedades de un objeto en una sola consulta. La lista va SEPARADA POR COMAS en un
// único argumento, no como argumentos sueltos: pedirlas de una en una son N viajes.
std::vector<std::string> propiedadesConcretas(const std::vector<std::string>& propiedades,
                                              const std::string& objeto);
// Los permisos de varios datasets a la vez.
std::vector<std::string> permisosDeVarios(const std::vector<std::string>& datasets);
// El GUID y el estado de TODOS los pools de golpe, que es lo que necesita el refresco. No
// lleva argumentos: preguntar pool a pool eran N viajes por refresco.
std::vector<std::string> guidYEstadoDeLosPools();
std::vector<std::string> montajes();
// Las letras de unidad de un pool, con su origen —«local», «temporary» o heredada—.
//
// El origen NO es un detalle: en Windows los descendientes heredan la letra del pool y se
// montan planos bajo esa unidad, así que dos datasets con la misma letra heredada es el
// funcionamiento normal. Sin el origen, cualquier pool con más de un dataset parecía tener
// letras duplicadas. Comprobado contra OldLau: «winpool Z: local», «winpool/sa z: temporary».
//
// Fuera de Windows el verbo existe pero `zfs` contesta que la propiedad no existe —en macOS,
// «invalid property 'driveletter'», comprobado— y devuelve un código distinto de cero. Quien
// llama lo lee como «no hay letras», que es la verdad.

std::vector<std::string> letrasDeUnidad(const std::string& pool);
std::vector<std::string> permisosDe(const std::string& dataset);
// Varios objetos en una llamada: el verbo los acepta detrás.
std::vector<std::string> holdsDe(const std::vector<std::string>& objetos);
std::vector<std::string> diferenciaEntre(const std::string& instantaneaA,
                                         const std::string& instantaneaB);

// ── Instantáneas programadas (GSA) ───────────────────────────────────────────

std::vector<std::string> gsaDeDataset(const std::string& dataset);
std::vector<std::string> gsaDeTodosLosPools();

// ── Ficheros ─────────────────────────────────────────────────────────────────

std::vector<std::string> contenidoDeDirectorio(const std::string& ruta);
// `desde` y `cuanto` en bytes; cero y cero significa el fichero entero.
std::vector<std::string> contenidoDeFichero(const std::string& ruta, unsigned long long desde,
                                            unsigned long long cuanto);

// ── El propio agente ─────────────────────────────────────────────────────────

std::vector<std::string> salud();
// El registro del daemon: desde qué byte y cuántos como mucho. Cero y cero es entero.
//
// Son BYTES, no líneas, aunque el nombre del verbo no lo diga: el daemon hace `seek` sobre
// el fichero. Confundirlo con líneas es lo que hace que un cliente pida «las últimas 200» y
// reciba 200 bytes a media palabra.
std::vector<std::string> registro(unsigned long long desdeByte, unsigned long long cuantos);
std::vector<std::string> dispositivosDeBloque();
std::vector<std::string> versionDeZfs();
std::vector<std::string> herramientasDisponibles();
std::vector<std::string> datosBasicosDelRefresco();
std::vector<std::string> pares();

// ── Mutaciones sin más regla que su forma ────────────────────────────────────
//
// Las que SÍ tienen regla —qué bandera significa qué, qué alcance es cuál— se componen en su
// módulo: `pools`, `instantaneas`, `datasets`, `avanzadas`, `zfsallow`. Aquí solo están las
// que se limitan a envolver un argv o a pasar unos argumentos.

// Un `zfs <op> …` cualquiera, con el argv codificado. El daemon lo ejecuta con execvp y
// comprueba que `op` esté en su lista blanca.
std::vector<std::string> zfsGenerico(const std::string& argvCodificado);
std::vector<std::string> zpoolGenerico(const std::string& argvCodificado);
// Crear un dataset es un verbo aparte y no un `zfs create` genérico **porque puede llevar
// frase de cifrado**: el daemon la recibe por la carga del RPC y se la da a `zfs` por una
// tubería. Por el camino genérico acabaría en argv, visible en un `ps`.
std::vector<std::string> creaDataset(const std::string& argvCodificado);

// **Estas dos llevan sus argumentos en base64, y no por capricho.** Una frase de paso en
// argv la ve cualquiera con un `ps` en la máquina; codificada viaja dentro de la carga del
// RPC, que va cifrada. Quien llama pasa el texto en claro y aquí se codifica: dejarlo en
// manos del llamante era invitar a que uno se olvidara.
std::vector<std::string> cargaClave(const std::string& dataset, const std::string& frase);
// `nueva` vacía significa quitar la clave.
std::vector<std::string> cambiaClave(const std::string& dataset, const std::string& frase,
                                     const std::string& nueva);

std::vector<std::string> reparaMontajesAlternativos(const std::vector<std::string>& extras);
std::vector<std::string> fijaPares(const std::string& cargaB64);
std::vector<std::string> fijaEscucha(const std::string& direccion);
std::vector<std::string> copiaConRsync(const std::string& cargaB64);
std::vector<std::string> permisosEnLote(const std::string& cargaB64);

// ── Trabajos ─────────────────────────────────────────────────────────────────

// Encolar: el verbo va DELANTE de la orden que se encola, no detrás.
//
// El daemon solo acepta encolar unas pocas mutaciones —las largas—, así que una lista vacía
// o una orden que no sea de esas devuelve vacío en vez de mandar algo que va a rebotar.
std::vector<std::string> encola(const std::vector<std::string>& orden);
bool sePuedeEncolar(const std::string& verbo);
std::vector<std::string> listaDeTrabajos();
std::vector<std::string> estadoDeTrabajo(const std::string& id);
std::vector<std::string> cancelaTrabajo(const std::string& id);

}  // namespace zfsmgr::commands::peticiones
