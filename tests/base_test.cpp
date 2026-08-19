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
#include "agentversion.h"
#include "connectionjson.h"
#include "secretcipher.h"
#include "strutil.h"
#include "transportcmd.h"
#include "transportsession.h"
#include "transportrpc.h"
#include "transportsession.h"
#include "transporttunnel.h"
#include "zfsmurl.h"
#include "gsa.h"
#include "zfsprops.h"

#include <chrono>
#include <thread>
#ifndef _WIN32
#include <signal.h>
#endif
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

    // --- Enmascarado de la SALIDA, que es otra cosa que la orden.
    //
    // Existe porque la clave privada del cliente TLS se leia ejecutando una orden, y su
    // salida se volcaba entera al registro: con `zfsmgr_cli -v` salia por la salida de
    // error, de donde se copia y se pega.
    {
        const std::string clave =
            "-----BEGIN PRIVATE KEY-----\nMIIEvQIBADANBgkq\nhkiG9w0BAQEFAASC\n"
            "-----END PRIVATE KEY-----\n";
        const std::string m = H::maskSecretOutput("antes\n" + clave + "despues\n");
        comprobar(!contains(m, "MIIEvQIBADANBgkq"), "el cuerpo de la clave privada no sobrevive");
        comprobar(contains(m, "-----BEGIN PRIVATE KEY-----"), "las marcas se conservan");
        comprobar(contains(m, "antes") && contains(m, "despues"), "el resto de la salida queda");
    }
    // Truncada a mitad: sin marca de cierre se recorta HASTA EL FINAL. Media clave sigue
    // siendo media clave, y es justo lo que deja un volcado interrumpido.
    comprobar(!contains(H::maskSecretOutput("-----BEGIN RSA PRIVATE KEY-----\nMIIEvQIBADAN"),
                        "MIIEvQIBADAN"),
              "una clave sin cierre tambien se tapa");
    // El paquete del lector de material TLS: se conserva la marca CON su ruta, que es lo
    // que sirve para diagnosticar, y se tira lo de dentro.
    {
        const std::string paq = "__ZFSMGR_TLS_BEGIN__:/etc/zfsmgr/tls/client.key\n"
                                "MIIEvQIBADANBgkq\n"
                                "__ZFSMGR_TLS_END__:/etc/zfsmgr/tls/client.key\n";
        const std::string m = H::maskSecretOutput(paq);
        comprobar(!contains(m, "MIIEvQIBADANBgkq"), "el material TLS no sobrevive");
        comprobar(contains(m, "/etc/zfsmgr/tls/client.key"), "la ruta del fichero se conserva");
    }
    // Lo normal es que no haya nada que tapar, y entonces el texto sale INTACTO: por aqui
    // pasa toda la salida de todas las ordenes.
    igual(H::maskSecretOutput("NAME  SIZE\nfc16  2.46T\n"), "NAME  SIZE\nfc16  2.46T\n",
          "una salida normal no se toca");

    // --- Los avisos del transporte, que son PROSA y por eso no los redacta esta capa.
    //
    // Lo que se comprueba es que un aviso NO SE PIERDE cuando nadie ha conectado
    // traductor: sale con su etiqueta estable. Un aviso mudo por falta de traductor seria
    // peor que uno feo.
    {
        using zfsmgr::base::TransportSession;
        namespace BTr = zfsmgr::base::transport;
        TransportSession ses;
        std::string visto;
        ses.sink = [&visto](TransportSession::Nivel, const std::string&, const std::string& m) {
            visto = m;
        };
        ses.aviso(TransportSession::Nivel::Warn, "local", {BTr::Aviso::TlsLocalSinSudo, {}, {}});
        igual(visto, "tls-local-sin-sudo", "sin traductor sale la etiqueta, no el silencio");
        ses.aviso(TransportSession::Nivel::Warn, "local",
                  {BTr::Aviso::TunelNoAceptaEsperaAgotada, {}, "5000"});
        igual(visto, "tunel-espera-agotada: 5000", "y el detalle se conserva");
        ses.avisoSink = [&visto](TransportSession::Nivel, const std::string&,
                                 const BTr::NotaDeAviso& a) {
            visto = std::string("traducido:") + BTr::etiquetaDe(a.aviso);
        };
        ses.aviso(TransportSession::Nivel::Warn, "local", {BTr::Aviso::SinSshpass, {}, {}});
        igual(visto, "traducido:sin-sshpass", "con traductor puesto, manda el traductor");
    }
    // Y las decisiones tipificadas sobre los fallos: lo que antes se leia de una frase.
    {
        namespace BTr = zfsmgr::base::transport;
        comprobar(BTr::sugiereRevivirDaemon(BTr::Fallo::HandshakeFallido),
                  "un saludo TLS fallido invita a levantar el daemon");
        comprobar(!BTr::sugiereRevivirDaemon(BTr::Fallo::CertificadosInvalidos),
                  "unos certificados malos NO se arreglan levantando el daemon");
        comprobar(!BTr::mereceCastigo(BTr::Fallo::TunelOcupado),
                  "ocupado no es roto: no se castiga la conexion");
        comprobar(BTr::esDeTls(BTr::Fallo::CertificadoNoCoincide) &&
                      !BTr::esDeTls(BTr::Fallo::TunelNoSeMonta),
                  "de TLS es lo de TLS, no lo de red");
    }

    // --- El analizador DESESCAPA. Es la propiedad de la que depende la persistencia de
    // trabajos del daemon: guarda el texto escapado y lo vuelve a leer en cada arranque.
    //
    // Con un lector que no desescapa, ese ciclo no es idempotente: lo leido se vuelve a
    // escapar al guardar y las barras se DUPLICAN en cada vuelta. Medido con el lector
    // anterior: 1, 2, 4, 8 barras en cuatro vueltas. En maquinas reales se veian errores
    // como «pool or dataset is busy» seguidos de treinta barras.
    {
        namespace J = zfsmgr::base::json;
        J::Value v;
        std::string err;
        comprobar(J::parse("[{\"error\":\"linea1\\nlinea2\"}]", v, &err), "el array se analiza");
        const std::string leido = v.toArray().at(0)["error"].toString();
        igual(leido, "linea1\nlinea2", "el \\n vuelve a ser un salto de linea, no dos caracteres");
        // Y el ciclo completo: volver a serializar y analizar da lo MISMO.
        J::Value otra;
        comprobar(J::parse(J::toCompact(v), otra, &err), "lo serializado se vuelve a analizar");
        igual(otra.toArray().at(0)["error"].toString(), leido, "guardar y leer es idempotente");
    }

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


    // --- abrir secretos y fundir el almacen de confianza
    //
    // Estaban escritas dos veces, y NO hacian lo mismo: la interfaz dejaba ganar al
    // almacen y el interprete al perfil. Con material viejo todavia en config.json, una
    // usaba el fresco y la otra el rancio. Aqui se fija cual manda.
    {
        namespace CJ = zfsmgr::base::connjson;
        namespace ST = zfsmgr::base::store;
        namespace J = zfsmgr::base::json;
        using zfsmgr::base::SecretCipher;

        const std::string maestra = "la-maestra";
        std::string cifrado;
        std::string err;
        comprobar(SecretCipher::encryptEncv1("secreto", maestra, cifrado, err), "cj: se prepara un cifrado");

        zfsmgr::base::ConnectionProfile p;
        p.id = "unibody";
        p.name = "Unibody";
        p.password = cifrado;
        ST::Avisos avisos;
        comprobar(CJ::abreSecretos(p, maestra, avisos) && avisos.empty(), "cj: abre con la maestra buena");
        igual(p.password, "secreto", "cj: y deja el valor en claro");

        // Con la maestra equivocada el campo CONSERVA su cifrado, que es lo que impide que
        // alguien lo use creyendo que es el valor.
        zfsmgr::base::ConnectionProfile q;
        q.id = "unibody";
        q.name = "Unibody";
        q.password = cifrado;
        avisos.clear();
        comprobar(!CJ::abreSecretos(q, "otra", avisos), "cj: con la maestra mala dice que no");
        igual(q.password, cifrado, "cj: y NO deja el campo a medias");
        comprobar(avisos.size() == 1 && avisos.front().motivo == ST::Motivo::NoSeDescifra,
                  "cj: con su motivo tipificado");
        igual(avisos.front().campo, "password", "cj: diciendo que campo");
        igual(avisos.front().conexion, "Unibody", "cj: y de que conexion");

        // Sin maestra ninguna, el motivo es otro: falta la clave, no es que no descifre.
        zfsmgr::base::ConnectionProfile r;
        r.id = "x"; r.password = cifrado;
        avisos.clear();
        comprobar(!CJ::abreSecretos(r, "", avisos), "cj: sin maestra tampoco abre");
        comprobar(avisos.size() == 1 && avisos.front().motivo == ST::Motivo::ClaveMaestraRequerida,
                  "cj: y el motivo lo distingue");

        // La fusion: MANDA EL ALMACEN sobre lo que traiga el perfil.
        zfsmgr::base::ConnectionProfile viejo;
        viejo.id = "unibody";
        viejo.name = "Unibody";
        viejo.daemonTlsClientKeyPem = "clave-vieja-de-config";
        std::vector<zfsmgr::base::ConnectionProfile> perfiles{viejo};
        J::Value entrada;
        entrada.set("id", J::Value(std::string("unibody")));
        entrada.set("daemon_tls_client_key_pem", J::Value(std::string("clave-fresca-del-almacen")));
        entrada.set("daemon_tls_port", J::Value(12345));
        J::Value trust;
        trust.set("connections", J::Value(J::Array{entrada}));
        avisos.clear();
        CJ::fundeTrustStore(perfiles, trust, maestra, avisos);
        igual(perfiles.at(0).daemonTlsClientKeyPem, "clave-fresca-del-almacen",
              "cj: manda el almacen, que es donde se persiste lo negociado");
        comprobar(perfiles.at(0).daemonTlsPort == 12345, "cj: y su puerto");

        // Pero una entrada A MEDIAS no borra lo que el perfil si tenga.
        std::vector<zfsmgr::base::ConnectionProfile> perfiles2{viejo};
        J::Value vacia;
        vacia.set("id", J::Value(std::string("unibody")));
        J::Value trust2;
        trust2.set("connections", J::Value(J::Array{vacia}));
        CJ::fundeTrustStore(perfiles2, trust2, maestra, avisos);
        igual(perfiles2.at(0).daemonTlsClientKeyPem, "clave-vieja-de-config",
              "cj: una entrada vacia no borra lo que habia");

        // Una entrada del almacen SIN conexion que le corresponda se añade como conexion:
        // es material negociado con una maquina que sigue ahi, y tirarlo obliga a
        // renegociarlo por SSH. Lo hacia la interfaz y ahora lo hacen las dos.
        std::vector<zfsmgr::base::ConnectionProfile> ninguno;
        J::Value huerfana;
        huerfana.set("id", J::Value(std::string("fantasma")));
        huerfana.set("daemon_tls_client_key_pem", J::Value(std::string("k")));
        J::Value trust3;
        trust3.set("connections", J::Value(J::Array{huerfana}));
        CJ::fundeTrustStore(ninguno, trust3, maestra, avisos);
        comprobar(ninguno.size() == 1, "cj: una entrada huerfana se convierte en conexion");
        igual(ninguno.at(0).id, "fantasma", "cj: con su identificador");

        // --- El perfil «Local»: se sintetiza si no esta y se corrige si esta.
        std::vector<zfsmgr::base::ConnectionProfile> sinLocal;
        CJ::aseguraPerfilLocal(sinLocal, "uid-de-esta-maquina");
        comprobar(sinLocal.size() == 1, "local: se sintetiza cuando no hay ninguno");
        igual(sinLocal.at(0).id, "local", "local: con el identificador reservado");
        igual(sinLocal.at(0).connType, "LOCAL", "local: y su tipo");
        igual(sinLocal.at(0).machineUid, "uid-de-esta-maquina", "local: con el uid que le dan");
        comprobar(!sinLocal.at(0).osType.empty(), "local: y el sistema de ESTA maquina");
        comprobar(!sinLocal.at(0).username.empty(), "local: nunca sin usuario");

        // Va DELANTE: es la maquina de uno, y aparecer la tercera en la lista no ayuda.
        std::vector<zfsmgr::base::ConnectionProfile> conOtra;
        zfsmgr::base::ConnectionProfile remota;
        remota.id = "unibody"; remota.name = "Unibody"; remota.connType = "SSH";
        conOtra.push_back(remota);
        CJ::aseguraPerfilLocal(conOtra, "uid");
        igual(conOtra.at(0).id, "local", "local: se pone la primera");
        comprobar(conOtra.size() == 2, "local: sin tocar las demas");

        // Y si YA esta, se le corrigen los datos de la maquina en vez de duplicarlo: un
        // perfil copiado de otro equipo trae el uid y el sistema del sitio equivocado.
        std::vector<zfsmgr::base::ConnectionProfile> conLocalRancio;
        zfsmgr::base::ConnectionProfile viejoLocal;
        viejoLocal.id = "local"; viejoLocal.name = "Local"; viejoLocal.connType = "LOCAL";
        viejoLocal.machineUid = "uid-de-otro-equipo";
        viejoLocal.osType = "Windows";
        conLocalRancio.push_back(viejoLocal);
        CJ::aseguraPerfilLocal(conLocalRancio, "uid-de-esta");
        comprobar(conLocalRancio.size() == 1, "local: no se duplica el que ya hay");
        igual(conLocalRancio.at(0).machineUid, "uid-de-esta", "local: con el uid corregido");
        comprobar(conLocalRancio.at(0).osType != "Windows" || sinLocal.at(0).osType == "Windows",
                  "local: y el sistema corregido al de esta maquina");
    }

    // --- versiones del agente
    //
    // Estaban solo en la interfaz, y el interprete comparaba con `!=`: un agente MAS NUEVO
    // que el cliente salia marcado igual que uno anticuado. Aqui se fija el orden.
    {
        namespace AV = zfsmgr::base::agentversion;
        comprobar(AV::compara("0.93.1.598479612", "0.93.1.598479612") == 0, "av: iguales");
        comprobar(AV::compara("0.92.0.598479612", "0.93.1.598479612") < 0, "av: 0.92 va antes que 0.93");
        comprobar(AV::compara("0.93.2.100000000", "0.93.1.999999999") > 0,
                  "av: manda el parche, no el sufijo de esquema");
        comprobar(AV::compara("0.93.0.111111111", "0.93.0.222222222") < 0,
                  "av: a igual version, ordena el sufijo");
        // Un candidato va ANTES que su final, que es lo que uno espera y lo contrario de
        // lo que sale al comparar como texto («0.93.0rc1» > «0.93.0» alfabeticamente).
        comprobar(AV::compara("0.93.0rc1", "0.93.0") < 0, "av: un rc va antes que su final");
        comprobar(AV::compara("0.93.0rc1", "0.93.0rc2") < 0, "av: y entre rc, por numero");
        // Lo que no tiene forma de version se compara como texto: no es correcto, pero es
        // predecible, y no debe hacer creer que dos cosas raras son iguales.
        comprobar(AV::compara("no-es-version", "tampoco") != 0, "av: lo informe no se declara igual");
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

        // --- Rotar la clave maestra.
        //
        // Lo que importa no es que la orden diga que si, sino que DESPUES abra todo con la
        // nueva y nada con la vieja. Un fallo a medias aqui deja la configuracion medio
        // cifrada, y eso no se nota hasta el arranque siguiente.
        {
            const std::string dirRot = "/tmp/zfsmgr-base-test-rotar";
            std::filesystem::remove_all(dirRot);
            namespace J = zfsmgr::base::json;
            using zfsmgr::base::SecretCipher;

            const std::string vieja = "clave-vieja";
            const std::string nueva = "clave-nueva";
            std::string pwCifrada;
            std::string keyCifrada;
            std::string err;
            comprobar(SecretCipher::encryptEncv1("secreto-de-la-conexion", vieja, pwCifrada, err),
                      "rotar: se prepara una contrasena cifrada con la vieja");
            comprobar(SecretCipher::encryptEncv1("-----BEGIN KEY-----", vieja, keyCifrada, err),
                      "rotar: y una clave TLS cifrada con la vieja");

            J::Value conexion;
            conexion.set("id", J::Value(std::string("unibody")));
            conexion.set("name", J::Value(std::string("Unibody")));
            conexion.set("password", J::Value(pwCifrada));
            conexion.set("daemon_tls_client_key_pem", J::Value(keyCifrada));
            J::Value cfg;
            cfg.set("connections", J::Value(J::Array{conexion}));
            ST::Aviso av;
            comprobar(ST::escribirConfig(dirRot, cfg, av), "rotar: config de partida");
            J::Value trustConn;
            trustConn.set("id", J::Value(std::string("unibody")));
            trustConn.set("daemon_tls_client_key_pem", J::Value(keyCifrada));
            J::Value trust;
            trust.set("connections", J::Value(J::Array{trustConn}));
            comprobar(ST::escribirTrustStore(dirRot, trust, av), "rotar: trust-store de partida");

            std::string copia;
            comprobar(ST::rotaClaveMaestra(dirRot, vieja, nueva, copia, av) && av.vacio(),
                      "rotar: la rotacion va bien");
            igual(copia, ".antes-de-rotar", "rotar: dice con que sufijo dejo la copia");
            comprobar(std::filesystem::exists(ST::rutaConfig(dirRot) + copia),
                      "rotar: la copia de config.json esta ahi");
            comprobar(std::filesystem::exists(ST::rutaTrustStore(dirRot) + copia),
                      "rotar: y la del trust-store");

            const auto cfgTras = ST::leerConfig(dirRot, av);
            const J::Value& c0 = cfgTras["connections"].toArray().at(0);
            std::string claro;
            comprobar(SecretCipher::decryptEncv1(c0["password"].toString(), nueva, claro, err),
                      "rotar: la contrasena abre con la NUEVA");
            igual(claro, "secreto-de-la-conexion", "rotar: y es la de antes, no otra");
            comprobar(!SecretCipher::decryptEncv1(c0["password"].toString(), vieja, claro, err),
                      "rotar: y ya NO abre con la vieja");
            comprobar(SecretCipher::decryptEncv1(c0["daemon_tls_client_key_pem"].toString(), nueva, claro, err),
                      "rotar: el material TLS de config tambien se rotó");
            const auto trustTras = ST::leerTrustStore(dirRot, av);
            const J::Value& t0 = trustTras["connections"].toArray().at(0);
            comprobar(SecretCipher::decryptEncv1(t0["daemon_tls_client_key_pem"].toString(), nueva, claro, err),
                      "rotar: y el del almacen de confianza, que es el que se olvida");
            igual(claro, "-----BEGIN KEY-----", "rotar: intacto");

            // La maestra se comprueba contra TODO lo cifrado, no contra el primer campo.
            //
            // El caso que lo obliga: una configuracion A MEDIO ROTAR, con unos campos en la
            // clave nueva y otros en la vieja. Mirando solo el primero, la clave nueva
            // parece buena y el programa arranca con la mitad de los secretos cerrados.
            comprobar(ST::hayAlgoCifrado(dirRot), "maestra: detecta que hay campos cifrados");
            ST::Aviso avAbre;
            comprobar(ST::maestraAbreTodo(dirRot, nueva, avAbre) && avAbre.vacio(),
                      "maestra: la nueva abre todo lo que hay");
            comprobar(!ST::maestraAbreTodo(dirRot, vieja, avAbre),
                      "maestra: la vieja ya no");
            comprobar(avAbre.motivo == ST::Motivo::NoSeDescifra, "maestra: con su motivo");
            {
                // Se ensucia UN campo del trust-store con la clave vieja: el primero de
                // config sigue abriendo con la nueva, asi que solo recorriendolo todo se ve.
                std::string aMedias;
                std::string errM;
                comprobar(SecretCipher::encryptEncv1("x", vieja, aMedias, errM), "maestra: se prepara el medio rotado");
                J::Value tr = ST::leerTrustStore(dirRot, av);
                J::Array conns;
                for (const J::Value& c : tr["connections"].toArray()) {
                    J::Value copia = c;
                    copia.set("daemon_tls_server_cert_pem", J::Value(aMedias));
                    conns.push_back(copia);
                }
                tr.set("connections", J::Value(conns));
                comprobar(ST::escribirTrustStore(dirRot, tr, av), "maestra: se escribe a medias");
                ST::Aviso avMedio;
                comprobar(!ST::maestraAbreTodo(dirRot, nueva, avMedio),
                          "maestra: una configuracion a medio rotar NO se da por buena");
                igual(avMedio.campo, "daemon_tls_server_cert_pem", "maestra: y dice que campo");
            }

            // Una maestra nueva vacia se rechaza ANTES de tocar nada.
            std::string copia2;
            comprobar(!ST::rotaClaveMaestra(dirRot, nueva, "", copia2, av),
                      "rotar: una clave nueva vacia se rechaza");
            comprobar(av.motivo == ST::Motivo::NuevaClaveMaestraVacia, "rotar: y con su motivo");
            comprobar(copia2.empty(), "rotar: sin dejar copia de nada");

            // Con la clave vieja EQUIVOCADA no se puede descifrar, y hay que decirlo.
            std::string copia3;
            comprobar(!ST::rotaClaveMaestra(dirRot, "la-que-no-es", "otra", copia3, av),
                      "rotar: con la clave actual equivocada NO se rota");
            comprobar(av.motivo == ST::Motivo::NoSeDescifra, "rotar: y el motivo es que no descifra");
            igual(av.conexion, "Unibody", "rotar: diciendo en qué conexión");
            std::filesystem::remove_all(dirRot);
        }

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
        comprobar(U::parseZfsmUrl("zfsm://unibody", u, err) && u.kind == U::ZfsmKind::Connection,
                  "solo conexion");
        igual(u.connection, "unibody", "la conexion es la autoridad");
        // Un pool ES un dataset en ZFS: `zfs list sback` lo devuelve. Por eso no hay
        // clase aparte, y por eso puede tener snapshots.
        comprobar(U::parseZfsmUrl("zfsm://unibody/sback", u, err) && u.kind == U::ZfsmKind::Dataset,
                  "un tramo tambien es un dataset");
        comprobar(u.isPoolRoot(), "y ademas es la raiz de su pool");
        igual(u.pool, "sback", "el primer tramo es el pool");
        comprobar(U::parseZfsmUrl("zfsm://unibody/sback/user", u, err)
                      && u.kind == U::ZfsmKind::Dataset && !u.isPoolRoot(),
                  "dos tramos son un dataset que NO es la raiz");
        // El nombre ZFS sale ya montado, que es como se le pasa a `zfs`.
        igual(u.dataset, "sback/user", "el dataset lleva el pool delante");
        igual(u.zfsName(), "sback/user", "nombreZfs sin snapshot");

        comprobar(U::parseZfsmUrl("zfsm://unibody/sback/user@ayer", u, err)
                      && u.kind == U::ZfsmKind::Snapshot,
                  "con @ es un snapshot");
        igual(u.snapshot, "ayer", "el snapshot va sin @");
        igual(u.dataset, "sback/user", "y el dataset no se lo lleva");
        igual(u.zfsName(), "sback/user@ayer", "nombreZfs como lo escribe ZFS");

        // El fragmento es el arbol: seccion y despues la ruta dentro.
        comprobar(U::parseZfsmUrl("zfsm://unibody/sback/user#properties/compression", u, err),
                  "una propiedad");
        igual(u.section, "properties", "la seccion, en ingles");
        comprobar(u.detail.size() == 1 && u.detail[0] == "compression", "el detalle");
        comprobar(U::parseZfsmUrl("zfsm://unibody/sback/user#content/docs/a.pdf", u, err)
                      && u.detail.size() == 2 && u.detail[1] == "a.pdf",
                  "un fichero dentro del contenido");
        comprobar(U::parseZfsmUrl("zfsm://unibody#daemon", u, err)
                      && u.kind == U::ZfsmKind::Connection && u.section == "daemon",
                  "una seccion de la conexion");
        // Una seccion desconocida NO se rechaza: el arbol puede ganar pestañas.
        comprobar(U::parseZfsmUrl("zfsm://unibody/sback/user#loquesea", u, err),
                  "una seccion desconocida se acepta");
        // La seccion no distingue mayusculas.
        comprobar(U::parseZfsmUrl("zfsm://u/p/d#PROPERTIES", u, err) && u.section == "properties",
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

        // El caso que el modelo anterior rechazaba por error: un pool ES un dataset, así
        // que puede tener snapshots. Variable propia para no pisar la de arriba.
        {
            U::ZfsmUrl pu;
            std::string pe;
            comprobar(U::parseZfsmUrl("zfsm://OldLau/winpool@snap1", pu, pe), "snapshot de un POOL");
            comprobar(pu.kind == U::ZfsmKind::Snapshot, "y es de clase snapshot");
            comprobar(pu.isPoolRoot(), "sobre la raiz del pool");
            igual(pu.zfsName(), "winpool@snap1", "nombreZfs del snapshot de un pool");
        }

        // Ida y vuelta: es lo que impide que las dos mitades se separen con el tiempo.
        for (const char* caso : {"zfsm://unibody",
                                 "zfsm://unibody#daemon",
                                 "zfsm://unibody/sback",
                                 "zfsm://unibody/sback/user",
                                 "zfsm://unibody/sback/user@ayer",
                                 "zfsm://unibody/sback/user#properties/compression",
                                 "zfsm://unibody/sback/user#content/docs/a.pdf",
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

    // --- transportcmd: la parte del transporte que decide y analiza texto.
    //
    // Estos valores esperados NO estan inventados: salen de contrastar la traduccion
    // contra la version con Qt sobre 9.279 casos, con control negativo para comprobar
    // que el contraste sabe fallar. Se fijan aqui para que no se pierdan cuando el
    // arnes de contraste se tire.
    {
        namespace T = zfsmgr::base::transport;

        // Que clase de maquina hay al otro lado.
        zfsmgr::base::ConnectionProfile win;
        win.osType = "Windows 11";
        win.connType = "SSH";
        zfsmgr::base::ConnectionProfile lin;
        lin.osType = "Linux";
        lin.connType = "local";  // en minuscula: la comparacion no distingue mayusculas
        comprobar(T::isWindowsConnection(win), "isWindowsConnection: Windows 11");
        comprobar(!T::isWindowsConnection(lin), "isWindowsConnection: Linux no");
        comprobar(T::isLocalConnection(lin), "isLocalConnection: 'local' en minuscula");
        comprobar(!T::isLocalConnection(win), "isLocalConnection: SSH no");

        // La clave sale de las COORDENADAS, no de la posicion en la lista.
        zfsmgr::base::ConnectionProfile k;
        k.username = "  Eladio  ";
        k.host = " OldLau.LOCAL ";
        k.port = 0;
        k.keyPath = " /k/id_rsa ";
        igual(T::remoteDaemonTlsCacheKey(k), "eladio|oldlau.local|22|/k/id_rsa",
              "remoteDaemonTlsCacheKey: recorta, baja a minusculas y pone el 22 por omision");

        // Que ordenes cambian el estado del otro lado. Es la funcion de la que depende
        // que una mutacion que pudo llegar NO se reenvie.
        struct { const char* verbo; bool muta; } verbos[] = {
            {"--mutate-zfs-destroy", true}, {"--zfs-pipe-local", true},
            {"--zfs-send-to-peer", true},   {"--zfs-recv-listen", true},
            {"--repair-alt-mountpoints", true},
            {"--job-submit", true},         {"--job-cancel", true},
            {"--dump-zfs-list-all", false}, {"--health", false},
            {"--heartbeat", false},         {"--job-status", false},
        };
        for (const auto& v : verbos) {
            comprobar(T::isMutatingAgentCommand({v.verbo}) == v.muta,
                      std::string("isMutatingAgentCommand: ") + v.verbo);
        }
        comprobar(!T::isMutatingAgentCommand({}), "isMutatingAgentCommand: vacio no muta");

        // El corte por separador respeta las comillas. Un directorio llamado
        // «Copias & Backups» truncaba la orden, y con --mutate-advanced-todir ese
        // directorio lo elige el usuario: se perdian el destino y el indicador de borrar.
        {
            std::vector<std::string> a;
            const std::string cmd = daemonpayload::unixBinPath()
                                    + " --mutate-advanced-todir '/tmp/Copias & Backups' 1";
            comprobar(T::extractLocalAgentArgs(cmd, a), "extractLocalAgentArgs: reconoce la ruta Unix");
            comprobar(a.size() == 3, "extractLocalAgentArgs: tres argumentos",
                      "obtenidos " + std::to_string(a.size()));
            if (a.size() == 3) {
                igual(a[1], "/tmp/Copias & Backups",
                      "extractLocalAgentArgs: el '&' entrecomillado NO corta");
                igual(a[2], "1", "extractLocalAgentArgs: no se pierde el ultimo argumento");
            }
        }
        {
            // Y sin comillas si corta, que es lo que evita ejecutar lo que venga detras.
            std::vector<std::string> a;
            comprobar(T::extractLocalAgentArgs(daemonpayload::unixBinPath() + " --dump-x & echo y", a),
                      "extractLocalAgentArgs: corta en el '&' suelto");
            comprobar(a.size() == 1 && a[0] == "--dump-x",
                      "extractLocalAgentArgs: se queda con el verbo y tira el resto",
                      "argumentos: " + std::to_string(a.size()));
        }
        {
            // La ruta de WINDOWS tambien. Buscar solo la Unix es lo que dejaba a Windows
            // fuera del RPC: el comando se iba por SSH en crudo, sin el mTLS del tunel.
            std::vector<std::string> a;
            comprobar(T::extractLocalAgentArgs("& \"" + daemonpayload::windowsBinPath() + "\" --health", a),
                      "extractLocalAgentArgs: reconoce la ruta Windows");
            comprobar(a.size() == 1 && a[0] == "--health", "extractLocalAgentArgs: --health");
        }
        {
            // Los verbos que el daemon NO sirve por RPC se quedan fuera a proposito.
            std::vector<std::string> a;
            comprobar(!T::extractLocalAgentArgs(
                          daemonpayload::unixBinPath() + " --mutate-shell-generic 'zfs list'", a),
                      "extractLocalAgentArgs: --mutate-shell-generic no se desvia al RPC");
            comprobar(!T::extractLocalAgentArgs("zfs list -H -p", a),
                      "extractLocalAgentArgs: sin marcador, no es del agente");
        }

        // El volcado TLS que llega por SSH.
        {
            T::RemoteTlsBundle b;
            const std::string txt =
                "__ZFSMGR_AGENT_PORT__:47700\n"
                "__ZFSMGR_TLS_BEGIN__:/etc/zfsmgr/tls/server.crt\n  AAA  \n"
                "__ZFSMGR_TLS_END__:/etc/zfsmgr/tls/server.crt\n"
                "__ZFSMGR_TLS_BEGIN__:/etc/zfsmgr/tls/client.crt\nBBB\n"
                "__ZFSMGR_TLS_END__:/etc/zfsmgr/tls/client.crt\n";
            comprobar(T::parseRemoteDaemonTlsBundle(txt, b), "parseRemoteDaemonTlsBundle: server+client bastan");
            igual(b.serverCertPem, "AAA\n", "bundle: el contenido se recorta y acaba en salto");
            igual(b.clientCertPem, "BBB\n", "bundle: certificado de cliente");
            comprobar(b.port == 47700, "bundle: el puerto anunciado");
            comprobar(!b.clientKeyIncluded, "bundle: sin clave privada, y se dice");
            // Sin certificado de cliente no hay conversacion posible.
            T::RemoteTlsBundle b2;
            comprobar(!T::parseRemoteDaemonTlsBundle(
                          "__ZFSMGR_TLS_BEGIN__:/x/server.crt\nA\n__ZFSMGR_TLS_END__:/x/server.crt\n", b2),
                      "parseRemoteDaemonTlsBundle: falta el del cliente");
            // Un END que no casa con su BEGIN no guarda nada.
            T::RemoteTlsBundle b3;
            comprobar(!T::parseRemoteDaemonTlsBundle(
                          "__ZFSMGR_TLS_BEGIN__:/x/server.crt\nA\n__ZFSMGR_TLS_END__:/otro\n", b3),
                      "parseRemoteDaemonTlsBundle: el END tiene que casar");
        }

        // agent.conf: lo que no aparezca conserva su valor por omision.
        {
            const auto c = T::parseLocalAgentConfig("# nota\n\nPORT = 47700 \nBIND=  \"::1\"  \n"
                                                    "TLS_CERT='/a/b.crt'\nsinigual\n=novale\n");
            igual(c.bindAddress, "::1", "agent.conf: se quitan las comillas y los espacios");
            comprobar(c.port == 47700, "agent.conf: PORT vale igual que AGENT_PORT");
            igual(c.tlsCertPath, "/a/b.crt", "agent.conf: TLS_CERT");
            igual(c.tlsClientKeyPath, T::defaultAgentTlsClientKeyPath(),
                  "agent.conf: lo ausente conserva su valor por omision");
            // Un puerto fuera de rango o con basura detras NO se acepta: medio leido es
            // peor que ninguno.
            comprobar(T::parseLocalAgentConfig("PORT=70000\n").port == 47653, "agent.conf: 70000 no vale");
            comprobar(T::parseLocalAgentConfig("PORT=12x\n").port == 47653, "agent.conf: '12x' no vale");
        }

        // El ruido con forma de XML que escupe PowerShell por la salida de error.
        igual(T::sanitizeWindowsCliXml("#< CLIXML\n<Objs Version=\"1.1\"><S>x</S></Objs>"), "",
              "sanitizeWindowsCliXml: solo preambulo");
        igual(T::sanitizeWindowsCliXml("salida util\n#< CLIXML<objs version=\"1\">basura"), "salida util",
              "sanitizeWindowsCliXml: sin distinguir mayusculas, y conserva lo util");
        igual(T::sanitizeWindowsCliXml("hola"), "hola", "sanitizeWindowsCliXml: sin CLIXML no toca nada");

        // Reintentar sin multiplexado SOLO ante lo que delata que el socket de control no
        // sirve; ante cualquier otro fallo seria esconder el problema.
        comprobar(T::shouldRetrySshWithoutMultiplexing("mux_client: getsockname failed"),
                  "shouldRetrySsh: getsockname failed");
        comprobar(T::shouldRetrySshWithoutMultiplexing("NOT A SOCKET"), "shouldRetrySsh: en mayusculas");
        comprobar(!T::shouldRetrySshWithoutMultiplexing("Permission denied (publickey)."),
                  "shouldRetrySsh: una clave rechazada NO se reintenta");

        // wrapRemoteCommand. En una conexion que no es Windows no toca NADA.
        igual(T::wrapRemoteCommand(lin, "zfs list -H -p"), "zfs list -H -p",
              "wrapRemoteCommand: fuera de Windows pasa tal cual");
        {
            // En Windows va en base64 de UTF-16LE, que es lo que come -EncodedCommand.
            const std::string w = T::wrapRemoteCommand(win, "zfs list");
            comprobar(zfsmgr::base::startsWith(w, "powershell -NoProfile -NonInteractive -EncodedCommand "),
                      "wrapRemoteCommand: -EncodedCommand", w.substr(0, 60));
            std::string crudo;
            comprobar(zfsmgr::base::base64Decode(w.substr(w.rfind(' ') + 1), crudo),
                      "wrapRemoteCommand: el base64 se descodifica");
            // UTF-16LE: cada caracter ASCII deja un byte nulo detras.
            comprobar(crudo.size() % 2 == 0 && crudo.size() > 2 && crudo[1] == '\0',
                      "wrapRemoteCommand: es UTF-16LE, no UTF-8");
            comprobar(zfsmgr::base::endsWith(crudo, std::string("z\0f\0s\0 \0l\0i\0s\0t\0", 16)),
                      "wrapRemoteCommand: la orden va al final del prologo");
        }
        {
            // El prologo Unix «PATH="..."; export PATH; » se quita: en PowerShell es un
            // error de sintaxis, y el prologo de PowerShell ya pone las rutas de OpenZFS.
            const std::string con = T::wrapRemoteCommand(win, "PATH=\"/usr/sbin\"; export PATH; zfs list");
            igual(con, T::wrapRemoteCommand(win, "zfs list"),
                  "wrapRemoteCommand: se retira el prologo PATH de sintaxis Unix");
            // Pero solo entero: sin el ';' final no es ese prologo y no se toca.
            comprobar(T::wrapRemoteCommand(win, "PATH=\"a\"; export PATH") != T::wrapRemoteCommand(win, ""),
                      "wrapRemoteCommand: un prologo a medias no se retira");
        }
    }

    // --- Las piezas que sostienen un tunel: un proceso que sigue VIVO entre llamadas, un
    // puerto libre y la sonda de «ya acepta conexiones». Aqui se prueban contra un
    // proceso local; contra un `ssh -L` de verdad se probaron aparte, y el tunel tardo
    // 832 ms en aceptar conexiones, que son los ~830 ms que ya documentaba el codigo.
    {
        namespace P = zfsmgr::base;

        const std::uint16_t p1 = P::reserveFreeLocalPort();
        const std::uint16_t p2 = P::reserveFreeLocalPort();
        comprobar(p1 != 0 && p2 != 0, "reserveFreeLocalPort: devuelve puertos");
        comprobar(p1 != p2, "reserveFreeLocalPort: dos llamadas no dan el mismo");
        comprobar(!P::canConnectLocal(p1, 200),
                  "canConnectLocal: en un puerto reservado y soltado no escucha nadie");
        comprobar(!P::canConnectLocal(0, 200), "canConnectLocal: el puerto 0 no vale");

        {
            P::ChildProcess c;
            comprobar(c.start("sleep", {"30"}), "ChildProcess: arranca");
            comprobar(c.pid() > 0, "ChildProcess: tiene pid");
            comprobar(c.isRunning(), "ChildProcess: sigue vivo");
            c.stop(1500);
            comprobar(!c.isRunning(), "ChildProcess: stop() lo termina");
            c.stop(1500);  // idempotente: llamarlo dos veces no debe romper nada
            comprobar(!c.isRunning(), "ChildProcess: stop() es idempotente");
        }
        {
            // Un programa que no existe: el hijo muere con 127 y NO se queda de zombi.
            P::ChildProcess c;
            c.start("no-existe-xyz-123", {});
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            comprobar(!c.isRunning(), "ChildProcess: un programa inexistente no queda vivo");
        }
        {
            // El destructor MATA. Un `ssh -L` que sobrevive a quien lo creo deja un puerto
            // escuchando y una conexion abierta contra la otra maquina, y nadie los cierra.
            long long pid = 0;
            {
                P::ChildProcess c;
                c.start("sleep", {"30"});
                pid = c.pid();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            comprobar(pid > 0 && ::kill(static_cast<pid_t>(pid), 0) != 0,
                      "ChildProcess: el destructor mata al hijo");
        }
    }

    // --- El transporte de alto nivel. Lo que necesita una maquina se probo aparte, contra
    // un daemon real por SSH; aqui queda lo que se puede afirmar sin salir de esta.
    {
        namespace T = zfsmgr::base::transport;

        // La direccion de ESCUCHA no sirve como direccion de CONEXION: 0.0.0.0 y :: quieren
        // decir «en todas», y a eso no se conecta nadie.
        igual(T::bindAddressToConnectHost("0.0.0.0"), "127.0.0.1", "bind: 0.0.0.0 no es un destino");
        igual(T::bindAddressToConnectHost("::"), "127.0.0.1", "bind: :: tampoco");
        igual(T::bindAddressToConnectHost("192.168.1.5"), "192.168.1.5", "bind: una IPv4 concreta se respeta");
        igual(T::bindAddressToConnectHost("::1"), "::1", "bind: una IPv6 concreta se respeta");
        igual(T::bindAddressToConnectHost("localhost"), "127.0.0.1",
              "bind: lo que no es una IP se toma como el daemon local");
        igual(T::bindAddressToConnectHost(""), "127.0.0.1", "bind: vacio");

        // Resolucion de nombres: solo se usa para CONTARLO en el registro.
        const auto r = T::resolveHostAddresses("localhost");
        comprobar(r.ok && !r.addresses.empty(), "resolveHostAddresses: localhost resuelve");
        comprobar(!T::resolveHostAddresses("no.existe.invalido.zfsmgr").ok,
                  "resolveHostAddresses: un nombre inexistente falla y lo dice");
        comprobar(!T::resolveHostAddresses("").ok, "resolveHostAddresses: vacio no resuelve");

        // La sesion sin nada puesto no debe reventar: un CLI de solo lectura vive asi.
        {
            TransportSession vacia;
            vacia.log(TransportSession::Nivel::Info, "nadie escucha");  // no revienta
            std::string u;
            std::string c;
            comprobar(!vacia.askCredentials("x", u, c),
                      "sesion sin proveedor de credenciales: devuelve false");
            comprobar(vacia.respira(true) && vacia.respira(false),
                      "sesion sin pump: respira() dice que siga en los dos contextos");
            comprobar(vacia.puedeMontarTuneles(),
                      "sesion sin restriccion de hilo: se pueden montar tuneles");
            std::string e;
            comprobar(!vacia.persistTls(ConnectionProfile{}, "a", "b", "c", 1, &e)
                          && !e.empty(),
                      "sesion sin donde guardar el TLS: falla y explica");
        }

        // El destino del registro distingue el mensaje general del de una conexion.
        {
            TransportSession ses;
            std::vector<std::string> visto;
            ses.sink = [&visto](TransportSession::Nivel n, const std::string& id,
                                const std::string& m) {
                visto.push_back(std::to_string(static_cast<int>(n)) + "|" + id + "|" + m);
            };
            ses.log(TransportSession::Nivel::Warn, "general");
            ses.logConn(TransportSession::Nivel::Error, "unib", "de conexion");
            comprobar(visto.size() == 2, "sink: llegan los dos");
            if (visto.size() == 2) {
                igual(visto[0], "2||general", "sink: el general va sin identificador");
                igual(visto[1], "3|unib|de conexion", "sink: el de conexion lo lleva");
            }
        }

        // El desvio al hilo donde se pueden montar tuneles.
        {
            TransportSession ses;
            bool ejecutado = false;
            ses.enElHiloDeTuneles([&] { ejecutado = true; });
            comprobar(ejecutado, "enElHiloDeTuneles: sin restriccion, se ejecuta en linea");

            ejecutado = false;
            bool desviado = false;
            ses.tunnelsAllowedHere = [] { return false; };
            ses.runWhereTunnelsAllowed = [&desviado](const std::function<void()>& t) {
                desviado = true;
                t();
            };
            ses.enElHiloDeTuneles([&] { ejecutado = true; });
            comprobar(desviado && ejecutado, "enElHiloDeTuneles: con restriccion, se desvia");

            // Y si NO hay a donde desviar, se ejecuta igualmente: no hacerlo dejaria la
            // operacion sin ocurrir, que es peor.
            ses.runWhereTunnelsAllowed = nullptr;
            ejecutado = false;
            ses.enElHiloDeTuneles([&] { ejecutado = true; });
            comprobar(ejecutado, "enElHiloDeTuneles: sin desvio posible, se hace aqui");
        }

        // Con el transporte de mentira puesto NO se abre ninguna conexion. Lo que no sea
        // una invocacion del agente se anota y fracasa: es lo que permite afirmar en un
        // test que algo NO debia irse por shell.
        {
            TransportSession ses;
            ses.transportForTest = [](const std::vector<std::string>& argv, std::string& out,
                                      std::string& err, int& rc) {
                out = "argv:" + std::to_string(argv.size());
                err.clear();
                rc = 0;
                return true;
            };
            ConnectionProfile p;
            p.id = "x";
            p.connType = "SSH";
            p.host = "no.se.debe.contactar";
            std::string out;
            std::string err;
            int rc = -1;
            comprobar(T::runSsh(ses, p, daemonpayload::unixBinPath() + " --dump-x uno", 1000, out,
                                err, rc),
                      "transporte de prueba: la orden del agente se atiende");
            igual(out, "argv:2", "transporte de prueba: llegan los argumentos, no la cadena");
            comprobar(ses.callsForTest.size() == 1 && !ses.callsForTest[0].argv.empty(),
                      "transporte de prueba: se anota con argv");

            comprobar(!T::runSsh(ses, p, "zfs list -H", 1000, out, err, rc),
                      "transporte de prueba: una orden de shell FRACASA");
            comprobar(rc == 127, "transporte de prueba: y con rc=127");
            comprobar(ses.callsForTest.size() == 2 && ses.callsForTest[1].argv.empty()
                          && !ses.callsForTest[1].shellCommand.empty(),
                      "transporte de prueba: se anota como cadena de shell");
        }
    }

    // --- Las banderas de `zfs send` que se dejan llegar al mandato.
    //
    // Esta lista es una frontera de SEGURIDAD, no una comodidad: lo que la pase acaba en el
    // argv de un `zfs send` que corre con privilegios. Por eso se comprueba también el caso
    // que la motivó —un nombre de dataset colado entre las banderas—, y no solo que las
    // buenas pasen: un validador que acepte todo también haría pasar las buenas.
    {
        using zfsmgr::base::zfsprops::banderasDeSendValidas;
        std::string mala;
        comprobar(banderasDeSendValidas("", mala), "send: sin banderas vale");
        comprobar(banderasDeSendValidas("-w -L -c", mala), "send: las banderas de verdad pasan");
        comprobar(banderasDeSendValidas("-R -X tank/otro", mala),
                  "send: -X se lleva su dataset por delante");
        comprobar(!banderasDeSendValidas("tank/otro@ayer", mala),
                  "send: un dataset suelto NO pasa");
        igual(mala, "tank/otro@ayer", "send: y se dice cuál era");
        comprobar(!banderasDeSendValidas("-w tank/otro@ayer", mala),
                  "send: ni escondido detrás de una buena");
        comprobar(!banderasDeSendValidas("-Z", mala), "send: una bandera inventada NO pasa");
        comprobar(!banderasDeSendValidas("-i tank@a", mala),
                  "send: -i es del programa, no del usuario");
        comprobar(!banderasDeSendValidas("-t testigo", mala),
                  "send: -t tampoco");
        comprobar(!banderasDeSendValidas("-R -X", mala), "send: -X sin dataset NO pasa");
        // Agrupadas: es como las escribe OpenZFS y como las manda el planificador.
        comprobar(banderasDeSendValidas("-wLec", mala), "send: un grupo de banderas buenas pasa");
        comprobar(!banderasDeSendValidas("-wLZ", mala), "send: un grupo con una ajena NO pasa");
        igual(mala, "-wLZ", "send: y se señala el grupo entero, no una letra suelta");
        comprobar(!banderasDeSendValidas("-wX", mala),
                  "send: un grupo con una que lleva valor tampoco: no se sabe dónde empieza");
    }

    // --- Las instantáneas programadas (GSA).
    //
    // Estas reglas vivían dentro de la interfaz gráfica y en ningún sitio más. Al traerlas
    // aquí lo que se gana no es compartirlas: es que por fin se puedan comprobar, porque
    // antes hacía falta arrancar Qt y una ventana para llegar a ellas.
    {
        namespace G = zfsmgr::base::gsa;
        const auto siempreExiste = [](const std::string&) { return true; };
        const auto nuncaExiste = [](const std::string&) { return false; };
        G::Motivo m;

        // Leer las propiedades.
        G::Programacion p;
        comprobar(G::desdePropiedades({{"org.fc16.gsa:activado", "on"},
                                       {"org.fc16.gsa:diario", "7"},
                                       {"org.fc16.gsa:horario", ""}},
                                      p, m),
                  "gsa: se leen las propiedades");
        comprobar(p.activado && p.diario == 7 && p.horario == 0,
                  "gsa: vacío es 0 y «on» es activado");
        comprobar(G::desdePropiedades({{"ORG.FC16.GSA:ACTIVADO", "yes"}}, p, m) && p.activado,
                  "gsa: el nombre de la propiedad no distingue mayúsculas");
        comprobar(G::desdePropiedades({{"org.fc16.gsa:activado", "quizá"}}, p, m) && !p.activado,
                  "gsa: un booleano que no se entiende es «off», que es lo conservador");
        comprobar(!G::desdePropiedades({{"org.fc16.gsa:diario", "7d"}}, p, m),
                  "gsa: «7d» NO es una retención");
        comprobar(m.fallo == G::Fallo::RetencionNoEntera && m.detalle == "org.fc16.gsa:diario",
                  "gsa: y se dice cuál de las cinco");
        comprobar(!G::desdePropiedades({{"org.fc16.gsa:anual", "-1"}}, p, m),
                  "gsa: una retención negativa tampoco");

        // Ida y vuelta: lo escrito se vuelve a leer igual.
        G::Programacion q;
        q.activado = true; q.recursivo = true; q.diario = 7; q.destino = "oldlau::tank/copias";
        G::Programacion vuelta;
        comprobar(G::desdePropiedades(G::aPropiedades(q), vuelta, m),
                  "gsa: lo escrito se vuelve a leer");
        comprobar(vuelta.activado && vuelta.recursivo && vuelta.diario == 7
                      && vuelta.destino == "oldlau::tank/copias",
                  "gsa: y llega igual");

        // Las reglas, una a una, con su control.
        G::Programacion base;
        base.activado = true; base.diario = 7;
        comprobar(G::valida("tank/datos", base, siempreExiste, m), "gsa: la mínima válida vale");

        G::Programacion sinRet = base; sinRet.diario = 0;
        comprobar(!G::valida("tank/datos", sinRet, siempreExiste, m),
                  "gsa: activada y sin retenciones NO vale");
        comprobar(m.fallo == G::Fallo::ActivadaSinRetencion && m.dataset == "tank/datos",
                  "gsa: con su motivo y su dataset");

        G::Programacion apagadaSinRet = sinRet; apagadaSinRet.activado = false;
        comprobar(G::valida("tank/datos", apagadaSinRet, siempreExiste, m),
                  "gsa: apagada y sin retenciones SÍ vale: no hace nada");

        G::Programacion nivelar = base; nivelar.nivelar = true;
        comprobar(!G::valida("tank/datos", nivelar, siempreExiste, m),
                  "gsa: nivelar sin destino NO vale");
        comprobar(m.fallo == G::Fallo::NivelarSinDestino, "gsa: y lo dice");

        nivelar.destino = "tank/copias";
        comprobar(!G::valida("tank/datos", nivelar, siempreExiste, m),
                  "gsa: un destino sin «::» NO vale");
        comprobar(m.fallo == G::Fallo::DestinoMalFormado, "gsa: y lo dice");

        nivelar.destino = "oldlau::tank/copias";
        comprobar(G::valida("tank/datos", nivelar, siempreExiste, m),
                  "gsa: con conexión que existe, vale");
        comprobar(!G::valida("tank/datos", nivelar, nuncaExiste, m),
                  "gsa: si la conexión no existe, NO vale");
        comprobar(m.fallo == G::Fallo::DestinoSinConexion && m.detalle == "oldlau",
                  "gsa: y se nombra la conexión que falta");

        // Y el conjunto.
        G::Programacion rec = base; rec.recursivo = true;
        std::vector<G::Entrada> juego{{"tank/datos", rec}, {"tank/datos/hijo", base}};
        comprobar(!G::validaConjunto(juego, m), "gsa: un hijo bajo una recursiva choca");
        comprobar(m.fallo == G::Fallo::ChocaConRecursiva && m.dataset == "tank/datos/hijo"
                      && m.detalle == "tank/datos",
                  "gsa: y se dice quién con quién");
        comprobar(G::validaConjunto({{"tank/datos", base}, {"tank/datos/hijo", base}}, m),
                  "gsa: sin recursiva no chocan");
        comprobar(G::validaConjunto({{"tank/datos", rec}, {"tank/otro", base}}, m),
                  "gsa: y un hermano tampoco");
        G::Programacion apagada = base; apagada.activado = false;
        comprobar(G::validaConjunto({{"tank/datos", rec}, {"tank/datos/hijo", apagada}}, m),
                  "gsa: con el hijo apagado no hay choque: no hace instantáneas");
        // Y que «datosviejos» no cuente como hijo de «datos» por empezar igual.
        comprobar(G::validaConjunto({{"tank/datos", rec}, {"tank/datosviejos", base}}, m),
                  "gsa: el prefijo no basta, hace falta la barra");

        // --- Cómo se enseñan: manuales primero, programadas agrupadas por clase.
        //
        // Punto 6 del backlog. La regla estaba escrita dentro del bucle que pinta el árbol,
        // así que comprobarla exigía una ventana; aquí se comprueba el orden, que es lo
        // pedido, y de paso el intérprete la tiene para cuando liste instantáneas.
        igual(G::claseDeInstantanea("GSA-daily-20260322-000000"), "daily",
              "gsa: la clase va entre el primer y el segundo guion");
        igual(G::claseDeInstantanea("GSA-HOURLY-20260322-120000"), "hourly",
              "gsa: la clase no distingue mayúsculas");
        igual(G::claseDeInstantanea("manual-001"), "",
              "gsa: una manual no tiene clase");
        igual(G::claseDeInstantanea("GSA-20260322-120000"), "20260322",
              "gsa: sin clase en el nombre no se inventa una: se toma lo que hay");

        {
            const auto g = G::agrupaInstantaneas({"manual-001",
                                                  "GSA-daily-20260322-000000",
                                                  "GSA-hourly-20260322-120000",
                                                  "manual-002",
                                                  "GSA-hourly-20260322-130000",
                                                  "GSA-loquesea-20260322-140000"});
            comprobar(g.size() == 4, "gsa: manuales + horarias + diarias + la desconocida");
            igual(g[0].first, "", "gsa: el grupo de las manuales va primero");
            comprobar(g[0].second == std::vector<std::string>{"manual-001", "manual-002"},
                      "gsa: y conserva el orden en que llegaron");
            igual(g[1].first, "hourly", "gsa: de la hora al año, así que horarias antes que diarias");
            comprobar(g[1].second.size() == 2, "gsa: las dos horarias en su grupo");
            igual(g[2].first, "daily", "gsa: y después las diarias");
            igual(g[3].first, "loquesea", "gsa: una clase que no conocemos va al final, no se pierde");
        }
        {
            const auto g = G::agrupaInstantaneas({"GSA-yearly-20260101-000000", "GSA-weekly-20260322-000000"});
            comprobar(g.size() == 2, "gsa: sin manuales no hay grupo vacío por delante");
            igual(g[0].first, "weekly", "gsa: semanal antes que anual aunque llegara después");
        }
        comprobar(G::agrupaInstantaneas({}).empty(), "gsa: sin instantáneas, ningún grupo");
    }

    std::fprintf(stderr, "%d pasados, %d fallos\n", pasados, fallos);
    return fallos == 0 ? 0 : 1;
}
