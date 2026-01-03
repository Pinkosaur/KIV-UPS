import javax.swing.*;
import java.awt.*;
import java.awt.event.*;
import java.io.File;
import java.io.IOException;
import javax.imageio.ImageIO;

public class ChessClient {
    private String serverHost = "127.0.0.1";
    private int serverPort = 10001;
    private static final String BACK_DIR = "backgrounds"; 
    
    private String clientName;
    
    // State
    private volatile boolean intentionalDisconnect = false;
    private volatile boolean isReconnecting = false;
    private volatile boolean connectionEstablished = false; 
    
    // UI Components
    private JFrame frame;
    private JPanel cards;
    private JLabel statusLabel;
    private JLabel timerLabel;
    private ResultOverlay resultOverlay;
    
    /* [FIX] Volatile to ensure background threads see the update immediately */
    private volatile NetworkClient networkClient;
    
    // Login UI
    private JTextField nameField;
    private JTextField serverIpField; 
    private JTextField serverPortField;
    
    // Sub-components
    private LobbyPanel lobbyPanel;
    private GamePanel gamePanel;
    private WaitingPanel waitingPanel;
    private final ImageManager imageManager;

    // Logic State
    private Timer lobbyTimer;
    private Timer turnTimer;
    private int remainingSeconds = 0;
    
    private String pendingFrom = null;
    private String pendingTo = null;
    private JDialog disconnectPopup = null;
    
    private volatile boolean pendingLobbyReturn = false; 

    private static final String CARD_WELCOME = "welcome";
    private static final String CARD_LOBBY = "lobby";     
    private static final String CARD_WAITING = "waiting";
    private static final String CARD_GAME = "game";

    public ChessClient() {
        this.imageManager = new ImageManager("pieces"); 
    }

    public static void main(String[] args) {
        SwingUtilities.invokeLater(() -> {
            try { new ChessClient().createAndShowGUI(); } catch (Exception e) { e.printStackTrace(); }
        });
    }

    private void createAndShowGUI() {
        imageManager.loadIcons();
        clientName = Utils.generateRandomName();

        frame = new JFrame("Chess Client");
        frame.setDefaultCloseOperation(JFrame.DO_NOTHING_ON_CLOSE);
        frame.addWindowListener(new WindowAdapter() {
            public void windowClosing(WindowEvent e) { handleDisconnectAndExit(); }
        });

        // Initialize Panels
        lobbyPanel = new LobbyPanel(this);
        gamePanel = new GamePanel(this, imageManager);
        waitingPanel = new WaitingPanel(e -> {
            sendNetworkCommand(Protocol.CMD_EXT);
            switchToLobby();
        });

        // Card Layout
        cards = new JPanel(new CardLayout());
        cards.add(createWelcomePanel(), CARD_WELCOME);
        cards.add(lobbyPanel, CARD_LOBBY);
        cards.add(waitingPanel, CARD_WAITING);
        cards.add(gamePanel, CARD_GAME);

        frame.getContentPane().add(cards, BorderLayout.CENTER);

        // Status Bar
        JPanel statusPanel = new JPanel(new BorderLayout());
        statusPanel.setBackground(new Color(40, 40, 40));
        statusPanel.setBorder(BorderFactory.createEmptyBorder(4, 12, 4, 12));
        statusLabel = new JLabel("Not connected", SwingConstants.LEFT);
        statusLabel.setForeground(Color.WHITE);
        statusPanel.add(statusLabel, BorderLayout.CENTER);
        timerLabel = new JLabel("--:--", SwingConstants.RIGHT);
        timerLabel.setForeground(Color.CYAN);
        timerLabel.setFont(new Font("Monospaced", Font.BOLD, 16));
        timerLabel.setBorder(BorderFactory.createEmptyBorder(0, 20, 0, 0));
        statusPanel.add(timerLabel, BorderLayout.EAST);
        frame.getContentPane().add(statusPanel, BorderLayout.NORTH);

        // Overlay
        resultOverlay = new ResultOverlay(e -> onOverlayContinueClicked());
        frame.getLayeredPane().add(resultOverlay, JLayeredPane.MODAL_LAYER);
        frame.addComponentListener(new ComponentAdapter() {
            public void componentResized(ComponentEvent e) { resizeOverlay(); }
            public void componentShown(ComponentEvent e) { resizeOverlay(); }
        });

        // Timers
        turnTimer = new Timer(1000, e -> {
            if (remainingSeconds > 0) {
                remainingSeconds--;
                updateTimerDisplay();
            }
        });
        
        lobbyTimer = new Timer(5000, e -> sendNetworkCommand(Protocol.CMD_LIST));
        lobbyTimer.setRepeats(true);

        frame.setPreferredSize(new Dimension(1100, 760));
        frame.setMinimumSize(new Dimension(800, 600));
        frame.pack();
        frame.setLocationRelativeTo(null);
        frame.setVisible(true);
        resizeOverlay();
    }

    public void setPendingMove(String from, String to) {
        this.pendingFrom = from;
        this.pendingTo = to;
    }

    public void sendNetworkCommand(String cmd) {
        // Safe access to volatile field
        NetworkClient nc = this.networkClient;
        if (nc != null) nc.sendRaw(cmd);
    }

    /* Disconnects immediately on UI, cleans up network in background */
    public void disconnect() {
        intentionalDisconnect = true;
        
        // 1. Reset UI State immediately (EDT)
        exitToWelcome();
        statusLabel.setText("Disconnected");
        
        // 2. Capture reference and nullify field to prevent further usage
        NetworkClient nc = this.networkClient;
        this.networkClient = null; 
        
        // 3. Perform Network I/O in background to prevent freezing
        if (nc != null) {
            new Thread(() -> {
                try {
                    nc.sendRaw(Protocol.CMD_EXT); 
                } catch (Exception ignored) {}
                nc.closeConnection();
            }).start();
        }
    }

    public void handleDisconnectAndExit() {
        intentionalDisconnect = true;
        closeDisconnectPopup();
        frame.setVisible(false); 
        frame.dispose();
        
        // Start cleanup in background
        new Thread(() -> {
            NetworkClient nc = this.networkClient;
            if (nc != null) {
                try { nc.sendRaw(Protocol.CMD_EXT); } catch (Exception ignored) {}
                try { nc.closeConnection(); } catch (Exception ignored) {}
            }
            System.exit(0);
        }).start();

        // Safety Net: Force exit if cleanup hangs
        new Thread(() -> {
            try { Thread.sleep(200); } catch (InterruptedException ignored) {}
            System.exit(0);
        }).start();
    }

    private void switchToLobby() {
        pendingLobbyReturn = false; 
        waitingPanel.stopAnimation();
        lobbyTimer.start();
        ((CardLayout)cards.getLayout()).show(cards, CARD_LOBBY);
        statusLabel.setText("Connected as " + clientName);
        sendNetworkCommand(Protocol.CMD_LIST);
    }

    private void onOverlayContinueClicked() {
        resultOverlay.hideOverlay();
        switchToLobby();
    }

    private void resizeOverlay() {
        Rectangle b = frame.getContentPane().getBounds();
        resultOverlay.setBounds(b);
        resultOverlay.setInnerBounds(0, 0, b.width, b.height);
    }

    private void closeDisconnectPopup() {
        if (disconnectPopup != null) {
            disconnectPopup.setVisible(false);
            disconnectPopup.dispose();
            disconnectPopup = null;
        }
    }

    private void updateTimerDisplay() {
        int m = remainingSeconds / 60;
        int s = remainingSeconds % 60;
        timerLabel.setText(String.format("%02d:%02d", m, s));
        timerLabel.setForeground(remainingSeconds < 30 ? Color.RED : Color.CYAN);
    }

    // --- Networking ---

    private void startConnection() {
        // Handle cleanup of existing client OFF the EDT
        NetworkClient oldClient = this.networkClient;
        if (oldClient != null) {
            this.networkClient = null; // Detach first
            new Thread(oldClient::closeConnection).start();
        }

        intentionalDisconnect = false;
        connectionEstablished = false; 

        // [FIXED] Removed incorrect garbage code here.
        // We create the listener, then the client, then start the thread.
        this.networkClient = new NetworkClient(createNetworkListener());
        
        final NetworkClient clientRef = this.networkClient; 
        
        Thread t = new Thread(() -> clientRef.connect(serverHost, serverPort, clientName));
        t.setDaemon(true);
        t.start();
    }

    private NetworkClient.NetworkListener createNetworkListener() {
        return new NetworkClient.NetworkListener() {
            public void onConnected() { connectionEstablished = true; }
            
            public void onDisconnected() {
                SwingUtilities.invokeLater(() -> {
                    // Check if this event is from the active client
                    if (networkClient == null) return; 
                    
                    if (!intentionalDisconnect && !isReconnecting) attemptReconnect();
                    else if (!isReconnecting && !intentionalDisconnect) exitToWelcome();
                });
            }
            
            public void onServerMessage(String line, int seq) {
                // Ensure we are handling the active client's messages
                NetworkClient nc = networkClient;
                if (nc == null) return;

                if (seq >= 0) nc.syncSequenceFromReception(seq);
                System.out.println("<< " + line);
                parseServerMessage(line);
            }
            
            public void onNetworkError(Exception ex) {
                if (networkClient == null) return;
                if (!intentionalDisconnect && !isReconnecting) SwingUtilities.invokeLater(() -> attemptReconnect());
            }
        };
    }

    private void attemptReconnect() {
        if (isReconnecting || intentionalDisconnect) return;
        if (!connectionEstablished) {
            JOptionPane.showMessageDialog(frame, "Connection failed.", "Error", JOptionPane.ERROR_MESSAGE);
            exitToWelcome();
            return;
        }
        isReconnecting = true;
        
        JDialog d = new JDialog(frame, "Reconnecting", true);
        d.setSize(300,100); d.setLocationRelativeTo(frame);
        new Thread(() -> {
             startConnection();
             SwingUtilities.invokeLater(() -> { d.dispose(); isReconnecting = false; });
        }).start();
        d.setVisible(true);
    }

    private void exitToWelcome() {
        lobbyTimer.stop();
        turnTimer.stop();
        gamePanel.setGameEnded(false);
        closeDisconnectPopup();
        resultOverlay.hideOverlay();
        lobbyPanel.reset();
        ((CardLayout)cards.getLayout()).show(cards, CARD_WELCOME);
        connectionEstablished = false;
        isReconnecting = false;
    }

    private void parseServerMessage(String msg) {
        String u = msg.trim();
        
        if (u.startsWith(Protocol.RESP_TIME)) {
            try {
                remainingSeconds = Integer.parseInt(u.substring(Protocol.RESP_TIME.length()).trim());
                updateTimerDisplay();
                if (!turnTimer.isRunning()) turnTimer.start();
            } catch (Exception ignore){}
            return;
        }

        if (u.startsWith(Protocol.RESP_RESUME)) {
            String[] parts = u.split("\\s+");
            String opponentName = (parts.length >= 2) ? parts[1] : "Unknown";
            String cStr = (parts.length >= 3) ? parts[2] : "white";
            int color = cStr.equalsIgnoreCase("white") ? 0 : 1;
            
            SwingUtilities.invokeLater(() -> {
                lobbyTimer.stop();
                ((CardLayout)cards.getLayout()).show(cards, CARD_GAME);
                statusLabel.setText("Game Resumed! VS " + opponentName);
                gamePanel.initGame(color, color == 0);
                closeDisconnectPopup();
            });
            return;
        }
        
        if (u.startsWith(Protocol.RESP_OPP_RESUME)) {
            SwingUtilities.invokeLater(() -> {
                closeDisconnectPopup();
                JOptionPane.showMessageDialog(frame, "Opponent returned. Resuming game.");
            });
            return;
        }

        if (u.startsWith(Protocol.RESP_HISTORY)) {
            String payload = u.substring(Protocol.RESP_HISTORY.length()).trim();
            SwingUtilities.invokeLater(() -> {
                int myColor = gamePanel.getMyColor();
                gamePanel.initGame(myColor, myColor==0); 
                
                if (!payload.isEmpty()) {
                    String[] moves = payload.split(" ");
                    for (String mv : moves) if (!mv.isEmpty()) gamePanel.applyOpponentMove(mv);
                    gamePanel.setTurn((myColor == 0) == (moves.length % 2 == 0));
                }
            });
            return;
        }

        if (u.startsWith(Protocol.RESP_WAIT_CONN)) {
            SwingUtilities.invokeLater(() -> {
                statusLabel.setText("Opponent disconnected. Waiting...");
                turnTimer.stop();
                if (disconnectPopup == null || !disconnectPopup.isVisible()) {
                    JOptionPane pane = new JOptionPane("Your opponent is having trouble with their network connection.\n" +
                                                       "You will win if they can't reconnect within 60 seconds.", 
                                                       JOptionPane.WARNING_MESSAGE);
                    disconnectPopup = pane.createDialog(frame, "Opponent Disconnected");
                    disconnectPopup.setModal(false); 
                    disconnectPopup.setVisible(true);
                }
            });
            return;
        }

        if (u.startsWith(Protocol.RESP_LOBBY)) {
            if (resultOverlay.isEndOverlayShown()) {
                pendingLobbyReturn = true;
                return;
            }
            SwingUtilities.invokeLater(this::switchToLobby);
            return;
        }

        if (u.startsWith(Protocol.RESP_ROOMLIST)) {
            lobbyPanel.updateRoomList(u.substring(Protocol.RESP_ROOMLIST.length()).trim());
            return;
        }

        if (u.startsWith(Protocol.RESP_WAITING)) {
            SwingUtilities.invokeLater(() -> {
                lobbyTimer.stop();
                ((CardLayout)cards.getLayout()).show(cards, CARD_WAITING);
                statusLabel.setText("Hosting room... Waiting for opponent.");
                waitingPanel.startAnimation();
            });
            return;
        }

        if (u.startsWith(Protocol.RESP_START)) {
            String[] p = u.split("\\s+");
            String opp = (p.length >= 2) ? p[1] : "Unknown";
            String cStr = (p.length >= 3) ? p[2] : "white";
            int color = cStr.equalsIgnoreCase("white") ? 0 : 1;
            
            SwingUtilities.invokeLater(() -> {
                lobbyTimer.stop();
                waitingPanel.stopAnimation();
                ((CardLayout)cards.getLayout()).show(cards, CARD_GAME);
                statusLabel.setText("VS " + opp);
                gamePanel.initGame(color, color == 0);
            });
            return;
        }

        if (u.startsWith(Protocol.RESP_OK_MV)) {
            if (pendingFrom != null && pendingTo != null) {
                SwingUtilities.invokeLater(() -> {
                    gamePanel.applyLocalMove(pendingFrom, pendingTo);
                    gamePanel.setTurn(false);
                    gamePanel.setWaitingForOk(false);
                    pendingFrom = pendingTo = null;
                });
            }
            return;
        }

        if (u.startsWith(Protocol.RESP_OPP_MV)) {
            String[] parts = u.split("\\s+");
            if (parts.length >= 2) {
                SwingUtilities.invokeLater(() -> gamePanel.applyOpponentMove(parts[1]));
            }
            return;
        }

        if (u.startsWith(Protocol.RESP_ERR)) {
            SwingUtilities.invokeLater(() -> {
                JOptionPane.showMessageDialog(frame, "Server Error: " + u.substring(4));
                gamePanel.setWaitingForOk(false);
            });
            return;
        }

        // End Game / Draw Logic
        boolean isGameEnd = false;
        if (u.startsWith(Protocol.RESP_WIN_CHKM)) { showEnd(true, "Checkmate"); isGameEnd = true; }
        else if (u.startsWith(Protocol.RESP_CHKM)) { showEnd(false, "Checkmate"); isGameEnd = true; }
        else if (u.startsWith(Protocol.RESP_SM)) { showNeutral("Stalemate"); isGameEnd = true; }
        else if (u.startsWith(Protocol.RESP_OPP_RES)) { showEnd(true, "Opponent Resigned"); isGameEnd = true; }
        else if (u.startsWith(Protocol.RESP_OPP_EXT)) { showEnd(true, "Opponent Disconnected"); isGameEnd = true; }
        else if (u.startsWith(Protocol.RESP_RES)) { showEnd(false, "You Resigned"); isGameEnd = true; }
        else if (u.startsWith(Protocol.RESP_OPP_TOUT)) { showEnd(true, "Opponent Timed Out"); isGameEnd = true; }
        else if (u.startsWith(Protocol.RESP_TOUT)) { showEnd(false, "You Timed Out"); isGameEnd = true; }
        else if (u.startsWith(Protocol.RESP_DRW_ACD)) { showNeutral("Draw Agreed"); isGameEnd = true; }
        
        if (isGameEnd) {
            sendNetworkCommand(Protocol.CMD_LIST);
            turnTimer.stop();
            SwingUtilities.invokeLater(() -> {
                timerLabel.setText("--:--");
                gamePanel.setGameEnded(true);
            });
        }

        if (u.startsWith(Protocol.RESP_DRW_OFF)) {
            SwingUtilities.invokeLater(() -> {
                int ch = JOptionPane.showConfirmDialog(frame, "Opponent offers draw. Accept?", "Draw?", JOptionPane.YES_NO_OPTION);
                sendNetworkCommand(ch == JOptionPane.YES_OPTION ? Protocol.CMD_DRW_ACC : Protocol.CMD_DRW_DEC);
            });
            return;
        }
        if (u.startsWith(Protocol.RESP_DRW_DCD)) {
            SwingUtilities.invokeLater(() -> {
                JOptionPane.showMessageDialog(frame, "Draw offer declined");
                gamePanel.setControlsEnabled(true);
            });
        }
    }

    private void showEnd(boolean win, String sub) {
        SwingUtilities.invokeLater(() -> {
            closeDisconnectPopup();
            resultOverlay.showEndOverlay(win, sub);
        });
    }
    
    private void showNeutral(String sub) {
        SwingUtilities.invokeLater(() -> {
            closeDisconnectPopup();
            resultOverlay.showNeutralOverlay(sub);
        });
    }

    // -- Welcome Panel --
    private JPanel createWelcomePanel() {
        JPanel welcome = new JPanel(new BorderLayout());
        
        // Load BG if exists
        File f = new File(BACK_DIR, "chessboardbg.jpg");
        JLabel tempBg = null;
        if (f.exists()) {
            try { tempBg = new JLabel(new ImageIcon(ImageIO.read(f))); } 
            catch (IOException e) { /* fall through */ }
        }
        if (tempBg == null) {
            tempBg = new JLabel(); 
            tempBg.setBackground(new Color(30, 30, 60)); 
            tempBg.setOpaque(true);
        }
        final JLabel welcomeBg = tempBg; 
        
        JLayeredPane welcomeLayer = new JLayeredPane();
        welcomeLayer.setLayout(null);
        welcomeBg.setBounds(0, 0, 1600, 800);
        welcomeLayer.add(welcomeBg, JLayeredPane.DEFAULT_LAYER);

        JPanel wc = new JPanel();
        wc.setOpaque(false);
        wc.setLayout(new BoxLayout(wc, BoxLayout.Y_AXIS));

        JPanel nameRow = new JPanel(); nameRow.setOpaque(false);
        nameRow.add(new JLabel("Your name:"));
        nameField = new JTextField(clientName, 16);
        nameRow.add(nameField);
        wc.add(nameRow);

        JPanel ipRow = new JPanel(); ipRow.setOpaque(false);
        ipRow.add(new JLabel("Server IP:"));
        serverIpField = new JTextField(serverHost, 16);
        ipRow.add(serverIpField);
        wc.add(ipRow);
        
        JPanel portRow = new JPanel(); portRow.setOpaque(false);
        portRow.add(new JLabel("Port:"));
        serverPortField = new JTextField(String.valueOf(serverPort), 6);
        portRow.add(serverPortField);
        wc.add(portRow);

        JButton connectBtn = new JButton("Connect to Server");
        connectBtn.setAlignmentX(Component.CENTER_ALIGNMENT);
        connectBtn.addActionListener(e -> {
            serverHost = serverIpField.getText();
            try { serverPort = Integer.parseInt(serverPortField.getText()); } catch(Exception ignore){}
            clientName = nameField.getText();
            ((CardLayout)cards.getLayout()).show(cards, CARD_LOBBY);
            statusLabel.setText("Connecting...");
            lobbyTimer.start();
            startConnection();
        });
        
        wc.add(Box.createVerticalStrut(10));
        wc.add(connectBtn);
        welcome.add(wc, BorderLayout.SOUTH);
        
        welcome.addComponentListener(new ComponentAdapter() {
            public void componentResized(ComponentEvent e) {
                welcomeBg.setBounds(0,0,welcome.getWidth(), welcome.getHeight());
                welcomeLayer.setPreferredSize(welcome.getSize());
            }
        });
        welcome.add(welcomeLayer, BorderLayout.CENTER);
        return welcome;
    }
}