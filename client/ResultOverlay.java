import javax.swing.*;
import java.awt.*;
import java.awt.event.ActionListener;

/**
 * ResultOverlay: a reusable overlay component that covers the content area.
 *
 * The original single-file client constructed overlayPanel + overlayColorPanel + inner UI.
 * This class implements the same UI and exposes:
 *   - showEndOverlay(isWinner, subtitle)
 *   - showNeutralOverlay(text)
 *   - hideOverlay()
 *
 * The Continue button's action is provided by the GUI via the constructor (so GUI can call exitToWelcome()).
 *
 * The overlay is sized by the caller (the GUI sets bounds on frame resize as in original).
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

    /** Show end overlay (colorized winner/loser) */
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

    /** Show a neutral overlay (non-colored) */
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

    /** Allow caller to check whether overlay is currently visible as an "end overlay" */
    public boolean isEndOverlayShown() {
        return endOverlayShown;
    }

    /** Adjust the color-panel bounds to match parent content area; caller should set overlay bounds */
    public void setInnerBounds(int x, int y, int w, int h) {
        overlayColorPanel.setBounds(x, y, w, h);
    }

    public JButton getContinueButton() { return overlayContinue; }
}
