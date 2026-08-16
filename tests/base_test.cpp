// Test de la capa base. SIN Qt: este ejecutable se enlaza solo contra zfsmgr_base, y
// esa es justamente la comprobación que aporta. Por eso no usa QTest y trae su propio
// arnés de cuatro líneas.

#include "daemonpayload.h"
#include "strutil.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int fallos = 0;
int pasados = 0;

void comprobar(bool ok, const std::string& nombre, const std::string& detalle = {}) {
    if (ok) {
        ++pasados;
        return;
    }
    ++fallos;
    std::fprintf(stderr, "FALLO: %s\n", nombre.c_str());
    if (!detalle.empty()) {
        std::fprintf(stderr, "       %s\n", detalle.c_str());
    }
}

void igual(const std::string& obtenido, const std::string& esperado, const std::string& nombre) {
    comprobar(obtenido == esperado, nombre,
              "obtenido [" + obtenido + "] esperado [" + esperado + "]");
}

}  // namespace

int main() {
    using namespace zfsmgr::base;

    // --- trim
    igual(trim("  a b  "), "a b", "trim quita por los dos lados");
    igual(trim("\t\n x \r\n"), "x", "trim cuenta tabuladores y saltos");
    igual(trim("   "), "", "trim de solo espacios da vacio");
    igual(trim(""), "", "trim de vacio no revienta");

    // --- replaceAll
    {
        std::string s = "a.b.c";
        replaceAll(s, ".", "-");
        igual(s, "a-b-c", "replaceAll sustituye todas");
    }
    {
        // El caso que cuelga si se avanza mal: lo insertado contiene lo buscado.
        std::string s = "xx";
        replaceAll(s, "x", "xy");
        igual(s, "xyxy", "replaceAll no se repite sobre lo insertado");
    }
    {
        std::string s = "abc";
        replaceAll(s, "", "z");
        igual(s, "abc", "replaceAll con patron vacio no hace nada");
    }

    // --- format: la semantica que importa es UNA sola pasada
    igual(format("%1-%2", {"a", "b"}), "a-b", "format sustituye posicionalmente");
    igual(format("%2-%1", {"a", "b"}), "b-a", "format respeta el orden pedido");
    // Comprobado contra QString::arg(): un argumento que contiene %1 NO se re-sustituye.
    igual(format("A=%1 B=%2", {"%2", "z"}), "A=%2 B=z",
          "format no vuelve a mirar dentro de lo insertado");
    // Con trece argumentos conviven %1 y %10: hay que leer el numero mas largo.
    {
        std::vector<std::string> args;
        for (int i = 1; i <= 13; ++i) {
            args.push_back("<" + std::to_string(i) + ">");
        }
        igual(format("%1|%10|%13", args), "<1>|<10>|<13>", "format lee numeros de dos cifras");
    }
    igual(format("%9", {"a"}), "%9", "format deja literal lo que se sale de rango");
    igual(format("100%", {}), "100%", "format tolera un porcentaje suelto al final");
    igual(format("50%x", {"a"}), "50%x", "format tolera un porcentaje sin numero");

    // --- shSingleQuote
    igual(shSingleQuote("simple"), "'simple'", "shSingleQuote envuelve");
    igual(shSingleQuote("a'b"), "'a'\"'\"'b'", "shSingleQuote escapa la comilla simple");
    igual(shSingleQuote(""), "''", "shSingleQuote de vacio da dos comillas");

    // --- daemonpayload: rutas fijas, que son contrato con el agente instalado
    namespace dp = zfsmgr::base::daemonpayload;
    igual(dp::unixBinPath(), "/usr/local/libexec/zfsmgr-agent", "ruta del binario Unix");
    igual(dp::tlsClientKeyPath(), "/etc/zfsmgr/tls/client.key", "ruta de la clave de cliente");
    igual(dp::windowsBinPath(), "C:\\ProgramData\\ZFSMgr\\agent\\zfsmgr-agent.exe",
          "ruta del binario de Windows");

    // La version se recorta antes de incrustarla, y el marcador desaparece.
    {
        const std::string s = dp::unixStubScript("  0.92.0  ", "  v3  ");
        comprobar(s.find("# ZFSMgr Agent Version: 0.92.0\n") != std::string::npos,
                  "el guion Unix incrusta la version recortada");
        comprobar(s.find("__VERSION__") == std::string::npos,
                  "no queda ningun marcador __VERSION__ sin sustituir");
        comprobar(s.find("__API__") == std::string::npos,
                  "no queda ningun marcador __API__ sin sustituir");
    }

    // Y la configuracion cita para el shell, que es lo que impide que una version con
    // comilla se coma el resto del fichero.
    {
        const std::string s = dp::simpleConfigPayload("v'q", "v3");
        comprobar(s.find("VERSION='v'\"'\"'q'\n") != std::string::npos,
                  "simpleConfigPayload cita la comilla simple");
        comprobar(s.find("TLS_CLIENT_KEY='/etc/zfsmgr/tls/client.key'\n") != std::string::npos,
                  "simpleConfigPayload llega hasta el ultimo campo");
    }

    std::fprintf(stderr, "%d pasados, %d fallos\n", pasados, fallos);
    return fallos == 0 ? 0 : 1;
}
