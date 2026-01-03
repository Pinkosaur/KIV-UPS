import javax.swing.*;
import java.awt.*;
import java.awt.event.ActionListener;

public class WaitingPanel extends JPanel {
    private final Timer timer;
    private int angle = 0;

    public WaitingPanel(ActionListener onCancel) {
        setLayout(new BorderLayout());
        setBackground(new Color(40, 40, 40));
        
        JLabel lbl = new JLabel("Waiting for opponent...", SwingConstants.CENTER);
        lbl.setForeground(Color.WHITE);
        lbl.setFont(new Font("SansSerif", Font.BOLD, 18));
        add(lbl, BorderLayout.NORTH);
        
        JButton cancelBtn = new JButton("Cancel");
        cancelBtn.addActionListener(onCancel);
        
        JPanel btnPanel = new JPanel();
        btnPanel.setOpaque(false);
        btnPanel.add(cancelBtn);
        add(btnPanel, BorderLayout.SOUTH);

        timer = new Timer(50, e -> {
            angle = (angle + 5) % 360;
            repaint();
        });
    }

    public void startAnimation() {
        timer.start();
    }

    public void stopAnimation() {
        timer.stop();
    }

    @Override
    protected void paintComponent(Graphics g) {
        super.paintComponent(g);
        Graphics2D g2 = (Graphics2D) g;
        g2.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
        
        int r = 30;
        int cx = getWidth() / 2;
        int cy = getHeight() / 2;
        
        // Draw static circle track (optional, looks nicer)
        g2.setColor(new Color(80, 80, 80));
        g2.setStroke(new BasicStroke(4));
        g2.drawOval(cx - r, cy - r, 2 * r, 2 * r);

        // Draw spinning arc
        g2.setColor(Color.WHITE);
        g2.drawArc(cx - r, cy - r, 2 * r, 2 * r, -angle, 120);
    }
}