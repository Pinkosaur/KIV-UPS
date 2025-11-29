import javax.imageio.ImageIO;
import javax.swing.*;
import java.awt.*;
import java.awt.event.ComponentAdapter;
import java.awt.event.ComponentEvent;
import java.awt.event.MouseAdapter;
import java.awt.event.MouseEvent;
import java.awt.image.BufferedImage;
import java.io.*;
import java.net.*;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

public class ChessClientGUI {
    // default host; will be overriden by the text field on the welcome screen
    private static final int PORT = 10001;
    private String serverHost = "127.0.0.1";
    private JTextField serverIpField; // welcome-screen input for server IP
    private static final String PIECES_DIR = "client/pieces"; // relative to working dir
    private static final String BACK_DIR = "client/backgrounds"; // placeholders
    private static final int SQUARE_SIZE = 64;
    private String clientName;
    // --- Random name generator data ---
    private static final String[] NAME_ADJECTIVES = {"Brilliant","Interesting","Amazing","Quick","Clever","Curious","Lucky","Silent","Bold","Fierce","Gentle","Merry","Young","Old","Special","Talented"};
    private static final String[] NAME_NOUNS = {"Magician","Llama","Explorer","Fox","Pioneer","Wanderer","Scholar","Knight","Builder","Artist","Nimbus"};

    private String generateRandomName() {
        java.util.Random rnd = new java.util.Random();
        String adj = NAME_ADJECTIVES[rnd.nextInt(NAME_ADJECTIVES.length)];
        String noun = NAME_NOUNS[rnd.nextInt(NAME_NOUNS.length)];
        int num = rnd.nextInt(100); // 0..99
        return adj + noun + num;
    }
    private JTextField nameField;

    // waiting-for-match timeout (milliseconds)
    //private static final int WAIT_MATCH_TIMEOUT_MS = 10_000;
    //private javax.swing.Timer waitTimer;
    private volatile boolean matchmakingTimeoutReceived = false;

    private JFrame frame;
    private JTextArea log;
    private JTextField input;
    private JButton sendBtn;
    private JLabel statusLabel;

    private Socket socket;
    private BufferedReader in;
    private PrintWriter out;
    private Thread readerThread;

    private JPanel cards; // CardLayout for welcome/waiting/game
    private static final String CARD_WELCOME = "welcome";
    private static final String CARD_WAITING = "waiting";
    private static final String CARD_GAME = "game";

    private JPanel boardPanel;
    private SquareLabel[][] squares = new SquareLabel[8][8];
    private char[][] board = new char[8][8];

    private Map<Character, ImageIcon> pieceIcons = new HashMap<>();
    // raw piece images (original sized) and a cache of scaled icons by size
    private final Map<Character, BufferedImage> pieceImagesRaw = new HashMap<>();
    private final Map<Integer, Map<Character, ImageIcon>> iconCacheBySize = new HashMap<>();

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

    
    /* en-passant target (model coordinates) - square a capturing pawn would move to */
    private int epR = -1, epC = -1;

    // result UI (overlay)
    private JPanel overlayPanel;         // covers the window with transparent color
    private JPanel overlayColorPanel;    // colored semi-transparent panel inside overlay
    private JLabel overlayTitle;
    private JLabel overlaySubtitle;
    private JButton overlayContinue;

    public static void main(String[] args) {
        SwingUtilities.invokeLater(() -> {
            try {
                new ChessClientGUI().createAndShowGUI();
            } catch (Exception e) {
                e.printStackTrace();
            }
        });
    }

    // --- UI component that can draw red circle behind piece when in check ---
    private static class SquareLabel extends JLabel {
        private boolean drawCheckCircle = false;
        public SquareLabel(String t, int align) { super(t, align); setOpaque(true); }
        public void setDrawCheckCircle(boolean v) { drawCheckCircle = v; repaint(); }
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
                    int diameter = (int)(Math.min(w, h) * 0.85);
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

    private void loadIcons() {
        Map<Character, String> map = new HashMap<>();
        map.put('P', "white-pawn.png");
        map.put('N', "white-knight.png");
        map.put('B', "white-bishop.png");
        map.put('R', "white-rook.png");
        map.put('Q', "white-queen.png");
        map.put('K', "white-king.png");
        map.put('p', "black-pawn.png");
        map.put('n', "black-knight.png");
        map.put('b', "black-bishop.png");
        map.put('r', "black-rook.png");
        map.put('q', "black-queen.png");
        map.put('k', "black-king.png");

        pieceImagesRaw.clear();
        iconCacheBySize.clear();

        for (Map.Entry<Character, String> e : map.entrySet()) {
            File f = new File(PIECES_DIR, e.getValue());
            if (f.exists()) {
                try {
                    BufferedImage img = ImageIO.read(f);
                    pieceImagesRaw.put(e.getKey(), img);
                } catch (IOException ex) {
                    System.err.println("Failed to load " + f + " : " + ex.getMessage());
                }
            } else {
                System.err.println("Piece image not found: " + f.getPath());
            }
        }
    }

    private ImageIcon getScaledIconForPiece(char p, int cellSize) {
        if (p == '.' || cellSize <= 0) return null;
        Map<Character, ImageIcon> cache = iconCacheBySize.get(cellSize);
        if (cache != null && cache.containsKey(p)) return cache.get(p);

        BufferedImage src = pieceImagesRaw.get(p);
        if (src == null) return null;

        Image scaled = src.getScaledInstance(cellSize, cellSize, Image.SCALE_SMOOTH);
        ImageIcon ico = new ImageIcon(scaled);

        if (cache == null) {
            cache = new HashMap<>();
            iconCacheBySize.put(cellSize, cache);
        }
        cache.put(p, ico);
        return ico;
    }

    private void initBoardModel() {
        char[] backBlack = {'r','n','b','q','k','b','n','r'};
        char[] backWhite = {'R','N','B','Q','K','B','N','R'};
        for (int c = 0; c < 8; c++) board[0][c] = backBlack[c];
        for (int c = 0; c < 8; c++) board[1][c] = 'p';
        for (int r = 2; r < 6; r++) for (int c = 0; c < 8; c++) board[r][c] = '.';
        for (int c = 0; c < 8; c++) board[6][c] = 'P';
        for (int c = 0; c < 8; c++) board[7][c] = backWhite[c];
    }

    private void updateBoardUI() {
        SwingUtilities.invokeLater(() -> {
            computeKingCheck();
            for (int uiR = 0; uiR < 8; uiR++) {
                for (int uiC = 0; uiC < 8; uiC++) {
                    /* map UI coordinates to model coordinates depending on POV */
                    int modelR = (myColor == 1) ? (7 - uiR) : uiR;
                    int modelC = (myColor == 1) ? (7 - uiC) : uiC;

                    char p = board[modelR][modelC];

                    // compute cell size based on actual boardPanel size
                    int boardPx = Math.min(boardPanel.getWidth(), boardPanel.getHeight());
                    int cellSize = (boardPx > 0) ? (boardPx / 8) : SQUARE_SIZE;

                    // get scaled icon for this piece and size
                    ImageIcon icon = getScaledIconForPiece(p, cellSize);
                    squares[uiR][uiC].setIcon(icon);
                    squares[uiR][uiC].setText((icon == null) ? (p=='.'?"":""+p) : "");

                    // last-move highlight (yellow) — shown with precedence over normal borders,
                    // but selection (red) takes visual precedence if same square is selected.
                    boolean isLastFrom = (lastFrom != null && lastFrom.x == modelR && lastFrom.y == modelC);
                    boolean isLastTo   = (lastTo   != null && lastTo.x   == modelR && lastTo.y   == modelC);

                    if (selR == modelR && selC == modelC) {
                        // selected square (highest visual priority)
                        squares[uiR][uiC].setBorder(BorderFactory.createLineBorder(Color.RED, 3));
                    } else if (highlighted.contains(new Point(modelR, modelC))) {
                        squares[uiR][uiC].setBorder(BorderFactory.createLineBorder(Color.GREEN, 3));
                    } else if (isLastFrom || isLastTo) {
                        // last move squares highlighted in yellow
                        squares[uiR][uiC].setBorder(BorderFactory.createLineBorder(Color.YELLOW, 3));
                    } else {
                        squares[uiR][uiC].setBorder(BorderFactory.createLineBorder(Color.BLACK, 1));
                    }

                    /* draw red check circle behind the king if this square is the king in model coords */
                    squares[uiR][uiC].setDrawCheckCircle(modelR == kingR && modelC == kingC && kingInCheck);
                }
            }
            if (statusLabel != null) statusLabel.setText("Color: " + (myColor==0?"WHITE":myColor==1?"BLACK":"?") + "   " + (myTurn?"Your turn":"Opponent"));
        });
    }


    private void computeKingCheck() {
        if (myColor != 0 && myColor != 1) { kingInCheck = false; kingR = kingC = -1; return; }
        int[] pos = findKing(board, myColor);
        if (pos == null) { kingInCheck = false; kingR = kingC = -1; return; }
        kingR = pos[0]; kingC = pos[1];
        kingInCheck = isSquareAttacked(board, kingR, kingC, 1 - myColor);
    }

    /* --- board helpers mirrored from server --- */
    private boolean inBounds(int r,int c){ return r>=0 && r<8 && c>=0 && c<8; }

    private boolean pathClear(char[][] b, int r1,int c1,int r2,int c2) {
        int dr = (r2>r1)?1: (r2<r1)?-1:0;
        int dc = (c2>c1)?1: (c2<c1)?-1:0;
        int r = r1 + dr, c = c1 + dc;
        while (r != r2 || c != c2) {
            if (!inBounds(r,c)) return false;
            if (b[r][c] != '.') return false;
            r += dr; c += dc;
        }
        return true;
    }

    private boolean isLegalMoveBasic(int r1,int c1,int r2,int c2) {
        if (!inBounds(r1,c1) || !inBounds(r2,c2)) return false;
        char p = board[r1][c1];
        if (p=='.') return false;
        char t = board[r2][c2];
        if (t!='.' && (Character.isUpperCase(t)==Character.isUpperCase(p))) return false;
        int dr = r2 - r1; int dc = c2 - c1; int absdr = Math.abs(dr), absdc = Math.abs(dc);

        if (p=='P') {
            if (c1==c2 && board[r2][c2]=='.') { if (dr==-1) return true; if (r1==6 && dr==-2 && board[5][c1]=='.') return true; }
            if (absdc==1 && dr==-1 && t!='.' && Character.isLowerCase(t)) return true;
            /* en-passant capture (client-side detection): diagonal to empty square that equals ep target */
            if (absdc==1 && dr==-1 && t=='.' && epR==r2 && epC==c2) return true;
            return false;
        }
        if (p=='p') {
            if (c1==c2 && board[r2][c2]=='.') { if (dr==1) return true; if (r1==1 && dr==2 && board[2][c1]=='.') return true; }
            if (absdc==1 && dr==1 && t!='.' && Character.isUpperCase(t)) return true;
            if (absdc==1 && dr==1 && t=='.' && epR==r2 && epC==c2) return true;
            return false;
        }
        if (p=='N' || p=='n') return (absdr==2 && absdc==1) || (absdr==1 && absdc==2);
        if (p=='B' || p=='b') { if (absdr!=absdc) return false; return pathClear(board,r1,c1,r2,c2); }
        if (p=='R' || p=='r') { if (dr!=0 && dc!=0) return false; return pathClear(board,r1,c1,r2,c2); }
        if (p=='Q' || p=='q') { if (absdr==absdc || dr==0 || dc==0) return pathClear(board,r1,c1,r2,c2); return false; }

        /* King: include castling (client-side permissive check).
        We can't know server-side castling rights here, but we can allow castling when:
        - king on original square, rook present on expected square,
        - path squares between king and rook are empty.
        Final legality (king-in-check, passing-through-check) will be filtered-out by moveLeavesInCheck. */
        if (p=='K' || p=='k') {
            if (absdr<=1 && absdc<=1) return true;
            /* castling attempt: two-square horizontal move */
            if (r1 == r2 && absdc == 2) {
                // white original: r==7,c==4; black original: r==0,c==4
                if (p == 'K') {
                    int color = 0; int opp = 1;
                    if (!(r1 == 7 && c1 == 4)) return false;
                    // kingside
                    if (c2 == 6) {
                        if (board[7][7] != 'R') return false;
                        if (board[7][5] != '.' || board[7][6] != '.') return false;
                        // additional checks: king not currently in check, and passing/landing squares not attacked
                        if (isSquareAttacked(board, r1, c1, opp)) return false; // currently in check -> no castle
                        // check the square the king passes through (f-file) and the destination (g-file)
                        int midC = 5;
                        if (moveLeavesInCheck(board, color, r1, c1, r1, midC)) return false; // would be in check while passing
                        if (moveLeavesInCheck(board, color, r1, c1, r2, c2)) return false; 
                            return true;
                    }
                    // queenside
                    if (c2 == 2) {
                        if (board[7][0] != 'R') return false;
                        if (board[7][1] != '.' || board[7][2] != '.' || board[7][3] != '.') return false;
                        if (isSquareAttacked(board, r1, c1, opp)) return false;
                        int midC = 3;
                        if (moveLeavesInCheck(board, color, r1, c1, r1, midC)) return false;
                        if (moveLeavesInCheck(board, color, r1, c1, r2, c2)) return false;
                        return true;
                    }
                } else {
                    int color = 1; int opp = 0;
                    if (!(r1 == 0 && c1 == 4)) return false;
                    if (c2 == 6) {
                        if (board[0][7] != 'r') return false;
                        if (board[0][5] != '.' || board[0][6] != '.') return false;
                        if (isSquareAttacked(board, r1, c1, opp)) return false;
                        int midC = 5;
                        if (moveLeavesInCheck(board, color, r1, c1, r1, midC)) return false;
                        if (moveLeavesInCheck(board, color, r1, c1, r2, c2)) return false;
                        return true;
                    }
                    if (c2 == 2) {
                        if (board[0][0] != 'r') return false;
                        if (board[0][1] != '.' || board[0][2] != '.' || board[0][3] != '.') return false;
                        if (isSquareAttacked(board, r1, c1, opp)) return false;
                        int midC = 3;
                        if (moveLeavesInCheck(board, color, r1, c1, r1, midC)) return false;
                        if (moveLeavesInCheck(board, color, r1, c1, r2, c2)) return false;
                        return true;
                    }
                }
            }
            return false;
        }
        return false;
    }


    private boolean isSquareAttacked(char[][] b, int r, int c, int byColor) {
        for (int r0=0;r0<8;r0++) for (int c0=0;c0<8;c0++) {
            char p = b[r0][c0];
            if (p=='.') continue;
            int pColor = Character.isUpperCase(p)?0:1;
            if (pColor != byColor) continue;
            int dr = r - r0; int dc = c - c0; int absdr = Math.abs(dr), absdc = Math.abs(dc);
            switch (p) {
                case 'P': if (r == r0-1 && (c==c0-1 || c==c0+1)) return true; break;
                case 'p': if (r == r0+1 && (c==c0-1 || c==c0+1)) return true; break;
                case 'N': case 'n': if ((absdr==2 && absdc==1) || (absdr==1 && absdc==2)) return true; break;
                case 'B': case 'b': if (absdr==absdc && pathClear(b,r0,c0,r,c)) return true; break;
                case 'R': case 'r': if ((dr==0 || dc==0) && pathClear(b,r0,c0,r,c)) return true; break;
                case 'Q': case 'q': if ((absdr==absdc || dr==0 || dc==0) && pathClear(b,r0,c0,r,c)) return true; break;
                case 'K': case 'k': if (absdr<=1 && absdc<=1) return true; break;
                default: break;
            }
        }
        return false;
    }

    private int[] findKing(char[][] b, int color) {
        char target = (color==0)?'K':'k';
        for (int r=0;r<8;r++) for (int c=0;c<8;c++) if (b[r][c]==target) return new int[]{r,c};
        return null;
    }

    private boolean moveLeavesInCheck(char[][] b, int color, int r1,int c1,int r2,int c2) {
        char[][] copy = new char[8][8];
        for (int r=0;r<8;r++) System.arraycopy(b[r],0,copy[r],0,8);
        char p = copy[r1][c1];

        /* En-passant capture simulation: pawn moves diagonally to empty square -> remove captured pawn behind it */
        if ((p == 'P' || p == 'p') && copy[r2][c2] == '.' && c1 != c2) {
            int capR = (p == 'P') ? r2 + 1 : r2 - 1;
            if (inBounds(capR, c2)) copy[capR][c2] = '.';
        }

        /* Castling simulation: if king moves two squares horizontally, also move the rook */
        if ((p == 'K' || p == 'k') && r1 == r2 && Math.abs(c2 - c1) == 2) {
            /* move king */
            copy[r2][c2] = p; copy[r1][c1] = '.';
            if (c2 - c1 == 2) {
                /* kingside: rook from file 7 -> file 5 */
                copy[r2][5] = copy[r2][7];
                copy[r2][7] = '.';
            } else {
                /* queenside: rook from file 0 -> file 3 */
                copy[r2][3] = copy[r2][0];
                copy[r2][0] = '.';
            }
            int[] kingPos = findKing(copy, color);
            if (kingPos == null) return false;
            return isSquareAttacked(copy, kingPos[0], kingPos[1], 1-color);
        }

        /* Normal move */
        copy[r2][c2] = copy[r1][c1];
        copy[r1][c1] = '.';

        /* promotions - approximate: if pawn reaches last rank promote to queen to be conservative */
        if (p=='P' && r2==0) copy[r2][c2] = 'Q';
        if (p=='p' && r2==7) copy[r2][c2] = 'q';

        int[] kingPos = findKing(copy, color);
        if (kingPos==null) return false;
        return isSquareAttacked(copy, kingPos[0], kingPos[1], 1-color);
    }


    // --- UI and networking ---
    private void createAndShowGUI() {
        loadIcons();
        clientName = generateRandomName();

        frame = new JFrame("Chess Client");
        // Use DO_NOTHING so we can perform clean shutdown and then exit.
        frame.setDefaultCloseOperation(JFrame.DO_NOTHING_ON_CLOSE);
        frame.addWindowListener(new java.awt.event.WindowAdapter() {
            @Override
            public void windowClosing(java.awt.event.WindowEvent e) {
                // Make closing idempotent (ignore repeated clicks)
                if (intentionalDisconnect) return;
                intentionalDisconnect = true;

                // Best-effort: notify server if we have an open writer.
                try {
                    if (out != null) {
                        out.println("QUIT");
                        out.flush();
                    } else if (socket != null && !socket.isClosed()) {
                        // If we don't have a PrintWriter, try to shutdown socket output to wake server/threads.
                        try { socket.shutdownOutput(); } catch (IOException ignored) {}
                    }
                } catch (Exception ignored) {
                    // ignore any network errors during shutdown
                }

                // Close connection resources (safe if already closed)
                closeConnection();

                // Dispose UI and terminate JVM so no stray threads keep process alive.
                // We run on EDT, so perform final disposal here and then exit.
                SwingUtilities.invokeLater(() -> {
                    try { frame.dispose(); } catch (Exception ignored) {}
                    // forcing exit avoids cases where AWT/other threads keep the process alive.
                    System.exit(0);
                });
            }
        });

        // top-level layout: card layout center
        cards = new JPanel(new CardLayout());

        // --- WELCOME card ---
        JPanel welcome = new JPanel(new BorderLayout());
        JLabel welcomeBg = loadBackgroundLabel(BACK_DIR + "/welcome.jpg", new Color(30,30,60));
        welcome.add(welcomeBg, BorderLayout.CENTER);
        // server IP input + find button panel
        JPanel wc = new JPanel();
        wc.setOpaque(false);
        wc.setLayout(new BoxLayout(wc, BoxLayout.Y_AXIS));

        // name row
        JPanel nameRow = new JPanel(new FlowLayout(FlowLayout.CENTER, 8, 6));
        nameRow.setOpaque(false);
        nameRow.add(new JLabel("Your name:"));
        nameField = new JTextField(clientName, 16);
        nameRow.add(nameField);
        wc.add(nameRow);

        // server IP row
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


        // --- WAITING card ---
        JPanel waiting = new JPanel(new BorderLayout());
        JLabel waitingBg = loadBackgroundLabel(BACK_DIR + "/waiting.png", new Color(40,40,40));
        waiting.add(waitingBg, BorderLayout.CENTER);
        JLabel waitingLbl = new JLabel("Searching for opponent...", SwingConstants.CENTER);
        waitingLbl.setForeground(Color.WHITE);
        waiting.add(waitingLbl, BorderLayout.SOUTH);

        // --- GAME card (board + right pane) ---
        JPanel gameCard = new JPanel(new BorderLayout());

        // --- create square-preserving board panel + wrapper ---
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

        // wrapper that keeps the boardPanel square and centered
        boardContainer = new JPanel(new GridBagLayout());
        boardContainer.setOpaque(false);

        // initial preferred size for the board (so it shows before first resize)
        int initialBoardPx = SQUARE_SIZE * 8;
        boardPanel.setPreferredSize(new Dimension(initialBoardPx, initialBoardPx));

        // add boardPanel centered within boardContainer (GridBagLayout honors preferred size)
        GridBagConstraints gbc = new GridBagConstraints();
        gbc.gridx = 0; gbc.gridy = 0;
        gbc.anchor = GridBagConstraints.CENTER;
        gbc.fill = GridBagConstraints.NONE;
        boardContainer.add(boardPanel, gbc);

        // on resize, enforce a square preferred size (with padding) and revalidate
        boardContainer.addComponentListener(new ComponentAdapter() {
            @Override
            public void componentResized(ComponentEvent e) {
                int w = boardContainer.getWidth();
                int h = boardContainer.getHeight();
                // padding in pixels to leave some window margin; tweak as desired
                int padding = 40;
                int size = Math.min(w, h) - padding;
                if (size < 40) size = 40; // minimum sensible size
                // ensure dimension is multiple of 8 so cell sizes are integral
                size = (size / 8) * 8;
                boardPanel.setPreferredSize(new Dimension(size, size));
                boardPanel.revalidate();
                // clear icon cache for new size (avoid stale scaled icons)
                iconCacheBySize.remove(size);
                updateBoardUI();
            }
        });

        JPanel right = new JPanel(new BorderLayout());
        log = new JTextArea(10,20); log.setEditable(false);
        statusLabel = new JLabel("Not connected");
        JPanel topRight = new JPanel(new BorderLayout()); topRight.add(statusLabel, BorderLayout.NORTH);
        topRight.add(new JScrollPane(log), BorderLayout.CENTER);
        
        JPanel bottom = new JPanel(new BorderLayout());
        input = new JTextField();
        sendBtn = new JButton("Send");
        bottom.add(input, BorderLayout.CENTER);
        bottom.add(sendBtn, BorderLayout.EAST);

        // Extra controls: Resign and Draw Offer
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
            if (ok == JOptionPane.YES_OPTION && out != null) {
                out.println("RESIGN");
                append(">> RESIGN");
                // disable controls until server replies
                resignBtn.setEnabled(false);
                drawBtn.setEnabled(false);
            }
        });

        drawBtn.addActionListener(e -> {
            if (out != null) {
                out.println("DRAW_OFFER");
                append(">> DRAW_OFFER");
                // disable the draw button until opponent responds (avoid spamming)
                drawBtn.setEnabled(false);
            }
        });

        // place controls under the input
        JPanel bottomWrap = new JPanel(new BorderLayout());
        bottomWrap.add(bottom, BorderLayout.NORTH);
        bottomWrap.add(ctrl, BorderLayout.SOUTH);

        cards = new JPanel(new CardLayout());
        sendBtn.addActionListener(e -> sendInput()); input.addActionListener(e -> sendInput());
        right.add(topRight, BorderLayout.CENTER);
        right.add(bottomWrap, BorderLayout.SOUTH);
        gameCard.add(bottomWrap, BorderLayout.SOUTH);
        // add the board container (not raw boardPanel) so the board remains square
        gameCard.add(boardContainer, BorderLayout.CENTER); 
        gameCard.add(right, BorderLayout.EAST);

        cards.add(welcome, CARD_WELCOME);
        cards.add(waiting, CARD_WAITING);
        cards.add(gameCard, CARD_GAME);

        frame.getContentPane().add(cards, BorderLayout.CENTER);

        // create overlay (initially hidden)
        overlayPanel = new JPanel(null);
        overlayPanel.setOpaque(false);
        overlayPanel.setVisible(false);

        overlayColorPanel = new JPanel();
        overlayColorPanel.setOpaque(true);
        overlayColorPanel.setBackground(new Color(0,0,0,120));
        overlayColorPanel.setLayout(new GridBagLayout());

        overlayTitle = new JLabel("", SwingConstants.CENTER);
        overlayTitle.setForeground(Color.WHITE);
        overlayTitle.setFont(overlayTitle.getFont().deriveFont(48f).deriveFont(Font.BOLD));

        // subtitle below the title (smaller)
        overlaySubtitle = new JLabel("", SwingConstants.CENTER);
        overlaySubtitle.setForeground(Color.WHITE);
        overlaySubtitle.setFont(overlaySubtitle.getFont().deriveFont(18f));

        overlayContinue = new JButton("Continue");
        overlayContinue.setFocusPainted(false);
        overlayContinue.addActionListener(e -> {
            exitToWelcome();
        });

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
        overlayPanel.add(overlayColorPanel);

        // add overlay to layered pane so it sits above cards
        frame.getLayeredPane().add(overlayPanel, JLayeredPane.MODAL_LAYER);

        // ensure overlay sizes match content area on resize
        frame.addComponentListener(new ComponentAdapter() {
            @Override
            public void componentResized(ComponentEvent e) {
                Rectangle b = frame.getContentPane().getBounds();
                overlayPanel.setBounds(b);
                overlayColorPanel.setBounds(0,0,b.width,b.height);
            }
            @Override
            public void componentShown(ComponentEvent e) {
                Rectangle b = frame.getContentPane().getBounds();
                overlayPanel.setBounds(b);
                overlayColorPanel.setBounds(0,0,b.width,b.height);
            }
        });

        frame.pack(); frame.setLocationRelativeTo(null); frame.setVisible(true);

        // set initial overlay bounds now that frame is visible
        Rectangle b = frame.getContentPane().getBounds();
        overlayPanel.setBounds(b);
        overlayColorPanel.setBounds(0,0,b.width,b.height);

        // after frame visible, run a one-shot invokeLater to let layout compute and apply square sizing
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
            try { BufferedImage img = ImageIO.read(f); return new JLabel(new ImageIcon(img)); }
            catch (IOException e) { }
        }
        JLabel p = new JLabel(); p.setOpaque(true); p.setBackground(fallback); return p;
    }

    private void onFindMatchClicked() {
        CardLayout cl = (CardLayout) cards.getLayout(); cl.show(cards, CARD_WAITING);

        // read server host from text field (trim) and fallback to default
        if (serverIpField != null) {
            String entered = serverIpField.getText().trim();
            if (!entered.isEmpty()) serverHost = entered;
        }
        if (nameField != null) {
            String n = nameField.getText().trim();
            if (!n.isEmpty()) clientName = n;
        }

        log = (log == null) ? new JTextArea() : log; // ensure exists
        startConnection();
    }

    private void startConnection() {
        // close existing if any
        closeConnection();
        matchmakingTimeoutReceived = false;
        intentionalDisconnect = false;
        readerThread = new Thread(this::connect, "ChessClient-Reader");
        readerThread.setDaemon(true); // don't keep JVM alive solely for this thread
        readerThread.start();
    }

    private void closeConnection() {
        try { if (socket != null) socket.close(); } catch (Exception ignored) {}
        socket = null; in = null; out = null;
        if (readerThread != null) { readerThread.interrupt(); readerThread = null; }
    }

    private void exitToWelcome() {
        matchmakingTimeoutReceived = false;

        // Reset game state
        myColor = -1; 
        myTurn = false; 
        selR = selC = -1; 
        highlighted.clear(); 
        pendingFrom = pendingTo = null; 
        lastFrom = lastTo = null;
        waitingForOk = false;
        endOverlayShown = false;
        gameEnded = false;  // Reset for next game
        if (resignBtn != null) resignBtn.setEnabled(false);
        if (drawBtn != null) drawBtn.setEnabled(false);

        
        initBoardModel(); 
        updateBoardUI();
        
        // Hide overlay and switch card
        SwingUtilities.invokeLater(() -> {
            overlayTitle.setText("");
            overlaySubtitle.setText("");
            overlayPanel.setVisible(false);
            CardLayout cl = (CardLayout) cards.getLayout(); 
            cl.show(cards, CARD_WELCOME);
        });
        
        append("Returned to menu.");
    }


    private void onSquareClicked(int uiR, int uiC) {
        if (waitingForOk || gameEnded) return;

        /* map UI coords to model coords depending on POV */
        int r = (myColor == 1) ? (7 - uiR) : uiR;
        int c = (myColor == 1) ? (7 - uiC) : uiC;

        if (selR == -1) {
            char p = board[r][c]; if (p == '.') return; if (!isOwnPiece(p)) return; if (!myTurn) return;
            selR = r; selC = c;
            highlighted.clear();

            for (int tr=0; tr<8; tr++) {
                for (int tc=0; tc<8; tc++) {
                    if (!isLegalMoveBasic(selR, selC, tr, tc)) continue;
                    /* skip moves that leave us in check */
                    if (moveLeavesInCheck(board, myColor, selR, selC, tr, tc)) continue;
                    highlighted.add(new Point(tr, tc));
                }
            }
            updateBoardUI();
        } else {
            if (selR==r && selC==c) { selR=selC=-1; highlighted.clear(); updateBoardUI(); return; }

            if (highlighted.contains(new Point(r,c))) {
                /* If moving pawn to last rank -> ask for promotion piece first */
                char moving = board[selR][selC];
                boolean isPawnPromotion = false;
                if (moving == 'P' && r == 0) isPawnPromotion = true;
                if (moving == 'p' && r == 7) isPawnPromotion = true;

                String from = coordToAlg(selR, selC);
                String to = coordToAlg(r, c);

                if (isPawnPromotion) {
                    /* show promotion selector dialog on EDT and send chosen promotion */
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
                        /* send lowercase promo (server accepts either case) */
                        pendingFrom = from;
                        pendingTo = to;
                        pendingPromo = promo; 
                        sendMove(from + to + promo);
                        waitingForOk = true;
                    });
                } else {
                    pendingFrom = from;
                    pendingTo = to;
                    pendingPromo = 0;
                    sendMove(from + to);
                    waitingForOk = true;
                }
                selR = selC = -1;
                highlighted.clear();
                updateBoardUI();
                return;
            }

            char p = board[r][c];
            if (p != '.' && isOwnPiece(p)) {
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



    private boolean isOwnPiece(char p) { if (p=='.') return false; if (myColor==0) return Character.isUpperCase(p); if (myColor==1) return Character.isLowerCase(p); return false; }
    private String coordToAlg(int r,int c) { char file = (char)('a'+c); char rank = (char)('0' + (8 - r)); return "" + file + rank; }

    private void sendMove(String move) { if (out==null) return; out.println("MOVE " + move); append(">> MOVE " + move); }

    private void applyLocalMove(String from, String to) {
        int r1 = 8 - (from.charAt(1) - '0');
        int c1 = from.charAt(0) - 'a';
        int r2 = 8 - (to.charAt(1) - '0');
        int c2 = to.charAt(0) - 'a';

        // record last move (model coords)
        lastFrom = new Point(r1, c1);
        lastTo = new Point(r2, c2);

        char piece = board[r1][c1];
        board[r1][c1] = '.';

        // Castling: if king moved two squares horizontally, move corresponding rook
        if ((piece == 'K' || piece == 'k') && r1 == r2 && Math.abs(c2 - c1) == 2) {
            // place king
            board[r2][c2] = piece;
            // move rook
            if (c2 > c1) {
                // kingside: rook from file 7 -> file 5
                int rookR = r2, rookFromC = 7, rookToC = 5;
                board[rookR][rookToC] = board[rookR][rookFromC];
                board[rookR][rookFromC] = '.';
            } else {
                // queenside: rook from file 0 -> file 3
                int rookR = r2, rookFromC = 0, rookToC = 3;
                board[rookR][rookToC] = board[rookR][rookFromC];
                board[rookR][rookFromC] = '.';
            }

            // clear any ep target (king move)
            epR = epC = -1;

            // Clear pendingPromo now (not relevant for castling) and update UI
            pendingPromo = 0;
            updateBoardUI();
            return;
        }

        // en-passant capture: pawn moves diagonally into empty square => captured pawn was behind target
        if ((piece == 'P' || piece == 'p') && board[r2][c2] == '.' && c1 != c2) {
            int capR = (piece == 'P') ? r2 + 1 : r2 - 1;
            if (capR >= 0 && capR < 8 && board[capR][c2] != '.') {
                board[capR][c2] = '.';
            }
        }

        // Normal move
        board[r2][c2] = piece;

        // pawn double step sets local en-passant target (the square that can be captured)
        if (piece == 'P' && r1 == 6 && r2 == 4 && c1 == c2) { epR = 5; epC = c1; }
        else if (piece == 'p' && r1 == 1 && r2 == 3 && c1 == c2) { epR = 2; epC = c1; }
        else { epR = epC = -1; }

        // Promotion: use pendingPromo if set (the piece we requested), otherwise fall back to queen
        if (piece == 'P' && r2 == 0) {
            if (pendingPromo != 0) board[r2][c2] = Character.toUpperCase(pendingPromo);
            else board[r2][c2] = 'Q';
        } else if (piece == 'p' && r2 == 7) {
            if (pendingPromo != 0) board[r2][c2] = Character.toLowerCase(pendingPromo);
            else board[r2][c2] = 'q';
        }

        // clear pendingPromo after applying locally
        pendingPromo = 0;

        updateBoardUI();
    }


    private void applyOpponentMove(String mv) {
        /* mv may be 4-char or 5-char (promotion) */
        String from = mv.substring(0,2);
        String to = mv.substring(2,4);
        char promo = (mv.length() >= 5) ? mv.charAt(4) : 0;

        int r1 = 8 - (from.charAt(1) - '0');
        int c1 = from.charAt(0) - 'a';
        int r2 = 8 - (to.charAt(1) - '0');
        int c2 = to.charAt(0) - 'a';

        // record last move (model coords)
        lastFrom = new Point(r1, c1);
        lastTo = new Point(r2, c2);

        char piece = board[r1][c1];
        board[r1][c1] = '.';

        // Castling from opponent: if king moved two squares horizontally, also move rook
        if ((piece == 'K' || piece == 'k') && r1 == r2 && Math.abs(c2 - c1) == 2) {
            // move king
            board[r2][c2] = piece;
            // rook move
            if (c2 > c1) {
                // kingside: rook from file 7 -> file 5
                int rookR = r2, rookFromC = 7, rookToC = 5;
                board[rookR][rookToC] = board[rookR][rookFromC];
                board[rookR][rookFromC] = '.';
            } else {
                // queenside: rook from file 0 -> file 3
                int rookR = r2, rookFromC = 0, rookToC = 3;
                board[rookR][rookToC] = board[rookR][rookFromC];
                board[rookR][rookFromC] = '.';
            }
            epR = epC = -1;
            myTurn = true;
            updateBoardUI();
            return;
        }

        /* en-passant capture detection: opponent captured our pawn en-passant */
        if ((piece == 'P' || piece == 'p') && board[r2][c2] == '.' && c1 != c2) {
            int capR = (piece == 'P') ? r2 + 1 : r2 - 1;
            if (capR >= 0 && capR < 8 && board[capR][c2] != '.') {
                board[capR][c2] = '.';
            }
        }

        board[r2][c2] = piece;

        /* handle promotion if presented explicitly */
        if (promo != 0) {
            char promoted = (Character.isUpperCase(piece)) ? Character.toUpperCase(promo) : Character.toLowerCase(promo);
            board[r2][c2] = promoted;
        } else {
            if (piece == 'P' && r2 == 0) board[r2][c2] = 'Q';
            if (piece == 'p' && r2 == 7) board[r2][c2] = 'q';
        }

        /* set ephemeral en-passant target if opponent moved a pawn two squares */
        if (Character.toUpperCase(piece) == 'P' && Math.abs(r2 - r1) == 2) {
            /* middle square is capture target */
            epR = (r1 + r2) / 2;
            epC = c2;
        } else {
            epR = epC = -1;
        }

        myTurn = true;
        updateBoardUI();
    }


    private void parseServerMessage(String msg) {
        // always log the raw message for debugging
        append("<< " + msg);

        if (msg == null) return;

        if (msg.startsWith("WELCOME") || msg.startsWith("SEND ")) {
            // ignore
            return;
        }

        if (msg.startsWith("WAITING")) {
            // ensure any UI work is done on EDT
            SwingUtilities.invokeLater(() -> append("Server: waiting for opponent..."));
            return;
        }

        if (msg.startsWith("MATCHMAKING_TIMEOUT")) {
            append("Matchmaking timed out (no opponent found).");
            matchmakingTimeoutReceived = true;

            // Wait a short moment to allow the server to close/cleanup its socket, then show popup
            // Use a single-shot Swing Timer to run on EDT after a short delay (500 ms).
            javax.swing.Timer t = new javax.swing.Timer(500, ev -> {
                ((javax.swing.Timer)ev.getSource()).stop();
                // this runs on EDT
                int res = JOptionPane.showOptionDialog(frame,
                        "No opponent found. Retry searching for a match or return to the menu.",
                        "No opponent found",
                        JOptionPane.DEFAULT_OPTION,
                        JOptionPane.INFORMATION_MESSAGE,
                        null,
                        new String[] { "Retry", "Back to menu" },
                        "Retry");
                if (res == 0) { // Retry
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
            // do all UI updates on EDT to avoid race conditions / invisible card issues
            SwingUtilities.invokeLater(() -> {
                append("Game started!");
                String[] p = msg.split("\s+");
                String color = null;
                if (p.length >= 3) {
                    // assume format: START <opponent> <white|black>
                    color = p[p.length - 1];
                } else if (p.length == 2) {
                    // fallback: START <color>
                    color = p[1];
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
                // enable resign/draw now that game is active
                if (resignBtn != null) resignBtn.setEnabled(true);
                if (drawBtn != null) drawBtn.setEnabled(true);

            });
            return;
        }

        if (msg.startsWith("OPPONENT_MOVE")) {
            // network thread -> model update (which schedules UI update)
            String[] parts = msg.split("\s+");
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

        if (msg.startsWith("BYE")) {
            append("Server closed connection");
            showNeutralOverlay("Connection closed by server");
            return;
        }

        if (msg.startsWith("STALEMATE")) {
            append("Game drawn by stalemate");
            showNeutralOverlay("Draw (stalemate)");
            return;
        }

        if (msg.startsWith("DRAW_OFFER")) {
            // record that we have a pending offer
            drawOfferPending = true;

            // show accept/decline dialog on EDT
            SwingUtilities.invokeLater(() -> {
                int choice = JOptionPane.showConfirmDialog(frame,
                        "Opponent offers a draw. Accept?",
                        "Draw offer",
                        JOptionPane.YES_NO_OPTION);
                if (choice == JOptionPane.YES_OPTION) {
                    if (out != null) {
                        out.println("DRAW_ACCEPT");
                        append(">> DRAW_ACCEPT");
                    }
                    drawOfferPending = false;
                } else {
                    if (out != null) {
                        out.println("DRAW_DECLINE");
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
            // handle draw finish: show neutral overlay / end state
            showNeutralOverlay("DRAW");
            return;
        }

        if (msg.startsWith("DRAW_DECLINED")) {
            drawOfferPending = false;
            SwingUtilities.invokeLater(() -> {
                append("Draw offer was declined by opponent.");
                JOptionPane.showMessageDialog(frame, "Your draw offer was declined.", "Draw declined", JOptionPane.INFORMATION_MESSAGE);
                // re-enable draw button if you disable it when offering
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

        // fallback: just log unknown messages
        append("Unhandled server msg: " + msg);
    }

    private void showEndOverlay(boolean isWinner, String subtitle) {
        SwingUtilities.invokeLater(() -> {
            endOverlayShown = true;
            gameEnded = true;

            // disable controls
            if (resignBtn != null) resignBtn.setEnabled(false);
            if (drawBtn != null) drawBtn.setEnabled(false);


            if (isWinner) {
                overlayColorPanel.setBackground(new Color(0, 64, 192, 140));
                overlayTitle.setText("VICTORY");
            } else {
                overlayColorPanel.setBackground(new Color(192, 32, 32, 160));
                overlayTitle.setText("DEFEAT");
            }

            overlaySubtitle.setText(subtitle != null ? subtitle : "");
            
            // Ensure bounds are set before making visible
            Rectangle b = frame.getContentPane().getBounds();
            overlayPanel.setBounds(b);
            overlayColorPanel.setBounds(0, 0, b.width, b.height);
            
            overlayPanel.setVisible(true);
            overlayPanel.revalidate();
            overlayPanel.repaint();
        });
    }

    private void showNeutralOverlay(String text) {
        SwingUtilities.invokeLater(() -> {
            if (endOverlayShown) return;
            endOverlayShown = true;
            gameEnded = true;

            // disable controls
            if (resignBtn != null) resignBtn.setEnabled(false);
            if (drawBtn != null) drawBtn.setEnabled(false);

            overlayColorPanel.setBackground(new Color(48, 48, 48, 160));
            overlayTitle.setText(text);
            overlaySubtitle.setText("");
            
            // Ensure bounds are set before making visible
            Rectangle b = frame.getContentPane().getBounds();
            overlayPanel.setBounds(b);
            overlayColorPanel.setBounds(0, 0, b.width, b.height);
            
            overlayPanel.setVisible(true);
            overlayPanel.revalidate();
            overlayPanel.repaint();
        });
    }

    private void append(String s) { 
        SwingUtilities.invokeLater(() -> { 
            if (log!=null) { 
                log.append(s+"\n"); 
                log.setCaretPosition(log.getDocument().getLength()); 
            } else System.out.println(s); 
        }); }


    private void connect() {
        try {
            append("Connecting to server " + serverHost + ":" + PORT + " ...");
            socket = new Socket(serverHost, PORT);
            in = new BufferedReader(new InputStreamReader(socket.getInputStream(), "UTF-8"));
            out = new PrintWriter(new OutputStreamWriter(socket.getOutputStream(), "UTF-8"), true);

            append("Connected");
            out.println("HELLO " + clientName);
            append("Sent HELLO " + clientName);


            String line;
            while ((line = in.readLine()) != null) {
                parseServerMessage(line);
            }

            // readLine returned null => peer closed connection (EOF)
            append("Server closed connection (EOF).");

            // If we previously received MATCH_TIMEOUT we already showed the popup.
            if (matchmakingTimeoutReceived) {
                // do nothing extra — the popup already handled menu/retry choices
                append("Matchmaking EOF after MATCH_TIMEOUT; no further UI changes.");
                matchmakingTimeoutReceived = false;
            } else {
                // If we already showed a game-end overlay (CHECKMATE etc), don't overwrite it.
                if (myColor != -1) {
                    if (!endOverlayShown) {
                        showNeutralOverlay("Connection closed by server");
                    }
                } else {
                    SwingUtilities.invokeLater(() -> {
                        CardLayout cl = (CardLayout) cards.getLayout();
                        cl.show(cards, CARD_WELCOME);
                    });
                }
            }

            // If we already showed a game-end overlay (CHECKMATE etc), don't overwrite it.
            if (myColor != -1) {
                if (!endOverlayShown) {
                    showNeutralOverlay("Connection closed by server");
                }
            } else {
                SwingUtilities.invokeLater(() -> {
                    CardLayout cl = (CardLayout) cards.getLayout();
                    cl.show(cards, CARD_WELCOME);
                });
            }

            waitingForOk = false;
            pendingFrom = pendingTo = null;

        } catch (IOException e) {
            // If the user intentionally disconnected (or the game overlay was showing) don't show an error popup.
            if (!intentionalDisconnect && !endOverlayShown) {
                // For other unexpected network errors show the dialog.
                e.printStackTrace();
                append("Connection error: " + e.getMessage());
                SwingUtilities.invokeLater(() -> {
                    try {
                        CardLayout cl = (CardLayout)cards.getLayout();
                        cl.show(cards, CARD_WELCOME);
                    } catch (Exception ignored) {}
                    JOptionPane.showMessageDialog(frame, "Network error: " + e.getMessage(), "Connection error", JOptionPane.ERROR_MESSAGE);
                });
            } else {
                // Expected/intentional disconnect (or game end overlay shown) — just log quietly.
                append("Disconnected from server.");
            }
        } finally {
            closeConnection();
        }
    }

    private void sendInput() {
        String txt = input.getText().trim();
        if (txt.isEmpty() || out == null) return;

        String upper = txt.trim().toUpperCase();
        if (upper.equals("DRAW_ACCEPT") || upper.equals("DRAW_DECLINE")) {
            if (!drawOfferPending) {
                // refuse to send accept/decline with no pending offer
                append("Cannot send " + txt + ": no draw offer pending.");
                input.setText("");
                return;
            }
            // allowed: fall through to send
        }

        out.println(txt);
        append(">> " + txt);
        input.setText("");
    }
}
