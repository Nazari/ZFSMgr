// Test de la capa base. SIN Qt: este ejecutable se enlaza solo contra zfsmgr_base, y
// esa es justamente la comprobación que aporta. Por eso no usa QTest y trae su propio
// arnés de cuatro líneas.

#include "daemonpayload.h"
#include "connectionprofile.h"
#include "helpers.h"
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

    // --- recortes por CARACTER, no por byte
    // El caso que lo destapo comparando contra Qt: cortar "aE" acentuado por bytes deja
    // UTF-8 invalido, y left() es lo que recorta las lineas del registro.
    {
        const std::string ae = "\xc3\xa1\xc3\x89";  // "áÉ" en UTF-8, 2 caracteres, 4 bytes
        igual(left(ae, 1), "\xc3\xa1", "left corta por caracteres, no por bytes");
        igual(left(ae, 3), ae, "left mas alla del final devuelve todo");
        igual(mid(ae, 1), "\xc3\x89", "mid cuenta caracteres");
        igual(mid(ae, 2), "", "mid en el final da vacio");
        igual(mid(ae, 5), "", "mid pasado el final no revienta");
        igual(mid("abcdef", 1, 3), "bcd", "mid con longitud");
        comprobar(byteOfChar(ae, 1) == 2, "byteOfChar salta el byte de continuacion");
    }

    // --- caja ASCII, y SOLO ASCII: es una divergencia buscada respecto a Qt
    igual(toLowerAscii("ABC"), "abc", "toLowerAscii en ASCII");
    igual(toUpperAscii("abc"), "ABC", "toUpperAscii en ASCII");
    igual(toLowerAscii("\xc3\x89"), "\xc3\x89", "toLowerAscii NO toca los acentos (a proposito)");

    // --- contains / startsWith / endsWith / indexOf
    comprobar(contains("abc", "b"), "contains encuentra");
    comprobar(!contains("abc", "z"), "contains no inventa");
    comprobar(startsWith("abc", "ab"), "startsWith");
    comprobar(!startsWith("a", "ab"), "startsWith no se sale del final");
    comprobar(endsWith("abc", "bc"), "endsWith");
    comprobar(indexOf("abc", "z") == -1, "indexOf devuelve -1 si no hay");
    comprobar(indexOf("abcb", "b") == 1, "indexOf da la primera");
    comprobar(lastIndexOf("abcb", "b") == 3, "lastIndexOf da la ultima");

    // --- simplify: colapsa y recorta, como QString::simplified()
    igual(simplify("  a  b  "), "a b", "simplify colapsa y recorta");
    igual(simplify("a\tb\nc"), "a b c", "simplify trata tabuladores y saltos");
    igual(simplify("   "), "", "simplify de solo espacios da vacio");

    // --- split / join
    igual(join(split("//a//b//", "/", true), "|"), "a|b", "split saltando vacios");
    igual(join(split("a/b", "/", false), "|"), "a|b", "split conservando vacios");
    igual(join(split("", "/", false), "|"), "", "split de vacio da un trozo vacio");
    igual(join({"a", "b"}, "-"), "a-b", "join");
    igual(join({}, "-"), "", "join de nada da vacio");


    // --- helpers portados a mano desde mwhelpers
    namespace H = zfsmgr::base::helpers;
    igual(H::parentDatasetName("pool/a/b"), "pool/a", "parentDatasetName sube un nivel");
    igual(H::parentDatasetName("pool"), "", "la raiz del pool no tiene padre");
    igual(H::parentDatasetName("/x"), "", "una barra al principio no cuenta como padre");
    comprobar(H::isMountedValueTrue(" YES "), "isMountedValueTrue recorta y no distingue caja");
    comprobar(!H::isMountedValueTrue("no"), "isMountedValueTrue rechaza el no");
    comprobar(H::isWindowsOsType("Microsoft Windows 11"), "isWindowsOsType reconoce");
    comprobar(!H::parentMountCheckRequired("none", "on"), "un padre en none no exige montaje");
    comprobar(!H::parentMountCheckRequired("/m", "off"), "canmount=off no exige montaje");
    comprobar(H::parentAllowsChildMount("none", "on", "no"), "si no se exige, se permite");
    comprobar(!H::parentAllowsChildMount("/m", "on", "no"), "padre exigido y sin montar: no");

    // El apostrofo es lo que rompe una orden si se cuela sin citar.
    igual(H::buildSingleMountCommand("po'ol/ds"), "zfs mount 'po'\"'\"'ol/ds'",
          "buildSingleMountCommand cita el apostrofo");
    comprobar(contains(H::buildHasMountedChildrenCommand(true, "a'b"), "$ds='a''b'"),
              "en PowerShell el apostrofo se duplica");
    comprobar(contains(H::buildHasMountedChildrenCommand(false, "x"), "DATASET='x'"),
              "en Unix se cita con comillas simples");

    igual(H::streamCodecName(H::StreamCodec::Zstd), "zstd-fast", "nombre del codec zstd");
    comprobar(H::chooseStreamCodec(true, true) == H::StreamCodec::Zstd, "zstd gana a gzip");
    comprobar(H::chooseStreamCodec(false, true) == H::StreamCodec::Gzip, "gzip si no hay zstd");
    comprobar(H::chooseStreamCodec(false, false) == H::StreamCodec::None, "sin codec si no hay ninguno");
    // El destino Unix repite %1 dos veces: si format solo sustituyera la primera, el
    // tar acabaria extrayendo en el directorio equivocado.
    comprobar(contains(H::buildTarDestinationCommand(false, "/m", H::StreamCodec::None),
                       "mkdir -p '/m' && tar --acls --xattrs -xpf - -C '/m'"),
              "buildTarDestinationCommand sustituye las DOS apariciones de %1");

    igual(H::stripToJson("basura {\"a\":1}"), "{\"a\":1}", "stripToJson descarta lo previo");
    igual(H::stripToJson("sin llave"), "sin llave", "stripToJson devuelve tal cual si no hay");
    igual(H::oneLine("  a   b  ", 220), "a b", "oneLine colapsa espacios");
    igual(H::oneLine("\xc3\xa1\xc3\x89", 1), "\xc3\xa1", "oneLine recorta por caracteres");


    // --- lo que se apoya en ConnectionProfile
    {
        zfsmgr::base::ConnectionProfile p;
        p.username = "linarese";
        p.host = "unib.local";
        igual(H::sshUserHost(p), "linarese@unib.local", "sshUserHost");
        igual(H::sshUserHostPort(p), "linarese@unib.local:22", "sin puerto se asume el 22");
        p.port = 2222;
        igual(H::sshUserHostPort(p), "linarese@unib.local:2222", "con puerto explicito");
        igual(H::sshAddressFamilyOption(p), "", "sin familia no se pasa opcion");
        p.sshAddressFamily = " IPv6 ";
        igual(H::sshAddressFamilyOption(p), "-6", "la familia se recorta y no distingue caja");
        p.sshAddressFamily = "raro";
        igual(H::sshAddressFamilyOption(p), "", "una familia desconocida no inventa opcion");

        // scp usa -P mayuscula, no -p. Confundirlas es un fallo silencioso: -p conserva
        // marcas de tiempo y el puerto se va al 22.
        p.sshAddressFamily.clear();
        const auto args = H::scpUploadArgs(p, "/l/a b", "/r/c", false);
        bool tieneMayus = false;
        for (const auto& a : args) {
            if (a == "-P") { tieneMayus = true; }
            comprobar(a != "-p", "scpUploadArgs NO usa -p minuscula");
        }
        comprobar(tieneMayus, "scpUploadArgs usa -P mayuscula para el puerto");
        comprobar(args.back() == "linarese@unib.local:/r/c", "el destino va al final");
        // Sin multiplexado no debe aparecer ControlPath.
        for (const auto& a : args) {
            comprobar(!contains(a, "ControlPath"), "sin multiplexado no hay ControlPath");
        }

        // sudo: sin contrasena va -n; con contrasena, la clave viaja en octal y NUNCA
        // en claro dentro de la orden.
        p.osType = "linux";
        p.useSudo = true;
        comprobar(contains(H::withSudoCommand(p, "zfs list"), "sudo -n sh -c"),
                  "sin contrasena se usa sudo -n");
        p.password = "rpq231";
        const std::string conPw = H::withSudoCommand(p, "zfs list");
        comprobar(!contains(conPw, "rpq231"), "la contrasena NO aparece en claro");
        comprobar(contains(conPw, H::shPrintfOctalEscaped("rpq231")), "va escapada en octal");
        // En Windows no hay sudo: la orden sale intacta.
        p.osType = "Windows 11";
        igual(H::withSudoCommand(p, "zfs list"), "zfs list", "en Windows no se envuelve en sudo");
        // Y el agente se invoca con el operador de llamada de PowerShell.
        comprobar(startsWith(H::agentCommand(p, "--health"), "& \""),
                  "en Windows el agente lleva el operador &");
    }

    igual(H::shPrintfOctalEscaped("A"), "\\0101", "escapado octal de un ASCII");
    igual(H::shPrintfOctalEscaped(""), "", "escapado octal de vacio");
    // Un caracter multibyte sale como VARIOS escapes, uno por byte.
    igual(H::shPrintfOctalEscaped("\xc3\xb1"), "\\0303\\0261", "escapado octal byte a byte");
    comprobar(contains(H::sshControlPath(), "%C"), "sshControlPath lleva el marcador %C");


    // --- caja UTF-8: el caso que obligo a implementarla
    // Con toLowerAscii esto devolvia false y un rechazo de contrasena se habria
    // clasificado como «no se pudo comprobar», sin avisar al usuario.
    comprobar(H::looksLikeSudoAuthFailure("SUDO: 1 INTENTO DE CONTRASE\xc3\x91""A INCORRECTO"),
              "detecta el rechazo aunque venga en mayusculas CON acento");
    comprobar(H::looksLikeSudoAuthFailure("FALLO DE AUTENTICACI\xc3\x93N"),
              "detecta el fallo de autenticacion en mayusculas");
    comprobar(H::looksLikeSudoAuthFailure("Sorry, try again."), "detecta el mensaje en ingles");
    // Autorizacion, NO autenticacion: volver a teclear la clave no arregla nada.
    comprobar(!H::looksLikeSudoAuthFailure("user is not in the sudoers file"),
              "no confunde sudoers con contrasena mala");
    comprobar(!H::looksLikeSudoAuthFailure("EL USUARIO NO EST\xc3\x81 EN EL FICHERO SUDOERS"),
              "tampoco en mayusculas con acento");
    comprobar(!H::looksLikeSudoAuthFailure(""), "el vacio no es un fallo");
    igual(toLowerUtf8("CONTRASE\xc3\x91""A"), "contrase\xc3\xb1""a", "toLowerUtf8 baja la enye");
    igual(toUpperUtf8("ni\xc3\xb1o"), "NI\xc3\x91O", "toUpperUtf8 sube la enye");

    // --- secretos: lo que NUNCA debe acabar en disco
    {
        std::vector<H::StorableSecret> ss{{"k1", "rpq231"}};
        bool ok = false;
        const std::string red = H::redactSecretsForStorage("echo rpq231 | sudo -S x", ss, &ok);
        comprobar(ok, "redactSecretsForStorage dice que pudo");
        comprobar(!contains(red, "rpq231"), "el secreto NO queda en el texto guardado");
        comprobar(contains(red, H::storedSecretMarkerPrefix()), "queda el marcador");
        igual(H::restoreSecretsFromStorage(red, ss),
              "echo " + H::shPrintfOctalEscaped("rpq231") + " | sudo -S x",
              "restoreSecretsFromStorage devuelve la forma octal");
        // Y la forma octal tambien se tapa, que es la que produce withSudoCommand.
        bool ok2 = false;
        const std::string red2 = H::redactSecretsForStorage(H::shPrintfOctalEscaped("rpq231"), ss, &ok2);
        comprobar(ok2 && !contains(red2, H::shPrintfOctalEscaped("rpq231")),
                  "tambien tapa el secreto escapado en octal");
    }

    // --- letras de unidad
    igual(H::normalizeDriveLetterValue("Z:\\"), "Z", "normaliza Z:\\");
    igual(H::normalizeDriveLetterValue(" y: "), "Y", "recorta y sube la caja");
    igual(H::normalizeDriveLetterValue("none"), "", "none no es letra");
    igual(H::normalizeDriveLetterValue("12"), "", "un digito no es letra");
    igual(H::normalizeDriveLetterValue(""), "", "el vacio no es letra");

    // --- particiones protegidas: ofrecer a ZFS el disco de arranque seria destructivo
    comprobar(H::windowsPartitionTypeIsProtected("type=System"), "la particion de sistema esta protegida");
    comprobar(H::windowsPartitionTypeIsProtected("type=RECOVERY"), "recuperacion, sin distinguir caja");
    comprobar(H::windowsPartitionTypeIsProtected("isBoot=True|type=Basic"), "el disco de arranque");
    comprobar(!H::windowsPartitionTypeIsProtected("type=Basic"), "una basica no esta protegida");
    comprobar(!H::windowsPartitionTypeIsProtected(""), "el vacio no protege nada");

    // --- troceo POSIX: el token vacio ESCRITO no es lo mismo que no haberlo
    {
        const auto v = H::posixShellSplitArgs("a '' b");
        comprobar(v.size() == 3 && v[1].empty(), "un '' escrito cuenta como argumento vacio");
        const auto v2 = H::posixShellSplitArgs("  ");
        comprobar(v2.empty(), "solo espacios no da ningun argumento");
    }

    // --- el secreto no debe llegar al registro
    {
        const std::string m = H::maskedAgentArgvForLog({"--mutate-zfs-load-key", "ds", "SECRETO"});
        comprobar(!contains(m, "SECRETO"), "maskedAgentArgvForLog tapa la passphrase");
        comprobar(contains(m, "[secret]"), "y deja el marcador");
    }


    // --- lo que ahora va con std::regex
    // Enmascarado: la orden real que construye withSudoCommand con la clave en octal.
    {
        const std::string oct = H::shPrintfOctalEscaped("rpq231");
        const std::string cmd = "printf '%b\\n' '" + oct + "' | sudo -k -S -p '' sh -c 'zfs list'";
        const std::string m = H::maskCommandSecrets(cmd);
        comprobar(!contains(m, oct), "el secreto en octal no sobrevive al enmascarado");
        comprobar(contains(m, "'[secret]'"), "queda el marcador");
        comprobar(contains(m, "sudo -k -S"), "el resto de la orden se conserva");
    }
    comprobar(!contains(H::maskCommandSecrets("printf '%s\\n' 'rpq231' | sudo -S x"), "rpq231"),
              "tambien tapa la forma literal");
    // La bandera icase sustituye al (?i) de PCRE, que std::regex no entiende.
    comprobar(contains(H::maskCommandSecrets("PASSWORD: hunter2"), "[secret]"),
              "password en mayusculas tambien se tapa (icase, no (?i))");
    comprobar(!contains(H::maskCommandSecrets("PASSWORD: hunter2"), "hunter2"),
              "y el valor no queda");
    // La anticipacion (?=...) SI existe en ECMAScript: sin ella este caso no casaria.
    comprobar(contains(H::maskCommandSecrets("{ printf '%s\\n' 'x'; cat; } | sudo y"), "[secret]"),
              "la forma con ; cat depende de la anticipacion");

    igual(H::parseOpenZfsVersionText("zfs-2.3.3"), "2.3.3", "version de zfs");
    igual(H::parseOpenZfsVersionText("OpenZFS version: 2.4.1"), "2.4.1", "version de openzfs");
    // Un mayor absurdo delata que se ha pescado otra cosa.
    igual(H::parseOpenZfsVersionText("zpool 99.0.0"), "", "un mayor por encima de 10 se descarta");
    igual(H::parseOpenZfsVersionText("nada"), "", "sin version no inventa");
    igual(H::parseOpenZfsVersionText(""), "", "el vacio no revienta");

    {
        const auto rows = H::parseZpoolImportOutput(
            "   pool: p1\n     id: 1\n  state: FAULTED\n status: metadata corrupta\n"
            "         y sigue\n action: x\n\n   pool: p2\n     id: 2\n  state: ONLINE\n");
        comprobar(rows.size() == 2, "dos pools");
        comprobar(rows[0].pool == "p1" && rows[0].state == "FAULTED", "el primero");
        comprobar(contains(rows[0].reason, "y sigue"), "el status continuado se concatena");
        comprobar(rows[1].pool == "p2" && rows[1].guid == "2", "el segundo");
        // Un nombre con espacio no es un nombre de pool valido y se descarta.
        comprobar(H::parseZpoolImportOutput("   pool: mal nombre\n  state: ONLINE\n").empty(),
                  "un nombre invalido se descarta");
    }

    std::fprintf(stderr, "%d pasados, %d fallos\n", pasados, fallos);
    return fallos == 0 ? 0 : 1;
}
