#include "zfsallow.h"

#include "strutil.h"

namespace zfsmgr::base::zfsallow {

const char* claveDe(Alcance a) {
    switch (a) {
        case Alcance::Local:               return "local";
        case Alcance::Descendientes:       return "descendientes";
        case Alcance::LocalYDescendientes: return "ambos";
        case Alcance::AlCrear:             return "alcrear";
        case Alcance::Conjunto:            return "conjunto";
    }
    return "ambos";
}

const char* seccionZfs(Alcance a) {
    switch (a) {
        case Alcance::Local:               return "Local permissions";
        case Alcance::Descendientes:       return "Descendent permissions";
        case Alcance::LocalYDescendientes: return "Local+Descendent permissions";
        case Alcance::AlCrear:             return "Create time permissions";
        case Alcance::Conjunto:            return "Permission sets";
    }
    return "";
}

const char* tokenZfs(Quien q) {
    switch (q) {
        case Quien::Usuario:  return "user";
        case Quien::Grupo:    return "group";
        case Quien::Todos:    return "everyone";
        case Quien::Conjunto: return "set";
    }
    return "";
}

const char* claveDe(Quien q) {
    switch (q) {
        case Quien::Usuario:  return "usuario";
        case Quien::Grupo:    return "grupo";
        case Quien::Todos:    return "todos";
        case Quien::Conjunto: return "conjunto";
    }
    return "usuario";
}

std::string etiquetaDe(Alcance a) {
    switch (a) {
        case Alcance::Local:               return "solo aquí";
        case Alcance::Descendientes:       return "solo en los descendientes";
        case Alcance::LocalYDescendientes: return "aquí y en los descendientes";
        case Alcance::AlCrear:             return "al crear un descendiente";
        case Alcance::Conjunto:            return "conjunto de permisos";
    }
    return {};
}

std::string etiquetaDe(Quien q) {
    switch (q) {
        case Quien::Usuario:  return "usuario";
        case Quien::Grupo:    return "grupo";
        case Quien::Todos:    return "todos";
        case Quien::Conjunto: return "conjunto";
    }
    return {};
}

Alcance alcanceDesde(const std::string& clave) {
    for (const Alcance a : {Alcance::Local, Alcance::Descendientes, Alcance::LocalYDescendientes,
                            Alcance::AlCrear, Alcance::Conjunto}) {
        if (clave == claveDe(a)) {
            return a;
        }
    }
    return Alcance::LocalYDescendientes;
}

Quien quienDesde(const std::string& clave) {
    for (const Quien q : {Quien::Usuario, Quien::Grupo, Quien::Todos, Quien::Conjunto}) {
        if (clave == claveDe(q)) {
            return q;
        }
    }
    return Quien::Usuario;
}

std::vector<Entrada> analiza(const std::string& salida) {
    std::vector<Entrada> out;
    // El alcance viene del TÍTULO de la sección, así que hay que llevarlo mientras se leen
    // las líneas de debajo. Una línea suelta no dice a qué sección pertenece.
    Alcance actual = Alcance::LocalYDescendientes;
    bool dentroDeSeccion = false;
    for (const std::string& cruda : split(salida, "\n", false)) {
        const std::string linea = trim(cruda);
        if (linea.empty() || startsWith(linea, "----")) {
            continue;
        }
        const std::string bajo = toLowerAscii(linea);
        if (bajo == "permission sets:") {
            actual = Alcance::Conjunto;
            dentroDeSeccion = true;
            continue;
        }
        if (bajo == "local permissions:") {
            actual = Alcance::Local;
            dentroDeSeccion = true;
            continue;
        }
        if (bajo == "descendent permissions:") {
            actual = Alcance::Descendientes;
            dentroDeSeccion = true;
            continue;
        }
        if (bajo == "local+descendent permissions:") {
            actual = Alcance::LocalYDescendientes;
            dentroDeSeccion = true;
            continue;
        }
        if (bajo == "create time permissions:") {
            actual = Alcance::AlCrear;
            dentroDeSeccion = true;
            continue;
        }
        if (!dentroDeSeccion) {
            continue;   // algo antes de la primera sección: no es una entrada
        }
        // «user linarese create,mount», «everyone mount», «@basico hold,snapshot».
        const std::vector<std::string> trozos = split(linea, " ", true);
        if (trozos.empty()) {
            continue;
        }
        Entrada e;
        e.alcance = actual;
        std::string listaPermisos;
        const std::string primero = toLowerAscii(trozos[0]);
        if (primero == "user" && trozos.size() >= 3) {
            e.quien = Quien::Usuario;
            e.nombre = trozos[1];
            listaPermisos = trozos[2];
        } else if (primero == "group" && trozos.size() >= 3) {
            e.quien = Quien::Grupo;
            e.nombre = trozos[1];
            listaPermisos = trozos[2];
        } else if (primero == "everyone" && trozos.size() >= 2) {
            e.quien = Quien::Todos;
            listaPermisos = trozos[1];
        } else if (startsWith(trozos[0], "@") && trozos.size() >= 2) {
            e.quien = Quien::Conjunto;
            e.nombre = trozos[0];
            listaPermisos = trozos[1];
        } else if (actual == Alcance::AlCrear) {
            // «Create time permissions» no nombra a nadie: su línea es solo la lista de
            // permisos, sin «user» ni nada delante. Sin este caso se saltaba entera y esos
            // permisos —los que hereda quien cree un descendiente— no salían por ninguna
            // parte.
            e.quien = Quien::Todos;
            listaPermisos = trozos[0];
        } else {
            continue;   // una línea que no se entiende no se inventa
        }
        for (const std::string& p : split(listaPermisos, ",", true)) {
            e.permisos.push_back(trim(p));
        }
        if (e.permisos.empty()) {
            continue;
        }
        out.push_back(e);
    }
    return out;
}

namespace {

// Las banderas comunes a `allow` y `unallow`. El orden importa poco, pero el CONJUNTO no:
// olvidar la de alcance concede a los descendientes lo que se quería conceder solo aquí.
void ponBanderas(const Entrada& e, std::vector<std::string>& argv) {
    switch (e.alcance) {
        case Alcance::Local:
            argv.push_back("-l");
            break;
        case Alcance::Descendientes:
            argv.push_back("-d");
            break;
        case Alcance::LocalYDescendientes:
            break;   // sin bandera: es lo que hace `zfs allow` por omisión
        case Alcance::AlCrear:
            argv.push_back("-c");
            return;  // «al crear» no lleva destinatario: es para quien cree
        case Alcance::Conjunto:
            argv.push_back("-s");
            break;
    }
    switch (e.quien) {
        case Quien::Usuario:
            argv.push_back("-u");
            break;
        case Quien::Grupo:
            argv.push_back("-g");
            break;
        case Quien::Todos:
            argv.push_back("-e");
            break;
        case Quien::Conjunto:
            break;   // el nombre del conjunto va tal cual, con su «@»
    }
}

std::string juntaPermisos(const Entrada& e) {
    std::string s;
    for (const std::string& p : e.permisos) {
        if (!s.empty()) {
            s.push_back(',');
        }
        s += p;
    }
    return s;
}

std::vector<std::string> argvDe(const char* orden, const Entrada& e, const std::string& dataset) {
    std::vector<std::string> argv = {orden};
    ponBanderas(e, argv);
    // «Todos» y «al crear» no nombran a nadie: el destinatario es la bandera.
    if (e.quien != Quien::Todos && e.alcance != Alcance::AlCrear && !e.nombre.empty()) {
        argv.push_back(e.nombre);
    }
    argv.push_back(juntaPermisos(e));
    argv.push_back(dataset);
    return argv;
}

}  // namespace

std::vector<std::string> argvConceder(const Entrada& e, const std::string& dataset) {
    return argvDe("allow", e, dataset);
}

std::vector<std::string> argvRetirar(const Entrada& e, const std::string& dataset) {
    return argvDe("unallow", e, dataset);
}

}  // namespace zfsmgr::base::zfsallow
