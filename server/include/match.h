/**
 * @file match.h
 * @brief Match structure and management interface.
 *
 * Defines the Match entity which binds two clients, a game state, and timing logic.
 */

#ifndef MATCH_H
#define MATCH_H

#include <pthread.h>
#include <time.h>
#include "game.h"

/* Forward declare Client to avoid circular include with client.h */
typedef struct Client Client;

/**
 * @brief Represents a single chess match (room).
 */
typedef struct Match {
    int id;                 /**< Unique Room ID */
    struct Match *next;     /**< Linked list pointer for the global registry */
    
    Client *white;          /**< Pointer to White player */
    Client *black;          /**< Pointer to Black player */
    int turn;               /**< Current turn: 0 for White, 1 for Black */
    
    pthread_mutex_t lock;   /**< Synchronization lock for match state */
    
    /* Move History */
    char **moves;
    size_t moves_count;
    size_t moves_cap;
    
    GameState state;        /**< Current board configuration */
    int finished;           /**< Flag: 1 if game has ended */
    
    /* Special Chess Rules State */
    int w_can_kingside;
    int w_can_queenside;
    int b_can_kingside;
    int b_can_queenside;
    int ep_r;               /**< En passant target row */
    int ep_c;               /**< En passant target col */
    int draw_offered_by;    /**< Color who offered draw, or -1 */
    
    /* Timing & Lifecycle */
    time_t last_move_time;
    int turn_timeout_seconds;
    int refs;               /**< Reference count (Players + Watchdog) */

    /* Timer Pause Logic (for disconnects) */
    time_t elapsed_at_pause; 
    int is_paused;
} Match;

/* --- Lifecycle Management --- */
Match *match_create(Client *white);
void match_free(Match *m);
int match_join(Match *m, Client *black);
int match_join_by_id(int id, Client *black);

/* --- Registry Access --- */
char *get_room_list_str();
Match *find_open_room(int id);
int get_active_room_count(void);

/* --- Game Flow & Events --- */
int match_release_after_client(Client *me);
int match_append_move(Match *m, const char *mv);
void notify_start(Match *m);
void *match_watchdog(void *arg);

/* --- Reconnection & Timing --- */
Client *match_reconnect(const char *name, const char *id, int new_sock);
int match_try_resume(Match *m);
int match_get_remaining_time(Match *m); 

/* --- Cleanup --- */
void match_leave_by_client(Client *me);

#endif /* MATCH_H */