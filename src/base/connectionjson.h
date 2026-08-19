#pragma once

#include <string>

#include "connectionprofile.h"
#include "json.h"
#include "storewarnings.h"

// Traducción entre `ConnectionProfile` y el JSON que se guarda en disco, sin Qt.
//
// Es el pegamento de `ConnectionStore`, y lo que decide qué acaba escrito en
// `config.json` y en `trust-store.json`. El formato no cambia: hay ficheros ya escritos
// así. Ver docs/diseno_tecnico_capa_base_sin_qt.md.
namespace zfsmgr::base::connjson {

// Puerto SSH por omisión cuando no hay uno válido guardado.
int ensurePort(const std::string& connType, int port);

// «local» por identificador o por tipo de conexión.
bool isLocalProfile(const ConnectionProfile& p);

// La conexión local en Unix va SIEMPRE con sudo: sin él no se puede leer ni la mitad de
// lo que la aplicación necesita. En Windows no aplica porque allí no hay sudo.
bool shouldForceLocalSudo(const ConnectionProfile& p);

bool profileHasDaemonTls(const ConnectionProfile& p);

// Abre con la maestra los cinco campos que van cifrados: usuario, contraseña y el trío
// TLS del daemon. Devuelve false si alguno se quedó cerrado.
//
// **Un campo que no se pudo abrir CONSERVA su texto cifrado**, y por eso se avisa: quien
// lo reciba no debe usarlo como si fuera el valor en claro. Cada fallo va en `avisos` con
// su motivo, su conexión y su campo, sin texto: lo redacta quien tenga catálogo.
//
// Estaba escrito dos veces —la interfaz recogía avisos tipificados y el intérprete se los
// tragaba en silencio— y por eso la misma configuración se describía distinto según por
// dónde se mirara.
bool abreSecretos(ConnectionProfile& p, const std::string& maestra, store::Avisos& avisos);

// Funde en cada perfil el material TLS que viva en el almacén de confianza, indexando por
// identificador.
//
// **Manda el ALMACÉN, no el perfil**, y esto es una decisión, no un detalle: el almacén es
// donde se persiste el material que se negocia con cada daemon, y lo que quede en
// `config.json` es de antes de que existiera —hay una migración que lo saca de ahí—. Las
// dos mitades del programa hacían esto al revés la una de la otra: la interfaz dejaba
// ganar al almacén y el intérprete al perfil, así que con material viejo todavía en
// `config.json` una usaba el fresco y la otra el rancio.
//
// NO añade perfiles: una entrada del almacén sin conexión que le corresponda se ignora
// aquí. La interfaz sí la añade, y esa decisión se queda donde estaba.
void fundeTrustStore(std::vector<ConnectionProfile>& perfiles, const json::Value& trust,
                     const std::string& maestra, store::Avisos& avisos);

// PSRP se retiró como transporte: no admite el daemon, porque el RPC viaja por un túnel
// `ssh -L` y sin SSH no hay túnel. Un perfil guardado con PSRP no puede quedarse como
// está —fallaría de forma opaca— ni desaparecer sin más, así que se convierte a SSH.
//
// El puerto es la parte que se olvida: 5986 es WinRM, y dejarlo convierte una conexión
// rota en una conexión rota SIN explicación, que es peor que la de partida.
bool migratePsrpProfileToSsh(ConnectionProfile& p);

// Si `raw` es el hexadecimal ASCII de un UUID, devuelve el UUID; si no, vacío.
//
// Imita a `QByteArray::fromHex`, que **se salta los caracteres no hexadecimales** en vez
// de fallar, y que ante una cantidad impar de dígitos actúa como si llevara un '0'
// delante. No es un capricho: hay identificadores guardados que dependen de eso.
std::string decodeHexAsciiIfUuid(const std::string& raw);

// `uidLocal` es el identificador de ESTA máquina. Se pasa como argumento en vez de
// consultarlo aquí porque averiguarlo cuesta entre 400 y 600 ms —lanza `ioreg` en macOS
// o lee el registro en Windows— y eso es justo lo que no puede vivir en la capa base.
// Solo se usa como respaldo para el perfil local cuando no hay nada guardado.
std::string normalizeMachineUidForStorage(const ConnectionProfile& p,
                                          std::string raw,
                                          const std::string& uidLocal);

// `config.json`: los datos de conexión y la contraseña. SIN el material TLS, que vive
// aparte en el almacén de confianza.
json::Value connectionToJson(const ConnectionProfile& p, const std::string& uidLocal);

// `trust-store.json`: lo mismo SIN contraseña y CON el material TLS.
json::Value connectionTrustToJson(const ConnectionProfile& p, const std::string& uidLocal);

// Lee de cualquiera de los dos: los campos ausentes se quedan con su valor por omisión.
ConnectionProfile connectionFromJson(const json::Value& obj, const std::string& uidLocal);

// Posición de una conexión por identificador, sin distinguir mayúsculas. -1 si no está.
long long indexOfConnectionById(const json::Array& connections, const std::string& id);

// Inserta o sustituye por identificador. Devuelve false si el perfil no tiene ninguno.
bool upsertConnectionJson(json::Array& connections,
                          const ConnectionProfile& p,
                          const std::string& uidLocal);

}  // namespace zfsmgr::base::connjson
