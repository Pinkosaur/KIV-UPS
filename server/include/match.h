#ifndef MATCH_H
#define MATCH_H

#include <pthread.h>
#include <time.h>
#include "game.h"

#define TURN_TIMEOUT_SECONDS 180

/* forward declare Client */
typedef struct Client Client;

/* Match struct exposed so other modules can access fields (white/black etc) */
typedef struct Match {
    Client *white;
    Client *black;
    int turn; /* 0 white, 1 black */
    pthread_mutex_t lock;
    char **moves;
    size_t moves_count;
    size_t moves_cap;
    GameState state;
    int finished;
    int w_can_kingside;
    int w_can_queenside;
    int b_can_kingside;
    int b_can_queenside;
    int ep_r;
    int ep_c;
    int draw_offered_by;
    time_t last_move_time;
    int turn_timeout_seconds;
    int refs; /* refcount for cleanup */
} Match;

/* lifecycle and helpers */
Match *match_create(Client *white, Client *black);
void match_free(Match *m);
void match_release_after_client(Client *me);
int match_append_move(Match *m, const char *mv);
void notify_start(Match *m);

/* watchdog entry */
void *match_watchdog(void *arg);

#endif /* MATCH_H */
