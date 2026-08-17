#include "transportcmd.h"

#include "daemonpayload.h"
#include "helpers.h"
#include "strutil.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace zfsmgr::base::transport {
namespace {

namespace H = zfsmgr::base::helpers;

// Entero ESTRICTO, como `QString::toInt`: o la cadena entera es un número, o no vale.
// `strtol` a secas aceptaría «47653 basura» y «47653x», y un puerto medio leído es peor
// que ninguno.
bool parseIntStrict(const std::string& s, int& out) {
    const std::string t = trim(s);
    if (t.empty()) {
        return false;
    }
    char* fin = nullptr;
    const long v = std::strtol(t.c_str(), &fin, 10);
    if (!fin || *fin != '\0') {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

// Búsqueda sin distinguir mayúsculas, solo en ASCII. Es equivalente a
// `indexOf(..., Qt::CaseInsensitive)` para lo que se busca aquí —agujas que son ASCII
// puro—, y evita tener que plegar mayúsculas Unicode, que además CAMBIA la longitud en
// bytes de algunos caracteres y desplazaría el índice resultante.
long long indexOfAsciiCI(const std::string& s, const std::string& sub) {
    if (sub.empty() || sub.size() > s.size()) {
        return sub.empty() ? 0 : -1;
    }
    const auto baja = [](unsigned char c) -> unsigned char {
        return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c - 'A' + 'a') : c;
    };
    for (std::size_t i = 0; i + sub.size() <= s.size(); ++i) {
        std::size_t j = 0;
        for (; j < sub.size(); ++j) {
            if (baja(static_cast<unsigned char>(s[i + j])) != baja(static_cast<unsigned char>(sub[j]))) {
                break;
            }
        }
        if (j == sub.size()) {
            return static_cast<long long>(i);
        }
    }
    return -1;
}

// UTF-8 a UTF-16LE en bytes, que es lo que come `powershell -EncodedCommand`.
//
// No es un detalle cosmético: PowerShell descodifica el base64 como UTF-16LE, así que
// mandarle UTF-8 le entrega una orden ilegible. Se hace a mano porque `QString` ya
// guardaba UTF-16 dentro y aquí no hay Qt del que sacarlo.
std::string aUtf16Le(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size() * 2);
    const auto emite = [&out](unsigned int u) {
        out.push_back(static_cast<char>(u & 0xFF));
        out.push_back(static_cast<char>((u >> 8) & 0xFF));
    };
    for (std::size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        unsigned int cp = 0;
        std::size_t n = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1Fu;
            n = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0Fu;
            n = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07u;
            n = 4;
        } else {
            // Byte suelto que no empieza ninguna secuencia. Se sustituye en vez de
            // abortar: una orden con un byte roto debe llegar y fallar en el otro lado
            // con un mensaje, no desaparecer aquí en silencio.
            emite(0xFFFD);
            ++i;
            continue;
        }
        if (i + n > utf8.size()) {
            emite(0xFFFD);
            break;
        }
        for (std::size_t k = 1; k < n; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3Fu);
        }
        i += n;
        if (cp >= 0x10000u) {
            // Fuera del plano básico: pareja suplente, que es como UTF-16 los representa.
            cp -= 0x10000u;
            emite(0xD800u + (cp >> 10));
            emite(0xDC00u + (cp & 0x3FFu));
        } else {
            emite(cp);
        }
    }
    return out;
}

// Quita el prólogo «PATH="…"; export PATH; » del principio, si está.
//
// A mano y no con una expresión regular: el patrón es un ancla al principio y una parte
// entre comillas sin comillas dentro, o sea justo lo que se lee de un tirón. Ver en la
// cabecera de wrapRemoteCommand por qué hay que quitarlo.
void quitaPrologoPathUnix(std::string& s) {
    static const std::string kIni = "PATH=\"";
    if (s.rfind(kIni, 0) != 0) {
        return;
    }
    const std::size_t cierre = s.find('"', kIni.size());
    if (cierre == std::string::npos) {
        return;
    }
    std::size_t i = cierre + 1;
    if (i >= s.size() || s[i] != ';') {
        return;
    }
    ++i;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    static const std::string kExport = "export PATH;";
    if (s.compare(i, kExport.size(), kExport) != 0) {
        return;
    }
    i += kExport.size();
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    s.erase(0, i);
}

std::string stripConfigQuotes(const std::string& v) {
    const std::string t = trim(v);
    if (t.size() >= 2) {
        const char a = t.front();
        const char b = t.back();
        if ((a == '\'' && b == '\'') || (a == '"' && b == '"')) {
            return t.substr(1, t.size() - 2);
        }
    }
    return t;
}

int parseConfigInt(const std::string& v, int fallback) {
    int parsed = 0;
    return parseIntStrict(stripConfigQuotes(v), parsed) ? parsed : fallback;
}

}  // namespace

bool isLocalConnection(const ConnectionProfile& p) {
    return toLowerAscii(p.connType) == "local";
}

bool isWindowsConnection(const ConnectionProfile& p) {
    return H::isWindowsOsType(p.osType);
}

std::string remoteDaemonTlsCacheKey(const ConnectionProfile& p) {
    return toLowerAscii(trim(p.username)) + "|" + toLowerAscii(trim(p.host)) + "|"
           + std::to_string(p.port > 0 ? p.port : 22) + "|" + trim(p.keyPath);
}

bool isMutatingAgentCommand(const std::vector<std::string>& agentArgs) {
    if (agentArgs.empty()) {
        return false;
    }
    const std::string cmd = trim(agentArgs.front());
    return startsWith(cmd, "--mutate-")
           || startsWith(cmd, "--zfs-pipe-")
           || startsWith(cmd, "--zfs-send-")
           || startsWith(cmd, "--zfs-recv-")
           || cmd == "--repair-alt-mountpoints"
           // --job-submit también: reenviarlo lanza la MISMA transferencia por segunda
           // vez, con dos jobs corriendo a la vez sobre los mismos datos. Estaba
           // --job-cancel pero no éste, que es el que causa daño al duplicarse.
           || cmd == "--job-submit"
           || cmd == "--job-cancel";
}

bool extractLocalAgentArgs(const std::string& remoteCmd, std::vector<std::string>& argsOut) {
    argsOut.clear();
    // Se prueban las DOS rutas del agente, no solo la de Unix. Buscar únicamente la Unix
    // es lo que dejaba a Windows fuera del RPC: agentCommand() emite la ruta de Windows,
    // no casaba con el marcador, y el comando acababa ejecutándose por SSH en crudo, sin
    // el mTLS del túnel y sin que nada lo advirtiera.
    long long pos = -1;
    std::size_t markerLen = 0;
    for (const std::string& marker : {daemonpayload::unixBinPath(), daemonpayload::windowsBinPath()}) {
        const long long found = lastIndexOf(remoteCmd, marker);
        if (found > pos) {
            pos = found;
            markerLen = marker.size();
        }
    }
    if (pos < 0) {
        return false;
    }
    std::string tail = trim(remoteCmd.substr(static_cast<std::size_t>(pos) + markerLen));
    // La forma de Windows es: & "C:\...\zfsmgr-agent.exe" --health
    // Tras el marcador queda la comilla de cierre, que no es parte de los argumentos.
    if (!tail.empty() && tail.front() == '"') {
        tail = trim(tail.substr(1));
    }
    if (tail.empty()) {
        return false;
    }
    // Se corta en el primer separador de shell NO entrecomillado.
    //
    // El comentario original decía «no entrecomillado» pero la regex no lo comprobaba:
    // cortaba en el primer ';', '&' o '|' apareciera donde apareciera, incluso dentro de
    // un argumento correctamente protegido. Un directorio llamado «Copias & Backups»
    // truncaba la orden, y con --mutate-advanced-todir ese directorio lo elige el usuario:
    // se perdían el destino y el indicador de borrar el origen.
    //
    // Se recorre por BYTES, que aquí es equivalente a por caracteres: los separadores y
    // las comillas son ASCII, y ningún byte de continuación de UTF-8 puede confundirse con
    // ellos porque todos valen 0x80 o más.
    {
        bool inSQ = false;
        bool inDQ = false;
        std::size_t sep = std::string::npos;
        for (std::size_t i = 0; i < tail.size(); ++i) {
            const char c = tail[i];
            if (inSQ) {
                if (c == '\'') { inSQ = false; }
                continue;
            }
            if (inDQ) {
                if (c == '\\' && i + 1 < tail.size()) { ++i; continue; }
                if (c == '"') { inDQ = false; }
                continue;
            }
            if (c == '\'') { inSQ = true; continue; }
            if (c == '"') { inDQ = true; continue; }
            if (c == ';' || c == '&' || c == '|' || c == '\n' || c == '\r') {
                sep = i;
                break;
            }
        }
        if (sep != std::string::npos) {
            tail = trim(tail.substr(0, sep));
        }
    }
    // Los argumentos vienen del comando construido con shSingleQuote() y luego envuelto en
    // otro shSingleQuote() para el argumento de `sh -c '...'`. El patrón '"'"' representa
    // una comilla simple escapada en ese doble envoltorio. Deshacerlo ANTES de analizar
    // evita que las comillas dobles se tomen por delimitadores de más.
    replaceAll(tail, "'\"'\"'", "'");
    if (tail.empty()) {
        return false;
    }
    const std::vector<std::string> parsed = H::posixShellSplitArgs(tail);
    if (parsed.empty()) {
        return false;
    }
    const std::string cmd = trim(parsed.front());
    // El resultado de esta función se usa para DESVIAR la orden al RPC. Hay verbos que el
    // daemon no sirve por ahí a propósito —transportan flujos por la entrada o la salida
    // estándar, y --mutate-shell-generic además ejecuta shell arbitrario como root—, así
    // que desviarlos es garantizar un «unknown command».
    if (!(cmd == "--health" || cmd == "--heartbeat" || startsWith(cmd, "--dump-")
          || startsWith(cmd, "--mutate-"))) {
        return false;
    }
    if (H::isCliOnlyAgentCommand(cmd)) {
        return false;  // se queda en el camino clásico; el RPC no lo sirve
    }
    argsOut = parsed;
    return true;
}

bool parseRemoteDaemonTlsBundle(const std::string& text, RemoteTlsBundle& out) {
    out = RemoteTlsBundle{};
    static const std::string kBegin = "__ZFSMGR_TLS_BEGIN__:";
    static const std::string kEnd = "__ZFSMGR_TLS_END__:";
    static const std::string kPort = "__ZFSMGR_AGENT_PORT__:";

    std::string currentPath;
    std::string currentContent;
    for (const std::string& rawLine : split(text, "\n", false)) {
        if (startsWith(rawLine, kBegin)) {
            currentPath = trim(rawLine.substr(kBegin.size()));
            currentContent.clear();
            continue;
        }
        if (startsWith(rawLine, kEnd)) {
            const std::string endPath = trim(rawLine.substr(kEnd.size()));
            if (!currentPath.empty() && endPath == currentPath) {
                const std::string content = trim(currentContent) + "\n";
                if (endsWith(currentPath, "/server.crt")) {
                    out.serverCertPem = content;
                } else if (endsWith(currentPath, "/client.crt")) {
                    out.clientCertPem = content;
                } else if (endsWith(currentPath, "/client.key")) {
                    out.clientKeyPem = content;
                    out.clientKeyIncluded = true;
                }
            }
            currentPath.clear();
            currentContent.clear();
            continue;
        }
        if (startsWith(rawLine, kPort)) {
            int parsed = 0;
            if (parseIntStrict(rawLine.substr(kPort.size()), parsed) && parsed > 0
                && parsed <= 65535) {
                out.port = static_cast<std::uint16_t>(parsed);
            }
            continue;
        }
        if (!currentPath.empty()) {
            currentContent += rawLine;
            currentContent += '\n';
        }
    }
    // Sin el certificado del servidor o el del cliente no hay conversación posible; la
    // clave privada puede faltar legítimamente, porque a veces ya la tenemos guardada.
    return !out.serverCertPem.empty() && !out.clientCertPem.empty();
}

const char* defaultAgentConfigPath() {
#ifdef _WIN32
    return "C:\\ProgramData\\ZFSMgr\\agent\\agent.conf";
#else
    return "/etc/zfsmgr/agent.conf";
#endif
}
const char* defaultAgentTlsCertPath() {
#ifdef _WIN32
    return "C:\\ProgramData\\ZFSMgr\\agent\\tls\\server.crt";
#else
    return "/etc/zfsmgr/tls/server.crt";
#endif
}
const char* defaultAgentTlsClientCertPath() {
#ifdef _WIN32
    return "C:\\ProgramData\\ZFSMgr\\agent\\tls\\client.crt";
#else
    return "/etc/zfsmgr/tls/client.crt";
#endif
}
const char* defaultAgentTlsClientKeyPath() {
#ifdef _WIN32
    return "C:\\ProgramData\\ZFSMgr\\agent\\tls\\client.key";
#else
    return "/etc/zfsmgr/tls/client.key";
#endif
}

LocalAgentConfig parseLocalAgentConfig(const std::string& text) {
    LocalAgentConfig cfg;
    cfg.tlsCertPath = defaultAgentTlsCertPath();
    cfg.tlsClientCertPath = defaultAgentTlsClientCertPath();
    cfg.tlsClientKeyPath = defaultAgentTlsClientKeyPath();
    for (const std::string& raw : split(text, "\n", false)) {
        const std::string line = trim(raw);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == 0 || eq == std::string::npos) {
            continue;
        }
        const std::string key = toUpperAscii(trim(line.substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "AGENT_BIND" || key == "BIND") {
            cfg.bindAddress = stripConfigQuotes(value);
        } else if (key == "AGENT_PORT" || key == "PORT") {
            const int parsedPort = parseConfigInt(value, cfg.port);
            if (parsedPort > 0 && parsedPort <= 65535) {
                cfg.port = static_cast<std::uint16_t>(parsedPort);
            }
        } else if (key == "TLS_CERT") {
            cfg.tlsCertPath = stripConfigQuotes(value);
        } else if (key == "TLS_CLIENT_CERT") {
            cfg.tlsClientCertPath = stripConfigQuotes(value);
        } else if (key == "TLS_CLIENT_KEY") {
            cfg.tlsClientKeyPath = stripConfigQuotes(value);
        }
    }
    return cfg;
}

LocalAgentConfig loadLocalAgentConfig(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return parseLocalAgentConfig(std::string());
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return parseLocalAgentConfig(ss.str());
}

std::string wrapRemoteCommand(const ConnectionProfile& p, const std::string& remoteCmd) {
    if (!isWindowsConnection(p)) {
        return remoteCmd;
    }
    std::string trimmed = trim(remoteCmd);
    // Los comandos clásicos llegan envueltos por withUnixSearchPathCommand, que antepone
    // un «PATH=...; export PATH; » de sintaxis Unix. Eso existía para el bash de MSYS2; en
    // PowerShell es un error de sintaxis. Se retira aquí, en un único punto, en vez de en
    // la treintena de sitios que lo aplican: el prólogo de abajo ya pone las rutas de
    // OpenZFS en $env:Path, que es lo único que ese prefijo aportaba.
    quitaPrologoPathUnix(trimmed);

    std::string script =
        "$ProgressPreference='SilentlyContinue'; "
        "$InformationPreference='SilentlyContinue'; "
        "$WarningPreference='Continue'; "
        "$zfsPaths=@("
        "'C:\\\\Program Files\\\\OpenZFS On Windows\\\\bin',"
        "'C:\\\\Program Files\\\\OpenZFS On Windows'"
        "); "
        "foreach($p in $zfsPaths){ "
        "  if(Test-Path -LiteralPath $p){ "
        "    if(-not (($env:Path -split ';') -contains $p)){ $env:Path = $p + ';' + $env:Path } "
        "  } "
        "}; ";
    script += trimmed;

    const std::string b64 = base64Encode(aUtf16Le(script));
    // La línea de órdenes de cmd.exe se agota con cargas muy grandes en ejecución local.
    // Por SSH no se usa -Command: el shell remoto expandiría las variables de PowerShell
    // (por ejemplo «$p») y rompería los foreach.
    if (isLocalConnection(p) && b64.size() > 7000) {
        std::string inlineScript = script;
        replaceAll(inlineScript, "\"", "`\"");
        return "powershell -NoProfile -NonInteractive -Command \"& { " + inlineScript + " }\"";
    }
    return "powershell -NoProfile -NonInteractive -EncodedCommand " + b64;
}

std::string sanitizeWindowsCliXml(const std::string& raw) {
    if (raw.empty()) {
        return raw;
    }
    std::string s = raw;
    replaceAll(s, "#< CLIXML", "");
    const long long xmlPos = indexOfAsciiCI(s, "<Objs Version=");
    if (xmlPos >= 0) {
        s = s.substr(0, static_cast<std::size_t>(xmlPos));
    }
    return trim(s);
}

bool shouldRetrySshWithoutMultiplexing(const std::string& stderrText) {
    // Plegado UTF-8 y no solo ASCII: esto viene de la salida de error de `ssh`, que es un
    // programa traducible. Con agujas ASCII el resultado coincide, pero es la misma regla
    // que ya obligó a distinguirlos al portar looksLikeSudoAuthFailure.
    const std::string lowered = toLowerUtf8(stderrText);
    return contains(lowered, "getsockname failed") || contains(lowered, "not a socket")
           || contains(lowered, "bad stdio forwarding specification");
}

}  // namespace zfsmgr::base::transport
