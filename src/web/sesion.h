#pragma once

#include <string>

// La sesión del navegador y el testigo anti-CSRF.
//
// **Por qué hace falta y en el intérprete no.** Un terminal solo hace lo que uno teclea; un
// navegador manda peticiones que uno no ha pedido. Cualquier página abierta en otra pestaña
// puede enviar un formulario a `https://127.0.0.1:…/destruir` y el navegador adjuntaría la
// cookie de sesión por su cuenta. El testigo lo impide: va en el formulario, y una página
// ajena no puede leerlo —para eso está la política del mismo origen—.
namespace zfsmgr::web {

class Sesion {
public:
    // Abre una sesión nueva con su identificador y su testigo, ambos de 32 bytes al azar
    // de OpenSSL. Al azar de verdad: `std::rand()` aquí sería adivinable.
    void abre();

    bool abierta() const { return !m_id.empty(); }
    const std::string& id() const { return m_id; }
    const std::string& testigo() const { return m_testigo; }

    // ¿Es esta la cookie de la sesión viva? Comparación en TIEMPO CONSTANTE: comparar con
    // `==` filtra por el tiempo cuántos bytes coinciden.
    bool cookieVale(const std::string& valor) const;

    // ¿Es este el testigo del formulario? También en tiempo constante.
    bool testigoVale(const std::string& valor) const;

    // La cabecera `Set-Cookie` de esta sesión.
    //
    // `HttpOnly` para que ningún script pueda leerla, `Secure` porque solo viaja por TLS,
    // y `SameSite=Strict` para que el navegador no la adjunte a peticiones que vengan de
    // otro sitio — que es la segunda mitad de la defensa contra CSRF.
    std::string cabeceraCookie() const;

    void cierra();

private:
    std::string m_id;
    std::string m_testigo;
};

// 32 bytes al azar en hexadecimal. Vacío si OpenSSL no puede darlos, y entonces quien
// llame NO debe seguir: una sesión con un identificador previsible no es una sesión.
std::string alAzarHex();

}  // namespace zfsmgr::web
