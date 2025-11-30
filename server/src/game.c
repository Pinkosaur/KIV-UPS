#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"
#include "match.h"

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


int path_clear(GameState *g, int r1, int c1, int r2, int c2) {
    int dr = (r2 > r1) ? 1 : (r2 < r1) ? -1 : 0;
    int dc = (c2 > c1) ? 1 : (c2 < c1) ? -1 : 0;
    int r = r1 + dr;
    int c = c1 + dc;
    while (r != r2 || c != c2) {
        if (!in_bounds(r, c)) return 0; /* defensive */
        if (g->board[r][c] != EMPTY) return 0;
        r += dr; c += dc;
    }
    return 1;
}


/* return 1 if square (r,c) is attacked by any piece of by_color */
int is_square_attacked(GameState *g, int r, int c, int by_color) {
    for (int r0 = 0; r0 < 8; ++r0) {
        for (int c0 = 0; c0 < 8; ++c0) {
            Piece p = g->board[r0][c0];
            if (piece_color(p) != by_color) continue;
            int dr = r - r0;
            int dc = c - c0;
            int absdr = dr < 0 ? -dr : dr;
            int absdc = dc < 0 ? -dc : dc;

            switch (p) {
                case WPAWN:
                    /* white pawns attack one rank up (r0-1) and one file left/right */
                    if (r == r0 - 1 && (c == c0 - 1 || c == c0 + 1)) return 1;
                    break;
                case BPAWN:
                    if (r == r0 + 1 && (c == c0 - 1 || c == c0 + 1)) return 1;
                    break;
                case WKNIGHT: case BKNIGHT:
                    if ((absdr==2 && absdc==1) || (absdr==1 && absdc==2)) return 1;
                    break;
                case WBISHOP: case BBISHOP:
                    if (absdr == absdc && path_clear(g, r0, c0, r, c)) return 1;
                    break;
                case WROOK: case BROOK:
                    if ((dr==0 || dc==0) && path_clear(g, r0, c0, r, c)) return 1;
                    break;
                case WQUEEN: case BQUEEN:
                    if ((absdr==absdc || dr==0 || dc==0) && path_clear(g, r0, c0, r, c)) return 1;
                    break;
                case WKING: case BKING:
                    if (absdr <= 1 && absdc <= 1) return 1;
                    break;
                default:
                    break;
            }
        }
    }
    return 0;
}


int is_legal_move_basic(Match *m, int color, int r1, int c1, int r2, int c2) {
    GameState *g = &m->state;
    if (!in_bounds(r1,c1) || !in_bounds(r2,c2)) return 0;
    Piece p = g->board[r1][c1];
    if (piece_color(p) != color) return 0;
    Piece t = g->board[r2][c2];
    if (piece_color(t) == color) return 0;

    int dr = r2 - r1;
    int dc = c2 - c1;
    int absdr = dr < 0 ? -dr : dr;
    int absdc = dc < 0 ? -dc : dc;

    /* Pawns */
    if (p == WPAWN) {
        /* forward */
        if (c1 == c2 && g->board[r2][c2] == EMPTY) {
            if (dr == -1) return 1;
            if (r1 == 6 && dr == -2 && g->board[5][c1] == EMPTY) return 1;
        }
        /* capture normal */
        if (absdc == 1 && dr == -1 && t < 0) return 1;
        /* en-passant capture: target square is empty, diagonal move, and ep target equals r2,c2 */
        if (absdc == 1 && dr == -1 && t == EMPTY && m->ep_r == r2 && m->ep_c == c2) return 1;
        return 0;
    }
    if (p == BPAWN) {
        if (c1 == c2 && g->board[r2][c2] == EMPTY) {
            if (dr == 1) return 1;
            if (r1 == 1 && dr == 2 && g->board[2][c1] == EMPTY) return 1;
        }
        if (absdc == 1 && dr == 1 && t > 0) return 1;
        if (absdc == 1 && dr == 1 && t == EMPTY && m->ep_r == r2 && m->ep_c == c2) return 1;
        return 0;
    }

    /* Knight */
    if (p == WKNIGHT || p == BKNIGHT) {
        return (absdr==2 && absdc==1) || (absdr==1 && absdc==2);
    }

    /* Bishop */
    if (p == WBISHOP || p == BBISHOP) {
        if (absdr != absdc) return 0;
        return path_clear(g, r1, c1, r2, c2);
    }

    /* Rook */
    if (p == WROOK || p == BROOK) {
        if (dr!=0 && dc!=0) return 0;
        return path_clear(g, r1, c1, r2, c2);
    }

    /* Queen */
    if (p == WQUEEN || p == BQUEEN) {
        if (absdr == absdc || dr==0 || dc==0) return path_clear(g, r1, c1, r2, c2);
        return 0;
    }

    /* King (including castling) */
    if (p == WKING || p == BKING) {
        /* normal 1-square king move */
        if (absdr <= 1 && absdc <= 1) return 1;

        /* castling: king attempts to move 2 files horizontally on starting rank */
        if (r1 == r2 && absdc == 2) {

            /* check that the king doesn't cross an attacked square */
            int opp = 1 - color;
            if (is_square_attacked(g, r1, c1, opp)) return 0; /* king currently in check */
            if (c2 == 6) { /* kingside: check squares c=5 and c=6 */
                if (is_square_attacked(g, r1, 5, opp) || is_square_attacked(g, r1, 6, opp)) return 0;
            }
            if (c2 == 2) { /* queenside: check squares c=3 and c=2 (king passes through c=3) */
                if (is_square_attacked(g, r1, 3, opp) || is_square_attacked(g, r1, 2, opp)) return 0;
            }

            /* basic checks: king on starting square, rook present on that side, path clear */
            if (color == 0) {
                /* white must be on rank 7? (board indexing: 0=black back rank, 7=white back rank) */
                if (!(r1 == 7 && g->board[7][4] == WKING)) return 0; /* king not in original square */
                /* kingside */
                if (c2 == 6 && m->w_can_kingside) {
                    if (g->board[7][7] != WROOK) return 0;
                    if (g->board[7][5] != EMPTY || g->board[7][6] != EMPTY) return 0;
                    return 1;
                }
                /* queenside */
                if (c2 == 2 && m->w_can_queenside) {
                    if (g->board[7][0] != WROOK) return 0;
                    if (g->board[7][1] != EMPTY || g->board[7][2] != EMPTY || g->board[7][3] != EMPTY) return 0;
                    return 1;
                }
            } else {
                if (!(r1 == 0 && g->board[0][4] == BKING)) return 0;
                if (c2 == 6 && m->b_can_kingside) {
                    if (g->board[0][7] != BROOK) return 0;
                    if (g->board[0][5] != EMPTY || g->board[0][6] != EMPTY) return 0;
                    return 1;
                }
                if (c2 == 2 && m->b_can_queenside) {
                    if (g->board[0][0] != BROOK) return 0;
                    if (g->board[0][1] != EMPTY || g->board[0][2] != EMPTY || g->board[0][3] != EMPTY) return 0;
                    return 1;
                }
            }
        }

        return 0;
    }

    return 0;
}


/* apply move to match state (updates state, castling rights, en-passant target).
   promo_char may be 0 if no explicit promotion choice given; in that case queen promotion
   (existing behavior) is used for pawn reaching last rank.
*/
void apply_move(Match *m, int r1,int c1,int r2,int c2, char promo_char) {
    GameState *g = &m->state;
    Piece p = g->board[r1][c1];

    /* Reset en-passant target (unless a new double pawn move sets it below) */
    m->ep_r = m->ep_c = -1;

    /* en-passant capture: pawn moves diagonally to an empty square */
    if ((p == WPAWN || p == BPAWN) && g->board[r2][c2] == EMPTY && c1 != c2) {
        /* captured pawn located behind r2,c2 */
        int cap_r = (p == WPAWN) ? r2 + 1 : r2 - 1;
        g->board[cap_r][c2] = EMPTY;
    }

    /* castling: king moves two squares horizontally -> move rook accordingly and update castling rights */
    if ((p == WKING || p == BKING) && r1 == r2 && (c2 - c1 == 2 || c2 - c1 == -2)) {
        /* move king */
        g->board[r2][c2] = p;
        g->board[r1][c1] = EMPTY;

        if (c2 - c1 == 2) {
            /* kingside: rook from file 7 to file 5 */
            g->board[r2][5] = g->board[r2][7];
            g->board[r2][7] = EMPTY;
        } else {
            /* queenside: rook from file 0 to file 3 */
            g->board[r2][3] = g->board[r2][0];
            g->board[r2][0] = EMPTY;
        }
        /* remove castling rights for this side */
        if (p == WKING) { m->w_can_kingside = m->w_can_queenside = 0; }
        else { m->b_can_kingside = m->b_can_queenside = 0; }
        return;
    }

    /* check for rook capure */
    if  (g->board[r2][c2] == WROOK) {
        if (r2 == 7 && c2 == 7) m->w_can_kingside = 0;
        if (r2 == 7 && c2 == 0) m->w_can_queenside = 0;
    }
        if  (g->board[r2][c2] == BROOK) {
        if (r2 == 0 && c2 == 7) m->b_can_kingside = 0;
        if (r2 == 0 && c2 == 0) m->b_can_queenside = 0;
    }
    /* normal move */
    g->board[r2][c2] = p;
    g->board[r1][c1] = EMPTY;

    /* update castling rights if king or rook moved or was captured */
    if (p == WKING) { m->w_can_kingside = m->w_can_queenside = 0; }
    if (p == BKING) { m->b_can_kingside = m->b_can_queenside = 0; }
    /* if rook moved from original squares, revoke corresponding right */
    if (p == WROOK) {
        if (r1 == 7 && c1 == 7) m->w_can_kingside = 0;
        if (r1 == 7 && c1 == 0) m->w_can_queenside = 0;
    }
    if (p == BROOK) {
        if (r1 == 0 && c1 == 7) m->b_can_kingside = 0;
        if (r1 == 0 && c1 == 0) m->b_can_queenside = 0;
    }

    /* pawn double step -> set en-passant target (square behind the pawn after double move) */
    if (p == WPAWN && r1 == 6 && r2 == 4 && c1 == c2) {
        m->ep_r = 5; m->ep_c = c1;
    } else if (p == BPAWN && r1 == 1 && r2 == 3 && c1 == c2) {
        m->ep_r = 2; m->ep_c = c1;
    }

    /* promotion: if pawn reaches last rank */
    if (p == WPAWN && r2 == 0) {
        if (promo_char) {
            switch (promo_char) {
                case 'q': case 'Q': g->board[r2][c2] = WQUEEN; break;
                case 'r': case 'R': g->board[r2][c2] = WROOK; break;
                case 'b': case 'B': g->board[r2][c2] = WBISHOP; break;
                case 'n': case 'N': g->board[r2][c2] = WKNIGHT; break;
                default: g->board[r2][c2] = WQUEEN; break;
            }
        } else {
            g->board[r2][c2] = WQUEEN;
        }
    }
    if (p == BPAWN && r2 == 7) {
        if (promo_char) {
            switch (promo_char) {
                case 'q': case 'Q': g->board[r2][c2] = BQUEEN; break;
                case 'r': case 'R': g->board[r2][c2] = BROOK; break;
                case 'b': case 'B': g->board[r2][c2] = BBISHOP; break;
                case 'n': case 'N': g->board[r2][c2] = BKNIGHT; break;
                default: g->board[r2][c2] = BQUEEN; break;
            }
        } else {
            g->board[r2][c2] = BQUEEN;
        }
    }
}


/* simulate move and check whether 'color' is in check after it. */
int move_leaves_in_check(Match *m, int color, int r1, int c1, int r2, int c2) {
    GameState copy = m->state; /* struct copy is fine */
    Piece p = copy.board[r1][c1];

    /* detect en-passant capture: pawn moves diagonally to empty square that equals m->ep */
    if ((p == WPAWN || p == BPAWN) && copy.board[r2][c2] == EMPTY && c1 != c2) {
        /* en-passant: captured pawn sits behind the target square */
        int cap_r = (p == WPAWN) ? r2 + 1 : r2 - 1;
        copy.board[cap_r][c2] = EMPTY;
    }

    /* detect castling: king moves two files horizontally -> also move rook on same side */
    if ((p == WKING || p == BKING) && r1 == r2 && (c2 - c1 == 2 || c2 - c1 == -2)) {
        /* move king */
        copy.board[r2][c2] = p;
        copy.board[r1][c1] = EMPTY;
        /* rook handling */
        if (c2 - c1 == 2) {
            /* kingside rook moves from file 7 to file 5 */
            copy.board[r2][5] = copy.board[r2][7];
            copy.board[r2][7] = EMPTY;
        } else {
            /* queenside rook moves from file 0 to file 3 */
            copy.board[r2][3] = copy.board[r2][0];
            copy.board[r2][0] = EMPTY;
        }
        /* now check if king is in check */
        return is_in_check(&copy, color);
    }

    /* normal move */
    copy.board[r2][c2] = copy.board[r1][c1];
    copy.board[r1][c1] = EMPTY;

    /* promotions handled elsewhere; for check detection queen vs chosen piece is not critical */

    return is_in_check(&copy, color);
}

/* return 1 if 'color' has at least one legal move that does not leave king in check */
int has_any_legal_move(Match *m, int color) {
    GameState *g = &m->state;
    for (int r1 = 0; r1 < 8; ++r1) {
        for (int c1 = 0; c1 < 8; ++c1) {
            Piece p = g->board[r1][c1];
            if (piece_color(p) != color) continue;
            for (int r2 = 0; r2 < 8; ++r2) {
                for (int c2 = 0; c2 < 8; ++c2) {
                    if (!is_legal_move_basic(m, color, r1, c1, r2, c2)) continue;
                    if (!move_leaves_in_check(m, color, r1, c1, r2, c2)) return 1;
                }
            }
        }
    }
    return 0;
}

int is_in_check(GameState *g, int color) {
    int kr, kc;
    if (!find_king(g, color, &kr, &kc)) return 0; /* should not happen normally */
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


/* simple move-format validation: e.g. e2e4 or a7a8 */
int is_move_format(const char *m) {
    if (!m) return 0;
    size_t len = strlen(m);
    if (len != 4 && len != 5) return 0;
    if (m[0] < 'a' || m[0] > 'h') return 0;
    if (m[1] < '1' || m[1] > '8') return 0;
    if (m[2] < 'a' || m[2] > 'h') return 0;
    if (m[3] < '1' || m[3] > '8') return 0;
    if (len == 5) {
        char pc = m[4];
        /* promotion piece must be one of q,r,b,n (either case) */
        if (!(pc=='q' || pc=='r' || pc=='b' || pc=='n' ||
              pc=='Q' || pc=='R' || pc=='B' || pc=='N')) return 0;
    }
    return 1;
}


/* convert "e2e4", ... to coordinates */
void parse_move(const char *mv, int *r1,int *c1,int *r2,int *c2) {
    *c1 = mv[0] - 'a';
    *r1 = 8 - (mv[1] - '0');
    *c2 = mv[2] - 'a';
    *r2 = 8 - (mv[3] - '0');
}