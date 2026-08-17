#pragma once

#include <functional>
#include <string>
#include <vector>

// Leer una línea del terminal con historial y completado por tabulador.
//
// **Escrito a mano y no con `readline`**, por la misma razón por la que las expresiones
// regulares son `std::regex`: meter una dependencia por comodidad va en contra de todo lo
// hecho hasta ahora, y además GNU readline es GPL, que para un binario que se distribuye
// no es una decisión que se tome de paso.
//
// Cubre lo que se usa de verdad al teclear órdenes: escribir, borrar, moverse, historial
// arriba y abajo, inicio y fin, y el tabulador. No pretende ser un editor completo.
//
// Sin terminal —una tubería, un `cron`— se cae a leer la línea entera y ya está: sin eco
// que gestionar, el completado no significa nada.
namespace zfsmgr::cli {

// Qué se puede poner donde el cursor. Recibe la línea entera y la posición del cursor, y
// devuelve las opciones y desde qué byte de la línea sustituyen.
using Completador =
    std::function<std::vector<std::string>(const std::string& linea, std::size_t cursor,
                                           std::size_t& desdeByte)>;

class LectorDeLinea {
public:
    // `indicador` se reescribe en cada repintado, así que no puede llevar nada que ocupe
    // sitio sin verse.
    bool lee(const std::string& indicador, std::string& out);

    void setCompletador(Completador c) { m_completa = std::move(c); }
    void recuerda(const std::string& linea);

private:
    // `anchoPintado` entra con lo que ocupaba el repintado anterior y sale con lo que
    // ocupa este: hace falta para saber cuántos espacios borran la cola de la línea
    // anterior cuando esta es más corta.
    void repinta(const std::string& indicador, const std::string& linea, std::size_t cursor,
                 std::size_t& anchoPintado);
    bool completa(std::string& linea, std::size_t& cursor);

    std::vector<std::string> m_historial;
    Completador m_completa;
};

}  // namespace zfsmgr::cli
