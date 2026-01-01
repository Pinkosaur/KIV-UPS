/* match.h */
#ifndef MATCH_H
#define MATCH_H

#include <pthread.h>
#include <time.h>
#include "game.h"

#define TURN_TIMEOUT_SECONDS 180
#define DISCONNECT_TIMEOUT_SECONDS 60

typedef struct Client Client;

typedef struct Match {
    int id;             /* Unique room ID */
    struct Match *next; /* Linked list pointer */
    
    Client *white;
    Client *black;
    int turn; 
    pthread_mutex_t lock;
    char **moves;
    size_t moves_count;
    size_t moves_cap;
    GameState state;
    int finished;
    
    /* Castling/En-passant state */
    int w_can_kingside;
    int w_can_queenside;
    int b_can_kingside;
    int b_can_queenside;
    int ep_r;
    int ep_c;
    int draw_offered_by;
    
    time_t last_move_time;
    int turn_timeout_seconds;
    int refs; 

    /* [NEW] Timer Pause State */
    time_t elapsed_at_pause; 
    int is_paused;
} Match;

/* Global Room Management */
Match *match_create(Client *white);
int match_join(Match *m, Client *black);
char *get_room_list_str();
Match *find_open_room(int id);

/* Helpers */
void match_free(Match *m);
int match_release_after_client(Client *me); /* Changed from void to int */
int match_append_move(Match *m, const char *mv);
void notify_start(Match *m);
void *match_watchdog(void *arg);

/* Reconnection & Timer Helpers */
Client *match_reconnect(const char *name, int new_sock);
void match_try_resume(Match *m); /* [NEW] Resume timer if both players present */
int match_get_remaining_time(Match *m); /* [NEW] Get seconds left for current turn */

#endif