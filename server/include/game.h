#ifndef GAME_H
#define GAME_H

#include <time.h>

typedef enum {
    EMPTY = 0,
    WPAWN = 1, WKNIGHT = 2, WBISHOP = 3, WROOK = 4, WQUEEN = 5, WKING = 6,
    BPAWN = -1, BKNIGHT = -2, BBISHOP = -3, BROOK = -4, BQUEEN = -5, BKING = -6
} Piece;

/* Game state */
typedef struct {
    Piece board[8][8];
} GameState;

/* Forward declare Match so game functions that need ep/castling can accept Match* */
typedef struct Match Match;

/* Game functions */
void init_board(GameState *g);
int piece_color(Piece p);
int in_bounds(int r,int c);
int path_clear(GameState *g, int r1,int c1,int r2,int c2);
int is_square_attacked(GameState *g, int r, int c, int by_color);

/* Higher-level move checks that require Match (for ep/castling info) */
int is_legal_move_basic(Match *m, int color, int r1, int c1, int r2, int c2);
void apply_move(Match *m, int r1,int c1,int r2,int c2, char promo_char);
int move_leaves_in_check(Match *m, int color, int r1, int c1, int r2, int c2);
int has_any_legal_move(Match *m, int color);
int is_in_check(GameState *g, int color);
int find_king(GameState *g, int color, int *rk, int *ck);
int is_move_format(const char *m);
void parse_move(const char *mv, int *r1,int *c1,int *r2,int *c2);

#endif /* GAME_H */
