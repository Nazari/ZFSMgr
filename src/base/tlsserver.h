#pragma once

#include <functional>
#include <string>

// Un servidor TLS mínimo, sin Qt: acepta conexiones y entrega bytes.
//
// No sabe de HTTP ni del protocolo del daemon: lee, deja que quien llame conteste, y
// cierra. Quien decide qué significan esos bytes es el llamante.
//
// Vive en la capa base porque lo necesitan dos artefactos —el agente ya emitía sus propios
// certificados, y ahora el servidor web necesita los suyos— y porque emitirlos con OpenSSL
// en vez de invocando `openssl` por shell es lo que hace que funcione en Windows, donde ese
// binario no está en el PATH.
namespace zfsmgr::base::tlsserver {

// Emite un par certificado + clave AUTOFIRMADO.
//
// Lleva subjectAltName, keyUsage y extendedKeyUsage porque sin ellos el backend TLS de
// Apple los rechaza. Los ficheros quedan con permisos solo del dueño.
bool escribeParAutofirmado(const std::string& rutaCert, const std::string& rutaClave,
                           const std::string& commonName, bool paraServidor,
                           const std::string& altNames, std::string& error);

// Escucha en `bind:puerto` y atiende conexiones de una en una.
//
// `atiende` recibe lo que llegó y devuelve lo que hay que contestar; si devuelve false, se
// cierra sin responder. `sigueVivo` se consulta entre conexiones para poder parar.
//
// De una en una y no con hilos: en la fase 0 lo que importa es la superficie, no el
// rendimiento, y un servidor secuencial no tiene carreras que revisar.
// Escribe un trozo de respuesta directamente en la conexión. Devuelve false si se cortó.
using Escritor = std::function<bool(const char* datos, std::size_t cuantos)>;

// `alEscuchar`, si se pasa, se llama UNA vez y solo cuando el socket ya está escuchando.
// Es donde va el cartel de «servidor en marcha»: escribirlo antes de llamar aquí deja al
// usuario con una URL que nunca funcionó cuando el puerto estaba cogido.
bool sirve(const std::string& bind, int puerto, const std::string& rutaCert,
           const std::string& rutaClave,
           const std::function<bool(const std::string& peticion, std::string& respuesta)>& atiende,
           const std::function<bool()>& sigueVivo, std::string& error,
           const std::function<void()>& alEscuchar = {},
           // Para respuestas que NO caben en memoria. Se prueba antes que `atiende`; si
           // devuelve true, ya escribió lo suyo y no se llama al otro.
           //
           // Existe por los ficheros: servir una imagen de 50 GB componiendo la respuesta
           // en un `std::string` es pedir 50 GB de memoria para mandarlos por un socket.
           const std::function<bool(const std::string& peticion, const Escritor& escribe)>&
               atiendeChorro = {});

}  // namespace zfsmgr::base::tlsserver
