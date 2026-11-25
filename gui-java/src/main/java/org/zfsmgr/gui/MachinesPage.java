package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import java.awt.BorderLayout;
import java.awt.Component;
import java.awt.Dimension;
import java.time.Instant;
import java.time.LocalDateTime;
import java.time.ZoneId;
import java.time.format.DateTimeFormatter;
import java.time.format.FormatStyle;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.Supplier;
import javax.swing.BorderFactory;
import javax.swing.Box;
import javax.swing.BoxLayout;
import javax.swing.JButton;
import javax.swing.JCheckBox;
import javax.swing.JComboBox;
import javax.swing.JComponent;
import javax.swing.JLabel;
import javax.swing.JOptionPane;
import javax.swing.JPanel;
import javax.swing.JScrollPane;
import javax.swing.JSpinner;
import javax.swing.JSplitPane;
import javax.swing.JTabbedPane;
import javax.swing.JTable;
import javax.swing.JTextArea;
import javax.swing.JTextField;
import javax.swing.JTree;
import javax.swing.SpinnerNumberModel;
import javax.swing.event.TreeExpansionEvent;
import javax.swing.event.TreeWillExpandListener;
import javax.swing.table.DefaultTableCellRenderer;
import javax.swing.table.DefaultTableModel;
import javax.swing.tree.DefaultMutableTreeNode;
import javax.swing.tree.DefaultTreeModel;
import javax.swing.tree.TreePath;

/**
 * «Máquinas»: a tree —machine, pool, dataset— and the detail of what is chosen, with what can be
 * done to it. Every level is asked to the CLI when it is opened ({@code ls}, {@code ls -r},
 * {@code probe}), on the service of its own machine, so a slow machine only slows its own
 * branch.
 */
public final class MachinesPage extends JPanel {
    enum Kind { MACHINE, POOL, DATASET, WAITING }

    /** What a node of the tree stands for. */
    record Node(Kind kind, String machine, String name, String label) {
        @Override
        public String toString() {
            return label;
        }
    }

    /** What the page asks the window for: dialogs that belong to it, and its status line. */
    public interface Host {
        void say(String text);

        void addMachine();

        void newCopy(String machine, String dataset);

        void createPool(String machine);
    }

    private final StateStore store;
    private final Host host;
    private final DefaultMutableTreeNode root = new DefaultMutableTreeNode();
    private final DefaultTreeModel model = new DefaultTreeModel(root);
    private final JTree tree = new JTree(model);
    private final JScrollPane detail = new JScrollPane();
    private boolean advanced;
    private int datasetTab;
    /** The chosen tab of a pool: «Pool», then its root dataset's tabs. */
    private int poolTab;
    /** Pools whose datasets were asked when choosing the pool, without opening it. */
    private final Set<String> entriesAsked = new HashSet<>();
    private final Map<String, Map<String, Object>> probe = new HashMap<>();
    private final Map<String, Integer> stranded = new HashMap<>();
    private final Map<String, List<Map<String, Object>>> pools = new HashMap<>();
    private final Map<String, List<Map<String, Object>>> entries = new HashMap<>();
    private final Map<String, Map<String, Object>> status = new HashMap<>();
    private final Map<String, String> filesPath = new HashMap<>();
    private final Map<String, List<Map<String, Object>>> files = new HashMap<>();
    private final Map<String, List<Map<String, Object>>> props = new HashMap<>();
    private final Set<String> propsAsked = new HashSet<>();
    /**
     * What has been asked and not answered yet. The detail is rebuilt with every answer that
     * arrives —another machine's overview, say—, and without this each rebuild asked the same
     * thing again while the first answer was on its way.
     */
    private final Set<String> inFlight = new HashSet<>();
    private final Map<String, List<Map<String, Object>>> breakdown = new HashMap<>();
    private String revealMachine = "";
    private String revealDataset = "";
    private List<String> shownMachines = List.of();

    public MachinesPage(StateStore store, Host host) {
        super(new BorderLayout(0, 8));
        this.store = store;
        this.host = host;
        setBorder(BorderFactory.createEmptyBorder(10, 12, 10, 12));
        JPanel top = new JPanel(new BorderLayout());
        JLabel title = Ui.heading(L("Machines", "Máquinas"));
        title.setFont(title.getFont().deriveFont(title.getFont().getSize2D() * 1.3f));
        top.add(title, BorderLayout.WEST);
        JButton add = new JButton(L("+ Add", "+ Añadir"));
        add.addActionListener(e -> host.addMachine());
        top.add(add, BorderLayout.EAST);
        add(top, BorderLayout.NORTH);

        tree.setRootVisible(false);
        tree.setShowsRootHandles(true);
        tree.getSelectionModel().setSelectionMode(javax.swing.tree.TreeSelectionModel.SINGLE_TREE_SELECTION);
        JScrollPane treeScroll = new JScrollPane(tree);
        treeScroll.setMinimumSize(new Dimension(220, 100));
        detail.setBorder(null);
        detail.getVerticalScrollBar().setUnitIncrement(16);
        JSplitPane split = new JSplitPane(JSplitPane.HORIZONTAL_SPLIT, treeScroll, detail);
        split.setDividerLocation(280);
        split.setBorder(null);
        add(split, BorderLayout.CENTER);

        // For captures without a screen: which dataset (and pool) tab opens first.
        try {
            datasetTab = Integer.parseInt(System.getenv().getOrDefault("ZFSMGR_GUI_TAB", "0"));
            poolTab = datasetTab;
        } catch (NumberFormatException ignored) {
            datasetTab = 0;
        }
        tree.addTreeWillExpandListener(new TreeWillExpandListener() {
            @Override
            public void treeWillExpand(TreeExpansionEvent e) {
                expand((DefaultMutableTreeNode) e.getPath().getLastPathComponent());
            }

            @Override
            public void treeWillCollapse(TreeExpansionEvent e) {}
        });
        tree.addTreeSelectionListener(e -> showDetail());
        store.onChanged(() -> {
            rebuildMachines();
            continueReveal();
        });
        store.onJobEnded(this::reloadOpenPools);
        rebuildMachines();
        showDetail();
    }

    // ── The tree.

    private static String url(String machine, String name) {
        return "zfsm://" + machine + (name == null || name.isEmpty() ? "" : "/" + name);
    }

    private static String poolOf(String dataset) {
        int slash = dataset.indexOf('/');
        return slash < 0 ? dataset : dataset.substring(0, slash);
    }

    private static Node nodeOf(DefaultMutableTreeNode n) {
        return n != null && n.getUserObject() instanceof Node x ? x : null;
    }

    private static DefaultMutableTreeNode waiting() {
        return new DefaultMutableTreeNode(new Node(Kind.WAITING, "", "", "…"));
    }

    private void rebuildMachines() {
        // Rebuilt only when the list of machines changes: a refresh every minute must not close
        // what the person opened.
        List<String> now = new ArrayList<>();
        for (StatusModel.MachineView m : store.machines()) {
            now.add(m.id + (m.connected ? "+" : "-"));
        }
        if (now.equals(shownMachines)) {
            return;
        }
        shownMachines = now;
        root.removeAllChildren();
        for (StatusModel.MachineView m : store.machines()) {
            DefaultMutableTreeNode it = new DefaultMutableTreeNode(new Node(Kind.MACHINE, m.id, m.id,
                    m.connected ? m.name : m.name + L("  (disconnected)", "  (desconectada)")));
            if (m.connected) {
                it.add(waiting());
            }
            root.add(it);
        }
        model.reload();
    }

    private static boolean unloaded(DefaultMutableTreeNode n) {
        return n.getChildCount() == 1 && nodeOf((DefaultMutableTreeNode) n.getChildAt(0)).kind() == Kind.WAITING;
    }

    private void expand(DefaultMutableTreeNode item) {
        Node n = nodeOf(item);
        if (n == null || !unloaded(item)) {
            return;
        }
        if (n.kind() == Kind.MACHINE) {
            String machine = n.machine();
            store.forMachine(machine).request("ls " + url(machine, ""), r -> {
                List<Map<String, Object>> ps = Json.objects(Json.firstObject(r.out()), "pools");
                pools.put(machine, ps);
                // Looked up again: the tree may have been rebuilt while the answer travelled.
                DefaultMutableTreeNode mi = findItem(machine, "");
                if (mi == null || !unloaded(mi)) {
                    return;
                }
                mi.removeAllChildren();
                for (Map<String, Object> p : ps) {
                    String pn = Json.str(p, "name");
                    DefaultMutableTreeNode pi = new DefaultMutableTreeNode(new Node(Kind.POOL, machine, pn, pn));
                    if (Json.bool(p, "imported", true)) {
                        pi.add(waiting());
                    }
                    mi.add(pi);
                }
                model.nodeStructureChanged(mi);
                tree.expandPath(new TreePath(mi.getPath()));
                continueReveal();
            });
        }
        if (n.kind() == Kind.POOL) {
            loadPool(item);
        }
    }

    private void loadPool(DefaultMutableTreeNode poolItem) {
        Node n = nodeOf(poolItem);
        String machine = n.machine();
        String pool = n.name();
        store.forMachine(machine).request("ls -r " + url(machine, pool), r -> {
            List<Map<String, Object>> es = Json.objects(Json.firstObject(r.out()), "entries");
            entries.put(machine + "/" + pool, es);
            DefaultMutableTreeNode pi = findItem(machine, pool);
            if (pi == null) {
                return;
            }
            // What was chosen inside, chosen again after rebuilding the branch.
            Node cur = selected();
            String chosen = cur != null && cur.kind() == Kind.DATASET && cur.machine().equals(machine) ? cur.name() : "";
            boolean wasExpanded = tree.isExpanded(new TreePath(pi.getPath()));
            Set<String> open = new HashSet<>();
            for (Enumeration<TreePath> en = tree.getExpandedDescendants(new TreePath(pi.getPath())); en != null
                    && en.hasMoreElements();) {
                Node x = nodeOf((DefaultMutableTreeNode) en.nextElement().getLastPathComponent());
                if (x != null) {
                    open.add(x.name());
                }
            }
            // The datasets as a tree, by their names: «pool/a/b» hangs from «pool/a».
            pi.removeAllChildren();
            Map<String, DefaultMutableTreeNode> byName = new HashMap<>();
            byName.put(pool, pi);
            for (Map<String, Object> e : es) {
                String name = Json.str(e, "name");
                if (Json.str(e, "type").equals("snapshot") || name.equals(pool) || name.contains("@")) {
                    continue;
                }
                int slash = name.lastIndexOf('/');
                DefaultMutableTreeNode parent = byName.getOrDefault(slash < 0 ? "" : name.substring(0, slash), pi);
                DefaultMutableTreeNode di = new DefaultMutableTreeNode(
                        new Node(Kind.DATASET, machine, name, name.substring(slash + 1)));
                parent.add(di);
                byName.put(name, di);
            }
            model.nodeStructureChanged(pi);
            if (wasExpanded || pi.getChildCount() > 0) {
                tree.expandPath(new TreePath(pi.getPath()));
            }
            for (String o : open) {
                DefaultMutableTreeNode x = byName.get(o);
                if (x != null) {
                    tree.expandPath(new TreePath(x.getPath()));
                }
            }
            DefaultMutableTreeNode again = byName.get(chosen);
            if (again != null && !chosen.isEmpty()) {
                tree.setSelectionPath(new TreePath(again.getPath()));
            }
            continueReveal();
            showDetail();
        });
    }

    /** Opens the tree down to that machine, or to one of its datasets, and chooses it. */
    public void reveal(String machine, String dataset) {
        revealMachine = machine;
        revealDataset = dataset == null ? "" : dataset;
        continueReveal();
    }

    // One step further each time something arrives: the machine, its pools, the pool's datasets.
    private void continueReveal() {
        if (revealMachine.isEmpty()) {
            return;
        }
        DefaultMutableTreeNode mi = findItem(revealMachine, "");
        if (mi == null) {
            return;  // the list of machines has not arrived yet
        }
        if (revealDataset.isEmpty()) {
            tree.setSelectionPath(new TreePath(mi.getPath()));
            revealMachine = "";
            return;
        }
        tree.expandPath(new TreePath(mi.getPath()));
        DefaultMutableTreeNode pi = findItem(revealMachine, poolOf(revealDataset));
        if (pi == null) {
            return;  // its pools are on their way
        }
        if (!revealDataset.contains("/")) {
            // The pool itself.
            TreePath path = new TreePath(pi.getPath());
            tree.setSelectionPath(path);
            tree.scrollPathToVisible(path);
            revealMachine = "";
            revealDataset = "";
            return;
        }
        tree.expandPath(new TreePath(pi.getPath()));
        for (Enumeration<javax.swing.tree.TreeNode> en = pi.depthFirstEnumeration(); en.hasMoreElements();) {
            DefaultMutableTreeNode it = (DefaultMutableTreeNode) en.nextElement();
            Node x = nodeOf(it);
            if (x != null && x.kind() == Kind.DATASET && x.name().equals(revealDataset)) {
                TreePath path = new TreePath(it.getPath());
                tree.expandPath(path.getParentPath());
                tree.setSelectionPath(path);
                tree.scrollPathToVisible(path);
                revealMachine = "";
                revealDataset = "";
                return;
            }
        }
    }

    /** The tree's node for a machine (empty pool) or for one of its pools; null if gone. */
    private DefaultMutableTreeNode findItem(String machine, String pool) {
        for (int i = 0; i < root.getChildCount(); i++) {
            DefaultMutableTreeNode mi = (DefaultMutableTreeNode) root.getChildAt(i);
            if (!nodeOf(mi).machine().equals(machine)) {
                continue;
            }
            if (pool.isEmpty()) {
                return mi;
            }
            for (int j = 0; j < mi.getChildCount(); j++) {
                Node p = nodeOf((DefaultMutableTreeNode) mi.getChildAt(j));
                if (p != null && p.kind() == Kind.POOL && p.name().equals(pool)) {
                    return (DefaultMutableTreeNode) mi.getChildAt(j);
                }
            }
        }
        return null;
    }

    private Node selected() {
        TreePath p = tree.getSelectionPath();
        return p == null ? null : nodeOf((DefaultMutableTreeNode) p.getLastPathComponent());
    }

    /** Advanced mode: the pool's maintenance, creating and importing pools, more dataset tabs. */
    public void setAdvanced(boolean on) {
        advanced = on;
        showDetail();
    }

    // ── The detail.

    private static Component left(Component c) {
        if (c instanceof JComponent j) {
            j.setAlignmentX(Component.LEFT_ALIGNMENT);
        }
        return c;
    }

    /** A vertical page of lines, for the detail or a tab. */
    private static JPanel column() {
        JPanel w = new JPanel();
        w.setLayout(new BoxLayout(w, BoxLayout.Y_AXIS));
        w.setBorder(BorderFactory.createEmptyBorder(6, 10, 10, 10));
        return w;
    }

    private static JTextArea note(String text) {
        JTextArea t = Ui.wrapped(text);
        java.awt.Color c = javax.swing.UIManager.getColor("Label.disabledForeground");
        t.setForeground(c != null ? c : java.awt.Color.GRAY);
        t.setAlignmentX(LEFT_ALIGNMENT);
        return t;
    }

    private static JTextArea coloured(String text, String state) {
        JTextArea t = Ui.wrapped(text);
        t.setForeground(Ui.colourOf(state));
        t.setAlignmentX(LEFT_ALIGNMENT);
        return t;
    }

    private static JLabel section(String text) {
        JLabel h = Ui.heading(text);
        h.setBorder(BorderFactory.createEmptyBorder(10, 0, 2, 0));
        h.setAlignmentX(LEFT_ALIGNMENT);
        return h;
    }

    private void setDetail(JComponent w) {
        int pos = detail.getVerticalScrollBar().getValue();
        JPanel holder = new StatusPage.WidthTracking(new BorderLayout());
        holder.add(w, w instanceof JTabbedPane ? BorderLayout.CENTER : BorderLayout.NORTH);
        detail.setViewportView(holder);
        javax.swing.SwingUtilities.invokeLater(() -> detail.getVerticalScrollBar().setValue(pos));
    }

    private void showDetail() {
        Node n = selected();
        if (n == null || n.kind() == Kind.WAITING) {
            JPanel w = column();
            w.add(left(Ui.muted(L("Choose a machine, a pool or a dataset.", "Elige una máquina, un pool o un dataset."))));
            setDetail(w);
            return;
        }
        switch (n.kind()) {
            case MACHINE -> showMachine(n.machine());
            case POOL -> showPool(n.machine(), n.name());
            case DATASET -> showDataset(n.machine(), n.name());
            default -> { }
        }
    }

    /** A word for each of {@code probe}'s fixed codes. */
    static String probeWord(String what, String code) {
        if (code.equals("ok")) {
            return L("● fine", "● bien");
        }
        switch (what) {
            case "sudo" -> {
                switch (code) {
                    case "not_used":
                        return L("not used", "no se usa");
                    case "wrong_password":
                        return L("✕ does not accept the stored password", "✕ no acepta la contraseña guardada");
                    case "needs_password":
                        return L("✕ asks for a password the connection does not have", "✕ pide una contraseña que la conexión no tiene");
                    case "smartcard_pin":
                        return L("✕ asks for the PIN of a connected smart card", "✕ pide el PIN de una tarjeta inteligente conectada");
                    default:
                        break;
                }
            }
            case "daemon" -> {
                switch (code) {
                    case "outdated":
                        return L("▲ outdated", "▲ anticuado");
                    case "latest":
                        return L("● the newest there is to install", "● el más nuevo que hay para instalar");
                    case "unreachable":
                        return L("✕ cannot be reached", "✕ no se llega a él");
                    default:
                        break;
                }
            }
            case "openzfs" -> {
                switch (code) {
                    case "too_old":
                        return L("✕ too old", "✕ demasiado antiguo");
                    case "missing":
                        return L("✕ not installed", "✕ no está instalado");
                    case "not_loaded":
                        return L("✕ not loaded", "✕ no está cargado");
                    default:
                        break;
                }
            }
            default -> { }
        }
        return code;
    }

    private StatusModel.MachineView viewOf(String id) {
        for (StatusModel.MachineView m : store.machines()) {
            if (m.id.equals(id)) {
                return m;
            }
        }
        return null;
    }

    private void showMachine(String id) {
        StatusModel.MachineView mv = viewOf(id);
        JPanel w = column();
        JLabel title = Ui.heading(mv != null ? mv.name : id);
        title.setFont(title.getFont().deriveFont(title.getFont().getSize2D() * 1.2f));
        w.add(left(title));
        if (mv != null && !mv.connected) {
            w.add(note(L("Disconnected: the interface does not ask it anything until it is connected again.",
                    "Desconectada: la interfaz no le pregunta nada hasta que se vuelva a conectar.")));
        }
        Map<String, Object> p = probe.get(id);
        if (p == null) {
            w.add(left(Ui.muted(L("Checking the machine…", "Comprobando la máquina…"))));
            // Disconnected or not: checking is how one decides to connect it.
            if (inFlight.add("probe " + id)) store.forMachine(id).request("probe " + url(id, ""), r -> {
                inFlight.remove("probe " + id);
                probe.put(id, Json.firstObject(r.out()));
                showDetail();
            });
        } else {
            Ui.Form form = new Ui.Form();
            if (!Json.bool(p, "reachable", false)) {
                form.row(L("Answers", "Responde"), new JLabel(L("✕ no: %1", "✕ no: %1", Json.str(p, "reason"))));
            } else {
                form.row(L("System", "Sistema"), new JLabel(Json.str(p, "os_detail")));
                form.row("sudo", new JLabel(probeWord("sudo", Json.str(p, "sudo"))));
                form.row("OpenZFS", new JLabel(Json.str(p, "openzfs_version") + "  " + probeWord("openzfs", Json.str(p, "openzfs"))));
                form.row(L("Daemon", "Daemon"), new JLabel(Json.str(p, "daemon_version") + "  " + probeWord("daemon", Json.str(p, "daemon"))));
            }
            w.add(left(form));
            for (Map<String, Object> wv : Json.objects(p, "warnings")) {
                boolean err = Json.str(wv, "level").equals("error");
                w.add(coloured((err ? "✕ " : "▲ ") + Json.str(wv, "text"), err ? "error" : "warning"));
            }
        }
        w.add(Box.createVerticalStrut(8));

        List<JButton> buttons = new ArrayList<>();
        // sudo asks for a password the connection lacks, or rejects the stored one: the way to
        // give it, first. The CLI asks for it as a secret; it never goes on a line.
        String sudo = p == null ? "" : Json.str(p, "sudo");
        boolean sudoFails = sudo.equals("needs_password") || sudo.equals("wrong_password");
        if (sudoFails) {
            buttons.add(button(L("Give the sudo password…", "Dar la contraseña de sudo…"),
                    () -> act("edit " + url(id, "") + " --sudo", L("sudo password stored.", "Contraseña de sudo guardada."),
                            true, () -> {
                                probe.remove(id);
                                showDetail();
                            })));
        }
        // Every button names what it acts on, and only the ones that make sense now are shown.
        buttons.add(button(L("Check this machine again", "Volver a comprobar esta máquina"), () -> {
            probe.remove(id);
            stranded.remove(id);
            showDetail();
        }));
        boolean connected = mv == null || mv.connected;
        JButton conn = button(connected ? L("Disconnect this machine", "Desconectar esta máquina")
                                        : L("Connect this machine", "Conectar esta máquina"),
                () -> act((connected ? "disconnect " : "connect ") + url(id, ""),
                        connected ? L("Disconnected.", "Desconectada.") : L("Connected.", "Conectada."), true, null));
        if (connected) {
            conn.setToolTipText(L("ZFSMgr stops asking it anything until it is connected again. Nothing changes on the machine.",
                    "ZFSMgr deja de preguntarle nada hasta que se vuelva a conectar. En la máquina no cambia nada."));
        }
        buttons.add(conn);
        // The daemon: only when `probe` says it is missing or behind. Unreachable because sudo
        // fails is not «missing»: the sudo button above is the way.
        if (p != null) {
            String daemon = Json.str(p, "daemon");
            String text = daemon.equals("unreachable") && !sudoFails ? L("Install its daemon…", "Instalar su daemon…")
                    : daemon.equals("outdated") ? L("Update its daemon…", "Actualizar su daemon…") : "";
            if (!text.isEmpty()) {
                // Checked again when the installation has FINISHED.
                buttons.add(button(text, () -> act("install-daemon --on " + url(id, ""),
                        L("Daemon installed.", "Daemon instalado."), false, () -> {
                            probe.remove(id);
                            showDetail();
                        })));
            }
        }
        if (advanced && connected) {
            buttons.add(button(L("Create a pool on this machine…", "Crear un pool en esta máquina…"), () -> host.createPool(id)));
            buttons.add(button(L("Import a pool on this machine…", "Importar un pool en esta máquina…"), () -> importPool(id)));
            // Datasets an interrupted action left on a temporary mountpoint: asked once, and the
            // button only when there are some.
            Integer left = stranded.get(id);
            if (left == null) {
                if (inFlight.add("mounts " + id)) store.forMachine(id).request("repair-mounts --on " + url(id, ""), r -> {
                    inFlight.remove("mounts " + id);
                    stranded.put(id, Json.objects(Json.firstObject(r.out()), "stranded").size());
                    showDetail();
                });
            } else if (left > 0) {
                JButton mounts = button(L("%1 dataset(s) left on a temporary mountpoint…", "%1 dataset(s) en un montaje temporal…", left),
                        () -> strandedMounts(id));
                mounts.setToolTipText(L("An interrupted action left them mounted somewhere else.",
                        "Una acción interrumpida los dejó montados en otro sitio."));
                buttons.add(mounts);
            }
            buttons.add(button(L("See what its daemon did", "Ver lo que ha hecho su daemon"),
                    () -> showOutput(id, "log --lines 500 --on " + url(id, ""),
                            L("What the daemon of %1 did", "Qué ha hecho el daemon de %1", id))));
            List<String> others = new ArrayList<>();
            for (StatusModel.MachineView m : store.machines()) {
                if (!m.id.equals(id) && m.connected) {
                    others.add(m.id);
                }
            }
            // Both need another machine to go to: without one they are not shown.
            if (!others.isEmpty()) {
                JButton trust = button(L("Copy the daemons it knows to another machine…", "Copiar a otra máquina los daemons que conoce…"),
                        () -> toAnother(id, others, L("Trust store", "Trust-store"),
                                L("The client opened on that machine will talk to the same daemons. It carries private keys.",
                                  "El cliente que se abra en esa máquina hablará con los mismos daemons. Lleva claves privadas."),
                                "export-trust", L("Trust store copied to %1.", "Trust-store copiado a %1.")));
                trust.setToolTipText(L("Its trust store: the certificates of the daemons this machine talks to.",
                        "Su trust-store: los certificados de los daemons con los que habla esta máquina."));
                buttons.add(trust);
                buttons.add(button(L("Let it log into another machine without a password…", "Que entre en otra máquina sin contraseña…"),
                        () -> toAnother(id, others, L("SSH key", "Clave SSH"),
                                L("Its SSH key is authorized on that one: after this, this machine logs into it without a password.",
                                  "Su clave SSH se autoriza en aquella: después, esta máquina entra en ella sin contraseña."),
                                "authorize-key", L("Key authorized on %1.", "Clave autorizada en %1."))));
            }
        }
        // Its address can change —moved to another network, or reached through Tailscale now—.
        JButton where = button(L("Change its address…", "Cambiar su dirección…"), () -> changeAddress(id));
        where.setToolTipText(L("Where ZFSMgr finds it: host, SSH port and user. Nothing on the machine is touched.",
                "Dónde la encuentra ZFSMgr: máquina (host), puerto SSH y usuario. En la máquina no se toca nada."));
        buttons.add(where);
        JButton remove = button(L("Remove this machine from ZFSMgr…", "Quitar esta máquina de ZFSMgr…"),
                () -> act("destroy " + url(id, ""), L("Machine removed.", "Máquina quitada."), true, null));
        remove.setToolTipText(L("ZFSMgr forgets it: its connection and stored password. Nothing on the machine is touched.",
                "ZFSMgr la olvida: su conexión y su contraseña guardada. En la máquina no se toca nada."));
        buttons.add(remove);
        // In rows of two: in one row they pushed the page wider than the window.
        w.add(Ui.buttonGrid(buttons, 2));
        setDetail(w);
    }

    private static JButton button(String text, Runnable r) {
        JButton b = new JButton(text);
        b.addActionListener(e -> r.run());
        return b;
    }

    private void toAnother(String id, List<String> others, String title, String text, String verb, String done) {
        Object other = JOptionPane.showInputDialog(this, Ui.paragraph(text), title, JOptionPane.QUESTION_MESSAGE, null,
                others.toArray(), others.get(0));
        if (other != null) {
            act(verb + " " + Ui.quoted(other.toString()) + " --on " + url(id, ""), done.replace("%1", other.toString()), false, null);
        }
    }

    // The datasets left on a temporary mountpoint: where they are and where they belong, and the
    // way to put them back right there.
    private void strandedMounts(String id) {
        store.forMachine(id).request("repair-mounts --on " + url(id, ""), "text", false, r -> {
            JTextArea t = new JTextArea(r.out().trim().isEmpty() ? r.err().trim() : r.out().trim());
            t.setEditable(false);
            t.setFont(new java.awt.Font(java.awt.Font.MONOSPACED, java.awt.Font.PLAIN, t.getFont().getSize()));
            JScrollPane sp = new JScrollPane(t);
            sp.setPreferredSize(new Dimension(640, 260));
            JPanel p = new JPanel(new BorderLayout(0, 8));
            p.add(Ui.paragraph(L("An interrupted action left these datasets mounted somewhere else. Putting them back mounts each one where it belongs; nothing else is touched.",
                    "Una acción interrumpida dejó estos datasets montados en otro sitio. Devolverlos monta cada uno donde le corresponde; no se toca nada más.")),
                    BorderLayout.NORTH);
            p.add(sp, BorderLayout.CENTER);
            Object[] options = {L("Put them back", "Devolverlos a su sitio"), L("Close", "Cerrar")};
            if (JOptionPane.showOptionDialog(this, p, L("Left on a temporary mountpoint", "En un montaje temporal"),
                    JOptionPane.DEFAULT_OPTION, JOptionPane.PLAIN_MESSAGE, null, options, options[1]) == 0) {
                act("repair-mounts --apply --on " + url(id, ""), L("Put back where they belong.", "Devueltos a su sitio."), true, () -> {
                    stranded.remove(id);
                    showDetail();
                });
            }
        });
    }

    private void showPool(String machine, String pool) {
        String key = machine + "/" + pool;
        JPanel w = column();
        w.add(left(Ui.muted(url(machine, pool))));
        JLabel title = Ui.heading(pool);
        title.setFont(title.getFont().deriveFont(title.getFont().getSize2D() * 1.2f));
        w.add(left(title));
        Map<String, Object> p = Map.of();
        for (Map<String, Object> x : pools.getOrDefault(machine, List.of())) {
            if (Json.str(x, "name").equals(pool)) {
                p = x;
            }
        }
        String state = "ok";
        StatusModel.MachineView mv = viewOf(machine);
        if (mv != null) {
            for (Map<String, Object> x : Json.objects(mv.overview, "pools")) {
                if (Json.str(x, "name").equals(pool)) {
                    state = Json.str(x, "status");
                }
            }
        }
        w.add(Ui.row(Ui.stateLabel(state), Ui.muted(L("%1 · %2 in use · %3 free", "%1 · %2 ocupado · quedan %3",
                Json.str(p, "size"), Json.str(p, "capacity"), Json.str(p, "free")))));

        Map<String, Object> st = status.get(key);
        if (st == null) {
            w.add(left(Ui.muted(L("Asking for its disks…", "Preguntando por sus discos…"))));
            if (inFlight.add("status " + key)) store.forMachine(machine).request("status " + url(machine, pool), r -> {
                inFlight.remove("status " + key);
                status.put(key, Json.firstObject(r.out()));
                showDetail();
            });
        } else {
            // zpool's own explanation and advice, when there is something to say.
            String[][] says = {{"status", L("What happens", "Qué pasa")}, {"action", L("What to do", "Qué hacer")}};
            for (String[] s : says) {
                String text = Json.str(st, s[0]);
                if (!text.isEmpty()) {
                    w.add(state.equals("ok") ? left(Ui.wrapped(s[1] + ": " + text)) : coloured(s[1] + ": " + text, state));
                }
            }
            List<Object[]> rows = new ArrayList<>();
            List<String> looks = new ArrayList<>();
            for (Map<String, Object> d : Json.objects(st, "devices")) {
                String name = "  ".repeat((int) Json.lng(d, "depth", 0)) + Json.str(d, "name");
                String nt = Json.str(d, "note");
                if (!nt.isEmpty()) {
                    name += "  (" + nt + ")";
                }
                String s = Json.str(d, "state");
                looks.add(s.equals("ONLINE") ? "ok" : s.isEmpty() ? "" : s.equals("DEGRADED") ? "warning" : "error");
                String errs = Json.text(d, "read").isEmpty() ? ""
                        : Json.text(d, "read") + " / " + Json.text(d, "write") + " / " + Json.text(d, "cksum");
                rows.add(new Object[] {name, s, errs});
            }
            JTable t = Ui.table(new String[] {L("Disk", "Disco"), L("State", "Estado"),
                    L("Errors (read/write/checksum)", "Errores (lectura/escritura/suma)")}, rows);
            t.getColumnModel().getColumn(1).setCellRenderer(new DefaultTableCellRenderer() {
                @Override
                public Component getTableCellRendererComponent(JTable tb, Object v, boolean sel, boolean f, int r, int c) {
                    Component comp = super.getTableCellRendererComponent(tb, v, sel, f, r, c);
                    if (!sel) {
                        comp.setForeground(looks.get(r).isEmpty() ? tb.getForeground() : Ui.colourOf(looks.get(r)));
                    }
                    return comp;
                }
            });
            w.add(Ui.tableScroll(t, 12));
            String scan = Json.str(st, "scan");
            w.add(left(Ui.muted(L("Last check: %1", "Última comprobación: %1", scan.isEmpty() ? L("never", "nunca") : scan))));
            String errors = Json.str(st, "errors");
            if (!errors.isEmpty() && !errors.startsWith("No known")) {
                w.add(coloured(errors, "error"));
            }
        }
        w.add(Box.createVerticalStrut(8));

        Consumer<String[]> again = lineDone -> {
            status.remove(key);
            act(lineDone[0], lineDone[1], false, null);
        };
        List<JButton> actions = new ArrayList<>();
        // With --on: the pool verbs do not take the URL as a loose argument.
        actions.add(button(L("Check the data (scrub)", "Comprobar los datos (scrub)"), () -> again.accept(new String[] {
            "scrub --on " + url(machine, pool), L("Checking: it reads the whole pool and fixes what it can.",
                    "Comprobando: lee todo el pool y corrige lo que pueda.")})));
        if (advanced) {
            String[][] more = {
                {"trim", L("Free blocks (trim)", "Liberar bloques (trim)"), L("Trimming: only useful on SSDs.", "Liberando: solo sirve en SSD.")},
                {"clear", L("Reset errors…", "Poner a cero los errores…"), L("Errors reset.", "Errores a cero.")},
                {"upgrade", L("Upgrade format…", "Actualizar formato…"), L("Pool upgraded.", "Pool actualizado.")},
                {"reguid", L("New identifier…", "Identificador nuevo…"), L("Identifier changed.", "Identificador cambiado.")},
                {"export", L("Export…", "Exportar…"), L("Exported: its disks can go to another machine.", "Exportado: sus discos pueden ir a otra máquina.")},
                {"export -f", L("Export by force…", "Exportar a la fuerza…"), L("Exported: its disks can go to another machine.", "Exportado: sus discos pueden ir a otra máquina.")},
                {"scrub -p", L("Pause the check", "Pausar la comprobación"), L("Check paused: checking again resumes it.", "Comprobación en pausa: comprobar de nuevo la reanuda.")},
                {"scrub -s", L("Stop the check", "Parar la comprobación"), L("Check stopped.", "Comprobación parada.")},
                {"flush", L("Write pending data (sync)", "Escribir lo pendiente (sync)"), L("Pending writes are on disk.", "Lo pendiente ya está en disco.")},
                {"initialize", L("Initialize free space…", "Inicializar el espacio libre…"), L("Initializing: it writes over the unused space.", "Inicializando: escribe sobre el espacio sin usar.")},
                {"destroy", L("Destroy the pool…", "Destruir el pool…"), L("Pool destroyed.", "Pool destruido.")}};
            for (String[] m : more) {
                // destroy takes the pool as its target; the maintenance verbs only with --on.
                String line = m[0].equals("destroy") ? "destroy " + url(machine, pool) : m[0] + " --on " + url(machine, pool);
                boolean leaves = m[0].startsWith("export") || m[0].equals("destroy");
                actions.add(button(m[1], () -> {
                    if (leaves) {
                        forgetPools(machine);
                    }
                    again.accept(new String[] {line, m[2]});
                }));
            }
            actions.add(button(L("History", "Historial"), () -> showOutput(machine, "history --on " + url(machine, pool),
                    L("What was done to %1", "Qué se le ha hecho a %1", pool))));
        }
        w.add(Ui.buttonGrid(actions, 3));

        // A pool is also a dataset —its root—: after its own tab, the same tabs a dataset has.
        // They need its datasets listed; asked here without opening the branch of the tree.
        if (!entries.containsKey(key) && entriesAsked.add(key)) {
            store.forMachine(machine).request("ls -r " + url(machine, pool), r -> {
                entries.put(key, Json.objects(Json.firstObject(r.out()), "entries"));
                showDetail();
            });
        }
        JTabbedPane tabs = new JTabbedPane();
        JPanel poolPage = new JPanel(new BorderLayout());
        poolPage.add(w, BorderLayout.NORTH);
        tabs.addTab(L("Pool", "Pool"), poolPage);
        addDatasetTabs(tabs, machine, pool);
        tabs.setSelectedIndex(Math.max(0, Math.min(poolTab, tabs.getTabCount() - 1)));
        tabs.addChangeListener(e -> poolTab = tabs.getSelectedIndex());
        setDetail(tabs);
    }

    // The machine's pools are listed again on next opening: one left or one arrived.
    private void forgetPools(String machine) {
        pools.remove(machine);
        DefaultMutableTreeNode mi = findItem(machine, "");
        if (mi != null) {
            tree.collapsePath(new TreePath(mi.getPath()));
            mi.removeAllChildren();
            mi.add(waiting());
            model.nodeStructureChanged(mi);
        }
    }

    private void importPool(String machine) {
        store.forMachine(machine).request("import " + url(machine, ""), r -> {
            List<String> names = new ArrayList<>();
            for (Map<String, Object> p : Json.objects(Json.firstObject(r.out()), "importable_pools")) {
                names.add(Json.str(p, "pool", Json.str(p, "name")));
            }
            if (names.isEmpty()) {
                host.say(L("No pool to import on %1.", "No hay ningún pool que importar en %1.", machine));
                return;
            }
            JComboBox<String> which = new JComboBox<>(names.toArray(new String[0]));
            JTextField as = new JTextField(20);
            as.putClientProperty("JTextField.placeholderText", L("optional: import it under another name", "opcional: importarlo con otro nombre"));
            JCheckBox force = new JCheckBox(L("Even if another machine seems to be using it (-f)", "Aunque otra máquina parezca estar usándolo (-f)"));
            Ui.Form form = new Ui.Form();
            form.row(L("Pool", "Pool"), which).row(L("New name", "Nombre nuevo"), as).row("", force);
            if (JOptionPane.showConfirmDialog(this, form, L("Import a pool", "Importar un pool"), JOptionPane.OK_CANCEL_OPTION,
                    JOptionPane.PLAIN_MESSAGE) != JOptionPane.OK_OPTION) {
                return;
            }
            String name = String.valueOf(which.getSelectedItem());
            String line = "import " + Ui.quoted(name);
            if (!as.getText().trim().isEmpty()) {
                line += " --as " + Ui.quoted(as.getText().trim());
            }
            if (force.isSelected()) {
                line += " -f";
            }
            forgetPools(machine);
            act(line + " --on " + url(machine, ""), L("Pool %1 imported.", "Pool %1 importado.", name), false, null);
        });
    }

    private void showDataset(String machine, String dataset) {
        // The chosen tab is kept: the detail is rebuilt every time an answer arrives.
        JTabbedPane tabs = new JTabbedPane();
        addDatasetTabs(tabs, machine, dataset);
        tabs.setSelectedIndex(Math.max(0, Math.min(datasetTab, tabs.getTabCount() - 1)));
        tabs.addChangeListener(e -> datasetTab = tabs.getSelectedIndex());
        setDetail(tabs);
    }

    /** A dataset's tabs: the summary and the files for everyone; the rest in advanced mode. */
    private void addDatasetTabs(JTabbedPane tabs, String machine, String dataset) {
        // Each tab at the top of its space: a text area in a box layout takes all the height left.
        Function<JComponent, JComponent> top = c -> {
            JPanel p = new JPanel(new BorderLayout());
            p.add(c, BorderLayout.NORTH);
            return p;
        };
        tabs.addTab(L("Summary", "Resumen"), top.apply(datasetSummary(machine, dataset)));
        tabs.addTab(L("Files", "Ficheros"), top.apply(filesTab(machine, dataset)));
        if (advanced) {
            tabs.addTab(L("Properties", "Propiedades"), top.apply(propertiesTab(machine, dataset)));
            tabs.addTab(L("Manage", "Gestionar"), top.apply(manageTab(machine, dataset)));
            tabs.addTab(L("Send", "Enviar"), top.apply(sendTab(machine, dataset)));
            tabs.addTab(L("Organize", "Organizar"), top.apply(organizeTab(machine, dataset)));
        }
    }

    private static String when(long t) {
        return t > 0 ? LocalDateTime.ofInstant(Instant.ofEpochSecond(t), ZoneId.systemDefault())
                .format(DateTimeFormatter.ofLocalizedDateTime(FormatStyle.SHORT)) : "";
    }

    private JComponent datasetSummary(String machine, String dataset) {
        Map<String, Object> ds = Map.of();
        List<Map<String, Object>> snaps = new ArrayList<>();
        for (Map<String, Object> e : entries.getOrDefault(machine + "/" + poolOf(dataset), List.of())) {
            String name = Json.str(e, "name");
            if (name.equals(dataset)) {
                ds = e;
            } else if (name.startsWith(dataset + "@")) {
                snaps.add(e);
            }
        }
        snaps.sort((a, b) -> Long.compare(Json.lng(b, "creation", 0), Json.lng(a, "creation", 0)));

        JPanel w = column();
        w.add(left(Ui.muted(url(machine, dataset))));
        JLabel title = Ui.heading(dataset.substring(dataset.lastIndexOf('/') + 1));
        title.setFont(title.getFont().deriveFont(title.getFont().getSize2D() * 1.2f));
        w.add(left(title));
        List<Component> summaryButtons = new ArrayList<>();
        summaryButtons.add(button(L("Snapshot now", "Snapshot ahora"), () -> {
            String name = "manual-" + LocalDateTime.now().format(DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss"));
            act("create @" + name + " --on " + url(machine, dataset), L("Snapshot %1 taken.", "Hecho el snapshot %1.", name), true, null);
        }));
        summaryButtons.add(button(L("Copy…", "Copiar…"), () -> host.newCopy(machine, dataset)));
        // The CLI says what is lost and asks for the name: the dialog shows it. Not for a pool's
        // root: that is destroying the pool, which its own tab offers as such.
        if (dataset.contains("/")) {
            summaryButtons.add(button(L("Destroy…", "Destruir…"),
                    () -> act("destroy -r " + url(machine, dataset), L("Destroyed.", "Destruido."), true, null)));
        }
        summaryButtons.add(button(L("New dataset inside…", "Dataset nuevo dentro…"), () -> newDataset(machine, dataset)));
        w.add(Ui.row(summaryButtons.toArray(new Component[0])));

        Ui.Form form = new Ui.Form();
        form.row(L("Uses", "Ocupa"), new JLabel(StatusModel.bytesText(Json.lng(ds, "used", 0))));
        form.row(L("Mounted at", "Montado en"), new JLabel(Json.str(ds, "mounted").equals("yes") ? Json.str(ds, "mountpoint")
                : L("not mounted", "sin montar")));
        String enc = Json.str(ds, "encryption");
        form.row(L("Encrypted", "Cifrado"), new JLabel(enc.isEmpty() || enc.equals("off") ? L("no", "no") : enc));
        form.row(L("Snapshots", "Snapshots"), new JLabel(String.valueOf(snaps.size())));
        List<String> copies = new ArrayList<>();
        String here = url(machine, dataset);
        Map<String, Object> keep = Map.of();
        boolean has = false;
        boolean pulled = false;
        for (StatusModel.ScheduleRow r : StatusModel.scheduleRows(store.machines())) {
            boolean mine = r.machineId().equals(machine) && r.dataset().equals(dataset);
            if (r.kind().equals("pull") && r.peer().equals(here)) {
                copies.add(L("%1 pulls it into %2", "la trae %1 a %2", r.machine(), r.dataset()));
            } else if (r.kind().equals("push") && mine) {
                copies.add(L("sent to %1", "se envía a %1", r.peer()));
            } else if (r.kind().equals("pull") && mine) {
                copies.add(L("it is a copy pulled from %1", "es una copia traída de %1", r.peer()));
            }
            if (mine) {
                if (r.kind().equals("pull")) {
                    pulled = true;
                } else {
                    keep = r.keep();
                    has = true;
                }
            }
        }
        JTextArea copiesText = Ui.wrapped(copies.isEmpty() ? L("none", "ninguna") : String.join("\n", copies));
        form.row(L("Copies", "Copias"), copiesText);
        // Automatic snapshots: its own schedule (alone, or the one a pushed copy sends). A pulled
        // copy's snapshots are the origin's: they are changed in Copies.
        if (!pulled) {
            final Map<String, Object> k = keep;
            JPanel row = new JPanel(new BorderLayout(8, 0));
            row.add(new JLabel(has ? StatusModel.keepText(keep) : L("none", "ninguno")), BorderLayout.CENTER);
            row.add(button(has ? L("Change…", "Cambiar…") : L("Schedule…", "Programar…"), () -> {
                String flags = Dialogs.askKeep(this, L("Automatic snapshots", "Snapshots automáticos"),
                        L("How many of each are kept; the older ones are deleted. All at 0: none are taken.",
                          "Cuántos de cada se guardan; los más viejos se borran. Todos a 0: no se hace ninguno."), k);
                if (flags != null) {
                    act("schedule " + flags + " --on " + url(machine, dataset), L("Schedule changed.", "Programación cambiada."), false, null);
                }
            }), BorderLayout.EAST);
            form.row(L("Automatic snapshots", "Snapshots automáticos"), row);
        }
        w.add(left(form));

        w.add(section(L("Snapshots", "Snapshots")));
        if (snaps.isEmpty()) {
            w.add(left(Ui.muted(L("None.", "Ninguno."))));
            return w;
        }
        List<Object[]> rows = new ArrayList<>();
        for (Map<String, Object> s : snaps) {
            String full = Json.str(s, "name");
            rows.add(new Object[] {full.substring(full.indexOf('@') + 1), when(Json.lng(s, "creation", 0)),
                                   StatusModel.bytesText(Json.lng(s, "used", 0))});
        }
        JTable table = Ui.table(new String[] {L("Snapshot", "Snapshot"), L("Taken", "Creado"), L("Uses", "Ocupa")}, rows);
        w.add(Ui.tableScroll(table, 10));
        Supplier<String> chosen = () -> {
            int r = table.getSelectedRow();
            return r < 0 ? "" : dataset + "@" + table.getValueAt(r, 0);
        };
        JButton back = button(L("Go back to this one…", "Volver a este…"), () -> {
            String s = chosen.get();
            if (!s.isEmpty()) {
                // -r: the later snapshots go too; the CLI lists them and asks for the name.
                act("rollback -r " + url(machine, s), L("Back to %1.", "Vuelto a %1.", s), true, null);
            }
        });
        JButton del = button(L("Delete…", "Borrar…"), () -> {
            String s = chosen.get();
            if (!s.isEmpty()) {
                act("destroy " + url(machine, s), L("Snapshot deleted.", "Snapshot borrado."), true, null);
            }
        });
        back.setEnabled(false);
        del.setEnabled(false);
        table.getSelectionModel().addListSelectionListener(e -> {
            boolean any = table.getSelectedRow() >= 0;
            back.setEnabled(any);
            del.setEnabled(any);
        });
        w.add(Ui.row(back, del));
        if (advanced) {
            w.add(Ui.row(snapshotExtras(machine, dataset, chosen).toArray(new Component[0])));
        }
        return w;
    }

    private JComponent filesTab(String machine, String dataset) {
        JPanel w = column();
        String key = machine + "/" + dataset;
        String rel = filesPath.getOrDefault(key, "");
        JButton up = button(L("Up", "Subir"), () -> {
            filesPath.put(key, rel.contains("/") ? rel.substring(0, rel.lastIndexOf('/')) : "");
            showDetail();
        });
        up.setEnabled(!rel.isEmpty());
        w.add(Ui.row(up, Ui.muted("/" + rel)));
        String listKey = key + "#" + rel;
        List<Map<String, Object>> es = files.get(listKey);
        if (es == null) {
            w.add(left(Ui.muted(L("Reading the directory…", "Leyendo el directorio…"))));
            if (inFlight.add("files " + listKey)) store.forMachine(machine).request("ls " + url(machine, dataset) + "#content" + (rel.isEmpty() ? "" : "/" + rel), r -> {
                inFlight.remove("files " + listKey);
                List<Map<String, Object>> got = new ArrayList<>(Json.objects(Json.firstObject(r.out()), "entries"));
                if (!r.ok() && got.isEmpty()) {
                    got.add(Map.of("error", r.err().trim()));
                }
                files.put(listKey, got);
                showDetail();
            });
            return w;
        }
        if (es.size() == 1 && es.get(0).containsKey("error")) {
            w.add(left(Ui.wrapped(Json.str(es.get(0), "error"))));
            return w;
        }
        List<Object[]> rows = new ArrayList<>();
        List<String> dirs = new ArrayList<>();
        for (Map<String, Object> e : es) {
            boolean dir = Json.str(e, "type").equals("directory");
            rows.add(new Object[] {(dir ? "▸ " : "  ") + Json.str(e, "name"), dir ? "" : StatusModel.bytesText(Json.lng(e, "size", 0)),
                                   Json.text(e, "modified")});
            dirs.add(dir ? Json.str(e, "name") : "");
        }
        JTable table = Ui.table(new String[] {L("Name", "Nombre"), L("Size", "Tamaño"), L("Modified", "Modificado")}, rows);
        // Into a directory with a double click.
        table.addMouseListener(new java.awt.event.MouseAdapter() {
            @Override
            public void mouseClicked(java.awt.event.MouseEvent e) {
                int r = table.rowAtPoint(e.getPoint());
                if (e.getClickCount() == 2 && r >= 0 && !dirs.get(r).isEmpty()) {
                    filesPath.put(key, rel.isEmpty() ? dirs.get(r) : rel + "/" + dirs.get(r));
                    showDetail();
                }
            }
        });
        w.add(Ui.tableScroll(table, 18));
        w.add(left(Ui.muted(L("Double click a directory to go into it.", "Doble clic en un directorio para entrar."))));
        return w;
    }

    private JComponent propertiesTab(String machine, String dataset) {
        JPanel w = column();
        w.add(note(L("Every ZFS property with where it comes from: set here (local), inherited from a parent, or the default. Double click a value to change it.",
                "Todas las propiedades de ZFS con su origen: puesta aquí (local), heredada de un padre o la de por defecto. Doble clic en un valor para cambiarlo.")));
        String key = machine + "/" + dataset;
        if (!ensureProps(machine, dataset)) {
            w.add(left(Ui.muted(L("Reading its properties…", "Leyendo sus propiedades…"))));
            return w;
        }
        List<Object[]> rows = new ArrayList<>();
        for (Map<String, Object> p : props.get(key)) {
            rows.add(new Object[] {Json.str(p, "property"), Json.text(p, "value"), Json.str(p, "source")});
        }
        JTable table = Ui.table(new String[] {L("Property", "Propiedad"), L("Value", "Valor"), L("From", "Origen")}, rows);
        // Changing one: a value sets it here; empty puts it back to inherited. The CLI refuses what
        // cannot be changed (a computed property) and says why.
        table.addMouseListener(new java.awt.event.MouseAdapter() {
            @Override
            public void mouseClicked(java.awt.event.MouseEvent e) {
                int r = table.rowAtPoint(e.getPoint());
                if (e.getClickCount() != 2 || r < 0) {
                    return;
                }
                String prop = String.valueOf(table.getValueAt(r, 0));
                Object v = JOptionPane.showInputDialog(MachinesPage.this,
                        L("New value for %1 (empty: inherit it):", "Valor nuevo de %1 (vacío: heredarlo):", prop), prop,
                        JOptionPane.PLAIN_MESSAGE, null, null, table.getValueAt(r, 1));
                if (v == null) {
                    return;
                }
                props.remove(key);
                String on = " --on " + url(machine, dataset);
                String val = v.toString().trim();
                act(val.isEmpty() ? "inherit " + prop + on : "set " + prop + "=" + Ui.quoted(val) + on,
                        L("%1 changed.", "%1 cambiada.", prop), false, null);
            }
        });
        JScrollPane sp = new JScrollPane(table);
        sp.setPreferredSize(new Dimension(400, 460));
        sp.setAlignmentX(LEFT_ALIGNMENT);
        w.add(sp);
        return w;
    }

    private String propOf(String machine, String dataset, String name) {
        for (Map<String, Object> p : props.getOrDefault(machine + "/" + dataset, List.of())) {
            if (Json.str(p, "property").equals(name)) {
                return Json.text(p, "value");
            }
        }
        return "";
    }

    /**
     * The dataset's properties: true when they are here; false asks for them (once) and the
     * detail is drawn again when they arrive.
     */
    private boolean ensureProps(String machine, String dataset) {
        String key = machine + "/" + dataset;
        if (props.containsKey(key)) {
            return true;
        }
        if (propsAsked.add(key)) {
            store.forMachine(machine).request("ls " + url(machine, dataset) + "#properties", r -> {
                propsAsked.remove(key);
                props.put(key, Json.objects(Json.firstObject(r.out()), "properties"));
                showDetail();
            });
        }
        return false;
    }

    /** A check-box column first, the rest as given: what the organize tables are made of. */
    private static DefaultTableModel tickTable(String[] heads, int editableColumn) {
        return new DefaultTableModel(heads, 0) {
            @Override
            public Class<?> getColumnClass(int c) {
                return c == 0 ? Boolean.class : String.class;
            }

            @Override
            public boolean isCellEditable(int r, int c) {
                return c == 0 || c == editableColumn;
            }
        };
    }

    private JComponent organizeTab(String machine, String dataset) {
        JPanel w = column();
        String on = " --on " + url(machine, dataset);
        String key = machine + "/" + dataset;
        String running = L("Running as a job: follow it in Jobs.", "En marcha como trabajo: síguelo en Trabajos.");

        // ── Directories into datasets (breakdown).
        w.add(section(L("Turn directories into datasets", "Convertir carpetas en datasets")));
        w.add(note(L("Each directory chosen becomes a child dataset in the same place: from outside it looks the same, but it can have its own snapshots, quota and copies. Every file is moved once and the originals are deleted at the end, when all copies are verified. It runs as a job.",
                "Cada carpeta marcada pasa a ser un dataset hijo en el mismo sitio: desde fuera se ve igual, pero puede tener sus propios snapshots, cuota y copias. Cada fichero se mueve una sola vez y los originales se borran al final, cuando todas las copias están verificadas. Se hace como trabajo.")));
        List<Map<String, Object>> dirs = breakdown.get(key);
        if (dirs == null) {
            w.add(left(Ui.muted(L("Measuring its directories…", "Midiendo sus carpetas…"))));
            if (inFlight.add("breakdown " + key)) store.forMachine(machine).request("breakdown --list" + on, r -> {
                inFlight.remove("breakdown " + key);
                breakdown.put(key, Json.objects(Json.firstObject(r.out()), "directories"));
                showDetail();
            });
        } else {
            DefaultTableModel m = tickTable(new String[] {"", L("Directory", "Carpeta"), L("Uses", "Ocupa"), L("Files", "Ficheros"),
                    L("New dataset", "Dataset nuevo")}, 4);
            for (Map<String, Object> d : dirs) {
                String dir = Json.str(d, "dir");
                // A dataset name cannot hold «/» as a level that does not exist, nor a leading dot.
                String name = dir.replace('/', ':').replaceFirst("^\\.+", "");
                m.addRow(new Object[] {false, dir, StatusModel.bytesText(Json.lng(d, "bytes", 0)), Json.text(d, "files"), name});
            }
            JTable t = new JTable(m);
            t.getColumnModel().getColumn(0).setMaxWidth(36);
            w.add(Ui.tableScroll(t, 8));
            w.add(Ui.row(button(L("Turn into datasets…", "Convertir en datasets…"), () -> {
                if (t.isEditing()) {
                    t.getCellEditor().stopCellEditing();
                }
                List<String> pairs = new ArrayList<>();
                for (int i = 0; i < m.getRowCount(); i++) {
                    if (Boolean.TRUE.equals(m.getValueAt(i, 0))) {
                        pairs.add(Ui.quoted(String.valueOf(m.getValueAt(i, 1))));
                        pairs.add(Ui.quoted(String.valueOf(m.getValueAt(i, 4)).trim()));
                    }
                }
                if (pairs.isEmpty()) {
                    host.say(L("Tick the directories first.", "Marca antes las carpetas."));
                    return;
                }
                breakdown.remove(key);
                act("breakdown " + String.join(" ", pairs) + on, running, true, null);
            })));
        }

        // ── Child datasets back into directories (assemble).
        List<String> children = new ArrayList<>();
        for (Map<String, Object> e : entries.getOrDefault(machine + "/" + poolOf(dataset), List.of())) {
            String name = Json.str(e, "name");
            if (Json.str(e, "type").equals("dataset") && name.startsWith(dataset + "/")
                    && !name.substring(dataset.length() + 1).contains("/")) {
                children.add(name.substring(dataset.length() + 1));
            }
        }
        w.add(section(L("Turn child datasets into directories", "Convertir datasets hijos en carpetas")));
        w.add(note(L("The opposite: a child dataset becomes an ordinary directory again; its files stay where they are. Its own snapshots, quota and properties are lost, and its scheduled copies stop.",
                "Lo contrario: un dataset hijo vuelve a ser una carpeta normal; los ficheros se quedan donde están. Se pierden sus snapshots propios, su cuota y sus propiedades, y sus copias programadas dejan de hacerse.")));
        if (children.isEmpty()) {
            w.add(left(Ui.muted(L("It has no child datasets.", "No tiene datasets hijos."))));
        } else {
            DefaultTableModel m = tickTable(new String[] {"", L("Dataset", "Dataset")}, -1);
            for (String c : children) {
                m.addRow(new Object[] {false, c});
            }
            JTable t = new JTable(m);
            t.getColumnModel().getColumn(0).setMaxWidth(36);
            w.add(Ui.tableScroll(t, 8));
            w.add(Ui.row(button(L("Turn into directories…", "Convertir en carpetas…"), () -> {
                List<String> chosen = new ArrayList<>();
                for (int i = 0; i < m.getRowCount(); i++) {
                    if (Boolean.TRUE.equals(m.getValueAt(i, 0))) {
                        chosen.add(Ui.quoted(String.valueOf(m.getValueAt(i, 1))));
                    }
                }
                if (chosen.isEmpty()) {
                    host.say(L("Tick the datasets first.", "Marca antes los datasets."));
                    return;
                }
                act("assemble " + String.join(" ", chosen) + on, running, true, null);
            })));
        }

        // ── Into an ordinary directory (todir).
        w.add(section(L("Copy to an ordinary directory", "Volcar a una carpeta")));
        w.add(note(L("Copies all its contents to an ordinary directory of the same machine, for instance to take it out of ZFS or to an external disk.",
                "Copia todo su contenido a una carpeta corriente de la misma máquina, por ejemplo para sacarlo de ZFS o llevarlo a un disco externo.")));
        {
            JTextField dir = new JTextField(28);
            dir.putClientProperty("JTextField.placeholderText", L("destination directory, e.g. /mnt/disk/media", "carpeta de destino, p. ej. /mnt/disco/media"));
            JCheckBox del = new JCheckBox(L("Destroy the dataset afterwards", "Destruir el dataset al terminar"));
            Ui.Form form = new Ui.Form();
            form.row(L("Directory", "Carpeta"), dir).row("", del);
            w.add(form);
            w.add(Ui.row(button(L("Copy to the directory…", "Volcar a la carpeta…"), () -> {
                if (dir.getText().trim().isEmpty()) {
                    return;
                }
                act("todir " + Ui.quoted(dir.getText().trim()) + (del.isSelected() ? " --delete-source" : "") + on, running, true, null);
            })));
        }

        // ── From an ordinary directory (fromdir), of this or another machine.
        w.add(section(L("Bring a directory in", "Traer una carpeta")));
        w.add(note(L("Brings the contents of an ordinary directory —of this machine or another— into this dataset. Deleting the source happens only after the copy is checked identical.",
                "Trae el contenido de una carpeta corriente —de esta máquina o de otra— a este dataset. Borrar el origen solo ocurre tras comprobar que la copia es idéntica.")));
        {
            JComboBox<NewCopyWizard.Item<String>> from = machinesBox(machine);
            JTextField dir = new JTextField(28);
            dir.putClientProperty("JTextField.placeholderText", L("source directory", "carpeta de origen"));
            JTextField sub = new JTextField(28);
            sub.putClientProperty("JTextField.placeholderText", L("optional: where inside this dataset", "opcional: dónde dentro de este dataset"));
            JCheckBox del = new JCheckBox(L("Delete the source afterwards", "Borrar la carpeta de origen al terminar"));
            Ui.Form form = new Ui.Form();
            form.row(L("From machine", "De la máquina"), from).row(L("Directory", "Carpeta"), dir).row(L("Inside", "Dentro de"), sub).row("", del);
            w.add(form);
            w.add(Ui.row(button(L("Bring the directory…", "Traer la carpeta…"), () -> {
                String d = dir.getText().trim();
                if (d.isEmpty()) {
                    return;
                }
                // The source as a URL with its machine: zfsm://<machine>#path/<directory>.
                String line = "fromdir " + Ui.quoted("zfsm://" + chosenId(from) + "#path" + (d.startsWith("/") ? d : "/" + d));
                if (!sub.getText().trim().isEmpty()) {
                    line += " --subdir " + Ui.quoted(sub.getText().trim());
                }
                if (del.isSelected()) {
                    line += " --delete-source";
                }
                act(line + on, L("Copied.", "Copiado."), true, null);
            })));
        }
        return w;
    }

    /** The machines that answer, for a «where to» choice; the given one first chosen. */
    private JComboBox<NewCopyWizard.Item<String>> machinesBox(String first) {
        JComboBox<NewCopyWizard.Item<String>> box = new JComboBox<>();
        for (StatusModel.MachineView m : store.machines()) {
            if (m.connected) {
                NewCopyWizard.Item<String> it = new NewCopyWizard.Item<>(m.name, m.id);
                box.addItem(it);
                if (m.id.equals(first)) {
                    box.setSelectedItem(it);
                }
            }
        }
        return box;
    }

    @SuppressWarnings("unchecked")
    private static String chosenId(JComboBox<NewCopyWizard.Item<String>> box) {
        Object o = box.getSelectedItem();
        return o == null ? "" : ((NewCopyWizard.Item<String>) o).value();
    }

    // ── Advanced: name, mounting, keys, permissions; sending; snapshot extras
    //    Everything is a CLI line; the CLI asks what has to be
    //    confirmed and the passphrases.

    private void showOutput(String machine, String line, String title) {
        store.forMachine(machine).request(line, "text", false,
                r -> Ui.showText(this, title, r.ok() || r.err().trim().isEmpty() ? r.out() : r.err()));
    }

    private void newDataset(String machine, String parent) {
        JTextField name = new JTextField(24);
        name.putClientProperty("JTextField.placeholderText", L("e.g. photos", "p. ej. fotos"));
        JTextField properties = new JTextField(24);
        properties.putClientProperty("JTextField.placeholderText", "compression=zstd atime=off");
        JCheckBox volume = new JCheckBox(L("A volume (a block device), not a filesystem",
                "Un volumen (un dispositivo de bloques), no un sistema de ficheros"));
        JTextField size = new JTextField(10);
        size.putClientProperty("JTextField.placeholderText", L("size, e.g. 20G", "tamaño, p. ej. 20G"));
        size.setEnabled(false);
        volume.addItemListener(e -> size.setEnabled(volume.isSelected()));
        Ui.Form form = new Ui.Form();
        form.row("", new JLabel(L("Created inside %1.", "Se crea dentro de %1.", parent)))
                .row(L("Name", "Nombre"), name).row(L("Properties (optional)", "Propiedades (opcional)"), properties)
                .row("", volume).row(L("Size", "Tamaño"), size);
        if (JOptionPane.showConfirmDialog(this, form, L("New dataset", "Dataset nuevo"), JOptionPane.OK_CANCEL_OPTION,
                JOptionPane.PLAIN_MESSAGE) != JOptionPane.OK_OPTION) {
            return;
        }
        String n = name.getText().trim();
        if (n.isEmpty() || (volume.isSelected() && size.getText().trim().isEmpty())) {
            return;
        }
        StringBuilder line = new StringBuilder("create " + Ui.quoted(n));
        for (String p : properties.getText().trim().split("\\s+")) {
            if (!p.isEmpty()) {
                line.append(' ').append(Ui.quoted(p));
            }
        }
        if (volume.isSelected()) {
            line.append(" -V ").append(Ui.quoted(size.getText().trim()));
        }
        act(line + " --on " + url(machine, parent), L("Dataset %1 created.", "Creado el dataset %1.", parent + "/" + n), true, null);
    }

    private JComponent manageTab(String machine, String dataset) {
        JPanel w = column();
        String here = url(machine, dataset);
        String on = " --on " + here;
        if (!ensureProps(machine, dataset)) {
            w.add(left(Ui.muted(L("Reading its properties…", "Leyendo sus propiedades…"))));
            return w;
        }

        // ── Name and place.
        w.add(section(L("Name and place", "Nombre y sitio")));
        w.add(note(L("Renaming keeps its snapshots and copies. Writing another parent moves it inside the same pool.",
                "Renombrar conserva sus snapshots y copias. Escribir otro padre lo mueve dentro del mismo pool.")));
        w.add(Ui.row(button(L("Rename or move…", "Renombrar o mover…"), () -> {
            String v = Ui.askLine(this, L("Rename", "Renombrar"), L("New full name (pool/…/name):", "Nombre completo nuevo (pool/…/nombre):"), dataset);
            if (v == null || v.equals(dataset)) {
                return;
            }
            // Same parent: the short form. Another parent: both names, the URL first.
            String parent = dataset.contains("/") ? dataset.substring(0, dataset.lastIndexOf('/')) : "";
            String vParent = v.contains("/") ? v.substring(0, v.lastIndexOf('/')) : "";
            String line = vParent.equals(parent) ? "rename " + Ui.quoted(v.substring(v.lastIndexOf('/') + 1)) + " --on " + here
                                                 : "rename " + here + " " + Ui.quoted(v);
            act(line, L("Now it is %1.", "Ahora es %1.", v), true, null);
        }), button(L("New dataset inside…", "Dataset nuevo dentro…"), () -> newDataset(machine, dataset))));

        // ── Mounting.
        Map<String, Object> entry = Map.of();
        for (Map<String, Object> e : entries.getOrDefault(machine + "/" + poolOf(dataset), List.of())) {
            if (Json.str(e, "name").equals(dataset)) {
                entry = e;
            }
        }
        if (!Json.str(entry, "type").equals("volume")) {
            w.add(section(L("Mounting", "Montaje")));
            boolean mounted = Json.str(entry, "mounted").equals("yes");
            w.add(left(new JLabel(mounted ? L("Mounted at %1.", "Montado en %1.", Json.str(entry, "mountpoint"))
                                          : L("Not mounted: its files cannot be reached.", "Sin montar: no se llega a sus ficheros."))));
            w.add(Ui.row(button(mounted ? L("Unmount", "Desmontar") : L("Mount", "Montar"),
                    () -> act((mounted ? "unmount" : "mount") + on, mounted ? L("Unmounted.", "Desmontado.") : L("Mounted.", "Montado."), true, null))));
        }

        // ── A clone: it depends on the snapshot it came from until promoted.
        String origin = propOf(machine, dataset, "origin");
        if (!origin.isEmpty() && !origin.equals("-")) {
            w.add(section(L("Clone", "Clon")));
            w.add(note(L("It was cloned from %1, and that snapshot cannot be deleted while this exists. Making it independent swaps the roles: the snapshots up to that point move here.",
                    "Se clonó de %1, y ese snapshot no se puede borrar mientras esto exista. Hacerlo independiente cambia los papeles: los snapshots hasta ese punto pasan a este.", origin)));
            w.add(Ui.row(button(L("Make independent (promote)", "Hacer independiente (promote)"),
                    () -> act("promote" + on, L("Independent now.", "Ya es independiente."), true, null))));
        }

        // ── Encryption keys.
        String enc = propOf(machine, dataset, "encryption");
        if (!enc.isEmpty() && !enc.equals("off")) {
            String keyStatus = propOf(machine, dataset, "keystatus");
            String encRoot = propOf(machine, dataset, "encryptionroot");
            w.add(section(L("Encryption", "Cifrado")));
            boolean loaded = keyStatus.equals("available");
            String text = loaded ? L("The key is loaded: its data can be read.", "La clave está cargada: sus datos se pueden leer.")
                                 : L("The key is not loaded: nothing inside can be read or mounted.", "La clave no está cargada: no se puede leer ni montar nada.");
            if (!encRoot.isEmpty() && !encRoot.equals(dataset)) {
                text += " " + L("It uses the key of %1.", "Usa la clave de %1.", encRoot);
            }
            w.add(left(Ui.wrapped(text)));
            String propsKey = machine + "/" + dataset;
            if (loaded) {
                w.add(Ui.row(button(L("Lock (unload the key)", "Cerrar (descargar la clave)"), () -> {
                    props.remove(propsKey);
                    act("unload-key" + on, L("Locked.", "Cerrado."), true, null);
                }), button(L("Change the passphrase…", "Cambiar la frase de paso…"),
                        () -> act("change-key" + on, L("Passphrase changed.", "Frase de paso cambiada."), false, null))));
            } else {
                w.add(Ui.row(button(L("Unlock (load the key)…", "Abrir (cargar la clave)…"), () -> {
                    props.remove(propsKey);
                    act("load-key" + on, L("Unlocked.", "Abierto."), true, null);
                })));
            }
        }

        // ── Delegated permissions (zfs allow).
        w.add(section(L("Delegated permissions", "Permisos delegados")));
        w.add(note(L("Lets a user or group who is not root do some ZFS things here: take snapshots, send, mount… Permissions are ZFS's names, separated by commas: snapshot,send,hold,mount.",
                "Deja a un usuario o grupo que no es root hacer algunas cosas de ZFS aquí: hacer snapshots, enviar, montar… Los permisos son los nombres de ZFS, separados por comas: snapshot,send,hold,mount.")));
        w.add(Ui.row(button(L("See the permissions", "Ver los permisos"),
                () -> showOutput(machine, "allow" + on, L("Permissions on %1", "Permisos en %1", dataset)))));
        JComboBox<NewCopyWizard.Item<String>> kind = new JComboBox<>();
        kind.addItem(new NewCopyWizard.Item<>(L("User", "Usuario"), "user"));
        kind.addItem(new NewCopyWizard.Item<>(L("Group", "Grupo"), "group"));
        kind.addItem(new NewCopyWizard.Item<>(L("Everyone", "Todos"), "everyone"));
        kind.addItem(new NewCopyWizard.Item<>(L("Named set (@name)", "Conjunto con nombre (@nombre)"), "set"));
        JTextField who = new JTextField(20);
        kind.addActionListener(e -> who.setEnabled(!chosenId(kind).equals("everyone")));
        JTextField perms = new JTextField(20);
        perms.putClientProperty("JTextField.placeholderText", "snapshot,send,hold");
        JComboBox<NewCopyWizard.Item<String>> scope = new JComboBox<>();
        scope.addItem(new NewCopyWizard.Item<>(L("Here and below", "Aquí y debajo"), ""));
        scope.addItem(new NewCopyWizard.Item<>(L("Only here", "Solo aquí"), " --local"));
        scope.addItem(new NewCopyWizard.Item<>(L("Only below", "Solo debajo"), " --descend"));
        Ui.Form form = new Ui.Form();
        form.row(L("To", "A"), kind).row(L("Name", "Nombre"), who).row(L("Permissions", "Permisos"), perms).row(L("Where", "Dónde"), scope);
        w.add(form);
        Function<Boolean, String> line = grant -> {
            String k = chosenId(kind);
            StringBuilder s = new StringBuilder(grant ? "allow" : "unallow");
            if (k.equals("everyone")) {
                s.append(" --everyone");
            } else {
                String name = who.getText().trim();
                if (name.isEmpty()) {
                    return "";
                }
                if (k.equals("set") && !name.startsWith("@")) {
                    name = "@" + name;
                }
                s.append(" --").append(k).append(' ').append(Ui.quoted(name));
            }
            List<String> p = new ArrayList<>();
            for (String x : perms.getText().split("[,\\s]+")) {
                if (!x.isEmpty()) {
                    p.add(x);
                }
            }
            if (grant && p.isEmpty()) {
                return "";
            }
            for (String x : p) {
                s.append(' ').append(Ui.quoted(x));
            }
            return s + chosenId(scope) + on;
        };
        w.add(Ui.row(button(L("Grant", "Conceder"), () -> {
            String s = line.apply(true);
            if (s.isEmpty()) {
                host.say(L("Say to whom and which permissions.", "Di a quién y qué permisos."));
                return;
            }
            act(s, L("Granted.", "Concedido."), false, null);
        }), button(L("Revoke…", "Quitar…"), () -> {
            String s = line.apply(false);
            if (s.isEmpty()) {
                host.say(L("Say to whom.", "Di a quién."));
                return;
            }
            act(s, L("Revoked.", "Quitado."), false, null);
        }), Ui.muted(L("Revoking with no permissions written takes them all.", "Quitar sin escribir permisos los quita todos."))));
        return w;
    }

    private JComponent sendTab(String machine, String dataset) {
        JPanel w = column();
        List<String> snaps = new ArrayList<>();
        for (Map<String, Object> e : entries.getOrDefault(machine + "/" + poolOf(dataset), List.of())) {
            String name = Json.str(e, "name");
            if (name.startsWith(dataset + "@")) {
                snaps.add(0, name.substring(name.indexOf('@') + 1));  // the newest first
            }
        }
        String jobLater = L("Sending as a job: follow it in Jobs.", "Enviando como trabajo: síguelo en Trabajos.");

        // ── A snapshot to another dataset (zfs send | zfs receive, as a job).
        w.add(section(L("Send a snapshot", "Enviar un snapshot")));
        w.add(note(L("Once, by hand. For a copy that keeps itself up to date there is «Copy…» in the summary.",
                "Una vez, a mano. Para una copia que se mantenga al día está «Copiar…» en el resumen.")));
        {
            JComboBox<String> snap = new JComboBox<>(snaps.toArray(new String[0]));
            JComboBox<NewCopyWizard.Item<String>> to = machinesBox(machine);
            JTextField dest = new JTextField(24);
            dest.putClientProperty("JTextField.placeholderText", L("pool/dataset, e.g. backup/photos", "pool/dataset, p. ej. copias/fotos"));
            JCheckBox recursive = new JCheckBox(L("With its children (-R)", "Con sus hijos (-R)"));
            JCheckBox raw = new JCheckBox(L("Raw (-w)", "En crudo (-w)"));
            raw.setToolTipText(L("Encrypted data travels encrypted, without its key.", "Lo cifrado viaja cifrado, sin su clave."));
            JCheckBox level = new JCheckBox(L("Only what changed (--level)", "Solo lo cambiado (--level)"));
            level.setToolTipText(L("Only what changed since the newest snapshot both have.", "Solo lo cambiado desde el snapshot más nuevo que tengan los dos."));
            JComboBox<NewCopyWizard.Item<String>> partial = new JComboBox<>();
            partial.addItem(new NewCopyWizard.Item<>(L("Stop and tell me", "Parar y avisarme"), ""));
            partial.addItem(new NewCopyWizard.Item<>(L("Continue it", "Continuarla"), " --resume"));
            partial.addItem(new NewCopyWizard.Item<>(L("Discard it and start again", "Descartarla y empezar de nuevo"), " --discard-partial"));
            Ui.Form form = new Ui.Form();
            form.row(L("Snapshot", "Snapshot"), snap).row(L("To machine", "A la máquina"), to).row(L("Dataset", "Dataset"), dest)
                    .row("", recursive).row("", raw).row("", level).row(L("If cut off before", "Si quedó a medias"), partial);
            w.add(form);
            w.add(note(L("«If cut off before»: what to do when the destination has a receive that was interrupted.",
                    "«Si quedó a medias»: qué hacer si el destino tiene una recepción que se interrumpió.")));
            JButton go = button(L("Send", "Enviar"), () -> {
                String d = dest.getText().trim();
                if (d.isEmpty() || snap.getSelectedItem() == null) {
                    host.say(L("Say where it goes.", "Di adónde va."));
                    return;
                }
                StringBuilder line = new StringBuilder("send " + url(chosenId(to), d) + " --from " + url(machine, dataset) + "@" + snap.getSelectedItem());
                if (recursive.isSelected()) {
                    line.append(" -R");
                }
                if (raw.isSelected()) {
                    line.append(" -w");
                }
                if (level.isSelected()) {
                    line.append(" --level");
                }
                line.append(chosenId(partial));
                act(line.toString(), jobLater, true, null);
            });
            go.setEnabled(!snaps.isEmpty());
            w.add(Ui.row(go));
        }

        // ── Its files to another mounted tree (the agent's own tree copy, `rsync`).
        w.add(section(L("Sync its files", "Sincronizar sus ficheros")));
        w.add(note(L("Copies FILES, not snapshots, to another mounted dataset of this or another machine. Without deleting, it only adds and updates.",
                "Copia FICHEROS, no snapshots, a otro dataset montado de esta máquina o de otra. Sin borrar, solo añade y actualiza.")));
        {
            JComboBox<NewCopyWizard.Item<String>> to = machinesBox(machine);
            JTextField dest = new JTextField(24);
            dest.putClientProperty("JTextField.placeholderText", L("pool/dataset", "pool/dataset"));
            JCheckBox del = new JCheckBox(L("Delete what is extra there", "Borrar lo que sobre allí"));
            del.setToolTipText(L("--delete: the destination ends up identical, and that includes deleting.",
                    "--delete: el destino queda idéntico, y eso incluye borrar."));
            Ui.Form form = new Ui.Form();
            form.row(L("To machine", "A la máquina"), to).row(L("Dataset", "Dataset"), dest).row("", del);
            w.add(form);
            Supplier<String> line = () -> {
                String d = dest.getText().trim();
                return d.isEmpty() ? "" : "rsync " + url(chosenId(to), d) + (del.isSelected() ? " --delete" : "") + " --on " + url(machine, dataset);
            };
            w.add(Ui.row(button(L("See what it would do", "Ver qué haría"), () -> {
                String s = line.get();
                if (!s.isEmpty()) {
                    showOutput(machine, s + " --check --wait", L("What syncing would do", "Qué haría sincronizar"));
                }
            }), button(L("Sync", "Sincronizar"), () -> {
                String s = line.get();
                if (s.isEmpty()) {
                    host.say(L("Say where it goes.", "Di adónde va."));
                    return;
                }
                act(s, L("Syncing as a job: follow it in Jobs.", "Sincronizando como trabajo: síguelo en Trabajos."), false, null);
            })));
        }
        return w;
    }

    private List<Component> snapshotExtras(String machine, String dataset, Supplier<String> chosen) {
        // What only makes sense with one chosen: shown always, it answers «choose one» otherwise.
        Function<Consumer<String>, Runnable> withChosen = f -> () -> {
            String s = chosen.get();
            if (s.isEmpty()) {
                host.say(L("Choose a snapshot first.", "Elige antes un snapshot."));
                return;
            }
            f.accept(s);
        };
        List<Component> out = new ArrayList<>();
        out.add(button(L("Changes since…", "Cambios desde…"), withChosen.apply(s -> showOutput(machine,
                "diff " + url(machine, dataset) + " --from " + url(machine, s), L("Changes since %1", "Cambios desde %1", s)))));
        out.add(button(L("Clone…", "Clonar…"), withChosen.apply(s -> {
            String v = Ui.askLine(this, L("Clone", "Clonar"), L("Name of the new dataset (full: pool/…/name):",
                    "Nombre del dataset nuevo (completo: pool/…/nombre):"), dataset + "-" + s.substring(s.indexOf('@') + 1));
            if (v != null) {
                act("clone " + Ui.quoted(v) + " --from " + url(machine, s), L("Cloned into %1.", "Clonado en %1.", v), true, null);
            }
        })));
        out.add(button(L("Holds…", "Retenciones…"), withChosen.apply(s -> showOutput(machine, "holds " + url(machine, s),
                L("Holds of %1", "Retenciones de %1", s)))));
        out.add(button(L("Hold…", "Retener…"), withChosen.apply(s -> {
            String tag = Ui.askLine(this, L("Hold", "Retener"), L("A label: while it is there, the snapshot cannot be deleted.",
                    "Una etiqueta: mientras esté, el snapshot no se puede borrar."), "keep");
            if (tag != null) {
                act("hold " + Ui.quoted(tag) + " --on " + url(machine, s), L("Held.", "Retenido."), false, null);
            }
        })));
        out.add(button(L("Release…", "Soltar…"), withChosen.apply(s -> {
            String tag = Ui.askLine(this, L("Release", "Soltar"), L("The label of the hold to remove:",
                    "La etiqueta de la retención que se quita:"), "");
            if (tag != null) {
                act("release " + Ui.quoted(tag) + " --on " + url(machine, s), L("Released.", "Soltado."), false, null);
            }
        })));
        return out;
    }

    private void changeAddress(String id) {
        // What it has now, from the configuration: the page knows the machines by their overview,
        // which does not say how they are reached.
        store.general().request("ls zfsm://", r -> {
            Map<String, Object> conn = Map.of();
            for (Map<String, Object> c : Json.objects(Json.firstObject(r.out()), "connections")) {
                if (Json.str(c, "id").equals(id)) {
                    conn = c;
                }
            }
            if (conn.isEmpty()) {
                host.say(r.ok() ? L("That machine is no longer configured.", "Esa máquina ya no está en la configuración.") : r.err().trim());
                return;
            }
            // This computer is reached at its addresses by the OTHERS: they are changed from one of
            // them (or with `edit --addresses` in the CLI).
            if (Json.str(conn, "type").equalsIgnoreCase("LOCAL")) {
                host.say(L("This computer's address is changed from another computer of the network.",
                        "La dirección de este ordenador se cambia desde otro ordenador de la red."));
                return;
            }
            JTextField hostField = new JTextField(Json.str(conn, "host"), 24);
            JSpinner port = new JSpinner(new SpinnerNumberModel((int) Json.lng(conn, "port", 22), 1, 65535, 1));
            JTextField user = new JTextField(Json.str(conn, "user"), 24);
            Ui.Form form = new Ui.Form();
            form.row(L("Host", "Máquina (host)"), hostField).row(L("SSH port", "Puerto SSH"), port).row(L("User", "Usuario"), user);
            JPanel p = new JPanel(new BorderLayout(0, 8));
            p.add(form, BorderLayout.NORTH);
            p.add(Ui.paragraph(L("The other machines keep the address they had when you gave them keys: after this, give them keys again in Administration so they reach it at the new one.",
                    "Las demás máquinas guardan la dirección que tenían cuando les diste llaves: después de esto, dales llaves de nuevo en Administración para que la busquen en la nueva.")),
                    BorderLayout.CENTER);
            String title = L("Address of %1", "Dirección de %1", Json.str(conn, "name", id));
            while (true) {
                if (JOptionPane.showConfirmDialog(this, p, title, JOptionPane.OK_CANCEL_OPTION, JOptionPane.PLAIN_MESSAGE)
                        != JOptionPane.OK_OPTION) {
                    return;
                }
                // As «Add a machine»: no spaces nor quotes, so the line needs no quoting.
                String h = hostField.getText().trim();
                String u = user.getText().trim();
                if (h.isEmpty() || h.matches(".*[\\s\"'].*")) {
                    JOptionPane.showMessageDialog(this, L("The host, without spaces.", "La máquina (host), sin espacios."));
                    continue;
                }
                if (u.isEmpty() || u.matches(".*[\\s\"'].*")) {
                    JOptionPane.showMessageDialog(this, L("The user, without spaces.", "El usuario, sin espacios."));
                    continue;
                }
                act("edit " + url(id, "") + " --host " + h + " --port " + port.getValue() + " --user " + u + " --keep-password",
                        L("Address changed. Give the other machines keys again in Administration.",
                          "Dirección cambiada. Dales llaves de nuevo a las demás máquinas en Administración."), false, () -> {
                            probe.remove(id);
                            showDetail();
                        });
                return;
            }
        });
    }

    /**
     * Runs a command line on the general service, says how it went and refreshes. {@code then}:
     * what to do once it has finished —for instance, check the machine again—.
     */
    private void act(String line, String done, boolean reloadTree, Runnable then) {
        store.general().request(line, r -> {
            host.say(r.ok() ? done : r.err().trim());
            if (then != null) {
                then.run();
            }
            if (reloadTree) {
                reloadOpenPools();
            }
            if (r.ok()) {
                // A command that left a job behind changes its destination when the job ends.
                store.followIfJob(r.out());
            }
            store.refresh();
        });
    }

    /** The open pools are asked again: what changed is in one of them. */
    private void reloadOpenPools() {
        for (int i = 0; i < root.getChildCount(); i++) {
            DefaultMutableTreeNode mi = (DefaultMutableTreeNode) root.getChildAt(i);
            for (int j = 0; j < mi.getChildCount(); j++) {
                DefaultMutableTreeNode pi = (DefaultMutableTreeNode) mi.getChildAt(j);
                Node pn = nodeOf(pi);
                if (pn != null && pn.kind() == Kind.POOL && !unloaded(pi)
                        && entries.containsKey(pn.machine() + "/" + pn.name())) {
                    loadPool(pi);
                }
            }
        }
    }
}
