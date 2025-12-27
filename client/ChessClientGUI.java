import javax.imageio.ImageIO;
import javax.swing.*;
import javax.swing.event.ListSelectionEvent;
import javax.swing.event.ListSelectionListener;
import java.awt.*;
import java.awt.event.*;
import java.awt.image.BufferedImage;
import java.io.File;
import java.io.IOException;
import java.util.HashSet;
import java.util.Set;

/**
 * ChessClientGUI with Ghost Room Fixes:
 * 1. Cancel Button: Now disconnects socket to force server room deletion, then reconnects.
 * 2. Exit Logic: Synchronous close to ensure server sees EOF.
 * 3. Disconnect Logic: Suppressed "Connection Lost" alerts during intentional cancel/reconnect.
 */
public class ChessClientGUI {
    private String serverHost = "127.0.0.1";
    private int serverPort = 10001;
    private static final String PIECES_DIR = "pieces"; 
    private static final String BACK_DIR = "backgrounds"; 
    private static final int SQUARE_SIZE = 64;
    
    private String clientName;
    private String opponentName;

    // Login UI
    private JTextField nameField;
    private JTextField serverIpField; 
    private JTextField serverPortField;

    // State
    private volatile boolean waitingForOk = false;
    private volatile boolean gameEnded = false;
    private volatile boolean intentionalDisconnect = false;
    
    // UI State
    private volatile boolean endOverlayShown = false;
    private volatile boolean pendingLobbyReturn = false;

    private JFrame frame;
    private JLabel statusLabel;
    private ResultOverlay resultOverlay;
    private NetworkClient networkClient;

    // Navigation
    private JPanel cards; 
    private static final String CARD_WELCOME = "welcome";
    private static final String CARD_LOBBY = "lobby";     
    private static final String CARD_WAITING = "waiting";
    private static final String CARD_GAME = "game";

    // Lobby
    private DefaultListModel<String> roomListModel;
    private JList<String> roomList;
    private JButton btnJoinRoom;
    private JButton btnCreateRoom;
    private JButton btnRefreshRooms;

    private WaitingPanel waitingPanel;

    // Game
    private JPanel boardPanel;
    private SquareLabel[][] squares = new SquareLabel[8][8];
    private JPanel boardContainer;
    private JButton resignBtn;
    private JButton drawBtn;

    // Logic
    private final BoardModel boardModel = new BoardModel();
    private char[][] board = boardModel.board;
    private final ImageManager imageManager;
    private int myColor = -1; 
    private boolean myTurn = false;
    
    private int selR = -1, selC = -1;
    private Set<Point> highlighted = new HashSet<>();
    private volatile String pendingFrom = null, pendingTo = null;
    private volatile char pendingPromo = 0;
    
    private boolean kingInCheck = false;
    private int kingR = -1, kingC = -1;
    private Point lastFrom = null;
    private Point lastTo = null;

    public ChessClientGUI() {
        this.imageManager = new ImageManager(PIECES_DIR);
    }

    public static void main(String[] args) {
        SwingUtilities.invokeLater(() -> {
            try { new ChessClientGUI().createAndShowGUI(); } catch (Exception e) { e.printStackTrace(); }
        });
    }

    private void loadIcons() { imageManager.loadIcons(); }
    private ImageIcon getScaledIconForPiece(char p, int cellSize) { return imageManager.getScaledIconForPiece(p, cellSize); }

    private void initBoardModel() { boardModel.initBoardModel(); this.board = boardModel.board; }
    private void computeKingCheck() {
        if (myColor != 0 && myColor != 1) { kingInCheck = false; kingR = kingC = -1; return; }
        int[] pos = boardModel.findKing(board, myColor);
        if (pos == null) { kingInCheck = false; kingR = kingC = -1; return; }
        kingR = pos[0]; kingC = pos[1];
        kingInCheck = boardModel.isSquareAttacked(board, kingR, kingC, 1 - myColor);
    }
    private boolean inBounds(int r,int c){ return boardModel.inBounds(r,c); }
    private boolean isLegalMoveBasic(int r1,int c1,int r2,int c2) { return boardModel.isLegalMoveBasic(r1,c1,r2,c2); }
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
            if (statusLabel != null && myColor != -1) {
                statusLabel.setText((myColor==0?"White":"Black") + ": " + clientName + " (you) VS " + (myColor==1?"White":"Black") + ": " + opponentName + "  -  " + (myTurn?"Your turn":"Opponent's turn"));
            }
        });
    }

    private void createAndShowGUI() {
        loadIcons();
        clientName = Utils.generateRandomName();

        frame = new JFrame("Chess Client");
        frame.setDefaultCloseOperation(JFrame.DO_NOTHING_ON_CLOSE);
        
        // --- Aggressive Window Closing ---
        frame.addWindowListener(new WindowAdapter() {
            @Override
            public void windowClosing(WindowEvent e) {
                handleDisconnectAndExit();
            }
        });

        cards = new JPanel(new CardLayout());
        cards.add(createWelcomePanel(), CARD_WELCOME);
        cards.add(createLobbyPanel(), CARD_LOBBY);
        
        waitingPanel = createWaitingPanel();
        cards.add(waitingPanel, CARD_WAITING);
        
        cards.add(createGamePanel(), CARD_GAME);

        frame.getContentPane().add(cards, BorderLayout.CENTER);

        statusLabel = new JLabel("Not connected", SwingConstants.CENTER);
        statusLabel.setOpaque(true);
        statusLabel.setBackground(new Color(40, 40, 40));
        statusLabel.setForeground(Color.WHITE);
        statusLabel.setBorder(BorderFactory.createEmptyBorder(8, 12, 8, 12));
        statusLabel.setVisible(false);
        frame.getContentPane().add(statusLabel, BorderLayout.NORTH);

        resultOverlay = new ResultOverlay(e -> onOverlayContinueClicked());
        frame.getLayeredPane().add(resultOverlay, JLayeredPane.MODAL_LAYER);
        frame.addComponentListener(new ComponentAdapter() {
            @Override
            public void componentResized(ComponentEvent e) { resizeOverlay(); }
            @Override
            public void componentShown(ComponentEvent e) { resizeOverlay(); }
        });

        frame.setPreferredSize(new Dimension(1100, 760));
        frame.setMinimumSize(new Dimension(800, 600));
        frame.pack();
        frame.setLocationRelativeTo(null);
        frame.setVisible(true);
        resizeOverlay();
    }

    private void resizeOverlay() {
        Rectangle b = frame.getContentPane().getBounds();
        resultOverlay.setBounds(b);
        resultOverlay.setInnerBounds(0,0,b.width,b.height);
    }

    private JPanel createWelcomePanel() {
        JPanel welcome = new JPanel(new BorderLayout());
        JLabel welcomeBg = loadBackgroundLabel(BACK_DIR + "/chessboardbg.jpg", new Color(30,30,60));
        JLayeredPane welcomeLayer = new JLayeredPane();
        welcomeLayer.setLayout(null);
        welcomeBg.setBounds(0,0,1600,800);
        welcomeLayer.add(welcomeBg, JLayeredPane.DEFAULT_LAYER);

        JPanel wc = new JPanel();
        wc.setOpaque(false);
        wc.setLayout(new BoxLayout(wc, BoxLayout.Y_AXIS));

        JPanel nameRow = new JPanel(new FlowLayout(FlowLayout.CENTER));
        nameRow.setOpaque(false);
        nameRow.add(new JLabel("Your name:"));
        nameField = new JTextField(clientName, 16);
        nameRow.add(nameField);
        wc.add(nameRow);

        JPanel ipRow = new JPanel(new FlowLayout(FlowLayout.CENTER));
        ipRow.setOpaque(false);
        ipRow.add(new JLabel("Server IP:"));
        serverIpField = new JTextField(serverHost, 16);
        ipRow.add(serverIpField);
        wc.add(ipRow);

        JPanel portRow = new JPanel(new FlowLayout(FlowLayout.CENTER));
        portRow.setOpaque(false);
        portRow.add(new JLabel("Port:"));
        serverPortField = new JTextField(Integer.toString(serverPort), 6);
        portRow.add(serverPortField);
        wc.add(portRow);

        JButton connectBtn = new JButton("Connect to Server");
        connectBtn.setAlignmentX(Component.CENTER_ALIGNMENT);
        wc.add(Box.createVerticalStrut(10));
        wc.add(connectBtn);
        welcome.add(wc, BorderLayout.SOUTH);
        connectBtn.addActionListener(e -> onConnectClicked());
        welcome.addComponentListener(new ComponentAdapter() {
            @Override public void componentResized(ComponentEvent e) {
                welcomeBg.setBounds(0,0,welcome.getWidth(), welcome.getHeight());
                welcomeLayer.setPreferredSize(welcome.getSize());
            }
        });
        welcome.add(welcomeLayer, BorderLayout.CENTER);
        return welcome;
    }

    private JPanel createLobbyPanel() {
        JPanel lobby = new JPanel(new BorderLayout());
        lobby.setBackground(new Color(60,60,60));
        
        JLabel title = new JLabel("LOBBY - Select a Room", SwingConstants.CENTER);
        title.setFont(new Font("SansSerif", Font.BOLD, 24));
        title.setForeground(Color.WHITE);
        title.setBorder(BorderFactory.createEmptyBorder(20,0,20,0));
        lobby.add(title, BorderLayout.NORTH);

        roomListModel = new DefaultListModel<>();
        roomList = new JList<>(roomListModel);
        roomList.setSelectionMode(ListSelectionModel.SINGLE_SELECTION);
        roomList.setFont(new Font("Monospaced", Font.PLAIN, 16));
        JScrollPane scroll = new JScrollPane(roomList);
        scroll.setBorder(BorderFactory.createEmptyBorder(10,50,10,50));
        lobby.add(scroll, BorderLayout.CENTER);

        JPanel btnPanel = new JPanel(new FlowLayout(FlowLayout.CENTER, 20, 20));
        btnPanel.setOpaque(false);
        
        btnCreateRoom = new JButton("Create New Room");
        btnJoinRoom = new JButton("Join Selected Room");
        btnRefreshRooms = new JButton("Refresh List");
        JButton btnExit = new JButton("Exit");

        btnCreateRoom.addActionListener(e -> { if(networkClient!=null) networkClient.sendRaw("NEW"); });
        btnRefreshRooms.addActionListener(e -> { if(networkClient!=null) networkClient.sendRaw("LIST"); });
        btnJoinRoom.addActionListener(e -> onJoinRoomClicked());
        btnExit.addActionListener(e -> handleDisconnectAndExit());

        btnJoinRoom.setEnabled(false);
        roomList.addListSelectionListener(e -> btnJoinRoom.setEnabled(!roomList.isSelectionEmpty()));

        btnPanel.add(btnCreateRoom);
        btnPanel.add(btnJoinRoom);
        btnPanel.add(btnRefreshRooms);
        btnPanel.add(btnExit);
        lobby.add(btnPanel, BorderLayout.SOUTH);

        return lobby;
    }

    // --- Waiting Panel (Cancel Fix) ---
    private static class WaitingPanel extends JPanel {
        private final Timer timer;
        private int angle = 0;
        private final JButton cancelBtn;

        public WaitingPanel(ActionListener onCancel) {
            setLayout(new BorderLayout());
            setBackground(new Color(40,40,40));
            
            JLabel lbl = new JLabel("Waiting for opponent...", SwingConstants.CENTER);
            lbl.setForeground(Color.WHITE);
            lbl.setFont(new Font("SansSerif", Font.BOLD, 18));
            add(lbl, BorderLayout.NORTH);
            
            JPanel bottom = new JPanel(new FlowLayout(FlowLayout.CENTER));
            bottom.setOpaque(false);
            cancelBtn = new JButton("Cancel");
            cancelBtn.addActionListener(onCancel);
            bottom.add(cancelBtn);
            add(bottom, BorderLayout.SOUTH);

            timer = new Timer(50, e -> {
                angle = (angle + 5) % 360;
                repaint();
            });
        }
        
        public void startAnimation() { timer.start(); }
        public void stopAnimation() { timer.stop(); }

        @Override
        protected void paintComponent(Graphics g) {
            super.paintComponent(g);
            Graphics2D g2 = (Graphics2D) g;
            g2.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
            int cx = getWidth() / 2;
            int cy = getHeight() / 2;
            int r = 30;
            g2.setColor(new Color(100,100,100));
            g2.setStroke(new BasicStroke(4));
            g2.drawOval(cx-r, cy-r, r*2, r*2);
            g2.setColor(Color.WHITE);
            g2.drawArc(cx-r, cy-r, r*2, r*2, -angle, 120);
        }
    }

    private WaitingPanel createWaitingPanel() {
        // Pass an action listener to the WaitingPanel for the "Cancel" button
        return new WaitingPanel(e -> {
            if (networkClient != null) {
                // 1. Send EXT to tell server to destroy the hosted room
                networkClient.sendRaw("EXT"); 
                
                // 2. Stop animation and switch UI back to Lobby
                if (waitingPanel != null) waitingPanel.stopAnimation();
                ((CardLayout)cards.getLayout()).show(cards, CARD_LOBBY);
                statusLabel.setText("Connected as " + clientName);
                
                // 3. Refresh room list so we don't see our own ghost
                networkClient.sendRaw("LIST");
            }
        });
    }

    private JPanel createGamePanel() {
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

        boardContainer = new JPanel(new GridBagLayout());
        boardContainer.setOpaque(false);
        boardContainer.add(boardPanel, new GridBagConstraints());
        boardContainer.addComponentListener(new ComponentAdapter() {
            @Override public void componentResized(ComponentEvent e) {
                int w = boardContainer.getWidth();
                int h = boardContainer.getHeight();
                int size = Math.min(w, h) - 20;
                if (size < 40) size = 40;
                size = (size / 8) * 8;
                boardPanel.setPreferredSize(new Dimension(size, size));
                boardPanel.revalidate();
                imageManager.clearCacheForSize(size);
                updateBoardUI();
            }
        });

        JPanel ctrl = new JPanel(new FlowLayout(FlowLayout.CENTER, 8, 0));
        resignBtn = new JButton("Resign");
        drawBtn = new JButton("Offer Draw");
        resignBtn.setEnabled(false);
        drawBtn.setEnabled(false);
        ctrl.add(drawBtn);
        ctrl.add(resignBtn);

        resignBtn.addActionListener(e -> {
            int ok = JOptionPane.showConfirmDialog(frame, "Resign?", "Confirm", JOptionPane.YES_NO_OPTION);
            if (ok == JOptionPane.YES_OPTION && networkClient != null) {
                networkClient.sendRaw("RES");
                resignBtn.setEnabled(false);
                drawBtn.setEnabled(false);
            }
        });

        drawBtn.addActionListener(e -> {
            if (networkClient != null) {
                networkClient.sendRaw("DRW_OFF");
                drawBtn.setEnabled(false);
            }
        });

        gameCard.add(boardContainer, BorderLayout.CENTER);
        gameCard.add(ctrl, BorderLayout.SOUTH);
        return gameCard;
    }

    // --- Logic ---

    private void handleDisconnectAndExit() {
        intentionalDisconnect = true;
        
        // 1. Visually indicate shutdown (optional, but good practice)
        frame.setVisible(false); 
        frame.dispose();

        // 2. Start a safety timer: Force kill app in 500ms no matter what.
        // This guarantees the app never "freezes" or stays open if network hangs.
        new javax.swing.Timer(500, e -> System.exit(0)).start();

        // 3. Perform network cleanup in background
        new Thread(() -> {
            if (networkClient != null) {
                try {
                    // Send disconnect command
                    networkClient.sendRaw("EXT"); 
                    // Give the socket a moment to flush the data to the OS
                    Thread.sleep(100); 
                    networkClient.closeConnection();
                } catch (Exception ignored) {}
            }
            // Exit immediately once cleanup is done (cancels out the 500ms wait)
            System.exit(0);
        }).start();
    }

    private void onConnectClicked() {
        if (serverIpField != null) serverHost = serverIpField.getText().trim();
        if (serverPortField != null) {
            try { serverPort = Integer.parseInt(serverPortField.getText().trim()); } catch (Exception ignore){}
        }
        if (nameField != null && !nameField.getText().trim().isEmpty()) {
            clientName = nameField.getText().trim();
        }
        
        ((CardLayout)cards.getLayout()).show(cards, CARD_LOBBY); 
        statusLabel.setText("Connecting to " + serverHost + ":" + serverPort + "...");
        statusLabel.setVisible(true);
        startConnection();
    }

    private void onJoinRoomClicked() {
        String sel = roomList.getSelectedValue();
        if (sel == null) return;
        String[] parts = sel.split(":");
        if (parts.length > 0) {
            String id = parts[0];
            if (networkClient != null) networkClient.sendRaw("JOIN " + id);
        }
    }

    private void onOverlayContinueClicked() {
        resultOverlay.hideOverlay();
        endOverlayShown = false;
        
        if (pendingLobbyReturn) {
            pendingLobbyReturn = false;
            ((CardLayout)cards.getLayout()).show(cards, CARD_LOBBY);
            statusLabel.setText("Connected as " + clientName);
            statusLabel.setVisible(true);
            if (networkClient != null) networkClient.sendRaw("LIST");
        }
    }

    private void startConnection() {
        if (networkClient != null) networkClient.closeConnection();
        // Reset flag; we are starting a fresh desired connection.
        intentionalDisconnect = false;

        networkClient = new NetworkClient(new NetworkClient.NetworkListener() {
            @Override public void onConnected() { System.out.println("Connected."); }
            @Override public void onDisconnected() {
                SwingUtilities.invokeLater(() -> {
                    // Only return to Welcome screen if it wasn't an intentional cancel/reconnect
                    if (!intentionalDisconnect) {
                        JOptionPane.showMessageDialog(frame, "Connection Lost", "Error", JOptionPane.ERROR_MESSAGE);
                        exitToWelcome();
                    }
                });
            }
            @Override public void onServerMessage(String line, int seq) { parseServerMessageWithSeq(line, seq); }
            @Override public void onNetworkError(Exception ex) {
                if (!intentionalDisconnect) SwingUtilities.invokeLater(() -> {
                     JOptionPane.showMessageDialog(frame, "Network Error: " + ex.getMessage());
                     exitToWelcome();
                });
            }
        });
        Thread t = new Thread(() -> networkClient.connect(serverHost, serverPort, clientName));
        t.setDaemon(true);
        t.start();
    }

    private void exitToWelcome() {
        myColor = -1; myTurn = false; gameEnded = false;
        pendingLobbyReturn = false;
        resultOverlay.hideOverlay();
        statusLabel.setVisible(false);
        ((CardLayout)cards.getLayout()).show(cards, CARD_WELCOME);
        if (waitingPanel != null) waitingPanel.stopAnimation();
    }

    private void parseServerMessageWithSeq(String msg, int seq) {
        if (msg == null) return;
        if (seq >= 0 && networkClient != null) networkClient.syncSequenceFromReception(seq);
        System.out.println("<< " + msg);
        parseServerMessage(msg);
    }

    private void parseServerMessage(String msg) {
        String u = msg.trim();
        
        if (u.startsWith("LOBBY")) {
            SwingUtilities.invokeLater(() -> {
                gameEnded = false; 
                myColor = -1; 
                waitingForOk = false;
                
                if (endOverlayShown) {
                    pendingLobbyReturn = true;
                } else {
                    ((CardLayout)cards.getLayout()).show(cards, CARD_LOBBY);
                    statusLabel.setText("Connected as " + clientName);
                    statusLabel.setVisible(true);
                    if (networkClient != null) networkClient.sendRaw("LIST");
                }
            });
            return;
        }

        if (u.startsWith("ROOMLIST")) {
            String payload = u.substring("ROOMLIST".length()).trim();
            SwingUtilities.invokeLater(() -> {
                roomListModel.clear();
                if (!payload.equals("EMPTY") && !payload.isEmpty()) {
                    String[] rooms = payload.split(" ");
                    for (String r : rooms) if(!r.isEmpty()) roomListModel.addElement(r);
                }
            });
            return;
        }

        if (u.startsWith("WAITING")) {
            SwingUtilities.invokeLater(() -> {
                ((CardLayout)cards.getLayout()).show(cards, CARD_WAITING);
                statusLabel.setText("Hosting room... Waiting for opponent.");
                if (waitingPanel != null) waitingPanel.startAnimation();
            });
            return;
        }

        if (u.startsWith("START")) {
            SwingUtilities.invokeLater(() -> {
                String[] p = u.split("\\s+");
                String color = (p.length >= 3) ? p[p.length - 1] : "white";
                opponentName = (p.length >= 3) ? p[1] : "Unknown";
                myColor = color.equalsIgnoreCase("white") ? 0 : 1;
                myTurn = (myColor == 0);
                gameEnded = false; waitingForOk = false;
                pendingLobbyReturn = false;
                endOverlayShown = false;
                initBoardModel(); updateBoardUI();
                ((CardLayout) cards.getLayout()).show(cards, CARD_GAME);
                statusLabel.setText("VS " + opponentName);
                if (resignBtn != null) resignBtn.setEnabled(true);
                if (drawBtn != null) drawBtn.setEnabled(true);
                if (waitingPanel != null) waitingPanel.stopAnimation();
            });
            return;
        }

        if (u.startsWith("OK_MV") || u.equals("05")) {
            if (pendingFrom != null && pendingTo != null) {
                applyLocalMove(pendingFrom, pendingTo);
                pendingFrom = pendingTo = null; waitingForOk = false; myTurn = false;
            }
            return;
        }
        if (u.startsWith("OPP_MV")) {
            String[] parts = u.split("\\s+");
            if (parts.length >= 2) {
                applyOpponentMove(parts[1]);
                waitingForOk = false;
            }
            return;
        }
        if (u.startsWith("ERR") || u.equals("04")) {
            // Remove "ERR " prefix if present (length 4) to show only the reason
            String u2 = u.startsWith("ERR ") ? u.substring(4) : u;
            SwingUtilities.invokeLater(() -> JOptionPane.showMessageDialog(frame, "Server Error: " + u2));
            waitingForOk = false;
            return;
        }

        // --- Game End States ---
        boolean isGameEnd = false;
        
        if (u.startsWith("SM")) { showNeutralOverlay("Stalemate"); isGameEnd = true; }
        else if (u.startsWith("WIN_CM") || u.startsWith("WIN_CHKM")) { showEndOverlay(true, "Checkmate"); isGameEnd = true; }
        else if (u.startsWith("CHKM")) { showEndOverlay(false, "Checkmate"); isGameEnd = true; }
        else if (u.startsWith("CHK") || u.equals("07")) { System.out.println("In Check!"); }
        else if (u.startsWith("OPP_RES") || u.equals("14")) { showEndOverlay(true, "Opponent Resigned"); isGameEnd = true; }
        else if (u.startsWith("OPP_EXT") || u.equals("17")) { showEndOverlay(true, "Opponent Disconnected"); isGameEnd = true; }
        else if (u.startsWith("RES") || u.equals("13")) { showEndOverlay(false, "You Resigned"); isGameEnd = true; }
        else if (u.startsWith("OPP_TOUT") || u.equals("16")) { showEndOverlay(true, "Opponent Timed Out"); isGameEnd = true; }
        else if (u.startsWith("TOUT") || u.equals("15")) { showEndOverlay(false, "You Timed Out"); isGameEnd = true; }
        else if (u.startsWith("DRW_ACD") || u.equals("12")) { showNeutralOverlay("Draw Agreed"); isGameEnd = true; }
        
        if (isGameEnd && networkClient != null) {
            networkClient.sendRaw("LIST");
        }

        if (u.startsWith("DRW_OFF") || u.equals("10")) {
            SwingUtilities.invokeLater(() -> {
                int ch = JOptionPane.showConfirmDialog(frame, "Opponent offers draw. Accept?", "Draw?", JOptionPane.YES_NO_OPTION);
                if (networkClient != null) networkClient.sendRaw(ch == JOptionPane.YES_OPTION ? "DRW_ACC" : "DRW_DEC");
            });
            return;
        }
        if (u.startsWith("DRW_DCD") || u.equals("11")) { 
            SwingUtilities.invokeLater(() -> {
                JOptionPane.showMessageDialog(frame, "Draw offer declined");
                if (drawBtn != null) drawBtn.setEnabled(true);
            });
            return; 
        }
        
        if (u.matches("\\d{2}")) return; 
    }

    private void showEndOverlay(boolean isWinner, String subtitle) {
        SwingUtilities.invokeLater(() -> {
            endOverlayShown = true;
            gameEnded = true;
            if (resignBtn != null) resignBtn.setEnabled(false);
            if (drawBtn != null) drawBtn.setEnabled(false);
            resultOverlay.showEndOverlay(isWinner, subtitle);
            resizeOverlay();
        });
    }

    private void showNeutralOverlay(String text) {
        SwingUtilities.invokeLater(() -> {
            endOverlayShown = true;
            gameEnded = true;
            if (resignBtn != null) resignBtn.setEnabled(false);
            if (drawBtn != null) drawBtn.setEnabled(false);
            resultOverlay.showNeutralOverlay(text);
            resizeOverlay();
        });
    }

    private void applyLocalMove(String from, String to) {
        boardModel.pendingPromo = pendingPromo;
        boardModel.applyLocalMove(from, to);
        lastFrom = boardModel.lastFrom; lastTo = boardModel.lastTo;
        this.board = boardModel.board;
        pendingPromo = 0;
        updateBoardUI();
    }

    private void applyOpponentMove(String mv) {
        boardModel.applyOpponentMove(mv);
        this.board = boardModel.board;
        lastFrom = boardModel.lastFrom; lastTo = boardModel.lastTo;
        myTurn = true;
        updateBoardUI();
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
                boolean isPawnPromotion = (moving == 'P' && r == 0) || (moving == 'p' && r == 7);
                String from = Utils.coordToAlg(selR, selC);
                String to = Utils.coordToAlg(r, c);
                
                if (isPawnPromotion) {
                    SwingUtilities.invokeLater(() -> {
                        String[] options = {"Queen","Rook","Bishop","Knight"};
                        int choice = JOptionPane.showOptionDialog(frame, "Promote to:", "Promotion",
                                JOptionPane.DEFAULT_OPTION, JOptionPane.PLAIN_MESSAGE, null, options, options[0]);
                        char promo = 'q';
                        if (choice == 1) promo = 'r'; else if (choice == 2) promo = 'b'; else if (choice == 3) promo = 'n';
                        pendingFrom = from; pendingTo = to; pendingPromo = promo;
                        if (networkClient != null) { waitingForOk = true; networkClient.sendMove(from + to + promo); }
                    });
                } else {
                    pendingFrom = from; pendingTo = to; pendingPromo = 0;
                    if (networkClient != null) { waitingForOk = true; networkClient.sendMove(from + to); }
                }
                selR = selC = -1; highlighted.clear(); updateBoardUI();
                return;
            }
            char p = board[r][c];
            if (p != '.' && BoardModel.isOwnPiece(p, myColor)) {
                selR = r; selC = c; highlighted.clear();
                for (int tr=0; tr<8; tr++) for (int tc=0; tc<8; tc++)
                    if (isLegalMoveBasic(selR,selC,tr,tc) && !moveLeavesInCheck(board,myColor,selR,selC,tr,tc))
                        highlighted.add(new Point(tr,tc));
                updateBoardUI();
            } else {
                selR = selC = -1; highlighted.clear(); updateBoardUI();
            }
        }
    }
    
    private JLabel loadBackgroundLabel(String path, Color fallback) {
        File f = new File(path);
        if (f.exists()) {
            try {
                 if (path.toLowerCase().endsWith(".gif")) return new JLabel(new ImageIcon(path));
                 else return new JLabel(new ImageIcon(ImageIO.read(f)));
            } catch (IOException ignored) {}
        }
        JLabel p = new JLabel(); p.setOpaque(true); p.setBackground(fallback); return p;
    }
}