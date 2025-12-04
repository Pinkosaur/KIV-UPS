import javax.imageio.ImageIO;
import javax.swing.*;
import javax.swing.border.Border;
import java.awt.*;
import java.awt.event.*;
import java.awt.image.BufferedImage;
import java.io.File;
import java.io.IOException;
import java.util.HashSet;
import java.util.Map;
import java.util.HashMap;
import java.util.Set;

/**
 * Refactored ChessClientGUI that delegates low-level tasks to:
 *  - BoardModel
 *  - ImageManager
 *  - NetworkClient
 *  - Utils
 *  - SquareLabel
 *  - ResultOverlay
 *
 * Behavior and parsing logic follow the original single-file client closely.
 */
public class ChessClientGUI {
    // default host; will be overriden by the text field on the welcome screen
    private static final int PORT = 10001;
    private String serverHost = "127.0.0.1";
    private JTextField serverIpField; // welcome-screen input for server IP
    private static final String PIECES_DIR = "pieces"; // relative to working dir
    private static final String BACK_DIR = "backgrounds"; // placeholders
    private static final int SQUARE_SIZE = 64;
    private String clientName;
    private String opponentName;

    private JTextField nameField;

    private volatile boolean matchmakingTimeoutReceived = false;

    private JFrame frame;
    private JLabel statusLabel;

    // networking abstraction
    private NetworkClient networkClient;

    private JPanel cards; // CardLayout for welcome/waiting/game
    private static final String CARD_WELCOME = "welcome";
    private static final String CARD_WAITING = "waiting";
    private static final String CARD_GAME = "game";

    private JPanel boardPanel;
    private SquareLabel[][] squares = new SquareLabel[8][8];

    // model abstraction
    private final BoardModel boardModel = new BoardModel();
    // keep a local reference for legacy-style access
    private char[][] board = boardModel.board;

    // image manager
    private final ImageManager imageManager;

    // wrapper panel that will center the board and enforce square sizing
    private JPanel boardContainer;

    // interaction state
    private int myColor = -1; // 0 white, 1 black
    private boolean myTurn = false;
    private volatile boolean endOverlayShown = false;
    private int selR = -1, selC = -1;
    private Set<Point> highlighted = new HashSet<>();
    private volatile String pendingFrom = null, pendingTo = null;
    private volatile char pendingPromo = 0;
    private volatile boolean waitingForOk = false;

    private boolean kingInCheck = false;
    private int kingR = -1, kingC = -1;

    // last-move markers (model coordinates). null = none
    private Point lastFrom = null;
    private Point lastTo = null;

    // UI buttons (so we can enable/disable at game end)
    private JButton resignBtn;
    private JButton drawBtn;

    private volatile boolean gameEnded = false;
    private volatile boolean intentionalDisconnect = false;
    private volatile boolean drawOfferPending = false;

    // result UI (overlay)
    private ResultOverlay resultOverlay;

    public ChessClientGUI() {
        this.imageManager = new ImageManager(PIECES_DIR);
    }

    public static void main(String[] args) {
        SwingUtilities.invokeLater(() -> {
            try {
                new ChessClientGUI().createAndShowGUI();
            } catch (Exception e) {
                e.printStackTrace();
            }
        });
    }

    // ---- Image helpers (delegate to ImageManager) ----
    private void loadIcons() {
        imageManager.loadIcons();
    }

    private ImageIcon getScaledIconForPiece(char p, int cellSize) {
        return imageManager.getScaledIconForPiece(p, cellSize);
    }

    /** Initialize board state via BoardModel */
    private void initBoardModel() {
        boardModel.initBoardModel();
        this.board = boardModel.board; // ensure local alias updated
    }

    /* compute king check using BoardModel helpers */
    private void computeKingCheck() {
        if (myColor != 0 && myColor != 1) { kingInCheck = false; kingR = kingC = -1; return; }
        int[] pos = boardModel.findKing(board, myColor);
        if (pos == null) { kingInCheck = false; kingR = kingC = -1; return; }
        kingR = pos[0]; kingC = pos[1];
        kingInCheck = boardModel.isSquareAttacked(board, kingR, kingC, 1 - myColor);
    }

    private boolean inBounds(int r,int c){ return boardModel.inBounds(r,c); }
    private boolean pathClear(char[][] b, int r1,int c1,int r2,int c2) { return boardModel.pathClear(b,r1,c1,r2,c2); }
    private boolean isLegalMoveBasic(int r1,int c1,int r2,int c2) { return boardModel.isLegalMoveBasic(r1,c1,r2,c2); }
    private boolean isSquareAttacked(char[][] b, int r, int c, int byColor) { return boardModel.isSquareAttacked(b,r,c,byColor); }
    private int[] findKing(char[][] b, int color) { return boardModel.findKing(b, color); }
    private boolean moveLeavesInCheck(char[][] b, int color, int r1,int c1,int r2,int c2) { return boardModel.moveLeavesInCheck(b,color,r1,c1,r2,c2); }

    private void updateBoardUI() {
        SwingUtilities.invokeLater(() -> {
            computeKingCheck();
            for (int uiR = 0; uiR < 8; uiR++) {
                for (int uiC = 0; uiC < 8; uiC++) {
                    int modelR = (myColor == 1) ? (7 - uiR) : uiR;
                    int modelC = (myColor == 1) ? (7 - uiC) : uiC;

                    char p = board[modelR][modelC];

                    int boardPx = Math.min(boardPanel.getWidth(), boardPanel.getHeight());
                    int cellSize = (boardPx > 0) ? (boardPx / 8) : SQUARE_SIZE;
                    ImageIcon icon = getScaledIconForPiece(p, cellSize);
                    squares[uiR][uiC].setIcon(icon);
                    squares[uiR][uiC].setText((icon == null) ? (p=='.'?"":""+p) : "");

                    boolean isLastFrom = (lastFrom != null && lastFrom.x == modelR && lastFrom.y == modelC);
                    boolean isLastTo   = (lastTo   != null && lastTo.x   == modelR && lastTo.y   == modelC);

                    if (selR == modelR && selC == modelC) {
                        squares[uiR][uiC].setBorder(BorderFactory.createLineBorder(Color.RED, 3));
                    } else if (highlighted.contains(new Point(modelR, modelC))) {
                        squares[uiR][uiC].setBorder(BorderFactory.createLineBorder(Color.GREEN, 3));
                    } else if (isLastFrom || isLastTo) {
                        squares[uiR][uiC].setBorder(BorderFactory.createLineBorder(Color.YELLOW, 3));
                    } else {
                        squares[uiR][uiC].setBorder(BorderFactory.createLineBorder(Color.BLACK, 1));
                    }

                    squares[uiR][uiC].setDrawCheckCircle(modelR == kingR && modelC == kingC && kingInCheck);
                }
            }
            if (statusLabel != null) statusLabel.setText((myColor==0?"White":"Black") + ": " + clientName + " (you) VS " + (myColor==1?"White":"Black") + ": " + opponentName + "  -  " + (myTurn?"Your turn":"Opponent's turn"));
        });
    }

    private void createAndShowGUI() {
        loadIcons();
        clientName = Utils.generateRandomName();

        frame = new JFrame("Chess Client");
        frame.setDefaultCloseOperation(JFrame.DO_NOTHING_ON_CLOSE);
        frame.addWindowListener(new WindowAdapter() {
            @Override
            public void windowClosing(WindowEvent e) {
                if (intentionalDisconnect) return;
                intentionalDisconnect = true;
                // best-effort notify server
                try {
                    if (networkClient != null && networkClient.isConnected()) {
                        networkClient.sendRaw("QUIT");
                    }
                } catch (Exception ignored) {}
                // close and exit
                if (networkClient != null) networkClient.closeConnection();
                SwingUtilities.invokeLater(() -> {
                    try { frame.dispose(); } catch (Exception ignored) {}
                    System.exit(0);
                });
            }
        });

        cards = new JPanel(new CardLayout());

        // --- WELCOME card ---
        JPanel welcome = new JPanel(new BorderLayout());
        JLabel welcomeBg = loadBackgroundLabel(BACK_DIR + "/chessboardbg.jpg", new Color(30,30,60));
        JLayeredPane welcomeLayer = new JLayeredPane();
        welcomeLayer.setLayout(null);
        welcomeBg.setBounds(0,0,1600,800);
        welcomeLayer.add(welcomeBg, JLayeredPane.DEFAULT_LAYER);

        JComponent welcomeTextOverlay = new JComponent() {
            @Override
            protected void paintComponent(Graphics g) {
                super.paintComponent(g);
                Graphics2D g2 = (Graphics2D) g.create();
                try {
                    g2.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING, RenderingHints.VALUE_TEXT_ANTIALIAS_ON);
                    int w = getWidth(), h = getHeight();
                    if (w <= 0 || h <= 0) return;
                    String s = "WELCOME";
                    float fontSize = Math.max(24f, Math.min(w, h) / 5f);
                    Font f = getFont().deriveFont(Font.BOLD, fontSize);
                    g2.setFont(f);
                    FontMetrics fm = g2.getFontMetrics();
                    int tw = fm.stringWidth(s);
                    int th = fm.getAscent();
                    int x = (w - tw) / 2;
                    int y = (h + th) / 2 - fm.getDescent();
                    g2.setColor(new Color(0,0,0,100));
                    g2.drawString(s, x+3, y+3);
                    g2.setColor(new Color(255,255,255,200));
                    g2.drawString(s, x, y);
                } finally {
                    g2.dispose();
                }
            }
        };
        welcomeTextOverlay.setOpaque(false);
        welcomeTextOverlay.setBounds(0,0,800,600);
        welcomeLayer.add(welcomeTextOverlay, JLayeredPane.PALETTE_LAYER);
        welcome.add(welcomeLayer, BorderLayout.CENTER);

        JPanel wc = new JPanel();
        wc.setOpaque(false);
        wc.setLayout(new BoxLayout(wc, BoxLayout.Y_AXIS));

        JPanel nameRow = new JPanel(new FlowLayout(FlowLayout.CENTER, 8, 6));
        nameRow.setOpaque(false);
        nameRow.add(new JLabel("Your name:"));
        nameField = new JTextField(clientName, 16);
        nameRow.add(nameField);
        wc.add(nameRow);

        JPanel ipRow = new JPanel(new FlowLayout(FlowLayout.CENTER, 8, 6));
        ipRow.setOpaque(false);
        ipRow.add(new JLabel("Server IP:"));
        serverIpField = new JTextField(serverHost, 18);
        ipRow.add(serverIpField);
        wc.add(ipRow);

        JButton findBtn = new JButton("Find Match");
        findBtn.setAlignmentX(Component.CENTER_ALIGNMENT);
        wc.add(findBtn);
        welcome.add(wc, BorderLayout.SOUTH);
        findBtn.addActionListener(e -> onFindMatchClicked());

        welcome.addComponentListener(new ComponentAdapter() {
            @Override
            public void componentResized(ComponentEvent e) {
                int totalW = welcome.getWidth();
                int totalH = welcome.getHeight();
                if (totalW <= 0 || totalH <= 0) return;
                int bottomH = wc.getHeight();
                if (bottomH <= 0) bottomH = wc.getPreferredSize().height;
                if (bottomH < 0) bottomH = 0;
                int centerH = totalH - bottomH;
                if (centerH < 0) centerH = 0;
                welcomeLayer.setBounds(0, 0, totalW, centerH);
                welcomeBg.setBounds(0, 0, totalW, centerH);
                welcomeTextOverlay.setBounds(0, 0, totalW, centerH);
                welcomeLayer.revalidate();
                welcomeLayer.repaint();
            }
            @Override
            public void componentShown(ComponentEvent e) {
                SwingUtilities.invokeLater(() -> {
                    int totalW = welcome.getWidth();
                    int totalH = welcome.getHeight();
                    if (totalW <= 0 || totalH <= 0) return;
                    int bottomH = wc.getHeight();
                    if (bottomH <= 0) bottomH = wc.getPreferredSize().height;
                    if (bottomH < 0) bottomH = 0;
                    int centerH = totalH - bottomH;
                    if (centerH < 0) centerH = 0;
                    welcomeLayer.setBounds(0, 0, totalW, centerH);
                    welcomeBg.setBounds(0, 0, totalW, centerH);
                    welcomeTextOverlay.setBounds(0, 0, totalW, centerH);
                    welcomeLayer.revalidate();
                    welcomeLayer.repaint();
                });
            }
        });

        // --- WAITING card ---
        JPanel waiting = new JPanel(new BorderLayout());
        JLabel waitingBg = loadBackgroundLabel(BACK_DIR + "/waiting.gif", new Color(40,40,40));
        waiting.add(waitingBg, BorderLayout.CENTER);
        JLabel waitingLbl = new JLabel("Searching for opponent...", SwingConstants.CENTER);
        waitingLbl.setForeground(Color.WHITE);
        waiting.add(waitingLbl, BorderLayout.SOUTH);

        // --- GAME card ---
        JPanel gameCard = new JPanel(new BorderLayout());
        boardPanel = new JPanel(new GridLayout(8,8));
        for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
            SquareLabel sq = new SquareLabel("", SwingConstants.CENTER);
            sq.setPreferredSize(new Dimension(SQUARE_SIZE, SQUARE_SIZE));
            if ((r+c)%2==0) sq.setBackground(new Color(240,240,240)); else sq.setBackground(new Color(160,160,160));
            sq.setBorder(BorderFactory.createLineBorder(Color.BLACK,1));
            final int fr=r, fc=c;
            sq.addMouseListener(new MouseAdapter(){ public void mouseClicked(MouseEvent ev){ onSquareClicked(fr,fc); } });
            squares[r][c]=sq; boardPanel.add(sq);
        }
        initBoardModel(); updateBoardUI();

        boardContainer = new JPanel(new GridBagLayout());
        boardContainer.setOpaque(false);
        int initialBoardPx = SQUARE_SIZE * 8;
        boardPanel.setPreferredSize(new Dimension(initialBoardPx, initialBoardPx));
        GridBagConstraints gbc = new GridBagConstraints();
        gbc.gridx = 0; gbc.gridy = 0; gbc.anchor = GridBagConstraints.CENTER; gbc.fill = GridBagConstraints.NONE;
        boardContainer.add(boardPanel, gbc);

        boardContainer.addComponentListener(new ComponentAdapter() {
            @Override
            public void componentResized(ComponentEvent e) {
                int w = boardContainer.getWidth();
                int h = boardContainer.getHeight();
                int padding = 40;
                int size = Math.min(w, h) - padding;
                if (size < 40) size = 40;
                size = (size / 8) * 8;
                boardPanel.setPreferredSize(new Dimension(size, size));
                boardPanel.revalidate();
                imageManager.clearCacheForSize(size);
                updateBoardUI();
            }
        });

        // top status label
        statusLabel = new JLabel("Not connected", SwingConstants.CENTER);
        statusLabel.setOpaque(true);
        statusLabel.setBackground(new Color(40, 40, 40));
        statusLabel.setForeground(Color.WHITE);
        statusLabel.setBorder(BorderFactory.createEmptyBorder(8, 12, 8, 12));
        statusLabel.setFont(statusLabel.getFont().deriveFont(14f));
        statusLabel.setVisible(false);
        frame.getContentPane().add(statusLabel, BorderLayout.NORTH);

        JPanel bottom = new JPanel(new BorderLayout());

        JPanel ctrl = new JPanel(new FlowLayout(FlowLayout.CENTER, 8, 0));
        resignBtn = new JButton("Resign");
        drawBtn = new JButton("Offer Draw");
        resignBtn.setEnabled(false);
        drawBtn.setEnabled(false);
        ctrl.add(drawBtn);
        ctrl.add(resignBtn);

        // action listeners for resign/draw
        resignBtn.addActionListener(e -> {
            int ok = JOptionPane.showConfirmDialog(frame, "Are you sure you want to resign?", "Resign", JOptionPane.YES_NO_OPTION);
            if (ok == JOptionPane.YES_OPTION) {
                if (networkClient != null) networkClient.sendRaw("RESIGN");
                append(">> RESIGN");
                resignBtn.setEnabled(false);
                drawBtn.setEnabled(false);
            }
        });

        drawBtn.addActionListener(e -> {
            if (networkClient != null) {
                networkClient.sendRaw("DRAW_OFFER");
                append(">> DRAW_OFFER");
                drawBtn.setEnabled(false);
            }
        });

        JPanel bottomWrap = new JPanel(new BorderLayout());
        bottomWrap.add(bottom, BorderLayout.NORTH);
        bottomWrap.add(ctrl, BorderLayout.SOUTH);

        cards = new JPanel(new CardLayout());
// --- GAME background layer (chessboardbg.jpg) + board container layered ---
JComponent gameBg = new JComponent() {
    private BufferedImage img = null;
    private BufferedImage cachedScaled = null;
    private int cachedW = -1, cachedH = -1;
    {
        File f = new File(BACK_DIR + "/chessboardbg.jpg");
        if (f.exists()) {
            try { img = ImageIO.read(f); } catch (IOException ex) { img = null; }
        }
    }
    @Override
    protected void paintComponent(Graphics g) {
        super.paintComponent(g);
        Graphics2D g2 = (Graphics2D) g.create();
        try {
            int w = getWidth(), h = getHeight();
            if (w <= 0 || h <= 0) return;
            if (img != null) {
                // If cached scaled image does not match current size, recreate it.
                if (w != cachedW || h != cachedH || cachedScaled == null) {
                    // compute scale to COVER the area (scale = max(scaleX, scaleY))
                    double scaleX = (double) w / img.getWidth();
                    double scaleY = (double) h / img.getHeight();
                    double scale = Math.max(scaleX, scaleY);

                    int destW = (int) Math.round(img.getWidth() * scale);
                    int destH = (int) Math.round(img.getHeight() * scale);

                    // create a buffered image of the component size and draw the scaled image centered
                    BufferedImage tmp = new BufferedImage(w, h, BufferedImage.TYPE_INT_ARGB);
                    Graphics2D tg = tmp.createGraphics();
                    try {
                        tg.setRenderingHint(RenderingHints.KEY_INTERPOLATION, RenderingHints.VALUE_INTERPOLATION_BILINEAR);
                        int x = (w - destW) / 2;
                        int y = (h - destH) / 2;
                        tg.drawImage(img, x, y, destW, destH, null);
                    } finally {
                        tg.dispose();
                    }
                    cachedScaled = tmp;
                    cachedW = w; cachedH = h;
                }
                g2.drawImage(cachedScaled, 0, 0, null);
            } else {
                g2.setColor(new Color(30, 30, 30));
                g2.fillRect(0, 0, w, h);
            }
        } finally {
            g2.dispose();
        }
    }

    @Override
    public void invalidate() {
        super.invalidate();
        // clear cache when layout changes to force re-scale next paint
        cachedScaled = null;
        cachedW = cachedH = -1;
    }
};


// layered pane to show background behind the centered boardContainer
JLayeredPane gameLayer = new JLayeredPane();
gameLayer.setLayout(null); // we'll manage bounds manually

// add background at default layer
gameBg.setBounds(0, 0, 800, 600); // initial bounds; will be updated by resize handler
gameLayer.add(gameBg, JLayeredPane.DEFAULT_LAYER);

// add boardContainer above background
boardContainer.setBounds(0, 0, boardPanel.getPreferredSize().width, boardPanel.getPreferredSize().height);
gameLayer.add(boardContainer, JLayeredPane.PALETTE_LAYER);

// ensure layered pane fills the gameCard center area and children are resized/centered
gameCard.addComponentListener(new ComponentAdapter() {
    @Override
    public void componentResized(ComponentEvent e) {
        int totalW = gameCard.getWidth();
        int totalH = gameCard.getHeight();
        if (totalW <= 0 || totalH <= 0) return;

        // set layered pane size to available center area (exclude bottom controls height)
        // bottomWrap is placed separately below in SOUTH; for safety compute center area as gameCard center
        int centerW = totalW;
        int centerH = totalH;
        gameLayer.setBounds(0, 0, centerW, centerH);

        // background covers whole layered pane
        gameBg.setBounds(0, 0, centerW, centerH);

        // compute preferred board pixel size (reuse the same sizing logic as boardContainer)
        int padding = 40;
        int size = Math.min(centerW, centerH - bottomWrap.getHeight()) - padding;
        if (size < 40) size = Math.min(centerW, centerH);
        size = (size / 8) * 8;
        if (size < 8) size = (Math.min(centerW, centerH) / 8) * 8;

        // center the boardContainer inside the layered pane
        int bx = Math.max(0, (centerW - size) / 2);
        int by = Math.max(0, (centerH - size) / 2);
        boardPanel.setPreferredSize(new Dimension(size, size));
        boardContainer.setBounds(bx, by, size, size);

        // clear icon cache for the new size and repaint
        imageManager.clearCacheForSize(size);
        boardContainer.revalidate();
        gameLayer.revalidate();
        gameLayer.repaint();
    }

    @Override
    public void componentShown(ComponentEvent e) {
        SwingUtilities.invokeLater(() -> {
            componentResized(e);
        });
    }
});

// put layered pane in center of gameCard
gameCard.add(gameLayer, BorderLayout.CENTER);
// bottomWrap remains in SOUTH
gameCard.add(bottomWrap, BorderLayout.SOUTH);

        cards.add(welcome, CARD_WELCOME);
        cards.add(waiting, CARD_WAITING);
        cards.add(gameCard, CARD_GAME);

        frame.getContentPane().add(cards, BorderLayout.CENTER);

        resultOverlay = new ResultOverlay(e -> exitToWelcome());
        frame.getLayeredPane().add(resultOverlay, JLayeredPane.MODAL_LAYER);
        frame.addComponentListener(new ComponentAdapter() {
            @Override
            public void componentResized(ComponentEvent e) {
                Rectangle b = frame.getContentPane().getBounds();
                resultOverlay.setBounds(b);
                resultOverlay.setInnerBounds(0,0,b.width,b.height);
            }
            @Override
            public void componentShown(ComponentEvent e) {
                Rectangle b = frame.getContentPane().getBounds();
                resultOverlay.setBounds(b);
                resultOverlay.setInnerBounds(0,0,b.width,b.height);
            }
        });

        frame.setPreferredSize(new Dimension(1100, 760));
        frame.setMinimumSize(new Dimension(800, 600));
        frame.pack();
        frame.setLocationRelativeTo(null);
        frame.setVisible(true);
        Rectangle b = frame.getContentPane().getBounds();
        resultOverlay.setBounds(b);
        resultOverlay.setInnerBounds(0,0,b.width,b.height);
        SwingUtilities.invokeLater(() -> {
            if (boardContainer != null) {
                boardContainer.revalidate();
                boardContainer.repaint();
            }
        });
    }

    private JLabel loadBackgroundLabel(String path, Color fallback) {
        File f = new File(path);
        if (f.exists()) {
            String lower = path.toLowerCase();
            try {
                if (lower.endsWith(".gif")) {
                    ImageIcon ico = new ImageIcon(path);
                    JLabel lbl = new JLabel(ico);
                    lbl.setHorizontalAlignment(SwingConstants.CENTER);
                    lbl.setVerticalAlignment(SwingConstants.CENTER);
                    return lbl;
                } else {
                    BufferedImage img = ImageIO.read(f);
                    return new JLabel(new ImageIcon(img));
                }
            } catch (IOException e) {
                System.err.println("Failed to load background: " + path + " -> " + e.getMessage());
            }
        }
        JLabel p = new JLabel();
        p.setOpaque(true);
        p.setBackground(fallback);
        return p;
    }

    /* small helper to escape a few HTML chars in player names to avoid label-breaking names */
    private String escapeHtml(String s) {
        if (s == null) return "";
        return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;");
    }

    private void onFindMatchClicked() {
        CardLayout cl = (CardLayout) cards.getLayout(); cl.show(cards, CARD_WAITING);
        if (serverIpField != null) {
            String entered = serverIpField.getText().trim();
            if (!entered.isEmpty()) serverHost = entered;
        }
        if (nameField != null) {
            String n = nameField.getText().trim();
            if (!n.isEmpty()) clientName = n;
        }
        startConnection();
    }

    private void startConnection() {
        closeConnection();
        matchmakingTimeoutReceived = false;
        intentionalDisconnect = false;
        // create network client with listener that forwards to parseServerMessage
        networkClient = new NetworkClient(new NetworkClient.NetworkListener() {
            @Override public void onConnected() {
                append("Connected");
                // Log HELLO send like original
                append("Sent HELLO " + clientName);
            }
            @Override public void onDisconnected() {
                append("Server closed connection (EOF).");
                // Mirror the original connect() EOF handling:
                if (matchmakingTimeoutReceived) {
                    append("Matchmaking EOF after MATCH_TIMEOUT; no further UI changes.");
                    matchmakingTimeoutReceived = false;
                    return;
                } else {
                    if (myColor != -1) {
                        if (!resultOverlay.isEndOverlayShown()) {
                            showNeutralOverlay("Connection closed by server");
                        }
                    } else {
                        SwingUtilities.invokeLater(() -> {
                            try {
                                CardLayout cl = (CardLayout) cards.getLayout();
                                cl.show(cards, CARD_WELCOME);
                            } catch (Exception ignored) {}
                        });
                    }
                }
                // Ensure pending state reset
                waitingForOk = false;
                pendingFrom = pendingTo = null;
            }
            @Override public void onServerMessage(String line) {
                parseServerMessage(line);
            }
            @Override public void onNetworkError(Exception ex) {
                append("Connection error: " + ex.getMessage());
                if (!intentionalDisconnect && !resultOverlay.isEndOverlayShown()) {
                    SwingUtilities.invokeLater(() -> {
                        try { CardLayout cl = (CardLayout)cards.getLayout(); cl.show(cards, CARD_WELCOME); } catch (Exception ignored) {}
                        JOptionPane.showMessageDialog(frame, "Network error: " + ex.getMessage(), "Connection error", JOptionPane.ERROR_MESSAGE);
                    });
                }
            }
        });

        // connect in background similar to original: spawn a thread that calls networkClient.connect
        Thread t = new Thread(() -> {
            append("Connecting to server " + serverHost + ":" + PORT + " ...");
            networkClient.connect(serverHost, PORT, clientName);
            // Note: NetworkClient sends HELLO automatically in connect()
        }, "ChessClient-Connector");
        t.setDaemon(true);
        t.start();
    }

    private void closeConnection() {
        if (networkClient != null) {
            networkClient.closeConnection();
            networkClient = null;
        }
    }

    private void exitToWelcome() {
        matchmakingTimeoutReceived = false;
        myColor = -1;
        myTurn = false;
        selR = selC = -1;
        highlighted.clear();
        pendingFrom = pendingTo = null;
        lastFrom = lastTo = null;
        waitingForOk = false;
        endOverlayShown = false;
        gameEnded = false;
        if (resignBtn != null) resignBtn.setEnabled(false);
        if (drawBtn != null) drawBtn.setEnabled(false);
        initBoardModel();
        updateBoardUI();
        SwingUtilities.invokeLater(() -> {
            resultOverlay.hideOverlay();
            CardLayout cl = (CardLayout) cards.getLayout();
            statusLabel.setVisible(false);
            cl.show(cards, CARD_WELCOME);
        });
        append("Returned to menu.");
    }

    private void onSquareClicked(int uiR, int uiC) {
        if (waitingForOk || gameEnded) return;
        int r = (myColor == 1) ? (7 - uiR) : uiR;
        int c = (myColor == 1) ? (7 - uiC) : uiC;
        if (selR == -1) {
            char p = board[r][c]; if (p == '.') return; if (!BoardModel.isOwnPiece(p, myColor)) return; if (!myTurn) return;
            selR = r; selC = c;
            highlighted.clear();
            for (int tr=0; tr<8; tr++) {
                for (int tc=0; tc<8; tc++) {
                    if (!isLegalMoveBasic(selR, selC, tr, tc)) continue;
                    if (moveLeavesInCheck(board, myColor, selR, selC, tr, tc)) continue;
                    highlighted.add(new Point(tr, tc));
                }
            }
            updateBoardUI();
        } else {
            if (selR==r && selC==c) { selR=selC=-1; highlighted.clear(); updateBoardUI(); return; }
            if (highlighted.contains(new Point(r,c))) {
                char moving = board[selR][selC];
                boolean isPawnPromotion = false;
                if (moving == 'P' && r == 0) isPawnPromotion = true;
                if (moving == 'p' && r == 7) isPawnPromotion = true;
                String from = Utils.coordToAlg(selR, selC);
                String to = Utils.coordToAlg(r, c);
                if (isPawnPromotion) {
                    SwingUtilities.invokeLater(() -> {
                        String[] options = {"Queen (Q)","Rook (R)","Bishop (B)","Knight (N)"};
                        int choice = JOptionPane.showOptionDialog(frame,
                                "Choose piece for promotion",
                                "Promotion",
                                JOptionPane.DEFAULT_OPTION,
                                JOptionPane.PLAIN_MESSAGE,
                                null,
                                options,
                                options[0]);
                        char promo = 'q';
                        if (choice == 1) promo = 'r';
                        else if (choice == 2) promo = 'b';
                        else if (choice == 3) promo = 'n';
                        pendingFrom = from;
                        pendingTo = to;
                        pendingPromo = promo;
                        if (networkClient != null) networkClient.sendMove(from + to + promo);
                        waitingForOk = true;
                    });
                } else {
                    pendingFrom = from;
                    pendingTo = to;
                    pendingPromo = 0;
                    if (networkClient != null) networkClient.sendMove(from + to);
                    waitingForOk = true;
                }
                selR = selC = -1;
                highlighted.clear();
                updateBoardUI();
                return;
            }
            char p = board[r][c];
            if (p != '.' && BoardModel.isOwnPiece(p, myColor)) {
                selR = r; selC = c;
                highlighted.clear();
                for (int tr=0; tr<8; tr++) for (int tc=0; tc<8; tc++)
                    if (isLegalMoveBasic(selR,selC,tr,tc) && !moveLeavesInCheck(board,myColor,selR,selC,tr,tc))
                        highlighted.add(new Point(tr,tc));
                updateBoardUI();
            } else {
                selR = selC = -1; highlighted.clear(); updateBoardUI();
            }
        }
    }

    private String coordToAlg(int r,int c) { return Utils.coordToAlg(r,c); }

    private void sendMove(String move) {
        if (networkClient == null) return;
        networkClient.sendMove(move);
        append(">> MOVE " + move);
    }

    private void applyLocalMove(String from, String to) {
        // delegate to boardModel
        boardModel.pendingPromo = pendingPromo;
        boardModel.applyLocalMove(from, to);
        // sync lastFrom/lastTo
        lastFrom = boardModel.lastFrom;
        lastTo = boardModel.lastTo;
        // update local copy
        this.board = boardModel.board;
        pendingPromo = 0;
        updateBoardUI();
    }

    private void applyOpponentMove(String mv) {
        boardModel.applyOpponentMove(mv);
        this.board = boardModel.board;
        lastFrom = boardModel.lastFrom;
        lastTo = boardModel.lastTo;
        myTurn = true;
        updateBoardUI();
    }

    private void parseServerMessage(String msg) {
        append("<< " + msg);
        if (msg == null) return;
        if (msg.startsWith("WELCOME") || msg.startsWith("SEND ")) {
            return;
        }
        if (msg.startsWith("WAITING")) {
            SwingUtilities.invokeLater(() -> append("Server: waiting for opponent..."));
            return;
        }
        if (msg.startsWith("MATCHMAKING_TIMEOUT")) {
            append("Matchmaking timed out (no opponent found).");
            matchmakingTimeoutReceived = true;
            javax.swing.Timer t = new javax.swing.Timer(500, ev -> {
                ((javax.swing.Timer)ev.getSource()).stop();
                int res = JOptionPane.showOptionDialog(frame,
                        "No opponent found. Retry searching for a match or return to the menu.",
                        "No opponent found",
                        JOptionPane.DEFAULT_OPTION,
                        JOptionPane.INFORMATION_MESSAGE,
                        null,
                        new String[] { "Retry", "Back to menu" },
                        "Retry");
                if (res == 0) {
                    matchmakingTimeoutReceived = false;
                    startConnection();
                    CardLayout cl = (CardLayout) cards.getLayout();
                    cl.show(cards, CARD_WAITING);
                } else {
                    matchmakingTimeoutReceived = false;
                    exitToWelcome();
                }
            });
            t.setRepeats(false);
            t.start();
            return;
        }
        if (msg.startsWith("START")) {
            SwingUtilities.invokeLater(() -> {
                append("Game started!");
                String[] p = msg.split("\\s+");
                String color = null;
                if (p.length >= 3) {
                    // format: START <opponent> <white|black>
                    opponentName = p[1];
                    color = p[p.length - 1];
                } else if (p.length == 2) {
                    color = p[1];
                    opponentName = "";
                } else {
                    opponentName = "";
                }

                if (color != null && color.equalsIgnoreCase("white")) {
                    myColor = 0;
                    myTurn = true;
                } else {
                    myColor = 1;
                    myTurn = false;
                }
                initBoardModel();
                updateBoardUI();
                CardLayout cl = (CardLayout) cards.getLayout();
                cl.show(cards, CARD_GAME);
                statusLabel.setVisible(true);

                // enable resign/draw now that game is active (buttons still exist logically, but are not shown)
                if (resignBtn != null) resignBtn.setEnabled(true);
                if (drawBtn != null) drawBtn.setEnabled(true);

                // update the top status label
            });
            return;
        }
        if (msg.startsWith("OPPONENT_MOVE")) {
            String[] parts = msg.split("\\s+");
            if (parts.length >= 2) {
                String move = parts[1];
                applyOpponentMove(move);
                append("Opponent: " + move);
                waitingForOk = false;
            } else {
                append("Malformed OPPONENT_MOVE: " + msg);
            }
            return;
        }
        if (msg.startsWith("OK_MOVE")) {
            if (pendingFrom != null && pendingTo != null) {
                applyLocalMove(pendingFrom, pendingTo);
                append("Move accepted: " + pendingFrom + pendingTo);
                pendingFrom = pendingTo = null;
                waitingForOk = false;
                myTurn = false;
            } else {
                append("OK_MOVE received but no pending move");
            }
            return;
        }
        if (msg.startsWith("ERROR")) {
            append(msg);
            waitingForOk = false;
            pendingFrom = pendingTo = null;
            pendingPromo = 0;
            return;
        }
        if (msg.startsWith("CHECKMATE_WIN")) {
            append("You won by checkmate");
            showEndOverlay(true, "Checkmate");
            return;
        }
        if (msg.startsWith("CHECKMATE")) {
            append("Checkmate — you lost");
            showEndOverlay(false, "Checkmate");
            return;
        }
        if (msg.startsWith("CHECK")) {
            append("You are in check!");
            return;
        }
        if (msg.startsWith("OPPONENT_RESIGNED")) {
            append("Opponent resigned");
            showEndOverlay(true, "Opponent resigned");
            return;
        }
        if (msg.startsWith("OPPONENT_QUIT")) {
            append("Opponent disconnected");
            showEndOverlay(true, "Opponent quit the game");
            return;
        }
        if (msg.startsWith("RESIGN")) {
            append("You resigned");
            showEndOverlay(false, "You resigned");
            return;
        }
        if (msg.startsWith("STALEMATE")) {
            append("Game drawn by stalemate");
            showNeutralOverlay("Draw (stalemate)");
            return;
        }
        if (msg.startsWith("DRAW_OFFER")) {
            drawOfferPending = true;
            SwingUtilities.invokeLater(() -> {
                int choice = JOptionPane.showConfirmDialog(frame,
                        "Opponent offers a draw. Accept?",
                        "Draw offer",
                        JOptionPane.YES_NO_OPTION);
                if (choice == JOptionPane.YES_OPTION) {
                    if (networkClient != null) {
                        networkClient.sendRaw("DRAW_ACCEPT");
                        append(">> DRAW_ACCEPT");
                    }
                    drawOfferPending = false;
                } else {
                    if (networkClient != null) {
                        networkClient.sendRaw("DRAW_DECLINE");
                        append(">> DRAW_DECLINE");
                    } else {
                        append("You declined the draw offer.");
                    }
                    drawOfferPending = false;
                }
            });
            return;
        }
        if (msg.startsWith("DRAW_ACCEPTED")) {
            drawOfferPending = false;
            showNeutralOverlay("DRAW");
            return;
        }
        if (msg.startsWith("DRAW_DECLINED")) {
            drawOfferPending = false;
            SwingUtilities.invokeLater(() -> {
                append("Draw offer was declined by opponent.");
                JOptionPane.showMessageDialog(frame, "Your draw offer was declined.", "Draw declined", JOptionPane.INFORMATION_MESSAGE);
                if (drawBtn != null && !gameEnded) drawBtn.setEnabled(true);
            });
            return;
        }
        if (msg.startsWith("OPPONENT_TIMEOUT")) {
            append("Opponent timed out");
            showEndOverlay(true, "Opponent timed out");
            return;
        }
        if (msg.startsWith("TIMEOUT")) {
            append("You timed out");
            showEndOverlay(false, "You timed out");
            return;
        }
        append("Unhandled server msg: " + msg);
    }

    private void showEndOverlay(boolean isWinner, String subtitle) {
        SwingUtilities.invokeLater(() -> {
            endOverlayShown = true;
            gameEnded = true;
            if (resignBtn != null) resignBtn.setEnabled(false);
            if (drawBtn != null) drawBtn.setEnabled(false);
            resultOverlay.showEndOverlay(isWinner, subtitle);
            Rectangle b = frame.getContentPane().getBounds();
            resultOverlay.setBounds(b);
            resultOverlay.setInnerBounds(0,0,b.width,b.height);
            resultOverlay.revalidate();
            resultOverlay.repaint();
        });
    }

    private void showNeutralOverlay(String text) {
        SwingUtilities.invokeLater(() -> {
            if (endOverlayShown) return;
            endOverlayShown = true;
            gameEnded = true;
            if (resignBtn != null) resignBtn.setEnabled(false);
            if (drawBtn != null) drawBtn.setEnabled(false);
            resultOverlay.showNeutralOverlay(text);
            Rectangle b = frame.getContentPane().getBounds();
            resultOverlay.setBounds(b);
            resultOverlay.setInnerBounds(0,0,b.width,b.height);
        });
    }

    private void append(String s) {
        // Print logs to stdout instead of the in-game text area.
        // Avoiding Swing EDT dispatch here — System.out.println is synchronized and safe from any thread.
        System.out.println(s);
    }

/*
    private void sendInput() {
        String txt = input.getText().trim();
        if (txt.isEmpty() || networkClient == null) return;

        String upper = txt.trim().toUpperCase();
        if (upper.equals("DRAW_ACCEPT") || upper.equals("DRAW_DECLINE")) {
            if (!drawOfferPending) {
                append("Cannot send " + txt + ": no draw offer pending.");
                input.setText("");
                return;
            }
        }
        networkClient.sendRaw(txt);
        append(">> " + txt);
        input.setText("");
    }*/
}
