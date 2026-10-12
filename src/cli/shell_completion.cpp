// TAB completion: the children of a place, property names and file contents.

#include "shell_internal.h"

namespace zfsmgr::cli {

// Los hijos de un sitio, para completar una URL. Se pregunta a la máquina, que por un
// túnel ya montado son un par de milisegundos.
//
// Los fallos se TRAGAN a propósito: pulsar el tabulador no es pedir una operación, y llenar
// la pantalla de errores porque una máquina está apagada convierte una comodidad en un
// estorbo. Sin respuesta, simplemente no completa.
std::vector<std::string> hijosDe(Estado& e, const ZfsmUrl& u) {
    std::vector<std::string> out;
    switch (nodoDe(u)) {
        case Nodo::Raiz:
            for (const auto& p : e.conns.perfiles) {
                out.push_back(p.id.empty() ? p.name : p.id);
            }
            return out;
        case Nodo::Snapshot:
            return out;  // una instantánea no tiene hijos
        case Nodo::Conexion: {
            std::string texto;
            std::string err;
            int rc = -1;
            const auto* p = buscarConexion(e.conns, u.connection);
            if (!p || e.conns.desconectada(u.connection)
                || !ejecutarAgente(*e.ses, *p, PET::poolList(), texto, err, rc, nullptr, 8000)
                || rc != 0) {
                return out;
            }
            B::json::Value raiz;
            std::string errJson;
            if (!B::json::parse(texto, raiz, &errJson)) {
                return out;
            }
            for (const auto& kv : raiz["pools"].toObject()) {
                out.push_back(kv.first);
            }
            return out;
        }
        case Nodo::Dataset: {
            std::string texto;
            std::string err;
            int rc = -1;
            const auto* p = buscarConexion(e.conns, u.connection);
            if (!p || e.conns.desconectada(u.connection)
                || !ejecutarAgente(*e.ses, *p, PET::datasetList(u.dataset), texto, err, rc, nullptr,
                                   12000)
                || rc != 0) {
                return out;
            }
            const std::string prefijo = u.dataset + "/";
            for (const std::string& linea : B::split(texto, "\n", true)) {
                const std::size_t tab = linea.find('\t');
                const std::string nombre = tab == std::string::npos ? linea : linea.substr(0, tab);
                const std::size_t arroba = nombre.find('@');
                if (arroba != std::string::npos) {
                    if (nombre.substr(0, arroba) == u.dataset) {
                        out.push_back("@" + nombre.substr(arroba + 1));
                    }
                    continue;
                }
                if (B::startsWith(nombre, prefijo)
                    && nombre.find('/', prefijo.size()) == std::string::npos) {
                    out.push_back(nombre.substr(prefijo.size()));
                }
            }
            return out;
        }
    }
    return out;
}

// Qué se puede escribir donde está el cursor.
//
// Tres casos, y el orden importa: la PRIMERA palabra es una orden; una palabra que empieza
// por guion es una opción DE ESA orden; y cualquier otra cosa se trata como una URL.
// Las propiedades que ese dataset tiene de verdad, recordadas durante la sesión.
//
// Se preguntan a la máquina —`zfs get all`— en vez de llevar una lista escrita: así no
// envejece con OpenZFS y no se ofrece lo que ese pool no soporta. Se cachea porque el
// tabulador se pulsa muchas veces y cada pulsación costaría una ida y vuelta.
const std::vector<std::string>& propiedadesDe(Estado& e, const ZfsmUrl& donde) {
    const std::string clave = textoDe(donde);
    const auto ya = e.propsPorSitio.find(clave);
    if (ya != e.propsPorSitio.end()) {
        return ya->second;
    }
    std::vector<std::string> nombres;
    if (nodoDe(donde) == Nodo::Dataset || nodoDe(donde) == Nodo::Snapshot) {
        std::string out;
        B::json::Value raiz;
        std::string err;
        // El verbo devuelve el JSON de `zfs get -j`, no columnas: los nombres son las claves
        // de `properties`.
        if (agente(e, donde, PET::datasetProperties(donde.zfsName()), out, 20000)
            && B::json::parse(out, raiz, &err)) {
            for (const auto& ds : raiz["datasets"].toObject()) {
                for (const auto& prop : ds.second["properties"].toObject()) {
                    nombres.push_back(prop.first);
                }
            }
        }
    }
    std::sort(nombres.begin(), nombres.end());
    nombres.erase(std::unique(nombres.begin(), nombres.end()), nombres.end());
    return e.propsPorSitio.emplace(clave, std::move(nombres)).first->second;
}

// Los nombres que hay dentro de `#content[/ruta]`, para el tabulador.
//
// Only NOMBRES: `ls -lA` trae permisos, dueño y fechas que aquí no pintan nada, y habría
// que volver a partirlos. Los directorios salen con la barra puesta, que es lo que uno va
// a escribir a continuación.
std::vector<std::string> nombresDeContenido(Estado& e, const ZfsmUrl& destino) {
    std::vector<std::string> out;
    std::string error;
    const auto* p = perfilVivoDe(e, destino, error);
    if (!p) {
        return out;
    }
    // Vale para `#content` y para `#path`: la ruta sale de rutaDeFicheros y los nombres, del
    // agente. Antes era `ls -A -p` (o `Get-ChildItem`) por SSH: una shell para pulsar el
    // tabulador, y dos sintaxis que mantener.
    std::string base;
    if (!rutaDeFicheros(e, destino, base, /*callado=*/true)) {
        return out;
    }
    std::string texto;
    std::string err;
    int rc = -1;
    if (!ejecutarAgente(*e.ses, *p, PET::pathContents(base), texto, err, rc, nullptr, 12000)
        || rc != 0) {
        return out;
    }
    B::json::Value raiz;
    if (!B::json::parse(texto, raiz, nullptr)) {
        return out;
    }
    for (const auto& v : raiz["entries"].toArray()) {
        const std::string n = v["name"].toString();
        if (!n.empty()) {
            // Los directorios, con la barra puesta: es lo que uno escribe a continuación.
            out.push_back(n + (v["type"].toString() == "d" ? "/" : ""));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> completaEn(Estado& e, const std::string& linea, std::size_t cursor,
                                    std::size_t& desde, bool& puedeSeguir) {
    // Por omisión, lo completado se cierra: una orden, una opción, un valor de propiedad.
    // Solo lo que admite «/» detrás dice lo contrario, y lo dice donde se sabe.
    puedeSeguir = false;
    // El trozo que se está escribiendo: desde el último espacio antes del cursor.
    desde = linea.rfind(' ', cursor == 0 ? 0 : cursor - 1);
    desde = (desde == std::string::npos || cursor == 0) ? 0 : desde + 1;
    if (desde > cursor) {
        desde = cursor;
    }
    const std::string parcial = linea.substr(desde, cursor - desde);
    const std::string antes = B::trim(linea.substr(0, desde));

    if (antes.empty()) {
        return nombresQueEmpiezanPor(parcial);
    }
    const std::vector<std::string> palabras = B::split(antes, " ", true);
    const std::string orden = B::toLowerAscii(palabras.front());
    if (!parcial.empty() && parcial.front() == '-') {
        return opcionesQueEmpiezanPor(orden, parcial);
    }
    // `help` completa nombres de orden, que es lo que lleva detrás.
    if (orden == "help" || orden == "?") {
        return nombresQueEmpiezanPor(parcial);
    }

    // --- Propiedades, en `get` y `set`.
    //
    // Los NOMBRES salen de la máquina, no de una lista escrita aquí: se pregunta al dataset
    // en el que uno está. Una lista propia envejecería con cada versión de OpenZFS y, peor,
    // ofrecería propiedades que ese pool no tiene.
    //
    // Los VALORES salen del catálogo de la capa base, que es el mismo que usa el
    // desplegable de la interfaz. Solo las de lista cerrada: para `quota` o `mountpoint` no
    // se ofrece nada, que es mejor que inventar.
    if (orden == "get" || orden == "set") {
        const std::size_t igual = parcial.find('=');
        if (igual != std::string::npos) {
            const std::string prop = parcial.substr(0, igual);
            const std::string escrito = parcial.substr(igual + 1);
            std::vector<std::string> out;
            for (const std::string& v : B::zfsprops::valuesOf(prop)) {
                if (B::startsWith(v, escrito)) {
                    out.push_back(prop + "=" + v);
                }
            }
            return out;
        }
        std::vector<std::string> out;
        for (const std::string& p : propiedadesDe(e, e.actual)) {
            if (B::startsWith(p, parcial)) {
                out.push_back(orden == "set" ? p + "=" : p);
            }
        }
        return out;
    }

    // --- `import <pool>`: los pools que se pueden importar en la conexión en la que se está.
    //
    // Caía en el caso general, que ofrece los HIJOS del sitio actual —los pools ya
    // importados—: justo los que no se pueden importar. Visto en mmela con un pool recién
    // exportado que el tabulador no proponía.
    if (orden == "import" && palabras.size() == 1) {
        const ZfsmUrl conexion = subeHasta(e.actual, Objetivo::Conexion);
        if (conexion.connection.empty()) {
            return {};
        }
        const std::string clave = B::toLowerAscii(conexion.connection);
        const auto ahora = std::chrono::steady_clock::now();
        auto it = e.importablesCache.find(clave);
        if (it == e.importablesCache.end() || ahora - it->second.first > std::chrono::seconds(30)) {
            std::vector<std::string> nombres;
            std::string sonda;
            if (agente(e, conexion, PET::importableProbe(), sonda, 60000)) {
                std::set<std::string> vistos;
                for (const H::ImportablePoolInfo& imp : H::parseZpoolImportOutput(sonda)) {
                    if (!imp.pool.empty() && vistos.insert(imp.pool).second) {
                        nombres.push_back(imp.pool);
                    }
                }
            }
            it = e.importablesCache.insert_or_assign(clave, std::make_pair(ahora, nombres)).first;
        }
        std::vector<std::string> out;
        for (const std::string& n : it->second.second) {
            if (B::startsWith(n, parcial)) {
                out.push_back(n);
            }
        }
        return out;
    }

    // --- Las SECCIONES: `#content`, `#properties`, `#permissions`, y lo que cuelga.
    //
    // Van antes que la URL corriente porque el « # » no es un separador de URL: partiendo
    // por «/» y «@», `ls #con<tab>` preguntaba por los HIJOS del dataset —que se llaman
    // «datos», no «#content»— y no ofrecía nada. Lo mismo dentro: `#content/Doc<tab>` se
    // resolvía como si «Doc» fuera un dataset.
    const std::size_t alm = parcial.find('#');
    if (alm != std::string::npos) {
        const std::string prefijo = parcial.substr(0, alm);  // la URL de delante, si la hay
        const std::string resto = parcial.substr(alm);       // desde el « # »
        ZfsmUrl sitio;
        std::string err;
        if (!resuelve(e, prefijo.empty() ? std::string(".") : prefijo, sitio, err)) {
            return {};
        }
        const std::size_t barra = resto.find('/');
        if (barra == std::string::npos) {
            // Todavía se está escribiendo el nombre de la sección. Solo se ofrecen donde
            // significan algo: en una conexión o en la raíz no hay contenido ni propiedades.
            // En una máquina, `#path/`; en un dataset o una instantánea, las suyas. Las que
            // llevan algo detrás se ofrecen con la barra puesta, como un directorio:
            // `#permissions` no lleva nada.
            std::vector<std::string> out;
            std::vector<std::string> secciones;
            const Nodo n = nodoDe(sitio);
            if (n == Nodo::Conexion) {
                secciones = {std::string("#") + B::zfsmSection::kPath + "/"};
            } else if (n == Nodo::Dataset || n == Nodo::Snapshot) {
                secciones = {std::string("#") + B::zfsmSection::kContent + "/",
                             std::string("#") + B::zfsmSection::kProperties + "/",
                             std::string("#") + B::zfsmSection::kPermissions};
            }
            for (const std::string& s : secciones) {
                if (B::startsWith(s, resto)) {
                    out.push_back(prefijo + s);
                }
            }
            puedeSeguir = out.size() == 1 && out.front().back() == '/';
            return out;
        }
        const std::string seccion = B::toLowerAscii(resto.substr(1, barra - 1));
        const std::size_t ultima = resto.find_last_of('/');
        const std::string yaEscrito = resto.substr(0, ultima + 1);  // «#content/» o «#content/a/»
        const std::string cola = resto.substr(ultima + 1);
        std::vector<std::string> candidatos;
        if (seccion == B::zfsmSection::kProperties) {
            // `#properties/<nombre>`: una sola propiedad, y no hay más niveles.
            if (yaEscrito != std::string("#") + B::zfsmSection::kProperties + "/") {
                return {};
            }
            candidatos = propiedadesDe(e, sitio);
        } else if (seccion == B::zfsmSection::kContent || seccion == B::zfsmSection::kPath) {
            // Se resuelve el directorio ya escrito —sin la barra final, que `resuelve` no
            // espera— y se preguntan sus nombres.
            std::string dir = prefijo + yaEscrito;
            if (dir.size() > 1 && dir.back() == '/') {
                dir.erase(dir.size() - 1);
            }
            ZfsmUrl donde;
            if (!resuelve(e, dir, donde, err)) {
                return {};
            }
            candidatos = nombresDeContenido(e, donde);
            // «..» y «.» con su barra, como en la URL corriente (ver más abajo). Se suman a
            // los nombres: aquí uno puede empezar por punto, y `.bashrc` no debe taparse.
            if (cola == "." || cola == "..") {
                ZfsmUrl conPuntos;
                if (resuelve(e, prefijo + yaEscrito + cola, conPuntos, err)) {
                    candidatos.push_back(cola + "/");
                }
            }
        } else {
            return {};  // `#permissions` no tiene hijos, y una sección desconocida tampoco
        }
        std::vector<std::string> out;
        for (const std::string& c : candidatos) {
            if (B::startsWith(c, cola)) {
                out.push_back(prefijo + yaEscrito + c);
            }
        }
        // Dentro de `#content`, un DIRECTORIO admite seguir bajando y un fichero no. La
        // diferencia viene ya marcada con la barra final desde la máquina —`ls -p` en Unix,
        // `PSIsContainer` en Windows—, así que no hay que preguntarlo otra vez. En
        // `#properties` no hay más niveles: una propiedad se cierra.
        puedeSeguir = out.size() == 1 && !out.front().empty() && out.front().back() == '/';
        return out;
    }

    // Una URL. Se separa lo ya escrito en «lo que ya está resuelto» y «lo que se teclea»,
    // para preguntar por los hijos del sitio correcto.
    std::string base = parcial;
    std::string cola;
    const std::size_t corte = base.find_last_of("/@");
    if (corte == std::string::npos) {
        cola = base;
        base.clear();
    } else {
        cola = base.substr(base[corte] == '@' ? corte : corte + 1);
        base = base.substr(0, base[corte] == '@' ? corte : corte + 1);
    }
    ZfsmUrl sitio;
    std::string err;
    if (!resuelve(e, base.empty() ? std::string(".") : base, sitio, err)) {
        return {};
    }
    // «..» y «.» se completan con su barra, como en bash: `cd fc16/..<tab>` no ofrecía nada
    // porque ningún hijo se llama así, y con «../» sí, así que había que teclearla a mano.
    // Solo si de verdad resuelven. Tras una «@» la cola lleva la arroba delante y no entra.
    std::string conPuntos;
    if (cola == "." || cola == "..") {
        ZfsmUrl destinoPuntos;
        std::string errPuntos;
        if (resuelve(e, base + cola, destinoPuntos, errPuntos)) {
            conPuntos = base + cola + "/";
        }
    }
    // Dentro de `#path/…` o `#content/…`, un nombre suelto es un fichero de ese directorio,
    // no un dataset hijo: lo mismo que lee `resuelve`.
    if (esSeccionDeFicheros(sitio)) {
        std::vector<std::string> out;
        for (const std::string& n : nombresDeContenido(e, sitio)) {
            if (B::startsWith(n, cola)) {
                out.push_back(base + n);
            }
        }
        // Aquí un nombre SÍ puede empezar por punto (`.bashrc`): «../» se suma a ellos, no
        // los tapa.
        if (!conPuntos.empty()) {
            out.push_back(conPuntos);
        }
        puedeSeguir = out.size() == 1 && out.front().back() == '/';
        return out;
    }
    std::vector<std::string> out;
    for (const std::string& h : hijosDe(e, sitio)) {
        if (B::startsWith(h, cola)) {
            out.push_back(base + h);
        }
    }
    // Un dataset también puede empezar por punto (`tank/.oculto` es un nombre válido; solo
    // «.» y «..» no lo son): «../» se suma a ellos, igual que en los ficheros.
    if (!conPuntos.empty()) {
        out.push_back(conPuntos);
        puedeSeguir = out.size() == 1;
        return out;
    }
    // Lo escrito ya ES el único candidato —`cd local<tab>` en zfsm://—: no hay nada que
    // alargar y el tabulador no hacía nada. Se ofrece lo que puede seguir: «/» si hay hijos
    // —una conexión siempre—, «@» si tiene instantáneas, y «#» donde hay secciones, en un
    // dataset o una instantánea. Con uno solo se escribe; con varios se enseñan.
    if (out.size() == 1 && out.front() == parcial) {
        ZfsmUrl elegido;
        if (resuelve(e, parcial, elegido, err)) {
            std::vector<std::string> sigue;
            const Nodo n = nodoDe(elegido);
            bool hayHijos = n == Nodo::Conexion;
            bool hayInstantaneas = false;
            if (n == Nodo::Dataset) {
                for (const std::string& h : hijosDe(e, elegido)) {
                    if (!h.empty()) {
                        (h.front() == '@' ? hayInstantaneas : hayHijos) = true;
                    }
                }
            }
            if (hayHijos) {
                sigue.push_back(parcial + "/");
            }
            if (hayInstantaneas) {
                sigue.push_back(parcial + "@");
            }
            if (n == Nodo::Dataset || n == Nodo::Snapshot) {
                sigue.push_back(parcial + "#");
            }
            // Una máquina sigue con «/» (sus pools) o con `#path/` (sus ficheros).
            if (n == Nodo::Conexion) {
                sigue.push_back(parcial + "#" + B::zfsmSection::kPath + "/");
            }
            if (!sigue.empty()) {
                puedeSeguir = true;
                return sigue;
            }
        }
    }
    // Un pool, un dataset o una conexión admiten «/» y otro nombre detrás, así que no se
    // cierran: es lo que permite bajar con el tabulador sin borrar el espacio a cada paso.
    puedeSeguir = out.size() == 1;
    return out;
}

// =====================================================================================
// --- Patrones en el destino: `*`, `?`, `[..]` y `{a,b}`.
//
// La regla de cada orden está en `reglasDePatron`. La idea general:
//
//   - En `ls`, un patrón en el ÚLTIMO tramo FILTRA las filas de lo que se lista:
//     `ls tank/user/M*` lista tank/user y deja las filas que casan con «M*», como
//     `ls -d M*` y no como la shell, que listaría el contenido de cada uno. En un tramo
//     intermedio multiplica los destinos: `ls tank/*/backups`.
//   - En las lecturas, cada destino que casa se atiende y sus tablas se juntan en una.
//   - En las que MODIFICAN, el patrón solo puede ir en el último tramo —nunca abarcar
//     varias máquinas o pools de golpe con un `destroy`—, la lista exacta se enseña SIEMPRE
//     y se pide confirmación salvo `-y`.
//   - Si nada coincide es un ERROR. Nunca se pasa el patrón tal cual a la orden: un
//     `destroy` que buscara un dataset llamado «auto-*» es la peor forma de enterarse.
//
// La expansión se hace sobre el texto TODAVÍA CODIFICADO, antes de `resuelve()`: allí cada
// tramo tiene que existir, y una vez descodificado `%2A` y `*` ya no se distinguen.

}  // namespace zfsmgr::cli
