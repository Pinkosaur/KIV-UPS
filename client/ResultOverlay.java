import javax.swing.*;
import java.awt.*;
import java.awt.event.ActionListener;

/**
 * ResultOverlay
 *
 * A transparent overlay component used to display end-of-game messages
 * (Victory, Defeat, Draw) or important notifications over the game board.
 */
public class ResultOverlay extends JPanel {
    private final JPanel overlayColorPanel;
    private final JLabel overlayTitle;
    private final JLabel overlaySubtitle;
    private final JButton overlayContinue;
    private boolean endOverlayShown = false;

    public ResultOverlay(ActionListener continueAction) {
        super(null);
        setOpaque(false);
        overlayColorPanel = new JPanel();
        overlayColorPanel.setOpaque(true);
        overlayColorPanel.setBackground(new Color(0,0,0,120));
        overlayColorPanel.setLayout(new GridBagLayout());

        overlayTitle = new JLabel("", SwingConstants.CENTER);
        overlayTitle.setForeground(Color.WHITE);
        overlayTitle.setFont(overlayTitle.getFont().deriveFont(48f).deriveFont(Font.BOLD));

        overlaySubtitle = new JLabel("", SwingConstants.CENTER);
        overlaySubtitle.setForeground(Color.WHITE);
        overlaySubtitle.setFont(overlaySubtitle.getFont().deriveFont(18f));

        overlayContinue = new JButton("Continue");
        overlayContinue.setFocusPainted(false);
        if (continueAction != null) overlayContinue.addActionListener(continueAction);

        JPanel inner = new JPanel();
        inner.setOpaque(false);
        inner.setLayout(new BoxLayout(inner, BoxLayout.Y_AXIS));
        overlayTitle.setAlignmentX(Component.CENTER_ALIGNMENT);
        overlaySubtitle.setAlignmentX(Component.CENTER_ALIGNMENT);
        overlayContinue.setAlignmentX(Component.CENTER_ALIGNMENT);
        inner.add(Box.createVerticalGlue());
        inner.add(overlayTitle);
        inner.add(Box.createRigidArea(new Dimension(0,8)));
        inner.add(overlaySubtitle);
        inner.add(Box.createRigidArea(new Dimension(0,20)));
        inner.add(overlayContinue);
        inner.add(Box.createVerticalGlue());
        overlayColorPanel.add(inner, new GridBagConstraints());
        add(overlayColorPanel);
        setVisible(false);
    }

    /**
     * Shows a color-coded overlay indicating the game result.
     * Blue for Victory, Red for Defeat.
     */
    public void showEndOverlay(boolean isWinner, String subtitle) {
        endOverlayShown = true;
        if (isWinner) {
            overlayColorPanel.setBackground(new Color(0, 64, 192, 140));
            overlayTitle.setText("VICTORY");
        } else {
            overlayColorPanel.setBackground(new Color(192, 32, 32, 160));
            overlayTitle.setText("DEFEAT");
        }
        overlaySubtitle.setText(subtitle != null ? subtitle : "");
        setVisible(true);
        revalidate();
        repaint();
    }

    /**
     * Shows a neutral grey overlay for notifications or non-binary outcomes (Draw).
     */
    public void showNeutralOverlay(String text) {
        if (endOverlayShown) return;
        endOverlayShown = true;
        overlayColorPanel.setBackground(new Color(48,48,48,160));
        overlayTitle.setText(text);
        overlaySubtitle.setText("");
        setVisible(true);
        revalidate();
        repaint();
    }

    public void hideOverlay() {
        endOverlayShown = false;
        setVisible(false);
    }

    public boolean isEndOverlayShown() {
        return endOverlayShown;
    }

    public void setInnerBounds(int x, int y, int w, int h) {
        overlayColorPanel.setBounds(x, y, w, h);
    }

    public JButton getContinueButton() { return overlayContinue; }
}