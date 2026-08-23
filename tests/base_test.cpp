// Test de la capa base. SIN Qt: este ejecutable se enlaza solo contra zfsmgr_base, y
// esa es justamente la comprobación que aporta. Por eso no usa QTest y trae su propio
// arnés de cuatro líneas.

#include "zfsprops.h"
#include "daemoninstall.h"
#include "dosextremos.h"
#include "sincronizacion.h"
#include "sistemaoperativo.h"
#include "peers.h"
#include "avanzadas.h"
#include "peticiones.h"
#include "pools.h"
#include "instantaneas.h"
#include "datasets.h"
#include "transferencia.h"
#include "zfsallow.h"
#include "daemonpayload.h"
#include "connectionjson.h"
#include "listados.h"
#include "storefiles.h"
#include "storewarnings.h"
#include "connectionprofile.h"
#include "helpers.h"
#include "json.h"
#include "procesos.h"
#include "refreshparse.h"
#include "agentversion.h"
#include "connectionjson.h"
#include "listados.h"
#include "secretcipher.h"
#include "strutil.h"
#include "tlsclient.h"
#include "tlsserver.h"
#include "transportcmd.h"
#include "transportsession.h"
#include "transportrpc.h"
#include "transportsession.h"
#include "transporttunnel.h"
#include "zfsmurl.h"
#include "gsa.h"
#include "zfsprops.h"
#include <algorithm>
#include <sstream>
#include <atomic>

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

    {
        // «¿Hay descendientes montados?» ya no es un guion por plataforma ejecutado por SSH:
        // se contesta con la lista que da `--dump-zfs-mount`.
        namespace L2 = zfsmgr::base::listados;
        const std::string j =
            R"({"datasets":{"tank/datos":{"mountpoint":"/tank/datos"},)"
            R"("tank/datos/hijo":{"mountpoint":"/tank/datos/hijo"},)"
            R"("tank/datos2":{"mountpoint":"/tank/datos2"}}})";
        comprobar(L2::tieneDescendientesMontados(j, "tank/datos"),
                  "montados: un hijo montado cuenta");
        // **«tank/datos2» empieza por «tank/datos» y NO está debajo de él.** Sin la barra en
        // el prefijo, desmontar «tank/datos» preguntaría por un dataset hermano.
        comprobar(!L2::tieneDescendientesMontados(j, "tank/datos2"),
                  "montados: un hermano con nombre parecido no cuenta");
        // El propio dataset tampoco: la pregunta es si desmontarlo arrastra a otros.
        comprobar(!L2::tieneDescendientesMontados(
                      R"({"datasets":{"tank/solo":{"mountpoint":"/tank/solo"}}})", "tank/solo"),
                  "montados: uno mismo no es descendiente de sí mismo");
        comprobar(!L2::tieneDescendientesMontados(R"({"datasets":{}})", "tank/datos"),
                  "montados: ninguno montado no es un error");
    }

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
    // salida se volcaba entera al registro: con `zfsmgr-cli -v` salia por la salida de
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

    // --- los analizadores de listados
    //
    // Las muestras son SALIDA REAL de esta máquina, no ejemplos inventados: es lo que
    // distingue una prueba que sujeta algo de una que repite lo que el código ya hace.
    {
        namespace L = zfsmgr::base::listados;
        std::string err;

        // `zpool list -j`, recortado a un pool con sus campos.
        const std::string zpoolJson =
            R"({"output_version":{"command":"zpool list","vers_major":0,"vers_minor":1},)"
            R"("pools":{"fc16":{"name":"fc16","type":"POOL","state":"ONLINE",)"
            R"("pool_guid":"6150128433348792083","properties":{)"
            R"("size":{"value":"2.46T","source":{"type":"NONE","data":"-"}},)"
            R"("free":{"value":"763G","source":{"type":"NONE","data":"-"}},)"
            R"("capacity":{"value":"69%","source":{"type":"NONE","data":"-"}},)"
            R"("health":{"value":"ONLINE","source":{"type":"NONE","data":"-"}}}}}})";
        std::vector<L::Pool> ps;
        comprobar(L::pools(zpoolJson, ps, err) && err.empty(), "listados: se analiza zpool list -j");
        comprobar(ps.size() == 1, "listados: un pool");
        igual(ps.at(0).nombre, "fc16", "listados: su nombre");
        igual(ps.at(0).salud, "ONLINE", "listados: su salud, de properties");
        igual(ps.at(0).tamano, "2.46T", "listados: su tamaño");
        igual(ps.at(0).uso, "69%", "listados: y el uso tal cual, sin tocar");
        igual(ps.at(0).guid, "6150128433348792083", "listados: y el guid");

        // Sin pools NO es un error: macOS no imprime nada y sale con 0.
        comprobar(L::pools("", ps, err) && ps.empty() && err.empty(),
                  "listados: una salida vacia son cero pools, no un fallo");
        comprobar(L::pools("   \n", ps, err), "listados: ni con espacios");
        // Pero un JSON roto SI lo es.
        comprobar(!L::pools("{esto no es json", ps, err), "listados: un JSON roto si falla");
        comprobar(!err.empty(), "listados: y dice por que");

        // El TSV de --dump-zfs-list-all: diez columnas, salida real.
        const std::string tsv =
            "fc16/dockvols\t9566138329724705167\t464G\t1.16x\taes-256-gcm\tmar mar  3 19:06 2026\t33.3G\tyes\t/var/lib/docker/volumes\ton\n"
            "fc16/dockvols/axigen\t5564728462171259101\t127G\t1.21x\taes-256-gcm\tmar mar  3 19:07 2026\t127G\tyes\t/var/lib/docker/volumes/axigen\ton\n"
            "fc16/dockvols@ayer\t123\t0B\t1.00x\taes-256-gcm\tmar mar  3 19:07 2026\t33.3G\t-\t-\t-\n";
        const auto es = L::entradas(tsv);
        comprobar(es.size() == 3, "listados: tres entradas");
        igual(es.at(0).nombre, "fc16/dockvols", "listados: el nombre");
        igual(es.at(0).puntoMontaje, "/var/lib/docker/volumes", "listados: el punto de montaje");
        igual(es.at(0).cifrado, "aes-256-gcm", "listados: el cifrado");
        comprobar(!es.at(0).esInstantanea(), "listados: un dataset no es instantanea");
        comprobar(es.at(2).esInstantanea(), "listados: y una con @ si");

        // Una linea con columnas de menos se SALTA: rellenar corrido enseñaria el punto de
        // montaje donde va el guid.
        const auto pocas = L::entradas("solo\tdos\nfc16\t1\t2\t3\t4\t5\t6\t7\t8\t9\n");
        comprobar(pocas.size() == 1, "listados: la linea corta se salta");
        igual(pocas.at(0).nombre, "fc16", "listados: y la buena entra");

        // `zfs get -j all`.
        const std::string getJson =
            R"({"datasets":{"fc16/x":{"properties":{)"
            R"("compression":{"value":"lz4","source":{"type":"LOCAL","data":"-"}},)"
            R"("atime":{"value":"on","source":{"type":"INHERITED","data":"fc16"}}}}}})";
        std::vector<L::Propiedad> props;
        comprobar(L::propiedades(getJson, props, err), "listados: se analiza zfs get -j");
        comprobar(props.size() == 2, "listados: dos propiedades");
        igual(props.at(0).nombre, "atime", "listados: ordenadas por nombre");
        igual(props.at(0).origen, "inherited from fc16",
              "listados: el origen heredado dice de donde, como `zfs get -o source`");

        // El origen se escribe COMO LO ESCRIBE `zfs get -H -o source`, y no es cosmetico:
        // el «-» es la marca de una propiedad CALCULADA —`used`, `creation`— y es lo que
        // mira la regla que decide si se puede escribir encima. El JSON trae
        // «{"type":"DEFAULT","data":"-"}», asi que quedarse con `data` dejaba en «-» todo
        // lo que estuviera por omision y hacia que `atime`, `quota` y `recordsize`
        // salieran como si no se pudieran cambiar. Contrastado con la salida real de
        // `zfs get` sobre fc16/user.
        const std::string origenes =
            R"({"datasets":{"d":{"properties":{)"
            R"("atime":{"value":"on","source":{"type":"DEFAULT","data":"-"}},)"
            R"("used":{"value":"1","source":{"type":"NONE","data":"-"}},)"
            R"("mountpoint":{"value":"/x","source":{"type":"RECEIVED","data":"-"}},)"
            R"("quota":{"value":"none","source":{"type":"LOCAL","data":"-"}},)"
            R"("xattr":{"value":"sa","source":{"type":"INHERITED","data":"padre"}}}}}})";
        std::vector<L::Propiedad> orig;
        comprobar(L::propiedades(origenes, orig, err), "listados: se analizan los origenes");
        std::map<std::string, std::string> porNombre;
        for (const L::Propiedad& pr : orig) {
            porNombre[pr.nombre] = pr.origen;
        }
        igual(porNombre["atime"], "default", "listados: DEFAULT no es «-»");
        igual(porNombre["used"], "-", "listados: NONE si es «-», que es lo calculado");
        igual(porNombre["mountpoint"], "received", "listados: RECEIVED");
        igual(porNombre["quota"], "local", "listados: LOCAL");
        igual(porNombre["xattr"], "inherited from padre", "listados: INHERITED con su padre");

        // Y lo que de verdad importaba: con el origen bien, la regla deja escribir encima
        // de lo que esta por omision — que es la mayoria de las propiedades de un dataset
        // recien creado.
        comprobar(zfsmgr::base::zfsprops::editableEnLinea("atime", "filesystem",
                                                          porNombre["atime"], "off",
                                                          zfsmgr::base::zfsprops::Plataforma::Linux),
                  "listados: una propiedad por omision SI se puede cambiar");
        comprobar(!zfsmgr::base::zfsprops::editableEnLinea("used", "filesystem",
                                                           porNombre["used"], "off",
                                                           zfsmgr::base::zfsprops::Plataforma::Linux),
                  "listados: y una calculada no");
        igual(props.at(1).valor, "lz4", "listados: y el valor");

        // `zpool get -j all` es el MISMO formato con la seccion cambiada: los objetos
        // cuelgan de «pools» y no de «datasets». Fixture recortado de una salida real de
        // OpenZFS 2.4 —incluido el `feature@`, que es de donde salen las «capacidades»
        // del pool: no son otra consulta, son un filtro sobre estas mismas propiedades.
        const std::string getPool =
            R"({"output_version":{"command":"zpool get"},"pools":{"fc16":{"name":"fc16",)"
            R"("properties":{)"
            R"("size":{"value":"2.46T","source":{"type":"NONE","data":"-"}},)"
            R"("capacity":{"value":"69%","source":{"type":"NONE","data":"-"}},)"
            R"("feature@lz4_compress":{"value":"active","source":{"type":"LOCAL","data":"-"}}}}}})";
        std::vector<L::Propiedad> pp;
        comprobar(L::propiedadesDePool(getPool, pp, err), "listados: se analiza zpool get -j");
        comprobar(pp.size() == 3, "listados: las tres propiedades del pool");
        igual(pp.at(0).nombre, "capacity", "listados: ordenadas por nombre tambien aqui");
        igual(pp.at(1).nombre, "feature@lz4_compress", "listados: la capacidad es una propiedad");
        igual(pp.at(1).valor, "active", "listados: con su valor");
        igual(pp.at(2).nombre, "size", "listados: y la ultima por orden alfabetico");

        // Los controles negativos de la seccion: cada lector mira la SUYA. Si
        // `propiedadesDePool` mirase «datasets» —o al reves— no fallaria: devolveria una
        // lista VACIA, que es peor, porque parece un pool sin propiedades.
        std::vector<L::Propiedad> cruzado;
        comprobar(L::propiedades(getPool, cruzado, err) && cruzado.empty(),
                  "listados: zfs get NO lee la seccion de pools");
        comprobar(L::propiedadesDePool(getJson, cruzado, err) && cruzado.empty(),
                  "listados: y zpool get NO lee la de datasets");
        // Una salida vacia no es un error: es una maquina sin nada que contar.
        comprobar(L::propiedadesDePool("", cruzado, err) && cruzado.empty(),
                  "listados: salida vacia de zpool get no es un fallo");
        comprobar(!L::propiedadesDePool("{esto no es json", cruzado, err),
                  "listados: pero la basura si lo es");
    }

    // --- el destino de la programacion: guardado de una forma, ensenado de otra
    //
    // «Conexion::Pool/Dataset» es lo que hay ESCRITO en las propiedades de datasets que ya
    // existen, y lo que el planificador del daemon parte por «::». No se puede cambiar. Pero
    // esa nomenclatura es anterior a `zfsm://` y en pantalla convive mal con el resto, asi
    // que se convierte en los dos sentidos.
    {
        namespace G = zfsmgr::base::gsa;
        igual(G::destinoComoUrl("unibody::tank/copias"), "zfsm://unibody/tank/copias",
              "gsa: el destino guardado se ensena como URL");
        igual(G::destinoDesdeUrl("zfsm://unibody/tank/copias"), "unibody::tank/copias",
              "gsa: y la URL se guarda como siempre");
        // La vuelta y vuelta no pierde nada, que es lo unico que impide que las dos mitades
        // se separen.
        for (const char* d : {"unibody::tank/copias", "local::fc16", "oldlau::winpool/sb/x"}) {
            igual(G::destinoDesdeUrl(G::destinoComoUrl(d)), d,
                  std::string("gsa: ida y vuelta de «") + d + "»");
        }
        // Se admiten las DOS formas al teclear: quien escriba a mano puede poner cualquiera.
        igual(G::destinoDesdeUrl("unibody::tank/copias"), "unibody::tank/copias",
              "gsa: el formato de siempre se acepta tal cual");
        // Y lo que no tiene forma de nada se deja pasar para que lo rechace la validacion
        // CON SU MOTIVO, no aqui en silencio.
        igual(G::destinoComoUrl("sinformato"), "sinformato",
              "gsa: lo que no tiene la forma se deja como esta");
        igual(G::destinoDesdeUrl("zfsm://solomaquina"), "zfsm://solomaquina",
              "gsa: una URL sin dataset no se convierte a medias");
        igual(G::destinoComoUrl(""), "", "gsa: el vacio sigue vacio");
    }

    // --- por dónde van los bytes, y desde dónde se reanuda
    //
    // Fase 0 de docs/diseno_tecnico_transferencias.md: las DECISIONES de una transferencia,
    // que se pueden probar sin mover un byte. Lo que se fija son los NOES y su orden, porque
    // el orden es lo que hace que el motivo sea util: decir «no hay daemon» cuando el
    // problema es que un extremo es Windows manda a instalar algo que no arregla nada.
    {
        namespace TR = zfsmgr::base::transferencia;
        auto ext = [](const char* c, const char* o, bool win, bool dae, bool job) {
            TR::Extremo e;
            e.conexion = c; e.objeto = o;
            e.esWindows = win; e.tieneDaemon = dae; e.admiteTrabajos = job;
            return e;
        };
        const TR::Extremo snapOk = ext("local", "p/d@lunes", false, true, true);
        const TR::Extremo dsOk   = ext("unibody", "t/copias", false, true, true);

        // El caso bueno: los dos con daemon y con trabajos. Se pueden probar los TRES, y
        // en ese orden.
        const TR::Plan buena = TR::planea(snapOk, dsOk, false);
        comprobar(buena.sePuede() && buena.caminos.size() == 3, "transferencia: los tres caminos");
        comprobar(buena.caminos.at(0) == TR::Camino::TrabajoAsincrono
                      && buena.caminos.at(1) == TR::Camino::DaemonADaemon
                      && buena.caminos.at(2) == TR::Camino::TuberiaSsh,
                  "transferencia: y en orden de preferencia");

        // **La tuberia SSH no necesita daemon en ningun extremo**: manda `zfs send` y
        // `zfs recv` por SSH. Es lo que queda cuando no hay daemon, y por eso una copia
        // entre dos maquinas sin agente sigue siendo posible.
        const TR::Extremo sinNada = ext("unibody", "t/copias", false, false, false);
        const TR::Plan pelada = TR::planea(ext("local", "p/d@x", false, false, false), sinNada,
                                           false);
        comprobar(pelada.sePuede() && pelada.caminos.size() == 1
                      && pelada.caminos.at(0) == TR::Camino::TuberiaSsh,
                  "transferencia: sin daemon en ninguno, queda la tuberia SSH");

        // Con daemon en los dos pero sin trabajos: se cae el asincrono y quedan dos.
        const TR::Extremo sinJobs = ext("unibody", "t/copias", false, true, false);
        const TR::Plan dos = TR::planea(snapOk, sinJobs, false);
        comprobar(dos.caminos.size() == 2 && dos.caminos.at(0) == TR::Camino::DaemonADaemon,
                  "transferencia: sin trabajos, la interfaz aun tiene dos caminos");

        // Y para quien NO puede esperar, los otros dos no son un respaldo: son otra cosa
        // que no puede hacer. Mejor decir que no que empezar algo que se va a cortar.
        comprobar(TR::planea(snapOk, sinJobs, true).fallo == TR::Fallo::SinTrabajos,
                  "transferencia: quien no puede esperar solo tiene el asincrono");
        comprobar(TR::planea(snapOk, sinJobs, true).caminos.empty(),
                  "transferencia: y no se le ofrece ninguno");
        comprobar(TR::planea(snapOk, dsOk, true).caminos.size() == 1,
                  "transferencia: con trabajos en los dos, si");

        // EL ORDEN de los noes. Windows corta TODO, no solo un camino: los dos primeros
        // necesitan tuberia y el tercero es un guion POSIX que alli no se ejecuta.
        const TR::Extremo win = ext("oldlau", "wp/d", true, true, true);
        comprobar(TR::planea(snapOk, win, false).fallo == TR::Fallo::ExtremoWindows,
                  "transferencia: Windows corta aunque tenga daemon y trabajos");
        comprobar(TR::planea(snapOk, win, false).caminos.empty(),
                  "transferencia: y no deja ningun camino que probar");
        // Y lo que no depende del camino corta antes que Windows.
        comprobar(TR::planea(ext("local", "p/d", false, true, true), win, false).fallo
                      == TR::Fallo::OrigenNoEsInstantanea,
                  "transferencia: «el origen no es instantanea» manda sobre Windows");
        comprobar(TR::planea(snapOk, snapOk, false).fallo == TR::Fallo::ElMismoObjeto,
                  "transferencia: el mismo objeto, lo primero de todo");
        comprobar(TR::planea(snapOk, ext("unibody", "t/c@ya", false, true, true), false).fallo
                      == TR::Fallo::DestinoNoEsDataset,
                  "transferencia: no se recibe SOBRE una instantanea");

        // Cada motivo con su texto, y ninguno repetido: es lo que se enseña.
        std::set<std::string> textos;
        for (const TR::Fallo f : {TR::Fallo::ElMismoObjeto, TR::Fallo::OrigenNoEsInstantanea,
                                  TR::Fallo::DestinoNoEsDataset, TR::Fallo::ExtremoWindows,
                                  TR::Fallo::SinTrabajos}) {
            comprobar(!TR::etiquetaDe(f).empty(), "transferencia: el motivo tiene texto");
            textos.insert(TR::etiquetaDe(f));
        }
        comprobar(textos.size() == 5, "transferencia: y los cinco son distintos");

        // --- el testigo de reanudacion
        //
        // ESTO es lo que costo una tarde en su dia: las copias van con -R, o sea toda la
        // jerarquia en un flujo, y al cortarse ZFS deja el testigo en el dataset que estaba
        // recibiendo, que casi NUNCA es la raiz. Mirar solo la raiz decia «no hay nada que
        // reanudar» con 247 MB ya transferidos.
        const std::string enElHijo =
            "t/copias\t-\n"
            "t/copias/uno\t-\n"
            "t/copias/dos\t1-e7c3a...-token\n";
        const auto rHijo = TR::testigoDeReanudacion("t/copias", enElHijo);
        comprobar(rHijo.hay(), "transferencia: el testigo se encuentra en el DESCENDIENTE");
        igual(rHijo.quienLoTiene, "t/copias/dos", "transferencia: y se dice en cual estaba");

        // El del propio objetivo manda sobre los de sus descendientes.
        const std::string enLosDos =
            "t/copias\tTESTIGO-RAIZ\n"
            "t/copias/dos\tTESTIGO-HIJO\n";
        igual(TR::testigoDeReanudacion("t/copias", enLosDos).quienLoTiene, "t/copias",
              "transferencia: el del objetivo manda sobre el del hijo");

        // --- lo que contestan los dos extremos al lanzar un trabajo
        //
        // Se leen aparte de ir a buscarlos, para poder fijar QUE se descarta. Y lo que se
        // descarta es lo importante: un testigo que no mide 64 no viene recortado, es que no
        // es la respuesta que se esperaba, y seguir con el dejaria al emisor hablando con
        // quien no debe.
        {
            const std::string bueno =
                "PORT=41235\n"
                "TOKEN=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
            const auto e = TR::leeEscucha(bueno);
            comprobar(e.vale() && e.puerto == 41235, "escucha: puerto y testigo");
            igual(std::to_string(e.testigo.size()), "64", "escucha: el testigo mide 64");

            comprobar(!TR::leeEscucha("PORT=41235\nTOKEN=corto\n").vale(),
                      "escucha: un testigo corto NO vale");
            comprobar(!TR::leeEscucha("TOKEN=0123456789abcdef0123456789abcdef"
                                      "0123456789abcdef0123456789abcdef\n").vale(),
                      "escucha: sin puerto tampoco");
            comprobar(!TR::leeEscucha("PORT=0\nTOKEN=0123456789abcdef0123456789abcdef"
                                      "0123456789abcdef0123456789abcdef\n").vale(),
                      "escucha: el puerto cero no es un puerto");
            comprobar(!TR::leeEscucha("").vale(), "escucha: sin respuesta, nada");
            // Lineas de mas no estorban: el daemon puede escribir avisos por delante.
            comprobar(TR::leeEscucha("INFO algo\n" + bueno).vale(),
                      "escucha: lo que no reconoce se salta");

            igual(TR::leeIdentificadorDeTrabajo("JOB_ID=2a538be8659bd62d\n"), "2a538be8659bd62d",
                  "trabajo: el identificador");
            igual(TR::leeIdentificadorDeTrabajo("algo\nJOB_ID=abc\nmas\n"), "abc",
                  "trabajo: aunque venga rodeado");
            igual(TR::leeIdentificadorDeTrabajo("sin identificador"), "",
                  "trabajo: y sin el, vacio");

            // Los cinco puntos donde puede romperse, con su texto y ninguno repetido: cada
            // uno lleva a un sitio distinto —uno es del receptor, otro de la red, otro del
            // emisor— y confundirlos manda a mirar donde no es.
            std::set<std::string> ft;
            for (const TR::FalloTrabajo f :
                 {TR::FalloTrabajo::ReceptorNoEscucha, TR::FalloTrabajo::RespuestaDeEscuchaNoVale,
                  TR::FalloTrabajo::SinDireccionDeVuelta, TR::FalloTrabajo::EmisorNoArranco,
                  TR::FalloTrabajo::SinIdentificador}) {
                comprobar(!TR::etiquetaDe(f).empty(), "trabajo: el fallo tiene texto");
                ft.insert(TR::etiquetaDe(f));
            }
            comprobar(ft.size() == 5, "trabajo: y los cinco son distintos");
        }

        // --- donde se recibe DE VERDAD
        //
        // No es el dataset sobre el que se pulso: se le anade el nombre del origen. Este
        // detalle es tambien el que hace que buscar el testigo de reanudacion sobre el
        // dataset pulsado no encuentre nada — hay que buscarlo sobre ESTE.
        igual(TR::destinoReal("p/datos", "t/respaldos"), "t/respaldos/datos",
              "destino: se anade el nombre del origen");
        igual(TR::destinoReal("p/a/b/datos", "t/respaldos"), "t/respaldos/datos",
              "destino: solo la hoja, no la ruta entera");
        // Si el destino ya acaba en ese nombre se toma tal cual: copiar dos veces al mismo
        // sitio dejaria «respaldos/datos/datos».
        igual(TR::destinoReal("p/datos", "t/respaldos/datos"), "t/respaldos/datos",
              "destino: si ya acaba en el nombre, no se repite");
        igual(TR::destinoReal("p/datos", "t/datos"), "t/datos",
              "destino: aunque sea el primer nivel");
        // Un pool entero como origen: su «hoja» es el propio nombre del pool.
        igual(TR::destinoReal("wpool", "t/respaldos"), "t/respaldos/wpool",
              "destino: un pool entero tambien lleva su nombre");
        igual(TR::destinoReal("p/datos", ""), "", "destino: sin destino, nada que componer");

        // --- las ordenes de envio y recepcion
        igual(TR::ordenDeEnvio("p/d@lunes", ""), "zfs send 'p/d@lunes'",
              "orden: sin banderas");
        igual(TR::ordenDeEnvio("p/d@lunes", "-wR"), "zfs send -wR 'p/d@lunes'",
              "orden: con banderas");
        // El «-Fus» del receptor no es decorativo: la «s» es lo que hace que un corte deje
        // un envio EN SUSPENSO con su testigo en vez de basura. Sin ella no habria
        // reanudacion y cada corte obligaria a mandarlo todo otra vez.
        igual(TR::ordenDeRecepcion("t/respaldos/datos"), "zfs recv -Fus 't/respaldos/datos'",
              "orden: la recepcion lleva -Fus, con la «s» de suspenso");
        // Un nombre con comilla dentro no puede romper la orden. El escapado es el de
        // siempre en shell: cerrar la comilla, meter una entre dobles, y volver a abrir.
        igual(TR::ordenDeEnvio("p/d@ra\'ro", ""), "zfs send 'p/d@ra'\"'\"'ro'",
              "orden: la comilla del nombre no rompe la orden");


        // --- la version de OpenZFS que admite transferir
        //
        // Por debajo de 2.3.3 no. La regla es del proyecto, no de ZFS, y estaba escrita
        // dentro de la ventana. Lo que se fija es la FRONTERA y, sobre todo, que no saber la
        // version NO bloquea: no saberla es distinto de saber que es vieja, y bloquear por
        // no saber dejaria sin copiar a una maquina que quiza puede.
        comprobar(!TR::versionAdmiteTransferencia("2.3.2"), "version: 2.3.2 no");
        comprobar(TR::versionAdmiteTransferencia("2.3.3"), "version: 2.3.3 justo si");
        comprobar(TR::versionAdmiteTransferencia("2.3.4"), "version: y de ahi para arriba");
        comprobar(!TR::versionAdmiteTransferencia("2.2.99"), "version: 2.2.x no, por alta que sea");
        comprobar(TR::versionAdmiteTransferencia("2.4.0"), "version: 2.4 si");
        comprobar(TR::versionAdmiteTransferencia("3.0.0"), "version: la rama 3 no es «vieja»");
        // El sufijo de distribucion no dice nada del formato del flujo: se ignora.
        comprobar(TR::versionAdmiteTransferencia("2.3.3-1ubuntu2"), "version: con sufijo, igual");
        comprobar(!TR::versionAdmiteTransferencia("2.2.7-pve1"), "version: y la vieja con sufijo");
        // «2.3» sin el tercer numero es 2.3.0, que es menor que 2.3.3.
        comprobar(!TR::versionAdmiteTransferencia("2.3"), "version: «2.3» es 2.3.0");
        // No saberla no bloquea.
        comprobar(TR::versionAdmiteTransferencia(""), "version: vacia no bloquea");
        comprobar(TR::versionAdmiteTransferencia("zfswin-2.4.1rc14"),
                  "version: lo que no empieza por numero tampoco bloquea");

        // Y entra en el plan ANTES que el camino: da igual por donde vayan los bytes si el
        // formato del flujo no se entiende en el otro lado.
        TR::Extremo viejo = ext("unibody", "t/copias", false, true, true);
        viejo.versionZfs = "2.2.7";
        comprobar(TR::planea(snapOk, viejo, false).fallo == TR::Fallo::ZfsDemasiadoViejo,
                  "transferencia: un extremo con ZFS viejo corta el plan");
        comprobar(TR::planea(snapOk, viejo, false).caminos.empty(),
                  "transferencia: y no deja ningun camino");

        // --- las banderas de `zfs send`
        comprobar(TR::banderasDeEnvio({}).empty(), "banderas: sin ninguna, cadena vacia");
        igual(TR::banderasDeEnvio({true, false, false, false, true}), "-wR",
              "banderas: en el orden en que las escribe el programa");
        igual(TR::banderasDeEnvio({true, true, true, true, true}), "-wLecR", "banderas: las cinco");
        // Vacio y NO «-»: un guion suelto en medio del argv es algo que `zfs` no entiende.
        comprobar(TR::banderasDeEnvio({}) != "-", "banderas: el guion solo no se emite");

        // --- la direccion con la que el ORIGEN ve a este equipo
        //
        // Dos cosas que se aprendieron a base de fallar, y las dos se fijan aqui.
        igual(TR::direccionDeSshClient("192.168.1.40 54321 22\n"), "192.168.1.40",
              "sshclient: el primer campo es la direccion");
        // El recorte se hace en C++ y NO con `${SSH_CLIENT%% *}` en la orden: esa orden la
        // lanza el cliente, que puede ser Windows, y alli «%» es la expansion de cmd — se
        // comia parte del texto y devolvia una direccion con una letra de mas.
        igual(TR::direccionDeSshClient("10.0.0.1 1 2\n10.0.0.2 3 4\n"), "10.0.0.1",
              "sshclient: solo la primera linea");

        // **IPv6 CON ZONA.** sshd contesto `fe80::…%enp1s0f0` en la maquina de pruebas, y
        // una validacion de solo hexadecimal y puntos lo rechazaba: la copia se quedaba sin
        // direccion a la que volver y moria con «cannot connect to peer».
        igual(TR::direccionDeSshClient("fe80::d11d:24e3:5547:cbd6%enp1s0f0 54321 22"),
              "fe80::d11d:24e3:5547:cbd6%enp1s0f0", "sshclient: IPv6 con zona se admite");
        igual(TR::direccionDeSshClient("2001:db8::1 1 2"), "2001:db8::1",
              "sshclient: y IPv6 a secas");

        // Lo que no parece una direccion se descarta en vez de mandarse al otro extremo.
        igual(TR::direccionDeSshClient(""), "", "sshclient: sin salida, nada");
        igual(TR::direccionDeSshClient("\n"), "", "sshclient: una linea vacia tampoco");
        igual(TR::direccionDeSshClient("hola 1 2"), "",
              "sshclient: sin dos puntos ni punto no es una direccion");
        igual(TR::direccionDeSshClient("1.2.3.4;rm -rf / 1 2"), "",
              "sshclient: y un caracter que no toca lo descarta entero");

        // «-» es «no hay», no un testigo que se llama asi.
        comprobar(!TR::testigoDeReanudacion("t/copias", "t/copias\t-\n").hay(),
                  "transferencia: «-» es que no hay ninguno");
        comprobar(!TR::testigoDeReanudacion("t/copias", "").hay(),
                  "transferencia: y sin salida tampoco hay");
        // Que el dataset no salga NO significa que no haya nada a medias: significa que aun
        // no existe, que es lo normal en una copia nueva.
        comprobar(!TR::testigoDeReanudacion("t/nuevo", "t/copias\t-\n").hay(),
                  "transferencia: un destino que aun no existe no tiene testigo");
    }

    // --- los permisos delegados: leer `zfs allow` y componer lo que los cambia
    //
    // El fixture es SALIDA REAL, capturada de un pool creado para esto con las cinco formas
    // que tiene el mandato: usuario, grupo, everyone, un conjunto y el alcance «solo los
    // descendientes». El formato parece facil hasta que se mira: el ALCANCE va en el TITULO
    // de la seccion y no en la linea, asi que perderlo significa conceder a los
    // descendientes lo que se queria conceder solo aqui.
    {
        namespace ZA = zfsmgr::base::zfsallow;
        const std::string real =
            "---- Permissions on wperm/d ------------------------------------------\n"
            "Permission sets:\n"
            "\t@basico hold,snapshot\n"
            "Descendent permissions:\n"
            "\tuser linarese destroy\n"
            "Local+Descendent permissions:\n"
            "\tuser root @basico\n"
            "\tuser linarese create,mount,snapshot\n"
            "\teveryone mount\n";
        const auto es = ZA::analiza(real);
        comprobar(es.size() == 5, "zfsallow: las cinco entradas");

        comprobar(es[0].alcance == ZA::Alcance::Conjunto && es[0].quien == ZA::Quien::Conjunto,
                  "zfsallow: el conjunto");
        igual(es[0].nombre, "@basico", "zfsallow: con su nombre y su arroba");
        comprobar(es[0].permisos.size() == 2, "zfsallow: y sus dos permisos");

        // ESTA es la que importa: la linea dice «user linarese destroy» y no dice nada del
        // alcance. Sale del titulo de encima.
        comprobar(es[1].alcance == ZA::Alcance::Descendientes,
                  "zfsallow: el alcance sale del TITULO de la seccion");
        igual(es[1].nombre, "linarese", "zfsallow: y el usuario de la linea");

        comprobar(es[2].alcance == ZA::Alcance::LocalYDescendientes,
                  "zfsallow: la seccion siguiente cambia el alcance");
        igual(es[2].permisos.at(0), "@basico",
              "zfsallow: un conjunto se concede como si fuera un permiso");
        comprobar(es[4].quien == ZA::Quien::Todos && es[4].nombre.empty(),
                  "zfsallow: «everyone» no nombra a nadie");

        // Y el camino de vuelta: el argv que hay que ejecutar.
        const auto conceder = ZA::argvConceder(es[1], "wperm/d");
        igual(conceder.at(0), "allow", "zfsallow: la orden");
        comprobar(std::find(conceder.begin(), conceder.end(), "-d") != conceder.end(),
                  "zfsallow: «solo descendientes» lleva -d");
        comprobar(std::find(conceder.begin(), conceder.end(), "-u") != conceder.end(),
                  "zfsallow: y a un usuario, -u");
        igual(conceder.back(), "wperm/d", "zfsallow: el dataset al final");
        igual(ZA::argvRetirar(es[1], "wperm/d").at(0), "unallow",
              "zfsallow: retirar es la misma forma con otra orden");

        // «aqui y en los descendientes» va SIN bandera de alcance: es lo que hace `zfs
        // allow` por omision, y ponerle una lo estrecharia.
        const auto ambos = ZA::argvConceder(es[2], "wperm/d");
        comprobar(std::find(ambos.begin(), ambos.end(), "-l") == ambos.end()
                      && std::find(ambos.begin(), ambos.end(), "-d") == ambos.end(),
                  "zfsallow: el alcance de los dos no lleva bandera");

        // «everyone» no lleva nombre en el argv: el destinatario ES la bandera.
        const auto todos = ZA::argvConceder(es[4], "wperm/d");
        comprobar(std::find(todos.begin(), todos.end(), "-e") != todos.end(),
                  "zfsallow: everyone lleva -e");
        comprobar(std::find(todos.begin(), todos.end(), "everyone") == todos.end(),
                  "zfsallow: y NO repite la palabra como destinatario");

        // «Create time permissions» no nombra a nadie: su linea es SOLO la lista de
        // permisos. Sin ese caso se saltaba entera, y esos permisos —los que hereda quien
        // cree un descendiente— no salian por ninguna parte.
        const auto crear = ZA::analiza("Create time permissions:\n\trollback,mount\n");
        comprobar(crear.size() == 1, "zfsallow: «al crear» se lee aunque no nombre a nadie");
        comprobar(crear.at(0).alcance == ZA::Alcance::AlCrear, "zfsallow: con su alcance");
        comprobar(crear.at(0).permisos.size() == 2, "zfsallow: y sus dos permisos");
        const auto argvCrear = ZA::argvConceder(crear.at(0), "p/d");
        comprobar(std::find(argvCrear.begin(), argvCrear.end(), "-c") != argvCrear.end(),
                  "zfsallow: y se concede con -c");
        comprobar(argvCrear.size() == 4, "zfsallow: sin destinatario: allow -c <perms> <ds>");

        // Los textos de ZFS, que son CONTRATO: el tsv y el json del interprete los llevan y
        // un guion puede estar comparandolos. Cambiarlos por algo mas legible lo romperia
        // sin avisar, asi que se fijan aqui.
        igual(ZA::seccionZfs(ZA::Alcance::LocalYDescendientes), "Local+Descendent permissions",
              "zfsallow: el titulo exacto de la seccion");
        igual(ZA::seccionZfs(ZA::Alcance::AlCrear), "Create time permissions",
              "zfsallow: y el de «al crear»");
        igual(ZA::tokenZfs(ZA::Quien::Usuario), "user", "zfsallow: la palabra de zfs");
        igual(ZA::tokenZfs(ZA::Quien::Todos), "everyone", "zfsallow: y la de everyone");
        // Y la vuelta y vuelta: lo que se lee de una seccion se vuelve a nombrar igual.
        for (const ZA::Alcance a : {ZA::Alcance::Local, ZA::Alcance::Descendientes,
                                    ZA::Alcance::LocalYDescendientes, ZA::Alcance::AlCrear,
                                    ZA::Alcance::Conjunto}) {
            const auto ida = ZA::analiza(std::string(ZA::seccionZfs(a)) + ":\n\tuser x lee\n");
            comprobar(!ida.empty() && ida.at(0).alcance == a,
                      std::string("zfsallow: la seccion «") + ZA::seccionZfs(a) + "» se reconoce");
        }

        // Un dataset sin nada delegado devuelve la lista vacia, y eso no es un error.
        comprobar(ZA::analiza("").empty(), "zfsallow: sin permisos, lista vacia");
        comprobar(ZA::analiza("---- Permissions on x ----\n").empty(),
                  "zfsallow: solo la cabecera tampoco es una entrada");
        // Y una linea que no se entiende se salta en vez de inventarse una entrada.
        comprobar(ZA::analiza("Local permissions:\n\tvete a saber\n").empty(),
                  "zfsallow: lo que no se entiende no se inventa");
    }

    // --- qué se puede hacer con DOS extremos
    //
    // La regla estaba dentro del menú contextual de la interfaz. No es de interfaz: es qué
    // deja hacer ZFS entre dos objetos, y el servidor web necesita la misma para saber qué
    // ofrece y qué deja en gris. Lo que se fija aquí son los NOES, que es lo que se enseña.
    {
        namespace DX = zfsmgr::base::dosextremos;
        const DX::Extremo snap{"local", "fc16/user@lunes"};
        const DX::Extremo snap2{"local", "fc16/user@martes"};
        const DX::Extremo ds{"local", "fc16/user"};
        const DX::Extremo otroDs{"local", "fc16/work"};
        const DX::Extremo otraMaq{"unibody", "tank/x"};
        const DX::Extremo nada{};

        // Comparar: dos puntos de la MISMA historia. Es lo que la gente espera mal la
        // primera vez —cree que compara dos datasets cualesquiera— y por eso el motivo
        // tiene que salir escrito.
        comprobar(DX::compruebo(DX::Accion::Diff, snap, snap2) == DX::NoAplica::Ninguna,
                  "dosextremos: comparar dos instantaneas del mismo dataset");
        comprobar(DX::compruebo(DX::Accion::Diff, snap, ds) == DX::NoAplica::Ninguna,
                  "dosextremos: y una instantanea contra su dataset vivo");
        comprobar(DX::compruebo(DX::Accion::Diff, snap, otroDs) == DX::NoAplica::DistintoDataset,
                  "dosextremos: pero NO contra otro dataset");
        comprobar(DX::compruebo(DX::Accion::Diff, ds, otroDs)
                      == DX::NoAplica::OrigenNoEsInstantanea,
                  "dosextremos: ni con un dataset de origen");
        comprobar(DX::compruebo(DX::Accion::Diff, snap, otraMaq) == DX::NoAplica::DistintaMaquina,
                  "dosextremos: ni entre maquinas distintas");

        // Clonar: de una instantanea a un sitio, y ese sitio es un dataset.
        comprobar(DX::compruebo(DX::Accion::Clonar, snap, otroDs) == DX::NoAplica::Ninguna,
                  "dosextremos: clonar de una instantanea a un dataset");
        comprobar(DX::compruebo(DX::Accion::Clonar, snap, snap2)
                      == DX::NoAplica::DestinoNoEsDataset,
                  "dosextremos: no se clona SOBRE una instantanea");
        comprobar(DX::compruebo(DX::Accion::Clonar, ds, otroDs)
                      == DX::NoAplica::OrigenNoEsInstantanea,
                  "dosextremos: ni desde un dataset");

        // Sin origen, ninguna. Y el mismo objeto en los dos extremos tampoco.
        for (const DX::Accion a : {DX::Accion::Diff, DX::Accion::Clonar, DX::Accion::Copiar}) {
            comprobar(DX::compruebo(a, nada, ds) == DX::NoAplica::SinOrigen,
                      std::string("dosextremos: sin origen no aplica ") + DX::claveDe(a));
            comprobar(DX::compruebo(a, ds, ds) == DX::NoAplica::ElMismoObjeto,
                      std::string("dosextremos: el mismo objeto no aplica ") + DX::claveDe(a));
        }

        // Sincronizar NO es zfs send: compara FICHEROS sobre los puntos de montaje, y por
        // eso puede borrar en el destino. Lo que se fija aqui es cuando NO se puede, que es
        // lo que se pinta en gris.
        {
            namespace SY = zfsmgr::base::sincronizacion;
            SY::Extremo o;
            o.conexion = "local"; o.objeto = "wa/uno";
            o.montado = true; o.puntoMontaje = "/wa/uno"; o.tieneDaemon = true;
            SY::Extremo d = o;
            d.objeto = "wa/dos"; d.puntoMontaje = "/wa/dos";

            const SY::Plan ok = SY::planea(o, d);
            comprobar(ok.sePuede(), "sincronizar: dos datasets montados en la misma maquina");
            comprobar(ok.rutaOrigen == "/wa/uno" && ok.rutaDestino == "/wa/dos",
                      "sincronizar: y devuelve las dos rutas");

            // Entre maquinas SI se puede: va por el arbol por el socket entre daemons, que
            // no necesita rsync en ninguno de los dos lados.
            SY::Extremo otraMaq = d; otraMaq.conexion = "unibody";
            comprobar(SY::planea(o, otraMaq).sePuede(),
                      "sincronizar: entre maquinas si, por el arbol");
            // Y con un extremo Windows tambien, que es justo lo que ese mecanismo vino a
            // arreglar: por tar no habia ni borrado ni simulacion.
            SY::Extremo winRemoto = d;
            winRemoto.conexion = "oldlau";
            winRemoto.esWindows = true;
            // Con la ruta que de verdad se puede abrir en Windows. Comprobado en vivo: la
            // propiedad `mountpoint` de un dataset alli dice «/winpool/sa», y esa ruta NO
            // EXISTE para el sistema; la buena, con letra de unidad, sale de `zfs mount`.
            winRemoto.puntoMontaje = "Z:/sa";
            comprobar(SY::planea(o, winRemoto).sePuede(),
                      "sincronizar: con un extremo Windows entre maquinas, tambien");
            SY::Extremo winMal = winRemoto;
            winMal.puntoMontaje = "/winpool/sa";
            comprobar(SY::planea(o, winMal).fallo == SY::Fallo::RutaNoUsable,
                      "sincronizar: una ruta POSIX en Windows no vale, aunque lo diga zfs list");
            comprobar(SY::rutaUsable("Z:/sa", true) && !SY::rutaUsable("Z:/sa", false),
                      "sincronizar: la letra de unidad solo vale en Windows");
            // Y dentro de UNA misma maquina Windows tambien, desde que ese caso va por el
            // arbol —el daemon conectandose consigo mismo— en vez de por rsync. Tener dos
            // caminos segun la plataforma dejaba uno de los dos sin probar la mitad de las
            // veces.
            SY::Extremo win = d;
            win.esWindows = true;
            win.puntoMontaje = "Z:/dos";
            SY::Extremo winO = o;
            winO.esWindows = true;
            winO.puntoMontaje = "Z:/uno";
            comprobar(SY::planea(winO, win).sePuede(),
                      "sincronizar: dentro de una misma maquina Windows, por el arbol");
            SY::Extremo sinMontar = d; sinMontar.montado = false;
            comprobar(SY::planea(o, sinMontar).fallo == SY::Fallo::DestinoNoMontado,
                      "sincronizar: sin montar no hay nada que comparar");
            SY::Extremo sinRuta = d; sinRuta.puntoMontaje = "none";
            comprobar(SY::planea(o, sinRuta).fallo == SY::Fallo::RutaNoUsable,
                      "sincronizar: «none» no es una ruta");
            SY::Extremo instant = d; instant.objeto = "wa/dos@lunes";
            comprobar(SY::planea(o, instant).fallo == SY::Fallo::DestinoNoEsDataset,
                      "sincronizar: no se sincroniza contra una instantanea");
            SY::Extremo sinD = d; sinD.tieneDaemon = false;
            comprobar(SY::planea(o, sinD).fallo == SY::Fallo::SinDaemon,
                      "sincronizar: hace falta el daemon");

            // La comprobacion barata NO mira montajes: es la que se usa al pintar, y mirar
            // montajes ahi costaba una consulta por dataset dibujado.
            SY::Extremo desmontado = d; desmontado.montado = false;
            comprobar(SY::compruebo(o, desmontado) == SY::Fallo::Ninguno,
                      "sincronizar: la comprobacion barata no consulta montajes");

            comprobar(!SY::rutaUsable("none") && !SY::rutaUsable("legacy")
                          && !SY::rutaUsable("") && !SY::rutaUsable("relativa"),
                      "sincronizar: rutas que no sirven");
            comprobar(SY::rutaUsable("/wa/uno"), "sincronizar: una ruta absoluta si");

            // La carga del verbo tipado: base64 de un JSON con los flags delante.
            const std::string carga =
                SY::cargaRsync({{"/wa/uno", "/wa/dos"}}, true, true, "", "");
            comprobar(!carga.empty(), "sincronizar: la carga se construye");
            std::string claro;
            comprobar(zfsmgr::base::base64Decode(carga, claro),
                      "sincronizar: y es base64 valido");
            comprobar(claro.find("[\"1\",\"1\",\"\",\"\",\"/wa/uno\",\"/wa/dos\"]")
                          != std::string::npos,
                      "sincronizar: con los flags y el par en orden");
            comprobar(SY::cargaRsync({{"/wa/uno", "none"}}, false, false, "", "").empty(),
                      "sincronizar: una ruta mala no genera carga");
            comprobar(SY::cargaRsync({}, false, false, "", "").empty(),
                      "sincronizar: sin pares no hay carga");
        }

        // Nivelar: incremental sobre una base comun buscada POR GUID. La web hacia un
        // envio completo, que no es lo mismo ni de lejos: llega con `zfs recv -Fus` y
        // arrastra lo que el origen no tenga.
        {
            namespace TRN = zfsmgr::base::transferencia;
            const std::vector<TRN::Instantanea> orig = {
                {"lunes", "111"}, {"martes", "222"}, {"miercoles", "333"}, {"jueves", "444"}};

            // El caso normal: el destino llego hasta «martes», se manda de ahi a «jueves».
            const TRN::PlanNivelar ok =
                TRN::planeaNivelar(orig, {{"lunes", "111"}, {"martes", "222"}}, "jueves");
            comprobar(ok.sePuede(), "nivelar: hay incremental");
            comprobar(ok.base == "martes", "nivelar: la base es la ultima del destino");
            comprobar(ok.objetivo == "jueves", "nivelar: hasta la pedida");

            // El GUID manda sobre el nombre. Aqui el destino tiene un «martes» que NO es el
            // del origen —lo creo otro—, asi que no hay base comun aunque el nombre coincida.
            const TRN::PlanNivelar impostor =
                TRN::planeaNivelar(orig, {{"martes", "999"}}, "jueves");
            comprobar(impostor.fallo == TRN::FalloNivelar::BaseNoEstaEnOrigen,
                      "nivelar: un nombre igual con otro guid NO es base comun");

            // Sin instantaneas en el destino no hay desde donde seguir.
            comprobar(TRN::planeaNivelar(orig, {}, "jueves").fallo
                          == TRN::FalloNivelar::DestinoSinInstantaneas,
                      "nivelar: destino vacio no se nivela, se copia");

            // El destino va POR DELANTE de lo que se quiere enviar: se para.
            comprobar(TRN::planeaNivelar(orig, {{"miercoles", "333"}}, "martes").fallo
                          == TRN::FalloNivelar::DestinoMasNuevo,
                      "nivelar: no se pisa un destino mas moderno");

            // Ya esta al dia: no hay nada que mandar, y decirlo es mejor que mandar cero.
            comprobar(TRN::planeaNivelar(orig, {{"jueves", "444"}}, "jueves").fallo
                          == TRN::FalloNivelar::YaNivelado,
                      "nivelar: ya nivelado");

            // Y la que se pide tiene que existir en el origen.
            comprobar(TRN::planeaNivelar(orig, {{"lunes", "111"}}, "viernes").fallo
                          == TRN::FalloNivelar::ObjetivoNoEstaEnOrigen,
                      "nivelar: el objetivo tiene que existir");

            // Cada motivo con su texto, y ninguno repetido: es lo que se pinta.
            std::set<std::string> textosN;
            const std::vector<TRN::FalloNivelar> fallos = {
                TRN::FalloNivelar::ObjetivoNoEstaEnOrigen,
                TRN::FalloNivelar::DestinoSinInstantaneas,
                TRN::FalloNivelar::BaseNoEstaEnOrigen,
                TRN::FalloNivelar::DestinoMasNuevo,
                TRN::FalloNivelar::YaNivelado};
            for (const TRN::FalloNivelar f : fallos) {
                const std::string t = TRN::etiquetaDe(f);
                comprobar(!t.empty(), "nivelar: el motivo tiene texto");
                textosN.insert(t);
            }
            comprobar(textosN.size() == fallos.size(),
                      "nivelar: ningun motivo se confunde con otro");
        }

        // Mover NO es copiar y destruir: es un `zfs rename`, y por eso no sale de su pool
        // ni acepta instantaneas. El documento de diseno decia lo contrario; estas
        // aserciones son las que fijan la version buena.
        const DX::Extremo otroPool{"local", "worg/sitio"};
        const DX::Extremo hijo{"local", "fc16/user/dentro"};
        comprobar(DX::compruebo(DX::Accion::Mover, ds, otroDs) == DX::NoAplica::Ninguna,
                  "dosextremos: mover un dataset bajo otro del mismo pool");
        comprobar(DX::compruebo(DX::Accion::Mover, ds, otroPool) == DX::NoAplica::DistintoPool,
                  "dosextremos: pero NO a otro pool, que eso es copiar");
        comprobar(DX::compruebo(DX::Accion::Mover, snap, otroDs)
                      == DX::NoAplica::OrigenNoEsDataset,
                  "dosextremos: ni una instantanea de origen");
        comprobar(DX::compruebo(DX::Accion::Mover, ds, snap2)
                      == DX::NoAplica::DestinoNoEsDataset,
                  "dosextremos: ni sobre una instantanea");
        comprobar(DX::compruebo(DX::Accion::Mover, ds, otraMaq) == DX::NoAplica::DistintaMaquina,
                  "dosextremos: ni entre maquinas");
        comprobar(DX::compruebo(DX::Accion::Mover, ds, hijo)
                      == DX::NoAplica::DestinoDentroDelOrigen,
                  "dosextremos: ni dentro de si mismo");
        // «fc16/user» NO es padre de «fc16/user2»: la comparacion lleva la barra puesta.
        const DX::Extremo casiHijo{"local", "fc16/user2"};
        comprobar(DX::compruebo(DX::Accion::Mover, ds, casiHijo) == DX::NoAplica::Ninguna,
                  "dosextremos: y user2 no es descendiente de user");
        comprobar(DX::destinoDeMover(ds, otroDs) == "fc16/work/user",
                  "dosextremos: al mover conserva su ultimo nombre");

        // Las tres de transferencia que faltan se ofrecen y se dicen: esconderlas haria
        // creer que no existen, y el motivo es distinto de «no aplica aqui».
        for (const DX::Accion a : {DX::Accion::Copiar, DX::Accion::Sincronizar,
                                   DX::Accion::Nivelar}) {
            comprobar(DX::compruebo(a, snap, otroDs) == DX::NoAplica::TodaviaNoEstaEnLaWeb,
                      std::string("dosextremos: ") + DX::claveDe(a) + " dice que aun no esta");
        }

        // Cada motivo tiene su texto, y ninguno se confunde con otro: es lo que se pinta.
        std::set<std::string> textos;
        const std::vector<DX::NoAplica> motivos = {DX::NoAplica::SinOrigen,
                                                   DX::NoAplica::ElMismoObjeto,
                                                   DX::NoAplica::OrigenNoEsInstantanea,
                                                   DX::NoAplica::DestinoNoEsDataset,
                                                   DX::NoAplica::DistintoDataset,
                                                   DX::NoAplica::DistintaMaquina,
                                                   DX::NoAplica::DistintoPool,
                                                   DX::NoAplica::OrigenNoEsDataset,
                                                   DX::NoAplica::DestinoDentroDelOrigen,
                                                   DX::NoAplica::TodaviaNoEstaEnLaWeb};
        for (const DX::NoAplica n : motivos) {
            const std::string t = DX::etiquetaDe(n);
            comprobar(!t.empty(), "dosextremos: el motivo tiene texto");
            textos.insert(t);
        }
        // Se cuenta contra la propia lista, no contra un numero escrito a mano: con el 7
        // fijo, anadir un motivo rompia esta asercion por el conteo y no por lo que
        // comprueba, que es que NINGUN motivo se confunda con otro.
        comprobar(textos.size() == motivos.size(),
                  "dosextremos: y ningun motivo se confunde con otro");
        igual(DX::etiquetaDe(DX::NoAplica::Ninguna), "",
              "dosextremos: «si aplica» no tiene motivo que enseñar");
    }

    // --- una respuesta LENTA no se da por perdida
    //
    // El plazo de CONEXION no puede seguir siendo el plazo de LECTURA. `conecta()` lo pone
    // en el socket con SO_RCVTIMEO para acotar el connect, y ese valor se quedaba puesto:
    // para el daemon local vale como mucho 700 ms, asi que cualquier respuesta mas lenta se
    // daba por perdida con «el daemon no respondio», por muy alto que fuera el plazo
    // pedido —120 segundos en una mutacion—.
    //
    // No era teorico: `zpool sync` sobre un pool de 2,46 TB tarda 2,4 s y arrancar un scrub
    // otro tanto. La orden LLEGABA y el daemon la ejecutaba; lo que fallaba era recoger la
    // respuesta, asi que la accion se hacia y se contaba como error.
    //
    // Aqui se monta un servidor TLS de verdad que tarda A PROPOSITO mas que el plazo de
    // conexion. Solo lo pasa un cliente que distinga los dos plazos.
    {
        namespace TS = zfsmgr::base::tlsserver;
        namespace TC = zfsmgr::base;
        const std::string dir = "/tmp/zfsmgr-tls-lento";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        const std::string cert = dir + "/s.crt";
        const std::string clave = dir + "/s.key";
        const std::string certCli = dir + "/c.crt";
        const std::string claveCli = dir + "/c.key";
        std::string errC;
        comprobar(TS::escribeParAutofirmado(cert, clave, "localhost", true, "IP:127.0.0.1", errC),
                  "tls-lento: se emite el par del servidor");
        comprobar(TS::escribeParAutofirmado(certCli, claveCli, "cliente", false, std::string(),
                                            errC),
                  "tls-lento: y el del cliente, que el cliente exige tener");

        const int puerto = 47791;
        std::atomic<bool> vivo{true};
        std::atomic<bool> atendio{false};
        // El servidor tarda 1200 ms en contestar: cuatro veces el plazo de conexion que se
        // le pone abajo al cliente.
        std::thread hilo([&] {
            std::string errS;
            TS::sirve("127.0.0.1", puerto, cert, clave,
                      [&](const std::string&, std::string& resp) {
                          std::this_thread::sleep_for(std::chrono::milliseconds(1200));
                          atendio = true;
                          resp = "{\"rc\":0}\n";
                          return true;
                      },
                      [&] { return vivo.load(); }, errS);
        });
        // A que el socket este escuchando de verdad.
        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        const auto leePem = [](const std::string& r) {
            std::ifstream f(r);
            std::ostringstream ss;
            ss << f.rdbuf();
            return ss.str();
        };
        const std::string pemCert = leePem(cert);
        TC::TlsClientConfig cfg;
        cfg.host = "127.0.0.1";
        cfg.port = static_cast<unsigned short>(puerto);
        cfg.serverCertPem = pemCert;
        cfg.clientCertPem = leePem(certCli);
        cfg.clientKeyPem = leePem(claveCli);
        cfg.connectTimeoutMs = 300;    // el que mataba la lectura
        cfg.ioTimeoutMs = 20000;       // el que de verdad manda
        std::string resp;
        std::string errT;
        const auto t0 = std::chrono::steady_clock::now();
        // `sirve()` espera una peticion con forma de HTTP —hasta la linea en blanco—, asi
        // que la de la prueba la lleva. Lo que se mide es el PLAZO, no el protocolo.
        const bool ok = TC::tlsRequestLine(cfg, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", resp, errT);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        comprobar(ok, std::string("tls-lento: se espera la respuesta lenta (") + errT + ")");
        igual(resp, "{\"rc\":0}", "tls-lento: y llega entera");
        comprobar(ms > 1000, "tls-lento: se esperaron de verdad los 1200 ms, no se corto a los 300");
        comprobar(atendio.load(), "tls-lento: el servidor llego a atender");

        // El apagado: `sirve()` mira `sigueVivo` ENTRE conexiones, asi que hay que darle
        // una mas para que salga del accept(). Si aun asi no saliera, se le suelta el hilo
        // en vez de colgar la suite entera: lo que se estaba probando ya esta medido.
        vivo = false;
        {
            TC::TlsClientConfig fin = cfg;
            fin.ioTimeoutMs = 5000;
            std::string r2;
            std::string e2;
            TC::tlsRequestLine(fin, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", r2, e2);
        }
        hilo.join();
        std::filesystem::remove_all(dir);
    }

    // --- el puerto local de un tunel nunca es uno de los NUESTROS
    //
    // El rango efimero de Linux es 32768-60999 e incluye el 47653 (daemon) y el 47654
    // (servidor web), asi que el nucleo los reparte como cualquier otro. Un `ssh -L` se
    // quedaba con el 47654 y despues el servidor web no arrancaba, con un mensaje que no
    // decia quien lo tenia. Paso de verdad.
    {
        bool salioUnoNuestro = false;
        int cuantos = 0;
        for (int i = 0; i < 200; ++i) {
            const std::uint16_t p = zfsmgr::base::reserveFreeLocalPort();
            if (p == 0) {
                continue;   // sin puertos libres en esta maquina: no es lo que se prueba
            }
            ++cuantos;
            if (p == 47653 || p == 47654) {
                salioUnoNuestro = true;
            }
        }
        comprobar(cuantos > 0, "puertos: el sistema da puertos libres");
        comprobar(!salioUnoNuestro, "puertos: y NUNCA uno de los que este programa reserva");
    }

    // --- qué propiedad se puede escribir encima
    //
    // Esta regla estaba TRES veces: `isDatasetPropertySupportedOnPlatform` duplicada letra
    // por letra en `mainwindow_dataset_props.cpp` y `mainwindow_dataset_tree.cpp`, y la de
    // editabilidad otras dos con nombres distintos —`isDatasetPropertyEditable` y
    // `...EditableInline`— y el mismo cuerpo. Las tres con Qt dentro, así que el servidor
    // web no podía usarlas y habría acabado con una cuarta.
    {
        namespace ZP = zfsmgr::base::zfsprops;
        igual(std::to_string(static_cast<int>(ZP::plataformaDe("FreeBSD 15", ""))),
              std::to_string(static_cast<int>(ZP::Plataforma::FreeBsd)), "zfsprops: FreeBSD");
        igual(std::to_string(static_cast<int>(ZP::plataformaDe("", "Darwin 24.0"))),
              std::to_string(static_cast<int>(ZP::Plataforma::MacOs)),
              "zfsprops: la linea de uname vale cuando el perfil no dice nada");
        igual(std::to_string(static_cast<int>(ZP::plataformaDe("", ""))),
              std::to_string(static_cast<int>(ZP::Plataforma::Otra)),
              "zfsprops: sin datos, no se inventa una");

        // Lo que solo existe en un sistema. Ofrecerlo en otro es ofrecer un error.
        comprobar(ZP::soportadaEn("jailed", ZP::Plataforma::FreeBsd), "zfsprops: jailed en FreeBSD");
        comprobar(!ZP::soportadaEn("jailed", ZP::Plataforma::Linux), "zfsprops: y NO en Linux");
        comprobar(ZP::soportadaEn("zoned", ZP::Plataforma::Linux), "zfsprops: zoned en Linux");
        comprobar(!ZP::soportadaEn("zoned", ZP::Plataforma::FreeBsd), "zfsprops: y NO en FreeBSD");
        comprobar(!ZP::soportadaEn("sharesmb", ZP::Plataforma::MacOs), "zfsprops: sharesmb no en macOS");
        comprobar(!ZP::soportadaEn("vscan", ZP::Plataforma::Linux), "zfsprops: vscan en ningun sitio");

        const auto lin = ZP::Plataforma::Linux;
        comprobar(ZP::editableEnLinea("compression", "filesystem", "local", "off", lin),
                  "zfsprops: compression se escribe");
        comprobar(ZP::editableEnLinea("volsize", "volume", "local", "off", lin),
                  "zfsprops: volsize en un volumen");
        comprobar(!ZP::editableEnLinea("volsize", "filesystem", "local", "off", lin),
                  "zfsprops: pero NO en un sistema de ficheros");
        comprobar(ZP::editableEnLinea("quota", "filesystem", "local", "off", lin),
                  "zfsprops: quota en un sistema de ficheros");
        comprobar(!ZP::editableEnLinea("quota", "volume", "local", "off", lin),
                  "zfsprops: y NO en un volumen");

        // Los tres cortes que van ANTES de mirar la lista, y que son los que de verdad
        // evitan ofrecer una caja de edición que solo puede fallar.
        comprobar(!ZP::editableEnLinea("used", "filesystem", "-", "off", lin),
                  "zfsprops: origen «-» es calculada, no editable");
        comprobar(!ZP::editableEnLinea("compression", "filesystem", "local", "on", lin),
                  "zfsprops: lo que ZFS declara readonly no se toca");
        comprobar(!ZP::editableEnLinea("compression", "filesystem", "local", "yes", lin),
                  "zfsprops: y «yes» cuenta igual que «on»");
        comprobar(!ZP::editableEnLinea("jailed", "filesystem", "local", "off", lin),
                  "zfsprops: lo no soportado en la plataforma tampoco");

        // A una instantánea no se le cambia nada. Es de solo lectura por definición, y en
        // el árbol se seleccionan tanto como los datasets.
        comprobar(!ZP::editableEnLinea("compression", "snapshot", "local", "off", lin),
                  "zfsprops: a una instantanea no se le escribe");

        // Las del usuario SIEMPRE, porque ZFS no las interpreta — y ahí es donde este
        // programa guarda su programación.
        comprobar(ZP::editableEnLinea("org.fc16.gsa:diario", "filesystem", "local", "off", lin),
                  "zfsprops: las propiedades de usuario se escriben");
        comprobar(ZP::esPropiedadDeUsuario("org.fc16.gsa:diario"), "zfsprops: llevan dos puntos");
        comprobar(!ZP::esPropiedadDeUsuario("compression"), "zfsprops: y las de ZFS no");
        // Y SI se escriben en una instantánea, aunque las de ZFS no. No es un descuido de
        // la regla: se le preguntó a ZFS. `zfs set org.fc16.prueba:x=1 pool@s1` la acepta y
        // se lee de vuelta; `zfs set compression=zstd pool@s1` contesta «this property can
        // not be modified for snapshots». La regla dice exactamente eso.
        comprobar(ZP::editableEnLinea("org.fc16.gsa:diario", "snapshot", "local", "off", lin),
                  "zfsprops: las de usuario SI se escriben en una instantanea");

        // Sin saber el tipo se admite lo de cualquiera de los dos, que es lo que hacía la
        // interfaz: es mejor ofrecerlo y que ZFS diga que no, a esconder lo que sí valía.
        comprobar(ZP::editableEnLinea("recordsize", "", "local", "off", lin),
                  "zfsprops: sin tipo, se admite lo de los dos");
        comprobar(!ZP::editableEnLinea("creation", "", "local", "off", lin),
                  "zfsprops: pero no lo que no esta en ninguna lista");
    }

    // --- el guion que instala el daemon
    //
    // Estas 200 lineas vivian dentro de `src/cli/shell.cpp`, sueltas entre los fprintf del
    // interprete y sin una sola prueba: para verlas habia que instalar de verdad en una
    // maquina de cada sistema. Al bajarlas a la capa base —para que el servidor web use el
    // MISMO guion y no una copia— se pueden mirar aqui.
    //
    // Lo que se fija es lo que distingue a un sistema de otro y lo que, de romperse, deja
    // una maquina que PARECE atendida: el gestor de servicios y la comprobacion posterior.
    {
        namespace DI = zfsmgr::base::daemoninstall;
        const std::string lin = DI::guionDeInstalacion("linux", "9.9.9.1", "7");
        const std::string mac = DI::guionDeInstalacion("macos", "9.9.9.1", "7");
        const std::string bsd = DI::guionDeInstalacion("freebsd", "9.9.9.1", "7");

        // Los tres despliegan el binario POR LA ENTRADA ESTANDAR y lo colocan con
        // `install -m 700`. Un `cp` dejaria los permisos del origen.
        for (const auto& par : {std::make_pair("linux", lin), std::make_pair("macos", mac),
                                std::make_pair("freebsd", bsd)}) {
            comprobar(par.second.find("cat > \"$tmp_bin\"") != std::string::npos,
                      std::string("daemoninstall: ") + par.first + " recibe el binario por stdin");
            comprobar(par.second.find("install -m 700") != std::string::npos,
                      std::string("daemoninstall: ") + par.first + " lo instala con 700");
            comprobar(par.second.find("9.9.9.1") != std::string::npos,
                      std::string("daemoninstall: ") + par.first + " escribe la version dada");
            comprobar(par.second.find("rm -f \"$tmp_bin\"") != std::string::npos,
                      std::string("daemoninstall: ") + par.first + " no deja el temporal");
        }

        // Cada uno con SU gestor de servicios, y ninguno con el de otro. Lo segundo es lo
        // que de verdad hay que comprobar: un `systemctl` colado en el guion de macOS
        // fallaria en silencio y dejaria el daemon instalado y parado.
        comprobar(lin.find("systemctl restart zfsmgr-agent.service") != std::string::npos,
                  "daemoninstall: linux arranca por systemd");
        comprobar(lin.find("launchctl") == std::string::npos
                      && lin.find("service zfsmgr_agent") == std::string::npos,
                  "daemoninstall: y NO usa launchctl ni rc.d");
        comprobar(mac.find("launchctl bootstrap system") != std::string::npos,
                  "daemoninstall: macos arranca por launchd");
        comprobar(mac.find("systemctl") == std::string::npos,
                  "daemoninstall: y NO usa systemctl");
        comprobar(bsd.find("service zfsmgr_agent start") != std::string::npos,
                  "daemoninstall: freebsd arranca por rc.d");
        comprobar(bsd.find("systemctl") == std::string::npos
                      && bsd.find("launchctl") == std::string::npos,
                  "daemoninstall: y NO usa systemctl ni launchctl");

        // Los tres COMPRUEBAN que sigue vivo despues de arrancarlo. Sin esto se instala,
        // el servicio muere y el cliente cree que la maquina esta atendida.
        comprobar(lin.find("systemctl enable") != std::string::npos,
                  "daemoninstall: linux lo deja habilitado para el proximo arranque");
        comprobar(mac.find("launchd no dejo el agente corriendo tras instalar") != std::string::npos,
                  "daemoninstall: macos falla si launchd no lo deja activo");
        comprobar(bsd.find("no permanece activo tras el arranque") != std::string::npos,
                  "daemoninstall: freebsd falla si no permanece activo");
        // Y FreeBSD mira las dependencias ANTES: sin OpenSSL el daemon no arranca y el
        // motivo real queda en un error del cargador que no dice que falta.
        comprobar(bsd.find("pkg install openssl") != std::string::npos,
                  "daemoninstall: freebsd dice que falta OpenSSL cuando falta");

        // Una plataforma desconocida cae a Linux y NO a un guion vacio: un guion vacio se
        // ejecutaria con exito sin instalar nada, que es la forma silenciosa de fallar.
        igual(DI::guionDeInstalacion("loquesea", "9.9.9.1", "7"), lin,
              "daemoninstall: lo desconocido cae al guion de Linux");

        // La plataforma sale del perfil, sin preguntarle a la maquina.
        zfsmgr::base::ConnectionProfile perfilMac;
        perfilMac.osType = "macOS 15";
        igual(DI::plataformaDe(perfilMac), "macos", "daemoninstall: macOS por el osType");
        perfilMac.osType = "Darwin";
        igual(DI::plataformaDe(perfilMac), "macos", "daemoninstall: y Darwin tambien");
        perfilMac.osType = "FreeBSD 15";
        igual(DI::plataformaDe(perfilMac), "freebsd", "daemoninstall: FreeBSD");
        perfilMac.osType = "Ubuntu 24.04";
        igual(DI::plataformaDe(perfilMac), "linux", "daemoninstall: y lo demas es linux");
        perfilMac.osType.clear();
        igual(DI::plataformaDe(perfilMac), "linux", "daemoninstall: sin osType, linux");

        // El fallo es un TIPO y cada valor tiene su texto: un `bool` obligaba a adivinar
        // entre «no hay binario» —que se arregla compilando— y «la maquina lo rechazo».
        comprobar(DI::etiquetaDe(DI::Fallo::BinarioIlegible)
                      != DI::etiquetaDe(DI::Fallo::LaInstalacionFallo),
                  "daemoninstall: los motivos de fallo no se confunden");

        // Y un binario que no existe se para ANTES de tocar la maquina.
        zfsmgr::base::TransportSession sesionVacia;
        zfsmgr::base::ConnectionProfile local;
        local.name = "Local";
        const DI::Resultado sinBin =
            DI::instala(sesionVacia, local, "/no/existe/este/agente", {}, false);
        comprobar(sinBin.fallo == DI::Fallo::BinarioIlegible,
                  "daemoninstall: sin binario no se toca la maquina");
    }

    // --- guardar un perfil: cifrado, y el TLS segun cambie o no el EXTREMO
    //
    // Esta ultima parte es la que el interprete no tenia: escribia el perfil tal cual, asi
    // que un `edit` que cambiara el host se quedaba con el certificado fijado del host
    // VIEJO. Fiarse de un certificado que no corresponde a la maquina con la que se habla
    // es justo lo que el fijado existe para impedir.
    {
        namespace ST = zfsmgr::base::store;
        const std::string dirG = "/tmp/zfsmgr-base-test-guardar";
        std::filesystem::remove_all(dirG);
        const std::string maestra = "m";

        zfsmgr::base::ConnectionProfile p;
        p.id = "unibody"; p.name = "Unibody"; p.connType = "SSH";
        p.host = "unib.local"; p.port = 22; p.username = "linarese";
        p.password = "secreta";
        p.daemonTlsServerCertPem = "CERT-DE-UNIB";
        ST::Aviso av;
        comprobar(ST::guardaPerfil(dirG, p, maestra, av) && av.vacio(), "guardar: escribe el perfil");

        // La contrasena queda CIFRADA en disco, nunca en claro.
        auto leido = ST::leerConfig(dirG, av);
        const zfsmgr::base::json::Value& c0 = leido["connections"].toArray().at(0);
        comprobar(zfsmgr::base::SecretCipher::isEncrypted(c0["password"].toString()),
                  "guardar: la contrasena va cifrada");
        // El material TLS NO va en config.json: vive en el almacen de confianza, que es un
        // fichero aparte para separarlo del secreto de acceso. Y ahi va cifrado tambien.
        comprobar(!c0.contains("daemon_tls_server_cert_pem")
                      || c0["daemon_tls_server_cert_pem"].toString().empty(),
                  "guardar: el TLS no se escribe en config.json");
        auto leidoTrust = ST::leerTrustStore(dirG, av);
        comprobar(leidoTrust["connections"].toArray().size() == 1,
                  "guardar: la entrada va al almacen de confianza");
        comprobar(zfsmgr::base::SecretCipher::isEncrypted(
                      leidoTrust["connections"].toArray().at(0)["daemon_tls_server_cert_pem"].toString()),
                  "guardar: y alli el material TLS va cifrado");

        // Guardar OTRA VEZ sin material TLS, con el mismo extremo: se conserva el que habia.
        zfsmgr::base::ConnectionProfile igualExtremo = p;
        igualExtremo.daemonTlsServerCertPem.clear();
        comprobar(ST::guardaPerfil(dirG, igualExtremo, maestra, av), "guardar: segunda vez");
        leidoTrust = ST::leerTrustStore(dirG, av);
        zfsmgr::base::ConnectionProfile tras = zfsmgr::base::connjson::connectionFromJson(
            leidoTrust["connections"].toArray().at(0), std::string());
        ST::Avisos avisos;
        zfsmgr::base::connjson::abreSecretos(tras, maestra, avisos);
        igual(tras.daemonTlsServerCertPem, "CERT-DE-UNIB",
              "guardar: mismo extremo, se conserva el TLS que ya habia");

        // Y cambiando el HOST: el certificado del sitio anterior NO viaja.
        zfsmgr::base::ConnectionProfile otroHost = p;
        otroHost.host = "otra.local";
        otroHost.daemonTlsServerCertPem.clear();
        comprobar(ST::guardaPerfil(dirG, otroHost, maestra, av), "guardar: con otro host");
        leidoTrust = ST::leerTrustStore(dirG, av);
        comprobar(leidoTrust["connections"].toArray().empty(),
                  "guardar: cambiar de host SUELTA el certificado fijado del anterior");

        // Sin maestra no se escribe en claro: se dice y se para.
        zfsmgr::base::ConnectionProfile sinM;
        sinM.id = "otra"; sinM.name = "Otra"; sinM.connType = "SSH";
        sinM.host = "h"; sinM.username = "u"; sinM.password = "en-claro";
        comprobar(!ST::guardaPerfil(dirG, sinM, "", av), "guardar: sin maestra no se guarda");
        comprobar(av.motivo == ST::Motivo::ClaveMaestraRequeridaParaCifrar, "guardar: y lo dice");
        igual(av.conexion, "Otra", "guardar: diciendo de que conexion");

        // --- Cifrar lo que quedo en claro.
        {
            const std::string dirC = "/tmp/zfsmgr-base-test-cifrar";
            std::filesystem::remove_all(dirC);
            namespace J2 = zfsmgr::base::json;
            J2::Value conexion;
            conexion.set("id", J2::Value(std::string("x")));
            conexion.set("name", J2::Value(std::string("X")));
            conexion.set("password", J2::Value(std::string("EN-CLARO")));
            J2::Value cfg2;
            cfg2.set("connections", J2::Value(J2::Array{conexion}));
            ST::Aviso av2;
            comprobar(ST::escribirConfig(dirC, cfg2, av2), "cifrar: config con un secreto en claro");
            comprobar(ST::cifraLoQueFalte(dirC, "m", av2) && av2.vacio(), "cifrar: se cifra");
            auto tras2 = ST::leerConfig(dirC, av2);
            const std::string guardado2 = tras2["connections"].toArray().at(0)["password"].toString();
            comprobar(zfsmgr::base::SecretCipher::isEncrypted(guardado2), "cifrar: ya no esta en claro");
            std::string claro2;
            std::string err2;
            comprobar(zfsmgr::base::SecretCipher::decryptEncv1(guardado2, "m", claro2, err2)
                          && claro2 == "EN-CLARO",
                      "cifrar: y sigue siendo el mismo valor");
            // Lo ya cifrado NO se toca: no se sabe con que clave esta.
            comprobar(ST::cifraLoQueFalte(dirC, "otra", av2), "cifrar: segunda pasada con otra clave");
            auto tras3 = ST::leerConfig(dirC, av2);
            igual(tras3["connections"].toArray().at(0)["password"].toString(), guardado2,
                  "cifrar: lo ya cifrado se deja como esta");
            comprobar(!ST::cifraLoQueFalte(dirC, "", av2), "cifrar: sin maestra no se hace nada");
            comprobar(av2.motivo == ST::Motivo::ClaveMaestraRequerida, "cifrar: y se dice por que");
            std::filesystem::remove_all(dirC);
        }

        // --- Borrar: de los DOS ficheros, o la conexion RESUCITA.
        //
        // Desde que una entrada huerfana del almacen se convierte en conexion, dejar la
        // suya atras no es suciedad: es que vuelve a la lista en el siguiente arranque.
        // Medido en el interprete antes de arreglarlo, con «oldlau».
        {
            zfsmgr::base::ConnectionProfile conTls;
            conTls.id = "paraborrar"; conTls.name = "ParaBorrar"; conTls.connType = "SSH";
            conTls.host = "h"; conTls.username = "u";
            conTls.daemonTlsServerCertPem = "CERT";
            comprobar(ST::guardaPerfil(dirG, conTls, maestra, av), "borrar: se prepara con TLS");
            auto t = ST::leerTrustStore(dirG, av);
            bool estaEnElAlmacen = false;
            for (const auto& v : t["connections"].toArray()) {
                if (v["id"].toString() == "paraborrar") estaEnElAlmacen = true;
            }
            comprobar(estaEnElAlmacen, "borrar: y esta en el almacen");

            comprobar(ST::borraPerfil(dirG, "paraborrar", av), "borrar: se borra");
            auto cfgTrasBorrar = ST::leerConfig(dirG, av);
            for (const auto& v : cfgTrasBorrar["connections"].toArray()) {
                comprobar(v["id"].toString() != "paraborrar", "borrar: fuera de config.json");
            }
            t = ST::leerTrustStore(dirG, av);
            for (const auto& v : t["connections"].toArray()) {
                comprobar(v["id"].toString() != "paraborrar",
                          "borrar: y FUERA del almacen, o resucitaria");
            }
            // Y borrar una que no esta se dice, no se calla.
            comprobar(!ST::borraPerfil(dirG, "no-existe", av), "borrar: una que no esta falla");
            comprobar(av.motivo == ST::Motivo::NoSeGuardaConexion, "borrar: con su motivo");
            comprobar(!ST::borraPerfil(dirG, "  ", av), "borrar: sin identificador tampoco");
            comprobar(av.motivo == ST::Motivo::IdVacio, "borrar: y ese motivo es otro");
        }

        // Un perfil sin identificador no se guarda: sustituirlo o anadirlo seria adivinar.
        zfsmgr::base::ConnectionProfile sinId;
        sinId.name = "X";
        comprobar(!ST::guardaPerfil(dirG, sinId, maestra, av), "guardar: sin id no se guarda");
        comprobar(av.motivo == ST::Motivo::IdVacio, "guardar: con su motivo");
        std::filesystem::remove_all(dirG);
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

        {
            // La lista se comprueba POR CONTENIDO y no por tamaño: un número solo dice que
            // alguien la tocó, no si lo que metió tenía sentido.
            //
            // Seis son necesarias —sin ellas hay funciones que no se pueden ofrecer— y dos
            // son para ELEGIR: `zstd` y `gzip` deciden el códec de una transferencia. Esas
            // dos se sondeaban aparte, con cuatro viajes por SSH cada vez que se abría el
            // diálogo de sincronizar; aquí salen gratis del refresco que ya se hace.
            const auto set = R::zfsmgrUnixCommandSet();
            const std::vector<std::string> esperado = {"zfs",  "zpool", "rsync", "tar",
                                                       "ssh",  "sh",    "zstd",  "gzip"};
            comprobar(set == esperado, "la lista de herramientas es la que se espera");
            // Y sigue siendo corta a propósito: cada una es una respuesta que el agente tiene
            // que dar en cada refresco.
            comprobar(set.size() <= 10, "la lista de herramientas no ha crecido sin control");
        }
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
#ifdef _WIN32
            // `kill(pid, 0)` es POSIX. En Windows el destructor mata al hijo por otro
            // camino —TerminateProcess—, y comprobarlo pide la API de procesos de allí;
            // lo que se puede afirmar aquí sin ella es que hubo hijo.
            comprobar(pid > 0, "ChildProcess: hubo hijo");
#else
            comprobar(pid > 0 && ::kill(static_cast<pid_t>(pid), 0) != 0,
                      "ChildProcess: el destructor mata al hijo");
#endif
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

    {
        // Qué sistema corre en el otro extremo.
        //
        // Los casos vienen de ficheros REALES, no inventados: el de Arch es el de
        // `unib.local`, y es el que destapó el defecto. El guion de shell al que esto
        // sustituye —`printf "%s %s" "$NAME" "$VERSION_ID"`— devolvía «Arch Linux » con un
        // espacio de cola, porque Arch no trae `VERSION_ID`, y ese espacio llegaba a la
        // ficha de la conexión.
        namespace SO = zfsmgr::base::sistemaoperativo;
        igual(SO::deOsRelease("NAME=\"Fedora Linux\"\nVERSION_ID=42\n"), "Fedora Linux 42",
              "so: nombre entre comillas y versión suelta");
        igual(SO::deOsRelease("NAME=\"Arch Linux\"\nID=arch\n"), "Arch Linux",
              "so: sin VERSION_ID no queda espacio de cola (Arch, comprobado en vivo)");
        igual(SO::deOsRelease("NAME='Gentoo'\n"), "Gentoo",
              "so: las comillas simples también se quitan, que el formato las admite");
        igual(SO::deOsRelease("NAME=\"Ubuntu\"\r\nVERSION_ID=\"26.04\"\r\n"), "Ubuntu 26.04",
              "so: el retorno de carro no se cuela dentro de la versión");
        igual(SO::deOsRelease("PRETTY_NAME=\"Algo\"\n"), "",
              "so: sin NAME devuelve vacío, para que quien llame ponga su respaldo");
        igual(SO::deOsRelease(""), "", "so: fichero vacío, nada que decir");
        // `VERSION` a secas NO es `VERSION_ID`, y el prefijo es el mismo hasta la coma.
        igual(SO::deOsRelease("NAME=\"Debian\"\nVERSION=\"12 (bookworm)\"\n"), "Debian",
              "so: VERSION no se confunde con VERSION_ID");

        igual(SO::deSystemProfiler("Software:\n\n    System Software Overview:\n\n"
                                   "      System Version: macOS 15.5 (24F74)\n"
                                   "      Kernel Version: Darwin 24.5.0\n"),
              "macOS 15.5 (24F74)", "so: la versión de macOS sale de su línea");
        igual(SO::deSystemProfiler("      System Version: uno\n      System Version: dos\n"),
              "uno", "so: la primera, como hacía el head -1");
        igual(SO::deSystemProfiler("Kernel Version: Darwin 24.5.0\n"), "",
              "so: sin la línea buena, vacío");
    }

    {
        // Matar el árbol de procesos al cancelar.
        //
        // La cadena que motiva esto es la real de una transferencia:
        // sh(100) -> sudo(101) -> sh(102) -> zfsmgr-agent(103) -> tar(104). Lo que fallaba
        // antes era quedarse en los hijos directos y dejar vivo el `tar`.
        namespace P = zfsmgr::base;
        const std::string ps =
            "  1 0\n"
            "100 1\n"
            "101 100\n"
            "102 101\n"
            "103 102\n"
            "104 103\n"
            "200 1\n";
        const auto d = P::descendientesDe(100, ps);
        comprobar(d == std::vector<long long>{104, 103, 102, 101},
                  "arbol: la descendencia entera, de hojas a raíz");
        comprobar(P::descendientesDe(104, ps).empty(), "arbol: una hoja no tiene descendencia");
        comprobar(P::descendientesDe(200, ps).empty(), "arbol: un proceso suelto tampoco");
        comprobar(P::descendientesDe(99999, ps).empty(), "arbol: un pid que no está, nada");

        // Más de ocho niveles: el guion de shell que esto sustituye se paraba ahí.
        std::string hondo;
        for (int i = 1; i <= 15; ++i) {
            hondo += std::to_string(1000 + i) + " " + std::to_string(1000 + i - 1) + "\n";
        }
        const auto h = P::descendientesDe(1000, hondo);
        comprobar(h.size() == 15, "arbol: quince niveles, no se para en el octavo");
        igual(std::to_string(h.front()), "1015", "arbol: y el más hondo va el primero");

        // Dos ramas del mismo padre: las dos caen, y el nivel de abajo antes que el de arriba.
        const auto r = P::descendientesDe(1, "1 0\n10 1\n11 1\n20 10\n");
        comprobar(r.size() == 3, "arbol: las dos ramas");
        igual(std::to_string(r.front()), "20", "arbol: el nieto antes que los hijos");

        // Un ciclo no cuelga. No pasa en un árbol de procesos de verdad, pero la salida de
        // `ps` la escribe otro programa y no se le cree a ciegas.
        const auto c = P::descendientesDe(5, "5 6\n6 5\n");
        comprobar(c.size() == 1, "arbol: un ciclo se recorre una vez y para");

        comprobar(P::descendientesDe(1, "cabecera que no son numeros\n\n").empty(),
                  "arbol: una salida que no es la esperada no inventa procesos");
    }

    {
        // Con quién puede hablar el daemon de una máquina.
        namespace PE = zfsmgr::base::peers;

        // El listado. La línea SELF es la que faltaba, y su ausencia es un fallo mudo: sin
        // ella la nivelación GSA contra un dataset de la propia máquina no se ejecuta nunca.
        const PE::Vista v = PE::analiza("SELF\tlocal\n"
                                        "unibody\tunib.local\t47653\n"
                                        "oldlau\toldlau.local\t47653\n");
        igual(v.self, "local", "peers: quién dice ser la máquina");
        comprobar(v.pares.size() == 2, "peers: los dos pares");
        igual(v.pares[0].id, "unibody", "peers: el primero");
        igual(std::to_string(v.pares[1].puerto), "47653", "peers: el puerto llega como número");

        // Un daemon anterior a este cambio NO emite la línea SELF. Tiene que leerse igual, con
        // el self vacío, en vez de descuadrar la tabla.
        const PE::Vista vieja = PE::analiza("unibody\tunib.local\t47653\n");
        comprobar(vieja.self.empty(), "peers: sin SELF, vacío y no un par inventado");
        comprobar(vieja.pares.size() == 1, "peers: y el par se lee igual");

        comprobar(PE::analiza("").pares.empty(), "peers: salida vacía, ningún par");

        // La composición de la entrega.
        auto perfil = [](const std::string& id, bool conTls) {
            zfsmgr::base::ConnectionProfile p;
            p.id = id;
            p.name = id;
            p.host = id + ".local";
            p.connType = "SSH";
            if (conTls) {
                p.daemonTlsServerCertPem = "S";
                p.daemonTlsClientCertPem = "C";
                p.daemonTlsClientKeyPem = "K";
            }
            return p;
        };
        const std::vector<zfsmgr::base::ConnectionProfile> tres = {
            perfil("local", true), perfil("unibody", true), perfil("oldlau", true)};

        const PE::Entrega e1 = PE::componeEntrega(tres, "local");
        comprobar(e1.sePuede(), "peers: se puede entregar");
        comprobar(e1.nombres.size() == 2, "peers: van las OTRAS dos, no la de destino");
        comprobar(!e1.cargaB64.empty(), "peers: hay carga");
        {
            // La carga tiene que llevar `self` con el nombre del DESTINO: es lo único que esa
            // máquina no puede averiguar por su cuenta.
            std::string json;
            comprobar(zfsmgr::base::base64Decode(e1.cargaB64, json), "peers: la carga es base64");
            comprobar(json.find("\"self\"") != std::string::npos, "peers: la carga trae self");
            comprobar(json.find("local") != std::string::npos, "peers: y es el destino");
            comprobar(json.find("\"local\",\"") == std::string::npos
                          || json.find("unibody") != std::string::npos,
                      "peers: con los pares dentro");
        }

        // Sin material TLS no hay nada que entregar, y el motivo se distingue de «no hay otras».
        const std::vector<zfsmgr::base::ConnectionProfile> sinTls = {perfil("local", true),
                                                                    perfil("unibody", false)};
        igual(PE::etiquetaDe(PE::componeEntrega(sinTls, "local").fallo),
              PE::etiquetaDe(PE::Fallo::SinMaterialTls),
              "peers: las hay pero sin certificados");
        igual(PE::etiquetaDe(PE::componeEntrega({perfil("local", true)}, "local").fallo),
              PE::etiquetaDe(PE::Fallo::SinOtrasConexiones),
              "peers: no hay ninguna otra");

        // Las direcciones de escucha son tres y no más: el cliente llega por un túnel contra
        // 127.0.0.1 y una dirección suelta le cortaría el acceso.
        comprobar(PE::direccionDeEscuchaValida("127.0.0.1"), "peers: el bucle local vale");
        comprobar(PE::direccionDeEscuchaValida("0.0.0.0"), "peers: el comodín IPv4 vale");
        comprobar(PE::direccionDeEscuchaValida("::"), "peers: el comodín IPv6 vale");
        comprobar(!PE::direccionDeEscuchaValida("192.168.1.5"),
                  "peers: una dirección suelta NO, cortaría el túnel");
        comprobar(!PE::direccionDeEscuchaValida(""), "peers: vacía tampoco");
    }

    {
        // Las cuatro acciones que mueven contenido: Desglosar, Ensamblar, Hacia Dir.
        namespace AV = zfsmgr::commands::avanzadas;

        // **La regla que costó descubrir ejecutando.** El agente comprueba cada hijo con
        // `zfs list <hijo>`: un nombre relativo no existe para él, y la operación se saldaba
        // con «ya absorbido» y rc=0 —decía que sí sin hacer nada—.
        igual(AV::hijoConNombreCompleto("tank/datos", "fotos"), "tank/datos/fotos",
              "avanzadas: un relativo se completa");
        igual(AV::hijoConNombreCompleto("tank/datos", "tank/datos/fotos"), "tank/datos/fotos",
              "avanzadas: uno completo se respeta");
        // Un nieto ya lleva barra: completarlo otra vez daría «tank/datos/tank/datos/…».
        igual(AV::hijoConNombreCompleto("tank/datos", "tank/datos/fotos/2024"),
              "tank/datos/fotos/2024", "avanzadas: un nieto no se vuelve a completar");
        igual(AV::hijoConNombreCompleto("tank/datos", "  fotos  "), "tank/datos/fotos",
              "avanzadas: se recortan los espacios");
        igual(AV::hijoConNombreCompleto("tank/datos", ""), "", "avanzadas: vacío sigue vacío");

        {
            const auto a1 = AV::argvEnsamblar("tank/datos", {"fotos", "tank/datos/musica"});
            comprobar(a1 == std::vector<std::string>{"--mutate-advanced-assemble", "tank/datos",
                                                     "tank/datos/fotos", "tank/datos/musica"},
                      "avanzadas: ensamblar completa unos y respeta otros");
            comprobar(AV::argvEnsamblar("tank/datos", {}).empty(),
                      "avanzadas: ensamblar sin hijos no manda nada");
            comprobar(AV::argvEnsamblar("tank/datos", {"", "  "}).empty(),
                      "avanzadas: hijos vacíos tampoco cuentan");
            comprobar(AV::argvEnsamblar("", {"fotos"}).empty(),
                      "avanzadas: sin dataset no hay orden");
        }

        {
            const auto d = AV::argvDesglosar("tank/datos", {{"fotos", "fotos"}, {"cine", "cine"}});
            comprobar(d == std::vector<std::string>{"--mutate-advanced-breakdown", "tank/datos",
                                                    "fotos", "fotos", "cine", "cine"},
                      "avanzadas: desglosar empareja subdirectorio y dataset");
            // Un par a medias desplazaría TODOS los siguientes: el verbo los lee de dos en
            // dos y el daemon acabaría creando un dataset con el nombre de un directorio.
            const auto medio = AV::argvDesglosar("tank/datos", {{"fotos", ""}, {"cine", "cine"}});
            comprobar(medio == std::vector<std::string>{"--mutate-advanced-breakdown", "tank/datos",
                                                        "cine", "cine"},
                      "avanzadas: un par a medias se descarta entero, no a medias");
            comprobar(AV::argvDesglosar("tank/datos", {}).empty(),
                      "avanzadas: desglosar sin pares no manda nada");
            comprobar(AV::argvDesglosar("tank/datos", {{"fotos", ""}}).empty(),
                      "avanzadas: si el único par está a medias, tampoco");
        }

        {
            comprobar(AV::rutaDeDestinoValida("/mnt/copia"), "avanzadas: ruta absoluta Unix");
            comprobar(AV::rutaDeDestinoValida("Z:/copia"), "avanzadas: ruta con unidad Windows");
            comprobar(!AV::rutaDeDestinoValida("copia"),
                      "avanzadas: una relativa NO, el daemon la abriría desde su propio sitio");
            comprobar(!AV::rutaDeDestinoValida(""), "avanzadas: vacía tampoco");

            const auto t = AV::argvHaciaDir("tank/datos", "/mnt/copia", false);
            comprobar(t == std::vector<std::string>{"--mutate-advanced-todir", "tank/datos",
                                                    "/mnt/copia", "0"},
                      "avanzadas: hacia dir sin destruir el origen");
            const auto t2 = AV::argvHaciaDir("tank/datos", "/mnt/copia", true);
            igual(t2.back(), "1", "avanzadas: y con destrucción del origen es «1»");
            comprobar(AV::argvHaciaDir("tank/datos", "relativa", false).empty(),
                      "avanzadas: con una ruta que no sirve no se manda nada");
        }

        {
            // Desde Dir: dónde cae cada origen dentro del dataset.
            using O = AV::OrigenDesdeDir;

            // Uno solo: el dataset ES el directorio, así que su contenido va a la raíz.
            const auto uno = AV::subdirectoriosDeDestino({O{"/home/ana/docs", "fc16", false}});
            comprobar(uno == std::vector<std::string>{""},
                      "desdedir: un solo origen va a la raíz del dataset");

            // Varios con nombres distintos: cada uno al suyo.
            const auto varios = AV::subdirectoriosDeDestino(
                {O{"/home/ana/docs", "fc16", false}, O{"/home/ana/fotos", "fc16", false}});
            comprobar(varios == std::vector<std::string>{"docs", "fotos"},
                      "desdedir: varios orígenes, cada uno a su subdirectorio");

            // Mismo nombre en máquinas distintas: desempata la máquina.
            const auto dosMaquinas = AV::subdirectoriosDeDestino(
                {O{"/home/ana/docs", "fc16", false}, O{"/home/ana/docs", "unibody", false}});
            comprobar(dosMaquinas == std::vector<std::string>{"fc16-docs", "unibody-docs"},
                      "desdedir: el mismo nombre en dos máquinas se separa por máquina");

            // **El fallo que esto arregla.** Dos directorios con el mismo nombre en la MISMA
            // máquina daban los dos «fc16-docs»: el segundo tar se extraía encima del
            // primero y se perdía contenido sin decir nada.
            const auto mismaMaquina = AV::subdirectoriosDeDestino(
                {O{"/a/docs", "fc16", false}, O{"/b/docs", "fc16", false}});
            comprobar(mismaMaquina.size() == 2 && mismaMaquina[0] != mismaMaquina[1],
                      "desdedir: dos con el mismo nombre en la misma máquina NO se pisan");
            comprobar(mismaMaquina == std::vector<std::string>{"fc16-docs", "fc16-docs-2"},
                      "desdedir: el segundo lleva sufijo");

            // Windows: los separadores son «\\».
            const auto win = AV::subdirectoriosDeDestino(
                {O{"C:\\Users\\ana\\docs", "oldlau", true}, O{"/home/ana/fotos", "fc16", false}});
            comprobar(win == std::vector<std::string>{"docs", "fotos"},
                      "desdedir: una ruta de Windows también deja su último tramo");

            // Barras finales: «/home/ana/docs/» es el mismo directorio.
            const auto conBarra = AV::subdirectoriosDeDestino(
                {O{"/home/ana/docs///", "fc16", false}, O{"/home/ana/fotos", "fc16", false}});
            igual(conBarra[0], "docs", "desdedir: las barras finales no cuentan");

            // Una raíz no deja nombre detrás. Sin esto ese origen se iría a la raíz del
            // dataset mientras los demás van a su subdirectorio.
            const auto raiz = AV::subdirectoriosDeDestino(
                {O{"/", "fc16", false}, O{"/home/ana/fotos", "fc16", false}});
            comprobar(!raiz[0].empty() && raiz[0] != raiz[1],
                      "desdedir: una ruta sin último tramo tampoco se va a la raíz");

            // Un nombre de conexión con una barra dentro habría creado un nivel de más.
            const auto sucio = AV::subdirectoriosDeDestino(
                {O{"/a/docs", "casa/fc16", false}, O{"/b/docs", "casa/fc16", false}});
            comprobar(sucio[0].find('/') == std::string::npos,
                      "desdedir: el nombre resultante no lleva separadores");

            // Y «..» no puede salir del dataset.
            const auto fuera = AV::subdirectoriosDeDestino(
                {O{"/home/ana/..", "fc16", false}, O{"/home/ana/fotos", "fc16", false}});
            comprobar(fuera[0].find("..") == std::string::npos
                          || AV::subdirectorioRelativoValido(fuera[0]),
                      "desdedir: no se compone un destino que salga del dataset");
        }

        {
            // Subárboles de ficheros: `#content/{a,b}`.
            comprobar(AV::rutasDeContenido("") == std::vector<std::string>{""},
                      "contenido: sin ruta es el árbol entero");
            comprobar(AV::rutasDeContenido("sub") == std::vector<std::string>{"sub"},
                      "contenido: una ruta suelta");
            comprobar(AV::rutasDeContenido("{a,b,dir}")
                          == std::vector<std::string>{"a", "b", "dir"},
                      "contenido: las llaves se expanden");
            comprobar(AV::rutasDeContenido("docs/{2024,2025}")
                          == std::vector<std::string>{"docs/2024", "docs/2025"},
                      "contenido: con prefijo delante");
            comprobar(AV::rutasDeContenido(" {a , b } ")
                          == std::vector<std::string>{"a", "b"},
                      "contenido: se recortan los espacios de cada pieza");
            // Lo mal escrito NO se adivina: devolver algo a medias sincronizaría una parte
            // distinta de la que se pidió, y con `--delete` eso borra.
            comprobar(AV::rutasDeContenido("{a,b").empty(), "contenido: sin cerrar, nada");
            comprobar(AV::rutasDeContenido("a,b}").empty(), "contenido: cierre suelto, nada");
            comprobar(AV::rutasDeContenido("{a,{b,c}}").empty(), "contenido: anidadas, nada");
            comprobar(AV::rutasDeContenido("{a,,b}").empty(), "contenido: pieza vacía, nada");
            comprobar(AV::rutasDeContenido("{}").empty(), "contenido: llaves vacías, nada");
            comprobar(AV::rutasDeContenido("{a,b}/{c,d}").empty(),
                      "contenido: dos grupos, nada");

            // Y lo que no puede salir del árbol. Quien lo ejecuta corre como root, y el
            // daemon NO lo comprueba para rsync: solo exige que la ruta sea absoluta.
            comprobar(AV::rutaDeContenidoValida(""), "contenido: la raíz vale");
            comprobar(AV::rutaDeContenidoValida("a/b"), "contenido: una relativa vale");
            comprobar(!AV::rutaDeContenidoValida("/etc"), "contenido: absoluta no");
            comprobar(!AV::rutaDeContenidoValida("../fuera"), "contenido: «..» no");
            comprobar(!AV::rutaDeContenidoValida("sub/../../etc"),
                      "contenido: «..» en medio tampoco");
        }

        {
            // El subdirectorio, comprobado ANTES de abrir la tubería y no después, como
            // hacía el daemon: para cuando él lo miraba, el tar ya estaba corriendo.
            comprobar(AV::subdirectorioRelativoValido(""),
                      "desdedir: vacío vale, es la raíz del dataset");
            comprobar(AV::subdirectorioRelativoValido("copia/2026"),
                      "desdedir: un relativo con niveles vale");
            comprobar(!AV::subdirectorioRelativoValido("/copia"),
                      "desdedir: absoluto no es «dentro del dataset»");
            comprobar(!AV::subdirectorioRelativoValido("../fuera"),
                      "desdedir: «..» saldría del punto de montaje");
            comprobar(!AV::subdirectorioRelativoValido("con\ttabulador"),
                      "desdedir: un tabulador rompe el registro y la vista previa");

            const auto fd = AV::argvDesdeDir("tank/datos", "copia");
            comprobar(fd == std::vector<std::string>{"--mutate-advanced-fromdir", "tank/datos",
                                                     "copia"},
                      "desdedir: el argv lleva dataset y subdirectorio");
            // Sin subdirectorio NO se manda una cadena vacía detrás: el verbo lo trata como
            // opcional y una vacía le pide que decida qué significa.
            const auto fdRaiz = AV::argvDesdeDir("tank/datos", "");
            comprobar(fdRaiz == std::vector<std::string>{"--mutate-advanced-fromdir", "tank/datos"},
                      "desdedir: a la raíz se manda solo el dataset");
            comprobar(AV::argvDesdeDir("tank/datos", "../fuera").empty(),
                      "desdedir: con un subdirectorio que no vale no se manda nada");
            comprobar(AV::argvDesdeDir("", "copia").empty(),
                      "desdedir: sin dataset tampoco");

            // La primera mitad, la que permite hacerlo sin tubería de shell.
            const auto prep = AV::argvDesdeDirPreparar("tank/datos", "copia/2026");
            comprobar(prep == std::vector<std::string>{"--mutate-advanced-fromdir-prepare",
                                                       "tank/datos", "copia/2026"},
                      "desdedir: el argv de preparar el destino");
            comprobar(AV::argvDesdeDirPreparar("tank/datos", "../fuera").empty(),
                      "desdedir: preparar tampoco acepta salir del dataset");

            igual(AV::rutaPreparada("DST=/tpool/datos/copia/2026\n"), "/tpool/datos/copia/2026",
                  "desdedir: se lee la ruta preparada");
            // Por SSH la respuesta puede venir con un aviso delante: quedarse con la primera
            // línea daría una ruta que no es.
            igual(AV::rutaPreparada("Warning: algo\nDST=/tpool/x\n"), "/tpool/x",
                  "desdedir: la ruta se busca por su línea, no al principio");
            comprobar(AV::rutaPreparada("PORT=1234\n").empty(),
                      "desdedir: sin línea DST no hay ruta");

            // Las dos puntas con daemon, no una. Al camino del tar le basta con el destino.
            comprobar(AV::puedeIrPorElArbol(true, true), "desdedir: con daemon en las dos, árbol");
            comprobar(!AV::puedeIrPorElArbol(false, true),
                      "desdedir: sin daemon en el origen, no hay quien envíe");
            comprobar(!AV::puedeIrPorElArbol(true, false),
                      "desdedir: sin daemon en el destino, no hay quien escuche");
        }
    }

    {
        // Lo que se le PIDE al agente: una función por cosa, para que el nombre del verbo no
        // se escriba en tres clientes distintos.
        namespace PE2 = zfsmgr::commands::peticiones;

        comprobar(PE2::listaDePools() == std::vector<std::string>{"--dump-zpool-list"},
                  "peticiones: la lista de pools no lleva argumentos");
        comprobar(PE2::estadoDePool("tank")
                      == std::vector<std::string>{"--dump-zpool-status", "tank"},
                  "peticiones: el estado de un pool");

        // **Un verbo pelado NO se manda.** El daemon contestaría con su línea de uso y rc=2,
        // que es un error mucho peor de leer que no haber preguntado.
        comprobar(PE2::estadoDePool("").empty(), "peticiones: sin pool no se pregunta");
        comprobar(PE2::estadoDePool("   ").empty(), "peticiones: y los espacios no cuentan");

        // El orden es propiedad y luego objeto, que es al revés de como se dice en voz alta.
        comprobar(PE2::propiedadDeDataset("mountpoint", "tank/datos")
                      == std::vector<std::string>{"--dump-zfs-get-prop", "mountpoint",
                                                  "tank/datos"},
                  "peticiones: una propiedad va antes que su objeto");
        comprobar(PE2::propiedadDeDataset("", "tank/datos").empty(),
                  "peticiones: sin propiedad tampoco");

        // Los holds aceptan varios objetos detrás.
        comprobar(PE2::holdsDe({"tank@a", "tank@b"})
                      == std::vector<std::string>{"--dump-zfs-holds", "tank@a", "tank@b"},
                  "peticiones: varios objetos en una sola llamada");
        comprobar(PE2::holdsDe({}).empty(), "peticiones: sin objetos no hay holds que leer");
        comprobar(PE2::holdsDe({"", "  "}).empty(),
                  "peticiones: y unos objetos vacíos no son objetos");

        // El registro sin número pide desde el principio, y **no** manda una cadena vacía
        // detrás: no es lo mismo que no mandar nada.
        // Son BYTES, no líneas: el daemon hace `seek`. Cero y cero es el fichero entero, y
        // entonces no se mandan los argumentos.
        comprobar(PE2::registro(0, 0) == std::vector<std::string>{"--dump-daemon-log"},
                  "peticiones: el registro entero no lleva argumentos");
        comprobar(PE2::registro(0, 4096)
                      == std::vector<std::string>{"--dump-daemon-log", "0", "4096"},
                  "peticiones: con tope de bytes van los dos");

        {
            // Encolar pone el verbo DELANTE, no detrás.
            const auto e = PE2::encola({"--mutate-advanced-todir", "tank/d", "/mnt/x", "0"});
            comprobar(e == std::vector<std::string>{"--job-submit", "--mutate-advanced-todir",
                                                    "tank/d", "/mnt/x", "0"},
                      "peticiones: --job-submit va delante de lo que encola");
            // Y lo que el daemon no sabe encolar no se manda: rebotaría.
            comprobar(PE2::encola({"--mutate-zfs-destroy", "tank@a"}).empty(),
                      "peticiones: un verbo no encolable no se encola");
            comprobar(PE2::encola({}).empty(), "peticiones: nada que encolar, nada que mandar");
            comprobar(PE2::sePuedeEncolar("--tree-send-to-peer"),
                      "peticiones: el árbol entre daemons sí se encola");
            comprobar(!PE2::sePuedeEncolar("--dump-zpool-list"),
                      "peticiones: una lectura no es un trabajo");
        }
    }

    {
        // Mantenimiento de pools.
        namespace PL = zfsmgr::commands::pools;
        using Op = PL::Operacion;
        using Fase = PL::Fase;

        // **La regla que más cuesta ver**: «parar» y «pausar» NO son la misma letra.
        // `-s` significa PARAR en scrub y SUSPENDER en initialize. Un cliente que use la
        // misma para las tres pone un botón que dice una cosa y hace otra; pasó.
        comprobar(PL::argv(Op::Scrub, "tank", Fase::Parar)
                      == std::vector<std::string>{"scrub", "-s", "tank"},
                  "pools: parar un scrub es -s");
        comprobar(PL::argv(Op::Scrub, "tank", Fase::Pausar)
                      == std::vector<std::string>{"scrub", "-p", "tank"},
                  "pools: pausar un scrub es -p");
        comprobar(PL::argv(Op::Initialize, "tank", Fase::Parar)
                      == std::vector<std::string>{"initialize", "-c", "tank"},
                  "pools: parar un initialize es -c, NO -s");
        comprobar(PL::argv(Op::Initialize, "tank", Fase::Pausar)
                      == std::vector<std::string>{"initialize", "-s", "tank"},
                  "pools: y suspenderlo sí es -s");
        comprobar(PL::argv(Op::Trim, "tank", Fase::Parar)
                      == std::vector<std::string>{"trim", "-c", "tank"},
                  "pools: trim se comporta como initialize, no como scrub");

        // El orden: banderas, pool, discos. Las dos mitades vienen de verlo fallar.
        comprobar(PL::argv(Op::Trim, "tank", Fase::Arrancar, {"-r", "100M"}, {"sda", "sdb"})
                      == std::vector<std::string>{"trim", "-r", "100M", "tank", "sda", "sdb"},
                  "pools: banderas antes del pool y discos después");

        // Pedir una fase a algo que no la admite no se manda: `zpool export -s` es un error
        // de sintaxis, y vale más no mandarlo que traducir la queja de zpool.
        comprobar(PL::argv(Op::Export, "tank", Fase::Parar).empty(),
                  "pools: export no admite fase, así que no se manda nada");
        comprobar(PL::argv(Op::Scrub, "").empty(), "pools: sin pool no hay orden");

        // Qué se confirma, y por qué no es solo «lo que destruye».
        comprobar(PL::esIrreversible(Op::Destroy), "pools: destroy no se deshace");
        comprobar(PL::esIrreversible(Op::Upgrade), "pools: upgrade tampoco");
        comprobar(PL::esIrreversible(Op::Reguid), "pools: ni reguid");
        comprobar(!PL::esIrreversible(Op::Clear), "pools: clear sí se deshace… pero");
        comprobar(PL::pideConfirmacion(Op::Clear),
                  "pools: …se pregunta igual: borra la cuenta de errores y se teclea sin querer");
        comprobar(PL::pideConfirmacion(Op::Export),
                  "pools: y export, porque el pool desaparece de esa máquina");
        comprobar(!PL::pideConfirmacion(Op::Scrub), "pools: un scrub no necesita permiso");

        // Importar con otro nombre.
        comprobar(PL::argvImportarComo("viejo", "nuevo")
                      == std::vector<std::string>{"import", "viejo", "nuevo"},
                  "pools: importar renombrando");
        comprobar(PL::nombreDePoolValido("tank2"), "pools: nombre válido");
        comprobar(!PL::nombreDePoolValido("9tank"), "pools: no puede empezar por dígito");
        comprobar(!PL::nombreDePoolValido("con/barra"), "pools: la barra haría un dataset");
        comprobar(!PL::nombreDePoolValido("con espacio"), "pools: ni espacios");
        comprobar(PL::argvImportarComo("viejo", "9malo").empty(),
                  "pools: con un nombre inválido no se manda nada");
    }

    {
        // Instantáneas.
        namespace IN = zfsmgr::commands::instantaneas;
        using Al = IN::Alcance;

        comprobar(IN::esInstantanea("tank/d@ayer"), "inst: con arroba lo es");
        comprobar(!IN::esInstantanea("tank/d"), "inst: sin arroba no");
        igual(IN::nombreDeInstantanea("tank/d", "ayer"), "tank/d@ayer", "inst: se compone");
        igual(IN::nombreDeInstantanea("tank/d", "otro@ayer"), "otro@ayer",
              "inst: si ya trae arroba, se respeta");

        // El alcance, con nombres en vez de letras sueltas.
        igual(IN::letraDeAlcance(Al::Solo), "", "inst: solo, sin letra");
        igual(IN::letraDeAlcance(Al::Descendientes), "r", "inst: descendientes es r");
        igual(IN::letraDeAlcance(Al::Dependientes), "R", "inst: dependientes es R");
        comprobar(!IN::arrastraOtros(Al::Solo), "inst: solo no arrastra");
        comprobar(IN::arrastraOtros(Al::Dependientes), "inst: dependientes sí");

        comprobar(IN::argvDestruir("tank/d@ayer", false)
                      == std::vector<std::string>{"--mutate-zfs-destroy", "tank/d@ayer", "0", ""},
                  "inst: destruir una instantánea");
        comprobar(IN::argvDestruir("tank/d", true, Al::Dependientes)
                      == std::vector<std::string>{"--mutate-zfs-destroy", "tank/d", "1", "R"},
                  "inst: destruir un dataset con todo lo que dependa");
        comprobar(IN::argvDestruir("", false).empty(), "inst: sin objeto no hay orden");

        // Rollback exige instantánea: volver atrás a un dataset no significa nada.
        comprobar(IN::argvRollback("tank/d@ayer", false, Al::Descendientes)
                      == std::vector<std::string>{"--mutate-zfs-rollback", "tank/d@ayer", "0", "r"},
                  "inst: rollback con descendientes");
        comprobar(IN::argvRollback("tank/d", false).empty(),
                  "inst: rollback sobre un dataset no se manda");

        comprobar(IN::argvClonar("tank/d@ayer", "tank/copia")
                      == std::vector<std::string>{"--mutate-zfs-clone", "tank/d@ayer", "tank/copia"},
                  "inst: clonar");
        comprobar(IN::argvClonar("tank/d", "tank/copia").empty(),
                  "inst: clonar exige que el origen sea instantánea");
        comprobar(IN::argvClonar("tank/d@ayer", "tank/copia@x").empty(),
                  "inst: y que el destino NO lo sea");

        // **La etiqueta va primero.** Invertirlos no da error: `zfs hold` acepta dos cadenas
        // cualesquiera y falla luego diciendo que no encuentra la instantánea «micopia».
        comprobar(IN::argvRetener("micopia", "tank/d@ayer")
                      == std::vector<std::string>{"--mutate-zfs-hold", "micopia", "tank/d@ayer"},
                  "inst: retener, etiqueta primero");
        comprobar(IN::argvSoltar("micopia", "tank/d@ayer")
                      == std::vector<std::string>{"--mutate-zfs-release", "micopia", "tank/d@ayer"},
                  "inst: soltar, igual");
        // El verbo tipado NO admite -r; para eso está la forma genérica.
        comprobar(IN::argvZfsRetener("micopia", "tank/d@ayer", true)
                      == std::vector<std::string>{"hold", "-r", "micopia", "tank/d@ayer"},
                  "inst: retener recursivo va por el verbo genérico de zfs");
        comprobar(IN::argvZfsRetener("micopia", "tank/d@ayer", false)
                      == std::vector<std::string>{"hold", "micopia", "tank/d@ayer"},
                  "inst: y sin -r cuando no se pide");

        comprobar(!IN::etiquetaValida("con espacio"), "inst: la etiqueta no lleva espacios");
        comprobar(!IN::etiquetaValida("con@arroba"), "inst: ni arrobas");
        comprobar(!IN::etiquetaValida("con/barra"), "inst: ni barras");
        comprobar(!IN::etiquetaValida(""), "inst: ni vacía");
        comprobar(IN::etiquetaValida("mi-copia_2024.1"), "inst: guiones y puntos sí");
        comprobar(IN::argvRetener("mal etiqueta", "tank/d@ayer").empty(),
                  "inst: con etiqueta inválida no se manda nada");
        comprobar(IN::argvRetener("ok", "tank/d").empty(),
                  "inst: ni sobre algo que no es instantánea");
    }

    {
        // Datasets.
        namespace DS = zfsmgr::commands::datasets;

        // **La regla del renombrado**, que el intérprete aplicaba y el servidor web no: un
        // nombre SIN barra cambia la hoja y deja el dataset donde está. Sin ella, teclear
        // «fotos» manda `zfs rename tank/media/cine fotos` y ZFS responde «cannot create
        // 'fotos': missing dataset name», que no dice qué hay que hacer. Comprobado en vivo.
        igual(DS::nombreDeRenombrado("tank/media/cine", "fotos"), "tank/media/fotos",
              "ds: sin barra, se conserva el padre");
        igual(DS::nombreDeRenombrado("tank/media/cine", "tank/otro/fotos"), "tank/otro/fotos",
              "ds: con barra, se mueve donde diga");
        igual(DS::nombreDeRenombrado("tank", "otro"), "otro",
              "ds: un pool raíz no tiene padre que anteponer");

        comprobar(DS::argvRenombrar("tank/d", "e")
                      == std::vector<std::string>{"rename", "tank/d", "tank/e"},
                  "ds: renombrar dentro del mismo padre");
        comprobar(DS::argvRenombrar("tank/d", "d").empty(),
                  "ds: renombrar a lo mismo no es una orden");
        comprobar(DS::argvRenombrar("tank/d", "con@arroba").empty(),
                  "ds: la arroba haría una instantánea");

        comprobar(DS::nombreValido("tank/d_1-2.3:x"), "ds: nombre corriente");
        comprobar(!DS::nombreValido("tank/"), "ds: no puede acabar en barra");
        comprobar(!DS::nombreValido("/tank"), "ds: ni empezar");
        comprobar(!DS::nombreValido("tank//d"), "ds: ni llevar barra doble");
        comprobar(!DS::nombreValido("con espacio"), "ds: ni espacios");

        igual(DS::nombreDeHijo("tank/d", "sub"), "tank/d/sub", "ds: hijo");
        igual(DS::nombreDeHijo("tank/d", "otro/sub"), "otro/sub", "ds: con barra se respeta");

        comprobar(DS::argvCrear("tank/d") == std::vector<std::string>{"create", "tank/d"},
                  "ds: crear a secas");
        comprobar(DS::argvCrear("tank/a/b/c", {}, true)
                      == std::vector<std::string>{"create", "-p", "tank/a/b/c"},
                  "ds: con -p se crean los intermedios");
        comprobar(DS::argvCrear("tank/d", {"compression=lz4", "atime=off"})
                      == std::vector<std::string>{"create", "-o", "compression=lz4", "-o",
                                                  "atime=off", "tank/d"},
                  "ds: cada propiedad con su -o");
        // Sin «=» no es una propiedad: zfs la leería como el nombre del dataset.
        comprobar(DS::argvCrear("tank/d", {"basura"})
                      == std::vector<std::string>{"create", "tank/d"},
                  "ds: lo que no es prop=valor se descarta");

        comprobar(DS::argvMontar("tank/d", true)
                      == std::vector<std::string>{"mount", "-f", "tank/d"}, "ds: montar forzando");
        comprobar(DS::argvDesmontar("tank/d")
                      == std::vector<std::string>{"unmount", "tank/d"}, "ds: desmontar");
        comprobar(DS::argvPromover("tank/d") == std::vector<std::string>{"promote", "tank/d"},
                  "ds: promover");

        comprobar(DS::argvPonerPropiedad("tank/d", "atime", "off")
                      == std::vector<std::string>{"set", "atime=off", "tank/d"},
                  "ds: poner una propiedad");
        // Un valor vacío es legítimo; una propiedad con «=» dentro no.
        comprobar(DS::argvPonerPropiedad("tank/d", "org.x:nota", "")
                      == std::vector<std::string>{"set", "org.x:nota=", "tank/d"},
                  "ds: el valor sí puede ir vacío");
        comprobar(DS::argvPonerPropiedad("tank/d", "a=b", "c").empty(),
                  "ds: la propiedad no puede llevar «=» dentro");
        comprobar(DS::argvHeredarPropiedad("tank/d", "atime")
                      == std::vector<std::string>{"inherit", "atime", "tank/d"},
                  "ds: heredar");
    }

    {
        // El guion de instalación de macOS: las tres condiciones que faltaban el día que
        // una instalación dijo «daemon instalado» y dejó la máquina sin daemon.
        //
        // No se comprueba «que instale» —eso pide un Mac—, sino que el TEXTO conserve los
        // tres eslabones. Cada uno se rompió por su cuenta y ninguno lo notaba nadie: la
        // instalación seguía saliendo en verde.
        namespace DI = zfsmgr::base::daemoninstall;
        const std::string mac = DI::guionDeInstalacion("macos", "1.2.3", "3");
        comprobar(mac.find("launchctl bootstrap system") != std::string::npos,
                  "macos: el guion arranca el servicio");
        comprobar(mac.find("bootstrap system /Library/LaunchDaemons/org.zfsmgr.agent.plist "
                           ">/dev/null 2>&1 || true")
                      == std::string::npos,
                  "macos: el fallo de «bootstrap» NO se silencia con «|| true»");
        comprobar(mac.find("pid = ") != std::string::npos && mac.find("[ -z \"$pid\" ]") != std::string::npos,
                  "macos: se exige un PID, no solo que el trabajo esté declarado");
        // Entre sacar el viejo y meter el nuevo tiene que haber una espera: `bootout` no es
        // inmediato y `bootstrap` lanzado encima falla.
        const std::size_t bo = mac.find("launchctl bootout");
        const std::size_t bs = mac.find("launchctl bootstrap");
        comprobar(bo != std::string::npos && bs != std::string::npos && bo < bs,
                  "macos: primero sacar, luego meter");
        comprobar(mac.find("|| break", bo) < bs,
                  "macos: se espera a que el trabajo viejo salga del dominio");

        // Una plataforma que no se reconoce cae al guion de systemd A PROPÓSITO —y ese
        // comprueba `systemctl` antes de nada—, así que NO se espera vacío.
        comprobar(DI::guionDeInstalacion("plan9", "1.2.3", "3").find("systemctl")
                      != std::string::npos,
                  "una plataforma desconocida cae al guion de systemd, que se planta solo");
    }

    std::fprintf(stderr, "%d pasados, %d fallos\n", pasados, fallos);
    return fallos == 0 ? 0 : 1;
}
