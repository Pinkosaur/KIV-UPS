import javax.swing.*;
import java.awt.*;

public class SquareLabel extends JLabel {
    private boolean drawCheckCircle = false;
    
    // [FIX] Pre-allocate drawing objects to avoid garbage generation in paint loop
    private static final Color CHECK_FILL_COLOR = new Color(255, 0, 0, 160);
    private static final Color CHECK_BORDER_COLOR = new Color(160, 0, 0, 200);
    private static final Stroke CHECK_STROKE = new BasicStroke(2f);

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
                
                // [FIX] Use constants
                g2.setColor(CHECK_FILL_COLOR);
                g2.fillOval(x, y, diameter, diameter);
                
                g2.setColor(CHECK_BORDER_COLOR);
                g2.setStroke(CHECK_STROKE);
                g2.drawOval(x, y, diameter, diameter);
            }
        } finally {
            g2.dispose();
        }

        // Draw the icon (text/image) without painting background again
        boolean oldOpaque = isOpaque();
        setOpaque(false);
        try {
            super.paintComponent(g);
        } finally {
            setOpaque(oldOpaque);
        }
    }
}