#include "copytree.h"

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

#ifndef _WIN32
    // La diferencia principal con robocopy, y la razón de implementarlo en vez de
    // llamarlo: dos rutas al mismo fichero deben seguir siéndolo en el destino, no
    // convertirse en dos copias que ocupan el doble.
    void preservesHardLinks() {
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
    }

    void preservesSymlinksAsLinks() {
        writeFile(src_ / "real.txt", "contenido");
        fs::create_symlink("real.txt", src_ / "enlace.txt");

        const Result r = copyTree(src_.string(), dst_.string(), {});
        QVERIFY2(r.ok, r.error.c_str());
        QVERIFY2(fs::is_symlink(fs::symlink_status(dst_ / "enlace.txt")),
                 "debe copiarse como enlace, no como el fichero al que apunta");
        QCOMPARE(fs::read_symlink(dst_ / "enlace.txt").string(), std::string("real.txt"));
    }

    void preservesPermissions() {
        writeFile(src_ / "script.sh", "#!/bin/sh\n");
        QCOMPARE(::chmod((src_ / "script.sh").c_str(), 0755), 0);

        const Result r = copyTree(src_.string(), dst_.string(), {});
        QVERIFY2(r.ok, r.error.c_str());
        struct stat st {};
        QCOMPARE(::stat((dst_ / "script.sh").c_str(), &st), 0);
        QCOMPARE(st.st_mode & 07777, static_cast<mode_t>(0755));
    }

    // Un fichero mayormente vacío no debe llegar ocupando su tamaño aparente: una imagen
    // de disco de 100 GB con 2 GB usados arruinaría el destino.
    void keepsSparseFilesSparse() {
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
    }
#endif

#if defined(__linux__)
    // Los atributos extendidos, y con ellos las ACL POSIX de Linux, que viven en
    // `system.posix_acl_access`.
    //
    // Este caso no existía y por eso pasó desapercibido: al sustituir rsync se perdieron
    // las ACL, y la comparación que hice entonces no las incluía. `rsync -A` conservaba
    // el permiso y esta copia lo dejaba fuera.
    void preservesExtendedAttributes() {
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
    }
#endif

    // Un fichero ENTERAMENTE disperso: todo huecos, sin un solo byte de datos.
    //
    // Se añadió tras comparar la copia nativa con la de rsync sobre el mismo árbol: el
    // tamaño aparente coincidía, pero el nativo ocupaba 4 KiB donde rsync dejaba 0,
    // porque fijaba el tamaño escribiendo un byte al final en vez de con truncate.
    void fullySparseFileAllocatesNothing() {
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
};

QTEST_MAIN(CopyTreeTest)
#include "copytree_test.moc"
