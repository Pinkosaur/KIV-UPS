/* game.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"
#include "match.h"
#include "logging.h"

/* ... (init_board, piece_color, in_bounds UNCHANGED) ... */

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

int piece_color(Piece p) {
    if (p > 0) return 0; // white
    if (p < 0) return 1; // black
    return -1;
}

int in_bounds(int r,int c) {
    return r>=0 && r<8 && c>=0 && c<8;
}

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

/* HELPER: Centralized geometry logic for sliding/jumping pieces (No Pawns) */
static int piece_can_reach(GameState *g, Piece p, int r1, int c1, int r2, int c2) {
    int dr = r2 - r1;
    int dc = c2 - c1;
    int abs_p = abs(p);

    switch (abs_p) {
        case 2: /* KNIGHT */
            if (abs(dr)*abs(dc) == 2) return 1;
            break;
        case 4: /* ROOK */
            if ((dr==0 || dc==0) && path_clear(g, r1, c1, r2, c2)) return 1;
            break;
        case 3: /* BISHOP */
            if (abs(dr) == abs(dc) && path_clear(g, r1, c1, r2, c2)) return 1;
            break;
        case 5: /* QUEEN */
            if ((dr==0 || dc==0 || abs(dr)==abs(dc)) && path_clear(g, r1, c1, r2, c2)) return 1;
            break;
        case 6: /* KING */
            if (abs(dr)<=1 && abs(dc)<=1) return 1;
            break;
    }
    return 0;
}

int is_square_attacked(GameState *g, int r, int c, int by_color) {
    for (int i=0; i<8; ++i) {
        for (int j=0; j<8; ++j) {
            Piece p = g->board[i][j];
            if (p == EMPTY || piece_color(p) != by_color) continue;

            /* Check Pawns (Special Attack Geometry) */
            if (abs(p) == 1) { // PAWN
                int forward = (by_color == 0) ? -1 : 1; 
                /* Pawns attack diagonals only */
                if (i + forward == r && (j+1 == c || j-1 == c)) return 1;
            } 
            /* Check Others (Standard Geometry) */
            else {
                if (piece_can_reach(g, p, i, j, r, c)) return 1;
            }
        }
    }
    return 0;
}

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

    /* PAWN LOGIC (Unique: Moves vs Captures vs En Passant) */
    if (abs_p == 1) {
        int forward = (color == 0) ? -1 : 1;
        int start_rank = (color == 0) ? 6 : 1;

        /* Move Forward */
        if (dc == 0) {
            if (dest != EMPTY) return 0; /* Blocked */
            if (dr == forward) return 1;
            if (dr == 2*forward && r1 == start_rank && m->state.board[r1+forward][c1] == EMPTY) return 1;
        }
        /* Capture Diagonal */
        else if (abs(dc) == 1 && dr == forward) {
            if (dest != EMPTY) return 1; /* Normal capture */
            /* En Passant */
            if (m->ep_r == r2 && m->ep_c == c2) return 1;
        }
        return 0;
    }
    
    /* CASTLING LOGIC (King special) */
    if (abs_p == 6 && abs(dc) == 2 && dr == 0) {
        /* Standard geometry check will fail for castling, handle here */
        /* Note: Caller is responsible for checking if path is attacked (in apply_move logic usually)
           But here we just check availability flags and path emptiness */
        if (dest != EMPTY) return 0;
        
        // White
        if (color == 0 && r1 == 7 && c1 == 4) {
            if (c2 == 6) { // Kingside
                if (m->w_can_kingside && m->state.board[7][5]==EMPTY && m->state.board[7][6]==EMPTY) return 1;
            } else if (c2 == 2) { // Queenside
                if (m->w_can_queenside && m->state.board[7][3]==EMPTY && m->state.board[7][2]==EMPTY && m->state.board[7][1]==EMPTY) return 1;
            }
        }
        // Black
        else if (color == 1 && r1 == 0 && c1 == 4) {
            if (c2 == 6) { // Kingside
                if (m->b_can_kingside && m->state.board[0][5]==EMPTY && m->state.board[0][6]==EMPTY) return 1;
            } else if (c2 == 2) { // Queenside
                if (m->b_can_queenside && m->state.board[0][3]==EMPTY && m->state.board[0][2]==EMPTY && m->state.board[0][1]==EMPTY) return 1;
            }
        }
        return 0;
    }

    /* ALL OTHER PIECES (Shared Logic) */
    return piece_can_reach(&m->state, p, r1, c1, r2, c2);
}

/* ... (apply_move, move_leaves_in_check [Optimized], has_any_legal_move, is_in_check, find_king, is_move_format, parse_move UNCHANGED) ... */
void apply_move(Match *m, int r1,int c1,int r2,int c2, char promo_char) {
    Piece p = m->state.board[r1][c1];
    
    /* Reset en-passant target (unless a new double pawn move sets it below) */
    m->ep_r = m->ep_c = -1;

    /* Handle Castling */
    if (abs(p) == 6 && abs(c2 - c1) == 2) {
        m->state.board[r2][c2] = p;
        m->state.board[r1][c1] = EMPTY;
        /* Move Rook */
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
        /* Remove captured pawn */
        m->state.board[r1][c2] = EMPTY; 
    }
    else {
        m->state.board[r2][c2] = p;
        m->state.board[r1][c1] = EMPTY;
    }

    /* Update Castling Rights */
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

    /* Set En Passant Target */
    if (abs(p) == 1 && abs(r2 - r1) == 2) {
        m->ep_r = (r1 + r2) / 2;
        m->ep_c = c1;
    }

    /* Promotion */
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

int move_leaves_in_check(Match *m, int color, int r1, int c1, int r2, int c2) {
    GameState *g = &m->state;
    Piece moving_piece = g->board[r1][c1];
    Piece captured_piece = g->board[r2][c2];
    Piece ep_captured_pawn = EMPTY;
    int ep_cap_r = -1, ep_cap_c = -1;

    if ((moving_piece == WPAWN || moving_piece == BPAWN) && captured_piece == EMPTY && c1 != c2) {
        ep_cap_r = r1; 
        ep_cap_c = c2;
        ep_captured_pawn = g->board[ep_cap_r][ep_cap_c];
    }

    g->board[r2][c2] = moving_piece;
    g->board[r1][c1] = EMPTY;
    if (ep_captured_pawn != EMPTY) {
        g->board[ep_cap_r][ep_cap_c] = EMPTY;
    }
    
    int in_check = is_in_check(g, color);

    g->board[r1][c1] = moving_piece;
    g->board[r2][c2] = captured_piece;
    if (ep_captured_pawn != EMPTY) {
        g->board[ep_cap_r][ep_cap_c] = ep_captured_pawn;
    }

    return in_check;
}

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

int is_in_check(GameState *g, int color) {
    int kr, kc;
    if (!find_king(g, color, &kr, &kc)) return 0;
    return is_square_attacked(g, kr, kc, 1 - color);
}

int find_king(GameState *g, int color, int *rk, int *ck) {
    Piece k = (color == 0) ? WKING : BKING;
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            if (g->board[r][c] == k) { *rk = r; *ck = c; return 1; }
        }
    }
    return 0;
}

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

void parse_move(const char *m, int *r1, int *c1, int *r2, int *c2) {
    *c1 = m[0] - 'a';
    *r1 = 7 - (m[1] - '1');
    *c2 = m[2] - 'a';
    *r2 = 7 - (m[3] - '1');
}