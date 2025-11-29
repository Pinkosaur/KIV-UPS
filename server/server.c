/*
 * server.c
 * Multithreaded TCP pairing server with basic match state + turn enforcement.
 *
 * Compile:
 *   gcc -pthread -o server server.c
 * Run:
 *   ./server
 *
 * Listens on 0.0.0.0:10001
 *
 * Protocol (line-based UTF-8):
 * - Client -> Server:
 *   HELLO <name>
 *   MOVE e2e4
 *   RESIGN
 *   DRAW_OFFER
 *   DRAW_ACCEPT
 *   DRAW_DECLINE
 *   QUIT
 *
 * - Server -> Client:
 *   WAITING
 *   START <opponent> <white|black>
 *   OK_MOVE
 *   ERROR <reason>
 *   OPPONENT_MOVE <move>
 *   CHECK
 *   CHECKMATE
 *   CHECKMATE_WIN
 *   RESIGN
 *   OPPONENT_RESIGNED
 *   DRAW_OFFER
 *   DRAW_ACCEPTED
 *   DRAW_DECLINED
 *   TIMEOUT
 *   OPPONENT_TIMEOUT
 *   MATCH_TIMEOUT
 *   BYE
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <net/if.h>      /* IF_NAMESIZE */
#include <sys/socket.h>  /* for getsockname */
#include <netdb.h>       /* getnameinfo if needed */
#include <time.h>

#define PORT 10001
#define BACKLOG 10
#define BUF_SZ 1024
#define LINEBUF_SZ 4096
#define TURN_TIMEOUT_SECONDS 180
#define MATCHMAKING_TIMEOUT_SECONDS 10

/* forward */
typedef struct Match Match;

/* piece codes */
typedef enum {
    EMPTY = 0,
    WPAWN = 1, WKNIGHT = 2, WBISHOP = 3, WROOK = 4, WQUEEN = 5, WKING = 6,
    BPAWN = -1, BKNIGHT = -2, BBISHOP = -3, BROOK = -4, BQUEEN = -5, BKING = -6
} Piece;

/* board is 8x8 array board[row][col], 0=top (Black side) */
typedef struct {
    Piece board[8][8];
} GameState;

typedef struct Client {
    int sock;
    char name[64];
    int color; /* 0 = WHITE, 1 = BLACK, -1 = unassigned */
    int paired; /* 0 not paired, 1 paired */
    Match *match;

    /* verbose connection info for logging */
    char client_addr[64];     /* "192.168.1.5:52344" */
    char server_ifname[IF_NAMESIZE]; /* local interface name e.g. "eth0" */
    char server_ip[INET_ADDRSTRLEN]; /* local address used to accept */
} Client;


struct Match {
    Client *white;
    Client *black;
    int turn; /* 0 = WHITE to move, 1 = BLACK to move */
    pthread_mutex_t lock;
    /* move history as dynamic array of strings */
    char **moves;
    size_t moves_count;
    size_t moves_cap;
    GameState state;
    int finished; /* 0 = ongoing, 1 = finished - ignore further moves */

    /* castling rights - 1 = available, 0 = lost */
    int w_can_kingside;
    int w_can_queenside;
    int b_can_kingside;
    int b_can_queenside;

    /* en-passant target square (if a pawn moved two squares last turn, this is
       the square that can be captured by en-passant). -1 when none. */
    int ep_r;
    int ep_c;

    int draw_offered_by; /* -1 = none, 0 = white offered, 1 = black offered */
    time_t last_move_time; /* time when current turn started */
    int turn_timeout_seconds;

    /* NEW: how many client threads must run cleanup before the match can be freed.
       Initialized to 2 when a match is created (two client threads). */
    int refs;
};


static void init_board(GameState *g) {
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

static int piece_color(Piece p) {
    if (p > 0) return 0; // white
    if (p < 0) return 1; // black
    return -1;
}

static int in_bounds(int r,int c) {
    return r>=0 && r<8 && c>=0 && c<8;
}

/* convert "e2e4" to coordinates */
static void parse_move(const char *mv, int *r1,int *c1,int *r2,int *c2) {
    *c1 = mv[0] - 'a';
    *r1 = 8 - (mv[1] - '0');
    *c2 = mv[2] - 'a';
    *r2 = 8 - (mv[3] - '0');
}

static int path_clear(GameState *g, int r1, int c1, int r2, int c2) {
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
static int is_square_attacked(GameState *g, int r, int c, int by_color) {
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

/* NOTE: now accepts the Match* so we can see en-passant & infer castling rights by piece positions */
static int is_legal_move_basic(Match *m, int color, int r1, int c1, int r2, int c2) {
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
static void apply_move(Match *m, int r1,int c1,int r2,int c2, char promo_char) {
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



/* global waiting client (simple queue of length 1) */
static Client *waiting_client = NULL;
static pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;

/* helper: send a line (no automatic newline added) */
static void send_raw(int sock, const char *msg) {
    if (sock > 0) send(sock, msg, strlen(msg), 0);
}

/* helper: send line with newline */
static void send_line(int sock, const char *msg) {
    if (sock <= 0) return;
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s\n", msg);
    send_raw(sock, tmp);
}

/* trim CRLF */
static void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) { s[--n] = '\0'; }
}

/* simple move-format validation: e.g. e2e4 or a7a8 */
static int is_move_format(const char *m) {
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


static int find_king(GameState *g, int color, int *rk, int *ck) {
    Piece k = (color == 0) ? WKING : BKING;
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            if (g->board[r][c] == k) { *rk = r; *ck = c; return 1; }
        }
    }
    return 0;
}


static int is_in_check(GameState *g, int color) {
    int kr, kc;
    if (!find_king(g, color, &kr, &kc)) return 0; /* should not happen normally */
    return is_square_attacked(g, kr, kc, 1 - color);
}

/* simulate move and check whether 'color' is in check after it. */
static int move_leaves_in_check(Match *m, int color, int r1, int c1, int r2, int c2) {
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
static int has_any_legal_move(Match *m, int color) {
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

/* Forward */
static void *match_watchdog(void *arg);

/* create a new match for two clients (waiting becomes white, newcomer black) */
static Match *match_create(Client *white, Client *black) {
    Match *m = calloc(1, sizeof(Match));
    if (!m) return NULL;
    m->white = white;
    m->black = black;
    m->turn = 0; /* white to move first */
    pthread_mutex_init(&m->lock, NULL);
    m->moves = NULL;
    m->moves_count = 0;
    m->moves_cap = 0;
    m->finished = 0;   /* init finished flag */
    m->draw_offered_by = -1;

    /* initial castling rights: both sides full rights */
    m->w_can_kingside = 1;
    m->w_can_queenside = 1;
    m->b_can_kingside = 1;
    m->b_can_queenside = 1;

    /* no en-passant target initially */
    m->ep_r = -1;
    m->ep_c = -1;

    init_board(&m->state); /* initialize game state */

    /* initialization for inactivity detection */
    m->last_move_time = time(NULL); /* turn (white) starts now */
    m->turn_timeout_seconds = TURN_TIMEOUT_SECONDS;

    /* initialize client cleanup refcount: two client threads (white & black) */
    m->refs = 2;

    /* start a detached watchdog thread for this match */
    pthread_t wt;
    if (pthread_create(&wt, NULL, match_watchdog, m) == 0) {
        pthread_detach(wt);
    }

    return m;
}

/* watchdog thread for a match: ends match if current player exceeds inactivity timeout */
static void *match_watchdog(void *arg) {
    Match *m = (Match *)arg;
    if (!m) return NULL;

    while (1) {
        sleep(2); /* check every 2 seconds */

        pthread_mutex_lock(&m->lock);
        if (m->refs <= 0) {
            pthread_mutex_unlock(&m->lock);
            break;
        }
        if (m->finished) {
            pthread_mutex_unlock(&m->lock);
            break;
        }
        time_t now = time(NULL);
        time_t last = m->last_move_time;
        int timeout = m->turn_timeout_seconds;
        int timed_out = 0;

        if (last != 0 && timeout > 0 && (now - last) >= timeout) {
            /* the player whose turn it is failed to act in time */
            timed_out = 1;
        }

        if (timed_out) {
            /* Determine inactive (timed-out) and winner */
            Client *inactive = (m->turn == 0) ? m->white : m->black;
            Client *winner   = (m->turn == 0) ? m->black : m->white;

            /* mark finished */
            m->finished = 1;

            /* send messages if sockets exist and close them */
            if (inactive && inactive->sock > 0) {
                send_line(inactive->sock, "TIMEOUT"); /* you timed out */
                shutdown(inactive->sock, SHUT_RDWR);
                close(inactive->sock);
                inactive->sock = -1;
            }
            if (winner && winner->sock > 0) {
                send_line(winner->sock, "OPPONENT_TIMEOUT"); /* opponent timed out -> you win */
                shutdown(winner->sock, SHUT_RDWR);
                close(winner->sock);
                winner->sock = -1;
            }

            pthread_mutex_unlock(&m->lock);
            break;
        } else {
            pthread_mutex_unlock(&m->lock);
        }
    }

    return NULL;
}


/* free match and move history */
static void match_free(Match *m) {
    if (!m) return;
    for (size_t i = 0; i < m->moves_count; ++i) free(m->moves[i]);
    free(m->moves);
    pthread_mutex_destroy(&m->lock);
    free(m);
}

/* Called by a client thread during its cleanup path.
   Ensures:
   - notify / wake opponent so it will exit recv()
   - decrement match->refs (under lock)
   - free match only when both client threads have reached cleanup (refs==0)
*/
static void match_release_after_client(Client *me) {
    if (!me || !me->match) return;
    Match *m = me->match;
    Client *other = (me == m->white) ? m->black : m->white;

    /* First: mark finished and notify opponent (under lock) and wake recv() */
    pthread_mutex_lock(&m->lock);
    if (!m->finished) {
        m->finished = 1;
        if (other && other->sock > 0) {
            send_line(other->sock, "OPPONENT_QUIT");
        }
    }

    /* Wake the other thread (if blocked in recv) so it can run its cleanup */
    if (other && other->sock > 0) {
        /* shutdown wakes blocked recv() on the other side */
        shutdown(other->sock, SHUT_RDWR);
    }

    /* clear our match pointer (we do not free the Client here) */
    me->match = NULL;

    /* decrement refs under lock, remember whether we're the last */
    m->refs--;
    int last = (m->refs <= 0);
    pthread_mutex_unlock(&m->lock);

    if (last) {
        /* Both client threads reached cleanup. At this point both clients
           should have closed/marked their sockets; but for safety ensure sockets
           are shut/closed before freeing the match. */
        if (m->white && m->white->sock > 0) {
            shutdown(m->white->sock, SHUT_RDWR);
            close(m->white->sock);
            m->white->sock = -1;
        }
        if (m->black && m->black->sock > 0) {
            shutdown(m->black->sock, SHUT_RDWR);
            close(m->black->sock);
            m->black->sock = -1;
        }

        /* finally free match resources */
        match_free(m);
    }
}


/* Print all IPv4 interfaces and addresses on startup */
static void list_local_interfaces(void) {
    struct ifaddrs *ifaddr, *ifa;
    char addrbuf[INET_ADDRSTRLEN];

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return;
    }

    printf("Local network interfaces (IPv4):\n");
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            if (inet_ntop(AF_INET, &sa->sin_addr, addrbuf, sizeof(addrbuf))) {
                printf("  %s: %s\n", ifa->ifa_name, addrbuf);
            }
        }
    }
    freeifaddrs(ifaddr);
}

/* Given IPv4 address (in_addr), find interface name that has this address (returns 1 on success) */
static int get_interface_name_for_addr(struct in_addr inaddr, char *ifname_out, size_t ifname_out_sz) {
    struct ifaddrs *ifaddr, *ifa;
    char addrbuf[INET_ADDRSTRLEN];
    int found = 0;

    if (getifaddrs(&ifaddr) == -1) return 0;

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        if (sa->sin_addr.s_addr == inaddr.s_addr) {
            strncpy(ifname_out, ifa->ifa_name, ifname_out_sz-1);
            ifname_out[ifname_out_sz-1] = '\0';
            found = 1;
            break;
        }
    }

    freeifaddrs(ifaddr);
    return found;
}


/* append move string to match history (caller should lock) */
static int match_append_move(Match *m, const char *mv) {
    if (!m || !mv) return -1;
    if (m->moves_count + 1 > m->moves_cap) {
        size_t newcap = (m->moves_cap == 0) ? 8 : m->moves_cap * 2;
        char **tmp = realloc(m->moves, newcap * sizeof(char *));
        if (!tmp) return -1;
        m->moves = tmp;
        m->moves_cap = newcap;
    }
    m->moves[m->moves_count++] = strdup(mv);
    return 0;
}

/* send START messages to both clients indicating opponent and color */
static void notify_start(Match *m) {
    char buf[128];
    if (!m) return;
    /* to white: opponent name, your color */
    snprintf(buf, sizeof(buf), "START %s white", m->black->name);
    send_line(m->white->sock, buf);
    /* to black: opponent name, your color */
    snprintf(buf, sizeof(buf), "START %s black", m->white->name);
    send_line(m->black->sock, buf);
}

/* utility: send error */
static void send_error(Client *c, const char *reason) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "ERROR %s", reason);
    send_line(c->sock, tmp);
}

/* close a match and notify the other side */
static void match_close_and_notify(Match *m, Client *leaver, const char *reason_to_opponent) {
    if (!m) return;
    Client *other = (leaver == m->white) ? m->black : m->white;
    if (other && other->sock > 0) {
        if (reason_to_opponent)
            send_line(other->sock, reason_to_opponent);
    }
}


/* Send a textual line to a Client* and log to server console.
   Keeps a single place for logging so every message the client receives is visible on server. */
static void send_line_client(Client *c, const char *msg) {
    if (!c) return;
    if (c->sock > 0) {
        send_line(c->sock, msg);
        printf("SENT -> %s (%s) : %s\n", c->name[0] ? c->name : "(unknown)", c->client_addr, msg);
    } else {
        /* No socket available (already closed) — still log the intent. */
        printf("SENT -> %s (no socket) : %s\n", c->name[0] ? c->name : "(unknown)", msg);
    }
}



/* client thread */
static void *client_worker(void *arg) {
    Client *me = (Client *)arg;
    char readbuf[BUF_SZ];
    char linebuf[LINEBUF_SZ];
    size_t lp = 0;
    ssize_t r;

    send_line(me->sock, "WELCOME ChessServer");

    /* ask for HELLO */
    send_line(me->sock, "SEND HELLO <name>");

    /* simple read loop until HELLO received */
    int got_hello = 0;
    while (!got_hello) {
        r = recv(me->sock, readbuf, sizeof(readbuf), 0);
        if (r <= 0) { shutdown(me->sock, SHUT_RDWR); close(me->sock); free(me); return NULL; }
        for (ssize_t i = 0; i < r; ++i) {
            if (lp + 1 < sizeof(linebuf)) linebuf[lp++] = readbuf[i];
            if (readbuf[i] == '\n') {
                linebuf[lp] = '\0';
                trim_crlf(linebuf);
                if (strncmp(linebuf, "HELLO ", 6) == 0) {
                    strncpy(me->name, linebuf + 6, sizeof(me->name)-1);
                    me->name[sizeof(me->name)-1] = '\0';
                    got_hello = 1;
                    break;
                } else {
                    send_line(me->sock, "ERROR Expecting HELLO <name>");
                }
                lp = 0;
            }
        }
    }

    printf("Client connected: %s (peer %s) accepted on %s %s (sock %d)\n",
           me->name,
           me->client_addr,
           me->server_ifname,
           (me->server_ip[0] ? me->server_ip : ""),
           me->sock);

    /* Try to pair */
    pthread_mutex_lock(&wait_mutex);
    if (waiting_client == NULL) {
        /* become waiting */
        waiting_client = me;
        pthread_mutex_unlock(&wait_mutex);
        me->color = 0; /* will be WHITE */
        me->paired = 0;
        send_line(me->sock, "WAITING");
        printf("%s is waiting for opponent\n", me->name);

        /* wait until paired but also detect if client closed connection.
           Server now enforces matchmaking timeout (MATCHMAKING_TIMEOUT_SECONDS) server-side.
        */
        time_t wait_start = time(NULL);
        int search_timeout = MATCHMAKING_TIMEOUT_SECONDS;
        if (search_timeout <= 0) search_timeout = 1;

        while (!me->paired) {
            usleep(200000); /* 200ms */

            /* Detect peer closure quickly with non-blocking peek */
            char tmp;
            ssize_t rr = recv(me->sock, &tmp, 1, MSG_PEEK | MSG_DONTWAIT);
            if (rr == 0) {
                /* peer closed connection — remove from waiting queue and exit */
                pthread_mutex_lock(&wait_mutex);
                if (waiting_client == me) waiting_client = NULL;
                pthread_mutex_unlock(&wait_mutex);

                shutdown(me->sock, SHUT_RDWR);
                close(me->sock);
                printf("%s disconnected while waiting (socket closed)\n", me->name);
                free(me);
                return NULL;
            } else if (rr < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    /* socket error: treat as closed */
                    pthread_mutex_lock(&wait_mutex);
                    if (waiting_client == me) waiting_client = NULL;
                    pthread_mutex_unlock(&wait_mutex);

                    shutdown(me->sock, SHUT_RDWR);
                    close(me->sock);
                    perror("recv(MSG_PEEK)");
                    printf("%s disconnected while waiting (socket error)\n", me->name);
                    free(me);
                    return NULL;
                }
                /* rr < 0 with EAGAIN/EWOULDBLOCK -> no data pending, still alive */
            }

            /* Check server-side matchmaking timeout */
            time_t now = time(NULL);
            if (difftime(now, wait_start) >= search_timeout) {
                /* timed out waiting for opponent: inform client and close connection */
                pthread_mutex_lock(&wait_mutex);
                if (waiting_client == me) waiting_client = NULL;
                pthread_mutex_unlock(&wait_mutex);

                /* server-side message (explicit): MATCHMAKING_TIMEOUT */
                send_line(me->sock, "MATCHMAKING_TIMEOUT");

                /* close the socket and free the client (server cleans up) */
                shutdown(me->sock, SHUT_RDWR);
                close(me->sock);
                printf("%s matchmaking timed out after %d seconds\n", me->name, search_timeout);
                free(me);
                return NULL;
            }
        }

    } else {
        /* pair with waiting client */
        Client *op = waiting_client;
        waiting_client = NULL;
        pthread_mutex_unlock(&wait_mutex);

        /* create match */
        Match *m = match_create(op, me);
        if (!m) {
            send_line(me->sock, "ERROR Server internal");
            shutdown(me->sock, SHUT_RDWR);
            close(me->sock);
            free(me);
            return NULL;
        }
        /* assign */
        op->match = m; op->paired = 1; op->color = 0; /* white */
        me->match = m; me->paired = 1; me->color = 1; /* black */

        /* notify start */
        notify_start(m);
        printf("Paired %s (white, peer %s) <-> %s (black, peer %s)  [on %s/%s]\n",
               op->name, op->client_addr, me->name, me->client_addr, op->server_ifname, op->server_ip);
    }

    /* At this point me->match should be set by pairing routine */
    /* If we were the first waiting, other thread created the match and set me->match;
       if we were second, we already set match above. */

    /* Wait until match pointer is available (defensive) */
    while (me->match == NULL) {
        usleep(100000);
    }

    Match *myMatch = me->match;

    /* Main communication loop: read lines and act */
    lp = 0;
    while (1) {
        r = recv(me->sock, readbuf, sizeof(readbuf), 0);
        if (r < 0) {
            perror("recv");
            break;
        } else if (r == 0) {
            /* connection closed */
            printf("%s closed connection\n", me->name);
            break;
        }
        for (ssize_t i = 0; i < r; ++i) {
            if (lp + 1 < sizeof(linebuf)) linebuf[lp++] = readbuf[i];
            if (readbuf[i] == '\n') {
                linebuf[lp] = '\0';
                trim_crlf(linebuf);
                /* process command */
                if (strncmp(linebuf, "MOVE ", 5) == 0) {
                    const char *mv = linebuf + 5;
                    if (!is_move_format(mv)) {
                        send_error(me, "Bad MOVE format");
                    } else {
                        pthread_mutex_lock(&myMatch->lock);
                        int my_color = me->color;
                        if (myMatch->finished) {
                            pthread_mutex_unlock(&myMatch->lock);
                            send_error(me, "Game over");
                        } else if (myMatch->turn != my_color) {
                            pthread_mutex_unlock(&myMatch->lock);
                            send_error(me, "Not your turn");
                        } else {
                            /* parse and validate ... */
                            int r1,c1,r2,c2;
                            parse_move(mv,&r1,&c1,&r2,&c2);

                            /* detect optional promotion char */
                            char promo = 0;
                            if (strlen(mv) == 5) promo = mv[4];

                            /* basic movement legality (ignores checks) */
                            if (!is_legal_move_basic(myMatch, my_color, r1, c1, r2, c2)) {
                                pthread_mutex_unlock(&myMatch->lock);
                                send_error(me, "Illegal move");
                            } else {
                                /* do not allow move that leaves mover's king in check */
                                if (move_leaves_in_check(myMatch, my_color, r1, c1, r2, c2)) {
                                    pthread_mutex_unlock(&myMatch->lock);
                                    send_error(me, "Move leaves king in check");
                                } else {
                                    /* detect if the move captures the opponent king BEFORE applying it */
                                    Piece captured = myMatch->state.board[r2][c2];
                                    int opp_color = 1 - my_color;

                                    /* apply move (handles ep, castling, promotion), append, then evaluate check/mate/stalemate */
                                    apply_move(myMatch, r1, c1, r2, c2, promo);
                                    match_append_move(myMatch, mv);

                                    /* determine opponent and colors */
                                    Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
                                    opp_color = 1 - my_color;

                                    /* check whether opponent is in check now and whether they have any legal move */
                                    int opp_in_check = is_in_check(&myMatch->state, opp_color);
                                    int opp_has_move = has_any_legal_move(myMatch, opp_color);

                                    /* mark finished if mate or stalemate so future moves are rejected */
                                    if ((opp_in_check && !opp_has_move) || (!opp_in_check && !opp_has_move)) {
                                        myMatch->finished = 1;
                                        myMatch->last_move_time = 0;
                                    }

                                    /* send OK to mover first (client expects OK_MOVE) */
                                    send_line(me->sock, "OK_MOVE");

                                    /* always notify opponent of the move (so both boards update) */
                                    if (opp && opp->sock > 0) {
                                        char buf[128];
                                        snprintf(buf, sizeof(buf), "OPPONENT_MOVE %s", mv);
                                        send_line(opp->sock, buf);
                                    }

                                    /* if opponent is in check but not mate -> send CHECK to opponent */
                                    if (opp_in_check && opp_has_move) {
                                        if (opp && opp->sock > 0) send_line(opp->sock, "CHECK");
                                    }

                                    /* if it's checkmate -> send pair of messages (winner/loser) */
                                    if (opp_in_check && !opp_has_move) {
                                        /* mover delivered checkmate to opponent */
                                        if (opp && opp->sock > 0) send_line(opp->sock, "CHECKMATE");
                                        send_line(me->sock, "CHECKMATE_WIN");
                                        printf("%s won by checkmate against %s\n", me->name, opp->name);
                                    }

                                    /* stalemate: no legal moves and not in check */
                                    if (!opp_in_check && !opp_has_move) {
                                        /* send STALEMATE to both sides so they can display neutral overlay */
                                        if (opp && opp->sock > 0) send_line(opp->sock, "STALEMATE");
                                        send_line(me->sock, "STALEMATE");
                                        printf("%s and %s ended up in a stalemate\n", me->name, opp->name);
                                    }

                                    /* flip turn only if game continues */
                                    if (!myMatch->finished) {
                                        myMatch->turn = 1 - myMatch->turn;
                                        myMatch->last_move_time = time(NULL); /* start timer for new player */
                                        pthread_mutex_unlock(&myMatch->lock);
                                    } else {
                                        /* send BYE to both clients (if still connected) and close their sockets server-side */
                                        if (myMatch->white && myMatch->white->sock > 0) {
                                            send_line(myMatch->white->sock, "BYE");
                                            shutdown(myMatch->white->sock, SHUT_RDWR); /* wake any blocked recv on that socket */
                                            close(myMatch->white->sock);
                                            myMatch->white->sock = -1;
                                        }
                                        if (myMatch->black && myMatch->black->sock > 0) {
                                            send_line(myMatch->black->sock, "BYE");
                                            shutdown(myMatch->black->sock, SHUT_RDWR);
                                            close(myMatch->black->sock);
                                            myMatch->black->sock = -1;
                                        }
                                        pthread_mutex_unlock(&myMatch->lock);
                                        goto cleanup;
                                    }
                                }
                            }
                        }
                    }
                } else if (strncmp(linebuf, "RESIGN", 6) == 0) {
                    /* mark finished and notify opponent */
                    pthread_mutex_lock(&myMatch->lock);
                    myMatch->finished = 1;
                    myMatch->last_move_time = 0;
                    pthread_mutex_unlock(&myMatch->lock);

                    Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
                    
                    /* Send RESIGN to resigning player (they lost) */
                    send_line(me->sock, "RESIGN");
                    printf("%s resigned against %s\n", me->name, opp->name);
                    /* Send OPPONENT_RESIGNED to opponent (they won) */
                    if (opp && opp->sock > 0) {
                        send_line(opp->sock, "OPPONENT_RESIGNED");
                    }
                    goto cleanup;
                } else if (strncmp(linebuf, "DRAW_OFFER", 10) == 0) {
                    Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
                    printf("%s is offering %s a draw\n", me->name, opp->name);
                    pthread_mutex_lock(&myMatch->lock);
                    if (myMatch->draw_offered_by != -1) {
                        /* there is already a pending draw offer (reject) */
                        pthread_mutex_unlock(&myMatch->lock);
                        send_error(me, "Draw offer already pending");
                    } else if (opp && opp->sock > 0) {
                        /* record who offered and forward offer */
                        myMatch->draw_offered_by = me->color;
                        pthread_mutex_unlock(&myMatch->lock);
                        send_line(opp->sock, "DRAW_OFFER");
                    } else {
                        pthread_mutex_unlock(&myMatch->lock);
                        send_error(me, "No opponent");
                    }
                } else if (strncmp(linebuf, "DRAW_ACCEPT", 11) == 0) {
                    Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
                    printf("%s accepted %s's draw offer\n", me->name, opp ? opp->name : "(unknown)");
                    pthread_mutex_lock(&myMatch->lock);
                    /* only accept a draw if opponent actually offered it */
                    if (myMatch->draw_offered_by == -1 || myMatch->draw_offered_by == me->color) {
                        pthread_mutex_unlock(&myMatch->lock);
                        send_error(me, "No draw offer to accept");
                    } else {
                        /* mark finished and clear pending offer */
                        myMatch->draw_offered_by = -1;
                        myMatch->finished = 1;
                        myMatch->last_move_time = 0;

                        /* copy sockets locally and mark them -1 under lock so other code won't try to use them */
                        int me_sock  = me->sock;
                        int opp_sock = (opp ? opp->sock : -1);

                        /* mark sockets invalid in client structs so other code won't send on them */
                        if (opp) opp->sock = -1;
                        me->sock = -1;

                        pthread_mutex_unlock(&myMatch->lock);

                        /* Send DRAW_ACCEPTED to both if connected (use the copied fds) */
                        if (opp_sock > 0) send_line(opp_sock, "DRAW_ACCEPTED");
                        if (me_sock  > 0) send_line(me_sock,  "DRAW_ACCEPTED");

                        /* Important: wake blocked recv() callers on both sockets, then send BYE and close.
                        shutdown() ensures the peer thread's recv() returns promptly so it can run its cleanup. */
                        if (opp_sock > 0) {
                            /* send BYE, then shutdown and close */
                            send_line(opp_sock, "BYE");
                            shutdown(opp_sock, SHUT_RDWR);
                            close(opp_sock);
                        }
                        if (me_sock > 0) {
                            send_line(me_sock, "BYE");
                            shutdown(me_sock, SHUT_RDWR);
                            close(me_sock);
                        }

                        /* now jump to cleanup; the usual cleanup code (or match_release_after_client) will handle match freeing */
                        goto cleanup;
                    }
                } else if (strncmp(linebuf, "DRAW_DECLINE", 12) == 0) {
                    Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
                    printf("%s declined %s's draw offer\n", me->name, opp->name);
                    pthread_mutex_lock(&myMatch->lock);
                    /* decline only allowed if opponent offered */
                    if (myMatch->draw_offered_by == -1 || myMatch->draw_offered_by == me->color) {
                        pthread_mutex_unlock(&myMatch->lock);
                        send_error(me, "No draw offer to decline");
                    } else {
                        /* inform original offerer that their offer was declined */
                        myMatch->draw_offered_by = -1;
                        pthread_mutex_unlock(&myMatch->lock);
                        if (opp && opp->sock > 0) {
                            send_line(opp->sock, "DRAW_DECLINED");
                        }
                    }              
                } else if (strncmp(linebuf, "QUIT", 4) == 0) {
                    /* Treat explicit QUIT as opponent leaving -> opponent wins */
                    pthread_mutex_lock(&myMatch->lock);
                    myMatch->finished = 1;
                    myMatch->last_move_time = 0;
                    pthread_mutex_unlock(&myMatch->lock);

                    Client *opp = (me == myMatch->white) ? myMatch->black : myMatch->white;
                    printf("%s quit their match against %s\n", me->name, opp->name);
                    if (opp && opp->sock > 0) {
                        /* Notify opponent that their opponent quit and that they won */
                        send_line(opp->sock, "OPPONENT_QUIT");
                    }
                    /* send BYE to quitting client and cleanup their thread */
                    send_line(me->sock, "BYE");
                    goto cleanup;
                } else {

                    send_error(me, "Unknown command");
                }

                lp = 0;
            }
        }
    }

cleanup:
    /* cleanup client: close socket, free, notify other and free match */
    if (me->sock > 0) shutdown(me->sock, SHUT_RDWR); close(me->sock);
    printf("%s disconnected (peer %s) on %s %s\n", me->name, me->client_addr, me->server_ifname, me->server_ip);

    /* if we were in waiting queue, remove */
    pthread_mutex_lock(&wait_mutex);
    if (waiting_client == me) waiting_client = NULL;
    pthread_mutex_unlock(&wait_mutex);

    match_release_after_client(me);


    free(me);
    return NULL;
}

int main(void) {
    int srv;
    struct sockaddr_in addr;
    int opt = 1;

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    /* bind to all interfaces (0.0.0.0) so clients on the LAN can connect */
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    list_local_interfaces();

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); shutdown(srv, SHUT_RDWR); close(srv); return 1;
    }
    if (listen(srv, BACKLOG) < 0) {
        perror("listen"); shutdown(srv, SHUT_RDWR); close(srv); return 1;
    }

    printf("Server listening on all interfaces, port %d\n", PORT);

    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int csock = accept(srv, (struct sockaddr *)&cliaddr, &clilen);
        if (csock < 0) {
            perror("accept");
            continue;
        }

        Client *c = calloc(1, sizeof(Client));
        if (!c) {
            shutdown(csock, SHUT_RDWR);
            close(csock);
            continue;
        }

        c->sock = csock;
        c->color = -1;
        c->paired = 0;
        c->match = NULL;
        c->name[0] = '\0';
        c->client_addr[0] = '\0';
        c->server_ifname[0] = '\0';
        c->server_ip[0] = '\0';

        /* record remote (client) address:port */
        {
            char addrbuf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &cliaddr.sin_addr, addrbuf, sizeof(addrbuf));
            snprintf(c->client_addr, sizeof(c->client_addr), "%s:%u", addrbuf, ntohs(cliaddr.sin_port));
        }

        /* record which local interface/address the socket is bound to (use getsockname) */
        {
            struct sockaddr_in localaddr;
            socklen_t llen = sizeof(localaddr);
            if (getsockname(csock, (struct sockaddr *)&localaddr, &llen) == 0) {
                inet_ntop(AF_INET, &localaddr.sin_addr, c->server_ip, sizeof(c->server_ip));
                /* find interface name that owns this IP, if any */
                if (!get_interface_name_for_addr(localaddr.sin_addr, c->server_ifname, sizeof(c->server_ifname))) {
                    /* fallback to empty or "unknown" */
                    strncpy(c->server_ifname, "unknown", sizeof(c->server_ifname)-1);
                }
            } else {
                strncpy(c->server_ifname, "unknown", sizeof(c->server_ifname)-1);
                c->server_ip[0] = '\0';
            }
        }

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_worker, c) != 0) {
            perror("pthread_create");
            shutdown(csock, SHUT_RDWR);
            close(csock);
            free(c);
        } else {
            pthread_detach(tid);
        }
    }

    shutdown(srv, SHUT_RDWR);
    close(srv);
    return 0;
}
