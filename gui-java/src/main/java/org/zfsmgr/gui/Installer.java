package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import com.formdev.flatlaf.FlatLightLaf;
import java.awt.BorderLayout;
import java.awt.CardLayout;
import java.awt.Font;
import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Base64;
import java.util.List;
import java.util.Map;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;
import javax.swing.BorderFactory;
import javax.swing.Box;
import javax.swing.BoxLayout;
import javax.swing.JButton;
import javax.swing.JCheckBox;
import javax.swing.JFrame;
import javax.swing.JLabel;
import javax.swing.JOptionPane;
import javax.swing.JPanel;
import javax.swing.JPasswordField;
import javax.swing.JScrollPane;
import javax.swing.JTable;
import javax.swing.JTextArea;
import javax.swing.SwingUtilities;
import javax.swing.table.DefaultTableModel;

/**
 * The installer: the window that {@code java -jar zfsmgr-<v>.jar} opens. The .jar carries the
 * whole package (zfsmgr-&lt;v&gt;/, the same tree the shell installers use); a small class at its
 * root unpacks it and starts this one with that directory.
 *
 * <ol>
 *   <li><b>This computer</b>: runs the package's own installer for this system, with the rights
 *       it needs (sudo, the macOS administrator dialog, or Windows' UAC).
 *   <li><b>Your network</b>: opens the installed zfsmgr-cli (the master password, or creating
 *       it), shows every machine with its daemon, CLI and GUI against this package, adds
 *       machines, creates the network or joins one, and installs or updates the package on the
 *       machines chosen ({@code install-package}).
 * </ol>
 *
 * Every rule stays in the CLI and in the package's installers: this window only runs them.
 *
 * <p>For tests without a screen: {@code --password-fd 0} reads the master password from the
 * standard input; ZFSMGR_INSTALLER_STEP=network opens on the network; ZFSMGR_GUI_CAPTURE and
 * ZFSMGR_GUI_CAPTURE_MS save the window as an image and end, as in the interface.
 */
public final class Installer extends JFrame {
    private final Path pkg;
    private final String key;
    private final String pkgVersion;
    private final CardLayout cards = new CardLayout();
    private char[] given = new char[0];
    private final JPanel body = new JPanel(cards);

    // Step 1.
    private final JLabel hereState = new JLabel(" ");
    private final JCheckBox hereGui = new JCheckBox(L("The graphical interface", "La interfaz gráfica"), true);
    private final JCheckBox hereDaemon = new JCheckBox(
            L("The daemon, if this computer has OpenZFS", "El daemon, si este ordenador tiene OpenZFS"), true);
    private final JTextArea hereLog = log();
    private final JButton hereInstall = new JButton(L("Install on this computer", "Instalar en este ordenador"));

    // Step 2.
    private CliService svc;
    private StateStore store;
    private final JLabel netState = new JLabel(" ");
    private final JButton seed = new JButton(L("Create my network with these machines", "Crear mi red con estas máquinas"));
    private final JButton join = new JButton(L("Join an existing network…", "Unirme a una red que ya existe…"));
    private final JButton add = new JButton(L("Add a machine…", "Añadir una máquina…"));
    private final JButton installMarked = new JButton(L("Install on the ones marked", "Instalar en las marcadas"));
    private final JCheckBox netGui = new JCheckBox(L("With the graphical interface", "Con la interfaz gráfica"), true);
    private final JCheckBox netDaemon = new JCheckBox(L("With the daemon (where OpenZFS is)", "Con el daemon (donde haya OpenZFS)"), true);
    private final JTextArea netLog = log();
    private final List<String> rowIds = new ArrayList<>();
    private final DefaultTableModel machines = new DefaultTableModel(new String[] {"",
        L("Machine", "Máquina"), L("System", "Sistema"), L("Daemon", "Daemon"), "zfsmgr-cli",
        L("Interface", "Interfaz"), L("State", "Estado")}, 0) {
        @Override
        public Class<?> getColumnClass(int c) {
            return c == 0 ? Boolean.class : String.class;
        }

        @Override
        public boolean isCellEditable(int r, int c) {
            return c == 0 && getValueAt(r, 0) != null;
        }
    };

    public static void main(String[] args) throws Exception {
        String lang = java.util.prefs.Preferences.userNodeForPackage(MainWindow.class).get("lang", "");
        if (!lang.isEmpty()) {
            Text.setLang(lang);
        }
        if (args.length < 1) {
            System.err.println("usage: Installer <package directory>");
            System.exit(2);
        }
        FlatLightLaf.setup();
        Path pkg = Path.of(args[0]);
        char[] password = null;
        if (args.length >= 3 && args[1].equals("--password-fd") && args[2].equals("0")) {
            String first = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8)).readLine();
            password = first == null ? new char[0] : first.toCharArray();
        }
        final char[] pw = password;
        SwingUtilities.invokeLater(() -> {
            Installer w = new Installer(pkg);
            if (pw != null) {
                w.given = pw;
            }
            w.setVisible(true);
            if ("network".equals(System.getenv("ZFSMGR_INSTALLER_STEP"))) {
                w.toNetwork();
            }
            String capture = System.getenv("ZFSMGR_GUI_CAPTURE");
            if (capture != null && !capture.isEmpty()) {
                int ms = Integer.parseInt(System.getenv().getOrDefault("ZFSMGR_GUI_CAPTURE_MS", "8000"));
                javax.swing.Timer t = new javax.swing.Timer(ms, e -> {
                    java.awt.image.BufferedImage img = new java.awt.image.BufferedImage(w.getWidth(), w.getHeight(),
                            java.awt.image.BufferedImage.TYPE_INT_RGB);
                    java.awt.Graphics2D g = img.createGraphics();
                    w.getRootPane().paintAll(g);
                    g.dispose();
                    try {
                        javax.imageio.ImageIO.write(img, "png", new File(capture));
                    } catch (IOException ex) {
                        System.err.println("capture: " + ex.getMessage());
                    }
                    w.dispose();
                });
                t.setRepeats(false);
                t.start();
            }
        });
    }

    Installer(Path pkg) {
        super("ZFSMgr");
        this.pkg = pkg;
        this.key = platformKey();
        this.pkgVersion = fullVersion(run(List.of(pkgCli().toString(), "version"), 20));
        setDefaultCloseOperation(DISPOSE_ON_CLOSE);
        addWindowListener(new java.awt.event.WindowAdapter() {
            @Override
            public void windowClosed(java.awt.event.WindowEvent e) {
                if (svc != null) {
                    svc.stop(2000);
                }
                System.exit(0);
            }
        });
        body.add(stepHere(), "here");
        body.add(stepNetwork(), "network");
        setContentPane(body);
        setSize(900, 640);
        setLocationRelativeTo(null);
        refreshHere();
    }

    // --- Step 1: this computer -------------------------------------------------------------

    private JPanel stepHere() {
        JPanel p = page(L("1. This computer", "1. Este ordenador"),
                L("ZFSMgr %1 for %2. It installs the command line client, the interface and —where OpenZFS is— the daemon, with this system's installer.",
                  "ZFSMgr %1 para %2. Instala el cliente de línea de órdenes, la interfaz y —donde haya OpenZFS— el daemon, con el instalador de este sistema.",
                  pkgVersion.isEmpty() ? pkg.getFileName() : pkgVersion, key));
        JPanel opts = new JPanel();
        opts.setLayout(new BoxLayout(opts, BoxLayout.Y_AXIS));
        opts.add(hereState);
        opts.add(Box.createVerticalStrut(6));
        opts.add(hereGui);
        opts.add(hereDaemon);
        JPanel center = new JPanel(new BorderLayout(0, 8));
        center.add(opts, BorderLayout.NORTH);
        center.add(new JScrollPane(hereLog), BorderLayout.CENTER);
        p.add(center, BorderLayout.CENTER);
        JButton next = new JButton(L("Next: my network →", "Siguiente: mi red →"));
        next.addActionListener(e -> toNetwork());
        hereInstall.addActionListener(e -> installHere());
        p.add(buttons(hereInstall, next), BorderLayout.SOUTH);
        return p;
    }

    private void refreshHere() {
        String cli = Main.findCli();
        String installed = cli == null ? "" : fullVersion(run(List.of(cli, "version"), 20));
        if (installed.isEmpty()) {
            hereState.setText(L("ZFSMgr is not installed on this computer.", "ZFSMgr no está instalado en este ordenador."));
        } else if (installed.equals(pkgVersion)) {
            hereState.setText(L("This computer already has this version (%1).", "Este ordenador ya tiene esta versión (%1).", installed));
        } else {
            hereState.setText(L("This computer has %1; this package is %2.", "Este ordenador tiene la %1; este paquete es la %2.",
                    installed, pkgVersion));
        }
    }

    private void installHere() {
        List<String> opts = new ArrayList<>();
        boolean windows = key.startsWith("windows");
        if (windows) {
            opts.add("-Quiet");
            opts.add(hereDaemon.isSelected() ? "-WithDaemon" : "-NoDaemon");
            if (!hereGui.isSelected()) {
                opts.add("-CliOnly");
            }
        } else {
            opts.add(hereDaemon.isSelected() ? "--with-daemon" : "--no-daemon");
            if (!hereGui.isSelected()) {
                opts.add("--cli-only");
            }
            if (key.startsWith("freebsd")) {
                opts.add("--accept-experimental");
            }
        }
        hereLog.setText("");
        hereInstall.setEnabled(false);
        Consumer<Integer> done = rc -> {
            hereInstall.setEnabled(true);
            say(hereLog, rc == 0 ? L("— Installed.", "— Instalado.") : L("— It failed (code %1): see above.",
                    "— Falló (código %1): mira arriba.", rc));
            refreshHere();
        };
        if (windows) {
            background(() -> installWindows(opts), done);
        } else if (key.startsWith("macos")) {
            background(() -> installMac(opts), done);
        } else {
            installUnix(opts, done);
        }
    }

    /** Linux and FreeBSD: as root, by sudo without a password, or by sudo with the one asked here. */
    private void installUnix(List<String> opts, Consumer<Integer> done) {
        String script = pkg.resolve("install-" + (key.startsWith("freebsd") ? "freebsd" : "linux") + ".sh").toString();
        List<String> cmd = new ArrayList<>();
        char[] password = null;
        if (run(List.of("id", "-u"), 5).trim().equals("0")) {
            cmd.addAll(List.of("sh", script));
        } else if (exit(List.of("sudo", "-n", "true")) == 0) {
            cmd.addAll(List.of("sudo", "-n", "sh", script));
        } else {
            JPasswordField f = new JPasswordField(20);
            int r = JOptionPane.showConfirmDialog(this, new Object[] {
                L("Installing needs administrator rights. Your password for sudo:",
                  "Instalar necesita permisos de administrador. Tu contraseña de sudo:"), f},
                    "ZFSMgr", JOptionPane.OK_CANCEL_OPTION, JOptionPane.QUESTION_MESSAGE);
            if (r != JOptionPane.OK_OPTION) {
                hereInstall.setEnabled(true);
                return;
            }
            password = f.getPassword();
            // -k: the password is ALWAYS read, so it never reaches the installer's input.
            cmd.addAll(List.of("sudo", "-S", "-k", "-p", "", "sh", script));
        }
        cmd.addAll(opts);
        final char[] pw = password;
        background(() -> stream(cmd, pw, hereLog), done);
    }

    /** macOS: the system's own administrator dialog; the output through a file. */
    private int installMac(List<String> opts) {
        try {
            Path logFile = Files.createTempFile("zfsmgr-install", ".log");
            String line = "sh " + sq(pkg.resolve("install-macos.sh").toString()) + " " + String.join(" ", opts)
                    + " > " + sq(logFile.toString()) + " 2>&1";
            String apple = "do shell script \"" + line.replace("\\", "\\\\").replace("\"", "\\\"")
                    + "\" with administrator privileges";
            int rc = stream(List.of("osascript", "-e", apple), null, hereLog);
            say(hereLog, Files.readString(logFile));
            Files.deleteIfExists(logFile);
            return rc;
        } catch (IOException e) {
            say(hereLog, e.getMessage());
            return 1;
        }
    }

    /** Windows: an elevated PowerShell (the UAC question), the output through a file. */
    private int installWindows(List<String> opts) {
        try {
            Path logFile = Files.createTempFile("zfsmgr-install", ".log");
            Path wrapper = Files.createTempFile("zfsmgr-install", ".ps1");
            Files.writeString(wrapper, "& " + psq(pkg.resolve("install-windows.ps1").toString()) + " "
                    + String.join(" ", opts) + " *> " + psq(logFile.toString()) + "; exit $LASTEXITCODE\r\n",
                    StandardCharsets.UTF_8);
            String outer = "$p = Start-Process -FilePath powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList "
                    + psq("-NoProfile -ExecutionPolicy Bypass -File \"" + wrapper + "\"") + "; exit $p.ExitCode";
            int rc = stream(List.of("powershell.exe", "-NoProfile", "-EncodedCommand", encoded(outer)), null, hereLog);
            byte[] b = Files.readAllBytes(logFile);
            // `*>` of Windows PowerShell 5.1 writes UTF-16LE.
            boolean utf16 = b.length >= 2 && (b[0] & 0xff) == 0xff && (b[1] & 0xff) == 0xfe;
            say(hereLog, new String(b, utf16 ? StandardCharsets.UTF_16LE : StandardCharsets.UTF_8).replace("﻿", ""));
            Files.deleteIfExists(logFile);
            Files.deleteIfExists(wrapper);
            return rc;
        } catch (IOException e) {
            say(hereLog, e.getMessage());
            return 1;
        }
    }

    // --- Step 2: the network ---------------------------------------------------------------

    private JPanel stepNetwork() {
        JPanel p = page(L("2. Your network", "2. Tu red"),
                L("Every machine ZFSMgr knows, with what it has against this package. Mark the ones to install or update: each one runs this same package's installer, over SSH.",
                  "Las máquinas que conoce ZFSMgr, con lo que tienen frente a este paquete. Marca en cuáles instalar o actualizar: en cada una se ejecuta el instalador de este mismo paquete, por SSH."));
        JTable t = new JTable(machines);
        t.getColumnModel().getColumn(0).setMaxWidth(32);
        t.setRowHeight(t.getRowHeight() + 4);
        JPanel top = new JPanel();
        top.setLayout(new BoxLayout(top, BoxLayout.Y_AXIS));
        netState.setAlignmentX(LEFT_ALIGNMENT);
        top.add(netState);
        JPanel netButtons = Ui.row(seed, join, add);
        netButtons.setAlignmentX(LEFT_ALIGNMENT);
        top.add(netButtons);
        JPanel center = new JPanel(new BorderLayout(0, 8));
        center.add(top, BorderLayout.NORTH);
        center.add(new JScrollPane(t), BorderLayout.CENTER);
        JPanel south = new JPanel(new BorderLayout(0, 6));
        south.add(Ui.row(netGui, netDaemon), BorderLayout.NORTH);
        JScrollPane logScroll = new JScrollPane(netLog);
        logScroll.setPreferredSize(new java.awt.Dimension(100, 150));
        south.add(logScroll, BorderLayout.CENTER);
        center.add(south, BorderLayout.SOUTH);
        p.add(center, BorderLayout.CENTER);

        JButton back = new JButton(L("← This computer", "← Este ordenador"));
        back.addActionListener(e -> cards.show(body, "here"));
        JButton reload = new JButton(L("Look again", "Volver a mirar"));
        reload.addActionListener(e -> refreshNetwork());
        JButton open = new JButton(L("Open ZFSMgr and finish", "Abrir ZFSMgr y terminar"));
        open.addActionListener(e -> openGui());
        installMarked.addActionListener(e -> installMarked());
        add.addActionListener(e -> {
            if (store != null) {
                new AddMachineDialog(this, store, this::refreshNetwork).setVisible(true);
            }
        });
        seed.addActionListener(e -> seed());
        join.addActionListener(e -> join());
        p.add(buttons(back, reload, installMarked, open), BorderLayout.SOUTH);
        setNetworkEnabled(false);
        return p;
    }

    private void setNetworkEnabled(boolean on) {
        for (JButton b : List.of(seed, join, add, installMarked)) {
            b.setEnabled(on);
        }
    }

    private void toNetwork() {
        cards.show(body, "network");
        if (svc != null) {
            return;
        }
        String cli = Main.findCli();
        if (cli == null) {
            netState.setText(L("Install on this computer first: the network is seen from its zfsmgr-cli.",
                    "Instala primero en este ordenador: la red se ve desde su zfsmgr-cli."));
            return;
        }
        netState.setText(L("Opening ZFSMgr…", "Abriendo ZFSMgr…"));
        new Starter(cli, this::serviceReady).attempt(given);
        given = new char[0];
    }

    private void serviceReady(CliService s) {
        svc = s;
        store = new StateStore(s, id -> s);
        setNetworkEnabled(true);
        refreshNetwork();
    }

    private void refreshNetwork() {
        if (svc == null) {
            return;
        }
        seed.setVisible(!svc.network());
        join.setVisible(!svc.network());
        netState.setText(svc.network()
                ? L("This computer is in a network: every machine below is the network's.",
                    "Este ordenador está en una red: las máquinas de abajo son las de la red.")
                : L("This computer has no network yet: the machines below are only known here. «Create my network» makes them every computer's.",
                    "Este ordenador aún no tiene red: las máquinas de abajo solo se conocen aquí. «Crear mi red» las hace de todos los ordenadores."));
        svc.request("ls zfsm://", r -> {
            machines.setRowCount(0);
            rowIds.clear();
            for (Map<String, Object> c : Json.objects(Json.firstObject(r.out()), "connections")) {
                String id = Json.str(c, "id");
                String name = Json.str(c, "name", id);
                boolean local = "LOCAL".equalsIgnoreCase(Json.str(c, "type"));
                rowIds.add(id);
                machines.addRow(new Object[] {local ? null : Boolean.FALSE, name.isEmpty() ? id : name,
                    Json.str(c, "os"), "…", "…", "…",
                    local ? L("this computer (step 1)", "este ordenador (paso 1)") : L("asking…", "preguntando…")});
            }
            for (int i = 0; i < rowIds.size(); i++) {
                probe(i);
            }
        });
    }

    private void probe(int row) {
        String id = rowIds.get(row);
        svc.request("probe zfsm://" + id, r -> {
            if (row >= rowIds.size() || !rowIds.get(row).equals(id)) {
                return;
            }
            Map<String, Object> m = Json.firstObject(r.out());
            boolean local = machines.getValueAt(row, 0) == null;
            if (!Json.bool(m, "reachable", false)) {
                machines.setValueAt(L("does not answer", "no contesta"), row, 3);
                machines.setValueAt("-", row, 4);
                machines.setValueAt("-", row, 5);
                if (!local) {
                    machines.setValueAt(Boolean.FALSE, row, 0);
                    machines.setValueAt(Json.str(m, "reason"), row, 6);
                }
                return;
            }
            if (!Json.str(m, "os_detail").isEmpty()) {
                machines.setValueAt(Json.str(m, "os_detail"), row, 2);
            }
            boolean zfs = "ok".equals(Json.str(m, "openzfs"));
            String dv = Json.str(m, "daemon_version");
            String daemon = !zfs ? L("no OpenZFS: not needed", "sin OpenZFS: no hace falta")
                    : dv.isEmpty() ? L("not installed", "no instalado") : versionWord(dv);
            String cv = Json.str(m, "cli_version");
            String cli = cv.isEmpty() ? L("not installed", "no instalado") : versionWord(cv);
            boolean gui = Json.bool(m, "gui", false);
            String java = Json.str(m, "gui_java");
            String guiText = !gui ? L("no", "no") : "missing".equals(java) ? L("yes, no Java 21", "sí, sin Java 21") : L("yes", "sí");
            machines.setValueAt(daemon, row, 3);
            machines.setValueAt(cli, row, 4);
            machines.setValueAt(guiText, row, 5);
            if (local) {
                return;
            }
            String sudo = Json.str(m, "sudo");
            boolean sudoOk = sudo.isEmpty() || "ok".equals(sudo) || "not_used".equals(sudo) || "not_needed".equals(sudo);
            boolean behind = !cv.equals(pkgVersion) || (zfs && !dv.equals(pkgVersion)) || (!gui && netGui.isSelected());
            machines.setValueAt(behind && sudoOk, row, 0);
            machines.setValueAt(!sudoOk ? L("sudo does not work: %1", "sudo no funciona: %1", Json.str(m, "sudo_detail", sudo))
                    : behind ? L("to install or update", "para instalar o actualizar") : L("up to date", "al día"), row, 6);
        });
    }

    private String versionWord(String v) {
        return v.equals(pkgVersion) ? L("up to date", "al día") : v;
    }

    private void installMarked() {
        List<Integer> rows = new ArrayList<>();
        for (int i = 0; i < machines.getRowCount(); i++) {
            if (Boolean.TRUE.equals(machines.getValueAt(i, 0))) {
                rows.add(i);
            }
        }
        if (rows.isEmpty()) {
            say(netLog, L("No machine is marked.", "No hay ninguna máquina marcada."));
            return;
        }
        installMarked.setEnabled(false);
        installNext(rows, 0);
    }

    private void installNext(List<Integer> rows, int i) {
        if (i >= rows.size()) {
            installMarked.setEnabled(true);
            say(netLog, L("— Done.", "— Terminado."));
            return;
        }
        int row = rows.get(i);
        String id = rowIds.get(row);
        machines.setValueAt(L("installing…", "instalando…"), row, 6);
        String line = "install-package --package " + Ui.quoted(pkg.toString()) + " --on zfsm://" + id
                + (netDaemon.isSelected() ? "" : " --no-daemon") + (netGui.isSelected() ? "" : " --cli-only");
        say(netLog, "» " + machines.getValueAt(row, 1));
        svc.request(line, "text", false, r -> {
            say(netLog, (r.out() + r.err()).trim());
            machines.setValueAt(r.ok() ? L("installed", "instalado") : L("it failed: see below", "falló: mira abajo"), row, 6);
            if (r.ok()) {
                machines.setValueAt(Boolean.FALSE, row, 0);
                probe(row);
            }
            installNext(rows, i + 1);
        });
    }

    /** `network seed`: the service ends after it; another one opens with the same data key. */
    private void seed() {
        int r = JOptionPane.showConfirmDialog(this, Ui.paragraph(L(
                "This computer's machines, connections and master password become the network's, kept by every daemon. Then every computer that joins sees the same machines.",
                "Las máquinas, las conexiones y la contraseña maestra de este ordenador pasan a ser las de la red, que guardan todos los daemons. Desde entonces, cada ordenador que se una ve las mismas máquinas.")),
                L("Create my network", "Crear mi red"), JOptionPane.OK_CANCEL_OPTION);
        if (r != JOptionPane.OK_OPTION) {
            return;
        }
        String dataKey = svc.dataKey();
        setNetworkEnabled(false);
        svc.request("network seed", "text", false, res -> {
            say(netLog, (res.out() + res.err()).trim());
            reopen(dataKey);
        });
    }

    /** `network join`: the network's master password is asked by the CLI (a secret event). */
    private void join() {
        int r = JOptionPane.showConfirmDialog(this, Ui.paragraph(L(
                "Joining takes the network from a daemon this computer reaches —add one of its machines first— and opens it with the network's master password. This computer's own machines are replaced by the network's.",
                "Unirse toma la red de un daemon al que llega este ordenador —añade antes una de sus máquinas— y la abre con la contraseña maestra de la red. Las máquinas propias de este ordenador se sustituyen por las de la red.")),
                L("Join a network", "Unirme a una red"), JOptionPane.OK_CANCEL_OPTION);
        if (r != JOptionPane.OK_OPTION) {
            return;
        }
        setNetworkEnabled(false);
        svc.request("network join", "text", false, res -> {
            say(netLog, (res.out() + res.err()).trim());
            // The network's secrets are under ITS key: opened again with its master password.
            reopen(null);
        });
    }

    private void reopen(String dataKey) {
        CliService old = svc;
        svc = null;
        store = null;
        old.stop(3000);
        String cli = Main.findCli();
        if (dataKey == null || dataKey.isEmpty()) {
            new Starter(cli, this::serviceReady).attempt(new char[0]);
            return;
        }
        final CliService[] self = new CliService[1];
        CliService s = new CliService(cli, Text.lang(), SwingUtilities::invokeLater, new CliService.Listener() {
            @Override
            public void ready(Map<String, Object> ev) {
                serviceReady(self[0]);
            }

            @Override
            public void failed(String reason, boolean wrongPassword) {
                // The key no longer opens it: ask as at the start.
                new Starter(cli, Installer.this::serviceReady).attempt(new char[0]);
            }

            @Override
            public void confirm(CliService service, CliService.Question q) {
                Dialogs.Answer a = Dialogs.question(Installer.this, q);
                service.answerConfirm(q.id(), a.yes(), a.typed());
            }

            @Override
            public void secret(CliService service, int id, String text) {
                service.answerSecret(id, Dialogs.secret(Installer.this, text));
            }
        });
        self[0] = s;
        s.setSession(CliService.Session.TAKE);
        s.start(dataKey.toCharArray(), true);
    }

    /** The installed interface, and the end: it takes this computer's session, so this one lets it go. */
    private void openGui() {
        if (svc != null) {
            svc.stop(3000);
            svc = null;
        }
        try {
            if (key.startsWith("windows")) {
                String cmd = System.getenv("ProgramFiles") + "\\ZFSMgr\\bin\\zfsmgr-gui.cmd";
                new ProcessBuilder("cmd.exe", "/c", "start", "\"\"", cmd).start();
            } else if (key.startsWith("macos")) {
                new ProcessBuilder("open", "-a", "ZFSMgr").start();
            } else {
                String gui = new File("/usr/local/bin/zfsmgr-gui").canExecute() ? "/usr/local/bin/zfsmgr-gui" : "zfsmgr-gui";
                new ProcessBuilder(gui).redirectErrorStream(true)
                        .redirectOutput(ProcessBuilder.Redirect.DISCARD).start();
            }
        } catch (IOException e) {
            JOptionPane.showMessageDialog(this, e.getMessage(), "ZFSMgr", JOptionPane.ERROR_MESSAGE);
            return;
        }
        dispose();
    }

    // --- Pieces ----------------------------------------------------------------------------

    /** «linux-x86_64», «macos-arm64»…: the names the package uses. */
    static String platformKey() {
        String os = System.getProperty("os.name").toLowerCase();
        String arch = System.getProperty("os.arch").toLowerCase();
        boolean arm = arch.contains("aarch64") || arch.contains("arm64");
        if (os.contains("win")) {
            return "windows-x86_64";
        }
        if (os.contains("mac")) {
            return arm ? "macos-arm64" : "macos-amd64";
        }
        if (os.contains("freebsd")) {
            return "freebsd-x86_64";
        }
        return "linux-x86_64";
    }

    private Path pkgCli() {
        return pkg.resolve("cli").resolve("zfsmgr-cli-" + key + (key.startsWith("windows") ? ".exe" : ""));
    }

    /** «zfsmgr-cli 1.4.3 (1.4.3.527315060)» → the build between brackets. */
    static String fullVersion(String versionLine) {
        int a = versionLine.indexOf('(');
        int b = versionLine.indexOf(')', Math.max(a, 0));
        return a >= 0 && b > a ? versionLine.substring(a + 1, b).trim() : "";
    }

    private static JPanel page(String title, String text) {
        JPanel p = new JPanel(new BorderLayout(0, 10));
        p.setBorder(BorderFactory.createEmptyBorder(16, 18, 14, 18));
        JPanel head = new JPanel();
        head.setLayout(new BoxLayout(head, BoxLayout.Y_AXIS));
        JLabel h = Ui.heading(title);
        h.setAlignmentX(LEFT_ALIGNMENT);
        head.add(h);
        head.add(Box.createVerticalStrut(4));
        JLabel t = Ui.paragraph(text);
        t.setAlignmentX(LEFT_ALIGNMENT);
        head.add(t);
        p.add(head, BorderLayout.NORTH);
        return p;
    }

    private static JPanel buttons(JButton... bs) {
        JPanel p = new JPanel();
        p.setLayout(new BoxLayout(p, BoxLayout.X_AXIS));
        p.add(Box.createHorizontalGlue());
        for (JButton b : bs) {
            p.add(Box.createHorizontalStrut(6));
            p.add(b);
        }
        return p;
    }

    private static JTextArea log() {
        JTextArea a = new JTextArea();
        a.setEditable(false);
        a.setFont(new Font(Font.MONOSPACED, Font.PLAIN, a.getFont().getSize()));
        return a;
    }

    private static void say(JTextArea a, String text) {
        if (text == null || text.isEmpty()) {
            return;
        }
        Runnable r = () -> {
            a.append(text.endsWith("\n") ? text : text + "\n");
            a.setCaretPosition(a.getDocument().getLength());
        };
        if (SwingUtilities.isEventDispatchThread()) {
            r.run();
        } else {
            SwingUtilities.invokeLater(r);
        }
    }

    private static void background(java.util.function.IntSupplier work, Consumer<Integer> done) {
        Thread t = new Thread(() -> {
            int rc = work.getAsInt();
            SwingUtilities.invokeLater(() -> done.accept(rc));
        }, "zfsmgr-installer");
        t.start();
    }

    /** Runs `cmd`, its output line by line into `log`; `stdinFirst` (a password) as the first line. */
    private static int stream(List<String> cmd, char[] stdinFirst, JTextArea log) {
        try {
            Process p = new ProcessBuilder(cmd).redirectErrorStream(true).start();
            try (OutputStream in = p.getOutputStream()) {
                if (stdinFirst != null) {
                    byte[] b = (new String(stdinFirst) + "\n").getBytes(StandardCharsets.UTF_8);
                    java.util.Arrays.fill(stdinFirst, '\0');
                    in.write(b);
                    java.util.Arrays.fill(b, (byte) 0);
                }
            }
            try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
                for (String l; (l = r.readLine()) != null; ) {
                    say(log, l);
                }
            }
            return p.waitFor();
        } catch (IOException e) {
            say(log, e.getMessage());
            return 1;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return 1;
        }
    }

    private static String run(List<String> cmd, int seconds) {
        try {
            Process p = new ProcessBuilder(cmd).redirectErrorStream(true).start();
            p.getOutputStream().close();
            String out = new String(p.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
            p.waitFor(seconds, TimeUnit.SECONDS);
            return out;
        } catch (IOException e) {
            return "";
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return "";
        }
    }

    private static int exit(List<String> cmd) {
        try {
            Process p = new ProcessBuilder(cmd).redirectErrorStream(true)
                    .redirectOutput(ProcessBuilder.Redirect.DISCARD).start();
            p.getOutputStream().close();
            return p.waitFor(10, TimeUnit.SECONDS) ? p.exitValue() : 1;
        } catch (IOException e) {
            return 1;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return 1;
        }
    }

    /** A value between single quotes for sh. */
    private static String sq(String s) {
        return "'" + s.replace("'", "'\\''") + "'";
    }

    /** A value between single quotes for PowerShell. */
    private static String psq(String s) {
        return "'" + s.replace("'", "''") + "'";
    }

    private static String encoded(String script) {
        return Base64.getEncoder().encodeToString(script.getBytes(StandardCharsets.UTF_16LE));
    }
}
