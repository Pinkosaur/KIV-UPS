import javax.swing.*;
import java.awt.*;
import java.awt.event.*;
import java.io.File;
import java.io.IOException;
import java.util.UUID;
import java.util.concurrent.ConcurrentLinkedQueue;
import javax.imageio.ImageIO;

/**
 * Main application entry point and controller for the Chess Client.
 * <p>
 * This class orchestrates the application lifecycle, manages the main UI frame
 * (switching between Welcome, Lobby, Waiting, and Game views), and handles
 * high-level network logic such as connection initiation and automatic reconnection.
 * </p>
 */
public class ChessClient {
    private String serverHost = "127.0.0.1";
    private int serverPort = 10001;
    private static final String BACK_DIR = "backgrounds"; 
    private int autoRefreshTime = 8000; // Automatic refresh interval in milliseconds
    
    private String clientName;
    private final String sessionID;
    
    // Connection State
    private volatile boolean intentionalDisconnect = false;
    private volatile boolean isReconnecting = false;
    private volatile boolean connectionEstablished = false; 
    private volatile boolean handshakeCompleted = false;
    
    // Queue for buffering messages when offline
    private final ConcurrentLinkedQueue<String> offlineQueue = new ConcurrentLinkedQueue<>();
    
    // Retry limits
    private int reconnectAttempts = 0;
    private static final int MAX_RECONNECT_ATTEMPTS = 5;
    
    // UI Components
    private JFrame frame;
    private JPanel cards;
    private JLabel statusLabel;
    private JLabel timerLabel;
    private ResultOverlay resultOverlay;
    private volatile NetworkClient networkClient;
    
    // Login UI Inputs
    private JTextField nameField;
    private JTextField serverIpField; 
    private JTextField serverPortField;
    private JButton connectBtn; 
    
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

    /**
     * Constructs the ChessClient and generates a unique session ID.
     */
    public ChessClient() {
        this.imageManager = new ImageManager("pieces");
        // Generate a persistent session ID for reconnection support
        this.sessionID = UUID.randomUUID().toString().substring(0, 8);
    }

    /**
     * Application entry point.
     * @param args Command line arguments (unused).
     */
    public static void main(String[] args) {
        SwingUtilities.invokeLater(() -> {
            try { new ChessClient().createAndShowGUI(); } catch (Exception e) { e.printStackTrace(); }
        });
    }

    /**
     * Initializes the graphical user interface, including the main frame,
     * layout manager, sub-panels, and status bar.
     */
    private void createAndShowGUI() {
        imageManager.loadIcons();
        clientName = Utils.generateRandomName();

        frame = new JFrame("Chess Client");
        frame.setDefaultCloseOperation(JFrame.DO_NOTHING_ON_CLOSE);
        frame.addWindowListener(new WindowAdapter() {
            public void windowClosing(WindowEvent e) { handleDisconnectAndExit(); }
        });

        lobbyPanel = new LobbyPanel(this);
        gamePanel = new GamePanel(this, imageManager);
        waitingPanel = new WaitingPanel(e -> {
            sendNetworkCommand(Protocol.CMD_EXT);
            switchToLobby();
        });

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

        // Result Overlay
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
        
        lobbyTimer = new Timer(autoRefreshTime, e -> sendNetworkCommand(Protocol.CMD_LIST));
        lobbyTimer.setRepeats(true);

        frame.setPreferredSize(new Dimension(1100, 760));
        frame.setMinimumSize(new Dimension(800, 600));
        frame.pack();
        frame.setLocationRelativeTo(null);
        frame.setVisible(true);
        resizeOverlay();
    }

    /**
     * Stores the details of a locally initiated move to be applied upon server confirmation.
     * @param from Algebraic coordinate of the source square (e.g., "e2").
     * @param to Algebraic coordinate of the destination square (e.g., "e4").
     */
    public void setPendingMove(String from, String to) {
        this.pendingFrom = from;
        this.pendingTo = to;
    }

    /**
     * Sends a command to the server via the NetworkClient.
     * <p>
     * If the client is currently disconnected, the message is queued in {@code offlineQueue}
     * and sent automatically once the connection is re-established.
     * </p>
     * @param cmd The raw protocol command string.
     */
    public void sendNetworkCommand(String cmd) {
        NetworkClient nc = this.networkClient;
        if (nc != null && nc.isConnected()) {
            nc.sendRaw(cmd);
        } else {
            offlineQueue.offer(cmd);
        }
    }

    /**
     * Initiates a graceful disconnect from the server and returns the user to the Welcome screen.
     */
    public void disconnect() {
        intentionalDisconnect = true;
        exitToWelcome();
        statusLabel.setText("Disconnected");
        NetworkClient nc = this.networkClient;
        this.networkClient = null; 
        if (nc != null) {
            new Thread(() -> {
                try { nc.sendRaw(Protocol.CMD_EXT); } catch (Exception ignored) {}
                nc.closeConnection();
            }).start();
        }
    }

    /**
     * Handles the application closing event. Attempts to send disconnect commands
     * before terminating the JVM.
     */
    public void handleDisconnectAndExit() {
        intentionalDisconnect = true;
        closeDisconnectPopup();
        frame.setVisible(false); 
        frame.dispose();
        
        new Thread(() -> {
            NetworkClient nc = this.networkClient;
            if (nc != null) {
                try { 
                    nc.sendRaw(Protocol.CMD_EXT); 
                } catch (Exception ignored) {}
                try { nc.closeConnection(); } catch (Exception ignored) {}
            }
            System.exit(0);
        }).start();

        // Fallback force exit
        new Thread(() -> {
            try { Thread.sleep(200); } catch (InterruptedException ignored) {}
            System.exit(0);
        }).start();
    }

    /**
     * Switches the UI view to the Lobby panel and initiates a room list refresh.
     */
    private void switchToLobby() {
        if (resultOverlay.isEndOverlayShown()) {
            pendingLobbyReturn = true;
            return; 
        }
        pendingLobbyReturn = false; 
        waitingPanel.stopAnimation();
        lobbyTimer.start();
        ((CardLayout)cards.getLayout()).show(cards, CARD_LOBBY);
        statusLabel.setText("Connected as " + clientName);
        sendNetworkCommand(Protocol.CMD_LIST);
    }

    /**
     * Callback for the "Continue" button on the end-game overlay.
     */
    private void onOverlayContinueClicked() {
        resultOverlay.hideOverlay();
        switchToLobby();
    }

    /**
     * Synchronizes the result overlay size with the current frame size.
     */
    private void resizeOverlay() {
        Rectangle b = frame.getContentPane().getBounds();
        resultOverlay.setBounds(b);
        resultOverlay.setInnerBounds(0, 0, b.width, b.height);
    }

    /**
     * Disposes of the opponent-disconnected notification popup.
     */
    private void closeDisconnectPopup() {
        if (disconnectPopup != null) {
            disconnectPopup.setVisible(false);
            disconnectPopup.dispose();
            disconnectPopup = null;
        }
    }

    /**
     * Updates the UI timer label with the current remaining seconds.
     */
    private void updateTimerDisplay() {
        int m = remainingSeconds / 60;
        int s = remainingSeconds % 60;
        timerLabel.setText(String.format("%02d:%02d", m, s));
        timerLabel.setForeground(remainingSeconds < 30 ? Color.RED : Color.CYAN);
    }

    // --- Networking ---

    /**
     * Starts a new network connection on a background thread.
     */
    private void startConnection() {
        NetworkClient oldClient = this.networkClient;
        if (oldClient != null) {
            this.networkClient = null;
            new Thread(oldClient::closeConnection).start();
        }

        intentionalDisconnect = false;
        connectionEstablished = false; 
        handshakeCompleted = false; 
        offlineQueue.clear(); 

        this.networkClient = new NetworkClient(createNetworkListener());
        final NetworkClient clientRef = this.networkClient; 
        
        Thread t = new Thread(() -> {
            try {
                clientRef.connect(serverHost, serverPort, clientName, sessionID);
            } catch (IOException e) {
                SwingUtilities.invokeLater(() -> {
                    JOptionPane.showMessageDialog(frame, "Connection failed: " + e.getMessage(), "Error", JOptionPane.ERROR_MESSAGE);
                    exitToWelcome();
                });
            }
        });
        t.setDaemon(true);
        t.start();
    }

    /**
     * Creates a new NetworkListener to handle socket-level events.
     * Protocol-layer sequencing is omitted.
     * * @return Implementation of the NetworkListener.
     */
    private NetworkClient.NetworkListener createNetworkListener() {
        return new NetworkClient.NetworkListener() {
            @Override
            public void onConnected() { connectionEstablished = true; }
            
            @Override
            public void onDisconnected() {
                SwingUtilities.invokeLater(() -> {
                    if (networkClient == null) return; 
                    
                    if (!handshakeCompleted) {
                        if (!intentionalDisconnect) {
                            JOptionPane.showMessageDialog(frame, 
                                "Connection failed. The server might be full or unavailable.", 
                                "Connection Error", 
                                JOptionPane.ERROR_MESSAGE);
                        }
                        exitToWelcome();
                        return;
                    }

                    if (!intentionalDisconnect && !isReconnecting) attemptReconnect();
                    else if (!isReconnecting && !intentionalDisconnect) exitToWelcome();
                });
            }
            
            @Override
            public void onServerMessage(String line) {
                NetworkClient nc = networkClient;
                if (nc == null) return;
                System.out.println("<< " + line);
                parseServerMessage(line);
            }
            
            @Override
            public void onNetworkError(Exception ex) {
                if (networkClient == null) return;
                if (!intentionalDisconnect && !isReconnecting) SwingUtilities.invokeLater(() -> attemptReconnect());
            }
        };
    }

    /**
     * Logic for automatic reconnection. Attempts to reconnect up to MAX_RECONNECT_ATTEMPTS.
     * Upon success, flushes any queued offline messages.
     */
    private void attemptReconnect() {
        if (isReconnecting || intentionalDisconnect) return;
        
        if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
            SwingUtilities.invokeLater(() -> {
                JOptionPane.showMessageDialog(frame, "Unable to reconnect after multiple attempts.", "Connection Failed", JOptionPane.ERROR_MESSAGE);
                exitToWelcome();
            });
            return;
        }
        
        if (!connectionEstablished) {
            JOptionPane.showMessageDialog(frame, "Could not connect to server.", "Connection Error", JOptionPane.ERROR_MESSAGE);
            exitToWelcome();
            return;
        }
        
        isReconnecting = true;
        reconnectAttempts++;
        
        SwingUtilities.invokeLater(() -> {
            statusLabel.setText("Connection lost. Reconnecting (" + reconnectAttempts + "/" + MAX_RECONNECT_ATTEMPTS + ")...");
            statusLabel.setForeground(Color.ORANGE);
            if (lobbyPanel != null) lobbyPanel.setButtonsEnabled(false);
        });
        
        new Thread(() -> {
            try {
                NetworkClient old = networkClient;
                if (old != null) { networkClient = null; old.closeConnection(); }
                
                NetworkClient newNc = new NetworkClient(createNetworkListener());
                networkClient = newNc;
                
                Thread.sleep(1000); 
                
                newNc.connect(serverHost, serverPort, clientName, sessionID);
                
                // Flush the offline queue immediately after connection
                String queuedMsg;
                while ((queuedMsg = offlineQueue.poll()) != null) {
                    newNc.sendRaw(queuedMsg);
                }

                SwingUtilities.invokeLater(() -> {
                    isReconnecting = false;
                    reconnectAttempts = 0; 
                    statusLabel.setText("Connected as " + clientName);
                    statusLabel.setForeground(Color.WHITE);
                    if (gamePanel != null && !gamePanel.isGameEnded()) gamePanel.setControlsEnabled(true);
                    if (lobbyPanel != null) lobbyPanel.setButtonsEnabled(true);
                    
                    if (lobbyPanel.isShowing()) sendNetworkCommand(Protocol.CMD_LIST);
                });
            } catch (Exception e) {
                SwingUtilities.invokeLater(() -> {
                    isReconnecting = false; 
                    if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
                         JOptionPane.showMessageDialog(frame, "Server unavailable.", "Error", JOptionPane.ERROR_MESSAGE);
                         exitToWelcome();
                    } else {
                         attemptReconnect(); 
                    }
                });
            }
        }).start();
    }

    /**
     * Resets the application state and returns to the initial Welcome screen.
     */
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
        offlineQueue.clear(); 
        
        reconnectAttempts = 0; 
        statusLabel.setText("Not connected");
        statusLabel.setForeground(Color.WHITE);
        if (connectBtn != null) connectBtn.setEnabled(true);
    }

    /**
     * Parses messages received from the server and updates the UI accordingly.
     * Sequence numbers are no longer present in the protocol string.
     * * @param msg The raw message string from the server.
     */
    private void parseServerMessage(String msg) {
        String u = msg.trim();
        
        if (u.equals(Protocol.RESP_FULL)) {
            intentionalDisconnect = true; 
            SwingUtilities.invokeLater(() -> {
                JOptionPane.showMessageDialog(frame, 
                    "The server is at maximum capacity. Please try again later.", 
                    "Server Full", 
                    JOptionPane.WARNING_MESSAGE);
                exitToWelcome();
            });
            return;
        }

        if (u.startsWith(Protocol.RESP_WELCOME)) {
            if (reconnectAttempts > 0) reconnectAttempts = 0;
            handshakeCompleted = true;
        }

        if (u.equals("OPP_KICK")) {
             SwingUtilities.invokeLater(() -> {
                 closeDisconnectPopup();
                 showEnd(true, "Opponent was kicked due to illegal input");
             });
             return;
        }

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
            SwingUtilities.invokeLater(() -> {
                if (resultOverlay.isEndOverlayShown()) {
                    pendingLobbyReturn = true;
                    return;
                }
                switchToLobby();
            });
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
                
                String roomInfo = u.substring(Protocol.RESP_WAITING.length()).trim();
                statusLabel.setText("Hosting " + roomInfo + ". Waiting for opponent.");
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
            String errorMsg = u.substring(4);
            if (errorMsg.contains("Server is full")) {
                intentionalDisconnect = true;
            }
            SwingUtilities.invokeLater(() -> {
                JOptionPane.showMessageDialog(frame, "Server Error: " + errorMsg);
                gamePanel.setWaitingForOk(false);
                
                if (errorMsg.contains("Server is full")) {
                    intentionalDisconnect = true;
                    exitToWelcome();
                    return; 
                }

                if (waitingPanel.isShowing()) {
                    switchToLobby();
                }
                if (lobbyPanel.isShowing()) {
                    lobbyPanel.setButtonsEnabled(true);
                }
            });
            return;
        }

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

    /**
     * Displays the victory or defeat overlay.
     * * @param win True if the local player won, false otherwise.
     * @param sub Subtitle description of the outcome.
     */
    private void showEnd(boolean win, String sub) {
        SwingUtilities.invokeLater(() -> {
            closeDisconnectPopup();
            resultOverlay.showEndOverlay(win, sub);
        });
    }
    
    /**
     * Displays a neutral result overlay (e.g., Stalemate).
     * * @param sub Description of the outcome.
     */
    private void showNeutral(String sub) {
        SwingUtilities.invokeLater(() -> {
            closeDisconnectPopup();
            resultOverlay.showNeutralOverlay(sub);
        });
    }

    /**
     * Constructs the Welcome panel where users enter their name and server address.
     * * @return The completed welcome JPanel.
     */
    private JPanel createWelcomePanel() {
        JPanel welcome = new JPanel(new BorderLayout());
        
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
        
        JLabel welcomeTitle = new JLabel("WELCOME", SwingConstants.CENTER);
        welcomeTitle.setFont(new Font("Serif", Font.BOLD, 100));
        welcomeTitle.setForeground(new Color(255, 255, 255, 128)); 
        welcomeLayer.add(welcomeTitle, JLayeredPane.PALETTE_LAYER);

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

        connectBtn = new JButton("Connect to Server");
        connectBtn.setAlignmentX(Component.CENTER_ALIGNMENT);
        connectBtn.addActionListener(e -> {
            serverHost = serverIpField.getText();
            try { serverPort = Integer.parseInt(serverPortField.getText()); } catch(Exception ignore){}
            clientName = nameField.getText();
            
            connectBtn.setEnabled(false);
            statusLabel.setText("Connecting...");
            
            startConnection();
        });
        
        wc.add(Box.createVerticalStrut(10));
        wc.add(connectBtn);
        welcome.add(wc, BorderLayout.SOUTH);
        
        welcome.addComponentListener(new ComponentAdapter() {
            @Override
            public void componentResized(ComponentEvent e) {
                welcomeBg.setBounds(0,0,welcome.getWidth(), welcome.getHeight());
                welcomeLayer.setPreferredSize(welcome.getSize());
                welcomeTitle.setBounds(0, 0, welcome.getWidth(), welcome.getHeight() - 200); 
            }
        });
        welcome.add(welcomeLayer, BorderLayout.CENTER);
        return welcome;
    }
}