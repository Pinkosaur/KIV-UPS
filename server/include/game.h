/* game.h */
/**
 * @file game.h
 * @brief Chess definitions and logic prototypes.
 *
 * Defines the board representation, piece encoding, and prototypes for
 * move validation and game state analysis.
 */

#ifndef GAME_H
#define GAME_H

#include <time.h>

/**
 * @brief Piece encoding.
 * Positive values represent White pieces, negative values represent Black pieces.
 */
typedef enum {
    EMPTY = 0,
    WPAWN = 1, WKNIGHT = 2, WBISHOP = 3, WROOK = 4, WQUEEN = 5, WKING = 6,
    BPAWN = -1, BKNIGHT = -2, BBISHOP = -3, BROOK = -4, BQUEEN = -5, BKING = -6
} Piece;

/**
 * @brief Represents the physical state of the chess board.
 */
typedef struct {
    Piece board[8][8]; /**< 8x8 grid, [row][col], row 0 is Black's back rank */
} GameState;

/* Forward declare Match to avoid circular include with match.h */
typedef struct Match Match;

/* --- Board & Logic Functions --- */

void init_board(GameState *g);
int piece_color(Piece p);
int in_bounds(int r, int c);
int path_clear(GameState *g, int r1, int c1, int r2, int c2);
int is_square_attacked(GameState *g, int r, int c, int by_color);

/* Move Validation */
int is_legal_move_basic(Match *m, int color, int r1, int c1, int r2, int c2);
void apply_move(Match *m, int r1, int c1, int r2, int c2, char promo_char);
int move_leaves_in_check(Match *m, int color, int r1, int c1, int r2, int c2);
int has_any_legal_move(Match *m, int color);

/* State Detection */
int is_in_check(GameState *g, int color);
int find_king(GameState *g, int color, int *rk, int *ck);

/* Input Parsing */
int is_move_format(const char *m);
void parse_move(const char *mv, int *r1, int *c1, int *r2, int *c2);

#endif /* GAME_H */