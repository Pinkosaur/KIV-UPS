import javax.swing.*;
import java.awt.*;

/**
 * SquareLabel: lightweight JLabel subclass used for board squares.
 * Mirrors the original inner-class behavior: supports a red "check" circle
 * drawn under the piece by setDrawCheckCircle(true).
 */
public class SquareLabel extends JLabel {
    private boolean drawCheckCircle = false;

    public SquareLabel(String t, int align) {
        super(t, align);
        setOpaque(true);
    }

    public void setDrawCheckCircle(boolean v) {
        drawCheckCircle = v;
        repaint();
    }

    @Override
    protected void paintComponent(Graphics g) {
        Graphics2D g2 = (Graphics2D) g.create();
        try {
            // paint background
            g2.setColor(getBackground());
            g2.fillRect(0, 0, getWidth(), getHeight());

            // draw check circle if requested
            if (drawCheckCircle) {
                int w = getWidth();
                int h = getHeight();
                int diameter = (int) (Math.min(w, h) * 0.85);
                int x = (w - diameter) / 2;
                int y = (h - diameter) / 2;
                g2.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
                g2.setColor(new Color(255, 0, 0, 160));
                g2.fillOval(x, y, diameter, diameter);
                g2.setColor(new Color(160, 0, 0, 200));
                g2.setStroke(new BasicStroke(2f));
                g2.drawOval(x, y, diameter, diameter);
            }
        } finally {
            g2.dispose();
        }

        boolean oldOpaque = isOpaque();
        setOpaque(false);
        try {
            super.paintComponent(g);
        } finally {
            setOpaque(oldOpaque);
        }
    }
}
