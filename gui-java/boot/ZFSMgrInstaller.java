import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.Method;
import java.net.URL;
import java.net.URLClassLoader;
import java.util.Enumeration;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import javax.swing.JOptionPane;

/**
 * The first class of the installer .jar, the one `java -jar zfsmgr-<v>.jar` runs. Compiled for
 * Java 8 on purpose: on an older Java it can still say that ZFSMgr needs 21, instead of the
 * JVM's «UnsupportedClassVersionError».
 *
 * The .jar is also the package: `zfsmgr-<v>/` inside it is the tree the shell installers use
 * (unzip it to install without Java). This class unpacks that tree to a temporary directory,
 * gives back the execution bits a zip entry does not carry for Java, and starts the installer
 * window that lives in the package's own interface (gui/zfsmgr-gui.jar), with that directory.
 */
public final class ZFSMgrInstaller {
    private ZFSMgrInstaller() {}

    public static void main(String[] args) throws Exception {
        boolean es = Locale.getDefault().getLanguage().equals("es");
        int major = major(System.getProperty("java.specification.version", "0"));
        if (major < 21) {
            String msg = es
                ? "El instalador de ZFSMgr necesita Java 21 o posterior, y este es el " + major + ".\n"
                  + "Instale uno (por ejemplo Temurin 21, https://adoptium.net) y vuelva a abrirlo.\n\n"
                  + "Sin Java: descomprima este .jar (es un zip) y ejecute el instalador de su sistema\n"
                  + "que hay dentro de zfsmgr-<versión>/, por ejemplo «sh install-linux.sh»."
                : "The ZFSMgr installer needs Java 21 or later, and this one is " + major + ".\n"
                  + "Install one (for instance Temurin 21, https://adoptium.net) and open it again.\n\n"
                  + "Without Java: unzip this .jar (it is a zip) and run your system's installer\n"
                  + "inside zfsmgr-<version>/, for instance «sh install-linux.sh».";
            say(msg);
            System.exit(1);
        }
        File jar = new File(ZFSMgrInstaller.class.getProtectionDomain().getCodeSource().getLocation().toURI());
        File tmp = File.createTempFile("zfsmgr-installer", "");
        if (!tmp.delete() || !tmp.mkdirs()) {
            say("cannot create " + tmp);
            System.exit(1);
        }
        final File base = tmp;
        Runtime.getRuntime().addShutdownHook(new Thread() {
            @Override
            public void run() {
                remove(base);
            }
        });
        String top = null;
        ZipFile zip = new ZipFile(jar);
        try {
            for (Enumeration<? extends ZipEntry> en = zip.entries(); en.hasMoreElements(); ) {
                String n = en.nextElement().getName();
                if (n.endsWith("/MANIFIESTO.sha256") && n.indexOf('/') == n.lastIndexOf('/')) {
                    top = n.substring(0, n.indexOf('/') + 1);
                }
            }
            if (top == null) {
                say(es ? "Este .jar no lleva dentro ningún paquete de ZFSMgr." : "This .jar carries no ZFSMgr package.");
                System.exit(1);
            }
            for (Enumeration<? extends ZipEntry> en = zip.entries(); en.hasMoreElements(); ) {
                ZipEntry e = en.nextElement();
                String n = e.getName();
                if (!n.startsWith(top) || n.contains("..")) {
                    continue;
                }
                File out = new File(base, n);
                if (e.isDirectory()) {
                    out.mkdirs();
                    continue;
                }
                out.getParentFile().mkdirs();
                InputStream in = zip.getInputStream(e);
                OutputStream o = new FileOutputStream(out);
                try {
                    byte[] buf = new byte[1 << 16];
                    for (int k; (k = in.read(buf)) > 0; ) {
                        o.write(buf, 0, k);
                    }
                } finally {
                    o.close();
                    in.close();
                }
                String rel = n.substring(top.length());
                if (rel.endsWith(".sh") || rel.startsWith("cli/") || rel.startsWith("agents/")) {
                    out.setExecutable(true, false);
                }
            }
        } finally {
            zip.close();
        }
        File pkg = new File(base, top);
        File gui = new File(pkg, "gui/zfsmgr-gui.jar");
        URLClassLoader loader = new URLClassLoader(new URL[] {gui.toURI().toURL()}, ZFSMgrInstaller.class.getClassLoader());
        Class<?> c = Class.forName("org.zfsmgr.gui.Installer", true, loader);
        Thread.currentThread().setContextClassLoader(loader);
        Method m = c.getMethod("main", String[].class);
        String[] pass = new String[args.length + 1];
        pass[0] = pkg.getPath();
        System.arraycopy(args, 0, pass, 1, args.length);
        m.invoke(null, (Object) pass);
    }

    private static int major(String v) {
        try {
            if (v.startsWith("1.")) {
                return Integer.parseInt(v.substring(2));
            }
            int dot = v.indexOf('.');
            return Integer.parseInt(dot < 0 ? v : v.substring(0, dot));
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    private static void say(String msg) {
        System.err.println(msg);
        try {
            JOptionPane.showMessageDialog(null, msg, "ZFSMgr", JOptionPane.ERROR_MESSAGE);
        } catch (Throwable headless) {
            // no screen: stderr was enough
        }
    }

    private static void remove(File f) {
        File[] kids = f.listFiles();
        if (kids != null) {
            for (File k : kids) {
                remove(k);
            }
        }
        f.delete();
    }
}
