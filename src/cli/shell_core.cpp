// The interpreter's state, resolving what a command refers to (URLs, the current
// position, profiles) and the shared pieces every command uses: preparing a request,
// confirming, filtering tables.

#include "shell_internal.h"

namespace zfsmgr::cli::detail {

Nodo nodoDe(const ZfsmUrl& u) {
    if (u.connection.empty()) {
        return Nodo::Raiz;
    }
    if (!u.snapshot.empty()) {
        return Nodo::Snapshot;
    }
    if (!u.dataset.empty()) {
        return Nodo::Dataset;
    }
    return Nodo::Conexion;
}

std::string textoDe(const ZfsmUrl& u) {
    if (u.connection.empty()) {
        // La raíz se enseña como «zfsm://», con las DOS barras del esquema.
        //
        // Decía «zfsm:/», que no es una URL de este esquema: `kEsquema` es «zfsm://» y el
        // analizador exige las dos. Se veía poco porque la raíz era un sitio de paso; desde
        // que el intérprete arranca aquí, es lo primero que se lee, y una URL que el propio
        // programa no aceptaría de vuelta es mal sitio donde empezar.
        return "zfsm://";
    }
    return B::formatZfsmUrl(u);
}

// --- Resolución de un destino.
//
// El modelo es el de un sistema de ficheros, con la lista de conexiones como raíz:
//
//     /                          las conexiones
//     /OldLau                    una conexión
//     /OldLau/winpool            el pool, que ES un dataset
//     /OldLau/winpool/sa         un dataset
//     /OldLau/winpool/sa@ayer    una instantánea
//
// Se aceptan las cuatro formas que uno escribiría sin pensar: la URL entera, la ruta
// absoluta, la relativa, y `..` / `.` / `-`.
std::vector<std::string> tramosDe(const std::string& texto) {
    std::vector<std::string> t = B::split(texto, "/", false);
    if (t.empty()) {
        t.push_back(std::string());
    }
    return t;
}

// La URL de la CONEXIÓN en la que se está, sin pool ni dataset. Es la base desde la que se
// lee un nombre ZFS completo.
ZfsmUrl base_conexion_de(const Estado& e) {
    ZfsmUrl base;
    base.kind = ZfsmKind::Connection;
    base.connection = e.actual.connection;
    return base;
}

bool resuelveCrudo(const Estado& e, const std::string& textoEntrada, ZfsmUrl& out,
                   std::string& error) {
    error.clear();
    std::string texto = B::trim(textoEntrada);
    if (texto.empty()) {
        out = e.actual;
        return true;
    }

    // Un esquema que NO es el nuestro se dice claramente. Sin esto, «zfsmgr://fc16» se
    // trataba como una ruta relativa y el error hablaba de un tramo llamado «zfsmgr:»,
    // que no ayuda a ver que lo único que sobra son tres letras.
    {
        const std::size_t dosPuntos = texto.find("://");
        if (dosPuntos != std::string::npos
            && B::toLowerAscii(texto.substr(0, dosPuntos)) != "zfsm") {
            error = B::format(T("t_esquema_malo", "el esquema es «zfsm://», no «%1://»"),
                              {texto.substr(0, dosPuntos)});
            return false;
        }
    }

    // La raíz escrita como URL. Es lo que enseña `pwd` en la raíz, y no se podía volver a
    // escribir: el analizador de URL exige una conexión y contestaba «falta la conexión».
    if (B::toLowerAscii(texto) == "zfsm://" || B::toLowerAscii(texto) == "zfsm:///") {
        out = ZfsmUrl{};
        return true;
    }

    // La URL completa: siempre vale y nunca es ambigua.
    if (B::startsWith(B::toLowerAscii(texto), "zfsm://")) {
        if (!B::parseZfsmUrl(texto, out, error)) {
            return false;
        }
        // El nombre de la conexión se normaliza a su IDENTIFICADOR. Se admite escribirla
        // por su nombre visible —«Local»— pero la URL que se enseña y se copia tiene que
        // ser siempre la misma, o dos formas de decir lo mismo parecen dos sitios.
        const auto* p = buscarConexion(e.conns, out.connection);
        if (!p) {
            error = B::format(T("t_no_conn_nombre", "no hay ninguna conexión llamada «%1»"),
                              {out.connection});
            return false;
        }
        out.connection = p->id.empty() ? p->name : p->id;
        return true;
    }

    if (texto == "-") {
        if (!e.hayAnterior) {
            error = T("t_no_hay_anterior", "no hay sitio anterior");
            return false;
        }
        out = e.anterior;
        return true;
    }

    // Un fragmento suelto (`#properties`) o una instantánea suelta (`@ayer`) se aplican al
    // sitio actual: es lo que uno escribe estando ya en el dataset.
    if (texto.front() == '#' || texto.front() == '@') {
        const std::string base = textoDe(e.actual);
        if (e.actual.connection.empty()) {
            error = T("t_falta_estar_ds", "hace falta estar en un dataset");
            return false;
        }
        // Al pegar un `@` hay que quitar antes el que hubiera: `@a` desde `ds@b` es `ds@a`,
        // no `ds@b@a`, que además ni siquiera es una URL válida.
        ZfsmUrl limpia = e.actual;
        if (texto.front() == '@') {
            limpia.snapshot.clear();
        }
        // Y la sección en la que se esté: `#properties` desde `ds#content/x` es
        // `ds#properties`, no `ds#content/x#properties`, que no es una URL.
        limpia.section.clear();
        limpia.detail.clear();
        return B::parseZfsmUrl(B::formatZfsmUrl(limpia) + texto, out, error);
    }

    // La SECCIÓN se aparta antes de trocear por «/».
    //
    // `ls zfsm://local/fc16/user#content` funcionaba y `ls user#content` no: la ruta
    // relativa se troceaba entera y «user#content» acababa siendo el nombre del dataset,
    // que viajaba tal cual hasta zfs y volvía con «bookmark delimiter '#' is not expected
    // here» — un error de ZFS sobre una sintaxis que es NUESTRA, en una orden idéntica a
    // otra que sí valía. La sección se pega al final, cuando la ruta ya está resuelta.
    std::string colaSeccion;
    const std::size_t almohadilla = texto.find('#');
    if (almohadilla != std::string::npos) {
        colaSeccion = texto.substr(almohadilla);
        texto = texto.substr(0, almohadilla);
        if (texto.empty()) {
            texto = ".";
        }
    }

    const bool absoluta = texto.front() == '/';
    if (absoluta) {
        texto = texto.substr(1);
    }

    // Se trocea y se aplica tramo a tramo, para que `..` funcione en medio.
    const auto aplica = [&](ZfsmUrl base, const std::vector<std::string>& tramos, ZfsmUrl& res,
                            std::string& err) -> bool {
        for (const std::string& tramoCrudo : tramos) {
            std::string tramo;
            if (!B::percentDecode(tramoCrudo, tramo)) {
                err = B::format(T("t_tramo_malo", "tramo mal codificado: %1"), {tramoCrudo});
                return false;
            }
            if (tramo.empty() || tramo == ".") {
                continue;
            }
            if (tramo == "..") {
                switch (nodoDe(base)) {
                    case Nodo::Raiz:
                        break;  // de la raíz no se sube
                    case Nodo::Conexion:
                        base = ZfsmUrl{};
                        break;
                    case Nodo::Snapshot:
                        base.snapshot.clear();
                        base.section.clear();
                        base.detail.clear();
                        break;
                    case Nodo::Dataset: {
                        base.section.clear();
                        base.detail.clear();
                        const std::size_t barra = base.dataset.rfind('/');
                        if (barra == std::string::npos) {
                            // Del pool se sube a la conexión: el pool ES el dataset raíz.
                            base.dataset.clear();
                            base.pool.clear();
                        } else {
                            base.dataset = base.dataset.substr(0, barra);
                        }
                        break;
                    }
                }
                continue;
            }
            // Un tramo con `@` dentro nombra una instantánea del hijo.
            std::string nombre = tramo;
            std::string snap;
            const std::size_t arroba = nombre.find('@');
            if (arroba != std::string::npos) {
                snap = nombre.substr(arroba + 1);
                nombre = nombre.substr(0, arroba);
            }
            switch (nodoDe(base)) {
                case Nodo::Raiz: {
                    const auto* p = buscarConexion(e.conns, nombre);
                    if (!p) {
                        err =
                            B::format(T("t_no_conn_nombre", "no hay ninguna conexión llamada «%1»"),
                                      {nombre});
                        return false;
                    }
                    base = ZfsmUrl{};
                    base.kind = ZfsmKind::Connection;
                    base.connection = p->id.empty() ? p->name : p->id;
                    break;
                }
                case Nodo::Conexion:
                    base.pool = nombre;
                    base.dataset = nombre;
                    base.kind = ZfsmKind::Dataset;
                    break;
                case Nodo::Dataset:
                    base.dataset += "/" + nombre;
                    break;
                case Nodo::Snapshot:
                    err = B::format(T("t_snap_sin_hijos", "un snapshot no tiene hijos: %1"),
                                    {nombre});
                    return false;
            }
            if (!snap.empty()) {
                base.snapshot = snap;
                base.kind = ZfsmKind::Snapshot;
            }
        }
        // Y aquí se vuelve a pegar la sección, con la ruta ya resuelta. Se hace DENTRO
        // porque de esta función se sale por cuatro sitios distintos —absoluta, por nombre
        // de pool, por nombre de conexión y relativa— y ponerlo en cada uno era pedir que
        // se olvidara en el quinto.
        if (!colaSeccion.empty()) {
            const std::string sinAlmohadilla = colaSeccion.substr(1);
            const std::vector<std::string> trozos = B::split(sinAlmohadilla, "/", false);
            base.section.clear();
            base.detail.clear();
            for (std::size_t i = 0; i < trozos.size(); ++i) {
                std::string trozo;
                if (!B::percentDecode(trozos[i], trozo)) {
                    err = B::format(T("t_tramo_malo", "tramo mal codificado: %1"), {trozos[i]});
                    return false;
                }
                if (i == 0) {
                    base.section = B::toLowerAscii(trozo);
                } else if (!trozo.empty()) {
                    base.detail.push_back(trozo);
                }
            }
        }
        res = base;
        return true;
    };

    const std::vector<std::string> tramos = B::split(texto, "/", false);
    if (absoluta) {
        return aplica(ZfsmUrl{}, tramos, out, error);
    }

    // DENTRO de una sección de ficheros —`#content/…` o `#path/…`— lo relativo son
    // directorios, como en una shell: `cd sub` baja a `…/sub` y `cd ..` sube uno. Antes
    // `cd ..` desde `ds#content/docs` se saltaba la sección y subía de DATASET, y `cd sub`
    // buscaba un dataset hijo llamado «sub».
    //
    // Al salir por arriba (`..` en la raíz de la sección) se vuelve al dataset o a la
    // conexión, y lo que quede se lee como siempre. Para saltar a otra máquina desde aquí
    // está la URL completa o la barra inicial: un nombre suelto es un fichero.
    {
        const std::string sec = B::toLowerAscii(e.actual.section);
        if (sec == B::zfsmSection::kContent || sec == B::zfsmSection::kPath) {
            ZfsmUrl u = e.actual;
            std::size_t i = 0;
            for (; i < tramos.size(); ++i) {
                std::string tramo;
                if (!B::percentDecode(tramos[i], tramo)) {
                    error = B::format(T("t_tramo_malo", "tramo mal codificado: %1"), {tramos[i]});
                    return false;
                }
                if (tramo.empty() || tramo == ".") {
                    continue;
                }
                if (tramo == "..") {
                    if (!u.detail.empty()) {
                        u.detail.pop_back();
                        continue;
                    }
                    u.section.clear();
                    ++i;
                    break;
                }
                u.detail.push_back(tramo);
            }
            if (!u.section.empty()) {
                out = u;
                return true;
            }
            const std::vector<std::string> resto(tramos.begin() + static_cast<long>(i),
                                                 tramos.end());
            return aplica(u, resto, out, error);
        }
    }

    // Una ruta que empieza por el nombre del POOL en el que estamos es el nombre ZFS
    // completo, no un hijo. Sin esto, `destroy zfsmgrtest/clonado` desde
    // `zfsm://Local/zfsmgrtest/origen` apuntaba a
    // `zfsmgrtest/origen/zfsmgrtest/clonado`, que es un nombre válido y no existe — o sea
    // un error confuso donde uno había escrito exactamente lo que quería.
    //
    // Con la MISMA salvedad que la regla de abajo: si no mueve, no era eso. Desde
    // `zfsm://local/local` —el pool `local` de la conexión `local`— un `cd local` daba otra
    // vez `zfsm://local/local` y el intérprete se quedaba callado, aunque existiera un hijo
    // `local/local`. Cuando la lectura de «nombre ZFS completo» devuelve el sitio actual,
    // la única que queda en pie es la relativa: un hijo que se llama así.
    bool saltaAtajoDeConexion = false;
    if (!e.actual.pool.empty() && tramosDe(texto).front() == e.actual.pool) {
        ZfsmUrl porPool;
        std::string errPool;
        if (!aplica(base_conexion_de(e), tramosDe(texto), porPool, errPool)) {
            error = errPool;
            return false;
        }
        if (B::formatZfsmUrl(porPool) != B::formatZfsmUrl(e.actual)) {
            out = porPool;
            return true;
        }
        // Y se va DIRECTO a la relativa, sin pasar por el atajo de conexión: si el primer
        // tramo se llama igual que una conexión —pasa cuando la máquina y su pool comparten
        // nombre— ese atajo se llevaría el `cd` a la raíz de la máquina, que es aún menos lo
        // que se quería decir que quedarse quieto.
        saltaAtajoDeConexion = true;
    }

    // Si el primer tramo nombra una CONEXIÓN, la ruta es absoluta. Es lo que uno escribe al
    // saltar de una máquina a otra, sin anteponer una barra que nadie recuerda.
    //
    // Se comprueba ANTES de probar la relativa, y esa es la corrección: antes se intentaba
    // la relativa primero y solo se caía aquí «si el primer tramo no encajaba donde
    // estamos». El problema es que estando DENTRO de una conexión no encajar es imposible:
    // cualquier nombre se acepta como pool sin preguntar si existe. Así que la regla nunca
    // llegaba a aplicarse y `cd unibody` desde `zfsm://local` acababa en
    // `zfsm://local/unibody`, que no existe — mientras la ayuda prometía lo contrario.
    //
    // Va DESPUÉS de la regla del pool: si uno está plantado en el pool `sback` y escribe
    // `sback/x`, quiere el nombre ZFS completo aunque exista una conexión llamada igual.
    // Para llegar a un hijo que se llame como una conexión queda `./nombre`, que empieza
    // por un tramo que no es nombre de nada.
    if (!saltaAtajoDeConexion && buscarConexion(e.conns, tramos.front()) != nullptr) {
        ZfsmUrl porConexion;
        std::string errConexion;
        if (!aplica(ZfsmUrl{}, tramos, porConexion, errConexion)) {
            error = errConexion;
            return false;
        }
        // …salvo que el atajo no MUEVA. Estando en `zfsm://fc16`, `cd fc16` resolvía como
        // absoluta desde la raíz y aterrizaba exactamente donde ya estabas: el intérprete no
        // decía nada y no pasaba nada, que es la peor respuesta posible a una orden que uno
        // ha escrito a propósito. Y en esa máquina había un pool llamado `fc16`, o sea que
        // lo escrito sí nombraba algo; solo que el atajo se lo comía antes.
        //
        // La regla es la que ya justifica el atajo: existe para SALTAR de máquina, y saltar
        // a la que ya se está no es un salto. Cuando el resultado coincide con el sitio
        // actual se sigue por la vía relativa, que desde una conexión lee el tramo como el
        // pool — que es lo que quedaba por probar y lo que el usuario quería decir.
        //
        // Se compara el resultado, no «¿es el primer tramo mi conexión?»: así `cd fc16`
        // desde un dataset hondo de fc16 sigue subiendo a la raíz de la máquina, que sí
        // mueve y sí es útil.
        if (B::formatZfsmUrl(porConexion) != B::formatZfsmUrl(e.actual)) {
            out = porConexion;
            return true;
        }
    }

    std::string errRelativa;
    if (aplica(e.actual, tramos, out, errRelativa)) {
        return true;
    }
    error = errRelativa;
    return false;
}

// Resolver, y lo que vale para TODAS las URL sea cual sea la forma en que se escribieron:
// `#path` cuelga de una máquina, nunca de un dataset. Ahí la ruta es `#content`, y
// aceptarlo sería tener dos nombres para lo mismo —o, peor, uno que parece lo que no es—.
bool resuelve(const Estado& e, const std::string& texto, ZfsmUrl& out, std::string& error) {
    if (!resuelveCrudo(e, texto, out, error)) {
        return false;
    }
    if (B::toLowerAscii(out.section) == B::zfsmSection::kPath && !out.dataset.empty()) {
        error = CS::labelOf(CS::Failure::PathOnDataset);
        return false;
    }
    return true;
}

// Una copia del perfil con las credenciales de sudo puestas si hacían falta y no estaban.
//
// Hace falta en los caminos que ejecutan una orden de shell —el listado de contenido—,
// porque `withSudoCommand` mete la contraseña en la orden y sin ella `sudo` se queda
// pidiéndola por un terminal que no está escuchando. El RPC del daemon no pasa por aquí:
// allí quien eleva es el propio daemon.
B::ConnectionProfile conSudo(Estado& e, const B::ConnectionProfile& p) {
    B::ConnectionProfile copia = p;
    if (!copia.useSudo || !copia.password.empty() || !T::isLocalConnection(copia)) {
        return copia;
    }
    e.ses->transporte.resolveLocalSudo(copia);
    return copia;
}

void recarga(Estado& e) {
    e.conns = cargarConexiones(e.ses->dirConfig, e.ses->maestra);
    bool cambio = false;
    for (const auto& p : e.conns.perfiles) {
        const std::string id = p.id.empty() ? p.name : p.id;
        std::string ruta;
        if (!sshClaveFalta(p, ruta) || e.conns.desconectada(id)) {
            continue;
        }
        std::string error;
        if (marcarDesconectada(*e.ses, id, true, error)) {
            cambio = true;
            std::fprintf(stderr,
                         TC("t_clave_falta_desconectada",
                            "aviso: la clave SSH de «%s» no existe (%s); la conexión queda "
                            "desconectada. Cuando la clave esté: connect zfsm://%s\n"),
                         id.c_str(), ruta.c_str(), id.c_str());
        }
    }
    if (cambio) {
        e.conns = cargarConexiones(e.ses->dirConfig, e.ses->maestra);
    }
}
bool sshClaveFalta(const B::ConnectionProfile& p, std::string& ruta) {
    return H::sshKeyMissing(p, ruta);
}

bool estaApartada(const Estado& e, const std::string& id) {
    return e.conns.desconectada(id);
}

// El perfil de la conexión que nombra una URL.
const B::ConnectionProfile* perfilDe(const Estado& e, const ZfsmUrl& u, std::string& error) {
    if (u.connection.empty()) {
        error = T("t_falta_conexion", "hace falta estar en una conexión");
        return nullptr;
    }
    const auto* p = buscarConexion(e.conns, u.connection);
    if (!p) {
        error = B::format(T("t_no_conn_nombre", "no hay ninguna conexión llamada «%1»"),
                          {u.connection});
    }
    return p;
}

// El perfil de una conexión CON LA QUE SE VA A HABLAR. Distinto de `perfilDe`: aquí se
// respeta la marca de desconexión, que es lo que le da sentido.
//
// Navegar a una conexión apartada sí se permite —hay que poder llegar a ella para volver a
// conectarla—; lo que no se hace es tocar la máquina.
const B::ConnectionProfile* perfilVivoDe(const Estado& e, const ZfsmUrl& u, std::string& error) {
    const auto* p = perfilDe(e, u, error);
    if (!p) {
        return nullptr;
    }
    const std::string id = p->id.empty() ? p->name : p->id;
    if (estaApartada(e, id)) {
        error = B::format(
            T("t_apartada", "%1 está marcada como desconectada; use «connect %1» para usarla"),
            {id});
        return nullptr;
    }
    return p;
}

// --- Ejecutar un verbo del agente sobre el destino, contando lo que salga mal.
// `errorDeOrden`: si se da, el error de la ORDEN (rc ≠ 0) se devuelve ahí en vez de
// imprimirse, para quien tiene un plan B —`status` de un pool no importado busca entre los
// importables, y el «no such pool» de zfs solo vale si ese plan B tampoco encuentra nada—.
bool agente(Estado& e, const ZfsmUrl& destino, const std::vector<std::string>& args,
            std::string& out, int timeoutMs, std::string* errorDeOrden) {
    out.clear();
    std::string error;
    const auto* p = perfilVivoDe(e, destino, error);
    if (!p) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    std::string err;
    int rc = -1;
    std::string motivo;
    if (!ejecutarAgente(*e.ses, *p, args, out, err, rc, &motivo, timeoutMs)) {
        std::fprintf(stderr, TC("t_no_se_pudo_f6e380", "no se pudo hablar con %s: %s\n"),
                     (p->name.empty() ? p->id : p->name).c_str(), motivo.c_str());
        e.ultimoRc = 1;
        return false;
    }
    if (rc != 0) {
        const std::string detalle = B::trim(err).empty() ? B::trim(out) : B::trim(err);
        if (errorDeOrden) {
            *errorDeOrden = detalle;
        } else {
            std::fprintf(stderr, "%s\n", detalle.empty() ? "la orden falló" : detalle.c_str());
        }
        e.ultimoRc = rc;
        return false;
    }
    e.ultimoRc = 0;
    return true;
}

// El destino de una orden: `--on`/`--from` si se dan, y si no, el sitio actual.
//
// **Las dos son sinónimas a propósito.** «from» es la palabra natural en las órdenes que
// además tienen un destino —`todir`, `assemble`—, y «on» en las que actúan sobre un solo
// sitio. Obligar a recordar cuál lleva cada una sería una regla que no aporta nada.
// El destino cuando la orden NO tiene argumentos propios: vale escribirlo suelto.
//
// Existe porque `edit fc16` se limitaba a IGNORAR el «fc16» y editaba la conexión donde
// uno estaba — decía «actualizada la conexión local» y uno se quedaba pensando que había
// editado fc16. Ignorar un argumento en silencio es la peor manera de equivocarse.

// Pide un campo por el terminal, ofreciendo un valor por omisión entre corchetes.
// Devuelve false solo si no hay terminal: en un guion se usan las opciones.
bool pide(const std::string& etiqueta, const std::string& porOmision, std::string& out) {
    std::string err;
    const std::string aviso = etiqueta + (porOmision.empty() ? "" : " [" + porOmision + "]") + ": ";
    if (!preguntarPorTerminal(aviso, out, err)) {
        return false;
    }
    out = B::trim(out);
    if (out.empty()) {
        out = porOmision;
    }
    return true;
}

// --- El preámbulo único: resolver el destino y repartir los argumentos contra la FIRMA.
//
// Sustituye a las cuatro maneras que convivían de hacer lo mismo. Ver docs/gramatica_cli.md.
//
// Lo importante no es que centralice: es la última comprobación. **Lo que no consume
// ninguna ranura es un error.** Sin ella, una orden que no mira sus argumentos sueltos los
// ignora en silencio, que es como `install-daemon oldlau` acabó reinstalando el daemon en
// la máquina local sin decir nada.

// ¿Encaja este nodo en lo que la orden pide?
bool encaja(const ZfsmUrl& u, Objetivo pedido) {
    switch (pedido) {
        case Objetivo::None_:
        case Objetivo::Cualquiera:
            return true;
        case Objetivo::Conexion:
            // La conexión y NADA MÁS. Antes bastaba con que la URL tuviera conexión, y con
            // eso `install-daemon local/sobra1` se daba por bueno tomando «sobra1» como si
            // fuera una máquina.
            // Una ruta suelta de la máquina no es la máquina: `devices zfsm://m#path/x` es
            // un error, no `devices` de m. Desde la POSICIÓN sí se sube (subeHasta).
            return nodoDe(u) == Nodo::Conexion
                   && B::toLowerAscii(u.section) != B::zfsmSection::kPath;
        case Objetivo::Pool:
            return nodoDe(u) == Nodo::Dataset && u.isPoolRoot();
        case Objetivo::Dataset:
            return nodoDe(u) == Nodo::Dataset;
        case Objetivo::Snapshot:
            return nodoDe(u) == Nodo::Snapshot;
        case Objetivo::DatasetOInstantanea:
            return nodoDe(u) == Nodo::Dataset || nodoDe(u) == Nodo::Snapshot;
    }
    return false;
}

// Devuelve `std::string` y NO `const char*`: `TC()` da el `c_str()` de un temporal, así
// que un puntero a eso queda colgando en cuanto termina la expresión. Se coló aquí y el
// mensaje salió sin el objeto —«hace falta  (ahora: zfsm://local)»—, que es el mismo fallo
// que ya había mordido en `etiqueta()`. El compilador no lo ve: no es un switch incompleto,
// es una vida útil.
std::string nombreDe(Objetivo pedido) {
    switch (pedido) {
        case Objetivo::None_:
            return {};
        case Objetivo::Cualquiera:
            return {};
        case Objetivo::Conexion:
            return T("t_obj_conexion", "una conexión");
        case Objetivo::Pool:
            return T("t_obj_pool", "un pool");
        case Objetivo::Dataset:
            return T("t_obj_dataset", "un dataset");
        case Objetivo::Snapshot:
            return T("t_obj_snap", "un snapshot");
        case Objetivo::DatasetOInstantanea:
            return T("t_obj_ds_snap", "un dataset o un snapshot");
    }
    return {};
}

// El sitio ACTUAL, subido hasta lo que la orden pide.
//
// Un argumento explícito tiene que ser exactamente lo que se pide —si escribo algo, quiero
// eso—, pero el sitio actual se interpreta con sentido: estando en `local/tank/datos`,
// `install-daemon` habla de la MÁQUINA, no del dataset, y no tiene sentido obligar a subir
// a mano antes de teclearlo.
ZfsmUrl subeHasta(const ZfsmUrl& u, Objetivo pedido) {
    ZfsmUrl r = u;
    switch (pedido) {
        case Objetivo::Conexion:
            r.pool.clear();
            r.dataset.clear();
            r.snapshot.clear();
            r.section.clear();
            r.detail.clear();
            if (!r.connection.empty()) {
                r.kind = ZfsmKind::Connection;
            }
            return r;
        case Objetivo::Pool:
            if (!r.pool.empty()) {
                r.dataset = r.pool;
                r.snapshot.clear();
                r.section.clear();
                r.detail.clear();
                r.kind = ZfsmKind::Dataset;
            }
            return r;
        case Objetivo::Dataset:
            // De una instantánea se sube a su dataset; de un dataset no se sube.
            if (!r.snapshot.empty()) {
                r.snapshot.clear();
                r.section.clear();
                r.detail.clear();
                r.kind = ZfsmKind::Dataset;
            }
            return r;
        case Objetivo::None_:
        case Objetivo::Cualquiera:
        case Objetivo::Snapshot:
        case Objetivo::DatasetOInstantanea:
            return r;
    }
    return r;
}

bool prepara(Estado& e, const LineaAnalizada& linea, Peticion& p) {
    const Orden* orden = ordenPorNombre(linea.verbo);
    if (!orden) {
        return false;
    }
    p.l = &linea;
    p.orden = orden;

    std::string explicito = p.valor("on");
    // En `fromdir`, `--from` es la máquina de ORIGEN y no el destino de la orden: es la única
    // en la que las dos opciones no son sinónimas. Tomarlo por el destino lo comprobaba
    // contra «un dataset» y `fromdir … --from zfsm://maq` no podía funcionar nunca.
    if (explicito.empty() && std::string(orden->nombre) != "fromdir") {
        explicito = p.valor("from");
    }
    if (explicito.empty()) {
        explicito = linea.objetivo;
    }
    if (!explicito.empty()) {
        std::string err;
        if (!resuelve(e, explicito, p.objetivo, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return false;
        }
    } else {
        p.objetivo = subeHasta(e.actual, orden->objetivo);
        // Una orden de MÁQUINA cuya ruta viene como `zfsm://maq#path/…` —`labelclear
        // zfsm://fc16#path/dev/sda9`—: la máquina es la de la URL, no la posición.
        if (orden->objetivo == Objetivo::Conexion) {
            for (const Ranura& r : orden->ranuras) {
                if (r.tipo != Ranura::Tipo::Ruta || p.lista(r.nombre).empty()) {
                    continue;
                }
                ZfsmUrl u;
                std::string err;
                if (B::startsWith(B::toLowerAscii(p.uno(r.nombre)), "zfsm://")
                    && resuelve(e, p.uno(r.nombre), u, err) && esRutaSuelta(u)) {
                    p.objetivo = subeHasta(u, Objetivo::Conexion);
                }
            }
        }
    }

    // Las BANDERAS CORTAS, contra lo declarado. Es el mismo criterio que las largas, y su
    // ausencia era un agujero: `import apar -N` se tragaba la bandera sin protestar Y sin
    // pasarla a zpool, así que ni hacía lo pedido ni lo decía.
    for (const std::string& b : linea.banderas) {
        bool declarada = false;
        for (const Nativa& n : orden->nativas) {
            if (b == n.forma) {
                declarada = true;
            }
        }
        for (const Parametro& par : orden->params) {
            const std::string forma = T(par.forma.clave, par.forma.es);
            for (const std::string& trozo : B::split(forma, " ", true)) {
                if (trozo == b) {
                    declarada = true;
                }
            }
        }
        // `-y` es universal: confirma cualquier acción destructiva.
        if (!declarada && b != "-y") {
            std::fprintf(stderr, TC("t_bandera_no_admitida", "«%s» no es una bandera de %s\n"),
                         b.c_str(), orden->nombre);
            return false;
        }
    }

    // Las OPCIONES que la orden no declara son un error, no ruido que se ignora.
    //
    // Sin esto, `ls --daemon` dentro de una conexión se tragaba la opción y listaba los
    // pools como si nada: `ls` sí declara `--daemon`, pero solo hace algo en la raíz. Y una
    // opción mal escrita —`--daemn`— desaparecía sin más. Es la misma familia que los
    // argumentos ignorados, por la otra puerta.
    //
    // Las declaradas se leen del catálogo, que ya las tiene como datos: la línea de un
    // parámetro es «--delete» o «--name / --type / --os» o «--password-fd <n>», así que se
    // saca de ahí cada palabra que empiece por dos guiones.
    for (const auto& kv : linea.opciones) {
        static const std::set<std::string> universales{"on", "from"};
        if (universales.count(kv.first) > 0) {
            continue;
        }
        bool declarada = false;
        for (const Nativa& n : orden->nativas) {
            // Peladas de guiones, que es como se guardan. Comparando solo las largas, una
            // bandera corta CON valor —`trim -r 100M`— se declaraba y aun así se rechazaba.
            std::string f = n.forma;
            while (!f.empty() && f.front() == '-') {
                f.erase(f.begin());
            }
            if (f == kv.first) {
                declarada = true;
            }
        }
        for (const Parametro& par : orden->params) {
            const std::string forma = T(par.forma.clave, par.forma.es);
            for (const std::string& trozo : B::split(forma, " ", true)) {
                if (B::startsWith(trozo, "--") && trozo.substr(2) == kv.first) {
                    declarada = true;
                }
            }
        }
        if (!declarada) {
            // Con uno o dos guiones según su forma: las claves se guardan peladas, y
            // escribir siempre «--» delante producía cosas como «---r».
            const std::string comoSeEscribe = (kv.first.size() == 1 ? "-" : "--") + kv.first;
            std::fprintf(stderr, TC("t_opcion_no_admitida", "«%s» no es una opción de %s\n"),
                         comoSeEscribe.c_str(), orden->nombre);
            return false;
        }
    }

    // Un FICHERO —`#path/…` o `#content/…`— donde la orden quiere algo de ZFS: solo si es
    // exactamente el punto de montaje (ver equivaleADataset). `destroy` y `create` declaran
    // «cualquiera» porque también actúan sobre pools y conexiones, pero tampoco pueden
    // tomar un subdirectorio por su dataset. `rsync` maneja sus extremos él mismo.
    //
    // Desde la POSICIÓN dentro de `#content` se sigue actuando sobre el dataset, como
    // siempre: estando en `ds#content/docs`, `snapshot` es del dataset en el que se está.
    // En `send`, un fichero escrito como origen puede ser un flujo ya volcado: lo decide
    // cmdSend, que es quien sabe mirar si es un fichero o el directorio de una instantánea.
    if (std::string(orden->nombre) == "send" && !explicito.empty()
        && esSeccionDeFicheros(p.objetivo)) {
        return true;
    }
    {
        const bool quiereZfs =
            orden->objetivo == Objetivo::Pool || orden->objetivo == Objetivo::Dataset
            || orden->objetivo == Objetivo::Snapshot
            || orden->objetivo == Objetivo::DatasetOInstantanea
            || std::string(orden->nombre) == "destroy" || std::string(orden->nombre) == "create";
        if (quiereZfs && std::string(orden->nombre) != "rsync" && esSeccionDeFicheros(p.objetivo)
            && (!explicito.empty() || esRutaSuelta(p.objetivo))) {
            // Y después NO se sube: si el punto de montaje es de `tank/data`, una orden de
            // pool no puede acabar en `tank`. Lo que no encaje lo dice la comprobación de
            // abajo.
            if (!equivaleADataset(e, p.objetivo, explicito)) {
                return false;
            }
        }
    }

    if (orden->objetivo != Objetivo::None_ && !encaja(p.objetivo, orden->objetivo)) {
        // Si el destino vino ESCRITO, se dice qué es lo que no encaja; si es el sitio
        // actual, se dice qué falta. Son dos errores distintos para quien los lee.
        if (!explicito.empty()) {
            std::fprintf(stderr, TC("t_no_es_lo_pedido", "%s no es %s\n"), explicito.c_str(),
                         nombreDe(orden->objetivo).c_str());
        } else {
            std::fprintf(stderr, TC("t_hace_falta_obj", "hace falta %s (ahora: %s)\n"),
                         nombreDe(orden->objetivo).c_str(), textoDe(p.objetivo).c_str());
        }
        return false;
    }
    return true;
}

bool confirma(const Estado& e, const std::string& que) {
    if (e.asumirSi) {
        return true;
    }
    if (!hayTerminal()) {
        std::fprintf(stderr,
                     TC("t_s_hace_fal_27dc69",
                        "%s: hace falta confirmación y no hay terminal. Use -y si está seguro.\n"),
                     que.c_str());
        return false;
    }
    std::string resp;
    std::string err;
    if (!preguntarPorTerminal(que + T("t_sufijo_sn", " [s/N]: "), resp, err)) {
        return false;
    }
    const std::string r = B::toLowerAscii(B::trim(resp));
    return r == "s" || r == "si" || r == "sí" || r == "y" || r == "yes";
}

// Deja en la tabla solo las filas cuyo nombre casa con el filtro, mirando las columnas
// dadas (la del nombre; en las conexiones, el identificador y el nombre visible).
//
// Falso —y lo dice— si con filtro no queda ninguna: «nada coincide» es un error y no una
// lista vacía. Una lista vacía se lee como «no hay nada», que es otra afirmación.
bool aplicaFiltro(Estado& e, Tabla& t, std::initializer_list<std::size_t> columnas) {
    if (e.filtro.patron.empty()) {
        return true;
    }
    std::vector<std::vector<std::string>> quedan;
    for (auto& fila : t.filas) {
        bool casa = false;
        for (const std::size_t c : columnas) {
            if (c >= fila.size()) {
                continue;
            }
            std::string nombre = fila[c];
            const bool esSnap = !nombre.empty() && nombre[0] == '@';
            if (e.filtro.clase == FiltroFilas::Clase::Hijos && esSnap) {
                continue;
            }
            if (e.filtro.clase == FiltroFilas::Clase::Instantaneas) {
                if (!esSnap) {
                    continue;
                }
                nombre = nombre.substr(1);
            }
            if (e.filtro.ocultos && !nombre.empty() && nombre[0] == '.'
                && (e.filtro.patron.empty() || e.filtro.patron[0] != '.')) {
                continue;
            }
            if (B::matchesRawPattern(e.filtro.patron, nombre)) {
                casa = true;
                break;
            }
        }
        if (casa) {
            quedan.push_back(std::move(fila));
        }
    }
    t.filas = std::move(quedan);
    if (t.filas.empty() && e.filtro.enVarios) {
        e.filtro.quedoVacio = true;
        return false;
    }
    if (t.filas.empty()) {
        std::fprintf(stderr, TC("t_nada_coincide", "nada coincide con «%s»\n"),
                     e.filtro.texto.c_str());
        e.ultimoRc = 1;
        return false;
    }
    return true;
}

// --- ls

// El listado de conexiones es EL MISMO que el de la orden suelta `connections list`. La
// tabla se construye en un solo sitio (session.cpp): tenerla duplicada hacía que la misma
// pregunta se contestara con columnas distintas según por dónde se preguntara.
// El listado de conexiones, SIEMPRE con la versión del agente de cada máquina, marcada
// con `*` cuando no es la que espera este cliente.
//
// Estuvo detrás de una opción `--daemon` con el argumento de que `ls` en la raíz era la
// única orden que responde al instante y sin hablar con nadie, y que con una máquina
// apagada cada una cuesta su plazo de espera. Se retiró porque el argumento se cae por su
// propio peso: la versión del agente es la razón por la que uno mira ese listado, y una
// columna que hay que pedir aparte es una columna que no se mira. Quien no quiera esperar
// tiene la conexión desconectada, que sigue sin sondearse.
//
// El plazo por máquina es de 8 s y el resultado se recuerda durante la sesión —el fallo
// también—, así que la espera se paga una vez.
// Olvida la versión del daemon que la sesión recordaba para esta conexión.
//
// `listaConexiones` la recuerda toda la sesión —el fallo también— para no esperar dos veces
// por una máquina que no contesta. Pero hay órdenes que la dejan OBSOLETA en el acto, y sin
// esto el «-» de antes de instalar seguía saliendo después de instalar, hasta salir del
// intérprete: `install-daemon` cambia el daemon, `edit` puede cambiar la máquina a la que
// apunta la conexión, y `refresh` es justo para volver a preguntar.
void olvidaVersionDaemon(Estado& e, const B::ConnectionProfile& p) {
    e.versionDaemon.erase(p.id);
    e.versionDaemon.erase(p.name);
    e.avisoDaemon.erase(p.id);
    e.avisoDaemon.erase(p.name);
}

// TODO lo que la sesión recuerda de una conexión y que una de estas órdenes deja obsoleto: el
// túnel abierto, el material TLS leído, la penalización por un fallo reciente y la versión
// del daemon. Es lo que hacía `refresh`, sacado aquí para que `install-daemon` y `edit` hagan
// lo mismo en vez de dejar a medias lo que había.
//
// Las tres primeras van por usuario, máquina, puerto y clave SSH; tras un `edit` que cambie
// la dirección hay que llamarla con el perfil de ANTES, o el túnel contra la máquina vieja
// seguiría abierto.
void olvidaEstadoDeConexion(Estado& e, const B::ConnectionProfile& p) {
    T::closeTunnelForConnection(e.ses->transporte, p);
    T::clearRemoteDaemonTlsCacheForConnection(p);
    {
        std::lock_guard<std::mutex> lock(e.ses->transporte.mutex);
        const std::string clave = T::remoteDaemonTlsCacheKey(p);
        e.ses->transporte.retryAfterByConnKey.erase(clave);
        e.ses->transporte.retryReasonByConnKey.erase(clave);
    }
    olvidaVersionDaemon(e, p);
}

}  // namespace zfsmgr::cli::detail
