package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import java.awt.Color;
import java.awt.Font;
import javax.swing.BorderFactory;
import javax.swing.JLabel;
import javax.swing.JPanel;
import javax.swing.UIManager;

/** The small kit the pages share: state colours and words, cards and text. */
public final class Ui {
    private Ui() {}

    public static Color colourOf(String state) {
        return switch (state) {
            case "ok" -> new Color(0x1A7F37);
            case "warning", "pending" -> new Color(0x9A6700);
            case "error" -> new Color(0xB42318);
            default -> new Color(0x6B7380);
        };
    }

    public static String stateWord(String state) {
        return switch (state) {
            case "ok" -> L("● Fine", "● Bien");
            case "warning" -> L("▲ Needs a look", "▲ Revisar");
            case "pending" -> L("○ Pending", "○ Pendiente");
            case "error" -> L("✕ Failing", "✕ Falla");
            case "paused" -> L("‖ Paused", "‖ En pausa");
            default -> L("… Asking", "… Preguntando");
        };
    }

    public static JLabel stateLabel(String state) {
        JLabel l = new JLabel(stateWord(state));
        l.setForeground(colourOf(state));
        l.setFont(l.getFont().deriveFont(Font.BOLD));
        return l;
    }

    public static JLabel heading(String text) {
        JLabel l = new JLabel(text);
        l.setFont(l.getFont().deriveFont(Font.BOLD));
        return l;
    }

    public static JLabel muted(String text) {
        JLabel l = new JLabel(text);
        Color c = UIManager.getColor("Label.disabledForeground");
        l.setForeground(c != null ? c : Color.GRAY);
        return l;
    }

    /**
     * Text that wraps at the width it is given. A JLabel with HTML only wraps if its preferred
     * width is bounded; a JTextArea that looks like a label wraps at whatever width the layout
     * gives it.
     */
    public static javax.swing.JTextArea wrapped(String text) {
        javax.swing.JTextArea t = new javax.swing.JTextArea(text);
        t.setLineWrap(true);
        t.setWrapStyleWord(true);
        t.setEditable(false);
        t.setFocusable(false);
        t.setOpaque(false);
        t.setBorder(null);
        t.setFont(UIManager.getFont("Label.font"));
        return t;
    }

    /**
     * A paragraph for a dialog: HTML with a fixed width, so the dialog knows its height before it
     * is shown. A wrapping text area there reported one line, and the buttons were cut off.
     */
    public static JLabel paragraph(String text) {
        return new JLabel("<html><div style='width:420px'>" + escape(text).replace("\n", "<br>")
                + "</div></html>");
    }

    public static JPanel card() {
        JPanel p = new JPanel();
        Color line = UIManager.getColor("Component.borderColor");
        p.setBorder(BorderFactory.createCompoundBorder(
                BorderFactory.createLineBorder(line != null ? line : Color.LIGHT_GRAY, 1, true),
                BorderFactory.createEmptyBorder(8, 10, 8, 10)));
        return p;
    }

    public static String escape(String s) {
        return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;");
    }
    /**
     * A value as one word of a CLI line: as it is when it has no spaces nor quotes; otherwise
     * between double quotes.
     */
    public static String quoted(String value) {
        if (!value.contains(" ") && !value.contains("\"") && !value.contains("'")) {
            return value;
        }
        return "\"" + value.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
    }

    /** A text in a window of its own, monospaced, as a command wrote it. */
    public static void showText(java.awt.Component parent, String title, String text) {
        javax.swing.JTextArea t = new javax.swing.JTextArea(text.trim().isEmpty() ? L("(nothing)", "(nada)") : text);
        t.setEditable(false);
        t.setFont(new Font(Font.MONOSPACED, Font.PLAIN, t.getFont().getSize()));
        javax.swing.JScrollPane sp = new javax.swing.JScrollPane(t);
        sp.setPreferredSize(new java.awt.Dimension(760, 460));
        javax.swing.JOptionPane.showMessageDialog(parent, sp, title, javax.swing.JOptionPane.PLAIN_MESSAGE);
    }

    /** A one-line question; null when cancelled or left empty. */
    public static String askLine(java.awt.Component parent, String title, String text, String initial) {
        Object v = javax.swing.JOptionPane.showInputDialog(parent, paragraph(text), title,
                javax.swing.JOptionPane.PLAIN_MESSAGE, null, null, initial);
        if (v == null || v.toString().trim().isEmpty()) {
            return null;
        }
        return v.toString().trim();
    }

    /** Labels on the left, fields on the right, one row each. */
    public static final class Form extends JPanel {
        private int row;

        public Form() {
            super(new java.awt.GridBagLayout());
            setAlignmentX(java.awt.Component.LEFT_ALIGNMENT);
        }

        public Form row(String label, java.awt.Component field) {
            java.awt.GridBagConstraints c = new java.awt.GridBagConstraints();
            c.gridy = row++;
            c.insets = new java.awt.Insets(3, 0, 3, 10);
            c.anchor = java.awt.GridBagConstraints.WEST;
            c.gridx = 0;
            add(new JLabel(label == null ? "" : label), c);
            c.gridx = 1;
            c.weightx = 1;
            c.fill = java.awt.GridBagConstraints.HORIZONTAL;
            add(field, c);
            return this;
        }

        @Override
        public java.awt.Dimension getMaximumSize() {
            return new java.awt.Dimension(Integer.MAX_VALUE, getPreferredSize().height);
        }
    }

    /** A table that cannot be edited, from rows of cells. */
    public static javax.swing.JTable table(String[] heads, java.util.List<Object[]> rows) {
        javax.swing.table.DefaultTableModel m = new javax.swing.table.DefaultTableModel(heads, 0) {
            @Override
            public boolean isCellEditable(int r, int c) {
                return false;
            }
        };
        for (Object[] r : rows) {
            m.addRow(r);
        }
        javax.swing.JTable t = new javax.swing.JTable(m);
        t.setSelectionMode(javax.swing.ListSelectionModel.SINGLE_SELECTION);
        t.setFillsViewportHeight(true);
        return t;
    }

    /** A scroll for a table, as tall as its rows up to {@code max} rows. */
    public static javax.swing.JScrollPane tableScroll(javax.swing.JTable t, int max) {
        javax.swing.JScrollPane sp = new javax.swing.JScrollPane(t);
        int rows = Math.max(1, Math.min(max, t.getRowCount()));
        int h = t.getRowHeight() * rows + t.getTableHeader().getPreferredSize().height + 6;
        sp.setPreferredSize(new java.awt.Dimension(400, h));
        sp.setMaximumSize(new java.awt.Dimension(Integer.MAX_VALUE, h));
        sp.setAlignmentX(java.awt.Component.LEFT_ALIGNMENT);
        return sp;
    }

    /** Buttons in rows, left to right, wrapping at {@code perRow}. */
    public static JPanel buttonGrid(java.util.List<? extends java.awt.Component> buttons, int perRow) {
        JPanel p = new JPanel(new java.awt.GridBagLayout());
        java.awt.GridBagConstraints c = new java.awt.GridBagConstraints();
        c.insets = new java.awt.Insets(3, 0, 3, 6);
        c.fill = java.awt.GridBagConstraints.HORIZONTAL;
        for (int i = 0; i < buttons.size(); i++) {
            c.gridx = i % perRow;
            c.gridy = i / perRow;
            p.add(buttons.get(i), c);
        }
        c.gridx = perRow;
        c.weightx = 1;
        p.add(javax.swing.Box.createHorizontalGlue(), c);
        p.setAlignmentX(java.awt.Component.LEFT_ALIGNMENT);
        p.setMaximumSize(new java.awt.Dimension(Integer.MAX_VALUE, p.getPreferredSize().height));
        return p;
    }

    /** A row of components, left aligned. */
    public static JPanel row(java.awt.Component... cs) {
        JPanel p = new JPanel(new java.awt.FlowLayout(java.awt.FlowLayout.LEFT, 0, 2));
        for (java.awt.Component c : cs) {
            p.add(c);
            p.add(javax.swing.Box.createHorizontalStrut(6));
        }
        p.setAlignmentX(java.awt.Component.LEFT_ALIGNMENT);
        p.setMaximumSize(new java.awt.Dimension(Integer.MAX_VALUE, p.getPreferredSize().height));
        return p;
    }
}
