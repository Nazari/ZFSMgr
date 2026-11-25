package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import java.awt.BorderLayout;
import java.awt.CardLayout;
import java.awt.Dimension;
import java.awt.event.WindowAdapter;
import java.awt.event.WindowEvent;
import java.util.LinkedHashMap;
import java.util.List;
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
 */
public final class MainWindow extends JFrame {
    private final String cli;
    private final CliService general;
    private final Map<String, CliService> perMachine = new LinkedHashMap<>();
    private final StateStore store;
    private final JLabel statusLine = Ui.muted(" ");
    private MachinesPage machines;
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
        // Administration only in advanced mode.
        javax.swing.DefaultListModel<String> navModel = new javax.swing.DefaultListModel<>();
        for (String p : pages) {
            navModel.addElement(p);
        }
        JList<String> nav = new JList<>(navModel);
        nav.setSelectionMode(ListSelectionModel.SINGLE_SELECTION);
        nav.setBorder(BorderFactory.createEmptyBorder(6, 6, 6, 6));
        CardLayout cards = new CardLayout();
        JPanel content = new JPanel(cards);
        content.add(new StatusPage(store), pages[0]);
        content.add(new CopiesPage(store, this::say, () -> newCopy("", "")), pages[1]);
        machines = new MachinesPage(store, new MachinesPage.Host() {
            @Override
            public void say(String text) {
                MainWindow.this.say(text);
            }

            @Override
            public void addMachine() {
                new AddMachineDialog(MainWindow.this, store, store::refresh).setVisible(true);
            }

            @Override
            public void newCopy(String machine, String dataset) {
                MainWindow.this.newCopy(machine, dataset);
            }

            @Override
            public void createPool(String machine) {
                new CreatePoolDialog(MainWindow.this, store, machine, store::refresh).setVisible(true);
            }
        });
        content.add(machines, pages[2]);
        content.add(new JobsPage(store, this::say), pages[3]);
        content.add(new AdminPage(store, this::say,
                () -> new AddMachineDialog(this, store, store::refresh).setVisible(true)), pages[4]);
        nav.addListSelectionListener(e -> {
            if (!e.getValueIsAdjusting() && nav.getSelectedValue() != null) {
                cards.show(content, nav.getSelectedValue());
            }
        });
        // ZFSMGR_GUI_PAGE=<n>: opens on that page, for the captures without a screen.
        int first = 0;
        try {
            first = Math.max(0, Math.min(pages.length - 1,
                    Integer.parseInt(System.getenv().getOrDefault("ZFSMGR_GUI_PAGE", "0"))));
        } catch (NumberFormatException ignored) {
            // the first one
        }
        nav.setSelectedIndex(first);
        // ZFSMGR_GUI_REVEAL=<machine>[/<dataset>]: Machines opened down to it, for captures.
        String reveal = System.getenv().getOrDefault("ZFSMGR_GUI_REVEAL", "");
        if (!reveal.isEmpty()) {
            int slash = reveal.indexOf('/');
            machines.reveal(slash < 0 ? reveal : reveal.substring(0, slash), slash < 0 ? "" : reveal.substring(slash + 1));
        }
        JScrollPane navScroll = new JScrollPane(nav);
        navScroll.setPreferredSize(new Dimension(180, 100));
        // Advanced mode: what a person who knows ZFS wants and the rest is better without. It is
        // remembered on this computer. ZFSMGR_GUI_ADVANCED=1 switches it on for captures.
        java.util.prefs.Preferences prefs = java.util.prefs.Preferences.userNodeForPackage(MainWindow.class);
        javax.swing.JCheckBox advanced = new javax.swing.JCheckBox(L("Advanced mode", "Modo avanzado"),
                prefs.getBoolean("advanced", false) || "1".equals(System.getenv("ZFSMGR_GUI_ADVANCED")));
        advanced.setBorder(BorderFactory.createEmptyBorder(6, 8, 6, 8));
        Runnable applyAdvanced = () -> {
            boolean on = advanced.isSelected();
            machines.setAdvanced(on);
            if (on && navModel.size() < pages.length) {
                navModel.addElement(pages[4]);
            } else if (!on && navModel.size() == pages.length) {
                if (nav.getSelectedIndex() == 4) {
                    nav.setSelectedIndex(0);
                }
                navModel.removeElementAt(4);
            }
        };
        // What a capture switches on by environment is not remembered as the person's choice.
        boolean forced = "1".equals(System.getenv("ZFSMGR_GUI_ADVANCED"));
        advanced.addItemListener(e -> {
            if (!forced) {
                prefs.putBoolean("advanced", advanced.isSelected());
            }
            applyAdvanced.run();
        });
        applyAdvanced.run();
        JPanel side = new JPanel(new BorderLayout());
        side.add(navScroll, BorderLayout.CENTER);
        side.add(advanced, BorderLayout.SOUTH);
        getContentPane().add(side, BorderLayout.WEST);
        getContentPane().add(content, BorderLayout.CENTER);
        statusLine.setBorder(BorderFactory.createEmptyBorder(3, 8, 3, 8));
        getContentPane().add(statusLine, BorderLayout.SOUTH);
        setSize(1100, 760);
        setLocationByPlatform(true);
    }

    /** The new-copy wizard, with this dataset chosen when both are given. */
    public void newCopy(String machine, String dataset) {
        NewCopyWizard w = new NewCopyWizard(this, store, store::refresh);
        if (!dataset.isEmpty()) {
            w.preselect(machine, dataset);
        }
        w.setVisible(true);
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
            String localId = "";
            for (Map<String, Object> c : Json.objects(Json.firstObject(l.out()), "connections")) {
                if (Json.str(c, "type").equalsIgnoreCase("LOCAL")) {
                    hasLocal = true;
                    localId = Json.str(c, "id");
                }
            }
            if (l.ok() && !hasLocal) {
                firstRunDone(L("This computer has no OpenZFS ZFSMgr can manage: it will be the control for your other machines.",
                        "Este ordenador no tiene un OpenZFS que ZFSMgr pueda manejar: será el mando de tus otras máquinas."));
                return;
            }
            final String here = localId;
            probeHere(here);
        });
    }

    /** THIS computer's connection, by its name: there is no «local» (docs/diseno_red_unica.md). */
    private void probeHere(String id) {
        general.request("probe zfsm://" + id, r -> {
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
            // The sudo password helps only when it is missing or wrong; a smart card asking for
            // its PIN, or no sudo at all, is said by probe's own warning.
            if (!sudo.equals("needs_password") && !sudo.equals("wrong_password") && !sudo.equals("ok")
                    && !sudo.equals("not_used")) {
                List<String> why = new java.util.ArrayList<>();
                for (Map<String, Object> w : Json.objects(p, "warnings")) {
                    if (Json.str(w, "subject").equals("sudo")) {
                        why.add(Json.str(w, "text"));
                    }
                }
                firstRunDone(why.isEmpty() ? Json.str(p, "sudo_detail") : String.join(" ", why));
                return;
            }
            if (!sudo.equals("ok") && !sudo.equals("not_used") && !sudoOffered) {
                sudoOffered = true;
                if (yes(L("This computer has OpenZFS %1. To manage its pools ZFSMgr needs your sudo password here. It is kept encrypted with the master password.",
                        "Este ordenador tiene OpenZFS %1. Para manejar sus pools ZFSMgr necesita tu contraseña de sudo de aquí. Se guarda cifrada con la contraseña maestra.",
                        Json.str(p, "openzfs_version")), L("Give the sudo password…", "Dar la contraseña de sudo…"))) {
                    // The CLI asks for it (a secret event): never on a command line.
                    general.request("edit zfsm://" + id + " --sudo", e -> {
                        if (!e.ok()) {
                            say(e.err().trim());
                        }
                        probeHere(id);
                    });
                } else {
                    probeHere(id);
                }
                return;
            }
            if (sudo.equals("ok") && daemon.equals("unreachable") && !daemonOffered) {
                daemonOffered = true;
                if (yes(L("This computer has no ZFSMgr daemon, or it does not answer. It is the service that does the ZFS work here, and what lets other machines copy to and from it. Install it now?",
                        "Este ordenador no tiene el daemon de ZFSMgr, o no responde. Es el servicio que hace aquí el trabajo con ZFS, y el que deja a otras máquinas copiar hacia y desde él. ¿Instalarlo ahora?"),
                        L("Install it", "Instalarlo"))) {
                    general.request("install-daemon --on zfsm://" + id, e -> {
                        if (!e.ok()) {
                            say(e.err().trim());
                        }
                        probeHere(id);
                    });
                } else {
                    probeHere(id);
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
