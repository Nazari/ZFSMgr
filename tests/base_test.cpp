// Test de la capa base. SIN Qt: este ejecutable se enlaza solo contra zfsmgr_base, y
// esa es justamente la comprobación que aporta. Por eso no usa QTest y trae su propio
// arnés de cuatro líneas.

#include "daemonpayload.h"
#include "connectionjson.h"
#include "storefiles.h"
#include "storewarnings.h"
#include "connectionprofile.h"
#include "helpers.h"
#include "json.h"
#include "process.h"
#include "refreshparse.h"
#include "secretcipher.h"
#include "strutil.h"
#include "zfsmurl.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
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


    // --- JSON
    {
        namespace J = zfsmgr::base::json;
        J::Value v;
        std::string err;
        comprobar(J::parse("{\"b\":1,\"a\":\"x\"}", v, &err), "analiza un objeto simple");
        comprobar(v.isObject(), "la raiz es objeto");
        igual(v["a"].toString(), "x", "lee una cadena por clave");
        comprobar(v["b"].toInt() == 1, "lee un entero");
        comprobar(v["noexiste"].isNull(), "una clave ausente da nulo, no revienta");
        // Las claves salen ORDENADAS, que es lo que hace QJsonObject y de lo que depende
        // que el fichero no cambie en cada guardado.
        igual(J::toCompact(v), "{\"a\":\"x\",\"b\":1}", "las claves salen ordenadas");

        // La rareza de Qt que hay que replicar: el array vacio NO es «[]».
        J::Value raiz;
        raiz.set("vacio", J::Value(J::Array{}));
        igual(J::toIndented(raiz), "{\n    \"vacio\": [\n    ]\n}\n",
              "un array vacio se escribe como Qt, no como []");

        // Enteros como enteros: si saliera 47653.0 el fichero dejaria de ser legible.
        J::Value puerto;
        puerto.set("port", J::Value(47653));
        igual(J::toCompact(puerto), "{\"port\":47653}", "un entero no lleva decimales");

        // Escapes de ida y vuelta.
        J::Value esc;
        comprobar(J::parse("{\"k\":\"a\\\"b\\\\c\\nd\\u00f1\"}", esc, &err), "analiza escapes");
        igual(esc["k"].toString(), "a\"b\\c\nd\xc3\xb1", "los escapes se decodifican, \\u incluido");
        igual(J::toCompact(esc), "{\"k\":\"a\\\"b\\\\c\\nd\xc3\xb1\"}",
              "al escribir, lo no ASCII va crudo en UTF-8 y los control escapados");

        // Lo que debe RECHAZAR: un fichero corrupto no puede dar una config a medias.
        for (const char* malo : {"{", "{\"a\":}", "[1,]", "{'a':1}", "{\"a\":01}",
                                 "{\"a\":1}sobra", "", "nul", "{\"a\":1,}"}) {
            J::Value x;
            comprobar(!J::parse(malo, x, &err), std::string("rechaza JSON invalido: ") + malo);
        }
        comprobar(J::parse("{}", v, &err) && v.isObject(), "el objeto vacio si es valido");
        comprobar(J::parse("[]", v, &err) && v.isArray(), "el array vacio si es valido");
    }


    // --- base64
    {
        igual(base64Encode(""), "", "base64 de vacio");
        igual(base64Encode("f"), "Zg==", "relleno de dos");
        igual(base64Encode("fo"), "Zm8=", "relleno de uno");
        igual(base64Encode("foo"), "Zm9v", "sin relleno");
        igual(base64Encode("\xff\xfe\xfd"), "//79", "bytes altos");
        std::string d;
        comprobar(base64Decode("Zm9vYmFy", d) && d == "foobar", "descodifica");
        comprobar(base64Decode("Zg==", d) && d == "f", "descodifica con relleno");
        comprobar(!base64Decode("Zm9v*", d), "rechaza un caracter fuera del alfabeto");
        // Ida y vuelta con todos los bytes posibles.
        std::string todos;
        for (int i = 0; i < 256; ++i) { todos.push_back(static_cast<char>(i)); }
        comprobar(base64Decode(base64Encode(todos), d) && d == todos,
                  "ida y vuelta con los 256 bytes");
    }

    // --- cifrado de los secretos de la configuracion
    {
        using C = zfsmgr::base::SecretCipher;
        comprobar(C::isEncrypted("encv1$a$b"), "reconoce el prefijo");
        comprobar(!C::isEncrypted("una contrasena normal"), "no confunde texto en claro");

        std::string enc, err;
        comprobar(C::encryptEncv1("rpq231", "maestra", enc, err), "cifra");
        comprobar(C::isEncrypted(enc), "lo cifrado lleva el prefijo");
        comprobar(!contains(enc, "rpq231"), "el texto en claro NO aparece en la salida");
        std::string dec;
        comprobar(C::decryptEncv1(enc, "maestra", dec, err) && dec == "rpq231", "descifra");

        // Con la clave equivocada debe FALLAR y no dejar salida: si devolviera basura,
        // acabaria enviandose como contrasena de sudo.
        std::string mal;
        comprobar(!C::decryptEncv1(enc, "otra", mal, err), "rechaza la clave equivocada");
        comprobar(mal.empty(), "y no deja nada en la salida");

        // Manipular un byte del token invalida la firma. Se comprueba ANTES de descifrar.
        std::string roto = enc;
        roto[roto.size() - 3] = (roto[roto.size() - 3] == 'a') ? 'b' : 'a';
        comprobar(!C::decryptEncv1(roto, "maestra", mal, err), "rechaza un token manipulado");

        // Clave maestra vacia: cifrar asi seria una falsa sensacion de proteccion.
        comprobar(!C::encryptEncv1("x", "", enc, err), "rechaza cifrar sin clave maestra");

        // Formato invalido, sin reventar.
        comprobar(!C::decryptEncv1("encv1$solodospartes", "m", mal, err), "rechaza formato invalido");
        comprobar(!C::decryptEncv1("", "m", mal, err), "rechaza la entrada vacia");
    }


    // --- traduccion entre ConnectionProfile y el JSON de disco
    {
        namespace CJ = zfsmgr::base::connjson;
        comprobar(CJ::ensurePort("SSH", 0) == 22, "sin puerto se asume el 22");
        comprobar(CJ::ensurePort("SSH", -1) == 22, "un puerto negativo tambien");
        comprobar(CJ::ensurePort("SSH", 2222) == 2222, "un puerto valido se respeta");

        zfsmgr::base::ConnectionProfile loc;
        loc.id = "local";
        comprobar(CJ::isLocalProfile(loc), "reconoce el perfil local por id");
        comprobar(CJ::shouldForceLocalSudo(loc), "el local en Unix va con sudo");
        loc.osType = "Windows 11";
        comprobar(!CJ::shouldForceLocalSudo(loc), "en Windows no, porque alli no hay sudo");
        loc.id = "otro"; loc.connType = "LOCAL"; loc.osType.clear();
        comprobar(CJ::isLocalProfile(loc), "tambien lo reconoce por conn_type");

        // PSRP: lo que se olvida es el puerto. 5986 es WinRM, y dejarlo convierte una
        // conexion rota en una conexion rota SIN explicacion.
        zfsmgr::base::ConnectionProfile ps;
        ps.connType = "psrp"; ps.port = 5986; ps.useSudo = true;
        comprobar(CJ::migratePsrpProfileToSsh(ps), "convierte PSRP sin distinguir caja");
        igual(ps.connType, "SSH", "pasa a SSH");
        igual(ps.osType, "Windows", "y a Windows");
        comprobar(ps.port == 22, "el puerto 5986 se corrige al 22");
        comprobar(!ps.useSudo, "y se quita el sudo");
        zfsmgr::base::ConnectionProfile ssh;
        ssh.connType = "SSH"; ssh.port = 2222;
        comprobar(!CJ::migratePsrpProfileToSsh(ssh) && ssh.port == 2222, "un SSH no se toca");

        // decodeHexAsciiIfUuid imita a QByteArray::fromHex: SALTA lo no hexadecimal.
        const std::string hexUuid =
            "34346162636465662d313233342d353637382d396162632d646566303132333435363738";
        igual(CJ::decodeHexAsciiIfUuid(hexUuid), "44abcdef-1234-5678-9abc-def012345678",
              "decodifica el hexadecimal ASCII de un UUID");
        igual(CJ::decodeHexAsciiIfUuid("no es hex"), "", "lo que no da un UUID devuelve vacio");
        igual(CJ::decodeHexAsciiIfUuid(""), "", "el vacio no revienta");

        // Ida y vuelta por JSON.
        zfsmgr::base::ConnectionProfile p;
        p.id = "  c1  "; p.name = "  Mi conexion  "; p.connType = "SSH"; p.osType = "Linux";
        p.host = "  unib.local  "; p.port = 2222; p.sshAddressFamily = "IPv6";
        p.username = "linarese"; p.password = "secreto"; p.keyPath = "  /k  ";
        p.useSudo = true; p.daemonTlsPort = 40000;
        const auto obj = CJ::connectionToJson(p, "uid-local");
        igual(obj["id"].toString(), "c1", "el id se recorta al guardar");
        igual(obj["host"].toString(), "unib.local", "y el host");
        igual(obj["ssh_address_family"].toString(), "ipv6", "la familia se normaliza a minusculas");
        igual(obj["password"].toString(), "secreto", "config.json SI lleva la contrasena");
        const auto tr = CJ::connectionTrustToJson(p, "uid-local");
        comprobar(!tr.contains("password"),
                  "el almacen de confianza NO lleva contrasena: es su razon de ser");
        comprobar(tr.contains("daemon_tls_client_key_pem"), "y si lleva el material TLS");

        const auto vuelta = CJ::connectionFromJson(obj, "uid-local");
        igual(vuelta.id, "c1", "ida y vuelta del id");
        igual(vuelta.host, "unib.local", "ida y vuelta del host");
        comprobar(vuelta.port == 2222, "ida y vuelta del puerto");
        igual(vuelta.sshAddressFamily, "ipv6", "ida y vuelta de la familia");

        // Una familia desconocida se guarda como «auto», no se propaga la basura.
        p.sshAddressFamily = "loquesea";
        igual(CJ::connectionToJson(p, "").
                  operator[]("ssh_address_family").toString(), "auto",
              "una familia desconocida se guarda como auto");

        // El puerto TLS fuera de rango vuelve al de siempre.
        zfsmgr::base::json::Value malo;
        malo.set("daemon_tls_port", zfsmgr::base::json::Value(999999));
        comprobar(CJ::connectionFromJson(malo, "").daemonTlsPort == 47653,
                  "un puerto TLS imposible vuelve al 47653");

        // upsert sustituye por id sin distinguir caja, no duplica.
        zfsmgr::base::json::Array arr;
        comprobar(CJ::upsertConnectionJson(arr, p, "") && arr.size() == 1, "inserta");
        p.id = "C1"; p.host = "otro";
        comprobar(CJ::upsertConnectionJson(arr, p, "") && arr.size() == 1,
                  "sustituye por id sin distinguir caja, no duplica");
        igual(arr[0]["host"].toString(), "otro", "y se queda el nuevo valor");
        zfsmgr::base::ConnectionProfile sinId;
        comprobar(!CJ::upsertConnectionJson(arr, sinId, ""), "sin id no inserta");
    }


    // --- ficheros del almacen y motivos tipificados
    {
        namespace ST = zfsmgr::base::store;
        const std::string dir = "/tmp/zfsmgr-base-test-store";
        std::filesystem::remove_all(dir);

        // Que el fichero NO exista es el primer arranque, no un aviso.
        ST::Aviso a;
        const auto vacio = ST::leerConfig(dir, a);
        comprobar(a.vacio(), "un config.json inexistente NO produce aviso");
        comprobar(vacio.isObject() && vacio.toObject().empty(), "y devuelve un objeto vacio");

        // Escribir crea el directorio y deja el fichero solo para el dueno.
        zfsmgr::base::json::Value root;
        root.set("app", zfsmgr::base::json::Value(zfsmgr::base::json::Object{}));
        comprobar(ST::escribirConfig(dir, root, a) && a.vacio(), "escribe config.json");
        comprobar(std::filesystem::exists(ST::rutaConfig(dir)), "el fichero esta ahi");
#ifndef _WIN32
        const auto permisos = std::filesystem::status(ST::rutaConfig(dir)).permissions();
        comprobar((permisos & std::filesystem::perms::group_all) == std::filesystem::perms::none
                      && (permisos & std::filesystem::perms::others_all) == std::filesystem::perms::none,
                  "config.json queda SOLO para el dueno");
#endif
        // Ida y vuelta.
        const auto leido = ST::leerConfig(dir, a);
        comprobar(a.vacio() && leido.contains("app"), "se relee lo escrito");

        // Un fichero corrupto tiene que dar motivo, no una configuracion a medias.
        {
            std::ofstream f(ST::rutaConfig(dir), std::ios::trunc);
            f << "{esto no es json";
        }
        const auto malo = ST::leerConfig(dir, a);
        comprobar(a.motivo == ST::Motivo::ConfigNoValido, "un config.json corrupto da motivo");
        comprobar(!a.detalle.empty(), "y explica por que");
        comprobar(malo.toObject().empty(), "sin devolver nada a medias");

        // El almacen de confianza usa sus propios motivos, no los de config.
        {
            std::ofstream f(ST::rutaTrustStore(dir), std::ios::trunc);
            f << "[1,2]";  // valido como JSON, pero no es un objeto
        }
        ST::leerTrustStore(dir, a);
        comprobar(a.motivo == ST::Motivo::TrustNoValido,
                  "el trust-store tiene motivo propio, no el de config");

        std::filesystem::remove_all(dir);
    }


    // --- analizadores de la respuesta al refrescar
    {
        namespace R = zfsmgr::base::refresh;
        igual(R::normalizeMachineUuid("  {ABCD-1234}  "), "abcd-1234",
              "quita las llaves del registro de Windows y baja la caja");
        igual(R::normalizeMachineUuid("{}"), "{}", "unas llaves solas no son un envoltorio");

        igual(R::extractMachineUuid("basura abcd1234-11d3-9d69-0008-c781f39f0000 mas"),
              "abcd1234-11d3-9d69-0008-c781f39f0000", "encuentra el UUID con guiones");
        igual(R::extractMachineUuid("0123456789ABCDEF0123456789abcdef"),
              "0123456789abcdef0123456789abcdef", "y el de 32 digitos seguidos");
        igual(R::extractMachineUuid("primera\nsegunda"), "primera",
              "sin UUID reconocible se queda con la primera linea");
        igual(R::extractMachineUuid("   "), "", "el vacio no inventa");

        {
            const auto kv = R::parseKeyValueOutput(
                "OS_LINE=Linux 6.1\n  version  =  0.92.0  \n=sinclave\nsuelta\nK=a=b\n");
            comprobar(kv.size() == 3, "solo las lineas con clave cuentan");
            comprobar(kv.at("OS_LINE") == "Linux 6.1", "lee el valor");
            comprobar(kv.at("VERSION") == "0.92.0", "la clave sube a mayusculas y se recorta");
            // El valor puede llevar un '=' dentro: solo parte por el PRIMERO.
            comprobar(kv.at("K") == "a=b", "el valor conserva los '=' que lleve dentro");
        }

        {
            const auto ps = R::parsePoolGuidStatusBatch(
                "__ZFSMGR_POOL__: p1\n__ZFSMGR_GUID__: 123\n__ZFSMGR_STATUS_BEGIN__\n"
                "  con sangria\n  otra\n__ZFSMGR_STATUS_END__\n"
                "__ZFSMGR_POOL__:p2\n__ZFSMGR_GUID__:9\n");
            comprobar(ps.size() == 2, "dos pools");
            comprobar(ps.at("p1").guid == "123", "el guid del primero");
            // La sangria INTERIOR se conserva: es parte de lo que ve el usuario en
            // `zpool status`. Solo se recorta el bloque entero por los extremos.
            comprobar(contains(ps.at("p1").status, "  otra"),
                      "la sangria de las lineas de estado se conserva");
            comprobar(ps.at("p2").status.empty(), "un pool sin bloque de estado queda vacio");
            comprobar(R::parsePoolGuidStatusBatch("sin marcadores\n").empty(),
                      "sin marcadores no hay pools");
            comprobar(R::parsePoolGuidStatusBatch("__ZFSMGR_GUID__:9\n").empty(),
                      "un guid sin pool se descarta");
        }

        comprobar(R::zfsmgrUnixCommandSet().size() == 6,
                  "la lista de herramientas es corta a proposito");
    }


    // --- ejecución de procesos con retroalimentación
    //
    // Estos casos NO son ceremonia: es la pieza que sustituye a QProcess, y sus fallos
    // —tuberías que se llenan, hijos que no mueren, líneas que no salen hasta el final—
    // no se ven leyendo el código. Solo se ven ejecutándolo.
    //
    // POSIX solamente: en Windows haría falta cmd.exe y otras rutas, y una prueba que
    // solo corre en una plataforma es peor que declararlo.
#ifndef _WIN32
    {
        auto comprobar2 = [](bool ok, const char* q) { comprobar(ok, q); };
    // 1) Captura básica y código de salida
        {
            StreamCallbacks cb;
            std::vector<std::string> lineas;
            cb.onStdoutLine = [&](const std::string& l) { lineas.push_back(l); };
            auto r = runExecStream("/bin/sh", {"-c", "echo uno; echo dos; exit 3"}, "", 5000, cb);
            comprobar2(r.rc == 3, "código de salida");
            comprobar2(lineas.size() == 2 && lineas[0] == "uno" && lineas[1] == "dos", "líneas por callback");
            comprobar2(r.out == "uno\ndos\n", "texto completo también");
        }
        // 2) Las líneas llegan MIENTRAS corre, no al final: es el punto de todo esto
        {
            StreamCallbacks cb;
            std::vector<int> momentos;
            const auto t0 = std::chrono::steady_clock::now();
            cb.onStdoutLine = [&](const std::string&) {
                momentos.push_back((int)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count());
            };
            auto r = runExecStream("/bin/sh", {"-c", "echo a; sleep 0.4; echo b"}, "", 5000, cb);
            comprobar2(r.rc == 0, "sale bien");
            comprobar2(momentos.size() == 2, "dos líneas");
            comprobar2(momentos.size() == 2 && momentos[0] < 200 && momentos[1] > 300,
                "la primera llega ANTES de terminar");
        }
        // 3) stderr por separado
        {
            StreamCallbacks cb;
            std::string o, e;
            cb.onStdoutLine = [&](const std::string& l) { o += l; };
            cb.onStderrLine = [&](const std::string& l) { e += l; };
            auto r = runExecStream("/bin/sh", {"-c", "echo salida; echo error >&2"}, "", 5000, cb);
            comprobar2(o == "salida" && e == "error", "stdout y stderr separados");
        }
        // 4) Entrada estándar: hay que CERRARLA o el otro espera para siempre
        {
            StreamCallbacks cb;
            std::string o;
            cb.onStdoutLine = [&](const std::string& l) { o += l; };
            auto r = runExecStream("/bin/cat", {}, "hola mundo\n", 5000, cb);
            comprobar2(r.rc == 0, "cat termina (la entrada se cierra)");
            comprobar2(o == "hola mundo", "cat devuelve lo que se le dio");
        }
        // 5) Entrada GRANDE: la tubería se llena y hay que alternar escritura y lectura
        {
            StreamCallbacks cb;
            const std::string grande(4 * 1024 * 1024, 'x');
            auto r = runExecStream("/bin/cat", {}, grande, 20000, cb);
            comprobar2(r.rc == 0, "4 MB por la entrada no bloquea");
            comprobar2(r.out.size() == grande.size(), "vuelven los 4 MB enteros");
        }
        // 6) Cancelación desde onTick
        {
            StreamCallbacks cb;
            cb.onTick = [](int ms) { return ms < 300; };
            const auto t0 = std::chrono::steady_clock::now();
            auto r = runExecStream("/bin/sh", {"-c", "sleep 30"}, "", 0, cb);
            const int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            comprobar2(r.rc == 130, "cancelado da 130");
            comprobar2(ms < 3000, "y corta de verdad, no espera los 30 s");
        }
        // 7) Tiempo límite
        {
            StreamCallbacks cb;
            auto r = runExecStream("/bin/sh", {"-c", "sleep 30"}, "", 400, cb);
            comprobar2(r.rc == 124, "el tiempo límite da 124");
        }
        // 8) timeoutMs=0 es SIN límite, no "cero milisegundos"
        {
            StreamCallbacks cb;
            auto r = runExecStream("/bin/sh", {"-c", "sleep 0.3; echo ya"}, "", 0, cb);
            comprobar2(r.rc == 0 && r.out == "ya\n", "0 = sin límite");
        }
        // 9) Programa inexistente
        {
            StreamCallbacks cb;
            auto r = runExecStream("/no/existe/programa", {}, "", 2000, cb);
            comprobar2(r.rc == 127, "programa inexistente da 127");
        }
        // 10) Retorno de carro como fin de línea (el progreso de zfs send)
        {
            StreamCallbacks cb;
            std::vector<std::string> l;
            cb.onStdoutLine = [&](const std::string& x) { l.push_back(x); };
            auto r = runExecStream("/bin/sh", {"-c", "printf 'a\\rb\\rc'"}, "", 5000, cb);
            comprobar2(l.size() == 3, "el retorno de carro corta línea (progreso de zfs send)");
        }
        // 11) Argumentos con caracteres que un shell interpretaría
        {
            StreamCallbacks cb;
            std::string o;
            cb.onStdoutLine = [&](const std::string& x) { o += x; };
            auto r = runExecStream("/bin/echo", {"a;rm -rf /", "b|c", "d$e"}, "", 5000, cb);
            comprobar2(o == "a;rm -rf / b|c d$e", "sin shell: los metacaracteres son literales");
        }
        // 12) Salida sin salto final
        {
            StreamCallbacks cb;
            std::vector<std::string> l;
            cb.onStdoutLine = [&](const std::string& x) { l.push_back(x); };
            auto r = runExecStream("/bin/printf", {"sin salto"}, "", 5000, cb);
            comprobar2(l.size() == 1 && l[0] == "sin salto", "la última línea sin salto se entrega");
        }
    }
#endif


    // --- zfsm://  nombrar cualquier elemento del arbol
    {
        namespace U = zfsmgr::base;
        U::ZfsmUrl u;
        std::string err;

        // Lo que nombra lo decide cuantos tramos hay, no la seccion.
        comprobar(U::parseZfsmUrl("zfsm://unibody", u, err) && u.kind == U::ZfsmKind::Conexion,
                  "solo conexion");
        igual(u.conexion, "unibody", "la conexion es la autoridad");
        comprobar(U::parseZfsmUrl("zfsm://unibody/sback", u, err) && u.kind == U::ZfsmKind::Pool,
                  "un tramo es un pool");
        igual(u.pool, "sback", "el primer tramo es el pool");
        comprobar(U::parseZfsmUrl("zfsm://unibody/sback/user", u, err)
                      && u.kind == U::ZfsmKind::Dataset,
                  "dos tramos son un dataset");
        // El nombre ZFS sale ya montado, que es como se le pasa a `zfs`.
        igual(u.dataset, "sback/user", "el dataset lleva el pool delante");
        igual(u.nombreZfs(), "sback/user", "nombreZfs sin snapshot");

        comprobar(U::parseZfsmUrl("zfsm://unibody/sback/user@ayer", u, err)
                      && u.kind == U::ZfsmKind::Snapshot,
                  "con @ es un snapshot");
        igual(u.snapshot, "ayer", "el snapshot va sin @");
        igual(u.dataset, "sback/user", "y el dataset no se lo lleva");
        igual(u.nombreZfs(), "sback/user@ayer", "nombreZfs como lo escribe ZFS");

        // El fragmento es el arbol: seccion y despues la ruta dentro.
        comprobar(U::parseZfsmUrl("zfsm://unibody/sback/user#propiedades/compression", u, err),
                  "una propiedad");
        igual(u.seccion, "propiedades", "la seccion");
        comprobar(u.detalle.size() == 1 && u.detalle[0] == "compression", "el detalle");
        comprobar(U::parseZfsmUrl("zfsm://unibody/sback/user#contenido/docs/a.pdf", u, err)
                      && u.detalle.size() == 2 && u.detalle[1] == "a.pdf",
                  "un fichero dentro del contenido");
        comprobar(U::parseZfsmUrl("zfsm://unibody#daemon", u, err)
                      && u.kind == U::ZfsmKind::Conexion && u.seccion == "daemon",
                  "una seccion de la conexion");
        // Una seccion desconocida NO se rechaza: el arbol puede ganar pestañas.
        comprobar(U::parseZfsmUrl("zfsm://unibody/sback/user#loquesea", u, err),
                  "una seccion desconocida se acepta");
        // La seccion no distingue mayusculas.
        comprobar(U::parseZfsmUrl("zfsm://u/p/d#PROPIEDADES", u, err) && u.seccion == "propiedades",
                  "la seccion se normaliza a minusculas");
        comprobar(U::parseZfsmUrl("ZFSM://u/p/d", u, err), "el esquema tampoco distingue caja");

        // Espacios: ZFS los admite en los nombres, asi que hay que codificarlos.
        comprobar(U::parseZfsmUrl("zfsm://unibody/sback/con%20espacio", u, err)
                      && u.dataset == "sback/con espacio",
                  "un nombre con espacio, codificado");
        igual(U::percentEncodeSegment("con espacio"), "con%20espacio", "y al reves");
        // Los tres separadores de esta sintaxis, dentro de un nombre.
        igual(U::percentEncodeSegment("a/b@c#d"), "a%2Fb%40c%23d", "los separadores se codifican");
        // Los legales de ZFS que NO hay que codificar: seria ruido inutil.
        igual(U::percentEncodeSegment("a-b_c.d:e"), "a-b_c.d:e", "los legales de ZFS van tal cual");

        // Ida y vuelta: es lo que impide que las dos mitades se separen con el tiempo.
        for (const char* caso : {"zfsm://unibody",
                                 "zfsm://unibody#daemon",
                                 "zfsm://unibody/sback",
                                 "zfsm://unibody/sback/user",
                                 "zfsm://unibody/sback/user@ayer",
                                 "zfsm://unibody/sback/user#propiedades/compression",
                                 "zfsm://unibody/sback/user#contenido/docs/a.pdf",
                                 "zfsm://local/winpool/sa@antes%20de%20migrar"}) {
            U::ZfsmUrl x;
            std::string e2;
            comprobar(U::parseZfsmUrl(caso, x, e2), std::string("analiza: ") + caso);
            igual(U::formatZfsmUrl(x), caso, std::string("ida y vuelta: ") + caso);
        }

        // Lo que debe RECHAZAR, y por que.
        struct { const char* url; const char* porque; } malas[] = {
            {"http://unibody/sback", "otro esquema"},
            {"zfsm://", "sin conexion"},
            {"zfsm:///sback", "conexion vacia"},
            {"zfsm://u/p/d@", "'@' sin nombre"},
            {"zfsm://u/p/d@a@b", "dos '@'"},
            {"zfsm://u/p@snap", "un snapshot necesita dataset, no solo pool"},
            {"zfsm://u/p/d@con/barra", "el snapshot no lleva '/'"},
            {"zfsm://u/p//d", "tramo vacio"},
            {"zfsm://u/p/d#", "'#' sin seccion"},
            {"zfsm://u/p/d%ZZ", "por-ciento invalido"},
            {"zfsm://u/p/d%4", "por-ciento a medias"},
        };
        for (const auto& m : malas) {
            U::ZfsmUrl x;
            std::string e2;
            comprobar(!U::parseZfsmUrl(m.url, x, e2),
                      std::string("rechaza (") + m.porque + "): " + m.url);
            comprobar(!e2.empty(), std::string("y explica por que: ") + m.url);
        }
    }

    std::fprintf(stderr, "%d pasados, %d fallos\n", pasados, fallos);
    return fallos == 0 ? 0 : 1;
}
