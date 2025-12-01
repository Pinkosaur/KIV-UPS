import java.awt.Point;
import java.util.Arrays;

/**
 * BoardModel
 *
 * Encapsulates an 8x8 chessboard (char[][]), en-passant target, and provides the
 * move legality / simulation utilities used by the GUI.
 *
 * This class mirrors the move/attack/king-check logic from the original single-file client,
 * so higher-level code can call these methods with minimal change.
 *
 * Piece encoding:
 *   Uppercase = White: P N B R Q K
 *   Lowercase = Black: p n b r q k
 *   Empty square = '.'
 */
public class BoardModel {
    public final char[][] board;
    public int epR = -1, epC = -1; // en-passant target (model coords) or -1,-1
    public char pendingPromo = 0;   // used if calling applyLocalMove with a pending promo
    public Point lastFrom = null, lastTo = null;

    public BoardModel() {
        board = new char[8][8];
        initBoardModel();
    }

    /** Initialize starting chess position */
    public void initBoardModel() {
        char[] backBlack = {'r','n','b','q','k','b','n','r'};
        char[] backWhite = {'R','N','B','Q','K','B','N','R'};
        for (int c = 0; c < 8; c++) board[0][c] = backBlack[c];
        for (int c = 0; c < 8; c++) board[1][c] = 'p';
        for (int r = 2; r < 6; r++) for (int c = 0; c < 8; c++) board[r][c] = '.';
        for (int c = 0; c < 8; c++) board[6][c] = 'P';
        for (int c = 0; c < 8; c++) board[7][c] = backWhite[c];
        epR = epC = -1;
        pendingPromo = 0;
        lastFrom = lastTo = null;
    }

    public boolean inBounds(int r,int c){ return r>=0 && r<8 && c>=0 && c<8; }

    /** Path-clear test (excludes endpoints). Uses the instance board. */
    public boolean pathClear(int r1,int c1,int r2,int c2) {
        int dr = (r2>r1)?1: (r2<r1)?-1:0;
        int dc = (c2>c1)?1: (c2<c1)?-1:0;
        int r = r1 + dr, c = c1 + dc;
        while (r != r2 || c != c2) {
            if (!inBounds(r,c)) return false;
            if (board[r][c] != '.') return false;
            r += dr; c += dc;
        }
        return true;
    }

    /** Functional variant that checks pathClear on an arbitrary board array */
    public boolean pathClear(char[][] b, int r1,int c1,int r2,int c2) {
        int dr = (r2>r1)?1: (r2<r1)?-1:0;
        int dc = (c2>c1)?1: (c2<c1)?-1:0;
        int r = r1 + dr, c = c1 + dc;
        while (r != r2 || c != c2) {
            if (r < 0 || r >= 8 || c < 0 || c >= 8) return false;
            if (b[r][c] != '.') return false;
            r += dr; c += dc;
        }
        return true;
    }

    /** Return true if the piece on r1,c1 can move to r2,c2 ignoring check (basic board rules). */
    public boolean isLegalMoveBasic(int r1,int c1,int r2,int c2) {
        if (!inBounds(r1,c1) || !inBounds(r2,c2)) return false;
        char p = board[r1][c1];
        if (p=='.') return false;
        char t = board[r2][c2];
        if (t!='.' && (Character.isUpperCase(t)==Character.isUpperCase(p))) return false;
        int dr = r2 - r1; int dc = c2 - c1; int absdr = Math.abs(dr), absdc = Math.abs(dc);

        if (p=='P') {
            if (c1==c2 && board[r2][c2]=='.') { if (dr==-1) return true; if (r1==6 && dr==-2 && board[5][c1]=='.') return true; }
            if (absdc==1 && dr==-1 && t!='.' && Character.isLowerCase(t)) return true;
            /* en-passant capture: diagonal to empty square equals ep target */
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

        /* King: allow normal king steps and permissive client-side castling checks
         * (final legality must be filtered by moveLeavesInCheck). */
        if (p=='K' || p=='k') {
            if (absdr<=1 && absdc<=1) return true;
            /* castling attempt: two-square horizontal move */
            if (r1 == r2 && Math.abs(c2 - c1) == 2) {
                if (p == 'K') {
                    int color = 0; int opp = 1;
                    if (!(r1 == 7 && c1 == 4)) return false;
                    // kingside
                    if (c2 == 6) {
                        if (board[7][7] != 'R') return false;
                        if (board[7][5] != '.' || board[7][6] != '.') return false;
                        if (isSquareAttacked(board, r1, c1, opp)) return false;
                        int midC = 5;
                        if (moveLeavesInCheck(board, color, r1, c1, r1, midC)) return false;
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

    /** Return true if square r,c is attacked by color 'byColor' on the provided board */
    public boolean isSquareAttacked(char[][] b, int r, int c, int byColor) {
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

    /** Find king of the specified color on the instance board; returns {r,c} or null */
    public int[] findKing(int color) {
        char target = (color==0)?'K':'k';
        for (int r=0;r<8;r++) for (int c=0;c<8;c++) if (board[r][c]==target) return new int[]{r,c};
        return null;
    }

    /** Functional findKing over a given board array */
    public int[] findKing(char[][] b, int color) {
        char target = (color==0)?'K':'k';
        for (int r=0;r<8;r++) for (int c=0;c<8;c++) if (b[r][c]==target) return new int[]{r,c};
        return null;
    }

    /**
     * Simulate the move on a copy and return true if after the move the player's king
     * would be attacked (i.e., the move *leaves* the player in check).
     *
     * This mirrors the original client-side simulation, including en-passant and castling handling.
     */
    public boolean moveLeavesInCheck(char[][] b, int color, int r1,int c1,int r2,int c2) {
        char[][] copy = new char[8][8];
        for (int r=0;r<8;r++) System.arraycopy(b[r],0,copy[r],0,8);
        char p = copy[r1][c1];

        /* En-passant capture simulation */
        if ((p == 'P' || p == 'p') && copy[r2][c2] == '.' && c1 != c2) {
            int capR = (p == 'P') ? r2 + 1 : r2 - 1;
            if (inBounds(capR, c2)) copy[capR][c2] = '.';
        }

        /* Castling simulation */
        if ((p == 'K' || p == 'k') && r1 == r2 && Math.abs(c2 - c1) == 2) {
            copy[r2][c2] = p; copy[r1][c1] = '.';
            if (c2 - c1 == 2) {
                copy[r2][5] = copy[r2][7];
                copy[r2][7] = '.';
            } else {
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

        /* Promotions approximate: promote to queen */
        if (p=='P' && r2==0) copy[r2][c2] = 'Q';
        if (p=='p' && r2==7) copy[r2][c2] = 'q';

        int[] kingPos = findKing(copy, color);
        if (kingPos==null) return false;
        return isSquareAttacked(copy, kingPos[0], kingPos[1], 1-color);
    }

    /** Overload using the instance board */
    public boolean moveLeavesInCheck(int color, int r1,int c1,int r2,int c2) {
        return moveLeavesInCheck(this.board, color, r1,c1,r2,c2);
    }

    /** Convenience: returns true if piece belongs to color (0 white, 1 black) */
    public static boolean isOwnPiece(char p, int myColor) {
        if (p=='.') return false;
        if (myColor==0) return Character.isUpperCase(p);
        if (myColor==1) return Character.isLowerCase(p);
        return false;
    }

    /**
     * Apply a local move (from..to) to the instance board, preserving the exact
     * client-side update semantics (castling rook move, en-passant capture, promotion).
     *
     * from/to are two-char algebraic coordinates like "e2","e4".
     */
    public void applyLocalMove(String from, String to) {
        int r1 = 8 - (from.charAt(1) - '0');
        int c1 = from.charAt(0) - 'a';
        int r2 = 8 - (to.charAt(1) - '0');
        int c2 = to.charAt(0) - 'a';
        lastFrom = new Point(r1, c1);
        lastTo = new Point(r2, c2);
        char piece = board[r1][c1];
        board[r1][c1] = '.';
        // Castling: if king moved two squares horizontally, move corresponding rook
        if ((piece == 'K' || piece == 'k') && r1 == r2 && Math.abs(c2 - c1) == 2) {
            board[r2][c2] = piece;
            if (c2 > c1) {
                int rookR = r2, rookFromC = 7, rookToC = 5;
                board[rookR][rookToC] = board[rookR][rookFromC];
                board[rookR][rookFromC] = '.';
            } else {
                int rookR = r2, rookFromC = 0, rookToC = 3;
                board[rookR][rookToC] = board[rookR][rookFromC];
                board[rookR][rookFromC] = '.';
            }
            epR = epC = -1;
            pendingPromo = 0;
            return;
        }
        // en-passant capture
        if ((piece == 'P' || piece == 'p') && board[r2][c2] == '.' && c1 != c2) {
            int capR = (piece == 'P') ? r2 + 1 : r2 - 1;
            if (capR >= 0 && capR < 8 && board[capR][c2] != '.') {
                board[capR][c2] = '.';
            }
        }
        // Normal move
        board[r2][c2] = piece;
        // pawn double step sets ep target
        if (piece == 'P' && r1 == 6 && r2 == 4 && c1 == c2) { epR = 5; epC = c1; }
        else if (piece == 'p' && r1 == 1 && r2 == 3 && c1 == c2) { epR = 2; epC = c1; }
        else { epR = epC = -1; }
        // Promotion: use pendingPromo if set, otherwise queen
        if (piece == 'P' && r2 == 0) {
            if (pendingPromo != 0) board[r2][c2] = Character.toUpperCase(pendingPromo);
            else board[r2][c2] = 'Q';
        } else if (piece == 'p' && r2 == 7) {
            if (pendingPromo != 0) board[r2][c2] = Character.toLowerCase(pendingPromo);
            else board[r2][c2] = 'q';
        }
        pendingPromo = 0;
    }

    /**
     * Apply an opponent move provided as "e7e5" or "e7e8q" (promotion char optional).
     * Mirrors the GUI's applyOpponentMove logic.
     */
    public void applyOpponentMove(String mv) {
        String from = mv.substring(0,2);
        String to = mv.substring(2,4);
        char promo = (mv.length() >= 5) ? mv.charAt(4) : 0;
        int r1 = 8 - (from.charAt(1) - '0');
        int c1 = from.charAt(0) - 'a';
        int r2 = 8 - (to.charAt(1) - '0');
        int c2 = to.charAt(0) - 'a';
        lastFrom = new Point(r1, c1);
        lastTo = new Point(r2, c2);
        char piece = board[r1][c1];
        board[r1][c1] = '.';
        // Castling
        if ((piece == 'K' || piece == 'k') && r1 == r2 && Math.abs(c2 - c1) == 2) {
            board[r2][c2] = piece;
            if (c2 > c1) {
                int rookR = r2, rookFromC = 7, rookToC = 5;
                board[rookR][rookToC] = board[rookR][rookFromC];
                board[rookR][rookFromC] = '.';
            } else {
                int rookR = r2, rookFromC = 0, rookToC = 3;
                board[rookR][rookToC] = board[rookR][rookFromC];
                board[rookR][rookFromC] = '.';
            }
            epR = epC = -1;
            return;
        }
        // en-passant capture detection
        if ((piece == 'P' || piece == 'p') && board[r2][c2] == '.' && c1 != c2) {
            int capR = (piece == 'P') ? r2 + 1 : r2 - 1;
            if (capR >= 0 && capR < 8 && board[capR][c2] != '.') {
                board[capR][c2] = '.';
            }
        }
        board[r2][c2] = piece;
        if (promo != 0) {
            char promoted = (Character.isUpperCase(piece)) ? Character.toUpperCase(promo) : Character.toLowerCase(promo);
            board[r2][c2] = promoted;
        } else {
            if (piece == 'P' && r2 == 0) board[r2][c2] = 'Q';
            if (piece == 'p' && r2 == 7) board[r2][c2] = 'q';
        }
        if (Character.toUpperCase(piece) == 'P' && Math.abs(r2 - r1) == 2) {
            epR = (r1 + r2) / 2;
            epC = c2;
        } else {
            epR = epC = -1;
        }
    }

    /** Return defensive copy of board (if needed) */
    public char[][] getBoardCopy() {
        char[][] copy = new char[8][8];
        for (int r=0;r<8;r++) System.arraycopy(board[r],0,copy[r],0,8);
        return copy;
    }

    @Override
    public String toString() {
        StringBuilder sb = new StringBuilder();
        for (int r=0;r<8;r++) {
            for (int c=0;c<8;c++) sb.append(board[r][c]);
            sb.append('\n');
        }
        return sb.toString();
    }
}
