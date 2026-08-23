#include "zfsallow.h"

#include "strutil.h"

namespace zfsmgr::base::zfsallow {

const char* keyOf(Scope a) {
    switch (a) {
        case Scope::Local:               return "local";
        case Scope::Descendants:       return "descendientes";
        case Scope::LocalAndDescendants: return "ambos";
        case Scope::OnCreate:             return "alcrear";
        case Scope::Set:            return "conjunto";
    }
    return "ambos";
}

const char* zfsSectionTitle(Scope a) {
    switch (a) {
        case Scope::Local:               return "Local permissions";
        case Scope::Descendants:       return "Descendent permissions";
        case Scope::LocalAndDescendants: return "Local+Descendent permissions";
        case Scope::OnCreate:             return "Create time permissions";
        case Scope::Set:            return "Permission sets";
    }
    return "";
}

const char* zfsToken(Who q) {
    switch (q) {
        case Who::User:  return "user";
        case Who::Group:    return "group";
        case Who::Everyone:    return "everyone";
        case Who::Set: return "set";
    }
    return "";
}

const char* keyOf(Who q) {
    switch (q) {
        case Who::User:  return "usuario";
        case Who::Group:    return "grupo";
        case Who::Everyone:    return "todos";
        case Who::Set: return "conjunto";
    }
    return "usuario";
}

std::string labelOf(Scope a) {
    switch (a) {
        case Scope::Local:               return "solo aquí";
        case Scope::Descendants:       return "solo en los descendientes";
        case Scope::LocalAndDescendants: return "aquí y en los descendientes";
        case Scope::OnCreate:             return "al crear un descendiente";
        case Scope::Set:            return "conjunto de permisos";
    }
    return {};
}

std::string labelOf(Who q) {
    switch (q) {
        case Who::User:  return "usuario";
        case Who::Group:    return "grupo";
        case Who::Everyone:    return "todos";
        case Who::Set: return "conjunto";
    }
    return {};
}

Scope scopeFrom(const std::string& clave) {
    for (const Scope a : {Scope::Local, Scope::Descendants, Scope::LocalAndDescendants,
                            Scope::OnCreate, Scope::Set}) {
        if (clave == keyOf(a)) {
            return a;
        }
    }
    return Scope::LocalAndDescendants;
}

Who whoFrom(const std::string& clave) {
    for (const Who q : {Who::User, Who::Group, Who::Everyone, Who::Set}) {
        if (clave == keyOf(q)) {
            return q;
        }
    }
    return Who::User;
}

std::vector<Entry> parse(const std::string& salida) {
    std::vector<Entry> out;
    // El alcance viene del TÍTULO de la sección, así que hay que llevarlo mientras se leen
    // las líneas de debajo. Una línea suelta no dice a qué sección pertenece.
    Scope actual = Scope::LocalAndDescendants;
    bool dentroDeSeccion = false;
    for (const std::string& cruda : split(salida, "\n", false)) {
        const std::string linea = trim(cruda);
        if (linea.empty() || startsWith(linea, "----")) {
            continue;
        }
        const std::string bajo = toLowerAscii(linea);
        if (bajo == "permission sets:") {
            actual = Scope::Set;
            dentroDeSeccion = true;
            continue;
        }
        if (bajo == "local permissions:") {
            actual = Scope::Local;
            dentroDeSeccion = true;
            continue;
        }
        if (bajo == "descendent permissions:") {
            actual = Scope::Descendants;
            dentroDeSeccion = true;
            continue;
        }
        if (bajo == "local+descendent permissions:") {
            actual = Scope::LocalAndDescendants;
            dentroDeSeccion = true;
            continue;
        }
        if (bajo == "create time permissions:") {
            actual = Scope::OnCreate;
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
        Entry e;
        e.scope = actual;
        std::string listaPermisos;
        const std::string primero = toLowerAscii(trozos[0]);
        if (primero == "user" && trozos.size() >= 3) {
            e.who = Who::User;
            e.name = trozos[1];
            listaPermisos = trozos[2];
        } else if (primero == "group" && trozos.size() >= 3) {
            e.who = Who::Group;
            e.name = trozos[1];
            listaPermisos = trozos[2];
        } else if (primero == "everyone" && trozos.size() >= 2) {
            e.who = Who::Everyone;
            listaPermisos = trozos[1];
        } else if (startsWith(trozos[0], "@") && trozos.size() >= 2) {
            e.who = Who::Set;
            e.name = trozos[0];
            listaPermisos = trozos[1];
        } else if (actual == Scope::OnCreate) {
            // «Create time permissions» no nombra a nadie: su línea es solo la lista de
            // permisos, sin «user» ni nada delante. Sin este caso se saltaba entera y esos
            // permisos —los que hereda quien cree un descendiente— no salían por ninguna
            // parte.
            e.who = Who::Everyone;
            listaPermisos = trozos[0];
        } else {
            continue;   // una línea que no se entiende no se inventa
        }
        for (const std::string& p : split(listaPermisos, ",", true)) {
            e.permissions.push_back(trim(p));
        }
        if (e.permissions.empty()) {
            continue;
        }
        out.push_back(e);
    }
    return out;
}

namespace {

// Las banderas comunes a `allow` y `unallow`. El orden importa poco, pero el CONJUNTO no:
// olvidar la de alcance concede a los descendientes lo que se quería conceder solo aquí.
void ponBanderas(const Entry& e, std::vector<std::string>& argv) {
    switch (e.scope) {
        case Scope::Local:
            argv.push_back("-l");
            break;
        case Scope::Descendants:
            argv.push_back("-d");
            break;
        case Scope::LocalAndDescendants:
            break;   // sin bandera: es lo que hace `zfs allow` por omisión
        case Scope::OnCreate:
            argv.push_back("-c");
            return;  // «al crear» no lleva destinatario: es para quien cree
        case Scope::Set:
            argv.push_back("-s");
            break;
    }
    switch (e.who) {
        case Who::User:
            argv.push_back("-u");
            break;
        case Who::Group:
            argv.push_back("-g");
            break;
        case Who::Everyone:
            argv.push_back("-e");
            break;
        case Who::Set:
            break;   // el nombre del conjunto va tal cual, con su «@»
    }
}

std::string juntaPermisos(const Entry& e) {
    std::string s;
    for (const std::string& p : e.permissions) {
        if (!s.empty()) {
            s.push_back(',');
        }
        s += p;
    }
    return s;
}

std::vector<std::string> argvDe(const char* orden, const Entry& e, const std::string& dataset) {
    std::vector<std::string> argv = {orden};
    ponBanderas(e, argv);
    // «Todos» y «al crear» no nombran a nadie: el destinatario es la bandera.
    if (e.who != Who::Everyone && e.scope != Scope::OnCreate && !e.name.empty()) {
        argv.push_back(e.name);
    }
    argv.push_back(juntaPermisos(e));
    argv.push_back(dataset);
    return argv;
}

}  // namespace

std::vector<std::string> argvAllow(const Entry& e, const std::string& dataset) {
    return argvDe("allow", e, dataset);
}

std::vector<std::string> argvUnallow(const Entry& e, const std::string& dataset) {
    return argvDe("unallow", e, dataset);
}

}  // namespace zfsmgr::base::zfsallow
