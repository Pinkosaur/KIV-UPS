/* match.h */
#ifndef MATCH_H
#define MATCH_H

#include <pthread.h>
#include <time.h>
#include "game.h"

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

    /* Timer Pause State */
    time_t elapsed_at_pause; 
    int is_paused;
} Match;

/* Global Room Management */
Match *match_create(Client *white);
int match_join(Match *m, Client *black);
char *get_room_list_str();
Match *find_open_room(int id);
int get_active_room_count(void);

/* Helpers */
void match_free(Match *m);
int match_release_after_client(Client *me);
int match_append_move(Match *m, const char *mv);
int match_join_by_id(int id, Client *black);
void notify_start(Match *m);
void *match_watchdog(void *arg);

/* Reconnection & Timer Helpers */
Client *match_reconnect(const char *name, const char *id, int new_sock);
int match_try_resume(Match *m); /* [CHANGED] Returns 1 if resumed from pause, 0 otherwise */
int match_get_remaining_time(Match *m); 

/* Graceful Leave Helper */
void match_leave_by_client(Client *me);

#endif