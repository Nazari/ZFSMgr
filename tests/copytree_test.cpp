#include "copytree.h"
#include "remotetree.h"
#ifdef __linux__
#include <sys/xattr.h>
#endif

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
namespace AR = zfsmgr::remotetree;
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
        std::vector<AR::Entry> e;
        std::string err;
        QVERIFY2(AR::walk(src_.string(), e, err), err.c_str());
        std::map<std::string, AR::EntryKind> porRuta;
        for (const auto& x : e) porRuta[x.path] = x.kind;
        QCOMPARE(porRuta.count("sub"), size_t(1));
        QVERIFY(porRuta["sub"] == AR::EntryKind::Directory);
        QVERIFY(porRuta["a.txt"] == AR::EntryKind::File);
        QVERIFY(porRuta["enlace"] == AR::EntryKind::Symlink);
        // El segundo nombre del mismo fichero llega como enlace duro, no como copia: es
        // la diferencia entre sincronizar un arbol y duplicarlo. Y cual de los dos es «el
        // fichero» NO depende del orden del directorio: es el primero por orden
        // alfabetico, para que los dos extremos describan el arbol igual.
        QVERIFY(porRuta["duro.txt"] == AR::EntryKind::HardLink);
        for (const auto& x : e) {
            if (x.path == "duro.txt") {
                QCOMPARE(QString::fromStdString(x.target), QStringLiteral("a.txt"));
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
        std::vector<AR::Entry> e;
        std::string err;
        QVERIFY2(AR::walk(src_.string(), e, err), err.c_str());
        for (const auto& x : e) {
            QVERIFY2(x.path.rfind("$RECYCLE.BIN", 0) != 0, x.path.c_str());
            QVERIFY2(x.path.rfind("System Volume Information", 0) != 0, x.path.c_str());
        }
        bool hayElDelUsuario = false;
        for (const auto& x : e) {
            if (x.path == "mio/$RECYCLE.BIN/dato.txt") hayElDelUsuario = true;
        }
        QVERIFY2(hayElDelUsuario, "lo del usuario mas abajo si va");
    }

    // Quien lee el manifiesto de un socket tiene que saber cuántos bytes de nombres siguen a
    // cada línea. Contaba la ruta y el destino y no el dueño y el grupo: se quedaban en el
    // socket y la copia entre máquinas moría en cuanto el destino tenía algo dentro.
    void elLargoDeNombresCuentaDuenoYGrupo() {
        AR::Entry x;
        x.path = "sub/a.txt";
        x.kind = AR::EntryKind::File;
        x.owner = "linarese";
        x.group = "wheel";
        const std::string m = AR::serializeManifest({x});
        const std::string linea = m.substr(0, m.find('\n'));
        std::size_t largo = 0;
        QVERIFY(AR::manifestNamesLength(linea, largo));
        QCOMPARE(largo, x.path.size() + x.owner.size() + x.group.size());
        // Tras saltar esos bytes, lo siguiente es la línea de cierre y nada más.
        QCOMPARE(QString::fromStdString(m.substr(linea.size() + 1 + largo)), QStringLiteral("E\n"));
        // Una línea de un agente anterior, sin dueño ni grupo, sigue valiendo.
        QVERIFY(AR::manifestNamesLength("F 3 0 420 5 0", largo));
        QCOMPARE(largo, std::size_t(5));
        QVERIFY(!AR::manifestNamesLength("root 0 0", largo));
    }

    // Un fichero suelto viaja como un árbol de una entrada: su nombre, sin directorio.
    void unFicheroSueltoEsUnArbolDeUnaEntrada() {
        writeFile(src_ / "suelto.txt", "hola");
        AR::Entry x;
        std::string err;
        QVERIFY2(AR::walkSingleFile((src_ / "suelto.txt").string(), x, err), err.c_str());
        QCOMPARE(QString::fromStdString(x.path), QStringLiteral("suelto.txt"));
        QVERIFY(x.kind == AR::EntryKind::File);
        QCOMPARE(x.size, std::uint64_t(4));
        QVERIFY(x.mtime != 0);
        // Un directorio no es un fichero suelto: va por el recorrido de siempre.
        QVERIFY(!AR::walkSingleFile(src_.string(), x, err));
    }

    // La copia entre máquinas no lleva ACL ni xattr, y lo AVISA. Para eso el emisor tiene que
    // distinguir un fichero que los lleva de uno que no.
    void distingueLosFicherosConXattr() {
#ifdef __linux__
        writeFile(src_ / "limpio.txt", "a");
        writeFile(src_ / "con.txt", "b");
        QVERIFY(!AR::hasExtraMetadata((src_ / "limpio.txt").string()));
        if (::setxattr((src_ / "con.txt").c_str(), "user.prueba", "1", 1, 0) != 0) {
            QSKIP("este sistema de ficheros no admite xattr de usuario");
        }
        QVERIFY(AR::hasExtraMetadata((src_ / "con.txt").string()));
        // Y el recorrido lo anota en la entrada, que es de donde lo cuenta el emisor.
        std::vector<AR::Entry> e;
        std::string err;
        QVERIFY(AR::walk(src_.string(), e, err));
        for (const auto& x : e) {
            QCOMPARE(x.extraMetadata, x.path == "con.txt");
        }
#else
        QSKIP("solo en Linux");
#endif
    }

    void lasRutasVanSiempreConBarras() {
        namedirs();
        std::vector<AR::Entry> e;
        std::string err;
        QVERIFY(AR::walk(src_.string(), e, err));
        for (const auto& x : e) {
            QVERIFY2(x.path.find('\\') == std::string::npos,
                     "una ruta con barra invertida no casaria en el otro extremo");
        }
    }

    void elManifiestoSobreviveAlViajeDeIdaYVuelta() {
        namedirs();
        std::vector<AR::Entry> e;
        std::string err;
        QVERIFY(AR::walk(src_.string(), e, err));
        std::vector<AR::Entry> vuelta;
        QVERIFY2(AR::parseManifest(AR::serializeManifest(e), vuelta, err), err.c_str());
        QCOMPARE(vuelta.size(), e.size());
        for (size_t i = 0; i < e.size(); ++i) {
            QCOMPARE(QString::fromStdString(vuelta[i].path), QString::fromStdString(e[i].path));
            QCOMPARE(vuelta[i].size, e[i].size);
            QCOMPARE(vuelta[i].mtime, e[i].mtime);
        }
    }

    void copiarUnFicheroSuelto() {
        // `copyTree` solo aceptaba directorios, porque todo lo que la llamaba copiaba
        // arboles. `cp` copia ficheros sueltos, y envolverlo por fuera en un directorio
        // cambiaria donde aterriza: el destino aqui es la RUTA FINAL del fichero.
        namedirs();
        const std::filesystem::path f = src_ / "suelto.txt";
        {
            std::ofstream o(f);
            o << "contenido";
        }
        ::chmod(f.string().c_str(), 0640);
        const std::filesystem::path destino = dst_ / "copiado.txt";
        const Result r = copyTree(f.string(), destino.string(), Options{});
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(r.filesCopied, uint64_t(1));
        QVERIFY(std::filesystem::is_regular_file(destino));
        {
            std::ifstream i(destino);
            std::string leido;
            i >> leido;
            QCOMPARE(QString::fromStdString(leido), QString("contenido"));
        }
        // El modo viaja, como en el resto del motor.
        struct stat st {};
        QCOMPARE(::lstat(destino.string().c_str(), &st), 0);
        QCOMPARE(uint32_t(st.st_mode & 07777), uint32_t(0640));
    }

    void unFicheroNoSeMeteDentroDeUnDirectorioHomonimo() {
        // Si el destino ya existe y es un directorio, el fichero acabaria DENTRO y con el
        // mismo nombre: `destino/f.txt/f.txt`. Se niega, como `cp` de toda la vida.
        namedirs();
        const std::filesystem::path f = src_ / "choque.txt";
        {
            std::ofstream o(f);
            o << "x";
        }
        const std::filesystem::path destino = dst_ / "choque.txt";
        std::filesystem::create_directories(destino);
        const Result r = copyTree(f.string(), destino.string(), Options{});
        QVERIFY(!r.ok);
        QVERIFY2(r.error.find("DIRECTORIO") != std::string::npos, r.error.c_str());
    }

    void laPasadaEnSecoDeUnFicheroNoEscribe() {
        namedirs();
        const std::filesystem::path f = src_ / "seco.txt";
        {
            std::ofstream o(f);
            o << "x";
        }
        const std::filesystem::path destino = dst_ / "seco-copia.txt";
        Options opt;
        opt.dryRun = true;
        const Result r = copyTree(f.string(), destino.string(), opt);
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(r.filesCopied, uint64_t(1));
        QVERIFY2(!std::filesystem::exists(destino), "la pasada en seco escribio");
    }

    void elManifiestoLlevaDuenoYGrupo() {
        // De esto depende que una copia entre maquinas no deje todo perteneciendo a root.
        // Medido en vivo antes del arreglo: un /home copiado a otra maquina llegaba entero
        // de root, y eso lo deja inservible sin que nada avise.
        std::vector<AR::Entry> e(1);
        e[0].path = "f.txt";
        e[0].kind = AR::EntryKind::File;
        e[0].mode = 0640;
        e[0].uid = 1000;
        e[0].gid = 1000;
        e[0].owner = "linarese";
        e[0].group = "usuarios";
        std::vector<AR::Entry> vuelta;
        std::string err;
        QVERIFY2(AR::parseManifest(AR::serializeManifest(e), vuelta, err), err.c_str());
        QCOMPARE(vuelta.size(), size_t(1));
        QCOMPARE(vuelta[0].uid, e[0].uid);
        QCOMPARE(vuelta[0].gid, e[0].gid);
        QCOMPARE(QString::fromStdString(vuelta[0].owner), QString::fromStdString(e[0].owner));
        QCOMPARE(QString::fromStdString(vuelta[0].group), QString::fromStdString(e[0].group));
        QCOMPARE(vuelta[0].mode, e[0].mode);
    }

    void unManifiestoSinDuenoSigueValiendo() {
        // **La compatibilidad, que es lo que permite actualizar las maquinas de una en una.**
        // Un agente anterior a esto manda seis campos y ahi acaba la linea. Exigir los
        // nuevos convertiria «la otra maquina tiene un agente de la semana pasada» en «la
        // copia no funciona», que es mucho peor que copiar sin conservar el dueno.
        const std::string viejo = "f 7 1700000000 420 5 0\n" "f.txt" "E\n";
        std::vector<AR::Entry> vuelta;
        std::string err;
        QVERIFY2(AR::parseManifest(viejo, vuelta, err), err.c_str());
        QCOMPARE(vuelta.size(), size_t(1));
        QCOMPARE(QString::fromStdString(vuelta[0].path), QString("f.txt"));
        QCOMPARE(vuelta[0].mode, uint32_t(420));
        // Sin dueno: a cero y vacio, que es «no se sabe» y se traduce en no tocarlo.
        QCOMPARE(vuelta[0].uid, uint32_t(0));
        QVERIFY(vuelta[0].owner.empty());
    }

    void laCabeceraDeOperacionTambienLosLleva() {
        // Hace falta en las DOS: el manifiesto describe el arbol y la cabecera de operacion
        // es por donde viaja cada fichero. Con el dueno solo en el manifiesto, el receptor
        // —que no ve el manifiesto del origen— seguiria sin saberlo.
        AR::Operation o;
        o.action = AR::Action::Send;
        o.entry.path = "f.txt";
        o.entry.mode = 0755;
        o.entry.uid = 1000;
        o.entry.gid = 100;
        o.entry.owner = "linarese";
        o.entry.group = "users";
        AR::Operation vuelta;
        std::size_t lr = 0, ld = 0, ldu = 0, lgr = 0;
        std::string err;
        QVERIFY2(AR::parseHeader(AR::headerOf(o), vuelta, lr, ld, ldu, lgr, err), err.c_str());
        QCOMPARE(vuelta.entry.uid, o.entry.uid);
        QCOMPARE(vuelta.entry.gid, o.entry.gid);
        QCOMPARE(ldu, o.entry.owner.size());
        QCOMPARE(lgr, o.entry.group.size());
    }

    void unaCabeceraDeOperacionSinDuenoSigueValiendo() {
        AR::Operation vuelta;
        std::size_t lr = 0, ld = 0, ldu = 0, lgr = 0;
        std::string err;
        QVERIFY2(AR::parseHeader("F 420 1700000000 7 5 0", vuelta, lr, ld, ldu, lgr, err),
                 err.c_str());
        QCOMPARE(lr, size_t(5));
        QCOMPARE(ldu, size_t(0));
        QCOMPARE(lgr, size_t(0));
        QCOMPARE(vuelta.entry.uid, uint32_t(0));
    }

    void elRecorridoAnotaElModoDeLosDirectorios() {
        // `en.mode` se rellenaba en UN solo sitio —la rama del fichero corriente— asi que
        // los directorios viajaban con modo 0 y el receptor se saltaba el chmod: un
        // `drwx------` llegaba al otro lado como `drwxr-xr-x`.
        namedirs();
        const std::filesystem::path d = src_ / "cerrado";
        std::filesystem::create_directories(d);
        ::chmod(d.string().c_str(), 0700);
        std::vector<AR::Entry> e;
        std::string err;
        QVERIFY(AR::walk(src_.string(), e, err));
        bool visto = false;
        for (const AR::Entry& x : e) {
            if (x.path == "cerrado") {
                visto = true;
                QCOMPARE(x.mode, uint32_t(0700));
                QVERIFY(x.uid != 0 || !x.owner.empty());
            }
        }
        QVERIFY2(visto, "el directorio no salio en el recorrido");
    }

    void unNombreConSaltoDeLineaNoRompeElManifiesto() {
        // Las longitudes van explicitas justo por esto: un nombre puede llevar dentro un
        // salto de linea, y partir por lineas dejaria el manifiesto descolocado.
        std::vector<AR::Entry> e(1);
        e[0].path = "raro\ncon salto.txt";
        e[0].kind = AR::EntryKind::File;
        e[0].size = 7;
        std::vector<AR::Entry> vuelta;
        std::string err;
        QVERIFY2(AR::parseManifest(AR::serializeManifest(e), vuelta, err), err.c_str());
        QCOMPARE(vuelta.size(), size_t(1));
        QCOMPARE(QString::fromStdString(vuelta[0].path), QString::fromStdString(e[0].path));
    }

    void elPlanSaltaLoQueYaEstaIgual() {
        std::vector<AR::Entry> o(1), d(1);
        o[0].path = d[0].path = "a.txt";
        o[0].size = d[0].size = 10;
        o[0].mtime = d[0].mtime = 1000;
        const AR::Plan p = AR::makePlan(o, d, false);
        QCOMPARE(p.operations.size(), size_t(0));
        QCOMPARE(p.unchanged, uint64_t(1));
        QCOMPARE(p.bytes, uint64_t(0));
    }

    void unaFechaDistintaLoVuelveACopiar() {
        std::vector<AR::Entry> o(1), d(1);
        o[0].path = d[0].path = "a.txt";
        o[0].size = d[0].size = 10;
        o[0].mtime = 1001;
        d[0].mtime = 1000;
        const AR::Plan p = AR::makePlan(o, d, false);
        QCOMPARE(p.operations.size(), size_t(1));
        QVERIFY(p.operations[0].action == AR::Action::Send);
        QCOMPARE(p.bytes, uint64_t(10));
    }

    void sinBorradoNoSeBorraNada() {
        std::vector<AR::Entry> o;
        std::vector<AR::Entry> d(1);
        d[0].path = "sobra.txt";
        QCOMPARE(AR::makePlan(o, d, false).operations.size(), size_t(0));
    }

    void conBorradoSeVaDeDentroHaciaFuera() {
        // Un directorio se borra DESPUES de lo que tiene dentro. Al reves, el borrado
        // falla y el motivo no explica por que.
        std::vector<AR::Entry> o;
        std::vector<AR::Entry> d(2);
        d[0].path = "dir";
        d[0].kind = AR::EntryKind::Directory;
        d[1].path = "dir/dentro.txt";
        const AR::Plan p = AR::makePlan(o, d, true);
        QCOMPARE(p.operations.size(), size_t(2));
        QCOMPARE(QString::fromStdString(p.operations[0].entry.path),
                 QStringLiteral("dir/dentro.txt"));
        QCOMPARE(QString::fromStdString(p.operations[1].entry.path), QStringLiteral("dir"));
    }

    void unEnlaceQueCambiaDeDestinoSeRehace() {
        std::vector<AR::Entry> o(1), d(1);
        o[0].path = d[0].path = "l";
        o[0].kind = d[0].kind = AR::EntryKind::Symlink;
        o[0].target = "a.txt";
        d[0].target = "b.txt";
        const AR::Plan p = AR::makePlan(o, d, false);
        QCOMPARE(p.operations.size(), size_t(1));
        QVERIFY(p.operations[0].action == AR::Action::Symlink_);
    }

    void laCabeceraSobreviveAlViajeDeIdaYVuelta() {
        AR::Operation o;
        o.action = AR::Action::Send;
        o.entry.path = "sub/a.txt";
        o.entry.target = "";
        o.entry.mode = 0644;
        o.entry.mtime = 1700000000;
        o.entry.size = 12345;
        AR::Operation vuelta;
        size_t lr = 0, ld = 0;
        std::string err;
        std::size_t ldu = 0;
        std::size_t lgr = 0;
        QVERIFY2(AR::parseHeader(AR::headerOf(o), vuelta, lr, ld, ldu, lgr, err), err.c_str());
        QVERIFY(vuelta.action == AR::Action::Send);
        QCOMPARE(vuelta.entry.size, uint64_t(12345));
        QCOMPARE(vuelta.entry.mtime, int64_t(1700000000));
        QCOMPARE(lr, o.entry.path.size());
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
        uint32_t s = AR::rollingSum(p, v);
        for (size_t i = 0; i + v < datos.size(); ++i) {
            const unsigned char sale = p[i];
            const unsigned char entra = p[i + v];
            uint32_t a = s & 0xffff, b = (s >> 16) & 0xffff;
            a = (a - sale + entra) & 0xffff;
            b = (b - uint32_t(v) * sale + a) & 0xffff;
            s = a | (b << 16);
            QCOMPARE(s, AR::rollingSum(p + i + 1, v));
        }
    }

    void unFicheroIgualNoMandaNiUnByte() {
        // Con contenido variado, para que cada bloque tenga su propia firma. Con un solo
        // byte repetido todos los bloques son iguales y la prueba mediria otra cosa.
        std::string datos;
        for (size_t i = 0; i < 300000; ++i) datos.push_back(char('a' + (i * 13) % 26));
        writeFile(src_ / "g.bin", datos);
        std::vector<AR::Signature> fi;
        std::string err;
        const size_t tb = 8192;
        QVERIFY2(AR::signaturesOf((src_ / "g.bin").string(), tb, fi, err), err.c_str());
        std::vector<AR::Instruction> ins;
        uint64_t literales = 0;
        QVERIFY2(AR::delta((src_ / "g.bin").string(), fi, tb, ins, literales, err), err.c_str());
        QCOMPARE(literales, uint64_t(0));
        // Y en UNA sola instruccion: los bloques seguidos se juntan.
        QCOMPARE(ins.size(), size_t(1));
        QVERIFY(ins[0].kind == AR::InstructionKind::Send);
    }

    void unFicheroDeBloquesRepetidosNoExplotaEnInstrucciones() {
        // Un disco virtual medio vacio son megas del mismo byte: todos esos bloques tienen
        // la misma firma. Cogiendo siempre el primer candidato salia una instruccion por
        // bloque; prefiriendo el siguiente al ultimo copiado, sale una para todos.
        writeFile(src_ / "r.bin", std::string(300000, 'x'));
        std::vector<AR::Signature> fi;
        std::string err;
        const size_t tb = 8192;
        QVERIFY(AR::signaturesOf((src_ / "r.bin").string(), tb, fi, err));
        std::vector<AR::Instruction> ins;
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
        std::vector<AR::Signature> fi;
        std::string err;
        const size_t tb = 8192;
        QVERIFY(AR::signaturesOf((dst_ / "viejo.bin").string(), tb, fi, err));
        std::vector<AR::Instruction> ins;
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
        std::vector<AR::Signature> fi;
        std::string err;
        const size_t tb = 8192;
        QVERIFY(AR::signaturesOf((dst_ / "b.bin").string(), tb, fi, err));
        std::vector<AR::Instruction> ins;
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
        const size_t tb = AR::blockSize(nuevo.size());
        std::vector<AR::Signature> fi;
        std::string err;
        QVERIFY(AR::signaturesOf((dst_ / "v.bin").string(), tb, fi, err));
        std::vector<AR::Instruction> ins;
        uint64_t literales = 0;
        QVERIFY(AR::delta((src_ / "n.bin").string(), fi, tb, ins, literales, err));

        std::string rehecho;
        for (const auto& in : ins) {
            if (in.kind == AR::InstructionKind::Literal) {
                rehecho += in.data;
            } else {
                for (uint64_t k = 0; k < in.howMany; ++k) {
                    const size_t off = size_t(in.block + k) * tb;
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
        std::vector<AR::Signature> fi;
        std::string err;
        QVERIFY(AR::signaturesOf((dst_ / "big.bin").string(), tb, fi, err));
        std::vector<AR::Instruction> ins;
        uint64_t literales = 0;
        QVERIFY(AR::delta((src_ / "big.bin").string(), fi, tb, ins, literales, err));

        std::string rehecho;
        for (const auto& in : ins) {
            if (in.kind == AR::InstructionKind::Literal) {
                rehecho += in.data;
            } else {
                for (uint64_t k = 0; k < in.howMany; ++k) {
                    const size_t off = size_t(in.block + k) * tb;
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
        std::vector<AR::Signature> fi;
        std::string err;
        QVERIFY(AR::signaturesOf((src_ / "f.bin").string(), 8192, fi, err));
        std::vector<AR::Signature> vuelta;
        QVERIFY2(AR::parseSignatures(AR::serializeSignatures(fi), vuelta, err), err.c_str());
        QCOMPARE(vuelta.size(), fi.size());
        for (size_t i = 0; i < fi.size(); ++i) {
            QCOMPARE(vuelta[i].weak, fi[i].weak);
            QCOMPARE(memcmp(vuelta[i].strong, fi[i].strong, 16), 0);
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
