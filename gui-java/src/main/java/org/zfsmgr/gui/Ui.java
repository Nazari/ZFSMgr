package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import java.awt.Color;
import java.awt.Font;
import javax.swing.BorderFactory;
import javax.swing.JLabel;
import javax.swing.JPanel;
import javax.swing.UIManager;

/** The small kit the pages share: state colours and words, cards and text. (src/gui/uikit.cpp.) */
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
}
