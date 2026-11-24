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
import javax.swing.JList;
import javax.swing.JPanel;
import javax.swing.JScrollPane;
import javax.swing.ListSelectionModel;
import javax.swing.SwingUtilities;

/**
 * The window: the pages on the left, the chosen one on the right. In the spike only «Estado»
 * exists; the others say so.
 */
public final class MainWindow extends JFrame {
    private final CliService general;
    private final Map<String, CliService> perMachine = new LinkedHashMap<>();
    private final StateStore store;

    public MainWindow(String cli, CliService general, CliService.Listener listener) {
        super("ZFSMgr");
        this.general = general;
        // Each machine through its own service, opened with the data key the general one got:
        // a slow machine delays only its own answer.
        this.store = new StateStore(general, id -> perMachine.computeIfAbsent(id, k -> {
            CliService s = new CliService(cli, Text.lang(), SwingUtilities::invokeLater, listener);
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
            p.add(Ui.muted(L("Not in the Java spike yet.", "Todavía no está en la prueba en Java.")),
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
        setSize(1100, 760);
        setLocationByPlatform(true);
    }

    public StateStore store() {
        return store;
    }

    /** The services say goodbye —the general one to the daemons, as the session— and it ends. */
    public void close() {
        setVisible(false);
        for (CliService s : perMachine.values()) {
            s.stop(1500);
        }
        general.stop(4000);
        dispose();
        System.exit(0);
    }
}
