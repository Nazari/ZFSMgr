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
 * after ZFSMGR_GUI_CAPTURE_MS (8000) and ends; ZFSMGR_GUI_CAPTURE_SIZE=&lt;w&gt;x&lt;h&gt;.
 *
 * <p>Not in the spike: the session lock (it will be taken by {@code --serve} for the interface),
 * FIDO2 keys, the first run, and every page but «Estado».
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
        final char[] given = fromFd;
        SwingUtilities.invokeLater(() -> attempt(cliPath, given));
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

    private static char[] askPassword(String why) {
        JPasswordField field = new JPasswordField(24);
        Object[] msg = why == null ? new Object[] {L("Master password", "Contraseña maestra"), field}
                                   : new Object[] {why, L("Master password", "Contraseña maestra"), field};
        Timing.mark("password_dialog");
        int r = JOptionPane.showConfirmDialog(null, msg, "ZFSMgr", JOptionPane.OK_CANCEL_OPTION,
                JOptionPane.PLAIN_MESSAGE);
        if (r != JOptionPane.OK_OPTION) {
            System.exit(0);
        }
        // From here the times are the program's again, not the person typing.
        Timing.mark("password_given");
        return field.getPassword();
    }

    private static void attempt(String cli, char[] given) {
        char[] master = given != null ? given : askPassword(null);
        if (master.length == 0) {
            // With FIDO2 keys an empty line waits for a touch, which the spike does not handle.
            attempt(cli, askPassword(L("The spike needs the master password.",
                    "La prueba necesita la contraseña maestra.")));
            return;
        }
        final MainWindow[] window = new MainWindow[1];
        // Something on screen while the service starts: on Windows it can take a few seconds,
        // and with no window at all it looked as if the program had closed.
        javax.swing.JWindow opening = new javax.swing.JWindow();
        javax.swing.JLabel openingText = new javax.swing.JLabel(L("Opening ZFSMgr…", "Abriendo ZFSMgr…"));
        openingText.setBorder(javax.swing.BorderFactory.createCompoundBorder(
                javax.swing.BorderFactory.createLineBorder(java.awt.Color.GRAY),
                javax.swing.BorderFactory.createEmptyBorder(16, 28, 16, 28)));
        opening.getContentPane().add(openingText);
        opening.pack();
        opening.setLocationRelativeTo(null);
        opening.setVisible(true);
        CliService.Listener listener = new CliService.Listener() {
            @Override
            public void ready(java.util.Map<String, Object> ev) {}

            @Override
            public void failed(String reason, boolean wrongPassword) {
                opening.dispose();
                if (window[0] == null) {
                    if (wrongPassword) {
                        SwingUtilities.invokeLater(() -> attempt(cli, askPassword(
                                L("That is not the master password.", "Esa no es la contraseña maestra."))));
                    } else {
                        JOptionPane.showMessageDialog(null, reason);
                        System.exit(1);
                    }
                }
            }

            @Override
            public void confirm(CliService service, CliService.Question q) {
                int r = JOptionPane.showConfirmDialog(window[0], q.text(), q.title(), JOptionPane.YES_NO_OPTION);
                service.answerConfirm(q.id(), r == JOptionPane.YES_OPTION, null);
            }

            @Override
            public void secret(CliService service, int id, String text) {
                JPasswordField f = new JPasswordField(24);
                int r = JOptionPane.showConfirmDialog(window[0], new Object[] {text, f}, "ZFSMgr",
                        JOptionPane.OK_CANCEL_OPTION, JOptionPane.PLAIN_MESSAGE);
                service.answerSecret(id, r == JOptionPane.OK_OPTION ? f.getPassword() : null);
            }

            @Override
            public void closeAsked(String host, String user) {
                JOptionPane.showMessageDialog(window[0], L("%1 on %2 asked this session to close.",
                        "%1 en %2 ha pedido que esta sesión se cierre.", user, host));
                if (window[0] != null) {
                    window[0].close();
                }
            }
        };
        CliService general = new CliService(cli, Text.lang(), SwingUtilities::invokeLater, new CliService.Listener() {
            @Override
            public void ready(java.util.Map<String, Object> ev) {
                Timing.mark("service_ready");
                CliService self = generalRef[0];
                MainWindow w = new MainWindow(cli, self, listener);
                window[0] = w;
                showWindow(w);
                opening.dispose();
            }

            @Override
            public void failed(String reason, boolean wrongPassword) {
                listener.failed(reason, wrongPassword);
            }

            @Override
            public void confirm(CliService service, CliService.Question q) {
                listener.confirm(service, q);
            }

            @Override
            public void secret(CliService service, int id, String text) {
                listener.secret(service, id, text);
            }

            @Override
            public void closeAsked(String host, String user) {
                listener.closeAsked(host, user);
            }
        });
        generalRef[0] = general;
        general.setPresence(true);
        general.start(master, false);
    }

    private static final CliService[] generalRef = new CliService[1];

    private static void showWindow(MainWindow w) {
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
        System.err.println("[log] stderr of the CLI services: " + Timing.cliLog());
        // Every 10 s, what each service is still waiting for: the spike's diagnosis of a hang.
        new Timer(10_000, e -> w.waitingReport()).start();
        Timing.memory("window_shown");
        w.store().refresh();
        String capture = System.getenv("ZFSMGR_GUI_CAPTURE");
        if (capture != null && !capture.isEmpty()) {
            int ms = 8000;
            try {
                ms = Integer.parseInt(System.getenv().getOrDefault("ZFSMGR_GUI_CAPTURE_MS", "8000"));
            } catch (NumberFormatException ignored) {
                // default
            }
            Timer t = new Timer(ms, e -> {
                // At the screen's scale, as the person sees it (HiDPI: 2x the logical size).
                double scale = w.getGraphicsConfiguration().getDefaultTransform().getScaleX();
                BufferedImage img = new BufferedImage((int) Math.round(w.getWidth() * scale),
                        (int) Math.round(w.getHeight() * scale), BufferedImage.TYPE_INT_RGB);
                Graphics2D g = img.createGraphics();
                g.scale(scale, scale);
                w.getRootPane().paintAll(g);
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
