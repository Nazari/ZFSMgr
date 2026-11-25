package org.zfsmgr.gui;

import java.awt.AlphaComposite;
import java.awt.Color;
import java.awt.Cursor;
import java.awt.Graphics;
import java.awt.Graphics2D;
import java.awt.event.MouseAdapter;
import javax.swing.JComponent;
import javax.swing.JLayer;
import javax.swing.JPanel;
import javax.swing.UIManager;
import javax.swing.plaf.LayerUI;

/**
 * What is shown while it may be about to change: greyed, with the wait cursor over it, and not
 * taking clicks. The rule of the whole interface: whatever is being read again —because it can
 * change outside ZFSMgr, or because the person just did something to it— is veiled from the
 * moment it is asked until its answer is painted. What only this interface changes, and has not,
 * stays as it is. A refresh that shows nothing and then repaints by surprise is what this avoids.
 *
 * <p>{@code wrap} once and {@code set} as the state goes; or {@link #over} for a page that builds
 * its components again on every render.
 */
final class Veil extends LayerUI<JComponent> {
    private boolean on;
    private JLayer<JComponent> layer;

    /** The component with this veil over it; {@code set} turns it on and off. */
    JLayer<JComponent> wrap(JComponent c) {
        layer = new JLayer<>(c, this);
        JPanel glass = new JPanel();
        glass.setOpaque(false);
        // Listening is what takes the clicks: a glass pane with no listener lets them through.
        MouseAdapter swallow = new MouseAdapter() {};
        glass.addMouseListener(swallow);
        glass.addMouseMotionListener(swallow);
        glass.addMouseWheelListener(swallow);
        glass.setCursor(Cursor.getPredefinedCursor(Cursor.WAIT_CURSOR));
        glass.setVisible(false);
        layer.setGlassPane(glass);
        return layer;
    }

    void set(boolean veiled) {
        if (on == veiled) {
            return;
        }
        on = veiled;
        if (layer != null) {
            layer.getGlassPane().setVisible(on);
            layer.repaint();
        }
    }

    boolean on() {
        return on;
    }

    /** For pages built again on every render: {@code c}, veiled or not. */
    static JComponent over(JComponent c, boolean veiled) {
        Veil v = new Veil();
        JLayer<JComponent> l = v.wrap(c);
        v.set(veiled);
        l.setAlignmentX(c.getAlignmentX());
        l.setAlignmentY(c.getAlignmentY());
        return l;
    }

    @Override
    public void paint(Graphics g, JComponent c) {
        super.paint(g, c);
        if (!on) {
            return;
        }
        Graphics2D g2 = (Graphics2D) g.create();
        Color bg = UIManager.getColor("Panel.background");
        g2.setColor(bg != null ? bg : Color.LIGHT_GRAY);
        g2.setComposite(AlphaComposite.getInstance(AlphaComposite.SRC_OVER, 0.6f));
        g2.fillRect(0, 0, c.getWidth(), c.getHeight());
        g2.dispose();
    }
}
