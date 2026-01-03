import javax.swing.*;
import java.awt.*;
import java.awt.event.*;
import java.util.HashSet;
import java.util.Set;

public class GamePanel extends JPanel {
    private final ChessClient controller;
    private final ImageManager imageManager;
    private final BoardModel boardModel = new BoardModel();
    private final SquareLabel[][] squares = new SquareLabel[8][8];
    private final JPanel boardPanel;
    private final JPanel boardContainer;
    private final JButton resignBtn;
    private final JButton drawBtn;
    
    private static final int SQUARE_SIZE = 64;

    // Game State Local Cache
    private char[][] board;
    private int myColor = -1;
    private boolean myTurn = false;
    
    // Selection State
    private int selR = -1, selC = -1;
    private Set<Point> highlighted = new HashSet<>();
    private boolean waitingForOk = false;
    private boolean gameEnded = false;
    
    // King Check State
    private boolean kingInCheck = false;
    private int kingR = -1, kingC = -1;
    private Point lastFrom = null;
    private Point lastTo = null;

    public GamePanel(ChessClient controller, ImageManager imageManager) {
        this.controller = controller;
        this.imageManager = imageManager;
        this.board = boardModel.board;
        setLayout(new BorderLayout());

        // 1. Board Grid
        boardPanel = new JPanel(new GridLayout(8, 8));
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) {
                SquareLabel sq = new SquareLabel("", SwingConstants.CENTER);
                sq.setPreferredSize(new Dimension(SQUARE_SIZE, SQUARE_SIZE));
                // Default checkerboard colors (updated in updateBoardUI)
                if ((r + c) % 2 == 0) sq.setBackground(new Color(240, 240, 240));
                else sq.setBackground(new Color(160, 160, 160));
                
                sq.setBorder(BorderFactory.createLineBorder(Color.BLACK, 1));
                final int fr = r, fc = c;
                sq.addMouseListener(new MouseAdapter() {
                    public void mouseClicked(MouseEvent ev) { onSquareClicked(fr, fc); }
                });
                squares[r][c] = sq;
                boardPanel.add(sq);
            }
        }

        boardContainer = new JPanel(new GridBagLayout());
        boardContainer.setOpaque(false);
        boardContainer.add(boardPanel, new GridBagConstraints());
        
        // Responsive Resizing
        boardContainer.addComponentListener(new ComponentAdapter() {
            @Override
            public void componentResized(ComponentEvent e) {
                int w = boardContainer.getWidth();
                int h = boardContainer.getHeight();
                int size = Math.min(w, h) - 20;
                if (size < 40) size = 40;
                size = (size / 8) * 8; // Snap to multiple of 8
                boardPanel.setPreferredSize(new Dimension(size, size));
                boardPanel.revalidate();
                imageManager.clearCacheForSize(size);
                updateBoardUI();
            }
        });
        add(boardContainer, BorderLayout.CENTER);

        // 2. Controls
        JPanel ctrl = new JPanel(new FlowLayout(FlowLayout.CENTER, 8, 0));
        resignBtn = new JButton("Resign");
        drawBtn = new JButton("Offer Draw");
        resignBtn.setEnabled(false);
        drawBtn.setEnabled(false);
        
        resignBtn.addActionListener(e -> {
            int ok = JOptionPane.showConfirmDialog(this, "Resign?", "Confirm", JOptionPane.YES_NO_OPTION);
            if (ok == JOptionPane.YES_OPTION) {
                controller.sendNetworkCommand(Protocol.CMD_RES);
                setControlsEnabled(false);
            }
        });
        
        drawBtn.addActionListener(e -> {
            controller.sendNetworkCommand(Protocol.CMD_DRW_OFF);
            drawBtn.setEnabled(false);
        });
        
        ctrl.add(drawBtn);
        ctrl.add(resignBtn);
        add(ctrl, BorderLayout.SOUTH);
    }

    public boolean isGameEnded() {
        return gameEnded;
    }

    public void initGame(int color, boolean isTurn) {
        this.myColor = color;
        this.myTurn = isTurn;
        this.gameEnded = false;
        this.waitingForOk = false;
        
        // Reset Board Model
        boardModel.initBoardModel();
        this.board = boardModel.board;
        this.lastFrom = null;
        this.lastTo = null;
        this.selR = -1;
        this.selC = -1;
        this.highlighted.clear();
        
        setControlsEnabled(true);
        updateBoardUI();
    }
    
    /* [FIX] Added getter for ChessClient to access color safely */
    public int getMyColor() {
        return myColor;
    }
    
    public void setControlsEnabled(boolean enabled) {
        resignBtn.setEnabled(enabled);
        drawBtn.setEnabled(enabled);
    }

    public void applyLocalMove(String from, String to) {
        boardModel.applyLocalMove(from, to);
        lastFrom = boardModel.lastFrom;
        lastTo = boardModel.lastTo;
        this.board = boardModel.board;
        boardModel.pendingPromo = 0;
        
        // Clear selection
        selR = selC = -1;
        highlighted.clear();
        
        updateBoardUI();
    }

    public void applyOpponentMove(String mv) {
        boardModel.applyOpponentMove(mv);
        this.board = boardModel.board;
        lastFrom = boardModel.lastFrom;
        lastTo = boardModel.lastTo;
        myTurn = true;
        waitingForOk = false;
        updateBoardUI();
    }
    
    public void setPendingPromo(char promo) {
        boardModel.pendingPromo = promo;
    }
    
    public void setWaitingForOk(boolean waiting) {
        this.waitingForOk = waiting;
    }
    
    public void setGameEnded(boolean ended) {
        this.gameEnded = ended;
        if (ended) setControlsEnabled(false);
    }
    
    public void setTurn(boolean turn) {
        this.myTurn = turn;
    }

    private void updateBoardUI() {
        SwingUtilities.invokeLater(() -> {
            computeKingCheck();
            for (int uiR = 0; uiR < 8; uiR++) {
                for (int uiC = 0; uiC < 8; uiC++) {
                    // Flip board if playing Black
                    int modelR = (myColor == 1) ? (7 - uiR) : uiR;
                    int modelC = (myColor == 1) ? (7 - uiC) : uiC;
                    char p = board[modelR][modelC];

                    int boardPx = Math.min(boardPanel.getWidth(), boardPanel.getHeight());
                    int cellSize = (boardPx > 0) ? (boardPx / 8) : 64;
                    
                    ImageIcon icon = imageManager.getScaledIconForPiece(p, cellSize);
                    squares[uiR][uiC].setIcon(icon);
                    squares[uiR][uiC].setText((icon == null) ? (p == '.' ? "" : "" + p) : "");

                    boolean isLastFrom = (lastFrom != null && lastFrom.x == modelR && lastFrom.y == modelC);
                    boolean isLastTo = (lastTo != null && lastTo.x == modelR && lastTo.y == modelC);

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
        });
    }

    private void computeKingCheck() {
        if (myColor != 0 && myColor != 1) {
            kingInCheck = false; kingR = kingC = -1; return;
        }
        int[] pos = boardModel.findKing(board, myColor);
        if (pos == null) {
            kingInCheck = false; kingR = kingC = -1; return;
        }
        kingR = pos[0];
        kingC = pos[1];
        kingInCheck = boardModel.isSquareAttacked(board, kingR, kingC, 1 - myColor);
    }

    private void onSquareClicked(int uiR, int uiC) {
        if (waitingForOk || gameEnded) return;
        
        int r = (myColor == 1) ? (7 - uiR) : uiR;
        int c = (myColor == 1) ? (7 - uiC) : uiC;

        // 1. Select Piece
        if (selR == -1) {
            char p = board[r][c];
            if (p == '.') return;
            if (!BoardModel.isOwnPiece(p, myColor)) return;
            if (!myTurn) return;

            selR = r; 
            selC = c;
            highlighted.clear();
            
            // Calculate pseudo-legal moves locally
            for (int tr = 0; tr < 8; tr++) {
                for (int tc = 0; tc < 8; tc++) {
                    if (!boardModel.isLegalMoveBasic(selR, selC, tr, tc)) continue;
                    if (boardModel.moveLeavesInCheck(board, myColor, selR, selC, tr, tc)) continue;
                    highlighted.add(new Point(tr, tc));
                }
            }
            updateBoardUI();
        } 
        // 2. Move or Deselect
        else {
            if (selR == r && selC == c) {
                // Clicked self -> Deselect
                selR = selC = -1;
                highlighted.clear();
                updateBoardUI();
                return;
            }
            
            if (highlighted.contains(new Point(r, c))) {
                // Execute Move
                char moving = board[selR][selC];
                boolean isPawnPromotion = (moving == 'P' && r == 0) || (moving == 'p' && r == 7);
                String from = Utils.coordToAlg(selR, selC);
                String to = Utils.coordToAlg(r, c);
                
                // Store pending move locally so we can apply it when OK_MV arrives
                controller.setPendingMove(from, to);

                if (isPawnPromotion) {
                    SwingUtilities.invokeLater(() -> {
                        String[] options = {"Queen", "Rook", "Bishop", "Knight"};
                        int choice = JOptionPane.showOptionDialog(this, "Promote to:", "Promotion",
                                JOptionPane.DEFAULT_OPTION, JOptionPane.PLAIN_MESSAGE, null, options, options[0]);
                        char promo = 'q';
                        if (choice == 1) promo = 'r';
                        else if (choice == 2) promo = 'b';
                        else if (choice == 3) promo = 'n';
                        
                        setPendingPromo(promo);
                        waitingForOk = true;
                        controller.sendNetworkCommand(Protocol.CMD_MV + from + to + promo);
                    });
                } else {
                    setPendingPromo((char)0);
                    waitingForOk = true;
                    controller.sendNetworkCommand(Protocol.CMD_MV + from + to);
                }
                
                selR = selC = -1;
                highlighted.clear();
                updateBoardUI();
                return;
            }
            
            // Clicked another own piece -> Change selection
            char p = board[r][c];
            if (p != '.' && BoardModel.isOwnPiece(p, myColor)) {
                selR = r; selC = c;
                highlighted.clear();
                for (int tr = 0; tr < 8; tr++) {
                    for (int tc = 0; tc < 8; tc++) {
                        if (boardModel.isLegalMoveBasic(selR, selC, tr, tc) && 
                            !boardModel.moveLeavesInCheck(board, myColor, selR, selC, tr, tc)) {
                            highlighted.add(new Point(tr, tc));
                        }
                    }
                }
                updateBoardUI();
            } else {
                // Clicked empty or enemy -> Deselect
                selR = selC = -1;
                highlighted.clear();
                updateBoardUI();
            }
        }
    }
}