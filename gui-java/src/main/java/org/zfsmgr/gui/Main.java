package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import com.formdev.flatlaf.FlatLightLaf;
import java.awt.Graphics2D;
import java.awt.image.BufferedImage;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import javax.imageio.ImageIO;
import javax.swing.JOptionPane;
import javax.swing.JPasswordField;
import javax.swing.SwingUtilities;
import javax.swing.Timer;

/**
 * Starts the interface: the look, the zfsmgr-cli to use, the master password, the general
 * service and the window.
 *
 * <pre>
 * java -jar zfsmgr-gui.jar [--cli &lt;zfsmgr-cli&gt;] [--lang es|en] [--password-fd &lt;n&gt;]
 *                            [--config-dir &lt;dir&gt;]
 * </pre>
 *
 * <p>For tests without a screen (under Xvfb): ZFSMGR_GUI_CAPTURE=&lt;file.png&gt; saves the window
 * after ZFSMGR_GUI_CAPTURE_MS (8000) and ends; ZFSMGR_GUI_CAPTURE_SIZE=&lt;w&gt;x&lt;h&gt;;
 * ZFSMGR_GUI_PAGE=&lt;n&gt; opens on page n (0 Status … 4 Administration);
 * ZFSMGR_GUI_OPEN=newcopy opens the new-copy wizard, and the capture is the dialog on top.
 *
 * <p>ZFSMGR_GUI_DEBUG=1: timings, every request's time, and a watchdog of the Swing thread, on
 * stderr; the CLI services' stderr always goes to zfsmgr-gui-cli.log in the temporary directory.
 */
public final class Main {
    private Main() {}

    public static void main(String[] args) throws Exception {
        Timing.mark("main");
        String cli = null;
        Integer passwordFd = null;
        for (int i = 0; i < args.length; i++) {
            switch (args[i]) {
                case "--cli" -> cli = args[++i];
                case "--lang" -> Text.setLang(args[++i]);
                case "--password-fd" -> passwordFd = Integer.parseInt(args[++i]);
                case "--config-dir" -> CliService.setCommonArgs(List.of("--config-dir", args[++i]));
                default -> {
                    System.err.println("unknown option: " + args[i]);
                    System.exit(2);
                }
            }
        }
        FlatLightLaf.setup();
        Timing.mark("look_and_feel");
        final String cliPath = cli != null ? cli : findCli();
        if (cliPath == null) {
            JOptionPane.showMessageDialog(null, L("zfsmgr-cli was not found: use --cli <path>.",
                    "No se encuentra zfsmgr-cli: use --cli <ruta>."));
            System.exit(1);
        }
        char[] fromFd = null;
        if (passwordFd != null) {
            // 0 is the standard input, everywhere. Any other through /dev/fd, which FreeBSD only
            // has beyond 2 with fdescfs mounted.
            String first;
            if (passwordFd == 0) {
                first = new java.io.BufferedReader(new java.io.InputStreamReader(System.in,
                        StandardCharsets.UTF_8)).readLine();
            } else {
                List<String> lines = Files.readAllLines(Path.of("/dev/fd/" + passwordFd), StandardCharsets.UTF_8);
                first = lines.isEmpty() ? null : lines.get(0);
            }
            fromFd = first == null ? new char[0] : first.toCharArray();
        }
        // Empty unless --password-fd: the service's answer says whether a password is needed
        // (or a touch of a key, or a first master password). See Starter.
        final char[] given = fromFd != null ? fromFd : new char[0];
        SwingUtilities.invokeLater(() -> new Starter(cliPath).attempt(given));
    }

    /** zfsmgr-cli: in the PATH, or where each installer puts it. */
    static String findCli() {
        boolean windows = System.getProperty("os.name").toLowerCase().contains("win");
        String name = windows ? "zfsmgr-cli.exe" : "zfsmgr-cli";
        String path = System.getenv("PATH");
        if (path != null) {
            for (String dir : path.split(File.pathSeparator)) {
                File f = new File(dir, name);
                if (f.canExecute()) {
                    return f.getPath();
                }
            }
        }
        String[] known = windows ? new String[] {System.getenv("ProgramFiles") + "\\ZFSMgr\\bin\\" + name}
                                 : new String[] {"/usr/local/bin/" + name, "/usr/bin/" + name};
        for (String k : known) {
            if (new File(k).canExecute()) {
                return k;
            }
        }
        return null;
    }

    static void showWindow(MainWindow w) {
        String size = System.getenv("ZFSMGR_GUI_CAPTURE_SIZE");
        if (size != null && size.matches("\\d+x\\d+")) {
            String[] wh = size.split("x");
            w.setSize(Integer.parseInt(wh[0]), Integer.parseInt(wh[1]));
        }
        final boolean[] firstData = {false};
        w.store().onChanged(() -> {
            if (!firstData[0] && w.store().machines().stream().anyMatch(m -> m.loaded)) {
                firstData[0] = true;
                Timing.mark("first_machine");
            }
        });
        w.store().onFirstComplete(() -> {
            Timing.mark("all_machines");
            Timing.memory("after_all_machines");
        });
        w.setVisible(true);
        Timing.mark("window_shown");
        if (!Timing.on()) {
            w.store().refresh();
            startCapture(w);
            return;
        }
        System.err.println("[log] stderr of the CLI services: " + Timing.cliLog());
        // Every 5 s, from a thread of its own: whether the Swing thread still answers —if it is
        // stuck, where— and what each service is still waiting for. The spike's diagnosis of
        // a hang (a1278: «Updating…» forever after `ls`).
        Thread watch = new Thread(() -> {
            final long[] beat = {0};
            while (true) {
                final long posted = System.currentTimeMillis();
                SwingUtilities.invokeLater(() -> beat[0] = System.currentTimeMillis());
                try {
                    Thread.sleep(5000);
                } catch (InterruptedException ex) {
                    return;
                }
                // Stuck only if what was posted 5 s ago has not run yet.
                long late = beat[0] >= posted ? 0 : System.currentTimeMillis() - posted;
                if (late > 4000) {
                    System.err.println("[watch] the Swing thread has not answered for " + late + " ms:");
                    Thread.getAllStackTraces().forEach((t, st) -> {
                        if (t.getName().startsWith("AWT-EventQueue") || t.getName().startsWith("zfsmgr")) {
                            System.err.println("  thread " + t.getName() + " " + t.getState());
                            for (int i = 0; i < Math.min(25, st.length); i++) {
                                System.err.println("    at " + st[i]);
                            }
                        }
                    });
                } else {
                    w.waitingReport();
                }
            }
        }, "zfsmgr-watch");
        watch.setDaemon(true);
        watch.start();
        Timing.memory("window_shown");
        w.store().refresh();
        startCapture(w);
    }

    /** ZFSMGR_GUI_CAPTURE: the window saved as an image after a while, and the end. */
    private static void startCapture(MainWindow w) {
        String capture = System.getenv("ZFSMGR_GUI_CAPTURE");
        if (capture != null && !capture.isEmpty()) {
            int ms = 8000;
            try {
                ms = Integer.parseInt(System.getenv().getOrDefault("ZFSMGR_GUI_CAPTURE_MS", "8000"));
            } catch (NumberFormatException ignored) {
                // default
            }
            // ZFSMGR_GUI_OPEN=newcopy: the wizard opened over the window, a while after the
            // machines start answering; the capture is then the dialog.
            if ("newcopy".equals(System.getenv("ZFSMGR_GUI_OPEN"))) {
                Timer open = new Timer(Math.max(1000, ms / 2), e -> w.newCopy("", ""));
                open.setRepeats(false);
                open.start();
            }
            Timer t = new Timer(ms, e -> {
                // The dialog on top, if there is one; otherwise the window.
                java.awt.Window shot = w;
                for (java.awt.Window x : java.awt.Window.getWindows()) {
                    if (x != w && x.isShowing() && x instanceof javax.swing.RootPaneContainer) {
                        shot = x;
                    }
                }
                javax.swing.JRootPane root = ((javax.swing.RootPaneContainer) shot).getRootPane();
                // At the screen's scale, as the person sees it (HiDPI: 2x the logical size).
                double scale = w.getGraphicsConfiguration().getDefaultTransform().getScaleX();
                BufferedImage img = new BufferedImage((int) Math.round(shot.getWidth() * scale),
                        (int) Math.round(shot.getHeight() * scale), BufferedImage.TYPE_INT_RGB);
                Graphics2D g = img.createGraphics();
                g.scale(scale, scale);
                root.paintAll(g);
                g.dispose();
                try {
                    ImageIO.write(img, "png", new File(capture));
                } catch (Exception ex) {
                    System.err.println("capture: " + ex.getMessage());
                }
                w.close();
            });
            t.setRepeats(false);
            t.start();
        }
    }
}
