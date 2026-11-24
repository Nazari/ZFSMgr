package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import java.awt.BorderLayout;
import java.awt.CardLayout;
import java.awt.Dimension;
import java.awt.event.WindowAdapter;
import java.awt.event.WindowEvent;
import java.util.LinkedHashMap;
import java.util.Map;
import javax.swing.BorderFactory;
import javax.swing.JFrame;
import javax.swing.JLabel;
import javax.swing.JList;
import javax.swing.JOptionPane;
import javax.swing.JPanel;
import javax.swing.JScrollPane;
import javax.swing.ListSelectionModel;
import javax.swing.SwingUtilities;
import javax.swing.Timer;

/**
 * The window: the pages on the left, the chosen one on the right, a status line below.
 * (src/gui/mainwindow.cpp.)
 */
public final class MainWindow extends JFrame {
    private final String cli;
    private final CliService general;
    private final Map<String, CliService> perMachine = new LinkedHashMap<>();
    private final StateStore store;
    private final JLabel statusLine = Ui.muted(" ");
    private boolean closing;
    private boolean closeAskedShown;
    private boolean sudoOffered;
    private boolean daemonOffered;

    /** The machine services' questions and secrets: the same dialogs as the general one's. */
    private final CliService.Listener machineListener = new CliService.Listener() {
        @Override
        public void confirm(CliService service, CliService.Question q) {
            Dialogs.Answer a = Dialogs.question(MainWindow.this, q);
            service.answerConfirm(q.id(), a.yes(), a.typed());
        }

        @Override
        public void secret(CliService service, int id, String text) {
            service.answerSecret(id, Dialogs.secret(MainWindow.this, text));
        }
    };

    public MainWindow(String cli, CliService general) {
        super("ZFSMgr");
        this.cli = cli;
        this.general = general;
        // Each machine through its own service, opened with the data key the general one got:
        // a slow machine delays only its own answer.
        this.store = new StateStore(general, id -> perMachine.computeIfAbsent(id, k -> {
            CliService s = new CliService(cli, Text.lang(), SwingUtilities::invokeLater, machineListener);
            s.start(general.dataKey().toCharArray(), true);
            return s;
        }));
        setDefaultCloseOperation(JFrame.DO_NOTHING_ON_CLOSE);
        addWindowListener(new WindowAdapter() {
            @Override
            public void windowClosing(WindowEvent e) {
                close();
            }
        });

        String[] pages = {L("Status", "Estado"), L("Copies", "Copias"), L("Machines", "Máquinas"),
                          L("Jobs", "Trabajos"), L("Administration", "Administración")};
        JList<String> nav = new JList<>(pages);
        nav.setSelectionMode(ListSelectionModel.SINGLE_SELECTION);
        nav.setBorder(BorderFactory.createEmptyBorder(6, 6, 6, 6));
        CardLayout cards = new CardLayout();
        JPanel content = new JPanel(cards);
        content.add(new StatusPage(store), pages[0]);
        for (int i = 1; i < pages.length; i++) {
            JPanel p = new JPanel(new BorderLayout());
            p.setBorder(BorderFactory.createEmptyBorder(16, 16, 16, 16));
            p.add(Ui.muted(L("Not in the Java interface yet.", "Todavía no está en la interfaz en Java.")),
                    BorderLayout.NORTH);
            content.add(p, pages[i]);
        }
        nav.addListSelectionListener(e -> {
            if (!e.getValueIsAdjusting() && nav.getSelectedValue() != null) {
                cards.show(content, nav.getSelectedValue());
            }
        });
        nav.setSelectedIndex(0);
        JScrollPane navScroll = new JScrollPane(nav);
        navScroll.setPreferredSize(new Dimension(180, 100));
        getContentPane().add(navScroll, BorderLayout.WEST);
        getContentPane().add(content, BorderLayout.CENTER);
        statusLine.setBorder(BorderFactory.createEmptyBorder(3, 8, 3, 8));
        getContentPane().add(statusLine, BorderLayout.SOUTH);
        setSize(1100, 760);
        setLocationByPlatform(true);
    }

    public StateStore store() {
        return store;
    }

    /** A line in the status bar, for 15 s. */
    public void say(String text) {
        statusLine.setText(text);
        Timer t = new Timer(15_000, e -> {
            if (statusLine.getText().equals(text)) {
                statusLine.setText(" ");
            }
        });
        t.setRepeats(false);
        t.start();
    }

    void waitingReport() {
        String g = general.waiting();
        if (!g.isEmpty()) {
            System.err.println("[waiting] general: " + g);
        }
        perMachine.forEach((id, s) -> {
            String w = s.waiting();
            if (!w.isEmpty()) {
                System.err.println("[waiting] " + id + ": " + w);
            }
        });
    }

    /**
     * The general service ended while the window was open: it holds the session, so the
     * interface cannot go on —this is also how a forced close from another machine arrives—.
     */
    public void serviceLost(String reason) {
        if (closing) {
            return;
        }
        closing = true;
        JOptionPane.showMessageDialog(this, L("ZFSMgr has to close: %1", "ZFSMgr tiene que cerrarse: %1", reason),
                "ZFSMgr", JOptionPane.WARNING_MESSAGE);
        close();
    }

    /**
     * Someone on another machine asked this session to close (`sessions close`): it closes in
     * 20 s unless the person chooses to keep it open. (MainWindow::closeAsked.)
     */
    public void closeAsked(String host, String user) {
        if (closeAskedShown) {
            return;
        }
        closeAskedShown = true;
        String who = user.isEmpty() ? "?" : user;
        String where = host.isEmpty() ? "?" : host;
        JLabel text = new JLabel();
        Object[] options = {L("Close now", "Cerrar ahora"), L("Keep it open", "Seguir abierto")};
        JOptionPane pane = new JOptionPane(text, JOptionPane.INFORMATION_MESSAGE, JOptionPane.DEFAULT_OPTION,
                null, options, options[0]);
        javax.swing.JDialog d = pane.createDialog(this, "ZFSMgr");
        d.setModal(false);
        final int[] left = {20};
        Runnable say = () -> text.setText("<html>" + Ui.escape(L("%1, on %2, asked that ZFSMgr close here.",
                "%1, desde %2, ha pedido que ZFSMgr se cierre aquí.", who, where)) + "<br>"
                + L("It closes in %1 s.", "Se cierra en %1 s.", left[0]) + "</html>");
        say.run();
        Timer t = new Timer(1000, null);
        t.addActionListener(e -> {
            if (--left[0] <= 0) {
                t.stop();
                d.dispose();
                close();
                return;
            }
            say.run();
        });
        pane.addPropertyChangeListener(JOptionPane.VALUE_PROPERTY, e -> {
            t.stop();
            d.dispose();
            if (options[0].equals(pane.getValue())) {
                close();
            } else {
                closeAskedShown = false;
            }
        });
        t.start();
        d.setVisible(true);
    }

    /**
     * The first run on this computer: how it is (`probe`), and the first thing missing offered,
     * one step at a time. Without a Local connection —neither the agent nor OpenZFS here— it is
     * only the control for the others. (MainWindow::firstRunStep.)
     */
    public void startFirstRun() {
        general.request("ls zfsm://", l -> {
            boolean hasLocal = false;
            for (Map<String, Object> c : Json.objects(Json.firstObject(l.out()), "connections")) {
                if (Json.str(c, "type").equalsIgnoreCase("LOCAL")) {
                    hasLocal = true;
                }
            }
            if (l.ok() && !hasLocal) {
                firstRunDone(L("This computer has no OpenZFS ZFSMgr can manage: it will be the control for your other machines.",
                        "Este ordenador no tiene un OpenZFS que ZFSMgr pueda manejar: será el mando de tus otras máquinas."));
                return;
            }
            probeHere();
        });
    }

    private void probeHere() {
        general.request("probe zfsm://local", r -> {
            Map<String, Object> p = Json.firstObject(r.out());
            if (p.isEmpty()) {
                firstRunDone(r.err().trim());
                return;
            }
            String zfs = Json.str(p, "openzfs");
            String sudo = Json.str(p, "sudo");
            String daemon = Json.str(p, "daemon");
            if (!zfs.equals("ok")) {
                firstRunDone(L("This computer has no OpenZFS ZFSMgr can manage: it will be the control for your other machines.",
                        "Este ordenador no tiene un OpenZFS que ZFSMgr pueda manejar: será el mando de tus otras máquinas."));
                return;
            }
            if (!sudo.equals("ok") && !sudo.equals("not_used") && !sudoOffered) {
                sudoOffered = true;
                if (yes(L("This computer has OpenZFS %1. To manage its pools ZFSMgr needs your sudo password here. It is kept encrypted with the master password.",
                        "Este ordenador tiene OpenZFS %1. Para manejar sus pools ZFSMgr necesita tu contraseña de sudo de aquí. Se guarda cifrada con la contraseña maestra.",
                        Json.str(p, "openzfs_version")), L("Give the sudo password…", "Dar la contraseña de sudo…"))) {
                    // The CLI asks for it (a secret event): never on a command line.
                    general.request("edit zfsm://local --sudo", e -> {
                        if (!e.ok()) {
                            say(e.err().trim());
                        }
                        probeHere();
                    });
                } else {
                    probeHere();
                }
                return;
            }
            if (sudo.equals("ok") && daemon.equals("unreachable") && !daemonOffered) {
                daemonOffered = true;
                if (yes(L("This computer has no ZFSMgr daemon, or it does not answer. It is the service that does the ZFS work here, and what lets other machines copy to and from it. Install it now?",
                        "Este ordenador no tiene el daemon de ZFSMgr, o no responde. Es el servicio que hace aquí el trabajo con ZFS, y el que deja a otras máquinas copiar hacia y desde él. ¿Instalarlo ahora?"),
                        L("Install it", "Instalarlo"))) {
                    general.request("install-daemon --on zfsm://local", e -> {
                        if (!e.ok()) {
                            say(e.err().trim());
                        }
                        probeHere();
                    });
                } else {
                    probeHere();
                }
                return;
            }
            firstRunDone(L("Ready.", "Listo."));
        });
    }

    private boolean yes(String text, String action) {
        Object[] options = {action, L("Not now", "Ahora no")};
        return JOptionPane.showOptionDialog(this, Ui.paragraph(text), "ZFSMgr", JOptionPane.DEFAULT_OPTION,
                JOptionPane.QUESTION_MESSAGE, null, options, options[0]) == 0;
    }

    private void firstRunDone(String text) {
        say(text);
        store.refresh();
    }

    /** The services say goodbye —the general one to the daemons, as the session— and it ends. */
    public void close() {
        closing = true;
        setVisible(false);
        for (CliService s : perMachine.values()) {
            s.stop(1500);
        }
        general.stop(4000);
        dispose();
        System.exit(0);
    }
}
