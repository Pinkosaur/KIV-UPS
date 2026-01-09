/**
 * @file game.c
 * @brief Core chess logic implementation.
 *
 * This file contains the rules engine for the chess server. It handles board initialization,
 * movement validation (including special moves like castling and en passant), move application,
 * and check/checkmate detection. It operates on the GameState and Match structures.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"
#include "match.h"
#include "logging.h"

/**
 * @brief Initializes the game board to the standard starting chess position.
 * * Sets up the 8x8 grid with pieces:
 * - White pieces: Rows 6 (pawns) and 7 (royals).
 * - Black pieces: Rows 1 (pawns) and 0 (royals).
 * - Empty squares: Rows 2 through 5.
 * * @param g Pointer to the GameState structure to initialize.
 */
void init_board(GameState *g) {
    Piece init[8][8] = {
        {BROOK, BKNIGHT, BBISHOP, BQUEEN, BKING, BBISHOP, BKNIGHT, BROOK},
        {BPAWN, BPAWN, BPAWN, BPAWN, BPAWN, BPAWN, BPAWN, BPAWN},
        {EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY},
        {EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY},
        {EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY},
        {EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY},
        {WPAWN, WPAWN, WPAWN, WPAWN, WPAWN, WPAWN, WPAWN, WPAWN},
        {WROOK, WKNIGHT, WBISHOP, WQUEEN, WKING, WBISHOP, WKNIGHT, WROOK}
    };
    memcpy(g->board, init, sizeof(init));
}

/**
 * @brief Determines the color of a given piece.
 * * The internal representation uses positive integers for White pieces
 * and negative integers for Black pieces.
 * * @param p The piece to check.
 * @return 0 for White, 1 for Black, -1 for Empty/Invalid.
 */
int piece_color(Piece p) {
    if (p > 0) return 0; // white
    if (p < 0) return 1; // black
    return -1;
}

/**
 * @brief Checks if coordinates are within the board boundaries (0-7).
 * @return 1 if valid, 0 otherwise.
 */
int in_bounds(int r,int c) {
    return r>=0 && r<8 && c>=0 && c<8;
}

/**
 * @brief Checks if the path between two squares is clear of obstructions.
 * * Travels along the rank, file, or diagonal from (r1, c1) to (r2, c2).
 * Does not check the start or destination squares, only the squares in between.
 * * @return 1 if the path is clear, 0 if blocked.
 */
int path_clear(GameState *g, int r1,int c1,int r2,int c2) {
    int dr = r2 - r1;
    int dc = c2 - c1;
    int steps = 0;
    
    if (abs(dr) > abs(dc)) steps = abs(dr);
    else steps = abs(dc);

    int step_r = 0; if (dr!=0) step_r = dr/steps;
    int step_c = 0; if (dc!=0) step_c = dc/steps;

    int r = r1 + step_r;
    int c = c1 + step_c;
    for (int i=0; i<steps-1; ++i) {
        if (g->board[r][c] != EMPTY) return 0;
        r += step_r;
        c += step_c;
    }
    return 1;
}

/**
 * @brief Internal helper to validate geometric movement rules for sliding/jumping pieces.
 * * Does NOT verify if the destination contains a friendly piece or handles special
 * pawn logic. This function purely checks if a piece *type* is capable of the 
 * requested displacement.
 * * @param g Game state.
 * @param p The piece moving.
 * @param r1, c1 Start coordinates.
 * @param r2, c2 Destination coordinates.
 * @return 1 if the geometry and path are valid, 0 otherwise.
 */
static int piece_can_reach(GameState *g, Piece p, int r1, int c1, int r2, int c2) {
    int dr = r2 - r1;
    int dc = c2 - c1;
    int abs_p = abs(p);

    switch (abs_p) {
        case 2: /* KNIGHT: L-shape jump */
            if (abs(dr)*abs(dc) == 2) return 1;
            break;
        case 4: /* ROOK: Straight lines */
            if ((dr==0 || dc==0) && path_clear(g, r1, c1, r2, c2)) return 1;
            break;
        case 3: /* BISHOP: Diagonals */
            if (abs(dr) == abs(dc) && path_clear(g, r1, c1, r2, c2)) return 1;
            break;
        case 5: /* QUEEN: Straight lines or Diagonals */
            if ((dr==0 || dc==0 || abs(dr)==abs(dc)) && path_clear(g, r1, c1, r2, c2)) return 1;
            break;
        case 6: /* KING: One square in any direction */
            if (abs(dr)<=1 && abs(dc)<=1) return 1;
            break;
    }
    return 0;
}

/**
 * @brief Checks if a specific square is under attack by an opponent.
 * * Used primarily for validating castling rights and checking for check/checkmate.
 * * @param r, c Coordinates of the square to check.
 * @param by_color The color of the attacker (0=White, 1=Black).
 * @return 1 if attacked, 0 otherwise.
 */
int is_square_attacked(GameState *g, int r, int c, int by_color) {
    for (int i=0; i<8; ++i) {
        for (int j=0; j<8; ++j) {
            Piece p = g->board[i][j];
            if (p == EMPTY || piece_color(p) != by_color) continue;

            /* Pawns have unique capture geometry (diagonal only) */
            if (abs(p) == 1) { 
                int forward = (by_color == 0) ? -1 : 1; 
                if (i + forward == r && (j+1 == c || j-1 == c)) return 1;
            } 
            /* All other pieces use standard reachability */
            else {
                if (piece_can_reach(g, p, i, j, r, c)) return 1;
            }
        }
    }
    return 0;
}

/**
 * @brief Checks basic legality of a move (geometry, obstructions, turn order).
 * * Does NOT check if the move leaves the king in check (that is handled by `move_leaves_in_check`).
 * Includes logic for En Passant and Castling preconditions.
 * * @param m The match context (includes board and special state flags).
 * @param color The side making the move.
 * @return 1 if the move is physically possible on the board, 0 otherwise.
 */
int is_legal_move_basic(Match *m, int color, int r1, int c1, int r2, int c2) {
    if (!in_bounds(r1,c1) || !in_bounds(r2,c2)) return 0;
    if (r1 == r2 && c1 == c2) return 0;

    Piece p = m->state.board[r1][c1];
    if (p == EMPTY) return 0;
    if (piece_color(p) != color) return 0;

    Piece dest = m->state.board[r2][c2];
    /* Cannot capture own piece */
    if (dest != EMPTY && piece_color(dest) == color) return 0;

    int dr = r2 - r1;
    int dc = c2 - c1;
    int abs_p = abs(p);

    /* PAWN LOGIC */
    if (abs_p == 1) {
        int forward = (color == 0) ? -1 : 1;
        int start_rank = (color == 0) ? 6 : 1;

        /* Move Forward (non-capture) */
        if (dc == 0) {
            if (dest != EMPTY) return 0; /* Blocked */
            if (dr == forward) return 1;
            if (dr == 2*forward && r1 == start_rank && m->state.board[r1+forward][c1] == EMPTY) return 1;
        }
        /* Capture Diagonal */
        else if (abs(dc) == 1 && dr == forward) {
            if (dest != EMPTY) return 1; /* Standard capture */
            /* En Passant capture: Destination is empty, but matches EP target */
            if (m->ep_r == r2 && m->ep_c == c2) return 1;
        }
        return 0;
    }
    
    /* CASTLING LOGIC */
    if (abs_p == 6 && abs(dc) == 2 && dr == 0) {
        if (dest != EMPTY) return 0;
        
        // White Castling
        if (color == 0 && r1 == 7 && c1 == 4) {
            if (c2 == 6) { // Kingside
                if (m->w_can_kingside && m->state.board[7][5]==EMPTY && m->state.board[7][6]==EMPTY) return 1;
            } else if (c2 == 2) { // Queenside
                if (m->w_can_queenside && m->state.board[7][3]==EMPTY && m->state.board[7][2]==EMPTY && m->state.board[7][1]==EMPTY) return 1;
            }
        }
        // Black Castling
        else if (color == 1 && r1 == 0 && c1 == 4) {
            if (c2 == 6) { // Kingside
                if (m->b_can_kingside && m->state.board[0][5]==EMPTY && m->state.board[0][6]==EMPTY) return 1;
            } else if (c2 == 2) { // Queenside
                if (m->b_can_queenside && m->state.board[0][3]==EMPTY && m->state.board[0][2]==EMPTY && m->state.board[0][1]==EMPTY) return 1;
            }
        }
        return 0;
    }

    /* ALL OTHER PIECES (Standard Geometry) */
    return piece_can_reach(&m->state, p, r1, c1, r2, c2);
}

/**
 * @brief Executes a move on the board, updating game state.
 * * Handles piece displacement, captures, castling rook movement, en passant pawn removal,
 * and pawn promotion. Updates castling rights flags and en passant targets.
 * * @param promo_char Character indicating promotion choice ('q','r','b','n'), or 0 for default.
 */
void apply_move(Match *m, int r1,int c1,int r2,int c2, char promo_char) {
    Piece p = m->state.board[r1][c1];
    
    /* Reset en-passant target by default */
    m->ep_r = m->ep_c = -1;

    /* Handle Castling */
    if (abs(p) == 6 && abs(c2 - c1) == 2) {
        m->state.board[r2][c2] = p;
        m->state.board[r1][c1] = EMPTY;
        /* Move the corresponding rook */
        if (c2 > c1) { // Kingside
            Piece r = m->state.board[r1][7];
            m->state.board[r1][5] = r;
            m->state.board[r1][7] = EMPTY;
        } else { // Queenside
            Piece r = m->state.board[r1][0];
            m->state.board[r1][3] = r;
            m->state.board[r1][0] = EMPTY;
        }
    }
    /* Handle En Passant Capture */
    else if (abs(p) == 1 && c1 != c2 && m->state.board[r2][c2] == EMPTY) {
        m->state.board[r2][c2] = p;
        m->state.board[r1][c1] = EMPTY;
        /* Remove the pawn captured 'in passing' */
        m->state.board[r1][c2] = EMPTY; 
    }
    else {
        /* Standard Move/Capture */
        m->state.board[r2][c2] = p;
        m->state.board[r1][c1] = EMPTY;
    }

    /* Update Castling Rights based on King/Rook movement */
    if (p == WKING) { m->w_can_kingside = 0; m->w_can_queenside = 0; }
    if (p == BKING) { m->b_can_kingside = 0; m->b_can_queenside = 0; }
    if (p == WROOK) {
        if (r1 == 7 && c1 == 0) m->w_can_queenside = 0;
        if (r1 == 7 && c1 == 7) m->w_can_kingside = 0;
    }
    if (p == BROOK) {
        if (r1 == 0 && c1 == 0) m->b_can_queenside = 0;
        if (r1 == 0 && c1 == 7) m->b_can_kingside = 0;
    }

    /* Set En Passant Target if pawn moved two squares */
    if (abs(p) == 1 && abs(r2 - r1) == 2) {
        m->ep_r = (r1 + r2) / 2;
        m->ep_c = c1;
    }

    /* Handle Promotion */
    if (abs(p) == 1) {
        if ((piece_color(p) == 0 && r2 == 0) || (piece_color(p) == 1 && r2 == 7)) {
            Piece newP = (piece_color(p) == 0) ? WQUEEN : BQUEEN;
            if (promo_char == 'r' || promo_char == 'R') newP = (piece_color(p)==0)? WROOK:BROOK;
            if (promo_char == 'b' || promo_char == 'B') newP = (piece_color(p)==0)? WBISHOP:BBISHOP;
            if (promo_char == 'n' || promo_char == 'N') newP = (piece_color(p)==0)? WKNIGHT:BKNIGHT;
            m->state.board[r2][c2] = newP;
        }
    }
}

/**
 * @brief Simulates a move to check if it results in the player's own king being attacked.
 * * Performs the move on the actual board state, checks for check, and then reverts the move.
 * This approach avoids allocating a temporary board copy.
 * * @return 1 if the move is illegal because it leaves the king in check, 0 otherwise.
 */
int move_leaves_in_check(Match *m, int color, int r1, int c1, int r2, int c2) {
    GameState *g = &m->state;
    Piece moving_piece = g->board[r1][c1];
    Piece captured_piece = g->board[r2][c2];
    Piece ep_captured_pawn = EMPTY;
    int ep_cap_r = -1, ep_cap_c = -1;

    /* Handle En Passant capture logic for simulation */
    if ((moving_piece == WPAWN || moving_piece == BPAWN) && captured_piece == EMPTY && c1 != c2) {
        ep_cap_r = r1; 
        ep_cap_c = c2;
        ep_captured_pawn = g->board[ep_cap_r][ep_cap_c];
    }

    /* Apply Move Temporarily */
    g->board[r2][c2] = moving_piece;
    g->board[r1][c1] = EMPTY;
    if (ep_captured_pawn != EMPTY) {
        g->board[ep_cap_r][ep_cap_c] = EMPTY;
    }
    
    int in_check = is_in_check(g, color);

    /* Revert Move */
    g->board[r1][c1] = moving_piece;
    g->board[r2][c2] = captured_piece;
    if (ep_captured_pawn != EMPTY) {
        g->board[ep_cap_r][ep_cap_c] = ep_captured_pawn;
    }

    return in_check;
}

/**
 * @brief Checks if the player has any legal moves available.
 * * Iterates through all pieces belonging to the player and attempts to find at least
 * one move that is both basic-legal and safe for the king.
 * * @return 1 if at least one move exists, 0 if no moves are possible (Checkmate or Stalemate).
 */
int has_any_legal_move(Match *m, int color) {
    for (int r1=0; r1<8; ++r1) {
        for (int c1=0; c1<8; ++c1) {
            Piece p = m->state.board[r1][c1];
            if (p == EMPTY || piece_color(p) != color) continue;
            for (int r2=0; r2<8; ++r2) {
                for (int c2=0; c2<8; ++c2) {
                    if (is_legal_move_basic(m, color, r1, c1, r2, c2)) {
                        if (!move_leaves_in_check(m, color, r1, c1, r2, c2)) return 1;
                    }
                }
            }
        }
    }
    return 0;
}

/**
 * @brief Determines if the king of the specified color is currently under attack.
 * @return 1 if in check, 0 otherwise.
 */
int is_in_check(GameState *g, int color) {
    int kr, kc;
    if (!find_king(g, color, &kr, &kc)) return 0;
    return is_square_attacked(g, kr, kc, 1 - color);
}

/**
 * @brief Locates the coordinates of the king for a specific color.
 * @param rk Pointer to store the row index.
 * @param ck Pointer to store the column index.
 * @return 1 if found, 0 if king is missing (should not happen in valid game).
 */
int find_king(GameState *g, int color, int *rk, int *ck) {
    Piece k = (color == 0) ? WKING : BKING;
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            if (g->board[r][c] == k) { *rk = r; *ck = c; return 1; }
        }
    }
    return 0;
}

/**
 * @brief Validates the syntax of a move string.
 * Supports standard algebraic notation like "e2e4" or "a7a8q".
 */
int is_move_format(const char *m) {
    if (!m) return 0;
    size_t len = strlen(m);
    if (len != 4 && len != 5) return 0;
    if (m[0] < 'a' || m[0] > 'h') return 0;
    if (m[1] < '1' || m[1] > '8') return 0;
    if (m[2] < 'a' || m[2] > 'h') return 0;
    if (m[3] < '1' || m[3] > '8') return 0;
    return 1;
}

/**
 * @brief Converts an algebraic move string into board indices.
 * Note: '1' maps to row 7, '8' maps to row 0.
 */
void parse_move(const char *m, int *r1, int *c1, int *r2, int *c2) {
    *c1 = m[0] - 'a';
    *r1 = 7 - (m[1] - '1');
    *c2 = m[2] - 'a';
    *r2 = 7 - (m[3] - '1');
}