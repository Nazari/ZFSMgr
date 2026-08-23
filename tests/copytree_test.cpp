#include "copytree.h"
#include "arbolremoto.h"

#include <QtTest/QtTest>

#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/xattr.h>
#endif
#endif

namespace fs = std::filesystem;
namespace AR = zfsmgr::arbolremoto;
using namespace zfsmgr::copytree;

namespace {

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

// La copia de árboles sustituye a rsync en operaciones que BORRAN el origen
// (Desglosar, Ensamblar, Hacia Dir). El camino feliz importa menos que los casos
// límite: por eso el grueso de este fichero son enlaces, exclusiones y verificación.
class CopyTreeTest final : public QObject {
    Q_OBJECT

    fs::path root_;
    fs::path src_;
    fs::path dst_;

private Q_SLOTS:
    void init() {
        root_ = fs::temp_directory_path()
                / fs::path("zfsmgr-copytree-" + std::to_string(QRandomGenerator::global()->generate()));
        src_ = root_ / "src";
        dst_ = root_ / "dst";
        fs::create_directories(src_);
        fs::create_directories(dst_);
    }

    void cleanup() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    void copiesTreeAndContents() {
        writeFile(src_ / "a.txt", "hola");
        writeFile(src_ / "sub" / "b.txt", "que tal");
        writeFile(src_ / "sub" / "hondo" / "c.txt", "bien");

        const Result r = copyTree(src_.string(), dst_.string(), {});
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(readFile(dst_ / "a.txt"), std::string("hola"));
        QCOMPARE(readFile(dst_ / "sub" / "b.txt"), std::string("que tal"));
        QCOMPARE(readFile(dst_ / "sub" / "hondo" / "c.txt"), std::string("bien"));
        QCOMPARE(r.filesCopied, static_cast<std::uint64_t>(3));
    }

    // Las exclusiones van ANCLADAS en la raíz. Sin eso, excluir "Tools" se comería
    // cualquier directorio con ese nombre en todo el subárbol, que no es lo pedido.
    void excludesOnlyAtTopLevel() {
        writeFile(src_ / "Tools" / "x.txt", "fuera");
        writeFile(src_ / "sub" / "Tools" / "y.txt", "dentro");

        Options opt;
        opt.excludes = {"Tools"};
        const Result r = copyTree(src_.string(), dst_.string(), opt);
        QVERIFY2(r.ok, r.error.c_str());
        QVERIFY(!fs::exists(dst_ / "Tools"));
        QVERIFY2(fs::exists(dst_ / "sub" / "Tools" / "y.txt"),
                 "un 'Tools' anidado NO debe excluirse");
    }

    // **Las ranuras se declaran SIEMPRE y el salto va DENTRO.**
    //
    // Estaban tras un «#ifndef _WIN32», y moc no evalúa esa guarda: genera las
    // referencias a las cuatro ranuras y el compilador, que sí la evalúa, no las tiene.
    // El CI de Windows moría en el .moc con «preservesHardLinks: is not a member of
    // CopyTreeTest», señalando un fichero generado. Con QSKIP la lista de pruebas es la
    // misma en las tres plataformas y en Windows se lee «skipped», que es la verdad.

    // La diferencia principal con robocopy, y la razón de implementarlo en vez de
    // llamarlo: dos rutas al mismo fichero deben seguir siéndolo en el destino, no
    // convertirse en dos copias que ocupan el doble.
    void preservesHardLinks() {
#ifdef _WIN32
        QSKIP("enlaces duros, symlinks, permisos y huecos son de POSIX: en Windows la copia no los replica y la prueba no significaría nada");
#else
        writeFile(src_ / "uno.txt", "compartido");
        QCOMPARE(::link((src_ / "uno.txt").c_str(), (src_ / "dos.txt").c_str()), 0);

        const Result r = copyTree(src_.string(), dst_.string(), {});
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(r.hardLinksRecreated, static_cast<std::uint64_t>(1));

        struct stat a {};
        struct stat b {};
        QCOMPARE(::stat((dst_ / "uno.txt").c_str(), &a), 0);
        QCOMPARE(::stat((dst_ / "dos.txt").c_str(), &b), 0);
        QVERIFY2(a.st_ino == b.st_ino, "el destino debe conservar el enlace duro");
        QCOMPARE(readFile(dst_ / "dos.txt"), std::string("compartido"));
#endif
    }

    void preservesSymlinksAsLinks() {
#ifdef _WIN32
        QSKIP("enlaces duros, symlinks, permisos y huecos son de POSIX: en Windows la copia no los replica y la prueba no significaría nada");
#else
        writeFile(src_ / "real.txt", "contenido");
        fs::create_symlink("real.txt", src_ / "enlace.txt");

        const Result r = copyTree(src_.string(), dst_.string(), {});
        QVERIFY2(r.ok, r.error.c_str());
        QVERIFY2(fs::is_symlink(fs::symlink_status(dst_ / "enlace.txt")),
                 "debe copiarse como enlace, no como el fichero al que apunta");
        QCOMPARE(fs::read_symlink(dst_ / "enlace.txt").string(), std::string("real.txt"));
#endif
    }

    void preservesPermissions() {
#ifdef _WIN32
        QSKIP("enlaces duros, symlinks, permisos y huecos son de POSIX: en Windows la copia no los replica y la prueba no significaría nada");
#else
        writeFile(src_ / "script.sh", "#!/bin/sh\n");
        QCOMPARE(::chmod((src_ / "script.sh").c_str(), 0755), 0);

        const Result r = copyTree(src_.string(), dst_.string(), {});
        QVERIFY2(r.ok, r.error.c_str());
        struct stat st {};
        QCOMPARE(::stat((dst_ / "script.sh").c_str(), &st), 0);
        QCOMPARE(st.st_mode & 07777, static_cast<mode_t>(0755));
#endif
    }

    // Un fichero mayormente vacío no debe llegar ocupando su tamaño aparente: una imagen
    // de disco de 100 GB con 2 GB usados arruinaría el destino.
    void keepsSparseFilesSparse() {
#ifdef _WIN32
        QSKIP("enlaces duros, symlinks, permisos y huecos son de POSIX: en Windows la copia no los replica y la prueba no significaría nada");
#else
        const fs::path big = src_ / "disperso.img";
        {
            std::ofstream out(big, std::ios::binary);
            out.seekp(8 * 1024 * 1024 - 1);
            out.put('\0');
        }
        writeFile(src_ / "otro.txt", "x");

        const Result r = copyTree(src_.string(), dst_.string(), {});
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(fs::file_size(dst_ / "disperso.img"), fs::file_size(big));

        struct stat st {};
        QCOMPARE(::stat((dst_ / "disperso.img").c_str(), &st), 0);
        // 8 MiB aparentes; si se hubieran escrito los ceros ocuparía ~16384 bloques.
        QVERIFY2(st.st_blocks < 1024,
                 "el destino no debe materializar los huecos como ceros en disco");
#endif
    }



    // Los atributos extendidos, y con ellos las ACL POSIX de Linux, que viven en
    // `system.posix_acl_access`.
    //
    // Este caso no existía y por eso pasó desapercibido: al sustituir rsync se perdieron
    // las ACL, y la comparación que hice entonces no las incluía. `rsync -A` conservaba
    // el permiso y esta copia lo dejaba fuera.
    void preservesExtendedAttributes() {
#ifndef __linux__
        QSKIP("los atributos extendidos de esta forma son de Linux");
#else
        writeFile(src_ / "conattr.txt", "x");
        const std::string path = (src_ / "conattr.txt").string();
        if (::lsetxattr(path.c_str(), "user.zfsmgr_prueba", "valor", 5, 0) != 0) {
            QSKIP("el sistema de ficheros de /tmp no admite atributos extendidos");
        }
        const Result r = copyTree(src_.string(), dst_.string(), {});
        QVERIFY2(r.ok, r.error.c_str());
        char buf[32] = {};
        const std::string dstPath = (dst_ / "conattr.txt").string();
        const ssize_t n = ::lgetxattr(dstPath.c_str(), "user.zfsmgr_prueba", buf, sizeof(buf));
        QVERIFY2(n == 5, "el atributo extendido debe llegar al destino");
        QCOMPARE(std::string(buf, 5), std::string("valor"));
#endif
    }


    // Un fichero ENTERAMENTE disperso: todo huecos, sin un solo byte de datos.
    //
    // Se añadió tras comparar la copia nativa con la de rsync sobre el mismo árbol: el
    // tamaño aparente coincidía, pero el nativo ocupaba 4 KiB donde rsync dejaba 0,
    // porque fijaba el tamaño escribiendo un byte al final en vez de con truncate.
    void fullySparseFileAllocatesNothing() {
#ifdef _WIN32
        QSKIP("enlaces duros, symlinks, permisos y huecos son de POSIX: en Windows la copia no los replica y la prueba no significaría nada");
#else
        const fs::path big = src_ / "vacio.img";
        {
            std::ofstream out(big, std::ios::binary);
            out.seekp(8 * 1024 * 1024 - 1);
            out.put('\0');
        }
        const Result r = copyTree(src_.string(), dst_.string(), {});
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(fs::file_size(dst_ / "vacio.img"), fs::file_size(big));
        struct stat st {};
        QCOMPARE(::stat((dst_ / "vacio.img").c_str(), &st), 0);
        QVERIFY2(st.st_blocks == 0, "un fichero todo huecos no debe asignar ni un bloque");
#endif
    }

    // Lo que ya está igual no se reescribe. Es el comportamiento por omisión de rsync,
    // y sin él una sincronización rehace el árbol entero en cada pasada.
    void skipsWhatIsAlreadyThere() {
        writeFile(src_ / "a.txt", "uno");
        writeFile(src_ / "sub" / "b.txt", "dos");

        const Result first = copyTree(src_.string(), dst_.string(), {});
        QVERIFY2(first.ok, first.error.c_str());
        QCOMPARE(first.filesCopied, static_cast<std::uint64_t>(2));
        QCOMPARE(first.filesSkipped, static_cast<std::uint64_t>(0));

        // Segunda pasada sin cambios: no debe copiarse nada.
        const Result second = copyTree(src_.string(), dst_.string(), {});
        QVERIFY2(second.ok, second.error.c_str());
        QCOMPARE(second.filesCopied, static_cast<std::uint64_t>(0));
        QCOMPARE(second.filesSkipped, static_cast<std::uint64_t>(2));
        QCOMPARE(second.bytesWritten, static_cast<std::uint64_t>(0));

        // Y lo que cambia SÍ se vuelve a copiar.
        writeFile(src_ / "a.txt", "uno pero mas largo");
        const Result third = copyTree(src_.string(), dst_.string(), {});
        QVERIFY2(third.ok, third.error.c_str());
        QCOMPARE(third.filesCopied, static_cast<std::uint64_t>(1));
        QCOMPARE(readFile(dst_ / "a.txt"), std::string("uno pero mas largo"));
    }

    void deleteExtraneousRemovesWhatIsNotInSource() {
        writeFile(src_ / "queda.txt", "1");
        writeFile(dst_ / "sobra.txt", "2");
        writeFile(dst_ / "dirsobra" / "x.txt", "3");

        Options opt;
        opt.deleteExtraneous = true;
        const Result r = copyTree(src_.string(), dst_.string(), opt);
        QVERIFY2(r.ok, r.error.c_str());
        QVERIFY(fs::exists(dst_ / "queda.txt"));
        QVERIFY2(!fs::exists(dst_ / "sobra.txt"), "lo que no está en el origen se borra");
        QVERIFY2(!fs::exists(dst_ / "dirsobra"), "también los directorios");
        QCOMPARE(r.entriesDeleted, static_cast<std::uint64_t>(2));
    }

    // Lo excluido NO se borra. Dejarlo fuera de la copia a propósito y que el borrado se
    // lo lleve por delante sería lo peor de los dos mundos.
    void deleteExtraneousSpareExcluded() {
        writeFile(src_ / "datos.txt", "1");
        writeFile(dst_ / "Tools" / "x.txt", "no me toques");

        Options opt;
        opt.deleteExtraneous = true;
        opt.excludes = {"Tools"};
        const Result r = copyTree(src_.string(), dst_.string(), opt);
        QVERIFY2(r.ok, r.error.c_str());
        QVERIFY2(fs::exists(dst_ / "Tools" / "x.txt"), "lo excluido se protege del borrado");
        QCOMPARE(r.entriesDeleted, static_cast<std::uint64_t>(0));
    }

    // El simulacro cuenta pero no toca. Es lo que alimenta la vista previa, así que un
    // simulacro que modificara algo sería exactamente lo contrario de lo que promete.
    void dryRunTouchesNothing() {
        writeFile(src_ / "nuevo.txt", "1");
        writeFile(src_ / "sub" / "otro.txt", "2");
        writeFile(dst_ / "sobra.txt", "3");

        Options opt;
        opt.dryRun = true;
        opt.deleteExtraneous = true;
        const Result r = copyTree(src_.string(), dst_.string(), opt);
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(r.filesCopied, static_cast<std::uint64_t>(2));
        QCOMPARE(r.entriesDeleted, static_cast<std::uint64_t>(1));
        QCOMPARE(r.bytesWritten, static_cast<std::uint64_t>(0));

        QVERIFY2(!fs::exists(dst_ / "nuevo.txt"), "el simulacro no debe copiar");
        QVERIFY2(!fs::exists(dst_ / "sub"), "ni crear directorios");
        QVERIFY2(fs::exists(dst_ / "sobra.txt"), "ni borrar");
    }

    // La verificación previa al borrado. Es la única red que impide perder datos, así
    // que se prueba tanto que cuenta lo que falta como que llega a cero cuando no falta.
    void countPendingReportsMissingFiles() {
        writeFile(src_ / "a.txt", "1");
        writeFile(src_ / "sub" / "b.txt", "2");
        writeFile(src_ / "sub" / "c.txt", "3");

        QCOMPARE(countPending(src_.string(), dst_.string(), {}), 4LL);  // 3 ficheros + sub

        const Result r = copyTree(src_.string(), dst_.string(), {});
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(countPending(src_.string(), dst_.string(), {}), 0LL);
    }

    // Con las MISMAS exclusiones que la copia: si no, lo que se dejó fuera a propósito
    // contaría como pendiente y la verificación no llegaría nunca a cero.
    void countPendingHonoursExcludes() {
        writeFile(src_ / "datos.txt", "1");
        writeFile(src_ / "Tools" / "x.txt", "2");

        Options opt;
        opt.excludes = {"Tools"};
        const Result r = copyTree(src_.string(), dst_.string(), opt);
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(countPending(src_.string(), dst_.string(), opt), 0LL);
        // Y sin las exclusiones, lo omitido SÍ debe salir como pendiente.
        QVERIFY(countPending(src_.string(), dst_.string(), {}) > 0);
    }

    // Un origen que no existe no es «cero pendientes»: es «no se pudo comprobar». La
    // diferencia importa, porque el llamante borra el origen cuando ve un cero.
    void countPendingReportsFailureAsMinusOne() {
        QCOMPARE(countPending((root_ / "no-existe").string(), dst_.string(), {}), -1LL);
    }

    void copyTreeRefusesNonDirectorySource() {
        writeFile(root_ / "suelto.txt", "x");
        const Result r = copyTree((root_ / "suelto.txt").string(), dst_.string(), {});
        QVERIFY(!r.ok);
        QVERIFY(!r.error.empty());
    }

    // --- El arbol por el socket entre daemons -------------------------------------
    //
    // Aqui se prueba TODO menos la red: recorrer, el formato de cable y el plan. Lo que
    // queda fuera es el socket, que ya esta probado por el rele de las transferencias.

    void recorreDistingueLosCuatroTipos() {
        namedirs();
        std::vector<AR::Entrada> e;
        std::string err;
        QVERIFY2(AR::recorre(src_.string(), e, err), err.c_str());
        std::map<std::string, AR::Tipo> porRuta;
        for (const auto& x : e) porRuta[x.ruta] = x.tipo;
        QCOMPARE(porRuta.count("sub"), size_t(1));
        QVERIFY(porRuta["sub"] == AR::Tipo::Directorio);
        QVERIFY(porRuta["a.txt"] == AR::Tipo::Fichero);
        QVERIFY(porRuta["enlace"] == AR::Tipo::Enlace);
        // El segundo nombre del mismo fichero llega como enlace duro, no como copia: es
        // la diferencia entre sincronizar un arbol y duplicarlo. Y cual de los dos es «el
        // fichero» NO depende del orden del directorio: es el primero por orden
        // alfabetico, para que los dos extremos describan el arbol igual.
        QVERIFY(porRuta["duro.txt"] == AR::Tipo::EnlaceDuro);
        for (const auto& x : e) {
            if (x.ruta == "duro.txt") {
                QCOMPARE(QString::fromStdString(x.destino), QStringLiteral("a.txt"));
            }
        }
    }

    void loDelVolumenNoEsDelArbol() {
        // La papelera y la carpeta de restauracion son del VOLUMEN, no de quien lo usa.
        // Sin esto, sincronizar la raiz de una unidad Windows con borrado proponia borrar
        // la papelera entera; se vio en una pasada en seco contra una unidad de verdad.
        fs::create_directories(src_ / "$RECYCLE.BIN" / "S-1-5-21");
        writeFile(src_ / "$RECYCLE.BIN" / "S-1-5-21" / "desktop.ini", "x");
        fs::create_directories(src_ / "System Volume Information");
        // Pero uno del usuario con el mismo nombre MAS ABAJO si es suyo.
        fs::create_directories(src_ / "mio" / "$RECYCLE.BIN");
        writeFile(src_ / "mio" / "$RECYCLE.BIN" / "dato.txt", "mio");
        std::vector<AR::Entrada> e;
        std::string err;
        QVERIFY2(AR::recorre(src_.string(), e, err), err.c_str());
        for (const auto& x : e) {
            QVERIFY2(x.ruta.rfind("$RECYCLE.BIN", 0) != 0, x.ruta.c_str());
            QVERIFY2(x.ruta.rfind("System Volume Information", 0) != 0, x.ruta.c_str());
        }
        bool hayElDelUsuario = false;
        for (const auto& x : e) {
            if (x.ruta == "mio/$RECYCLE.BIN/dato.txt") hayElDelUsuario = true;
        }
        QVERIFY2(hayElDelUsuario, "lo del usuario mas abajo si va");
    }

    void lasRutasVanSiempreConBarras() {
        namedirs();
        std::vector<AR::Entrada> e;
        std::string err;
        QVERIFY(AR::recorre(src_.string(), e, err));
        for (const auto& x : e) {
            QVERIFY2(x.ruta.find('\\') == std::string::npos,
                     "una ruta con barra invertida no casaria en el otro extremo");
        }
    }

    void elManifiestoSobreviveAlViajeDeIdaYVuelta() {
        namedirs();
        std::vector<AR::Entrada> e;
        std::string err;
        QVERIFY(AR::recorre(src_.string(), e, err));
        std::vector<AR::Entrada> vuelta;
        QVERIFY2(AR::analizaManifiesto(AR::serializaManifiesto(e), vuelta, err), err.c_str());
        QCOMPARE(vuelta.size(), e.size());
        for (size_t i = 0; i < e.size(); ++i) {
            QCOMPARE(QString::fromStdString(vuelta[i].ruta), QString::fromStdString(e[i].ruta));
            QCOMPARE(vuelta[i].tamano, e[i].tamano);
            QCOMPARE(vuelta[i].fecha, e[i].fecha);
        }
    }

    void unNombreConSaltoDeLineaNoRompeElManifiesto() {
        // Las longitudes van explicitas justo por esto: un nombre puede llevar dentro un
        // salto de linea, y partir por lineas dejaria el manifiesto descolocado.
        std::vector<AR::Entrada> e(1);
        e[0].ruta = "raro\ncon salto.txt";
        e[0].tipo = AR::Tipo::Fichero;
        e[0].tamano = 7;
        std::vector<AR::Entrada> vuelta;
        std::string err;
        QVERIFY2(AR::analizaManifiesto(AR::serializaManifiesto(e), vuelta, err), err.c_str());
        QCOMPARE(vuelta.size(), size_t(1));
        QCOMPARE(QString::fromStdString(vuelta[0].ruta), QString::fromStdString(e[0].ruta));
    }

    void elPlanSaltaLoQueYaEstaIgual() {
        std::vector<AR::Entrada> o(1), d(1);
        o[0].ruta = d[0].ruta = "a.txt";
        o[0].tamano = d[0].tamano = 10;
        o[0].fecha = d[0].fecha = 1000;
        const AR::Plan p = AR::planea(o, d, false);
        QCOMPARE(p.operaciones.size(), size_t(0));
        QCOMPARE(p.iguales, uint64_t(1));
        QCOMPARE(p.bytes, uint64_t(0));
    }

    void unaFechaDistintaLoVuelveACopiar() {
        std::vector<AR::Entrada> o(1), d(1);
        o[0].ruta = d[0].ruta = "a.txt";
        o[0].tamano = d[0].tamano = 10;
        o[0].fecha = 1001;
        d[0].fecha = 1000;
        const AR::Plan p = AR::planea(o, d, false);
        QCOMPARE(p.operaciones.size(), size_t(1));
        QVERIFY(p.operaciones[0].accion == AR::Accion::Copiar);
        QCOMPARE(p.bytes, uint64_t(10));
    }

    void sinBorradoNoSeBorraNada() {
        std::vector<AR::Entrada> o;
        std::vector<AR::Entrada> d(1);
        d[0].ruta = "sobra.txt";
        QCOMPARE(AR::planea(o, d, false).operaciones.size(), size_t(0));
    }

    void conBorradoSeVaDeDentroHaciaFuera() {
        // Un directorio se borra DESPUES de lo que tiene dentro. Al reves, el borrado
        // falla y el motivo no explica por que.
        std::vector<AR::Entrada> o;
        std::vector<AR::Entrada> d(2);
        d[0].ruta = "dir";
        d[0].tipo = AR::Tipo::Directorio;
        d[1].ruta = "dir/dentro.txt";
        const AR::Plan p = AR::planea(o, d, true);
        QCOMPARE(p.operaciones.size(), size_t(2));
        QCOMPARE(QString::fromStdString(p.operaciones[0].entrada.ruta),
                 QStringLiteral("dir/dentro.txt"));
        QCOMPARE(QString::fromStdString(p.operaciones[1].entrada.ruta), QStringLiteral("dir"));
    }

    void unEnlaceQueCambiaDeDestinoSeRehace() {
        std::vector<AR::Entrada> o(1), d(1);
        o[0].ruta = d[0].ruta = "l";
        o[0].tipo = d[0].tipo = AR::Tipo::Enlace;
        o[0].destino = "a.txt";
        d[0].destino = "b.txt";
        const AR::Plan p = AR::planea(o, d, false);
        QCOMPARE(p.operaciones.size(), size_t(1));
        QVERIFY(p.operaciones[0].accion == AR::Accion::Enlazar);
    }

    void laCabeceraSobreviveAlViajeDeIdaYVuelta() {
        AR::Operacion o;
        o.accion = AR::Accion::Copiar;
        o.entrada.ruta = "sub/a.txt";
        o.entrada.destino = "";
        o.entrada.modo = 0644;
        o.entrada.fecha = 1700000000;
        o.entrada.tamano = 12345;
        AR::Operacion vuelta;
        size_t lr = 0, ld = 0;
        std::string err;
        QVERIFY2(AR::analizaCabecera(AR::cabeceraDe(o), vuelta, lr, ld, err), err.c_str());
        QVERIFY(vuelta.accion == AR::Accion::Copiar);
        QCOMPARE(vuelta.entrada.tamano, uint64_t(12345));
        QCOMPARE(vuelta.entrada.fecha, int64_t(1700000000));
        QCOMPARE(lr, o.entrada.ruta.size());
        QCOMPARE(ld, size_t(0));
    }

    // --- Delta: mandar solo lo que cambio -----------------------------------------

    void laSumaRodanteCoincideConLaCalculadaDeCero() {
        // El truco del algoritmo es actualizar la suma en O(1) al deslizar un byte. Si la
        // version rodante y la version «de cero» no dieran lo mismo, no se reconoceria
        // ningun bloque y el delta mandaria el fichero entero sin decir nada.
        std::string datos = "abcdefghijklmnopqrstuvwxyz0123456789";
        const size_t v = 8;
        const auto* p = reinterpret_cast<const unsigned char*>(datos.data());
        uint32_t s = AR::sumaRodante(p, v);
        for (size_t i = 0; i + v < datos.size(); ++i) {
            const unsigned char sale = p[i];
            const unsigned char entra = p[i + v];
            uint32_t a = s & 0xffff, b = (s >> 16) & 0xffff;
            a = (a - sale + entra) & 0xffff;
            b = (b - uint32_t(v) * sale + a) & 0xffff;
            s = a | (b << 16);
            QCOMPARE(s, AR::sumaRodante(p + i + 1, v));
        }
    }

    void unFicheroIgualNoMandaNiUnByte() {
        // Con contenido variado, para que cada bloque tenga su propia firma. Con un solo
        // byte repetido todos los bloques son iguales y la prueba mediria otra cosa.
        std::string datos;
        for (size_t i = 0; i < 300000; ++i) datos.push_back(char('a' + (i * 13) % 26));
        writeFile(src_ / "g.bin", datos);
        std::vector<AR::Firma> fi;
        std::string err;
        const size_t tb = 8192;
        QVERIFY2(AR::firmasDe((src_ / "g.bin").string(), tb, fi, err), err.c_str());
        std::vector<AR::Instruccion> ins;
        uint64_t literales = 0;
        QVERIFY2(AR::delta((src_ / "g.bin").string(), fi, tb, ins, literales, err), err.c_str());
        QCOMPARE(literales, uint64_t(0));
        // Y en UNA sola instruccion: los bloques seguidos se juntan.
        QCOMPARE(ins.size(), size_t(1));
        QVERIFY(ins[0].tipo == AR::TipoInstruccion::Copiar);
    }

    void unFicheroDeBloquesRepetidosNoExplotaEnInstrucciones() {
        // Un disco virtual medio vacio son megas del mismo byte: todos esos bloques tienen
        // la misma firma. Cogiendo siempre el primer candidato salia una instruccion por
        // bloque; prefiriendo el siguiente al ultimo copiado, sale una para todos.
        writeFile(src_ / "r.bin", std::string(300000, 'x'));
        std::vector<AR::Firma> fi;
        std::string err;
        const size_t tb = 8192;
        QVERIFY(AR::firmasDe((src_ / "r.bin").string(), tb, fi, err));
        std::vector<AR::Instruccion> ins;
        uint64_t literales = 0;
        QVERIFY(AR::delta((src_ / "r.bin").string(), fi, tb, ins, literales, err));
        QCOMPARE(literales, uint64_t(0));
        QVERIFY2(ins.size() <= 2,
                 QStringLiteral("salieron %1 instrucciones").arg(ins.size())
                     .toUtf8().constData());
    }

    void soloViajaLoQueCambio() {
        std::string base(300000, 'x');
        writeFile(dst_ / "viejo.bin", base);
        std::string nuevo = base;
        nuevo.replace(150000, 10, "CAMBIADOxx");
        writeFile(src_ / "nuevo.bin", nuevo);
        std::vector<AR::Firma> fi;
        std::string err;
        const size_t tb = 8192;
        QVERIFY(AR::firmasDe((dst_ / "viejo.bin").string(), tb, fi, err));
        std::vector<AR::Instruccion> ins;
        uint64_t literales = 0;
        QVERIFY(AR::delta((src_ / "nuevo.bin").string(), fi, tb, ins, literales, err));
        // Un cambio de 10 bytes no puede costar 300 KB: como mucho el bloque que lo
        // contiene. Si esto crece, el delta ha dejado de servir para nada.
        QVERIFY2(literales <= 2 * tb,
                 QStringLiteral("literales=%1").arg(literales).toUtf8().constData());
        QVERIFY(literales > 0);
    }

    void unByteInsertadoAlPrincipioNoTiraElReconocimiento() {
        // Este es el caso que separa la ventana deslizante de comparar bloques alineados:
        // con bloques alineados, insertar un byte al principio desplaza TODO y no se
        // reconoceria ni un bloque.
        std::string base(200000, 'q');
        for (size_t i = 0; i < base.size(); ++i) base[i] = char('a' + (i % 26));
        writeFile(dst_ / "b.bin", base);
        writeFile(src_ / "b.bin", std::string("Z") + base);
        std::vector<AR::Firma> fi;
        std::string err;
        const size_t tb = 8192;
        QVERIFY(AR::firmasDe((dst_ / "b.bin").string(), tb, fi, err));
        std::vector<AR::Instruccion> ins;
        uint64_t literales = 0;
        QVERIFY(AR::delta((src_ / "b.bin").string(), fi, tb, ins, literales, err));
        QVERIFY2(literales < 3 * tb,
                 QStringLiteral("con un byte insertado viajaron %1").arg(literales)
                     .toUtf8().constData());
    }

    void elDeltaReconstruyeExactamenteElOriginal() {
        // La prueba que de verdad importa: aplicar las instrucciones tiene que devolver el
        // fichero del origen byte a byte. Un fallo aqui corrompe datos en silencio.
        std::string viejo;
        for (size_t i = 0; i < 250000; ++i) viejo.push_back(char('a' + (i * 7) % 26));
        std::string nuevo = viejo;
        nuevo.replace(1000, 5, "XXXXX");
        nuevo.replace(120000, 3, "YYY");
        nuevo += "cola que antes no estaba";
        writeFile(dst_ / "v.bin", viejo);
        writeFile(src_ / "n.bin", nuevo);
        const size_t tb = AR::tamanoDeBloque(nuevo.size());
        std::vector<AR::Firma> fi;
        std::string err;
        QVERIFY(AR::firmasDe((dst_ / "v.bin").string(), tb, fi, err));
        std::vector<AR::Instruccion> ins;
        uint64_t literales = 0;
        QVERIFY(AR::delta((src_ / "n.bin").string(), fi, tb, ins, literales, err));

        std::string rehecho;
        for (const auto& in : ins) {
            if (in.tipo == AR::TipoInstruccion::Literal) {
                rehecho += in.datos;
            } else {
                for (uint64_t k = 0; k < in.cuantos; ++k) {
                    const size_t off = size_t(in.bloque + k) * tb;
                    rehecho += viejo.substr(off, std::min(tb, viejo.size() - off));
                }
            }
        }
        QCOMPARE(int(rehecho.size()), int(nuevo.size()));
        QVERIFY2(rehecho == nuevo, "lo reconstruido NO es igual al original");
    }

    void unFicheroMayorQueElBufferNoSeTrunca() {
        // Con bloques de 8 KiB el buffer interno es de 1 MiB. Un consumo que caia justo en
        // el final del buffer dejaba `ini == fin` y el bucle salia SIN volver a leer: el
        // delta describia solo el primer megabyte y el resto desaparecia en silencio.
        // Aqui se usan 3 MB para cruzar ese limite varias veces.
        std::string datos;
        for (size_t i = 0; i < 3u * 1024 * 1024; ++i) datos.push_back(char('a' + (i * 31) % 26));
        writeFile(dst_ / "big.bin", datos);
        std::string cambiado = datos;
        cambiado.replace(2u * 1024 * 1024, 4, "ZZZZ");
        writeFile(src_ / "big.bin", cambiado);
        const size_t tb = 8192;
        std::vector<AR::Firma> fi;
        std::string err;
        QVERIFY(AR::firmasDe((dst_ / "big.bin").string(), tb, fi, err));
        std::vector<AR::Instruccion> ins;
        uint64_t literales = 0;
        QVERIFY(AR::delta((src_ / "big.bin").string(), fi, tb, ins, literales, err));

        std::string rehecho;
        for (const auto& in : ins) {
            if (in.tipo == AR::TipoInstruccion::Literal) {
                rehecho += in.datos;
            } else {
                for (uint64_t k = 0; k < in.cuantos; ++k) {
                    const size_t off = size_t(in.bloque + k) * tb;
                    rehecho += datos.substr(off, std::min(tb, datos.size() - off));
                }
            }
        }
        QCOMPARE(int(rehecho.size()), int(cambiado.size()));
        QVERIFY2(rehecho == cambiado, "el delta trunco el fichero en el limite del buffer");
        QVERIFY2(literales <= 2 * tb,
                 QStringLiteral("viajaron %1 bytes").arg(literales).toUtf8().constData());
    }

    void lasFirmasSobrevivenAlViajeDeIdaYVuelta() {
        writeFile(src_ / "f.bin", std::string(100000, 'k'));
        std::vector<AR::Firma> fi;
        std::string err;
        QVERIFY(AR::firmasDe((src_ / "f.bin").string(), 8192, fi, err));
        std::vector<AR::Firma> vuelta;
        QVERIFY2(AR::analizaFirmas(AR::serializaFirmas(fi), vuelta, err), err.c_str());
        QCOMPARE(vuelta.size(), fi.size());
        for (size_t i = 0; i < fi.size(); ++i) {
            QCOMPARE(vuelta[i].debil, fi[i].debil);
            QCOMPARE(memcmp(vuelta[i].fuerte, fi[i].fuerte, 16), 0);
        }
    }

private:
    // Un arbol con los cuatro tipos dentro.
    void namedirs() {
        fs::create_directories(src_ / "sub");
        writeFile(src_ / "a.txt", "hola");
        fs::create_hard_link(src_ / "a.txt", src_ / "duro.txt");
        fs::create_symlink("a.txt", src_ / "enlace");
    }

};

QTEST_MAIN(CopyTreeTest)
#include "copytree_test.moc"
