/* match.h */
#ifndef MATCH_H
#define MATCH_H

#include <pthread.h>
#include <time.h>
#include "game.h"

#define TURN_TIMEOUT_SECONDS 180
#define DISCONNECT_TIMEOUT_SECONDS 60 /* Increased from 15 to 60 */

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
} Match;

/* Global Room Management */
Match *match_create(Client *white);
int match_join(Match *m, Client *black);
char *get_room_list_str();
Match *find_open_room(int id);

/* Helpers */
void match_free(Match *m);
void match_release_after_client(Client *me);
int match_append_move(Match *m, const char *mv);
void notify_start(Match *m);
void *match_watchdog(void *arg);

/* Reconnection Helper */
Client *match_reconnect(const char *name, int new_sock);

#endif